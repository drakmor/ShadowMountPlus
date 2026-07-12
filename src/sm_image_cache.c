#include "sm_platform.h"
#include <pthread.h>

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
static pthread_mutex_t g_image_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static void copy_cache_entry(int index, image_cache_entry_t *entry_out) {
  memset(entry_out, 0, sizeof(*entry_out));
  (void)strlcpy(entry_out->path, g_image_cache[index].path,
                sizeof(entry_out->path));
  (void)strlcpy(entry_out->mount_point, g_image_cache[index].mount_point,
                sizeof(entry_out->mount_point));
  entry_out->unit_id = g_image_cache[index].unit_id;
  entry_out->backend = g_image_cache[index].backend;
}

static int find_cache_index(const char *path, const char *mount_point) {
  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    if (!g_image_cache[k].valid)
      continue;
    if (mount_point && strcmp(g_image_cache[k].mount_point, mount_point) == 0) {
      return k;
    }
    if (path && strcmp(g_image_cache[k].path, path) == 0)
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
  pthread_mutex_lock(&g_image_cache_mutex);
  int entry_index = find_cache_index(path, mount_point);
  if (entry_index >= 0) {
    const struct ImageCache *entry = &g_image_cache[entry_index];
    // A metadata-only path mapping must never retarget a live mount entry.
    // The live entry owns its actual filesystem mount point until release.
    if (entry->backend != ATTACH_BACKEND_NONE && entry->unit_id >= 0 &&
        (strcmp(entry->path, path) != 0 ||
         strcmp(entry->mount_point, mount_point) != 0)) {
      pthread_mutex_unlock(&g_image_cache_mutex);
      return true;
    }
  }
  bool ok = upsert_image_source_mapping(path, mount_point) >= 0;
  pthread_mutex_unlock(&g_image_cache_mutex);
  return ok;
}

bool cache_image_mount(const char *path, const char *mount_point, int unit_id,
                       attach_backend_t backend) {
  pthread_mutex_lock(&g_image_cache_mutex);
  int entry_index = upsert_image_source_mapping(path, mount_point);
  if (entry_index < 0) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    return false;
  }

  struct ImageCache *entry = &g_image_cache[entry_index];
  entry->unit_id = unit_id;
  entry->backend = backend;
  pthread_mutex_unlock(&g_image_cache_mutex);
  return true;
}

bool get_image_cache_entry(int index, image_cache_entry_t *entry_out) {
  pthread_mutex_lock(&g_image_cache_mutex);
  if (index < 0 || index >= MAX_IMAGE_MOUNTS || !g_image_cache[index].valid) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    return false;
  }

  copy_cache_entry(index, entry_out);
  pthread_mutex_unlock(&g_image_cache_mutex);
  return true;
}

bool find_image_cache_entry(const char *path, image_cache_entry_t *entry_out,
                            int *index_out) {
  if (!path || !entry_out)
    return false;

  pthread_mutex_lock(&g_image_cache_mutex);
  int index = find_cache_index(path, NULL);
  if (index < 0) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    return false;
  }
  copy_cache_entry(index, entry_out);
  if (index_out)
    *index_out = index;
  pthread_mutex_unlock(&g_image_cache_mutex);
  return true;
}

void invalidate_image_cache_entry(int index) {
  pthread_mutex_lock(&g_image_cache_mutex);
  if (index < 0 || index >= MAX_IMAGE_MOUNTS) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    return;
  }
  memset(&g_image_cache[index], 0, sizeof(g_image_cache[index]));
  pthread_mutex_unlock(&g_image_cache_mutex);
}

bool resolve_device_from_mount_cache(const char *mount_point,
                                     attach_backend_t *backend_out,
                                     int *unit_out) {
  pthread_mutex_lock(&g_image_cache_mutex);
  int entry_index = find_cache_index(NULL, mount_point);
  if (entry_index < 0) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    return false;
  }
  const struct ImageCache *entry = &g_image_cache[entry_index];
  if (entry->backend == ATTACH_BACKEND_NONE || entry->unit_id < 0) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    return false;
  }
  *backend_out = entry->backend;
  *unit_out = entry->unit_id;
  pthread_mutex_unlock(&g_image_cache_mutex);
  return true;
}

bool resolve_image_source_from_mount_cache(const char *mount_point,
                                           char *path_out,
                                           size_t path_out_size) {
  pthread_mutex_lock(&g_image_cache_mutex);
  int entry_index = find_cache_index(NULL, mount_point);
  if (entry_index < 0) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    return false;
  }
  const struct ImageCache *entry = &g_image_cache[entry_index];

  path_out[0] = '\0';
  (void)strlcpy(path_out, entry->path, path_out_size);
  pthread_mutex_unlock(&g_image_cache_mutex);
  return true;
}

static int find_owning_cache_index(const char *path) {
  int best_index = -1;
  size_t best_length = 0;
  for (int i = 0; i < MAX_IMAGE_MOUNTS; ++i) {
    if (!g_image_cache[i].valid)
      continue;
    size_t length = strlen(g_image_cache[i].mount_point);
    if (length <= best_length || strncmp(path, g_image_cache[i].mount_point,
                                         length) != 0) {
      continue;
    }
    if (path[length] != '\0' && path[length] != '/')
      continue;
    best_index = i;
    best_length = length;
  }
  return best_index;
}

static size_t find_owning_chain_indices(const char *path,
                                        int indices[MAX_IMAGE_CHAIN_DEPTH]) {
  char current[MAX_PATH];
  (void)strlcpy(current, path, sizeof(current));

  size_t count = 0;
  while (count < MAX_IMAGE_CHAIN_DEPTH) {
    int index = find_owning_cache_index(current);
    if (index < 0)
      break;
    indices[count++] = index;
    (void)strlcpy(current, g_image_cache[index].path, sizeof(current));
  }
  return count;
}

bool resolve_owning_image_source_from_mount_cache(const char *path,
                                                  char *path_out,
                                                  size_t path_out_size) {
  if (!path || !path_out || path_out_size == 0)
    return false;

  pthread_mutex_lock(&g_image_cache_mutex);
  int index = find_owning_cache_index(path);
  if (index >= 0)
    (void)strlcpy(path_out, g_image_cache[index].path, path_out_size);
  pthread_mutex_unlock(&g_image_cache_mutex);
  return index >= 0;
}

size_t resolve_image_source_chain_from_mount_cache(
    const char *path,
    char chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH]) {
  if (!path || !chain)
    return 0;

  int indices[MAX_IMAGE_CHAIN_DEPTH];
  pthread_mutex_lock(&g_image_cache_mutex);
  size_t count = find_owning_chain_indices(path, indices);
  for (size_t i = 0; i < count; ++i)
    (void)strlcpy(chain[i], g_image_cache[indices[count - i - 1u]].path,
                  MAX_PATH);
  pthread_mutex_unlock(&g_image_cache_mutex);
  return count;
}

bool resolve_outermost_image_source_from_mount_cache(
    const char *path, char *path_out, size_t path_out_size) {
  if (!path || !path_out || path_out_size == 0)
    return false;

  int indices[MAX_IMAGE_CHAIN_DEPTH];
  pthread_mutex_lock(&g_image_cache_mutex);
  size_t count = find_owning_chain_indices(path, indices);
  if (count > 0)
    (void)strlcpy(path_out, g_image_cache[indices[count - 1u]].path,
                  path_out_size);
  pthread_mutex_unlock(&g_image_cache_mutex);
  return count > 0;
}
