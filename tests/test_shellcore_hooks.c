#include "host_platform.h"
#include "sm_shellcore_remote.h"

static uint8_t memory[65536];
static pid_t current_pid = 100;
static bool attached, stopping, attach_fail, detach_fail, resolve_fail;
static int writes, attaches, dispatches;
static bool bridge_locked;
static bool fail_bridge_lock;
static int bridge_unmaps;
static uintptr_t fail_address;
static int fail_writes;
static bool fail_disarm;
static uint16_t firmware = 0x1340;

static uint32_t kernel_get_fw_version(void) { return (uint32_t)firmware << 16; }
static int kernel_dynlib_handle(pid_t pid, const char *name, uint32_t *handle) {
  assert(pid == current_pid && strcmp(name, "libkernel_sys.sprx") == 0);
  *handle = 1;
  return 0;
}
static uintptr_t kernel_dynlib_dlsym(pid_t pid, uint32_t handle,
                                  const char *name) {
  assert(pid == current_pid && handle == 1 && name);
  return 0x900000;
}
static int sceAppInstUtilAppInstallAll(const char *path);
static int mock_kill(pid_t pid, int sig) {
  assert(pid == current_pid && sig == 0);
  return 0;
}

#define kill mock_kill
#include "shellcore_hooks.inc"
#undef kill

static sm_shellcore_firmware_offsets_t offsets = {
    .firmware = 0x1340, .name = "test", .bridge_cave_offset = 4096,
    .bridge_cave_size = 4096, .sandbox_call_target_offset = 512,
    .targets = {{128, 12}, {256, 5}, {384, 12}, {640, 12}},
};

pid_t find_pid_by_name(const char *name, bool exclude_self) {
  assert(exclude_self && name);
  return current_pid;
}
bool should_stop_requested(void) { return stopping; }
void log_debug(const char *fmt, ...) { assert(fmt); }
const char *sm_shellcore_target_name(sm_shellcore_target_t target) {
  assert(target < SM_SHELLCORE_TARGET_COUNT);
  return "test";
}
bool sm_shellcore_remote_resolve(pid_t pid, sm_shellcore_remote_t *remote) {
  assert(attached && pid == current_pid);
  if (resolve_fail)
    return false;
  *remote = (sm_shellcore_remote_t){.pid = pid, .image_base = 0x100000,
                                  .offsets = &offsets};
  for (size_t i = 0; i < SM_SHELLCORE_TARGET_COUNT; ++i)
    remote->targets[i] = remote->image_base + offsets.targets[i].offset;
  return true;
}
bool sm_remote_process_attach(pid_t pid) {
  assert(pid == current_pid && !attached);
  attaches++;
  if (attach_fail)
    return false;
  attached = true;
  return true;
}
bool sm_remote_process_detach(pid_t pid) {
  assert(pid == current_pid && attached);
  if (detach_fail)
    return false;
  attached = false;
  return true;
}
static size_t memory_offset(pid_t pid, uintptr_t address, size_t size) {
  assert(pid == current_pid && address >= 0x100000);
  size_t offset = address - 0x100000;
  assert(offset <= sizeof(memory) && size <= sizeof(memory) - offset);
  return offset;
}
bool sm_remote_process_read(pid_t pid, uintptr_t address, void *buffer,
                            size_t size) {
  memcpy(buffer, memory + memory_offset(pid, address, size), size);
  return true;
}
bool sm_kstuff_remote_mlock(pid_t pid, uintptr_t address, size_t size) {
  (void)memory_offset(pid, address, size);
  if (address == 0x108000 && size == 0x4000) {
    if (fail_bridge_lock)
      return false;
    bridge_locked = true;
  }
  return true;
}
bool sm_kstuff_remote_mmap(pid_t pid, size_t size, uintptr_t *address) {
  assert(pid == current_pid && attached && size == 0x4000 && address);
  *address = 0x108000;
  return true;
}
bool sm_kstuff_remote_munmap(pid_t pid, uintptr_t address, size_t size) {
  assert(pid == current_pid && attached && address == 0x108000 && size == 0x4000);
  bridge_unmaps++;
  return true;
}
bool sm_kstuff_remote_mprotect(pid_t pid, uintptr_t address, size_t size,
                               int protection) {
  assert(pid == current_pid && address == 0x108000 && size == 0x4000);
  assert(protection == (PROT_READ | PROT_EXEC));
  assert(bridge_locked);
  return true;
}
bool sm_remote_process_copyin(pid_t pid, uintptr_t address, const void *source,
                              size_t size) {
  assert(pid == current_pid && attached && bridge_locked);
  memcpy(memory + memory_offset(pid, address, size), source, size);
  return true;
}
bool sm_remote_process_write(pid_t pid, uintptr_t address, const void *buffer,
                             size_t size) {
  size_t offset = memory_offset(pid, address, size);
  writes++;
  if (fail_disarm && g_hooks.bridge_address &&
      address == remote_bridge_symbol(sm_shellcore_bridge_install_armed) &&
      size == 1 && *(const uint8_t *)buffer == 0) {
    return false;
  }
  // Even a failed write can modify part of the target.
  if (address == fail_address && fail_writes != 0) {
    if (fail_writes > 0)
      fail_writes--;
    memcpy(memory + offset, buffer, 1);
    return false;
  }
  memcpy(memory + offset, buffer, size);
  return true;
}
static int sceAppInstUtilAppInstallAll(const char *path) {
  assert(path == NULL && !attached && hook_installed(current_pid, 2));
  assert(memory[remote_bridge_symbol(sm_shellcore_bridge_install_armed) -
                0x100000] == 1);
  dispatches++;
  return 0x1234;
}

static void prepare_process(void) {
  offsets.firmware = firmware;
  memset(memory, 0, sizeof(memory));
  for (size_t i = 0; i < SM_SHELLCORE_TARGET_COUNT; ++i) {
    uintptr_t address = 0x100000 + offsets.targets[i].offset;
    if (i == SM_SHELLCORE_TARGET_SANDBOX_READY) {
      assert(build_relative_call(memory + offsets.targets[i].offset, address,
                                 0x100000 + offsets.sandbox_call_target_offset));
    } else {
      memcpy(memory + offsets.targets[i].offset, k_expected_function_prologue,
             sizeof(k_expected_function_prologue));
    }
  }
}
static void reset(void) {
  memset(&g_hooks, 0, sizeof(g_hooks));
  attached = stopping = attach_fail = detach_fail = resolve_fail = false;
  fail_disarm = false;
  fail_bridge_lock = false;
  bridge_unmaps = 0;
  bridge_locked = false;
  current_pid = 100;
  fail_address = 0;
  fail_writes = writes = attaches = dispatches = 0;
  prepare_process();
  assert(sm_shellcore_hooks_start());
  assert(g_hooks.enabled && g_hooks.status == SHELLCORE_HOOKS_READY);
  assert(!attached && verify_installed_bridge(current_pid));
  writes = attaches = 0;
}
static void lose_hook(size_t index) {
  const shellcore_hook_record_t *hook = &g_hooks.hooks[index];
  memcpy(memory + offsets.targets[hook->target].offset,
         hook->original, hook->original_size);
}
static void install(void) {
  int result = 0;
  assert(sm_shellcore_install_title_dir("PPSA12345", "/game/sce_sys", &result));
  assert(result == 0x1234 && !attached);
}

int main(void) {
  // Healthy hooks do not attach, patch or rewrite the cave.
  reset();
  assert(sm_shellcore_hooks_refresh());
  assert(writes == 0 && attaches == 0);
  install();
  assert(dispatches == 1);
  // Game switch: restore AppInstallAll with populated mutable title slots.
  lose_hook(2);
  int before = writes;
  install();
  assert(dispatches == 2 && writes == before + 7 && attaches == 1);
  assert(g_hooks.status == SHELLCORE_HOOKS_READY);
  // Rest mode can restore more than just the registration prologue.
  reset();
  for (size_t i = 0; i < 3; ++i)
    lose_hook(i);
  assert(sm_shellcore_hooks_refresh());
  assert(writes == 3 && attaches == 1 && !attached);
  for (size_t i = 0; i < 3; ++i)
    assert(hook_installed(current_pid, i));
  install();
  // A failed attach is retried only by a later explicit operation.
  reset();
  lose_hook(2);
  attach_fail = true;
  assert(!sm_shellcore_hooks_refresh() && writes == 0);
  attach_fail = false;
  install();
  // Unknown target bytes prevent all repairs, even when another hook is lost.
  reset();
  lose_hook(0);
  memory[offsets.targets[SM_SHELLCORE_TARGET_INSTALL_ALL].offset] = 0xcc;
  assert(!sm_shellcore_hooks_refresh() && writes == 0 && !attached);
  assert(g_hooks.status == SHELLCORE_HOOKS_STALE);
  lose_hook(2);
  install();
  // A real disarm failure isolates AppInstallAll and must not re-arm it later.
  reset();
  fail_disarm = true;
  int result;
  assert(!sm_shellcore_install_title_dir("PPSA12345", "/game", &result));
  assert(g_hooks.status == SHELLCORE_HOOKS_STALE && !hook_installed(current_pid, 2));
  fail_disarm = false;
  before = writes;
  assert(!sm_shellcore_install_title_dir("PPSA12345", "/game", &result));
  assert(writes == before && dispatches == 1 && !attached);
  // Corrupt cave code outside the old 64-byte signature is rejected.
  reset();
  lose_hook(2);
  memory[remote_bridge_symbol(sm_shellcore_bridge_install_all_hook) -
         0x100000 + 4] ^= 1;
  assert(!sm_shellcore_hooks_refresh() && writes == 0 && !attached);
  // Never reconnect a bridge which is still armed after an earlier failure.
  reset();
  lose_hook(2);
  memory[remote_bridge_symbol(sm_shellcore_bridge_install_armed) - 0x100000] = 1;
  assert(!sm_shellcore_hooks_refresh() && writes == 0 && !attached);
  // Partial write + successful rollback preserves ownership and allows retry.
  reset();
  lose_hook(2);
  fail_address = g_hooks.remote.targets[SM_SHELLCORE_TARGET_INSTALL_ALL];
  fail_writes = 1;
  assert(!sm_shellcore_hooks_refresh() && writes == 2 && !attached);
  assert(g_hooks.status == SHELLCORE_HOOKS_STALE);
  assert(verify_remote_bytes(current_pid, fail_address, g_hooks.hooks[2].original,
                             g_hooks.hooks[2].original_size));
  install();
  // Failed rollback blocks dispatch; shutdown still owns the partial patch.
  reset();
  lose_hook(2);
  fail_address = g_hooks.remote.targets[SM_SHELLCORE_TARGET_INSTALL_ALL];
  fail_writes = -1;
  assert(!sm_shellcore_hooks_refresh() && attached && g_hooks.attached);
  assert(g_hooks.status == SHELLCORE_HOOKS_ROLLBACK_PENDING);
  before = writes;
  assert(!sm_shellcore_install_title_dir("PPSA12345", "/game", &result));
  assert(writes == before && dispatches == 0);
  fail_writes = 0;
  resolve_fail = true;
  sm_shellcore_hooks_stop();
  assert(attached && g_hooks.attached); // unconfirmed bytes must stay stopped
  resolve_fail = false;
  sm_shellcore_hooks_stop();
  assert(!attached && g_hooks.status == SHELLCORE_HOOKS_EMPTY);
  // Process replacement installs fresh hooks in the new process.
  reset();
  current_pid = 101;
  prepare_process();
  install();
  assert(g_hooks.remote.pid == 101 && !attached);
  // Stop (including incomplete cleanup) cannot be undone by a late refresh.
  reset();
  attach_fail = true;
  sm_shellcore_hooks_stop();
  assert(!g_hooks.enabled && g_hooks.status != SHELLCORE_HOOKS_EMPTY);
  attach_fail = false;
  before = writes;
  assert(sm_shellcore_hooks_refresh());
  assert(!sm_shellcore_install_title_dir("PPSA12345", "/game", &result));
  assert(writes == before && dispatches == 0);
  sm_shellcore_hooks_stop();
  assert(g_hooks.status == SHELLCORE_HOOKS_EMPTY);
  // Failed process validation and detach never permit registration dispatch.
  reset();
  lose_hook(2);
  resolve_fail = true;
  assert(!sm_shellcore_hooks_refresh() && writes == 0 && !attached);
  resolve_fail = false;
  install();
  reset();
  lose_hook(2);
  detach_fail = true;
  assert(!sm_shellcore_hooks_refresh() && attached);
  assert(g_hooks.status == SHELLCORE_HOOKS_ROLLBACK_PENDING);
  assert(!sm_shellcore_install_title_dir("PPSA12345", "/game", &result));
  assert(dispatches == 0);
  assert(g_hooks.attached);
  detach_fail = false;
  sm_shellcore_hooks_stop();
  assert(g_hooks.status == SHELLCORE_HOOKS_EMPTY);
  // Failed initial target resolution plus failed detach owns a stop even
  // though it installed no bytes. Shutdown must release it without reattach.
  memset(&g_hooks, 0, sizeof(g_hooks));
  attached = false;
  resolve_fail = detach_fail = true;
  assert(!sm_shellcore_hooks_start());
  assert(attached && g_hooks.attached && g_hooks.hook_count == 0);
  resolve_fail = detach_fail = false;
  sm_shellcore_hooks_stop();
  assert(!attached && g_hooks.status == SHELLCORE_HOOKS_EMPTY);
  reset();
  stopping = true;
  lose_hook(2);
  assert(!sm_shellcore_hooks_refresh() && writes == 0 && attaches == 0);
  // Pre-12.00 firmware has only launch/sandbox hooks and a shorter cave.
  firmware = 0x1001;
  reset();
  assert(g_hooks.hook_count == 2 && !sm_shellcore_install_bridge_enabled());
  lose_hook(0);
  lose_hook(1);
  assert(sm_shellcore_hooks_refresh() && writes == 2 && !attached);
  assert(!sm_shellcore_install_title_dir("PPSA12345", "/game", &result));
  sm_shellcore_hooks_stop();
  assert(g_hooks.status == SHELLCORE_HOOKS_EMPTY);
  assert(sm_shellcore_hooks_refresh() && g_hooks.status == SHELLCORE_HOOKS_EMPTY);
  // An unpinned mapping is never published or used as executable bridge code.
  memset(&g_hooks, 0, sizeof(g_hooks));
  prepare_process();
  bridge_locked = false;
  fail_bridge_lock = true;
  assert(!sm_shellcore_hooks_start() && !attached);
  assert(bridge_unmaps == 1 && !bridge_locked);
  assert(remote_range_is_zero(current_pid, 0x100000 + offsets.bridge_cave_offset,
                              BRIDGE_ANCHOR_SIZE));
  fail_bridge_lock = false;
  assert(sm_shellcore_hooks_start() && bridge_locked);
  sm_shellcore_hooks_stop();
  puts("ShellCore hooks: loss, safe repair, rollback, replacement and shutdown passed");
}
