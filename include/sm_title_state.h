#ifndef SM_TITLE_STATE_H
#define SM_TITLE_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Return whether registration was already attempted for a title.
bool was_register_attempted(const char *title_id);
// Return the number of registration attempts for a title.
uint8_t get_register_attempts(const char *title_id);
// Record a registration attempt for a title.
void mark_register_attempted(const char *title_id);
// Clear registration attempt state after success or confirmed user removal.
void clear_register_attempts(const char *title_id);
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
// Atomically return and clear both title-level registration and
// install/remount attempt counters.
void reset_title_attempts(const char *title_id,
                          uint8_t *register_attempts_out,
                          uint8_t *mount_install_attempts_out);
// Clear registration and install/remount attempt counters for every title and
// return the number of title entries that changed.
size_t reset_all_title_attempts(void);

#endif
