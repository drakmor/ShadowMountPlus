#include "sm_platform.h"
#include "sm_shellcore_service.h"

#include "sm_fakelib.h"
#include "sm_config_mount.h"
#include "sm_filesystem.h"
#include "sm_gameinfo.h"
#include "sm_game_lifecycle.h"
#include "sm_log.h"
#include "sm_mount_diag.h"
#include "sm_path_utils.h"
#include "sm_paths.h"
#include "sm_runtime.h"
#include "sm_scan.h"
#include "sm_types.h"
#include "sm_time.h"

#include <pthread.h>
#include <sys/time.h>
#include <time.h>

#define FAKELIB_CACHE_VERSION 4u
#define FAKELIB_CACHE_MAX_AGE_SECONDS (7u * 24u * 60u * 60u)
#define FAKELIB_CACHE_MAGIC 0x534D4643u
#define SANDBOX_USB_ROOT_COUNT 8u
#define SANDBOX_EXT_ROOT_COUNT 2u
#define FAKELIB_CACHE_HAS_GLOBAL (1u << 0)
#define FAKELIB_CACHE_GLOBAL_PRIORITY (1u << 1)
#define FAKELIB_CACHE_EMULATORS_ONLY (1u << 2)
#define FAKELIB_CACHE_PACKAGE (1u << 3)
#define FAKELIB_CACHE_EXCLUSIVE (1u << 4)
#define FAKELIB_CACHE_FILTERED (1u << 5)
#define FAKELIB_CACHE_FLAG_MASK                                               \
  (FAKELIB_CACHE_HAS_GLOBAL | FAKELIB_CACHE_GLOBAL_PRIORITY | \
   FAKELIB_CACHE_EMULATORS_ONLY | FAKELIB_CACHE_PACKAGE | \
   FAKELIB_CACHE_EXCLUSIVE | FAKELIB_CACHE_FILTERED)

typedef struct {
  // An empty source records a created parent directory without a mount.
  char source_path[MAX_PATH];
  char mount_path[MAX_PATH];
  const char *label;
  bool created_mount_path;
  bool source_detached;
  bool owned_mount;
  bool mount_identity_valid;
  fsid_t mount_fsid;
  // Identify the source volume, rather than its sandbox nullfs alias.
  fsid_t source_fsid;
} fakelib_layer_t;

typedef struct {
  pid_t pid;
  char title_id[MAX_TITLE_ID];
  char sandbox_app0_path[MAX_PATH];
  char mount_path[MAX_PATH];
  // fakelib, /data, /mnt/disc, fonts, and up to three created parents.
  fakelib_layer_t layers[7u + SANDBOX_USB_ROOT_COUNT + SANDBOX_EXT_ROOT_COUNT];
  size_t layer_count;
  sm_l10n_key_t notification;
  char notification_target[MAX_PATH];
  bool notify_pending;
  bool mounts_ready;
  uint64_t cleanup_next_us;
  uint64_t cleanup_deadline_us;
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
  return runtime_config().backport_fakelib_enabled;
}

bool sm_fakelib_source_is_in_use(const char *path) {
  if (!path || !path[0])
    return false;
  char source[MAX_PATH] = {0};
  pthread_mutex_lock(&g_fakelib_mutex);
  for (size_t i = 0; i < g_fakelib_mount.layer_count; ++i) {
    const fakelib_layer_t *layer = &g_fakelib_mount.layers[i];
    if (!layer->source_path[0] || !g_fakelib_mount.mount_path[0] ||
        strcmp(layer->mount_path, g_fakelib_mount.mount_path) != 0)
      continue;
    strlcpy(source, layer->source_path, sizeof(source));
    break; // The session owns one common/lib overlay.
  }
  pthread_mutex_unlock(&g_fakelib_mutex);
  return source[0] && paths_overlap(path, source);
}

static bool is_sandbox_usb_root(const char *path) {
  return path && strncmp(path, "/mnt/usb", 8u) == 0 &&
         path[8] >= '0' && path[8] < '0' + (int)SANDBOX_USB_ROOT_COUNT &&
         path[9] == '\0';
}

static bool mount_sandbox_layer(const char *title_id,
                                const char *source_path,
                                const char *mount_path,
                                const char *label, bool overlay, int flags) {
  struct iovec directory_iov[] = {
      IOVEC_ENTRY("fstype"), IOVEC_ENTRY("nullfs"),
      IOVEC_ENTRY("from"),   IOVEC_ENTRY(source_path),
      IOVEC_ENTRY("fspath"), IOVEC_ENTRY(mount_path)};
  struct iovec overlay_iov[] = {
      IOVEC_ENTRY("fstype"), IOVEC_ENTRY("unionfs"),
      IOVEC_ENTRY("from"),   IOVEC_ENTRY(source_path),
      IOVEC_ENTRY("fspath"), IOVEC_ENTRY(mount_path),
      IOVEC_ENTRY("copymode"), IOVEC_ENTRY("transparent"),
      IOVEC_ENTRY("notime"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("fnodup"), IOVEC_ENTRY(NULL)};

  if (nmount(overlay ? overlay_iov : directory_iov,
             overlay ? IOVEC_SIZE(overlay_iov) : IOVEC_SIZE(directory_iov),
             flags) == 0) {
    log_debug("  [FAKELIB] %s mounted for %s: %s -> %s", label,
              title_id, source_path, mount_path);
    return true;
  }

  log_debug("  [FAKELIB] %s mount failed for %s (%s -> %s): %s", label,
            title_id, source_path, mount_path, strerror(errno));
  return false;
}

static bool sandbox_mount_matches(const struct statfs *fs,
                                   const char *source, bool overlay) {
  if (strcmp(fs->f_fstypename, overlay ? "unionfs" : "nullfs") != 0)
    return false;
  const char *mounted_source = fs->f_mntfromname;
  if (strncmp(mounted_source, "<above>:", 8) == 0)
    mounted_source += 8;
  // ShellCore exposes /user/data at the sandbox's /data. Borrow that nullfs
  // just like an existing /data mount, without accepting unrelated sources.
  return strcmp(mounted_source, source) == 0 ||
      (!overlay && strcmp(source, "/data") == 0 &&
       strcmp(mounted_source, "/user/data") == 0);
}

static int sandbox_mount_identity_present(const fakelib_layer_t *layer) {
  struct statfs *mounts = NULL;
  int count = sm_mount_table_snapshot(&mounts);
  if (count < 0)
    return -1;
  bool found = false;
  for (int i = 0; i < count; ++i) {
    bool matches = layer->mount_identity_valid
        ? memcmp(&mounts[i].f_fsid, &layer->mount_fsid,
                 sizeof(layer->mount_fsid)) == 0
        : strcmp(mounts[i].f_mntonname, layer->mount_path) == 0 &&
          strcmp(mounts[i].f_fstypename,
                 strcmp(layer->label, "directory") == 0 ? "nullfs" : "unionfs") == 0;
    if (matches) {
      found = true;
      break;
    }
  }
  free(mounts);
  return found;
}

// A removed USB filesystem can make nullfs statfs fail with EPIPE/EAGAIN.
// Resolve only the recorded mount, without traversing its reclaimed lower vnode.
static bool unmount_unreachable_usb_layer(fakelib_layer_t *layer) {
  struct statfs *mounts = NULL;
  int count = sm_mount_table_snapshot(&mounts);
  if (count < 0)
    return false;

  unsigned identity_mounts = 0;
  bool matches = false;
  unsigned path_mounts = 0;
  for (int i = 0; i < count; ++i) {
    bool same_path = strcmp(mounts[i].f_mntonname, layer->mount_path) == 0;
    if (same_path)
      ++path_mounts;
    if (memcmp(&mounts[i].f_fsid, &layer->mount_fsid,
               sizeof(layer->mount_fsid)) == 0) {
      ++identity_mounts;
      matches = same_path &&
          sandbox_mount_matches(&mounts[i], layer->source_path, false);
    }
  }
  free(mounts);
  if (identity_mounts == 0) {
    // The old identity is gone; a replacement path belongs to its new owner.
    layer->created_mount_path = false;
    return true;
  }
  if (!matches || identity_mounts != 1u || path_mounts != 1u) {
    errno = EBUSY;
    return false;
  }

  int32_t ids[2];
  _Static_assert(sizeof(ids) == sizeof(layer->mount_fsid), "fsid layout");
  memcpy(ids, &layer->mount_fsid, sizeof(ids));
  char fsid_path[32];
  (void)snprintf(fsid_path, sizeof(fsid_path), "FSID:%d:%d",
                 (int)ids[0], (int)ids[1]);
  // No force and no path fallback: a vanished ID cannot select a newer mount.
  if (unmount(fsid_path, MNT_BYFSID) != 0) {
    if (errno != ENOENT)
      return false;
    layer->created_mount_path = false;
    return true;
  }
  log_debug("  [FAKELIB] USB directory unmounted by fsid: %s -> %s",
            layer->source_path, layer->mount_path);
  return true;
}

static bool unmount_sandbox_layer(fakelib_layer_t *layer) {
  const char *mount_path = layer->mount_path;
  if (layer->source_path[0] && !layer->owned_mount)
    return true;

  if (layer->source_path[0]) {
    struct statfs fs;
    if (statfs(mount_path, &fs) != 0) {
      int lookup_errno = errno;
      if ((lookup_errno == EPIPE || lookup_errno == EAGAIN) &&
          layer->mount_identity_valid && layer->label &&
          strcmp(layer->label, "directory") == 0 &&
          is_sandbox_usb_root(layer->source_path)) {
        if (!unmount_unreachable_usb_layer(layer))
          return false;
      } else {
        if (lookup_errno != ENOENT && lookup_errno != ENOTDIR)
          return false;
        int present = sandbox_mount_identity_present(layer);
        if (present != 0) {
          if (present > 0)
            errno = EBUSY;
          return false;
        }
        layer->created_mount_path = false;
      }
    } else if (strcmp(fs.f_mntonname, mount_path) != 0 ||
               (layer->mount_identity_valid &&
                memcmp(&fs.f_fsid, &layer->mount_fsid,
                       sizeof(layer->mount_fsid)) != 0)) {
      // A newer mount may cover this one. Never unmount it by the old path.
      int present = sandbox_mount_identity_present(layer);
      if (present != 0) {
        if (present > 0)
          errno = EBUSY;
        return false;
      }
      layer->created_mount_path = false;
      return true;
    } else {
      if (!layer->mount_identity_valid &&
          !sandbox_mount_matches(&fs, layer->source_path,
                                 strcmp(layer->label, "directory") != 0)) {
        errno = EBUSY;
        return false;
      }
      if (unmount(mount_path, 0) != 0 && errno != ENOENT && errno != EINVAL) {
        int unmount_errno = errno;
        if (unmount_errno == EBUSY && g_fakelib_mount.cleanup_deadline_us == 0)
          sm_mount_diag_log_busy(mount_path);
        errno = unmount_errno;
        return false;
      }
      log_debug("  [FAKELIB] %s unmounted: %s -> %s", layer->label,
                layer->source_path, mount_path);
    }
    // A leftover directory is separate from an owned mount.
    layer->source_path[0] = '\0';
    layer->owned_mount = false;
  }

  if (layer->created_mount_path) {
    if (rmdir(mount_path) != 0 && errno != ENOENT) {
      if (g_fakelib_mount.cleanup_deadline_us == 0)
        log_debug("  [FAKELIB] mount directory cleanup skipped for %s: %s",
                  mount_path, strerror(errno));
      // ShellCore or another producer may have populated the directory. Its
      // contents must not keep an already-unmounted session active.
    }
  }
  return true;
}

static bool track_sandbox_layer(const char *title_id,
                                const char *source_path,
                                const char *mount_path,
                                const char *label, bool overlay, int flags) {
  if (g_fakelib_mount.layer_count >=
      sizeof(g_fakelib_mount.layers) / sizeof(g_fakelib_mount.layers[0])) {
    errno = ENOSPC;
    return false;
  }
  struct statfs fs;
  if (statfs(mount_path, &fs) != 0)
    return false;
  // unionfs intentionally covers ShellCore's native library mount. An existing
  // unionfs at that same path, however, must be reused or left to its owner.
  bool owned = strcmp(fs.f_mntonname, mount_path) != 0 ||
               (overlay && strcmp(fs.f_fstypename, "unionfs") != 0);
  if (!owned && !sandbox_mount_matches(&fs, source_path, overlay)) {
    log_debug("  [FAKELIB] sandbox mount occupied: %s (%s)", title_id,
              mount_path);
    errno = EBUSY;
    return false;
  }
  if (owned && !mount_sandbox_layer(title_id, source_path, mount_path, label,
                                   overlay, flags))
    return false;

  fakelib_layer_t *layer =
      &g_fakelib_mount.layers[g_fakelib_mount.layer_count++];
  memset(layer, 0, sizeof(*layer));
  layer->owned_mount = owned;
  layer->label = label;
  (void)strlcpy(layer->source_path, source_path, sizeof(layer->source_path));
  (void)strlcpy(layer->mount_path, mount_path, sizeof(layer->mount_path));
  if (owned) {
    // Keep the successful nmount recorded even if its identity cannot be read.
    if (statfs(mount_path, &fs) != 0)
      return false;
    // f_mntfromname can truncate long source paths. nmount already succeeded;
    // capture the target identity without requiring a full source round-trip.
    if (strcmp(fs.f_mntonname, mount_path) != 0 ||
        strcmp(fs.f_fstypename, overlay ? "unionfs" : "nullfs") != 0) {
      errno = EBUSY;
      return false;
    }
  }
  layer->mount_fsid = fs.f_fsid;
  layer->mount_identity_valid = true;
  return true;
}

static bool mount_sandbox_directory(const char *title_id,
                                   const char *app0_path,
                                   const char *source_path, int flags) {
  size_t app0_len = strlen(app0_path);
  if (app0_len < 5u || strcmp(app0_path + app0_len - 5u, "/app0") != 0) {
    errno = ENOENT;
    return false;
  }

  char mount_path[MAX_PATH];
  int written = snprintf(mount_path, sizeof(mount_path), "%.*s%s",
                         (int)(app0_len - 5u), app0_path, source_path);
  if (written < 0 || (size_t)written >= sizeof(mount_path)) {
    errno = ENAMETOOLONG;
    return false;
  }

  struct stat st;
  if (stat(source_path, &st) != 0)
    return false;
  if (!S_ISDIR(st.st_mode)) {
    errno = ENOTDIR;
    return false;
  }

  bool created = mkdir(mount_path, 0777) == 0;
  if (!created && errno != EEXIST) {
    log_debug("  [FAKELIB] mount directory creation failed for %s (%s): %s",
              title_id, mount_path, strerror(errno));
    return false;
  }
  if (!created) {
    if (lstat(mount_path, &st) != 0)
      return false;
    if (!S_ISDIR(st.st_mode)) {
      errno = ENOTDIR;
      return false;
    }
  }

  size_t previous_count = g_fakelib_mount.layer_count;
  bool mounted = track_sandbox_layer(title_id, source_path, mount_path,
                                     "directory", false, flags);
  if (g_fakelib_mount.layer_count > previous_count)
    g_fakelib_mount.layers[previous_count].created_mount_path = created;
  if (!mounted) {
    int mount_errno = errno;
    if (created && g_fakelib_mount.layer_count == previous_count)
      (void)rmdir(mount_path);
    errno = mount_errno;
    return false;
  }
  return true;
}

// Own only directories we create; cleanup removes them after their children.
static bool ensure_sandbox_parent(const char *app0_path, const char *parent) {
  size_t app0_len = strlen(app0_path);
  if (app0_len < 5u || strcmp(app0_path + app0_len - 5u, "/app0") != 0) {
    errno = ENOENT;
    return false;
  }
  if (g_fakelib_mount.layer_count >=
      sizeof(g_fakelib_mount.layers) / sizeof(g_fakelib_mount.layers[0])) {
    errno = ENOSPC;
    return false;
  }
  char path[MAX_PATH];
  int written = snprintf(path, sizeof(path), "%.*s%s",
                         (int)(app0_len - 5u), app0_path, parent);
  if (written < 0 || (size_t)written >= sizeof(path)) {
    errno = ENAMETOOLONG;
    return false;
  }
  if (mkdir(path, 0777) != 0) {
    struct stat st;
    if (errno != EEXIST || lstat(path, &st) != 0)
      return false;
    if (!S_ISDIR(st.st_mode)) {
      errno = ENOTDIR;
      return false;
    }
    return true;
  }
  fakelib_layer_t *layer =
      &g_fakelib_mount.layers[g_fakelib_mount.layer_count++];
  layer->created_mount_path = true;
  (void)strlcpy(layer->mount_path, path, sizeof(layer->mount_path));
  return true;
}

static bool mount_sandbox_fonts(const char *title_id, const char *app0_path) {
  const char *source_path = "/preinst/common/font";
  struct stat st;
  if (stat(source_path, &st) != 0)
    return errno == ENOENT;
  if (!S_ISDIR(st.st_mode)) {
    errno = ENOTDIR;
    return false;
  }

  if (!ensure_sandbox_parent(app0_path, "/preinst") ||
      !ensure_sandbox_parent(app0_path, "/preinst/common"))
    return false;

  if (mount_sandbox_directory(title_id, app0_path, source_path, MNT_RDONLY))
    return true;
  int mount_errno = errno;
  if (mount_errno == ENOENT && stat(source_path, &st) != 0 && errno == ENOENT)
    return true;
  errno = mount_errno;
  return false;
}

static bool sync_sandbox_storage_root(const char *title_id,
                                       const char *app0_path,
                                       const char *source_path, bool mounted) {
  struct statfs source_fs = {0};
  if (mounted) {
    // A removed volume may leave its empty mount directory behind.
    if (statfs(source_path, &source_fs) != 0) {
      if (errno != ENOENT)
        return false;
      mounted = false;
    } else {
      mounted = strcmp(source_fs.f_mntonname, source_path) == 0;
    }
  }

  for (size_t i = 0; i < g_fakelib_mount.layer_count; ++i) {
    fakelib_layer_t *layer = &g_fakelib_mount.layers[i];
    if (strcmp(layer->source_path, source_path) != 0 ||
        !layer->label || strcmp(layer->label, "directory") != 0)
      continue;
    if (mounted && !layer->source_detached &&
        memcmp(&layer->source_fsid, &source_fs.f_fsid,
               sizeof(layer->source_fsid)) == 0)
      return true;

    // Never reuse a busy alias when a different volume occupies this path.
    layer->source_detached = true;
    if (!layer->owned_mount && mounted) {
      errno = EBUSY;
      return false;
    }
    if (!unmount_sandbox_layer(layer))
      return false;
    --g_fakelib_mount.layer_count;
    memmove(layer, layer + 1u,
            (g_fakelib_mount.layer_count - i) * sizeof(*layer));
    memset(&g_fakelib_mount.layers[g_fakelib_mount.layer_count], 0,
           sizeof(*layer));
    break;
  }

  if (!mounted)
    return true;
  if (mount_sandbox_directory(title_id, app0_path, source_path, 0)) {
    g_fakelib_mount.layers[g_fakelib_mount.layer_count - 1u].source_fsid =
        source_fs.f_fsid;
    return true;
  }
  int mount_errno = errno;
  struct stat source_st;
  if (mount_errno == ENOENT && stat(source_path, &source_st) != 0 &&
      errno == ENOENT)
    return true;
  errno = mount_errno;
  return false;
}

static bool mount_sandbox_storage_roots(const char *title_id,
                                        const char *app0_path, bool usb_only) {
  struct statfs *mounts = NULL;
  int mount_count = sm_mount_table_snapshot(&mounts);
  if (mount_count < 0) {
    log_debug("  [FAKELIB] storage mount table unavailable for %s: %s",
              title_id, strerror(errno));
    return false;
  }

  int sync_errno = 0;
  unsigned root_count = usb_only ? SANDBOX_USB_ROOT_COUNT
      : SANDBOX_USB_ROOT_COUNT + SANDBOX_EXT_ROOT_COUNT + 1u;
  // Keep sandbox /mnt local and expose only mounted storage volumes.
  for (unsigned slot = 0; slot < root_count; ++slot) {
    char source_path[MAX_PATH];
    if (slot < SANDBOX_USB_ROOT_COUNT)
      (void)snprintf(source_path, sizeof(source_path), "/mnt/usb%u", slot);
    else if (slot < SANDBOX_USB_ROOT_COUNT + SANDBOX_EXT_ROOT_COUNT)
      (void)snprintf(source_path, sizeof(source_path), "/mnt/ext%u",
                     slot - SANDBOX_USB_ROOT_COUNT);
    else
      (void)strlcpy(source_path, "/mnt/disc", sizeof(source_path));

    bool mounted = false;
    for (int i = 0; i < mount_count; ++i) {
      if (strcmp(mounts[i].f_mntonname, source_path) == 0) {
        mounted = true;
        break;
      }
    }
    if (!sync_sandbox_storage_root(title_id, app0_path, source_path, mounted)) {
      if (!g_fakelib_mount.mounts_ready) {
        int mount_errno = errno;
        free(mounts);
        errno = mount_errno;
        return false;
      }
      // One failed/busy alias must not prevent updating the other volumes.
      if (sync_errno == 0)
        sync_errno = errno;
    }
  }

  free(mounts);
  if (sync_errno != 0) {
    errno = sync_errno;
    return false;
  }
  return true;
}

static bool resolve_sandbox_paths(const char *title_id,
                                  char app0_path[MAX_PATH],
                                  char mount_path[MAX_PATH]) {
  app0_path[0] = '\0';
  mount_path[0] = '\0';

  char sandbox_id[MAX_TITLE_ID];
  DIR *d = opendir("/mnt/sandbox");
  if (!d)
    return false;

  size_t title_len = strlen(title_id);
  bool found = false;
  struct dirent *entry;
  int read_errno = 0;
  while (true) {
    errno = 0;
    entry = readdir(d);
    if (!entry) {
      read_errno = errno;
      break;
    }
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
    // ShellCore reuses the first free suffix. An older, higher-numbered
    // directory containing only our aliases must not hide the current app0.
    char candidate[MAX_PATH];
    int length = snprintf(candidate, sizeof(candidate), "/mnt/sandbox/%s/app0",
                          entry->d_name);
    struct stat candidate_st;
    if (length < 0 || (size_t)length >= sizeof(candidate))
      continue;
    if (stat(candidate, &candidate_st) != 0) {
      int lookup_errno = errno;
      if (lookup_errno == ENOENT || lookup_errno == ENOTDIR)
        continue;
      closedir(d);
      errno = lookup_errno;
      return false;
    }
    if (!S_ISDIR(candidate_st.st_mode))
      continue;
    if (found) {
      closedir(d);
      errno = EBUSY; // Multiple live sandboxes need an explicit owner.
      return false;
    }
    (void)strlcpy(sandbox_id, entry->d_name, sizeof(sandbox_id));
    found = true;
  }

  closedir(d);
  if (read_errno != 0 || !found) {
    errno = read_errno != 0 ? read_errno : ENOENT;
    return false;
  }

  struct stat st;
  char sandbox_root[MAX_PATH];
  int written = snprintf(sandbox_root, sizeof(sandbox_root), "/mnt/sandbox/%s",
                         sandbox_id);
  if (written < 0 || (size_t)written >= sizeof(sandbox_root)) {
    errno = ENAMETOOLONG;
    return false;
  }
  written = snprintf(app0_path, MAX_PATH, "%s/app0", sandbox_root);
  if (written < 0 || (size_t)written >= MAX_PATH) {
    app0_path[0] = '\0';
    errno = ENAMETOOLONG;
    return false;
  }
  if (stat(app0_path, &st) != 0) {
    app0_path[0] = '\0';
    return false;
  }
  if (!S_ISDIR(st.st_mode)) {
    app0_path[0] = '\0';
    errno = ENOTDIR;
    return false;
  }
  d = opendir(sandbox_root);
  if (!d) {
    app0_path[0] = '\0';
    return false;
  }

  found = false;
  read_errno = 0;
  while (true) {
    errno = 0;
    entry = readdir(d);
    if (!entry) {
      read_errno = errno;
      break;
    }
    if (entry->d_name[0] == '.')
      continue;
    if (strcmp(entry->d_name, "app0") == 0)
      continue;

    written = snprintf(mount_path, MAX_PATH, "%s/%s/common/lib", sandbox_root,
                       entry->d_name);
    if (written < 0 || (size_t)written >= MAX_PATH)
      continue;
    if (stat(mount_path, &st) != 0) {
      if (errno == ENOENT || errno == ENOTDIR)
        continue;
      read_errno = errno;
      break;
    }
    if (!S_ISDIR(st.st_mode))
      continue;

    found = true;
    break;
  }

  closedir(d);
  if (read_errno != 0) {
    app0_path[0] = mount_path[0] = '\0';
    errno = read_errno;
    return false;
  }
  if (!found)
    mount_path[0] = '\0';
  return true;
}

static fakelib_source_kind_t resolve_game_fakelib_source_for_path(
    const char *title_id, const char *game_path, bool include_backport,
    char source_path[MAX_PATH], bool *backport_source_out) {
  source_path[0] = '\0';
  if (backport_source_out)
    *backport_source_out = false;

  char backport_path[MAX_PATH] = {0};
  bool has_backport = include_backport &&
      resolve_backport_path_for_title(title_id, NULL, backport_path);
  bool has_game = game_path && game_path[0] != '\0';
  const struct {
    const char *root;
    const char *directory;
    fakelib_source_kind_t kind;
    bool backport_source;
  } candidates[] = {
      {has_backport ? backport_path : NULL, "fakelib2",
       FAKELIB_SOURCE_FAKELIB2, true},
      {has_backport ? backport_path : NULL, "fakelib",
       FAKELIB_SOURCE_COMPOSABLE, true},
      {has_game ? game_path : NULL, "fakelib2", FAKELIB_SOURCE_FAKELIB2, false},
      {has_game ? game_path : NULL, "fakelib", FAKELIB_SOURCE_COMPOSABLE, false},
  };
  struct stat st;
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
    if (!candidates[i].root)
      continue;
    int written = snprintf(source_path, MAX_PATH, "%s/%s",
                           candidates[i].root, candidates[i].directory);
    if (written > 0 && (size_t)written < MAX_PATH &&
        stat(source_path, &st) == 0 && S_ISDIR(st.st_mode)) {
      if (backport_source_out)
        *backport_source_out = candidates[i].backport_source;
      return candidates[i].kind;
    }
  }

  source_path[0] = '\0';
  return FAKELIB_SOURCE_NONE;
}

static bool path_overlaps_fakelib_cache(const char *path) {
  return path_matches_root_or_child(path, FAKELIB_CACHE_PATH) ||
         path_matches_root_or_child(FAKELIB_CACHE_PATH, path);
}

static bool resolve_global_fakelib_source(const char *title_id,
                                           char source_path[MAX_PATH],
                                           const runtime_config_t *cfg) {
  source_path[0] = '\0';

  if (!cfg->global_fakelib_enabled)
    return false;
  if (sm_config_title_global_fakelib_excluded(cfg, title_id))
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
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(d);
    if (!entry) {
      if (errno != 0)
        ok = false;
      break;
    }
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
  errno = 0;
  if (lstat(path, &st) == 0)
    return 1;
  return errno == 0 || errno == ENOENT ? 0 : -1;
}

static const char *const fakelib_emulator_names[] = {
    "libSceAppContent.sprx", "libSceGameUpdate.sprx",
    "libSceNpEntitlementAccess.sprx", "libSceAmpr.sprx", "libScePlayGo.sprx"};

static bool fakelib_emulator_allowed(const char *name, uint32_t flags) {
  size_t count = flags & FAKELIB_CACHE_PACKAGE ? 3u : 5u;
  for (size_t i = 0; i < count; ++i)
    if (strcmp(name, fakelib_emulator_names[i]) == 0)
      return true;
  return false;
}

static bool compute_fakelib_signature(const char *source_path, uint32_t flags,
                                      fakelib_cache_signature_t *signature) {
  if (!(flags & FAKELIB_CACHE_EMULATORS_ONLY))
    return compute_source_signature(source_path, signature);
  memset(signature, 0, sizeof(*signature));
  size_t count = flags & FAKELIB_CACHE_PACKAGE ? 3u : 5u;
  for (size_t i = 0; i < count; ++i) {
    char path[MAX_PATH];
    int written = snprintf(path, sizeof(path), "%s/%s", source_path,
                           fakelib_emulator_names[i]);
    if (written < 0 || (size_t)written >= sizeof(path)) {
      errno = ENAMETOOLONG;
      return false;
    }
    struct stat st;
    errno = 0;
    if (stat(path, &st) != 0) {
      if (errno == 0 || errno == ENOENT)
        continue;
      return false;
    }
    if (S_ISREG(st.st_mode))
      cache_signature_add(signature, fakelib_emulator_names[i], &st);
  }
  return true;
}

static bool compute_emulator_files_signature(
    const char *emulators_path, const char *source_path,
    const char *higher_priority_path,
    uint32_t flags, fakelib_cache_signature_t *signature,
    size_t *matching_count_out) {
  memset(signature, 0, sizeof(*signature));
  *matching_count_out = 0;

  DIR *d = opendir(emulators_path);
  if (!d)
    return errno == ENOENT;

  bool ok = true;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(d);
    if (!entry) {
      if (errno != 0)
        ok = false;
      break;
    }
    if (entry->d_name[0] == '.' &&
        (entry->d_name[1] == '\0' ||
         (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
      continue;
    }

    if (strcmp(entry->d_name, "libkernel.sprx") == 0 ||
        ((flags & FAKELIB_CACHE_EMULATORS_ONLY) &&
         !fakelib_emulator_allowed(entry->d_name, flags)))
      continue;

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
    if (stat(emulator_file, &emulator_st) != 0) {
      if (errno == ENOENT || errno == ENOTDIR)
        continue;
      ok = false;
      break;
    }
    if (!S_ISREG(emulator_st.st_mode)) {
      continue;
    }
    if (stat(source_file, &source_st) != 0) {
      if (errno == ENOENT || errno == ENOTDIR)
        continue;
      ok = false;
      break;
    }
    if (!S_ISREG(source_st.st_mode))
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
  int mount_count = sm_mount_table_snapshot(&mounts);
  if (mount_count < 0)
    return true;

  for (int i = 0; i < mount_count; ++i) {
    const char *source = mounts[i].f_mntfromname;
    if (strncmp(source, "<above>:", 8) == 0)
      source += 8;
    if (strcmp(source, cache_fakelib) == 0) {
      free(mounts);
      return true;
    }
  }
  free(mounts);
  return false;
}

static void remove_title_cache(const char *title_id) {
  if (sm_shellcore_title_is_protected(title_id))
    return;
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
         (has_global || manifest->emulator_file_count > 0 ||
          (manifest->flags & FAKELIB_CACHE_FILTERED));
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
                                         uint32_t flags,
                                         size_t *copied_count_out) {
  *copied_count_out = 0;
  DIR *d = opendir(emulators_path);
  if (!d)
    return errno == ENOENT;

  bool ok = true;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(d);
    if (!entry) {
      if (errno != 0)
        ok = false;
      break;
    }
    if (entry->d_name[0] == '.' &&
        (entry->d_name[1] == '\0' ||
         (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
      continue;
    }

    char emulator_file[MAX_PATH];
    char source_file[MAX_PATH];
    char cache_file[MAX_PATH];
    if (strcmp(entry->d_name, "libkernel.sprx") == 0 ||
        ((flags & FAKELIB_CACHE_EMULATORS_ONLY) &&
         !fakelib_emulator_allowed(entry->d_name, flags)))
      continue;
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
    if (stat(emulator_file, &emulator_st) != 0) {
      if (errno == ENOENT || errno == ENOTDIR)
        continue;
      ok = false;
      break;
    }
    if (!S_ISREG(emulator_st.st_mode)) {
      continue;
    }
    if (stat(source_file, &source_st) != 0) {
      if (errno == ENOENT || errno == ENOTDIR)
        continue;
      ok = false;
      break;
    }
    if (!S_ISREG(source_st.st_mode))
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
      if (sm_shellcore_title_is_protected(temporary_title_id))
        continue;
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
    if (sm_shellcore_title_is_protected(entry->d_name))
      continue;

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

static bool init_cache_context(const char *title_id, const char *game_path,
                               bool allow_emulator_updates,
                               bool exclusive,
                               fakelib_cache_context_t *context,
                               const runtime_config_t *cfg) {
  memset(context, 0, sizeof(*context));
  (void)strlcpy(context->game_path, game_path, sizeof(context->game_path));

  if (!allow_emulator_updates)
    context->flags |= FAKELIB_CACHE_PACKAGE;
  if (exclusive)
    context->flags |= FAKELIB_CACHE_EXCLUSIVE;
  if (sm_config_title_fakelib_mode(cfg, title_id) == SM_FAKELIB_EMULATORS)
    context->flags |= FAKELIB_CACHE_EMULATORS_ONLY | FAKELIB_CACHE_FILTERED;
  if (!exclusive && allow_emulator_updates && cfg->update_emulators_enabled) {
    (void)strlcpy(context->emulators_path, cfg->emulators_path,
                  sizeof(context->emulators_path));
  }

  if (!exclusive && resolve_global_fakelib_source(title_id, context->global_path, cfg) &&
      strcmp(context->global_path, game_path) != 0) {
    context->flags |= FAKELIB_CACHE_HAS_GLOBAL;
    if (!cfg->global_fakelib_game_priority)
      context->flags |= FAKELIB_CACHE_GLOBAL_PRIORITY;
  } else {
    context->global_path[0] = '\0';
  }
  int game_kernel = directory_entry_exists(game_path, "libkernel.sprx");
  int global_kernel = directory_entry_exists(
      context->global_path[0] ? context->global_path : NULL, "libkernel.sprx");
  if (game_kernel < 0 || global_kernel < 0)
    return false;
  if (game_kernel || global_kernel)
    context->flags |= FAKELIB_CACHE_FILTERED;
  return true;
}

static bool compute_cache_context_signatures(
    fakelib_cache_context_t *context) {
  if (!compute_fakelib_signature(context->game_path, context->flags,
                                &context->game_signature)) {
    return false;
  }
  if ((context->flags & FAKELIB_CACHE_HAS_GLOBAL) &&
      !compute_fakelib_signature(context->global_path, context->flags,
                                &context->global_signature)) {
    return false;
  }
  if (context->emulators_path[0] != '\0' &&
      !compute_emulator_files_signature(
          context->emulators_path, context->game_path,
          context->flags & FAKELIB_CACHE_GLOBAL_PRIORITY
              ? context->global_path
              : NULL,
          context->flags,
          &context->emulator_files_signature,
          &context->emulator_file_count)) {
    return false;
  }
  return true;
}

static bool cache_context_requires_cache(
    const fakelib_cache_context_t *context) {
  return (context->flags & FAKELIB_CACHE_HAS_GLOBAL) ||
         context->emulator_file_count > 0 ||
         (context->flags & FAKELIB_CACHE_FILTERED);
}

static bool remove_fakelib_kernel(const char *source_path) {
  int exists = directory_entry_exists(source_path, "libkernel.sprx");
  if (exists <= 0)
    return exists == 0;
  char path[MAX_PATH];
  int written = snprintf(path, sizeof(path), "%s/libkernel.sprx", source_path);
  if (written < 0 || (size_t)written >= sizeof(path)) {
    errno = ENAMETOOLONG;
    return false;
  }
  if (unlink(path) == 0 || errno == ENOENT)
    return true;
  log_debug("  [FAKELIB] cannot remove %s: %s", path, strerror(errno));
  return false;
}

static bool copy_fakelib_source(const char *source_path, const char *destination,
                                uint32_t flags) {
  if (!(flags & FAKELIB_CACHE_EMULATORS_ONLY)) {
    if (copy_dir_with_mode(source_path, destination, 0777) != 0)
      return false;
    char kernel_path[MAX_PATH];
    int written = snprintf(kernel_path, sizeof(kernel_path),
                           "%s/libkernel.sprx", destination);
    if (written < 0 || (size_t)written >= sizeof(kernel_path)) {
      errno = ENAMETOOLONG;
      return false;
    }
    return remove_cache_tree(kernel_path);
  }
  if (mkdir(destination, 0777) != 0 && errno != EEXIST)
    return false;
  size_t count = flags & FAKELIB_CACHE_PACKAGE ? 3u : 5u;
  for (size_t i = 0; i < count; ++i) {
    char source[MAX_PATH];
    char target[MAX_PATH];
    int source_len = snprintf(source, sizeof(source), "%s/%s", source_path,
                              fakelib_emulator_names[i]);
    int target_len = snprintf(target, sizeof(target), "%s/%s", destination,
                              fakelib_emulator_names[i]);
    if (source_len < 0 || (size_t)source_len >= sizeof(source) ||
        target_len < 0 || (size_t)target_len >= sizeof(target)) {
      errno = ENAMETOOLONG;
      return false;
    }
    struct stat st;
    errno = 0;
    if (stat(source, &st) != 0) {
      if (errno == 0 || errno == ENOENT)
        continue;
      return false;
    }
    if (S_ISREG(st.st_mode) && copy_file_with_mode(source, target, 0777) != 0)
      return false;
  }
  return true;
}

static bool fakelib_content_profile(const char *path, bool *has_files,
                                    bool *emulators_only) {
  *has_files = false;
  *emulators_only = true;
  DIR *dir = opendir(path);
  if (!dir)
    return false;
  bool ok = true;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      ok = errno == 0;
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    struct stat st;
    if (fstatat(dirfd(dir), entry->d_name, &st, 0) != 0) {
      ok = false;
      break;
    }
    *has_files = true;
    if (!S_ISREG(st.st_mode) || !fakelib_emulator_allowed(entry->d_name, 0))
      *emulators_only = false;
  }
  int saved_errno = errno;
  if (closedir(dir) != 0)
    return false;
  errno = saved_errno;
  return ok;
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
               copy_fakelib_source(base_path, temp_fakelib, context->flags);
  if (built && has_global && !global_priority) {
    built = copy_fakelib_source(context->game_path, temp_fakelib, context->flags);
  }
  size_t copied_emulator_files = 0;
  if (built && context->emulator_file_count > 0) {
    built = copy_emulator_files_to_cache(context->emulators_path,
                                         context->game_path,
                                         global_priority
                                             ? context->global_path
                                             : NULL,
                                         temp_fakelib, context->flags,
                                         &copied_emulator_files) &&
            copied_emulator_files == context->emulator_file_count;
  }
  if (built && has_global && global_priority) {
    built = copy_fakelib_source(context->global_path, temp_fakelib, context->flags);
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

static void prepare_title_cache(const char *title_id, const char *game_path,
                                bool include_backport,
                                bool allow_emulator_updates,
                                const runtime_config_t *cfg) {
  if (!is_supported_game_title_id(title_id))
    return;
  if (!cfg->backport_fakelib_enabled ||
      sm_config_title_fakelib_mode(cfg, title_id) == SM_FAKELIB_DISABLED)
    return;

  char game_source_path[MAX_PATH];
  fakelib_source_kind_t source_kind = resolve_game_fakelib_source_for_path(
      title_id, game_path, include_backport, game_source_path, NULL);
  bool exclusive = source_kind == FAKELIB_SOURCE_FAKELIB2;
  if (source_kind == FAKELIB_SOURCE_NONE &&
      !resolve_global_fakelib_source(title_id, game_source_path, cfg)) {
    remove_title_cache(title_id);
    return;
  }
  if (path_overlaps_fakelib_cache(game_source_path)) {
    log_debug("  [FAKELIB] refusing cache source inside cache tree for %s: %s",
              title_id, game_source_path);
    remove_title_cache(title_id);
    return;
  }
  if (allow_emulator_updates && !is_under_image_mount_base(game_path)) {
    if (!remove_fakelib_kernel(game_source_path))
      return;
    char global_path[MAX_PATH];
    if (!exclusive && resolve_global_fakelib_source(title_id, global_path, cfg) &&
        !remove_fakelib_kernel(global_path))
      return;
  }
  fakelib_cache_context_t context;
  if (!init_cache_context(title_id, game_source_path, allow_emulator_updates,
                          exclusive, &context, cfg))
    return;
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
  const runtime_config_t cfg = runtime_config();
  pthread_mutex_lock(&g_fakelib_cache_mutex);
  prepare_title_cache(title_id, game_path, true, true, &cfg);
  pthread_mutex_unlock(&g_fakelib_cache_mutex);
}

static bool resolve_cached_fakelib_locked(
    const char *title_id, const char *game_path, bool allow_emulator_updates,
    char cache_path[MAX_PATH],
    size_t *emulator_file_count_out, bool *includes_global_out,
    bool exclusive, bool *required_out, const runtime_config_t *cfg) {
  *emulator_file_count_out = 0;
  *includes_global_out = false;
  *required_out = true;
  if (!is_supported_game_title_id(title_id))
    return false;

  fakelib_cache_context_t context;
  if (!init_cache_context(title_id, game_path, allow_emulator_updates,
                          exclusive, &context, cfg))
    return false;
  *required_out = (context.flags & FAKELIB_CACHE_FILTERED) != 0;

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
                                    bool allow_emulator_updates,
                                    char cache_path[MAX_PATH],
                                    size_t *emulator_file_count_out,
                                    bool *includes_global_out,
                                    bool exclusive, bool *required_out,
                                    const runtime_config_t *cfg) {
  pthread_mutex_lock(&g_fakelib_cache_mutex);
  bool resolved = resolve_cached_fakelib_locked(
      title_id, game_path, allow_emulator_updates, cache_path,
      emulator_file_count_out,
      includes_global_out, exclusive, required_out, cfg);
  pthread_mutex_unlock(&g_fakelib_cache_mutex);
  return resolved;
}

static bool cleanup_fakelib_mount(void) {
  if (!fakelib_session_active()) {
    memset(&g_fakelib_mount, 0, sizeof(g_fakelib_mount));
    return true;
  }

  g_fakelib_mount.mounts_ready = false;
  int cleanup_errno = 0;
  bool removed[sizeof(g_fakelib_mount.layers) /
               sizeof(g_fakelib_mount.layers[0])] = {false};
  for (size_t i = g_fakelib_mount.layer_count; i > 0; --i) {
    fakelib_layer_t *layer = &g_fakelib_mount.layers[i - 1u];
    size_t length = strlen(layer->mount_path);
    bool dependent = false;
    for (size_t j = 0; j < g_fakelib_mount.layer_count; ++j) {
      const fakelib_layer_t *other = &g_fakelib_mount.layers[j];
      if (removed[j] || other == layer)
        continue;
      if ((strncmp(other->mount_path, layer->mount_path, length) == 0 &&
           other->mount_path[length] == '/') ||
          (strncmp(other->source_path, layer->mount_path, length) == 0 &&
           (other->source_path[length] == '/' ||
            other->source_path[length] == '\0'))) {
        dependent = true;
        break;
      }
    }
    if (dependent)
      continue;
    if (!unmount_sandbox_layer(layer)) {
      // Retry only transient busy failures. Preserve any real error
      // even when another independent mount is merely busy.
      int error = errno != 0 ? errno : EIO;
      if (cleanup_errno == 0 || cleanup_errno == EBUSY)
        cleanup_errno = error;
      continue;
    }
    removed[i - 1u] = true;
  }
  // Preserve dependency order and compact retained layers once per pass.
  size_t retained = 0;
  for (size_t i = 0; i < g_fakelib_mount.layer_count; ++i) {
    if (removed[i])
      continue;
    if (retained != i)
      g_fakelib_mount.layers[retained] = g_fakelib_mount.layers[i];
    ++retained;
  }
  memset(&g_fakelib_mount.layers[retained], 0,
         (g_fakelib_mount.layer_count - retained) *
             sizeof(g_fakelib_mount.layers[0]));
  g_fakelib_mount.layer_count = retained;
  if (g_fakelib_mount.layer_count > 0) {
    errno = cleanup_errno != 0 ? cleanup_errno : EBUSY;
    return false;
  }
  memset(&g_fakelib_mount, 0, sizeof(g_fakelib_mount));
  return true;
}

static void schedule_fakelib_cleanup(void) {
  int cleanup_errno = errno;
  if (runtime_sleep_mode_active() || should_stop_requested()) {
    g_fakelib_mount.cleanup_next_us = 0;
    return;
  }
  uint64_t now_us = monotonic_time_us();
  if (cleanup_errno != EBUSY) {
    g_fakelib_mount.cleanup_next_us = 0;
    log_debug("  [FAKELIB] cleanup retained for %s: %s",
              g_fakelib_mount.title_id, strerror(cleanup_errno));
    return;
  }
  if (g_fakelib_mount.cleanup_deadline_us == 0 && now_us != 0) {
    g_fakelib_mount.cleanup_deadline_us =
        now_us + GAME_SANDBOX_CLEANUP_TIMEOUT_US;
    log_debug("  [FAKELIB] busy sandbox cleanup scheduled: %s",
              g_fakelib_mount.title_id);
  }
  if (now_us == 0 || now_us >= g_fakelib_mount.cleanup_deadline_us) {
    g_fakelib_mount.cleanup_next_us = 0;
    log_debug("  [FAKELIB] sandbox cleanup wait ended; retaining owned layers: %s",
              g_fakelib_mount.title_id);
    return;
  }
  uint64_t next_us = now_us + GAME_LIFECYCLE_POLL_INTERVAL_US;
  bool needs_wake = g_fakelib_mount.cleanup_next_us == 0;
  g_fakelib_mount.cleanup_next_us =
      next_us < g_fakelib_mount.cleanup_deadline_us
          ? next_us : g_fakelib_mount.cleanup_deadline_us;
  if (needs_wake)
    wake_game_lifecycle_watcher();
}

uint64_t sm_fakelib_game_next_cleanup_us(void) {
  pthread_mutex_lock(&g_fakelib_mutex);
  uint64_t next_us = g_fakelib_mount.cleanup_next_us;
  pthread_mutex_unlock(&g_fakelib_mutex);
  return next_us;
}

void sm_fakelib_game_poll_cleanup(void) {
  pthread_mutex_lock(&g_fakelib_mutex);
  if (runtime_sleep_mode_active() || should_stop_requested()) {
    g_fakelib_mount.cleanup_next_us = 0;
    pthread_mutex_unlock(&g_fakelib_mutex);
    return;
  }
  uint64_t now_us = monotonic_time_us();
  if (g_fakelib_mount.cleanup_next_us != 0 &&
      (now_us == 0 || now_us >= g_fakelib_mount.cleanup_next_us)) {
    if (!cleanup_fakelib_mount())
      schedule_fakelib_cleanup();
  }
  pthread_mutex_unlock(&g_fakelib_mutex);
}

static bool cleanup_ppr_backport_fakelib(const char *title_id,
                                       const char *sandbox_app0_path) {
  char backport_path[MAX_PATH];
  static const char internal_backport_root[] =
      DEFAULT_BACKPORT_SCAN_PATH "/" DEFAULT_BACKPORTS_DIR_NAME "/";
  if (!resolve_backport_path_for_title(title_id, NULL, backport_path) ||
      strncmp(backport_path, internal_backport_root,
              sizeof(internal_backport_root) - 1u) != 0 ||
      !backport_name_matches_title(
          title_id, backport_path + sizeof(internal_backport_root) - 1u)) {
    return true;
  }

  struct statfs fs;
  if (statfs(sandbox_app0_path, &fs) != 0 ||
      strcmp(fs.f_fstypename, "nsfs") != 0 ||
      strcmp(fs.f_mntonname, sandbox_app0_path) != 0) {
    return true;
  }
  char package_path[MAX_PATH];
  (void)snprintf(package_path, sizeof(package_path),
                 "/mnt/sandbox/pfsmnt/%s-app0", title_id);
  if (statfs(package_path, &fs) != 0 ||
      strcmp(fs.f_fstypename, "ppr_pfs") != 0 ||
      strcmp(fs.f_mntonname, package_path) != 0) {
    return true;
  }

  int backport_fd = open(backport_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (backport_fd < 0) {
    int saved_errno = errno;
    if (saved_errno == ENOENT)
      return true;
    log_debug("  [FAKELIB] cannot open PPR backport %s: %s",
              backport_path, strerror(saved_errno));
    errno = saved_errno;
    return false;
  }
  int fakelib_fd = openat(backport_fd, "fakelib",
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  int saved_errno = errno;
  close(backport_fd);
  if (fakelib_fd < 0) {
    if (saved_errno == ENOENT)
      return true;
    log_debug("  [FAKELIB] cannot open PPR fakelib %s/fakelib: %s",
              backport_path, strerror(saved_errno));
    errno = saved_errno;
    return false;
  }

  static const char *const filenames[] = {
      "libSceAmpr.sprx", "libScePlayGo.sprx"};
  size_t removed = 0;
  for (size_t i = 0; i < sizeof(filenames) / sizeof(filenames[0]); ++i) {
    // Skip absent targets before requesting a write on the backing filesystem.
    struct stat st;
    errno = 0;
    if (fstatat(fakelib_fd, filenames[i], &st, AT_SYMLINK_NOFOLLOW) != 0) {
      saved_errno = errno;
      // PPR may fail lookup of an absent entry without setting errno.
      if (saved_errno == 0 || saved_errno == ENOENT)
        continue;
    } else {
      errno = 0;
      if (unlinkat(fakelib_fd, filenames[i], 0) == 0) {
        removed++;
        continue;
      }
      saved_errno = errno != 0 ? errno : EIO;
      if (saved_errno == ENOENT)
        continue;
    }
    close(fakelib_fd);
    log_debug("  [FAKELIB] cannot clean up PPR library %s/fakelib/%s: %s",
              backport_path, filenames[i], strerror(saved_errno));
    errno = saved_errno;
    return false;
  }
  close(fakelib_fd);
  if (removed > 0) {
    log_debug("  [FAKELIB] removed %zu PPR libraries from %s/fakelib",
              removed, backport_path);
  }
  return true;
}

static bool mount_fakelib_for_game_locked(pid_t pid, const char *title_id,
                                          bool notify_user) {
  if (!title_id || !is_supported_game_title_id(title_id)) {
    errno = EINVAL;
    return false;
  }

  // A confirmed EXEC owns this title's retained layers before any filesystem
  // lookup. Supersede the dead/pre-spawn owner's retry even if app0 is gone.
  if (pid > 0 && fakelib_session_active() &&
      strcmp(g_fakelib_mount.title_id, title_id) == 0) {
    g_fakelib_mount.pid = pid;
    g_fakelib_mount.cleanup_next_us = 0;
    g_fakelib_mount.cleanup_deadline_us = 0;
  }

  // Recheck under the session lock: a pre-spawn request may have waited behind
  // suspend/shutdown cleanup after its service-side sleep check.
  if (runtime_sleep_mode_active() || should_stop_requested()) {
    errno = EBUSY;
    return false;
  }

  char managed_game_path[MAX_PATH] = {0};
  const runtime_config_t cfg = runtime_config();
  bool fakelib_enabled = cfg.backport_fakelib_enabled &&
      sm_config_title_fakelib_mode(&cfg, title_id) != SM_FAKELIB_DISABLED;
  bool managed_title =
      read_mount_link(title_id, managed_game_path, sizeof(managed_game_path));
  char sandbox_app0_path[MAX_PATH] = {0};
  char mount_path[MAX_PATH] = {0};
  bool sandbox_resolved = false;

  if (!managed_title) {
    sandbox_resolved =
        resolve_sandbox_paths(title_id, sandbox_app0_path, mount_path);
  }

  // Sandbox-ready mounts the overlay before the game process exists.  The
  // subsequent NOTE_EXEC only needs to attach the PID to that session; avoid
  // fingerprinting and rebuilding the package cache a second time.
  if (fakelib_session_active() && g_fakelib_mount.mounts_ready &&
      strcmp(g_fakelib_mount.title_id, title_id) == 0) {
    if (!sandbox_resolved) {
      sandbox_resolved =
          resolve_sandbox_paths(title_id, sandbox_app0_path, mount_path);
    }
    if (sandbox_resolved && sandbox_app0_path[0] != '\0' &&
        strcmp(g_fakelib_mount.sandbox_app0_path, sandbox_app0_path) == 0 &&
        strcmp(g_fakelib_mount.mount_path, mount_path) == 0) {
      if (pid > 0)
        g_fakelib_mount.pid = pid;
      bool notify = pid > 0 && notify_user && g_fakelib_mount.notify_pending;
      if (pid > 0)
        g_fakelib_mount.notify_pending = false;
      if (notify) {
        notify_system_info_l10n(g_fakelib_mount.notification,
                               g_fakelib_mount.notification_target);
      }
      return true;
    }
  }

  if (fakelib_enabled) {
    pthread_mutex_lock(&g_fakelib_cache_mutex);
    if (!managed_title && sandbox_resolved &&
        !cleanup_ppr_backport_fakelib(title_id, sandbox_app0_path)) {
      int saved_errno = errno;
      pthread_mutex_unlock(&g_fakelib_cache_mutex);
      errno = saved_errno;
      return false;
    }
    prepare_title_cache(title_id,
                        managed_title ? managed_game_path : sandbox_app0_path,
                        true, managed_title, &cfg);
    pthread_mutex_unlock(&g_fakelib_cache_mutex);
  }

  char game_source_path[MAX_PATH] = {0};
  char global_source_path[MAX_PATH] = {0};
  const char *game_path = managed_title ? managed_game_path
                                        : sandbox_app0_path;
  bool backport_source = false;
  fakelib_source_kind_t source_kind =
      fakelib_enabled ? resolve_game_fakelib_source_for_path(
                            title_id, game_path, true, game_source_path,
                            &backport_source)
                      : FAKELIB_SOURCE_NONE;
  bool has_game = source_kind != FAKELIB_SOURCE_NONE;
  bool allows_composition = source_kind != FAKELIB_SOURCE_FAKELIB2;
  bool has_global = fakelib_enabled && allows_composition &&
                    resolve_global_fakelib_source(title_id,
                                                  global_source_path, &cfg);

  bool needs_combined_cache =
      has_global && has_game &&
      strcmp(global_source_path, game_source_path) != 0;
  bool has_source = has_game || has_global;
  const char *original_source_path =
      has_game ? game_source_path : global_source_path;
  const char *source_path = original_source_path;
  size_t emulator_file_count = 0;
  bool cache_resolved = false;
  bool cache_includes_global = false;
  char cache_path[MAX_PATH];
  if (has_source) {
    bool required = false;
    cache_resolved = resolve_cached_fakelib(
        title_id, original_source_path,
        managed_title, cache_path,
        &emulator_file_count, &cache_includes_global, !allows_composition, &required, &cfg);
    if (cache_resolved) {
      source_path = cache_path;
      log_debug("  [FAKELIB] using cache for %s: %s", title_id,
                cache_path);
    } else if (required) {
      log_debug("  [FAKELIB] filtered cache unavailable for %s; "
                "refusing unfiltered overlay", title_id);
      errno = EIO;
      return false;
    }
  }
  if (needs_combined_cache &&
      (!cache_resolved || !cache_includes_global)) {
    log_debug("  [FAKELIB] combined cache unavailable for %s; "
              "skipping global fakelib",
              title_id);
  }

  bool emulators_only = false;
  if (has_source) {
    bool has_files;
    if (!fakelib_content_profile(source_path, &has_files, &emulators_only))
      return false;
    has_source = has_files;
  }
  if (!sandbox_resolved) {
    sandbox_resolved =
        resolve_sandbox_paths(title_id, sandbox_app0_path, mount_path);
  }
  if (!sandbox_resolved)
    return false;
  if (sandbox_app0_path[0] == '\0' || (has_source && mount_path[0] == '\0')) {
    errno = ENOENT;
    return false;
  }

  if (fakelib_session_active()) {
    log_debug("  [FAKELIB] rebuilding sandbox mounts: %s pid=%ld",
              title_id, (long)pid);
    if (!cleanup_fakelib_mount()) {
      log_debug("  [FAKELIB] handoff cleanup failed for pid=%ld, skipping %s",
                (long)g_fakelib_mount.pid, title_id);
      return false;
    }
  }

  memset(&g_fakelib_mount, 0, sizeof(g_fakelib_mount));
  g_fakelib_mount.pid = pid;
  (void)strlcpy(g_fakelib_mount.title_id, title_id,
                sizeof(g_fakelib_mount.title_id));
  (void)strlcpy(g_fakelib_mount.sandbox_app0_path, sandbox_app0_path,
                sizeof(g_fakelib_mount.sandbox_app0_path));
  (void)strlcpy(g_fakelib_mount.mount_path, mount_path,
                sizeof(g_fakelib_mount.mount_path));
  const char *label = !allows_composition
                          ? "fakelib2"
                          : (has_game ? "game" : "global");
  if ((has_source && !track_sandbox_layer(title_id, source_path, mount_path,
                                        label, true, 0)) ||
      !ensure_sandbox_parent(sandbox_app0_path, "/mnt") ||
      !mount_sandbox_directory(title_id, sandbox_app0_path, "/data", 0) ||
      !mount_sandbox_storage_roots(title_id, sandbox_app0_path, false) ||
      !mount_sandbox_fonts(title_id, sandbox_app0_path)) {
    int mount_errno = errno;
    if (!cleanup_fakelib_mount())
      schedule_fakelib_cleanup();
    errno = mount_errno;
    return false;
  }
  g_fakelib_mount.mounts_ready = true;

  if (!has_source || (!has_game && !emulators_only))
    return true;

  g_fakelib_mount.notification =
      emulators_only ? SM_L10N_GAME_EMULATORS_APPLIED
                     : (emulator_file_count > 0
                            ? SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED
                            : SM_L10N_GAME_BACKPORTED);
  // Capture the chosen origin before NOTE_EXEC: standalone backports show their
  // root directory, while libraries from the game itself show just its title.
  (void)strlcpy(g_fakelib_mount.notification_target,
                emulators_only || !backport_source ? title_id
                                                  : original_source_path,
                sizeof(g_fakelib_mount.notification_target));
  if (backport_source && !emulators_only) {
    char *separator = strrchr(g_fakelib_mount.notification_target, '/');
    if (separator && separator != g_fakelib_mount.notification_target)
      *separator = '\0';
  }
  g_fakelib_mount.notify_pending = pid <= 0;
  if (notify_user)
    notify_system_info_l10n(g_fakelib_mount.notification,
                           g_fakelib_mount.notification_target);
  return true;
}

bool sm_fakelib_game_on_sandbox_ready(const char *title_id) {
  if (!title_id || !is_supported_game_title_id(title_id))
    return true;

  pthread_mutex_lock(&g_fakelib_mutex);
  bool ready = mount_fakelib_for_game_locked(0, title_id, false);
  pthread_mutex_unlock(&g_fakelib_mutex);
  return ready;
}

bool sm_fakelib_game_set_usb_suspended(bool suspended) {
  int sync_errno = 0;
  pthread_mutex_lock(&g_fakelib_mutex);
  // Sleep may have ended while the lifecycle watcher waited for game exit.
  // Reject a stale phase before touching any aliases.
  if (runtime_sleep_mode_active() != suspended || should_stop_requested()) {
    pthread_mutex_unlock(&g_fakelib_mutex);
    errno = EBUSY;
    return false;
  }

  if (suspended) {
    // USB aliases are siblings. Unmount and compact in one pass, preserving
    // the order and ownership of all other layers and any failed aliases.
    size_t retained = 0;
    for (size_t i = 0; i < g_fakelib_mount.layer_count; ++i) {
      fakelib_layer_t *layer = &g_fakelib_mount.layers[i];
      bool usb_alias = layer->owned_mount && layer->label &&
          strcmp(layer->label, "directory") == 0 &&
          is_sandbox_usb_root(layer->source_path);
      if (usb_alias && unmount_sandbox_layer(layer))
        continue;
      if (usb_alias) {
        // Do not mask a real failure with an earlier transient busy error.
        if (sync_errno == 0 || sync_errno == EBUSY)
          sync_errno = errno != 0 ? errno : EIO;
      }
      if (retained != i)
        g_fakelib_mount.layers[retained] = *layer;
      ++retained;
    }
    memset(&g_fakelib_mount.layers[retained], 0,
           (g_fakelib_mount.layer_count - retained) *
               sizeof(g_fakelib_mount.layers[0]));
    g_fakelib_mount.layer_count = retained;
  } else if (g_fakelib_mount.mounts_ready &&
             !mount_sandbox_storage_roots(g_fakelib_mount.title_id,
                                          g_fakelib_mount.sandbox_app0_path,
                                          true)) {
    sync_errno = errno != 0 ? errno : EIO;
  }
  // Exit or rollback can leave a busy core layer after removing every USB
  // alias. Its retained dependencies must still block USB source cleanup.
  if (sync_errno == 0 && fakelib_session_active() &&
      !g_fakelib_mount.mounts_ready)
    sync_errno = EBUSY;
  if (sync_errno != 0)
    log_debug("  [FAKELIB] USB sandbox %s incomplete for %s: %s",
              suspended ? "suspend" : "resume", g_fakelib_mount.title_id,
              strerror(sync_errno));
  pthread_mutex_unlock(&g_fakelib_mutex);
  if (sync_errno != 0)
    errno = sync_errno;
  return sync_errno == 0;
}

void sm_fakelib_game_on_usb_mount_change(const char *source_path, bool mounted) {
  if (!is_sandbox_usb_root(source_path))
    return;

  pthread_mutex_lock(&g_fakelib_mutex);
  if (!g_fakelib_mount.mounts_ready || runtime_sleep_mode_active() ||
      should_stop_requested()) {
    pthread_mutex_unlock(&g_fakelib_mutex);
    return;
  }

  if (!sync_sandbox_storage_root(g_fakelib_mount.title_id,
                                  g_fakelib_mount.sandbox_app0_path,
                                  source_path, mounted)) {
    log_debug("  [FAKELIB] USB sandbox update failed for %s (%s): %s",
              g_fakelib_mount.title_id, source_path, strerror(errno));
  }
  pthread_mutex_unlock(&g_fakelib_mutex);
}

void sm_fakelib_game_refresh_storage(void) {
  pthread_mutex_lock(&g_fakelib_mutex);
  if (g_fakelib_mount.mounts_ready && !runtime_sleep_mode_active() &&
      !should_stop_requested() &&
      !mount_sandbox_storage_roots(g_fakelib_mount.title_id,
                                    g_fakelib_mount.sandbox_app0_path, false)) {
    log_debug("  [FAKELIB] sandbox storage refresh failed for %s: %s",
              g_fakelib_mount.title_id, strerror(errno));
  }
  pthread_mutex_unlock(&g_fakelib_mutex);
}

void sm_fakelib_game_on_launch_failed(const char *title_id) {
  if (!title_id)
    return;

  pthread_mutex_lock(&g_fakelib_mutex);
  if (fakelib_session_active() && g_fakelib_mount.pid <= 0 &&
      strcmp(g_fakelib_mount.title_id, title_id) == 0) {
    if (!cleanup_fakelib_mount())
      schedule_fakelib_cleanup();
  }
  pthread_mutex_unlock(&g_fakelib_mutex);
}

void sm_fakelib_game_on_exec(pid_t pid, const char *title_id,
                             bool notify_user) {
  if (pid <= 0 || !title_id || !is_supported_game_title_id(title_id))
    return;

  pthread_mutex_lock(&g_fakelib_mutex);
  (void)mount_fakelib_for_game_locked(pid, title_id, notify_user);
  pthread_mutex_unlock(&g_fakelib_mutex);
}

void sm_fakelib_game_on_exit(pid_t pid) {
  pthread_mutex_lock(&g_fakelib_mutex);
  if (!fakelib_session_active() || g_fakelib_mount.pid != pid) {
    pthread_mutex_unlock(&g_fakelib_mutex);
    return;
  }

  log_debug("  [FAKELIB] game stopped: pid=%ld title=%s layers=%u",
            (long)pid, g_fakelib_mount.title_id,
            (unsigned)g_fakelib_mount.layer_count);
  if (!cleanup_fakelib_mount())
    schedule_fakelib_cleanup();
  pthread_mutex_unlock(&g_fakelib_mutex);
}

void sm_fakelib_game_shutdown(void) {
  pthread_mutex_lock(&g_fakelib_mutex);
  (void)cleanup_fakelib_mount();
  g_fakelib_mount.cleanup_next_us = 0;
  g_fakelib_mount.cleanup_deadline_us = 0;
  pthread_mutex_unlock(&g_fakelib_mutex);
}
