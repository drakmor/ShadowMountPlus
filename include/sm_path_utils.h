#ifndef SM_PATH_UTILS_H
#define SM_PATH_UTILS_H

#include <stdbool.h>
#include <stddef.h>

#include "sm_types.h"

// Return the last path component without modifying the input string.
const char *get_filename_component(const char *path);
// Return true when a path lives under the image mount root.
bool is_under_image_mount_base(const char *path);
// Build "<scan_path>/backports" for a managed scan root.
bool build_backports_root_path(const char *scan_path, char out[MAX_PATH]);

#endif
