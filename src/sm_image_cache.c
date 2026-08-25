#include "sm_platform.h"
#include <pthread.h>

#include "sm_image_cache.h"
#include "sm_limits.h"

struct ImageCache {
  uint64_t generation;
  char path[MAX_PATH];
  char mount_point[MAX_PATH];
  int unit_id;
  attach_backend_t backend;
  attached_device_state_t state;
  attached_unit_detach_state_t detach_state;
  bool detach_attempt_active;
  bool valid;
};

static struct ImageCache g_image_cache[MAX_IMAGE_MOUNTS];
static pthread_mutex_t g_image_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_image_cache_generation;

enum {
  IMAGE_CACHE_INDEX_CONFLICT = -2,
};

static uint64_t next_cache_generation(void) {
  ++g_image_cache_generation;
  if (g_image_cache_generation == 0)
    ++g_image_cache_generation;
  return g_image_cache_generation;
}

static bool device_identity_matches(
    const attached_unit_detach_state_t *left,
    const attached_unit_detach_state_t *right) {
  if (!left->node_identity_valid || !right->node_identity_valid)
    return true;
  return left->node_device == right->node_device &&
         left->node_inode == right->node_inode &&
         left->node_rdev == right->node_rdev;
}

static void capture_attached_unit_identity(struct ImageCache *entry) {
  memset(&entry->detach_state, 0, sizeof(entry->detach_state));
  if (entry->backend == ATTACH_BACKEND_NONE || entry->unit_id < 0)
    return;

  const char *prefix =
      entry->backend == ATTACH_BACKEND_MD ? "/dev/md" : "/dev/lvd";
  char devname[64];
  snprintf(devname, sizeof(devname), "%s%d", prefix, entry->unit_id);

  struct stat st;
  if (stat(devname, &st) != 0)
    return;

  entry->detach_state.node_identity_valid = true;
  entry->detach_state.node_device = (uint64_t)st.st_dev;
  entry->detach_state.node_inode = (uint64_t)st.st_ino;
  entry->detach_state.node_rdev = (uint64_t)st.st_rdev;
}

static void copy_cache_entry(int index, image_cache_entry_t *entry_out) {
  memset(entry_out, 0, sizeof(*entry_out));
  entry_out->generation = g_image_cache[index].generation;
  (void)strlcpy(entry_out->path, g_image_cache[index].path,
                sizeof(entry_out->path));
  (void)strlcpy(entry_out->mount_point, g_image_cache[index].mount_point,
                sizeof(entry_out->mount_point));
  entry_out->unit_id = g_image_cache[index].unit_id;
  entry_out->backend = g_image_cache[index].backend;
  entry_out->state = g_image_cache[index].state;
  entry_out->detach_state = g_image_cache[index].detach_state;
}

static int find_source_path_index(const char *path) {
  if (!path)
    return -1;

  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    if (!g_image_cache[k].valid)
      continue;
    if (strcmp(g_image_cache[k].path, path) == 0)
      return k;
  }

  return -1;
}

static int find_mount_point_index(const char *mount_point) {
  if (!mount_point)
    return -1;

  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    if (!g_image_cache[k].valid)
      continue;
    if (strcmp(g_image_cache[k].mount_point, mount_point) == 0)
      return k;
  }

  return -1;
}

static bool cache_entry_is_metadata_only(const struct ImageCache *entry) {
  return entry->state == ATTACHED_DEVICE_RELEASED && entry->unit_id < 0 &&
         entry->backend == ATTACH_BACKEND_NONE &&
         !entry->detach_attempt_active;
}

static bool find_mapping_indices(const char *path, const char *mount_point,
                                 int *path_index_out,
                                 int *mount_index_out) {
  *path_index_out = -1;
  *mount_index_out = -1;

  for (int k = 0; k < MAX_IMAGE_MOUNTS; ++k) {
    if (!g_image_cache[k].valid)
      continue;
    if (strcmp(g_image_cache[k].path, path) == 0) {
      if (*path_index_out >= 0) {
        errno = EEXIST;
        return false;
      }
      *path_index_out = k;
    }
    if (strcmp(g_image_cache[k].mount_point, mount_point) == 0) {
      if (*mount_index_out >= 0) {
        errno = EEXIST;
        return false;
      }
      *mount_index_out = k;
    }
  }

  return true;
}

static int resolve_or_merge_mapping_index(const char *path,
                                          const char *mount_point,
                                          bool *merged_out) {
  int path_index = -1;
  int mount_index = -1;
  *merged_out = false;
  if (!find_mapping_indices(path, mount_point, &path_index, &mount_index))
    return IMAGE_CACHE_INDEX_CONFLICT;

  if (path_index < 0 || mount_index < 0 || path_index == mount_index)
    return path_index >= 0 ? path_index : mount_index;

  struct ImageCache *path_entry = &g_image_cache[path_index];
  struct ImageCache *mount_entry = &g_image_cache[mount_index];
  if (!cache_entry_is_metadata_only(path_entry) ||
      !cache_entry_is_metadata_only(mount_entry)) {
    errno = EBUSY;
    return IMAGE_CACHE_INDEX_CONFLICT;
  }

  // Both IDs are metadata-only. The requested pair supersedes their stale
  // half-mappings, so merge them into the source-path owner.
  memset(mount_entry, 0, sizeof(*mount_entry));
  *merged_out = true;
  return path_index;
}

static struct ImageCache *find_attachment_record(
    const char *path, uint64_t generation, int unit_id,
    attach_backend_t backend) {
  int index = find_source_path_index(path);
  if (index < 0 || g_image_cache[index].generation != generation ||
      g_image_cache[index].unit_id != unit_id ||
      g_image_cache[index].backend != backend) {
    return NULL;
  }
  return &g_image_cache[index];
}

static int upsert_image_source_mapping(const char *path,
                                       const char *mount_point) {
  if (!path || path[0] == '\0' || !mount_point || mount_point[0] == '\0') {
    errno = EINVAL;
    return -1;
  }
  if (strnlen(path, MAX_PATH) >= MAX_PATH ||
      strnlen(mount_point, MAX_PATH) >= MAX_PATH) {
    errno = ENAMETOOLONG;
    return -1;
  }

  bool merged = false;
  int entry_index =
      resolve_or_merge_mapping_index(path, mount_point, &merged);
  if (entry_index == IMAGE_CACHE_INDEX_CONFLICT)
    return -1;
  if (entry_index >= 0) {
    struct ImageCache *entry = &g_image_cache[entry_index];
    bool mapping_changed = merged || strcmp(entry->path, path) != 0 ||
                           strcmp(entry->mount_point, mount_point) != 0;
    if (!cache_entry_is_metadata_only(entry) && mapping_changed) {
      errno = EBUSY;
      return -1;
    }
    (void)strlcpy(entry->path, path, sizeof(entry->path));
    (void)strlcpy(entry->mount_point, mount_point, sizeof(entry->mount_point));
    if (mapping_changed)
      entry->generation = next_cache_generation();
    return entry_index;
  }

  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    if (!g_image_cache[k].valid) {
      (void)strlcpy(g_image_cache[k].path, path, sizeof(g_image_cache[k].path));
      (void)strlcpy(g_image_cache[k].mount_point, mount_point,
                    sizeof(g_image_cache[k].mount_point));
      g_image_cache[k].unit_id = -1;
      g_image_cache[k].backend = ATTACH_BACKEND_NONE;
      g_image_cache[k].state = ATTACHED_DEVICE_RELEASED;
      g_image_cache[k].generation = next_cache_generation();
      g_image_cache[k].valid = true;
      return k;
    }
  }

  errno = ENOSPC;
  return -1;
}

bool cache_image_source_mapping(const char *path, const char *mount_point) {
  pthread_mutex_lock(&g_image_cache_mutex);
  bool ok = upsert_image_source_mapping(path, mount_point) >= 0;
  pthread_mutex_unlock(&g_image_cache_mutex);
  return ok;
}

bool begin_image_attachment(const char *path, const char *mount_point,
                            uint64_t *generation_out) {
  if (!path || !mount_point || !generation_out) {
    errno = EINVAL;
    return false;
  }

  pthread_mutex_lock(&g_image_cache_mutex);
  int entry_index = upsert_image_source_mapping(path, mount_point);
  if (entry_index < 0) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    return false;
  }
  if (!cache_entry_is_metadata_only(&g_image_cache[entry_index])) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    errno = EBUSY;
    return false;
  }

  struct ImageCache *entry = &g_image_cache[entry_index];
  entry->generation = next_cache_generation();
  entry->unit_id = -1;
  entry->backend = ATTACH_BACKEND_NONE;
  entry->state = ATTACHED_DEVICE_ATTACHING;
  memset(&entry->detach_state, 0, sizeof(entry->detach_state));
  entry->detach_attempt_active = false;
  *generation_out = entry->generation;
  pthread_mutex_unlock(&g_image_cache_mutex);
  return true;
}

bool cancel_image_attachment(const char *path, uint64_t generation) {
  if (!path || generation == 0)
    return false;

  pthread_mutex_lock(&g_image_cache_mutex);
  int index = find_source_path_index(path);
  if (index < 0 || g_image_cache[index].generation != generation ||
      g_image_cache[index].state != ATTACHED_DEVICE_ATTACHING ||
      g_image_cache[index].unit_id >= 0 ||
      g_image_cache[index].backend != ATTACH_BACKEND_NONE) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    return false;
  }

  memset(&g_image_cache[index], 0, sizeof(g_image_cache[index]));
  pthread_mutex_unlock(&g_image_cache_mutex);
  return true;
}

bool publish_image_attachment(const char *path, uint64_t generation,
                              int unit_id, attach_backend_t backend) {
  if (!path || generation == 0 || unit_id < 0 ||
      backend == ATTACH_BACKEND_NONE) {
    return false;
  }

  pthread_mutex_lock(&g_image_cache_mutex);
  int index = find_source_path_index(path);
  if (index < 0 || g_image_cache[index].generation != generation ||
      g_image_cache[index].state != ATTACHED_DEVICE_ATTACHING ||
      g_image_cache[index].unit_id >= 0 ||
      g_image_cache[index].backend != ATTACH_BACKEND_NONE) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    return false;
  }

  struct ImageCache *entry = &g_image_cache[index];
  entry->unit_id = unit_id;
  entry->backend = backend;
  capture_attached_unit_identity(entry);
  pthread_mutex_unlock(&g_image_cache_mutex);
  return true;
}

bool complete_image_attachment(const char *path, uint64_t generation,
                               int unit_id, attach_backend_t backend) {
  if (!path || generation == 0)
    return false;

  pthread_mutex_lock(&g_image_cache_mutex);
  int index = find_source_path_index(path);
  if (index < 0 || g_image_cache[index].generation != generation ||
      g_image_cache[index].unit_id != unit_id ||
      g_image_cache[index].backend != backend ||
      g_image_cache[index].state != ATTACHED_DEVICE_ATTACHING) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    return false;
  }

  g_image_cache[index].state = ATTACHED_DEVICE_MOUNTED;
  capture_attached_unit_identity(&g_image_cache[index]);
  pthread_mutex_unlock(&g_image_cache_mutex);
  return true;
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
  entry->generation = next_cache_generation();
  entry->unit_id = unit_id;
  entry->backend = backend;
  entry->state = ATTACHED_DEVICE_MOUNTED;
  entry->detach_attempt_active = false;
  capture_attached_unit_identity(entry);
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
  int index = find_source_path_index(path);
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

bool mark_image_cache_detach_requested(const char *path, uint64_t generation,
                                       int unit_id,
                                       attach_backend_t backend) {
  if (!path || generation == 0)
    return false;

  pthread_mutex_lock(&g_image_cache_mutex);
  struct ImageCache *entry = find_attachment_record(
      path, generation, unit_id, backend);
  if (!entry) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    return false;
  }

  entry->state = ATTACHED_DEVICE_DETACH_REQUESTED;
  pthread_mutex_unlock(&g_image_cache_mutex);
  return true;
}

bool claim_image_cache_detach(const char *path, int unit_id,
                              attach_backend_t backend,
                              image_cache_entry_t *entry_out) {
  if (!path || !entry_out) {
    errno = EINVAL;
    return false;
  }

  pthread_mutex_lock(&g_image_cache_mutex);
  int index = find_source_path_index(path);
  if (index < 0 || g_image_cache[index].unit_id != unit_id ||
      g_image_cache[index].backend != backend) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    errno = ENOENT;
    return false;
  }
  if (g_image_cache[index].detach_attempt_active) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    errno = EINPROGRESS;
    return false;
  }

  g_image_cache[index].state = ATTACHED_DEVICE_DETACH_REQUESTED;
  g_image_cache[index].detach_attempt_active = true;
  copy_cache_entry(index, entry_out);
  pthread_mutex_unlock(&g_image_cache_mutex);
  return true;
}

bool finish_image_cache_detach_attempt(
    const char *path, uint64_t generation, int unit_id,
    attach_backend_t backend,
    const attached_unit_detach_state_t *detach_state) {
  if (!path || !detach_state)
    return false;

  pthread_mutex_lock(&g_image_cache_mutex);
  struct ImageCache *entry = find_attachment_record(
      path, generation, unit_id, backend);
  if (!entry || !entry->detach_attempt_active ||
      !device_identity_matches(&entry->detach_state, detach_state)) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    return false;
  }

  entry->detach_state = *detach_state;
  entry->state = ATTACHED_DEVICE_DETACH_REQUESTED;
  entry->detach_attempt_active = false;
  pthread_mutex_unlock(&g_image_cache_mutex);
  return true;
}

bool invalidate_image_cache_entry(int index, uint64_t generation) {
  pthread_mutex_lock(&g_image_cache_mutex);
  if (index < 0 || index >= MAX_IMAGE_MOUNTS ||
      !g_image_cache[index].valid ||
      g_image_cache[index].generation != generation) {
    pthread_mutex_unlock(&g_image_cache_mutex);
    return false;
  }
  g_image_cache[index].state = ATTACHED_DEVICE_RELEASED;
  memset(&g_image_cache[index], 0, sizeof(g_image_cache[index]));
  pthread_mutex_unlock(&g_image_cache_mutex);
  return true;
}

bool resolve_device_from_mount_cache(const char *mount_point,
                                     attach_backend_t *backend_out,
                                     int *unit_out) {
  pthread_mutex_lock(&g_image_cache_mutex);
  int entry_index = find_mount_point_index(mount_point);
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
  int entry_index = find_mount_point_index(mount_point);
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
