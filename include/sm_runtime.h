#ifndef SM_RUNTIME_H
#define SM_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

// Install process signal handlers used for graceful shutdown.
void install_signal_handlers(void);
// Return process pid with the given name, or 0 if not found.
pid_t find_pid_by_name(const char *name, bool exclude_self);
// Return true when shutdown was requested by signal or kill file.
bool should_stop_requested(void);
// Request graceful shutdown with a descriptive source string.
void request_shutdown_stop(const char *reason);
// Return true when the payload is paused for system suspend/resume.
bool runtime_sleep_mode_active(void);
// Return true while a launch may briefly wait for USB storage after resume.
bool runtime_resume_grace_active(void);
// Enter or leave sleep mode with a descriptive source string.
// Returns true when the runtime sleep state changed.
bool request_runtime_sleep_mode(bool active, const char *reason);
// Serialize mount/link/cache publication against suspend cleanup.
void runtime_mount_state_lock(void);
void runtime_mount_state_unlock(void);
// Request an immediate scan cycle with a descriptive source string.
void request_scan_now(const char *reason);
// Consume a pending immediate scan request and copy its reason into caller storage.
bool consume_scan_now_request(char *reason_out, size_t reason_out_size);
// Sleep in chunks and stop early if shutdown was requested.
bool sleep_with_stop_check(unsigned int total_us);

#endif
