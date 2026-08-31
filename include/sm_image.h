#ifndef SM_IMAGE_H
#define SM_IMAGE_H

#include <stdbool.h>

#include "sm_types.h"

typedef struct unmount_result {
  bool filesystem_released;
  bool device_released;
  bool directory_removed;
  int error;
} unmount_result_t;

// Log filesystem statistics for a mounted path.
void log_fs_stats(const char *tag, const char *path, const char *type_hint);
// Attach and mount an image file to its runtime mount point.
bool mount_image(const char *file_path, image_fs_type_t fs_type);
// Attach and mount with a request-scoped mode override. A NULL override uses
// the configured per-image/global mode.
bool mount_image_with_mode(const char *file_path, image_fs_type_t fs_type,
                           const bool *mount_read_only_override);
// Unmount an image mount point and detach its backing device.
unmount_result_t unmount_image(const char *file_path, int unit_id,
                               attach_backend_t backend);
// Unmount a runtime image without deleting persistent title metadata links.
unmount_result_t unmount_runtime_image(const char *file_path, int unit_id,
                                       attach_backend_t backend);
// Release one cached runtime image mount. An absent live mount is harmless.
bool release_runtime_image_mount(const char *file_path);
// Release all discovery/runtime image mounts without deleting title metadata.
bool release_runtime_image_mounts(void);
// Reconcile cached image mounts with current sources and remount if needed.
void cleanup_stale_image_mounts(void);
// Reconcile cached image mounts that belong to a specific scan root.
void cleanup_stale_image_mounts_for_root(const char *root);
// Unmount cached image mounts backed by USB storage during suspend.
bool unmount_usb_image_mounts_for_suspend(void);
// Unmount every cached image mount during shutdown.
bool shutdown_image_mounts(void);
// Remove empty directories left under the image mount root.
void cleanup_mount_dirs(void);
// Mount an image file if it is stable and not currently rate-limited.
bool maybe_mount_image_file(const char *full_path, const char *display_name,
                            bool *unstable_out);
bool maybe_mount_image_file_with_mode(
    const char *full_path, const char *display_name, bool *unstable_out,
    const bool *mount_read_only_override);
// Build the deterministic runtime mount point for an image source path.
void get_image_mount_point_for_source(const char *file_path,
                                      char mount_point[MAX_PATH]);
// Return true when the filename has a supported image extension.
bool is_supported_image_file_name(const char *name);
// Return true when the source path is a supported image, including nested names.
bool is_supported_image_file_path(const char *full_path, const char *name);
// Return the public filesystem type name for a supported image path.
const char *image_fs_type_name_for_path(const char *path);

#endif
