/* Immutable, no-follow FPKG cache generations. */
#include "sm_platform.h"

#include <sys/file.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <stdatomic.h>
#include <strings.h>
#include <dirent.h>
#include <stdio.h>

#include "sm_fpkg_cache.h"
#include "sm_fpkg_backport.h"
#include "sm_time.h"

#define SM_FPKG_CACHE_MANIFEST_LIMIT (4u * 1024u * 1024u)
#define SM_FPKG_CACHE_RESERVE_BYTES (16ull * 1024ull * 1024ull)
#define SM_FPKG_CACHE_COMPONENT_LIMIT 255u
#define SM_FPKG_CACHE_OWNED_FILE_LIMIT 100000u

#if defined(__linux__) && !defined(RENAME_NOREPLACE)
#define RENAME_NOREPLACE (1u << 0)
#endif

typedef struct {
  char *data;
  size_t size;
  size_t capacity;
} cache_text_t;

typedef struct {
  int directory_fd;
  const sm_fpkg_cache_options_t *options;
  uint64_t total_written;
} cache_sink_t;

typedef struct {
  int fd;
  mode_t mode;
  uint64_t expected_size;
  uint64_t written;
  cache_sink_t *sink;
} cache_file_t;

static atomic_uint_fast64_t g_cache_generation_counter = ATOMIC_VAR_INIT(1);

static int rename_noreplace(int from_fd, const char *from, int to_fd,
                            const char *to);
static bool validate_generation_owned_tree(int generation_fd,
                                           const char *title_id);
static bool generation_executable_modes_valid(int generation_fd);

static bool is_cancelled(const sm_fpkg_cache_options_t *options) {
  return options && options->is_cancelled &&
         options->is_cancelled(options->cancel_ctx);
}

static bool valid_text_path(const char *path, size_t capacity) {
  if (!path || path[0] != '/' || path[1] == '\0' ||
      strnlen(path, capacity) >= capacity || strchr(path, '\n') ||
      strchr(path, '\r'))
    return false;
  return true;
}

static bool valid_title_id(const char *title_id) {
  if (!title_id || strlen(title_id) != 9u)
    return false;
  for (size_t i = 0; i < 4u; ++i) {
    if (title_id[i] < 'A' || title_id[i] > 'Z')
      return false;
  }
  for (size_t i = 4u; i < 9u; ++i) {
    if (title_id[i] < '0' || title_id[i] > '9')
      return false;
  }
  return memcmp(title_id, "NPXS", 4u) != 0;
}

static bool safe_relative_path(const char *path) {
  if (!path || !path[0] || path[0] == '/' || strchr(path, '\\') ||
      strchr(path, '\n') || strchr(path, '\r') || strlen(path) >= MAX_PATH)
    return false;
  const char *component = path;
  for (const char *p = path;; ++p) {
    if (*p != '/' && *p != '\0')
      continue;
    size_t length = (size_t)(p - component);
    if (length == 0 || length > SM_FPKG_CACHE_COMPONENT_LIMIT ||
        (length == 1 && component[0] == '.') ||
        (length == 2 && component[0] == '.' && component[1] == '.'))
      return false;
    if (*p == '\0')
      return true;
    component = p + 1;
  }
}

static int open_directory_path(const char *path, bool create_missing) {
  if (!valid_text_path(path, MAX_PATH)) {
    errno = EINVAL;
    return -1;
  }
  char copy[MAX_PATH];
  size_t length = strlen(path);
  if (length <= 1u || length >= sizeof(copy)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(copy, path, length + 1u);
  while (length > 1u && copy[length - 1u] == '/')
    copy[--length] = '\0';

  int current = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (current < 0)
    return -1;
  char *cursor = copy + 1;
  while (*cursor) {
    char *slash = strchr(cursor, '/');
    if (slash)
      *slash = '\0';
    size_t component_length = strlen(cursor);
    if (component_length == 0 || component_length > SM_FPKG_CACHE_COMPONENT_LIMIT ||
        strcmp(cursor, ".") == 0 || strcmp(cursor, "..") == 0) {
      close(current);
      errno = EINVAL;
      return -1;
    }
    bool created = false;
    if (create_missing) {
      created = mkdirat(current, cursor, 0700) == 0;
      if (!created && errno != EEXIST) {
        int saved = errno;
        close(current);
        errno = saved;
        return -1;
      }
    }
    int next = openat(current, cursor,
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0) {
      int saved = errno;
      close(current);
      errno = saved;
      return -1;
    }
    struct stat st;
    int stat_result = fstat(next, &st);
    if (stat_result != 0 || !S_ISDIR(st.st_mode)) {
      int saved = stat_result != 0 ? errno : EPERM;
      close(next);
      close(current);
      errno = saved;
      return -1;
    }
    if (created && fsync(current) != 0) {
      int saved = errno;
      close(next);
      close(current);
      errno = saved;
      return -1;
    }
    close(current);
    current = next;
    if (!slash)
      break;
    cursor = slash + 1;
  }
  return current;
}

static int open_or_create_child_dir(int parent_fd, const char *name,
                                    mode_t mode, bool *created_out) {
  *created_out = false;
  if (!name || !name[0] || strchr(name, '/') || strcmp(name, ".") == 0 ||
      strcmp(name, "..") == 0 || strlen(name) > SM_FPKG_CACHE_COMPONENT_LIMIT) {
    errno = EINVAL;
    return -1;
  }
  if (mkdirat(parent_fd, name, mode) == 0) {
    *created_out = true;
    if (fsync(parent_fd) != 0)
      return -1;
  } else if (errno != EEXIST) {
    return -1;
  }
  int fd = openat(parent_fd, name,
                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return -1;
  struct stat st;
  int stat_result = fstat(fd, &st);
  if (stat_result != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 0022) != 0) {
    int saved = stat_result != 0 ? errno : EPERM;
    close(fd);
    errno = saved;
    return -1;
  }
  return fd;
}

static bool relative_parent(int root_fd, const char *path, bool create,
                            int *parent_fd_out, char leaf_out[NAME_MAX + 1]) {
  *parent_fd_out = -1;
  if (!safe_relative_path(path)) {
    errno = EINVAL;
    return false;
  }
  char copy[MAX_PATH];
  (void)strlcpy(copy, path, sizeof(copy));
  char *last = strrchr(copy, '/');
  if (last)
    *last = '\0';
  const char *leaf = last ? last + 1 : copy;
  size_t leaf_length = strlen(leaf);
  if (leaf_length == 0 || leaf_length > NAME_MAX) {
    errno = ENAMETOOLONG;
    return false;
  }
  memcpy(leaf_out, leaf, leaf_length + 1u);

  int current = dup(root_fd);
  if (current < 0)
    return false;
  if (last) {
    char *component = copy;
    for (;;) {
      char *slash = strchr(component, '/');
      if (slash)
        *slash = '\0';
      if (create && mkdirat(current, component, 0700) == 0) {
        if (fsync(current) != 0) {
          int saved = errno;
          close(current);
          errno = saved;
          return false;
        }
      } else if (create && errno != EEXIST) {
        int saved = errno;
        close(current);
        errno = saved;
        return false;
      }
      int next = openat(current, component,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next < 0) {
        int saved = errno;
        close(current);
        errno = saved;
        return false;
      }
      struct stat directory_stat;
      int directory_stat_result = fstat(next, &directory_stat);
      if (directory_stat_result != 0 || !S_ISDIR(directory_stat.st_mode) ||
          directory_stat.st_uid != geteuid() ||
          (directory_stat.st_mode & 0022) != 0) {
        int saved = directory_stat_result != 0 ? errno : EPERM;
        close(next);
        close(current);
        errno = saved;
        return false;
      }
      close(current);
      current = next;
      if (!slash)
        break;
      component = slash + 1;
    }
  }
  *parent_fd_out = current;
  return true;
}

static sm_fpkg_status_t append_text(cache_text_t *text, const char *bytes,
                                    size_t length) {
  if (length > SM_FPKG_CACHE_MANIFEST_LIMIT - text->size)
    return SM_FPKG_UNSUPPORTED;
  size_t needed = text->size + length;
  if (needed > text->capacity) {
    size_t capacity = text->capacity ? text->capacity : 1024u;
    while (capacity < needed) {
      if (capacity > SM_FPKG_CACHE_MANIFEST_LIMIT / 2u) {
        capacity = SM_FPKG_CACHE_MANIFEST_LIMIT;
        break;
      }
      capacity *= 2u;
    }
    char *grown = realloc(text->data, capacity);
    if (!grown)
      return SM_FPKG_NOMEM;
    text->data = grown;
    text->capacity = capacity;
  }
  memcpy(text->data + text->size, bytes, length);
  text->size = needed;
  return SM_FPKG_OK;
}

static sm_fpkg_status_t append_manifest_file(cache_text_t *text,
                                             const char *path,
                                             uint64_t size) {
  char line[MAX_PATH + 64];
  int length = snprintf(line, sizeof(line), "file=%llu:%s\n",
                        (unsigned long long)size, path);
  if (length < 0 || (size_t)length >= sizeof(line))
    return SM_FPKG_UNSUPPORTED;
  return append_text(text, line, (size_t)length);
}

static bool lower_extension_is(const char *path, const char *extension) {
  const char *dot = strrchr(path, '.');
  return dot && strcasecmp(dot, extension) == 0;
}

static bool is_package_fakelib_path(const char *path) {
  return !strncasecmp(path, "fakelib/", 8u) ||
         !strncasecmp(path, "fakelib2/", 9u);
}

static bool is_required_loose_file(const char *path) {
  if (!strcasecmp(path, "eboot.bin") || !strcasecmp(path, "eboot2.bin"))
    return true;
  if (!strncasecmp(path, "sce_sys/", 8u))
    return true;
  return lower_extension_is(path, ".prx") ||
         lower_extension_is(path, ".sprx") || is_package_fakelib_path(path);
}

static bool is_executable_file(const char *path) {
  return !strcasecmp(path, "eboot.bin") || !strcasecmp(path, "eboot2.bin") ||
         lower_extension_is(path, ".prx") || lower_extension_is(path, ".sprx");
}

static bool is_ampr_pack_index(const char *path) {
  return !strcasecmp(path, "ampr_assets.index") ||
         !strcasecmp(path, "ampr_zstd.index");
}

static bool is_ampr_input_index(const char *path) {
  return !strcasecmp(path, "ampr_emu.index");
}

static bool is_reserved_cache_path(const char *path) {
  return !strcasecmp(path, "ampr_emu.index") ||
         !strcasecmp(path, "ampr_assets.index") ||
         !strcasecmp(path, "ampr_zstd.index") ||
         !strcasecmp(path, "kraken.image") || !strcasecmp(path, ".built") ||
         !strcasecmp(path, ".done") || !strcasecmp(path, ".journal") ||
         !strncasecmp(path, "kraken/", 7u) || !strcasecmp(path, "kraken");
}

typedef struct {
  sm_fpkg_index_extract_fn extra_selector;
  void *extra_ctx;
  const sm_fpkg_backport_t *backport;
  const sm_fpkg_index_file_t *overrides;
} selector_ctx_t;

static bool select_loose_file(const char *path, void *opaque) {
  selector_ctx_t *selector = opaque;
  return is_required_loose_file(path) ||
         (selector->extra_selector &&
          selector->extra_selector(path, selector->extra_ctx));
}

/* KrakenMount extracts these files and indexes all remaining game files. It
 * also replaces a prebuilt loose AMPR index and leaves fakelib files outside
 * the package-data index because SMP exposes that tree through its own overlay.
 */
static bool skip_package_index_file(const char *path, void *opaque) {
  (void)opaque;
  return is_ampr_input_index(path) || is_package_fakelib_path(path);
}

static uint64_t source_fingerprint(const sm_pkg_info_t *metadata) {
  uint64_t hash = 14695981039346656037ull;
  const uint64_t fields[] = {
      metadata->source_device, metadata->source_inode, metadata->source_size,
      metadata->source_ctime, (uint64_t)metadata->source_mtime,
      metadata->source_mtime_ns, metadata->source_ctime_ns};
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    uint64_t value = fields[i];
    for (unsigned byte = 0; byte < 8u; ++byte) {
      hash ^= (uint8_t)value;
      hash *= 1099511628211ull;
      value >>= 8;
    }
  }
  return hash;
}

static bool checked_free_space(int directory_fd, uint64_t required) {
  struct statvfs fs;
  if (fstatvfs(directory_fd, &fs) != 0)
    return false;
  uint64_t block_size = fs.f_frsize ? (uint64_t)fs.f_frsize
                                    : (uint64_t)fs.f_bsize;
  if (block_size == 0 || fs.f_bavail > UINT64_MAX / block_size)
    return false;
  uint64_t available = (uint64_t)fs.f_bavail * block_size;
  return available >= required;
}

static sm_fpkg_status_t estimate_extraction(
    const sm_fpkg_t *fpkg, selector_ctx_t *selector, uint64_t limit,
    uint64_t *bytes_out, size_t *count_out, bool *eboot_out,
    bool *metadata_out, cache_text_t *manifest) {
  *bytes_out = 0;
  *count_out = 0;
  *eboot_out = false;
  *metadata_out = false;
  size_t count = sm_fpkg_file_count(fpkg);
  if (count == 0 || count > 1000000u)
    return SM_FPKG_UNSUPPORTED;
  for (size_t i = 0; i < count; ++i) {
    sm_fpkg_file_t file;
    if (!sm_fpkg_file_at(fpkg, i, &file))
      return SM_FPKG_CORRUPT;
    if (file.is_directory)
      continue;
    if (is_ampr_pack_index(file.path))
      return SM_FPKG_UNSUPPORTED;
    if (!select_loose_file(file.path, selector))
      continue;
    if (sm_fpkg_backport_find(selector->backport, file.path) != SIZE_MAX)
      continue;
    if (!safe_relative_path(file.path) || is_reserved_cache_path(file.path) ||
        file.size > limit ||
        *bytes_out > limit - file.size)
      return SM_FPKG_UNSUPPORTED;
    *bytes_out += file.size;
    ++*count_out;
    if (!strcasecmp(file.path, "eboot.bin") ||
        !strcasecmp(file.path, "eboot2.bin"))
      *eboot_out = true;
    if (!strcasecmp(file.path, "sce_sys/param.json") ||
        !strcasecmp(file.path, "sce_sys/param.sfo"))
      *metadata_out = true;
    sm_fpkg_status_t status = append_manifest_file(manifest, file.path,
                                                   file.size);
    if (status != SM_FPKG_OK)
      return status;
  }
  size_t override_count = sm_fpkg_backport_count(selector->backport);
  const sm_fpkg_index_file_t *overrides = selector->overrides;
  if (override_count && !overrides)
    return SM_FPKG_CORRUPT;
  for (size_t i = 0; i < override_count; ++i) {
    const sm_fpkg_index_file_t *file = &overrides[i];
    if (!safe_relative_path(file->path) || is_reserved_cache_path(file->path) ||
        file->size > limit || *bytes_out > limit - file->size)
      return SM_FPKG_UNSUPPORTED;
    *bytes_out += file->size;
    ++*count_out;
    if (!strcasecmp(file->path, "eboot.bin") || !strcasecmp(file->path, "eboot2.bin"))
      *eboot_out = true;
    if (!strcasecmp(file->path, "sce_sys/param.json") ||
        !strcasecmp(file->path, "sce_sys/param.sfo"))
      *metadata_out = true;
    sm_fpkg_status_t status = append_manifest_file(manifest, file->path, file->size);
    if (status != SM_FPKG_OK)
      return status;
  }
  return *eboot_out && *metadata_out ? SM_FPKG_OK : SM_FPKG_UNSUPPORTED;
}

static sm_fpkg_status_t write_all(int fd, const void *data, size_t size) {
  const uint8_t *bytes = data;
  size_t written = 0;
  while (written < size) {
    ssize_t count = write(fd, bytes + written, size - written);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return SM_FPKG_IO;
    written += (size_t)count;
  }
  return SM_FPKG_OK;
}

static sm_fpkg_status_t write_new_file(int root_fd, const char *path,
                                       const void *data, size_t size,
                                       mode_t mode) {
  int parent_fd = -1;
  char leaf[NAME_MAX + 1];
  if (!relative_parent(root_fd, path, true, &parent_fd, leaf))
    return SM_FPKG_IO;
  int fd = openat(parent_fd, leaf,
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                  mode);
  if (fd < 0) {
    close(parent_fd);
    return SM_FPKG_IO;
  }
  sm_fpkg_status_t status = write_all(fd, data, size);
  if (status == SM_FPKG_OK && fsync(fd) != 0)
    status = SM_FPKG_IO;
  if (close(fd) != 0 && status == SM_FPKG_OK)
    status = SM_FPKG_IO;
  if (status == SM_FPKG_OK && fsync(parent_fd) != 0)
    status = SM_FPKG_IO;
  close(parent_fd);
  return status;
}

static bool valid_generation_name(const char *name) {
  if (!name || strncmp(name, "gen-", 4u) != 0 ||
      strlen(name) >= 112u || strchr(name, '/') || strchr(name, '\\'))
    return false;
  for (const unsigned char *p = (const unsigned char *)name + 4u; *p; ++p) {
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || *p == '-'))
      return false;
  }
  return name[4] != '\0';
}

static sm_fpkg_status_t read_owned_file(int directory_fd, const char *name,
                                        size_t max_size, char **contents_out,
                                        size_t *size_out) {
  *contents_out = NULL;
  *size_out = 0;
  int fd = openat(directory_fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return errno == ENOENT ? SM_FPKG_UNSUPPORTED : SM_FPKG_IO;
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return SM_FPKG_IO;
  }
  if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
      (st.st_mode & 0022) != 0 || st.st_size < 0 ||
      (uint64_t)st.st_size > max_size) {
    close(fd);
    errno = EPERM;
    return SM_FPKG_CORRUPT;
  }
  size_t size = (size_t)st.st_size;
  char *contents = malloc(size + 1u);
  if (!contents) {
    close(fd);
    return SM_FPKG_NOMEM;
  }
  size_t done = 0;
  while (done < size) {
    ssize_t count = read(fd, contents + done, size - done);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      int saved = count == 0 ? EIO : errno;
      close(fd);
      free(contents);
      errno = saved;
      return SM_FPKG_IO;
    }
    done += (size_t)count;
  }
  if (close(fd) != 0) {
    int saved = errno;
    free(contents);
    errno = saved;
    return SM_FPKG_IO;
  }
  if (memchr(contents, '\0', size) != NULL) {
    free(contents);
    errno = EINVAL;
    return SM_FPKG_CORRUPT;
  }
  contents[size] = '\0';
  *contents_out = contents;
  *size_out = size;
  return SM_FPKG_OK;
}

static bool text_has_line(const char *text, const char *line) {
  size_t line_length = strlen(line);
  const char *cursor = text;
  while (*cursor) {
    const char *end = strchr(cursor, '\n');
    if (!end)
      return false;
    if ((size_t)(end - cursor) == line_length &&
        memcmp(cursor, line, line_length) == 0)
      return true;
    cursor = end + 1;
  }
  return false;
}

static bool text_line_value(const char *text, const char *key, char *value_out,
                            size_t value_capacity) {
  if (!text || !key || !value_out || value_capacity == 0)
    return false;
  value_out[0] = '\0';
  size_t key_length = strlen(key);
  const char *cursor = text;
  bool found = false;
  while (*cursor) {
    const char *end = strchr(cursor, '\n');
    if (!end)
      return false;
    if ((size_t)(end - cursor) >= key_length &&
        memcmp(cursor, key, key_length) == 0) {
      if (found)
        return false;
      size_t length = (size_t)(end - cursor) - key_length;
      if (length == 0 || length >= value_capacity)
        return false;
      memcpy(value_out, cursor + key_length, length);
      value_out[length] = '\0';
      found = true;
    }
    cursor = end + 1;
  }
  return found;
}

static bool parse_u64_decimal(const char *text, uint64_t *value_out) {
  if (!text || !text[0] || !value_out)
    return false;
  uint64_t value = 0;
  for (const char *p = text; *p; ++p) {
    if (*p < '0' || *p > '9' ||
        value > (UINT64_MAX - (uint64_t)(*p - '0')) / 10u)
      return false;
    value = value * 10u + (uint64_t)(*p - '0');
  }
  *value_out = value;
  return true;
}

static bool parse_time_value(const char *text, int64_t *seconds_out,
                             uint32_t *nanoseconds_out) {
  if (!text || !seconds_out || !nanoseconds_out)
    return false;
  const char *dot = strchr(text, '.');
  if (!dot || dot == text || strlen(dot + 1) != 9u)
    return false;
  char seconds[32];
  size_t seconds_length = (size_t)(dot - text);
  if (seconds_length >= sizeof(seconds))
    return false;
  memcpy(seconds, text, seconds_length);
  seconds[seconds_length] = '\0';
  errno = 0;
  char *end = NULL;
  long long parsed_seconds = strtoll(seconds, &end, 10);
  if (errno || !end || *end)
    return false;
  uint64_t nanoseconds;
  if (!parse_u64_decimal(dot + 1, &nanoseconds) || nanoseconds >= 1000000000u)
    return false;
  *seconds_out = (int64_t)parsed_seconds;
  *nanoseconds_out = (uint32_t)nanoseconds;
  return true;
}

static bool read_generation_source_metadata(
    int generation_fd, const char *title_id,
    sm_fpkg_cache_generation_info_t *info_out) {
  char *built = NULL;
  size_t built_size = 0;
  sm_fpkg_status_t status = read_owned_file(
      generation_fd, ".built", SM_FPKG_CACHE_MANIFEST_LIMIT, &built,
      &built_size);
  if (status != SM_FPKG_OK)
    return false;
  (void)built_size;

  char expected_title[32];
  int title_length = snprintf(expected_title, sizeof(expected_title),
                              "title=%s", title_id);
  char source_path[MAX_PATH], fingerprint[32], device[32], inode[32],
      source_size[32], mtime[64], ctime[64];
  bool valid = title_length > 0 &&
      (size_t)title_length < sizeof(expected_title) &&
      text_has_line(built, "SM-FPKG-BUILT-1") &&
      text_has_line(built, expected_title) &&
      text_line_value(built, "source=", source_path, sizeof(source_path)) &&
      valid_text_path(source_path, sizeof(source_path)) &&
      text_line_value(built, "fingerprint=", fingerprint,
                      sizeof(fingerprint)) && strlen(fingerprint) == 16u &&
      text_line_value(built, "device=", device, sizeof(device)) &&
      text_line_value(built, "inode=", inode, sizeof(inode)) &&
      text_line_value(built, "size=", source_size, sizeof(source_size)) &&
      text_line_value(built, "mtime=", mtime, sizeof(mtime)) &&
      text_line_value(built, "ctime=", ctime, sizeof(ctime));
  sm_pkg_info_t metadata = {0};
  uint64_t parsed_device = 0, parsed_inode = 0, parsed_size = 0,
           parsed_ctime = 0;
  int64_t parsed_mtime = 0, parsed_ctime_signed = 0;
  uint32_t parsed_mtime_ns = 0, parsed_ctime_ns = 0;
  if (valid) {
    valid = parse_u64_decimal(device, &parsed_device) &&
            parse_u64_decimal(inode, &parsed_inode) &&
            parse_u64_decimal(source_size, &parsed_size) &&
            parse_time_value(mtime, &parsed_mtime, &parsed_mtime_ns) &&
            parse_time_value(ctime, &parsed_ctime_signed, &parsed_ctime_ns) &&
            parsed_ctime_signed >= 0;
    parsed_ctime = (uint64_t)parsed_ctime_signed;
  }
  if (valid) {
    (void)strlcpy(metadata.title_id, title_id, sizeof(metadata.title_id));
    (void)strlcpy(metadata.path, source_path, sizeof(metadata.path));
    metadata.source_device = parsed_device;
    metadata.source_inode = parsed_inode;
    metadata.source_size = parsed_size;
    metadata.source_ctime = parsed_ctime;
    metadata.source_mtime = (time_t)parsed_mtime;
    metadata.source_mtime_ns = parsed_mtime_ns;
    metadata.source_ctime_ns = parsed_ctime_ns;
    metadata.pkg_kind = PKG_KIND_BASE;
    metadata.pkg_classification_confident = true;
    char expected_fingerprint[17];
    (void)snprintf(expected_fingerprint, sizeof(expected_fingerprint),
                   "%016llx",
                   (unsigned long long)source_fingerprint(&metadata));
    if (strlen(fingerprint) != 16u ||
        strcmp(fingerprint, expected_fingerprint) != 0)
      valid = false;
  }
  if (valid) {
    info_out->source_metadata = metadata;
    char backport[32];
    info_out->backport_fingerprint = 0;
    if (strstr(built, "\nbackport=") != NULL) {
      if (!text_line_value(built, "backport=", backport, sizeof(backport)) ||
          strlen(backport) != 16u) {
        valid = false;
      } else {
        for (size_t i = 0; i < 16u; ++i) {
          if (!((backport[i] >= '0' && backport[i] <= '9') ||
                (backport[i] >= 'a' && backport[i] <= 'f'))) {
            valid = false;
            break;
          }
        }
        errno = 0;
        char *end = NULL;
        unsigned long long value = strtoull(backport, &end, 16);
        if (!valid || errno || !end || *end)
          valid = false;
        else
          info_out->backport_fingerprint = (uint64_t)value;
      }
    }
    (void)strlcpy(info_out->source_path, source_path,
                  sizeof(info_out->source_path));
  }
  free(built);
  return valid;
}

bool sm_fpkg_cache_backport_matches(
    const sm_fpkg_cache_generation_info_t *info, const char *backport_path) {
  if (!info) return false;
  sm_fpkg_backport_t *backport = NULL;
  if (sm_fpkg_backport_open(backport_path, &backport) != SM_FPKG_OK)
    return false;
  bool matches = info->backport_fingerprint ==
                 sm_fpkg_backport_fingerprint(backport);
  sm_fpkg_backport_close(backport);
  return matches;
}

static sm_fpkg_status_t read_generation_identity(
    int title_fd, const char *generation_name, const char *title_id,
    bool require_published, int *generation_fd_out,
    char fingerprint_out[17]) {
  if (generation_fd_out)
    *generation_fd_out = -1;
  if (!valid_generation_name(generation_name))
    return SM_FPKG_CORRUPT;
  int generation_fd = openat(title_fd, generation_name,
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (generation_fd < 0)
    return errno == ENOENT ? SM_FPKG_UNSUPPORTED : SM_FPKG_IO;
  struct stat directory_stat;
  if (fstat(generation_fd, &directory_stat) != 0) {
    close(generation_fd);
    return SM_FPKG_IO;
  }
  if (!S_ISDIR(directory_stat.st_mode) || directory_stat.st_uid != geteuid() ||
      (directory_stat.st_mode & 0022) != 0) {
    close(generation_fd);
    errno = EPERM;
    return SM_FPKG_CORRUPT;
  }

  char *journal = NULL, *done = NULL, *built = NULL;
  size_t journal_size = 0, done_size = 0, built_size = 0;
  sm_fpkg_status_t status = read_owned_file(
      generation_fd, ".journal", 256u, &journal, &journal_size);
  if (status == SM_FPKG_OK)
    status = read_owned_file(generation_fd, ".done", 256u, &done, &done_size);
  if (status == SM_FPKG_OK)
    status = read_owned_file(generation_fd, ".built",
                             SM_FPKG_CACHE_MANIFEST_LIMIT, &built,
                             &built_size);

  char expected_title[32];
  int title_length = snprintf(expected_title, sizeof(expected_title),
                              "title=%s", title_id);
  char *fingerprint_line = NULL;
  if (status == SM_FPKG_OK) {
    if (title_length < 0 || (size_t)title_length >= sizeof(expected_title) ||
        !text_has_line(built, "SM-FPKG-BUILT-1") ||
        !text_has_line(built, expected_title) ||
        !text_has_line(done, "SM-FPKG-DONE-1") ||
        !text_has_line(done, expected_title)) {
      status = SM_FPKG_CORRUPT;
    }
  }
  if (status == SM_FPKG_OK) {
    sm_fpkg_cache_generation_info_t source_identity = {0};
    if (!read_generation_source_metadata(generation_fd, title_id,
                                         &source_identity))
      status = SM_FPKG_CORRUPT;
  }
  if (status == SM_FPKG_OK) {
    char *cursor = built;
    while ((cursor = strstr(cursor, "fingerprint=")) != NULL) {
      if (cursor == built || cursor[-1] == '\n') {
        fingerprint_line = cursor + strlen("fingerprint=");
        break;
      }
      ++cursor;
    }
    if (!fingerprint_line || strlen(fingerprint_line) < 17u ||
        fingerprint_line[16] != '\n') {
      status = SM_FPKG_CORRUPT;
    } else {
      for (size_t i = 0; i < 16u; ++i) {
        char c = fingerprint_line[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
          status = SM_FPKG_CORRUPT;
          break;
        }
      }
    }
  }
  if (status == SM_FPKG_OK) {
    char expected_fingerprint[32];
    int length = snprintf(expected_fingerprint, sizeof(expected_fingerprint),
                          "fingerprint=%.16s", fingerprint_line);
    if (length < 0 || (size_t)length >= sizeof(expected_fingerprint) ||
        !text_has_line(done, expected_fingerprint) ||
        strncmp(generation_name + 4u, fingerprint_line, 16u) != 0 ||
        generation_name[20] != '-') {
      status = SM_FPKG_CORRUPT;
    } else if (fingerprint_out) {
      memcpy(fingerprint_out, fingerprint_line, 16u);
      fingerprint_out[16] = '\0';
    }
  }
  if (status == SM_FPKG_OK && require_published &&
      (!text_has_line(journal, "SM-FPKG-JOURNAL-1") ||
       !text_has_line(journal, "state=published"))) {
    status = SM_FPKG_CORRUPT;
  }
  if (status == SM_FPKG_OK && require_published &&
      !validate_generation_owned_tree(generation_fd, title_id)) {
    status = SM_FPKG_CORRUPT;
  }

  free(journal);
  free(done);
  free(built);
  if (status != SM_FPKG_OK) {
    close(generation_fd);
    return status;
  }
  if (generation_fd_out)
    *generation_fd_out = generation_fd;
  else
    close(generation_fd);
  (void)journal_size;
  (void)done_size;
  (void)built_size;
  return SM_FPKG_OK;
}

/* A crash between linkat publication and unlinking the private temporary
 * leaves two names for the same owned inode. Remove only that exact alias. */
static bool valid_publication_alias(const char *target, const char *name) {
  const char *prefix = !strcmp(target, "active") ? ".active-" : ".active-write-";
  size_t prefix_length = strlen(prefix);
  if (strncmp(name, prefix, prefix_length) != 0)
    return false;
  const char *p = name + prefix_length;
  if (*p < '0' || *p > '9') return false;
  while (*p >= '0' && *p <= '9') ++p;
  if (*p++ != '-' || *p < '0' || *p > '9') return false;
  while (*p >= '0' && *p <= '9') ++p;
  return *p == '\0';
}

static bool recover_publication_alias(int title_fd, const char *target) {
  struct stat published;
  if (fstatat(title_fd, target, &published, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT;
  if (!S_ISREG(published.st_mode) || published.st_uid != geteuid() ||
      (published.st_mode & 0022) != 0) { errno = EPERM; return false; }
  if (published.st_nlink == 1) return true;
  if (published.st_nlink != 2) { errno = EPERM; return false; }
  int iterator = openat(title_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (iterator < 0) return false;
  DIR *dir = fdopendir(iterator);
  if (!dir) { close(iterator); return false; }
  char alias[NAME_MAX + 1] = {0};
  unsigned matches = 0;
  bool ok = true;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) { if (errno) ok = false; break; }
    if (!valid_publication_alias(target, entry->d_name)) continue;
    struct stat candidate;
    if (fstatat(title_fd, entry->d_name, &candidate, AT_SYMLINK_NOFOLLOW) != 0) {
      ok = false; break;
    }
    if (candidate.st_dev != published.st_dev || candidate.st_ino != published.st_ino)
      continue;
    if (!S_ISREG(candidate.st_mode) || candidate.st_uid != geteuid() ||
        candidate.st_nlink != 2 || (candidate.st_mode & 0022) != 0 ||
        strlcpy(alias, entry->d_name, sizeof(alias)) >= sizeof(alias)) {
      ok = false; break;
    }
    ++matches;
  }
  if (closedir(dir) != 0) ok = false;
  struct stat current, temporary;
  if (!ok || matches != 1 ||
      fstatat(title_fd, target, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
      fstatat(title_fd, alias, &temporary, AT_SYMLINK_NOFOLLOW) != 0 ||
      current.st_dev != published.st_dev || current.st_ino != published.st_ino ||
      current.st_nlink != 2 || temporary.st_dev != published.st_dev ||
      temporary.st_ino != published.st_ino || temporary.st_nlink != 2) {
    errno = EPERM; return false;
  }
  return unlinkat(title_fd, alias, 0) == 0 && fsync(title_fd) == 0;
}

static sm_fpkg_status_t read_active_generation(
    int title_fd, const char *title_id, char generation_name_out[112]) {
  generation_name_out[0] = '\0';
  if (!recover_publication_alias(title_fd, "active"))
    return SM_FPKG_CORRUPT;
  char *active = NULL;
  size_t active_size = 0;
  sm_fpkg_status_t status = read_owned_file(
      title_fd, "active", 256u, &active, &active_size);
  if (status == SM_FPKG_UNSUPPORTED && errno == ENOENT)
    return SM_FPKG_OK;
  if (status != SM_FPKG_OK)
    return status;
  const char prefix[] = "SM-FPKG-ACTIVE-1\ngeneration=";
  size_t prefix_size = sizeof(prefix) - 1u;
  if (active_size <= prefix_size + 1u ||
      memcmp(active, prefix, prefix_size) != 0 ||
      active[active_size - 1u] != '\n') {
    free(active);
    errno = EINVAL;
    return SM_FPKG_CORRUPT;
  }
  size_t generation_length = active_size - prefix_size - 1u;
  if (generation_length >= 112u) {
    free(active);
    errno = ENAMETOOLONG;
    return SM_FPKG_CORRUPT;
  }
  memcpy(generation_name_out, active + prefix_size, generation_length);
  generation_name_out[generation_length] = '\0';
  free(active);
  if (!valid_generation_name(generation_name_out))
    return SM_FPKG_CORRUPT;
  return read_generation_identity(title_fd, generation_name_out, title_id,
                                  true, NULL, NULL);
}

static bool source_metadata_matches(const sm_pkg_info_t *expected,
                                    const sm_pkg_info_t *built) {
  return expected && built &&
         strcmp(expected->title_id, built->title_id) == 0 &&
         strcmp(expected->path, built->path) == 0 &&
         source_fingerprint(expected) == source_fingerprint(built);
}

static bool generation_matches_source(int title_fd,
                                      const char *generation_name,
                                      const char *title_id,
                                      const sm_pkg_info_t *source_metadata,
                                      uint64_t backport_fingerprint) {
  int generation_fd = -1;
  if (read_generation_identity(title_fd, generation_name, title_id, true,
                               &generation_fd, NULL) != SM_FPKG_OK)
    return false;
  sm_fpkg_cache_generation_info_t info = {0};
  bool matches = read_generation_source_metadata(generation_fd, title_id,
                                                 &info) &&
                 source_metadata_matches(source_metadata,
                                         &info.source_metadata) &&
                 generation_executable_modes_valid(generation_fd);
  if (matches) {
    char *built = NULL;
    size_t built_size = 0;
    char expected[48], recorded[32];
    (void)snprintf(expected, sizeof(expected), "backport=%016llx",
                   (unsigned long long)backport_fingerprint);
    matches = read_owned_file(generation_fd, ".built",
                              SM_FPKG_CACHE_MANIFEST_LIMIT, &built,
                              &built_size) == SM_FPKG_OK &&
              (text_has_line(built, expected) ||
               (backport_fingerprint == 0 &&
                !text_line_value(built, "backport=", recorded,
                                 sizeof(recorded))));
    free(built);
    (void)built_size;
  }
  close(generation_fd);
  return matches;
}

typedef struct {
  char *path;
  uint64_t size;
  bool seen;
} owned_cache_file_t;

typedef struct {
  owned_cache_file_t *files;
  size_t count;
  size_t capacity;
} owned_cache_manifest_t;

static int compare_owned_cache_file(const void *left, const void *right) {
  const owned_cache_file_t *a = left;
  const owned_cache_file_t *b = right;
  return strcmp(a->path, b->path);
}

static void free_owned_cache_manifest(owned_cache_manifest_t *manifest) {
  if (!manifest)
    return;
  for (size_t i = 0; i < manifest->count; ++i)
    free(manifest->files[i].path);
  free(manifest->files);
  memset(manifest, 0, sizeof(*manifest));
}

static bool add_owned_cache_file(owned_cache_manifest_t *manifest,
                                 const char *path, size_t path_length,
                                 uint64_t size) {
  if (manifest->count >= SM_FPKG_CACHE_OWNED_FILE_LIMIT ||
      path_length == 0 || path_length >= MAX_PATH)
    return false;
  char path_copy[MAX_PATH];
  memcpy(path_copy, path, path_length);
  path_copy[path_length] = '\0';
  if (!safe_relative_path(path_copy))
    return false;
  if (manifest->count == manifest->capacity) {
    size_t capacity = manifest->capacity ? manifest->capacity * 2u : 64u;
    if (capacity > SM_FPKG_CACHE_OWNED_FILE_LIMIT)
      capacity = SM_FPKG_CACHE_OWNED_FILE_LIMIT;
    owned_cache_file_t *grown = realloc(
        manifest->files, capacity * sizeof(*manifest->files));
    if (!grown)
      return false;
    manifest->files = grown;
    manifest->capacity = capacity;
  }
  char *copy = malloc(path_length + 1u);
  if (!copy)
    return false;
  memcpy(copy, path, path_length);
  copy[path_length] = '\0';
  manifest->files[manifest->count++] =
      (owned_cache_file_t){.path = copy, .size = size};
  return true;
}

static sm_fpkg_status_t parse_owned_cache_manifest_named(
    int generation_fd, const char *name, owned_cache_manifest_t *manifest) {
  memset(manifest, 0, sizeof(*manifest));
  char *contents = NULL;
  size_t size = 0;
  sm_fpkg_status_t status = read_owned_file(
      generation_fd, name, SM_FPKG_CACHE_MANIFEST_LIMIT, &contents, &size);
  if (status != SM_FPKG_OK)
    return status;
  char *cursor = contents;
  while (cursor < contents + size) {
    char *end = memchr(cursor, '\n', (size_t)(contents + size - cursor));
    if (!end) {
      status = SM_FPKG_CORRUPT;
      break;
    }
    size_t line_length = (size_t)(end - cursor);
    if (line_length >= 5u && memcmp(cursor, "file=", 5u) == 0) {
      char *number = cursor + 5u;
      char *colon = memchr(number, ':', line_length - 5u);
      if (!colon || colon == number) {
        status = SM_FPKG_CORRUPT;
        break;
      }
      uint64_t file_size = 0;
      for (char *p = number; p < colon; ++p) {
        if (*p < '0' || *p > '9' ||
            file_size > (UINT64_MAX - (uint64_t)(*p - '0')) / 10u) {
          status = SM_FPKG_CORRUPT;
          break;
        }
        file_size = file_size * 10u + (uint64_t)(*p - '0');
      }
      if (status != SM_FPKG_OK)
        break;
      const char *path = colon + 1;
      size_t path_length = (size_t)(end - path);
      if (!add_owned_cache_file(manifest, path, path_length, file_size)) {
        status = SM_FPKG_CORRUPT;
        break;
      }
    }
    cursor = end + 1;
  }
  free(contents);
  if (status != SM_FPKG_OK) {
    free_owned_cache_manifest(manifest);
    return status;
  }
  qsort(manifest->files, manifest->count, sizeof(*manifest->files),
        compare_owned_cache_file);
  for (size_t i = 1; i < manifest->count; ++i) {
    if (strcmp(manifest->files[i - 1u].path, manifest->files[i].path) == 0) {
      free_owned_cache_manifest(manifest);
      return SM_FPKG_CORRUPT;
    }
  }
  if (!manifest->count) {
    free_owned_cache_manifest(manifest);
    return SM_FPKG_CORRUPT;
  }
  return SM_FPKG_OK;
}

static sm_fpkg_status_t parse_owned_cache_manifest(
    int generation_fd, owned_cache_manifest_t *manifest) {
  return parse_owned_cache_manifest_named(generation_fd, ".built", manifest);
}

static bool generation_executable_modes_valid(int generation_fd) {
  owned_cache_manifest_t manifest;
  if (parse_owned_cache_manifest(generation_fd, &manifest) != SM_FPKG_OK)
    return false;
  bool valid = true;
  for (size_t i = 0; valid && i < manifest.count; ++i) {
    if (!is_executable_file(manifest.files[i].path))
      continue;
    int parent_fd = -1;
    char leaf[NAME_MAX + 1];
    struct stat st = {0};
    if (!relative_parent(generation_fd, manifest.files[i].path, false,
                         &parent_fd, leaf)) {
      valid = false;
      break;
    }
    valid = fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(st.st_mode) && (st.st_mode & 0111) == 0111;
    close(parent_fd);
  }
  free_owned_cache_manifest(&manifest);
  return valid;
}

static owned_cache_file_t *find_owned_cache_file(
    owned_cache_manifest_t *manifest, const char *path) {
  size_t left = 0, right = manifest->count;
  while (left < right) {
    size_t middle = left + (right - left) / 2u;
    int order = strcmp(manifest->files[middle].path, path);
    if (order == 0)
      return &manifest->files[middle];
    if (order < 0)
      left = middle + 1u;
    else
      right = middle;
  }
  return NULL;
}

static bool manifest_has_child_directory(
    const owned_cache_manifest_t *manifest, const char *directory) {
  size_t length = strlen(directory);
  for (size_t i = 0; i < manifest->count; ++i) {
    if (strncmp(manifest->files[i].path, directory, length) == 0 &&
        manifest->files[i].path[length] == '/')
      return true;
  }
  return false;
}

typedef struct {
  owned_cache_manifest_t *manifest;
  bool built_seen;
  bool done_seen;
  bool journal_seen;
  bool kraken_seen;
} owned_tree_check_t;

static bool validate_owned_cache_tree(int directory_fd, const char *prefix,
                                      unsigned int depth,
                                      owned_tree_check_t *check) {
  /* MAX_PATH bounds every owned relative path; even one-character names and
   * separators cannot produce more than MAX_PATH / 2 nested directories. */
  if (depth > MAX_PATH / 2u)
    return false;
  int iterator_fd = openat(directory_fd, ".",
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (iterator_fd < 0)
    return false;
  DIR *directory = fdopendir(iterator_fd);
  if (!directory) {
    close(iterator_fd);
    return false;
  }
  char *path = malloc(MAX_PATH);
  if (!path) {
    closedir(directory);
    return false;
  }
  bool valid = true;
  struct dirent *entry;
  while (valid && (entry = readdir(directory)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    int length = prefix[0]
                     ? snprintf(path, MAX_PATH, "%s/%s", prefix,
                                entry->d_name)
                     : snprintf(path, MAX_PATH, "%s", entry->d_name);
    if (length < 0 || (size_t)length >= MAX_PATH) {
      valid = false;
      break;
    }
    struct stat st;
    if (fstatat(directory_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      valid = false;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      if (st.st_uid != geteuid() || (st.st_mode & 0022) != 0 ||
          (strcmp(path, "kraken") != 0 &&
           !manifest_has_child_directory(check->manifest, path))) {
        valid = false;
        break;
      }
      if (strcmp(path, "kraken") == 0)
        check->kraken_seen = true;
      int child_fd = openat(directory_fd, entry->d_name,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (child_fd < 0) {
        valid = false;
        break;
      }
      valid = validate_owned_cache_tree(child_fd, path, depth + 1u, check);
      close(child_fd);
      continue;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
        (st.st_mode & 0022) != 0) {
      valid = false;
      break;
    }
    if (!prefix[0] && !strcmp(entry->d_name, ".built")) {
      check->built_seen = true;
      continue;
    }
    if (!prefix[0] && !strcmp(entry->d_name, ".done")) {
      check->done_seen = true;
      continue;
    }
    if (!prefix[0] && !strcmp(entry->d_name, ".journal")) {
      check->journal_seen = true;
      continue;
    }
    owned_cache_file_t *owned = find_owned_cache_file(check->manifest, path);
    if (!owned || owned->seen || st.st_size < 0 ||
        (uint64_t)st.st_size != owned->size) {
      valid = false;
      break;
    }
    owned->seen = true;
  }
  closedir(directory);
  free(path);
  return valid;
}

static bool validate_generation_owned_tree(int generation_fd,
                                           const char *title_id) {
  owned_cache_manifest_t manifest;
  if (parse_owned_cache_manifest(generation_fd, &manifest) != SM_FPKG_OK)
    return false;
  sm_fpkg_cache_generation_info_t source = {0};
  bool source_valid =
      read_generation_source_metadata(generation_fd, title_id, &source);
  bool image_valid = false;
  if (source_valid) {
    const char *leaf = strrchr(source.source_path, '/');
    leaf = leaf ? leaf + 1 : source.source_path;
    char expected_image[MAX_PATH + 8u];
    int expected_length = snprintf(expected_image, sizeof(expected_image),
                                   "kraken/%s\n", leaf);
    if (expected_length > 0 &&
        (size_t)expected_length < sizeof(expected_image)) {
      char *image = NULL;
      size_t image_size = 0;
      if (read_owned_file(generation_fd, "kraken.image",
                          sizeof(expected_image) - 1u, &image,
                          &image_size) == SM_FPKG_OK) {
        image_valid = image_size == (size_t)expected_length &&
                      memcmp(image, expected_image, image_size) == 0;
        free(image);
      }
    }
  }
  int index_fd = openat(generation_fd, "ampr_emu.index",
                        O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat index_st;
  bool index_valid = index_fd >= 0 && fstat(index_fd, &index_st) == 0 &&
                     S_ISREG(index_st.st_mode) &&
                     index_st.st_uid == geteuid() && index_st.st_nlink == 1 &&
                     (index_st.st_mode & 0022) == 0;
  int manifest_fd = openat(generation_fd, "ampr_assets.index",
                           O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat manifest_st;
  bool manifest_valid = manifest_fd >= 0 &&
                        fstat(manifest_fd, &manifest_st) == 0 &&
                        S_ISREG(manifest_st.st_mode) &&
                        manifest_st.st_uid == geteuid() &&
                        manifest_st.st_nlink == 1 &&
                        (manifest_st.st_mode & 0022) == 0;
  bool indexes_valid = index_valid && manifest_valid &&
                       sm_fpkg_index_validate_pair_fds(index_fd,
                                                       manifest_fd);
  if (index_fd >= 0)
    close(index_fd);
  if (manifest_fd >= 0)
    close(manifest_fd);
  owned_tree_check_t check = {.manifest = &manifest};
  bool valid = source_valid && image_valid && indexes_valid &&
               validate_owned_cache_tree(generation_fd, "", 0u, &check) &&
               check.built_seen && check.done_seen && check.journal_seen &&
               check.kraken_seen &&
               find_owned_cache_file(&manifest, "ampr_emu.index") &&
               find_owned_cache_file(&manifest, "ampr_assets.index") &&
               find_owned_cache_file(&manifest, "kraken.image");
  for (size_t i = 0; valid && i < manifest.count; ++i)
    valid = manifest.files[i].seen;
  free_owned_cache_manifest(&manifest);
  return valid;
}

static bool safe_cache_directory_fd(int fd) {
  struct stat st;
  if (fstat(fd, &st) != 0)
    return false;
  return S_ISDIR(st.st_mode) && st.st_uid == geteuid() &&
         (st.st_mode & 0022) == 0;
}

static int open_title_lock(int title_fd, bool create) {
  int flags = O_RDWR | O_NOFOLLOW | O_CLOEXEC;
  if (create)
    flags |= O_CREAT;
  int fd = openat(title_fd, ".lock", flags, 0600);
  if (fd < 0)
    return -1;
  struct stat st;
  int stat_result = fstat(fd, &st);
  if (stat_result != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid() || st.st_nlink != 1 ||
      (st.st_mode & 0077) != 0) {
    int saved = stat_result != 0 ? errno : EPERM;
    close(fd);
    errno = saved;
    return -1;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  return fd;
}

static bool normalize_cache_root(const char *cache_root,
                                 char normalized[MAX_PATH]) {
  if (!valid_text_path(cache_root, MAX_PATH))
    return false;
  (void)strlcpy(normalized, cache_root, MAX_PATH);
  size_t length = strlen(normalized);
  while (length > 1u && normalized[length - 1u] == '/')
    normalized[--length] = '\0';
  return true;
}

typedef struct {
  const char **paths;
  size_t count;
} writable_conflicts_t;

static int compare_writable_paths(const void *a, const void *b) {
  return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static bool writable_conflict(const writable_conflicts_t *conflicts,
                              const char *path, bool check_descendants) {
  if (is_reserved_cache_path(path))
    return true;
  size_t left = 0, right = conflicts->count;
  while (left < right) {
    size_t mid = left + (right - left) / 2u;
    if (strcasecmp(conflicts->paths[mid], path) < 0)
      left = mid + 1u;
    else
      right = mid;
  }
  if (left == conflicts->count)
    return false;
  if (!strcasecmp(conflicts->paths[left], path))
    return true;
  size_t length = strlen(path);
  return check_descendants &&
         !strncasecmp(conflicts->paths[left], path, length) &&
         conflicts->paths[left][length] == '/';
}

static bool same_writable_stat(const struct stat *a, const struct stat *b) {
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
         a->st_mode == b->st_mode && a->st_size == b->st_size &&
         a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
         a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
         a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
         a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

typedef struct {
  uint64_t xor_hash, sum_hash, count;
} writable_signature_t;

static void writable_hash_bytes(uint64_t *hash, const void *bytes, size_t size) {
  const unsigned char *p = bytes;
  for (size_t i = 0; i < size; ++i) {
    *hash ^= p[i];
    *hash *= UINT64_C(1099511628211);
  }
}

static bool fingerprint_writable_tree(int source, const char *prefix,
                                      unsigned depth, writable_signature_t *signature) {
  if (depth > MAX_PATH / 2u) { errno = ELOOP; return false; }
  struct stat before, after;
  if (fstat(source, &before) != 0 || !S_ISDIR(before.st_mode)) return false;
  int iterator = openat(source, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (iterator < 0) return false;
  DIR *dir = fdopendir(iterator);
  if (!dir) { close(iterator); return false; }
  bool ok = true;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) { if (errno) ok = false; break; }
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char path[MAX_PATH];
    int length = snprintf(path, sizeof(path), "%s%s%s", prefix,
                          prefix[0] ? "/" : "", entry->d_name);
    struct stat st = {0};
    if (length < 0 || (size_t)length >= sizeof(path) ||
        !safe_relative_path(path) ||
        fstatat(source, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      ok = false; break;
    }
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
      errno = ENOTSUP; ok = false; break;
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    writable_hash_bytes(&hash, path, (size_t)length + 1u);
    const uint64_t fields[] = {(uint64_t)st.st_dev, (uint64_t)st.st_ino,
      (uint64_t)st.st_mode, (uint64_t)st.st_size,
      (uint64_t)st.st_mtim.tv_sec, (uint64_t)st.st_mtim.tv_nsec,
      (uint64_t)st.st_ctim.tv_sec, (uint64_t)st.st_ctim.tv_nsec};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
      uint64_t value = fields[i];
      for (unsigned byte = 0; byte < 8u; ++byte) {
        hash ^= (uint8_t)value;
        hash *= UINT64_C(1099511628211);
        value >>= 8;
      }
    }
    signature->xor_hash ^= hash;
    signature->sum_hash += hash;
    ++signature->count;
    if (S_ISDIR(st.st_mode)) {
      int child = openat(source, entry->d_name,
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      struct stat opened;
      ok = child >= 0 && fstat(child, &opened) == 0 &&
           same_writable_stat(&st, &opened) &&
           fingerprint_writable_tree(child, path, depth + 1u, signature);
      if (child >= 0) close(child);
      if (!ok) break;
    }
  }
  if (closedir(dir) != 0) ok = false;
  return ok && fstat(source, &after) == 0 && same_writable_stat(&before, &after);
}

/* Copy only game-created entries. A conflicting file remains in the previous
 * upper as a retained backup; the new generation's file wins in the sandbox. */
static bool copy_writable_tree(int source, int destination, const char *prefix,
                               unsigned depth,
                               const writable_conflicts_t *conflicts,
                               char *buffer) {
  if (depth > MAX_PATH / 2u) { errno = ELOOP; return false; }
  struct stat before, after;
  if (fstat(source, &before) != 0 || !S_ISDIR(before.st_mode))
    return false;
  int iterator = openat(source, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (iterator < 0) return false;
  DIR *dir = fdopendir(iterator);
  if (!dir) { close(iterator); return false; }
  bool ok = true;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) { if (errno) ok = false; break; }
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char path[MAX_PATH];
    int length = snprintf(path, sizeof(path), "%s%s%s", prefix,
                          prefix[0] ? "/" : "", entry->d_name);
    if (length < 0 || (size_t)length >= sizeof(path) ||
        !safe_relative_path(path)) { errno = ENAMETOOLONG; ok = false; break; }
    struct stat st;
    if (fstatat(source, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      ok = false; break;
    }
    bool is_dir = S_ISDIR(st.st_mode);
    if (writable_conflict(conflicts, path, !is_dir)) continue;
    if (is_dir) {
      int src = openat(source, entry->d_name,
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (src < 0 || mkdirat(destination, entry->d_name, 0700) != 0) {
        if (src >= 0) close(src);
        ok = false; break;
      }
      int dst = openat(destination, entry->d_name,
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      struct stat opened;
      struct timespec times[2] = {{.tv_nsec = UTIME_OMIT}, st.st_mtim};
      ok = dst >= 0 && fstat(src, &opened) == 0 && same_writable_stat(&st, &opened) &&
           copy_writable_tree(src, dst, path, depth + 1u, conflicts, buffer) &&
           fchmod(dst, st.st_mode & 0777) == 0 && futimens(dst, times) == 0 &&
           fsync(dst) == 0;
      if (dst >= 0) close(dst);
      close(src);
    } else if (S_ISREG(st.st_mode)) {
      int src = openat(source, entry->d_name,
                       O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
      int dst = openat(destination, entry->d_name,
                       O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      struct stat opened;
      struct timespec times[2] = {{.tv_nsec = UTIME_OMIT}, st.st_mtim};
      ok = src >= 0 && dst >= 0 && fstat(src, &opened) == 0 &&
           same_writable_stat(&st, &opened);
      while (ok) {
        ssize_t count = read(src, buffer, 65536u);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { ok = false; break; }
        if (count == 0) break;
        ok = write_all(dst, buffer, (size_t)count) == SM_FPKG_OK;
      }
      ok = ok && fstat(src, &opened) == 0 && same_writable_stat(&st, &opened) &&
           fchmod(dst, st.st_mode & 0777) == 0 && futimens(dst, times) == 0 &&
           fsync(dst) == 0;
      if (src >= 0) close(src);
      if (dst >= 0 && close(dst) != 0) ok = false;
    } else if (S_ISLNK(st.st_mode)) {
      char target[MAX_PATH];
      ssize_t count = readlinkat(source, entry->d_name, target, sizeof(target) - 1u);
      if (count < 0 || (size_t)count >= sizeof(target) - 1u) { ok = false; break; }
      target[count] = '\0';
      ok = symlinkat(target, destination, entry->d_name) == 0;
    } else {
      errno = ENOTSUP;
      ok = false;
    }
    if (!ok) break;
  }
  if (closedir(dir) != 0) ok = false;
  if (ok && (fstat(source, &after) != 0 || !same_writable_stat(&before, &after)))
    ok = false;
  return ok;
}

static bool valid_writable_name(const char *name) {
  if (!name || strncmp(name, "write-", 6u) != 0 || !name[6] ||
      strlen(name) >= 96u) return false;
  for (const char *p = name + 6; *p; ++p)
    if (!((*p >= '0' && *p <= '9') || *p == '-')) return false;
  return true;
}

sm_fpkg_status_t sm_fpkg_cache_prepare_writable(
    const char *cache_root, const char *title_id, const char *generation_path,
    char writable_path_out[MAX_PATH]) {
  if (writable_path_out)
    writable_path_out[0] = '\0';
  char root[MAX_PATH];
  if (!writable_path_out || !generation_path || !valid_title_id(title_id) ||
      !normalize_cache_root(cache_root, root))
    return SM_FPKG_CORRUPT;
  char prefix[MAX_PATH];
  int length = snprintf(prefix, sizeof(prefix), "%s/%s/", root, title_id);
  if (length < 0 || (size_t)length >= sizeof(prefix) ||
      strncmp(generation_path, prefix, (size_t)length) != 0 ||
      !valid_generation_name(generation_path + length))
    return SM_FPKG_CORRUPT;
  const char *generation = generation_path + length;
  int root_fd = open_directory_path(root, false);
  if (root_fd < 0) {
    writable_path_out[0] = '\0';
    return SM_FPKG_IO;
  }
  int title_fd = -1, lock_fd = -1, writable_fd = -1, generation_fd = -1;
  int source_fd = -1;
  owned_cache_manifest_t old_manifest = {0}, new_manifest = {0};
  writable_conflicts_t conflicts = {0};
  char *pointer = NULL, *built = NULL;
  char *copy_buffer = NULL;
  size_t pointer_size = 0, built_size = 0;
  sm_fpkg_status_t status = SM_FPKG_IO;
  if (!safe_cache_directory_fd(root_fd)) {
    status = SM_FPKG_CORRUPT;
    goto done_writable;
  }
  title_fd = openat(root_fd, title_id,
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (title_fd < 0)
    goto done_writable;
  if (!safe_cache_directory_fd(title_fd)) {
    status = SM_FPKG_CORRUPT;
    goto done_writable;
  }
  lock_fd = open_title_lock(title_fd, false);
  if (lock_fd < 0)
    goto done_writable;
  if (!recover_publication_alias(title_fd, ".writable-active")) {
    status = SM_FPKG_CORRUPT; goto done_writable;
  }
  status = read_generation_identity(title_fd, generation, title_id, true,
                                    &generation_fd, NULL);
  if (status != SM_FPKG_OK) goto done_writable;
  status = parse_owned_cache_manifest(generation_fd, &new_manifest);
  if (status != SM_FPKG_OK) goto done_writable;
  status = read_owned_file(generation_fd, ".built", SM_FPKG_CACHE_MANIFEST_LIMIT,
                           &built, &built_size);
  if (status != SM_FPKG_OK) goto done_writable;
  struct stat before_pointer = {0};
  int pointer_stat_result = fstatat(title_fd, ".writable-active", &before_pointer,
                                    AT_SYMLINK_NOFOLLOW);
  if (pointer_stat_result != 0 && errno != ENOENT) {
    status = SM_FPKG_IO; goto done_writable;
  }
  status = read_owned_file(title_fd, ".writable-active", 512u,
                           &pointer, &pointer_size);
  bool had_pointer = status == SM_FPKG_OK;
  struct stat pointer_identity = {0};
  if (!had_pointer && !(status == SM_FPKG_UNSUPPORTED && errno == ENOENT))
    goto done_writable;
  char old_generation[112] = {0}, old_upper[96] = {0}, old_meta[112] = {0};
  if (had_pointer) {
    if (pointer_stat_result != 0 ||
        fstatat(title_fd, ".writable-active", &pointer_identity,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_writable_stat(&before_pointer, &pointer_identity) ||
        !S_ISREG(pointer_identity.st_mode) ||
        pointer_identity.st_uid != geteuid() || pointer_identity.st_nlink != 1 ||
        (pointer_identity.st_mode & 0022) != 0) {
      status = SM_FPKG_CORRUPT; goto done_writable;
    }
    if (!text_has_line(pointer, "SM-FPKG-WRITABLE-1") ||
        !text_line_value(pointer, "generation=", old_generation, sizeof(old_generation)) ||
        !valid_generation_name(old_generation) ||
        !text_line_value(pointer, "upper=", old_upper, sizeof(old_upper)) ||
        !valid_writable_name(old_upper)) {
      status = SM_FPKG_CORRUPT; goto done_writable;
    }
    int meta_length = snprintf(old_meta, sizeof(old_meta), ".meta-%s", old_upper);
    if (meta_length < 0 || (size_t)meta_length >= sizeof(old_meta)) {
      status = SM_FPKG_CORRUPT; goto done_writable;
    }
    status = parse_owned_cache_manifest_named(title_fd, old_meta, &old_manifest);
    if (status != SM_FPKG_OK) goto done_writable;
    source_fd = openat(title_fd, old_upper,
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (source_fd < 0 || !safe_cache_directory_fd(source_fd)) {
      status = SM_FPKG_CORRUPT; goto done_writable;
    }
    if (!strcmp(old_generation, generation)) {
      char *old_built = NULL;
      size_t old_built_size = 0;
      status = read_owned_file(title_fd, old_meta, SM_FPKG_CACHE_MANIFEST_LIMIT,
                               &old_built, &old_built_size);
      bool same = status == SM_FPKG_OK && old_built_size == built_size &&
                  !memcmp(old_built, built, built_size);
      free(old_built);
      if (!same) { status = SM_FPKG_CORRUPT; goto done_writable; }
      length = snprintf(writable_path_out, MAX_PATH, "%s/%s/%s", root, title_id, old_upper);
      status = length > 0 && length < MAX_PATH ? SM_FPKG_OK : SM_FPKG_UNSUPPORTED;
      goto done_writable;
    }
  } else {
    if (pointer_stat_result == 0) { status = SM_FPKG_CORRUPT; goto done_writable; }
    source_fd = openat(title_fd, "writable",
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (source_fd < 0 && errno != ENOENT) { status = SM_FPKG_CORRUPT; goto done_writable; }
    if (source_fd >= 0 && !safe_cache_directory_fd(source_fd)) {
      status = SM_FPKG_CORRUPT; goto done_writable;
    }
  }
  conflicts.count = old_manifest.count + new_manifest.count;
  conflicts.paths = calloc(conflicts.count ? conflicts.count : 1u, sizeof(*conflicts.paths));
  if (!conflicts.paths) { status = SM_FPKG_NOMEM; goto done_writable; }
  for (size_t i = 0; i < old_manifest.count; ++i)
    conflicts.paths[i] = old_manifest.files[i].path;
  for (size_t i = 0; i < new_manifest.count; ++i)
    conflicts.paths[old_manifest.count + i] = new_manifest.files[i].path;
  qsort(conflicts.paths, conflicts.count, sizeof(*conflicts.paths), compare_writable_paths);

  uint64_t serial = atomic_fetch_add_explicit(&g_cache_generation_counter, 1u,
                                               memory_order_relaxed);
  char upper[96], stage[112], meta[112], temporary[112], contents[320];
  (void)snprintf(upper, sizeof(upper), "write-%ld-%llu", (long)getpid(),
                 (unsigned long long)serial);
  (void)snprintf(stage, sizeof(stage), ".stage-%s", upper);
  (void)snprintf(meta, sizeof(meta), ".meta-%s", upper);
  (void)snprintf(temporary, sizeof(temporary), ".active-%s", upper);
  length = snprintf(writable_path_out, MAX_PATH, "%s/%s/%s", root, title_id, upper);
  if (length < 0 || length >= MAX_PATH) { status = SM_FPKG_UNSUPPORTED; goto done_writable; }
  if (mkdirat(title_fd, stage, 0700) != 0) { status = SM_FPKG_IO; goto done_writable; }
  writable_fd = openat(title_fd, stage, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (source_fd >= 0) {
    copy_buffer = malloc(65536u);
    if (!copy_buffer) { status = SM_FPKG_NOMEM; goto done_writable; }
  }
  writable_signature_t before = {0}, after = {0};
  if (writable_fd < 0 || (source_fd >= 0 &&
      (!fingerprint_writable_tree(source_fd, "", 0, &before) ||
       !copy_writable_tree(source_fd, writable_fd, "", 0, &conflicts, copy_buffer) ||
       !fingerprint_writable_tree(source_fd, "", 0, &after) ||
       before.xor_hash != after.xor_hash || before.sum_hash != after.sum_hash ||
       before.count != after.count)) || fsync(writable_fd) != 0) {
    status = SM_FPKG_IO; goto done_writable;
  }
  status = write_new_file(title_fd, meta, built, built_size, 0600);
  if (status != SM_FPKG_OK) goto done_writable;
  if (rename_noreplace(title_fd, stage, title_fd, upper) != 0 || fsync(title_fd) != 0) {
    status = SM_FPKG_IO; goto done_writable;
  }
  length = snprintf(contents, sizeof(contents),
                    "SM-FPKG-WRITABLE-1\ngeneration=%s\nupper=%s\n",
                    generation, upper);
  if (length < 0 || (size_t)length >= sizeof(contents)) {
    status = SM_FPKG_UNSUPPORTED; goto done_writable;
  }
  status = write_new_file(title_fd, temporary, contents, (size_t)length, 0600);
  if (status != SM_FPKG_OK) goto done_writable;
  if (had_pointer) {
    struct stat current_pointer;
    if (fstatat(title_fd, ".writable-active", &current_pointer,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_writable_stat(&pointer_identity, &current_pointer)) {
      errno = ESTALE;
      status = SM_FPKG_CORRUPT; goto done_writable;
    }
  }
  int renamed = had_pointer ? renameat(title_fd, temporary, title_fd, ".writable-active")
                            : rename_noreplace(title_fd, temporary, title_fd, ".writable-active");
  if (renamed != 0 || fsync(title_fd) != 0) { status = SM_FPKG_IO; goto done_writable; }
  status = SM_FPKG_OK;
done_writable:
  free(pointer);
  free(built);
  free(copy_buffer);
  free(conflicts.paths);
  free_owned_cache_manifest(&old_manifest);
  free_owned_cache_manifest(&new_manifest);
  if (source_fd >= 0) close(source_fd);
  if (generation_fd >= 0) close(generation_fd);
  if (writable_fd >= 0)
    close(writable_fd);
  if (lock_fd >= 0) {
    (void)flock(lock_fd, LOCK_UN);
    close(lock_fd);
  }
  if (title_fd >= 0)
    close(title_fd);
  close(root_fd);
  if (status != SM_FPKG_OK)
    writable_path_out[0] = '\0';
  return status;
}

static bool remove_validated_owned_tree(int directory_fd, const char *prefix,
                                        unsigned int depth,
                                        owned_cache_manifest_t *manifest) {
  if (depth > MAX_PATH / 2u)
    return false;
  int iterator_fd = openat(directory_fd, ".",
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (iterator_fd < 0)
    return false;
  DIR *directory = fdopendir(iterator_fd);
  if (!directory) {
    close(iterator_fd);
    return false;
  }
  char *path = malloc(MAX_PATH);
  if (!path) {
    closedir(directory);
    return false;
  }
  bool removed = true;
  struct dirent *entry;
  while (removed && (entry = readdir(directory)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    int length = prefix[0]
                     ? snprintf(path, MAX_PATH, "%s/%s", prefix,
                                entry->d_name)
                     : snprintf(path, MAX_PATH, "%s", entry->d_name);
    if (length < 0 || (size_t)length >= MAX_PATH) {
      removed = false;
      break;
    }
    struct stat st;
    if (fstatat(directory_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      removed = false;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      if (st.st_uid != geteuid() || (st.st_mode & 0022) != 0 ||
          (strcmp(path, "kraken") != 0 &&
           !manifest_has_child_directory(manifest, path))) {
        removed = false;
        break;
      }
      int child_fd = openat(directory_fd, entry->d_name,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (child_fd < 0) {
        removed = false;
        break;
      }
      removed = remove_validated_owned_tree(child_fd, path, depth + 1u,
                                            manifest);
      close(child_fd);
      if (removed && unlinkat(directory_fd, entry->d_name, AT_REMOVEDIR) != 0)
        removed = false;
      continue;
    }
    bool marker = !prefix[0] &&
                  (!strcmp(entry->d_name, ".built") ||
                   !strcmp(entry->d_name, ".done") ||
                   !strcmp(entry->d_name, ".journal"));
    owned_cache_file_t *owned = find_owned_cache_file(manifest, path);
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
        (st.st_mode & 0022) != 0 ||
        (!marker && (!owned || st.st_size < 0 ||
                     (uint64_t)st.st_size != owned->size))) {
      removed = false;
      break;
    }
    if (unlinkat(directory_fd, entry->d_name, 0) != 0)
      removed = false;
  }
  closedir(directory);
  free(path);
  return removed;
}

sm_fpkg_status_t sm_fpkg_cache_recover_title(
    const char *cache_root, const char *title_id,
    sm_fpkg_cache_recovery_report_t *report_out) {
  if (!report_out || !valid_title_id(title_id))
    return SM_FPKG_CORRUPT;
  memset(report_out, 0, sizeof(*report_out));
  char normalized_root[MAX_PATH];
  if (!normalize_cache_root(cache_root, normalized_root))
    return SM_FPKG_CORRUPT;
  int root_fd = open_directory_path(normalized_root, false);
  if (root_fd < 0)
    return errno == ENOENT ? SM_FPKG_OK : SM_FPKG_IO;
  if (!safe_cache_directory_fd(root_fd)) {
    close(root_fd);
    return SM_FPKG_CORRUPT;
  }
  int title_fd = openat(root_fd, title_id,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (title_fd < 0) {
    int saved = errno;
    close(root_fd);
    return saved == ENOENT ? SM_FPKG_OK : SM_FPKG_IO;
  }
  if (!safe_cache_directory_fd(title_fd)) {
    close(title_fd);
    close(root_fd);
    return SM_FPKG_CORRUPT;
  }
  int lock_fd = open_title_lock(title_fd, false);
  if (lock_fd < 0) {
    int saved = errno;
    close(title_fd);
    close(root_fd);
    return saved == ENOENT ? SM_FPKG_OK : SM_FPKG_IO;
  }

  char active_generation[112];
  sm_fpkg_status_t status = read_active_generation(
      title_fd, title_id, active_generation);
  if (status == SM_FPKG_OK && active_generation[0]) {
    int length = snprintf(report_out->active_generation,
                          sizeof(report_out->active_generation), "%s/%s/%s",
                          normalized_root, title_id, active_generation);
    if (length < 0 ||
        (size_t)length >= sizeof(report_out->active_generation))
      status = SM_FPKG_UNSUPPORTED;
  }
  if (status == SM_FPKG_OK) {
    int iterator_fd = openat(title_fd, ".",
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (iterator_fd < 0) {
      status = SM_FPKG_IO;
    } else {
      DIR *directory = fdopendir(iterator_fd);
      if (!directory) {
        close(iterator_fd);
        status = SM_FPKG_IO;
      } else {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
          if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
          if (strncmp(entry->d_name, ".stage-", 7u) == 0)
            ++report_out->staging_generations;
          else if (strncmp(entry->d_name, ".quarantine-", 12u) == 0)
            ++report_out->quarantined_generations;
          else if (strncmp(entry->d_name, "gen-", 4u) == 0 &&
                   strcmp(entry->d_name, active_generation) != 0) {
            int generation_fd = openat(
                title_fd, entry->d_name,
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            char *journal = NULL;
            size_t journal_size = 0;
            sm_fpkg_status_t journal_status = generation_fd >= 0
                ? read_owned_file(generation_fd, ".journal", 256u, &journal,
                                  &journal_size)
                : SM_FPKG_IO;
            bool still_staging = journal_status != SM_FPKG_OK ||
                                 text_has_line(journal, "state=staging") ||
                                 text_has_line(journal, "state=verified");
            if (still_staging)
              ++report_out->staging_generations;
            else
              ++report_out->orphaned_generations;
            free(journal);
            if (generation_fd >= 0)
              close(generation_fd);
            (void)journal_size;
          }
        }
        closedir(directory);
      }
    }
  }

  (void)flock(lock_fd, LOCK_UN);
  close(lock_fd);
  close(title_fd);
  close(root_fd);
  return status;
}

sm_fpkg_status_t sm_fpkg_cache_find_current_generation_with_backport(
    const char *cache_root, const sm_pkg_info_t *source_metadata,
    const char *backport_path, char generation_path_out[MAX_PATH]) {
  if (generation_path_out)
    generation_path_out[0] = '\0';
  if (!generation_path_out || !source_metadata ||
      !valid_title_id(source_metadata->title_id) ||
      !valid_text_path(source_metadata->path, MAX_PATH))
    return SM_FPKG_CORRUPT;
  sm_fpkg_backport_t *backport = NULL;
  sm_fpkg_status_t snapshot_status = sm_fpkg_backport_open(backport_path, &backport);
  if (snapshot_status != SM_FPKG_OK)
    return snapshot_status;
  uint64_t backport_fingerprint = sm_fpkg_backport_fingerprint(backport);
  sm_fpkg_backport_close(backport);
  char normalized_root[MAX_PATH];
  if (!normalize_cache_root(cache_root, normalized_root))
    return SM_FPKG_CORRUPT;
  int root_fd = open_directory_path(normalized_root, false);
  if (root_fd < 0)
    return errno == ENOENT ? SM_FPKG_UNSUPPORTED : SM_FPKG_IO;
  if (!safe_cache_directory_fd(root_fd)) {
    close(root_fd);
    return SM_FPKG_CORRUPT;
  }
  int title_fd = openat(root_fd, source_metadata->title_id,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (title_fd < 0) {
    int saved = errno;
    close(root_fd);
    return saved == ENOENT ? SM_FPKG_UNSUPPORTED : SM_FPKG_IO;
  }
  if (!safe_cache_directory_fd(title_fd)) {
    close(title_fd);
    close(root_fd);
    return SM_FPKG_CORRUPT;
  }
  int lock_fd = open_title_lock(title_fd, false);
  if (lock_fd < 0) {
    int saved = errno;
    close(title_fd);
    close(root_fd);
    return saved == ENOENT ? SM_FPKG_UNSUPPORTED : SM_FPKG_IO;
  }

  char generation_name[112];
  sm_fpkg_status_t status = read_active_generation(
      title_fd, source_metadata->title_id, generation_name);
  if (status == SM_FPKG_OK && generation_name[0] == '\0')
    status = SM_FPKG_UNSUPPORTED;
  if (status == SM_FPKG_OK &&
      !generation_matches_source(title_fd, generation_name,
                                 source_metadata->title_id,
                                 source_metadata, backport_fingerprint))
    status = SM_FPKG_UNSUPPORTED;
  if (status == SM_FPKG_OK) {
    int length = snprintf(generation_path_out, MAX_PATH, "%s/%s/%s",
                          normalized_root, source_metadata->title_id,
                          generation_name);
    if (length < 0 || length >= MAX_PATH) {
      generation_path_out[0] = '\0';
      status = SM_FPKG_UNSUPPORTED;
    }
  }

  (void)flock(lock_fd, LOCK_UN);
  close(lock_fd);
  close(title_fd);
  close(root_fd);
  return status;
}

sm_fpkg_status_t sm_fpkg_cache_find_current_generation(
    const char *cache_root, const sm_pkg_info_t *source_metadata,
    char generation_path_out[MAX_PATH]) {
  return sm_fpkg_cache_find_current_generation_with_backport(
      cache_root, source_metadata, NULL, generation_path_out);
}

sm_fpkg_status_t sm_fpkg_cache_read_generation_info(
    const char *cache_root, const char *title_id,
    const char *generation_path,
    sm_fpkg_cache_generation_info_t *info_out) {
  if (!info_out || !valid_title_id(title_id) ||
      !valid_text_path(generation_path, MAX_PATH))
    return SM_FPKG_CORRUPT;
  memset(info_out, 0, sizeof(*info_out));
  char normalized_root[MAX_PATH];
  if (!normalize_cache_root(cache_root, normalized_root))
    return SM_FPKG_CORRUPT;
  char expected_prefix[MAX_PATH];
  int prefix_length = snprintf(expected_prefix, sizeof(expected_prefix),
                               "%s/%s/", normalized_root, title_id);
  if (prefix_length < 0 || (size_t)prefix_length >= sizeof(expected_prefix) ||
      strncmp(generation_path, expected_prefix, (size_t)prefix_length) != 0)
    return SM_FPKG_CORRUPT;
  const char *generation_name = generation_path + prefix_length;
  if (!valid_generation_name(generation_name))
    return SM_FPKG_CORRUPT;
  char expected_path[MAX_PATH];
  int path_length = snprintf(expected_path, sizeof(expected_path), "%s/%s/%s",
                             normalized_root, title_id, generation_name);
  if (path_length < 0 || (size_t)path_length >= sizeof(expected_path) ||
      strcmp(expected_path, generation_path) != 0)
    return SM_FPKG_CORRUPT;

  int root_fd = open_directory_path(normalized_root, false);
  if (root_fd < 0)
    return SM_FPKG_IO;
  if (!safe_cache_directory_fd(root_fd)) {
    close(root_fd);
    return SM_FPKG_CORRUPT;
  }
  int title_fd = openat(root_fd, title_id,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (title_fd < 0) {
    close(root_fd);
    return SM_FPKG_IO;
  }
  if (!safe_cache_directory_fd(title_fd)) {
    close(title_fd);
    close(root_fd);
    return SM_FPKG_CORRUPT;
  }
  int lock_fd = open_title_lock(title_fd, false);
  if (lock_fd < 0) {
    close(title_fd);
    close(root_fd);
    return SM_FPKG_IO;
  }

  int generation_fd = -1;
  sm_fpkg_status_t status = read_generation_identity(
      title_fd, generation_name, title_id, true, &generation_fd, NULL);
  if (status == SM_FPKG_OK) {
    int length = snprintf(info_out->generation_path,
                          sizeof(info_out->generation_path), "%s",
                          expected_path);
    if (length < 0 || (size_t)length >= sizeof(info_out->generation_path) ||
        !read_generation_source_metadata(generation_fd, title_id, info_out)) {
      status = SM_FPKG_CORRUPT;
    } else {
      char source_copy[MAX_PATH];
      (void)strlcpy(source_copy, info_out->source_path, sizeof(source_copy));
      char *slash = strrchr(source_copy, '/');
      if (!slash || slash == source_copy) {
        status = SM_FPKG_CORRUPT;
      } else {
        const char *leaf = strrchr(info_out->source_path, '/');
        leaf = leaf ? leaf + 1 : info_out->source_path;
        if (slash == source_copy)
          slash[1] = '\0';
        else
          *slash = '\0';
        int parent_fd = open_directory_path(source_copy, false);
        if (parent_fd >= 0) {
          int source_fd = openat(parent_fd, leaf,
                                 O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
          if (source_fd >= 0) {
            struct stat source_stat;
            info_out->source_matches_snapshot =
                fstat(source_fd, &source_stat) == 0 &&
                sm_pkg_source_matches(&info_out->source_metadata,
                                      &source_stat);
            close(source_fd);
          }
          close(parent_fd);
        }
      }
    }
    close(generation_fd);
  }

  (void)flock(lock_fd, LOCK_UN);
  close(lock_fd);
  close(title_fd);
  close(root_fd);
  if (status != SM_FPKG_OK)
    memset(info_out, 0, sizeof(*info_out));
  return status;
}

bool sm_fpkg_cache_source_matches_remounted_image(
    const sm_fpkg_cache_generation_info_t *info) {
  if (!info || !info->source_path[0] ||
      info->source_metadata.source_size > (uint64_t)INT64_MAX)
    return false;
  int fd = open(info->source_path,
                O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0)
    return false;
  struct stat st;
  bool matches = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                 st.st_size >= 0 &&
                 (uint64_t)st.st_size == info->source_metadata.source_size &&
                 st.st_mtime == info->source_metadata.source_mtime &&
                 (uint32_t)st.st_mtim.tv_nsec ==
                     info->source_metadata.source_mtime_ns &&
                 (uint64_t)st.st_ctime == info->source_metadata.source_ctime &&
                 (uint32_t)st.st_ctim.tv_nsec ==
                     info->source_metadata.source_ctime_ns;
  close(fd);
  return matches;
}

sm_fpkg_status_t sm_fpkg_cache_remove_generation(
    const char *cache_root, const char *title_id,
    const char *generation_name,
    sm_fpkg_cache_cleanup_begin_fn begin_cleanup,
    sm_fpkg_cache_cleanup_end_fn end_cleanup, void *ctx) {
  if (!valid_title_id(title_id) || !valid_generation_name(generation_name) ||
      !begin_cleanup || !end_cleanup)
    return SM_FPKG_CORRUPT;
  char normalized_root[MAX_PATH];
  if (!normalize_cache_root(cache_root, normalized_root))
    return SM_FPKG_CORRUPT;
  char generation_path[MAX_PATH];
  int path_length = snprintf(generation_path, sizeof(generation_path),
                             "%s/%s/%s", normalized_root, title_id,
                             generation_name);
  if (path_length < 0 || (size_t)path_length >= sizeof(generation_path))
    return SM_FPKG_UNSUPPORTED;
  if (!begin_cleanup(title_id, generation_path, ctx))
    return SM_FPKG_CANCELLED;

  sm_fpkg_status_t status = SM_FPKG_IO;
  int root_fd = open_directory_path(normalized_root, false);
  int title_fd = -1, lock_fd = -1, generation_fd = -1;
  owned_cache_manifest_t manifest = {0};
  if (root_fd < 0) {
    if (errno == ENOENT)
      status = SM_FPKG_OK;
    goto cleanup;
  }
  if (!safe_cache_directory_fd(root_fd))
    goto cleanup;
  title_fd = openat(root_fd, title_id,
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (title_fd < 0) {
    if (errno == ENOENT)
      status = SM_FPKG_OK;
    goto cleanup;
  }
  if (!safe_cache_directory_fd(title_fd))
    goto cleanup;
  lock_fd = open_title_lock(title_fd, false);
  if (lock_fd < 0)
    goto cleanup;

  char active_generation[112];
  status = read_active_generation(title_fd, title_id, active_generation);
  if (status != SM_FPKG_OK)
    goto cleanup;
  if (strcmp(active_generation, generation_name) == 0) {
    status = SM_FPKG_CANCELLED;
    goto cleanup;
  }
  status = read_generation_identity(title_fd, generation_name, title_id, true,
                                    &generation_fd, NULL);
  if (status == SM_FPKG_UNSUPPORTED && errno == ENOENT) {
    /* Cleanup is idempotent after a crash or a concurrent prior cleanup. */
    status = SM_FPKG_OK;
    goto cleanup;
  }
  if (status != SM_FPKG_OK)
    goto cleanup;
  status = parse_owned_cache_manifest(generation_fd, &manifest);
  if (status != SM_FPKG_OK)
    goto cleanup;
  if (!validate_generation_owned_tree(generation_fd, title_id)) {
    status = SM_FPKG_CORRUPT;
    goto cleanup;
  }

  uint64_t serial = atomic_fetch_add_explicit(
      &g_cache_generation_counter, 1u, memory_order_relaxed);
  char quarantine_name[160];
  int quarantine_length = snprintf(
      quarantine_name, sizeof(quarantine_name), ".quarantine-%s-%ld-%llu",
      generation_name + 4u, (long)getpid(), (unsigned long long)serial);
  if (quarantine_length < 0 ||
      (size_t)quarantine_length >= sizeof(quarantine_name)) {
    status = SM_FPKG_UNSUPPORTED;
    goto cleanup;
  }
  if (rename_noreplace(title_fd, generation_name, title_fd,
                       quarantine_name) != 0 || fsync(title_fd) != 0) {
    status = SM_FPKG_IO;
    goto cleanup;
  }

  /* New launches cannot resolve this generation after quarantine. */
  end_cleanup(ctx);
  begin_cleanup = NULL;
  if (!remove_validated_owned_tree(generation_fd, "", 0u, &manifest) ||
      unlinkat(title_fd, quarantine_name, AT_REMOVEDIR) != 0 ||
      fsync(title_fd) != 0) {
    status = SM_FPKG_IO;
    goto cleanup;
  }
  status = SM_FPKG_OK;

cleanup:
  free_owned_cache_manifest(&manifest);
  if (generation_fd >= 0)
    close(generation_fd);
  if (lock_fd >= 0) {
    (void)flock(lock_fd, LOCK_UN);
    close(lock_fd);
  }
  if (title_fd >= 0)
    close(title_fd);
  if (root_fd >= 0)
    close(root_fd);
  if (begin_cleanup)
    end_cleanup(ctx);
  return status;
}

static sm_fpkg_status_t replace_journal_state(int directory_fd,
                                              const char *state) {
  if (strcmp(state, "staging") && strcmp(state, "verified") &&
      strcmp(state, "published") && strcmp(state, "failed"))
    return SM_FPKG_CORRUPT;
  char contents[96];
  int length = snprintf(contents, sizeof(contents),
                        "SM-FPKG-JOURNAL-1\nstate=%s\n", state);
  if (length < 0 || (size_t)length >= sizeof(contents))
    return SM_FPKG_UNSUPPORTED;

  struct stat existing;
  if (fstatat(directory_fd, ".journal", &existing, AT_SYMLINK_NOFOLLOW) != 0)
    return SM_FPKG_IO;
  if (!S_ISREG(existing.st_mode) || existing.st_uid != geteuid() ||
      existing.st_nlink != 1 || (existing.st_mode & 0022) != 0) {
    errno = EPERM;
    return SM_FPKG_CORRUPT;
  }
  char temporary[96];
  uint64_t serial = atomic_fetch_add_explicit(
      &g_cache_generation_counter, 1u, memory_order_relaxed);
  int temp_length = snprintf(temporary, sizeof(temporary),
                             ".journal-%ld-%llu", (long)getpid(),
                             (unsigned long long)serial);
  if (temp_length < 0 || (size_t)temp_length >= sizeof(temporary))
    return SM_FPKG_UNSUPPORTED;
  int fd = openat(directory_fd, temporary,
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0)
    return SM_FPKG_IO;
  sm_fpkg_status_t status = write_all(fd, contents, (size_t)length);
  if (status == SM_FPKG_OK && fsync(fd) != 0)
    status = SM_FPKG_IO;
  if (close(fd) != 0 && status == SM_FPKG_OK)
    status = SM_FPKG_IO;
  if (status == SM_FPKG_OK && renameat(directory_fd, temporary,
                                       directory_fd, ".journal") != 0)
    status = SM_FPKG_IO;
  if (status == SM_FPKG_OK && fsync(directory_fd) != 0)
    status = SM_FPKG_IO;
  if (status != SM_FPKG_OK)
    (void)unlinkat(directory_fd, temporary, 0);
  return status;
}

static int rename_noreplace(int from_fd, const char *from, int to_fd,
                            const char *to) {
#if !defined(SM_FPKG_FORCE_PORTABLE_RENAME) && defined(__APPLE__) && defined(RENAME_EXCL)
  return renameatx_np(from_fd, from, to_fd, to, RENAME_EXCL);
#elif !defined(SM_FPKG_FORCE_PORTABLE_RENAME) && defined(__linux__) && defined(SYS_renameat2)
  return (int)syscall(SYS_renameat2, from_fd, from, to_fd, to,
                      RENAME_NOREPLACE);
#elif !defined(SM_FPKG_FORCE_PORTABLE_RENAME) && defined(__FreeBSD__) && defined(AT_RENAME_NOREPLACE) && \
    defined(SYS_renameat2)
  return (int)syscall(SYS_renameat2, from_fd, from, to_fd, to,
                      AT_RENAME_NOREPLACE);
#else
  struct stat source;
  if (fstatat(from_fd, from, &source, AT_SYMLINK_NOFOLLOW) != 0)
    return -1;
  if (S_ISREG(source.st_mode)) {
    if (source.st_uid != geteuid() || source.st_nlink != 1 ||
        (source.st_mode & 0022) != 0) { errno = EPERM; return -1; }
    if (linkat(from_fd, from, to_fd, to, 0) != 0)
      return -1;
    if (fsync(to_fd) != 0)
      return -1;
    return unlinkat(from_fd, from, 0);
  }
  if (!S_ISDIR(source.st_mode) || source.st_uid != geteuid() ||
      (source.st_mode & 0022) != 0) { errno = EPERM; return -1; }
  /* The title directory is private and held under its lock. Reserve an empty
   * destination exclusively, then replace only that verified reservation. */
  if (mkdirat(to_fd, to, 0700) != 0)
    return -1;
  int reservation = openat(to_fd, to, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat reserved = {0}, current = {0}, source_now = {0};
  bool reservation_valid = reservation >= 0 && fstat(reservation, &reserved) == 0 &&
      S_ISDIR(reserved.st_mode) && reserved.st_uid == geteuid() &&
      (reserved.st_mode & 0022) == 0;
  bool verified = reservation_valid &&
      fstatat(to_fd, to, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
      current.st_dev == reserved.st_dev && current.st_ino == reserved.st_ino &&
      fstatat(from_fd, from, &source_now, AT_SYMLINK_NOFOLLOW) == 0 &&
      source_now.st_dev == source.st_dev && source_now.st_ino == source.st_ino;
  int result = verified ? renameat(from_fd, from, to_fd, to) : -1;
  int saved = verified ? errno : EPERM;
  if (result == 0) {
    if (fstatat(to_fd, to, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
        current.st_dev != source.st_dev || current.st_ino != source.st_ino) {
      result = -1;
      saved = ESTALE;
    }
  } else if (reservation_valid &&
             fstatat(to_fd, to, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
             current.st_dev == reserved.st_dev && current.st_ino == reserved.st_ino) {
    (void)unlinkat(to_fd, to, AT_REMOVEDIR);
  }
  if (reservation >= 0) close(reservation);
  if (result != 0) errno = saved;
  return result;
#endif
}

#ifdef SM_FPKG_TEST_RENAME
int sm_fpkg_cache_test_rename_noreplace(int from_fd, const char *from,
                                        int to_fd, const char *to) {
  return rename_noreplace(from_fd, from, to_fd, to);
}
#endif

static sm_fpkg_status_t write_active_generation(
    int title_fd, const char *generation_name, bool had_active) {
  char contents[160];
  int length = snprintf(contents, sizeof(contents),
                        "SM-FPKG-ACTIVE-1\ngeneration=%s\n",
                        generation_name);
  if (length < 0 || (size_t)length >= sizeof(contents))
    return SM_FPKG_UNSUPPORTED;
  uint64_t serial = atomic_fetch_add_explicit(
      &g_cache_generation_counter, 1u, memory_order_relaxed);
  char temporary[96];
  int temp_length = snprintf(temporary, sizeof(temporary),
                             ".active-%ld-%llu", (long)getpid(),
                             (unsigned long long)serial);
  if (temp_length < 0 || (size_t)temp_length >= sizeof(temporary))
    return SM_FPKG_UNSUPPORTED;
  int fd = openat(title_fd, temporary,
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0)
    return SM_FPKG_IO;
  sm_fpkg_status_t status = write_all(fd, contents, (size_t)length);
  if (status == SM_FPKG_OK && fsync(fd) != 0)
    status = SM_FPKG_IO;
  if (close(fd) != 0 && status == SM_FPKG_OK)
    status = SM_FPKG_IO;
  if (status == SM_FPKG_OK) {
    int rename_result = had_active
                            ? renameat(title_fd, temporary, title_fd, "active")
                            : rename_noreplace(title_fd, temporary, title_fd,
                                               "active");
    if (rename_result != 0)
      status = SM_FPKG_IO;
  }
  if (status == SM_FPKG_OK && fsync(title_fd) != 0)
    status = SM_FPKG_IO;
  if (status != SM_FPKG_OK)
    (void)unlinkat(title_fd, temporary, 0);
  return status;
}

static sm_fpkg_status_t cache_begin(void *opaque,
                                    const sm_fpkg_file_t *file,
                                    void **file_ctx_out) {
  cache_sink_t *sink = opaque;
  *file_ctx_out = NULL;
  if (is_cancelled(sink->options))
    return SM_FPKG_CANCELLED;
  if (file->is_directory || !safe_relative_path(file->path))
    return SM_FPKG_CORRUPT;
  cache_file_t *context = calloc(1, sizeof(*context));
  if (!context)
    return SM_FPKG_NOMEM;
  int parent_fd = -1;
  char leaf[NAME_MAX + 1];
  if (!relative_parent(sink->directory_fd, file->path, true, &parent_fd,
                       leaf)) {
    free(context);
    return SM_FPKG_IO;
  }
  context->fd = openat(parent_fd, leaf,
                       O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                       0600);
  if (context->fd < 0) {
    close(parent_fd);
    free(context);
    return SM_FPKG_IO;
  }
  if (fsync(parent_fd) != 0) {
    int saved = errno;
    close(context->fd);
    close(parent_fd);
    free(context);
    errno = saved;
    return SM_FPKG_IO;
  }
  close(parent_fd);
  context->expected_size = file->size;
  context->mode = is_executable_file(file->path) ? 0755 : 0644;
  context->sink = sink;
  *file_ctx_out = context;
  return SM_FPKG_OK;
}

static sm_fpkg_status_t cache_write(void *opaque, uint64_t offset,
                                    const void *bytes, size_t size) {
  cache_file_t *file = opaque;
  if (is_cancelled(file->sink->options))
    return SM_FPKG_CANCELLED;
  if (offset != file->written || file->written > file->expected_size ||
      size > file->expected_size - file->written)
    return SM_FPKG_CORRUPT;
  sm_fpkg_status_t status = write_all(file->fd, bytes, size);
  if (status == SM_FPKG_OK)
    file->written += size;
  return status;
}

static sm_fpkg_status_t cache_finish(void *opaque, bool commit) {
  cache_file_t *file = opaque;
  sm_fpkg_status_t status = SM_FPKG_OK;
  if (commit && file->written != file->expected_size)
    status = SM_FPKG_CORRUPT;
  if (commit && status == SM_FPKG_OK && fchmod(file->fd, file->mode) != 0)
    status = SM_FPKG_IO;
  if (commit && status == SM_FPKG_OK && fsync(file->fd) != 0)
    status = SM_FPKG_IO;
  if (close(file->fd) != 0 && status == SM_FPKG_OK)
    status = SM_FPKG_IO;
  if (commit && status == SM_FPKG_OK) {
    if (file->written > UINT64_MAX - file->sink->total_written)
      status = SM_FPKG_UNSUPPORTED;
    else
      file->sink->total_written += file->written;
  }
  free(file);
  return status;
}

static sm_fpkg_status_t stage_backport_files(
    const sm_fpkg_backport_t *backport,
    const sm_fpkg_index_file_t *files, cache_sink_t *sink) {
  size_t count = sm_fpkg_backport_count(backport);
  uint8_t *buffer = count ? malloc(65536u) : NULL;
  if (count && !buffer)
    return SM_FPKG_NOMEM;
  sm_fpkg_status_t status = SM_FPKG_OK;
  for (size_t i = 0; i < count && status == SM_FPKG_OK; ++i) {
    if (is_cancelled(sink->options)) { status = SM_FPKG_CANCELLED; break; }
    int source = sm_fpkg_backport_open_file(backport, i);
    if (source < 0) { status = SM_FPKG_IO; break; }
    sm_fpkg_file_t file = {.path = files[i].path, .size = files[i].size};
    void *context = NULL;
    status = cache_begin(sink, &file, &context);
    uint64_t offset = 0;
    while (status == SM_FPKG_OK && offset < file.size) {
      size_t chunk = file.size - offset < 65536u
                         ? (size_t)(file.size - offset) : 65536u;
      ssize_t got = read(source, buffer, chunk);
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) { status = SM_FPKG_IO; break; }
      status = cache_write(context, offset, buffer, (size_t)got);
      offset += (uint64_t)got;
    }
    if (close(source) != 0 && status == SM_FPKG_OK)
      status = SM_FPKG_IO;
    if (context) {
      sm_fpkg_status_t finish = cache_finish(context, status == SM_FPKG_OK);
      if (status == SM_FPKG_OK) status = finish;
    }
  }
  free(buffer);
  return status;
}

static bool source_snapshot_matches(int fd, const char *canonical_path,
                                    const sm_pkg_info_t *metadata) {
  struct stat opened, named;
  if (fstat(fd, &opened) != 0 || lstat(canonical_path, &named) != 0)
    return false;
  return sm_pkg_source_matches(metadata, &opened) &&
         sm_pkg_source_matches(metadata, &named);
}

static sm_fpkg_status_t append_identity(cache_text_t *manifest,
                                        const char *title_id,
                                        const char *source_path,
                                        const sm_pkg_info_t *metadata,
                                        uint64_t fingerprint) {
  char header[MAX_PATH + 512];
  int length = snprintf(
      header, sizeof(header),
      "SM-FPKG-BUILT-1\ntitle=%s\nsource=%s\nfingerprint=%016llx\n"
      "device=%llu\ninode=%llu\nsize=%llu\nmtime=%lld.%09u\n"
      "ctime=%llu.%09u\n",
      title_id, source_path, (unsigned long long)fingerprint,
      (unsigned long long)metadata->source_device,
      (unsigned long long)metadata->source_inode,
      (unsigned long long)metadata->source_size,
      (long long)metadata->source_mtime, metadata->source_mtime_ns,
      (unsigned long long)metadata->source_ctime, metadata->source_ctime_ns);
  if (length < 0 || (size_t)length >= sizeof(header))
    return SM_FPKG_UNSUPPORTED;
  return append_text(manifest, header, (size_t)length);
}

static sm_fpkg_status_t write_cache_artifacts(
    int stage_fd, const char *source_path, const char *title_id,
    uint64_t fingerprint,
    const sm_fpkg_index_blob_t *index, const sm_fpkg_index_blob_t *manifest,
    const cache_text_t *built) {
  const char *filename = strrchr(source_path, '/');
  filename = filename ? filename + 1 : source_path;
  if (!filename[0] || strchr(filename, '\n') || strchr(filename, '\r') ||
      strlen(filename) > NAME_MAX)
    return SM_FPKG_UNSUPPORTED;
  char image_link[NAME_MAX + 16];
  int link_length = snprintf(image_link, sizeof(image_link), "kraken/%s\n",
                             filename);
  if (link_length < 0 || (size_t)link_length >= sizeof(image_link))
    return SM_FPKG_UNSUPPORTED;
  sm_fpkg_status_t status = write_new_file(
      stage_fd, "ampr_emu.index", index->data, index->size, 0644);
  if (status == SM_FPKG_OK)
    status = write_new_file(stage_fd, "ampr_assets.index", manifest->data,
                            manifest->size, 0644);
  if (status == SM_FPKG_OK)
    status = write_new_file(stage_fd, "kraken.image", image_link,
                            (size_t)link_length, 0644);
  if (status == SM_FPKG_OK)
    status = write_new_file(stage_fd, ".built", built->data, built->size,
                            0600);
  if (status != SM_FPKG_OK)
    return status;

  char done[256];
  int done_length = snprintf(
      done, sizeof(done),
      "SM-FPKG-DONE-1\ntitle=%s\nfingerprint=%016llx\n",
      title_id, (unsigned long long)fingerprint);
  if (done_length < 0 || (size_t)done_length >= sizeof(done))
    return SM_FPKG_UNSUPPORTED;
  status = write_new_file(stage_fd, ".done", done, (size_t)done_length, 0600);
  if (status == SM_FPKG_OK && fsync(stage_fd) != 0)
    status = SM_FPKG_IO;
  return status;
}

sm_fpkg_status_t sm_fpkg_cache_build_generation(
    const char *cache_root, const char *source_path, int source_fd,
    const sm_pkg_info_t *metadata, sm_fpkg_t *fpkg,
    sm_fpkg_index_extract_fn should_extract_fn, void *extract_ctx,
    const sm_fpkg_cache_options_t *options,
    char generation_path_out[MAX_PATH]) {
  if (generation_path_out)
    generation_path_out[0] = '\0';
  if (!generation_path_out || !valid_text_path(cache_root, MAX_PATH) ||
      !valid_text_path(source_path, MAX_PATH) || source_fd < 0 || !metadata ||
      !fpkg || !valid_title_id(metadata->title_id) ||
      metadata->pkg_kind != PKG_KIND_BASE ||
      !metadata->pkg_classification_confident)
    return SM_FPKG_CORRUPT;
  if (is_cancelled(options))
    return SM_FPKG_CANCELLED;

  char normalized_cache_root[MAX_PATH];
  (void)strlcpy(normalized_cache_root, cache_root,
                sizeof(normalized_cache_root));
  size_t cache_root_length = strlen(normalized_cache_root);
  while (cache_root_length > 1u &&
         normalized_cache_root[cache_root_length - 1u] == '/')
    normalized_cache_root[--cache_root_length] = '\0';

  char canonical_source[MAX_PATH];
  if (!realpath(source_path, canonical_source) ||
      strcmp(source_path, canonical_source) != 0 ||
      !source_snapshot_matches(source_fd, canonical_source, metadata))
    return SM_FPKG_UNSUPPORTED;
  int source_flags = fcntl(source_fd, F_GETFL);
  if (source_flags < 0 || (source_flags & O_ACCMODE) != O_RDONLY)
    return SM_FPKG_UNSUPPORTED;
  struct stat fd_stat;
  if (fstat(source_fd, &fd_stat) != 0 || !S_ISREG(fd_stat.st_mode))
    return SM_FPKG_IO;
  int reader_fd = sm_fpkg_source_fd(fpkg);
  struct stat reader_stat;
  if (reader_fd < 0 || fstat(reader_fd, &reader_stat) != 0 ||
      reader_stat.st_dev != fd_stat.st_dev ||
      reader_stat.st_ino != fd_stat.st_ino ||
      !sm_pkg_source_matches(metadata, &reader_stat))
    return SM_FPKG_UNSUPPORTED;
  uint64_t limit = options && options->max_extract_bytes
                       ? options->max_extract_bytes
                       : SM_FPKG_CACHE_DEFAULT_EXTRACT_LIMIT;

  int root_fd = open_directory_path(normalized_cache_root, true);
  if (root_fd < 0)
    return SM_FPKG_IO;
  struct stat root_stat;
  if (fstat(root_fd, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode) ||
      root_stat.st_uid != geteuid() || (root_stat.st_mode & 0022) != 0) {
    close(root_fd);
    errno = EPERM;
    return SM_FPKG_CORRUPT;
  }
  bool title_created = false;
  int title_fd = open_or_create_child_dir(root_fd, metadata->title_id, 0700,
                                          &title_created);
  (void)title_created;
  if (title_fd < 0) {
    close(root_fd);
    return SM_FPKG_IO;
  }
  int lock_fd = open_title_lock(title_fd, true);
  if (lock_fd < 0) {
    close(title_fd);
    close(root_fd);
    return SM_FPKG_IO;
  }

  sm_fpkg_backport_t *backport = NULL;
  sm_fpkg_status_t backport_status = sm_fpkg_backport_open(
      options ? options->backport_path : NULL, &backport);
  if (backport_status != SM_FPKG_OK) {
    (void)flock(lock_fd, LOCK_UN);
    close(lock_fd);
    close(title_fd);
    close(root_fd);
    return backport_status;
  }
  uint64_t backport_fingerprint = sm_fpkg_backport_fingerprint(backport);

  char active_generation[112];
  sm_fpkg_status_t status = read_active_generation(
      title_fd, metadata->title_id, active_generation);
  if (status != SM_FPKG_OK) {
    sm_fpkg_backport_close(backport);
    (void)flock(lock_fd, LOCK_UN);
    close(lock_fd);
    close(title_fd);
    close(root_fd);
    return status;
  }
  if (active_generation[0] &&
      generation_matches_source(title_fd, active_generation,
                                 metadata->title_id, metadata,
                                 backport_fingerprint)) {
    int path_length = snprintf(generation_path_out, MAX_PATH, "%s/%s/%s",
                               normalized_cache_root, metadata->title_id,
                               active_generation);
    if (path_length < 0 || path_length >= MAX_PATH) {
      generation_path_out[0] = '\0';
      status = SM_FPKG_UNSUPPORTED;
    }
    sm_fpkg_backport_close(backport);
    (void)flock(lock_fd, LOCK_UN);
    close(lock_fd);
    close(title_fd);
    close(root_fd);
    return status;
  }
  bool had_active_generation = active_generation[0] != '\0';

  size_t override_count = sm_fpkg_backport_count(backport);
  sm_fpkg_index_file_t *overrides = override_count
      ? calloc(override_count, sizeof(*overrides)) : NULL;
  if (override_count && !overrides) {
    sm_fpkg_backport_close(backport);
    (void)flock(lock_fd, LOCK_UN);
    close(lock_fd);
    close(title_fd);
    close(root_fd);
    return SM_FPKG_NOMEM;
  }
  if (override_count) {
    memcpy(overrides, sm_fpkg_backport_files(backport),
           override_count * sizeof(*overrides));
    for (size_t i = 0; i < sm_fpkg_file_count(fpkg); ++i) {
      sm_fpkg_file_t file;
      if (!sm_fpkg_file_at(fpkg, i, &file) || file.is_directory)
        continue;
      size_t match = sm_fpkg_backport_find(backport, file.path);
      if (match != SIZE_MAX)
        overrides[match].path = file.path;
    }
  }
  selector_ctx_t selector = {should_extract_fn, extract_ctx, backport, overrides};
  cache_text_t built = {0};
  status = append_identity(
      &built, metadata->title_id, canonical_source, metadata,
      source_fingerprint(metadata));
  if (status == SM_FPKG_OK) {
    char line[48];
    int length = snprintf(line, sizeof(line), "backport=%016llx\n",
                          (unsigned long long)backport_fingerprint);
    status = length > 0 && (size_t)length < sizeof(line)
                 ? append_text(&built, line, (size_t)length)
                 : SM_FPKG_UNSUPPORTED;
  }
  uint64_t extract_bytes = 0;
  size_t extract_count = 0;
  bool has_eboot = false, has_metadata = false;
  if (status == SM_FPKG_OK)
    status = estimate_extraction(fpkg, &selector, limit, &extract_bytes,
                                 &extract_count, &has_eboot, &has_metadata,
                                 &built);
  (void)has_eboot;
  (void)has_metadata;
  (void)extract_count;
  sm_fpkg_index_blob_t index = {0}, manifest = {0};
  if (status == SM_FPKG_OK)
    status = sm_fpkg_index_build_package_with_overrides(
        fpkg, select_loose_file, &selector, skip_package_index_file, NULL,
        overrides, override_count,
        (int64_t)metadata->source_mtime, &index, &manifest);
  char image_link[NAME_MAX + 16];
  int image_link_length = -1;
  const char *source_filename = strrchr(canonical_source, '/');
  source_filename = source_filename ? source_filename + 1 : canonical_source;
  if (status == SM_FPKG_OK) {
    image_link_length = snprintf(image_link, sizeof(image_link), "kraken/%s\n",
                                 source_filename);
    if (image_link_length < 0 ||
        (size_t)image_link_length >= sizeof(image_link))
      status = SM_FPKG_UNSUPPORTED;
  }
  if (status == SM_FPKG_OK)
    status = append_manifest_file(&built, "ampr_emu.index", index.size);
  if (status == SM_FPKG_OK)
    status = append_manifest_file(&built, "ampr_assets.index", manifest.size);
  if (status == SM_FPKG_OK)
    status = append_manifest_file(&built, "kraken.image",
                                   (uint64_t)image_link_length);
  uint64_t required_space = extract_bytes;
  if (status == SM_FPKG_OK &&
      (index.size > UINT64_MAX - required_space ||
       manifest.size > UINT64_MAX - required_space - index.size ||
       SM_FPKG_CACHE_RESERVE_BYTES > UINT64_MAX - required_space -
                                         index.size - manifest.size))
    status = SM_FPKG_UNSUPPORTED;
  if (status == SM_FPKG_OK)
    required_space += index.size + manifest.size +
                      SM_FPKG_CACHE_RESERVE_BYTES;
  if (status == SM_FPKG_OK && !checked_free_space(title_fd, required_space))
    status = SM_FPKG_IO;
  if (status == SM_FPKG_OK && is_cancelled(options))
    status = SM_FPKG_CANCELLED;

  uint64_t fingerprint = source_fingerprint(metadata);
  uint64_t serial = atomic_fetch_add_explicit(
      &g_cache_generation_counter, 1u, memory_order_relaxed);
  uint64_t clock_value = monotonic_time_us();
  char generation_name[112];
  (void)snprintf(generation_name, sizeof(generation_name),
                 "gen-%016llx-%llu-%llu", (unsigned long long)fingerprint,
                 (unsigned long long)clock_value,
                 (unsigned long long)serial);
  int stage_fd = -1;
  if (status == SM_FPKG_OK) {
    /* The generation name is unique and is created exclusively. Until the
     * active pointer is committed, its contents are only an unreachable stage.
     */
    if (mkdirat(title_fd, generation_name, 0700) != 0)
      status = SM_FPKG_IO;
    else if (fsync(title_fd) != 0)
      status = SM_FPKG_IO;
    else {
      stage_fd = openat(title_fd, generation_name,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (stage_fd < 0)
        status = SM_FPKG_IO;
    }
  }
  if (status == SM_FPKG_OK) {
    static const char journal[] = "SM-FPKG-JOURNAL-1\nstate=staging\n";
    status = write_new_file(stage_fd, ".journal", journal,
                            sizeof(journal) - 1u, 0600);
  }
  if (status == SM_FPKG_OK) {
    bool created = false;
    int kraken_dir = open_or_create_child_dir(stage_fd, "kraken", 0700,
                                              &created);
    (void)created;
    if (kraken_dir < 0)
      status = SM_FPKG_IO;
    else
      close(kraken_dir);
  }

  cache_sink_t sink_ctx = {.directory_fd = stage_fd,
                           .options = options,
                           .total_written = 0};
  sm_fpkg_extract_sink_t sink = {.begin = cache_begin,
                                 .write = cache_write,
                                 .finish = cache_finish,
                                 .ctx = &sink_ctx};
  if (status == SM_FPKG_OK) {
    size_t file_count = sm_fpkg_file_count(fpkg);
    for (size_t i = 0; i < file_count; ++i) {
      if (is_cancelled(options)) {
        status = SM_FPKG_CANCELLED;
        break;
      }
      sm_fpkg_file_t file;
      if (!sm_fpkg_file_at(fpkg, i, &file)) {
        status = SM_FPKG_CORRUPT;
        break;
      }
      if (file.is_directory || !select_loose_file(file.path, &selector))
        continue;
      if (sm_fpkg_backport_find(backport, file.path) != SIZE_MAX)
        continue;
      status = sm_fpkg_extract_file((sm_fpkg_t *)fpkg, i, &sink);
      if (status != SM_FPKG_OK)
        break;
      if (sink_ctx.total_written > limit) {
        status = SM_FPKG_UNSUPPORTED;
        break;
      }
    }
  }
  if (status == SM_FPKG_OK)
    status = stage_backport_files(backport, overrides, &sink_ctx);
  if (status == SM_FPKG_OK && sink_ctx.total_written != extract_bytes)
    status = SM_FPKG_CORRUPT;
  if (status == SM_FPKG_OK && is_cancelled(options))
    status = SM_FPKG_CANCELLED;
  if (status == SM_FPKG_OK && !source_snapshot_matches(
                                   source_fd, canonical_source, metadata))
    status = SM_FPKG_UNSUPPORTED;
  if (status == SM_FPKG_OK && !sm_fpkg_backport_unchanged(backport))
    status = SM_FPKG_UNSUPPORTED;
  if (status == SM_FPKG_OK)
    status = write_cache_artifacts(stage_fd, canonical_source,
                                   metadata->title_id, fingerprint,
                                   &index, &manifest, &built);
  if (status == SM_FPKG_OK && is_cancelled(options))
    status = SM_FPKG_CANCELLED;
  if (status == SM_FPKG_OK) {
    char expected_generation_path[MAX_PATH];
    int path_length = snprintf(expected_generation_path,
                               sizeof(expected_generation_path), "%s/%s/%s",
                               normalized_cache_root, metadata->title_id,
                               generation_name);
    if (path_length < 0 || (size_t)path_length >= sizeof(expected_generation_path))
      status = SM_FPKG_UNSUPPORTED;
    if (status == SM_FPKG_OK &&
        !validate_generation_owned_tree(stage_fd, metadata->title_id))
      status = SM_FPKG_CORRUPT;
    if (status == SM_FPKG_OK)
      status = replace_journal_state(stage_fd, "verified");
    if (status == SM_FPKG_OK && fsync(title_fd) != 0)
      status = SM_FPKG_IO;
    if (status == SM_FPKG_OK &&
        replace_journal_state(stage_fd, "published") != SM_FPKG_OK)
      status = SM_FPKG_IO;
    if (status == SM_FPKG_OK &&
        write_active_generation(title_fd, generation_name,
                                had_active_generation) != SM_FPKG_OK)
      status = SM_FPKG_IO;
    if (status == SM_FPKG_OK)
      (void)strlcpy(generation_path_out, expected_generation_path, MAX_PATH);
  } else if (stage_fd >= 0) {
    char failed_marker[96];
    int marker_length = snprintf(failed_marker, sizeof(failed_marker),
                                 "failed=%d\n", (int)status);
    if (marker_length > 0 && (size_t)marker_length < sizeof(failed_marker))
      (void)write_new_file(stage_fd, ".failed", failed_marker,
                           (size_t)marker_length, 0600);
  }

  if (stage_fd >= 0)
    close(stage_fd);
  sm_fpkg_index_blob_free(&index);
  sm_fpkg_index_blob_free(&manifest);
  free(built.data);
  free(overrides);
  sm_fpkg_backport_close(backport);
  (void)flock(lock_fd, LOCK_UN);
  close(lock_fd);
  close(title_fd);
  close(root_fd);
  return status;
}
