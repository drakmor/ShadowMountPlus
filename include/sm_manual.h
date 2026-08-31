#ifndef SM_MANUAL_H
#define SM_MANUAL_H

#include <stdbool.h>

#include "sm_limits.h"

struct AppDbTitleList;

// Normalize one absolute manual source path using manual.lst rules.
bool sm_manual_normalize_path(const char *path, char out[MAX_PATH]);
// Visit each manual source path from manual.lst.
bool sm_manual_for_each_path(bool (*visit)(const char *path, void *ctx),
                             void *ctx);
// Idempotently add or remove one normalized absolute source path. The list is
// replaced atomically and changed_out reports whether its contents changed.
bool sm_manual_add_path(const char *path, bool *changed_out);
bool sm_manual_remove_path(const char *path, bool *changed_out);
// Mark previously installed manual titles as deleted when they disappear from
// app.db, and remove their source lines from manual.lst.
bool sm_manual_reconcile_deleted_titles(const struct AppDbTitleList *app_db_titles,
                                        bool app_db_titles_ready);
// Record that a manual source is installed and associated with a title.
void sm_manual_note_installed(const char *manual_source_path,
                              const char *title_id,
                              const char *title_name);

#endif
