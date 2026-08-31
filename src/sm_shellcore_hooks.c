#include "sm_platform.h"

#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

#include "sm_limits.h"
#include "sm_log.h"
#include "sm_runtime.h"
#include "sm_shellcore_hooks.h"
#include "sm_shellcore_protocol_defs.h"
#include "sm_shellcore_remote.h"

#define SHELLCORE_BASE_HOOK_COUNT 1u
#define SHELLCORE_MAX_HOOK_COUNT 2u
#define MAX_HOOK_PROLOGUE_SIZE SM_SHELLCORE_BRIDGE_MAX_PROLOGUE_SIZE
#define ABSOLUTE_JUMP_SIZE SM_SHELLCORE_BRIDGE_ABSOLUTE_JUMP_SIZE
#define TRAMPOLINE_SIZE SM_SHELLCORE_BRIDGE_TRAMPOLINE_SIZE

static const uint8_t k_expected_function_prologue[] = {0x55, 0x48, 0x89, 0xe5};

_Static_assert(MAX_TITLE_ID == SM_SHELLCORE_REQUEST_TITLE_ID_SIZE,
               "ShellCore bridge title id size mismatch");
_Static_assert(MAX_PATH == SM_SHELLCORE_BRIDGE_INSTALL_DIR_SIZE,
               "ShellCore bridge install dir size mismatch");
_Static_assert(TRAMPOLINE_SIZE ==
                   MAX_HOOK_PROLOGUE_SIZE + ABSOLUTE_JUMP_SIZE,
               "ShellCore bridge trampoline size mismatch");
_Static_assert(SOL_SOCKET == SM_SHELLCORE_BRIDGE_SOL_SOCKET,
               "ShellCore bridge SOL_SOCKET mismatch");
_Static_assert(SO_SNDTIMEO == SM_SHELLCORE_BRIDGE_SO_SNDTIMEO,
               "ShellCore bridge SO_SNDTIMEO mismatch");
_Static_assert(SO_RCVTIMEO == SM_SHELLCORE_BRIDGE_SO_RCVTIMEO,
               "ShellCore bridge SO_RCVTIMEO mismatch");
_Static_assert(sizeof(struct timeval) == SM_SHELLCORE_BRIDGE_TIMEVAL_SIZE,
               "ShellCore bridge timeval size mismatch");

extern const uint8_t sm_shellcore_bridge_blob_start[];
extern const uint8_t sm_shellcore_bridge_code_end[];
extern const uint8_t sm_shellcore_bridge_blob_end[];
extern const uint8_t sm_shellcore_bridge_launch_hook[];
extern const uint8_t sm_shellcore_bridge_install_all_hook[];
extern const uint8_t sm_shellcore_bridge_launch_trampoline[];
extern const uint8_t sm_shellcore_bridge_install_all_trampoline[];
extern const uint8_t sm_shellcore_bridge_install_title_dir[];
extern const uint8_t sm_shellcore_bridge_install_armed[];
extern const uint8_t sm_shellcore_bridge_install_title_id[];
extern const uint8_t sm_shellcore_bridge_install_dir[];
extern const uint8_t sm_shellcore_bridge_socket[];
extern const uint8_t sm_shellcore_bridge_setsockopt[];
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
    {sm_shellcore_bridge_setsockopt, "setsockopt"},
    {sm_shellcore_bridge_connect, "connect"},
    {sm_shellcore_bridge_read, "read"},
    {sm_shellcore_bridge_write, "write"},
    {sm_shellcore_bridge_close, "close"},
};

typedef struct {
  sm_shellcore_target_t target;
  uint8_t original[MAX_HOOK_PROLOGUE_SIZE];
  uint8_t original_size;
} shellcore_hook_record_t;

typedef enum {
  SHELLCORE_HOOKS_EMPTY = 0,
  SHELLCORE_HOOKS_READY,
  SHELLCORE_HOOKS_ROLLBACK_PENDING,
  SHELLCORE_HOOKS_STALE,
} shellcore_hooks_status_t;

typedef struct {
  shellcore_hooks_status_t status;
  sm_shellcore_remote_t remote;
  uintptr_t bridge_address;
  size_t bridge_size;
  size_t hook_count;
  shellcore_hook_record_t hooks[SHELLCORE_MAX_HOOK_COUNT];
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

static bool build_hook_patch(uint8_t patch[MAX_HOOK_PROLOGUE_SIZE],
                             uintptr_t destination, size_t patch_size) {
  if (patch_size < ABSOLUTE_JUMP_SIZE ||
      patch_size > MAX_HOOK_PROLOGUE_SIZE) {
    return false;
  }
  memset(patch, 0x90, patch_size);
  build_absolute_jump(patch, destination);
  return true;
}

static bool patch_remote_jump(pid_t pid, uintptr_t source,
                              uintptr_t destination, size_t patch_size) {
  uint8_t patch[MAX_HOOK_PROLOGUE_SIZE];
  if (!build_hook_patch(patch, destination, patch_size) ||
      !sm_remote_process_write(pid, source, patch, patch_size)) {
    return false;
  }
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

static bool restore_remote_bytes(pid_t pid, uintptr_t address,
                                 const void *original, size_t size) {
  return sm_remote_process_write(pid, address, original, size) &&
         verify_remote_bytes(pid, address, original, size);
}

static bool remote_hook_matches(pid_t pid, uintptr_t source,
                                uintptr_t destination, size_t patch_size) {
  uint8_t patch[MAX_HOOK_PROLOGUE_SIZE];
  return build_hook_patch(patch, destination, patch_size) &&
         verify_remote_bytes(pid, source, patch, patch_size);
}

static bool cleanup_remote_bridge(
    pid_t pid, const sm_shellcore_remote_t *remote,
    const shellcore_hook_record_t hooks[SHELLCORE_MAX_HOOK_COUNT],
    size_t hook_count, uintptr_t bridge_address, size_t bridge_size) {
  bool restored = true;
  for (size_t i = 0; i < hook_count; ++i) {
    uintptr_t target_address = remote->targets[hooks[i].target];
    if (!restore_remote_bytes(pid, target_address, hooks[i].original,
                              hooks[i].original_size)) {
      restored = false;
    }
  }
  if (!restored) {
    log_debug("  [SHELLCORE] hook rollback incomplete; bridge kept mapped");
    return false;
  }
  if (bridge_address &&
      !sm_remote_process_unmap(pid, bridge_address, bridge_size)) {
    log_debug("  [SHELLCORE] inactive bridge unmap failed: address=0x%lx "
              "size=0x%zx",
              (unsigned long)bridge_address, bridge_size);
  }
  return true;
}

static bool bridge_symbol_offset(const uint8_t *symbol, size_t size,
                                 size_t blob_size, size_t *offset_out) {
  uintptr_t start = (uintptr_t)sm_shellcore_bridge_blob_start;
  uintptr_t address = (uintptr_t)symbol;
  if (address < start)
    return false;
  size_t offset = (size_t)(address - start);
  if (offset > blob_size || size > blob_size - offset)
    return false;
  *offset_out = offset;
  return true;
}

static bool write_embedded_trampoline(uint8_t *bridge, size_t blob_size,
                                      const uint8_t *local_symbol,
                                      const uint8_t *original,
                                      size_t original_size,
                                      uintptr_t return_address) {
  size_t offset = 0;
  if (!original || original_size == 0 ||
      original_size > MAX_HOOK_PROLOGUE_SIZE ||
      !bridge_symbol_offset(local_symbol, TRAMPOLINE_SIZE, blob_size,
                            &offset)) {
    return false;
  }
  memset(bridge + offset, 0x90, TRAMPOLINE_SIZE);
  memcpy(bridge + offset, original, original_size);
  build_absolute_jump(bridge + offset + original_size, return_address);
  return true;
}

static bool set_bridge_pointer(uint8_t *bridge, size_t blob_size,
                               const uint8_t *local_symbol, uintptr_t value) {
  size_t offset = 0;
  if (!bridge_symbol_offset(local_symbol, sizeof(value), blob_size, &offset))
    return false;
  memcpy(bridge + offset, &value, sizeof(value));
  return true;
}

static bool resolve_bridge_imports(pid_t pid, uint8_t *bridge,
                                   size_t blob_size) {
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
    if (!set_bridge_pointer(bridge, blob_size, k_bridge_imports[i].slot,
                            address)) {
      log_debug("  [SHELLCORE] invalid bridge import slot: %s",
                k_bridge_imports[i].name);
      return false;
    }
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
  const size_t code_size =
      (size_t)(sm_shellcore_bridge_code_end -
               sm_shellcore_bridge_blob_start);
  const size_t full_blob_size =
      (size_t)(sm_shellcore_bridge_blob_end - sm_shellcore_bridge_blob_start);
  const bool install_hook_enabled = sm_shellcore_install_bridge_enabled();
  const size_t blob_size = install_hook_enabled ? full_blob_size : code_size;
  if (code_size != PAGE_SIZE || full_blob_size <= code_size ||
      blob_size > SIZE_MAX - (PAGE_SIZE - 1u)) {
    log_debug("  [SHELLCORE] invalid bridge layout: code=0x%zx full=0x%zx",
              code_size, full_blob_size);
    return false;
  }
  const size_t bridge_size =
      (blob_size + PAGE_SIZE - 1u) & ~(size_t)(PAGE_SIZE - 1u);
  uint8_t *bridge = calloc(1, bridge_size);
  if (!bridge)
    return false;
  memcpy(bridge, sm_shellcore_bridge_blob_start, blob_size);

  if (!sm_remote_process_attach(pid)) {
    log_debug("  [SHELLCORE] failed to attach to pid=%ld", (long)pid);
    free(bridge);
    return false;
  }

  bool ok = false;
  bool cleanup_pending = false;
  size_t patched_count = 0;
  uintptr_t bridge_address = 0;
  shellcore_hooks_state_t hooks = {0};
  if (!sm_shellcore_remote_resolve(pid, &hooks.remote)) {
    log_debug("  [SHELLCORE] attached-process target resolution failed");
    goto done;
  }

  if (install_hook_enabled !=
      (hooks.remote.offsets->firmware >= 0x1200u)) {
    log_debug("  [SHELLCORE] bridge firmware mode changed during install");
    goto done;
  }
  const size_t hook_count = install_hook_enabled
                                ? SHELLCORE_MAX_HOOK_COUNT
                                : SHELLCORE_BASE_HOOK_COUNT;
  hooks.hooks[0].target = SM_SHELLCORE_TARGET_LAUNCH_APP;
  hooks.hooks[1].target = SM_SHELLCORE_TARGET_INSTALL_ALL;

  for (size_t i = 0; i < hook_count; ++i) {
    shellcore_hook_record_t *hook = &hooks.hooks[i];
    uintptr_t target_address = hooks.remote.targets[hook->target];
    uint8_t patch_size =
        hooks.remote.offsets->targets[hook->target].patch_size;
    if (patch_size < ABSOLUTE_JUMP_SIZE ||
        patch_size > MAX_HOOK_PROLOGUE_SIZE ||
        !sm_remote_process_read(pid, target_address, hook->original,
                                patch_size)) {
      goto done;
    }
    if (memcmp(hook->original, k_expected_function_prologue,
               sizeof(k_expected_function_prologue)) != 0) {
      log_debug("  [SHELLCORE] unexpected prologue: %s at 0x%lx",
                sm_shellcore_target_name(hook->target),
                (unsigned long)target_address);
      goto done;
    }
    hook->original_size = patch_size;
  }
  if (install_hook_enabled &&
      !verify_remote_bytes(
          pid,
          hooks.remote.targets[SM_SHELLCORE_TARGET_INSTALL_TITLE_DIR],
          k_expected_function_prologue, sizeof(k_expected_function_prologue))) {
    log_debug("  [SHELLCORE] unexpected prologue: %s at 0x%lx",
              sm_shellcore_target_name(SM_SHELLCORE_TARGET_INSTALL_TITLE_DIR),
              (unsigned long)hooks.remote.targets
                  [SM_SHELLCORE_TARGET_INSTALL_TITLE_DIR]);
    goto done;
  }

  bridge_address = sm_remote_process_map(pid, bridge_size);
  if (!bridge_address) {
    log_debug("  [SHELLCORE] failed to allocate bridge memory: size=0x%zx",
              bridge_size);
    goto done;
  }

  shellcore_hook_record_t *launch_hook_record = &hooks.hooks[0];
  uintptr_t launch_target = hooks.remote.targets[launch_hook_record->target];
  shellcore_hook_record_t *install_hook_record = &hooks.hooks[1];
  uintptr_t install_target = hooks.remote.targets[install_hook_record->target];
  if (!write_embedded_trampoline(
          bridge, blob_size, sm_shellcore_bridge_launch_trampoline,
          launch_hook_record->original, launch_hook_record->original_size,
          launch_target + launch_hook_record->original_size)) {
    goto done;
  }
  if (install_hook_enabled) {
    if (!write_embedded_trampoline(
            bridge, blob_size, sm_shellcore_bridge_install_all_trampoline,
            install_hook_record->original, install_hook_record->original_size,
            install_target + install_hook_record->original_size)) {
      goto done;
    }
    if (!set_bridge_pointer(
            bridge, blob_size, sm_shellcore_bridge_install_title_dir,
            hooks.remote.targets[SM_SHELLCORE_TARGET_INSTALL_TITLE_DIR])) {
      goto done;
    }
  }
  if (!resolve_bridge_imports(pid, bridge, blob_size))
    goto done;

  bool bridge_written =
      sm_remote_process_write(pid, bridge_address, bridge, bridge_size) &&
      verify_remote_bytes(pid, bridge_address, bridge, bridge_size);
  bool protection_ok = false;
  if (bridge_written) {
    protection_ok =
        kernel_mprotect(pid, (intptr_t)bridge_address, code_size,
                        PROT_READ | PROT_EXEC) == 0;
    if (install_hook_enabled && protection_ok) {
      protection_ok =
          kernel_mprotect(pid, (intptr_t)(bridge_address + code_size),
                          bridge_size - code_size,
                          PROT_READ | PROT_WRITE) == 0;
    }
  }
  if (!bridge_written || !protection_ok) {
    log_debug("  [SHELLCORE] bridge install verification failed");
    goto done;
  }

  uintptr_t launch_hook =
      bridge_address + (uintptr_t)(sm_shellcore_bridge_launch_hook -
                                   sm_shellcore_bridge_blob_start);
  uintptr_t install_all_hook =
      bridge_address +
      (uintptr_t)(sm_shellcore_bridge_install_all_hook -
                  sm_shellcore_bridge_blob_start);
  // A failed write verification can still mean that the target was modified.
  // Publish rollback ownership before attempting each remote write.
  patched_count = 1;
  if (!patch_remote_jump(pid, launch_target, launch_hook,
                         launch_hook_record->original_size)) {
    goto done;
  }
  if (install_hook_enabled) {
    patched_count = SHELLCORE_MAX_HOOK_COUNT;
    if (!patch_remote_jump(pid, install_target, install_all_hook,
                           install_hook_record->original_size)) {
      goto done;
    }
  }

  hooks.bridge_address = bridge_address;
  hooks.bridge_size = bridge_size;
  hooks.hook_count = hook_count;
  hooks.status = SHELLCORE_HOOKS_READY;
  ok = true;

done:
  if (!ok) {
    cleanup_pending = !cleanup_remote_bridge(
                          pid, &hooks.remote, hooks.hooks, patched_count,
                          bridge_address, bridge_size) &&
                      patched_count != 0;
  }
  bool detached = sm_remote_process_detach(pid);
  if (!detached) {
    log_debug("  [SHELLCORE] failed to detach from pid=%ld", (long)pid);
    if (ok) {
      cleanup_pending = !cleanup_remote_bridge(
          pid, &hooks.remote, hooks.hooks, hook_count, bridge_address,
          bridge_size);
    }
    (void)sm_remote_process_detach(pid);
    ok = false;
  }
  free(bridge);
  if (ok) {
    g_hooks = hooks;
    uintptr_t launch_trampoline =
        bridge_address +
        (uintptr_t)(sm_shellcore_bridge_launch_trampoline -
                    sm_shellcore_bridge_blob_start);
    log_debug("  [SHELLCORE] hooks installed: fw=%s pid=%ld bridge=0x%lx+0x%zx "
              "launch=1 trampoline=0x%lx install=%d prot=%s",
              hooks.remote.offsets->name, (long)pid,
              (unsigned long)bridge_address, bridge_size,
              (unsigned long)launch_trampoline,
              install_hook_enabled ? 1 : 0,
              install_hook_enabled ? "rx/rw" : "rx");
  } else if (cleanup_pending) {
    hooks.bridge_address = bridge_address;
    hooks.bridge_size = bridge_size;
    hooks.hook_count = patched_count;
    hooks.status = SHELLCORE_HOOKS_ROLLBACK_PENDING;
    g_hooks = hooks;
    log_debug("  [SHELLCORE] retaining bridge state for shutdown cleanup");
  }
  return ok;
}

bool sm_shellcore_hooks_start(void) {
  pthread_mutex_lock(&g_install_mutex);
  if (g_hooks.status != SHELLCORE_HOOKS_EMPTY) {
    bool ready = g_hooks.status == SHELLCORE_HOOKS_READY;
    pthread_mutex_unlock(&g_install_mutex);
    return ready;
  }
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
}

void sm_shellcore_hooks_stop(void) {
  pthread_mutex_lock(&g_install_mutex);
  if (g_hooks.status == SHELLCORE_HOOKS_EMPTY) {
    pthread_mutex_unlock(&g_install_mutex);
    return;
  }

  pid_t pid = g_hooks.remote.pid;
  pid_t current_pid = find_shellcore_pid();
  if (current_pid > 0 && current_pid != pid) {
    log_debug("  [SHELLCORE] stale hook state dropped: old pid=%ld current=%ld",
              (long)pid, (long)current_pid);
    memset(&g_hooks, 0, sizeof(g_hooks));
    pthread_mutex_unlock(&g_install_mutex);
    return;
  }
  if (!sm_remote_process_attach(pid)) {
    if (kill(pid, 0) != 0 && errno == ESRCH)
      memset(&g_hooks, 0, sizeof(g_hooks));
    else
      log_debug("  [SHELLCORE] failed to attach for hook cleanup: pid=%ld",
                (long)pid);
    pthread_mutex_unlock(&g_install_mutex);
    return;
  }

  sm_shellcore_remote_t current_remote;
  if (!sm_shellcore_remote_resolve(pid, &current_remote)) {
    log_debug("  [SHELLCORE] cleanup target validation failed; state retained");
    (void)sm_remote_process_detach(pid);
    pthread_mutex_unlock(&g_install_mutex);
    return;
  }
  if (current_remote.image_base != g_hooks.remote.image_base) {
    log_debug("  [SHELLCORE] stale hook image detected during cleanup");
    (void)sm_remote_process_detach(pid);
    memset(&g_hooks, 0, sizeof(g_hooks));
    pthread_mutex_unlock(&g_install_mutex);
    return;
  }

  bool restored = true;
  bool found_installed_hook = false;
  for (size_t i = 0; i < g_hooks.hook_count; ++i) {
    shellcore_hook_record_t *hook = &g_hooks.hooks[i];
    uintptr_t target_address = g_hooks.remote.targets[hook->target];
    if (g_hooks.status == SHELLCORE_HOOKS_ROLLBACK_PENDING) {
      found_installed_hook = true;
      if (!restore_remote_bytes(pid, target_address, hook->original,
                                hook->original_size)) {
        restored = false;
      }
      continue;
    }
    const uint8_t *hook_symbol =
        hook->target == SM_SHELLCORE_TARGET_LAUNCH_APP
            ? sm_shellcore_bridge_launch_hook
            : sm_shellcore_bridge_install_all_hook;
    uintptr_t hook_address =
        g_hooks.bridge_address +
        (uintptr_t)(hook_symbol - sm_shellcore_bridge_blob_start);
    if (remote_hook_matches(pid, target_address, hook_address,
                            hook->original_size)) {
      found_installed_hook = true;
      if (!restore_remote_bytes(pid, target_address, hook->original,
                                hook->original_size)) {
        restored = false;
      }
    } else if (!verify_remote_bytes(pid, target_address, hook->original,
                                    hook->original_size)) {
      restored = false;
    }
  }
  if (!sm_remote_process_detach(pid)) {
    log_debug("  [SHELLCORE] failed to detach after hook cleanup: pid=%ld",
              (long)pid);
    (void)sm_remote_process_detach(pid);
  }
  if (!restored) {
    log_debug("  [SHELLCORE] hook cleanup incomplete; bridge kept mapped");
    pthread_mutex_unlock(&g_install_mutex);
    return;
  }
  if (!found_installed_hook) {
    log_debug("  [SHELLCORE] hooks already absent; stale bridge state dropped");
  } else if (g_hooks.bridge_address) {
    // A thread that entered the bridge before its prologue was restored may
    // still return through it after detach. Keep the small bridge mapping in
    // ShellCore rather than introducing a shutdown-only use-after-unmap race.
    log_debug("  [SHELLCORE] inactive bridge retained: address=0x%lx size=0x%zx",
              (unsigned long)g_hooks.bridge_address, g_hooks.bridge_size);
  }
  memset(&g_hooks, 0, sizeof(g_hooks));
  pthread_mutex_unlock(&g_install_mutex);
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
  if (g_hooks.status != SHELLCORE_HOOKS_READY ||
      g_hooks.hook_count < SHELLCORE_MAX_HOOK_COUNT) {
    pthread_mutex_unlock(&g_install_mutex);
    return false;
  }

  pid_t pid = g_hooks.remote.pid;
  pid_t current_pid = find_shellcore_pid();
  if (current_pid > 0 && current_pid != pid) {
    memset(&g_hooks, 0, sizeof(g_hooks));
    pthread_mutex_unlock(&g_install_mutex);
    return false;
  }
  if (current_pid <= 0) {
    pthread_mutex_unlock(&g_install_mutex);
    return false;
  }
  uintptr_t install_hook =
      remote_bridge_symbol(sm_shellcore_bridge_install_all_hook);
  uintptr_t install_target =
      g_hooks.remote.targets[SM_SHELLCORE_TARGET_INSTALL_ALL];
  if (!remote_hook_matches(pid, install_target, install_hook,
                           g_hooks.hooks[1].original_size)) {
    g_hooks.status = SHELLCORE_HOOKS_STALE;
    log_debug("  [SHELLCORE] AppInstallAll hook is no longer installed");
    pthread_mutex_unlock(&g_install_mutex);
    return false;
  }

  char remote_title[MAX_TITLE_ID] = {0};
  char remote_dir[MAX_PATH] = {0};
  memcpy(remote_title, title_id, title_len);
  memcpy(remote_dir, install_dir, dir_len);

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
