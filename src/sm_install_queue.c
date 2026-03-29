#include "sm_platform.h"

#include "sm_install_queue.h"
#include "sm_types.h"
#include "sm_game_cache.h"
#include "sm_log.h"
#include "sm_appdb.h"
#include "sm_title_state.h"
#include "sm_time.h"
#include "sm_limits.h"

#define INSTALL_WAIT_TIMEOUT_US (300ull * 1000000ull)
#define INSTALL_POLL_INTERVAL_US (2ull * 1000000ull)
#define INSTALL_SUBMIT_RETRY_US (2ull * 1000000ull)

typedef enum {
  INSTALL_TRACK_NONE = 0,
  INSTALL_TRACK_QUEUED,
  INSTALL_TRACK_SUBMITTED,
} install_track_state_t;

typedef struct {
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  char source_path[MAX_PATH];
  uint64_t requested_at_us;
  bool has_src_snd0;
  install_track_state_t state;
} pending_install_entry_t;

static pending_install_entry_t g_pending_installs[MAX_PENDING];
static uint64_t g_pending_install_poll_due_us = 0;
static uint64_t g_queued_install_submit_due_us = 0;
static int g_tracked_install_count = 0;
static int g_submitted_install_count = 0;

static int count_queued_installs(void);

static pending_install_entry_t *find_pending_install_entry(
    const char *title_id) {
  if (!title_id || title_id[0] == '\0')
    return NULL;

  for (int i = 0; i < MAX_PENDING; i++) {
    if (g_pending_installs[i].state == INSTALL_TRACK_NONE)
      continue;
    if (strcmp(g_pending_installs[i].title_id, title_id) == 0)
      return &g_pending_installs[i];
  }

  return NULL;
}

static pending_install_entry_t *reserve_pending_install_entry(
    const char *title_id) {
  pending_install_entry_t *entry = find_pending_install_entry(title_id);
  if (entry)
    return entry;

  for (int i = 0; i < MAX_PENDING; i++) {
    if (g_pending_installs[i].state == INSTALL_TRACK_NONE)
      return &g_pending_installs[i];
  }

  return NULL;
}

static uint64_t next_pending_install_timeout_us(void) {
  if (g_submitted_install_count <= 0)
    return 0;

  uint64_t next_deadline = 0;
  for (int i = 0; i < MAX_PENDING; i++) {
    const pending_install_entry_t *entry = &g_pending_installs[i];
    if (entry->state != INSTALL_TRACK_SUBMITTED || entry->requested_at_us == 0)
      continue;

    uint64_t deadline_us = entry->requested_at_us + INSTALL_WAIT_TIMEOUT_US;
    if (next_deadline == 0 || deadline_us < next_deadline)
      next_deadline = deadline_us;
  }

  return next_deadline;
}

static void schedule_pending_install_poll(uint64_t now_us) {
  if (g_submitted_install_count <= 0) {
    g_pending_install_poll_due_us = 0;
    return;
  }

  g_pending_install_poll_due_us =
      now_us == 0 ? 1 : now_us + INSTALL_POLL_INTERVAL_US;
}

static void schedule_queued_install_submit(uint64_t now_us) {
  if (count_queued_installs() <= 0 || g_submitted_install_count > 0) {
    g_queued_install_submit_due_us = 0;
    return;
  }

  g_queued_install_submit_due_us = now_us == 0 ? 1 : now_us;
}

static void schedule_queued_install_submit_retry(uint64_t now_us) {
  if (count_queued_installs() <= 0 || g_submitted_install_count > 0) {
    g_queued_install_submit_due_us = 0;
    return;
  }

  g_queued_install_submit_due_us =
      now_us == 0 ? 1 : now_us + INSTALL_SUBMIT_RETRY_US;
}

static void clear_pending_install_entry(pending_install_entry_t *entry) {
  if (!entry)
    return;
  if (entry->state == INSTALL_TRACK_SUBMITTED && g_submitted_install_count > 0)
    g_submitted_install_count--;
  if (entry->state != INSTALL_TRACK_NONE && g_tracked_install_count > 0)
    g_tracked_install_count--;
  memset(entry, 0, sizeof(*entry));
}

bool is_title_install_pending(const char *title_id) {
  return find_pending_install_entry(title_id) != NULL;
}

static void append_batch_install_line(char *message, size_t message_size,
                                      const char *title_name,
                                      const char *title_id, int *shown_count) {
  if (!message || message_size == 0 || !title_name || !title_id || !shown_count)
    return;

  size_t used = strlen(message);
  if (used >= message_size)
    return;

  int written = snprintf(message + used, message_size - used, "\n%s (%s)",
                         title_name, title_id);
  if (written < 0 || (size_t)written >= message_size - used)
    return;

  (*shown_count)++;
}

bool sm_install_queue_candidate(const scan_candidate_t *candidate,
                                bool has_src_snd0) {
  pending_install_entry_t *entry =
      reserve_pending_install_entry(candidate->title_id);
  if (!entry) {
    log_debug("  [REG] install queue full, cannot queue %s (%s)",
              candidate->title_name, candidate->title_id);
    return false;
  }

  install_track_state_t previous_state = entry->state;
  if (previous_state == INSTALL_TRACK_SUBMITTED)
    return true;

  memset(entry, 0, sizeof(*entry));
  (void)strlcpy(entry->title_id, candidate->title_id, sizeof(entry->title_id));
  (void)strlcpy(entry->title_name, candidate->title_name,
                sizeof(entry->title_name));
  (void)strlcpy(entry->source_path, candidate->path, sizeof(entry->source_path));
  entry->has_src_snd0 = has_src_snd0;
  entry->state = INSTALL_TRACK_QUEUED;
  if (previous_state == INSTALL_TRACK_NONE)
    g_tracked_install_count++;
  schedule_queued_install_submit(monotonic_time_us());
  return true;
}

static void finalize_pending_install_success(pending_install_entry_t *entry) {
  if (!entry)
    return;

  log_debug("  [REG] Installed: %s (%s)", entry->title_name, entry->title_id);
  notify_game_installed_rich(entry->title_id);
  if (entry->has_src_snd0) {
    int snd0_updates = update_snd0info(entry->title_id);
    if (snd0_updates >= 0)
      log_debug("  [DB] snd0info updated rows=%d", snd0_updates);
  }
  cache_game_entry(entry->source_path, entry->title_id, entry->title_name);
  clear_failed_mount_attempts(entry->title_id);
  clear_pending_install_entry(entry);
}

static void finalize_pending_install_timeout(pending_install_entry_t *entry) {
  if (!entry)
    return;

  log_debug("  [REG] Install timeout after 5 minutes: %s (%s)",
            entry->title_name, entry->title_id);
  notify_system("Install failed: %s (%s)\nTimed out after 5 minutes",
                entry->title_name, entry->title_id);
  clear_pending_install_entry(entry);
}

static void poll_pending_installs(void) {
  if (g_submitted_install_count <= 0)
    return;

  invalidate_app_db_title_cache();

  const struct AppDbTitleList *app_db_titles = NULL;
  bool app_db_titles_ready = get_app_db_title_list_cached(&app_db_titles);
  uint64_t now_us = monotonic_time_us();

  if (!app_db_titles_ready)
    log_debug("  [DB] app.db title list unavailable while polling installs");

  for (int i = 0; i < MAX_PENDING; i++) {
    pending_install_entry_t *entry = &g_pending_installs[i];
    if (entry->state != INSTALL_TRACK_SUBMITTED)
      continue;

    if (app_db_titles_ready &&
        app_db_title_list_contains(app_db_titles, entry->title_id)) {
      finalize_pending_install_success(entry);
      continue;
    }

    if (now_us != 0 && entry->requested_at_us != 0 &&
        now_us >= entry->requested_at_us &&
        now_us - entry->requested_at_us >= INSTALL_WAIT_TIMEOUT_US) {
      finalize_pending_install_timeout(entry);
    }
  }

  schedule_pending_install_poll(now_us);
  if (g_submitted_install_count == 0)
    schedule_queued_install_submit(now_us);
}

uint64_t sm_install_next_wake_us(uint64_t now_us) {
  if (g_submitted_install_count <= 0)
    return g_queued_install_submit_due_us;

  uint64_t next_wake_us = next_pending_install_timeout_us();

  if (g_pending_install_poll_due_us != 0 &&
      (next_wake_us == 0 || g_pending_install_poll_due_us < next_wake_us)) {
    next_wake_us = g_pending_install_poll_due_us;
  }

  if (g_queued_install_submit_due_us != 0 &&
      (next_wake_us == 0 || g_queued_install_submit_due_us < next_wake_us)) {
    next_wake_us = g_queued_install_submit_due_us;
  }

  if (next_wake_us == 0)
    next_wake_us = now_us == 0 ? 1 : now_us + INSTALL_POLL_INTERVAL_US;

  return next_wake_us;
}

void sm_install_poll_pending(void) {
  poll_pending_installs();
}

void sm_install_service_pending(void) {
  sm_install_poll_pending();
  if (count_queued_installs() > 0)
    (void)sm_install_submit_queued();
}

static void log_batch_submit_retry_limit(const pending_install_entry_t *entry) {
  uint8_t failed_attempts = get_failed_mount_attempts(entry->title_id);
  if (failed_attempts == MAX_FAILED_MOUNT_ATTEMPTS) {
    log_debug("  [RETRY] limit reached (%u/%u): %s (%s)",
              (unsigned)failed_attempts,
              (unsigned)MAX_FAILED_MOUNT_ATTEMPTS, entry->title_name,
              entry->title_id);
  }
}

static int count_queued_installs(void) {
  return g_tracked_install_count - g_submitted_install_count;
}

static void log_queued_install_batch(void) {
  int queued_count = count_queued_installs();
  if (queued_count <= 0)
    return;

  log_debug("  [REG] Batch install request contains %d title(s):", queued_count);
  for (int i = 0; i < MAX_PENDING; i++) {
    const pending_install_entry_t *entry = &g_pending_installs[i];
    if (entry->state != INSTALL_TRACK_QUEUED)
      continue;
    log_debug("    - %s (%s)", entry->title_name, entry->title_id);
  }
}

static void notify_queued_install_batch(void) {
  int queued_count = count_queued_installs();
  if (queued_count <= 0)
    return;

  char message[3075];
  int shown_count = 0;
  snprintf(message, sizeof(message), "Batch install queued (%d):", queued_count);

  for (int i = 0; i < MAX_PENDING; i++) {
    const pending_install_entry_t *entry = &g_pending_installs[i];
    if (entry->state != INSTALL_TRACK_QUEUED)
      continue;
    append_batch_install_line(message, sizeof(message), entry->title_name,
                              entry->title_id, &shown_count);
  }

  if (shown_count < queued_count) {
    size_t used = strlen(message);
    if (used < sizeof(message)) {
      (void)snprintf(message + used, sizeof(message) - used, "\n... and %d more",
                     queued_count - shown_count);
    }
  }

  notify_system_info("%s", message);
}

bool sm_install_submit_queued(void) {
  int queued_count = count_queued_installs();
  if (queued_count <= 0)
    return true;

  if (g_submitted_install_count > 0)
    return true;

  uint64_t now_us = monotonic_time_us();
  if (g_queued_install_submit_due_us != 0 && now_us != 0 &&
      now_us < g_queued_install_submit_due_us) {
    return true;
  }

  log_queued_install_batch();
  notify_queued_install_batch();

  int res = sceAppInstUtilAppInstallAll();
  if (res != 0) {
    log_debug("  [REG] Batch install request failed: 0x%x", res);
    notify_system("Batch install failed for %d app(s).\ncode=0x%08X",
                  queued_count, (uint32_t)res);
    schedule_queued_install_submit_retry(now_us);
    return false;
  }

  g_queued_install_submit_due_us = 0;
  for (int i = 0; i < MAX_PENDING; i++) {
    pending_install_entry_t *entry = &g_pending_installs[i];
    if (entry->state != INSTALL_TRACK_QUEUED)
      continue;
    entry->state = INSTALL_TRACK_SUBMITTED;
    entry->requested_at_us = now_us;
    g_submitted_install_count++;
    clear_failed_mount_attempts(entry->title_id);
  }

  invalidate_app_db_title_cache();
  schedule_pending_install_poll(now_us);
  return true;
}

void sm_install_note_submit_failure(void) {
  for (int i = 0; i < MAX_PENDING; i++) {
    pending_install_entry_t *entry = &g_pending_installs[i];
    if (entry->state != INSTALL_TRACK_QUEUED)
      continue;

    (void)bump_failed_mount_attempts(entry->title_id);
    log_batch_submit_retry_limit(entry);
  }
}
