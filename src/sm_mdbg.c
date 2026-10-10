#include "sm_platform.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>

#include "sm_limits.h"
#include "sm_log.h"
#include "sm_mdbg.h"
#include "sm_time.h"

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
#define MDBG_MONITOR_WINDOW_US (60ull * 1000000ull)
#define MDBG_LOG_LINE_BUFFER_SIZE 512u
#define MDBG_RTLD_ERROR_PREFIX_SIZE 32u
#define MDBG_FATAL_ERROR_BUFFER_SIZE 256u
#define MDBG_FATAL_EXCEPTION_PREFIX "# exception: "
#define MDBG_FATAL_THREAD_NAME_PREFIX "# thread name: "
#define MDBG_FATAL_PROCESS_ID_PREFIX "# proc ID: "
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

typedef enum {
  MDBG_FAILURE_GENERIC = 0,
  MDBG_FAILURE_RTLD,
  MDBG_FAILURE_FATAL_SIGNAL,
} mdbg_failure_kind_t;

typedef struct {
  bool active;
  pid_t pid;
  uint64_t monitor_deadline_us;
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
  size_t fatal_error_length;
  pid_t fatal_error_pid;
  bool fatal_error_active;
  char *log_snapshot;
  char *log_storage;
  char log_line[MDBG_LOG_LINE_BUFFER_SIZE];
  char fatal_error[MDBG_FATAL_ERROR_BUFFER_SIZE];
  mdbg_game_state_t game;
} sm_mdbg_state_t;

static sm_mdbg_state_t g_mdbg;
static pthread_mutex_t g_mdbg_kernel_log_mutex = PTHREAD_MUTEX_INITIALIZER;

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
  pthread_mutex_lock(&g_mdbg_kernel_log_mutex);
  if (g_mdbg.privilege_probe_done) {
    bool ready = g_mdbg.privilege_ready;
    pthread_mutex_unlock(&g_mdbg_kernel_log_mutex);
    return ready;
  }

  g_mdbg.privilege_probe_done = true;
#if MDBG_SKIP_PRIVILEGE_ELEVATION
  g_mdbg.privilege_ready = true;
  log_debug("  [MDBG] coredump privilege probe skipped");
#else
  g_mdbg.privilege_ready = elevate_to_coredump() == 0;
  if (g_mdbg.privilege_ready) {
    log_debug("  [MDBG] coredump privileges enabled");
  } else {
    log_debug("  [MDBG] failed to enable coredump privileges; "
              "kernel log unavailable");
  }
#endif

  bool ready = g_mdbg.privilege_ready;
  pthread_mutex_unlock(&g_mdbg_kernel_log_mutex);
  return ready;
}

static int query_mdbg_flags(pid_t pid, uint64_t *flags_out) {
  mdbg_cmd_t cmd = {MDBG_CMD_TYPE_SERVICE, MDBG_CMD_PROCESS_STATE};
  mdbg_req_t req = {.pid = pid, .subcmd = MDBG_SUBCMD_FLAGS};
  mdbg_res_t res = {0};
  int ret = mdbg_call(&cmd, &req, &res);
  if (ret != 0)
    return ret;
  if (res.status != 0)
    return (int)res.status;
  *flags_out = res.value;
  return 0;
}

static bool is_mdbg_process_gone_error(int ret) {
  return ret == -ESRCH || ret == -ENOENT || ret == ESRCH || ret == ENOENT;
}

static void reset_log_line_buffer(void) {
  g_mdbg.log_line_length = 0;
  g_mdbg.log_line[0] = '\0';
}

static void reset_fatal_error_buffer(void) {
  g_mdbg.fatal_error_active = false;
  g_mdbg.fatal_error_length = 0;
  g_mdbg.fatal_error_pid = 0;
  g_mdbg.fatal_error[0] = '\0';
}

static void reset_log_snapshot(void) {
  g_mdbg.log_snapshot_length = 0;
  reset_log_line_buffer();
  reset_fatal_error_buffer();
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
  if (raw_size == 0 || raw_size > SIZE_MAX) {
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

static int fetch_log_text(void *storage, size_t buffer_size,
                          const char **text_out, size_t *text_len_out) {
  char *raw_text = NULL;
  uint64_t raw_len = 0;
  *text_out = NULL;
  *text_len_out = 0;

  pthread_mutex_lock(&g_mdbg_kernel_log_mutex);
  int ret = MDBG_FETCH_LOG_TEXT(storage, buffer_size, &raw_text, &raw_len);
  pthread_mutex_unlock(&g_mdbg_kernel_log_mutex);
  if (ret != 0)
    return EIO;
  if (raw_len == 0)
    return 0;
  if (!raw_text)
    return EIO;

  uintptr_t storage_start = (uintptr_t)storage;
  uintptr_t text_start = (uintptr_t)raw_text;
  if (text_start < storage_start || text_start - storage_start > buffer_size)
    return EIO;
  size_t offset = (size_t)(text_start - storage_start);
  if (raw_len > buffer_size - offset)
    return EOVERFLOW;

  *text_out = raw_text;
  *text_len_out = (size_t)raw_len;
  return 0;
}

int sm_mdbg_get_log_tail(size_t max_bytes, char **text_out,
                         size_t *text_length_out, size_t *total_length_out) {
  if (!text_out || !text_length_out || !total_length_out || max_bytes == 0)
    return EINVAL;
  *text_out = NULL;
  *text_length_out = 0;
  *total_length_out = 0;

  if (!ensure_mdbg_privileges())
    return EACCES;

  uint64_t raw_buffer_size = sceKernelDebugGetLogBufferSize();
  if (raw_buffer_size == 0 || raw_buffer_size > SIZE_MAX)
    return EOVERFLOW;
  size_t buffer_size = (size_t)raw_buffer_size;
  char *storage = malloc(buffer_size);
  if (!storage)
    return ENOMEM;

  const char *raw_text = NULL;
  size_t total_length = 0;
  int ret = fetch_log_text(storage, buffer_size, &raw_text, &total_length);
  if (ret != 0) {
    free(storage);
    return ret;
  }
  if (total_length == 0) {
    free(storage);
    char *empty = calloc(1u, 1u);
    if (!empty)
      return ENOMEM;
    *text_out = empty;
    return 0;
  }

  size_t start = total_length > max_bytes ? total_length - max_bytes : 0;
  if (start > 0) {
    char *newline = memchr(raw_text + start, '\n', total_length - start);
    if (newline)
      start = (size_t)(newline - raw_text) + 1u;
  }
  size_t text_length = total_length - start;
  char *result = malloc(text_length + 1u);
  if (!result) {
    free(storage);
    return ENOMEM;
  }
  memcpy(result, raw_text + start, text_length);
  result[text_length] = '\0';
  free(storage);

  *text_out = result;
  *text_length_out = text_length;
  *total_length_out = total_length;
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

static void handle_game_failure(const char *reason, uint64_t now_us,
                                mdbg_failure_kind_t kind) {
  if (!g_mdbg.game.active)
    return;
  if (now_us == 0 || now_us >= g_mdbg.game.monitor_deadline_us) {
    clear_tracked_game();
    return;
  }

  log_debug("  [MDBG] game error: %s pid=%ld: %s", g_mdbg.game.title_id,
            (long)g_mdbg.game.pid, reason);
  if (kind == MDBG_FAILURE_GENERIC)
    notify_system_info_l10n(SM_L10N_GAME_CRASHED, g_mdbg.game.title_id);
  else
    notify_system_info_l10n(SM_L10N_GAME_ERROR, g_mdbg.game.title_id, reason);
  clear_tracked_game();
}

static void append_fatal_error_detail(const char *detail) {
  if (!detail || detail[0] == '\0' ||
      g_mdbg.fatal_error_length + 1u >= sizeof(g_mdbg.fatal_error)) {
    return;
  }

  size_t remaining = sizeof(g_mdbg.fatal_error) - g_mdbg.fatal_error_length;
  int written = snprintf(g_mdbg.fatal_error + g_mdbg.fatal_error_length,
                         remaining, "%s%s",
                         g_mdbg.fatal_error_length != 0 ? "\n" : "",
                         detail);
  if (written < 0)
    return;
  if ((size_t)written >= remaining) {
    g_mdbg.fatal_error_length = sizeof(g_mdbg.fatal_error) - 1u;
    return;
  }

  g_mdbg.fatal_error_length += (size_t)written;
}

static void parse_fatal_error_pid(const char *line) {
  const char *value = line + strlen(MDBG_FATAL_PROCESS_ID_PREFIX);
  char *end = NULL;
  errno = 0;
  long parsed = strtol(value, &end, 10);
  pid_t pid = (pid_t)parsed;

  if (errno == ERANGE || end == value || end[0] != '\0' || pid <= 0 ||
      (long)pid != parsed)
    return;

  g_mdbg.fatal_error_pid = pid;
}

static void finish_fatal_error(uint64_t now_us) {
  if (!g_mdbg.fatal_error_active)
    return;

  g_mdbg.fatal_error_active = false;
  if (g_mdbg.fatal_error_length == 0) {
    reset_fatal_error_buffer();
    return;
  }

  if (g_mdbg.fatal_error_pid != g_mdbg.game.pid) {
    log_debug("  [MDBG] ignoring fatal error for pid=%ld while tracking pid=%ld",
              (long)g_mdbg.fatal_error_pid, (long)g_mdbg.game.pid);
    reset_fatal_error_buffer();
    return;
  }

  handle_game_failure(g_mdbg.fatal_error, now_us,
                       MDBG_FAILURE_FATAL_SIGNAL);
}

static void process_log_line(const char *line, uint64_t now_us) {
  if (matches_tracked_rtld_error(line)) {
    handle_game_failure(line, now_us, MDBG_FAILURE_RTLD);
    return;
  }

  if (!strncmp(line, MDBG_FATAL_EXCEPTION_PREFIX,
               strlen(MDBG_FATAL_EXCEPTION_PREFIX))) {
    reset_fatal_error_buffer();
    g_mdbg.fatal_error_active = true;
    append_fatal_error_detail(line + strlen(MDBG_FATAL_EXCEPTION_PREFIX));
    return;
  }

  if (g_mdbg.fatal_error_active) {
    if (line[0] != '#')
      return;

    if (!strcmp(line, "#")) {
      finish_fatal_error(now_us);
      return;
    }

    if (!strncmp(line, MDBG_FATAL_THREAD_NAME_PREFIX,
                 strlen(MDBG_FATAL_THREAD_NAME_PREFIX))) {
      append_fatal_error_detail(line + 2);
    } else if (!strncmp(line, MDBG_FATAL_PROCESS_ID_PREFIX,
                        strlen(MDBG_FATAL_PROCESS_ID_PREFIX))) {
      parse_fatal_error_pid(line);
    }
    return;
  }

}

static void flush_log_line(uint64_t now_us) {
  if (g_mdbg.log_line_length == 0)
    return;

  g_mdbg.log_line[g_mdbg.log_line_length] = '\0';
  process_log_line(g_mdbg.log_line, now_us);
  reset_log_line_buffer();
}

static void append_log_char(char ch, uint64_t now_us) {
  if (!g_mdbg.game.active)
    return;

  if (ch == '\r' || ch == '\n') {
    flush_log_line(now_us);
    return;
  }

  // Truncate an oversized line without interpreting its suffix as a new line.
  if (g_mdbg.log_line_length + 1u >= sizeof(g_mdbg.log_line))
    return;

  g_mdbg.log_line[g_mdbg.log_line_length++] = ch;
}

static void poll_log_monitor(uint64_t now_us) {
  if (!g_mdbg.game.active) {
    return;
  }

  const char *text = NULL;
  size_t text_len = 0;
  int ret = fetch_log_text(g_mdbg.log_storage, g_mdbg.log_buffer_size,
                           &text, &text_len);
  if (ret != 0) {
    log_debug("  [MDBG] log snapshot failed for %s: 0x%08x",
              g_mdbg.game.title_id, ret);
    clear_tracked_game();
    return;
  }

  size_t skip = 0;
  if (text_len >= g_mdbg.log_snapshot_length &&
      (g_mdbg.log_snapshot_length == 0 ||
       !memcmp(g_mdbg.log_snapshot, text, g_mdbg.log_snapshot_length))) {
    skip = g_mdbg.log_snapshot_length;
    if (skip == text_len)
      return;
  } else {
    skip = find_log_overlap(text, text_len);
    if (skip == 0 && g_mdbg.log_snapshot_length != 0) {
      log_debug("  [MDBG] log stream reset or truncated for %s",
                g_mdbg.game.title_id);
      reset_log_line_buffer();
      reset_fatal_error_buffer();
    }
  }

  for (size_t i = skip; i < text_len; ++i) {
    append_log_char(text[i], now_us);
    if (!g_mdbg.game.active)
      return;
  }

  update_log_snapshot(text, text_len);
}

static void drain_log_monitor_before_clear(uint64_t now_us) {
  poll_log_monitor(now_us);
  if (!g_mdbg.game.active)
    return;

  flush_log_line(now_us);
  if (!g_mdbg.game.active)
    return;

  finish_fatal_error(now_us);
}

static bool start_log_monitoring(void) {
  if (!g_mdbg.game.active)
    return false;

  if (!ensure_mdbg_privileges()) {
    return false;
  }

  if (!ensure_log_buffers()) {
    return false;
  }

  const char *text = NULL;
  size_t text_len = 0;
  int ret = fetch_log_text(g_mdbg.log_storage, g_mdbg.log_buffer_size,
                           &text, &text_len);
  if (ret != 0) {
    log_debug("  [MDBG] initial log snapshot failed for %s: 0x%08x",
              g_mdbg.game.title_id, ret);
    reset_log_snapshot();
    return false;
  }

  update_log_snapshot(text, text_len);
  // Complete a line split across the baseline and the next SDK snapshot.
  size_t start = text_len;
  while (start > 0 && text[start - 1u] != '\n' && text[start - 1u] != '\r')
    --start;
  for (size_t i = start; i < text_len; ++i)
    append_log_char(text[i], 0);
  return true;
}

static void set_tracked_game_pid(pid_t pid) {
  g_mdbg.game.pid = pid;
  snprintf(g_mdbg.game.rtld_error_prefix, sizeof(g_mdbg.game.rtld_error_prefix),
           "[rtld] <%ld> ERROR", (long)pid);
}

void sm_mdbg_init(void) {
  memset(&g_mdbg, 0, sizeof(g_mdbg));
}

void sm_mdbg_shutdown(void) {
  clear_tracked_game();
  memset(&g_mdbg, 0, sizeof(g_mdbg));
}

void sm_mdbg_game_on_exec(pid_t pid, const char *title_id,
                          uint64_t exec_time_us) {
  clear_tracked_game();
  uint64_t now_us = monotonic_time_us();
  if (pid <= 0 || !title_id || title_id[0] == '\0' || now_us == 0)
    return;
  uint64_t launch_time_us = exec_time_us != 0 ? exec_time_us : now_us;
  if (launch_time_us > now_us ||
      launch_time_us > UINT64_MAX - MDBG_MONITOR_WINDOW_US)
    return;
  uint64_t deadline_us = launch_time_us + MDBG_MONITOR_WINDOW_US;
  if (now_us >= deadline_us)
    return;

  g_mdbg.game.active = true;
  g_mdbg.game.monitor_deadline_us = deadline_us;
  g_mdbg.game.next_poll_us = now_us + GAME_LIFECYCLE_POLL_INTERVAL_US;
  strlcpy(g_mdbg.game.title_id, title_id, sizeof(g_mdbg.game.title_id));
  set_tracked_game_pid(pid);
  if (!start_log_monitoring()) {
    clear_tracked_game();
    return;
  }
  log_debug("  [MDBG] monitoring game errors: %s pid=%ld for up to 60 seconds",
            title_id, (long)pid);
}

void sm_mdbg_game_handoff(pid_t old_pid, pid_t new_pid) {
  if (!g_mdbg.game.active || g_mdbg.game.pid != old_pid || new_pid <= 0)
    return;
  uint64_t now_us = monotonic_time_us();
  if (now_us == 0 || now_us >= g_mdbg.game.monitor_deadline_us) {
    clear_tracked_game();
    return;
  }
  set_tracked_game_pid(new_pid);
  // The log stream is continuous across PID replacement. Keep partial lines
  // and fatal reports; their PID is checked when the message is complete.
  poll_log_monitor(now_us);
}

void sm_mdbg_game_on_exit(pid_t pid) {
  if (!g_mdbg.game.active || g_mdbg.game.pid != pid)
    return;
  uint64_t now_us = monotonic_time_us();
  if (now_us != 0 && now_us < g_mdbg.game.monitor_deadline_us)
    drain_log_monitor_before_clear(now_us);
  clear_tracked_game();
}

void sm_mdbg_game_shutdown(void) {
  clear_tracked_game();
}

uint64_t sm_mdbg_next_wake_us(void) {
  if (!g_mdbg.game.active)
    return 0;
  return g_mdbg.game.next_poll_us < g_mdbg.game.monitor_deadline_us
             ? g_mdbg.game.next_poll_us
             : g_mdbg.game.monitor_deadline_us;
}

void sm_mdbg_poll(bool process_active) {
  if (!g_mdbg.game.active)
    return;
  uint64_t now_us = monotonic_time_us();
  if (now_us == 0 || now_us >= g_mdbg.game.monitor_deadline_us) {
    log_debug("  [MDBG] game error monitoring expired: %s pid=%ld",
              g_mdbg.game.title_id, (long)g_mdbg.game.pid);
    clear_tracked_game();
    return;
  }
  if (now_us < g_mdbg.game.next_poll_us)
    return;

  poll_log_monitor(now_us);
  if (!g_mdbg.game.active)
    return;
  g_mdbg.game.next_poll_us = now_us + GAME_LIFECYCLE_POLL_INTERVAL_US;
  if (!process_active ||
      (g_mdbg.fatal_error_active &&
       (g_mdbg.fatal_error_pid == 0 || g_mdbg.fatal_error_pid == g_mdbg.game.pid)))
    return;

  uint64_t flags = 0;
  int ret = query_mdbg_flags(g_mdbg.game.pid, &flags);
  if (is_mdbg_process_gone_error(ret)) {
    drain_log_monitor_before_clear(now_us);
    clear_tracked_game();
    return;
  }
  if (ret == 0 && (flags & MDBG_FLAG_EXCEPTION_STOP) != 0)
    handle_game_failure("crash-candidate", now_us, MDBG_FAILURE_GENERIC);
}
