#include "sm_platform.h"

#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <zlib.h>
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/sha.h>

#include "sm_config_file.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_patch_sources.h"

#define UPDATE_STAMP SM_PATCH_SOURCE_ROOT "/updates.ini"
#define SOURCE_MAX_FILES 20000u
#define SOURCE_MAX_BYTES (128u * 1024u * 1024u)
#define SOURCE_FILE_MAX (4u * 1024u * 1024u)

const sm_patch_source_t sm_patch_source_presets[] = {
    {true, "https://codeload.github.com/TeeKay87/HEN-Cheats-Collection/zip/refs/heads/master"},
    {true, "https://codeload.github.com/etaHEN/PS5_Cheats/zip/refs/heads/main"},
    {true, "https://codeload.github.com/GoldHEN/GoldHEN_Cheat_Repository/zip/refs/heads/main"},
    {true, "https://codeload.github.com/RDX-Sci01/HEN-PPSA-Cheats/zip/refs/heads/main"},
    {false, "https://github.com/illusionyy/PS-Game-Patch/releases/latest/download/patch1.zip"},
    {false, "https://codeload.github.com/notmaj0r/CheatRunner_Patches/zip/refs/heads/main"}
  };

static pthread_rwlock_t g_source_lock = PTHREAD_RWLOCK_INITIALIZER;

bool sm_patch_source_valid(const sm_patch_source_t *source) {
  if (!source || strnlen(source->url, sizeof(source->url)) == sizeof(source->url)) return false;
  const char *host = !strncmp(source->url, "https://", 8) ? source->url + 8 :
                     !strncmp(source->url, "http://", 7) ? source->url + 7 : NULL;
  if (!host || !*host || *host == '/') return false;
  for (const unsigned char *s = (const unsigned char *)host; *s; ++s)
    if (*s <= 0x20 || *s >= 0x7f || *s == '\\' || *s == '|' || *s == '#' || *s == '@' || *s == '"' || *s == '<' || *s == '>') return false;
  return strchr(host, '/') != NULL;
}

static uint64_t source_hash(uint64_t hash, const sm_patch_source_t *source) {
  hash = (hash ^ source->cheats) * UINT64_C(1099511628211);
  for (const unsigned char *s = (const unsigned char *)source->url; *s; ++s)
    hash = (hash ^ *s) * UINT64_C(1099511628211);
  return (hash ^ 0xff) * UINT64_C(1099511628211);
}

uint64_t sm_patch_source_hash(const sm_patch_source_config_t *config) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < config->count; ++i) hash = source_hash(hash, &config->sources[i]);
  return hash;
}

void sm_patch_source_directory(const sm_patch_source_t *source, char out[128]) {
  snprintf(out, 128, "%s/%016llx", SM_PATCH_SOURCE_ROOT,
           (unsigned long long)source_hash(UINT64_C(14695981039346656037), source));
}

void sm_patch_sources_read_lock(void) { pthread_rwlock_rdlock(&g_source_lock); }
void sm_patch_sources_read_unlock(void) { pthread_rwlock_unlock(&g_source_lock); }
void sm_patch_sources_write_lock(void) { pthread_rwlock_wrlock(&g_source_lock); }
void sm_patch_sources_write_unlock(void) { pthread_rwlock_unlock(&g_source_lock); }

static bool generation_name(const char *name) {
  if (strncmp(name, ".next-", 6) || strlen(name) != 12) return false;
  for (size_t i = 6; name[i]; ++i)
    if (!((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= 'a' && name[i] <= 'z') ||
          (name[i] >= '0' && name[i] <= '9'))) return false;
  return true;
}

static bool read_at(int fd, uint64_t offset, void *data, size_t size);

bool sm_patch_source_current(const sm_patch_source_t *source, char out[192]) {
  char root[128], pointer[160], name[13] = {0};
  sm_patch_source_directory(source, root);
  snprintf(pointer, sizeof(pointer), "%s/current", root);
  int fd = open(pointer, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return false;
  struct stat st;
  bool valid = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == 12 && read_at(fd, 0, name, 12);
  close(fd);
  if (!valid || !generation_name(name)) return false;
  snprintf(out, 192, "%s/%s", root, name);
  return !lstat(out, &st) && S_ISDIR(st.st_mode);
}

static bool ensure_directory(const char *path) {
  if (mkdir(path, 0777) && errno != EEXIST) return false;
  struct stat st;
  if (lstat(path, &st)) return false;
  if (S_ISDIR(st.st_mode)) return true;
  errno = ENOTDIR; return false;
}

// Generations contain only flat files produced by this extractor.
static void remove_generation(const char *path) {
  struct stat st;
  if (lstat(path, &st) || !S_ISDIR(st.st_mode)) return;
  DIR *dir = opendir(path);
  if (!dir) return;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char file[MAX_PATH];
    int n = snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
    if (n > 0 && (size_t)n < sizeof(file)) unlink(file);
  }
  closedir(dir);
  rmdir(path);
}

static void cleanup_old_generations(const sm_patch_source_t *source, const char *root) {
  sm_patch_sources_read_lock();
  char current[192] = {0};
  bool valid = sm_patch_source_current(source, current);
  sm_patch_sources_read_unlock();
  if (!valid) {
    char pointer[160]; struct stat st;
    snprintf(pointer, sizeof(pointer), "%s/current", root);
    // A damaged/unreadable publication marker must not turn the current
    // generation into an orphan and delete files still in use by readers.
    if (!lstat(pointer, &st) || errno != ENOENT) return;
  }
  DIR *dir = opendir(root);
  struct dirent *entry;
  while (dir && (entry = readdir(dir))) {
    if (!generation_name(entry->d_name)) continue;
    char path[192]; snprintf(path, sizeof(path), "%s/%.12s", root, entry->d_name);
    if (strcmp(current, path)) remove_generation(path);
  }
  if (dir) closedir(dir);
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16; }

static bool read_at(int fd, uint64_t offset, void *data, size_t size) {
  uint8_t *out = data;
  while (size) {
    ssize_t n = pread(fd, out, size, (off_t)offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    out += n; offset += (size_t)n; size -= (size_t)n;
  }
  return true;
}

static bool write_all(int fd, const void *data, size_t size) {
  const uint8_t *p = data;
  while (size) {
    ssize_t n = write(fd, p, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n; size -= (size_t)n;
  }
  return true;
}

static bool title_in_name(const char *name) {
  for (size_t i = 0; name[i]; ++i) {
    if (strncmp(name + i, "CUSA", 4) && strncmp(name + i, "PPSA", 4)) continue;
    size_t n = 4;
    while (n < 9 && name[i+n] >= '0' && name[i+n] <= '9') ++n;
    if (n == 9 && !(name[i+n] >= '0' && name[i+n] <= '9')) return true;
  }
  return false;
}

static const char *archive_filename(const char *name, bool cheats) {
  if (!*name || *name == '/') return NULL;
  const char *base = name;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
    if (*p < 0x20 || *p >= 0x7f || *p == '\\' || *p == ':') return NULL;
    if (*p == '/') {
      if ((const char *)p == base || ((const char *)p - base == 1 && *base == '.') ||
          ((const char *)p - base == 2 && base[0] == '.' && base[1] == '.')) return NULL;
      base = (const char *)p + 1;
    }
  }
  if (!*base || strlen(base) > 200 || !title_in_name(base)) return NULL;
  const char *extension = strrchr(base, '.');
  if (!extension) return NULL;
  if (cheats) {
    if (strcasecmp(extension, ".json") && strcasecmp(extension, ".shn") && strcasecmp(extension, ".mc4")) return NULL;
  } else if (strcasecmp(extension, ".xml")) return NULL;
  return base;
}

static int extract_file(int archive, uint64_t offset, uint32_t compressed,
                         uint32_t size, uint32_t crc, unsigned method, const char *path,
                         uint8_t *buffers, sm_patch_cancel_t cancelled, void *context) {
  int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0666);
  if (fd < 0) return errno;
  uint8_t *input = buffers, *output = buffers + 65536;
  int error = 0;
  z_stream stream = {0};
  bool inflate_ready = false;
  if (!error && method == 8) {
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) error = EINVAL;
    else inflate_ready = true;
  }
  uint32_t total = 0, checksum = (uint32_t)crc32(0, NULL, 0);
  int status = Z_OK;
  while (!error && compressed) {
    if (cancelled(context)) { error = ECANCELED; break; }
    size_t n = compressed < 65536 ? compressed : 65536;
    if (!read_at(archive, offset, input, n)) { error = EIO; break; }
    offset += n; compressed -= (uint32_t)n;
    stream.next_in = input; stream.avail_in = (uInt)n;
    do {
      if (cancelled(context)) { error = ECANCELED; break; }
      stream.next_out = output; stream.avail_out = 65536;
      size_t bytes = n;
      if (method == 8) {
        status = inflate(&stream, Z_NO_FLUSH);
        bytes = 65536 - stream.avail_out;
        if (status == Z_BUF_ERROR && !stream.avail_in && !bytes) break;
        if (status != Z_OK && status != Z_STREAM_END) { error = EINVAL; break; }
      }
      const uint8_t *data = method == 8 ? output : input;
      if (bytes > size - total || !write_all(fd, data, bytes)) { error = EIO; break; }
      total += (uint32_t)bytes;
      checksum = (uint32_t)crc32(checksum, data, (uInt)bytes);
      if (!method) break;
    } while (!error && status != Z_STREAM_END && (stream.avail_in || !stream.avail_out));
    if (method == 8 && status == Z_STREAM_END) {
      if (stream.avail_in || compressed) error = EINVAL;
      break;
    }
  }
  if (!error && (total != size || checksum != crc || (method == 8 && status != Z_STREAM_END))) error = EINVAL;
  if (!error && fsync(fd)) error = errno;
  if (close(fd) && !error) error = errno;
  if (inflate_ready) inflateEnd(&stream);
  if (error) unlink(path);
  return error;
}

static bool duplicate_matches(const char *path, uint32_t size, uint32_t crc,
                                sm_patch_cancel_t cancelled, void *context) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return false;
  struct stat st;
  bool valid = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == size;
  uint8_t data[4096];
  uint32_t checksum = (uint32_t)crc32(0, NULL, 0), remaining = size;
  while (valid && remaining) {
    if (cancelled(context)) { valid = false; break; }
    size_t n = remaining < sizeof(data) ? remaining : sizeof(data);
    if (!read_at(fd, size - remaining, data, n)) { valid = false; break; }
    checksum = (uint32_t)crc32(checksum, data, (uInt)n);
    remaining -= (uint32_t)n;
  }
  close(fd);
  return valid && checksum == crc;
}

static int extract_archive(const char *path, const char *directory, bool cheats,
                            sm_patch_cancel_t cancelled, void *context) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return errno;
  struct stat st;
  int error = fstat(fd, &st) ? errno : !S_ISREG(st.st_mode) || st.st_size < 22 || st.st_size > SM_PATCH_ARCHIVE_MAX ? EFBIG : 0;
  size_t tail_size = !error ? (st.st_size < 65557 ? (size_t)st.st_size : 65557) : 0;
  uint8_t *tail = !error ? malloc(tail_size) : NULL;
  if (!error && !tail) error = ENOMEM;
  uint64_t central = 0, central_end = 0;
  unsigned entries = 0;
  if (!error && !read_at(fd, (uint64_t)st.st_size - tail_size, tail, tail_size)) error = EIO;
  if (!error) {
    error = EINVAL;
    for (size_t i = tail_size - 22;; --i) {
      if (u32(tail + i) == 0x06054b50 && i + 22u + u16(tail + i + 20) == tail_size) {
        entries = u16(tail + i + 10);
        central = u32(tail + i + 16); central_end = central + u32(tail + i + 12);
        if (!u16(tail + i + 4) && !u16(tail + i + 6) && entries != 65535 &&
            entries == u16(tail + i + 8) && central_end <= (uint64_t)st.st_size - tail_size + i) error = 0;
        break;
      }
      if (!i) break;
    }
  }
  free(tail);
  uint8_t *buffers = !error ? malloc(2u * 65536u) : NULL;
  if (!error && !buffers) error = ENOMEM;
  uint64_t central_begin = central;
  size_t files = 0;
  uint64_t total = 0;
  for (unsigned i = 0; !error && i < entries; ++i) {
    if (cancelled(context)) { error = ECANCELED; break; }
    uint8_t header[46]; char name[1024];
    if (central + sizeof(header) > central_end || !read_at(fd, central, header, sizeof(header)) || u32(header) != 0x02014b50) { error = EINVAL; break; }
    size_t length = u16(header + 28);
    uint64_t next = central + 46u + length + u16(header + 30) + u16(header + 32);
    if (!length || length >= sizeof(name) || next > central_end || !read_at(fd, central + 46, name, length) || memchr(name, 0, length)) { error = EINVAL; break; }
    name[length] = 0; central = next;
    const char *base = archive_filename(name, cheats);
    if (!base) continue;
    uint32_t size = u32(header + 24), compressed = u32(header + 20), offset = u32(header + 42);
    unsigned method = u16(header + 10), flags = u16(header + 8);
    total += size;
    if (++files > SOURCE_MAX_FILES || !size || size > SOURCE_FILE_MAX || total > SOURCE_MAX_BYTES ||
        (flags & 1) || (method != 0 && method != 8) || u16(header + 34)) { error = EFBIG; break; }
    uint8_t local[30];
    if ((uint64_t)offset + sizeof(local) > central_begin || !read_at(fd, offset, local, sizeof(local)) ||
        u32(local) != 0x04034b50 || u16(local + 8) != method || u16(local + 6) != flags) { error = EINVAL; break; }
    uint64_t data = (uint64_t)offset + 30u + u16(local + 26) + u16(local + 28);
    if (data + compressed > central_begin) { error = EINVAL; break; }
    char destination[MAX_PATH];
    int written = snprintf(destination, sizeof(destination), "%s/%.200s", directory, base);
    if (written < 0 || (size_t)written >= sizeof(destination)) { error = ENAMETOOLONG; break; }
    error = extract_file(fd, data, compressed, size, u32(header + 16), method, destination, buffers, cancelled, context);
    // PS-Game-Patch ships the same XML in xml/ and xml_orbis/.
    if (error == EEXIST && duplicate_matches(destination, size, u32(header + 16), cancelled, context)) error = 0;
  }
  if (!error && (central != central_end || !files)) error = EINVAL;
  free(buffers);
  close(fd);
  return error;
}

bool sm_patch_sources_last_status(const sm_patch_source_config_t *config, sm_patch_update_status_t *status) {
  sm_ini_t doc = {0};
  bool valid = false;
  if (sm_ini_read(UPDATE_STAMP, &doc, false)) {
    const char *stamp = sm_ini_get(&doc, "updates", "checked"), *hash = sm_ini_get(&doc, "updates", "sources");
    const char *error = sm_ini_get(&doc, "updates", "error");
    char expected[24]; snprintf(expected, sizeof(expected), "%016llx", (unsigned long long)sm_patch_source_hash(config));
    if (stamp && hash && !strcmp(hash, expected)) {
      char *end; errno = 0; unsigned long long checked = strtoull(stamp, &end, 10);
      if (!errno && *stamp && !*end && checked && checked <= INT64_MAX) {
        long code = 0;
        if (error) { errno = 0; code = strtol(error, &end, 10); }
        if (!errno && (!error || !*end) && code >= 0 && code <= INT_MAX) {
          status->last_check = (time_t)checked; status->error = (int)code;
          status->sources_hash = sm_patch_source_hash(config);
          valid = true;
        }
      }
    }
  }
  sm_ini_free(&doc);
  return valid;
}

uint32_t sm_patch_sources_delay(const sm_patch_source_config_t *config) {
  if (!config->interval_hours || !config->count) return UINT32_MAX;
  sm_patch_update_status_t status = {0};
  time_t now = time(NULL);
  if (!sm_patch_sources_last_status(config, &status) || now <= 0 || status.last_check > now) return 0;
  uint64_t elapsed = (uint64_t)(now - status.last_check), interval = (uint64_t)config->interval_hours * 3600;
  return elapsed >= interval ? 0 : (uint32_t)(interval - elapsed);
}

static int archive_digest(const char *path, char hash[65], sm_patch_cancel_t cancelled, void *context) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return errno;
  uint8_t *buffer = malloc(65536), digest[SHA256_DIGEST_LENGTH];
  int error = buffer ? 0 : ENOMEM;
  SHA256_CTX sha;
  SHA256_Init(&sha);
  static const char format[] = "SM+ catalog v2\n";
  SHA256_Update(&sha, format, sizeof(format) - 1);
  while (!error) {
    if (cancelled(context)) { error = ECANCELED; break; }
    ssize_t n = read(fd, buffer, 65536);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) { error = errno; break; }
    if (!n) break;
    SHA256_Update(&sha, buffer, (size_t)n);
  }
  SHA256_Final(digest, &sha);
  close(fd); free(buffer);
  if (!error) for (size_t i = 0; i < sizeof(digest); ++i) snprintf(hash + i * 2, 3, "%02x", digest[i]);
  return error;
}

static bool source_unchanged(const sm_patch_source_t *source, const char *hash) {
  char directory[192], path[256], previous[65] = {0};
  sm_patch_sources_read_lock();
  bool same = false;
  if (sm_patch_source_current(source, directory)) {
    snprintf(path, sizeof(path), "%s/.archive-sha256", directory);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0) {
      struct stat st;
      same = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == 64 && read_at(fd, 0, previous, 64) && !strcmp(previous, hash);
      close(fd);
    }
  }
  sm_patch_sources_read_unlock();
  return same;
}

static int sync_directory(const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return errno;
  int error = fsync(fd) ? errno : 0;
  close(fd);
  return error;
}

static int publish_generation(const sm_patch_source_t *source, const char *root,
                                const char *stage, sm_patch_cancel_t cancelled, void *context) {
  char pointer[160], temporary[160], old[192] = {0};
  snprintf(pointer, sizeof(pointer), "%s/current", root);
  snprintf(temporary, sizeof(temporary), "%s/.current-new", root);
  unlink(temporary);
  int fd = open(temporary, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0666);
  if (fd < 0) return errno;
  // symlink is unavailable in this PS5 payload environment. Publish a small
  // regular file by atomic rename; it names exactly one immutable generation.
  const char *name = strrchr(stage, '/') + 1;
  int error = !write_all(fd, name, 12) || fsync(fd) ? (errno ? errno : EIO) : 0;
  if (close(fd) && !error) error = errno;
  bool retire_old = false;
  if (!error) {
    pthread_rwlock_wrlock(&g_source_lock);
    sm_patch_source_current(source, old);
    if (cancelled(context)) error = ECANCELED;
    else if (rename(temporary, pointer)) error = errno;
    if (!error) {
      int sync_error = sync_directory(root);
      retire_old = !sync_error && *old;
      if (sync_error) log_debug("  [PATCH-UPDATE] directory sync failed: %s status=%d", root, sync_error);
    }
    pthread_rwlock_unlock(&g_source_lock);
  }
  // The writer lock drained old readers; new readers use current only.
  if (retire_old) remove_generation(old);
  if (error) unlink(temporary);
  return error;
}

int sm_patch_sources_update(const sm_patch_source_config_t *config,
                            sm_patch_fetch_t fetch, sm_patch_cancel_t cancelled, void *context) {
  if (!config->count || config->count > SM_PATCH_SOURCE_MAX) return EINVAL;
  if (!ensure_directory(SM_PATCH_SOURCE_ROOT)) return errno ? errno : ENOTDIR;
  int first_error = 0;
  for (size_t i = 0; i < config->count; ++i) {
    if (cancelled(context)) return ECANCELED;
    const sm_patch_source_t *source = &config->sources[i];
    char root[128], archive[160], stage[192];
    sm_patch_source_directory(source, root);
    if (!ensure_directory(root)) return errno ? errno : ENOTDIR;
    cleanup_old_generations(source, root);
    snprintf(archive, sizeof(archive), "%s/.download", root);
    snprintf(stage, sizeof(stage), "%s/.next-XXXXXX", root);
    if (!mkdtemp(stage)) return errno;
    const char *phase = "download";
    int error = fetch(source->url, archive, context);
    char hash[65];
    if (!error) { phase = "hash archive"; error = archive_digest(archive, hash, cancelled, context); }
    bool unchanged = !error && source_unchanged(source, hash);
    if (!error && !unchanged) { phase = "extract archive"; error = extract_archive(archive, stage, source->cheats, cancelled, context); }
    if (!error && !unchanged) {
      phase = "sync generation";
      char digest_path[256]; snprintf(digest_path, sizeof(digest_path), "%s/.archive-sha256", stage);
      int fd = open(digest_path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0666);
      if (fd < 0) error = errno;
      else {
        if (!write_all(fd, hash, 64) || fsync(fd)) error = errno ? errno : EIO;
        if (close(fd) && !error) error = errno;
      }
      if (!error) error = sync_directory(stage);
    }
    unlink(archive);
    if (!error && cancelled(context)) error = ECANCELED;
    if (!error && !unchanged) {
      phase = "publish generation";
      error = publish_generation(source, root, stage, cancelled, context);
    }
    if (error || unchanged) remove_generation(stage);
    log_debug("  [PATCH-UPDATE] source=%s status=%d stage=%s", source->url, error, error ? phase : unchanged ? "unchanged" : "published");
    if (error == ECANCELED) return error;
    if (!first_error) first_error = error;
  }
  sm_ini_t doc = {0};
  char stamp[32], hash[24], code[16];
  snprintf(code, sizeof(code), "%d", first_error);
  snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
  snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)sm_patch_source_hash(config));
  bool saved = sm_ini_set(&doc, "updates", "checked", stamp) && sm_ini_set(&doc, "updates", "sources", hash) && sm_ini_set(&doc, "updates", "error", code) && sm_ini_write(UPDATE_STAMP, &doc);
  sm_ini_free(&doc);
  return first_error ? first_error : saved ? 0 : EIO;
}
