#include "sm_platform.h"
#include <pthread.h>

#include "sm_game_cache.h"
#include "sm_appdb.h"
#include "sm_config_mount.h"
#include "sm_image_cache.h"
#include "sm_image_index.h"
#include "sm_install_queue.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_path_utils.h"
#include "sm_time.h"
#include "sm_title_state.h"

struct GameCache {
  char path[MAX_PATH];
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  char owning_scan_root[MAX_PATH];
  uint64_t missing_since_us;
  bool valid;
};

static struct GameCache g_game_cache[MAX_PENDING];
static pthread_mutex_t g_game_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool game_cache_source_exists(const struct GameCache *entry) {
  return entry->path[0] != '\0' && path_exists(entry->path);
}

static bool title_is_protected_by_dlc(const char *title_id,
                                      bool remove_games_with_dlc) {
  if (remove_games_with_dlc)
    return false;

  char addcont_path[MAX_PATH];
  int written = snprintf(addcont_path, sizeof(addcont_path),
                         "/user/addcont/%s", title_id);
  if (written <= 0 || (size_t)written >= sizeof(addcont_path))
    return true;

  struct stat st;
  if (stat(addcont_path, &st) == 0)
    return S_ISDIR(st.st_mode);

  return errno != ENOENT && errno != ENOTDIR;
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
  entry->missing_since_us = 0;
  entry->valid = true;
}

static bool ensure_game_cache_owning_scan_root(struct GameCache *entry) {
  if (entry->owning_scan_root[0] != '\0')
    return true;

  return resolve_game_cache_owning_scan_root(entry->path, entry->owning_scan_root);
}

static void start_missing_timer(struct GameCache *entry, uint64_t now_us) {
  if (entry->missing_since_us != 0 || now_us == 0)
    return;

  entry->missing_since_us = now_us;
  if (entry->path[0] != '\0') {
    log_debug("  [CACHE] source missing, auto-remove timer started: %s (%s)",
              entry->title_id, entry->path);
  } else {
    log_debug("  [CACHE] source missing, auto-remove timer started: %s",
              entry->title_id);
  }
}

static bool missing_timer_expired(const struct GameCache *entry,
                                  uint64_t now_us, uint64_t delay_us) {
  return entry->valid && entry->missing_since_us != 0 &&
         now_us >= entry->missing_since_us &&
         now_us - entry->missing_since_us >= delay_us;
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

  pthread_mutex_lock(&g_game_cache_mutex);
  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid)
      continue;
    if (strcmp(g_game_cache[k].path, path) != 0 &&
        strcmp(g_game_cache[k].title_id, title_id) != 0) {
      continue;
    }
    write_game_cache_slot(&g_game_cache[k], path, title_id, title_name,
                          owning_scan_root);
    pthread_mutex_unlock(&g_game_cache_mutex);
    return;
  }

  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid) {
      write_game_cache_slot(&g_game_cache[k], path, title_id, title_name,
                            owning_scan_root);
      pthread_mutex_unlock(&g_game_cache_mutex);
      return;
    }
  }
  pthread_mutex_unlock(&g_game_cache_mutex);
}

static void prune_game_cache_entries(const char *root) {
  const bool auto_remove = runtime_config()->auto_remove_missing_games;

  pthread_mutex_lock(&g_game_cache_mutex);
  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid)
      continue;
    if (root) {
      const char *entry_root =
          ensure_game_cache_owning_scan_root(&g_game_cache[k])
              ? g_game_cache[k].owning_scan_root
              : g_game_cache[k].path;
      if (!path_matches_root_or_child(entry_root, root))
        continue;
    }

    struct GameCache *entry = &g_game_cache[k];
    if (game_cache_source_exists(entry)) {
      if (entry->missing_since_us != 0) {
        entry->missing_since_us = 0;
        log_debug("  [CACHE] source restored, auto-remove cancelled: %s (%s)",
                  entry->title_id, entry->path);
      }
      continue;
    }
    if (auto_remove)
      continue;
    clear_game_cache_slot(k, "source removed");
  }
  pthread_mutex_unlock(&g_game_cache_mutex);
}

void prune_game_cache(void) { prune_game_cache_entries(NULL); }

void prune_game_cache_for_root(const char *root) {
  if (!root || root[0] == '\0') {
    prune_game_cache();
    return;
  }

  prune_game_cache_entries(root);
}

void note_game_cache_source_seen(const char *path, const char *title_id,
                                 const char *title_name) {
  if (!path || !title_id || title_id[0] == '\0')
    return;

  pthread_mutex_lock(&g_game_cache_mutex);
  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid || g_game_cache[k].missing_since_us == 0)
      continue;
    if (strcmp(g_game_cache[k].title_id, title_id) != 0)
      continue;

    char owning_scan_root[MAX_PATH];
    (void)resolve_game_cache_owning_scan_root(path, owning_scan_root);
    write_game_cache_slot(&g_game_cache[k], path, title_id,
                          title_name ? title_name : "", owning_scan_root);
    log_debug("  [CACHE] source restored, auto-remove cancelled: %s (%s)",
              title_id, path);
    break;
  }
  pthread_mutex_unlock(&g_game_cache_mutex);
}

static void reconcile_missing_game_cache(
    const struct AppDbTitleList *auto_remove_titles, uint64_t now_us,
    bool remove_games_with_dlc) {
  pthread_mutex_lock(&g_game_cache_mutex);
  for (int k = 0; k < MAX_PENDING; ++k) {
    struct GameCache *entry = &g_game_cache[k];
    if (!entry->valid || game_cache_source_exists(entry) ||
        app_db_title_list_contains(auto_remove_titles, entry->title_id)) {
      continue;
    }
    clear_game_cache_slot(k, "source removed");
  }
  pthread_mutex_unlock(&g_game_cache_mutex);

  bool cache_full_logged = false;
  for (int i = 0; i < auto_remove_titles->count; ++i) {
    const char *title_id = auto_remove_titles->ids[i];
    bool protected_by_dlc =
        title_is_protected_by_dlc(title_id, remove_games_with_dlc);
    bool image_source_exists =
        !protected_by_dlc && sm_image_index_has_source_for_title(title_id);

    pthread_mutex_lock(&g_game_cache_mutex);
    int entry_index = -1;
    int free_index = -1;
    for (int k = 0; k < MAX_PENDING; ++k) {
      if (!g_game_cache[k].valid) {
        if (free_index < 0)
          free_index = k;
        continue;
      }
      if (strcmp(g_game_cache[k].title_id, title_id) == 0) {
        entry_index = k;
        break;
      }
    }

    if (protected_by_dlc) {
      if (entry_index >= 0) {
        struct GameCache *entry = &g_game_cache[entry_index];
        if (game_cache_source_exists(entry))
          entry->missing_since_us = 0;
        else
          clear_game_cache_slot(entry_index, NULL);
      }
    } else if (entry_index >= 0) {
      struct GameCache *entry = &g_game_cache[entry_index];
      if (game_cache_source_exists(entry) || image_source_exists) {
        if (entry->missing_since_us != 0) {
          log_debug("  [CACHE] source restored, auto-remove cancelled: %s",
                    title_id);
        }
        if (entry->path[0] == '\0')
          clear_game_cache_slot(entry_index, NULL);
        else
          entry->missing_since_us = 0;
      } else {
        start_missing_timer(entry, now_us);
      }
    } else if (!image_source_exists && free_index >= 0) {
      struct GameCache *entry = &g_game_cache[free_index];
      write_game_cache_slot(entry, "", title_id, "", "");
      start_missing_timer(entry, now_us);
    } else if (!image_source_exists && !cache_full_logged) {
      log_debug("  [CACHE] auto-remove tracking full; remaining titles deferred");
      cache_full_logged = true;
    }
    pthread_mutex_unlock(&g_game_cache_mutex);
  }
}

void reconcile_missing_app_db_games(void) {
  const runtime_config_t *cfg = runtime_config();
  if (!cfg->auto_remove_missing_games)
    return;

  struct AppDbTitleList auto_remove_titles = {0};
  if (!get_app_db_auto_remove_title_list(&auto_remove_titles)) {
    log_debug("  [REG] app.db auto-remove title list unavailable");
    return;
  }

  const uint64_t now_us = monotonic_time_us();
  const uint64_t delay_us =
      (uint64_t)cfg->auto_remove_missing_delay_seconds * 1000000ull;
  reconcile_missing_game_cache(&auto_remove_titles, now_us,
                               cfg->auto_remove_games_with_dlc);

  for (;;) {
    char title_id[MAX_TITLE_ID];
    title_id[0] = '\0';

    pthread_mutex_lock(&g_game_cache_mutex);
    for (int k = 0; k < MAX_PENDING; k++) {
      const struct GameCache *entry = &g_game_cache[k];
      if (!missing_timer_expired(entry, now_us, delay_us))
        continue;

      (void)strlcpy(title_id, entry->title_id, sizeof(title_id));
      clear_game_cache_slot(k, NULL);
      break;
    }
    pthread_mutex_unlock(&g_game_cache_mutex);

    if (title_id[0] == '\0')
      break;

    if (!app_db_title_list_contains(&auto_remove_titles, title_id))
      continue;
    if (title_is_protected_by_dlc(title_id,
                                  cfg->auto_remove_games_with_dlc))
      continue;

    int res = sceAppInstUtilAppUnInstall(title_id);
    if (res == 0) {
      sm_install_forget_pending_title(title_id);
      invalidate_app_db_title_cache();
      log_debug("  [REG] auto-remove requested: %s", title_id);
      continue;
    }

    log_debug("  [REG] auto-remove failed (not retried): %s code=0x%08X",
              title_id, (uint32_t)res);
  }

  free_app_db_title_list(&auto_remove_titles);
}

void reset_missing_game_cache_timers(void) {
  pthread_mutex_lock(&g_game_cache_mutex);
  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid)
      continue;
    if (g_game_cache[k].path[0] == '\0')
      clear_game_cache_slot(k, NULL);
    else
      g_game_cache[k].missing_since_us = 0;
  }
  pthread_mutex_unlock(&g_game_cache_mutex);
}

void for_each_cached_game_entry(const char *root, game_cache_iter_fn fn,
                                void *ctx) {
  if (!fn)
    return;

  for (int k = 0; k < MAX_PENDING; k++) {
    struct GameCache entry;
    bool valid = false;
    pthread_mutex_lock(&g_game_cache_mutex);
    if (g_game_cache[k].valid && g_game_cache[k].missing_since_us == 0) {
      bool has_owning_scan_root =
          ensure_game_cache_owning_scan_root(&g_game_cache[k]);
      const char *entry_root = has_owning_scan_root
                                   ? g_game_cache[k].owning_scan_root
                                   : g_game_cache[k].path;
      if (!root || root[0] == '\0' || strcmp(entry_root, root) == 0) {
        (void)strlcpy(entry.path, g_game_cache[k].path, sizeof(entry.path));
        (void)strlcpy(entry.title_id, g_game_cache[k].title_id,
                      sizeof(entry.title_id));
        (void)strlcpy(entry.title_name, g_game_cache[k].title_name,
                      sizeof(entry.title_name));
        (void)strlcpy(entry.owning_scan_root,
                      has_owning_scan_root ? g_game_cache[k].owning_scan_root
                                           : "",
                      sizeof(entry.owning_scan_root));
        valid = true;
      }
    }
    pthread_mutex_unlock(&g_game_cache_mutex);
    if (!valid)
      continue;
    if (!fn(entry.path, entry.title_id, entry.title_name,
            entry.owning_scan_root[0] != '\0' ? entry.owning_scan_root : NULL,
            ctx)) {
      break;
    }
  }
}

bool sm_game_cache_snapshot(sm_game_cache_snapshot_entry_t **entries_out,
                            size_t *count_out) {
  if (!entries_out || !count_out)
    return false;
  *entries_out = NULL;
  *count_out = 0;

  pthread_mutex_lock(&g_game_cache_mutex);
  size_t count = 0;
  for (int k = 0; k < MAX_PENDING; ++k) {
    if (g_game_cache[k].valid && g_game_cache[k].missing_since_us == 0)
      count++;
  }

  sm_game_cache_snapshot_entry_t *entries = NULL;
  if (count > 0) {
    entries = calloc(count, sizeof(*entries));
    if (!entries) {
      pthread_mutex_unlock(&g_game_cache_mutex);
      return false;
    }
  }

  size_t copied = 0;
  for (int k = 0; k < MAX_PENDING; ++k) {
    if (!g_game_cache[k].valid || g_game_cache[k].missing_since_us != 0)
      continue;
    sm_game_cache_snapshot_entry_t *entry = &entries[copied++];
    (void)strlcpy(entry->path, g_game_cache[k].path, sizeof(entry->path));
    (void)strlcpy(entry->title_id, g_game_cache[k].title_id,
                  sizeof(entry->title_id));
    (void)strlcpy(entry->title_name, g_game_cache[k].title_name,
                  sizeof(entry->title_name));
  }
  pthread_mutex_unlock(&g_game_cache_mutex);
  *entries_out = entries;
  *count_out = copied;
  return true;
}

bool find_cached_game(const char *path, const char *title_id,
                      const char **existing_path_out) {
  if (existing_path_out)
    *existing_path_out = NULL;

  pthread_mutex_lock(&g_game_cache_mutex);
  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid)
      continue;
    if (path && strcmp(g_game_cache[k].path, path) == 0) {
      if (existing_path_out)
        *existing_path_out = g_game_cache[k].path;
      pthread_mutex_unlock(&g_game_cache_mutex);
      return true;
    }
    if (title_id && title_id[0] != '\0' &&
        strcmp(g_game_cache[k].title_id, title_id) == 0) {
      if (existing_path_out)
        *existing_path_out = g_game_cache[k].path;
      pthread_mutex_unlock(&g_game_cache_mutex);
      return true;
    }
  }

  pthread_mutex_unlock(&g_game_cache_mutex);
  return false;
}

void clear_cached_game(const char *path) {
  pthread_mutex_lock(&g_game_cache_mutex);
  for (int k = 0; k < MAX_PENDING; k++) {
    if (!g_game_cache[k].valid || g_game_cache[k].missing_since_us != 0)
      continue;
    if (strcmp(g_game_cache[k].path, path) != 0)
      continue;
    clear_game_cache_slot(k, "removed from duplicate tracking");
  }
  pthread_mutex_unlock(&g_game_cache_mutex);
}
