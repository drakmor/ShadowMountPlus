#include "sm_platform.h"
#include "sm_runtime.h"
#include "sm_scan.h"
#include "sm_scan_tree.h"
#include "sm_types.h"
#include "sm_game_cache.h"
#include "sm_gameinfo.h"
#include "sm_log.h"
#include "sm_config_mount.h"
#include "sm_mount_device.h"
#include "sm_filesystem.h"
#include "sm_appdb.h"
#include "sm_paths.h"
#include "sm_path_state.h"
#include "sm_path_utils.h"
#include "sm_stability.h"
#include "sm_title_state.h"
#include "sm_image_cache.h"
#include "sm_image.h"

typedef struct {
  char discovered_param_roots[MAX_PENDING][MAX_PATH];
} scan_workspace_t;

// Reuse the largest transient scan buffer instead of placing ~512 KiB of path
// state on the stack each cycle.
static scan_workspace_t g_scan_workspace;

static bool is_under_discovered_param_root(
    const char *path, char discovered_param_roots[][MAX_PATH],
    int discovered_count) {
  for (int i = 0; i < discovered_count; i++) {
    const char *root = discovered_param_roots[i];
    size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0)
      continue;
    if (path[root_len] == '\0' || path[root_len] == '/')
      return true;
  }
  return false;
}

// --- Candidate Discovery ---
typedef struct {
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
} directory_candidate_info_t;

typedef enum {
  DIRECTORY_CANDIDATE_DESCEND = 0,
  DIRECTORY_CANDIDATE_SKIP_DESCEND,
  DIRECTORY_CANDIDATE_READY,
} directory_candidate_probe_t;

typedef enum {
  EXISTING_DIRECTORY_CONTINUE = 0,
  EXISTING_DIRECTORY_HANDLED,
  EXISTING_DIRECTORY_PREFER_CURRENT,
  EXISTING_DIRECTORY_PREFER_CACHED,
} existing_directory_result_t;

static directory_candidate_probe_t probe_directory_candidate(
    const char *full_path, char discovered_param_roots[][MAX_PATH],
    int *discovered_param_root_count, directory_candidate_info_t *info_out) {
  struct stat param_st;

  if (is_under_discovered_param_root(full_path, discovered_param_roots,
                                     *discovered_param_root_count)) {
    return DIRECTORY_CANDIDATE_SKIP_DESCEND;
  }

  if (is_under_image_mount_base(full_path) && !is_active_image_mount_point(full_path)) {
    log_debug("  [SKIP] inactive mount path: %s", full_path);
    return DIRECTORY_CANDIDATE_SKIP_DESCEND;
  }

  if (!directory_has_param_json(full_path, &param_st)) {
    if (is_missing_param_scan_limited(full_path)) {
      log_debug("  [SKIP] param.json retry limit reached: %s", full_path);
    } else {
      record_missing_param_failure(full_path);
    }
    return DIRECTORY_CANDIDATE_DESCEND;
  }

  if (!get_game_info(full_path, &param_st, info_out->title_id,
                     info_out->title_name)) {
    record_missing_param_failure(full_path);
    log_debug("  [SKIP] game info unavailable: %s", full_path);
    return DIRECTORY_CANDIDATE_SKIP_DESCEND;
  }

  if (*discovered_param_root_count < MAX_PENDING) {
    (void)strlcpy(discovered_param_roots[*discovered_param_root_count], full_path,
                  MAX_PATH);
    (*discovered_param_root_count)++;
  }
  clear_missing_param_entry(full_path);
  return DIRECTORY_CANDIDATE_READY;
}

static int find_scan_candidate_index_by_title_id(scan_candidate_t *candidates,
                                                 int candidate_count,
                                                 const char *title_id) {
  for (int i = 0; i < candidate_count; i++) {
    if (strcmp(candidates[i].title_id, title_id) == 0)
      return i;
  }
  return -1;
}

static void remove_scan_candidate_at(scan_candidate_t *candidates,
                                     int *candidate_count, int index) {
  int trailing_count = *candidate_count - index - 1;
  if (trailing_count > 0) {
    memmove(&candidates[index], &candidates[index + 1],
            (size_t)trailing_count * sizeof(candidates[0]));
  }
  (*candidate_count)--;
}

static void notify_duplicate_scan_candidate(const char *title_id,
                                            const char *ignored_path,
                                            const char *existing_path) {
  notify_duplicate_title_once(title_id, ignored_path, existing_path);
}

static existing_directory_result_t handle_existing_directory_candidate(
    const char *full_path, const struct AppDbTitleList *app_db_titles,
    bool app_db_titles_ready, const directory_candidate_info_t *info,
    bool *installed_out, bool *in_app_db_out,
    char preferred_existing_path_out[MAX_PATH]) {
  char tracked_path[MAX_PATH];
  preferred_existing_path_out[0] = '\0';
  bool has_tracked_path =
      read_mount_link(info->title_id, tracked_path, sizeof(tracked_path));
  bool link_matches_source =
      has_tracked_path && strcmp(tracked_path, full_path) == 0;

  if (!app_db_titles_ready) {
    if (link_matches_source && is_data_mounted(info->title_id)) {
      cache_game_entry(full_path, info->title_id, info->title_name);
      return EXISTING_DIRECTORY_PREFER_CURRENT;
    }
    return EXISTING_DIRECTORY_HANDLED;
  }

  bool in_app_db = app_db_title_list_contains(app_db_titles, info->title_id);
  bool installed = in_app_db && is_installed(info->title_id);
  link_matches_source = installed && link_matches_source;
  bool source_valid = false;
  if (link_matches_source) {
    source_valid = is_data_mounted(info->title_id);
    if (!source_valid && mount_title_nullfs(info->title_id, full_path))
      source_valid = is_data_mounted(info->title_id);
  }

  if (source_valid) {
    cache_game_entry(full_path, info->title_id, info->title_name);
    return EXISTING_DIRECTORY_PREFER_CURRENT;
  }

  if (installed && has_tracked_path && strcmp(tracked_path, full_path) != 0 &&
      is_data_mounted(info->title_id)) {
    (void)strlcpy(preferred_existing_path_out, tracked_path, MAX_PATH);
    cache_game_entry(tracked_path, info->title_id, info->title_name);
    return EXISTING_DIRECTORY_PREFER_CACHED;
  }

  if (!in_app_db && was_register_attempted(info->title_id)) {
    uint8_t register_attempts = get_register_attempts(info->title_id);
    log_debug("  [SKIP] register/install retry limit reached (%u/%u): %s (%s)",
              (unsigned)register_attempts, (unsigned)MAX_REGISTER_ATTEMPTS,
              info->title_name, info->title_id);
    return EXISTING_DIRECTORY_HANDLED;
  }

  *installed_out = installed;
  *in_app_db_out = in_app_db;
  return EXISTING_DIRECTORY_CONTINUE;
}

static bool enqueue_directory_candidate(
    const char *full_path, scan_candidate_t *candidates, int max_candidates,
    int *candidate_count, const directory_candidate_info_t *info, bool installed,
    bool in_app_db, bool *unstable_found_out) {
  uint8_t failed_attempts = get_failed_mount_attempts(info->title_id);
  if (failed_attempts >= MAX_FAILED_MOUNT_ATTEMPTS) {
    log_debug("  [SKIP] mount/register retry limit reached (%u/%u): %s (%s)",
              (unsigned)failed_attempts, (unsigned)MAX_FAILED_MOUNT_ATTEMPTS,
              info->title_name, info->title_id);
    return true;
  }

  if (!wait_for_stability_fast(full_path, info->title_name)) {
    if (unstable_found_out)
      *unstable_found_out = true;
    log_debug("  [SKIP] source not stable yet: %s (%s)", info->title_name,
              full_path);
    return true;
  }

  if (*candidate_count >= max_candidates) {
    log_debug("  [SKIP] candidate queue full (%d): %s (%s)", max_candidates,
              info->title_name, info->title_id);
    return true;
  }

  (void)strlcpy(candidates[*candidate_count].path, full_path,
                sizeof(candidates[*candidate_count].path));
  (void)strlcpy(candidates[*candidate_count].title_id, info->title_id,
                sizeof(candidates[*candidate_count].title_id));
  (void)strlcpy(candidates[*candidate_count].title_name, info->title_name,
                sizeof(candidates[*candidate_count].title_name));
  candidates[*candidate_count].installed = installed;
  candidates[*candidate_count].in_app_db = in_app_db;
  (*candidate_count)++;
  return true;
}

static bool try_collect_candidate_for_directory(
    const char *full_path, scan_candidate_t *candidates, int max_candidates,
    int *candidate_count, const struct AppDbTitleList *app_db_titles,
    bool app_db_titles_ready, char discovered_param_roots[][MAX_PATH],
    int *discovered_param_root_count, bool *unstable_found_out) {
  directory_candidate_info_t info;
  directory_candidate_probe_t probe_result =
      probe_directory_candidate(full_path, discovered_param_roots,
                                discovered_param_root_count, &info);

  if (probe_result == DIRECTORY_CANDIDATE_SKIP_DESCEND)
    return true;
  if (probe_result == DIRECTORY_CANDIDATE_DESCEND)
    return false;

  int duplicate_candidate_index = find_scan_candidate_index_by_title_id(
      candidates, *candidate_count, info.title_id);
  const char *duplicate_candidate_path =
      duplicate_candidate_index >= 0 ? candidates[duplicate_candidate_index].path
                                     : NULL;
  bool duplicate_candidate_same_path =
      duplicate_candidate_path && strcmp(duplicate_candidate_path, full_path) == 0;
  bool installed = false;
  bool in_app_db = false;
  char preferred_existing_path[MAX_PATH];
  existing_directory_result_t existing_result =
      handle_existing_directory_candidate(full_path, app_db_titles,
                                          app_db_titles_ready, &info,
                                          &installed, &in_app_db,
                                          preferred_existing_path);
  if (existing_result == EXISTING_DIRECTORY_PREFER_CURRENT) {
    if (duplicate_candidate_index >= 0) {
      if (!duplicate_candidate_same_path)
        notify_duplicate_scan_candidate(info.title_id, duplicate_candidate_path,
                                        full_path);
      remove_scan_candidate_at(candidates, candidate_count,
                               duplicate_candidate_index);
    }
    return true;
  }
  if (existing_result == EXISTING_DIRECTORY_PREFER_CACHED) {
    notify_duplicate_scan_candidate(info.title_id, full_path,
                                    preferred_existing_path);
    if (duplicate_candidate_index >= 0)
      remove_scan_candidate_at(candidates, candidate_count,
                               duplicate_candidate_index);
    return true;
  }
  if (existing_result == EXISTING_DIRECTORY_HANDLED) {
    if (duplicate_candidate_index >= 0 && !duplicate_candidate_same_path)
      notify_duplicate_scan_candidate(info.title_id, full_path,
                                      duplicate_candidate_path);
    return true;
  }

  if (duplicate_candidate_index >= 0) {
    if (!duplicate_candidate_same_path)
      notify_duplicate_scan_candidate(info.title_id, full_path,
                                      duplicate_candidate_path);
    return true;
  }

  return enqueue_directory_candidate(full_path, candidates, max_candidates,
                                     candidate_count, &info, installed,
                                     in_app_db, unstable_found_out);
}

typedef struct {
  scan_candidate_t *candidates;
  int max_candidates;
  int *candidate_count;
  const struct AppDbTitleList *app_db_titles;
  bool app_db_titles_ready;
  char (*discovered_param_roots)[MAX_PATH];
  int *discovered_param_root_count;
  bool *unstable_found_out;
} collect_candidates_walk_ctx_t;

static sm_scan_tree_dir_visit_t collect_candidate_directory_visit(
    const char *dir_path, unsigned int depth_from_root, void *ctx_ptr) {
  if (depth_from_root == 0u)
    return SM_SCAN_TREE_DIR_DESCEND;

  collect_candidates_walk_ctx_t *ctx = (collect_candidates_walk_ctx_t *)ctx_ptr;
  if (try_collect_candidate_for_directory(
          dir_path, ctx->candidates, ctx->max_candidates, ctx->candidate_count,
          ctx->app_db_titles, ctx->app_db_titles_ready,
          ctx->discovered_param_roots, ctx->discovered_param_root_count,
          ctx->unstable_found_out)) {
    return SM_SCAN_TREE_DIR_SKIP_DESCEND;
  }

  return SM_SCAN_TREE_DIR_DESCEND;
}

static bool collect_candidate_image_visit(const char *image_path,
                                          const char *image_name,
                                          unsigned int depth_from_root,
                                          void *ctx_ptr) {
  (void)depth_from_root;

  collect_candidates_walk_ctx_t *ctx = (collect_candidates_walk_ctx_t *)ctx_ptr;
  maybe_mount_image_file(image_path, image_name, ctx->unstable_found_out);
  return true;
}

static bool build_backport_mount_context(const char *title_id,
                                         const char *owning_scan_path,
                                         char backport_path[MAX_PATH],
                                         char system_ex_path[MAX_PATH]) {
  if (!owning_scan_path || owning_scan_path[0] == '\0')
    return false;
  char backport_root[MAX_PATH];
  if (!build_backports_root_path(owning_scan_path, backport_root))
    return false;

  snprintf(backport_path, MAX_PATH, "%s/%s", backport_root, title_id);
  snprintf(system_ex_path, MAX_PATH, "/system_ex/app/%s", title_id);
  return true;
}

typedef struct {
  bool *unstable_found_out;
} backport_overlay_ctx_t;

static bool mount_backport_overlay_for_cached_game(const char *source_path,
                                                   const char *title_id,
                                                   const char *title_name,
                                                   const char *owning_scan_root,
                                                   void *ctx_ptr) {
  (void)title_name;

  if (should_stop_requested())
    return false;

  backport_overlay_ctx_t *ctx = (backport_overlay_ctx_t *)ctx_ptr;
  char backport_path[MAX_PATH];
  char system_ex_path[MAX_PATH];
  if (!build_backport_mount_context(title_id, owning_scan_root, backport_path,
                                    system_ex_path)) {
    return true;
  }

  bool overlay_active = false;
  if (!reconcile_title_backport_mount(title_id, source_path, backport_path,
                                      &overlay_active) ||
      overlay_active) {
    return true;
  }
  if (!wait_for_stability_fast(backport_path, "BKP")) {
    if (ctx->unstable_found_out)
      *ctx->unstable_found_out = true;
    return true;
  }
  overlay_active = false;
  if (!reconcile_title_backport_mount(title_id, source_path, backport_path,
                                      &overlay_active) ||
      overlay_active) {
    return true;
  }

  mount_backport_overlay(system_ex_path, backport_path, title_id);
  return true;
}

static void mount_backport_overlays_internal(const char *scan_root_filter,
                                             bool *unstable_found_out) {
  backport_overlay_ctx_t ctx = {
      .unstable_found_out = unstable_found_out,
  };
  for_each_cached_game_entry(scan_root_filter, mount_backport_overlay_for_cached_game,
                             &ctx);
}

void mount_backport_overlays(bool *unstable_found_out) {
  mount_backport_overlays_internal(NULL, unstable_found_out);
}

void mount_backport_overlays_for_scan_root(const char *scan_root,
                                           bool *unstable_found_out) {
  mount_backport_overlays_internal(scan_root, unstable_found_out);
}

// --- Unified Scan Pass (images + game candidates) ---
void cleanup_lost_sources_before_scan(void) {
  // 1) Drop stale game cache entries for deleted sources.
  prune_game_cache();
  // 2) Drop stale/broken mount links and unmount stale /system_ex stacks.
  cleanup_mount_links(NULL, true);
  // 3) Unmount stale image mounts for deleted image files.
  cleanup_stale_image_mounts();
  // 4) Drop stale path-state entries.
  prune_path_state();
}

void cleanup_lost_sources_for_scan_root(const char *scan_root) {
  prune_game_cache_for_root(scan_root);
  cleanup_mount_links(scan_root, true);
  cleanup_stale_image_mounts_for_root(scan_root);
  prune_path_state_for_root(scan_root);
}

static void collect_scan_candidates_from_root(
    const char *scan_path, scan_candidate_t *candidates, int max_candidates,
    int *candidate_count, const struct AppDbTitleList *app_db_titles,
    bool app_db_titles_ready, char discovered_param_roots[][MAX_PATH],
    int *discovered_param_root_count, bool *unstable_found_out) {
  if (should_stop_requested())
    return;

  unsigned int scan_depth = runtime_config()->scan_depth;
  if (scan_depth < MIN_SCAN_DEPTH)
    scan_depth = MIN_SCAN_DEPTH;

  collect_candidates_walk_ctx_t ctx = {
      .candidates = candidates,
      .max_candidates = max_candidates,
      .candidate_count = candidate_count,
      .app_db_titles = app_db_titles,
      .app_db_titles_ready = app_db_titles_ready,
      .discovered_param_roots = discovered_param_roots,
      .discovered_param_root_count = discovered_param_root_count,
      .unstable_found_out = unstable_found_out,
  };
  sm_scan_tree_callbacks_t callbacks = {
      .on_directory = collect_candidate_directory_visit,
      .on_image_file = collect_candidate_image_visit,
  };
  (void)sm_scan_tree_walk(scan_path, scan_path, 0u, scan_depth, &callbacks, &ctx);
}

int collect_scan_candidates_for_scan_root(const char *scan_root,
                                          scan_candidate_t *candidates,
                                          int max_candidates,
                                          int *total_found_out,
                                          bool *unstable_found_out) {
  int candidate_count = 0;
  const struct AppDbTitleList *app_db_titles = NULL;
  bool app_db_titles_ready = get_app_db_title_list_cached(&app_db_titles);
  int discovered_param_root_count = 0;

  if (!app_db_titles_ready)
    log_debug("  [DB] app.db title list unavailable for this scan cycle");

  collect_scan_candidates_from_root(scan_root, candidates, max_candidates,
                                    &candidate_count, app_db_titles,
                                    app_db_titles_ready,
                                    g_scan_workspace.discovered_param_roots,
                                    &discovered_param_root_count,
                                    unstable_found_out);

  if (total_found_out)
    *total_found_out = discovered_param_root_count;
  return candidate_count;
}

int collect_scan_candidates(scan_candidate_t *candidates, int max_candidates,
                            int *total_found_out,
                            bool *unstable_found_out) {
  int candidate_count = 0;
  const struct AppDbTitleList *app_db_titles = NULL;
  bool app_db_titles_ready = get_app_db_title_list_cached(&app_db_titles);
  int discovered_param_root_count = 0;

  if (!app_db_titles_ready)
    log_debug("  [DB] app.db title list unavailable for this scan cycle");

  for (int i = 0; i < get_scan_path_count(); i++) {
    if (should_stop_requested())
      break;
    collect_scan_candidates_from_root(get_scan_path(i), candidates,
                                      max_candidates,
                                      &candidate_count,
                                      app_db_titles, app_db_titles_ready,
                                      g_scan_workspace.discovered_param_roots,
                                      &discovered_param_root_count,
                                      unstable_found_out);
  }

  if (total_found_out)
    *total_found_out = discovered_param_root_count;
  return candidate_count;
}
