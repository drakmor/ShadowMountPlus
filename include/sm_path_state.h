#ifndef SM_PATH_STATE_H
#define SM_PATH_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

// Load cached game info if the param file state still matches.
bool load_cached_game_info(const char *path, const struct stat *param_st,
                           char *out_id, char *out_name, bool *valid_out);
// Store parsed game info together with the current param file state.
void store_cached_game_info(const char *path, const struct stat *param_st,
                            bool valid, const char *title_id,
                            const char *title_name);
// Drop expired entries from path-based retry and metadata state.
void prune_path_state(void);
// Return whether scans for a path are temporarily suppressed after failures.
bool is_missing_param_scan_limited(const char *path);
// Record a missing param.json failure for scan backoff.
void record_missing_param_failure(const char *path);
// Clear missing-param backoff state for a path.
void clear_missing_param_entry(const char *path);
// Return whether image mount retries are temporarily suppressed for a path.
bool is_image_mount_limited(const char *path);
// Increment and return the image mount retry counter for a path.
uint8_t bump_image_mount_attempts(const char *path);
// Clear image mount retry state for a path.
void clear_image_mount_attempts(const char *path);

#endif
