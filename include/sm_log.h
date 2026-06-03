#ifndef SM_LOG_H
#define SM_LOG_H

#include <stdbool.h>

#include "sm_l10n.h"

typedef struct sm_error sm_error_t;

// Write a formatted line to the debug log.
void log_debug(const char *fmt, ...);
// Prepare notification assets such as the packaged icon file.
void sm_notifications_init(void);
// Flush and close persistent log resources.
void sm_log_shutdown(void);
// Send a rich toast notification with packaged icon and version header.
void notify_system_rich(bool allow_in_quiet_mode, const char *fmt, ...);
void notify_system_rich_l10n(bool allow_in_quiet_mode, sm_l10n_key_t key, ...);
// Send the "game installed" rich toast for the given title ID.
void notify_game_installed_rich(const char *title_id);
// Send an informational system notification (suppressed in quiet mode).
void notify_system_info(const char *fmt, ...);
void notify_system_info_l10n(sm_l10n_key_t key, ...);
// Send a system notification with formatted text.
void notify_system(const char *fmt, ...);
void notify_system_l10n(sm_l10n_key_t key, ...);
// Clear the last recorded subsystem error.
void sm_error_clear(void);
// Store the last subsystem error with formatted details.
void sm_error_set(const char *subsystem, int code, const char *path,
                  const char *fmt, ...);
void sm_error_set_l10n(const char *subsystem, int code, const char *path,
                       sm_l10n_key_t key, ...);
// Return the last recorded subsystem error.
const sm_error_t *sm_last_error(void);
// Return whether the current error was already notified.
bool sm_error_notified(void);
// Mark the current error as already shown to the user.
void sm_error_mark_notified(void);
// Notify the user about an image mount failure once per error state.
void notify_image_mount_failed(const char *path, int mount_err);

#endif
