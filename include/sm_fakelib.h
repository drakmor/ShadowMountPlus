#ifndef SM_FAKELIB_H
#define SM_FAKELIB_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

// Return true when per-game fakelib backport automation is enabled.
bool sm_fakelib_game_feature_enabled(void);
// Prepare or refresh the combined fakelib cache before title runtime mounting.
void sm_fakelib_prepare_title_cache(const char *title_id,
                                    const char *game_path);
// Remove invalid and unused fakelib caches during the scanner's rare cycle.
void sm_fakelib_cleanup_caches(void);
// Track a supported game launch and mount its fakelib overlay immediately.
void sm_fakelib_game_on_exec(pid_t pid, const char *title_id);
// Remove a tracked fakelib overlay for a stopped game.
void sm_fakelib_game_on_exit(pid_t pid);
// Unmount all tracked overlays during watcher shutdown.
void sm_fakelib_game_shutdown(void);

#endif
