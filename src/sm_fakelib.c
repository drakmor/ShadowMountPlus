#include "sm_platform.h"

#include "sm_fakelib.h"
#include "sm_config_mount.h"
#include "sm_log.h"
#include "sm_types.h"

typedef struct {
  bool active;
  pid_t pid;
  char mount_path[MAX_PATH];
} fakelib_mount_t;

static fakelib_mount_t g_fakelib_mount;

bool sm_fakelib_game_feature_enabled(void) {
  return runtime_config()->backport_fakelib_enabled;
}

static bool mount_fakelib_overlay(const char *title_id,
                                  const char *source_path,
                                  const char *mount_path) {
  struct iovec overlay_iov[] = {
      IOVEC_ENTRY("fstype"), IOVEC_ENTRY("unionfs"),
      IOVEC_ENTRY("from"),   IOVEC_ENTRY(source_path),
      IOVEC_ENTRY("fspath"), IOVEC_ENTRY(mount_path)};

  if (nmount(overlay_iov, IOVEC_SIZE(overlay_iov), 0) == 0) {
    notify_system_info("Game backported: %s", title_id);
    log_debug("  [FAKELIB] libraries mounted for %s: %s -> %s", title_id,
              source_path, mount_path);
    return true;
  }

  log_debug("  [FAKELIB] mount failed for %s (%s -> %s): %s", title_id,
            source_path, mount_path, strerror(errno));
  return false;
}

static bool unmount_fakelib_overlay(const char *mount_path) {
  if (unmount(mount_path, MNT_FORCE) == 0 || errno == ENOENT ||
      errno == EINVAL) {
    return true;
  }

  log_debug("  [FAKELIB] unmount failed for %s: %s", mount_path,
            strerror(errno));
  return false;
}

static bool resolve_sandbox_fakelib_context(const char *title_id,
                                            char source_path[MAX_PATH],
                                            char mount_path[MAX_PATH]) {
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

    int idx = atoi(suffix);
    if (idx < best_index)
      continue;

    best_index = idx;
    (void)strlcpy(sandbox_id, entry->d_name, sizeof(sandbox_id));
    found = true;
  }

  closedir(d);
  if (!found)
    return false;

  snprintf(source_path, MAX_PATH, "/mnt/sandbox/%s/app0/fakelib", sandbox_id);
  struct stat st;
  if (stat(source_path, &st) != 0 || !S_ISDIR(st.st_mode))
    return false;

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

static bool cleanup_fakelib_mount(void) {
  if (!g_fakelib_mount.active)
    return true;

  if (!unmount_fakelib_overlay(g_fakelib_mount.mount_path)) {
    return false;
  }

  memset(&g_fakelib_mount, 0, sizeof(g_fakelib_mount));
  return true;
}

void sm_fakelib_game_on_exec(pid_t pid, const char *title_id) {
  if (!sm_fakelib_game_feature_enabled())
    return;

  if (g_fakelib_mount.active && g_fakelib_mount.pid == pid) {
    log_debug("  [FAKELIB] already tracking pid=%ld for %s", (long)pid,
              title_id);
    return;
  }

  if (g_fakelib_mount.active && g_fakelib_mount.pid != pid) {
    log_debug("  [FAKELIB] handoff active mount pid=%ld -> pid=%ld (%s)",
              (long)g_fakelib_mount.pid, (long)pid, title_id);
    if (!cleanup_fakelib_mount()) {
      log_debug("  [FAKELIB] handoff cleanup failed for pid=%ld, skipping %s",
                (long)g_fakelib_mount.pid, title_id);
      return;
    }
  }

  char source_path[MAX_PATH];
  char mount_path[MAX_PATH];
  if (!resolve_sandbox_fakelib_context(title_id, source_path, mount_path))
    return;

  if (!mount_fakelib_overlay(title_id, source_path, mount_path))
    return;

  g_fakelib_mount.active = true;
  g_fakelib_mount.pid = pid;
  (void)strlcpy(g_fakelib_mount.mount_path, mount_path,
                sizeof(g_fakelib_mount.mount_path));
}

void sm_fakelib_game_on_exit(pid_t pid) {
  if (!g_fakelib_mount.active || g_fakelib_mount.pid != pid)
    return;

  log_debug("  [FAKELIB] game stopped: pid=%ld mount=%s", (long)pid,
            g_fakelib_mount.mount_path);
  cleanup_fakelib_mount();
}

void sm_fakelib_game_shutdown(void) {
  (void)cleanup_fakelib_mount();
}
