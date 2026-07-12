#ifndef SM_IMAGE_INDEX_H
#define SM_IMAGE_INDEX_H

#include <stdbool.h>
#include <sys/stat.h>

struct AppDbTitleList;

// Return true when an image must be opened to discover/install its contents.
bool sm_image_index_needs_scan(const char *path, const struct stat *st,
                               const struct AppDbTitleList *app_db_titles,
                               bool app_db_titles_ready);
// Start a new fingerprint generation before mounting an image.
void sm_image_index_begin_scan(const char *path, const struct stat *st);
// Associate a discovered mounted game with its outer backing image.
void sm_image_index_record_game(const char *game_path, const char *title_id);
// Commit one fingerprint after its mounted content was scanned.
void sm_image_index_complete_scan(const char *path);
// Atomically persist accumulated index changes at the scan-cycle boundary.
void sm_image_index_flush(void);
// Drop persistent entries whose backing image disappeared.
void sm_image_index_prune(void);

#endif
