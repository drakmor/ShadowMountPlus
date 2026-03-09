#ifndef SM_IMAGE_CACHE_H
#define SM_IMAGE_CACHE_H

#include <stdbool.h>

#include "sm_types.h"

typedef struct {
  char path[MAX_PATH];
  char mount_point[MAX_PATH];
  int unit_id;
  attach_backend_t backend;
} image_cache_entry_t;

// Cache a successful image mount and its attached device.
// Returns false when no free cache slot is available.
bool cache_image_mount(const char *path, const char *mount_point,
                       int unit_id, attach_backend_t backend);
// Cache an image source mapping without attach metadata.
bool cache_image_source_mapping(const char *path, const char *mount_point);
// Return a cached image mount entry by index.
bool get_image_cache_entry(int index, image_cache_entry_t *entry_out);
// Mark a cached image mount entry as invalid.
void invalidate_image_cache_entry(int index);
// Resolve a device mapping from the in-memory mount cache.
bool resolve_device_from_mount_cache(const char *mount_point,
                                     attach_backend_t *backend_out,
                                     int *unit_out);
// Resolve the original image file path for a cached runtime mount point.
bool resolve_image_source_from_mount_cache(const char *mount_point,
                                           char *path_out,
                                           size_t path_out_size);

#endif
