#include "sm_platform.h"
#include "sm_image_cache.h"
#include "sm_limits.h"

struct ImageCache {
  char path[MAX_PATH];
  char mount_point[MAX_PATH];
  int unit_id;
  attach_backend_t backend;
  bool valid;
};

static struct ImageCache g_image_cache[MAX_IMAGE_MOUNTS];

static int find_cache_index(const char *path, const char *mount_point) {
  if ((!path || path[0] == '\0') && (!mount_point || mount_point[0] == '\0'))
    return -1;

  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    if (!g_image_cache[k].valid)
      continue;
    if (mount_point && mount_point[0] != '\0' &&
        strcmp(g_image_cache[k].mount_point, mount_point) == 0) {
      return k;
    }
    if (path && path[0] != '\0' && strcmp(g_image_cache[k].path, path) == 0)
      return k;
  }

  return -1;
}

static int upsert_image_source_mapping(const char *path, const char *mount_point) {
  int entry_index = find_cache_index(path, mount_point);
  if (entry_index >= 0) {
    struct ImageCache *entry = &g_image_cache[entry_index];
    (void)strlcpy(entry->path, path, sizeof(entry->path));
    (void)strlcpy(entry->mount_point, mount_point, sizeof(entry->mount_point));
    return entry_index;
  }

  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    if (!g_image_cache[k].valid) {
      (void)strlcpy(g_image_cache[k].path, path, sizeof(g_image_cache[k].path));
      (void)strlcpy(g_image_cache[k].mount_point, mount_point,
                    sizeof(g_image_cache[k].mount_point));
      g_image_cache[k].unit_id = -1;
      g_image_cache[k].backend = ATTACH_BACKEND_NONE;
      g_image_cache[k].valid = true;
      return k;
    }
  }

  return -1;
}

bool cache_image_source_mapping(const char *path, const char *mount_point) {
  return upsert_image_source_mapping(path, mount_point) >= 0;
}

bool cache_image_mount(const char *path, const char *mount_point, int unit_id,
                       attach_backend_t backend) {
  int entry_index = upsert_image_source_mapping(path, mount_point);
  if (entry_index < 0)
    return false;

  struct ImageCache *entry = &g_image_cache[entry_index];
  entry->unit_id = unit_id;
  entry->backend = backend;
  return true;
}

bool get_image_cache_entry(int index, image_cache_entry_t *entry_out) {
  if (!entry_out || index < 0 || index >= MAX_IMAGE_MOUNTS ||
      !g_image_cache[index].valid) {
    return false;
  }

  memset(entry_out, 0, sizeof(*entry_out));
  (void)strlcpy(entry_out->path, g_image_cache[index].path,
                sizeof(entry_out->path));
  (void)strlcpy(entry_out->mount_point, g_image_cache[index].mount_point,
                sizeof(entry_out->mount_point));
  entry_out->unit_id = g_image_cache[index].unit_id;
  entry_out->backend = g_image_cache[index].backend;
  return true;
}

void invalidate_image_cache_entry(int index) {
  if (index < 0 || index >= MAX_IMAGE_MOUNTS)
    return;
  memset(&g_image_cache[index], 0, sizeof(g_image_cache[index]));
}

bool resolve_device_from_mount_cache(const char *mount_point,
                                     attach_backend_t *backend_out,
                                     int *unit_out) {
  int entry_index = find_cache_index(NULL, mount_point);
  if (entry_index < 0)
    return false;
  const struct ImageCache *entry = &g_image_cache[entry_index];
  if (entry->backend == ATTACH_BACKEND_NONE || entry->unit_id < 0)
    return false;
  *backend_out = entry->backend;
  *unit_out = entry->unit_id;
  return true;
}

bool resolve_image_source_from_mount_cache(const char *mount_point,
                                           char *path_out,
                                           size_t path_out_size) {
  int entry_index = find_cache_index(NULL, mount_point);
  if (entry_index < 0 || !path_out || path_out_size == 0)
    return false;
  const struct ImageCache *entry = &g_image_cache[entry_index];

  path_out[0] = '\0';
  (void)strlcpy(path_out, entry->path, path_out_size);
  return path_out[0] != '\0';
}
