#include "sm_platform.h"

#include <sqlite3.h>

#include "sm_runtime.h"
#include "sm_types.h"
#include "sm_appdb.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_paths.h"

static sqlite3 *g_app_db;
static sqlite3_stmt *g_app_db_stmt_update_snd0;
static struct AppDbTitleList g_app_db_title_cache;
static bool g_app_db_title_cache_ready = false;
static time_t g_app_db_title_cache_mtime = 0;

static void close_app_db(void) {
  if (g_app_db_stmt_update_snd0) {
    sqlite3_finalize(g_app_db_stmt_update_snd0);
    g_app_db_stmt_update_snd0 = NULL;
  }
  if (g_app_db) {
    sqlite3_close(g_app_db);
    g_app_db = NULL;
  }
}

static void free_app_db_title_list(struct AppDbTitleList *list) {
  if (!list)
    return;
  free(list->ids);
  list->ids = NULL;
  list->count = 0;
  list->capacity = 0;
}

void shutdown_app_db(void) {
  free_app_db_title_list(&g_app_db_title_cache);
  g_app_db_title_cache_ready = false;
  g_app_db_title_cache_mtime = 0;
  close_app_db();
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

int update_snd0info(const char *title_id) {
  if (!title_id || title_id[0] == '\0')
    return -1;

  if (!g_app_db_stmt_update_snd0) {
    const char *sql =
        "UPDATE tbl_contentinfo "
        "SET snd0info = '/user/appmeta/' || ?1 || '/snd0.at9' "
        "WHERE titleId = ?1;";
    int prep_rc = app_db_prepare_with_retry(sql, &g_app_db_stmt_update_snd0,
                                            APP_DB_PREPARE_BUSY_RETRIES,
                                            "snd0info update");
    if (prep_rc != SQLITE_OK)
      return -1;
  }

  for (int attempt = 0; attempt < APP_DB_UPDATE_BUSY_RETRIES; attempt++) {
    sqlite3_reset(g_app_db_stmt_update_snd0);
    sqlite3_clear_bindings(g_app_db_stmt_update_snd0);
    if (sqlite3_bind_text(g_app_db_stmt_update_snd0, 1, title_id, -1,
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      if (g_app_db) {
        log_debug("  [DB] bind failed for snd0info update: %s",
                  sqlite3_errmsg(g_app_db));
      }
      close_app_db();
      return -1;
    }

    int rc = sqlite3_step(g_app_db_stmt_update_snd0);
    if (rc == SQLITE_DONE) {
      int changes = sqlite3_changes(g_app_db);
      close_app_db();
      return changes;
    }

    if (app_db_wait_retry(rc, attempt, APP_DB_UPDATE_BUSY_RETRIES, false))
      continue;

    if (g_app_db) {
      log_debug("  [DB] step failed for snd0info update: rc=%d err=%s", rc,
                sqlite3_errmsg(g_app_db));
    }
    close_app_db();
    return -1;
  }

  close_app_db();
  return -1;
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
  if (!list || !title_id || title_id[0] == '\0')
    return false;
  return bsearch(title_id, list->ids, (size_t)list->count, sizeof(*list->ids),
                 compare_title_id_str) != NULL;
}

static bool load_app_db_title_list(struct AppDbTitleList *list) {
  if (!list)
    return false;
  free_app_db_title_list(list);

  const char *sql =
      "SELECT DISTINCT titleId "
      "FROM tbl_contentinfo "
      "WHERE titleId != '' "
      "ORDER BY titleId;";
  sqlite3_stmt *stmt = NULL;
  int prep_rc = app_db_prepare_with_retry(sql, &stmt, APP_DB_PREPARE_BUSY_RETRIES,
                                          "title list query");
  if (prep_rc != SQLITE_OK) {
    close_app_db();
    return false;
  }

  int busy_attempts = 0;
  while (!should_stop_requested()) {
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      const char *title_id = (const char *)sqlite3_column_text(stmt, 0);
      if (title_id && title_id[0] != '\0') {
        if (!append_app_db_title(list, title_id)) {
          log_debug("  [DB] title list allocation failed");
          sqlite3_finalize(stmt);
          close_app_db();
          free_app_db_title_list(list);
          return false;
        }
      }
      continue;
    }
    if (rc == SQLITE_DONE) {
      sqlite3_finalize(stmt);
      close_app_db();
      log_debug("  [DB] loaded app.db title list: %d entries", list->count);
      return true;
    }
    if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) &&
        busy_attempts + 1 < APP_DB_QUERY_BUSY_RETRIES) {
      busy_attempts++;
      sceKernelUsleep(APP_DB_BUSY_RETRY_SLEEP_US);
      continue;
    }

    log_debug("  [DB] title list query failed: rc=%d err=%s", rc,
              (g_app_db ? sqlite3_errmsg(g_app_db) : "unknown"));
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

void invalidate_app_db_title_cache(void) {
  g_app_db_title_cache_ready = false;
  g_app_db_title_cache_mtime = 0;
}

bool get_app_db_title_list_cached(const struct AppDbTitleList **list_out) {
  if (!list_out)
    return false;
  *list_out = NULL;

  struct stat st;
  int app_db_stat_rc = stat(APP_DB_PATH, &st);

  if (!g_app_db_title_cache_ready ||
      (app_db_stat_rc == 0 && g_app_db_title_cache_mtime != st.st_mtime)) {
    struct AppDbTitleList fresh = {0};
    if (app_db_stat_rc == 0 && load_app_db_title_list(&fresh)) {
      free_app_db_title_list(&g_app_db_title_cache);
      g_app_db_title_cache = fresh;
      g_app_db_title_cache_mtime = st.st_mtime;
      g_app_db_title_cache_ready = true;
    } else {
      free_app_db_title_list(&fresh);
      if (!g_app_db_title_cache_ready)
        return false;
    }
  }

  *list_out = &g_app_db_title_cache;
  return true;
}
