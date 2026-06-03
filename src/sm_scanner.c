#include "sm_platform.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/select.h>

#include "sm_config_mount.h"
#include "sm_appdb.h"
#include "sm_fakelib.h"
#include "sm_filesystem.h"
#include "sm_game_lifecycle.h"
#include "sm_hash.h"
#include "sm_image.h"
#include "sm_install.h"
#include "sm_install_queue.h"
#include "sm_kstuff.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_path_utils.h"
#include "sm_paths.h"
#include "sm_runtime.h"
#include "sm_scan.h"
#include "sm_scan_tree.h"
#include "sm_scanner.h"
#include "sm_time.h"
#include "sm_types.h"

#define SCANNER_EVENT_BATCH 32
#define SCANNER_EVENT_DRAIN_BATCHES 8
#define SCANNER_WATCH_INDEX_NONE ((size_t)-1)
#define SCANNER_CONFIG_RELOAD_DEBOUNCE_US 250000ull
#define SCANNER_CONFIG_PROBE_INTERVAL_US 10000000ull
#define SCANNER_MANUAL_RELOAD_DEBOUNCE_US 250000ull
#define SCANNER_MANUAL_PROBE_INTERVAL_US 10000000ull

typedef enum {
  SCANNER_WATCH_SCAN_ROOT = 0,
  SCANNER_WATCH_SCAN_ROOT_PARENT,
  SCANNER_WATCH_SCAN_BACKPORT_ROOT,
  SCANNER_WATCH_SCAN_SUBDIR,
  SCANNER_WATCH_SCAN_IMAGE_FILE,
} scanner_watch_kind_t;

typedef struct {
  int fd;
  int scan_root_index;
  uint8_t depth;
  scanner_watch_kind_t kind;
  size_t prev_root_watch_index;
  size_t next_root_watch_index;
  char path[MAX_PATH];
} scanner_watch_entry_t;

typedef struct {
  bool dirty;
  bool cleanup_pending;
  bool watch_tree_stale;
  bool root_present;
  uint8_t watch_tree_rebuild_depth;
  scanner_watch_kind_t watch_tree_rebuild_kind;
  uint64_t root_device;
  uint64_t root_inode;
  uint64_t ready_after_us;
  char watch_tree_rebuild_path[MAX_PATH];
} scanner_root_state_t;

static int g_scanner_wake_pipe[2] = {-1, -1};
static int g_scanner_config_fd = -1;
static int g_scanner_manual_fd = -1;
static volatile sig_atomic_t g_scanner_wake_write_fd = -1;
static scan_candidate_t g_scanner_scan_candidates[MAX_PENDING];
static scanner_watch_entry_t *g_scanner_watch_entries = NULL;
static size_t g_scanner_watch_count = 0;
static size_t g_scanner_watch_capacity = 0;
static size_t g_scanner_root_watch_heads[MAX_SCAN_PATHS];
static size_t *g_scanner_watch_fd_index = NULL;
static size_t g_scanner_watch_fd_index_capacity = 0;
static scanner_root_state_t g_scanner_root_states[MAX_SCAN_PATHS];
static bool g_scanner_config_reload_pending = false;
static uint64_t g_scanner_config_reload_ready_after_us = 0;
static uint64_t g_scanner_config_probe_due_us = 0;
static uint64_t g_scanner_manual_scan_due_us = 0;
static uint64_t g_scanner_manual_probe_due_us = 0;

static uint64_t scanner_stability_wait_us(void) {
  return (uint64_t)runtime_config()->stability_wait_seconds * 1000000ull;
}

static uint64_t scanner_full_resync_interval_us(void) {
  return (uint64_t)runtime_config()->scan_interval_us;
}

static void schedule_config_reload(uint64_t now_us) {
  g_scanner_config_reload_pending = true;
  g_scanner_config_reload_ready_after_us =
      now_us + SCANNER_CONFIG_RELOAD_DEBOUNCE_US;
}

static void schedule_config_probe(uint64_t now_us) {
  g_scanner_config_probe_due_us = now_us + SCANNER_CONFIG_PROBE_INTERVAL_US;
}

static void schedule_manual_scan(uint64_t now_us) {
  g_scanner_manual_scan_due_us = now_us + SCANNER_MANUAL_RELOAD_DEBOUNCE_US;
}

static void schedule_manual_probe(uint64_t now_us) {
  g_scanner_manual_probe_due_us = now_us + SCANNER_MANUAL_PROBE_INTERVAL_US;
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

static void reset_scanner_root_watch_heads(void) {
  for (int i = 0; i < MAX_SCAN_PATHS; i++)
    g_scanner_root_watch_heads[i] = SCANNER_WATCH_INDEX_NONE;
}

static void clear_scan_root_watch_tree_state(int scan_root_index) {
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

static void close_scanner_config_file(void) {
  if (g_scanner_config_fd >= 0) {
    close(g_scanner_config_fd);
    g_scanner_config_fd = -1;
  }
}

static void close_scanner_manual_file(void) {
  if (g_scanner_manual_fd >= 0) {
    close(g_scanner_manual_fd);
    g_scanner_manual_fd = -1;
  }
}

static void clear_scanner_watch_entries(void) {
  for (size_t i = 0; i < g_scanner_watch_count; i++) {
    if (g_scanner_watch_entries[i].fd >= 0)
      close(g_scanner_watch_entries[i].fd);
  }
  free(g_scanner_watch_entries);
  free(g_scanner_watch_fd_index);
  g_scanner_watch_entries = NULL;
  g_scanner_watch_fd_index = NULL;
  g_scanner_watch_count = 0;
  g_scanner_watch_capacity = 0;
  g_scanner_watch_fd_index_capacity = 0;
  reset_scanner_root_watch_heads();
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

static void clear_scanner_config_reload_state(void) {
  g_scanner_config_reload_pending = false;
  g_scanner_config_reload_ready_after_us = 0;
  g_scanner_config_probe_due_us = 0;
}

static void clear_scanner_manual_scan_state(void) {
  g_scanner_manual_scan_due_us = 0;
  g_scanner_manual_probe_due_us = 0;
}

static bool fakelib_runtime_config_changed(const runtime_config_t *old_cfg,
                                           const runtime_config_t *new_cfg) {
  return old_cfg->backport_fakelib_enabled !=
             new_cfg->backport_fakelib_enabled ||
         old_cfg->global_fakelib_enabled != new_cfg->global_fakelib_enabled ||
         old_cfg->global_fakelib_mount_first !=
             new_cfg->global_fakelib_mount_first ||
         strcmp(old_cfg->global_fakelib_path,
                new_cfg->global_fakelib_path) != 0 ||
         old_cfg->global_fakelib_exclude_title_count !=
             new_cfg->global_fakelib_exclude_title_count ||
         memcmp(old_cfg->global_fakelib_exclude_title_ids,
                new_cfg->global_fakelib_exclude_title_ids,
                sizeof(old_cfg->global_fakelib_exclude_title_ids)) != 0;
}

static uint32_t scanner_config_topology_hash(void) {
  uint32_t hash = 2166136261u;
  int scan_path_count = get_scan_path_count();
  uint32_t values[] = {
      runtime_config()->scan_depth,
      runtime_config()->watch_subdirs ? 1u : 0u,
      runtime_config()->watch_image_files ? 1u : 0u,
      (uint32_t)scan_path_count,
  };

  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
      hash ^= (values[i] >> shift) & 0xffu;
      hash *= 16777619u;
    }
  }
  for (int i = 0; i < scan_path_count; i++) {
    hash ^= sm_fnv1a32(get_scan_path(i));
    hash *= 16777619u;
  }
  return hash;
}

static size_t scanner_watch_fd_hash(uintptr_t ident) {
  return (size_t)((ident * 11400714819323198485ull) >>
                  (sizeof(uintptr_t) >= sizeof(uint64_t) ? 0 : 1));
}

static bool insert_scanner_watch_fd_index_entry(uintptr_t ident,
                                                size_t watch_index) {
  if (!g_scanner_watch_fd_index || g_scanner_watch_fd_index_capacity == 0)
    return false;

  size_t mask = g_scanner_watch_fd_index_capacity - 1u;
  size_t slot = scanner_watch_fd_hash(ident) & mask;
  while (g_scanner_watch_fd_index[slot] != SCANNER_WATCH_INDEX_NONE)
    slot = (slot + 1u) & mask;

  g_scanner_watch_fd_index[slot] = watch_index;
  return true;
}

static bool rebuild_scanner_watch_fd_index_with_count(size_t watch_count) {
  size_t needed_capacity = 16u;
  while (needed_capacity < (watch_count * 2u))
    needed_capacity *= 2u;

  if (g_scanner_watch_fd_index_capacity != needed_capacity) {
    size_t *new_index =
        realloc(g_scanner_watch_fd_index, needed_capacity * sizeof(*new_index));
    if (!new_index) {
      log_debug("  [SCAN] watcher fd index allocation failed");
      return false;
    }
    g_scanner_watch_fd_index = new_index;
    g_scanner_watch_fd_index_capacity = needed_capacity;
  }

  for (size_t i = 0; i < g_scanner_watch_fd_index_capacity; i++)
    g_scanner_watch_fd_index[i] = SCANNER_WATCH_INDEX_NONE;

  for (size_t i = 0; i < g_scanner_watch_count; i++) {
    if (!insert_scanner_watch_fd_index_entry(
            (uintptr_t)g_scanner_watch_entries[i].fd, i)) {
      return false;
    }
  }

  return true;
}

static bool ensure_scanner_watch_fd_index_capacity(size_t watch_count) {
  size_t needed_capacity = 16u;
  while (needed_capacity < (watch_count * 2u))
    needed_capacity *= 2u;

  if (g_scanner_watch_fd_index && g_scanner_watch_fd_index_capacity >= needed_capacity)
    return true;

  return rebuild_scanner_watch_fd_index_with_count(watch_count);
}

static bool rebuild_scanner_watch_fd_index(void) {
  return rebuild_scanner_watch_fd_index_with_count(g_scanner_watch_count);
}

static void link_scanner_watch_entry_to_root(size_t index) {
  scanner_watch_entry_t *entry = &g_scanner_watch_entries[index];
  size_t head = g_scanner_root_watch_heads[entry->scan_root_index];
  entry->prev_root_watch_index = SCANNER_WATCH_INDEX_NONE;
  entry->next_root_watch_index = head;
  if (head != SCANNER_WATCH_INDEX_NONE)
    g_scanner_watch_entries[head].prev_root_watch_index = index;
  g_scanner_root_watch_heads[entry->scan_root_index] = index;
}

static void unlink_scanner_watch_entry_from_root(size_t index) {
  scanner_watch_entry_t *entry = &g_scanner_watch_entries[index];
  if (entry->prev_root_watch_index != SCANNER_WATCH_INDEX_NONE) {
    g_scanner_watch_entries[entry->prev_root_watch_index].next_root_watch_index =
        entry->next_root_watch_index;
  } else {
    g_scanner_root_watch_heads[entry->scan_root_index] =
        entry->next_root_watch_index;
  }
  if (entry->next_root_watch_index != SCANNER_WATCH_INDEX_NONE) {
    g_scanner_watch_entries[entry->next_root_watch_index].prev_root_watch_index =
        entry->prev_root_watch_index;
  }
  entry->prev_root_watch_index = SCANNER_WATCH_INDEX_NONE;
  entry->next_root_watch_index = SCANNER_WATCH_INDEX_NONE;
}

static void rebind_scanner_watch_entry_root_index(size_t old_index,
                                                  size_t new_index) {
  scanner_watch_entry_t *entry = &g_scanner_watch_entries[new_index];
  if (g_scanner_root_watch_heads[entry->scan_root_index] == old_index)
    g_scanner_root_watch_heads[entry->scan_root_index] = new_index;
  if (entry->prev_root_watch_index != SCANNER_WATCH_INDEX_NONE) {
    g_scanner_watch_entries[entry->prev_root_watch_index].next_root_watch_index =
        new_index;
  }
  if (entry->next_root_watch_index != SCANNER_WATCH_INDEX_NONE) {
    g_scanner_watch_entries[entry->next_root_watch_index].prev_root_watch_index =
        new_index;
  }
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
  if (!ensure_scanner_watch_fd_index_capacity(g_scanner_watch_count + 1u)) {
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
  entry->prev_root_watch_index = SCANNER_WATCH_INDEX_NONE;
  entry->next_root_watch_index = SCANNER_WATCH_INDEX_NONE;
  (void)strlcpy(entry->path, path, sizeof(entry->path));
  link_scanner_watch_entry_to_root(g_scanner_watch_count - 1u);
  if (!insert_scanner_watch_fd_index_entry((uintptr_t)fd,
                                           g_scanner_watch_count - 1u)) {
    unlink_scanner_watch_entry_from_root(g_scanner_watch_count - 1u);
    memset(entry, 0, sizeof(*entry));
    close(fd);
    g_scanner_watch_count--;
    (void)rebuild_scanner_watch_fd_index();
    return false;
  }
  return true;
}

static void remove_scanner_watch_entry_at(size_t index) {
  if (index >= g_scanner_watch_count)
    return;

  unlink_scanner_watch_entry_from_root(index);
  if (g_scanner_watch_entries[index].fd >= 0)
    close(g_scanner_watch_entries[index].fd);

  size_t last_index = g_scanner_watch_count - 1u;
  if (index != last_index) {
    g_scanner_watch_entries[index] = g_scanner_watch_entries[last_index];
    rebind_scanner_watch_entry_root_index(last_index, index);
  }
  memset(&g_scanner_watch_entries[last_index], 0,
         sizeof(g_scanner_watch_entries[last_index]));
  g_scanner_watch_count--;
}

static bool remove_scan_root_watch_entries(int scan_root_index) {
  bool removed_any = false;
  while (g_scanner_root_watch_heads[scan_root_index] != SCANNER_WATCH_INDEX_NONE) {
    remove_scanner_watch_entry_at(g_scanner_root_watch_heads[scan_root_index]);
    removed_any = true;
  }

  return !removed_any || rebuild_scanner_watch_fd_index();
}

static bool remove_scan_root_watch_entries_for_path(int scan_root_index,
                                                    const char *path) {
  size_t index = g_scanner_root_watch_heads[scan_root_index];
  bool removed_any = false;
  while (index != SCANNER_WATCH_INDEX_NONE) {
    if (path_matches_root_or_child(g_scanner_watch_entries[index].path, path)) {
      remove_scanner_watch_entry_at(index);
      removed_any = true;
      index = g_scanner_root_watch_heads[scan_root_index];
      continue;
    }
    index = g_scanner_watch_entries[index].next_root_watch_index;
  }

  return !removed_any || rebuild_scanner_watch_fd_index();
}

static scanner_watch_entry_t *find_scanner_watch_entry_by_fd(uintptr_t ident) {
  if (!g_scanner_watch_fd_index || g_scanner_watch_fd_index_capacity == 0)
    return NULL;

  size_t mask = g_scanner_watch_fd_index_capacity - 1u;
  size_t slot = scanner_watch_fd_hash(ident) & mask;
  while (g_scanner_watch_fd_index[slot] != SCANNER_WATCH_INDEX_NONE) {
    size_t watch_index = g_scanner_watch_fd_index[slot];
    if ((uintptr_t)g_scanner_watch_entries[watch_index].fd == ident)
      return &g_scanner_watch_entries[watch_index];
    slot = (slot + 1u) & mask;
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

static bool stat_directory_identity(const char *path, uint64_t *device_out,
                                    uint64_t *inode_out) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
    return false;

  if (device_out)
    *device_out = (uint64_t)st.st_dev;
  if (inode_out)
    *inode_out = (uint64_t)st.st_ino;
  return true;
}

static bool update_scan_root_presence_state(int scan_root_index,
                                            const char *scan_root,
                                            bool *present_out) {
  uint64_t root_device = 0;
  uint64_t root_inode = 0;
  bool present =
      stat_directory_identity(scan_root, &root_device, &root_inode);
  scanner_root_state_t *state = &g_scanner_root_states[scan_root_index];
  bool changed = state->root_present != present;
  if (present && state->root_present &&
      (state->root_device != root_device || state->root_inode != root_inode)) {
    changed = true;
  }

  state->root_present = present;
  state->root_device = present ? root_device : 0;
  state->root_inode = present ? root_inode : 0;
  *present_out = present;
  return changed;
}

static bool resolve_existing_parent_directory_path(
    const char *path, char parent_path[MAX_PATH]) {
  char current_path[MAX_PATH];
  (void)strlcpy(current_path, path, sizeof(current_path));

  while (true) {
    char candidate_parent[MAX_PATH];
    if (!build_parent_directory_path(current_path, candidate_parent))
      return false;
    if (strcmp(candidate_parent, current_path) == 0)
      return false;
    if (stat_directory_identity(candidate_parent, NULL, NULL)) {
      (void)strlcpy(parent_path, candidate_parent, MAX_PATH);
      return true;
    }
    (void)strlcpy(current_path, candidate_parent, sizeof(current_path));
  }
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
    *rebuild_depth_out = entry->depth;
    *kind_out = entry->kind;
    return true;
  case SCANNER_WATCH_SCAN_ROOT_PARENT:
    (void)strlcpy(rebuild_path, get_scan_path(entry->scan_root_index),
                  MAX_PATH);
    *rebuild_depth_out = 0;
    *kind_out = SCANNER_WATCH_SCAN_ROOT;
    return true;
  case SCANNER_WATCH_SCAN_IMAGE_FILE:
    if (!build_parent_directory_path(entry->path, rebuild_path))
      return false;
    *rebuild_depth_out = (entry->depth > 0u) ? (uint8_t)(entry->depth - 1u) : 0u;
    *kind_out =
        (entry->depth <= 1u) ? SCANNER_WATCH_SCAN_ROOT : SCANNER_WATCH_SCAN_SUBDIR;
    return true;
  default:
    return false;
  }
}

typedef struct {
  int kq;
  int scan_root_index;
  bool watch_subdirs;
} register_watch_tree_ctx_t;

static sm_scan_tree_dir_visit_t register_watch_directory_visit(
    const char *dir_path, unsigned int depth_from_root, void *ctx_ptr) {
  register_watch_tree_ctx_t *ctx = (register_watch_tree_ctx_t *)ctx_ptr;
  scanner_watch_kind_t kind =
      (depth_from_root == 0u) ? SCANNER_WATCH_SCAN_ROOT
                              : SCANNER_WATCH_SCAN_SUBDIR;
  if (depth_from_root == 0u || ctx->watch_subdirs) {
    if (!register_scanner_watch_entry(ctx->kq, ctx->scan_root_index, dir_path,
                                      kind, (uint8_t)depth_from_root)) {
      return SM_SCAN_TREE_DIR_ABORT;
    }
  }

  return SM_SCAN_TREE_DIR_DESCEND;
}

static bool register_watch_image_visit(const char *image_path,
                                       const char *image_name,
                                       unsigned int depth_from_root,
                                       void *ctx_ptr) {
  (void)image_name;

  register_watch_tree_ctx_t *ctx = (register_watch_tree_ctx_t *)ctx_ptr;
  return register_scanner_watch_entry(ctx->kq, ctx->scan_root_index, image_path,
                                      SCANNER_WATCH_SCAN_IMAGE_FILE,
                                      (uint8_t)depth_from_root);
}

static bool register_scan_root_parent_watch(int kq, int scan_root_index,
                                            const char *scan_root) {
  char parent_path[MAX_PATH];
  if (!resolve_existing_parent_directory_path(scan_root, parent_path))
    return true;

  return register_scanner_watch_entry(kq, scan_root_index, parent_path,
                                      SCANNER_WATCH_SCAN_ROOT_PARENT, 0u);
}

static bool rebuild_scan_root_watch_tree(int kq, int scan_root_index) {
  const char *scan_root = get_scan_path(scan_root_index);
  if (!remove_scan_root_watch_entries(scan_root_index))
    return false;

  bool root_present = false;
  (void)update_scan_root_presence_state(scan_root_index, scan_root,
                                        &root_present);
  if (!root_present) {
    if (!register_scan_root_parent_watch(kq, scan_root_index, scan_root))
      return false;
    clear_scan_root_watch_tree_state(scan_root_index);
    return true;
  }

  const runtime_config_t *cfg = runtime_config();
  if (!cfg->watch_subdirs && !cfg->watch_image_files) {
    if (!register_scanner_watch_entry(kq, scan_root_index, scan_root,
                                      SCANNER_WATCH_SCAN_ROOT, 0u)) {
      return false;
    }
  } else {
    unsigned int scan_depth = get_scan_depth_for_root(scan_root);
    register_watch_tree_ctx_t walk_ctx = {
        .kq = kq,
        .scan_root_index = scan_root_index,
        .watch_subdirs = cfg->watch_subdirs,
    };
    sm_scan_tree_callbacks_t callbacks = {
        .on_directory = register_watch_directory_visit,
        .on_image_file =
            cfg->watch_image_files ? register_watch_image_visit : NULL,
    };
    if (!sm_scan_tree_walk(scan_root, scan_root, 0u, scan_depth, &callbacks,
                           &walk_ctx)) {
      return false;
    }
  }

  char backport_root[MAX_PATH];
  if (build_backports_root_path(scan_root, backport_root)) {
    if (!register_scanner_watch_entry(kq, scan_root_index, backport_root,
                                      SCANNER_WATCH_SCAN_BACKPORT_ROOT, 1u)) {
      return false;
    }
  }

  if (!register_scan_root_parent_watch(kq, scan_root_index, scan_root))
    return false;

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
    if (!remove_scan_root_watch_entries_for_path(scan_root_index, rebuild_path))
      return false;
    if (!register_scanner_watch_entry(kq, scan_root_index, rebuild_path,
                                      SCANNER_WATCH_SCAN_BACKPORT_ROOT,
                                      rebuild_depth)) {
      return false;
    }
    clear_scan_root_watch_tree_state(scan_root_index);
    return true;
  }

  if (!remove_scan_root_watch_entries_for_path(scan_root_index, rebuild_path))
    return false;
  const runtime_config_t *cfg = runtime_config();
  if (!cfg->watch_subdirs && !cfg->watch_image_files) {
    clear_scan_root_watch_tree_state(scan_root_index);
    return true;
  }

  unsigned int scan_depth = get_scan_depth_for_root(scan_root);
  if (rebuild_depth > scan_depth) {
    clear_scan_root_watch_tree_state(scan_root_index);
    return true;
  }

  register_watch_tree_ctx_t walk_ctx = {
      .kq = kq,
      .scan_root_index = scan_root_index,
      .watch_subdirs = cfg->watch_subdirs,
  };
  sm_scan_tree_callbacks_t callbacks = {
      .on_directory = register_watch_directory_visit,
      .on_image_file = cfg->watch_image_files ? register_watch_image_visit : NULL,
  };
  if (!sm_scan_tree_walk(scan_root, rebuild_path, rebuild_depth,
                         scan_depth - rebuild_depth, &callbacks, &walk_ctx)) {
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
  g_scanner_root_states[scan_root_index].cleanup_pending = true;
}

static void schedule_scan_root_dirty(int scan_root_index, uint64_t now_us,
                                     bool immediate) {
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
    return entry->depth <
               get_scan_depth_for_root(get_scan_path(entry->scan_root_index)) &&
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

static void register_config_file_watch(int kq, uint64_t now_us) {
  if (g_scanner_config_fd < 0)
    return;

  struct kevent kev;
  EV_SET(&kev, (uintptr_t)g_scanner_config_fd, EVFILT_VNODE,
         EV_ADD | EV_ENABLE | EV_CLEAR,
         NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME |
             NOTE_REVOKE,
         0, NULL);
  if (kevent(kq, &kev, 1, NULL, 0, NULL) != 0) {
    log_debug("  [CFG] config watcher registration failed: %s",
              strerror(errno));
    close_scanner_config_file();
    schedule_config_probe(now_us);
    return;
  }

  g_scanner_config_probe_due_us = 0;
}

static void reopen_config_file_watch(int kq, uint64_t now_us) {
  close_scanner_config_file();

  g_scanner_config_fd = open(CONFIG_FILE, O_RDONLY);
  if (g_scanner_config_fd < 0) {
    if (errno != ENOENT) {
      log_debug("  [CFG] config watcher unavailable for %s: %s", CONFIG_FILE,
                strerror(errno));
    }
    schedule_config_probe(now_us);
    return;
  }

  register_config_file_watch(kq, now_us);
}

static void register_manual_file_watch(int kq, uint64_t now_us) {
  if (g_scanner_manual_fd < 0)
    return;

  struct kevent kev;
  EV_SET(&kev, (uintptr_t)g_scanner_manual_fd, EVFILT_VNODE,
         EV_ADD | EV_ENABLE | EV_CLEAR,
         NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME |
             NOTE_REVOKE,
         0, NULL);
  if (kevent(kq, &kev, 1, NULL, 0, NULL) != 0) {
    log_debug("  [MANUAL] manual.lst watcher registration failed: %s",
              strerror(errno));
    close_scanner_manual_file();
    schedule_manual_probe(now_us);
    return;
  }

  g_scanner_manual_probe_due_us = 0;
}

static int open_manual_list_file(void) {
  mkdir(LOG_DIR, 0777);
  int fd = open(MANUAL_LIST_FILE, O_RDONLY | O_CREAT, 0666);
  if (fd < 0) {
    log_debug("  [MANUAL] manual.lst open/create failed for %s: %s",
              MANUAL_LIST_FILE, strerror(errno));
  }
  return fd;
}

static void reopen_manual_file_watch(int kq, uint64_t now_us) {
  close_scanner_manual_file();

  g_scanner_manual_fd = open_manual_list_file();
  if (g_scanner_manual_fd < 0) {
    schedule_manual_probe(now_us);
    return;
  }

  register_manual_file_watch(kq, now_us);
}

static bool apply_runtime_config_reload_effects(int kq,
                                                const runtime_config_t *old_cfg,
                                                const runtime_config_t *new_cfg,
                                                bool scan_topology_changed) {
  if (old_cfg->backport_fakelib_enabled &&
      fakelib_runtime_config_changed(old_cfg, new_cfg))
    sm_fakelib_game_shutdown();

  sm_kstuff_on_config_reload();

  if (scan_topology_changed) {
    clear_scanner_watch_entries();
    reset_scanner_root_states();
    if (!rebuild_all_scan_root_watch_trees(kq)) {
      log_debug("  [CFG] scanner watch rebuild failed after config reload");
      return false;
    } else {
      log_debug("  [CFG] scanner watches rebuilt after scan config reload");
    }
  }

  if (!refresh_game_lifecycle_watcher()) {
    log_debug("  [CFG] lifecycle watcher refresh failed after config reload");
  }
  return true;
}

static bool should_abort_scan_cycle(void) {
  return should_stop_requested() || runtime_sleep_mode_active();
}

static bool run_full_scan_cycle(bool startup_sync, const char *reason,
                                bool *unstable_found_out) {
  scan_candidate_t *candidates = g_scanner_scan_candidates;

  log_immediate_scan_reason(reason);

  if (should_abort_scan_cycle())
    return false;

  bool unstable_found = false;
  cleanup_lost_sources_before_scan();
  if (should_abort_scan_cycle())
    return false;

  int total_found_games = 0;
  int *total_found_ptr = startup_sync ? &total_found_games : NULL;
  int candidate_count = collect_scan_candidates(candidates, MAX_PENDING,
                                                total_found_ptr,
                                                &unstable_found);
  if (should_abort_scan_cycle())
    return false;

  if (candidate_count > 0 && startup_sync) {
    int new_games = 0;
    for (int i = 0; i < candidate_count; i++) {
      if (!candidates[i].installed)
        new_games++;
    }
    if (new_games > 0)
      notify_system_info("Found %d new games. Executing...", new_games);
  }

  process_scan_candidates(candidates, candidate_count);
  if (should_abort_scan_cycle())
    return false;

  mount_backport_overlays(&unstable_found);
  if (should_abort_scan_cycle())
    return false;

  if (unstable_found_out)
    *unstable_found_out = unstable_found;

  if (startup_sync && !should_abort_scan_cycle()) {
    notify_system_rich(true, "Library Synchronized.\nFound %d games.",
                       total_found_games);
  }

  return !should_abort_scan_cycle();
}

static bool run_targeted_scan_cycle(int scan_root_index,
                                    bool *unstable_found_out) {
  const char *scan_root = get_scan_path(scan_root_index);
  scan_candidate_t *candidates = g_scanner_scan_candidates;

  log_debug("[SCAN] running targeted scan for %s", scan_root);

  if (should_abort_scan_cycle())
    return false;

  bool unstable_found = false;
  cleanup_lost_sources_for_scan_root(scan_root);
  if (should_abort_scan_cycle())
    return false;

  int candidate_count = collect_scan_candidates_for_scan_root(
      scan_root, candidates, MAX_PENDING, NULL, &unstable_found);
  if (should_abort_scan_cycle())
    return false;

  process_scan_candidates(candidates, candidate_count);
  if (should_abort_scan_cycle())
    return false;

  mount_backport_overlays(&unstable_found);
  if (should_abort_scan_cycle())
    return false;

  if (unstable_found_out)
    *unstable_found_out = unstable_found;

  return !should_abort_scan_cycle();
}

static int find_pending_cleanup_scan_root(void) {
  for (int i = 0; i < get_scan_path_count(); i++) {
    if (g_scanner_root_states[i].cleanup_pending)
      return i;
  }

  return -1;
}

static bool config_reload_due(uint64_t now_us) {
  return g_scanner_config_reload_pending &&
         now_us >= g_scanner_config_reload_ready_after_us;
}

static bool config_probe_due(uint64_t now_us) {
  return g_scanner_config_fd < 0 && g_scanner_config_probe_due_us != 0 &&
         now_us >= g_scanner_config_probe_due_us;
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

static uint64_t compute_next_scan_deadline_us(uint64_t now_us,
                                              uint64_t full_resync_due_us) {
  uint64_t next_deadline = full_resync_due_us;
  uint64_t install_wake_us = sm_install_next_wake_us(now_us);

  if (install_wake_us != 0 &&
      (next_deadline == 0 || install_wake_us < next_deadline)) {
    next_deadline = install_wake_us;
  }

  if (g_scanner_config_reload_pending &&
      (next_deadline == 0 ||
       g_scanner_config_reload_ready_after_us < next_deadline)) {
    next_deadline = g_scanner_config_reload_ready_after_us;
  }

  if (g_scanner_config_fd < 0 && g_scanner_config_probe_due_us != 0 &&
      (next_deadline == 0 || g_scanner_config_probe_due_us < next_deadline)) {
    next_deadline = g_scanner_config_probe_due_us;
  }

  if (g_scanner_manual_scan_due_us != 0 &&
      (next_deadline == 0 || g_scanner_manual_scan_due_us < next_deadline)) {
    next_deadline = g_scanner_manual_scan_due_us;
  }

  if (g_scanner_manual_fd < 0 && g_scanner_manual_probe_due_us != 0 &&
      (next_deadline == 0 || g_scanner_manual_probe_due_us < next_deadline)) {
    next_deadline = g_scanner_manual_probe_due_us;
  }

  for (int i = 0; i < get_scan_path_count(); i++) {
    const scanner_root_state_t *state = &g_scanner_root_states[i];
    if (!state->dirty)
      continue;
    if (next_deadline == 0 || state->ready_after_us < next_deadline)
      next_deadline = state->ready_after_us;
  }

  return next_deadline;
}

static const struct timespec *build_wait_timeout(struct timespec *timeout,
                                                 uint64_t now_us,
                                                 uint64_t deadline_us) {
  if (deadline_us == 0)
    return NULL;

  memset(timeout, 0, sizeof(*timeout));
  if (deadline_us <= now_us)
    return timeout;

  uint64_t delta_us = deadline_us - now_us;
  timeout->tv_sec = (time_t)(delta_us / 1000000ull);
  timeout->tv_nsec = (long)((delta_us % 1000000ull) * 1000ull);
  return timeout;
}

static bool handle_scan_root_parent_event(
    int kq, const scanner_watch_entry_t *entry, uint64_t now_us) {
  const char *scan_root = get_scan_path(entry->scan_root_index);
  bool root_present = false;
  bool root_changed = update_scan_root_presence_state(
      entry->scan_root_index, scan_root, &root_present);
  if (root_present && root_changed) {
    schedule_scan_root_dirty(entry->scan_root_index, now_us, false);
    schedule_scan_root_watch_tree_rebuild(entry);
    return true;
  }
  if (root_present)
    return true;
  if (root_changed) {
    schedule_scan_root_cleanup(entry->scan_root_index);
    schedule_scan_root_dirty(entry->scan_root_index, now_us, true);
    schedule_scan_root_watch_tree_rebuild(entry);
    return true;
  }

  char parent_path[MAX_PATH];
  if (!resolve_existing_parent_directory_path(scan_root, parent_path) ||
      strcmp(parent_path, entry->path) != 0) {
    return rebuild_scan_root_watch_tree(kq, entry->scan_root_index);
  }

  return true;
}

static bool process_scanner_events(int kq, const struct timespec *timeout,
                                   bool *timed_out_out) {
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

    if (event->ident == (uintptr_t)g_scanner_config_fd) {
      if ((event->fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) != 0)
        reopen_config_file_watch(kq, now_us);
      schedule_config_reload(now_us);
      continue;
    }

    if (event->ident == (uintptr_t)g_scanner_manual_fd) {
      if ((event->fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) != 0) {
        close_scanner_manual_file();
        g_scanner_manual_probe_due_us = now_us == 0 ? 1 : now_us;
        continue;
      }
      schedule_manual_scan(now_us);
      continue;
    }

    scanner_watch_entry_t *watch_entry =
        find_scanner_watch_entry_by_fd(event->ident);
    if (!watch_entry)
      continue;

    if (watch_entry->kind == SCANNER_WATCH_SCAN_ROOT_PARENT) {
      if (!handle_scan_root_parent_event(kq, watch_entry, now_us))
        return false;
      continue;
    }

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

static bool discard_scanner_events_nowait(int kq) {
  struct kevent events[SCANNER_EVENT_BATCH];
  struct timespec timeout;
  memset(&timeout, 0, sizeof(timeout));

  while (true) {
    int nev = kevent(kq, NULL, 0, events, SCANNER_EVENT_BATCH, &timeout);
    if (nev < 0) {
      if (errno == EINTR)
        continue;
      log_debug("  [SCAN] stale event drain failed: %s", strerror(errno));
      return false;
    }
    if (nev == 0)
      return true;
  }
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
  close_scanner_config_file();
  close_scanner_manual_file();
  clear_scanner_watch_entries();
  reset_scanner_root_states();
  clear_scanner_config_reload_state();
  clear_scanner_manual_scan_state();

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

  g_scanner_config_fd = open(CONFIG_FILE, O_RDONLY);
  if (g_scanner_config_fd < 0 && errno != ENOENT) {
    log_debug("  [CFG] config watch unavailable for %s: %s", CONFIG_FILE,
              strerror(errno));
  }
  if (g_scanner_config_fd < 0)
    schedule_config_probe(monotonic_time_us());

  g_scanner_manual_fd = open_manual_list_file();
  if (g_scanner_manual_fd < 0)
    schedule_manual_probe(monotonic_time_us());

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
  while (!should_stop_requested()) {
    while (runtime_sleep_mode_active() && !should_stop_requested())
      sceKernelUsleep(200000);

    if (should_stop_requested())
      return false;
    if (run_full_scan_cycle(true, NULL, NULL))
      return true;
    if (!runtime_sleep_mode_active())
      return false;
  }

  return false;
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

  register_config_file_watch(kq, monotonic_time_us());
  register_manual_file_watch(kq, monotonic_time_us());
  if (!rebuild_all_scan_root_watch_trees(kq)) {
    close(kq);
    clear_scanner_watch_entries();
    close_scanner_config_file();
    close_scanner_manual_file();
    request_scanner_shutdown("scanner watcher initialization failed");
    return;
  }

  uint64_t next_full_resync_us =
      monotonic_time_us() + scanner_full_resync_interval_us();
  bool was_sleeping = false;

  while (true) {
    if (should_stop_requested()) {
      log_debug("[SHUTDOWN] stop requested");
      break;
    }

    if (runtime_sleep_mode_active()) {
      was_sleeping = true;
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(g_scanner_wake_pipe[0], &readfds);
      int rc = select(g_scanner_wake_pipe[0] + 1, &readfds, NULL, NULL, NULL);
      if (rc < 0) {
        if (errno == EINTR)
          continue;
        log_debug("  [SCAN] sleep wait failed: %s", strerror(errno));
        close(kq);
        clear_scanner_watch_entries();
        request_scanner_shutdown("scanner sleep wait failed");
        return;
      }
      drain_scanner_wake_pipe();
      continue;
    }
    if (was_sleeping) {
      was_sleeping = false;
      if (!discard_scanner_events_nowait(kq)) {
        close(kq);
        clear_scanner_watch_entries();
        request_scanner_shutdown("scanner stale event drain failed");
        return;
      }
    }

    char scan_reason[128];
    if (consume_scan_now_request(scan_reason, sizeof(scan_reason))) {
      bool unstable_found = false;
      if (!run_full_scan_cycle(false, scan_reason, &unstable_found)) {
        if (runtime_sleep_mode_active())
          continue;
        break;
      }
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
    if (sm_game_lifecycle_has_active_game()) {
      next_full_resync_us = 0;
    } else if (next_full_resync_us == 0) {
      next_full_resync_us = now_us;
    }

    if (config_probe_due(now_us)) {
      g_scanner_config_probe_due_us = 0;
      reopen_config_file_watch(kq, now_us);
      if (g_scanner_config_fd >= 0)
        schedule_config_reload(now_us);
      continue;
    }

    if (g_scanner_manual_fd < 0 && g_scanner_manual_probe_due_us != 0 &&
        now_us >= g_scanner_manual_probe_due_us) {
      g_scanner_manual_probe_due_us = 0;
      reopen_manual_file_watch(kq, now_us);
      if (g_scanner_manual_fd >= 0)
        schedule_manual_scan(now_us);
      continue;
    }

    if (config_reload_due(now_us)) {
      g_scanner_config_reload_pending = false;
      g_scanner_config_reload_ready_after_us = 0;

      uint32_t old_scan_topology_hash = scanner_config_topology_hash();
      runtime_config_t old_cfg = *runtime_config();
      bool reloaded = false;
      if (!reload_runtime_config_if_changed(&reloaded)) {
        log_debug("  [CFG] runtime config reload failed");
      } else if (reloaded) {
        const runtime_config_t *new_cfg = runtime_config();
        bool scan_topology_changed =
            old_scan_topology_hash != scanner_config_topology_hash();
        if (!apply_runtime_config_reload_effects(kq, &old_cfg, new_cfg,
                                                 scan_topology_changed)) {
          close(kq);
          clear_scanner_watch_entries();
          request_scanner_shutdown("scanner watcher rebuild after config reload failed");
          return;
        }
        if (scan_topology_changed && !discard_scanner_events_nowait(kq)) {
          close(kq);
          clear_scanner_watch_entries();
          request_scanner_shutdown("scanner stale event drain after config reload failed");
          return;
        }
        notify_system("ShadowMount+: config reloaded.");
        log_debug("  [CFG] runtime config reloaded");
        now_us = monotonic_time_us();
        next_full_resync_us =
            scan_topology_changed ? now_us
                                  : now_us + scanner_full_resync_interval_us();
      }
      continue;
    }

    if (g_scanner_manual_scan_due_us != 0 &&
        now_us >= g_scanner_manual_scan_due_us) {
      g_scanner_manual_scan_due_us = 0;
      invalidate_app_db_title_cache();

      bool unstable_found = false;
      if (!run_full_scan_cycle(false, "manual.lst changed", &unstable_found)) {
        if (runtime_sleep_mode_active())
          continue;
        break;
      }
      clear_all_dirty_scan_roots();
      if (!rebuild_all_scan_root_watch_trees(kq) ||
          !drain_scanner_events_nowait(kq)) {
        close(kq);
        clear_scanner_watch_entries();
        request_scanner_shutdown("scanner watcher refresh after manual scan failed");
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

    uint64_t install_wake_us = sm_install_next_wake_us(now_us);
    if (install_wake_us != 0 && now_us >= install_wake_us) {
      sm_install_service_pending();
      continue;
    }

    if (next_full_resync_us != 0 && now_us >= next_full_resync_us) {
      bool unstable_found = false;
      if (!run_full_scan_cycle(false, NULL, &unstable_found)) {
        if (runtime_sleep_mode_active())
          continue;
        break;
      }
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
      bool cleanup_pending =
          g_scanner_root_states[dirty_root_index].cleanup_pending;
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
      if (!run_targeted_scan_cycle(dirty_root_index, &unstable_found)) {
        if (runtime_sleep_mode_active()) {
          scanner_root_state_t *state =
              &g_scanner_root_states[dirty_root_index];
          if (cleanup_pending)
            schedule_scan_root_cleanup(dirty_root_index);
          schedule_scan_root_dirty(dirty_root_index, monotonic_time_us(), true);
          if (rebuild_watch_tree) {
            state->watch_tree_stale = true;
            state->watch_tree_rebuild_depth = rebuild_watch_tree_depth;
            state->watch_tree_rebuild_kind = rebuild_watch_tree_kind;
            (void)strlcpy(state->watch_tree_rebuild_path,
                          rebuild_watch_tree_path,
                          sizeof(state->watch_tree_rebuild_path));
          }
          continue;
        }
        break;
      }

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

    uint64_t deadline_us =
        compute_next_scan_deadline_us(now_us, next_full_resync_us);
    struct timespec timeout;
    const struct timespec *timeout_ptr =
        build_wait_timeout(&timeout, now_us, deadline_us);

    bool timed_out = false;
    if (!process_scanner_events(kq, timeout_ptr, &timed_out)) {
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
  close_scanner_config_file();
  close_scanner_manual_file();
  close_scanner_wake_pipe();
  reset_scanner_root_states();
  clear_scanner_config_reload_state();
  clear_scanner_manual_scan_state();
}
