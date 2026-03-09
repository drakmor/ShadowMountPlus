#ifndef SM_RUNTIME_H
#define SM_RUNTIME_H

#include <stdbool.h>
#include <sys/types.h>

// Install process signal handlers used for graceful shutdown.
void install_signal_handlers(void);
// Return process pid with the given name, or 0 if not found.
pid_t find_pid_by_name(const char *name, bool exclude_self);
// Return true when shutdown was requested by signal or kill file.
bool should_stop_requested(void);
// Request graceful shutdown with a descriptive source string.
void request_shutdown_stop(const char *reason);
// Request an immediate scan cycle with a descriptive source string.
void request_scan_now(const char *reason);
// Consume a pending immediate scan request, returning true if one was pending.
bool consume_scan_now_request(const char **reason_out);
// Sleep in chunks and stop early if shutdown was requested.
bool sleep_with_stop_check(unsigned int total_us);

#endif
