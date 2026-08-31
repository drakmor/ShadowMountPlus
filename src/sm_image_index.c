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
#define IMAGE_INDEX_VERSION 4u
#define IMAGE_INDEX_BUILD_TIME_LENGTH 24u

#ifndef SHADOWMOUNT_BUILD_TIME
#define SHADOWMOUNT_BUILD_TIME __DATE__ " " __TIME__
#endif

_Static_assert(sizeof(SHADOWMOUNT_BUILD_TIME) <=
                   IMAGE_INDEX_BUILD_TIME_LENGTH,
               "ShadowMount+ build time does not fit the image index");

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
  uint64_t inode;
  uint64_t size;
  int64_t mtime_sec;
  int64_t ctime_sec;
  int32_t mtime_nsec;
  int32_t ctime_nsec;
  uint8_t present;
  uint8_t reserved[7];
} image_index_file_stamp_t;

typedef struct {
  image_index_file_stamp_t config;
  image_index_file_stamp_t autotune;
} image_index_context_t;

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t entry_capacity;
  uint32_t title_capacity;
  char build_time[IMAGE_INDEX_BUILD_TIME_LENGTH];
  image_index_context_t context;
} image_index_header_t;

static image_index_entry_t g_image_index[MAX_IMAGE_MOUNTS];
static image_index_title_t g_image_titles[MAX_IMAGE_TITLES];
static bool g_image_index_loaded;
static bool g_image_index_dirty;
static bool g_image_index_context_valid;
static image_index_context_t g_image_index_context;
static pthread_mutex_t g_image_index_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool read_file_stamp(const char *path,
                            image_index_file_stamp_t *stamp) {
  memset(stamp, 0, sizeof(*stamp));

  struct stat st;
  if (stat(path, &st) != 0)
    return errno == ENOENT;

  stamp->inode = (uint64_t)st.st_ino;
  stamp->size = (uint64_t)st.st_size;
  stamp->mtime_sec = (int64_t)st.st_mtim.tv_sec;
  stamp->ctime_sec = (int64_t)st.st_ctim.tv_sec;
  stamp->mtime_nsec = (int32_t)st.st_mtim.tv_nsec;
  stamp->ctime_nsec = (int32_t)st.st_ctim.tv_nsec;
  stamp->present = 1;
  return true;
}

static bool read_index_context(image_index_context_t *context) {
  bool config_ready = read_file_stamp(CONFIG_FILE, &context->config);
  bool autotune_ready = read_file_stamp(AUTOTUNE_FILE, &context->autotune);
  return config_ready && autotune_ready;
}

static bool file_stamp_equals(const image_index_file_stamp_t *a,
                              const image_index_file_stamp_t *b) {
  return a->present == b->present && a->inode == b->inode &&
         a->size == b->size && a->mtime_sec == b->mtime_sec &&
         a->mtime_nsec == b->mtime_nsec && a->ctime_sec == b->ctime_sec &&
         a->ctime_nsec == b->ctime_nsec;
}

static void clear_index(void) {
  memset(g_image_index, 0, sizeof(g_image_index));
  memset(g_image_titles, 0, sizeof(g_image_titles));
  g_image_index_dirty = false;
}

static void discard_index(const char *reason) {
  clear_index();
  (void)unlink(IMAGE_INDEX_FILE);
  if (reason)
    log_debug("  [IMGIDX] cache discarded (%s): %s", reason,
              IMAGE_INDEX_FILE);
}

static bool refresh_index_context(void) {
  image_index_context_t context;
  if (!read_index_context(&context)) {
    discard_index(g_image_index_context_valid
                      ? "configuration stamp unavailable"
                      : NULL);
    g_image_index_context_valid = false;
    return false;
  }

  if (!g_image_index_context_valid) {
    discard_index(NULL);
  } else if (!file_stamp_equals(&g_image_index_context.config,
                                &context.config)) {
    discard_index("config.ini changed");
  } else if (!file_stamp_equals(&g_image_index_context.autotune,
                                &context.autotune)) {
    discard_index("autotune.ini changed");
  } else {
    return true;
  }

  g_image_index_context = context;
  g_image_index_context_valid = true;
  return true;
}

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
      .context = g_image_index_context,
  };
  (void)strlcpy(header.build_time, SHADOWMOUNT_BUILD_TIME,
                sizeof(header.build_time));
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

static const char *index_header_invalid_reason(
    const image_index_header_t *header) {
  if (header->magic != IMAGE_INDEX_MAGIC ||
      header->version != IMAGE_INDEX_VERSION ||
      header->entry_capacity != MAX_IMAGE_MOUNTS ||
      header->title_capacity != MAX_IMAGE_TITLES) {
    return "cache format changed";
  }
  if (strncmp(header->build_time, SHADOWMOUNT_BUILD_TIME,
              sizeof(header->build_time)) != 0) {
    return "ShadowMount+ build changed";
  }
  if (!g_image_index_context_valid)
    return "configuration stamp unavailable";
  if (!file_stamp_equals(&header->context.config,
                         &g_image_index_context.config)) {
    return "config.ini changed";
  }
  if (!file_stamp_equals(&header->context.autotune,
                         &g_image_index_context.autotune)) {
    return "autotune.ini changed";
  }
  return NULL;
}

static void load_index(void) {
  if (g_image_index_loaded)
    return;
  g_image_index_loaded = true;

  g_image_index_context_valid = read_index_context(&g_image_index_context);

  FILE *file = fopen(IMAGE_INDEX_FILE, "rb");
  if (!file)
    return;
  image_index_header_t header;
  bool header_ready = fread(&header, sizeof(header), 1, file) == 1;
  const char *invalid_reason =
      header_ready ? index_header_invalid_reason(&header)
                   : "cache header is incomplete";
  bool ok = !invalid_reason &&
            fread(g_image_index, sizeof(g_image_index), 1, file) == 1 &&
            fread(g_image_titles, sizeof(g_image_titles), 1, file) == 1;
  (void)fclose(file);
  if (ok) {
    ok = loaded_index_is_valid();
    if (!ok)
      invalid_reason = "cache entries are invalid";
  } else if (!invalid_reason) {
    invalid_reason = "cache data is incomplete";
  }
  if (!ok) {
    discard_index(invalid_reason);
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

bool sm_image_index_visit_ready_titles(
    const char *path, const struct stat *st,
    const struct AppDbTitleList *app_db_titles, bool app_db_titles_ready,
    sm_image_index_title_visitor_t visitor, const void *visitor_ctx) {
  if (!path || !st || !visitor)
    return false;

  char (*titles)[MAX_TITLE_ID] = NULL;
  size_t title_count = 0;
  pthread_mutex_lock(&g_image_index_mutex);
  load_index();
  int entry_index = find_entry(path);
  bool ready = entry_index >= 0 && g_image_index[entry_index].complete &&
               stamp_matches(&g_image_index[entry_index], st) &&
               cached_titles_ready(entry_index, app_db_titles,
                                   app_db_titles_ready);
  if (ready) {
    for (int i = 0; i < MAX_IMAGE_TITLES; ++i) {
      const image_index_title_t *title = &g_image_titles[i];
      if (title->valid && title->image_index == (uint16_t)entry_index)
        title_count++;
    }
    if (title_count > 0) {
      titles = calloc(title_count, sizeof(*titles));
      if (!titles) {
        ready = false;
      } else {
        size_t copied = 0;
        for (int i = 0; i < MAX_IMAGE_TITLES; ++i) {
          const image_index_title_t *title = &g_image_titles[i];
          if (!title->valid ||
              title->image_index != (uint16_t)entry_index) {
            continue;
          }
          (void)strlcpy(titles[copied++], title->title_id, MAX_TITLE_ID);
        }
      }
    }
  }
  pthread_mutex_unlock(&g_image_index_mutex);

  if (ready) {
    for (size_t i = 0; i < title_count; ++i)
      visitor(titles[i], visitor_ctx);
  }
  free(titles);
  return ready;
}

bool sm_image_index_has_source_for_title(const char *title_id) {
  if (!title_id || title_id[0] == '\0')
    return false;

  bool found = false;
  pthread_mutex_lock(&g_image_index_mutex);
  load_index();
  for (int i = 0; i < MAX_IMAGE_TITLES; ++i) {
    const image_index_title_t *title = &g_image_titles[i];
    if (!title->valid || strcmp(title->title_id, title_id) != 0)
      continue;

    const image_index_entry_t *entry = &g_image_index[title->image_index];
    found = entry->valid && path_exists(entry->path);
    break;
  }
  pthread_mutex_unlock(&g_image_index_mutex);
  return found;
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
  if (refresh_index_context() && !save_index())
    log_debug("  [IMGIDX] failed to persist scan index: %s", strerror(errno));
  pthread_mutex_unlock(&g_image_index_mutex);
}

void sm_image_index_prune(void) {
  struct stat st;

  pthread_mutex_lock(&g_image_index_mutex);
  load_index();
  bool context_ready = refresh_index_context();
  for (int i = 0; i < MAX_IMAGE_MOUNTS; ++i) {
    if (g_image_index[i].valid && stat(g_image_index[i].path, &st) != 0 &&
        errno == ENOENT) {
      clear_entry(i);
    }
  }
  if (context_ready && !save_index())
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
