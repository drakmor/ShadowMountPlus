#include "sm_platform.h"

#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

#include "sm_limits.h"
#include "sm_log.h"
#include "sm_runtime.h"
#include "sm_shellcore_hooks.h"
#include "sm_shellcore_protocol_defs.h"
#include "sm_shellcore_remote.h"

#define SHELLCORE_BASE_HOOK_COUNT 2u
#define SHELLCORE_MAX_HOOK_COUNT 3u
#define MAX_HOOK_PROLOGUE_SIZE SM_SHELLCORE_BRIDGE_MAX_PROLOGUE_SIZE
#define ABSOLUTE_JUMP_SIZE SM_SHELLCORE_BRIDGE_ABSOLUTE_JUMP_SIZE
#define TRAMPOLINE_SIZE SM_SHELLCORE_BRIDGE_TRAMPOLINE_SIZE
#define BRIDGE_SIGNATURE_SIZE 64u

static const uint8_t k_expected_function_prologue[] = {0x55, 0x48, 0x89, 0xe5};

_Static_assert(MAX_TITLE_ID == SM_SHELLCORE_REQUEST_TITLE_ID_SIZE,
               "ShellCore bridge title id size mismatch");
_Static_assert(SM_SHELLCORE_BRIDGE_INSTALL_STRING_SIZE % sizeof(uint64_t) == 0,
               "ShellCore bridge install string alignment mismatch");
_Static_assert(sizeof(SM_SHELLCORE_SOCKET_PATH) + 2 ==
                   SM_SHELLCORE_BRIDGE_SOCKADDR_SIZE,
               "ShellCore bridge socket path encoding mismatch");
_Static_assert(TRAMPOLINE_SIZE ==
                   MAX_HOOK_PROLOGUE_SIZE +
                       SM_SHELLCORE_BRIDGE_RELATIVE_JUMP_SIZE,
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
extern const uint8_t sm_shellcore_bridge_base_end[];
extern const uint8_t sm_shellcore_bridge_blob_end[];
extern const uint8_t sm_shellcore_bridge_launch_hook[];
extern const uint8_t sm_shellcore_bridge_sandbox_hook[];
extern const uint8_t sm_shellcore_bridge_sandbox_original[];
extern const uint8_t sm_shellcore_bridge_install_all_hook[];
extern const uint8_t sm_shellcore_bridge_launch_trampoline[];
extern const uint8_t sm_shellcore_bridge_install_all_trampoline[];
extern const uint8_t sm_shellcore_bridge_install_title_dir[];
extern const uint8_t sm_shellcore_bridge_install_armed[];
extern const uint8_t sm_shellcore_bridge_install_title_id_0[];
extern const uint8_t sm_shellcore_bridge_install_title_id_1[];
extern const uint8_t sm_shellcore_bridge_install_dir_0[];
extern const uint8_t sm_shellcore_bridge_install_dir_1[];
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

static const uint8_t *const k_install_title_slots[] = {
    sm_shellcore_bridge_install_title_id_0,
    sm_shellcore_bridge_install_title_id_1,
};

static const uint8_t *const k_install_dir_slots[] = {
    sm_shellcore_bridge_install_dir_0,
    sm_shellcore_bridge_install_dir_1,
};

static pid_t find_shellcore_pid(void);

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

static bool write_remote_bridge_chunks(
    pid_t pid, const uint8_t *const *slots, size_t slot_count,
    const uint8_t *bytes, size_t size) {
  if (!slots || !bytes || size != slot_count * sizeof(uint64_t))
    return false;
  for (size_t i = 0; i < slot_count; ++i) {
    if (!sm_remote_process_write(pid, remote_bridge_symbol(slots[i]),
                                 bytes + i * sizeof(uint64_t),
                                 sizeof(uint64_t))) {
      return false;
    }
  }
  return true;
}

static void build_absolute_jump(uint8_t jump[ABSOLUTE_JUMP_SIZE],
                                uintptr_t destination) {
  jump[0] = 0x48;
  jump[1] = 0xb8;
  memcpy(jump + 2, &destination, sizeof(destination));
  jump[10] = 0xff;
  jump[11] = 0xe0;
}

static bool build_relative_jump(
    uint8_t jump[SM_SHELLCORE_BRIDGE_RELATIVE_JUMP_SIZE], uintptr_t source,
    uintptr_t destination) {
  int64_t wide_displacement =
      (int64_t)destination -
      (int64_t)(source + SM_SHELLCORE_BRIDGE_RELATIVE_JUMP_SIZE);
  int32_t displacement = (int32_t)wide_displacement;
  if ((int64_t)displacement != wide_displacement)
    return false;
  jump[0] = 0xe9;
  memcpy(jump + 1, &displacement, sizeof(displacement));
  return true;
}

static bool build_relative_call(
    uint8_t call[SM_SHELLCORE_BRIDGE_RELATIVE_JUMP_SIZE], uintptr_t source,
    uintptr_t destination) {
  if (!build_relative_jump(call, source, destination))
    return false;
  call[0] = 0xe8;
  return true;
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

static bool patch_remote_call(pid_t pid, uintptr_t source,
                              uintptr_t destination) {
  uint8_t patch[SM_SHELLCORE_BRIDGE_RELATIVE_JUMP_SIZE];
  uint8_t verify[SM_SHELLCORE_BRIDGE_RELATIVE_JUMP_SIZE];
  if (!build_relative_call(patch, source, destination) ||
      !sm_remote_process_write(pid, source, patch, sizeof(patch))) {
    return false;
  }
  return sm_remote_process_read(pid, source, verify, sizeof(verify)) &&
         memcmp(verify, patch, sizeof(patch)) == 0;
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

static bool remote_range_is_zero(pid_t pid, uintptr_t address, size_t size) {
  uint8_t buffer[256];
  while (size != 0) {
    size_t chunk = size < sizeof(buffer) ? size : sizeof(buffer);
    if (!sm_remote_process_read(pid, address, buffer, chunk))
      return false;
    for (size_t i = 0; i < chunk; ++i) {
      if (buffer[i] != 0)
        return false;
    }
    address += chunk;
    size -= chunk;
  }
  return true;
}

static bool bridge_cave_is_available(pid_t pid, uintptr_t bridge_address,
                                     size_t bridge_size) {
  if (bridge_size < BRIDGE_SIGNATURE_SIZE)
    return false;
  return remote_range_is_zero(pid, bridge_address, bridge_size) ||
         verify_remote_bytes(pid, bridge_address,
                             sm_shellcore_bridge_blob_start,
                             BRIDGE_SIGNATURE_SIZE);
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

static bool parse_absolute_jump(const uint8_t *patch, size_t patch_size,
                                uintptr_t *destination_out) {
  if (!patch || !destination_out || patch_size < ABSOLUTE_JUMP_SIZE ||
      patch[0] != 0x48 || patch[1] != 0xb8 || patch[10] != 0xff ||
      patch[11] != 0xe0) {
    return false;
  }
  for (size_t i = ABSOLUTE_JUMP_SIZE; i < patch_size; ++i) {
    if (patch[i] != 0x90)
      return false;
  }
  memcpy(destination_out, patch + 2, sizeof(*destination_out));
  return true;
}

static bool parse_relative_jump(const uint8_t *jump, uintptr_t source,
                                uintptr_t *destination_out) {
  if (!jump || !destination_out || jump[0] != 0xe9)
    return false;
  int32_t displacement = 0;
  memcpy(&displacement, jump + 1, sizeof(displacement));
  *destination_out = source + SM_SHELLCORE_BRIDGE_RELATIVE_JUMP_SIZE +
                     (intptr_t)displacement;
  return true;
}

static bool parse_relative_call(const uint8_t *call, uintptr_t source,
                                uintptr_t *destination_out) {
  if (!call || call[0] != 0xe8)
    return false;
  int32_t displacement = 0;
  memcpy(&displacement, call + 1, sizeof(displacement));
  *destination_out = source + SM_SHELLCORE_BRIDGE_RELATIVE_JUMP_SIZE +
                     (intptr_t)displacement;
  return true;
}

static bool remote_call_matches(pid_t pid, uintptr_t source,
                                uintptr_t destination) {
  uint8_t patch[SM_SHELLCORE_BRIDGE_RELATIVE_JUMP_SIZE];
  return build_relative_call(patch, source, destination) &&
         verify_remote_bytes(pid, source, patch, sizeof(patch));
}

/*
 * A forced payload replacement can bypass our shutdown path while leaving
 * ShellCore alive. Recover only a bridge with this exact code signature and
 * valid embedded trampolines; unknown hooks remain untouched.
 */
static bool recover_stale_bridge(
    pid_t pid, const sm_shellcore_remote_t *remote,
    const shellcore_hook_record_t hooks[SHELLCORE_MAX_HOOK_COUNT],
    size_t hook_count, uintptr_t bridge_address, size_t bridge_size) {
  static const uint8_t *const hook_symbols[SHELLCORE_MAX_HOOK_COUNT] = {
      sm_shellcore_bridge_launch_hook,
      sm_shellcore_bridge_sandbox_hook,
      sm_shellcore_bridge_install_all_hook,
  };
  static const uint8_t *const trampoline_symbols[SHELLCORE_MAX_HOOK_COUNT] = {
      sm_shellcore_bridge_launch_trampoline,
      NULL,
      sm_shellcore_bridge_install_all_trampoline,
  };
  uint8_t originals[SHELLCORE_MAX_HOOK_COUNT][MAX_HOOK_PROLOGUE_SIZE] = {{0}};
  uint8_t original_sizes[SHELLCORE_MAX_HOOK_COUNT] = {0};
  bool stale[SHELLCORE_MAX_HOOK_COUNT] = {false};
  size_t stale_count = 0;

  for (size_t i = 0; i < hook_count; ++i) {
    sm_shellcore_target_t target = hooks[i].target;
    uintptr_t target_address = remote->targets[target];
    uint8_t patch_size = remote->offsets->targets[target].patch_size;
    uint8_t patch[MAX_HOOK_PROLOGUE_SIZE];
    if (patch_size == 0 || patch_size > MAX_HOOK_PROLOGUE_SIZE ||
        !sm_remote_process_read(pid, target_address, patch, patch_size)) {
      return false;
    }
    size_t hook_offset =
        (size_t)(hook_symbols[i] - sm_shellcore_bridge_blob_start);
    if (target == SM_SHELLCORE_TARGET_SANDBOX_READY) {
      uintptr_t destination = 0;
      uintptr_t original_target =
          remote->image_base + remote->offsets->sandbox_call_target_offset;
      if (patch_size != SM_SHELLCORE_BRIDGE_RELATIVE_JUMP_SIZE ||
          !parse_relative_call(patch, target_address, &destination)) {
        return false;
      }
      if (destination == original_target)
        continue;
      if (destination != bridge_address + hook_offset)
        return false;
      stale[i] = true;
      ++stale_count;
      continue;
    }
    if (patch_size < ABSOLUTE_JUMP_SIZE)
      return false;
    if (memcmp(patch, k_expected_function_prologue,
               sizeof(k_expected_function_prologue)) == 0) {
      continue;
    }

    uintptr_t destination = 0;
    if (!parse_absolute_jump(patch, patch_size, &destination) ||
        destination != bridge_address + hook_offset) {
      return false;
    }
    stale[i] = true;
    ++stale_count;
  }
  if (stale_count == 0)
    return true;

  if (!verify_remote_bytes(pid, bridge_address, sm_shellcore_bridge_blob_start,
                           BRIDGE_SIGNATURE_SIZE)) {
    return false;
  }
  for (size_t i = 0; i < hook_count; ++i) {
    if (!stale[i])
      continue;
    sm_shellcore_target_t target = hooks[i].target;
    uintptr_t target_address = remote->targets[target];
    uint8_t patch_size = remote->offsets->targets[target].patch_size;
    if (target == SM_SHELLCORE_TARGET_SANDBOX_READY) {
      uintptr_t original_target =
          remote->image_base + remote->offsets->sandbox_call_target_offset;
      if (patch_size != SM_SHELLCORE_BRIDGE_RELATIVE_JUMP_SIZE ||
          !build_relative_call(originals[i], target_address,
                               original_target)) {
        return false;
      }
      original_sizes[i] = patch_size;
      continue;
    }
    size_t trampoline_offset =
        (size_t)(trampoline_symbols[i] - sm_shellcore_bridge_blob_start);
    uint8_t trampoline[TRAMPOLINE_SIZE];
    uintptr_t return_address = 0;
    if (trampoline_offset > bridge_size ||
        TRAMPOLINE_SIZE > bridge_size - trampoline_offset ||
        !sm_remote_process_read(pid, bridge_address + trampoline_offset,
                                trampoline, sizeof(trampoline)) ||
        memcmp(trampoline, k_expected_function_prologue,
               sizeof(k_expected_function_prologue)) != 0 ||
        !parse_relative_jump(
            trampoline + patch_size,
            bridge_address + trampoline_offset + patch_size,
            &return_address) ||
        return_address != target_address + patch_size) {
      return false;
    }
    memcpy(originals[i], trampoline, patch_size);
    original_sizes[i] = patch_size;
  }

  for (size_t i = 0; i < hook_count; ++i) {
    if (stale[i] &&
        !restore_remote_bytes(pid, remote->targets[hooks[i].target],
                              originals[i], original_sizes[i])) {
      log_debug("  [SHELLCORE] stale hook recovery incomplete");
      return false;
    }
  }

  log_debug("  [SHELLCORE] stale cave bridge detached: address=0x%lx "
            "size=0x%zx",
            (unsigned long)bridge_address, bridge_size);
  return true;
}

static bool cleanup_remote_bridge(
    pid_t pid, const sm_shellcore_remote_t *remote,
    const shellcore_hook_record_t hooks[SHELLCORE_MAX_HOOK_COUNT],
    size_t hook_count) {
  bool restored = true;
  for (size_t i = 0; i < hook_count; ++i) {
    uintptr_t target_address = remote->targets[hooks[i].target];
    if (!restore_remote_bytes(pid, target_address, hooks[i].original,
                              hooks[i].original_size)) {
      restored = false;
    }
  }
  if (!restored)
    log_debug("  [SHELLCORE] hook rollback incomplete; cave bridge retained");
  return restored;
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
                                      uintptr_t remote_bridge_address,
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
  return build_relative_jump(bridge + offset + original_size,
                             remote_bridge_address + offset + original_size,
                             return_address);
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
  const size_t base_blob_size =
      (size_t)(sm_shellcore_bridge_base_end -
               sm_shellcore_bridge_blob_start);
  const size_t full_blob_size =
      (size_t)(sm_shellcore_bridge_blob_end - sm_shellcore_bridge_blob_start);
  const bool install_hook_enabled = sm_shellcore_install_bridge_enabled();
  const size_t blob_size =
      install_hook_enabled ? full_blob_size : base_blob_size;
  if (base_blob_size < BRIDGE_SIGNATURE_SIZE ||
      full_blob_size <= base_blob_size ||
      full_blob_size > SM_SHELLCORE_BRIDGE_MAX_BLOB_SIZE) {
    log_debug("  [SHELLCORE] invalid bridge layout: base=0x%zx full=0x%zx",
              base_blob_size, full_blob_size);
    return false;
  }
  uint8_t bridge[SM_SHELLCORE_BRIDGE_MAX_BLOB_SIZE] = {0};
  memcpy(bridge, sm_shellcore_bridge_blob_start, blob_size);

  if (!sm_remote_process_attach(pid)) {
    log_debug("  [SHELLCORE] failed to attach to pid=%ld", (long)pid);
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
  hooks.hooks[1].target = SM_SHELLCORE_TARGET_SANDBOX_READY;
  hooks.hooks[2].target = SM_SHELLCORE_TARGET_INSTALL_ALL;

  if (hooks.remote.offsets->bridge_cave_offset >
          UINTPTR_MAX - hooks.remote.image_base ||
      blob_size > hooks.remote.offsets->bridge_cave_size) {
    log_debug("  [SHELLCORE] cave too small: fw=%s need=0x%zx have=0x%zx",
              hooks.remote.offsets->name, blob_size,
              hooks.remote.offsets->bridge_cave_size);
    goto done;
  }
  bridge_address = hooks.remote.image_base +
                   hooks.remote.offsets->bridge_cave_offset;

  if (!recover_stale_bridge(pid, &hooks.remote, hooks.hooks, hook_count,
                            bridge_address, blob_size)) {
    log_debug("  [SHELLCORE] existing hook is not a recoverable bridge");
    goto done;
  }

  for (size_t i = 0; i < hook_count; ++i) {
    shellcore_hook_record_t *hook = &hooks.hooks[i];
    uintptr_t target_address = hooks.remote.targets[hook->target];
    uint8_t patch_size =
        hooks.remote.offsets->targets[hook->target].patch_size;
    if (patch_size == 0 || patch_size > MAX_HOOK_PROLOGUE_SIZE ||
        !sm_remote_process_read(pid, target_address, hook->original,
                                patch_size)) {
      goto done;
    }
    if (hook->target == SM_SHELLCORE_TARGET_SANDBOX_READY) {
      uintptr_t original_target =
          hooks.remote.image_base +
          hooks.remote.offsets->sandbox_call_target_offset;
      uintptr_t decoded_target = 0;
      if (patch_size != SM_SHELLCORE_BRIDGE_RELATIVE_JUMP_SIZE ||
          !parse_relative_call(hook->original, target_address,
                               &decoded_target) ||
          decoded_target != original_target) {
        log_debug("  [SHELLCORE] unexpected call target: %s at 0x%lx",
                  sm_shellcore_target_name(hook->target),
                  (unsigned long)target_address);
        goto done;
      }
      hook->original_size = patch_size;
      continue;
    }
    if (patch_size < ABSOLUTE_JUMP_SIZE)
      goto done;
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

  if (!bridge_cave_is_available(pid, bridge_address, blob_size)) {
    log_debug("  [SHELLCORE] bridge cave is occupied: address=0x%lx "
              "size=0x%zx reserved=0x%zx",
              (unsigned long)bridge_address, blob_size,
              hooks.remote.offsets->bridge_cave_reserved);
    goto done;
  }
  shellcore_hook_record_t *launch_hook_record = &hooks.hooks[0];
  uintptr_t launch_target = hooks.remote.targets[launch_hook_record->target];
  shellcore_hook_record_t *sandbox_hook_record = &hooks.hooks[1];
  uintptr_t sandbox_target = hooks.remote.targets[sandbox_hook_record->target];
  shellcore_hook_record_t *install_hook_record = &hooks.hooks[2];
  uintptr_t install_target = hooks.remote.targets[install_hook_record->target];
  if (!write_embedded_trampoline(
          bridge, blob_size, sm_shellcore_bridge_launch_trampoline,
          launch_hook_record->original, launch_hook_record->original_size,
          bridge_address,
          launch_target + launch_hook_record->original_size)) {
    goto done;
  }
  uintptr_t sandbox_original_target =
      hooks.remote.image_base +
      hooks.remote.offsets->sandbox_call_target_offset;
  if (!set_bridge_pointer(bridge, blob_size,
                          sm_shellcore_bridge_sandbox_original,
                          sandbox_original_target)) {
    goto done;
  }
  if (install_hook_enabled) {
    if (!write_embedded_trampoline(
            bridge, blob_size, sm_shellcore_bridge_install_all_trampoline,
            install_hook_record->original, install_hook_record->original_size,
            bridge_address,
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

  if (!sm_remote_process_write(pid, bridge_address, bridge, blob_size) ||
      !verify_remote_bytes(pid, bridge_address, bridge, blob_size)) {
    log_debug("  [SHELLCORE] failed to populate bridge cave");
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
  uintptr_t sandbox_hook =
      bridge_address + (uintptr_t)(sm_shellcore_bridge_sandbox_hook -
                                   sm_shellcore_bridge_blob_start);
  patched_count = SHELLCORE_BASE_HOOK_COUNT;
  if (!patch_remote_call(pid, sandbox_target, sandbox_hook)) {
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
  hooks.bridge_size = blob_size;
  hooks.hook_count = hook_count;
  hooks.status = SHELLCORE_HOOKS_READY;
  ok = true;

done:
  if (!ok && patched_count != 0) {
    cleanup_pending =
        !cleanup_remote_bridge(pid, &hooks.remote, hooks.hooks, patched_count);
  }
  bool detached = sm_remote_process_detach(pid);
  if (!detached) {
    log_debug("  [SHELLCORE] failed to detach from pid=%ld", (long)pid);
    if (ok) {
      cleanup_pending = !cleanup_remote_bridge(
          pid, &hooks.remote, hooks.hooks, hook_count);
    }
    (void)sm_remote_process_detach(pid);
    ok = false;
  }
  if (ok) {
    g_hooks = hooks;
    uintptr_t launch_trampoline =
        bridge_address +
        (uintptr_t)(sm_shellcore_bridge_launch_trampoline -
                    sm_shellcore_bridge_blob_start);
    log_debug("  [SHELLCORE] hooks installed: fw=%s pid=%ld cave=0x%lx+0x%zx "
              "reserved=0x%zx launch=1 sandbox=1 trampoline=0x%lx install=%d",
              hooks.remote.offsets->name, (long)pid,
              (unsigned long)bridge_address, blob_size,
              hooks.remote.offsets->bridge_cave_reserved,
              (unsigned long)launch_trampoline,
              install_hook_enabled ? 1 : 0);
  } else if (cleanup_pending) {
    hooks.bridge_address = bridge_address;
    hooks.bridge_size = blob_size;
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
  static const uint8_t *const hook_symbols[SHELLCORE_MAX_HOOK_COUNT] = {
      sm_shellcore_bridge_launch_hook,
      sm_shellcore_bridge_sandbox_hook,
      sm_shellcore_bridge_install_all_hook,
  };
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
    const uint8_t *hook_symbol = hook_symbols[i];
    uintptr_t hook_address =
        g_hooks.bridge_address +
        (uintptr_t)(hook_symbol - sm_shellcore_bridge_blob_start);
    bool hook_matches =
        hook->target == SM_SHELLCORE_TARGET_SANDBOX_READY
            ? remote_call_matches(pid, target_address, hook_address)
            : remote_hook_matches(pid, target_address, hook_address,
                                  hook->original_size);
    if (hook_matches) {
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
    log_debug("  [SHELLCORE] hook cleanup incomplete; cave bridge retained");
    pthread_mutex_unlock(&g_install_mutex);
    return;
  }
  if (!found_installed_hook) {
    log_debug("  [SHELLCORE] hooks already absent; stale bridge state dropped");
  } else if (g_hooks.bridge_address) {
    // A thread that entered the bridge before its prologue was restored may
    // still return through it after detach. Keep our fixed cave bytes intact.
    log_debug("  [SHELLCORE] inactive cave bridge retained: address=0x%lx "
              "size=0x%zx",
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

  size_t title_len =
      strnlen(title_id, SM_SHELLCORE_BRIDGE_INSTALL_STRING_SIZE);
  size_t dir_len =
      strnlen(install_dir, SM_SHELLCORE_BRIDGE_INSTALL_STRING_SIZE);
  if (title_len == 0 ||
      title_len >= SM_SHELLCORE_BRIDGE_INSTALL_STRING_SIZE || dir_len == 0 ||
      dir_len >= SM_SHELLCORE_BRIDGE_INSTALL_STRING_SIZE) {
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
                           g_hooks.hooks[2].original_size)) {
    g_hooks.status = SHELLCORE_HOOKS_STALE;
    log_debug("  [SHELLCORE] AppInstallAll hook is no longer installed");
    pthread_mutex_unlock(&g_install_mutex);
    return false;
  }

  uint8_t remote_title[SM_SHELLCORE_BRIDGE_INSTALL_STRING_SIZE] = {0};
  uint8_t remote_dir[SM_SHELLCORE_BRIDGE_INSTALL_STRING_SIZE] = {0};
  memcpy(remote_title, title_id, title_len);
  memcpy(remote_dir, install_dir, dir_len);

  uint8_t armed = 1;
  bool written =
      write_remote_bridge_chunks(
          pid, k_install_title_slots,
          sizeof(k_install_title_slots) / sizeof(k_install_title_slots[0]),
          remote_title, sizeof(remote_title)) &&
      write_remote_bridge_chunks(
          pid, k_install_dir_slots,
          sizeof(k_install_dir_slots) / sizeof(k_install_dir_slots[0]),
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
  armed = 0;
  bool disarmed = sm_remote_process_write(
      pid, remote_bridge_symbol(sm_shellcore_bridge_install_armed), &armed,
      sizeof(armed));
  if (!disarmed) {
    bool isolated = restore_remote_bytes(pid, install_target,
                                         g_hooks.hooks[2].original,
                                         g_hooks.hooks[2].original_size);
    g_hooks.status = isolated ? SHELLCORE_HOOKS_STALE
                              : SHELLCORE_HOOKS_ROLLBACK_PENDING;
    log_debug("  [SHELLCORE] failed to disarm AppInstallAll bridge: %s "
              "hook_restored=%d",
              title_id, isolated ? 1 : 0);
  }
  pthread_mutex_unlock(&g_install_mutex);

  if (!disarmed)
    return false;
  log_debug("  [SHELLCORE] AppInstallAll bridge consumed: %s result=0x%08x",
            title_id, (uint32_t)result);
  *result_out = result;
  return true;
}
