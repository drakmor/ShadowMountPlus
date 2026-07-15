#include "sm_platform.h"

#include <signal.h>
#include <sys/mman.h>
#include <pthread.h>

#include "sm_limits.h"
#include "sm_log.h"
#include "sm_runtime.h"
#include "sm_shellcore_hooks.h"
#include "sm_shellcore_remote.h"

#define SHELLCORE_BASE_HOOK_COUNT 2u
#define SHELLCORE_MAX_HOOK_COUNT 3u
#define MAX_HOOK_PROLOGUE_SIZE 32u
#define ABSOLUTE_JUMP_SIZE 12u

/*
 * Private payload-SDK ABI from src/crt/kernel.h.  The public
 * <ps5/kernel.h> intentionally omits dynlib_obj_t/kernel_dynlib_obj(), but
 * kernel_dynlib_obj() copies the complete 0x180-byte object.  Keep the local
 * definition layout-checked because ShellCore bridge placement needs the
 * module bounds and its executable eh_frame area.
 */
typedef struct {
  uintptr_t next;
  uintptr_t path;
  uintptr_t unknown0[2];
  uint32_t refcount;
  uintptr_t handle;
  uintptr_t mapbase;
  uintptr_t mapsize;
  uintptr_t textsize;
  uintptr_t database;
  uintptr_t datasize;
  uintptr_t unknown1;
  uintptr_t unknown1size;
  uintptr_t entry;
  uintptr_t unknown2;
  uintptr_t vaddrbase;
  uint32_t tlsindex;
  uintptr_t tlsinit;
  uintptr_t tlsinitsize;
  uintptr_t tlssize;
  uintptr_t tlsoffset;
  uintptr_t tlsalign;
  uintptr_t pltgot;
  uintptr_t unknown3[6];
  uintptr_t init;
  uintptr_t fini;
  uintptr_t eh_frame_hdr;
  uintptr_t eh_frame_hdr_size;
  uintptr_t eh_frame;
  uintptr_t eh_frame_size;
  int32_t status;
  int32_t flags;
  uintptr_t unknown4[5];
  uintptr_t dynsec;
  uintptr_t unknown5[6];
} shellcore_dynlib_obj_t;

_Static_assert(offsetof(shellcore_dynlib_obj_t, mapbase) == 0x30,
               "unexpected dynlib_obj mapbase offset");
_Static_assert(offsetof(shellcore_dynlib_obj_t, eh_frame) == 0x108,
               "unexpected dynlib_obj eh_frame offset");
_Static_assert(sizeof(shellcore_dynlib_obj_t) == 0x180,
               "unexpected dynlib_obj size");

int kernel_dynlib_obj(pid_t pid, uint32_t handle, shellcore_dynlib_obj_t *obj);

extern const uint8_t sm_shellcore_bridge_blob_start[];
extern const uint8_t sm_shellcore_bridge_blob_end[];
extern const uint8_t sm_shellcore_bridge_launch_hook[];
extern const uint8_t sm_shellcore_bridge_app_exit_hook[];
extern const uint8_t sm_shellcore_bridge_install_all_hook[];
extern const uint8_t sm_shellcore_bridge_launch_trampoline[];
extern const uint8_t sm_shellcore_bridge_app_exit_trampoline[];
extern const uint8_t sm_shellcore_bridge_install_all_trampoline[];
extern const uint8_t sm_shellcore_bridge_install_title_dir[];
extern const uint8_t sm_shellcore_bridge_install_armed[];
extern const uint8_t sm_shellcore_bridge_install_title_id[];
extern const uint8_t sm_shellcore_bridge_install_dir[];
extern const uint8_t sm_shellcore_bridge_socket[];
extern const uint8_t sm_shellcore_bridge_connect[];
extern const uint8_t sm_shellcore_bridge_read[];
extern const uint8_t sm_shellcore_bridge_write[];
extern const uint8_t sm_shellcore_bridge_close[];

typedef struct {
  const uint8_t *slot;
  const char *name;
} shellcore_import_t;

static const shellcore_import_t k_bridge_imports[] = {
    {sm_shellcore_bridge_socket, "socket"},
    {sm_shellcore_bridge_connect, "connect"},
    {sm_shellcore_bridge_read, "read"},
    {sm_shellcore_bridge_write, "write"},
    {sm_shellcore_bridge_close, "close"},
};

typedef struct {
  bool installed;
  sm_shellcore_remote_t remote;
  uintptr_t bridge_address;
  size_t hook_count;
  sm_shellcore_target_t target_ids[SHELLCORE_MAX_HOOK_COUNT];
  uint8_t original[SHELLCORE_MAX_HOOK_COUNT][MAX_HOOK_PROLOGUE_SIZE];
  uint8_t original_size[SHELLCORE_MAX_HOOK_COUNT];
} shellcore_hooks_state_t;

static shellcore_hooks_state_t g_hooks;
static pthread_mutex_t g_install_mutex = PTHREAD_MUTEX_INITIALIZER;

static uintptr_t remote_bridge_symbol(const uint8_t *symbol) {
  return g_hooks.bridge_address +
         (uintptr_t)(symbol - sm_shellcore_bridge_blob_start);
}

static void build_absolute_jump(uint8_t jump[ABSOLUTE_JUMP_SIZE],
                                uintptr_t destination) {
  jump[0] = 0x48;
  jump[1] = 0xb8;
  memcpy(jump + 2, &destination, sizeof(destination));
  jump[10] = 0xff;
  jump[11] = 0xe0;
}

static bool patch_remote_jump(pid_t pid, uintptr_t source,
                              uintptr_t destination, size_t patch_size) {
  if (patch_size < ABSOLUTE_JUMP_SIZE || patch_size > MAX_HOOK_PROLOGUE_SIZE) {
    return false;
  }
  uint8_t patch[MAX_HOOK_PROLOGUE_SIZE];
  memset(patch, 0x90, patch_size);
  build_absolute_jump(patch, destination);
  if (!sm_remote_process_write(pid, source, patch, patch_size))
    return false;
  uint8_t verify[MAX_HOOK_PROLOGUE_SIZE];
  return sm_remote_process_read(pid, source, verify, patch_size) &&
         memcmp(verify, patch, patch_size) == 0;
}

static bool verify_remote_bytes(pid_t pid, uintptr_t address,
                                const void *expected, size_t size) {
  uint8_t buffer[256];
  const uint8_t *bytes = (const uint8_t *)expected;
  while (size != 0) {
    size_t chunk = size < sizeof(buffer) ? size : sizeof(buffer);
    if (!sm_remote_process_read(pid, address, buffer, chunk) ||
        memcmp(buffer, bytes, chunk) != 0) {
      return false;
    }
    address += chunk;
    bytes += chunk;
    size -= chunk;
  }
  return true;
}

static bool append_trampoline(uint8_t *bridge, size_t bridge_capacity,
                              size_t *cursor, const uint8_t *original,
                              size_t original_size, uintptr_t return_address,
                              uintptr_t remote_base,
                              uintptr_t *remote_trampoline_out) {
  size_t aligned = (*cursor + 15u) & ~(size_t)15u;
  size_t required = aligned + original_size + ABSOLUTE_JUMP_SIZE;
  if (required > bridge_capacity)
    return false;
  memcpy(bridge + aligned, original, original_size);
  build_absolute_jump(bridge + aligned + original_size, return_address);
  *remote_trampoline_out = remote_base + aligned;
  *cursor = required;
  return true;
}

static void set_bridge_pointer(uint8_t *bridge, const uint8_t *local_symbol,
                               uintptr_t value) {
  size_t offset = (size_t)(local_symbol - sm_shellcore_bridge_blob_start);
  memcpy(bridge + offset, &value, sizeof(value));
}

static bool resolve_bridge_imports(pid_t pid, uint8_t *bridge) {
  uint32_t handle = UINT32_MAX;
  if (kernel_dynlib_handle(pid, "libkernel_sys.sprx", &handle) != 0)
    return false;

  for (size_t i = 0; i < sizeof(k_bridge_imports) / sizeof(k_bridge_imports[0]);
       ++i) {
    uintptr_t address =
        (uintptr_t)kernel_dynlib_dlsym(pid, handle, k_bridge_imports[i].name);
    if (address == 0) {
      log_debug("  [SHELLCORE] failed to resolve libkernel %s",
                k_bridge_imports[i].name);
      return false;
    }
    set_bridge_pointer(bridge, k_bridge_imports[i].slot, address);
  }
  return true;
}

static pid_t find_shellcore_pid(void) {
  pid_t pid = find_pid_by_name("SceShellCore", true);
  if (pid <= 0)
    pid = find_pid_by_name("SceShellCore.elf", true);
  return pid;
}

bool sm_shellcore_install_bridge_enabled(void) {
  uint16_t firmware =
      (uint16_t)((kernel_get_fw_version() >> 16) & 0xffffu);
  return firmware >= 0x1200u;
}

static bool install_hooks_for_pid(pid_t pid) {
  const size_t blob_size =
      (size_t)(sm_shellcore_bridge_blob_end - sm_shellcore_bridge_blob_start);
  const size_t bridge_capacity =
      blob_size + SHELLCORE_MAX_HOOK_COUNT *
                      (MAX_HOOK_PROLOGUE_SIZE + ABSOLUTE_JUMP_SIZE + 16u);
  uint8_t *bridge = calloc(1, bridge_capacity);
  if (!bridge)
    return false;
  memcpy(bridge, sm_shellcore_bridge_blob_start, blob_size);

  if (kill(pid, SIGSTOP) != 0) {
    free(bridge);
    return false;
  }

  bool ok = false;
  sm_shellcore_remote_t remote;
  shellcore_dynlib_obj_t object;
  if (!sm_shellcore_remote_resolve(pid, &remote) ||
      kernel_dynlib_obj(pid, 0, &object) != 0) {
    log_debug("  [SHELLCORE] stopped-process layout resolution failed");
    goto done;
  }
  if (object.mapbase != remote.image_base ||
      object.mapbase > UINTPTR_MAX - object.mapsize ||
      object.eh_frame < object.mapbase ||
      object.eh_frame_size < bridge_capacity ||
      object.eh_frame > UINTPTR_MAX - bridge_capacity ||
      object.eh_frame + bridge_capacity > object.mapbase + object.mapsize) {
    log_debug("  [SHELLCORE] invalid layout: base=0x%lx map=0x%lx+0x%lx "
              "eh=0x%lx+0x%lx bridge=0x%zx",
              (unsigned long)remote.image_base, (unsigned long)object.mapbase,
              (unsigned long)object.mapsize, (unsigned long)object.eh_frame,
              (unsigned long)object.eh_frame_size, bridge_capacity);
    goto done;
  }
  log_debug("  [SHELLCORE] layout validated: map=0x%lx+0x%lx eh=0x%lx+0x%lx",
            (unsigned long)object.mapbase, (unsigned long)object.mapsize,
            (unsigned long)object.eh_frame,
            (unsigned long)object.eh_frame_size);

  const bool install_hook_enabled = sm_shellcore_install_bridge_enabled();
  const size_t hook_count = install_hook_enabled
                                ? SHELLCORE_MAX_HOOK_COUNT
                                : SHELLCORE_BASE_HOOK_COUNT;
  uintptr_t targets[SHELLCORE_MAX_HOOK_COUNT] = {
      remote.targets[SM_SHELLCORE_TARGET_LAUNCH_APP],
      remote.targets[SM_SHELLCORE_TARGET_APP_EXIT],
      remote.targets[SM_SHELLCORE_TARGET_INSTALL_ALL],
  };
  sm_shellcore_target_t target_ids[SHELLCORE_MAX_HOOK_COUNT] = {
      SM_SHELLCORE_TARGET_LAUNCH_APP,
      SM_SHELLCORE_TARGET_APP_EXIT,
      SM_SHELLCORE_TARGET_INSTALL_ALL,
  };

  for (size_t i = 0; i < hook_count; ++i) {
    uint8_t patch_size = remote.offsets->targets[target_ids[i]].patch_size;
    if (patch_size < ABSOLUTE_JUMP_SIZE ||
        patch_size > MAX_HOOK_PROLOGUE_SIZE ||
        !sm_remote_process_read(pid, targets[i], g_hooks.original[i],
                                patch_size)) {
      goto done;
    }
    static const uint8_t function_prologue[] = {0x55, 0x48, 0x89, 0xe5};
    if (memcmp(g_hooks.original[i], function_prologue,
               sizeof(function_prologue)) != 0) {
      log_debug("  [SHELLCORE] unexpected prologue: %s at 0x%lx",
                sm_shellcore_target_name(target_ids[i]),
                (unsigned long)targets[i]);
      goto done;
    }
    g_hooks.original_size[i] = patch_size;
  }

  size_t cursor = blob_size;
  uintptr_t launch_trampoline = 0;
  uintptr_t app_exit_trampoline = 0;
  uintptr_t install_all_trampoline = 0;
  if (!append_trampoline(bridge, bridge_capacity, &cursor, g_hooks.original[0],
                         g_hooks.original_size[0],
                         targets[0] + g_hooks.original_size[0], object.eh_frame,
                         &launch_trampoline)) {
    goto done;
  }
  set_bridge_pointer(bridge, sm_shellcore_bridge_launch_trampoline,
                     launch_trampoline);
  if (!append_trampoline(bridge, bridge_capacity, &cursor, g_hooks.original[1],
                         g_hooks.original_size[1],
                         targets[1] + g_hooks.original_size[1], object.eh_frame,
                         &app_exit_trampoline)) {
    goto done;
  }
  set_bridge_pointer(bridge, sm_shellcore_bridge_app_exit_trampoline,
                     app_exit_trampoline);
  if (install_hook_enabled) {
    if (!append_trampoline(bridge, bridge_capacity, &cursor,
                           g_hooks.original[2], g_hooks.original_size[2],
                           targets[2] + g_hooks.original_size[2],
                           object.eh_frame, &install_all_trampoline)) {
      goto done;
    }
    set_bridge_pointer(bridge, sm_shellcore_bridge_install_all_trampoline,
                       install_all_trampoline);
    set_bridge_pointer(
        bridge, sm_shellcore_bridge_install_title_dir,
        remote.targets[SM_SHELLCORE_TARGET_INSTALL_TITLE_DIR]);
  }
  if (!resolve_bridge_imports(pid, bridge))
    goto done;

  if (kernel_mprotect(pid, (intptr_t)object.mapbase, object.mapsize,
                      PROT_READ | PROT_WRITE | PROT_EXEC) != 0 ||
      !sm_remote_process_write(pid, object.eh_frame, bridge, cursor) ||
      !verify_remote_bytes(pid, object.eh_frame, bridge, cursor)) {
    log_debug("  [SHELLCORE] bridge write verification failed");
    goto done;
  }

  uintptr_t launch_hook =
      object.eh_frame + (uintptr_t)(sm_shellcore_bridge_launch_hook -
                                    sm_shellcore_bridge_blob_start);
  uintptr_t app_exit_hook =
      object.eh_frame + (uintptr_t)(sm_shellcore_bridge_app_exit_hook -
                                    sm_shellcore_bridge_blob_start);
  uintptr_t install_all_hook =
      object.eh_frame +
      (uintptr_t)(sm_shellcore_bridge_install_all_hook -
                  sm_shellcore_bridge_blob_start);
  if (!patch_remote_jump(pid, targets[0], launch_hook,
                         g_hooks.original_size[0]) ||
      !patch_remote_jump(pid, targets[1], app_exit_hook,
                         g_hooks.original_size[1]) ||
      (install_hook_enabled &&
       !patch_remote_jump(pid, targets[2], install_all_hook,
                          g_hooks.original_size[2]))) {
    goto rollback;
  }

  g_hooks.remote = remote;
  g_hooks.bridge_address = object.eh_frame;
  g_hooks.hook_count = hook_count;
  memcpy(g_hooks.target_ids, target_ids,
         hook_count * sizeof(g_hooks.target_ids[0]));
  g_hooks.installed = true;
  ok = true;
  log_debug("  [SHELLCORE] hooks installed: fw=%s pid=%ld lifecycle=1 "
            "app_exit=1 install=%d",
            remote.offsets->name, (long)pid, install_hook_enabled ? 1 : 0);
  goto done;

rollback:
  for (size_t i = 0; i < hook_count; ++i) {
    (void)sm_remote_process_write(pid, targets[i], g_hooks.original[i],
                                  g_hooks.original_size[i]);
  }

done:
  free(bridge);
  (void)kill(pid, SIGCONT);
  return ok;
}

bool sm_shellcore_hooks_start(void) {
  pthread_mutex_lock(&g_install_mutex);
  if (g_hooks.installed)
    goto installed;
  memset(&g_hooks, 0, sizeof(g_hooks));
  pid_t pid = find_shellcore_pid();
  if (pid <= 0) {
    log_debug("  [SHELLCORE] process not found");
    pthread_mutex_unlock(&g_install_mutex);
    return false;
  }
  bool ok = install_hooks_for_pid(pid);
  pthread_mutex_unlock(&g_install_mutex);
  return ok;

installed:
  pthread_mutex_unlock(&g_install_mutex);
  return true;
}

void sm_shellcore_hooks_stop(void) {
  pthread_mutex_lock(&g_install_mutex);
  if (!g_hooks.installed) {
    pthread_mutex_unlock(&g_install_mutex);
    return;
  }

  pid_t pid = g_hooks.remote.pid;
  if (kill(pid, SIGSTOP) == 0) {
    for (size_t i = 0; i < g_hooks.hook_count; ++i) {
      sm_shellcore_target_t target = g_hooks.target_ids[i];
      (void)sm_remote_process_write(pid, g_hooks.remote.targets[target],
                                    g_hooks.original[i],
                                    g_hooks.original_size[i]);
    }
    (void)kill(pid, SIGCONT);
  }
  memset(&g_hooks, 0, sizeof(g_hooks));
  pthread_mutex_unlock(&g_install_mutex);
}

bool sm_shellcore_app_exit_hook_active(void) {
  int saved_errno = errno;
  pthread_mutex_lock(&g_install_mutex);
  bool active = g_hooks.installed && g_hooks.remote.pid > 0;
  if (active && kill(g_hooks.remote.pid, 0) != 0 && errno == ESRCH)
    active = false;
  pthread_mutex_unlock(&g_install_mutex);
  errno = saved_errno;
  return active;
}

bool sm_shellcore_install_title_dir(const char *title_id,
                                    const char *install_dir,
                                    int *result_out) {
  if (!title_id || !install_dir || !result_out)
    return false;

  size_t title_len = strnlen(title_id, MAX_TITLE_ID);
  size_t dir_len = strnlen(install_dir, MAX_PATH);
  if (title_len == 0 || title_len >= MAX_TITLE_ID || dir_len == 0 ||
      dir_len >= MAX_PATH) {
    return false;
  }

  pthread_mutex_lock(&g_install_mutex);
  if (!g_hooks.installed ||
      g_hooks.hook_count < SHELLCORE_MAX_HOOK_COUNT) {
    pthread_mutex_unlock(&g_install_mutex);
    return false;
  }

  char remote_title[MAX_TITLE_ID] = {0};
  char remote_dir[MAX_PATH] = {0};
  memcpy(remote_title, title_id, title_len);
  memcpy(remote_dir, install_dir, dir_len);

  pid_t pid = g_hooks.remote.pid;
  uint8_t armed = 1;
  bool written =
      sm_remote_process_write(pid,
                              remote_bridge_symbol(
                                  sm_shellcore_bridge_install_title_id),
                              remote_title, sizeof(remote_title)) &&
      sm_remote_process_write(
          pid, remote_bridge_symbol(sm_shellcore_bridge_install_dir),
          remote_dir, sizeof(remote_dir)) &&
      sm_remote_process_write(
          pid, remote_bridge_symbol(sm_shellcore_bridge_install_armed),
          &armed, sizeof(armed));
  if (!written) {
    armed = 0;
    (void)sm_remote_process_write(
        pid, remote_bridge_symbol(sm_shellcore_bridge_install_armed), &armed,
        sizeof(armed));
    pthread_mutex_unlock(&g_install_mutex);
    return false;
  }

  int result = sceAppInstUtilAppInstallAll(NULL);
  uint8_t remaining = 1;
  bool read_back = sm_remote_process_read(
      pid, remote_bridge_symbol(sm_shellcore_bridge_install_armed), &remaining,
      sizeof(remaining));
  bool consumed = read_back && remaining == 0;
  if (!consumed) {
    armed = 0;
    (void)sm_remote_process_write(
        pid, remote_bridge_symbol(sm_shellcore_bridge_install_armed), &armed,
        sizeof(armed));
  }
  pthread_mutex_unlock(&g_install_mutex);

  if (!consumed) {
    log_debug("  [SHELLCORE] AppInstallAll trigger was not consumed: %s",
              title_id);
    return false;
  }
  log_debug("  [SHELLCORE] AppInstallAll bridge consumed: %s result=0x%08x",
            title_id, (uint32_t)result);
  *result_out = result;
  return true;
}
