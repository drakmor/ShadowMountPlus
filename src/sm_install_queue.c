#include "sm_platform.h"

#include <stdatomic.h>

#include "sm_install.h"
#include "sm_install_queue.h"
#include "sm_types.h"
#include "sm_appdb.h"
#include "sm_config_mount.h"
#include "sm_game_cache.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_manual.h"
#include "sm_runtime.h"
#include "sm_scan.h"
#include "sm_time.h"
#include "sm_title_state.h"

#define INSTALL_WAIT_TIMEOUT_US (300ull * 1000000ull)
#define INSTALL_POLL_INTERVAL_US (2ull * 1000000ull)
#define INSTALL_SUBMIT_RETRY_US (2ull * 1000000ull)
#define FPKG_GENERATION_NAME_BYTES 112u

typedef enum {
  INSTALL_TRACK_NONE = 0,
  INSTALL_TRACK_QUEUED,
  INSTALL_TRACK_SUBMITTED,
  INSTALL_TRACK_CLEANUP,
  INSTALL_TRACK_RETRY,
} install_track_state_t;

typedef struct pending_fpkg_generation {
  char name[FPKG_GENERATION_NAME_BYTES];
  struct pending_fpkg_generation *next;
} pending_fpkg_generation_t;

typedef struct {
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  char source_path[MAX_PATH];
  pending_fpkg_generation_t *previous_fpkg_generations;
  pending_fpkg_generation_t *previous_fpkg_generation_tail;
  size_t previous_fpkg_generation_count;
  char manual_source_path[MAX_PATH];
  uint64_t requested_at_us;
  bool has_src_snd0;
  bool manual;
  bool registration_confirmed;
  install_track_state_t state;
} pending_install_entry_t;

static pending_install_entry_t g_pending_installs[MAX_PENDING];
static uint64_t g_pending_install_poll_due_us = 0;
static uint64_t g_queued_install_submit_due_us = 0;
static atomic_int g_tracked_install_count = 0;
static atomic_int g_registration_pending_count = 0;
static int g_submitted_install_count = 0;
static bool g_queued_install_batch_announced = false;
static bool g_queued_install_submit_failure_notified = false;

static int count_queued_installs(void);
static uint8_t note_pending_install_failure(const pending_install_entry_t *entry);
static void drop_queued_install_entry(pending_install_entry_t *entry);

static bool append_fpkg_generation(pending_install_entry_t *entry,
                                  const char *generation_name) {
  if (!generation_name || !generation_name[0])
    return true;
  size_t length = strnlen(generation_name, FPKG_GENERATION_NAME_BYTES);
  if (length == 0 || length >= FPKG_GENERATION_NAME_BYTES)
    return false;
  for (pending_fpkg_generation_t *item = entry->previous_fpkg_generations;
       item; item = item->next)
    if (strcmp(item->name, generation_name) == 0)
      return true;
  pending_fpkg_generation_t *item = calloc(1, sizeof(*item));
  if (!item)
    return false;
  (void)strlcpy(item->name, generation_name, sizeof(item->name));
  if (entry->previous_fpkg_generation_tail)
    entry->previous_fpkg_generation_tail->next = item;
  else
    entry->previous_fpkg_generations = item;
  entry->previous_fpkg_generation_tail = item;
  ++entry->previous_fpkg_generation_count;
  return true;
}

static void free_fpkg_generations(pending_install_entry_t *entry) {
  if (!entry)
    return;
  pending_fpkg_generation_t *item = entry->previous_fpkg_generations;
  while (item) {
    pending_fpkg_generation_t *next = item->next;
    free(item);
    item = next;
  }
  entry->previous_fpkg_generations = NULL;
  entry->previous_fpkg_generation_tail = NULL;
  entry->previous_fpkg_generation_count = 0;
}

static bool release_pending_fpkg_generations(
    pending_install_entry_t *entry) {
  if (!entry)
    return false;
  bool all_released = true;
  pending_fpkg_generation_t *previous = NULL;
  pending_fpkg_generation_t *item = entry->previous_fpkg_generations;
  while (item) {
    pending_fpkg_generation_t *next = item->next;
    if (sm_install_release_replaced_fpkg_generation(
            entry->title_id, item->name, entry->source_path)) {
      if (previous)
        previous->next = next;
      else
        entry->previous_fpkg_generations = next;
      if (entry->previous_fpkg_generation_tail == item)
        entry->previous_fpkg_generation_tail = previous;
      if (entry->previous_fpkg_generation_count > 0)
        --entry->previous_fpkg_generation_count;
      free(item);
    } else {
      all_released = false;
      previous = item;
    }
    item = next;
  }
  return all_released;
}

static bool install_queue_active(void) {
  return runtime_config().app_install_all_enabled ||
         atomic_load_explicit(&g_tracked_install_count,
                              memory_order_acquire) > 0;
}

bool sm_install_has_pending_work(void) {
  return atomic_load_explicit(&g_registration_pending_count,
                              memory_order_acquire) > 0;
}

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
  bool needs_poll = false;
  for (int i = 0; i < MAX_PENDING; ++i) {
    if (g_pending_installs[i].state == INSTALL_TRACK_SUBMITTED ||
        g_pending_installs[i].state == INSTALL_TRACK_CLEANUP) {
      needs_poll = true;
      break;
    }
  }
  if (!needs_poll) {
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
  if (entry->state == INSTALL_TRACK_QUEUED ||
      entry->state == INSTALL_TRACK_SUBMITTED)
    atomic_fetch_sub_explicit(&g_registration_pending_count, 1,
                              memory_order_release);
  if (entry->state != INSTALL_TRACK_NONE &&
      atomic_load_explicit(&g_tracked_install_count,
                           memory_order_relaxed) > 0) {
    atomic_fetch_sub_explicit(&g_tracked_install_count, 1,
                              memory_order_release);
  }
  free_fpkg_generations(entry);
  memset(entry, 0, sizeof(*entry));
  if (g_submitted_install_count <= 0)
    schedule_pending_install_poll(monotonic_time_us());
  if (atomic_load_explicit(&g_tracked_install_count,
                           memory_order_acquire) <= 0) {
    g_queued_install_submit_due_us = 0;
    g_queued_install_batch_announced = false;
    g_queued_install_submit_failure_notified = false;
  }
}

bool is_title_install_pending(const char *title_id) {
  pending_install_entry_t *entry = find_pending_install_entry(title_id);
  return entry && (entry->state == INSTALL_TRACK_QUEUED ||
                   entry->state == INSTALL_TRACK_SUBMITTED);
}

void sm_install_forget_pending_title(const char *title_id) {
  pending_install_entry_t *entry = find_pending_install_entry(title_id);
  if (!entry)
    return;
  if (entry->previous_fpkg_generation_count == 0) {
    clear_pending_install_entry(entry);
    return;
  }
  if (entry->state == INSTALL_TRACK_SUBMITTED) {
    if (g_submitted_install_count > 0)
      --g_submitted_install_count;
    atomic_fetch_sub_explicit(&g_registration_pending_count, 1,
                              memory_order_release);
  } else if (entry->state == INSTALL_TRACK_QUEUED) {
    atomic_fetch_sub_explicit(&g_registration_pending_count, 1,
                              memory_order_release);
  }
  entry->state = INSTALL_TRACK_CLEANUP;
  entry->requested_at_us = 0;
  entry->registration_confirmed = true;
  schedule_queued_install_submit(monotonic_time_us());
  schedule_pending_install_poll(monotonic_time_us());
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
                                bool has_src_snd0,
                                const char *previous_fpkg_generation_name) {
  if (!candidate || candidate->title_id[0] == '\0' ||
      candidate->title_name[0] == '\0' || candidate->path[0] == '\0') {
    return false;
  }

  if (!runtime_config().app_install_all_enabled)
    return false;

  bool was_queue_empty = (count_queued_installs() <= 0);
  pending_install_entry_t *entry =
      reserve_pending_install_entry(candidate->title_id);
  if (!entry) {
    log_debug("  [REG] install queue full, cannot queue %s (%s)",
              candidate->title_name, candidate->title_id);
    return false;
  }

  install_track_state_t previous_state = entry->state;
  if (previous_state == INSTALL_TRACK_SUBMITTED)
    return append_fpkg_generation(entry, previous_fpkg_generation_name);

  if (!append_fpkg_generation(entry, previous_fpkg_generation_name))
    return false;
  pending_fpkg_generation_t *pending_generations =
      entry->previous_fpkg_generations;
  pending_fpkg_generation_t *pending_generation_tail =
      entry->previous_fpkg_generation_tail;
  size_t pending_generation_count = entry->previous_fpkg_generation_count;

  memset(entry, 0, sizeof(*entry));
  (void)strlcpy(entry->title_id, candidate->title_id, sizeof(entry->title_id));
  (void)strlcpy(entry->title_name, candidate->title_name,
                sizeof(entry->title_name));
  (void)strlcpy(entry->source_path, candidate->path, sizeof(entry->source_path));
  entry->previous_fpkg_generations = pending_generations;
  entry->previous_fpkg_generation_tail = pending_generation_tail;
  entry->previous_fpkg_generation_count = pending_generation_count;
  (void)strlcpy(entry->manual_source_path, candidate->manual_source_path,
                sizeof(entry->manual_source_path));
  entry->manual = candidate->manual;
  entry->has_src_snd0 = has_src_snd0;
  entry->state = INSTALL_TRACK_QUEUED;
  if (previous_state == INSTALL_TRACK_NONE) {
    atomic_fetch_add_explicit(&g_tracked_install_count, 1,
                              memory_order_release);
    atomic_fetch_add_explicit(&g_registration_pending_count, 1,
                              memory_order_release);
  } else if (previous_state == INSTALL_TRACK_CLEANUP ||
             previous_state == INSTALL_TRACK_RETRY) {
    atomic_fetch_add_explicit(&g_registration_pending_count, 1,
                              memory_order_release);
  }
  if (was_queue_empty) {
    g_queued_install_batch_announced = false;
    g_queued_install_submit_failure_notified = false;
  }
  schedule_queued_install_submit(monotonic_time_us());
  return true;
}

bool sm_install_defer_fpkg_generation_cleanup(
    const char *title_id, const char *previous_fpkg_generation_name,
    const char *current_path) {
  if (!title_id || !title_id[0] || !previous_fpkg_generation_name ||
      !previous_fpkg_generation_name[0] || !current_path || !current_path[0])
    return false;

  pending_install_entry_t *entry = find_pending_install_entry(title_id);
  if (!entry)
    entry = reserve_pending_install_entry(title_id);
  if (!entry)
    return false;

  install_track_state_t previous_state = entry->state;
  if (!append_fpkg_generation(entry, previous_fpkg_generation_name))
    return false;

  if (previous_state == INSTALL_TRACK_NONE) {
    pending_fpkg_generation_t *pending_generations =
        entry->previous_fpkg_generations;
    pending_fpkg_generation_t *pending_generation_tail =
        entry->previous_fpkg_generation_tail;
    size_t pending_generation_count = entry->previous_fpkg_generation_count;
    memset(entry, 0, sizeof(*entry));
    entry->previous_fpkg_generations = pending_generations;
    entry->previous_fpkg_generation_tail = pending_generation_tail;
    entry->previous_fpkg_generation_count = pending_generation_count;
    (void)strlcpy(entry->title_id, title_id, sizeof(entry->title_id));
    (void)strlcpy(entry->source_path, current_path,
                  sizeof(entry->source_path));
    entry->state = INSTALL_TRACK_CLEANUP;
    entry->registration_confirmed = true;
    atomic_fetch_add_explicit(&g_tracked_install_count, 1,
                              memory_order_release);
  } else if (previous_state == INSTALL_TRACK_CLEANUP ||
             previous_state == INSTALL_TRACK_RETRY) {
    (void)strlcpy(entry->source_path, current_path,
                  sizeof(entry->source_path));
    entry->state = INSTALL_TRACK_CLEANUP;
    entry->registration_confirmed = true;
  }
  schedule_pending_install_poll(monotonic_time_us());
  return true;
}

static void finalize_pending_install_success(pending_install_entry_t *entry) {
  if (!entry)
    return;

  if (!entry->registration_confirmed) {
    log_debug("  [REG] Installed: %s (%s)", entry->title_name,
              entry->title_id);
    notify_game_installed_rich(entry->title_id);
    if (entry->has_src_snd0) {
      int snd0_updates = update_snd0info(entry->title_id);
      if (snd0_updates >= 0)
        log_debug("  [DB] snd0info updated rows=%d", snd0_updates);
    }
    cache_game_entry(entry->source_path, entry->title_id, entry->title_name);
    if (entry->manual)
      sm_manual_note_installed(entry->manual_source_path, entry->title_id,
                               entry->title_name);
    clear_register_attempts(entry->title_id);
    clear_failed_mount_attempts(entry->title_id);
    entry->registration_confirmed = true;
  }
  if (!release_pending_fpkg_generations(entry)) {
    if (entry->state == INSTALL_TRACK_SUBMITTED) {
      if (g_submitted_install_count > 0)
        --g_submitted_install_count;
      atomic_fetch_sub_explicit(&g_registration_pending_count, 1,
                                memory_order_release);
    }
    entry->state = INSTALL_TRACK_CLEANUP;
    schedule_pending_install_poll(monotonic_time_us());
    return;
  }
  clear_pending_install_entry(entry);
}

static void finalize_pending_install_timeout(pending_install_entry_t *entry) {
  if (!entry)
    return;

  log_debug("  [REG] Install timeout after 5 minutes: %s (%s)",
            entry->title_name, entry->title_id);
  notify_system_l10n(SM_L10N_INSTALL_FAILED_TIMEOUT, entry->title_name,
                     entry->title_id);
  (void)note_pending_install_failure(entry);
  if (entry->previous_fpkg_generation_count > 0) {
    if (g_submitted_install_count > 0)
      --g_submitted_install_count;
    atomic_fetch_sub_explicit(&g_registration_pending_count, 1,
                              memory_order_release);
    entry->state = INSTALL_TRACK_RETRY;
    entry->requested_at_us = 0;
    schedule_pending_install_poll(monotonic_time_us());
  } else {
    clear_pending_install_entry(entry);
  }
}

static void poll_pending_installs(bool release_scan_mounts) {
  struct AppDbTitleList app_db_titles = {0};
  bool app_db_titles_ready = false;
  if (g_submitted_install_count > 0) {
    invalidate_app_db_title_cache();
    app_db_titles_ready = get_app_db_title_list_cached(&app_db_titles);
  }
  uint64_t now_us = monotonic_time_us();

  if (g_submitted_install_count > 0 && !app_db_titles_ready)
    log_debug("  [DB] app.db title list unavailable while polling installs");

  for (int i = 0; i < MAX_PENDING; i++) {
    pending_install_entry_t *entry = &g_pending_installs[i];
    if (entry->state == INSTALL_TRACK_CLEANUP) {
      if (release_pending_fpkg_generations(entry))
        clear_pending_install_entry(entry);
      continue;
    }
    if (entry->state != INSTALL_TRACK_SUBMITTED)
      continue;

    if (app_db_titles_ready &&
        app_db_title_list_contains(&app_db_titles, entry->title_id)) {
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
  free_app_db_title_list(&app_db_titles);
  if (release_scan_mounts && !sm_install_has_pending_work() &&
      !release_scan_runtime_mounts())
    log_debug("  [IMG] deferred release after install completion");
}

uint64_t sm_install_next_wake_us(uint64_t now_us) {
  if (runtime_sleep_mode_active())
    return 0;

  if (!install_queue_active())
    return 0;

  if (g_submitted_install_count <= 0) {
    if (g_pending_install_poll_due_us == 0)
      return g_queued_install_submit_due_us;
    if (g_queued_install_submit_due_us == 0 ||
        g_pending_install_poll_due_us < g_queued_install_submit_due_us)
      return g_pending_install_poll_due_us;
    return g_queued_install_submit_due_us;
  }

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
  if (runtime_sleep_mode_active())
    return;

  poll_pending_installs(true);
}

void sm_install_poll_pending_for_scan(void) {
  if (runtime_sleep_mode_active())
    return;

  poll_pending_installs(false);
}

void sm_install_service_pending(void) {
  if (runtime_sleep_mode_active())
    return;

  if (!install_queue_active())
    return;

  sm_install_poll_pending();
  if (count_queued_installs() > 0 && !sm_install_submit_queued())
    sm_install_note_submit_failure();
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
  int queued_count = 0;
  for (int i = 0; i < MAX_PENDING; ++i)
    queued_count += g_pending_installs[i].state == INSTALL_TRACK_QUEUED;
  return queued_count;
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
  if (queued_count <= 0 || g_queued_install_batch_announced)
    return;

  char message[3075];
  int shown_count = 0;
  snprintf(message, sizeof(message), sm_l10n_get(SM_L10N_BATCH_INSTALL_QUEUED),
           queued_count);

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
      (void)snprintf(message + used, sizeof(message) - used,
                     sm_l10n_get(SM_L10N_BATCH_INSTALL_AND_MORE),
                     queued_count - shown_count);
    }
  }

  notify_system_info("%s", message);
  g_queued_install_batch_announced = true;
}

static uint8_t note_pending_install_failure(const pending_install_entry_t *entry) {
  if (!entry)
    return 0;

  uint8_t failed_attempts = bump_failed_mount_attempts(entry->title_id);
  log_batch_submit_retry_limit(entry);
  return failed_attempts;
}

static void drop_queued_install_entry(pending_install_entry_t *entry) {
  if (!entry || entry->state != INSTALL_TRACK_QUEUED)
    return;

  log_debug("  [REG] Dropping queued install after retry limit: %s (%s)",
            entry->title_name, entry->title_id);
  if (entry->previous_fpkg_generation_count > 0) {
    atomic_fetch_sub_explicit(&g_registration_pending_count, 1,
                              memory_order_release);
    entry->state = INSTALL_TRACK_RETRY;
    entry->requested_at_us = 0;
    if (count_queued_installs() == 0) {
      g_queued_install_submit_due_us = 0;
      g_queued_install_batch_announced = false;
      g_queued_install_submit_failure_notified = false;
    }
    schedule_pending_install_poll(monotonic_time_us());
  } else {
    clear_pending_install_entry(entry);
  }
}

bool sm_install_submit_queued(void) {
  if (runtime_sleep_mode_active())
    return true;

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

  int res = sceAppInstUtilAppInstallAll(NULL);
  if (res != 0) {
    log_debug("  [REG] Batch install request failed: 0x%x", res);
    if (!g_queued_install_submit_failure_notified) {
      notify_system_l10n(SM_L10N_BATCH_INSTALL_FAILED, queued_count,
                         (uint32_t)res);
      g_queued_install_submit_failure_notified = true;
    }
    schedule_queued_install_submit_retry(now_us);
    return false;
  }

  g_queued_install_submit_due_us = 0;
  g_queued_install_submit_failure_notified = false;
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

    if (note_pending_install_failure(entry) >= MAX_FAILED_MOUNT_ATTEMPTS)
      drop_queued_install_entry(entry);
  }
}
