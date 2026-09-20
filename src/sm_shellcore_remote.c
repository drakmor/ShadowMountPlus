#include "sm_platform.h"

#include <ps5/mdbg.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#include "sm_log.h"
#include "sm_shellcore_remote.h"

#define X86_PAGE_FRAME 0x000ffffffffff000ull
#define X86_PAGE_VALID 0x001ull
#define X86_PAGE_LARGE 0x080ull
#define SCE_AUTHID_DEBUGGER 0x4800000000010003ull

static int privileged_ptrace(int request, pid_t pid, void *address, int data) {
  static const uint8_t k_priv_caps[16] = {
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  pid_t self = getpid();
  uint8_t saved_caps[sizeof(k_priv_caps)];
  uint64_t saved_authid = kernel_get_ucred_authid(self);
  if (!saved_authid || kernel_get_ucred_caps(self, saved_caps) != 0)
    return -1;
  if (kernel_set_ucred_authid(self, SCE_AUTHID_DEBUGGER) != 0 ||
      kernel_set_ucred_caps(self, k_priv_caps) != 0) {
    (void)kernel_set_ucred_authid(self, saved_authid);
    (void)kernel_set_ucred_caps(self, saved_caps);
    return -1;
  }

  int result = ptrace(request, pid, (caddr_t)address, data);
  bool restored = kernel_set_ucred_authid(self, saved_authid) == 0;
  restored = kernel_set_ucred_caps(self, saved_caps) == 0 && restored;
  return restored ? result : -1;
}

static bool wait_for_remote_stop(pid_t pid) {
  int status = 0;
  pid_t result;
  do {
    result = waitpid(pid, &status, 0);
  } while (result < 0 && errno == EINTR);
  return result == pid && WIFSTOPPED(status);
}

bool sm_remote_process_attach(pid_t pid) {
  if (pid <= 0 || privileged_ptrace(PT_ATTACH, pid, NULL, 0) != 0)
    return false;
  if (wait_for_remote_stop(pid))
    return true;
  (void)privileged_ptrace(PT_DETACH, pid, NULL, 0);
  return false;
}

bool sm_remote_process_detach(pid_t pid) {
  return pid > 0 && privileged_ptrace(PT_DETACH, pid, NULL, 0) == 0;
}

static uintptr_t resolve_vmspace_pmap(uintptr_t vmspace) {
  uint32_t version = kernel_get_fw_version() >> 16;
  if (version >= 0x0100u && version <= 0x0102u)
    return vmspace + 0x2c0u;
  if (version >= 0x0105u && version <= 0x0550u)
    return vmspace + 0x2e0u;
  if (version >= 0x0600u && version <= 0x1360u)
    return vmspace + 0x2e8u;
  return 0;
}

static bool resolve_process_paging(pid_t pid, uint64_t *cr3_out,
                                   uint64_t *dmap_out) {
  uintptr_t process = (uintptr_t)kernel_get_proc(pid);
  if (!process)
    return false;
  uintptr_t vmspace = (uintptr_t)kernel_getlong(
      process + (uintptr_t)KERNEL_OFFSET_PROC_P_VMSPACE);
  if (!vmspace)
    return false;
  uintptr_t pmap = resolve_vmspace_pmap(vmspace);
  if (!pmap)
    return false;

  uint64_t paging[2] = {0};
  if (kernel_copyout((intptr_t)(pmap + 32u), paging, sizeof(paging)) != 0)
    return false;
  if (!paging[0] || !paging[1] || paging[0] <= paging[1])
    return false;

  *cr3_out = paging[1];
  *dmap_out = paging[0] - paging[1];
  return true;
}

static uint64_t virtual_to_physical(uint64_t address, uint64_t dmap,
                                    uint64_t page_map,
                                    uint64_t *physical_limit_out) {
  page_map &= X86_PAGE_FRAME;
  for (int shift = 39; shift >= 12; shift -= 9) {
    uint64_t index = (address >> shift) & 0x1ffu;
    page_map = kernel_getlong((intptr_t)(dmap + page_map + index * 8u));
    if ((page_map & X86_PAGE_VALID) == 0)
      return UINT64_MAX;
    if ((page_map & X86_PAGE_LARGE) != 0 || shift == 12) {
      page_map &= (1ull << 52) - (1ull << shift);
      page_map |= address & ((1ull << shift) - 1ull);
      if (physical_limit_out)
        *physical_limit_out = (page_map | ((1ull << shift) - 1ull)) + 1ull;
      return page_map;
    }
    page_map &= X86_PAGE_FRAME;
  }
  return UINT64_MAX;
}

bool sm_remote_process_read(pid_t pid, uintptr_t address, void *buffer,
                            size_t size) {
  if (!buffer || size == 0 || address > UINTPTR_MAX - (size - 1u))
    return false;
  return mdbg_copyout(pid, (intptr_t)address, buffer, size) == 0;
}

bool sm_remote_process_write(pid_t pid, uintptr_t address, const void *buffer,
                             size_t size) {
  if (!buffer || size == 0 || address > UINTPTR_MAX - (size - 1u))
    return false;
  if ((kernel_get_fw_version() >> 16) <= 0x0820u)
    return mdbg_copyin(pid, buffer, (intptr_t)address, size) == 0;

  uint8_t probe = 0;
  if (!sm_remote_process_read(pid, address, &probe, sizeof(probe)) ||
      (size > 1u &&
       !sm_remote_process_read(pid, address + size - 1u, &probe,
                               sizeof(probe)))) {
    return false;
  }

  uint64_t cr3 = 0;
  uint64_t dmap = 0;
  if (!resolve_process_paging(pid, &cr3, &dmap))
    return false;

  const uint8_t *source = (const uint8_t *)buffer;
  while (size != 0) {
    uint64_t physical_limit = 0;
    uint64_t physical =
        virtual_to_physical(address, dmap, cr3, &physical_limit);
    if (physical == UINT64_MAX || physical_limit <= physical)
      return false;
    size_t chunk = (size_t)(physical_limit - physical);
    if (chunk > size)
      chunk = size;
    if (physical > UINTPTR_MAX - dmap ||
        kernel_copyin(source, (intptr_t)(dmap + physical), chunk) != 0) {
      return false;
    }
    address += chunk;
    source += chunk;
    size -= chunk;
  }
  return true;
}

bool sm_shellcore_remote_resolve(pid_t pid, sm_shellcore_remote_t *remote_out) {
  if (pid <= 0 || !remote_out)
    return false;
  memset(remote_out, 0, sizeof(*remote_out));

  uint32_t raw_firmware = kernel_get_fw_version();
  const sm_shellcore_firmware_offsets_t *firmware =
      sm_shellcore_offsets_for_firmware(raw_firmware);
  if (!firmware) {
    log_debug("  [SHELLCORE] unsupported firmware: 0x%08x",
              raw_firmware);
    return false;
  }

  uintptr_t image_base = (uintptr_t)kernel_dynlib_mapbase_addr(pid, 0);
  if (!image_base) {
    log_debug("  [SHELLCORE] failed to resolve image base for pid=%ld",
              (long)pid);
    return false;
  }

  for (int target = 0; target < SM_SHELLCORE_TARGET_COUNT; ++target) {
    if (firmware->targets[target].offset > UINTPTR_MAX - image_base) {
      log_debug("  [SHELLCORE] target offset overflow: %s",
                sm_shellcore_target_name((sm_shellcore_target_t)target));
      return false;
    }
    remote_out->targets[target] = image_base + firmware->targets[target].offset;
  }
  if (firmware->sandbox_call_target_offset > UINTPTR_MAX - image_base) {
    log_debug("  [SHELLCORE] sandbox call target offset overflow");
    return false;
  }

  remote_out->pid = pid;
  remote_out->image_base = image_base;
  remote_out->offsets = firmware;
  log_debug("  [SHELLCORE] resolved fw=%s pid=%ld base=0x%lx", firmware->name,
            (long)pid, (unsigned long)image_base);
  return true;
}
