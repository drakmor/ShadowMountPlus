#include "sm_platform.h"
#include "sm_game_cache.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_title_state.h"

struct GameCache {
  char path[MAX_PATH];
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  bool valid;
};

static struct GameCache g_game_cache[MAX_PENDING];

static void clear_game_cache_slot(int index, const char *reason) {
  if (index < 0 || index >= MAX_PENDING || !g_game_cache[index].valid)
    return;

  if (reason && reason[0] != '\0') {
    if (g_game_cache[index].title_id[0] != '\0')
      log_debug("  [CACHE] %s: %s (%s)", reason, g_game_cache[index].title_id,
                g_game_cache[index].path);
    else
      log_debug("  [CACHE] %s: %s", reason, g_game_cache[index].path);
  }

  if (g_game_cache[index].title_id[0] != '\0')
    clear_duplicate_title_notification(g_game_cache[index].title_id);

  memset(&g_game_cache[index], 0, sizeof(g_game_cache[index]));
}

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
    clear_game_cache_slot(k, "source removed");
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
    clear_game_cache_slot(k, "removed from duplicate tracking");
  }
}
