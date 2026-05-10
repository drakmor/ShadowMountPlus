#ifndef SM_FILESYSTEM_H
#define SM_FILESYSTEM_H

#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>

// Check whether a title is present in the installed app set.
bool is_installed(const char *title_id);
// Check whether /user/appmeta/<TITLE_ID>/param.json exists.
bool has_appmeta_data(const char *title_id);
// Check whether a title currently has mounted data.
bool is_data_mounted(const char *title_id);
// Read the mount.lnk source path for a title tracked under /user/app.
bool read_mount_link(const char *title_id, char *out, size_t out_size);
// Validate a /user/app/<TITLE_ID> tracker entry and optionally build its path.
bool resolve_title_app_dir(const struct dirent *entry, char *app_dir,
                           size_t app_dir_size);
// Recover staged mount links and warm image source mappings on startup.
void cleanup_staged_mount_links(void);
// Remove duplicated managed mount layers left from previous runs.
void cleanup_duplicate_title_mounts(void);
// Remount /system_ex with the expected flags.
int remount_system_ex(void);
// Mount a title source into /system_ex/app/<title_id> via nullfs.
bool mount_title_nullfs(const char *title_id, const char *src_path);
// Reconcile the title mount stack against the expected source/backport state.
bool reconcile_title_backport_mount(const char *title_id, const char *src_path,
                                    const char *expected_backport_path,
                                    bool *overlay_active_out);
// Mount a prepared backport overlay on top of an already mounted title.
// Returns false only when nmount() itself fails.
bool mount_backport_overlay(const char *mount_point,
                            const char *backport_path,
                            const char *title_id);
// Unmount all managed /system_ex/app/<title_id> mount stacks on shutdown.
void shutdown_title_mounts(void);
// Remove stale mount links and optionally restore image-backed mounts.
void cleanup_mount_links(const char *removed_source_root,
                         bool unmount_system_ex_bind);
// Unmount and remove title links that point under a source root.
void cleanup_mount_links_for_source_unmount(const char *source_root);
// Unmount and remove title links backed by USB sources or USB-backed images.
void cleanup_usb_mount_links_for_suspend(void);
// Recursively copy a directory tree.
int copy_dir(const char *src, const char *dst);
// Copy a single file.
int copy_file(const char *src, const char *dst);

#endif
