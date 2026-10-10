#include "sm_platform.h"

#include "sm_kstuff.h"

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
