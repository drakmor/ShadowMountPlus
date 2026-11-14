#ifndef SM_FAKELIB_H
#define SM_FAKELIB_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "sm_limits.h"

// Return true when fakelib backport automation is enabled.
// Sandbox directory mounting is always enabled for supported game titles.
bool sm_fakelib_game_feature_enabled(void);
// Protect the mounted overlay source, including retained cleanup sessions.
// General /data and storage-root aliases do not protect unrelated children.
bool sm_fakelib_source_is_in_use(const char *path);
// Prepare or refresh the combined fakelib cache before title runtime mounting.
void sm_fakelib_prepare_title_cache(const char *title_id,
                                    const char *game_path);
// Reattach the recorded image chain if needed and validate the cache source
// snapshot, allowing mount-local device/inode changes only after remount.
bool sm_fakelib_resolve_fpkg_generation_source(
    const char *title_id, const char *generation_path,
    char source_path_out[MAX_PATH]);
// Remove invalid and unused fakelib caches during the scanner's rare cycle.
void sm_fakelib_cleanup_caches(void);
// Mount sandbox directories and an optional prepared fakelib overlay after ShellCore
// creates the sandbox but before it spawns the application process.
bool sm_fakelib_game_on_sandbox_ready(const char *title_id);
// Update one USB alias in the prepared or running game's sandbox on hotplug.
void sm_fakelib_game_on_usb_mount_change(const char *source_path, bool mounted);
// Reconcile USB, extended storage and disc aliases after filesystem events.
void sm_fakelib_game_refresh_storage(void);
// Detach only owned sandbox /mnt/usbN aliases for sleep; reconcile USB aliases
// on resume. Other layers and borrowed mounts retain their lifetime/ownership.
// Failed aliases or an incomplete retained session return false so USB source
// cleanup can be skipped. Stale sleep phases and shutdown requests are rejected.
bool sm_fakelib_game_set_usb_suspended(bool suspended);
// Roll back unbound pre-spawn mounts when ShellCore rejects the launch.
void sm_fakelib_game_on_launch_failed(const char *title_id);
// Bind pre-spawn mounts to the process, or mount as a NOTE_EXEC fallback.
// notify_user is false for an internal process handoff of the same application.
void sm_fakelib_game_on_exec(pid_t pid, const char *title_id,
                             bool notify_user);
// Remove tracked sandbox mounts for a stopped game.
void sm_fakelib_game_on_exit(pid_t pid);
// Bounded cleanup of busy owned mounts, driven by the shared lifecycle watcher.
// A zero deadline means no automatic cleanup is scheduled.
uint64_t sm_fakelib_game_next_cleanup_us(void);
void sm_fakelib_game_poll_cleanup(void);
// Unmount all tracked overlays during watcher shutdown.
void sm_fakelib_game_shutdown(void);

#endif
