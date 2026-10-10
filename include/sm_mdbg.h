#ifndef SM_MDBG_H
#define SM_MDBG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

// Initialize game-error monitoring and kernel-log access state.
void sm_mdbg_init(void);
// Release monitoring state after the lifecycle watcher and API service stop.
void sm_mdbg_shutdown(void);
// Game-monitor callbacks are owned by the lifecycle watcher thread.
// Monitor errors for at most 60 seconds from the original game exec timestamp.
void sm_mdbg_game_on_exec(pid_t pid, const char *title_id,
                          uint64_t exec_time_us);
// Rebind a same-game process replacement without extending the deadline.
void sm_mdbg_game_handoff(pid_t old_pid, pid_t new_pid);
// Drain pending errors and stop monitoring the finalized game exit.
void sm_mdbg_game_on_exit(pid_t pid);
// Stop monitoring when the lifecycle watcher sleeps or shuts down.
void sm_mdbg_game_shutdown(void);
// Return the next monotonic deadline, or 0 when monitoring is inactive.
uint64_t sm_mdbg_next_wake_us(void);
// Read new errors; query process flags only while the process is still active.
void sm_mdbg_poll(bool process_active);
// Return a caller-owned tail of the SDK log stream. The total byte count
// describes the current complete snapshot.
int sm_mdbg_get_log_tail(size_t max_bytes, char **text_out,
                         size_t *text_length_out, size_t *total_length_out);

#endif
