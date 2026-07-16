#ifndef SM_PATH_UTILS_H
#define SM_PATH_UTILS_H

#include <stdbool.h>
#include <stddef.h>

#include "sm_types.h"

// Return the last path component without modifying the input string.
const char *get_filename_component(const char *path);
// Return true when stat() can see the path.
bool path_exists(const char *path);
// Return true when a path lives under the image mount root.
bool is_under_image_mount_base(const char *path);
// Return true when a path is the PFSC container mount root or lives under it.
bool is_pfsc_image_mount_base_or_child(const char *path);
// Return true when path is equal to root or is under it.
bool path_matches_root_or_child(const char *path, const char *root);
// Return true when a path lives on USB-backed storage.
bool is_usb_storage_path(const char *path);
// Return true when the path's USB root is mounted, not just an empty mount
// point under /mnt.
bool usb_storage_root_mounted(const char *path);
// Build "<scan_path>/backports" for a managed scan root.
bool build_backports_root_path(const char *scan_path, char out[MAX_PATH]);

#endif
