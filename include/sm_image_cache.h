#ifndef SM_IMAGE_CACHE_H
#define SM_IMAGE_CACHE_H

#include <stdbool.h>

#include "sm_types.h"

typedef struct {
  uint64_t generation;
  char path[MAX_PATH];
  char mount_point[MAX_PATH];
  int unit_id;
  attach_backend_t backend;
  attached_device_state_t state;
  attached_unit_detach_state_t detach_state;
} image_cache_entry_t;

// Reserve a cache record before issuing an attach ioctl.
bool begin_image_attachment(const char *path, const char *mount_point,
                            uint64_t *generation_out);
// Cancel an unpublished attachment reservation of the same generation.
bool cancel_image_attachment(const char *path, uint64_t generation);
// Publish the kernel unit immediately after a successful attach ioctl.
bool publish_image_attachment(const char *path, uint64_t generation,
                              int unit_id, attach_backend_t backend);
// Mark a published device as a verified filesystem mount.
bool complete_image_attachment(const char *path, uint64_t generation,
                               int unit_id, attach_backend_t backend);
// Cache a successful image mount and its attached device.
// Returns false when no slot is available or either key has another owner.
bool cache_image_mount(const char *path, const char *mount_point,
                       int unit_id, attach_backend_t backend);
// Cache an image source mapping without attach metadata. Conflicting active
// source-path and mount-point owners are never retargeted.
bool cache_image_source_mapping(const char *path, const char *mount_point);
// Return a cached image mount entry by index.
bool get_image_cache_entry(int index, image_cache_entry_t *entry_out);
// Find a cached source path with one cache lock acquisition.
bool find_image_cache_entry(const char *path, image_cache_entry_t *entry_out,
                             int *index_out);
// Keep a device in the detach-retry queue until its node is released.
bool mark_image_cache_detach_requested(const char *path, uint64_t generation,
                                       int unit_id,
                                       attach_backend_t backend);
// Claim one detach attempt and return a stable snapshot of its record.
bool claim_image_cache_detach(const char *path, int unit_id,
                              attach_backend_t backend,
                              image_cache_entry_t *entry_out);
// Publish the result of a claimed detach attempt and release its claim.
bool finish_image_cache_detach_attempt(
    const char *path, uint64_t generation, int unit_id,
    attach_backend_t backend,
    const attached_unit_detach_state_t *detach_state);
// Invalidate the same generation of a cached image mount entry.
bool invalidate_image_cache_entry(int index, uint64_t generation);
// Resolve a device mapping from the in-memory mount cache.
bool resolve_device_from_mount_cache(const char *mount_point,
                                     attach_backend_t *backend_out,
                                     int *unit_out);
// Resolve the original image file path for a cached runtime mount point.
bool resolve_image_source_from_mount_cache(const char *mount_point,
                                           char *path_out,
                                           size_t path_out_size);
// Resolve the image whose mount point owns a mounted child path.
bool resolve_owning_image_source_from_mount_cache(const char *path,
                                                  char *path_out,
                                                  size_t path_out_size);
// Resolve every owning image from the outermost source to the innermost one.
size_t resolve_image_source_chain_from_mount_cache(
    const char *path,
    char chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH]);
// Resolve only the outermost owning image source.
bool resolve_outermost_image_source_from_mount_cache(
    const char *path, char *path_out, size_t path_out_size);

#endif
