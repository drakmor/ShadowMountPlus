#ifndef SM_KSTUFF_H
#define SM_KSTUFF_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

// Resolve firmware-specific kstuff sysentvec addresses and initialize state.
void sm_kstuff_init(void);
// Shut down runtime kstuff control state.
void sm_kstuff_shutdown(void);
// Return true when firmware-specific kstuff control is available.
bool sm_kstuff_is_supported(void);
// Return true when both tracked kstuff sysentvecs are enabled.
bool sm_kstuff_is_enabled(void);
// Set both tracked kstuff sysentvecs and return the resulting enabled state.
bool sm_kstuff_set_enabled(bool enabled, bool notify_user);

// Return true when game-driven kstuff automation should run.
bool sm_kstuff_game_feature_enabled(void);
// Track a supported game launch for delayed auto-pause handling.
void sm_kstuff_game_on_exec(pid_t pid, const char *title_id, uint32_t app_id,
                            uint64_t exec_time_us);
// Publish an AppFocus change for processing on the lifecycle/kstuff thread.
void sm_kstuff_note_app_focus(uint32_t app_id);
// Return the next wake deadline in monotonic microseconds, or 0 when idle.
uint64_t sm_kstuff_game_next_wake_us(uint64_t now_us);
// Forget a tracked game and restore kstuff when no paused entries remain.
void sm_kstuff_game_on_exit(pid_t pid);
// Apply any delayed kstuff pause whose deadline has elapsed.
void sm_kstuff_game_poll(void);
// Clear tracked game state and restore kstuff if the watcher paused it.
void sm_kstuff_game_shutdown(void);
// Re-apply runtime config changes to the currently tracked game, if any.
void sm_kstuff_on_config_reload(void);

#endif
