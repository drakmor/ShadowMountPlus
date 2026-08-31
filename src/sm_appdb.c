#include "sm_platform.h"

#include <json-c/json.h>
#include <pthread.h>
#include <sqlite3.h>

#include "sm_runtime.h"
#include "sm_types.h"
#include "sm_appdb.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_paths.h"

static sqlite3 *g_app_db;

#define APP_DB_API_BUSY_TIMEOUT_MS 250

struct AppDbTitleCache {
  struct AppDbTitleList list;
  uint64_t revision;
  time_t mtime;
  bool ready;
};

static struct AppDbTitleCache g_app_db_title_cache;
static struct AppDbTitleCache g_app_db_auto_remove_cache;
static struct AppDbTitleCache g_app_db_blocked_uninstall_ppsa_cache;
static pthread_mutex_t g_app_db_write_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_app_db_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

#define APP_DB_STARTUP_MAINTENANCE_RETRIES 3
#define APP_DB_STARTUP_MAINTENANCE_BUSY_TIMEOUT_MS 250

static bool close_sqlite_handle(sqlite3 **db, const char *label) {
  if (!*db)
    return true;

  int rc = sqlite3_close_v2(*db);
  if (rc != SQLITE_OK) {
    log_debug("  [DB] close failed for %s: rc=%d err=%s", label, rc,
              sqlite3_errmsg(*db));
    return false;
  }
  *db = NULL;
  return true;
}

static bool close_app_db(void) {
  return close_sqlite_handle(&g_app_db, "write connection");
}

void free_app_db_title_list(struct AppDbTitleList *list) {
  free(list->ids);
  list->ids = NULL;
  list->count = 0;
  list->capacity = 0;
}

static void invalidate_app_db_title_cache_entry(
    struct AppDbTitleCache *cache) {
  cache->ready = false;
  cache->mtime = 0;
  ++cache->revision;
}

void shutdown_app_db(void) {
  pthread_mutex_lock(&g_app_db_write_mutex);
  (void)close_app_db();
  pthread_mutex_unlock(&g_app_db_write_mutex);

  pthread_mutex_lock(&g_app_db_cache_mutex);
  free_app_db_title_list(&g_app_db_title_cache.list);
  free_app_db_title_list(&g_app_db_auto_remove_cache.list);
  free_app_db_title_list(&g_app_db_blocked_uninstall_ppsa_cache.list);
  invalidate_app_db_title_cache_entry(&g_app_db_title_cache);
  invalidate_app_db_title_cache_entry(&g_app_db_auto_remove_cache);
  invalidate_app_db_title_cache_entry(
      &g_app_db_blocked_uninstall_ppsa_cache);
  pthread_mutex_unlock(&g_app_db_cache_mutex);
}

static bool ensure_app_db_open(void) {
  if (!g_app_db) {
    if (sqlite3_open_v2(APP_DB_PATH, &g_app_db, SQLITE_OPEN_READWRITE, NULL) !=
        SQLITE_OK) {
      log_debug("  [DB] open failed: %s",
                (g_app_db ? sqlite3_errmsg(g_app_db) : APP_DB_PATH));
      close_app_db();
      return false;
    }
    (void)sqlite3_busy_timeout(g_app_db, APP_DB_BUSY_TIMEOUT_MS);
  }
  return true;
}

static bool app_db_wait_retry(int rc, int attempt, int max_attempts,
                              bool close_before_retry) {
  if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED)
    return false;
  if (attempt + 1 >= max_attempts || should_stop_requested())
    return false;
  if (close_before_retry)
    close_app_db();
  sceKernelUsleep(APP_DB_BUSY_RETRY_SLEEP_US);
  return true;
}

static int app_db_prepare_with_retry(const char *sql, sqlite3_stmt **stmt_out,
                                     int max_attempts, const char *label) {
  for (int attempt = 0; attempt < max_attempts; attempt++) {
    if (!ensure_app_db_open())
      return -1;

    int rc = sqlite3_prepare_v2(g_app_db, sql, -1, stmt_out, NULL);
    if (rc == SQLITE_OK)
      return SQLITE_OK;

    if (app_db_wait_retry(rc, attempt, max_attempts, true))
      continue;

    log_debug("  [DB] prepare failed for %s: rc=%d err=%s", label, rc,
              (g_app_db ? sqlite3_errmsg(g_app_db) : "unknown"));
    close_app_db();
    return rc;
  }

  close_app_db();
  return -1;
}

static bool app_db_exec_with_retry(const char *sql, int max_attempts,
                                   const char *label, int *changes_out) {
  if (changes_out)
    *changes_out = 0;

  for (int attempt = 0; attempt < max_attempts; attempt++) {
    if (!ensure_app_db_open())
      return false;

    char *err_msg = NULL;
    int rc = sqlite3_exec(g_app_db, sql, NULL, NULL, &err_msg);
    if (rc == SQLITE_OK) {
      if (changes_out)
        *changes_out = sqlite3_changes(g_app_db);
      sqlite3_free(err_msg);
      return true;
    }

    if (app_db_wait_retry(rc, attempt, max_attempts, false)) {
      sqlite3_free(err_msg);
      continue;
    }

    if (err_msg) {
      log_debug("  [DB] exec failed for %s: rc=%d err=%s", label, rc, err_msg);
    } else {
      log_debug("  [DB] exec failed for %s: rc=%d err=%s", label, rc,
                sqlite3_errmsg(g_app_db));
    }
    sqlite3_free(err_msg);

    close_app_db();
    return false;
  }

  close_app_db();
  return false;
}

static int app_db_query_int(const char *sql, const char *label) {
  sqlite3_stmt *stmt = NULL;
  int prep_rc = app_db_prepare_with_retry(sql, &stmt,
                                          APP_DB_STARTUP_MAINTENANCE_RETRIES,
                                          label);
  if (prep_rc != SQLITE_OK)
    return -1;

  int result = -1;
  for (int attempt = 0; attempt < APP_DB_QUERY_BUSY_RETRIES; attempt++) {
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      result = sqlite3_column_int(stmt, 0);
      break;
    }
    if (app_db_wait_retry(rc, attempt, APP_DB_QUERY_BUSY_RETRIES, false))
      continue;

    log_debug("  [DB] query failed for %s: rc=%d err=%s", label, rc,
              sqlite3_errmsg(g_app_db));
    break;
  }

  sqlite3_finalize(stmt);
  return result;
}

bool app_db_run_startup_maintenance(void) {
  bool ok = false;
  pthread_mutex_lock(&g_app_db_write_mutex);

  if (!ensure_app_db_open())
    goto out;
  (void)sqlite3_busy_timeout(g_app_db,
                             APP_DB_STARTUP_MAINTENANCE_BUSY_TIMEOUT_MS);

  const char *backfill_count_sql =
      "SELECT COUNT(*) FROM tbl_contentinfo "
      "WHERE instr(snd0Info, '/user/app/') > 0 "
      "OR instr(snd0Info, '/sce_sys') > 0;";
  int pending_backfill =
      app_db_query_int(backfill_count_sql, "snd0info normalize backfill check");
  if (pending_backfill < 0) {
    close_app_db();
    goto out;
  }

  int changes = 0;
  if (pending_backfill > 0) {
    const char *backfill_sql =
        "UPDATE tbl_contentinfo "
        "SET snd0Info = replace(replace(snd0Info, '/user/app/', "
        "'/user/appmeta/'), '/sce_sys', '') "
        "WHERE instr(snd0Info, '/user/app/') > 0 "
        "OR instr(snd0Info, '/sce_sys') > 0;";

    if (!app_db_exec_with_retry(backfill_sql,
                                APP_DB_STARTUP_MAINTENANCE_RETRIES,
                                "snd0info startup normalize", &changes))
      goto out;
  }

  log_debug("  [DB] snd0info startup maintenance done rows=%d pending=%d",
            changes, pending_backfill);

  close_app_db();
  ok = true;

out:
  pthread_mutex_unlock(&g_app_db_write_mutex);
  return ok;
}

static int app_db_update_title(const char *title_id, const char *sql,
                               const char *label) {
  int result = -1;
  sqlite3_stmt *stmt = NULL;
  int prep_rc = app_db_prepare_with_retry(
      sql, &stmt, APP_DB_PREPARE_BUSY_RETRIES, label);
  if (prep_rc != SQLITE_OK)
    return -1;

  for (int attempt = 0; attempt < APP_DB_UPDATE_BUSY_RETRIES; ++attempt) {
    (void)sqlite3_reset(stmt);
    (void)sqlite3_clear_bindings(stmt);
    if (sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_TRANSIENT) !=
        SQLITE_OK) {
      log_debug("  [DB] bind failed for %s: %s", label,
                sqlite3_errmsg(g_app_db));
      break;
    }

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
      result = sqlite3_changes(g_app_db);
      break;
    }

    if (app_db_wait_retry(rc, attempt, APP_DB_UPDATE_BUSY_RETRIES, false))
      continue;

    log_debug("  [DB] step failed for %s: rc=%d err=%s", label, rc,
              sqlite3_errmsg(g_app_db));
    break;
  }

  (void)sqlite3_finalize(stmt);
  (void)close_app_db();
  return result;
}

int update_snd0info(const char *title_id) {
  const char *sql =
      "UPDATE tbl_contentinfo "
      "SET snd0info = '/user/appmeta/' || ?1 || '/snd0.at9' "
      "WHERE titleId = ?1;";

  pthread_mutex_lock(&g_app_db_write_mutex);
  int result = app_db_update_title(title_id, sql, "snd0info update");
  pthread_mutex_unlock(&g_app_db_write_mutex);
  return result;
}

int normalize_snd0info_for_title(const char *title_id) {
  const char *sql =
      "UPDATE tbl_contentinfo "
      "SET snd0Info = replace(replace(snd0Info, '/user/app/', "
      "'/user/appmeta/'), '/sce_sys', '') "
      "WHERE titleId = ?1 "
      "AND (instr(snd0Info, '/user/app/') > 0 "
      "OR instr(snd0Info, '/sce_sys') > 0);";

  pthread_mutex_lock(&g_app_db_write_mutex);
  int result = app_db_update_title(title_id, sql, "snd0info normalize");
  pthread_mutex_unlock(&g_app_db_write_mutex);
  return result;
}

static bool append_app_db_title(struct AppDbTitleList *list,
                                const char *title_id) {
  if (!list || !title_id || title_id[0] == '\0')
    return true;

  if (list->count >= list->capacity) {
    int new_capacity = (list->capacity > 0) ? (list->capacity * 2) : 1024;
    char(*new_ids)[MAX_TITLE_ID] =
        realloc(list->ids, (size_t)new_capacity * sizeof(*list->ids));
    if (!new_ids)
      return false;
    list->ids = new_ids;
    list->capacity = new_capacity;
  }

  (void)strlcpy(list->ids[list->count], title_id, MAX_TITLE_ID);
  list->count++;
  return true;
}

static int compare_title_id_str(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

bool app_db_title_list_contains(const struct AppDbTitleList *list,
                                const char *title_id) {
  if (list->count <= 0)
    return false;
  return bsearch(title_id, list->ids, (size_t)list->count, sizeof(*list->ids),
                 compare_title_id_str) != NULL;
}

static bool step_app_db_title_query(struct AppDbTitleList *list, sqlite3 *db,
                                    sqlite3_stmt *stmt, const char *label) {
  int busy_attempts = 0;
  while (!should_stop_requested()) {
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      const char *title_id = (const char *)sqlite3_column_text(stmt, 0);
      if (append_app_db_title(list, title_id))
        continue;
      log_debug("  [DB] %s allocation failed", label);
      return false;
    }
    if (rc == SQLITE_DONE)
      return true;
    if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) &&
        busy_attempts + 1 < APP_DB_QUERY_BUSY_RETRIES) {
      ++busy_attempts;
      sceKernelUsleep(APP_DB_BUSY_RETRY_SLEEP_US);
      continue;
    }

    log_debug("  [DB] %s query failed: rc=%d err=%s", label, rc,
              sqlite3_errmsg(db));
    return false;
  }

  return false;
}

static bool load_app_db_title_query(struct AppDbTitleList *list,
                                    const char *sql, const char *label) {
  free_app_db_title_list(list);
  if (should_stop_requested())
    return false;

  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(APP_DB_PATH, &db, SQLITE_OPEN_READONLY, NULL);
  if (rc != SQLITE_OK) {
    log_debug("  [DB] open failed for %s: rc=%d err=%s", label, rc,
              db ? sqlite3_errmsg(db) : APP_DB_PATH);
    if (db)
      (void)close_sqlite_handle(&db, label);
    return false;
  }
  (void)sqlite3_busy_timeout(db, APP_DB_BUSY_TIMEOUT_MS);

  sqlite3_stmt *stmt = NULL;
  for (int attempt = 0; attempt < APP_DB_PREPARE_BUSY_RETRIES; ++attempt) {
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK)
      break;
    if ((rc != SQLITE_BUSY && rc != SQLITE_LOCKED) ||
        attempt + 1 >= APP_DB_PREPARE_BUSY_RETRIES ||
        should_stop_requested()) {
      log_debug("  [DB] prepare failed for %s: rc=%d err=%s", label, rc,
                sqlite3_errmsg(db));
      break;
    }
    sceKernelUsleep(APP_DB_BUSY_RETRY_SLEEP_US);
  }

  bool loaded = rc == SQLITE_OK &&
                step_app_db_title_query(list, db, stmt, label);

  if (stmt)
    (void)sqlite3_finalize(stmt);
  if (!close_sqlite_handle(&db, label))
    loaded = false;
  if (!loaded)
    free_app_db_title_list(list);
  return loaded;
}

static bool load_app_db_title_list(struct AppDbTitleList *list) {
  const char *sql =
      "SELECT DISTINCT titleId "
      "FROM tbl_contentinfo "
      "WHERE titleId != '' "
      "ORDER BY titleId;";

  bool loaded = load_app_db_title_query(list, sql, "title list");
  if (loaded)
    log_debug("  [DB] loaded app.db title list: %d entries", list->count);
  return loaded;
}

static bool load_app_db_auto_remove_title_list(struct AppDbTitleList *list) {
  const char *sql =
      "SELECT DISTINCT titleId "
      "FROM tbl_contentinfo "
      "WHERE metaDataPath GLOB '/user/app/PPSA*/sce_sys' "
      "ORDER BY titleId;";
  bool loaded =
      load_app_db_title_query(list, sql, "auto-remove title list");
  if (loaded)
    log_debug("  [DB] loaded app.db auto-remove title list: %d entries",
              list->count);
  return loaded;
}

static bool load_app_db_blocked_uninstall_ppsa_list(struct AppDbTitleList *list) {
  const char *sql =
      "SELECT DISTINCT titleId "
      "FROM tbl_contentinfo "
      "WHERE titleId LIKE 'PPSA%' "
      "AND uninstallable = 0 "
      "ORDER BY titleId;";
  bool loaded =
      load_app_db_title_query(list, sql, "blocked PPSA uninstall");
  if (loaded)
    log_debug("  [DB] loaded blocked PPSA uninstall list: %d entries",
              list->count);
  return loaded;
}

void invalidate_app_db_title_cache(void) {
  pthread_mutex_lock(&g_app_db_cache_mutex);
  invalidate_app_db_title_cache_entry(&g_app_db_title_cache);
  invalidate_app_db_title_cache_entry(&g_app_db_auto_remove_cache);
  invalidate_app_db_title_cache_entry(
      &g_app_db_blocked_uninstall_ppsa_cache);
  pthread_mutex_unlock(&g_app_db_cache_mutex);
}

static bool copy_app_db_title_list(struct AppDbTitleList *dst,
                                   const struct AppDbTitleList *src) {
  if (!dst || !src)
    return false;

  free_app_db_title_list(dst);
  if (src->count <= 0)
    return true;

  char(*ids)[MAX_TITLE_ID] = malloc((size_t)src->count * sizeof(*ids));
  if (!ids)
    return false;

  memcpy(ids, src->ids, (size_t)src->count * sizeof(*ids));
  dst->ids = ids;
  dst->count = src->count;
  dst->capacity = src->count;
  return true;
}

typedef bool (*load_app_db_title_list_fn)(struct AppDbTitleList *list);

static bool get_cached_app_db_title_list(
    struct AppDbTitleList *list_out, struct AppDbTitleCache *cache,
    load_app_db_title_list_fn load_fn) {
  free_app_db_title_list(list_out);

  struct stat st;
  int app_db_stat_rc = stat(APP_DB_PATH, &st);

  // ShellCore can keep app.db busy for several five-second timeout windows
  // immediately after WORKING. The pre-suspend snapshot is sufficient for
  // mounting already registered games and keeps resume scans non-blocking.
  pthread_mutex_lock(&g_app_db_cache_mutex);
  bool cache_is_current =
      cache->ready &&
      (runtime_resume_grace_active() || app_db_stat_rc != 0 ||
       cache->mtime == st.st_mtime);
  if (cache_is_current) {
    bool copied = copy_app_db_title_list(list_out, &cache->list);
    pthread_mutex_unlock(&g_app_db_cache_mutex);
    return copied;
  }
  uint64_t revision = cache->revision;
  pthread_mutex_unlock(&g_app_db_cache_mutex);

  struct AppDbTitleList fresh = {0};
  bool loaded = app_db_stat_rc == 0 && load_fn(&fresh);
  struct stat verified_st;
  bool source_unchanged =
      loaded && stat(APP_DB_PATH, &verified_st) == 0 &&
      verified_st.st_mtime == st.st_mtime;

  pthread_mutex_lock(&g_app_db_cache_mutex);
  if (source_unchanged && cache->revision == revision) {
    free_app_db_title_list(&cache->list);
    cache->list = fresh;
    memset(&fresh, 0, sizeof(fresh));
    cache->mtime = st.st_mtime;
    cache->ready = true;
    ++cache->revision;
  }
  bool copied = cache->ready &&
                copy_app_db_title_list(list_out, &cache->list);
  pthread_mutex_unlock(&g_app_db_cache_mutex);
  free_app_db_title_list(&fresh);
  return copied;
}

bool get_app_db_title_list_cached(struct AppDbTitleList *list_out) {
  if (!list_out)
    return false;

  return get_cached_app_db_title_list(list_out, &g_app_db_title_cache,
                                      load_app_db_title_list);
}

bool get_app_db_auto_remove_title_list(struct AppDbTitleList *list_out) {
  if (!list_out)
    return false;

  return get_cached_app_db_title_list(
      list_out, &g_app_db_auto_remove_cache,
      load_app_db_auto_remove_title_list);
}

bool get_app_db_blocked_uninstall_ppsa_list(struct AppDbTitleList *list_out) {
  if (!list_out)
    return false;

  return get_cached_app_db_title_list(
      list_out, &g_app_db_blocked_uninstall_ppsa_cache,
      load_app_db_blocked_uninstall_ppsa_list);
}

static void copy_sqlite_text(sqlite3_stmt *stmt, int column, char *out,
                             size_t out_size) {
  const unsigned char *value = sqlite3_column_text(stmt, column);
  (void)strlcpy(out, value ? (const char *)value : "", out_size);
}

static void copy_json_string(struct json_object *object, const char *key,
                             char *out, size_t out_size) {
  struct json_object *value = NULL;
  if (!json_object_object_get_ex(object, key, &value) ||
      !json_object_is_type(value, json_type_string)) {
    return;
  }
  const char *text = json_object_get_string(value);
  if (text && text[0] != '\0')
    (void)strlcpy(out, text, out_size);
}

static void apply_app_info_timestamps(sqlite3_stmt *stmt, int column,
                                      sm_app_db_game_info_t *entry) {
  // ShellCore updates these JSON values while the top-level timestamp columns
  // can remain at their initial install values.
  const unsigned char *text = sqlite3_column_text(stmt, column);
  if (!text || text[0] == '\0')
    return;

  struct json_object *app_info =
      json_tokener_parse((const char *)text);
  if (!app_info)
    return;
  copy_json_string(app_info, "#_last_access_time", entry->last_access_time,
                   sizeof(entry->last_access_time));
  copy_json_string(app_info, "#_install_time", entry->install_time,
                   sizeof(entry->install_time));
  json_object_put(app_info);
}

bool app_db_game_info_snapshot(sm_app_db_game_info_t **entries_out,
                               size_t *count_out) {
  if (!entries_out || !count_out)
    return false;
  *entries_out = NULL;
  *count_out = 0;

  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(APP_DB_PATH, &db, SQLITE_OPEN_READONLY, NULL);
  if (rc != SQLITE_OK) {
    if (db)
      (void)close_sqlite_handle(&db, "game metadata snapshot");
    errno = EIO;
    return false;
  }
  (void)sqlite3_busy_timeout(db, APP_DB_API_BUSY_TIMEOUT_MS);

  static const char sql[] =
      "SELECT titleId, contentId, titleName, lastAccessTime, installTime, "
      "icon0Info, platform, size, AppInfoJson FROM tbl_contentinfo "
      "WHERE titleId != '' ORDER BY titleId COLLATE BINARY;";
  sqlite3_stmt *stmt = NULL;
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    log_debug("  [DB] prepare failed for game metadata snapshot: rc=%d err=%s",
              rc, sqlite3_errmsg(db));
    (void)close_sqlite_handle(&db, "game metadata snapshot");
    errno = EIO;
    return false;
  }

  sm_app_db_game_info_t *entries = NULL;
  size_t count = 0;
  size_t capacity = 0;
  int busy_attempts = 0;
  bool loaded = false;
  while (!should_stop_requested() && !runtime_sleep_mode_active()) {
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      busy_attempts = 0;
      if (count == capacity) {
        size_t new_capacity = capacity > 0 ? capacity * 2u : 128u;
        if (new_capacity < capacity ||
            new_capacity > SIZE_MAX / sizeof(*entries)) {
          errno = ENOMEM;
          break;
        }
        sm_app_db_game_info_t *new_entries =
            realloc(entries, new_capacity * sizeof(*new_entries));
        if (!new_entries) {
          errno = ENOMEM;
          break;
        }
        entries = new_entries;
        capacity = new_capacity;
      }

      sm_app_db_game_info_t *entry = &entries[count++];
      memset(entry, 0, sizeof(*entry));
      copy_sqlite_text(stmt, 0, entry->title_id, sizeof(entry->title_id));
      copy_sqlite_text(stmt, 1, entry->content_id, sizeof(entry->content_id));
      copy_sqlite_text(stmt, 2, entry->title_name, sizeof(entry->title_name));
      copy_sqlite_text(stmt, 3, entry->last_access_time,
                       sizeof(entry->last_access_time));
      copy_sqlite_text(stmt, 4, entry->install_time,
                       sizeof(entry->install_time));
      copy_sqlite_text(stmt, 5, entry->icon_path, sizeof(entry->icon_path));
      entry->platform = sqlite3_column_type(stmt, 6) == SQLITE_NULL
                            ? -1
                            : sqlite3_column_int(stmt, 6);
      sqlite3_int64 installed_size = sqlite3_column_int64(stmt, 7);
      entry->installed_size =
          installed_size > 0 ? (uint64_t)installed_size : 0;
      apply_app_info_timestamps(stmt, 8, entry);
      continue;
    }
    if (rc == SQLITE_DONE) {
      loaded = true;
      break;
    }
    if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) &&
        ++busy_attempts < APP_DB_QUERY_BUSY_RETRIES) {
      sceKernelUsleep(APP_DB_BUSY_RETRY_SLEEP_US);
      continue;
    }
    log_debug("  [DB] game metadata snapshot failed: rc=%d err=%s", rc,
              sqlite3_errmsg(db));
    errno = EIO;
    break;
  }

  (void)sqlite3_finalize(stmt);
  if (!close_sqlite_handle(&db, "game metadata snapshot"))
    loaded = false;
  if (!loaded) {
    if (should_stop_requested() || runtime_sleep_mode_active())
      errno = ECANCELED;
    else if (errno == 0)
      errno = EIO;
    free(entries);
    return false;
  }

  *entries_out = entries;
  *count_out = count;
  return true;
}

const sm_app_db_game_info_t *app_db_find_game_info(
    const sm_app_db_game_info_t *entries, size_t count, const char *title_id) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t mid = low + (high - low) / 2u;
    int comparison = strcmp(entries[mid].title_id, title_id);
    if (comparison < 0)
      low = mid + 1u;
    else
      high = mid;
  }
  return low < count && strcmp(entries[low].title_id, title_id) == 0
             ? &entries[low]
             : NULL;
}
