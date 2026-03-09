#ifndef SM_GAME_CACHE_H
#define SM_GAME_CACHE_H

#include <stdbool.h>

// Cache resolved metadata for a mounted or discovered game.
void cache_game_entry(const char *path, const char *title_id,
                      const char *title_name);
// Drop invalid or stale entries from the game cache.
void prune_game_cache(void);
// Look up a cached game entry by path or title ID.
bool find_cached_game(const char *path, const char *title_id,
                      const char **existing_path_out);
// Remove a game cache entry by path.
void clear_cached_game(const char *path);

#endif
