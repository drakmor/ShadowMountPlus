#include "sm_platform.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/select.h>

#include "sm_config_mount.h"
#include "sm_ampr_updater.h"
#include "sm_api_service.h"
#include "sm_appdb.h"
#include "sm_fakelib.h"
#include "sm_filesystem.h"
#include "sm_game_lifecycle.h"
#include "sm_game_cache.h"
#include "sm_gameinfo.h"
#include "sm_hash.h"
#include "sm_image.h"
#include "sm_install.h"
#include "sm_install_queue.h"
#include "sm_kstuff.h"
#include "sm_limits.h"
#include "sm_l10n.h"
#include "sm_log.h"
#include "sm_path_utils.h"
#include "sm_paths.h"
#include "sm_runtime.h"
#include "sm_scan.h"
#include "sm_scan_tree.h"
#include "sm_scanner.h"
#include "sm_shellcore_service.h"
#include "sm_time.h"
#include "sm_types.h"

#define SCANNER_EVENT_BATCH 32
#define SCANNER_EVENT_DRAIN_BATCHES 8
#define SCANNER_WATCH_INDEX_NONE ((size_t)-1)
#define SCANNER_CONFIG_RELOAD_DEBOUNCE_US 250000ull
#define SCANNER_CONFIG_PROBE_INTERVAL_US 10000000ull
#define SCANNER_MANUAL_RELOAD_DEBOUNCE_US 250000ull
#define SCANNER_MANUAL_PROBE_INTERVAL_US 10000000ull
#define SCANNER_USB_SLOT_COUNT 8
#define SCANNER_USB_MOUNT_PROBE_DELAY_US 1000000ull
#define SCANNER_GIB_BYTES (1024ull * 1024ull * 1024ull)
#define SCANNER_FAKELIB_CACHE_CLEANUP_INTERVAL_US                         \
  (24ull * 60ull * 60ull * 1000000ull)

typedef enum {
  SCANNER_WATCH_SCAN_ROOT = 0,
  SCANNER_WATCH_SCAN_ROOT_PARENT,
  SCANNER_WATCH_SCAN_SUBDIR,
} scanner_watch_kind_t;

typedef struct {
  int fd;
  bool owns_fd;      // Exactly one subscription closes each shared fd.
  bool fd_shareable; // Cleared after terminal vnode events until rebuild.
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
  bool usb_connect_counted;
  uint8_t watch_tree_rebuild_depth;
  scanner_watch_kind_t watch_tree_rebuild_kind;
  uint64_t root_device;
  uint64_t root_inode;
  uint64_t ready_after_us;
  int usb_connect_found_games;
  char watch_tree_rebuild_path[MAX_PATH];
} scanner_root_state_t;

typedef struct {
  int scan_root_index;
  uint8_t depth;
  scanner_watch_kind_t kind;
} scanner_event_subscription_t;

typedef struct {
  uint64_t available_tenths;
  uint64_t capacity_tenths;
  uint64_t block_size_kb;
} scanner_usb_info_t;

static int g_scanner_wake_pipe[2] = {-1, -1};
static atomic_bool g_usb_watches_suspended = ATOMIC_VAR_INIT(true);
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
static uint8_t g_scanner_usb_mounted_mask = 0;
static uint8_t g_scanner_usb_scan_result_pending_mask = 0;
static scanner_usb_info_t g_scanner_usb_info[SCANNER_USB_SLOT_COUNT];
static uint64_t g_scanner_usb_mount_probe_due_us = 0;

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

static void reset_scanner_root_states(void) {
  memset(g_scanner_root_states, 0, sizeof(g_scanner_root_states));
}

static int scanner_usb_slot_for_path(const char *path) {
  static const char prefix[] = "/mnt/usb";
  const size_t prefix_len = sizeof(prefix) - 1u;

  if (!path || strncmp(path, prefix, prefix_len) != 0)
    return -1;

  char slot = path[prefix_len];
  char suffix = path[prefix_len + 1u];
  if (slot < '0' || slot >= '0' + SCANNER_USB_SLOT_COUNT ||
      (suffix != '\0' && suffix != '/')) {
    return -1;
  }
  return slot - '0';
}

static void build_scanner_usb_root_path(int slot,
                                        char usb_root[sizeof("/mnt/usb0")]) {
  (void)strlcpy(usb_root, "/mnt/usb0", sizeof("/mnt/usb0"));
  usb_root[8] = (char)('0' + slot);
}

static bool scanner_usb_slot_has_scan_root(int slot) {
  for (int i = 0; i < get_scan_path_count(); i++) {
    if (scanner_usb_slot_for_path(get_scan_path(i)) == slot)
      return true;
  }
  return false;
}

static void reset_scanner_usb_scan_counts(int slot) {
  for (int i = 0; i < get_scan_path_count(); i++) {
    if (scanner_usb_slot_for_path(get_scan_path(i)) != slot)
      continue;
    g_scanner_root_states[i].usb_connect_counted = false;
    g_scanner_root_states[i].usb_connect_found_games = 0;
  }
}

static void reset_scanner_usb_mount_state(void) {
  g_scanner_usb_mounted_mask = 0;
  g_scanner_usb_scan_result_pending_mask = 0;
  memset(g_scanner_usb_info, 0, sizeof(g_scanner_usb_info));
  g_scanner_usb_mount_probe_due_us = 0;
  for (int slot = 0; slot < SCANNER_USB_SLOT_COUNT; slot++) {
    char usb_root[sizeof("/mnt/usb0")];
    build_scanner_usb_root_path(slot, usb_root);
    if (usb_storage_root_mounted(usb_root))
      g_scanner_usb_mounted_mask |= (uint8_t)(1u << slot);
  }
}

static uint64_t bytes_to_gib_tenths(uint64_t bytes) {
  uint64_t whole = bytes / SCANNER_GIB_BYTES;
  uint64_t remainder = bytes % SCANNER_GIB_BYTES;
  return whole * 10ull +
         (remainder * 10ull + SCANNER_GIB_BYTES / 2ull) /
             SCANNER_GIB_BYTES;
}

static bool notify_scanner_usb_mount_change(const char *path) {
  int slot = scanner_usb_slot_for_path(path);
  if (slot < 0)
    return false;

  char usb_root[sizeof("/mnt/usb0")];
  build_scanner_usb_root_path(slot, usb_root);
  uint8_t slot_mask = (uint8_t)(1u << slot);
  bool mounted = usb_storage_root_mounted(usb_root);
  bool was_mounted = (g_scanner_usb_mounted_mask & slot_mask) != 0;
  if (mounted == was_mounted)
    return false;

  if (mounted) {
    struct statfs sfs;
    if (statfs(usb_root, &sfs) != 0) {
      log_debug("[SCAN] USB storage info unavailable for %s: %s", usb_root,
                strerror(errno));
      return false;
    }

    uint64_t block_size = (uint64_t)sfs.f_bsize;
    uint64_t capacity_bytes = (uint64_t)sfs.f_blocks * block_size;
    uint64_t available_bytes = (uint64_t)sfs.f_bavail * block_size;
    uint64_t capacity_tenths = bytes_to_gib_tenths(capacity_bytes);
    uint64_t available_tenths = bytes_to_gib_tenths(available_bytes);
    uint64_t block_size_kb = (block_size + 1023ull) / 1024ull;

    g_scanner_usb_mounted_mask |= slot_mask;
    if (scanner_usb_slot_has_scan_root(slot)) {
      g_scanner_usb_scan_result_pending_mask |= slot_mask;
      reset_scanner_usb_scan_counts(slot);
    }
    g_scanner_usb_info[slot].available_tenths = available_tenths;
    g_scanner_usb_info[slot].capacity_tenths = capacity_tenths;
    g_scanner_usb_info[slot].block_size_kb = block_size_kb;
    log_debug("[SCAN] USB storage connected; scan scheduled: %s "
              "capacity=%lluB available=%lluB block_size=%lluB",
              usb_root, (unsigned long long)capacity_bytes,
              (unsigned long long)available_bytes,
              (unsigned long long)block_size);
    return true;
  }

  g_scanner_usb_mounted_mask &= (uint8_t)~slot_mask;
  g_scanner_usb_scan_result_pending_mask &= (uint8_t)~slot_mask;
  reset_scanner_usb_scan_counts(slot);
  memset(&g_scanner_usb_info[slot], 0, sizeof(g_scanner_usb_info[slot]));
  log_debug("[SCAN] USB storage disconnected: %s", usb_root);
  notify_system_info_l10n(SM_L10N_USB_DISCONNECTED, usb_root);
  return false;
}

static void schedule_scanner_usb_mount_probe(const char *path,
                                             uint64_t now_us) {
  int slot = scanner_usb_slot_for_path(path);
  if (slot < 0 ||
      (g_scanner_usb_mounted_mask & (uint8_t)(1u << slot)) != 0 ||
      g_scanner_usb_mount_probe_due_us != 0) {
    return;
  }
  g_scanner_usb_mount_probe_due_us = now_us + SCANNER_USB_MOUNT_PROBE_DELAY_US;
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
    if (g_scanner_watch_entries[i].owns_fd &&
        g_scanner_watch_entries[i].fd >= 0) {
      close(g_scanner_watch_entries[i].fd);
    }
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
         old_cfg->global_fakelib_game_priority !=
             new_cfg->global_fakelib_game_priority ||
         old_cfg->update_emulators_enabled !=
             new_cfg->update_emulators_enabled ||
         strcmp(old_cfg->emulators_path, new_cfg->emulators_path) != 0 ||
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
  uint32_t values[] = {runtime_config()->scan_depth, (uint32_t)scan_path_count};

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
    if (!g_scanner_watch_entries[i].owns_fd)
      continue;
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

static size_t append_scanner_watch_subscription(
    int fd, bool owns_fd, int scan_root_index, const char *path,
    scanner_watch_kind_t kind, uint8_t depth) {
  size_t index = g_scanner_watch_count++;
  scanner_watch_entry_t *entry = &g_scanner_watch_entries[index];
  memset(entry, 0, sizeof(*entry));
  entry->fd = fd;
  entry->owns_fd = owns_fd;
  entry->fd_shareable = true;
  entry->scan_root_index = scan_root_index;
  entry->depth = depth;
  entry->kind = kind;
  entry->prev_root_watch_index = SCANNER_WATCH_INDEX_NONE;
  entry->next_root_watch_index = SCANNER_WATCH_INDEX_NONE;
  (void)strlcpy(entry->path, path, sizeof(entry->path));
  link_scanner_watch_entry_to_root(index);
  return index;
}

static bool register_scanner_watch_entry(int kq, int scan_root_index,
                                         const char *path,
                                         scanner_watch_kind_t kind,
                                         uint8_t depth) {
  if (!ensure_scanner_watch_capacity(g_scanner_watch_count + 1u))
    return false;

  int fd = open(path, O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    if (errno != ENOENT && errno != ENOTDIR) {
      log_debug("  [SCAN] watcher open failed for %s: %s", path,
                strerror(errno));
    }
    return true;
  }

  struct stat opened_st;
  bool opened_identity_checked = false;
  bool opened_identity_ready = false;
  int shared_fd = -1;
  for (size_t i = 0; i < g_scanner_watch_count; i++) {
    const scanner_watch_entry_t *existing = &g_scanner_watch_entries[i];
    if (!existing->fd_shareable || strcmp(existing->path, path) != 0)
      continue;
    if (existing->fd != shared_fd) {
      struct stat existing_st;
      if (!opened_identity_checked) {
        opened_identity_ready = fstat(fd, &opened_st) == 0;
        opened_identity_checked = true;
      }
      if (!opened_identity_ready || fstat(existing->fd, &existing_st) != 0 ||
          existing_st.st_dev != opened_st.st_dev ||
          existing_st.st_ino != opened_st.st_ino) {
        continue;
      }
    }
    if (existing->scan_root_index == scan_root_index) {
      if (existing->kind == kind && existing->depth == depth) {
        close(fd);
        return true;
      }
      log_debug("  [SCAN] conflicting watcher subscription for %s", path);
      close(fd);
      return false;
    }
    if (shared_fd < 0)
      shared_fd = existing->fd;
  }

  if (shared_fd >= 0) {
    close(fd);
    (void)append_scanner_watch_subscription(
        shared_fd, false, scan_root_index, path, kind, depth);
    return true;
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

  size_t entry_index = append_scanner_watch_subscription(
      fd, true, scan_root_index, path, kind, depth);
  if (!insert_scanner_watch_fd_index_entry((uintptr_t)fd, entry_index)) {
    unlink_scanner_watch_entry_from_root(entry_index);
    memset(&g_scanner_watch_entries[entry_index], 0,
           sizeof(g_scanner_watch_entries[entry_index]));
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

  scanner_watch_entry_t *removed = &g_scanner_watch_entries[index];
  int removed_fd = removed->fd;
  bool close_fd = removed->owns_fd && removed_fd >= 0;

  unlink_scanner_watch_entry_from_root(index);
  if (close_fd) {
    for (size_t i = 0; i < g_scanner_watch_count; i++) {
      if (i == index || g_scanner_watch_entries[i].fd != removed_fd)
        continue;
      g_scanner_watch_entries[i].owns_fd = true;
      close_fd = false;
      break;
    }
  }
  if (close_fd)
    close(removed_fd);

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

static bool resolve_watch_tree_rebuild_target(
    const scanner_event_subscription_t *subscription, const char *watched_path,
    char rebuild_path[MAX_PATH],
    uint8_t *rebuild_depth_out, scanner_watch_kind_t *kind_out) {
  switch (subscription->kind) {
  case SCANNER_WATCH_SCAN_ROOT:
  case SCANNER_WATCH_SCAN_SUBDIR:
    (void)strlcpy(rebuild_path, watched_path, MAX_PATH);
    *rebuild_depth_out = subscription->depth;
    *kind_out = subscription->kind;
    return true;
  case SCANNER_WATCH_SCAN_ROOT_PARENT:
    (void)strlcpy(rebuild_path,
                  get_scan_path(subscription->scan_root_index), MAX_PATH);
    *rebuild_depth_out = 0;
    *kind_out = SCANNER_WATCH_SCAN_ROOT;
    return true;
  default:
    return false;
  }
}

typedef struct {
  int kq;
  int scan_root_index;
  unsigned int scan_depth;
} register_watch_tree_ctx_t;

static sm_scan_tree_dir_visit_t register_watch_directory_visit(
    const char *dir_path, unsigned int depth_from_root, void *ctx_ptr) {
  register_watch_tree_ctx_t *ctx = (register_watch_tree_ctx_t *)ctx_ptr;
  if (depth_from_root > 0u) {
    // Runtime image descendants can be active PFS/PFSC mounts. Keeping a
    // directory fd open there pins the mounted vnode and makes unmount return
    // EBUSY. The managed mount-base watcher is sufficient: mounted images are
    // read-only and every discovery pass rebuilds this tree.
    if (is_under_image_mount_base(dir_path))
      return SM_SCAN_TREE_DIR_SKIP_DESCEND;
    if (depth_from_root >= ctx->scan_depth ||
        directory_has_param_json(dir_path, NULL)) {
      return SM_SCAN_TREE_DIR_SKIP_DESCEND;
    }
  }

  scanner_watch_kind_t kind =
      (depth_from_root == 0u) ? SCANNER_WATCH_SCAN_ROOT
                              : SCANNER_WATCH_SCAN_SUBDIR;
  if (!register_scanner_watch_entry(ctx->kq, ctx->scan_root_index, dir_path, kind,
                                    (uint8_t)depth_from_root)) {
    return SM_SCAN_TREE_DIR_ABORT;
  }

  return SM_SCAN_TREE_DIR_DESCEND;
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

  unsigned int scan_depth = get_scan_depth_for_root(scan_root);

  register_watch_tree_ctx_t walk_ctx = {
      .kq = kq,
      .scan_root_index = scan_root_index,
      .scan_depth = scan_depth,
  };
  sm_scan_tree_callbacks_t callbacks = {
      .on_directory = register_watch_directory_visit,
      .on_image_file = NULL,
  };
  if (!sm_scan_tree_walk(scan_root, scan_root, 0u, scan_depth, &callbacks,
                         &walk_ctx)) {
    return false;
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

  unsigned int scan_depth = get_scan_depth_for_root(scan_root);

  if (!remove_scan_root_watch_entries_for_path(scan_root_index, rebuild_path))
    return false;
  if (rebuild_depth > scan_depth) {
    clear_scan_root_watch_tree_state(scan_root_index);
    return true;
  }

  register_watch_tree_ctx_t walk_ctx = {
      .kq = kq,
      .scan_root_index = scan_root_index,
      .scan_depth = scan_depth,
  };
  sm_scan_tree_callbacks_t callbacks = {
      .on_directory = register_watch_directory_visit,
      .on_image_file = NULL,
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

static bool suspend_usb_scan_root_watch_trees(void) {
  bool removed_any = false;
  for (int i = 0; i < get_scan_path_count(); i++) {
    if (!is_usb_storage_path(get_scan_path(i)))
      continue;
    while (g_scanner_root_watch_heads[i] != SCANNER_WATCH_INDEX_NONE) {
      remove_scanner_watch_entry_at(g_scanner_root_watch_heads[i]);
      removed_any = true;
    }
  }

  return !removed_any || rebuild_scanner_watch_fd_index();
}

static void schedule_scan_root_dirty(int scan_root_index, uint64_t now_us,
                                     bool immediate);

static void schedule_scan_roots_for_usb_slot_except(int slot,
                                                    int excluded_root_index,
                                                    uint64_t now_us) {
  for (int i = 0; i < get_scan_path_count(); i++) {
    if (i != excluded_root_index &&
        scanner_usb_slot_for_path(get_scan_path(i)) == slot) {
      schedule_scan_root_dirty(i, now_us, true);
    }
  }
}

static void schedule_scan_roots_for_usb_slot(int slot, uint64_t now_us) {
  schedule_scan_roots_for_usb_slot_except(slot, -1, now_us);
}

static bool scanner_usb_slot_scan_incomplete(int slot) {
  for (int i = 0; i < get_scan_path_count(); i++) {
    if (scanner_usb_slot_for_path(get_scan_path(i)) != slot)
      continue;
    if (g_scanner_root_states[i].dirty ||
        !g_scanner_root_states[i].usb_connect_counted) {
      return true;
    }
  }
  return false;
}

static void notify_scanner_usb_scan_complete(int slot) {
  uint8_t slot_mask = (uint8_t)(1u << slot);
  if ((g_scanner_usb_scan_result_pending_mask & slot_mask) == 0 ||
      (g_scanner_usb_mounted_mask & slot_mask) == 0 ||
      scanner_usb_slot_scan_incomplete(slot)) {
    return;
  }

  char usb_root[sizeof("/mnt/usb0")];
  build_scanner_usb_root_path(slot, usb_root);
  int game_count = 0;
  for (int i = 0; i < get_scan_path_count(); i++) {
    if (scanner_usb_slot_for_path(get_scan_path(i)) == slot)
      game_count += g_scanner_root_states[i].usb_connect_found_games;
  }

  g_scanner_usb_scan_result_pending_mask &= (uint8_t)~slot_mask;
  log_debug("[SCAN] USB storage scan complete: %s games=%d", usb_root,
            game_count);
  const scanner_usb_info_t *info = &g_scanner_usb_info[slot];
  notify_system_info_l10n(
      SM_L10N_USB_CONNECTED_SCANNING, usb_root,
      (unsigned long long)(info->available_tenths / 10ull),
      (unsigned long long)(info->available_tenths % 10ull),
      (unsigned long long)(info->capacity_tenths / 10ull),
      (unsigned long long)(info->capacity_tenths % 10ull),
      (unsigned long long)info->block_size_kb, game_count);
}

static void schedule_pending_scanner_usb_scans(uint64_t now_us) {
  for (int slot = 0; slot < SCANNER_USB_SLOT_COUNT; slot++) {
    if ((g_scanner_usb_scan_result_pending_mask & (uint8_t)(1u << slot)) != 0)
      schedule_scan_roots_for_usb_slot(slot, now_us);
  }
}

static void process_due_scanner_usb_mount_probes(uint64_t now_us) {
  if (g_scanner_usb_mount_probe_due_us == 0 ||
      g_scanner_usb_mount_probe_due_us > now_us) {
    return;
  }

  g_scanner_usb_mount_probe_due_us = 0;
  for (int slot = 0; slot < SCANNER_USB_SLOT_COUNT; slot++) {
    char usb_root[sizeof("/mnt/usb0")];
    build_scanner_usb_root_path(slot, usb_root);
    if (notify_scanner_usb_mount_change(usb_root))
      schedule_scan_roots_for_usb_slot(slot, now_us);
  }
}

static bool resume_usb_scan_root_watch_trees(int kq) {
  uint64_t now_us = monotonic_time_us();
  for (int i = 0; i < get_scan_path_count(); i++) {
    const char *scan_root = get_scan_path(i);
    if (!is_usb_storage_path(scan_root))
      continue;
    // Rebuild every configured USB root because the same disk can resume
    // under a different usbN assignment.
    if (!rebuild_scan_root_watch_tree(kq, i))
      return false;
    if (usb_storage_root_mounted(scan_root))
      schedule_scan_root_dirty(i, now_us, true);
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
  int usb_slot = scanner_usb_slot_for_path(get_scan_path(scan_root_index));
  if (usb_slot >= 0 &&
      (g_scanner_usb_scan_result_pending_mask &
       (uint8_t)(1u << usb_slot)) != 0) {
    state->usb_connect_counted = false;
  }
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

static bool scanner_event_requires_consistency_cleanup(uint32_t fflags) {
  return (fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) != 0;
}

static bool scanner_event_requires_watch_tree_refresh(
    const scanner_event_subscription_t *subscription, uint32_t fflags) {
  uint32_t tree_change_flags =
      NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE;

  switch (subscription->kind) {
  case SCANNER_WATCH_SCAN_ROOT:
    return (fflags & tree_change_flags) != 0;
  case SCANNER_WATCH_SCAN_SUBDIR:
    return subscription->depth <
               get_scan_depth_for_root(
                   get_scan_path(subscription->scan_root_index)) &&
           (fflags & tree_change_flags) != 0;
  default:
    return false;
  }
}

static void schedule_scan_root_watch_tree_rebuild(
    const scanner_event_subscription_t *subscription,
    const char *watched_path) {
  char rebuild_path[MAX_PATH];
  uint8_t rebuild_depth = 0;
  scanner_watch_kind_t rebuild_kind = SCANNER_WATCH_SCAN_ROOT;
  if (!resolve_watch_tree_rebuild_target(subscription, watched_path,
                                         rebuild_path, &rebuild_depth,
                                         &rebuild_kind)) {
    return;
  }

  scanner_root_state_t *state =
      &g_scanner_root_states[subscription->scan_root_index];
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
    if (rebuild_kind == SCANNER_WATCH_SCAN_ROOT) {
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
                get_scan_path(subscription->scan_root_index),
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
  if (strcmp(old_cfg->api_bind_address, new_cfg->api_bind_address) != 0 ||
      old_cfg->api_port != new_cfg->api_port) {
    if (!sm_api_service_reconfigure())
      log_debug("  [CFG] HTTP API reconfigure failed: %s", strerror(errno));
  }

  sm_ampr_updater_on_config_reload(old_cfg, new_cfg);

  if (old_cfg->backport_fakelib_enabled &&
      fakelib_runtime_config_changed(old_cfg, new_cfg))
    sm_fakelib_game_shutdown();

  sm_kstuff_on_config_reload();

  if (old_cfg->auto_remove_missing_games !=
      new_cfg->auto_remove_missing_games) {
    reset_missing_game_cache_timers();
  }

  if (old_cfg->persistent_image_mounts !=
      new_cfg->persistent_image_mounts) {
    request_scan_now(new_cfg->persistent_image_mounts
                         ? "persistent image mounting enabled"
                         : "persistent image mounting disabled");
  }

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

  if (!startup_sync) {
    for (int slot = 0; slot < SCANNER_USB_SLOT_COUNT; slot++) {
      char usb_root[sizeof("/mnt/usb0")];
      build_scanner_usb_root_path(slot, usb_root);
      (void)notify_scanner_usb_mount_change(usb_root);
    }
  }

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
      notify_system_info_l10n(SM_L10N_FOUND_NEW_GAMES, new_games);
  }

  process_scan_candidates(candidates, candidate_count);
  if (should_abort_scan_cycle())
    return false;

  reconcile_missing_app_db_games();

  if (!sm_install_has_pending_work() && !release_scan_runtime_mounts())
    log_debug("  [IMG] some discovery mounts remain busy");

  if (unstable_found_out)
    *unstable_found_out = unstable_found;

  if (startup_sync && !should_abort_scan_cycle()) {
    notify_system_rich_l10n(true, SM_L10N_LIBRARY_SYNCED, total_found_games);
  }

  return !should_abort_scan_cycle();
}

static bool run_targeted_scan_cycle(int scan_root_index,
                                    bool *unstable_found_out) {
  const char *scan_root = get_scan_path(scan_root_index);
  scan_candidate_t *candidates = g_scanner_scan_candidates;

  if (should_abort_scan_cycle())
    return false;

  int usb_slot = scanner_usb_slot_for_path(scan_root);
  if (notify_scanner_usb_mount_change(scan_root)) {
    schedule_scan_roots_for_usb_slot_except(usb_slot, scan_root_index,
                                            monotonic_time_us());
  }
  log_debug("[SCAN] running targeted scan for %s", scan_root);

  bool unstable_found = false;
  cleanup_lost_sources_for_scan_root(scan_root);
  if (should_abort_scan_cycle())
    return false;

  int total_found_games = 0;
  int candidate_count = collect_scan_candidates_for_scan_root(
      scan_root, candidates, MAX_PENDING, &total_found_games, &unstable_found);
  if (should_abort_scan_cycle())
    return false;

  if (usb_slot >= 0 && !unstable_found &&
      (g_scanner_usb_scan_result_pending_mask &
       (uint8_t)(1u << usb_slot)) != 0) {
    scanner_root_state_t *state = &g_scanner_root_states[scan_root_index];
    state->usb_connect_found_games = total_found_games;
    state->usb_connect_counted = true;
  }

  process_scan_candidates(candidates, candidate_count);
  if (should_abort_scan_cycle())
    return false;

  if (!sm_install_has_pending_work() && !release_scan_runtime_mounts())
    log_debug("  [IMG] some discovery mounts remain busy");

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
                                              uint64_t full_resync_due_us,
                                              uint64_t cache_cleanup_due_us,
                                              bool include_scan_work) {
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

  if (include_scan_work && g_scanner_manual_scan_due_us != 0 &&
      (next_deadline == 0 || g_scanner_manual_scan_due_us < next_deadline)) {
    next_deadline = g_scanner_manual_scan_due_us;
  }

  if (g_scanner_manual_fd < 0 && g_scanner_manual_probe_due_us != 0 &&
      (next_deadline == 0 || g_scanner_manual_probe_due_us < next_deadline)) {
    next_deadline = g_scanner_manual_probe_due_us;
  }

  if (g_scanner_usb_mount_probe_due_us != 0 &&
      (next_deadline == 0 ||
       g_scanner_usb_mount_probe_due_us < next_deadline)) {
    next_deadline = g_scanner_usb_mount_probe_due_us;
  }

  if (include_scan_work) {
    if (cache_cleanup_due_us != 0 &&
        (next_deadline == 0 || cache_cleanup_due_us < next_deadline)) {
      next_deadline = cache_cleanup_due_us;
    }
    for (int i = 0; i < get_scan_path_count(); i++) {
      const scanner_root_state_t *state = &g_scanner_root_states[i];
      if (!state->dirty)
        continue;
      if (next_deadline == 0 || state->ready_after_us < next_deadline)
        next_deadline = state->ready_after_us;
    }
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
    int kq, const scanner_event_subscription_t *subscription,
    const char *watched_path, uint64_t now_us) {
  int scan_root_index = subscription->scan_root_index;
  const char *scan_root = get_scan_path(scan_root_index);
  bool root_present = false;
  bool root_changed = update_scan_root_presence_state(scan_root_index, scan_root,
                                                       &root_present);
  if (root_present && root_changed) {
    bool resumed_usb_root =
        runtime_resume_grace_active() && is_usb_storage_path(scan_root);
    if (resumed_usb_root && !usb_storage_root_mounted(scan_root)) {
      // The permanent /mnt/usbN directory is visible again, but the USB
      // filesystem is not. Refresh the terminal vnode watch without scanning
      // or cleaning the temporarily unavailable source.
      return rebuild_scan_root_watch_tree(kq, scan_root_index);
    }
    schedule_scan_root_dirty(scan_root_index, now_us, resumed_usb_root);
    schedule_scan_root_watch_tree_rebuild(subscription, watched_path);
    return true;
  }
  if (root_present)
    return true;
  if (root_changed) {
    if (runtime_resume_grace_active() && is_usb_storage_path(scan_root))
      return rebuild_scan_root_watch_tree(kq, scan_root_index);
    schedule_scan_root_cleanup(scan_root_index);
    schedule_scan_root_dirty(scan_root_index, now_us, true);
    schedule_scan_root_watch_tree_rebuild(subscription, watched_path);
    return true;
  }

  char parent_path[MAX_PATH];
  if (!resolve_existing_parent_directory_path(scan_root, parent_path) ||
      strcmp(parent_path, watched_path) != 0) {
    return rebuild_scan_root_watch_tree(kq, scan_root_index);
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

    scanner_watch_entry_t *watch_owner =
        find_scanner_watch_entry_by_fd(event->ident);
    if (!watch_owner)
      continue;

    scanner_event_subscription_t subscriptions[MAX_SCAN_PATHS];
    size_t subscription_count = 0;
    char watched_path[MAX_PATH];
    (void)strlcpy(watched_path, watch_owner->path, sizeof(watched_path));
    bool immediate =
        scanner_event_requires_consistency_cleanup(event->fflags);

    for (size_t watch_index = 0; watch_index < g_scanner_watch_count;
         watch_index++) {
      scanner_watch_entry_t *entry = &g_scanner_watch_entries[watch_index];
      if ((uintptr_t)entry->fd != event->ident)
        continue;
      if (immediate)
        entry->fd_shareable = false;
      if (subscription_count >= MAX_SCAN_PATHS) {
        log_debug("  [SCAN] too many watcher subscriptions for %s",
                  watched_path);
        return false;
      }
      subscriptions[subscription_count++] =
          (scanner_event_subscription_t){
              .scan_root_index = entry->scan_root_index,
              .depth = entry->depth,
              .kind = entry->kind,
          };
    }

    uint8_t checked_usb_slots = 0;
    for (size_t subscription_index = 0;
         subscription_index < subscription_count; subscription_index++) {
      const scanner_event_subscription_t *subscription =
          &subscriptions[subscription_index];
      const char *scan_root = get_scan_path(subscription->scan_root_index);
      int usb_slot = scanner_usb_slot_for_path(scan_root);
      if (usb_slot >= 0) {
        uint8_t slot_mask = (uint8_t)(1u << usb_slot);
        if ((checked_usb_slots & slot_mask) == 0) {
          checked_usb_slots |= slot_mask;
          if (notify_scanner_usb_mount_change(scan_root))
            schedule_scan_roots_for_usb_slot(usb_slot, now_us);
        }
      }
      if (subscription->kind == SCANNER_WATCH_SCAN_ROOT_PARENT)
        schedule_scanner_usb_mount_probe(scan_root, now_us);

      if (subscription->kind == SCANNER_WATCH_SCAN_ROOT_PARENT) {
        if (!handle_scan_root_parent_event(kq, subscription, watched_path,
                                           now_us)) {
          return false;
        }
        continue;
      }

      if (immediate)
        schedule_scan_root_cleanup(subscription->scan_root_index);
      schedule_scan_root_dirty(subscription->scan_root_index, now_us,
                               immediate);

      if (scanner_event_requires_watch_tree_refresh(subscription,
                                                    event->fflags)) {
        schedule_scan_root_watch_tree_rebuild(subscription, watched_path);
      }
    }
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
  reset_scanner_usb_mount_state();
  atomic_store_explicit(&g_usb_watches_suspended, true,
                        memory_order_release);

  if (pipe(g_scanner_wake_pipe) != 0) {
    log_debug("  [SCAN] wake pipe creation failed: %s", strerror(errno));
    close_scanner_wake_pipe();
    return false;
  }
  if (!sm_set_fd_nonblocking(g_scanner_wake_pipe[0]) ||
      !sm_set_fd_nonblocking(g_scanner_wake_pipe[1])) {
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

bool sm_scanner_usb_watches_suspended(void) {
  return atomic_load_explicit(&g_usb_watches_suspended, memory_order_acquire);
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
  atomic_store_explicit(&g_usb_watches_suspended, false,
                        memory_order_release);
  if (!rebuild_all_scan_root_watch_trees(kq)) {
    close(kq);
    clear_scanner_watch_entries();
    atomic_store_explicit(&g_usb_watches_suspended, true,
                          memory_order_release);
    close_scanner_config_file();
    close_scanner_manual_file();
    request_scanner_shutdown("scanner watcher initialization failed");
    return;
  }
  uint64_t next_full_resync_us =
      monotonic_time_us() + scanner_full_resync_interval_us();
  uint64_t next_fakelib_cache_cleanup_us = monotonic_time_us();
  if (next_fakelib_cache_cleanup_us == 0)
    next_fakelib_cache_cleanup_us = 1;
  bool was_sleeping = false;
  bool scan_work_blocked = false;

  while (true) {
    if (should_stop_requested()) {
      log_debug("[SHUTDOWN] stop requested");
      break;
    }

    if (runtime_sleep_mode_active()) {
      if (!was_sleeping) {
        if (!suspend_usb_scan_root_watch_trees()) {
          close(kq);
          clear_scanner_watch_entries();
          request_scanner_shutdown("USB watcher suspend failed");
          return;
        }
        clear_all_dirty_scan_roots();
        g_scanner_usb_mount_probe_due_us = 0;
        atomic_store_explicit(&g_usb_watches_suspended, true,
                              memory_order_release);
        wake_game_lifecycle_watcher();
        was_sleeping = true;
        log_debug("[SLEEP] USB scanner watches suspended");
      }
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
      if (!discard_scanner_events_nowait(kq)) {
        close(kq);
        clear_scanner_watch_entries();
        request_scanner_shutdown("scanner stale event drain failed");
        return;
      }
      if (!resume_usb_scan_root_watch_trees(kq)) {
        close(kq);
        clear_scanner_watch_entries();
        request_scanner_shutdown("USB watcher resume failed");
        return;
      }
      atomic_store_explicit(&g_usb_watches_suspended, false,
                            memory_order_release);
      was_sleeping = false;
      next_full_resync_us =
          monotonic_time_us() + RUNTIME_RESUME_GRACE_US;
      log_debug("[SLEEP] USB scanner watches resumed");
    }

    bool game_mount_busy = sm_game_lifecycle_has_active_game() ||
                           sm_shellcore_service_has_prepared_mount();
    char scan_reason[128];
    if (!game_mount_busy &&
        consume_scan_now_request(scan_reason, sizeof(scan_reason))) {
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
      schedule_pending_scanner_usb_scans(monotonic_time_us());

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
    process_due_scanner_usb_mount_probes(now_us);
    game_mount_busy = sm_game_lifecycle_has_active_game() ||
                      sm_shellcore_service_has_prepared_mount();
    if (game_mount_busy) {
      next_full_resync_us = 0;
      scan_work_blocked = true;
    } else if (scan_work_blocked) {
      scan_work_blocked = false;
      next_full_resync_us = now_us + scanner_full_resync_interval_us();
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
        sm_l10n_init();
        notify_system_l10n(SM_L10N_CONFIG_RELOADED);
        log_debug("  [CFG] runtime config reloaded");
        now_us = monotonic_time_us();
        next_full_resync_us =
            scan_topology_changed ? now_us
                                  : now_us + scanner_full_resync_interval_us();
      }
      continue;
    }

    if (!game_mount_busy && g_scanner_manual_scan_due_us != 0 &&
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
      schedule_pending_scanner_usb_scans(monotonic_time_us());

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

    if (!game_mount_busy && now_us >= next_fakelib_cache_cleanup_us) {
      sm_fakelib_cleanup_caches();
      next_fakelib_cache_cleanup_us =
          monotonic_time_us() + SCANNER_FAKELIB_CACHE_CLEANUP_INTERVAL_US;
      continue;
    }

    if (!game_mount_busy && next_full_resync_us != 0 &&
        now_us >= next_full_resync_us) {
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
      schedule_pending_scanner_usb_scans(monotonic_time_us());

      now_us = monotonic_time_us();
      next_full_resync_us = now_us + scanner_full_resync_interval_us();
      if (unstable_found) {
        uint64_t retry_due = now_us + scanner_stability_wait_us();
        if (retry_due < next_full_resync_us)
          next_full_resync_us = retry_due;
      }
      continue;
    }

    int cleanup_root_index =
        game_mount_busy ? -1 : find_pending_cleanup_scan_root();
    if (cleanup_root_index >= 0) {
      g_scanner_root_states[cleanup_root_index].cleanup_pending = false;
      cleanup_lost_sources_for_scan_root(get_scan_path(cleanup_root_index));
      continue;
    }

    int dirty_root_index =
        game_mount_busy ? -1 : find_due_dirty_scan_root(now_us);
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
      int usb_slot = scanner_usb_slot_for_path(get_scan_path(dirty_root_index));
      if (usb_slot >= 0)
        notify_scanner_usb_scan_complete(usb_slot);
      continue;
    }

    uint64_t deadline_us = compute_next_scan_deadline_us(
        now_us, next_full_resync_us, next_fakelib_cache_cleanup_us,
        !game_mount_busy);
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
  g_scanner_usb_mounted_mask = 0;
  g_scanner_usb_scan_result_pending_mask = 0;
  memset(g_scanner_usb_info, 0, sizeof(g_scanner_usb_info));
  g_scanner_usb_mount_probe_due_us = 0;
  atomic_store_explicit(&g_usb_watches_suspended, true,
                        memory_order_release);
}
