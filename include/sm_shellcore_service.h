#ifndef SM_SHELLCORE_SERVICE_H
#define SM_SHELLCORE_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "sm_limits.h"

// Start/stop the local Unix-socket owner used by the SceShellCore bridge.
bool sm_shellcore_service_start(void);
void sm_shellcore_service_stop(void);

// Release transient title/image mounts after launch failure or explicit API use.
bool sm_shellcore_release_title_runtime(const char *title_id);
// Public API entry points. Return 0 or a positive errno value.
int sm_shellcore_mount_title_runtime(const char *title_id);
int sm_shellcore_unmount_title_runtime(const char *title_id);
// Bind the prepared managed title to the app id stored in LncApplication.
void sm_shellcore_service_bind_prepared_app(const char *title_id,
                                            uint32_t app_id, pid_t pid);
// Publish process exit while ShellCore still owns the title sandbox and copy
// the matching owned title to title_id_out.
// Returns true when an owned incoming/outgoing title became exit-pending.
bool sm_shellcore_service_note_game_exit(pid_t pid,
                                         char title_id_out[MAX_TITLE_ID]);
// Release exit-pending runtime stacks whose /mnt/sandbox/<TITLE_ID>_* entries
// have all disappeared. Returns 0 when no exit-pending runtime remains, EAGAIN
// while a sandbox remains, or another positive errno after a release failure.
int sm_shellcore_service_release_exited_titles(void);
// Return true while ShellCore still exposes a sandbox for the title.
bool sm_shellcore_service_title_sandbox_exists(const char *title_id);
// Ensure that a managed title has its image/nullfs/backport runtime stack.
// Unmanaged stock titles are treated as already ready.
bool sm_shellcore_ensure_title_runtime(const char *title_id);
// Return true when the named title owns an incoming or outgoing runtime mount.
bool sm_shellcore_service_title_is_prepared(const char *title_id);
// Return true while ShellCore owns any title mount prepared for launch or exit.
bool sm_shellcore_service_has_prepared_mount(void);

#endif
