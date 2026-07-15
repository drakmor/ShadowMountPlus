#include "sm_platform.h"

#include <pthread.h>
#include <sqlite3.h>

#include "sm_runtime.h"
#include "sm_types.h"
#include "sm_appdb.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_paths.h"

static sqlite3 *g_app_db;
static sqlite3_stmt *g_app_db_stmt_update_snd0;
static sqlite3_stmt *g_app_db_stmt_normalize_snd0;
static struct AppDbTitleList g_app_db_title_cache;
static struct AppDbTitleList g_app_db_auto_remove_cache;
static struct AppDbTitleList g_app_db_blocked_uninstall_ppsa_cache;
static bool g_app_db_title_cache_ready = false;
static bool g_app_db_auto_remove_cache_ready = false;
static bool g_app_db_blocked_uninstall_ppsa_cache_ready = false;
static time_t g_app_db_title_cache_mtime = 0;
static time_t g_app_db_auto_remove_cache_mtime = 0;
static time_t g_app_db_blocked_uninstall_ppsa_cache_mtime = 0;
static pthread_mutex_t g_app_db_mutex = PTHREAD_MUTEX_INITIALIZER;

#define APP_DB_STARTUP_MAINTENANCE_RETRIES 3
#define APP_DB_STARTUP_MAINTENANCE_BUSY_TIMEOUT_MS 250

static void close_app_db(void) {
  if (g_app_db_stmt_update_snd0) {
    sqlite3_finalize(g_app_db_stmt_update_snd0);
    g_app_db_stmt_update_snd0 = NULL;
  }
  if (g_app_db_stmt_normalize_snd0) {
    sqlite3_finalize(g_app_db_stmt_normalize_snd0);
    g_app_db_stmt_normalize_snd0 = NULL;
  }
  if (g_app_db) {
    sqlite3_close(g_app_db);
    g_app_db = NULL;
  }
}

void free_app_db_title_list(struct AppDbTitleList *list) {
  free(list->ids);
  list->ids = NULL;
  list->count = 0;
  list->capacity = 0;
}

void shutdown_app_db(void) {
  pthread_mutex_lock(&g_app_db_mutex);
  free_app_db_title_list(&g_app_db_title_cache);
  free_app_db_title_list(&g_app_db_auto_remove_cache);
  free_app_db_title_list(&g_app_db_blocked_uninstall_ppsa_cache);
  g_app_db_title_cache_ready = false;
  g_app_db_auto_remove_cache_ready = false;
  g_app_db_blocked_uninstall_ppsa_cache_ready = false;
  g_app_db_title_cache_mtime = 0;
  g_app_db_auto_remove_cache_mtime = 0;
  g_app_db_blocked_uninstall_ppsa_cache_mtime = 0;
  close_app_db();
  pthread_mutex_unlock(&g_app_db_mutex);
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
  pthread_mutex_lock(&g_app_db_mutex);

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
  pthread_mutex_unlock(&g_app_db_mutex);
  return ok;
}

int update_snd0info(const char *title_id) {
  int result = -1;
  pthread_mutex_lock(&g_app_db_mutex);

  if (!g_app_db_stmt_update_snd0) {
    const char *sql =
        "UPDATE tbl_contentinfo "
        "SET snd0info = '/user/appmeta/' || ?1 || '/snd0.at9' "
        "WHERE titleId = ?1;";
    int prep_rc = app_db_prepare_with_retry(sql, &g_app_db_stmt_update_snd0,
                                            APP_DB_PREPARE_BUSY_RETRIES,
                                            "snd0info update");
    if (prep_rc != SQLITE_OK)
      goto out;
  }

  for (int attempt = 0; attempt < APP_DB_UPDATE_BUSY_RETRIES; attempt++) {
    sqlite3_reset(g_app_db_stmt_update_snd0);
    sqlite3_clear_bindings(g_app_db_stmt_update_snd0);
    if (sqlite3_bind_text(g_app_db_stmt_update_snd0, 1, title_id, -1,
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      log_debug("  [DB] bind failed for snd0info update: %s",
                sqlite3_errmsg(g_app_db));
      close_app_db();
      goto out;
    }

    int rc = sqlite3_step(g_app_db_stmt_update_snd0);
    if (rc == SQLITE_DONE) {
      int changes = sqlite3_changes(g_app_db);
      close_app_db();
      result = changes;
      goto out;
    }

    if (app_db_wait_retry(rc, attempt, APP_DB_UPDATE_BUSY_RETRIES, false))
      continue;

    log_debug("  [DB] step failed for snd0info update: rc=%d err=%s", rc,
              sqlite3_errmsg(g_app_db));
    close_app_db();
    goto out;
  }

  close_app_db();
out:
  pthread_mutex_unlock(&g_app_db_mutex);
  return result;
}

int normalize_snd0info_for_title(const char *title_id) {
  int result = -1;
  pthread_mutex_lock(&g_app_db_mutex);

  if (!g_app_db_stmt_normalize_snd0) {
    const char *sql =
        "UPDATE tbl_contentinfo "
        "SET snd0Info = replace(replace(snd0Info, '/user/app/', "
        "'/user/appmeta/'), '/sce_sys', '') "
        "WHERE titleId = ?1 "
        "AND (instr(snd0Info, '/user/app/') > 0 "
        "OR instr(snd0Info, '/sce_sys') > 0);";
    int prep_rc = app_db_prepare_with_retry(sql, &g_app_db_stmt_normalize_snd0,
                                            APP_DB_PREPARE_BUSY_RETRIES,
                                            "snd0info normalize");
    if (prep_rc != SQLITE_OK)
      goto out;
  }

  for (int attempt = 0; attempt < APP_DB_UPDATE_BUSY_RETRIES; attempt++) {
    sqlite3_reset(g_app_db_stmt_normalize_snd0);
    sqlite3_clear_bindings(g_app_db_stmt_normalize_snd0);
    if (sqlite3_bind_text(g_app_db_stmt_normalize_snd0, 1, title_id, -1,
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      log_debug("  [DB] bind failed for snd0info normalize: %s",
                sqlite3_errmsg(g_app_db));
      close_app_db();
      goto out;
    }

    int rc = sqlite3_step(g_app_db_stmt_normalize_snd0);
    if (rc == SQLITE_DONE) {
      int changes = sqlite3_changes(g_app_db);
      close_app_db();
      result = changes;
      goto out;
    }

    if (app_db_wait_retry(rc, attempt, APP_DB_UPDATE_BUSY_RETRIES, false))
      continue;

    log_debug("  [DB] step failed for snd0info normalize: rc=%d err=%s", rc,
              sqlite3_errmsg(g_app_db));
    close_app_db();
    goto out;
  }

  close_app_db();
out:
  pthread_mutex_unlock(&g_app_db_mutex);
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

static bool step_app_db_title_query(struct AppDbTitleList *list,
                                    sqlite3_stmt *stmt,
                                    const char *label) {
  int busy_attempts = 0;
  while (!should_stop_requested()) {
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      const char *title_id = (const char *)sqlite3_column_text(stmt, 0);
      if (!append_app_db_title(list, title_id)) {
        log_debug("  [DB] %s allocation failed", label);
        sqlite3_finalize(stmt);
        close_app_db();
        free_app_db_title_list(list);
        return false;
      }
      continue;
    }
    if (rc == SQLITE_DONE) {
      sqlite3_finalize(stmt);
      close_app_db();
      return true;
    }
    if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) &&
        busy_attempts + 1 < APP_DB_QUERY_BUSY_RETRIES) {
      busy_attempts++;
      sceKernelUsleep(APP_DB_BUSY_RETRY_SLEEP_US);
      continue;
    }

    log_debug("  [DB] %s query failed: rc=%d err=%s", label, rc,
              sqlite3_errmsg(g_app_db));
    sqlite3_finalize(stmt);
    close_app_db();
    free_app_db_title_list(list);
    return false;
  }

  sqlite3_finalize(stmt);
  close_app_db();
  free_app_db_title_list(list);
  return false;
}

static bool load_app_db_title_list(struct AppDbTitleList *list) {
  free_app_db_title_list(list);

  const char *sql =
      "SELECT DISTINCT titleId "
      "FROM tbl_contentinfo "
      "WHERE titleId != '' "
      "ORDER BY titleId;";
  sqlite3_stmt *stmt = NULL;
  int prep_rc = app_db_prepare_with_retry(sql, &stmt,
                                          APP_DB_PREPARE_BUSY_RETRIES,
                                          "title list query");
  if (prep_rc != SQLITE_OK)
    return false;

  bool loaded = step_app_db_title_query(list, stmt, "title list");
  if (loaded)
    log_debug("  [DB] loaded app.db title list: %d entries", list->count);
  return loaded;
}

static bool load_app_db_auto_remove_title_list(struct AppDbTitleList *list) {
  free_app_db_title_list(list);

  const char *sql =
      "SELECT DISTINCT titleId "
      "FROM tbl_contentinfo "
      "WHERE metaDataPath GLOB '/user/app/PPSA*/sce_sys' "
      "ORDER BY titleId;";
  sqlite3_stmt *stmt = NULL;
  int prep_rc = app_db_prepare_with_retry(sql, &stmt,
                                          APP_DB_PREPARE_BUSY_RETRIES,
                                          "auto-remove title query");
  if (prep_rc != SQLITE_OK)
    return false;

  bool loaded = step_app_db_title_query(list, stmt, "auto-remove title list");
  if (loaded)
    log_debug("  [DB] loaded app.db auto-remove title list: %d entries",
              list->count);
  return loaded;
}

static bool load_app_db_blocked_uninstall_ppsa_list(struct AppDbTitleList *list) {
  free_app_db_title_list(list);

  const char *sql =
      "SELECT DISTINCT titleId "
      "FROM tbl_contentinfo "
      "WHERE titleId LIKE 'PPSA%' "
      "AND uninstallable = 0 "
      "ORDER BY titleId;";
  sqlite3_stmt *stmt = NULL;
  int prep_rc =
      app_db_prepare_with_retry(sql, &stmt, APP_DB_PREPARE_BUSY_RETRIES,
                                "blocked PPSA uninstallable query");
  if (prep_rc != SQLITE_OK)
    return false;

  bool loaded = step_app_db_title_query(list, stmt, "blocked PPSA uninstall");
  if (loaded)
    log_debug("  [DB] loaded blocked PPSA uninstall list: %d entries",
              list->count);
  return loaded;
}

void invalidate_app_db_title_cache(void) {
  pthread_mutex_lock(&g_app_db_mutex);
  g_app_db_title_cache_ready = false;
  g_app_db_auto_remove_cache_ready = false;
  g_app_db_blocked_uninstall_ppsa_cache_ready = false;
  g_app_db_title_cache_mtime = 0;
  g_app_db_auto_remove_cache_mtime = 0;
  g_app_db_blocked_uninstall_ppsa_cache_mtime = 0;
  pthread_mutex_unlock(&g_app_db_mutex);
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
    struct AppDbTitleList *list_out, struct AppDbTitleList *cache,
    bool *cache_ready, time_t *cache_mtime, load_app_db_title_list_fn load_fn) {
  free_app_db_title_list(list_out);

  struct stat st;
  int app_db_stat_rc = stat(APP_DB_PATH, &st);
  if (!*cache_ready ||
      (app_db_stat_rc == 0 && *cache_mtime != st.st_mtime)) {
    struct AppDbTitleList fresh = {0};
    if (app_db_stat_rc == 0 && load_fn(&fresh)) {
      free_app_db_title_list(cache);
      *cache = fresh;
      *cache_mtime = st.st_mtime;
      *cache_ready = true;
    } else {
      free_app_db_title_list(&fresh);
      if (!*cache_ready)
        return false;
    }
  }

  return copy_app_db_title_list(list_out, cache);
}

bool get_app_db_title_list_cached(struct AppDbTitleList *list_out) {
  if (!list_out)
    return false;

  pthread_mutex_lock(&g_app_db_mutex);
  bool copied = get_cached_app_db_title_list(
      list_out, &g_app_db_title_cache, &g_app_db_title_cache_ready,
      &g_app_db_title_cache_mtime, load_app_db_title_list);
  pthread_mutex_unlock(&g_app_db_mutex);
  return copied;
}

bool get_app_db_auto_remove_title_list(struct AppDbTitleList *list_out) {
  if (!list_out)
    return false;

  pthread_mutex_lock(&g_app_db_mutex);
  bool copied = get_cached_app_db_title_list(
      list_out, &g_app_db_auto_remove_cache,
      &g_app_db_auto_remove_cache_ready, &g_app_db_auto_remove_cache_mtime,
      load_app_db_auto_remove_title_list);
  pthread_mutex_unlock(&g_app_db_mutex);
  return copied;
}

bool get_app_db_blocked_uninstall_ppsa_list(struct AppDbTitleList *list_out) {
  if (!list_out)
    return false;

  pthread_mutex_lock(&g_app_db_mutex);
  bool loaded = get_cached_app_db_title_list(
      list_out, &g_app_db_blocked_uninstall_ppsa_cache,
      &g_app_db_blocked_uninstall_ppsa_cache_ready,
      &g_app_db_blocked_uninstall_ppsa_cache_mtime,
      load_app_db_blocked_uninstall_ppsa_list);
  pthread_mutex_unlock(&g_app_db_mutex);
  return loaded;
}
