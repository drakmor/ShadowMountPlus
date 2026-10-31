#include "sm_platform.h"

#include "sm_kstuff.h"
#include <sys/mman.h>

bool sm_kstuff_is_loaded(void) {
  register uint64_t r10 __asm__("r10") = 0;
  register uint64_t r8 __asm__("r8") = 0;
  register uint64_t r9 __asm__("r9") = 0;
  uint64_t result;
  unsigned char is_error;

  /* Keep this probe identical to ps5-kstuff's KEKCALL_CHECK contract. */
  __asm__ __volatile__("syscall"
                       : "=a"(result), "=@ccc"(is_error), "+r"(r10),
                         "+r"(r8), "+r"(r9)
                       : "a"(SM_KSTUFF_KEKCALL_CHECK), "D"(UINT64_C(0)),
                         "S"(UINT64_C(0)), "d"(UINT64_C(0))
                       : "rcx", "r11", "memory");

  return !is_error && result == 0;
}

bool sm_kstuff_remote_mprotect(pid_t pid, uintptr_t address, size_t size,
                               int protection) {
  uint64_t args[6] = {address, size, (uint64_t)protection, 0, 0, 0};
  register uint64_t r10 __asm__("r10") = 0;
  register uint64_t r8 __asm__("r8") = 0;
  register uint64_t r9 __asm__("r9") = 0;
  uint64_t result;
  unsigned char is_error;

  __asm__ __volatile__("syscall"
                       : "=a"(result), "=@ccc"(is_error), "+r"(r10),
                         "+r"(r8), "+r"(r9)
                       : "a"(SM_KSTUFF_KEKCALL_REMOTE_SYSCALL),
                         "D"((uint64_t)(uint32_t)pid),
                         "S"((uint64_t)SYS_mprotect), "d"((uint64_t)args)
                       : "rcx", "r11", "memory");

  return !is_error && result == 0;
}

/* Hook writes on firmware 8.40+ use the physical mapping and do not dirty the
 * executable's file-backed VM page. Retain it before writing, so pageout cannot
 * reload the original signed instructions over the hook. */
bool sm_kstuff_remote_mlock(pid_t pid, uintptr_t address, size_t size) {
  const uintptr_t page_size = 0x4000u;
  if (size == 0 || size - 1 > UINTPTR_MAX - address)
    return false;
  uintptr_t first_page = address & ~(page_size - 1);
  uintptr_t last_page = (address + size - 1) & ~(page_size - 1);
  if (last_page > UINTPTR_MAX - page_size)
    return false;
  uint64_t args[6] = {first_page, last_page - first_page + page_size,
                      0, 0, 0, 0};
  register uint64_t r10 __asm__("r10") = 0;
  register uint64_t r8 __asm__("r8") = 0;
  register uint64_t r9 __asm__("r9") = 0;
  uint64_t result;
  unsigned char is_error;

  __asm__ __volatile__("syscall"
                       : "=a"(result), "=@ccc"(is_error), "+r"(r10),
                         "+r"(r8), "+r"(r9)
                       : "a"(SM_KSTUFF_KEKCALL_REMOTE_SYSCALL),
                         "D"((uint64_t)(uint32_t)pid),
                         "S"((uint64_t)SYS_mlock), "d"((uint64_t)args)
                       : "rcx", "r11", "memory");

  return !is_error && result == 0;
}

bool sm_kstuff_remote_mmap(pid_t pid, size_t size, uintptr_t *address) {
  if (pid <= 0 || !address || !size || size >= UINT64_C(0x800000000000))
    return false;
  uint64_t args[6] = {0, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, UINT64_MAX, 0};
  register uint64_t r10 __asm__("r10") = 0;
  register uint64_t r8 __asm__("r8") = 0;
  register uint64_t r9 __asm__("r9") = 0;
  uint64_t result;
  unsigned char is_error;
  __asm__ __volatile__("syscall"
                       : "=a"(result), "=@ccc"(is_error), "+r"(r10),
                         "+r"(r8), "+r"(r9)
                       : "a"(SM_KSTUFF_KEKCALL_REMOTE_SYSCALL),
                         "D"((uint64_t)(uint32_t)pid),
                         "S"((uint64_t)SYS_mmap), "d"((uint64_t)args)
                       : "rcx", "r11", "memory");
  if (is_error || result < 0x10000 || result >= UINT64_C(0x800000000000) - size)
    return false;
  *address = result;
  return true;
}

bool sm_kstuff_remote_munmap(pid_t pid, uintptr_t address, size_t size) {
  if (pid <= 0 || address < 0x10000 || !size)
    return false;
  uint64_t args[6] = {address, size, 0, 0, 0, 0};
  register uint64_t r10 __asm__("r10") = 0;
  register uint64_t r8 __asm__("r8") = 0;
  register uint64_t r9 __asm__("r9") = 0;
  uint64_t result;
  unsigned char is_error;
  __asm__ __volatile__("syscall"
                       : "=a"(result), "=@ccc"(is_error), "+r"(r10),
                         "+r"(r8), "+r"(r9)
                       : "a"(SM_KSTUFF_KEKCALL_REMOTE_SYSCALL),
                         "D"((uint64_t)(uint32_t)pid),
                         "S"((uint64_t)SYS_munmap), "d"((uint64_t)args)
                       : "rcx", "r11", "memory");
  return !is_error && result == 0;
}
