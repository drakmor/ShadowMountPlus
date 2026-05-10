#ifndef SM_IMAGE_H
#define SM_IMAGE_H

#include <stdbool.h>

#include "sm_types.h"

// Log filesystem statistics for a mounted path.
void log_fs_stats(const char *tag, const char *path, const char *type_hint);
// Attach and mount an image file to its runtime mount point.
bool mount_image(const char *file_path, image_fs_type_t fs_type);
// Unmount an image mount point and detach its backing device.
bool unmount_image(const char *file_path, int unit_id, attach_backend_t backend);
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
void maybe_mount_image_file(const char *full_path, const char *name,
                            bool *unstable_out);
// Return true when the filename has a supported image extension.
bool is_supported_image_file_name(const char *name);

#endif
