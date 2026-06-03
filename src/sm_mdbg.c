#include "sm_platform.h"

#include <inttypes.h>
#include <stdlib.h>

#include "sm_config_mount.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_mdbg.h"
#include "sm_time.h"
#include "sm_types.h"

int sceKernelDebugGetPrivateLogText(void *buffer, size_t buffer_size,
                                    char **text, uint64_t *text_size);
int sceKernelDebugGetSdkLogText(void *buffer, size_t buffer_size,
                                char **text, uint64_t *text_size);
uint64_t sceKernelDebugGetLogBufferSize(void) __asm__("C49jelxiaVE");
int mdbg_call(void *cmd, void *req, void *res);

#define MDBG_CMD_TYPE_SERVICE 1ull
#define MDBG_CMD_PROCESS_STATE 30ull
#define MDBG_SUBCMD_FLAGS 2ull

#define MDBG_FLAG_EXCEPTION_STOP 0x00080000ull
#define MDBG_AUTOTUNE_WINDOW_US (300ull * 1000000ull)
#define MDBG_LOG_LINE_BUFFER_SIZE 512u
#define MDBG_RTLD_ERROR_PREFIX_SIZE 32u
#ifndef MDBG_USE_PRIVATE_LOG_TEXT
#define MDBG_USE_PRIVATE_LOG_TEXT 0
#endif
#ifndef MDBG_SKIP_PRIVILEGE_ELEVATION
#define MDBG_SKIP_PRIVILEGE_ELEVATION 0
#endif

#if MDBG_USE_PRIVATE_LOG_TEXT
#define MDBG_FETCH_LOG_TEXT sceKernelDebugGetPrivateLogText
#else
#define MDBG_FETCH_LOG_TEXT sceKernelDebugGetSdkLogText
#endif

typedef struct {
  uint64_t type;
  uint64_t cmd;
} mdbg_cmd_t;

typedef struct {
  int64_t pid;
  int64_t subcmd;
  uint64_t arg;
  uint64_t reserved[5];
} mdbg_req_t;

typedef struct {
  int64_t status;
  uint64_t value;
  uint64_t reserved[2];
} mdbg_res_t;

typedef struct {
  bool active;
  bool log_monitoring_active;
  bool pause_seen;
  pid_t pid;
  uint32_t pause_delay_seconds;
  uint64_t monitor_deadline_us;
  uint64_t pause_time_us;
  uint64_t next_poll_us;
  char title_id[MAX_TITLE_ID];
  char rtld_error_prefix[MDBG_RTLD_ERROR_PREFIX_SIZE];
} mdbg_game_state_t;

typedef struct {
  bool privilege_probe_done;
  bool privilege_ready;
  size_t log_buffer_size;
  size_t log_snapshot_length;
  size_t log_line_length;
  char *log_snapshot;
  char *log_storage;
  char log_line[MDBG_LOG_LINE_BUFFER_SIZE];
  mdbg_game_state_t game;
} sm_mdbg_state_t;

static sm_mdbg_state_t g_mdbg;

static bool sm_mdbg_enabled(void);
#if !MDBG_SKIP_PRIVILEGE_ELEVATION
static int elevate_to_coredump(void);
#endif
static bool ensure_mdbg_privileges(void);
static int mdbg_call_raw(int64_t pid, uint64_t subcmd, uint64_t arg,
                         int64_t *status_out, uint64_t *value_out);
static int query_mdbg_flags(pid_t pid, uint64_t *flags_out);
static bool is_mdbg_process_gone_error(int ret);
static void reset_log_line_buffer(void);
static void reset_log_snapshot(void);
static void free_log_buffers(void);
static bool ensure_log_buffers(void);
static int fetch_log_text(const char **text_out, size_t *text_len_out);
static size_t find_log_overlap(const char *current, size_t current_len);
static void update_log_snapshot(const char *text, size_t text_len);
static void clear_tracked_game(void);
static bool matches_tracked_rtld_error(const char *text);
static void summarize_failure_reason(const char *reason, char *summary_out,
                                     size_t summary_out_size,
                                     bool is_rtld_error);
static void handle_pre_pause_failure(const char *reason);
static void handle_post_pause_failure(const char *reason, uint64_t now_us);
static void process_log_line(const char *line, uint64_t now_us);
static void append_log_char(char ch, uint64_t now_us);
static void poll_log_monitor(uint64_t now_us);
static void start_log_monitoring(void);
static void handle_crash_candidate(uint64_t flags, uint64_t now_us);

static bool sm_mdbg_enabled(void) {
  return runtime_config()->kstuff_crash_detection_enabled &&
         runtime_config()->kstuff_game_auto_toggle;
}

#if !MDBG_SKIP_PRIVILEGE_ELEVATION
#define SCE_AUTHID_COREDUMP 0x4800000000000006ull
static int elevate_to_coredump(void) {
  static const uint8_t k_priv_caps[16] = {
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  pid_t pid = getpid();

  if (kernel_set_ucred_authid(pid, SCE_AUTHID_COREDUMP) < 0)
    return -1;
  if (kernel_set_ucred_caps(pid, k_priv_caps) < 0)
    return -1;

  return 0;
}
#endif

static bool ensure_mdbg_privileges(void) {
  if (g_mdbg.privilege_probe_done)
    return g_mdbg.privilege_ready;

  g_mdbg.privilege_probe_done = true;
#if MDBG_SKIP_PRIVILEGE_ELEVATION
  g_mdbg.privilege_ready = true;
  log_debug("  [MDBG] coredump privilege probe skipped");
#else
  g_mdbg.privilege_ready = elevate_to_coredump() == 0;
  if (g_mdbg.privilege_ready) {
    log_debug("  [MDBG] coredump privileges enabled");
  } else {
    log_debug("  [MDBG] failed to enable coredump privileges; mdbg polling "
              "disabled, log monitoring disabled");
  }
#endif

  return g_mdbg.privilege_ready;
}

static int mdbg_call_raw(int64_t pid, uint64_t subcmd, uint64_t arg,
                         int64_t *status_out, uint64_t *value_out) {
  mdbg_cmd_t cmd = {MDBG_CMD_TYPE_SERVICE, MDBG_CMD_PROCESS_STATE};
  mdbg_req_t req = {
      .pid = pid,
      .subcmd = (int64_t)subcmd,
      .arg = arg,
  };
  mdbg_res_t res = {0};
  int ret = mdbg_call(&cmd, &req, &res);
  if (ret != 0)
    return ret;

  if (status_out)
    *status_out = res.status;
  if (value_out)
    *value_out = res.value;
  return 0;
}

static int query_mdbg_flags(pid_t pid, uint64_t *flags_out) {
  int64_t status;
  uint64_t value;
  int ret = mdbg_call_raw(pid, MDBG_SUBCMD_FLAGS, 0, &status, &value);
  if (ret < 0)
    return ret;
  if (status != 0)
    return (int)status;
  if (flags_out)
    *flags_out = value;
  return 0;
}

static bool is_mdbg_process_gone_error(int ret) {
  return ret == -ESRCH || ret == -ENOENT || ret == ESRCH || ret == ENOENT;
}

static void reset_log_line_buffer(void) {
  g_mdbg.log_line_length = 0;
  g_mdbg.log_line[0] = '\0';
}

static void reset_log_snapshot(void) {
  g_mdbg.log_snapshot_length = 0;
  reset_log_line_buffer();
}

static void free_log_buffers(void) {
  free(g_mdbg.log_snapshot);
  free(g_mdbg.log_storage);
  g_mdbg.log_snapshot = NULL;
  g_mdbg.log_storage = NULL;
  g_mdbg.log_buffer_size = 0;
  reset_log_snapshot();
}

static bool ensure_log_buffers(void) {
  if (g_mdbg.log_storage && g_mdbg.log_snapshot && g_mdbg.log_buffer_size != 0)
    return true;

  free_log_buffers();

  const char *title_id =
      g_mdbg.game.title_id[0] != '\0' ? g_mdbg.game.title_id : "?";
  uint64_t raw_size = sceKernelDebugGetLogBufferSize();
  if (raw_size == 0) {
    log_debug("  [MDBG] invalid log buffer size for %s: 0x%016" PRIx64,
              title_id,
              raw_size);
    return false;
  }

  size_t buffer_size = (size_t)raw_size;
  char *storage = malloc(buffer_size);
  char *snapshot = malloc(buffer_size);
  if (!storage || !snapshot) {
    free(storage);
    free(snapshot);
    log_debug("  [MDBG] failed to allocate log buffers for %s: size=0x%zx",
              title_id,
              buffer_size);
    return false;
  }

  g_mdbg.log_storage = storage;
  g_mdbg.log_snapshot = snapshot;
  g_mdbg.log_buffer_size = buffer_size;
  reset_log_snapshot();
  log_debug("  [MDBG] log monitor ready: %s buffer_size=0x%zx", title_id,
            buffer_size);
  return true;
}

static int fetch_log_text(const char **text_out, size_t *text_len_out) {
  char *raw_text = NULL;
  uint64_t raw_len = 0;

  *text_out = NULL;
  *text_len_out = 0;

  int ret =
      MDBG_FETCH_LOG_TEXT(g_mdbg.log_storage, g_mdbg.log_buffer_size, &raw_text,
                          &raw_len);
  if (ret < 0)
    return ret;

  if (!raw_text || raw_len == 0) {
    return 0;
  }
  if (raw_len > g_mdbg.log_buffer_size)
    return -EOVERFLOW;

  *text_out = raw_text;
  *text_len_out = (size_t)raw_len;
  return 0;
}

static size_t find_log_overlap(const char *current, size_t current_len) {
  if (!g_mdbg.log_snapshot || g_mdbg.log_snapshot_length == 0 || !current ||
      current_len == 0) {
    return 0;
  }

  size_t max_overlap = g_mdbg.log_snapshot_length < current_len
                           ? g_mdbg.log_snapshot_length
                           : current_len;

  const char *search = g_mdbg.log_snapshot + g_mdbg.log_snapshot_length -
                       max_overlap;
  const char *end = g_mdbg.log_snapshot + g_mdbg.log_snapshot_length;
  while (search < end) {
    const char *candidate =
        memchr(search, current[0], (size_t)(end - search));
    if (!candidate)
      break;

    size_t overlap = (size_t)(end - candidate);
    if (!memcmp(candidate, current, overlap))
      return overlap;

    search = candidate + 1;
  }

  return 0;
}

static void update_log_snapshot(const char *text, size_t text_len) {
  if (!g_mdbg.log_snapshot || g_mdbg.log_buffer_size == 0)
    return;

  if (!text || text_len == 0) {
    g_mdbg.log_snapshot_length = 0;
    return;
  }

  if (text_len > g_mdbg.log_buffer_size)
    text_len = g_mdbg.log_buffer_size;

  memcpy(g_mdbg.log_snapshot, text, text_len);
  g_mdbg.log_snapshot_length = text_len;
}

static void clear_tracked_game(void) {
  memset(&g_mdbg.game, 0, sizeof(g_mdbg.game));
  free_log_buffers();
}

static bool matches_tracked_rtld_error(const char *text) {
  return text &&
         g_mdbg.game.rtld_error_prefix[0] != '\0' &&
         strstr(text, g_mdbg.game.rtld_error_prefix) != NULL;
}

static void summarize_failure_reason(const char *reason, char *summary_out,
                                     size_t summary_out_size,
                                     bool is_rtld_error) {
  if (!summary_out || summary_out_size == 0)
    return;

  summary_out[0] = '\0';
  if (!reason || reason[0] == '\0')
    return;

  if (is_rtld_error) {
    const char *open_paren = strrchr(reason, '(');
    const char *close_paren =
        open_paren ? strchr(open_paren + 1, ')') : NULL;
    if (open_paren && close_paren && close_paren > open_paren + 1) {
      int written = snprintf(summary_out, summary_out_size,
                             "can't load module %.*s after KStuff pause",
                             (int)(close_paren - open_paren - 1),
                             open_paren + 1);
      if (written > 0 && (size_t)written < summary_out_size)
        return;
    }

    (void)strlcpy(summary_out, "can't load module after KStuff pause",
                  summary_out_size);
    return;
  }

  (void)strlcpy(summary_out, reason, summary_out_size);
}

static void handle_pre_pause_failure(const char *reason) {
  log_debug("  [MDBG] %s crashed before kstuff auto-pause%s%s",
            g_mdbg.game.title_id, reason ? ": " : "", reason ? reason : "");
  notify_system_info_l10n(SM_L10N_CRASH_BEFORE_KSTUFF,
                          g_mdbg.game.title_id);
  clear_tracked_game();
}

static void handle_post_pause_failure(const char *reason, uint64_t now_us) {
  if (!g_mdbg.game.pause_seen || g_mdbg.game.pause_time_us == 0 ||
      now_us < g_mdbg.game.pause_time_us) {
    handle_pre_pause_failure(reason);
    return;
  }

  uint64_t post_pause_us = now_us - g_mdbg.game.pause_time_us;
  if (post_pause_us > MDBG_AUTOTUNE_WINDOW_US) {
    log_debug("  [MDBG] %s crashed %us after kstuff pause; autotune skipped%s%s",
              g_mdbg.game.title_id, (unsigned)(post_pause_us / 1000000ull),
              reason ? ": " : "", reason ? reason : "");
    notify_system_info_l10n(SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED,
                            g_mdbg.game.title_id);
    clear_tracked_game();
    return;
  }

  char reason_summary[128];
  bool is_rtld_error = matches_tracked_rtld_error(reason);
  summarize_failure_reason(reason, reason_summary, sizeof(reason_summary),
                           is_rtld_error);

  uint32_t tuned_delay_seconds = 0;
  if (upsert_kstuff_autotune_pause_delay(g_mdbg.game.title_id,
                                         g_mdbg.game.pause_delay_seconds,
                                         &tuned_delay_seconds)) {
    log_debug("  [MDBG] autotune pause delay updated: %s=%us",
              g_mdbg.game.title_id, tuned_delay_seconds);
    if (reason_summary[0] != '\0')
      log_debug("  [MDBG] autotune trigger: %s", reason_summary);
    if (is_rtld_error) {
      notify_system_info_l10n(
          SM_L10N_DELAY_INCREASED_RELAUNCH, g_mdbg.game.title_id,
          reason_summary[0] != '\0'
              ? reason_summary
              : sm_l10n_get(SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK),
          tuned_delay_seconds);
    } else {
      notify_system_info_l10n(SM_L10N_CRASH_DELAY_INCREASED,
                              g_mdbg.game.title_id, tuned_delay_seconds);
    }
    clear_tracked_game();
    return;
  }

  log_debug("  [MDBG] failed to persist autotune pause delay for %s%s%s",
            g_mdbg.game.title_id, reason ? ": " : "", reason ? reason : "");
  notify_system_info_l10n(SM_L10N_CRASH_DELAY_UPDATE_FAILED,
                          g_mdbg.game.title_id);
  clear_tracked_game();
}

static void process_log_line(const char *line, uint64_t now_us) {
  if (!matches_tracked_rtld_error(line))
    return;

  log_debug("  [MDBG] log load error for %s: %s", g_mdbg.game.title_id, line);
  handle_post_pause_failure(line, now_us);
}

static void append_log_char(char ch, uint64_t now_us) {
  if (!g_mdbg.game.active)
    return;

  if (ch == '\r' || ch == '\n') {
    if (g_mdbg.log_line_length == 0)
      return;
    g_mdbg.log_line[g_mdbg.log_line_length] = '\0';
    process_log_line(g_mdbg.log_line, now_us);
    reset_log_line_buffer();
    return;
  }

  if (g_mdbg.log_line_length + 1u >= sizeof(g_mdbg.log_line)) {
    g_mdbg.log_line[g_mdbg.log_line_length] = '\0';
    process_log_line(g_mdbg.log_line, now_us);
    reset_log_line_buffer();
  }

  g_mdbg.log_line[g_mdbg.log_line_length++] = ch;
}

static void poll_log_monitor(uint64_t now_us) {
  if (!g_mdbg.game.active || !g_mdbg.game.log_monitoring_active) {
    return;
  }

  if (!ensure_log_buffers()) {
    g_mdbg.game.log_monitoring_active = false;
    return;
  }

  const char *text = NULL;
  size_t text_len = 0;
  int ret = fetch_log_text(&text, &text_len);
  if (ret < 0) {
    log_debug("  [MDBG] log snapshot failed for %s: 0x%08x",
              g_mdbg.game.title_id, ret);
    g_mdbg.game.log_monitoring_active = false;
    reset_log_snapshot();
    return;
  }

  size_t skip = 0;
  if (g_mdbg.log_snapshot_length != 0 && text_len >= g_mdbg.log_snapshot_length &&
      !memcmp(g_mdbg.log_snapshot, text, g_mdbg.log_snapshot_length)) {
    skip = g_mdbg.log_snapshot_length;
  } else {
    skip = find_log_overlap(text, text_len);
    if (skip == 0 && g_mdbg.log_snapshot_length != 0) {
      log_debug("  [MDBG] log stream reset or truncated for %s",
                g_mdbg.game.title_id);
      reset_log_line_buffer();
    }
  }

  for (size_t i = skip; i < text_len; ++i) {
    append_log_char(text[i], now_us);
    if (!g_mdbg.game.active)
      return;
  }

  update_log_snapshot(text, text_len);
}

static void start_log_monitoring(void) {
  if (!g_mdbg.game.active)
    return;

  if (!ensure_mdbg_privileges()) {
    g_mdbg.game.log_monitoring_active = false;
    return;
  }

  if (!ensure_log_buffers()) {
    g_mdbg.game.log_monitoring_active = false;
    return;
  }

  g_mdbg.game.log_monitoring_active = true;

  const char *text = NULL;
  size_t text_len = 0;
  int ret = fetch_log_text(&text, &text_len);
  if (ret < 0) {
    log_debug("  [MDBG] initial log snapshot failed for %s: 0x%08x",
              g_mdbg.game.title_id, ret);
    g_mdbg.game.log_monitoring_active = false;
    reset_log_snapshot();
    return;
  }

  update_log_snapshot(text, text_len);
  reset_log_line_buffer();
}

static void handle_crash_candidate(uint64_t flags, uint64_t now_us) {
  if (!g_mdbg.game.active)
    return;

  log_debug("  [MDBG] crash-candidate: %s pid=%ld flags=0x%08" PRIx64,
            g_mdbg.game.title_id, (long)g_mdbg.game.pid, flags);

  handle_post_pause_failure("crash-candidate", now_us);
}

void sm_mdbg_init(void) {
  memset(&g_mdbg, 0, sizeof(g_mdbg));
}

void sm_mdbg_shutdown(void) {
  free_log_buffers();
  memset(&g_mdbg, 0, sizeof(g_mdbg));
}

void sm_mdbg_game_on_exec(pid_t pid, const char *title_id, uint32_t app_id) {
  if (pid <= 0 || !title_id || title_id[0] == '\0')
    return;
  if (!sm_mdbg_enabled()) {
    clear_tracked_game();
    return;
  }

  if (g_mdbg.game.active && g_mdbg.game.pid != pid) {
    log_debug("  [MDBG] replacing tracked game pid=%ld (%s) with pid=%ld (%s)",
              (long)g_mdbg.game.pid, g_mdbg.game.title_id, (long)pid, title_id);
  }

  clear_tracked_game();

  uint64_t now_us = monotonic_time_us();
  g_mdbg.game.active = true;
  g_mdbg.game.pid = pid;
  g_mdbg.game.next_poll_us = now_us;
  (void)strlcpy(g_mdbg.game.title_id, title_id, sizeof(g_mdbg.game.title_id));
  int rtld_prefix_len =
      snprintf(g_mdbg.game.rtld_error_prefix,
               sizeof(g_mdbg.game.rtld_error_prefix),
               "[rtld] <%ld> ERROR", (long)pid);
  if (rtld_prefix_len <= 0 ||
      (size_t)rtld_prefix_len >= sizeof(g_mdbg.game.rtld_error_prefix)) {
    g_mdbg.game.rtld_error_prefix[0] = '\0';
  }

  log_debug("  [MDBG] tracking crash-candidate state: %s pid=%ld app_id=0x%08X",
            g_mdbg.game.title_id, (long)pid, app_id);
}

void sm_mdbg_game_on_kstuff_pause(pid_t pid, uint64_t pause_time_us,
                                  uint32_t pause_delay_seconds) {
  if (!g_mdbg.game.active || g_mdbg.game.pid != pid)
    return;

  g_mdbg.game.pause_seen = true;
  g_mdbg.game.pause_time_us = pause_time_us;
  g_mdbg.game.monitor_deadline_us =
      g_mdbg.game.pause_time_us + MDBG_AUTOTUNE_WINDOW_US;
  g_mdbg.game.pause_delay_seconds = pause_delay_seconds;
  g_mdbg.game.next_poll_us = pause_time_us;
  start_log_monitoring();
}

void sm_mdbg_game_on_exit(pid_t pid) {
  if (!g_mdbg.game.active || g_mdbg.game.pid != pid)
    return;

  clear_tracked_game();
}

void sm_mdbg_game_shutdown(void) {
  clear_tracked_game();
}

uint64_t sm_mdbg_next_wake_us(uint64_t now_us) {
  (void)now_us;
  if (!sm_mdbg_enabled())
    return 0;
  if (!g_mdbg.game.active)
    return 0;

  uint64_t next_wake_us = g_mdbg.game.next_poll_us;
  if (g_mdbg.game.monitor_deadline_us != 0 &&
      (next_wake_us == 0 || g_mdbg.game.monitor_deadline_us < next_wake_us)) {
    next_wake_us = g_mdbg.game.monitor_deadline_us;
  }
  return next_wake_us;
}

void sm_mdbg_poll(void) {
  if (!sm_mdbg_enabled())
    return;
  if (!g_mdbg.game.active)
    return;

  uint64_t now_us = monotonic_time_us();
  if (now_us < g_mdbg.game.next_poll_us) {
    return;
  }
  if (g_mdbg.game.pause_seen && g_mdbg.game.monitor_deadline_us != 0 &&
      now_us >= g_mdbg.game.monitor_deadline_us) {
    log_debug("  [MDBG] crash monitoring window expired for %s pid=%ld",
              g_mdbg.game.title_id, (long)g_mdbg.game.pid);
    clear_tracked_game();
    return;
  }

  if (!ensure_mdbg_privileges()) {
    g_mdbg.game.next_poll_us = 0;
    return;
  }

  poll_log_monitor(now_us);
  if (!g_mdbg.game.active)
    return;

  g_mdbg.game.next_poll_us = now_us + GAME_LIFECYCLE_POLL_INTERVAL_US;

  uint64_t flags = 0;
  int ret = query_mdbg_flags(g_mdbg.game.pid, &flags);
  if (is_mdbg_process_gone_error(ret)) {
    clear_tracked_game();
    return;
  }
  if (ret != 0)
    return;

  if ((flags & MDBG_FLAG_EXCEPTION_STOP) == 0)
    return;

  handle_crash_candidate(flags, now_us);
}
