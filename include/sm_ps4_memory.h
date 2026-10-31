#ifndef SM_PS4_MEMORY_H
#define SM_PS4_MEMORY_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

bool sm_ps4_memory_supported(void);
const char *sm_ps4_memory_status(const char *title_id);
// Hook owner publishes the bridge while ShellCore is stopped. No RPC in callbacks.
bool sm_ps4_memory_bind_bridge(pid_t pid, uintptr_t address);
// The existing synchronous launch/spawn requests serialize DATA mutations.
int sm_ps4_memory_on_launch(const char *title_id, uint8_t policy);
// ENOENT identifies an unrelated app; do not consume the pending game callback.
int sm_ps4_memory_prepare(const char *title_id, uint32_t app_id, bool wanted);
void sm_ps4_memory_launch_failed(const char *title_id, bool pending_launch);
void sm_ps4_memory_on_exec(pid_t pid, const char *title_id, uint32_t app_id);
void sm_ps4_memory_on_exit(pid_t pid);
void sm_ps4_memory_poll(void);
uint64_t sm_ps4_memory_next_wake_us(void);
// Wait for owned container teardown before releasing the DATA increment.
// False retains the guarded bridge when foreign/uncertain DATA cannot be restored.
bool sm_ps4_memory_shutdown(void);

#endif
