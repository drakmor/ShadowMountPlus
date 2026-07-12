#include "sm_platform.h"

#include <ps5/mdbg.h>

#include "sm_log.h"
#include "sm_shellcore_remote.h"

#define X86_PAGE_FRAME 0x000ffffffffff000ull
#define X86_PAGE_VALID 0x001ull
#define X86_PAGE_LARGE 0x080ull

static uintptr_t resolve_vmspace_pmap(uintptr_t vmspace) {
  uint32_t version = kernel_get_fw_version() >> 16;
  if (version >= 0x0100u && version <= 0x0102u)
    return vmspace + 0x2c0u;
  if (version >= 0x0105u && version <= 0x0550u)
    return vmspace + 0x2e0u;
  if (version >= 0x0600u && version <= 0x1270u)
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
  if (!paging[0] || !paging[1])
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
  if (!buffer || size == 0)
    return false;
  return mdbg_copyout(pid, (intptr_t)address, buffer, size) == 0;
}

bool sm_remote_process_write(pid_t pid, uintptr_t address, const void *buffer,
                             size_t size) {
  if (!buffer || size == 0)
    return false;
  if ((kernel_get_fw_version() >> 16) <= 0x0820u)
    return mdbg_copyin(pid, buffer, (intptr_t)address, size) == 0;

  void *probe = malloc(size);
  if (!probe)
    return false;
  bool faulted = sm_remote_process_read(pid, address, probe, size);
  free(probe);
  if (!faulted)
    return false;

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
    if (kernel_copyin(source, (intptr_t)(dmap + physical), chunk) != 0)
      return false;
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

  const sm_shellcore_firmware_offsets_t *firmware =
      sm_shellcore_offsets_for_firmware(kernel_get_fw_version());
  if (!firmware) {
    log_debug("  [SHELLCORE] unsupported firmware: 0x%08x",
              kernel_get_fw_version());
    return false;
  }

  uintptr_t image_base = (uintptr_t)kernel_dynlib_mapbase_addr(pid, 0);
  if (!image_base) {
    log_debug("  [SHELLCORE] failed to resolve image base for pid=%ld",
              (long)pid);
    return false;
  }

  for (int target = 0; target < SM_SHELLCORE_TARGET_COUNT; ++target) {
    remote_out->targets[target] = image_base + firmware->targets[target].offset;
  }

  remote_out->pid = pid;
  remote_out->image_base = image_base;
  remote_out->offsets = firmware;
  log_debug("  [SHELLCORE] resolved fw=%s pid=%ld base=0x%lx", firmware->name,
            (long)pid, (unsigned long)image_base);
  return true;
}
