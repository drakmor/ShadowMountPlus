#ifndef SM_GAME_LIFECYCLE_H
#define SM_GAME_LIFECYCLE_H

#include <stdbool.h>

// Start the shared game lifecycle watcher used by backport fakelib and kstuff.
bool start_game_lifecycle_watcher(void);
// Wake the shared watcher so pending lifecycle work is processed immediately.
void wake_game_lifecycle_watcher(void);
// Stop the shared watcher and let lifecycle modules clean up tracked state.
void stop_game_lifecycle_watcher(void);

#endif
