#ifndef SM_SHELLCORE_SERVICE_H
#define SM_SHELLCORE_SERVICE_H

#include <stdbool.h>

// Start/stop the local Unix-socket owner used by the SceShellCore bridge.
bool sm_shellcore_service_start(void);
void sm_shellcore_service_stop(void);

// Release transient title/image mounts after the lifecycle watcher confirms
// that the game process exited.
bool sm_shellcore_release_title_runtime(const char *title_id);
// Ensure that a managed title has its image/nullfs/backport runtime stack.
// Unmanaged stock titles are treated as already ready.
bool sm_shellcore_ensure_title_runtime(const char *title_id);
// Return true while ShellCore owns a title mount prepared for launch or exit.
bool sm_shellcore_service_has_prepared_mount(void);

#endif
