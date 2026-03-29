#include "sm_platform.h"
#include "sm_runtime.h"
#include "sm_filesystem.h"
#include "sm_config_mount.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_image_cache.h"
#include "sm_image.h"
#include "sm_path_utils.h"
#include "sm_paths.h"

// --- FILESYSTEM ---
bool is_installed(const char *title_id) {
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s/%s", APP_BASE, title_id);
  struct stat st;
  return (stat(path, &st) == 0);
}

bool has_appmeta_data(const char *title_id) {
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s/%s/param.json", APPMETA_BASE, title_id);
  struct stat st;
  return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

bool is_data_mounted(const char *title_id) {
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "/system_ex/app/%s/sce_sys/param.json",
           title_id);
  return path_exists(path);
}

static bool read_mount_link_file(const char *lnk_path, char *out,
                                 size_t out_size) {
  if (!lnk_path || out_size == 0)
    return false;
  out[0] = '\0';

  FILE *f = fopen(lnk_path, "r");
  if (!f)
    return false;

  if (!fgets(out, (int)out_size, f)) {
    fclose(f);
    out[0] = '\0';
    return false;
  }
  fclose(f);

  size_t len = strcspn(out, "\r\n");
  out[len] = '\0';
  return out[0] != '\0';
}

static void build_title_link_path(const char *title_id, const char *filename,
                                  char out[MAX_PATH]) {
  snprintf(out, MAX_PATH, "%s/%s/%s", APP_BASE, title_id, filename);
}

typedef struct {
  char mount_link[MAX_PATH];
  char staged_mount_link[MAX_PATH];
  char mount_image_link[MAX_PATH];
} title_link_paths_t;

static void build_title_link_paths(const char *title_id,
                                   title_link_paths_t *paths) {
  build_title_link_path(title_id, "mount.lnk", paths->mount_link);
  build_title_link_path(title_id, "mount.lnk.cleanup",
                        paths->staged_mount_link);
  build_title_link_path(title_id, "mount_img.lnk", paths->mount_image_link);
}

typedef bool (*title_app_dir_iter_fn)(const char *title_id,
                                      const title_link_paths_t *paths,
                                      void *ctx);

static void for_each_title_app_dir(const char *open_error_context,
                                   bool stop_on_signal,
                                   title_app_dir_iter_fn fn, void *ctx) {
  DIR *d = opendir(APP_BASE);
  if (!d) {
    if (errno != ENOENT)
      log_debug("  [LINK] open %s failed%s: %s", APP_BASE,
                open_error_context ? open_error_context : "", strerror(errno));
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (stop_on_signal && should_stop_requested())
      break;
    if (!resolve_title_app_dir(entry, NULL, 0))
      continue;

    title_link_paths_t paths;
    build_title_link_paths(entry->d_name, &paths);
    if (!fn(entry->d_name, &paths, ctx))
      break;
  }

  closedir(d);
}

bool read_mount_link(const char *title_id, char *out, size_t out_size) {
  if (out_size == 0)
    return false;
  out[0] = '\0';

  title_link_paths_t paths;
  build_title_link_paths(title_id, &paths);
  return read_mount_link_file(paths.mount_link, out, out_size);
}

bool resolve_title_app_dir(const struct dirent *entry, char *app_dir,
                           size_t app_dir_size) {
  if (!entry)
    return false;
  if (entry->d_name[0] == '.')
    return false;
  if (strlen(entry->d_name) != 9)
    return false;

  char path_buf[MAX_PATH];
  char *path = path_buf;
  size_t path_size = sizeof(path_buf);
  if (app_dir && app_dir_size > 0) {
    path = app_dir;
    path_size = app_dir_size;
  }

  int written = snprintf(path, path_size, "%s/%s", APP_BASE, entry->d_name);
  if (written < 0 || (size_t)written >= path_size) {
    if (app_dir && app_dir_size > 0)
      app_dir[0] = '\0';
    return false;
  }
  if (entry->d_type != DT_DIR) {
    if (entry->d_type != DT_UNKNOWN)
      return false;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
      return false;
  }

  return true;
}

static bool source_path_needs_cleanup(const char *source_path,
                                      bool *tried_image_recovery) {
  if (is_under_image_mount_base(source_path)) {
    char image_source_path[MAX_PATH];
    bool has_image_source = resolve_image_source_from_mount_cache(
        source_path, image_source_path, sizeof(image_source_path));
    if (has_image_source && !path_exists(image_source_path))
      return true;
  }

  if (!path_exists(source_path))
    return true;
  if (path_matches_root_or_child(source_path, "/system_ex/app"))
    return true;

  char eboot_path[MAX_PATH];
  snprintf(eboot_path, sizeof(eboot_path), "%s/eboot.bin", source_path);
  if (path_exists(eboot_path))
    return false;

  if (!*tried_image_recovery && is_under_image_mount_base(source_path)) {
    cleanup_stale_image_mounts();
    *tried_image_recovery = true;
  }

  return !path_exists(eboot_path);
}

static bool resolve_mount_image_source_path(const title_link_paths_t *paths,
                                            const char *source_path,
                                            char image_source_path[MAX_PATH]) {
  image_source_path[0] = '\0';
  if (!is_under_image_mount_base(source_path))
    return false;

  if (read_mount_link_file(paths->mount_image_link, image_source_path, MAX_PATH)) {
    (void)cache_image_source_mapping(image_source_path, source_path);
    return true;
  }

  return resolve_image_source_from_mount_cache(source_path, image_source_path,
                                               MAX_PATH);
}

static bool cleanup_staged_mount_links_entry(const char *title_id,
                                             const title_link_paths_t *paths,
                                             void *ctx) {
  (void)title_id;
  (void)ctx;

  struct stat staged_st;
  if (stat(paths->staged_mount_link, &staged_st) == 0) {
    struct stat link_st;
    if (stat(paths->mount_link, &link_st) == 0) {
      if (unlink(paths->staged_mount_link) == 0) {
        log_debug("  [LINK] removed stale staged mount link: %s",
                  paths->staged_mount_link);
      } else if (errno != ENOENT) {
        log_debug("  [LINK] staged cleanup failed for %s: %s",
                  paths->staged_mount_link, strerror(errno));
      }
    } else if (errno != ENOENT) {
      log_debug("  [LINK] staged cleanup failed for %s: %s",
                paths->staged_mount_link, strerror(errno));
    } else if (rename(paths->staged_mount_link, paths->mount_link) == 0) {
      log_debug("  [LINK] restored interrupted mount link: %s",
                paths->mount_link);
    } else {
      log_debug("  [LINK] restore failed for %s: %s", paths->mount_link,
                strerror(errno));
      return true;
    }
  } else if (errno != ENOENT) {
    log_debug("  [LINK] staged cleanup failed for %s: %s",
              paths->staged_mount_link, strerror(errno));
    return true;
  }

  char source_path[MAX_PATH];
  if (!read_mount_link_file(paths->mount_link, source_path, sizeof(source_path))) {
    if (unlink(paths->mount_image_link) != 0 && errno != ENOENT) {
      log_debug("  [LINK] remove failed for %s: %s", paths->mount_image_link,
                strerror(errno));
    }
    return true;
  }

  if (!is_under_image_mount_base(source_path)) {
    if (unlink(paths->mount_image_link) != 0 && errno != ENOENT) {
      log_debug("  [LINK] remove failed for %s: %s", paths->mount_image_link,
                strerror(errno));
    }
    return true;
  }

  char image_source_path[MAX_PATH];
  if (!read_mount_link_file(paths->mount_image_link, image_source_path,
                            sizeof(image_source_path))) {
    return true;
  }
  if (!cache_image_source_mapping(image_source_path, source_path)) {
    log_debug("  [LINK] image source cache warmup failed: %s -> %s",
              source_path, image_source_path);
  }
  return true;
}

void cleanup_staged_mount_links(void) {
  for_each_title_app_dir(" for staged cleanup", false,
                         cleanup_staged_mount_links_entry, NULL);
}

static bool get_top_mount(const char *path, struct statfs *mount_st_out) {
  if (statfs(path, mount_st_out) == 0 &&
      strcmp(mount_st_out->f_mntonname, path) == 0) {
    return true;
  }

  struct statfs *mntbuf = NULL;
  int mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);
  if (mntcount <= 0 || !mntbuf)
    return false;

  const struct statfs *best_mount = NULL;
  for (int i = 0; i < mntcount; i++) {
    if (strcmp(mntbuf[i].f_mntonname, path) != 0)
      continue;
    if (!best_mount ||
        (strcmp(mntbuf[i].f_fstypename, "unionfs") == 0 &&
         strcmp(best_mount->f_fstypename, "unionfs") != 0)) {
      best_mount = &mntbuf[i];
    }
  }

  if (!best_mount)
    return false;

  *mount_st_out = *best_mount;
  return true;
}

typedef enum {
  TITLE_STACK_TOP_NONE = 0,
  TITLE_STACK_TOP_OTHER,
  TITLE_STACK_TOP_NULLFS,
  TITLE_STACK_TOP_BACKPORT,
} title_stack_top_kind_t;

typedef struct {
  bool mounted;
  bool has_our_nullfs;
  bool has_nullfs_from_root;
  bool has_our_backport;
  bool top_is_our_nullfs;
  int our_nullfs_count;
  int our_backport_count;
  title_stack_top_kind_t top_kind;
  char system_ex_path[MAX_PATH];
  char top_from[MAX_PATH];
} title_mount_state_t;

static bool path_is_managed_backport_for_title(const char *title_id,
                                               const char *path) {
  if (strncmp(path, "<above>:", 8) == 0)
    path += 8;

  char root[MAX_PATH];
  int scan_path_count = get_scan_path_count();
  for (int i = 0; i < scan_path_count; i++) {
    if (!build_backports_root_path(get_scan_path(i), root))
      continue;
    size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0)
      continue;
    if (path[root_len] != '/')
      continue;
    if (strcmp(path + root_len + 1u, title_id) == 0)
      return true;
  }

  return false;
}

static bool inspect_title_stack(const char *title_id, const char *source_path,
                                const char *source_root,
                                title_mount_state_t *state_out) {
  memset(state_out, 0, sizeof(*state_out));
  snprintf(state_out->system_ex_path, sizeof(state_out->system_ex_path),
           "/system_ex/app/%s", title_id);

  struct statfs statfs_mount;
  bool statfs_ok = false;
  int inspect_errno = 0;
  int statfs_res = statfs(state_out->system_ex_path, &statfs_mount);
  if (statfs_res == 0 &&
      strcmp(statfs_mount.f_mntonname, state_out->system_ex_path) == 0) {
    statfs_ok = true;
  } else if (statfs_res != 0) {
    inspect_errno = errno;
  }

  struct statfs *mntbuf = NULL;
  int mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);
  const struct statfs *best_mount = NULL;
  bool found_mount_entry = false;
  if (mntcount > 0 && mntbuf) {
    for (int i = 0; i < mntcount; i++) {
      if (strcmp(mntbuf[i].f_mntonname, state_out->system_ex_path) != 0)
        continue;

      found_mount_entry = true;
      if (!best_mount ||
          (strcmp(mntbuf[i].f_fstypename, "unionfs") == 0 &&
           strcmp(best_mount->f_fstypename, "unionfs") != 0)) {
        best_mount = &mntbuf[i];
      }

      if (strcmp(mntbuf[i].f_fstypename, "nullfs") == 0) {
        if (source_path && source_path[0] != '\0' &&
            strcmp(mntbuf[i].f_mntfromname, source_path) == 0) {
          state_out->has_our_nullfs = true;
          state_out->our_nullfs_count++;
        }
        if (source_root && source_root[0] != '\0' &&
            path_matches_root_or_child(mntbuf[i].f_mntfromname, source_root)) {
          state_out->has_nullfs_from_root = true;
        }
        continue;
      }

      if (strcmp(mntbuf[i].f_fstypename, "unionfs") == 0 &&
          path_is_managed_backport_for_title(title_id,
                                             mntbuf[i].f_mntfromname)) {
        state_out->has_our_backport = true;
        state_out->our_backport_count++;
      }
    }
  }

  const struct statfs *top_mount = NULL;
  if (statfs_ok) {
    top_mount = &statfs_mount;
  } else if (best_mount) {
    top_mount = best_mount;
    if (inspect_errno == EEXIST) {
      log_debug("  [LINK] inspect recovered from statfs(EEXIST) for %s",
                state_out->system_ex_path);
    }
  } else if (inspect_errno != 0 && inspect_errno != ENOENT &&
             inspect_errno != EINVAL) {
    log_debug("  [LINK] inspect statfs failed for %s: %s",
              state_out->system_ex_path, strerror(inspect_errno));
    if (mntcount <= 0 || !mntbuf) {
      log_debug("  [LINK] mount table unavailable for %s",
                state_out->system_ex_path);
    } else if (!found_mount_entry) {
      log_debug("  [LINK] no mount entries for %s", state_out->system_ex_path);
    }
    for (int i = 0; i < mntcount; i++) {
      if (strcmp(mntbuf[i].f_mntonname, state_out->system_ex_path) != 0)
        continue;
      log_debug("  [LINK] mount entry for %s: type=%s from=%s flags=0x%lX",
                state_out->system_ex_path, mntbuf[i].f_fstypename,
                mntbuf[i].f_mntfromname, (unsigned long)mntbuf[i].f_flags);
    }
    errno = inspect_errno;
    return false;
  }

  if (!top_mount)
    return true;

  const char *top_from = top_mount->f_mntfromname;
  if (strcmp(top_mount->f_fstypename, "unionfs") == 0 &&
      strncmp(top_from, "<above>:", 8) == 0) {
    top_from += 8;
  }

  state_out->mounted = true;
  (void)strlcpy(state_out->top_from, top_from, sizeof(state_out->top_from));
  state_out->top_kind = TITLE_STACK_TOP_OTHER;
  if (strcmp(top_mount->f_fstypename, "nullfs") == 0) {
    state_out->top_kind = TITLE_STACK_TOP_NULLFS;
    if (source_path && source_path[0] != '\0' &&
        strcmp(top_from, source_path) == 0) {
      state_out->top_is_our_nullfs = true;
      state_out->has_our_nullfs = true;
    }
  } else if (strcmp(top_mount->f_fstypename, "unionfs") == 0 &&
             path_is_managed_backport_for_title(title_id, top_from)) {
    state_out->top_kind = TITLE_STACK_TOP_BACKPORT;
    state_out->has_our_backport = true;
  }

  return true;
}

static bool unmount_top_controlled_layer(const char *path) {
  struct statfs mount_st;
  if (!get_top_mount(path, &mount_st))
    return true;

  bool controlled_top =
      (strcmp(mount_st.f_fstypename, "unionfs") == 0) ||
      (strcmp(mount_st.f_fstypename, "nullfs") == 0);
  if (!controlled_top) {
    log_debug("  [LINK] mount point busy for %s: type=%s from=%s flags=0x%lX",
              path, mount_st.f_fstypename, mount_st.f_mntfromname,
              (unsigned long)mount_st.f_flags);
    return false;
  }

  if (unmount(path, 0) == 0 || errno == ENOENT || errno == EINVAL)
    return true;
  if (unmount(path, MNT_FORCE) == 0 || errno == ENOENT || errno == EINVAL)
    return true;

  log_debug("  [LINK] unmount failed for %s: %s", path, strerror(errno));
  return false;
}

bool reconcile_title_backport_mount(const char *title_id, const char *src_path,
                                    const char *expected_backport_path,
                                    bool *overlay_active_out) {
  if (overlay_active_out)
    *overlay_active_out = false;
  if (!title_id || title_id[0] == '\0' || !src_path || src_path[0] == '\0' ||
      !expected_backport_path || expected_backport_path[0] == '\0') {
    return false;
  }

  struct stat backport_st;
  bool backport_present = stat(expected_backport_path, &backport_st) == 0 &&
                          S_ISDIR(backport_st.st_mode);

  title_mount_state_t state;
  if (!inspect_title_stack(title_id, src_path, NULL, &state)) {
    return false;
  }
  if (!state.has_our_nullfs)
    return false;

  if (state.top_is_our_nullfs)
    return backport_present;

  if (!state.has_our_backport || state.top_kind != TITLE_STACK_TOP_BACKPORT)
    return false;

  if (strcmp(state.top_from, expected_backport_path) == 0 && backport_present) {
    if (overlay_active_out)
      *overlay_active_out = true;
    return true;
  }

  if (!unmount_top_controlled_layer(state.system_ex_path))
    return false;
  log_debug("  [IMG] backport overlay removed: %s -> %s", state.top_from,
            state.system_ex_path);
  return backport_present;
}

void mount_backport_overlay(const char *mount_point,
                            const char *backport_path,
                            const char *title_id) {
  struct stat backport_st;
  if (stat(backport_path, &backport_st) != 0 || !S_ISDIR(backport_st.st_mode))
    return;

  struct statfs mounted_sfs;
  if (statfs(mount_point, &mounted_sfs) != 0 ||
      strcmp(mounted_sfs.f_mntonname, mount_point) != 0) {
    return;
  }
  if (strcmp(mounted_sfs.f_fstypename, "unionfs") == 0)
    return;

  bool mount_read_only = ((mounted_sfs.f_flags & MNT_RDONLY) != 0);
  struct iovec overlay_iov[] = {
      IOVEC_ENTRY("fstype"), IOVEC_ENTRY("unionfs"),
      IOVEC_ENTRY("from"),   IOVEC_ENTRY(backport_path),
      IOVEC_ENTRY("fspath"), IOVEC_ENTRY(mount_point),
      IOVEC_ENTRY("copymode"), IOVEC_ENTRY("transparent"),
      IOVEC_ENTRY("notime"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("fnodup"), IOVEC_ENTRY(NULL)};
  int overlay_flags = mount_read_only ? MNT_RDONLY : 0;
  if (nmount(overlay_iov, IOVEC_SIZE(overlay_iov), overlay_flags) == 0) {
    log_debug("  [IMG] backport overlay mounted (%s): %s -> %s",
              mount_read_only ? "ro" : "rw", backport_path, mount_point);
    return;
  }

  int overlay_err = errno;
  log_debug("  [IMG] backport overlay failed: %s -> %s (%s)", backport_path,
            mount_point, strerror(overlay_err));
  notify_system("Backport overlay failed: %s\n%s\n0x%08X", title_id,
                backport_path, (uint32_t)overlay_err);
}

static bool unmount_controlled_mount_stack(const char *path) {
  for (int i = 0; i < MAX_LAYERED_UNMOUNT_ATTEMPTS * 4; i++) {
    struct statfs mount_st;
    if (!get_top_mount(path, &mount_st))
      return true;
    if (!unmount_top_controlled_layer(path))
      return false;
  }

  struct statfs mount_st;
  return !get_top_mount(path, &mount_st);
}

static bool cleanup_duplicate_title_mounts_entry(const char *title_id,
                                                 const title_link_paths_t *paths,
                                                 void *ctx) {
  (void)ctx;

  char source_path[MAX_PATH];
  if (!read_mount_link_file(paths->mount_link, source_path, sizeof(source_path)))
    return true;

  title_mount_state_t state;
  if (!inspect_title_stack(title_id, source_path, NULL, &state))
    return true;
  if ((state.our_nullfs_count <= 1 && state.our_backport_count <= 1) ||
      !state.has_our_nullfs) {
    return true;
  }

  log_debug(
      "  [LINK] duplicate managed mount layers for %s: nullfs=%d backport=%d. "
      "Resetting stack.",
      state.system_ex_path, state.our_nullfs_count, state.our_backport_count);
  if (!unmount_controlled_mount_stack(state.system_ex_path)) {
    log_debug("  [LINK] failed to reset duplicate mount stack for %s",
              state.system_ex_path);
  }
  return true;
}

void cleanup_duplicate_title_mounts(void) {
  for_each_title_app_dir(" for duplicate cleanup", false,
                         cleanup_duplicate_title_mounts_entry, NULL);
}

// --- Copy Helpers for Install Action ---
static int copy_param_json_rewrite(const char *src, const char *dst) {
  FILE *fs = fopen(src, "rb");
  if (!fs)
    return -1;

  if (fseek(fs, 0, SEEK_END) != 0) {
    fclose(fs);
    return -1;
  }
  long file_size = ftell(fs);
  if (file_size < 0) {
    fclose(fs);
    return -1;
  }
  rewind(fs);

  size_t len = (size_t)file_size;
  char *buf = (char *)malloc(len + 1);
  if (!buf) {
    fclose(fs);
    return -1;
  }
  if (len > 0 && fread(buf, 1, len, fs) != len) {
    free(buf);
    fclose(fs);
    return -1;
  }
  fclose(fs);
  buf[len] = '\0';

  char *hit = strstr(buf, "upgradable");
  if (hit) {
    size_t offset = (size_t)(hit - buf);
    size_t tail = len - offset - 10u;
    memcpy(hit, "standard", 8u);
    memmove(hit + 8u, hit + 10u, tail + 1u);
    len -= 2u;
  }

  FILE *fd = fopen(dst, "wb");
  if (!fd) {
    free(buf);
    return -1;
  }

  int ret = 0;
  if (len > 0 && fwrite(buf, 1, len, fd) != len)
    ret = -1;
  if (fclose(fd) != 0)
    ret = -1;

  if (ret == 0 && hit)
    log_debug("  [COPY] param.json patched: %s", dst);

  free(buf);
  return ret;
}

int copy_file(const char *src, const char *dst) {
  if (strstr(src, "/sce_sys/param.json")) {
    return copy_param_json_rewrite(src, dst);
  }

  char buf[8192];
  FILE *fs = fopen(src, "rb");
  if (!fs)
    return -1;
  FILE *fd = fopen(dst, "wb");
  if (!fd) {
    fclose(fs);
    return -1;
  }
  int ret = 0;
  while (true) {
    size_t n = fread(buf, 1, sizeof(buf), fs);
    if (n > 0 && fwrite(buf, 1, n, fd) != n) {
      ret = -1;
      break;
    }
    if (n < sizeof(buf)) {
      if (ferror(fs))
        ret = -1;
      break;
    }
  }
  if (fflush(fd) != 0)
    ret = -1;
  if (fclose(fd) != 0)
    ret = -1;
  if (fclose(fs) != 0)
    ret = -1;
  return ret;
}

int copy_dir(const char *src, const char *dst) {
  if (mkdir(dst, 0777) != 0 && errno != EEXIST)
    return -1;
  DIR *d = opendir(src);
  if (!d)
    return -1;
  int ret = 0;
  struct dirent *e;
  char ss[MAX_PATH], dd[MAX_PATH];
  struct stat st;
  struct stat lst;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    snprintf(ss, sizeof(ss), "%s/%s", src, e->d_name);
    snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);
    if (lstat(ss, &lst) != 0) {
      ret = -1;
      break;
    }
    if (S_ISLNK(lst.st_mode)) {
      if (stat(ss, &st) != 0) {
        ret = -1;
        break;
      }
      if (S_ISDIR(st.st_mode)) {
        log_debug("  [COPY] refusing symlink directory: %s", ss);
        ret = -1;
        break;
      }
    } else {
      st = lst;
    }
    if (S_ISDIR(st.st_mode)) {
      if (copy_dir(ss, dd) != 0) {
        ret = -1;
        break;
      }
    } else {
      if (copy_file(ss, dd) != 0) {
        ret = -1;
        break;
      }
    }
  }
  if (closedir(d) != 0)
    ret = -1;
  return ret;
}

int remount_system_ex(void) {
  struct iovec iov[] = {
      IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
      IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
      IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
      IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
      IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
      IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL)};
  return nmount(iov, IOVEC_SIZE(iov), MNT_UPDATE);
}

bool mount_title_nullfs(const char *title_id, const char *src_path) {
  char dst[MAX_PATH];
  char src_eboot[MAX_PATH];
  char dst_eboot[MAX_PATH];
  snprintf(dst, sizeof(dst), "/system_ex/app/%s", title_id);
  snprintf(src_eboot, sizeof(src_eboot), "%s/eboot.bin", src_path);
  snprintf(dst_eboot, sizeof(dst_eboot), "%s/eboot.bin", dst);

  struct stat src_st;
  if (stat(src_path, &src_st) != 0) {
    log_debug("  [LINK] source path check failed for %s -> %s: %s", title_id,
              src_path, strerror(errno));
    return false;
  }
  if (!S_ISDIR(src_st.st_mode)) {
    log_debug("  [LINK] source path is not a directory for %s: %s mode=0%o",
              title_id, src_path, (unsigned)(src_st.st_mode & 077777));
    return false;
  }
  if (!path_exists(src_eboot)) {
    bool tried_image_recovery = false;
    if (source_path_needs_cleanup(src_path, &tried_image_recovery)) {
      log_debug("  [LINK] source eboot.bin missing for %s: %s", title_id,
                src_eboot);
      return false;
    }
  }

  int mkdir_res = mkdir(dst, 0755);
  if (mkdir_res != 0) {
    if (errno != EEXIST) {
      log_debug("  [LINK] Failed to create mount directory for title %s: %s",
                title_id, strerror(errno));
      return false;
    }
    struct stat dst_st;
    if (stat(dst, &dst_st) != 0) {
      log_debug("  [LINK] mount directory exists but stat failed for %s: %s",
                dst, strerror(errno));
      return false;
    }
    if (!S_ISDIR(dst_st.st_mode)) {
      log_debug("  [LINK] mount target is not a directory: %s mode=0%o", dst,
                (unsigned)(dst_st.st_mode & 077777));
      return false;
    }
  }

  title_mount_state_t state;
  if (!inspect_title_stack(title_id, src_path, NULL, &state)) {
    log_debug("  [LINK] inspect failed for %s (%s): %s", title_id, dst,
              strerror(errno));
    return false;
  }
  if (state.mounted) {
    if (state.has_our_nullfs &&
        (state.top_is_our_nullfs || state.has_our_backport) &&
        path_exists(dst_eboot)) {
      log_debug("  [LINK] mount stack already active: %s -> %s", src_path, dst);
      return true;
    }
    if (!unmount_controlled_mount_stack(dst)) {
      log_debug("  [LINK] failed to reset mount stack for %s (%s)", title_id,
                dst);
      return false;
    }
  }

  struct iovec iov[] = {
      IOVEC_ENTRY("fstype"), IOVEC_ENTRY("nullfs"),
      IOVEC_ENTRY("from"), IOVEC_ENTRY(src_path),
      IOVEC_ENTRY("fspath"), IOVEC_ENTRY(dst),
  };

  if (nmount(iov, IOVEC_SIZE(iov), 0) != 0) {
    log_debug("  [LINK] Failed to auto-mount nullfs title=%s src=%s dst=%s: %s",
              title_id, src_path, dst, strerror(errno));
    return false;
  }

  if (!path_exists(dst_eboot)) {
    log_debug("  [LINK] mounted nullfs but eboot.bin is missing at target: %s",
              dst_eboot);
    if (unmount(dst, 0) != 0 && errno != ENOENT && errno != EINVAL) {
      if (unmount(dst, MNT_FORCE) != 0 && errno != ENOENT && errno != EINVAL) {
        log_debug("  [LINK] failed to rollback empty nullfs mount %s: %s", dst,
                  strerror(errno));
      }
    }
    return false;
  }
  log_debug("  [LINK] nullfs mounted: %s -> %s", src_path, dst);
  return true;
}

bool path_matches_root_or_child(const char *path, const char *root) {
  if (!path || !root || root[0] == '\0')
    return false;
  size_t root_len = strlen(root);
  if (strncmp(path, root, root_len) != 0)
    return false;
  return (path[root_len] == '\0' || path[root_len] == '/');
}

typedef struct {
  const char *removed_source_root;
  bool unmount_system_ex_bind;
  bool tried_image_recovery;
} cleanup_mount_links_ctx_t;

static bool cleanup_mount_links_entry(const char *title_id,
                                      const title_link_paths_t *paths,
                                      void *ctx_ptr) {
  cleanup_mount_links_ctx_t *ctx = (cleanup_mount_links_ctx_t *)ctx_ptr;

  struct stat lst;
  if (stat(paths->mount_link, &lst) != 0 || !S_ISREG(lst.st_mode))
    return true;

  char source_path[MAX_PATH];
  char image_source_path[MAX_PATH];
  bool has_image_source = false;
  bool should_remove = false;
  bool matches_removed_source = false;
  if (!read_mount_link_file(paths->mount_link, source_path, sizeof(source_path))) {
    should_remove = true;
  } else {
    has_image_source =
        resolve_mount_image_source_path(paths, source_path, image_source_path);
  }

  if (!should_remove) {
    if (ctx->removed_source_root && ctx->removed_source_root[0] != '\0') {
      matches_removed_source =
          path_matches_root_or_child(source_path, ctx->removed_source_root) ||
          (has_image_source &&
           path_matches_root_or_child(image_source_path,
                                      ctx->removed_source_root));
      if (!matches_removed_source)
        return true;
      should_remove = (has_image_source && !path_exists(image_source_path)) ||
                      source_path_needs_cleanup(source_path,
                                                &ctx->tried_image_recovery);
    } else {
      should_remove = (has_image_source && !path_exists(image_source_path)) ||
                      source_path_needs_cleanup(source_path,
                                                &ctx->tried_image_recovery);
    }
  }

  if (!should_remove)
    return true;

  bool keep_mount_link = false;
  bool mount_link_staged = false;
  if (ctx->unmount_system_ex_bind) {
    title_mount_state_t state;
    bool inspected = inspect_title_stack(title_id, source_path,
                                         ctx->removed_source_root, &state);
    bool top_is_ours = state.top_is_our_nullfs || state.has_our_backport;
    bool stack_matches_link = false;
    if (source_path[0] != '\0') {
      stack_matches_link = state.has_our_nullfs;
    } else if (matches_removed_source && ctx->removed_source_root &&
               ctx->removed_source_root[0] != '\0') {
      stack_matches_link = state.has_nullfs_from_root;
    }

    if (inspected && stack_matches_link && !top_is_ours) {
      keep_mount_link = true;
      log_debug("  [LINK] keeping mount link for %s: stack still active but "
                "top layer is not ours", state.system_ex_path);
    } else if (inspected && top_is_ours && stack_matches_link) {
      if (rename(paths->mount_link, paths->staged_mount_link) != 0) {
        log_debug("  [LINK] stage failed for %s: %s", paths->mount_link,
                  strerror(errno));
        return true;
      }
      mount_link_staged = true;
      if (!unmount_controlled_mount_stack(state.system_ex_path)) {
        keep_mount_link = true;
        log_debug("  [LINK] failed to unmount mount stack for %s",
                  state.system_ex_path);
      }
    }
  }

  if (!keep_mount_link) {
    int unlink_res = mount_link_staged ? unlink(paths->staged_mount_link)
                                       : unlink(paths->mount_link);
    if (unlink_res == 0 || errno == ENOENT) {
      log_debug("  [LINK] removed stale mount link: %s", paths->mount_link);
      if (unlink(paths->mount_image_link) != 0 && errno != ENOENT) {
        log_debug("  [LINK] remove failed for %s: %s", paths->mount_image_link,
                  strerror(errno));
      }
    } else {
      log_debug("  [LINK] remove failed for %s: %s",
                mount_link_staged ? paths->staged_mount_link : paths->mount_link,
                strerror(errno));
    }
    return true;
  }

  if (mount_link_staged && rename(paths->staged_mount_link, paths->mount_link) == 0) {
    log_debug("  [LINK] restored mount link after failed cleanup: %s",
              paths->mount_link);
  } else if (mount_link_staged) {
    log_debug("  [LINK] restore failed for %s: %s", paths->mount_link,
              strerror(errno));
  }
  return true;
}

void cleanup_mount_links(const char *removed_source_root,
                         bool unmount_system_ex_bind) {
  cleanup_mount_links_ctx_t ctx = {
      .removed_source_root = removed_source_root,
      .unmount_system_ex_bind = unmount_system_ex_bind,
      .tried_image_recovery = false,
  };
  for_each_title_app_dir("", !unmount_system_ex_bind, cleanup_mount_links_entry,
                         &ctx);
}

static bool shutdown_title_mounts_entry(const char *title_id,
                                        const title_link_paths_t *paths,
                                        void *ctx) {
  (void)ctx;

  char source_path[MAX_PATH];
  if (!read_mount_link_file(paths->mount_link, source_path, sizeof(source_path))) {
    return true;
  }

  title_mount_state_t state;
  if (!inspect_title_stack(title_id, source_path, NULL, &state) ||
      !state.has_our_nullfs ||
      (!state.top_is_our_nullfs && !state.has_our_backport)) {
    return true;
  }

  if (!unmount_controlled_mount_stack(state.system_ex_path)) {
    log_debug("  [LINK] failed to unmount shutdown stack for %s",
              state.system_ex_path);
  }
  return true;
}

void shutdown_title_mounts(void) {
  for_each_title_app_dir(" during shutdown", false,
                         shutdown_title_mounts_entry, NULL);
}
