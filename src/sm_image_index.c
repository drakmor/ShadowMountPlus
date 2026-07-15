#include "sm_platform.h"

#include <pthread.h>

#include "sm_appdb.h"
#include "sm_filesystem.h"
#include "sm_image_cache.h"
#include "sm_image_index.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_path_utils.h"
#include "sm_paths.h"

#define IMAGE_INDEX_FILE IMAGE_INDEX_FILE_PATH
#define IMAGE_INDEX_TEMP_FILE LOG_DIR "/image_index.bin.tmp"
#define IMAGE_INDEX_MAGIC 0x58444953u
#define IMAGE_INDEX_VERSION 3u

typedef struct {
  char path[MAX_PATH];
  int64_t size;
  int64_t mtime_sec;
  int32_t mtime_nsec;
  uint8_t complete;
  uint8_t valid;
  uint8_t reserved[2];
} image_index_entry_t;

typedef struct {
  uint16_t image_index;
  uint8_t valid;
  uint8_t reserved;
  char title_id[MAX_TITLE_ID];
} image_index_title_t;

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t entry_capacity;
  uint32_t title_capacity;
} image_index_header_t;

static image_index_entry_t g_image_index[MAX_IMAGE_MOUNTS];
static image_index_title_t g_image_titles[MAX_IMAGE_TITLES];
static bool g_image_index_loaded;
static bool g_image_index_dirty;
static pthread_mutex_t g_image_index_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool stamp_matches(const image_index_entry_t *entry,
                          const struct stat *st) {
  return entry->size == (int64_t)st->st_size &&
         entry->mtime_sec == (int64_t)st->st_mtim.tv_sec &&
         entry->mtime_nsec == (int32_t)st->st_mtim.tv_nsec;
}

static int find_entry(const char *path) {
  for (int i = 0; i < MAX_IMAGE_MOUNTS; ++i) {
    if (g_image_index[i].valid && strcmp(g_image_index[i].path, path) == 0)
      return i;
  }
  return -1;
}

static void clear_entry_titles(int entry_index) {
  for (int i = 0; i < MAX_IMAGE_TITLES; ++i) {
    if (g_image_titles[i].valid &&
        g_image_titles[i].image_index == (uint16_t)entry_index) {
      memset(&g_image_titles[i], 0, sizeof(g_image_titles[i]));
    }
  }
}

static void clear_entry(int entry_index) {
  clear_entry_titles(entry_index);
  memset(&g_image_index[entry_index], 0, sizeof(g_image_index[entry_index]));
  g_image_index_dirty = true;
}

static bool save_index(void) {
  if (!g_image_index_dirty)
    return true;

  FILE *file = fopen(IMAGE_INDEX_TEMP_FILE, "wb");
  if (!file)
    return false;
  image_index_header_t header = {
      .magic = IMAGE_INDEX_MAGIC,
      .version = IMAGE_INDEX_VERSION,
      .entry_capacity = MAX_IMAGE_MOUNTS,
      .title_capacity = MAX_IMAGE_TITLES,
  };
  bool ok = fwrite(&header, sizeof(header), 1, file) == 1 &&
            fwrite(g_image_index, sizeof(g_image_index), 1, file) == 1 &&
            fwrite(g_image_titles, sizeof(g_image_titles), 1, file) == 1 &&
            fflush(file) == 0;
  if (ok && fsync(fileno(file)) != 0)
    ok = false;
  if (fclose(file) != 0)
    ok = false;
  if (!ok || rename(IMAGE_INDEX_TEMP_FILE, IMAGE_INDEX_FILE) != 0) {
    int saved_errno = errno;
    if (saved_errno == 0)
      saved_errno = EIO;
    (void)unlink(IMAGE_INDEX_TEMP_FILE);
    errno = saved_errno;
    return false;
  }
  g_image_index_dirty = false;
  return true;
}

static bool loaded_index_is_valid(void) {
  for (int i = 0; i < MAX_IMAGE_MOUNTS; ++i) {
    const image_index_entry_t *entry = &g_image_index[i];
    if (!entry->valid)
      continue;
    if (entry->complete > 1 || entry->path[0] == '\0' ||
        memchr(entry->path, '\0', sizeof(entry->path)) == NULL) {
      return false;
    }
  }
  for (int i = 0; i < MAX_IMAGE_TITLES; ++i) {
    const image_index_title_t *title = &g_image_titles[i];
    if (!title->valid)
      continue;
    if (title->image_index >= MAX_IMAGE_MOUNTS ||
        !g_image_index[title->image_index].valid ||
        title->title_id[0] == '\0' ||
        memchr(title->title_id, '\0', sizeof(title->title_id)) == NULL) {
      return false;
    }
  }
  return true;
}

static void load_index(void) {
  if (g_image_index_loaded)
    return;
  g_image_index_loaded = true;

  FILE *file = fopen(IMAGE_INDEX_FILE, "rb");
  if (!file)
    return;
  image_index_header_t header;
  bool ok = fread(&header, sizeof(header), 1, file) == 1 &&
            header.magic == IMAGE_INDEX_MAGIC &&
            header.version == IMAGE_INDEX_VERSION &&
            header.entry_capacity == MAX_IMAGE_MOUNTS &&
            header.title_capacity == MAX_IMAGE_TITLES &&
            fread(g_image_index, sizeof(g_image_index), 1, file) == 1 &&
            fread(g_image_titles, sizeof(g_image_titles), 1, file) == 1;
  (void)fclose(file);
  if (ok)
    ok = loaded_index_is_valid();
  if (!ok) {
    memset(g_image_index, 0, sizeof(g_image_index));
    memset(g_image_titles, 0, sizeof(g_image_titles));
    (void)unlink(IMAGE_INDEX_FILE);
    log_debug("  [IMGIDX] invalid cache discarded: %s", IMAGE_INDEX_FILE);
  }
}

static int reserve_entry(const char *path) {
  int entry_index = find_entry(path);
  if (entry_index >= 0)
    return entry_index;
  for (int i = 0; i < MAX_IMAGE_MOUNTS; ++i) {
    if (!g_image_index[i].valid) {
      g_image_index[i].valid = 1;
      (void)strlcpy(g_image_index[i].path, path, sizeof(g_image_index[i].path));
      return i;
    }
  }
  return -1;
}

static void begin_scan_locked(const char *path, const struct stat *st) {
  int entry_index = reserve_entry(path);
  if (entry_index < 0) {
    log_debug("  [IMGIDX] cache full, scanning without fingerprint: %s", path);
    return;
  }
  clear_entry_titles(entry_index);
  image_index_entry_t *entry = &g_image_index[entry_index];
  entry->size = (int64_t)st->st_size;
  entry->mtime_sec = (int64_t)st->st_mtim.tv_sec;
  entry->mtime_nsec = (int32_t)st->st_mtim.tv_nsec;
  entry->complete = 0;
  entry->valid = 1;
  g_image_index_dirty = true;
}

static bool cached_titles_ready(int entry_index,
                                const struct AppDbTitleList *app_db_titles,
                                bool app_db_titles_ready) {
  const char *image_path = g_image_index[entry_index].path;
  for (int i = 0; i < MAX_IMAGE_TITLES; ++i) {
    if (!g_image_titles[i].valid ||
        g_image_titles[i].image_index != (uint16_t)entry_index) {
      continue;
    }
    const char *title_id = g_image_titles[i].title_id;
    if (!is_installed(title_id) || !has_appmeta_data(title_id))
      return false;
    char runtime_path[MAX_PATH];
    char linked_image[MAX_PATH];
    if (!read_mount_link(title_id, runtime_path, sizeof(runtime_path)) ||
        !is_under_image_mount_base(runtime_path) ||
        !read_mount_image_link(title_id, linked_image, sizeof(linked_image)) ||
        strcmp(linked_image, image_path) != 0) {
      return false;
    }
    if (app_db_titles_ready &&
        !app_db_title_list_contains(app_db_titles, title_id)) {
      return false;
    }
  }
  return true;
}

bool sm_image_index_needs_scan(const char *path, const struct stat *st,
                               const struct AppDbTitleList *app_db_titles,
                               bool app_db_titles_ready) {
  pthread_mutex_lock(&g_image_index_mutex);
  load_index();
  int entry_index = find_entry(path);
  bool needs_scan = entry_index < 0 || !g_image_index[entry_index].complete ||
                    !stamp_matches(&g_image_index[entry_index], st) ||
                    !cached_titles_ready(entry_index, app_db_titles,
                                         app_db_titles_ready);
  pthread_mutex_unlock(&g_image_index_mutex);
  return needs_scan;
}

void sm_image_index_begin_scan(const char *path, const struct stat *st) {
  pthread_mutex_lock(&g_image_index_mutex);
  load_index();
  begin_scan_locked(path, st);
  pthread_mutex_unlock(&g_image_index_mutex);
}

bool sm_image_index_record_game(const char *game_path, const char *title_id) {
  pthread_mutex_lock(&g_image_index_mutex);
  load_index();
  char image_path[MAX_PATH];
  if (!resolve_outermost_image_source_from_mount_cache(
          game_path, image_path, sizeof(image_path))) {
    pthread_mutex_unlock(&g_image_index_mutex);
    return true;
  }
  int entry_index = find_entry(image_path);
  if (entry_index < 0) {
    struct stat st;
    if (stat(image_path, &st) != 0) {
      pthread_mutex_unlock(&g_image_index_mutex);
      return false;
    }
    begin_scan_locked(image_path, &st);
    entry_index = find_entry(image_path);
    if (entry_index < 0) {
      pthread_mutex_unlock(&g_image_index_mutex);
      return false;
    }
  }
  char tracked_path[MAX_PATH];
  char linked_image[MAX_PATH];
  if (is_installed(title_id) &&
      read_mount_link(title_id, tracked_path, sizeof(tracked_path)) &&
      strcmp(tracked_path, game_path) == 0 &&
      (!read_mount_image_link(title_id, linked_image, sizeof(linked_image)) ||
       strcmp(linked_image, image_path) != 0) &&
      !write_mount_image_link(title_id, image_path)) {
    log_debug("  [IMGIDX] failed to repair image link: %s -> %s", title_id,
              image_path);
  }
  for (int i = 0; i < MAX_IMAGE_TITLES; ++i) {
    if (g_image_titles[i].valid &&
        g_image_titles[i].image_index == (uint16_t)entry_index &&
        strcmp(g_image_titles[i].title_id, title_id) == 0) {
      pthread_mutex_unlock(&g_image_index_mutex);
      return true;
    }
  }
  for (int i = 0; i < MAX_IMAGE_TITLES; ++i) {
    if (g_image_titles[i].valid)
      continue;
    g_image_titles[i].valid = 1;
    g_image_titles[i].image_index = (uint16_t)entry_index;
    (void)strlcpy(g_image_titles[i].title_id, title_id,
                  sizeof(g_image_titles[i].title_id));
    g_image_index_dirty = true;
    pthread_mutex_unlock(&g_image_index_mutex);
    return true;
  }
  log_debug("  [IMGIDX] title cache full: %s", title_id);
  pthread_mutex_unlock(&g_image_index_mutex);
  return false;
}

void sm_image_index_complete_scan(const char *path) {
  pthread_mutex_lock(&g_image_index_mutex);
  load_index();
  int entry_index = find_entry(path);
  if (entry_index < 0) {
    pthread_mutex_unlock(&g_image_index_mutex);
    return;
  }
  image_index_entry_t *entry = &g_image_index[entry_index];
  if (!entry->complete) {
    entry->complete = 1;
    g_image_index_dirty = true;
  }
  pthread_mutex_unlock(&g_image_index_mutex);
}

void sm_image_index_flush(void) {
  pthread_mutex_lock(&g_image_index_mutex);
  load_index();
  if (!save_index())
    log_debug("  [IMGIDX] failed to persist scan index: %s", strerror(errno));
  pthread_mutex_unlock(&g_image_index_mutex);
}

void sm_image_index_prune(void) {
  struct stat st;

  pthread_mutex_lock(&g_image_index_mutex);
  load_index();
  for (int i = 0; i < MAX_IMAGE_MOUNTS; ++i) {
    if (g_image_index[i].valid && stat(g_image_index[i].path, &st) != 0 &&
        errno == ENOENT) {
      clear_entry(i);
    }
  }
  if (!save_index())
    log_debug("  [IMGIDX] failed to persist prune: %s", strerror(errno));
  pthread_mutex_unlock(&g_image_index_mutex);
}

bool sm_image_index_snapshot(sm_image_index_snapshot_entry_t **entries_out,
                             size_t *count_out) {
  if (!entries_out || !count_out)
    return false;
  *entries_out = NULL;
  *count_out = 0;

  pthread_mutex_lock(&g_image_index_mutex);
  load_index();
  size_t count = 0;
  for (int i = 0; i < MAX_IMAGE_MOUNTS; ++i) {
    if (g_image_index[i].valid)
      count++;
  }

  sm_image_index_snapshot_entry_t *entries = NULL;
  if (count > 0) {
    entries = calloc(count, sizeof(*entries));
    if (!entries) {
      pthread_mutex_unlock(&g_image_index_mutex);
      return false;
    }
  }

  size_t copied = 0;
  for (int i = 0; i < MAX_IMAGE_MOUNTS; ++i) {
    const image_index_entry_t *source = &g_image_index[i];
    if (!source->valid)
      continue;
    sm_image_index_snapshot_entry_t *entry = &entries[copied++];
    (void)strlcpy(entry->path, source->path, sizeof(entry->path));
    entry->size = source->size;
    entry->mtime_sec = source->mtime_sec;
    entry->mtime_nsec = source->mtime_nsec;
    entry->complete = source->complete != 0;
  }
  pthread_mutex_unlock(&g_image_index_mutex);
  *entries_out = entries;
  *count_out = copied;
  return true;
}
