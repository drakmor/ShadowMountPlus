#ifndef SM_IMAGE_INDEX_H
#define SM_IMAGE_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#include "sm_limits.h"

struct AppDbTitleList;

typedef void (*sm_image_index_title_visitor_t)(const char *title_id,
                                               const void *ctx);

typedef struct {
  char path[MAX_PATH];
  int64_t size;
  int64_t mtime_sec;
  int32_t mtime_nsec;
  bool complete;
} sm_image_index_snapshot_entry_t;

// Return true when a complete unchanged index entry can be reused instead of
// opening the image, visiting every retained title ID before returning.
bool sm_image_index_visit_ready_titles(
    const char *path, const struct stat *st,
    const struct AppDbTitleList *app_db_titles, bool app_db_titles_ready,
    sm_image_index_title_visitor_t visitor, const void *visitor_ctx);
// Return true when a title is mapped to an existing backing image.
bool sm_image_index_has_source_for_title(const char *title_id);
// Start a new fingerprint generation before mounting an image.
void sm_image_index_begin_scan(const char *path, const struct stat *st);
// Associate a discovered mounted game with its outer backing image. Returns
// false when an image-backed title could not be retained in the index.
bool sm_image_index_record_game(const char *game_path, const char *title_id);
// Commit one fingerprint after its mounted content was scanned.
void sm_image_index_complete_scan(const char *path);
// Atomically persist accumulated index changes at the scan-cycle boundary.
void sm_image_index_flush(void);
// Drop persistent entries whose backing image disappeared.
void sm_image_index_prune(void);
// Allocate a stable snapshot of all known backing images. Caller frees it.
bool sm_image_index_snapshot(sm_image_index_snapshot_entry_t **entries_out,
                             size_t *count_out);

#endif
