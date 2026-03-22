#include "sm_platform.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/event.h>

#include "sm_config_mount.h"
#include "sm_filesystem.h"
#include "sm_image.h"
#include "sm_install.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_path_utils.h"
#include "sm_paths.h"
#include "sm_runtime.h"
#include "sm_scan.h"
#include "sm_scanner.h"
#include "sm_time.h"
#include "sm_types.h"

#define SCANNER_EVENT_BATCH 32
#define SCANNER_EVENT_DRAIN_BATCHES 8

typedef enum {
  SCANNER_WATCH_SCAN_ROOT = 0,
  SCANNER_WATCH_SCAN_BACKPORT_ROOT,
  SCANNER_WATCH_SCAN_SUBDIR,
  SCANNER_WATCH_SCAN_IMAGE_FILE,
} scanner_watch_kind_t;

typedef struct {
  int fd;
  int scan_root_index;
  uint8_t depth;
  scanner_watch_kind_t kind;
  char path[MAX_PATH];
} scanner_watch_entry_t;

typedef struct {
  bool dirty;
  bool cleanup_pending;
  bool watch_tree_stale;
  uint8_t watch_tree_rebuild_depth;
  scanner_watch_kind_t watch_tree_rebuild_kind;
  uint64_t ready_after_us;
  char watch_tree_rebuild_path[MAX_PATH];
} scanner_root_state_t;

static int g_scanner_wake_pipe[2] = {-1, -1};
static int g_scanner_control_dir_fd = -1;
static volatile sig_atomic_t g_scanner_wake_write_fd = -1;
static scanner_watch_entry_t *g_scanner_watch_entries = NULL;
static size_t g_scanner_watch_count = 0;
static size_t g_scanner_watch_capacity = 0;
static scanner_root_state_t g_scanner_root_states[MAX_SCAN_PATHS];

static uint64_t scanner_stability_wait_us(void) {
  return (uint64_t)runtime_config()->stability_wait_seconds * 1000000ull;
}

static uint64_t scanner_full_resync_interval_us(void) {
  return (uint64_t)runtime_config()->scan_interval_us;
}

static bool set_fd_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return false;

  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void reset_scanner_root_states(void) {
  memset(g_scanner_root_states, 0, sizeof(g_scanner_root_states));
}

static void clear_scan_root_watch_tree_state(int scan_root_index) {
  if (scan_root_index < 0 || scan_root_index >= get_scan_path_count())
    return;

  g_scanner_root_states[scan_root_index].watch_tree_stale = false;
  g_scanner_root_states[scan_root_index].watch_tree_rebuild_depth = 0;
  g_scanner_root_states[scan_root_index].watch_tree_rebuild_kind =
      SCANNER_WATCH_SCAN_ROOT;
  g_scanner_root_states[scan_root_index].watch_tree_rebuild_path[0] = '\0';
}

static void close_scanner_wake_pipe(void) {
  g_scanner_wake_write_fd = -1;
  if (g_scanner_wake_pipe[0] >= 0) {
    close(g_scanner_wake_pipe[0]);
    g_scanner_wake_pipe[0] = -1;
  }
  if (g_scanner_wake_pipe[1] >= 0) {
    close(g_scanner_wake_pipe[1]);
    g_scanner_wake_pipe[1] = -1;
  }
}

static void close_scanner_control_dir(void) {
  if (g_scanner_control_dir_fd >= 0) {
    close(g_scanner_control_dir_fd);
    g_scanner_control_dir_fd = -1;
  }
}

static void clear_scanner_watch_entries(void) {
  for (size_t i = 0; i < g_scanner_watch_count; i++) {
    if (g_scanner_watch_entries[i].fd >= 0)
      close(g_scanner_watch_entries[i].fd);
  }
  free(g_scanner_watch_entries);
  g_scanner_watch_entries = NULL;
  g_scanner_watch_count = 0;
  g_scanner_watch_capacity = 0;
}

static void drain_scanner_wake_pipe(void) {
  if (g_scanner_wake_pipe[0] < 0)
    return;

  char buf[64];
  while (read(g_scanner_wake_pipe[0], buf, sizeof(buf)) > 0) {
  }
}

static void log_immediate_scan_reason(const char *reason) {
  if (!reason || reason[0] == '\0')
    return;
  log_debug("[SCAN] running immediate full scan (%s)", reason);
}

static bool ensure_scanner_watch_capacity(size_t needed_count) {
  if (needed_count <= g_scanner_watch_capacity)
    return true;

  size_t new_capacity = g_scanner_watch_capacity ? g_scanner_watch_capacity : 64;
  while (new_capacity < needed_count)
    new_capacity *= 2u;

  scanner_watch_entry_t *new_entries =
      realloc(g_scanner_watch_entries, new_capacity * sizeof(*new_entries));
  if (!new_entries) {
    log_debug("  [SCAN] watcher registry allocation failed");
    return false;
  }

  g_scanner_watch_entries = new_entries;
  g_scanner_watch_capacity = new_capacity;
  return true;
}

static bool is_distinct_configured_scan_root(const char *current_scan_root,
                                             const char *path) {
  for (int i = 0; i < get_scan_path_count(); i++) {
    const char *scan_path = get_scan_path(i);
    if (strcmp(scan_path, path) != 0)
      continue;
    if (current_scan_root && strcmp(current_scan_root, path) == 0)
      return false;
    return true;
  }

  return false;
}

static void classify_watch_entry(const char *full_path, unsigned char d_type,
                                 bool *is_dir_out, bool *is_regular_out) {
  bool is_dir = false;
  bool is_regular = false;

  if (d_type == DT_DIR) {
    is_dir = true;
  } else if (d_type == DT_REG) {
    is_regular = true;
  } else if (d_type == DT_UNKNOWN) {
    struct stat st;
    if (lstat(full_path, &st) == 0) {
      is_dir = S_ISDIR(st.st_mode);
      is_regular = S_ISREG(st.st_mode);
    }
  }

  if (is_dir_out)
    *is_dir_out = is_dir;
  if (is_regular_out)
    *is_regular_out = is_regular;
}

static bool register_scanner_watch_entry(int kq, int scan_root_index,
                                         const char *path,
                                         scanner_watch_kind_t kind,
                                         uint8_t depth) {
  int open_flags = O_RDONLY;
  if (kind != SCANNER_WATCH_SCAN_IMAGE_FILE)
    open_flags |= O_DIRECTORY;

  int fd = open(path, open_flags);
  if (fd < 0) {
    if (errno != ENOENT && errno != ENOTDIR) {
      log_debug("  [SCAN] watcher open failed for %s: %s", path,
                strerror(errno));
    }
    return true;
  }

  if (!ensure_scanner_watch_capacity(g_scanner_watch_count + 1u)) {
    close(fd);
    return false;
  }

  struct kevent kev;
  EV_SET(&kev, (uintptr_t)fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
         NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME |
             NOTE_REVOKE,
         0, NULL);
  if (kevent(kq, &kev, 1, NULL, 0, NULL) != 0) {
    log_debug("  [SCAN] watcher registration failed for %s: %s", path,
              strerror(errno));
    close(fd);
    return false;
  }

  scanner_watch_entry_t *entry = &g_scanner_watch_entries[g_scanner_watch_count++];
  memset(entry, 0, sizeof(*entry));
  entry->fd = fd;
  entry->scan_root_index = scan_root_index;
  entry->depth = depth;
  entry->kind = kind;
  (void)strlcpy(entry->path, path, sizeof(entry->path));
  return true;
}

static void remove_scanner_watch_entry_at(size_t index) {
  if (index >= g_scanner_watch_count)
    return;

  if (g_scanner_watch_entries[index].fd >= 0)
    close(g_scanner_watch_entries[index].fd);

  size_t last_index = g_scanner_watch_count - 1u;
  if (index != last_index)
    g_scanner_watch_entries[index] = g_scanner_watch_entries[last_index];
  memset(&g_scanner_watch_entries[last_index], 0,
         sizeof(g_scanner_watch_entries[last_index]));
  g_scanner_watch_count--;
}

static void remove_scan_root_watch_entries(int scan_root_index) {
  for (size_t i = 0; i < g_scanner_watch_count;) {
    if (g_scanner_watch_entries[i].scan_root_index != scan_root_index) {
      i++;
      continue;
    }
    remove_scanner_watch_entry_at(i);
  }
}

static void remove_scan_root_watch_entries_for_path(int scan_root_index,
                                                    const char *path) {
  for (size_t i = 0; i < g_scanner_watch_count;) {
    scanner_watch_entry_t *entry = &g_scanner_watch_entries[i];
    if (entry->scan_root_index != scan_root_index) {
      i++;
      continue;
    }
    if (!path_matches_root_or_child(entry->path, path)) {
      i++;
      continue;
    }
    remove_scanner_watch_entry_at(i);
  }
}

static scanner_watch_entry_t *find_scanner_watch_entry_by_fd(uintptr_t ident) {
  for (size_t i = 0; i < g_scanner_watch_count; i++) {
    if ((uintptr_t)g_scanner_watch_entries[i].fd == ident)
      return &g_scanner_watch_entries[i];
  }
  return NULL;
}

static bool build_parent_directory_path(const char *path,
                                        char parent_path[MAX_PATH]) {
  const char *slash = strrchr(path, '/');
  if (!slash)
    return false;
  if (slash == path) {
    (void)strlcpy(parent_path, "/", MAX_PATH);
    return true;
  }

  size_t parent_len = (size_t)(slash - path);
  if (parent_len >= MAX_PATH)
    parent_len = MAX_PATH - 1u;
  memcpy(parent_path, path, parent_len);
  parent_path[parent_len] = '\0';
  return true;
}

static bool resolve_watch_tree_rebuild_target(const scanner_watch_entry_t *entry,
                                              char rebuild_path[MAX_PATH],
                                              uint8_t *rebuild_depth_out,
                                              scanner_watch_kind_t *kind_out) {
  switch (entry->kind) {
  case SCANNER_WATCH_SCAN_ROOT:
  case SCANNER_WATCH_SCAN_BACKPORT_ROOT:
  case SCANNER_WATCH_SCAN_SUBDIR:
    (void)strlcpy(rebuild_path, entry->path, MAX_PATH);
    if (rebuild_depth_out)
      *rebuild_depth_out = entry->depth;
    if (kind_out)
      *kind_out = entry->kind;
    return true;
  case SCANNER_WATCH_SCAN_IMAGE_FILE:
    if (!build_parent_directory_path(entry->path, rebuild_path))
      return false;
    if (rebuild_depth_out)
      *rebuild_depth_out = (entry->depth > 0u) ? (uint8_t)(entry->depth - 1u) : 0u;
    if (kind_out) {
      *kind_out =
          (entry->depth <= 1u) ? SCANNER_WATCH_SCAN_ROOT : SCANNER_WATCH_SCAN_SUBDIR;
    }
    return true;
  default:
    return false;
  }
}

static bool register_scan_dir_tree(int kq, int scan_root_index,
                                   const char *current_scan_root,
                                   const char *dir_path,
                                   unsigned int depth_from_root,
                                   unsigned int remaining_depth) {
  if (depth_from_root > 0 &&
      is_distinct_configured_scan_root(current_scan_root, dir_path)) {
    return true;
  }

  scanner_watch_kind_t kind =
      (depth_from_root == 0u) ? SCANNER_WATCH_SCAN_ROOT
                              : SCANNER_WATCH_SCAN_SUBDIR;
  if (!register_scanner_watch_entry(kq, scan_root_index, dir_path, kind,
                                    (uint8_t)depth_from_root)) {
    return false;
  }

  if (remaining_depth == 0u)
    return true;

  DIR *d = opendir(dir_path);
  if (!d)
    return true;

  bool skip_backports_root =
      (depth_from_root == 0u && !is_under_image_mount_base(current_scan_root));

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (should_stop_requested()) {
      closedir(d);
      return true;
    }
    if (entry->d_name[0] == '.')
      continue;
    if (skip_backports_root &&
        strcmp(entry->d_name, DEFAULT_BACKPORTS_DIR_NAME) == 0) {
      continue;
    }

    char full_path[MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

    bool is_dir = false;
    bool is_regular = false;
    classify_watch_entry(full_path, entry->d_type, &is_dir, &is_regular);

    if (!path_matches_root_or_child(current_scan_root, IMAGE_MOUNT_BASE) &&
        is_regular && is_supported_image_file_name(entry->d_name)) {
      if (!register_scanner_watch_entry(kq, scan_root_index, full_path,
                                        SCANNER_WATCH_SCAN_IMAGE_FILE,
                                        (uint8_t)(depth_from_root + 1u))) {
        closedir(d);
        return false;
      }
    }
    if (!is_dir)
      continue;

    if (!register_scan_dir_tree(kq, scan_root_index, current_scan_root,
                                full_path, depth_from_root + 1u,
                                remaining_depth - 1u)) {
      closedir(d);
      return false;
    }
  }

  closedir(d);
  return true;
}

static bool rebuild_scan_root_watch_tree(int kq, int scan_root_index) {
  const char *scan_root = get_scan_path(scan_root_index);
  remove_scan_root_watch_entries(scan_root_index);

  unsigned int scan_depth = runtime_config()->scan_depth;
  if (scan_depth < MIN_SCAN_DEPTH)
    scan_depth = MIN_SCAN_DEPTH;

  if (!register_scan_dir_tree(kq, scan_root_index, scan_root, scan_root, 0u,
                              scan_depth)) {
    return false;
  }

  char backport_root[MAX_PATH];
  if (build_backports_root_path(scan_root, backport_root)) {
    if (!register_scanner_watch_entry(kq, scan_root_index, backport_root,
                                      SCANNER_WATCH_SCAN_BACKPORT_ROOT, 1u)) {
      return false;
    }
  }

  clear_scan_root_watch_tree_state(scan_root_index);
  return true;
}

static bool rebuild_scan_root_watch_subtree(int kq, int scan_root_index,
                                            const char *rebuild_path,
                                            uint8_t rebuild_depth,
                                            scanner_watch_kind_t rebuild_kind) {
  const char *scan_root = get_scan_path(scan_root_index);
  if (!rebuild_path || rebuild_path[0] == '\0' ||
      rebuild_kind == SCANNER_WATCH_SCAN_ROOT ||
      strcmp(rebuild_path, scan_root) == 0) {
    return rebuild_scan_root_watch_tree(kq, scan_root_index);
  }

  if (rebuild_kind == SCANNER_WATCH_SCAN_BACKPORT_ROOT) {
    remove_scan_root_watch_entries_for_path(scan_root_index, rebuild_path);
    if (!register_scanner_watch_entry(kq, scan_root_index, rebuild_path,
                                      SCANNER_WATCH_SCAN_BACKPORT_ROOT,
                                      rebuild_depth)) {
      return false;
    }
    clear_scan_root_watch_tree_state(scan_root_index);
    return true;
  }

  unsigned int scan_depth = runtime_config()->scan_depth;
  if (scan_depth < MIN_SCAN_DEPTH)
    scan_depth = MIN_SCAN_DEPTH;

  remove_scan_root_watch_entries_for_path(scan_root_index, rebuild_path);
  if (rebuild_depth > scan_depth) {
    clear_scan_root_watch_tree_state(scan_root_index);
    return true;
  }

  if (!register_scan_dir_tree(kq, scan_root_index, scan_root, rebuild_path,
                              rebuild_depth, scan_depth - rebuild_depth)) {
    return false;
  }

  clear_scan_root_watch_tree_state(scan_root_index);
  return true;
}

static bool rebuild_all_scan_root_watch_trees(int kq) {
  for (int i = 0; i < get_scan_path_count(); i++) {
    if (!rebuild_scan_root_watch_tree(kq, i))
      return false;
  }
  return true;
}

static void clear_all_dirty_scan_roots(void) {
  for (int i = 0; i < get_scan_path_count(); i++) {
    g_scanner_root_states[i].dirty = false;
    g_scanner_root_states[i].cleanup_pending = false;
    g_scanner_root_states[i].ready_after_us = 0;
    clear_scan_root_watch_tree_state(i);
  }
}

static void schedule_scan_root_cleanup(int scan_root_index) {
  if (scan_root_index < 0 || scan_root_index >= get_scan_path_count())
    return;

  g_scanner_root_states[scan_root_index].cleanup_pending = true;
}

static void schedule_scan_root_dirty(int scan_root_index, uint64_t now_us,
                                     bool immediate) {
  if (scan_root_index < 0 || scan_root_index >= get_scan_path_count())
    return;

  scanner_root_state_t *state = &g_scanner_root_states[scan_root_index];
  uint64_t ready_after_us =
      immediate ? now_us : now_us + scanner_stability_wait_us();

  if (!state->dirty) {
    state->dirty = true;
    state->ready_after_us = ready_after_us;
    return;
  }

  if (immediate) {
    if (ready_after_us < state->ready_after_us)
      state->ready_after_us = ready_after_us;
    return;
  }

  if (ready_after_us > state->ready_after_us)
    state->ready_after_us = ready_after_us;
}

static bool scanner_event_requires_consistency_cleanup(
    const scanner_watch_entry_t *entry, uint32_t fflags) {
  if (entry->kind == SCANNER_WATCH_SCAN_BACKPORT_ROOT)
    return false;

  return (fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) != 0;
}

static bool scanner_event_requires_watch_tree_refresh(
    const scanner_watch_entry_t *entry, uint32_t fflags) {
  uint32_t tree_change_flags =
      NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE;

  switch (entry->kind) {
  case SCANNER_WATCH_SCAN_ROOT:
    return (fflags & tree_change_flags) != 0;
  case SCANNER_WATCH_SCAN_BACKPORT_ROOT:
    return (fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) != 0;
  case SCANNER_WATCH_SCAN_SUBDIR:
    return entry->depth < runtime_config()->scan_depth &&
           (fflags & tree_change_flags) != 0;
  case SCANNER_WATCH_SCAN_IMAGE_FILE:
    return (fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) != 0;
  default:
    return false;
  }
}

static void schedule_scan_root_watch_tree_rebuild(
    const scanner_watch_entry_t *entry) {
  char rebuild_path[MAX_PATH];
  uint8_t rebuild_depth = 0;
  scanner_watch_kind_t rebuild_kind = SCANNER_WATCH_SCAN_ROOT;
  if (!resolve_watch_tree_rebuild_target(entry, rebuild_path, &rebuild_depth,
                                         &rebuild_kind)) {
    return;
  }

  scanner_root_state_t *state = &g_scanner_root_states[entry->scan_root_index];
  if (!state->watch_tree_stale || state->watch_tree_rebuild_path[0] == '\0') {
    state->watch_tree_stale = true;
    state->watch_tree_rebuild_depth = rebuild_depth;
    state->watch_tree_rebuild_kind = rebuild_kind;
    (void)strlcpy(state->watch_tree_rebuild_path, rebuild_path,
                  sizeof(state->watch_tree_rebuild_path));
    return;
  }

  if (strcmp(state->watch_tree_rebuild_path, rebuild_path) == 0) {
    if (rebuild_depth < state->watch_tree_rebuild_depth)
      state->watch_tree_rebuild_depth = rebuild_depth;
    if (rebuild_kind == SCANNER_WATCH_SCAN_ROOT ||
        (rebuild_kind == SCANNER_WATCH_SCAN_BACKPORT_ROOT &&
         state->watch_tree_rebuild_kind != SCANNER_WATCH_SCAN_ROOT)) {
      state->watch_tree_rebuild_kind = rebuild_kind;
    }
    return;
  }

  if (path_matches_root_or_child(rebuild_path, state->watch_tree_rebuild_path))
    return;

  if (path_matches_root_or_child(state->watch_tree_rebuild_path, rebuild_path)) {
    state->watch_tree_rebuild_depth = rebuild_depth;
    state->watch_tree_rebuild_kind = rebuild_kind;
    (void)strlcpy(state->watch_tree_rebuild_path, rebuild_path,
                  sizeof(state->watch_tree_rebuild_path));
    return;
  }

  state->watch_tree_stale = true;
  state->watch_tree_rebuild_depth = 0;
  state->watch_tree_rebuild_kind = SCANNER_WATCH_SCAN_ROOT;
  (void)strlcpy(state->watch_tree_rebuild_path,
                get_scan_path(entry->scan_root_index),
                sizeof(state->watch_tree_rebuild_path));
}

static bool register_control_dir_watch(int kq) {
  if (g_scanner_control_dir_fd < 0)
    return true;

  struct kevent kev;
  EV_SET(&kev, (uintptr_t)g_scanner_control_dir_fd, EVFILT_VNODE,
         EV_ADD | EV_ENABLE | EV_CLEAR,
         NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME |
             NOTE_REVOKE,
         0, NULL);
  if (kevent(kq, &kev, 1, NULL, 0, NULL) != 0) {
    log_debug("  [SCAN] control dir watcher registration failed: %s",
              strerror(errno));
    return false;
  }

  return true;
}

static bool reopen_control_dir_watch(int kq) {
  close_scanner_control_dir();

  g_scanner_control_dir_fd = open(LOG_DIR, O_RDONLY | O_DIRECTORY);
  if (g_scanner_control_dir_fd < 0) {
    log_debug("  [SCAN] control dir watch unavailable for %s: %s", LOG_DIR,
              strerror(errno));
    return true;
  }

  return register_control_dir_watch(kq);
}

static bool run_full_scan_cycle(bool startup_sync, const char *reason,
                                bool *unstable_found_out) {
  scan_candidate_t candidates[MAX_PENDING];
  memset(candidates, 0, sizeof(candidates));

  log_immediate_scan_reason(reason);

  bool unstable_found = false;
  cleanup_lost_sources_before_scan();

  int total_found_games = 0;
  int *total_found_ptr = startup_sync ? &total_found_games : NULL;
  int candidate_count = collect_scan_candidates(candidates, MAX_PENDING,
                                                total_found_ptr,
                                                &unstable_found);
  if (candidate_count > 0) {
    if (startup_sync) {
      int new_games = 0;
      for (int i = 0; i < candidate_count; i++) {
        if (!candidates[i].installed)
          new_games++;
      }
      if (new_games > 0)
        notify_system_info("Found %d new games. Executing...", new_games);
    }

    process_scan_candidates(candidates, candidate_count);
  }

  mount_backport_overlays(&unstable_found);

  if (unstable_found_out)
    *unstable_found_out = unstable_found;

  if (startup_sync) {
    notify_system_rich(true, "Library Synchronized.\nFound %d games.",
                       total_found_games);
  }

  return !should_stop_requested();
}

static bool run_targeted_scan_cycle(int scan_root_index,
                                    bool *unstable_found_out) {
  const char *scan_root = get_scan_path(scan_root_index);
  scan_candidate_t candidates[MAX_PENDING];
  memset(candidates, 0, sizeof(candidates));

  log_debug("[SCAN] running targeted scan for %s", scan_root);

  bool unstable_found = false;
  cleanup_lost_sources_for_scan_root(scan_root);

  int candidate_count = collect_scan_candidates_for_scan_root(
      scan_root, candidates, MAX_PENDING, NULL, &unstable_found);
  if (candidate_count > 0)
    process_scan_candidates(candidates, candidate_count);

  mount_backport_overlays_for_scan_root(scan_root, &unstable_found);

  if (unstable_found_out)
    *unstable_found_out = unstable_found;

  return !should_stop_requested();
}

static int find_pending_cleanup_scan_root(void) {
  for (int i = 0; i < get_scan_path_count(); i++) {
    if (g_scanner_root_states[i].cleanup_pending)
      return i;
  }

  return -1;
}

static int find_due_dirty_scan_root(uint64_t now_us) {
  int selected_root = -1;
  uint64_t selected_deadline = 0;

  for (int i = 0; i < get_scan_path_count(); i++) {
    const scanner_root_state_t *state = &g_scanner_root_states[i];
    if (!state->dirty)
      continue;
    if (state->ready_after_us > now_us)
      continue;
    if (selected_root < 0 || state->ready_after_us < selected_deadline) {
      selected_root = i;
      selected_deadline = state->ready_after_us;
    }
  }

  return selected_root;
}

static uint64_t compute_next_scan_deadline_us(uint64_t full_resync_due_us) {
  uint64_t next_deadline = full_resync_due_us;

  for (int i = 0; i < get_scan_path_count(); i++) {
    const scanner_root_state_t *state = &g_scanner_root_states[i];
    if (!state->dirty)
      continue;
    if (next_deadline == 0 || state->ready_after_us < next_deadline)
      next_deadline = state->ready_after_us;
  }

  return next_deadline;
}

static void build_wait_timeout(struct timespec *timeout, uint64_t now_us,
                               uint64_t deadline_us) {
  memset(timeout, 0, sizeof(*timeout));
  if (deadline_us <= now_us)
    return;

  uint64_t delta_us = deadline_us - now_us;
  timeout->tv_sec = (time_t)(delta_us / 1000000ull);
  timeout->tv_nsec = (long)((delta_us % 1000000ull) * 1000ull);
}

static bool process_scanner_events(int kq, const struct timespec *timeout,
                                   bool *timed_out_out) {
  if (timed_out_out)
    *timed_out_out = false;

  struct kevent events[SCANNER_EVENT_BATCH];
  int nev = kevent(kq, NULL, 0, events, SCANNER_EVENT_BATCH, timeout);
  if (nev < 0) {
    if (errno == EINTR)
      return true;

    log_debug("  [SCAN] kevent wait failed: %s", strerror(errno));
    return false;
  }

  if (nev == 0) {
    if (timed_out_out)
      *timed_out_out = true;
    return true;
  }

  uint64_t now_us = monotonic_time_us();

  for (int i = 0; i < nev; i++) {
    const struct kevent *event = &events[i];

    if (event->filter == EVFILT_READ &&
        event->ident == (uintptr_t)g_scanner_wake_pipe[0]) {
      drain_scanner_wake_pipe();
      continue;
    }

    if (event->filter != EVFILT_VNODE)
      continue;

    if (event->ident == (uintptr_t)g_scanner_control_dir_fd) {
      if ((event->fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) != 0 &&
          !reopen_control_dir_watch(kq)) {
        return false;
      }
      continue;
    }

    scanner_watch_entry_t *watch_entry =
        find_scanner_watch_entry_by_fd(event->ident);
    if (!watch_entry)
      continue;

    bool immediate =
        (event->fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) != 0;
    if (scanner_event_requires_consistency_cleanup(watch_entry, event->fflags))
      schedule_scan_root_cleanup(watch_entry->scan_root_index);
    schedule_scan_root_dirty(watch_entry->scan_root_index, now_us, immediate);

    if (scanner_event_requires_watch_tree_refresh(watch_entry, event->fflags))
      schedule_scan_root_watch_tree_rebuild(watch_entry);
  }

  return true;
}

static bool drain_scanner_events_nowait(int kq) {
  struct timespec timeout;
  memset(&timeout, 0, sizeof(timeout));

  for (int batch = 0; batch < SCANNER_EVENT_DRAIN_BATCHES; batch++) {
    bool timed_out = false;
    if (!process_scanner_events(kq, &timeout, &timed_out))
      return false;
    if (timed_out)
      return true;
  }

  return true;
}

static char g_scanner_shutdown_reason[128];

static void request_scanner_shutdown(const char *reason) {
  const char *resolved_reason =
      (reason && reason[0] != '\0') ? reason : "scanner failure";
  (void)strlcpy(g_scanner_shutdown_reason, resolved_reason,
                sizeof(g_scanner_shutdown_reason));
  log_debug("  [SCAN] %s; stopping scanner", g_scanner_shutdown_reason);
  request_shutdown_stop(g_scanner_shutdown_reason);
}

bool sm_scanner_init(void) {
  close_scanner_wake_pipe();
  close_scanner_control_dir();
  clear_scanner_watch_entries();
  reset_scanner_root_states();

  if (pipe(g_scanner_wake_pipe) != 0) {
    log_debug("  [SCAN] wake pipe creation failed: %s", strerror(errno));
    close_scanner_wake_pipe();
    return false;
  }
  if (!set_fd_nonblocking(g_scanner_wake_pipe[0]) ||
      !set_fd_nonblocking(g_scanner_wake_pipe[1])) {
    log_debug("  [SCAN] wake pipe nonblocking setup failed: %s",
              strerror(errno));
    close_scanner_wake_pipe();
    return false;
  }
  g_scanner_wake_write_fd = (sig_atomic_t)g_scanner_wake_pipe[1];

  g_scanner_control_dir_fd = open(LOG_DIR, O_RDONLY | O_DIRECTORY);
  if (g_scanner_control_dir_fd < 0) {
    log_debug("  [SCAN] control dir watch unavailable for %s: %s", LOG_DIR,
              strerror(errno));
  }

  return true;
}

void sm_scanner_wake(void) {
  sig_atomic_t wake_fd = g_scanner_wake_write_fd;
  if (wake_fd < 0)
    return;

  static const char token = 'S';
  (void)write((int)wake_fd, &token, sizeof(token));
}

bool sm_scanner_run_startup_sync(void) {
  if (should_stop_requested())
    return false;

  return run_full_scan_cycle(true, NULL, NULL);
}

void sm_scanner_run_loop(void) {
  if (g_scanner_wake_pipe[0] < 0 || g_scanner_wake_pipe[1] < 0) {
    request_scanner_shutdown("scanner wake pipe unavailable");
    return;
  }

  int kq = kqueue();
  if (kq < 0) {
    char reason[128];
    snprintf(reason, sizeof(reason), "scanner kqueue init failed: %s",
             strerror(errno));
    request_scanner_shutdown(reason);
    return;
  }

  struct kevent wake_event;
  EV_SET(&wake_event, (uintptr_t)g_scanner_wake_pipe[0], EVFILT_READ,
         EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, NULL);
  if (kevent(kq, &wake_event, 1, NULL, 0, NULL) != 0) {
    char reason[128];
    snprintf(reason, sizeof(reason), "scanner wake pipe registration failed: %s",
             strerror(errno));
    close(kq);
    request_scanner_shutdown(reason);
    return;
  }

  if (!register_control_dir_watch(kq) || !rebuild_all_scan_root_watch_trees(kq)) {
    close(kq);
    clear_scanner_watch_entries();
    request_scanner_shutdown("scanner watcher initialization failed");
    return;
  }

  uint64_t next_full_resync_us =
      monotonic_time_us() + scanner_full_resync_interval_us();

  while (true) {
    if (should_stop_requested()) {
      log_debug("[SHUTDOWN] stop requested");
      break;
    }

    const char *scan_reason = NULL;
    if (consume_scan_now_request(&scan_reason)) {
      bool unstable_found = false;
      if (!run_full_scan_cycle(false, scan_reason, &unstable_found))
        break;
      clear_all_dirty_scan_roots();
      if (!rebuild_all_scan_root_watch_trees(kq) ||
          !drain_scanner_events_nowait(kq)) {
        close(kq);
        clear_scanner_watch_entries();
        request_scanner_shutdown("scanner watcher refresh failed");
        return;
      }

      uint64_t now_us = monotonic_time_us();
      next_full_resync_us = now_us + scanner_full_resync_interval_us();
      if (unstable_found) {
        uint64_t retry_due = now_us + scanner_stability_wait_us();
        if (retry_due < next_full_resync_us)
          next_full_resync_us = retry_due;
      }
      continue;
    }

    uint64_t now_us = monotonic_time_us();
    if (now_us >= next_full_resync_us) {
      bool unstable_found = false;
      if (!run_full_scan_cycle(false, NULL, &unstable_found))
        break;
      clear_all_dirty_scan_roots();
      if (!rebuild_all_scan_root_watch_trees(kq) ||
          !drain_scanner_events_nowait(kq)) {
        close(kq);
        clear_scanner_watch_entries();
        request_scanner_shutdown("scanner watcher refresh after full resync failed");
        return;
      }

      now_us = monotonic_time_us();
      next_full_resync_us = now_us + scanner_full_resync_interval_us();
      if (unstable_found) {
        uint64_t retry_due = now_us + scanner_stability_wait_us();
        if (retry_due < next_full_resync_us)
          next_full_resync_us = retry_due;
      }
      continue;
    }

    int cleanup_root_index = find_pending_cleanup_scan_root();
    if (cleanup_root_index >= 0) {
      g_scanner_root_states[cleanup_root_index].cleanup_pending = false;
      cleanup_lost_sources_for_scan_root(get_scan_path(cleanup_root_index));
      continue;
    }

    int dirty_root_index = find_due_dirty_scan_root(now_us);
    if (dirty_root_index >= 0) {
      bool rebuild_watch_tree =
          g_scanner_root_states[dirty_root_index].watch_tree_stale;
      uint8_t rebuild_watch_tree_depth =
          g_scanner_root_states[dirty_root_index].watch_tree_rebuild_depth;
      scanner_watch_kind_t rebuild_watch_tree_kind =
          g_scanner_root_states[dirty_root_index].watch_tree_rebuild_kind;
      char rebuild_watch_tree_path[MAX_PATH];
      (void)strlcpy(rebuild_watch_tree_path,
                    g_scanner_root_states[dirty_root_index]
                        .watch_tree_rebuild_path,
                    sizeof(rebuild_watch_tree_path));
      g_scanner_root_states[dirty_root_index].cleanup_pending = false;
      g_scanner_root_states[dirty_root_index].dirty = false;
      g_scanner_root_states[dirty_root_index].ready_after_us = 0;
      clear_scan_root_watch_tree_state(dirty_root_index);

      bool unstable_found = false;
      if (!run_targeted_scan_cycle(dirty_root_index, &unstable_found))
        break;

      if (rebuild_watch_tree &&
          !rebuild_scan_root_watch_subtree(kq, dirty_root_index,
                                           rebuild_watch_tree_path,
                                           rebuild_watch_tree_depth,
                                           rebuild_watch_tree_kind)) {
        close(kq);
        clear_scanner_watch_entries();
        request_scanner_shutdown("scanner root watcher rebuild failed");
        return;
      }
      if (!drain_scanner_events_nowait(kq)) {
        close(kq);
        clear_scanner_watch_entries();
        request_scanner_shutdown("scanner event drain failed");
        return;
      }

      if (unstable_found)
        schedule_scan_root_dirty(dirty_root_index, monotonic_time_us(), false);
      continue;
    }

    uint64_t deadline_us = compute_next_scan_deadline_us(next_full_resync_us);
    struct timespec timeout;
    build_wait_timeout(&timeout, now_us, deadline_us);

    bool timed_out = false;
    if (!process_scanner_events(kq, &timeout, &timed_out)) {
      close(kq);
      clear_scanner_watch_entries();
      request_scanner_shutdown("scanner kevent wait failed");
      return;
    }
    if (should_stop_requested()) {
      log_debug("[SHUTDOWN] stop requested during scanner wait");
      break;
    }

    (void)timed_out;
  }

  close(kq);
  clear_scanner_watch_entries();
}

void sm_scanner_shutdown(void) {
  clear_scanner_watch_entries();
  close_scanner_control_dir();
  close_scanner_wake_pipe();
  reset_scanner_root_states();
}
