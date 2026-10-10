#ifndef SM_FAKELIB_H
#define SM_FAKELIB_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

// Return true when fakelib backport automation is enabled.
// Sandbox directory mounting is always enabled for supported game titles.
bool sm_fakelib_game_feature_enabled(void);
// Prepare or refresh the combined fakelib cache before title runtime mounting.
void sm_fakelib_prepare_title_cache(const char *title_id,
                                    const char *game_path);
// Remove invalid and unused fakelib caches during the scanner's rare cycle.
void sm_fakelib_cleanup_caches(void);
// Mount sandbox directories and an optional prepared fakelib overlay after ShellCore
// creates the sandbox but before it spawns the application process.
bool sm_fakelib_game_on_sandbox_ready(const char *title_id);
// Update one USB alias in the prepared or running game's sandbox on hotplug.
void sm_fakelib_game_on_usb_mount_change(const char *source_path, bool mounted);
// Roll back unbound pre-spawn mounts when ShellCore rejects the launch.
void sm_fakelib_game_on_launch_failed(const char *title_id);
// Bind pre-spawn mounts to the process, or mount as a NOTE_EXEC fallback.
// notify_user is false for an internal process handoff of the same application.
void sm_fakelib_game_on_exec(pid_t pid, const char *title_id,
                             bool notify_user);
// Remove tracked sandbox mounts for a stopped game.
void sm_fakelib_game_on_exit(pid_t pid);
// Unmount all tracked overlays during watcher shutdown.
void sm_fakelib_game_shutdown(void);

#endif
