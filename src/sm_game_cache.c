#include "sm_platform.h"
#include "sm_game_cache.h"
#include "sm_limits.h"
#include "sm_log.h"

struct GameCache {
  char path[MAX_PATH];
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  bool valid;
};

static struct GameCache g_game_cache[MAX_PENDING];

void cache_game_entry(const char *path, const char *title_id,
                      const char *title_name) {
  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid) {
      (void)strlcpy(g_game_cache[k].path, path, sizeof(g_game_cache[k].path));
      (void)strlcpy(g_game_cache[k].title_id, title_id,
                    sizeof(g_game_cache[k].title_id));
      (void)strlcpy(g_game_cache[k].title_name, title_name,
                    sizeof(g_game_cache[k].title_name));
      g_game_cache[k].valid = true;
      return;
    }
  }
}

void prune_game_cache(void) {
  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid)
      continue;
    if (access(g_game_cache[k].path, F_OK) == 0)
      continue;

    if (g_game_cache[k].title_id[0] != '\0')
      log_debug("  [CACHE] source removed: %s (%s)", g_game_cache[k].title_id,
                g_game_cache[k].path);
    else
      log_debug("  [CACHE] source removed: %s", g_game_cache[k].path);

    memset(&g_game_cache[k], 0, sizeof(g_game_cache[k]));
  }
}

bool find_cached_game(const char *path, const char *title_id,
                      const char **existing_path_out) {
  if (existing_path_out)
    *existing_path_out = NULL;

  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid)
      continue;
    if (path && strcmp(g_game_cache[k].path, path) == 0) {
      if (existing_path_out)
        *existing_path_out = g_game_cache[k].path;
      return true;
    }
    if (title_id && title_id[0] != '\0' &&
        strcmp(g_game_cache[k].title_id, title_id) == 0) {
      if (existing_path_out)
        *existing_path_out = g_game_cache[k].path;
      return true;
    }
  }

  return false;
}

void clear_cached_game(const char *path) {
  if (!path || path[0] == '\0')
    return;

  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid)
      continue;
    if (strcmp(g_game_cache[k].path, path) != 0)
      continue;
    memset(&g_game_cache[k], 0, sizeof(g_game_cache[k]));
  }
}
