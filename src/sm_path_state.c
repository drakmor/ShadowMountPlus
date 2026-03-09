#include "sm_platform.h"
#include "sm_path_state.h"
#include "sm_hash.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_path_utils.h"

struct PathStateEntry {
  char path[MAX_PATH];
  uint8_t missing_param_attempts;
  uint8_t image_mount_attempts;
  bool missing_param_limit_logged;
  bool image_mount_limit_logged;
  bool game_info_cached;
  bool game_info_valid;
  time_t game_info_mtime;
  off_t game_info_size;
  ino_t game_info_ino;
  char game_title_id[MAX_TITLE_ID];
  char game_title_name[MAX_TITLE_NAME];
  bool valid;
};

static struct PathStateEntry g_path_state[PATH_STATE_CAPACITY];
static uint16_t g_path_state_hash[STATE_HASH_SIZE];

static void rebuild_path_state_hash(void) {
  memset(g_path_state_hash, 0, sizeof(g_path_state_hash));
  for (int k = 0; k < PATH_STATE_CAPACITY; k++) {
    if (!g_path_state[k].valid || g_path_state[k].path[0] == '\0')
      continue;
    uint32_t slot = sm_fnv1a32(g_path_state[k].path) & (STATE_HASH_SIZE - 1u);
    for (uint32_t i = 0; i < STATE_HASH_SIZE; i++) {
      if (g_path_state_hash[slot] == 0) {
        g_path_state_hash[slot] = (uint16_t)(k + 1);
        break;
      }
      slot = (slot + 1u) & (STATE_HASH_SIZE - 1u);
    }
  }
}

static struct PathStateEntry *find_path_state(const char *path) {
  if (!path || path[0] == '\0')
    return NULL;
  uint32_t slot = sm_fnv1a32(path) & (STATE_HASH_SIZE - 1u);
  for (uint32_t i = 0; i < STATE_HASH_SIZE; i++) {
    uint16_t idx = g_path_state_hash[slot];
    if (idx == 0)
      return NULL;
    struct PathStateEntry *entry = &g_path_state[idx - 1u];
    if (entry->valid && strcmp(entry->path, path) == 0)
      return entry;
    slot = (slot + 1u) & (STATE_HASH_SIZE - 1u);
  }
  return NULL;
}

static struct PathStateEntry *create_path_state(const char *path) {
  if (!path || path[0] == '\0')
    return NULL;

  int slot_k = -1;
  for (int k = 0; k < PATH_STATE_CAPACITY; k++) {
    if (!g_path_state[k].valid) {
      slot_k = k;
      break;
    }
  }
  if (slot_k < 0) {
    int evict_k = -1;
    for (int k = 0; k < PATH_STATE_CAPACITY; k++) {
      if (!g_path_state[k].valid)
        continue;
      if (access(g_path_state[k].path, F_OK) != 0) {
        evict_k = k;
        break;
      }
    }
    if (evict_k < 0) {
      for (int k = 0; k < PATH_STATE_CAPACITY; k++) {
        if (!g_path_state[k].valid)
          continue;
        if (g_path_state[k].missing_param_attempts == 0 &&
            g_path_state[k].image_mount_attempts == 0 &&
            !g_path_state[k].game_info_cached) {
          evict_k = k;
          break;
        }
      }
    }
    if (evict_k < 0)
      evict_k = 0;
    memset(&g_path_state[evict_k], 0, sizeof(g_path_state[evict_k]));
    rebuild_path_state_hash();
    slot_k = evict_k;
  }

  memset(&g_path_state[slot_k], 0, sizeof(g_path_state[slot_k]));
  g_path_state[slot_k].valid = true;
  (void)strlcpy(g_path_state[slot_k].path, path,
                sizeof(g_path_state[slot_k].path));

  uint32_t slot = sm_fnv1a32(path) & (STATE_HASH_SIZE - 1u);
  for (uint32_t i = 0; i < STATE_HASH_SIZE; i++) {
    if (g_path_state_hash[slot] == 0) {
      g_path_state_hash[slot] = (uint16_t)(slot_k + 1);
      return &g_path_state[slot_k];
    }
    slot = (slot + 1u) & (STATE_HASH_SIZE - 1u);
  }

  g_path_state[slot_k].valid = false;
  g_path_state[slot_k].path[0] = '\0';
  rebuild_path_state_hash();
  return NULL;
}

static struct PathStateEntry *get_or_create_path_state(const char *path) {
  struct PathStateEntry *entry = find_path_state(path);
  return entry ? entry : create_path_state(path);
}

bool load_cached_game_info(const char *path, const struct stat *param_st,
                           char *out_id, char *out_name, bool *valid_out) {
  if (valid_out)
    *valid_out = false;
  if (!path || !param_st)
    return false;

  struct PathStateEntry *entry = find_path_state(path);
  if (!entry || !entry->game_info_cached ||
      entry->game_info_mtime != param_st->st_mtime ||
      entry->game_info_size != param_st->st_size ||
      entry->game_info_ino != param_st->st_ino) {
    return false;
  }

  if (valid_out)
    *valid_out = entry->game_info_valid;
  if (!entry->game_info_valid)
    return true;

  if (out_id)
    (void)strlcpy(out_id, entry->game_title_id, MAX_TITLE_ID);
  if (out_name)
    (void)strlcpy(out_name, entry->game_title_name, MAX_TITLE_NAME);
  return true;
}

void store_cached_game_info(const char *path, const struct stat *param_st,
                            bool valid, const char *title_id,
                            const char *title_name) {
  if (!path || !param_st)
    return;

  struct PathStateEntry *entry = get_or_create_path_state(path);
  if (!entry)
    return;

  entry->game_info_cached = true;
  entry->game_info_valid = valid;
  entry->game_info_mtime = param_st->st_mtime;
  entry->game_info_size = param_st->st_size;
  entry->game_info_ino = param_st->st_ino;
  (void)strlcpy(entry->game_title_id, valid ? title_id : "", MAX_TITLE_ID);
  (void)strlcpy(entry->game_title_name, valid ? title_name : "", MAX_TITLE_NAME);
}

void prune_path_state(void) {
  bool changed = false;
  for (int k = 0; k < PATH_STATE_CAPACITY; k++) {
    if (!g_path_state[k].valid || g_path_state[k].path[0] == '\0')
      continue;
    if (access(g_path_state[k].path, F_OK) == 0)
      continue;
    memset(&g_path_state[k], 0, sizeof(g_path_state[k]));
    changed = true;
  }
  if (changed)
    rebuild_path_state_hash();
}

bool is_missing_param_scan_limited(const char *path) {
  if (!is_under_image_mount_base(path))
    return false;
  struct PathStateEntry *entry = find_path_state(path);
  if (!entry)
    return false;
  return entry->missing_param_attempts >= MAX_MISSING_PARAM_SCAN_ATTEMPTS;
}

void record_missing_param_failure(const char *path) {
  if (!is_under_image_mount_base(path))
    return;

  struct PathStateEntry *entry = get_or_create_path_state(path);
  if (!entry) {
    log_debug("  [SCAN] missing/invalid param.json: %s", path);
    notify_system("Missing/invalid param.json:\n%s", path);
    return;
  }
  if (entry->missing_param_attempts < UINT8_MAX)
    entry->missing_param_attempts++;

  log_debug("  [SCAN] missing/invalid param.json: %s", path);
  if (entry->missing_param_attempts == 1)
    notify_system("Missing/invalid param.json:\n%s", path);
  if (entry->missing_param_attempts >= MAX_MISSING_PARAM_SCAN_ATTEMPTS &&
      !entry->missing_param_limit_logged) {
    log_debug("  [SCAN] attempt limit reached (%u), skipping path: %s",
              (unsigned)MAX_MISSING_PARAM_SCAN_ATTEMPTS, path);
    entry->missing_param_limit_logged = true;
  }
}

void clear_missing_param_entry(const char *path) {
  struct PathStateEntry *entry = find_path_state(path);
  if (!entry)
    return;
  entry->missing_param_attempts = 0;
  entry->missing_param_limit_logged = false;
}

bool is_image_mount_limited(const char *path) {
  struct PathStateEntry *entry = find_path_state(path);
  return entry ? entry->image_mount_attempts >= MAX_IMAGE_MOUNT_ATTEMPTS
               : false;
}

uint8_t bump_image_mount_attempts(const char *path) {
  struct PathStateEntry *entry = get_or_create_path_state(path);
  if (!entry)
    return MAX_IMAGE_MOUNT_ATTEMPTS;
  if (entry->image_mount_attempts < UINT8_MAX)
    entry->image_mount_attempts++;
  if (entry->image_mount_attempts >= MAX_IMAGE_MOUNT_ATTEMPTS &&
      !entry->image_mount_limit_logged) {
    log_debug("  [IMG] retry limit reached (%u/%u), skipping image: %s",
              (unsigned)entry->image_mount_attempts,
              (unsigned)MAX_IMAGE_MOUNT_ATTEMPTS, path);
    entry->image_mount_limit_logged = true;
  }
  return entry->image_mount_attempts;
}

void clear_image_mount_attempts(const char *path) {
  struct PathStateEntry *entry = find_path_state(path);
  if (!entry)
    return;
  entry->image_mount_attempts = 0;
  entry->image_mount_limit_logged = false;
}
