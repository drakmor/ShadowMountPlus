#include "sm_platform.h"

#include "sm_fakelib.h"
#include "sm_config_mount.h"
#include "sm_log.h"
#include "sm_types.h"

#include <pthread.h>

typedef struct {
  char source_path[MAX_PATH];
  char mount_path[MAX_PATH];
  const char *label;
} fakelib_layer_t;

typedef struct {
  pid_t pid;
  char mount_path[MAX_PATH];
  fakelib_layer_t layers[2];
  size_t layer_count;
} fakelib_session_t;

static fakelib_session_t g_fakelib_mount;
static pthread_mutex_t g_fakelib_mutex = PTHREAD_MUTEX_INITIALIZER;

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
  if (unmount(mount_path, MNT_FORCE) == 0 || errno == ENOENT ||
      errno == EINVAL) {
    log_debug("  [FAKELIB] %s libraries unmounted: %s -> %s", layer->label,
              layer->source_path, mount_path);
    return true;
  }

  log_debug("  [FAKELIB] %s unmount failed for %s: %s", layer->label,
            mount_path, strerror(errno));
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

static bool resolve_sandbox_context(const char *title_id,
                                    char game_source_path[MAX_PATH],
                                    char mount_path[MAX_PATH]) {
  game_source_path[0] = '\0';
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
    long idx_long = strtol(suffix, NULL, 10);
    if (errno == ERANGE || idx_long > INT_MAX)
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

  char source_path[MAX_PATH];
  snprintf(source_path, sizeof(source_path), "/mnt/sandbox/%s/app0/fakelib2",
           sandbox_id);
  struct stat st;
  if (stat(source_path, &st) == 0 && S_ISDIR(st.st_mode)) {
    (void)strlcpy(game_source_path, source_path, MAX_PATH);
  } else {
    snprintf(source_path, sizeof(source_path), "/mnt/sandbox/%s/app0/fakelib",
             sandbox_id);
    if (stat(source_path, &st) == 0 && S_ISDIR(st.st_mode))
      (void)strlcpy(game_source_path, source_path, MAX_PATH);
  }

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

  (void)strlcpy(source_path, cfg->global_fakelib_path, MAX_PATH);
  return true;
}

static bool cleanup_fakelib_mount(void) {
  if (g_fakelib_mount.layer_count == 0) {
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

void sm_fakelib_game_on_exec(pid_t pid, const char *title_id) {
  pthread_mutex_lock(&g_fakelib_mutex);
  if (!sm_fakelib_game_feature_enabled()) {
    pthread_mutex_unlock(&g_fakelib_mutex);
    return;
  }

  if (g_fakelib_mount.layer_count > 0 && g_fakelib_mount.pid == pid) {
    log_debug("  [FAKELIB] already tracking pid=%ld for %s", (long)pid,
              title_id);
    pthread_mutex_unlock(&g_fakelib_mutex);
    return;
  }

  if (g_fakelib_mount.layer_count > 0 && g_fakelib_mount.pid != pid) {
    log_debug("  [FAKELIB] handoff active mount pid=%ld -> pid=%ld (%s)",
              (long)g_fakelib_mount.pid, (long)pid, title_id);
    if (!cleanup_fakelib_mount()) {
      log_debug("  [FAKELIB] handoff cleanup failed for pid=%ld, skipping %s",
                (long)g_fakelib_mount.pid, title_id);
      pthread_mutex_unlock(&g_fakelib_mutex);
      return;
    }
  }

  char game_source_path[MAX_PATH];
  char global_source_path[MAX_PATH];
  char mount_path[MAX_PATH];
  if (!resolve_sandbox_context(title_id, game_source_path, mount_path)) {
    pthread_mutex_unlock(&g_fakelib_mutex);
    return;
  }

  bool has_global =
      resolve_global_fakelib_source(title_id, global_source_path);
  bool has_game = (game_source_path[0] != '\0');
  if (!has_global && !has_game) {
    pthread_mutex_unlock(&g_fakelib_mutex);
    return;
  }

  memset(&g_fakelib_mount, 0, sizeof(g_fakelib_mount));
  g_fakelib_mount.pid = pid;
  (void)strlcpy(g_fakelib_mount.mount_path, mount_path,
                sizeof(g_fakelib_mount.mount_path));

  bool global_first = runtime_config()->global_fakelib_mount_first;
  bool same_source =
      has_global && has_game &&
      strcmp(global_source_path, game_source_path) == 0;
  if (global_first && has_global && !same_source) {
    if (!track_fakelib_overlay(title_id, global_source_path, mount_path,
                               "global")) {
      (void)cleanup_fakelib_mount();
      pthread_mutex_unlock(&g_fakelib_mutex);
      return;
    }
  }

  if (has_game) {
    if (!track_fakelib_overlay(title_id, game_source_path, mount_path,
                               "game")) {
      (void)cleanup_fakelib_mount();
      pthread_mutex_unlock(&g_fakelib_mutex);
      return;
    }
  }

  if (!global_first && has_global && !same_source) {
    if (!track_fakelib_overlay(title_id, global_source_path, mount_path,
                               "global")) {
      (void)cleanup_fakelib_mount();
      pthread_mutex_unlock(&g_fakelib_mutex);
      return;
    }
  }

  if (has_game)
    notify_system_info("Game backported: %s", title_id);
  pthread_mutex_unlock(&g_fakelib_mutex);
}

void sm_fakelib_game_on_exit(pid_t pid) {
  pthread_mutex_lock(&g_fakelib_mutex);
  if (g_fakelib_mount.layer_count == 0 || g_fakelib_mount.pid != pid) {
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
