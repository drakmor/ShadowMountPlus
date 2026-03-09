#ifndef SM_APPDB_H
#define SM_APPDB_H

#include <stdbool.h>

struct AppDbTitleList;

// Close the cached app.db handle and release related resources.
void shutdown_app_db(void);
// Update snd0 metadata for a registered title.
int update_snd0info(const char *title_id);
// Check whether a title ID exists in a cached app.db title list.
bool app_db_title_list_contains(const struct AppDbTitleList *list,
                                const char *title_id);
// Drop the cached app.db title list so it is reloaded on next access.
void invalidate_app_db_title_cache(void);
// Return the cached app.db title list, loading it if needed.
bool get_app_db_title_list_cached(const struct AppDbTitleList **list_out);

#endif
