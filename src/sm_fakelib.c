#include "sm_platform.h"

#include "sm_fakelib.h"
#include "sm_config_mount.h"
#include "sm_filesystem.h"
#include "sm_gameinfo.h"
#include "sm_log.h"
#include "sm_mount_diag.h"
#include "sm_path_utils.h"
#include "sm_paths.h"
#include "sm_scan.h"
#include "sm_types.h"

#include <pthread.h>
#include <sys/time.h>
#include <time.h>

#define FAKELIB_CACHE_VERSION 3u
#define FAKELIB_CACHE_MAX_AGE_SECONDS (7u * 24u * 60u * 60u)
#define FAKELIB_CACHE_MAGIC 0x534D4643u
#define FAKELIB_CACHE_HAS_GLOBAL (1u << 0)
#define FAKELIB_CACHE_GLOBAL_PRIORITY (1u << 1)
#define FAKELIB_CACHE_FLAG_MASK                                               \
  (FAKELIB_CACHE_HAS_GLOBAL | FAKELIB_CACHE_GLOBAL_PRIORITY)

typedef struct {
  char source_path[MAX_PATH];
  char mount_path[MAX_PATH];
  const char *label;
} fakelib_layer_t;

typedef struct {
  pid_t pid;
  char mount_path[MAX_PATH];
  fakelib_layer_t layers[1];
  size_t layer_count;
} fakelib_session_t;

typedef struct {
  uint64_t xor_hash;
  uint64_t sum_hash;
  uint64_t entry_count;
} fakelib_cache_signature_t;

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t flags;
  uint32_t reserved;
  uint64_t emulator_file_count;
  fakelib_cache_signature_t game_signature;
  fakelib_cache_signature_t global_signature;
  fakelib_cache_signature_t emulator_files_signature;
  char game_path[MAX_PATH];
  char global_path[MAX_PATH];
  char emulators_path[MAX_PATH];
} fakelib_cache_manifest_t;

typedef struct {
  uint32_t flags;
  size_t emulator_file_count;
  fakelib_cache_signature_t game_signature;
  fakelib_cache_signature_t global_signature;
  fakelib_cache_signature_t emulator_files_signature;
  char game_path[MAX_PATH];
  char global_path[MAX_PATH];
  char emulators_path[MAX_PATH];
} fakelib_cache_context_t;

typedef enum {
  FAKELIB_SOURCE_NONE = 0,
  FAKELIB_SOURCE_COMPOSABLE,
  FAKELIB_SOURCE_FAKELIB2,
} fakelib_source_kind_t;

static fakelib_session_t g_fakelib_mount;
static pthread_mutex_t g_fakelib_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_fakelib_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool fakelib_session_active(void) {
  return g_fakelib_mount.layer_count > 0;
}

bool sm_fakelib_game_feature_enabled(void) {
  return runtime_config()->backport_fakelib_enabled;
}

static bool mount_fakelib_overlay(const char *title_id,
                                  const char *source_path,
                                  const char *mount_path,
                                  const char *label) {
  struct iovec overlay_iov[] = {
      IOVEC_ENTRY("fstype"), IOVEC_ENTRY("unionfs"),
      IOVEC_ENTRY("from"),   IOVEC_ENTRY(source_path),
      IOVEC_ENTRY("fspath"), IOVEC_ENTRY(mount_path),
      IOVEC_ENTRY("copymode"), IOVEC_ENTRY("transparent"),
      IOVEC_ENTRY("notime"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("fnodup"), IOVEC_ENTRY(NULL)};

  if (nmount(overlay_iov, IOVEC_SIZE(overlay_iov), 0) == 0) {
    log_debug("  [FAKELIB] %s libraries mounted for %s: %s -> %s", label,
              title_id, source_path, mount_path);
    return true;
  }

  log_debug("  [FAKELIB] %s mount failed for %s (%s -> %s): %s", label,
            title_id, source_path, mount_path, strerror(errno));
  return false;
}

static bool unmount_fakelib_overlay(const fakelib_layer_t *layer) {
  const char *mount_path = layer->mount_path;
  if (unmount(mount_path, 0) == 0 || errno == ENOENT ||
      errno == EINVAL) {
    log_debug("  [FAKELIB] %s libraries unmounted: %s -> %s", layer->label,
              layer->source_path, mount_path);
    return true;
  }

  int unmount_errno = errno;
  if (unmount_errno == EBUSY)
    sm_mount_diag_log_busy(mount_path);
  log_debug("  [FAKELIB] %s unmount deferred for %s: %s", layer->label,
            mount_path, strerror(unmount_errno));
  errno = unmount_errno;
  return false;
}

static bool track_fakelib_overlay(const char *title_id,
                                  const char *source_path,
                                  const char *mount_path,
                                  const char *label) {
  if (!mount_fakelib_overlay(title_id, source_path, mount_path, label))
    return false;

  fakelib_layer_t *layer = &g_fakelib_mount.layers[g_fakelib_mount.layer_count++];
  layer->label = label;
  (void)strlcpy(layer->source_path, source_path, sizeof(layer->source_path));
  (void)strlcpy(layer->mount_path, mount_path, sizeof(layer->mount_path));
  return true;
}

static bool resolve_sandbox_mount_path(const char *title_id,
                                       char mount_path[MAX_PATH]) {
  mount_path[0] = '\0';

  char sandbox_id[MAX_TITLE_ID];
  DIR *d = opendir("/mnt/sandbox");
  if (!d)
    return false;

  size_t title_len = strlen(title_id);
  int best_index = -1;
  bool found = false;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;
    if (strncmp(entry->d_name, title_id, title_len) != 0)
      continue;
    if (entry->d_name[title_len] != '_')
      continue;

    const char *suffix = entry->d_name + title_len + 1u;
    if (!isdigit((unsigned char)suffix[0]))
      continue;
    if (strlen(entry->d_name) >= sizeof(sandbox_id))
      continue;

    errno = 0;
    char *suffix_end = NULL;
    long idx_long = strtol(suffix, &suffix_end, 10);
    if (errno == ERANGE || !suffix_end || *suffix_end != '\0' ||
        idx_long > INT_MAX)
      continue;
    int idx = (int)idx_long;
    if (idx < best_index)
      continue;

    best_index = idx;
    (void)strlcpy(sandbox_id, entry->d_name, sizeof(sandbox_id));
    found = true;
  }

  closedir(d);
  if (!found)
    return false;

  struct stat st;
  char sandbox_root[MAX_PATH];
  snprintf(sandbox_root, sizeof(sandbox_root), "/mnt/sandbox/%s", sandbox_id);
  d = opendir(sandbox_root);
  if (!d)
    return false;

  found = false;
  while ((entry = readdir(d)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;
    if (strcmp(entry->d_name, "app0") == 0)
      continue;

    snprintf(mount_path, MAX_PATH, "%s/%s/common/lib", sandbox_root,
             entry->d_name);
    if (stat(mount_path, &st) != 0 || !S_ISDIR(st.st_mode))
      continue;

    found = true;
    break;
  }

  closedir(d);
  return found;
}

static fakelib_source_kind_t resolve_game_fakelib_source_for_path(
    const char *title_id, const char *game_path,
    char source_path[MAX_PATH]) {
  source_path[0] = '\0';

  char backport_path[MAX_PATH] = {0};
  bool has_backport =
      resolve_backport_path_for_title(title_id, NULL, backport_path);
  bool has_game = game_path && game_path[0] != '\0';
  const struct {
    const char *root;
    const char *directory;
    fakelib_source_kind_t kind;
  } candidates[] = {
      {has_backport ? backport_path : NULL, "fakelib2",
       FAKELIB_SOURCE_FAKELIB2},
      {has_backport ? backport_path : NULL, "fakelib",
       FAKELIB_SOURCE_COMPOSABLE},
      {has_game ? game_path : NULL, "fakelib", FAKELIB_SOURCE_COMPOSABLE},
  };
  struct stat st;
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
    if (!candidates[i].root)
      continue;
    int written = snprintf(source_path, MAX_PATH, "%s/%s",
                           candidates[i].root, candidates[i].directory);
    if (written > 0 && (size_t)written < MAX_PATH &&
        stat(source_path, &st) == 0 && S_ISDIR(st.st_mode)) {
      return candidates[i].kind;
    }
  }

  source_path[0] = '\0';
  return FAKELIB_SOURCE_NONE;
}

static fakelib_source_kind_t
resolve_game_fakelib_source(const char *title_id,
                            char source_path[MAX_PATH]) {
  char game_path[MAX_PATH];
  if (!read_mount_link(title_id, game_path, sizeof(game_path)))
    game_path[0] = '\0';
  return resolve_game_fakelib_source_for_path(title_id, game_path,
                                               source_path);
}

static bool path_overlaps_fakelib_cache(const char *path) {
  return path_matches_root_or_child(path, FAKELIB_CACHE_PATH) ||
         path_matches_root_or_child(FAKELIB_CACHE_PATH, path);
}

static bool resolve_global_fakelib_source(const char *title_id,
                                           char source_path[MAX_PATH]) {
  source_path[0] = '\0';

  const runtime_config_t *cfg = runtime_config();
  if (!cfg->global_fakelib_enabled)
    return false;
  if (is_global_fakelib_excluded_for_title(title_id))
    return false;

  struct stat st;
  if (stat(cfg->global_fakelib_path, &st) != 0) {
    if (errno != ENOENT)
      log_debug("  [FAKELIB] global path unavailable for %s: %s (%s)",
                title_id, cfg->global_fakelib_path, strerror(errno));
    return false;
  }
  if (!S_ISDIR(st.st_mode)) {
    log_debug("  [FAKELIB] global path is not a directory for %s: %s",
              title_id, cfg->global_fakelib_path);
    return false;
  }
  if (path_overlaps_fakelib_cache(cfg->global_fakelib_path)) {
    log_debug("  [FAKELIB] global path overlaps cache root for %s: %s",
              title_id, cfg->global_fakelib_path);
    return false;
  }

  (void)strlcpy(source_path, cfg->global_fakelib_path, MAX_PATH);
  return true;
}

static uint64_t cache_hash_bytes(uint64_t hash, const void *data, size_t size) {
  const uint8_t *bytes = (const uint8_t *)data;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

static void cache_signature_add(fakelib_cache_signature_t *signature,
                                const char *relative_path,
                                const struct stat *st) {
  uint64_t entry_hash = 1469598103934665603ULL;
  entry_hash = cache_hash_bytes(entry_hash, relative_path,
                                strlen(relative_path));
  entry_hash = cache_hash_bytes(entry_hash, &st->st_size,
                                sizeof(st->st_size));
  entry_hash = cache_hash_bytes(entry_hash, &st->st_mode,
                                sizeof(st->st_mode));
  entry_hash = cache_hash_bytes(entry_hash, &st->st_mtim.tv_sec,
                                sizeof(st->st_mtim.tv_sec));
  entry_hash = cache_hash_bytes(entry_hash, &st->st_mtim.tv_nsec,
                                sizeof(st->st_mtim.tv_nsec));
  entry_hash = cache_hash_bytes(entry_hash, &st->st_ctim.tv_sec,
                                sizeof(st->st_ctim.tv_sec));
  entry_hash = cache_hash_bytes(entry_hash, &st->st_ctim.tv_nsec,
                                sizeof(st->st_ctim.tv_nsec));
  signature->xor_hash ^= entry_hash;
  signature->sum_hash += entry_hash;
  signature->entry_count++;
}

static bool compute_tree_signature(const char *root, const char *relative_root,
                                   fakelib_cache_signature_t *signature) {
  char directory_path[MAX_PATH];
  int written = relative_root[0] == '\0'
                    ? snprintf(directory_path, sizeof(directory_path), "%s",
                               root)
                    : snprintf(directory_path, sizeof(directory_path), "%s/%s",
                               root, relative_root);
  if (written <= 0 || (size_t)written >= sizeof(directory_path))
    return false;

  DIR *d = opendir(directory_path);
  if (!d)
    return false;

  bool ok = true;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (entry->d_name[0] == '.' &&
        (entry->d_name[1] == '\0' ||
         (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
      continue;
    }

    char relative_path[MAX_PATH];
    char full_path[MAX_PATH];
    int relative_len =
        relative_root[0] == '\0'
            ? snprintf(relative_path, sizeof(relative_path), "%s",
                       entry->d_name)
            : snprintf(relative_path, sizeof(relative_path), "%s/%s",
                       relative_root, entry->d_name);
    int full_len = snprintf(full_path, sizeof(full_path), "%s/%s", root,
                            relative_path);
    if (relative_len <= 0 ||
        (size_t)relative_len >= sizeof(relative_path) || full_len <= 0 ||
        (size_t)full_len >= sizeof(full_path)) {
      ok = false;
      break;
    }

    struct stat lst;
    struct stat st;
    if (lstat(full_path, &lst) != 0) {
      ok = false;
      break;
    }
    if (S_ISLNK(lst.st_mode)) {
      if (stat(full_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        ok = false;
        break;
      }
    } else {
      st = lst;
    }
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
      ok = false;
      break;
    }
    cache_signature_add(signature, relative_path, &st);
    if (S_ISDIR(st.st_mode) &&
        !compute_tree_signature(root, relative_path, signature)) {
      ok = false;
      break;
    }
  }
  if (closedir(d) != 0)
    ok = false;
  return ok;
}

static bool compute_source_signature(
    const char *source_path, fakelib_cache_signature_t *signature) {
  memset(signature, 0, sizeof(*signature));
  return compute_tree_signature(source_path, "", signature);
}

static int directory_entry_exists(const char *directory, const char *name) {
  if (!directory)
    return 0;
  char path[MAX_PATH];
  int written = snprintf(path, sizeof(path), "%s/%s", directory, name);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  struct stat st;
  if (lstat(path, &st) == 0)
    return 1;
  return errno == ENOENT ? 0 : -1;
}

static bool compute_emulator_files_signature(
    const char *emulators_path, const char *source_path,
    const char *higher_priority_path,
    fakelib_cache_signature_t *signature, size_t *matching_count_out) {
  memset(signature, 0, sizeof(*signature));
  *matching_count_out = 0;

  DIR *d = opendir(emulators_path);
  if (!d)
    return errno == ENOENT;

  bool ok = true;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (entry->d_name[0] == '.' &&
        (entry->d_name[1] == '\0' ||
         (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
      continue;
    }

    char emulator_file[MAX_PATH];
    char source_file[MAX_PATH];
    int emulator_len = snprintf(emulator_file, sizeof(emulator_file), "%s/%s",
                                emulators_path, entry->d_name);
    int source_len = snprintf(source_file, sizeof(source_file), "%s/%s",
                              source_path, entry->d_name);
    if (emulator_len <= 0 ||
        (size_t)emulator_len >= sizeof(emulator_file) || source_len <= 0 ||
        (size_t)source_len >= sizeof(source_file)) {
      ok = false;
      break;
    }

    struct stat emulator_st;
    struct stat source_st;
    if (stat(emulator_file, &emulator_st) != 0 ||
        !S_ISREG(emulator_st.st_mode)) {
      continue;
    }
    if (stat(source_file, &source_st) != 0 || !S_ISREG(source_st.st_mode))
      continue;
    int shadowed = directory_entry_exists(higher_priority_path, entry->d_name);
    if (shadowed < 0) {
      ok = false;
      break;
    }
    if (shadowed > 0)
      continue;
    if (emulator_st.st_dev == source_st.st_dev &&
        emulator_st.st_ino == source_st.st_ino) {
      continue;
    }
    cache_signature_add(signature, entry->d_name, &emulator_st);
    (*matching_count_out)++;
  }
  if (closedir(d) != 0)
    ok = false;
  return ok;
}

static bool cache_signatures_equal(const fakelib_cache_signature_t *a,
                                   const fakelib_cache_signature_t *b) {
  return a->xor_hash == b->xor_hash && a->sum_hash == b->sum_hash &&
         a->entry_count == b->entry_count;
}

static bool remove_cache_tree(const char *path) {
  struct stat root_st;
  if (lstat(path, &root_st) != 0)
    return errno == ENOENT;
  if (!S_ISDIR(root_st.st_mode))
    return unlink(path) == 0 || errno == ENOENT;

  DIR *d = opendir(path);
  if (!d)
    return false;

  bool removed = true;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (entry->d_name[0] == '.' &&
        (entry->d_name[1] == '\0' ||
         (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
      continue;
    }

    char child_path[MAX_PATH];
    int written = snprintf(child_path, sizeof(child_path), "%s/%s", path,
                           entry->d_name);
    if (written <= 0 || (size_t)written >= sizeof(child_path)) {
      removed = false;
      break;
    }
    struct stat st;
    if (lstat(child_path, &st) != 0) {
      removed = false;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      if (!remove_cache_tree(child_path)) {
        removed = false;
        break;
      }
    } else if (unlink(child_path) != 0) {
      removed = false;
      break;
    }
  }
  if (closedir(d) != 0)
    removed = false;
  if (removed && rmdir(path) != 0 && errno != ENOENT)
    removed = false;
  return removed;
}

static bool build_cache_path(const char *title_id, const char *suffix,
                             char path[MAX_PATH]) {
  int written = snprintf(path, MAX_PATH, "%s/%s%s", FAKELIB_CACHE_PATH,
                         title_id, suffix ? suffix : "");
  return written > 0 && (size_t)written < MAX_PATH;
}

static bool cache_root_is_mounted(const char *cache_root) {
  char cache_fakelib[MAX_PATH];
  int written = snprintf(cache_fakelib, sizeof(cache_fakelib), "%s/fakelib",
                         cache_root);
  if (written <= 0 || (size_t)written >= sizeof(cache_fakelib))
    return false;

  struct statfs *mounts = NULL;
  int mount_count = getmntinfo(&mounts, MNT_NOWAIT);
  if (mount_count <= 0 || !mounts)
    return true;

  for (int i = 0; i < mount_count; ++i) {
    const char *source = mounts[i].f_mntfromname;
    if (strncmp(source, "<above>:", 8) == 0)
      source += 8;
    if (strcmp(source, cache_fakelib) == 0)
      return true;
  }
  return false;
}

static void remove_title_cache(const char *title_id) {
  char cache_root[MAX_PATH];
  if (!build_cache_path(title_id, "", cache_root))
    return;
  struct stat st;
  if (lstat(cache_root, &st) != 0)
    return;
  if (!S_ISDIR(st.st_mode)) {
    (void)unlink(cache_root);
    return;
  }

  char manifest_path[MAX_PATH];
  int written = snprintf(manifest_path, sizeof(manifest_path), "%s/manifest",
                         cache_root);
  if (written > 0 && (size_t)written < sizeof(manifest_path))
    (void)unlink(manifest_path);
  if (!cache_root_is_mounted(cache_root))
    (void)remove_cache_tree(cache_root);
}

static bool read_cache_manifest(const char *cache_root,
                                fakelib_cache_manifest_t *manifest,
                                time_t *last_used_out) {
  struct stat cache_st;
  if (lstat(cache_root, &cache_st) != 0 || !S_ISDIR(cache_st.st_mode))
    return false;

  char manifest_path[MAX_PATH];
  int written = snprintf(manifest_path, sizeof(manifest_path), "%s/manifest",
                         cache_root);
  if (written <= 0 || (size_t)written >= sizeof(manifest_path))
    return false;

  struct stat manifest_st;
  if (lstat(manifest_path, &manifest_st) != 0 ||
      !S_ISREG(manifest_st.st_mode)) {
    return false;
  }

  FILE *file = fopen(manifest_path, "rb");
  if (!file)
    return false;
  bool ok = fread(manifest, 1, sizeof(*manifest), file) == sizeof(*manifest);
  if (fclose(file) != 0)
    ok = false;
  ok = ok && manifest->magic == FAKELIB_CACHE_MAGIC &&
       manifest->version == FAKELIB_CACHE_VERSION &&
       memchr(manifest->game_path, '\0', sizeof(manifest->game_path)) &&
       memchr(manifest->global_path, '\0', sizeof(manifest->global_path)) &&
       memchr(manifest->emulators_path, '\0',
               sizeof(manifest->emulators_path));
  if (ok) {
    bool has_global = manifest->flags & FAKELIB_CACHE_HAS_GLOBAL;
    ok = (manifest->flags & ~FAKELIB_CACHE_FLAG_MASK) == 0 &&
         (!(manifest->flags & FAKELIB_CACHE_GLOBAL_PRIORITY) || has_global) &&
         manifest->game_path[0] == '/' &&
         (has_global ? manifest->global_path[0] == '/'
                     : manifest->global_path[0] == '\0') &&
         (has_global || manifest->emulator_file_count > 0);
  }
  if (ok && last_used_out)
    *last_used_out = manifest_st.st_mtime;
  return ok;
}

static bool touch_cache_manifest(const char *cache_root) {
  char manifest_path[MAX_PATH];
  int written = snprintf(manifest_path, sizeof(manifest_path), "%s/manifest",
                         cache_root);
  return written > 0 && (size_t)written < sizeof(manifest_path) &&
         utimes(manifest_path, NULL) == 0;
}

static bool write_cache_manifest(const char *cache_root,
                                 const fakelib_cache_manifest_t *manifest) {
  char manifest_path[MAX_PATH];
  int written = snprintf(manifest_path, sizeof(manifest_path), "%s/manifest",
                         cache_root);
  if (written <= 0 || (size_t)written >= sizeof(manifest_path))
    return false;

  int fd = open(manifest_path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if (fd < 0)
    return false;
  FILE *file = fdopen(fd, "wb");
  if (!file) {
    int saved_errno = errno;
    (void)close(fd);
    (void)unlink(manifest_path);
    errno = saved_errno;
    return false;
  }
  bool ok =
      fwrite(manifest, 1, sizeof(*manifest), file) == sizeof(*manifest) &&
      fflush(file) == 0 && fsync(fileno(file)) == 0;
  if (fclose(file) != 0)
    ok = false;
  if (!ok)
    (void)unlink(manifest_path);
  return ok;
}

static bool cache_manifest_matches_context(
    const fakelib_cache_manifest_t *manifest,
    const fakelib_cache_context_t *context) {
  return manifest->flags == context->flags &&
         strcmp(manifest->game_path, context->game_path) == 0 &&
         strcmp(manifest->global_path, context->global_path) == 0 &&
         strcmp(manifest->emulators_path, context->emulators_path) == 0;
}

static bool cache_manifest_is_current(
    const fakelib_cache_manifest_t *manifest,
    const fakelib_cache_context_t *context) {
  return cache_manifest_matches_context(manifest, context) &&
         manifest->emulator_file_count == context->emulator_file_count &&
         cache_signatures_equal(&manifest->game_signature,
                                &context->game_signature) &&
         cache_signatures_equal(&manifest->global_signature,
                                &context->global_signature) &&
         cache_signatures_equal(&manifest->emulator_files_signature,
                                &context->emulator_files_signature);
}

static bool copy_emulator_files_to_cache(const char *emulators_path,
                                         const char *source_path,
                                         const char *higher_priority_path,
                                         const char *cache_fakelib_path,
                                         size_t *copied_count_out) {
  *copied_count_out = 0;
  DIR *d = opendir(emulators_path);
  if (!d)
    return errno == ENOENT;

  bool ok = true;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (entry->d_name[0] == '.' &&
        (entry->d_name[1] == '\0' ||
         (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
      continue;
    }

    char emulator_file[MAX_PATH];
    char source_file[MAX_PATH];
    char cache_file[MAX_PATH];
    int emulator_len = snprintf(emulator_file, sizeof(emulator_file), "%s/%s",
                                emulators_path, entry->d_name);
    int source_len = snprintf(source_file, sizeof(source_file), "%s/%s",
                              source_path, entry->d_name);
    int cache_len = snprintf(cache_file, sizeof(cache_file), "%s/%s",
                             cache_fakelib_path, entry->d_name);
    if (emulator_len <= 0 ||
        (size_t)emulator_len >= sizeof(emulator_file) || source_len <= 0 ||
        (size_t)source_len >= sizeof(source_file) || cache_len <= 0 ||
        (size_t)cache_len >= sizeof(cache_file)) {
      ok = false;
      break;
    }

    struct stat emulator_st;
    struct stat source_st;
    if (stat(emulator_file, &emulator_st) != 0 ||
        !S_ISREG(emulator_st.st_mode)) {
      continue;
    }
    if (stat(source_file, &source_st) != 0 || !S_ISREG(source_st.st_mode))
      continue;
    int shadowed = directory_entry_exists(higher_priority_path, entry->d_name);
    if (shadowed < 0) {
      ok = false;
      break;
    }
    if (shadowed > 0)
      continue;
    if (emulator_st.st_dev == source_st.st_dev &&
        emulator_st.st_ino == source_st.st_ino) {
      continue;
    }
    if (copy_file_with_mode(emulator_file, cache_file, 0777) != 0) {
      ok = false;
      break;
    }
    (*copied_count_out)++;
  }
  if (closedir(d) != 0)
    ok = false;
  return ok;
}

static bool ensure_fakelib_cache_root(void) {
  if (mkdir("/data/shadowmount", 0777) != 0 && errno != EEXIST)
    return false;
  if (mkdir(FAKELIB_CACHE_PATH, 0777) != 0 && errno != EEXIST)
    return false;

  struct stat st;
  return lstat(FAKELIB_CACHE_PATH, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool parse_temporary_cache_title_id(
    const char *entry_name, char title_id[MAX_TITLE_ID]) {
  const char suffix[] = ".tmp";
  size_t length = strlen(entry_name);
  size_t suffix_length = sizeof(suffix) - 1u;
  if (length <= suffix_length || length - suffix_length >= MAX_TITLE_ID ||
      strcmp(entry_name + length - suffix_length, suffix) != 0) {
    return false;
  }
  size_t title_length = length - suffix_length;
  memcpy(title_id, entry_name, title_length);
  title_id[title_length] = '\0';
  return is_supported_game_title_id(title_id);
}

static void cleanup_expired_fakelib_caches(uint64_t now_sec) {
  DIR *d = opendir(FAKELIB_CACHE_PATH);
  if (!d)
    return;

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    char temporary_title_id[MAX_TITLE_ID];
    if (parse_temporary_cache_title_id(entry->d_name,
                                       temporary_title_id)) {
      char temporary_root[MAX_PATH];
      if (build_cache_path(temporary_title_id, ".tmp", temporary_root)) {
        if (remove_cache_tree(temporary_root)) {
          log_debug("  [FAKELIB] orphaned temporary cache removed: %s",
                    temporary_root);
        } else {
          log_debug("  [FAKELIB] temporary cache cleanup failed: %s",
                    temporary_root);
        }
      }
      continue;
    }
    if (!is_supported_game_title_id(entry->d_name)) {
      continue;
    }

    char cache_root[MAX_PATH];
    if (!build_cache_path(entry->d_name, "", cache_root))
      continue;
    if (cache_root_is_mounted(cache_root))
      continue;

    fakelib_cache_manifest_t manifest;
    time_t last_used = 0;
    if (!read_cache_manifest(cache_root, &manifest, &last_used)) {
      (void)remove_cache_tree(cache_root);
      continue;
    }
    if (last_used > 0 &&
        ((uint64_t)last_used > now_sec ||
         now_sec - (uint64_t)last_used <= FAKELIB_CACHE_MAX_AGE_SECONDS)) {
      continue;
    }
    if (remove_cache_tree(cache_root)) {
      log_debug("  [FAKELIB] stale cache removed: %s", cache_root);
    } else {
      log_debug("  [FAKELIB] stale cache cleanup failed: %s",
                 cache_root);
    }
  }
  closedir(d);
}

void sm_fakelib_cleanup_caches(void) {
  time_t now = time(NULL);
  if (now < 0)
    return;

  pthread_mutex_lock(&g_fakelib_cache_mutex);
  cleanup_expired_fakelib_caches((uint64_t)now);
  pthread_mutex_unlock(&g_fakelib_cache_mutex);
}

static void init_cache_context(const char *title_id, const char *game_path,
                               fakelib_cache_context_t *context) {
  memset(context, 0, sizeof(*context));
  (void)strlcpy(context->game_path, game_path, sizeof(context->game_path));

  const runtime_config_t *cfg = runtime_config();
  if (cfg->update_emulators_enabled) {
    (void)strlcpy(context->emulators_path, cfg->emulators_path,
                  sizeof(context->emulators_path));
  }

  if (resolve_global_fakelib_source(title_id, context->global_path) &&
      strcmp(context->global_path, game_path) != 0) {
    context->flags |= FAKELIB_CACHE_HAS_GLOBAL;
    if (!cfg->global_fakelib_game_priority)
      context->flags |= FAKELIB_CACHE_GLOBAL_PRIORITY;
  } else {
    context->global_path[0] = '\0';
  }
}

static bool compute_cache_context_signatures(
    fakelib_cache_context_t *context) {
  if (!compute_source_signature(context->game_path,
                                &context->game_signature)) {
    return false;
  }
  if ((context->flags & FAKELIB_CACHE_HAS_GLOBAL) &&
      !compute_source_signature(context->global_path,
                                &context->global_signature)) {
    return false;
  }
  if (context->emulators_path[0] != '\0' &&
      !compute_emulator_files_signature(
          context->emulators_path, context->game_path,
          context->flags & FAKELIB_CACHE_GLOBAL_PRIORITY
              ? context->global_path
              : NULL,
          &context->emulator_files_signature,
          &context->emulator_file_count)) {
    return false;
  }
  return true;
}

static bool cache_context_requires_cache(
    const fakelib_cache_context_t *context) {
  return (context->flags & FAKELIB_CACHE_HAS_GLOBAL) ||
         context->emulator_file_count > 0;
}

static bool rebuild_fakelib_cache(
    const char *title_id, const fakelib_cache_context_t *context) {
  char cache_root[MAX_PATH] = {0};
  char temp_root[MAX_PATH];
  if (!build_cache_path(title_id, "", cache_root) ||
      !build_cache_path(title_id, ".tmp", temp_root)) {
    return false;
  }
  if (!remove_cache_tree(temp_root)) {
    log_debug("  [FAKELIB] temporary cache cleanup failed for %s: %s",
              title_id, temp_root);
    return false;
  }
  if (mkdir(temp_root, 0777) != 0) {
    log_debug("  [FAKELIB] temporary cache creation failed for %s: %s (%s)",
              title_id, temp_root, strerror(errno));
    return false;
  }
  char temp_fakelib[MAX_PATH];
  int written = snprintf(temp_fakelib, sizeof(temp_fakelib), "%s/fakelib",
                         temp_root);
  bool has_global = context->flags & FAKELIB_CACHE_HAS_GLOBAL;
  bool global_priority = context->flags & FAKELIB_CACHE_GLOBAL_PRIORITY;
  const char *base_path = has_global && !global_priority
                              ? context->global_path
                              : context->game_path;
  bool built = written > 0 && (size_t)written < sizeof(temp_fakelib) &&
               copy_dir_with_mode(base_path, temp_fakelib, 0777) == 0;
  if (built && has_global && !global_priority) {
    built = copy_dir_with_mode(context->game_path, temp_fakelib, 0777) == 0;
  }
  size_t copied_emulator_files = 0;
  if (built && context->emulator_file_count > 0) {
    built = copy_emulator_files_to_cache(context->emulators_path,
                                         context->game_path,
                                         global_priority
                                             ? context->global_path
                                             : NULL,
                                         temp_fakelib,
                                         &copied_emulator_files) &&
            copied_emulator_files == context->emulator_file_count;
  }
  if (built && has_global && global_priority) {
    built = copy_dir_with_mode(context->global_path, temp_fakelib, 0777) == 0;
  }

  fakelib_cache_context_t current = *context;
  if (built) {
    built = compute_cache_context_signatures(&current) &&
            current.emulator_file_count == context->emulator_file_count &&
            cache_signatures_equal(&context->game_signature,
                                   &current.game_signature) &&
            cache_signatures_equal(&context->global_signature,
                                   &current.global_signature) &&
            cache_signatures_equal(&context->emulator_files_signature,
                                   &current.emulator_files_signature);
  }

  fakelib_cache_manifest_t manifest;
  if (built) {
    memset(&manifest, 0, sizeof(manifest));
    manifest.magic = FAKELIB_CACHE_MAGIC;
    manifest.version = FAKELIB_CACHE_VERSION;
    manifest.flags = context->flags;
    manifest.emulator_file_count = context->emulator_file_count;
    manifest.game_signature = context->game_signature;
    manifest.global_signature = context->global_signature;
    manifest.emulator_files_signature = context->emulator_files_signature;
    (void)strlcpy(manifest.game_path, context->game_path,
                  sizeof(manifest.game_path));
    (void)strlcpy(manifest.global_path, context->global_path,
                  sizeof(manifest.global_path));
    (void)strlcpy(manifest.emulators_path, context->emulators_path,
                  sizeof(manifest.emulators_path));
    built = write_cache_manifest(temp_root, &manifest);
  }

  if (!built) {
    log_debug("  [FAKELIB] cache build failed for %s", title_id);
    (void)remove_cache_tree(temp_root);
    return false;
  }
  if (cache_root_is_mounted(cache_root)) {
    log_debug("  [FAKELIB] cache is still mounted for %s", title_id);
    (void)remove_cache_tree(temp_root);
    return false;
  }
  if (!remove_cache_tree(cache_root) || rename(temp_root, cache_root) != 0) {
    log_debug("  [FAKELIB] cache publish failed for %s: %s", title_id,
              strerror(errno));
    (void)remove_cache_tree(temp_root);
    return false;
  }

  log_debug("  [FAKELIB] cache updated for %s: game=%s global=%s "
            "emulator_files=%u",
            title_id, context->game_path,
            has_global ? context->global_path : "none",
            (unsigned)context->emulator_file_count);
  return true;
}

static void prepare_title_cache(const char *title_id, const char *game_path) {
  if (!is_supported_game_title_id(title_id))
    return;
  const runtime_config_t *cfg = runtime_config();
  if (!cfg->backport_fakelib_enabled)
    return;

  char game_source_path[MAX_PATH];
  fakelib_source_kind_t source_kind = resolve_game_fakelib_source_for_path(
      title_id, game_path, game_source_path);
  if (source_kind != FAKELIB_SOURCE_COMPOSABLE) {
    remove_title_cache(title_id);
    return;
  }
  if (path_overlaps_fakelib_cache(game_source_path)) {
    log_debug("  [FAKELIB] refusing cache source inside cache tree for %s: %s",
              title_id, game_source_path);
    remove_title_cache(title_id);
    return;
  }

  fakelib_cache_context_t context;
  init_cache_context(title_id, game_source_path, &context);
  if (!compute_cache_context_signatures(&context)) {
    log_debug("  [FAKELIB] cache fingerprint failed for %s", title_id);
    remove_title_cache(title_id);
    return;
  }
  if (!cache_context_requires_cache(&context)) {
    remove_title_cache(title_id);
    return;
  }
  if (!ensure_fakelib_cache_root()) {
    log_debug("  [FAKELIB] cache root unavailable for %s: %s (%s)", title_id,
              FAKELIB_CACHE_PATH, strerror(errno));
    return;
  }

  char cache_root[MAX_PATH] = {0};
  bool current = false;
  if (build_cache_path(title_id, "", cache_root)) {
    fakelib_cache_manifest_t manifest;
    char cache_fakelib[MAX_PATH];
    int written = snprintf(cache_fakelib, sizeof(cache_fakelib),
                           "%s/fakelib", cache_root);
    struct stat st;
    current = written > 0 && (size_t)written < sizeof(cache_fakelib) &&
               lstat(cache_fakelib, &st) == 0 && S_ISDIR(st.st_mode) &&
               read_cache_manifest(cache_root, &manifest, NULL) &&
               cache_manifest_is_current(&manifest, &context);
  }
  if (!current) {
    if (!rebuild_fakelib_cache(title_id, &context)) {
      remove_title_cache(title_id);
    }
  } else {
    log_debug("  [FAKELIB] cache current for %s: global=%s emulator_files=%u",
              title_id,
              context.flags & FAKELIB_CACHE_HAS_GLOBAL ? "yes" : "no",
              (unsigned)context.emulator_file_count);
  }
}

void sm_fakelib_prepare_title_cache(const char *title_id,
                                    const char *game_path) {
  pthread_mutex_lock(&g_fakelib_cache_mutex);
  prepare_title_cache(title_id, game_path);
  pthread_mutex_unlock(&g_fakelib_cache_mutex);
}

static bool resolve_cached_fakelib_locked(
    const char *title_id, const char *game_path, char cache_path[MAX_PATH],
    size_t *emulator_file_count_out, bool *includes_global_out) {
  *emulator_file_count_out = 0;
  *includes_global_out = false;
  if (!is_supported_game_title_id(title_id))
    return false;

  fakelib_cache_context_t context;
  init_cache_context(title_id, game_path, &context);

  char cache_root[MAX_PATH];
  if (!build_cache_path(title_id, "", cache_root))
    return false;
  fakelib_cache_manifest_t manifest;
  if (!read_cache_manifest(cache_root, &manifest, NULL))
    return false;

  if (!cache_manifest_matches_context(&manifest, &context)) {
    return false;
  }

  int written = snprintf(cache_path, MAX_PATH, "%s/fakelib", cache_root);
  struct stat st;
  if (written <= 0 || (size_t)written >= MAX_PATH ||
      lstat(cache_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
    cache_path[0] = '\0';
    return false;
  }
  if (!touch_cache_manifest(cache_root)) {
    log_debug("  [FAKELIB] cache lifetime refresh failed for %s: %s",
              title_id, strerror(errno));
  }
  *emulator_file_count_out = (size_t)manifest.emulator_file_count;
  *includes_global_out = manifest.flags & FAKELIB_CACHE_HAS_GLOBAL;
  return true;
}

static bool resolve_cached_fakelib(const char *title_id,
                                    const char *game_path,
                                    char cache_path[MAX_PATH],
                                    size_t *emulator_file_count_out,
                                    bool *includes_global_out) {
  pthread_mutex_lock(&g_fakelib_cache_mutex);
  bool resolved = resolve_cached_fakelib_locked(
      title_id, game_path, cache_path, emulator_file_count_out,
      includes_global_out);
  pthread_mutex_unlock(&g_fakelib_cache_mutex);
  return resolved;
}

static bool cleanup_fakelib_mount(void) {
  if (!fakelib_session_active()) {
    memset(&g_fakelib_mount, 0, sizeof(g_fakelib_mount));
    return true;
  }

  while (g_fakelib_mount.layer_count > 0) {
    fakelib_layer_t *layer =
        &g_fakelib_mount.layers[g_fakelib_mount.layer_count - 1];
    if (!unmount_fakelib_overlay(layer))
      return false;
    memset(layer, 0, sizeof(*layer));
    g_fakelib_mount.layer_count--;
  }

  memset(&g_fakelib_mount, 0, sizeof(g_fakelib_mount));
  return true;
}

void sm_fakelib_game_on_exec(pid_t pid, const char *title_id,
                             bool notify_user) {
  pthread_mutex_lock(&g_fakelib_mutex);
  if (!sm_fakelib_game_feature_enabled()) {
    pthread_mutex_unlock(&g_fakelib_mutex);
    return;
  }

  if (fakelib_session_active() && g_fakelib_mount.pid == pid) {
    log_debug("  [FAKELIB] already tracking pid=%ld for %s", (long)pid,
              title_id);
    pthread_mutex_unlock(&g_fakelib_mutex);
    return;
  }

  if (fakelib_session_active() && g_fakelib_mount.pid != pid) {
    log_debug("  [FAKELIB] handoff active mount pid=%ld -> pid=%ld (%s)",
              (long)g_fakelib_mount.pid, (long)pid, title_id);
    if (!cleanup_fakelib_mount()) {
      log_debug("  [FAKELIB] handoff cleanup failed for pid=%ld, skipping %s",
                (long)g_fakelib_mount.pid, title_id);
      pthread_mutex_unlock(&g_fakelib_mutex);
      return;
    }
  }

  char game_source_path[MAX_PATH] = {0};
  char global_source_path[MAX_PATH] = {0};
  char mount_path[MAX_PATH] = {0};
  fakelib_source_kind_t source_kind =
      resolve_game_fakelib_source(title_id, game_source_path);
  bool has_game = source_kind != FAKELIB_SOURCE_NONE;
  bool allows_composition = source_kind != FAKELIB_SOURCE_FAKELIB2;
  bool has_global = allows_composition &&
                    resolve_global_fakelib_source(title_id,
                                                  global_source_path);
  if (!has_global && !has_game) {
    pthread_mutex_unlock(&g_fakelib_mutex);
    return;
  }

  bool needs_combined_cache =
      has_global && has_game &&
      strcmp(global_source_path, game_source_path) != 0;
  size_t emulator_file_count = 0;
  bool cache_resolved = false;
  bool cache_includes_global = false;
  if (source_kind == FAKELIB_SOURCE_COMPOSABLE) {
    char cache_path[MAX_PATH];
    cache_resolved = resolve_cached_fakelib(
        title_id, game_source_path, cache_path, &emulator_file_count,
        &cache_includes_global);
    if (cache_resolved) {
      (void)strlcpy(game_source_path, cache_path, sizeof(game_source_path));
      log_debug("  [FAKELIB] using cache for %s: %s", title_id,
                game_source_path);
    }
  }
  if (needs_combined_cache &&
      (!cache_resolved || !cache_includes_global)) {
    log_debug("  [FAKELIB] combined cache unavailable for %s; "
              "skipping global fakelib",
              title_id);
  }

  memset(&g_fakelib_mount, 0, sizeof(g_fakelib_mount));
  g_fakelib_mount.pid = pid;

  if (!resolve_sandbox_mount_path(title_id, mount_path)) {
    memset(&g_fakelib_mount, 0, sizeof(g_fakelib_mount));
    pthread_mutex_unlock(&g_fakelib_mutex);
    return;
  }
  (void)strlcpy(g_fakelib_mount.mount_path, mount_path,
                sizeof(g_fakelib_mount.mount_path));

  const char *source_path = has_game ? game_source_path : global_source_path;
  const char *label = !allows_composition
                          ? "fakelib2"
                          : (has_game ? "game" : "global");
  if (!track_fakelib_overlay(title_id, source_path, mount_path, label)) {
    (void)cleanup_fakelib_mount();
    pthread_mutex_unlock(&g_fakelib_mutex);
    return;
  }

  if (has_game && notify_user) {
    notify_system_info_l10n(emulator_file_count > 0
                                ? SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED
                                : SM_L10N_GAME_BACKPORTED,
                            title_id);
  }
  pthread_mutex_unlock(&g_fakelib_mutex);
}

void sm_fakelib_game_on_exit(pid_t pid) {
  pthread_mutex_lock(&g_fakelib_mutex);
  if (!fakelib_session_active() || g_fakelib_mount.pid != pid) {
    pthread_mutex_unlock(&g_fakelib_mutex);
    return;
  }

  log_debug("  [FAKELIB] game stopped: pid=%ld mount=%s", (long)pid,
            g_fakelib_mount.mount_path);
  cleanup_fakelib_mount();
  pthread_mutex_unlock(&g_fakelib_mutex);
}

void sm_fakelib_game_shutdown(void) {
  pthread_mutex_lock(&g_fakelib_mutex);
  (void)cleanup_fakelib_mount();
  pthread_mutex_unlock(&g_fakelib_mutex);
}
