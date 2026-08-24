#ifndef SM_GAME_LIFECYCLE_H
#define SM_GAME_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

// Start the shared game lifecycle watcher used by runtime game features.
bool start_game_lifecycle_watcher(void);
// Wake the shared watcher so pending lifecycle work is processed immediately.
void wake_game_lifecycle_watcher(void);
// Stop the shared watcher and let lifecycle modules clean up tracked state.
void stop_game_lifecycle_watcher(void);
// Return true when a supported game is currently being tracked as running.
bool sm_game_lifecycle_has_active_game(void);
// Publish and wake lifecycle/kstuff processing for an AppFocus change.
void sm_game_lifecycle_note_app_focus(uint32_t app_id);
// Ensure the shared watcher is running for runtime game-state tracking.
bool refresh_game_lifecycle_watcher(void);

#endif
