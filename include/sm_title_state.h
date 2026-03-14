#ifndef SM_TITLE_STATE_H
#define SM_TITLE_STATE_H

#include <stdbool.h>
#include <stdint.h>

// Return whether registration was already attempted for a title.
bool was_register_attempted(const char *title_id);
// Return the number of registration attempts for a title.
uint8_t get_register_attempts(const char *title_id);
// Record a registration attempt for a title.
void mark_register_attempted(const char *title_id);
// Notify once about duplicate title IDs from different paths.
void notify_duplicate_title_once(const char *title_id, const char *path_a,
                                 const char *path_b);
// Clear the one-shot duplicate notification state for a title.
void clear_duplicate_title_notification(const char *title_id);
// Return the number of failed install/remount attempts for a title.
uint8_t get_failed_mount_attempts(const char *title_id);
// Clear failed install/remount attempts for a title.
void clear_failed_mount_attempts(const char *title_id);
// Increment and return failed install/remount attempts for a title.
uint8_t bump_failed_mount_attempts(const char *title_id);

#endif
