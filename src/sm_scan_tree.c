#include "sm_platform.h"
#include "sm_scan_tree.h"

#include "sm_config_mount.h"
#include "sm_filesystem.h"
#include "sm_image.h"
#include "sm_path_utils.h"
#include "sm_paths.h"
#include "sm_runtime.h"

static void classify_scan_tree_entry(const char *full_path, unsigned char d_type,
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

  *is_dir_out = is_dir;
  *is_regular_out = is_regular;
}

static bool is_distinct_configured_scan_root(const char *current_scan_root,
                                             const char *path) {
  for (int i = 0; i < get_scan_path_count(); i++) {
    const char *scan_path = get_scan_path(i);
    if (strcmp(scan_path, path) != 0)
      continue;
    if (strcmp(current_scan_root, path) == 0)
      return false;
    return true;
  }

  return false;
}

bool sm_scan_tree_walk(const char *scan_root, const char *dir_path,
                       unsigned int depth_from_root,
                       unsigned int remaining_depth,
                       const sm_scan_tree_callbacks_t *callbacks, void *ctx) {
  if (should_stop_requested())
    return true;

  if (depth_from_root > 0u &&
      is_distinct_configured_scan_root(scan_root, dir_path)) {
    return true;
  }

  sm_scan_tree_dir_visit_t dir_visit = SM_SCAN_TREE_DIR_DESCEND;
  if (callbacks->on_directory) {
    dir_visit = callbacks->on_directory(dir_path, depth_from_root, ctx);
    if (dir_visit == SM_SCAN_TREE_DIR_ABORT)
      return false;
    if (dir_visit == SM_SCAN_TREE_DIR_SKIP_DESCEND)
      return true;
  }

  if (remaining_depth == 0u)
    return true;

  DIR *d = opendir(dir_path);
  if (!d)
    return true;

  bool skip_backports_root =
      (depth_from_root == 0u && !is_under_image_mount_base(scan_root));
  bool allow_image_file_visits =
      callbacks->on_image_file &&
      !path_matches_root_or_child(scan_root, IMAGE_MOUNT_BASE);

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (should_stop_requested()) {
      closedir(d);
      return true;
    }
    if (entry->d_name[0] == '.')
      continue;
    if (skip_backports_root &&
        strcmp(entry->d_name, DEFAULT_BACKPORTS_DIR_NAME) == 0) {
      continue;
    }

    char full_path[MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

    bool is_dir = false;
    bool is_regular = false;
    classify_scan_tree_entry(full_path, entry->d_type, &is_dir, &is_regular);

    if (allow_image_file_visits && is_regular &&
        is_supported_image_file_name(entry->d_name)) {
      if (!callbacks->on_image_file(full_path, entry->d_name, depth_from_root + 1u,
                                    ctx)) {
        closedir(d);
        return false;
      }
    }

    if (!is_dir)
      continue;

    if (!sm_scan_tree_walk(scan_root, full_path, depth_from_root + 1u,
                           remaining_depth - 1u, callbacks, ctx)) {
      closedir(d);
      return false;
    }
  }

  closedir(d);
  return true;
}
