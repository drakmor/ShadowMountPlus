#ifndef SM_SHELLCORE_SERVICE_H
#define SM_SHELLCORE_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

// Start/stop the local Unix-socket owner used by the SceShellCore bridge.
bool sm_shellcore_service_start(void);
void sm_shellcore_service_stop(void);

// Release transient title/image mounts after launch failure or app exit.
bool sm_shellcore_release_title_runtime(const char *title_id);
// Public API entry points. Return 0 or a positive errno value.
int sm_shellcore_mount_title_runtime(const char *title_id);
int sm_shellcore_unmount_title_runtime(const char *title_id);
// Bind the prepared managed title to the app id stored in LncApplication.
void sm_shellcore_service_bind_prepared_app(const char *title_id,
                                            uint32_t app_id);
// Publish process exit while ShellCore finishes its own app-exit cleanup.
// Returns true when a managed prepared title transitioned to exit-pending.
bool sm_shellcore_service_note_game_exit(const char *title_id);
// Ensure that a managed title has its image/nullfs/backport runtime stack.
// Unmanaged stock titles are treated as already ready.
bool sm_shellcore_ensure_title_runtime(const char *title_id);
// Return true when the named title owns the prepared runtime mount.
bool sm_shellcore_service_title_is_prepared(const char *title_id);
// Return true while ShellCore owns a title mount prepared for launch or exit.
bool sm_shellcore_service_has_prepared_mount(void);

#endif
