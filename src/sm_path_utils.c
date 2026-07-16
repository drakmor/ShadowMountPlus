#include "sm_platform.h"
#include "sm_path_utils.h"
#include "sm_paths.h"

const char *get_filename_component(const char *path) {
  const char *base = strrchr(path, '/');
  if (!base)
    base = strrchr(path, '\\');
  return base ? base + 1 : path;
}

bool path_exists(const char *path) {
  struct stat st;
  return (path && path[0] != '\0' && stat(path, &st) == 0);
}

bool is_under_image_mount_base(const char *path) {
  size_t image_prefix_len = strlen(IMAGE_MOUNT_BASE);
  return (strncmp(path, IMAGE_MOUNT_BASE, image_prefix_len) == 0 &&
          path[image_prefix_len] == '/');
}

bool is_pfsc_image_mount_base_or_child(const char *path) {
  return path_matches_root_or_child(path, PFSC_IMAGE_MOUNT_BASE);
}

bool path_matches_root_or_child(const char *path, const char *root) {
  if (!path || !root || root[0] == '\0')
    return false;
  size_t root_len = strlen(root);
  if (strncmp(path, root, root_len) != 0)
    return false;
  if (root_len == 1u && root[0] == '/')
    return path[0] == '/';
  return path[root_len] == '\0' || path[root_len] == '/';
}

static const char *find_usb_storage_root(const char *path) {
  static const char *usb_roots[] = {
      "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4",
      "/mnt/usb5", "/mnt/usb6", "/mnt/usb7", "/mnt/ext0",
  };

  if (!path || path[0] == '\0')
    return NULL;

  for (size_t i = 0; i < sizeof(usb_roots) / sizeof(usb_roots[0]); i++) {
    if (path_matches_root_or_child(path, usb_roots[i]))
      return usb_roots[i];
  }
  return NULL;
}

bool is_usb_storage_path(const char *path) {
  return find_usb_storage_root(path) != NULL;
}

bool usb_storage_root_mounted(const char *path) {
  const char *usb_root = find_usb_storage_root(path);
  if (!usb_root)
    return false;

  struct stat root_st;
  struct stat parent_st;
  if (stat(usb_root, &root_st) != 0 || stat("/mnt", &parent_st) != 0)
    return false;

  // Empty /mnt/usbN directories belong to /mnt. A mounted USB filesystem has
  // a distinct device identity even when the directory itself always exists.
  return root_st.st_dev != parent_st.st_dev;
}

bool build_backports_root_path(const char *scan_path, char out[MAX_PATH]) {
  if (is_under_image_mount_base(scan_path))
    return false;

  int written = snprintf(out, MAX_PATH, "%s/%s", scan_path,
                         DEFAULT_BACKPORTS_DIR_NAME);
  return written >= 0 && (size_t)written < MAX_PATH;
}
