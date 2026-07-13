#include "sm_platform.h"
#include "sm_runtime.h"
#include "sm_scan.h"
#include "sm_scan_tree.h"
#include "sm_types.h"
#include "sm_game_cache.h"
#include "sm_game_lifecycle.h"
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
#include "sm_image_index.h"
#include "sm_image.h"
#include "sm_install_queue.h"
#include "sm_manual.h"
#include "sm_shellcore_service.h"

typedef struct {
  char discovered_param_roots[MAX_PENDING][MAX_PATH];
  char checked_appmeta_titles[MAX_PENDING][MAX_TITLE_ID];
  char blocked_ppsa_uninstall_titles[MAX_PENDING][MAX_TITLE_ID];
  bool checked_appmeta_present[MAX_PENDING];
  int checked_appmeta_count;
  int blocked_ppsa_uninstall_count;
} scan_workspace_t;

// Reuse the largest transient scan buffer instead of placing ~512 KiB of path
// state on the stack each cycle.
static scan_workspace_t g_scan_workspace;

static void reset_scan_workspace(void) {
  g_scan_workspace.checked_appmeta_count = 0;
  g_scan_workspace.blocked_ppsa_uninstall_count = 0;
}

static bool blocked_ppsa_uninstall_requested(const char *title_id) {
  if (!title_id || title_id[0] == '\0')
    return false;

  for (int i = 0; i < g_scan_workspace.blocked_ppsa_uninstall_count; i++) {
    if (strcmp(g_scan_workspace.blocked_ppsa_uninstall_titles[i], title_id) == 0)
      return true;
  }
  return false;
}

static void remember_blocked_ppsa_uninstall(const char *title_id) {
  if (!title_id || title_id[0] == '\0' ||
      blocked_ppsa_uninstall_requested(title_id))
    return;

  if (g_scan_workspace.blocked_ppsa_uninstall_count >= MAX_PENDING)
    return;

  int slot = g_scan_workspace.blocked_ppsa_uninstall_count++;
  (void)strlcpy(g_scan_workspace.blocked_ppsa_uninstall_titles[slot], title_id,
                sizeof(g_scan_workspace.blocked_ppsa_uninstall_titles[slot]));
}

static bool is_blocked_ppsa_title(
    const struct AppDbTitleList *blocked_ppsa_titles,
    bool blocked_ppsa_titles_ready, const char *title_id) {
  return blocked_ppsa_titles_ready &&
         app_db_title_list_contains(blocked_ppsa_titles, title_id);
}

static bool request_blocked_ppsa_uninstall(const char *title_id,
                                           const char *source_path) {
  if (!title_id || title_id[0] == '\0' ||
      blocked_ppsa_uninstall_requested(title_id))
    return false;

  remember_blocked_ppsa_uninstall(title_id);
  int res = sceAppInstUtilAppUnInstall(title_id);
  if (res == 0) {
    sm_install_forget_pending_title(title_id);
    invalidate_app_db_title_cache();
    if (source_path && source_path[0] != '\0') {
      log_debug("  [REG] requested blocked PPSA uninstall: %s source=%s",
                title_id, source_path);
    } else {
      log_debug("  [REG] requested blocked PPSA uninstall: %s", title_id);
    }
    return true;
  }

  log_debug("  [REG] blocked PPSA uninstall failed: %s code=0x%08X", title_id,
            (uint32_t)res);
  return false;
}

static bool get_appmeta_present_for_scan_cycle(const char *title_id) {
  for (int i = 0; i < g_scan_workspace.checked_appmeta_count; i++) {
    if (strcmp(g_scan_workspace.checked_appmeta_titles[i], title_id) == 0)
      return g_scan_workspace.checked_appmeta_present[i];
  }

  bool present = has_appmeta_data(title_id);
  if (g_scan_workspace.checked_appmeta_count < MAX_PENDING) {
    int slot = g_scan_workspace.checked_appmeta_count++;
    (void)strlcpy(g_scan_workspace.checked_appmeta_titles[slot], title_id,
                  sizeof(g_scan_workspace.checked_appmeta_titles[slot]));
    g_scan_workspace.checked_appmeta_present[slot] = present;
  }
  return present;
}

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

typedef struct {
  const struct AppDbTitleList *titles;
  const struct AppDbTitleList *blocked_ppsa_titles;
  bool titles_ready;
  bool blocked_ppsa_titles_ready;
} scan_app_db_context_t;

static directory_candidate_probe_t probe_directory_candidate(
    const char *full_path, char discovered_param_roots[][MAX_PATH],
    int *discovered_param_root_count, bool allow_known_param_root,
    directory_candidate_info_t *info_out) {
  struct stat param_st;

  if (is_pfsc_image_mount_base_or_child(full_path))
    return DIRECTORY_CANDIDATE_DESCEND;

  if (!allow_known_param_root &&
      is_under_discovered_param_root(full_path, discovered_param_roots,
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

  if (*discovered_param_root_count < MAX_PENDING &&
      !is_under_discovered_param_root(full_path, discovered_param_roots,
                                      *discovered_param_root_count)) {
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

static void mark_scan_candidate_manual(scan_candidate_t *candidate,
                                       const char *manual_source_path) {
  if (!candidate || !manual_source_path || manual_source_path[0] == '\0')
    return;

  candidate->manual = true;
  (void)strlcpy(candidate->manual_source_path, manual_source_path,
                sizeof(candidate->manual_source_path));
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

static bool defer_full_candidate_batch(int candidate_count,
                                       int max_candidates,
                                       bool *incomplete_found_out) {
  if (candidate_count < max_candidates)
    return false;
  if (incomplete_found_out)
    *incomplete_found_out = true;
  return true;
}

static existing_directory_result_t handle_existing_directory_candidate(
    const char *full_path, const scan_app_db_context_t *app_db,
    const directory_candidate_info_t *info,
    bool *installed_out, bool *in_app_db_out,
    char preferred_existing_path_out[MAX_PATH]) {
  char tracked_path[MAX_PATH];
  preferred_existing_path_out[0] = '\0';
  bool has_tracked_path =
      read_mount_link(info->title_id, tracked_path, sizeof(tracked_path));
  bool link_matches_source =
      has_tracked_path && strcmp(tracked_path, full_path) == 0;

  if (is_blocked_ppsa_title(app_db->blocked_ppsa_titles,
                            app_db->blocked_ppsa_titles_ready,
                            info->title_id)) {
    request_blocked_ppsa_uninstall(info->title_id, full_path);
    return EXISTING_DIRECTORY_HANDLED;
  }

  if (!app_db->titles_ready) {
    if (link_matches_source && is_data_mounted(info->title_id)) {
      cache_game_entry(full_path, info->title_id, info->title_name);
      return EXISTING_DIRECTORY_PREFER_CURRENT;
    }
    return EXISTING_DIRECTORY_HANDLED;
  }

  bool in_app_db = app_db_title_list_contains(app_db->titles, info->title_id);
  bool installed = in_app_db && is_installed(info->title_id);
  bool appmeta_present =
      installed ? get_appmeta_present_for_scan_cycle(info->title_id) : false;
  link_matches_source = installed && link_matches_source;
  bool source_valid = false;
  if (link_matches_source) {
    char eboot_path[MAX_PATH];
    int written = snprintf(eboot_path, sizeof(eboot_path), "%s/eboot.bin",
                           full_path);
    source_valid = written > 0 && (size_t)written < sizeof(eboot_path) &&
                   path_exists(eboot_path);
  }

  if (source_valid && appmeta_present) {
    cache_game_entry(full_path, info->title_id, info->title_name);
    return EXISTING_DIRECTORY_PREFER_CURRENT;
  }

  if (installed && appmeta_present && has_tracked_path &&
      strcmp(tracked_path, full_path) != 0 &&
      is_data_mounted(info->title_id)) {
    (void)strlcpy(preferred_existing_path_out, tracked_path, MAX_PATH);
    cache_game_entry(tracked_path, info->title_id, info->title_name);
    return EXISTING_DIRECTORY_PREFER_CACHED;
  }

  if (!in_app_db && is_title_install_pending(info->title_id))
    return EXISTING_DIRECTORY_HANDLED;

  if (!in_app_db && was_register_attempted(info->title_id)) {
    return EXISTING_DIRECTORY_HANDLED;
  }

  *installed_out = installed;
  *in_app_db_out = in_app_db;
  return EXISTING_DIRECTORY_CONTINUE;
}

static bool enqueue_directory_candidate(
    const char *full_path, scan_candidate_t *candidates, int max_candidates,
    int *candidate_count, const directory_candidate_info_t *info, bool installed,
    bool in_app_db, const char *manual_source_path,
    bool *unstable_found_out) {
  char metadata_path[MAX_PATH];
  uint8_t failed_attempts = get_failed_mount_attempts(info->title_id);
  if (failed_attempts >= MAX_FAILED_MOUNT_ATTEMPTS) {
    log_debug("  [SKIP] mount/register retry limit reached (%u/%u): %s (%s)",
              (unsigned)failed_attempts, (unsigned)MAX_FAILED_MOUNT_ATTEMPTS,
              info->title_name, info->title_id);
    return true;
  }

  int written =
      snprintf(metadata_path, sizeof(metadata_path), "%s/sce_sys", full_path);
  if (written < 0 || (size_t)written >= sizeof(metadata_path)) {
    log_debug("  [SKIP] metadata path too long: %s (%s)", info->title_name,
              full_path);
    return true;
  }

  if (!wait_for_stability_fast(metadata_path, info->title_name)) {
    if (unstable_found_out)
      *unstable_found_out = true;
    log_debug("  [SKIP] source not stable yet: %s (%s)", info->title_name,
              full_path);
    return true;
  }

  if (defer_full_candidate_batch(*candidate_count, max_candidates,
                                 unstable_found_out)) {
    log_debug("  [SKIP] candidate queue full (%d), deferring: %s (%s)",
              max_candidates, info->title_name, info->title_id);
    return true;
  }

  (void)strlcpy(candidates[*candidate_count].path, full_path,
                sizeof(candidates[*candidate_count].path));
  (void)strlcpy(candidates[*candidate_count].title_id, info->title_id,
                sizeof(candidates[*candidate_count].title_id));
  (void)strlcpy(candidates[*candidate_count].title_name, info->title_name,
                sizeof(candidates[*candidate_count].title_name));
  candidates[*candidate_count].manual_source_path[0] = '\0';
  candidates[*candidate_count].installed = installed;
  candidates[*candidate_count].in_app_db = in_app_db;
  candidates[*candidate_count].manual = false;
  mark_scan_candidate_manual(&candidates[*candidate_count], manual_source_path);
  (*candidate_count)++;
  return true;
}

static bool try_collect_candidate_for_directory(
    const char *full_path, scan_candidate_t *candidates, int max_candidates,
    int *candidate_count, const scan_app_db_context_t *app_db,
    char discovered_param_roots[][MAX_PATH],
    int *discovered_param_root_count, bool allow_known_param_root,
    const char *manual_source_path, bool *unstable_found_out) {
  directory_candidate_info_t info;
  directory_candidate_probe_t probe_result =
      probe_directory_candidate(full_path, discovered_param_roots,
                                discovered_param_root_count,
                                allow_known_param_root, &info);

  if (probe_result == DIRECTORY_CANDIDATE_SKIP_DESCEND)
    return true;
  if (probe_result == DIRECTORY_CANDIDATE_DESCEND)
    return false;

  if (!sm_image_index_record_game(full_path, info.title_id) &&
      unstable_found_out) {
    *unstable_found_out = true;
  }

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
      handle_existing_directory_candidate(full_path, app_db, &info,
                                          &installed, &in_app_db,
                                          preferred_existing_path);
  if (existing_result == EXISTING_DIRECTORY_PREFER_CURRENT) {
    if (manual_source_path)
      sm_manual_note_installed(manual_source_path, info.title_id,
                               info.title_name);
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
    if (duplicate_candidate_index >= 0)
      mark_scan_candidate_manual(&candidates[duplicate_candidate_index],
                                 manual_source_path);
    return true;
  }

  if (duplicate_candidate_index >= 0) {
    if (!duplicate_candidate_same_path)
      notify_duplicate_scan_candidate(info.title_id, full_path,
                                      duplicate_candidate_path);
    mark_scan_candidate_manual(&candidates[duplicate_candidate_index],
                               manual_source_path);
    return true;
  }

  return enqueue_directory_candidate(full_path, candidates, max_candidates,
                                     candidate_count, &info, installed,
                                     in_app_db, manual_source_path,
                                     unstable_found_out);
}

typedef struct {
  scan_candidate_t *candidates;
  int max_candidates;
  int *candidate_count;
  const scan_app_db_context_t *app_db;
  char (*discovered_param_roots)[MAX_PATH];
  int *discovered_param_root_count;
  const char *manual_source_path;
  bool *unstable_found_out;
} collect_candidates_walk_ctx_t;

static void collect_scan_candidates_from_manual_root(
    const char *scan_path, const char *manual_source_path,
    scan_candidate_t *candidates, int max_candidates, int *candidate_count,
    const scan_app_db_context_t *app_db,
    char discovered_param_roots[][MAX_PATH], int *discovered_param_root_count,
    bool *unstable_found_out);

static sm_scan_tree_dir_visit_t collect_candidate_directory_visit(
    const char *dir_path, unsigned int depth_from_root, void *ctx_ptr) {
  if (depth_from_root == 0u)
    return SM_SCAN_TREE_DIR_DESCEND;

  collect_candidates_walk_ctx_t *ctx = (collect_candidates_walk_ctx_t *)ctx_ptr;
  if (defer_full_candidate_batch(*ctx->candidate_count, ctx->max_candidates,
                                 ctx->unstable_found_out)) {
    return SM_SCAN_TREE_DIR_ABORT;
  }
  if (try_collect_candidate_for_directory(
          dir_path, ctx->candidates, ctx->max_candidates, ctx->candidate_count,
          ctx->app_db, ctx->discovered_param_roots,
          ctx->discovered_param_root_count, false, ctx->manual_source_path,
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
  if (defer_full_candidate_batch(*ctx->candidate_count, ctx->max_candidates,
                                 ctx->unstable_found_out)) {
    return false;
  }
  struct stat image_st;
  if (stat(image_path, &image_st) != 0)
    return true;
  bool persistent_image = !is_under_image_mount_base(image_path);
  if (persistent_image &&
      !sm_image_index_needs_scan(image_path, &image_st, ctx->app_db->titles,
                                 ctx->app_db->titles_ready)) {
    return true;
  }
  bool image_unstable = false;
  if (!maybe_mount_image_file(image_path, image_name, &image_unstable)) {
    if (image_unstable && ctx->unstable_found_out)
      *ctx->unstable_found_out = true;
    return true;
  }
  if (persistent_image)
    sm_image_index_begin_scan(image_path, &image_st);

  char mount_point[MAX_PATH];
  get_image_mount_point_for_source(image_path, mount_point);
  collect_scan_candidates_from_manual_root(
      mount_point, ctx->manual_source_path, ctx->candidates,
      ctx->max_candidates, ctx->candidate_count, ctx->app_db,
      ctx->discovered_param_roots, ctx->discovered_param_root_count,
      &image_unstable);
  if (image_unstable && ctx->unstable_found_out)
    *ctx->unstable_found_out = true;
  if (persistent_image && !image_unstable && !should_stop_requested() &&
      !runtime_sleep_mode_active()) {
    sm_image_index_complete_scan(image_path);
  }
  return true;
}

static void collect_scan_candidates_from_manual_root(
    const char *scan_path, const char *manual_source_path,
    scan_candidate_t *candidates, int max_candidates, int *candidate_count,
    const scan_app_db_context_t *app_db,
    char discovered_param_roots[][MAX_PATH], int *discovered_param_root_count,
    bool *unstable_found_out) {
  if (should_stop_requested() || runtime_sleep_mode_active())
    return;

  bool pfsc_container_root = is_pfsc_image_mount_base_or_child(scan_path);
  bool try_root_candidate = !pfsc_container_root;
  if (try_root_candidate && is_under_image_mount_base(scan_path)) {
    struct stat param_st;
    try_root_candidate = directory_has_param_json(scan_path, &param_st);
  }

  if (try_root_candidate) {
    if (try_collect_candidate_for_directory(
            scan_path, candidates, max_candidates, candidate_count,
            app_db, discovered_param_roots, discovered_param_root_count, true,
            manual_source_path, unstable_found_out)) {
      return;
    }
  }

  unsigned int scan_depth = get_scan_depth_for_root(scan_path);

  collect_candidates_walk_ctx_t ctx = {
      .candidates = candidates,
      .max_candidates = max_candidates,
      .candidate_count = candidate_count,
      .app_db = app_db,
      .discovered_param_roots = discovered_param_roots,
      .discovered_param_root_count = discovered_param_root_count,
      .manual_source_path = manual_source_path,
      .unstable_found_out = unstable_found_out,
  };
  sm_scan_tree_callbacks_t callbacks = {
      .on_directory = collect_candidate_directory_visit,
      .on_image_file = pfsc_container_root ? collect_candidate_image_visit
                                           : NULL,
  };
  (void)sm_scan_tree_walk(scan_path, scan_path, 0u, scan_depth, &callbacks, &ctx);
}

static void collect_scan_candidates_from_manual_path(
    const char *manual_path, scan_candidate_t *candidates, int max_candidates,
    int *candidate_count, const scan_app_db_context_t *app_db,
    char discovered_param_roots[][MAX_PATH],
    int *discovered_param_root_count, bool *unstable_found_out) {
  if (should_stop_requested() || runtime_sleep_mode_active())
    return;
  if (defer_full_candidate_batch(*candidate_count, max_candidates,
                                 unstable_found_out)) {
    return;
  }

  struct stat st;
  if (stat(manual_path, &st) != 0) {
    if (note_manual_missing_source_once(manual_path))
      log_debug("  [MANUAL] source unavailable: %s", manual_path);
    return;
  }
  clear_manual_missing_source(manual_path);

  const char *name = get_filename_component(manual_path);
  if (S_ISREG(st.st_mode) &&
      is_supported_image_file_path(manual_path, name)) {
    if (!sm_image_index_needs_scan(manual_path, &st, app_db->titles,
                                   app_db->titles_ready))
      return;
    bool image_unstable = false;
    if (!maybe_mount_image_file(manual_path, name, &image_unstable)) {
      if (image_unstable && unstable_found_out)
        *unstable_found_out = true;
      return;
    }
    sm_image_index_begin_scan(manual_path, &st);

    char mount_point[MAX_PATH];
    get_image_mount_point_for_source(manual_path, mount_point);
    collect_scan_candidates_from_manual_root(
        mount_point, manual_path, candidates, max_candidates, candidate_count,
        app_db, discovered_param_roots, discovered_param_root_count,
        &image_unstable);
    if (image_unstable && unstable_found_out)
      *unstable_found_out = true;
    if (!image_unstable && !should_stop_requested() &&
        !runtime_sleep_mode_active()) {
      sm_image_index_complete_scan(manual_path);
    }
    return;
  }

  struct stat param_st;
  if (!S_ISDIR(st.st_mode) || !directory_has_param_json(manual_path, &param_st)) {
    log_debug("  [MANUAL] expected game directory or image file: %s",
              manual_path);
    return;
  }

  collect_scan_candidates_from_manual_root(
      manual_path, manual_path, candidates, max_candidates, candidate_count,
      app_db, discovered_param_roots, discovered_param_root_count,
      unstable_found_out);
}

typedef struct {
  scan_candidate_t *candidates;
  int max_candidates;
  int *candidate_count;
  const scan_app_db_context_t *app_db;
  char (*discovered_param_roots)[MAX_PATH];
  int *discovered_param_root_count;
  bool *unstable_found_out;
  int path_count;
} manual_scan_ctx_t;

static bool collect_manual_path_visit(const char *manual_path, void *ctx_ptr) {
  manual_scan_ctx_t *ctx = (manual_scan_ctx_t *)ctx_ptr;
  if (should_stop_requested() || runtime_sleep_mode_active())
    return false;
  if (*ctx->candidate_count >= ctx->max_candidates) {
    log_debug("  [MANUAL] candidate queue full (%d), remaining sources deferred",
              ctx->max_candidates);
    return false;
  }

  ctx->path_count++;
  collect_scan_candidates_from_manual_path(
      manual_path, ctx->candidates, ctx->max_candidates, ctx->candidate_count,
      ctx->app_db, ctx->discovered_param_roots,
      ctx->discovered_param_root_count, ctx->unstable_found_out);
  return true;
}

static bool build_backport_path_for_scan_root(const char *title_id,
                                              const char *scan_path,
                                              char backport_path[MAX_PATH]) {
  if (!title_id || title_id[0] == '\0' || !scan_path || scan_path[0] == '\0')
    return false;

  char backport_root[MAX_PATH];
  if (!build_backports_root_path(scan_path, backport_root))
    return false;

  int written = snprintf(backport_path, MAX_PATH, "%s/%s", backport_root,
                         title_id);
  return written > 0 && (size_t)written < MAX_PATH;
}

static bool backport_path_is_dir(const char *backport_path) {
  struct stat st;
  return backport_path && stat(backport_path, &st) == 0 &&
         S_ISDIR(st.st_mode);
}

static bool resolve_backport_path(const char *title_id,
                                  const char *owning_scan_path,
                                  char backport_path[MAX_PATH]) {
  if (!title_id || title_id[0] == '\0')
    return false;

  backport_path[0] = '\0';
  if (build_backport_path_for_scan_root(title_id, owning_scan_path,
                                        backport_path) &&
      backport_path_is_dir(backport_path)) {
    return true;
  }

  for (int i = 0; i < get_scan_path_count(); i++) {
    const char *scan_path = get_scan_path(i);
    if (owning_scan_path && strcmp(scan_path, owning_scan_path) == 0)
      continue;

    char candidate_path[MAX_PATH];
    if (!build_backport_path_for_scan_root(title_id, scan_path, candidate_path))
      continue;
    if (!backport_path_is_dir(candidate_path))
      continue;

    (void)strlcpy(backport_path, candidate_path, MAX_PATH);
    return true;
  }

  backport_path[0] = '\0';
  return false;
}

bool mount_backport_overlay_for_title(const char *source_path,
                                      const char *title_id,
                                      const char *owning_scan_root,
                                      bool *unstable_found_out) {
  if (should_stop_requested() || runtime_sleep_mode_active())
    return false;

  char backport_path[MAX_PATH];
  if (!resolve_backport_path(title_id, owning_scan_root, backport_path)) {
    return true;
  }
  if (is_backport_mount_blocked(backport_path))
    return false;

  bool overlay_active = false;
  if (!reconcile_title_backport_mount(title_id, source_path, backport_path,
                                      &overlay_active)) {
    return false;
  }
  if (overlay_active) {
    return true;
  }
  if (!wait_for_stability_fast(backport_path, "BKP")) {
    if (unstable_found_out)
      *unstable_found_out = true;
    return false;
  }
  overlay_active = false;
  if (!reconcile_title_backport_mount(title_id, source_path, backport_path,
                                      &overlay_active)) {
    return false;
  }
  if (overlay_active) {
    return true;
  }

  char system_ex_path[MAX_PATH];
  snprintf(system_ex_path, sizeof(system_ex_path), "/system_ex/app/%s",
           title_id);
  if (!mount_backport_overlay(system_ex_path, backport_path, title_id)) {
    note_backport_mount_failure(backport_path);
    return false;
  }
  clear_backport_mount_failure(backport_path);
  return true;
}

bool release_scan_runtime_mounts(void) {
  if (runtime_sleep_mode_active())
    return false;
  if (sm_game_lifecycle_has_active_game() ||
      sm_shellcore_service_has_prepared_mount()) {
    return true;
  }
  runtime_mount_state_lock();
  if (sm_game_lifecycle_has_active_game() ||
      sm_shellcore_service_has_prepared_mount()) {
    runtime_mount_state_unlock();
    return true;
  }
  unmount_all_title_runtime_layers();
  bool released = release_runtime_image_mounts();
  runtime_mount_state_unlock();
  return released;
}

// --- Unified Scan Pass (images + game candidates) ---
void cleanup_lost_sources_before_scan(void) {
  // 1) Drop stale game cache entries for deleted sources.
  prune_game_cache();
  // 2) Drop stale/broken mount links and unmount stale /system_ex stacks.
  cleanup_mount_links(NULL, true);
  // 3) Unmount stale image mounts for deleted image files.
  cleanup_stale_image_mounts();
  sm_image_index_prune();
  // 4) Drop stale path-state entries.
  prune_path_state();
}

void cleanup_lost_sources_for_scan_root(const char *scan_root) {
  prune_game_cache_for_root(scan_root);
  cleanup_mount_links(scan_root, true);
  cleanup_stale_image_mounts_for_root(scan_root);
  sm_image_index_prune();
  prune_path_state_for_root(scan_root);
}

void unmount_usb_sources_for_suspend(void) {
  log_debug("[SLEEP] USB-backed mount cleanup begin");
  cleanup_usb_mount_links_for_suspend();
  if (!unmount_usb_image_mounts_for_suspend()) {
    log_debug("[SLEEP] some USB-backed image mounts remain busy");
  }
  cleanup_mount_dirs();
  log_debug("[SLEEP] USB-backed mount cleanup done");
}

static void collect_scan_candidates_from_root(
    const char *scan_path, scan_candidate_t *candidates, int max_candidates,
    int *candidate_count, const scan_app_db_context_t *app_db,
    char discovered_param_roots[][MAX_PATH],
    int *discovered_param_root_count, bool *unstable_found_out) {
  if (should_stop_requested() || runtime_sleep_mode_active())
    return;

  unsigned int scan_depth = get_scan_depth_for_root(scan_path);

  collect_candidates_walk_ctx_t ctx = {
      .candidates = candidates,
      .max_candidates = max_candidates,
      .candidate_count = candidate_count,
      .app_db = app_db,
      .discovered_param_roots = discovered_param_roots,
      .discovered_param_root_count = discovered_param_root_count,
      .manual_source_path = NULL,
      .unstable_found_out = unstable_found_out,
  };
  sm_scan_tree_callbacks_t callbacks = {
      .on_directory = collect_candidate_directory_visit,
      .on_image_file = collect_candidate_image_visit,
  };
  (void)sm_scan_tree_walk(scan_path, scan_path, 0u, scan_depth, &callbacks, &ctx);
}

static void collect_scan_candidates_from_manual_list(
    scan_candidate_t *candidates, int max_candidates, int *candidate_count,
    const scan_app_db_context_t *app_db,
    char discovered_param_roots[][MAX_PATH], int *discovered_param_root_count,
    bool *unstable_found_out) {
  if (should_stop_requested() || runtime_sleep_mode_active())
    return;

  (void)sm_manual_reconcile_deleted_titles(app_db->titles,
                                           app_db->titles_ready);

  manual_scan_ctx_t ctx = {
      .candidates = candidates,
      .max_candidates = max_candidates,
      .candidate_count = candidate_count,
      .app_db = app_db,
      .discovered_param_roots = discovered_param_roots,
      .discovered_param_root_count = discovered_param_root_count,
      .unstable_found_out = unstable_found_out,
      .path_count = 0,
  };
  if (!sm_manual_for_each_path(collect_manual_path_visit, &ctx) ||
      ctx.path_count <= 0) {
    return;
  }

  log_debug("  [MANUAL] scanned %d manual source(s)", ctx.path_count);
}

static void uninstall_blocked_ppsa_titles(
    const struct AppDbTitleList *blocked_ppsa_titles,
    bool blocked_ppsa_titles_ready) {
  if (!blocked_ppsa_titles_ready || !blocked_ppsa_titles)
    return;

  int requested = 0;
  for (int i = 0; i < blocked_ppsa_titles->count &&
                  !should_stop_requested() && !runtime_sleep_mode_active();
       i++) {
    const char *title_id = blocked_ppsa_titles->ids[i];
    if (request_blocked_ppsa_uninstall(title_id, NULL))
      requested++;
  }

  if (requested > 0)
    log_debug("  [REG] requested %d blocked PPSA uninstall(s)", requested);
}

int collect_scan_candidates_for_scan_root(const char *scan_root,
                                          scan_candidate_t *candidates,
                                          int max_candidates,
                                          int *total_found_out,
                                          bool *unstable_found_out) {
  reset_scan_workspace();
  int candidate_count = 0;
  struct AppDbTitleList app_db_titles = {0};
  struct AppDbTitleList blocked_ppsa_titles = {0};
  bool app_db_titles_ready = get_app_db_title_list_cached(&app_db_titles);
  bool blocked_ppsa_titles_ready =
      get_app_db_blocked_uninstall_ppsa_list(&blocked_ppsa_titles);
  int discovered_param_root_count = 0;

  if (!app_db_titles_ready)
    log_debug("  [DB] app.db title list unavailable for this scan cycle");
  if (!blocked_ppsa_titles_ready)
    log_debug("  [DB] blocked PPSA uninstall list unavailable for this scan cycle");

  scan_app_db_context_t app_db = {
      .titles = &app_db_titles,
      .blocked_ppsa_titles = &blocked_ppsa_titles,
      .titles_ready = app_db_titles_ready,
      .blocked_ppsa_titles_ready = blocked_ppsa_titles_ready,
  };
  collect_scan_candidates_from_root(scan_root, candidates, max_candidates,
                                    &candidate_count, &app_db,
                                    g_scan_workspace.discovered_param_roots,
                                    &discovered_param_root_count,
                                    unstable_found_out);

  if (total_found_out)
    *total_found_out = discovered_param_root_count;
  sm_image_index_flush();
  free_app_db_title_list(&blocked_ppsa_titles);
  free_app_db_title_list(&app_db_titles);
  return candidate_count;
}

int collect_scan_candidates(scan_candidate_t *candidates, int max_candidates,
                            int *total_found_out,
                            bool *unstable_found_out) {
  reset_scan_workspace();
  int candidate_count = 0;
  struct AppDbTitleList app_db_titles = {0};
  struct AppDbTitleList blocked_ppsa_titles = {0};
  bool app_db_titles_ready = get_app_db_title_list_cached(&app_db_titles);
  bool blocked_ppsa_titles_ready =
      get_app_db_blocked_uninstall_ppsa_list(&blocked_ppsa_titles);
  int discovered_param_root_count = 0;

  if (!app_db_titles_ready)
    log_debug("  [DB] app.db title list unavailable for this scan cycle");
  if (!blocked_ppsa_titles_ready)
    log_debug("  [DB] blocked PPSA uninstall list unavailable for this scan cycle");

  scan_app_db_context_t app_db = {
      .titles = &app_db_titles,
      .blocked_ppsa_titles = &blocked_ppsa_titles,
      .titles_ready = app_db_titles_ready,
      .blocked_ppsa_titles_ready = blocked_ppsa_titles_ready,
  };
  for (int i = 0; i < get_scan_path_count(); i++) {
    if (should_stop_requested() || runtime_sleep_mode_active())
      break;
    collect_scan_candidates_from_root(get_scan_path(i), candidates,
                                      max_candidates,
                                      &candidate_count, &app_db,
                                      g_scan_workspace.discovered_param_roots,
                                      &discovered_param_root_count,
                                      unstable_found_out);
  }

  collect_scan_candidates_from_manual_list(
      candidates, max_candidates, &candidate_count, &app_db,
      g_scan_workspace.discovered_param_roots, &discovered_param_root_count,
      unstable_found_out);

  uninstall_blocked_ppsa_titles(&blocked_ppsa_titles,
                                blocked_ppsa_titles_ready);

  if (total_found_out)
    *total_found_out = discovered_param_root_count;
  sm_image_index_flush();
  free_app_db_title_list(&blocked_ppsa_titles);
  free_app_db_title_list(&app_db_titles);
  return candidate_count;
}
