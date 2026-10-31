#include "sm_platform.h"
#include <sys/mman.h>

#include "sm_config_mount.h"
#include "sm_dynlib.h"
#include "sm_kstuff.h"
#include "sm_log.h"
#include "sm_ps4_video.h"
#include "sm_ps4_video_defs.h"
#include "sm_shellcore_protocol_defs.h"
#include "sm_shellcore_remote.h"
#include "sm_time.h"

#define VIDEO_PAGE_SIZE 0x4000u
#define VIDEO_MAX_IMPORTS 256u
#define VIDEO_WAIT_US UINT64_C(10000000)
#define VIDEO_POLL_US UINT64_C(100000)

extern const uint8_t sm_ps4_video_start[], sm_ps4_video_end[];
extern const uint8_t sm_ps4_video_open[], sm_ps4_video_buffers[];
extern const uint8_t sm_ps4_video_functions[], sm_ps4_video_result[];
extern const uint8_t sm_ps4_video_loader_start[], sm_ps4_video_loader_end[];
extern const uint8_t sm_ps4_video_loader_dlsym[], sm_ps4_video_loader_exchange[];
extern const uint8_t sm_ps4_video_loader_pid[], sm_ps4_video_loader_once[];
extern const uint8_t sm_ps4_video_loader_open[], sm_ps4_video_loader_buffers[];
extern const uint8_t sm_shellcore_bridge_blob_start[], sm_shellcore_bridge_base_end[];
extern const uint8_t sm_shellcore_bridge_exchange[];
extern const uint8_t sm_shellcore_bridge_socket[], sm_shellcore_bridge_setsockopt[];
extern const uint8_t sm_shellcore_bridge_connect[], sm_shellcore_bridge_read[];
extern const uint8_t sm_shellcore_bridge_write[], sm_shellcore_bridge_close[];

typedef struct {
  uintptr_t address, original, replacement;
} video_import_t;

// Owned exclusively by the lifecycle watcher; only startup is polled, for 10 s.
static struct {
  pid_t pid;
  uint64_t deadline, next_poll;
  uintptr_t result;
  uintptr_t loader_result;
  bool attached;
  bool loader_failed;
  video_import_t *rollback;
  size_t rollback_count;
  video_import_t *loader_imports;
  size_t loader_count;
  char title_id[10];
} g_video;

static bool user_range(uintptr_t address, size_t size) {
  return size && address >= 0x10000 && address < UINT64_C(0x800000000000) &&
         size <= UINT64_C(0x800000000000) - address;
}

static bool kernel_pointer(uintptr_t address) {
  return address >= UINT64_C(0xffff800000000000);
}

static bool write_verified(uintptr_t address, const void *data, size_t size) {
  uint8_t actual[512];
  return size <= sizeof(actual) &&
         sm_remote_process_write(g_video.pid, address, data, size) &&
         sm_remote_process_read(g_video.pid, address, actual, size) &&
         !memcmp(actual, data, size);
}

// Inspect real ELF relocation slots, never arbitrary matching words in game data.
static bool collect_table(const dynlib_obj_t *object, uintptr_t table, size_t bytes,
                           const uintptr_t functions[4], video_import_t *imports,
                           size_t *count) {
  typedef struct { uint64_t offset, info; int64_t addend; } relocation_t;
  if (!bytes) return true;
  if (!kernel_pointer(table) || bytes > 4u * 1024u * 1024u ||
      bytes % sizeof(relocation_t) || table > UINTPTR_MAX - bytes)
    return false;
  relocation_t entries[32];
  uint8_t slots[0x1000];
  uintptr_t cached_address = 0;
  size_t cached_size = 0;
  for (size_t offset = 0; offset < bytes;) {
    size_t size = bytes - offset;
    if (size > sizeof(entries)) size = sizeof(entries);
    if (kernel_copyout(table + offset, entries, size) != 0) return false;
    for (size_t i = 0; i < size / sizeof(entries[0]); ++i) {
      const relocation_t *entry = &entries[i];
      uint32_t type = (uint32_t)entry->info;
      if (type != 6 && type != 7) continue; // GLOB_DAT / JUMP_SLOT
      if (entry->offset > object->mapsize || object->mapsize - entry->offset < 8)
        return false;
      uintptr_t address = object->mapbase + entry->offset, original;
      if (address & 7) return false;
      if (address < cached_address || address - cached_address >= cached_size) {
        cached_address = address & ~(uintptr_t)(sizeof(slots) - 1);
        if (cached_address < object->mapbase) cached_address = object->mapbase;
        cached_size = object->mapbase + object->mapsize - cached_address;
        if (cached_size > sizeof(slots)) cached_size = sizeof(slots);
        if (!sm_remote_process_read(g_video.pid, cached_address, slots, cached_size))
          return false;
      }
      if (sizeof(original) > cached_size - (address - cached_address)) return false;
      memcpy(&original, slots + address - cached_address, sizeof(original));
      if (original != functions[0] &&
          (!functions[1] || original != functions[1])) continue;
      bool duplicate = false;
      for (size_t j = 0; j < *count; ++j)
        if (imports[j].address == address) duplicate = true;
      if (duplicate) continue;
      if (*count == VIDEO_MAX_IMPORTS) return false;
      imports[*count] = (video_import_t){address, original, original == functions[0] ? 0 : 1};
      ++*count;
    }
    offset += size;
  }
  return true;
}

static bool collect_imports(const uintptr_t functions[4], uint32_t exclude_handle,
                             video_import_t *imports, size_t *count) {
  dynlib_obj_t object;
  if (kernel_dynlib_obj(g_video.pid, 0, &object) != 0) return false;
  for (unsigned n = 0; n < 256; ++n) {
    if (!user_range(object.mapbase, object.mapsize)) return false;
    if (object.handle != exclude_handle && object.dynsec) {
      sm_dynlib_relocations_t dynamic;
      if (!kernel_pointer(object.dynsec) ||
          kernel_copyout(object.dynsec, &dynamic, sizeof(dynamic)) != 0 ||
          !collect_table(&object, dynamic.pltrela, dynamic.pltrelasize, functions, imports, count) ||
          !collect_table(&object, dynamic.rela, dynamic.relasize, functions, imports, count))
        return false;
    }
    if (!object.next) return true;
    if (!kernel_pointer(object.next) ||
        kernel_copyout(object.next, &object, sizeof(object)) != 0) return false;
  }
  return false;
}

static bool detach_game(void) {
  if (!g_video.attached) return true;
  if (!sm_remote_process_detach(g_video.pid) && !sm_remote_process_detach(g_video.pid))
    return false;
  g_video.attached = false;
  return true;
}

static bool restore_imports(video_import_t *imports, size_t count, bool rollback) {
  bool restored = true;
  for (size_t i = 0; i < count; ++i) {
    if (!rollback) {
      uintptr_t current;
      if (!sm_remote_process_read(g_video.pid, imports[i].address,
                                  &current, sizeof(current))) {
        restored = false;
        continue;
      }
      // Keep a later owner's hook: it may still chain through our mapping.
      if (current != imports[i].replacement) continue;
    }
    if (!write_verified(imports[i].address, &imports[i].original, sizeof(uintptr_t)))
      restored = false;
  }
  return restored;
}

static bool set_loader_value(uint8_t *code, size_t code_size,
                             const uint8_t *symbol, const uint8_t *base,
                             uint64_t value, size_t value_size) {
  uintptr_t offset = (uintptr_t)symbol - (uintptr_t)base;
  if (offset > code_size || value_size > code_size - offset)
    return false;
  memcpy(code + offset, &value, value_size);
  return true;
}

static bool resolve_loader_ipc(uint32_t handle, uint8_t *code,
                               size_t bridge_size) {
  static const struct {
    const uint8_t *slot;
    const char *name;
  } imports[] = {
      {sm_shellcore_bridge_socket, "socket"},
      {sm_shellcore_bridge_setsockopt, "setsockopt"},
      {sm_shellcore_bridge_connect, "connect"},
      {sm_shellcore_bridge_read, "read"},
      {sm_shellcore_bridge_write, "write"},
      {sm_shellcore_bridge_close, "close"},
  };
  for (size_t i = 0; i < sizeof(imports) / sizeof(imports[0]); ++i) {
    uintptr_t value = kernel_dynlib_dlsym(g_video.pid, handle, imports[i].name);
    if (!user_range(value, 1) ||
        !set_loader_value(code, bridge_size, imports[i].slot,
                          sm_shellcore_bridge_blob_start, value, sizeof(value)))
      return false;
  }
  return true;
}

// The dlsym hook covers direct VideoOut pointers and signals the lifecycle
// watcher when a game resolves VideoOut after its initial import scan.
static int install_loader_signal(void) {
  static const char *const libraries[] = {
      "libkernel_sys.sprx", "libkernel.sprx", "libkernel_web.sprx"};
  uint32_t kernel_handle = 0;
  uintptr_t dlsym = 0;
  for (size_t i = 0; i < sizeof(libraries) / sizeof(libraries[0]); ++i) {
    if (kernel_dynlib_handle(g_video.pid, libraries[i], &kernel_handle) == 0) {
      dlsym = kernel_dynlib_dlsym(g_video.pid, kernel_handle,
                                  "sceKernelDlsym");
      if (user_range(dlsym, 1))
        break;
    }
  }
  if (!user_range(dlsym, 1))
    return 0;
  uintptr_t functions[4] = {dlsym, 0, 0, 0};
  video_import_t *imports = calloc(VIDEO_MAX_IMPORTS, sizeof(*imports));
  if (!imports)
    return -1;
  size_t count = 0;
  if (!collect_imports(functions, kernel_handle, imports, &count) || !count) {
    free(imports);
    return 0;
  }
  if (!sm_remote_process_attach(g_video.pid)) {
    free(imports);
    return -1;
  }
  g_video.attached = true;
  int result = -1;
  size_t attempted = 0;
  uintptr_t code_address = 0;
  count = 0;
  if (!collect_imports(functions, kernel_handle, imports, &count) || !count)
    goto done;
  size_t loader_size = (size_t)(sm_ps4_video_loader_end -
                                 sm_ps4_video_loader_start);
  size_t bridge_size = (size_t)(sm_shellcore_bridge_base_end -
                                 sm_shellcore_bridge_blob_start);
  size_t video_size = (size_t)(sm_ps4_video_end - sm_ps4_video_start);
  if (loader_size > SM_PS4_VIDEO_LOADER_CODE_OFFSET ||
      video_size > SM_PS4_VIDEO_LOADER_IPC_OFFSET -
                       SM_PS4_VIDEO_LOADER_CODE_OFFSET ||
      bridge_size > VIDEO_PAGE_SIZE - SM_PS4_VIDEO_LOADER_IPC_OFFSET)
    goto done;
  uint8_t code[SM_PS4_VIDEO_LOADER_IPC_OFFSET +
               SM_SHELLCORE_BRIDGE_MAX_BLOB_SIZE] = {0};
  size_t code_size = SM_PS4_VIDEO_LOADER_IPC_OFFSET + bridge_size;
  memcpy(code, sm_ps4_video_loader_start, loader_size);
  memcpy(code + SM_PS4_VIDEO_LOADER_CODE_OFFSET, sm_ps4_video_start,
         video_size);
  memcpy(code + SM_PS4_VIDEO_LOADER_IPC_OFFSET,
         sm_shellcore_bridge_blob_start,
         bridge_size);
  if (!resolve_loader_ipc(kernel_handle,
                          code + SM_PS4_VIDEO_LOADER_IPC_OFFSET,
                          bridge_size) ||
      !sm_kstuff_remote_mmap(g_video.pid, VIDEO_PAGE_SIZE * 2u,
                             &code_address) ||
      !sm_kstuff_remote_mlock(g_video.pid, code_address, VIDEO_PAGE_SIZE * 2u))
    goto done;
  if (!set_loader_value(code, loader_size, sm_ps4_video_loader_dlsym,
                        sm_ps4_video_loader_start, dlsym, sizeof(dlsym)) ||
      !set_loader_value(code, loader_size, sm_ps4_video_loader_exchange,
                        sm_ps4_video_loader_start,
                        code_address + SM_PS4_VIDEO_LOADER_IPC_OFFSET +
                            (sm_shellcore_bridge_exchange -
                             sm_shellcore_bridge_blob_start), sizeof(uintptr_t)) ||
      !set_loader_value(code, loader_size, sm_ps4_video_loader_pid,
                        sm_ps4_video_loader_start, (uint32_t)g_video.pid,
                        sizeof(uint32_t)) ||
      !set_loader_value(code, loader_size, sm_ps4_video_loader_once,
                        sm_ps4_video_loader_start,
                        code_address + VIDEO_PAGE_SIZE, sizeof(uintptr_t)) ||
      !set_loader_value(code, loader_size, sm_ps4_video_loader_open,
                        sm_ps4_video_loader_start,
                        code_address + SM_PS4_VIDEO_LOADER_CODE_OFFSET +
                            (sm_ps4_video_open - sm_ps4_video_start),
                        sizeof(uintptr_t)) ||
      !set_loader_value(code, loader_size, sm_ps4_video_loader_buffers,
                        sm_ps4_video_loader_start,
                        code_address + SM_PS4_VIDEO_LOADER_CODE_OFFSET +
                            (sm_ps4_video_buffers - sm_ps4_video_start),
                        sizeof(uintptr_t)) ||
      !set_loader_value(code + SM_PS4_VIDEO_LOADER_CODE_OFFSET, video_size,
                        sm_ps4_video_functions, sm_ps4_video_start,
                        code_address + VIDEO_PAGE_SIZE +
                            SM_PS4_VIDEO_LOADER_FUNCTIONS_OFFSET,
                        sizeof(uintptr_t)) ||
      !set_loader_value(code + SM_PS4_VIDEO_LOADER_CODE_OFFSET, video_size,
                        sm_ps4_video_result, sm_ps4_video_start,
                        code_address + VIDEO_PAGE_SIZE +
                            SM_PS4_VIDEO_LOADER_STATUS_OFFSET,
                        sizeof(uintptr_t)))
    goto done;
  uint8_t actual[sizeof(code)];
  uint8_t initial[SM_PS4_VIDEO_LOADER_STATUS_OFFSET + sizeof(int32_t) * 4u] = {0};
  if (!sm_remote_process_copyin(g_video.pid, code_address, code, code_size) ||
      !sm_remote_process_read(g_video.pid, code_address, actual, code_size) ||
      memcmp(actual, code, code_size) ||
      !sm_remote_process_copyin(g_video.pid, code_address + VIDEO_PAGE_SIZE,
                                initial, sizeof(initial)) ||
      !sm_remote_process_read(g_video.pid, code_address + VIDEO_PAGE_SIZE,
                              actual, sizeof(initial)) ||
      memcmp(actual, initial, sizeof(initial)) ||
      !sm_kstuff_remote_mprotect(g_video.pid, code_address, VIDEO_PAGE_SIZE,
                                PROT_READ | PROT_EXEC))
    goto done;
  for (size_t i = 0; i < count; ++i) {
    if (!sm_kstuff_remote_mlock(g_video.pid, imports[i].address,
                               sizeof(uintptr_t)))
      goto done;
    attempted = i + 1;
    imports[i].replacement = code_address;
    if (!write_verified(imports[i].address, &code_address,
                        sizeof(code_address)))
      goto done;
  }
  g_video.loader_imports = imports;
  g_video.loader_count = count;
  g_video.loader_result = code_address + VIDEO_PAGE_SIZE +
                          SM_PS4_VIDEO_LOADER_STATUS_OFFSET;
  imports = NULL;
  result = 1;
done:
  if (result < 0 && !restore_imports(imports, attempted, true)) {
    g_video.rollback = imports;
    g_video.rollback_count = attempted;
    log_debug("  [PS4] CRITICAL: loader hook rollback failed; pid=%ld kept stopped",
              (long)g_video.pid);
    return -1;
  }
  if (result < 0 && code_address != 0 &&
      !sm_kstuff_remote_munmap(g_video.pid, code_address, VIDEO_PAGE_SIZE * 2u))
    log_debug("  [PS4] failed to release loader bridge: pid=%ld",
              (long)g_video.pid);
  if (!detach_game())
    result = -1;
  free(imports);
  return result;
}

// 0: modules not ready, 1: installed, -1: failed (no automatic patch retries).
static int install_video_hooks(void) {
  static const char *const names[] = {"sceVideoOutOpen", "sceVideoOutRegisterBuffers",
                                     "sceVideoOutIsOutputSupported", "sceVideoOutConfigureOutput"};
  uint32_t handle;
  if (kernel_dynlib_handle(g_video.pid, "libSceVideoOut.sprx", &handle) != 0)
    return 0;
  uintptr_t functions[4];
  for (size_t i = 0; i < 4; ++i) {
    functions[i] = kernel_dynlib_dlsym(g_video.pid, handle, names[i]);
    if (!user_range(functions[i], 1)) return 0;
  }
  video_import_t *imports = calloc(VIDEO_MAX_IMPORTS + 4u, sizeof(*imports));
  if (!imports) return -1;
  size_t count = 0, attempted = 0;
  if (!collect_imports(functions, handle, imports, &count)) { free(imports); return -1; }
  if (!count) { free(imports); return 0; }
  if (!sm_remote_process_attach(g_video.pid)) { free(imports); return -1; }
  g_video.attached = true;
  count = 0; // Recheck the relocation snapshot after the process is stopped.
  int result = -1;
  uintptr_t code_address = 0;
  bool own_mapping = false;
  if (!collect_imports(functions, handle, imports, &count)) goto done;
  if (!count) { result = 0; goto done; }
  uintptr_t data_address, status_address;
  size_t function_slots = 0;
  if (g_video.loader_imports) {
    uintptr_t loader = g_video.loader_imports[0].replacement;
    code_address = loader + SM_PS4_VIDEO_LOADER_CODE_OFFSET;
    data_address = loader + VIDEO_PAGE_SIZE + SM_PS4_VIDEO_LOADER_FUNCTIONS_OFFSET;
    status_address = loader + VIDEO_PAGE_SIZE + SM_PS4_VIDEO_LOADER_STATUS_OFFSET;
    uintptr_t previous[4];
    if (!sm_remote_process_read(g_video.pid, data_address, previous, sizeof(previous)))
      goto done;
    for (size_t i = 0; i < 4; ++i)
      if (previous[i] && previous[i] != functions[i]) goto done;
    // The existing dlsym wrappers may already use this table. Include every
    // table write in the same stopped-process rollback as the GOT slots.
    memmove(imports + 4, imports, count * sizeof(*imports));
    for (size_t i = 0; i < 4; ++i)
      imports[i] = (video_import_t){data_address + i * sizeof(uintptr_t), previous[i], functions[i]};
    count += 4;
    function_slots = 4;
  } else {
    size_t code_size = (uintptr_t)sm_ps4_video_end - (uintptr_t)sm_ps4_video_start;
    uint8_t code[512], actual[512];
    if (code_size > sizeof(code) ||
        !sm_kstuff_remote_mmap(g_video.pid, VIDEO_PAGE_SIZE * 2u, &code_address)) goto done;
    own_mapping = true;
    if (!sm_kstuff_remote_mlock(g_video.pid, code_address, VIDEO_PAGE_SIZE * 2u))
      goto done;
    memcpy(code, sm_ps4_video_start, code_size);
    data_address = code_address + VIDEO_PAGE_SIZE;
    memcpy(code + (sm_ps4_video_functions - sm_ps4_video_start),
           &data_address, sizeof(data_address));
    status_address = data_address + sizeof(functions);
    memcpy(code + (sm_ps4_video_result - sm_ps4_video_start), &status_address, sizeof(status_address));
    // Materialize anonymous pages before the physical write path on 8.40+.
    if (!sm_remote_process_copyin(g_video.pid, code_address, code, code_size) ||
        !sm_remote_process_read(g_video.pid, code_address, actual, code_size) ||
        memcmp(actual, code, code_size) ||
        !sm_kstuff_remote_mprotect(g_video.pid, code_address, VIDEO_PAGE_SIZE, PROT_READ | PROT_EXEC))
      goto done;
    uint8_t initial[sizeof(functions) + sizeof(int32_t) * 4u] = {0};
    memcpy(initial, functions, sizeof(functions));
    if (!sm_remote_process_copyin(g_video.pid, data_address, initial, sizeof(initial)) ||
        !sm_remote_process_read(g_video.pid, data_address, actual, sizeof(initial)) ||
        memcmp(actual, initial, sizeof(initial)))
      goto done;
  }
  for (size_t i = 0; i < count; ++i) {
    uintptr_t replacement = imports[i].replacement;
    if (i >= function_slots) {
      if (!sm_kstuff_remote_mlock(g_video.pid, imports[i].address,
                                 sizeof(uintptr_t)))
        goto done;
      replacement = code_address + (imports[i].replacement
          ? sm_ps4_video_buffers - sm_ps4_video_start : sm_ps4_video_open - sm_ps4_video_start);
    }
    imports[i].replacement = replacement;
    if (i < function_slots && imports[i].original == replacement) continue;
    attempted = i + 1;
    if (!write_verified(imports[i].address, &replacement, sizeof(replacement))) goto done;
  }
  g_video.result = status_address;
  g_video.loader_result = 0;
  result = 1;
done:
  if (result < 0 && !restore_imports(imports, attempted, true)) {
    // A torn import must not execute. Keep both the stopped PID and undo data
    // for lifecycle shutdown; reset retries recovery before it may detach.
    g_video.rollback = imports;
    g_video.rollback_count = attempted;
    log_debug("  [PS4] CRITICAL: 120Hz rollback failed; pid=%ld kept stopped for recovery",
              (long)g_video.pid);
    return -1;
  }
  if (result < 0 && own_mapping &&
      !sm_kstuff_remote_munmap(g_video.pid, code_address, VIDEO_PAGE_SIZE * 2u))
    log_debug("  [PS4] failed to release unused 120Hz bridge: pid=%ld",
              (long)g_video.pid);
  // Wrappers belong to the process; retain the allocation until process exit.
  if (!detach_game()) {
    log_debug("  [PS4] 120Hz detach failed: pid=%ld retained for shutdown recovery", (long)g_video.pid);
    result = -1;
  }
  free(imports);
  return result;
}

void sm_ps4_video_reset(void) {
  bool exited = g_video.pid > 0 && kill(g_video.pid, 0) != 0 && errno == ESRCH;
  app_info_t info = {0};
  if (!exited && !g_video.attached && g_video.pid > 0 && g_video.title_id[0] &&
      sceKernelGetAppInfo(g_video.pid, &info) == 0 &&
      memcmp(info.title_id, g_video.title_id, 9))
    exited = true;
  if (!exited && g_video.rollback &&
      !restore_imports(g_video.rollback, g_video.rollback_count, true)) {
    g_video.next_poll = 0;
    log_debug("  [PS4] CRITICAL: 120Hz rollback recovery failed; reboot required for pid=%ld",
              (long)g_video.pid);
    return;
  }
  free(g_video.rollback);
  g_video.rollback = NULL;
  g_video.rollback_count = 0;
  if (!exited && g_video.loader_imports) {
    if (!g_video.attached && !sm_remote_process_attach(g_video.pid)) {
      g_video.next_poll = 0;
      log_debug("  [PS4] loader hook cleanup attach failed: pid=%ld",
                (long)g_video.pid);
      return;
    }
    g_video.attached = true;
    if (!restore_imports(g_video.loader_imports, g_video.loader_count, false)) {
      g_video.next_poll = 0;
      log_debug("  [PS4] CRITICAL: loader hook cleanup failed; pid=%ld kept stopped",
                (long)g_video.pid);
      return;
    }
  }
  free(g_video.loader_imports);
  g_video.loader_imports = NULL;
  g_video.loader_count = 0;
  if (!exited && !detach_game()) {
    g_video.next_poll = 0;
    log_debug("  [PS4] 120Hz detach recovery failed: pid=%ld", (long)g_video.pid);
    return;
  }
  memset(&g_video, 0, sizeof(g_video));
}

void sm_ps4_video_on_exec(pid_t pid, const char *title_id) {
  if (pid == g_video.pid && title_id && !strncmp(title_id, g_video.title_id, 10)) return;
  sm_ps4_video_reset();
  if (g_video.pid) return;
  uint64_t revision;
  if (pid <= 0 || !(sm_config_get_title_ps4_policy(title_id, &revision) & 32u)) return;
  uint64_t now = monotonic_time_us();
  if (!now) return;
  g_video.pid = pid;
  memcpy(g_video.title_id, title_id, 9);
  g_video.deadline = now + VIDEO_WAIT_US;
  g_video.next_poll = now;
  sm_ps4_video_poll();
}

void sm_ps4_video_note_module_ready(pid_t pid) {
  if (pid != g_video.pid || g_video.attached || g_video.rollback)
    return;
  uint64_t now = monotonic_time_us();
  if (now) {
    if (!g_video.next_poll) g_video.deadline = now + VIDEO_WAIT_US;
    g_video.next_poll = now;
  }
}

static bool log_video_result(uintptr_t address, const char *origin) {
  int32_t status[4];
  if (!sm_remote_process_read(g_video.pid, address, status, sizeof(status)) ||
      !status[2])
    return false;
  if (status[2] == 1)
    log_debug("  [PS4] %s120Hz unavailable: %s handle=%#x supported=%#x; "
              "ConfigureOutput not called", origin, g_video.title_id,
              (unsigned)status[3], (unsigned)status[0]);
  else
    log_debug("  [PS4] %s120Hz %s: %s handle=%#x supported=%#x configure=%#x",
              origin, status[1] == 0 ? "configured" : "not enabled",
              g_video.title_id, (unsigned)status[3], (unsigned)status[0],
              (unsigned)status[1]);
  return true;
}

void sm_ps4_video_poll(void) {
  if (!g_video.next_poll) return;
  uint64_t now = monotonic_time_us();
  if (!now || now < g_video.next_poll) return;
  if (kill(g_video.pid, 0) != 0 && errno == ESRCH) { sm_ps4_video_reset(); return; }
  app_info_t info = {0};
  if (sceKernelGetAppInfo(g_video.pid, &info) != 0) {
    if (now >= g_video.deadline) g_video.next_poll = 0;
    else g_video.next_poll = now + VIDEO_POLL_US;
    return;
  }
  if (memcmp(info.title_id, g_video.title_id, 9)) { sm_ps4_video_reset(); return; }
  if (!g_video.loader_imports && !g_video.loader_failed && !g_video.attached) {
    int loader = install_loader_signal();
    if (loader > 0)
      log_debug("  [PS4] VideoOut loader hook armed: %s pid=%ld",
                g_video.title_id, (long)g_video.pid);
    else if (loader < 0) {
      g_video.loader_failed = true;
      log_debug("  [PS4] VideoOut loader hook failed: %s pid=%ld",
                g_video.title_id, (long)g_video.pid);
      if (g_video.attached) { g_video.next_poll = 0; return; }
    }
  }
  if (!g_video.result) {
    int rc = install_video_hooks();
    if (rc < 0) {
      log_debug("  [PS4] 120Hz installation failed: %s pid=%ld", g_video.title_id, (long)g_video.pid);
      g_video.next_poll = 0; return;
    }
    if (rc > 0)
      log_debug("  [PS4] 120Hz VideoOut hooks installed: %s pid=%ld", g_video.title_id, (long)g_video.pid);
  }
  if (g_video.result && log_video_result(g_video.result, "")) {
    g_video.next_poll = 0;
    return;
  }
  if (g_video.loader_result && log_video_result(g_video.loader_result, "loader "))
    g_video.loader_result = 0;
  if (now >= g_video.deadline) {
    log_debug("  [PS4] 120Hz startup timeout: %s (%s)", g_video.title_id,
              g_video.result ? "no VideoOut call observed; hooks retained" :
              g_video.loader_imports ? "loader hook retained; no static imports" :
                                       "imports not ready");
    g_video.next_poll = 0; return;
  }
  g_video.next_poll = now + VIDEO_POLL_US;
}

uint64_t sm_ps4_video_next_wake_us(void) {
  return g_video.next_poll;
}
