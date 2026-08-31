#ifndef SM_APPDB_H
#define SM_APPDB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sm_limits.h"

struct AppDbTitleList;

typedef struct {
  char title_id[MAX_TITLE_ID];
  char content_id[MAX_CONTENT_ID];
  char title_name[MAX_TITLE_NAME];
  char last_access_time[MAX_APP_DB_TIMESTAMP];
  char install_time[MAX_APP_DB_TIMESTAMP];
  char icon_path[MAX_PATH];
  int platform;
  uint64_t installed_size;
} sm_app_db_game_info_t;

// Close the cached app.db handle and release related resources.
void shutdown_app_db(void);
// Normalize existing snd0info rows during process startup.
bool app_db_run_startup_maintenance(void);
// Update snd0 metadata for a registered title.
int update_snd0info(const char *title_id);
// Normalize an existing snd0Info path for one title.
int normalize_snd0info_for_title(const char *title_id);
// Check whether a title ID exists in a cached app.db title list.
bool app_db_title_list_contains(const struct AppDbTitleList *list,
                                const char *title_id);
// Release a title-list snapshot returned by get_app_db_title_list_cached().
void free_app_db_title_list(struct AppDbTitleList *list);
// Drop the cached app.db title list so it is reloaded on next access.
void invalidate_app_db_title_cache(void);
// Copy the cached app.db title list into caller-owned storage, loading it if needed.
bool get_app_db_title_list_cached(struct AppDbTitleList *list_out);
// Copy titles whose metadata path matches /user/app/PPSA*/sce_sys.
bool get_app_db_auto_remove_title_list(struct AppDbTitleList *list_out);
// Copy the cached PPSA titles that app.db marks as not uninstallable.
bool get_app_db_blocked_uninstall_ppsa_list(struct AppDbTitleList *list_out);
// Allocate a read-only snapshot of game metadata from tbl_contentinfo.
bool app_db_game_info_snapshot(sm_app_db_game_info_t **entries_out,
                               size_t *count_out);
// Find one title in a snapshot sorted by title_id.
const sm_app_db_game_info_t *app_db_find_game_info(
    const sm_app_db_game_info_t *entries, size_t count, const char *title_id);

#endif
