#ifndef SM_KSTUFF_H
#define SM_KSTUFF_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define SM_KSTUFF_KEKCALL_CHECK UINT64_C(0xffffffff00000027)
#define SM_KSTUFF_KEKCALL_REMOTE_SYSCALL UINT64_C(0x500000027)

// Return true when kstuff answers its own KEKCALL_CHECK readiness probe.
bool sm_kstuff_is_loaded(void);
// Run mprotect for another process through kstuff's remote-syscall service.
bool sm_kstuff_remote_mprotect(pid_t pid, uintptr_t address, size_t size,
                               int protection);

#endif
