#ifndef SM_GAME_CACHE_H
#define SM_GAME_CACHE_H

#include <stdbool.h>
#include <stddef.h>

#include "sm_limits.h"

typedef struct {
  char path[MAX_PATH];
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  char owning_scan_root[MAX_PATH];
} sm_game_cache_snapshot_entry_t;

typedef bool (*game_cache_iter_fn)(const char *path, const char *title_id,
                                   const char *title_name,
                                   const char *owning_scan_root, void *ctx);

// Cache resolved metadata for a mounted or discovered game.
void cache_game_entry(const char *path, const char *title_id,
                      const char *title_name);
// Drop invalid or stale entries from the game cache.
void prune_game_cache(void);
// Drop invalid or stale entries that belong to a specific scan root.
void prune_game_cache_for_root(const char *root);
// Look up a cached game entry by path or title ID.
bool find_cached_game(const char *path, const char *title_id,
                      const char **existing_path_out);
// Visit cached games, optionally limited to one source root.
void for_each_cached_game_entry(const char *root, game_cache_iter_fn fn,
                                void *ctx);
// Copy a stable, paginated snapshot for read-only API consumers.
size_t sm_game_cache_snapshot(size_t offset,
                              sm_game_cache_snapshot_entry_t *entries,
                              size_t capacity, size_t *total_out);
// Remove a game cache entry by path.
void clear_cached_game(const char *path);

#endif
