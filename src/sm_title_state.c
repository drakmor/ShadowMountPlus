#include "sm_platform.h"
#include "sm_title_state.h"
#include "sm_hash.h"
#include "sm_limits.h"
#include "sm_log.h"

struct TitleStateEntry {
  char title_id[MAX_TITLE_ID];
  uint8_t mount_reg_attempts;
  uint8_t register_attempts;
  bool duplicate_notified_once;
  bool valid;
};

static struct TitleStateEntry g_title_state[TITLE_STATE_CAPACITY];
static uint16_t g_title_state_hash[STATE_HASH_SIZE];

static void rebuild_title_state_hash(void) {
  memset(g_title_state_hash, 0, sizeof(g_title_state_hash));
  for (int k = 0; k < TITLE_STATE_CAPACITY; k++) {
    if (!g_title_state[k].valid || g_title_state[k].title_id[0] == '\0')
      continue;
    uint32_t slot =
        sm_fnv1a32(g_title_state[k].title_id) & (STATE_HASH_SIZE - 1u);
    for (uint32_t i = 0; i < STATE_HASH_SIZE; i++) {
      if (g_title_state_hash[slot] == 0) {
        g_title_state_hash[slot] = (uint16_t)(k + 1);
        break;
      }
      slot = (slot + 1u) & (STATE_HASH_SIZE - 1u);
    }
  }
}

static struct TitleStateEntry *find_title_state(const char *title_id) {
  uint32_t slot = sm_fnv1a32(title_id) & (STATE_HASH_SIZE - 1u);
  for (uint32_t i = 0; i < STATE_HASH_SIZE; i++) {
    uint16_t idx = g_title_state_hash[slot];
    if (idx == 0)
      return NULL;
    struct TitleStateEntry *entry = &g_title_state[idx - 1u];
    if (entry->valid && strcmp(entry->title_id, title_id) == 0)
      return entry;
    slot = (slot + 1u) & (STATE_HASH_SIZE - 1u);
  }
  return NULL;
}

static struct TitleStateEntry *create_title_state(const char *title_id) {
  int slot_k = -1;
  for (int k = 0; k < TITLE_STATE_CAPACITY; k++) {
    if (!g_title_state[k].valid) {
      slot_k = k;
      break;
    }
  }
  if (slot_k < 0) {
    int evict_k = -1;
    for (int k = 0; k < TITLE_STATE_CAPACITY; k++) {
      if (!g_title_state[k].valid)
        continue;
      if (g_title_state[k].mount_reg_attempts == 0 &&
          g_title_state[k].register_attempts == 0) {
        evict_k = k;
        break;
      }
    }
    if (evict_k < 0) {
      for (int k = 0; k < TITLE_STATE_CAPACITY; k++) {
        if (!g_title_state[k].valid)
          continue;
        if (g_title_state[k].mount_reg_attempts == 0) {
          evict_k = k;
          break;
        }
      }
    }
    if (evict_k < 0)
      evict_k = 0;
    memset(&g_title_state[evict_k], 0, sizeof(g_title_state[evict_k]));
    rebuild_title_state_hash();
    slot_k = evict_k;
  }

  memset(&g_title_state[slot_k], 0, sizeof(g_title_state[slot_k]));
  g_title_state[slot_k].valid = true;
  (void)strlcpy(g_title_state[slot_k].title_id, title_id,
                sizeof(g_title_state[slot_k].title_id));

  uint32_t slot = sm_fnv1a32(title_id) & (STATE_HASH_SIZE - 1u);
  for (uint32_t i = 0; i < STATE_HASH_SIZE; i++) {
    if (g_title_state_hash[slot] == 0) {
      g_title_state_hash[slot] = (uint16_t)(slot_k + 1);
      return &g_title_state[slot_k];
    }
    slot = (slot + 1u) & (STATE_HASH_SIZE - 1u);
  }

  g_title_state[slot_k].valid = false;
  g_title_state[slot_k].title_id[0] = '\0';
  rebuild_title_state_hash();
  return NULL;
}

static struct TitleStateEntry *get_or_create_title_state(const char *title_id) {
  struct TitleStateEntry *entry = find_title_state(title_id);
  return entry ? entry : create_title_state(title_id);
}

bool was_register_attempted(const char *title_id) {
  struct TitleStateEntry *entry = find_title_state(title_id);
  return entry ? (entry->register_attempts >= MAX_REGISTER_ATTEMPTS) : false;
}

uint8_t get_register_attempts(const char *title_id) {
  struct TitleStateEntry *entry = find_title_state(title_id);
  return entry ? entry->register_attempts : 0;
}

void mark_register_attempted(const char *title_id) {
  struct TitleStateEntry *entry = get_or_create_title_state(title_id);
  if (entry->register_attempts < UINT8_MAX)
    entry->register_attempts++;
}

void notify_duplicate_title_once(const char *title_id, const char *path_a,
                                 const char *path_b) {
  struct TitleStateEntry *entry = get_or_create_title_state(title_id);
  if (entry->duplicate_notified_once)
    return;
  entry->duplicate_notified_once = true;
  notify_system("Duplicate %s ignored:\n%s\nexisting: %s", title_id, path_a,
                path_b);
}

void clear_duplicate_title_notification(const char *title_id) {
  struct TitleStateEntry *entry = find_title_state(title_id);
  if (entry)
    entry->duplicate_notified_once = false;
}

uint8_t get_failed_mount_attempts(const char *title_id) {
  struct TitleStateEntry *entry = find_title_state(title_id);
  return entry ? entry->mount_reg_attempts : 0;
}

void clear_failed_mount_attempts(const char *title_id) {
  struct TitleStateEntry *entry = find_title_state(title_id);
  if (entry)
    entry->mount_reg_attempts = 0;
}

uint8_t bump_failed_mount_attempts(const char *title_id) {
  struct TitleStateEntry *entry = get_or_create_title_state(title_id);
  if (entry->mount_reg_attempts < UINT8_MAX)
    entry->mount_reg_attempts++;
  return entry->mount_reg_attempts;
}
