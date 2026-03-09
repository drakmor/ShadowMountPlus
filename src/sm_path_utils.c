#include "sm_platform.h"
#include "sm_path_utils.h"
#include "sm_paths.h"

const char *get_filename_component(const char *path) {
  if (!path)
    return "";
  const char *base = strrchr(path, '/');
  if (!base)
    base = strrchr(path, '\\');
  return base ? base + 1 : path;
}

bool is_under_image_mount_base(const char *path) {
  if (!path)
    return false;
  size_t image_prefix_len = strlen(IMAGE_MOUNT_BASE);
  return (strncmp(path, IMAGE_MOUNT_BASE, image_prefix_len) == 0 &&
          path[image_prefix_len] == '/');
}

bool build_backports_root_path(const char *scan_path, char out[MAX_PATH]) {
  if (!scan_path || scan_path[0] == '\0')
    return false;
  if (is_under_image_mount_base(scan_path))
    return false;

  snprintf(out, MAX_PATH, "%s/%s", scan_path, DEFAULT_BACKPORTS_DIR_NAME);
  return true;
}
