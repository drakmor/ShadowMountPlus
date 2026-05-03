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
// Return true when foreground work should pause for stop or power transition.
bool should_pause_work(void);
// Request graceful shutdown with a descriptive source string.
void request_shutdown_stop(const char *reason);
// Request an immediate scan cycle with a descriptive source string.
void request_scan_now(const char *reason);
// Consume a pending immediate scan request and copy its reason into caller storage.
bool consume_scan_now_request(char *reason_out, size_t reason_out_size);
// Sleep in chunks and stop early if shutdown was requested.
bool sleep_with_stop_check(unsigned int total_us);

// power management for rest mode
void request_power_pause(const char *reason);
void request_power_resume(const char *reason);
bool sm_is_power_paused(void);

#endif
