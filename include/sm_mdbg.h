#ifndef SM_MDBG_H
#define SM_MDBG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Initialize mdbg-backed crash monitoring state.
void sm_mdbg_init(void);
// Shut down mdbg-backed crash monitoring state.
void sm_mdbg_shutdown(void);
// Start monitoring the tracked game process for crash-candidate state.
void sm_mdbg_game_on_exec(pid_t pid, const char *title_id, uint32_t app_id);
// Record that kstuff auto-pause was actually applied to the tracked game.
void sm_mdbg_game_on_kstuff_pause(pid_t pid, uint64_t pause_time_us,
                                  uint32_t pause_delay_seconds);
// Stop monitoring the tracked game process on exit.
void sm_mdbg_game_on_exit(pid_t pid);
// Clear tracked game monitoring state unconditionally.
void sm_mdbg_game_shutdown(void);
// Return the next wake deadline in monotonic microseconds, or 0 when idle.
uint64_t sm_mdbg_next_wake_us(uint64_t now_us);
// Poll the tracked process for mdbg crash-candidate state.
void sm_mdbg_poll(void);
// Return a caller-owned tail of the same SDK log stream used by crash
// detection. The total byte count describes the current complete snapshot.
int sm_mdbg_get_log_tail(size_t max_bytes, char **text_out,
                         size_t *text_length_out, size_t *total_length_out);

#endif
