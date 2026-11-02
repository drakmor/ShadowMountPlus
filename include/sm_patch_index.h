#ifndef SM_PATCH_INDEX_H
#define SM_PATCH_INDEX_H

#include "sm_patch_sources.h"
#include "sm_game_patches.h"
#include "sm_config_file.h"

#ifndef SM_PATCH_INDEX_PATH
#define SM_PATCH_INDEX_PATH "/data/shadowmount/patch_index_v1.db"
#endif
#define SM_PATCH_MAX_FILES 64u

// Immutable generation directories are indexed once. Mutable directories are
// refreshed from file metadata, without reparsing unchanged content.
int sm_patch_index_build(const char *directory, bool cheats, bool immutable,
                           sm_patch_cancel_t cancelled, void *context);
// Append matching files, keeping the first source of each basename.
int sm_patch_index_collect(const char *directory, const char *title, bool cheats,
                             bool immutable, char **paths, size_t *count);
int sm_patch_index_collect_local(const char *title, char **paths, size_t *count);
void sm_patch_index_remove(const char *directory);
int sm_patch_index_warm_sources(const sm_patch_source_config_t *sources,
                                  sm_patch_cancel_t cancelled, void *context);
int sm_patch_index_badges(sm_patch_badges_t *items, size_t count, const sm_patch_source_config_t *sources,
                           const sm_ini_t *selection);

#endif
