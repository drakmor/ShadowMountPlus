#ifndef SM_SHELLCORE_REMOTE_H
#define SM_SHELLCORE_REMOTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "sm_shellcore_offsets.h"

typedef struct {
  pid_t pid;
  uintptr_t image_base;
  uintptr_t targets[SM_SHELLCORE_TARGET_COUNT];
  const sm_shellcore_firmware_offsets_t *offsets;
} sm_shellcore_remote_t;

// Resolve and validate every lifecycle target in a running SceShellCore.
bool sm_shellcore_remote_resolve(pid_t pid, sm_shellcore_remote_t *remote_out);
// Stop/continue a remote process for register-safe syscall injection.
bool sm_remote_process_attach(pid_t pid);
bool sm_remote_process_detach(pid_t pid);
// Manage anonymous memory while the remote process is attached.
uintptr_t sm_remote_process_map(pid_t pid, size_t size);
// Change mapping protection through the target process's syscall path.
bool sm_remote_process_protect(pid_t pid, uintptr_t address, size_t size,
                               int protection);
// Pin long-lived anonymous mappings while remote hooks can execute.
bool sm_remote_process_lock(pid_t pid, uintptr_t address, size_t size);
bool sm_remote_process_unlock(pid_t pid, uintptr_t address, size_t size);
bool sm_remote_process_unmap(pid_t pid, uintptr_t address, size_t size);
// Read or write another process while handling post-8.20 write restrictions.
bool sm_remote_process_read(pid_t pid, uintptr_t address, void *buffer,
                            size_t size);
// Write through the target VM while it is attached and the range is writable.
bool sm_remote_process_write_attached(pid_t pid, uintptr_t address,
                                      const void *buffer, size_t size);
bool sm_remote_process_write(pid_t pid, uintptr_t address, const void *buffer,
                             size_t size);

#endif
