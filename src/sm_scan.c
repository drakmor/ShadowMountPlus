#include "sm_platform.h"
#include "sm_runtime.h"
#include "sm_scan.h"
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

static bool is_under_discovered_param_root(
    const char *path, char discovered_param_roots[][MAX_PATH],
    int discovered_count) {
  if (!path)
    return false;
  for (int i = 0; i < discovered_count; i++) {
    const char *root = discovered_param_roots[i];
    if (root[0] == '\0')
      continue;
    size_t root_len = strlen(root);
    if (root_len == 0)
      continue;
    if (strncmp(path, root, root_len) != 0)
      continue;
    if (path[root_len] == '\0' || path[root_len] == '/')
      return true;
  }
  return false;
}

static void classify_scan_entry(const char *full_path, unsigned char d_type,
                                bool *is_dir_out, bool *is_regular_out) {
  bool is_dir = false;
  bool is_regular = false;

  if (d_type == DT_DIR) {
    is_dir = true;
  } else if (d_type == DT_REG) {
    is_regular = true;
  } else if (d_type == DT_UNKNOWN) {
    struct stat st;
    if (lstat(full_path, &st) == 0) {
      is_dir = S_ISDIR(st.st_mode);
      is_regular = S_ISREG(st.st_mode);
    }
  }

  if (is_dir_out)
    *is_dir_out = is_dir;
  if (is_regular_out)
    *is_regular_out = is_regular;
}

static bool is_distinct_configured_scan_root(const char *current_scan_root,
                                             const char *path) {
  if (!path || path[0] == '\0')
    return false;

  for (int i = 0; i < get_scan_path_count(); i++) {
    const char *scan_path = get_scan_path(i);
    if (!scan_path || strcmp(scan_path, path) != 0)
      continue;
    if (current_scan_root && strcmp(current_scan_root, path) == 0)
      return false;
    return true;
  }

  return false;
}

// --- Candidate Discovery ---
static bool try_collect_candidate_for_directory(
    const char *full_path, scan_candidate_t *candidates, int max_candidates,
    int *candidate_count, const struct AppDbTitleList *app_db_titles,
    bool app_db_titles_ready, char discovered_param_roots[][MAX_PATH],
    int *discovered_param_root_count) {
  struct stat param_st;
  memset(&param_st, 0, sizeof(param_st));
  bool has_param_json = false;
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  title_id[0] = '\0';
  title_name[0] = '\0';

  if (is_under_discovered_param_root(full_path, discovered_param_roots,
                                     *discovered_param_root_count)) {
    return true;
  }

  if (is_under_image_mount_base(full_path) && !is_active_image_mount_point(full_path)) {
    log_debug("  [SKIP] inactive mount path: %s", full_path);
    return true;
  }

  has_param_json = directory_has_param_json(full_path, &param_st);

  if (!has_param_json) {
    if (is_missing_param_scan_limited(full_path)) {
      log_debug("  [SKIP] param.json retry limit reached: %s", full_path);
    } else {
      record_missing_param_failure(full_path);
    }
    return false;
  }

  if (!get_game_info(full_path, &param_st, title_id, title_name)) {
    record_missing_param_failure(full_path);
    log_debug("  [SKIP] game info unavailable: %s", full_path);
    return true;
  }

  if (!is_under_discovered_param_root(full_path, discovered_param_roots,
                                      *discovered_param_root_count) &&
      *discovered_param_root_count < MAX_PENDING) {
    (void)strlcpy(discovered_param_roots[*discovered_param_root_count], full_path,
                  MAX_PATH);
    (*discovered_param_root_count)++;
  }
  clear_missing_param_entry(full_path);

  for (int i = 0; i < *candidate_count; i++) {
    if (strcmp(candidates[i].title_id, title_id) == 0) {
      notify_duplicate_title_once(title_id, full_path, candidates[i].path);
      return true;
    }
  }

  if (!app_db_titles_ready) {
    return true;
  }
  bool in_app_db = app_db_title_list_contains(app_db_titles, title_id);
  bool installed = in_app_db && is_installed(title_id);
  char tracked_path[MAX_PATH];
  bool link_matches_source =
      installed && read_mount_link(title_id, tracked_path, sizeof(tracked_path)) &&
      strcmp(tracked_path, full_path) == 0;
  bool mounted = false;
  if (link_matches_source) {
    mounted = is_data_mounted(title_id);
    if (!mounted && mount_title_nullfs(title_id, full_path))
      mounted = is_data_mounted(title_id);
  }

  if (in_app_db) {
    const char *cached_path = NULL;
    if (find_cached_game(full_path, title_id, &cached_path)) {
      if (cached_path && strcmp(cached_path, full_path) != 0)
        notify_duplicate_title_once(title_id, full_path, cached_path);
      return true;
    }
  }

  if (!in_app_db && was_register_attempted(title_id)) {
    uint8_t register_attempts = get_register_attempts(title_id);
    log_debug("  [SKIP] register/install retry limit reached (%u/%u): %s (%s)",
              (unsigned)register_attempts, (unsigned)MAX_REGISTER_ATTEMPTS,
              title_name, title_id);
    return true;
  }

  // Installed status requires both app files and app.db presence.
  if (link_matches_source) {
    return true;
  }

  uint8_t failed_attempts = get_failed_mount_attempts(title_id);
  if (failed_attempts >= MAX_FAILED_MOUNT_ATTEMPTS) {
    log_debug("  [SKIP] mount/register retry limit reached (%u/%u): %s (%s)",
              (unsigned)failed_attempts, (unsigned)MAX_FAILED_MOUNT_ATTEMPTS,
              title_name, title_id);
    return true;
  }

  if (!wait_for_stability_fast(full_path, title_name)) {
    log_debug("  [SKIP] source not stable yet: %s (%s)", title_name, full_path);
    return true;
  }

  if (*candidate_count >= max_candidates) {
    log_debug("  [SKIP] candidate queue full (%d): %s (%s)", max_candidates,
              title_name, title_id);
    return true;
  }

  (void)strlcpy(candidates[*candidate_count].path, full_path,
                sizeof(candidates[*candidate_count].path));
  (void)strlcpy(candidates[*candidate_count].title_id, title_id,
                sizeof(candidates[*candidate_count].title_id));
  (void)strlcpy(candidates[*candidate_count].title_name, title_name,
                sizeof(candidates[*candidate_count].title_name));
  candidates[*candidate_count].installed = installed;
  candidates[*candidate_count].in_app_db = in_app_db;
  (*candidate_count)++;
  return true;
}

static void collect_candidates_with_depth(
    const char *current_scan_root, const char *dir_path,
    unsigned int remaining_depth, scan_candidate_t *candidates,
    int max_candidates, int *candidate_count,
    const struct AppDbTitleList *app_db_titles, bool app_db_titles_ready,
    char discovered_param_roots[][MAX_PATH],
    int *discovered_param_root_count) {
  if (should_stop_requested() || !dir_path || dir_path[0] == '\0')
    return;

  if (is_distinct_configured_scan_root(current_scan_root, dir_path))
    return;

  // Once a directory has sce_sys/param.json (valid or not),
  // it is treated as a terminal game root and descendants are skipped.
  if (try_collect_candidate_for_directory(
          dir_path, candidates, max_candidates, candidate_count, app_db_titles,
          app_db_titles_ready, discovered_param_roots,
          discovered_param_root_count)) {
    return;
  }

  if (remaining_depth == 0)
    return;

  DIR *d = opendir(dir_path);
  if (!d)
    return;

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (should_stop_requested())
      break;
    if (entry->d_name[0] == '.')
      continue;

    char full_path[MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

    bool is_dir = false;
    classify_scan_entry(full_path, entry->d_type, &is_dir, NULL);

    if (!is_dir)
      continue;

    collect_candidates_with_depth(
        current_scan_root, full_path, remaining_depth - 1u, candidates,
        max_candidates, candidate_count, app_db_titles, app_db_titles_ready,
        discovered_param_roots, discovered_param_root_count);
  }

  closedir(d);
}

static bool resolve_backport_mount_context(const char *title_id,
                                           const char *source_path,
                                           int scan_path_count,
                                           char backport_path[MAX_PATH],
                                           char system_ex_path[MAX_PATH]) {
  char backport_source_path[MAX_PATH];
  if (is_under_image_mount_base(source_path)) {
    if (!resolve_image_source_from_mount_cache(source_path,
                                               backport_source_path,
                                               sizeof(backport_source_path))) {
      return false;
    }
  } else {
    (void)strlcpy(backport_source_path, source_path,
                  sizeof(backport_source_path));
  }

  const char *owning_scan_path = NULL;
  size_t owning_scan_path_len = 0;
  for (int i = 0; i < scan_path_count; i++) {
    const char *scan_path = get_scan_path(i);
    if (!path_matches_root_or_child(backport_source_path, scan_path))
      continue;

    size_t scan_path_len = strlen(scan_path);
    if (owning_scan_path && scan_path_len <= owning_scan_path_len)
      continue;

    owning_scan_path = scan_path;
    owning_scan_path_len = scan_path_len;
  }
  if (!owning_scan_path)
    return false;

  char backport_root[MAX_PATH];
  if (!build_backports_root_path(owning_scan_path, backport_root))
    return false;

  snprintf(backport_path, MAX_PATH, "%s/%s", backport_root, title_id);
  snprintf(system_ex_path, MAX_PATH, "/system_ex/app/%s", title_id);
  return true;
}

void mount_backport_overlays(void) {
  int scan_path_count = get_scan_path_count();
  if (scan_path_count == 0)
    return;

  DIR *d = opendir("/user/app");
  if (!d) {
    if (errno != ENOENT)
      log_debug("  [BKP] open /user/app failed: %s", strerror(errno));
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (should_stop_requested())
      break;
    if (!resolve_title_app_dir(entry, NULL, 0))
      continue;

    char source_path[MAX_PATH];
    if (!read_mount_link(entry->d_name, source_path, sizeof(source_path)))
      continue;

    char backport_path[MAX_PATH];
    char system_ex_path[MAX_PATH];
    if (!resolve_backport_mount_context(entry->d_name, source_path,
                                        scan_path_count, backport_path,
                                        system_ex_path)) {
      continue;
    }

    bool overlay_active = false;
    if (!reconcile_title_backport_mount(entry->d_name, source_path,
                                        backport_path, &overlay_active) ||
        overlay_active) {
      continue;
    }
    if (!wait_for_stability_fast(backport_path, "BKP"))
      continue;
    overlay_active = false;
    if (!reconcile_title_backport_mount(entry->d_name, source_path,
                                        backport_path, &overlay_active) ||
        overlay_active) {
      continue;
    }

    mount_backport_overlay(system_ex_path, backport_path, entry->d_name);
  }

  closedir(d);
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

int collect_scan_candidates(scan_candidate_t *candidates,
                            int max_candidates,
                            int *total_found_out) {
  int candidate_count = 0;
  unsigned int scan_depth = runtime_config()->scan_depth;
  const struct AppDbTitleList *app_db_titles = NULL;
  bool app_db_titles_ready = get_app_db_title_list_cached(&app_db_titles);
  char discovered_param_roots[MAX_PENDING][MAX_PATH];
  int discovered_param_root_count = 0;
  memset(discovered_param_roots, 0, sizeof(discovered_param_roots));

  if (scan_depth < MIN_SCAN_DEPTH)
    scan_depth = MIN_SCAN_DEPTH;

  if (!app_db_titles_ready) {
    log_debug("  [DB] app.db title list unavailable for this scan cycle");
  }

  for (int i = 0; i < get_scan_path_count(); i++) {
    const char *scan_path = get_scan_path(i);
    if (should_stop_requested())
      goto done;

    bool has_backports_root = !is_under_image_mount_base(scan_path);

    DIR *d = opendir(scan_path);
    if (!d)
      continue;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
      if (should_stop_requested()) {
        closedir(d);
        goto done;
      }
      if (entry->d_name[0] == '.')
        continue;
      if (has_backports_root &&
          strcmp(entry->d_name, DEFAULT_BACKPORTS_DIR_NAME) == 0) {
        continue;
      }

      char full_path[MAX_PATH];
      snprintf(full_path, sizeof(full_path), "%s/%s", scan_path, entry->d_name);

      bool is_dir = false;
      bool is_regular = false;
      classify_scan_entry(full_path, entry->d_type, &is_dir, &is_regular);

      if (!path_matches_root_or_child(scan_path, IMAGE_MOUNT_BASE) && is_regular)
        maybe_mount_image_file(full_path, entry->d_name);
      if (!is_dir)
        continue;

      collect_candidates_with_depth(
          scan_path, full_path, scan_depth - 1u, candidates, max_candidates,
          &candidate_count, app_db_titles, app_db_titles_ready,
          discovered_param_roots, &discovered_param_root_count);
    }
    closedir(d);
  }
done:
  if (total_found_out)
    *total_found_out = discovered_param_root_count;
  return candidate_count;
}
