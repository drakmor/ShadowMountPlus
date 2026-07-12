#include "sm_platform.h"
#include "sm_game_cache.h"
#include "sm_config_mount.h"
#include "sm_filesystem.h"
#include "sm_image_cache.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_path_utils.h"
#include "sm_title_state.h"

struct GameCache {
  char path[MAX_PATH];
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  char owning_scan_root[MAX_PATH];
  bool valid;
};

static struct GameCache g_game_cache[MAX_PENDING];

static bool game_cache_source_exists(const struct GameCache *entry) {
  if (path_exists(entry->path))
    return true;
  if (!is_under_image_mount_base(entry->path))
    return false;

  char image_path[MAX_PATH];
  return read_mount_image_link(entry->title_id, image_path,
                               sizeof(image_path)) &&
         path_exists(image_path);
}

static bool resolve_game_cache_owning_scan_root(const char *path,
                                                char owning_scan_root[MAX_PATH]) {
  char resolved_source_path[MAX_PATH];
  const char *match_path = path;
  owning_scan_root[0] = '\0';

  if (is_under_image_mount_base(path) &&
      resolve_image_source_from_mount_cache(path, resolved_source_path,
                                            sizeof(resolved_source_path))) {
    match_path = resolved_source_path;
  }

  size_t best_match_len = 0;
  for (int i = 0; i < get_scan_path_count(); i++) {
    const char *scan_path = get_scan_path(i);
    if (!path_matches_root_or_child(match_path, scan_path))
      continue;

    size_t scan_path_len = strlen(scan_path);
    if (scan_path_len <= best_match_len)
      continue;

    (void)strlcpy(owning_scan_root, scan_path, MAX_PATH);
    best_match_len = scan_path_len;
  }

  return owning_scan_root[0] != '\0';
}

static void write_game_cache_slot(struct GameCache *entry, const char *path,
                                  const char *title_id,
                                  const char *title_name,
                                  const char *owning_scan_root) {
  (void)strlcpy(entry->path, path, sizeof(entry->path));
  (void)strlcpy(entry->title_id, title_id, sizeof(entry->title_id));
  (void)strlcpy(entry->title_name, title_name, sizeof(entry->title_name));
  (void)strlcpy(entry->owning_scan_root, owning_scan_root,
                sizeof(entry->owning_scan_root));
  entry->valid = true;
}

static bool ensure_game_cache_owning_scan_root(struct GameCache *entry) {
  if (entry->owning_scan_root[0] != '\0')
    return true;

  return resolve_game_cache_owning_scan_root(entry->path, entry->owning_scan_root);
}

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
  char owning_scan_root[MAX_PATH];
  (void)resolve_game_cache_owning_scan_root(path, owning_scan_root);

  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid)
      continue;
    if (strcmp(g_game_cache[k].path, path) != 0 &&
        strcmp(g_game_cache[k].title_id, title_id) != 0) {
      continue;
    }
    write_game_cache_slot(&g_game_cache[k], path, title_id, title_name,
                          owning_scan_root);
    return;
  }

  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid) {
      write_game_cache_slot(&g_game_cache[k], path, title_id, title_name,
                            owning_scan_root);
      return;
    }
  }
}

void prune_game_cache(void) {
  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid)
      continue;
    if (game_cache_source_exists(&g_game_cache[k]))
      continue;
    clear_game_cache_slot(k, "source removed");
  }
}

void prune_game_cache_for_root(const char *root) {
  if (!root || root[0] == '\0') {
    prune_game_cache();
    return;
  }

  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid)
      continue;
    const char *entry_root =
        ensure_game_cache_owning_scan_root(&g_game_cache[k])
            ? g_game_cache[k].owning_scan_root
            : g_game_cache[k].path;
    if (!path_matches_root_or_child(entry_root, root))
      continue;
    if (game_cache_source_exists(&g_game_cache[k]))
      continue;
    clear_game_cache_slot(k, "source removed");
  }
}

void for_each_cached_game_entry(const char *root, game_cache_iter_fn fn,
                                void *ctx) {
  if (!fn)
    return;

  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid)
      continue;
    bool has_owning_scan_root = ensure_game_cache_owning_scan_root(&g_game_cache[k]);
    const char *entry_root =
        has_owning_scan_root ? g_game_cache[k].owning_scan_root : g_game_cache[k].path;
    if (root && root[0] != '\0' && strcmp(entry_root, root) != 0) {
      continue;
    }
    if (!fn(g_game_cache[k].path, g_game_cache[k].title_id,
            g_game_cache[k].title_name,
            has_owning_scan_root ? g_game_cache[k].owning_scan_root : NULL, ctx)) {
      break;
    }
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
  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid)
      continue;
    if (strcmp(g_game_cache[k].path, path) != 0)
      continue;
    clear_game_cache_slot(k, "removed from duplicate tracking");
  }
}
