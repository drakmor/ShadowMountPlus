#ifndef SM_GAME_LIFECYCLE_H
#define SM_GAME_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>
#include "sm_limits.h"

// Start the shared game lifecycle watcher used by runtime game features.
bool start_game_lifecycle_watcher(void);
// Wake the shared watcher so pending lifecycle work is processed immediately.
void wake_game_lifecycle_watcher(void);
// Stop the shared watcher and let lifecycle modules clean up tracked state.
void stop_game_lifecycle_watcher(void);
// Return true when a supported game is currently being tracked as running.
bool sm_game_lifecycle_has_active_game(void);
// Copy the protected live title even when returning false, or an empty string
// when no confirmed live title is available. Confirmed games allow unrelated
// work regardless of focus. Return false during a pending exit/handoff or
// when an active process has no confirmed supported title.
// A NULL output queries only the policy, without formatting the title ID.
bool sm_game_lifecycle_background_title(char title_id[MAX_TITLE_ID]);
// Publish and wake lifecycle processing for an AppFocus change.
void sm_game_lifecycle_note_app_focus(uint32_t app_id);
// Invalidate focus information used by per-game fan policy.
void sm_game_lifecycle_invalidate_app_focus(void);
// Queue a VideoOut loader signal without touching watcher-owned game state.
bool sm_game_lifecycle_note_video_ready(pid_t pid);
// Queue a PID returned by ShellCore's Spawn2 call for early VideoOut setup.
bool sm_game_lifecycle_note_process_spawned(pid_t pid);
// Ensure the shared watcher is running for runtime game-state tracking.
bool refresh_game_lifecycle_watcher(void);

#endif
