#include "sm_platform.h"

#include <pthread.h>
#include <stdatomic.h>
#include <sys/event.h>
#include <sys/select.h>

#include "sm_appdb.h"
#include "sm_fan.h"
#include "sm_fakelib.h"
#include "sm_filesystem.h"
#include "sm_game_lifecycle.h"
#include "sm_gameinfo.h"
#include "sm_kstuff.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_mdbg.h"
#include "sm_path_utils.h"
#include "sm_runtime.h"
#include "sm_scan.h"
#include "sm_scanner.h"
#include "sm_shellcore_service.h"
#include "sm_time.h"

#define MAX_PENDING_GAME_EXEC_CANDIDATES 32
#define GAME_SANDBOX_ROOT "/mnt/sandbox"

typedef struct {
  bool active;
  pid_t pid;
  uint64_t exec_time_us;
  uint64_t deadline_us;
} pending_game_launch_t;

typedef struct {
  bool active;
  pid_t pid;
  uint32_t app_id;
  uint64_t exit_time_us;
  char title_id[MAX_TITLE_ID];
} pending_game_exit_t;

// SysCore can emit multiple child exec events before the actual game title_id
// becomes visible, so keep a small raw exec candidate buffer here.
static pending_game_launch_t
    g_pending_game_launches[MAX_PENDING_GAME_EXEC_CANDIDATES];
static pthread_t g_game_lifecycle_thread;
static bool g_game_lifecycle_thread_started = false;
static atomic_bool g_game_lifecycle_stop_requested = false;
static int g_game_lifecycle_wake_pipe[2] = {-1, -1};
static pthread_mutex_t g_game_lifecycle_start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_game_lifecycle_start_cond = PTHREAD_COND_INITIALIZER;
static bool g_game_lifecycle_start_ready = false;
static bool g_game_lifecycle_start_success = false;
static _Atomic pid_t g_active_game_pid = 0;
static _Atomic bool g_game_exit_handoff_pending = false;
static uint32_t g_active_game_app_id = 0;
static pending_game_exit_t g_pending_game_exit;
static _Atomic uint32_t g_pending_app_focus_id;
static _Atomic bool g_pending_app_focus_valid;
// Title storage is owned exclusively by the lifecycle thread. Other threads
// consume only atomically published state through public helpers.
static char g_active_game_title_id[MAX_TITLE_ID];

static bool register_game_exit_watch(int kq, pid_t pid);
static void handle_game_exit(pid_t pid);
static void finalize_pending_game_exit(void);
static void maybe_finalize_pending_game_exit(void);
static bool try_game_process_handoff(int kq, pid_t pid, const char *title_id,
                                     uint32_t app_id);

static void publish_active_game_pid(pid_t pid) {
  pid_t previous_pid = atomic_exchange(&g_active_game_pid, pid);
  if (previous_pid == pid)
    return;
  sm_scanner_wake();
}

static void publish_active_game(pid_t pid, const char *title_id,
                                uint32_t app_id) {
  if (pid > 0 && title_id && title_id[0] != '\0') {
    (void)strlcpy(g_active_game_title_id, title_id,
                  sizeof(g_active_game_title_id));
    g_active_game_app_id = app_id;
  } else {
    g_active_game_title_id[0] = '\0';
    g_active_game_app_id = 0;
  }
  publish_active_game_pid(pid);
}

static bool consume_active_game(pid_t pid, char title_id_out[MAX_TITLE_ID],
                                uint32_t *app_id_out) {
  if (pid <= 0 || atomic_load(&g_active_game_pid) != pid ||
      g_active_game_title_id[0] == '\0')
    return false;
  (void)strlcpy(title_id_out, g_active_game_title_id, MAX_TITLE_ID);
  if (app_id_out)
    *app_id_out = g_active_game_app_id;
  g_active_game_title_id[0] = '\0';
  g_active_game_app_id = 0;
  return true;
}

static void handle_pending_app_focus(void) {
  if (!atomic_exchange(&g_pending_app_focus_valid, false))
    return;

  uint32_t focused_app_id = atomic_load(&g_pending_app_focus_id);
  if (atomic_load(&g_active_game_pid) <= 0 || g_active_game_app_id == 0 ||
      g_active_game_title_id[0] == '\0') {
    atomic_store(&g_pending_app_focus_valid, true);
    return;
  }
  if (focused_app_id == 0 || focused_app_id != g_active_game_app_id)
    return;

  sm_fan_apply_game_target(g_active_game_title_id);
}

static bool snapshot_active_game(pid_t pid, char title_id[MAX_TITLE_ID]) {
  title_id[0] = '\0';
  if (pid <= 0 || atomic_load(&g_active_game_pid) != pid ||
      g_active_game_title_id[0] == '\0')
    return false;
  (void)strlcpy(title_id, g_active_game_title_id, MAX_TITLE_ID);
  return true;
}

static bool title_is_usb_backed(const char *title_id, const char *source_path,
                                bool has_mount_link) {
  char image_path[MAX_PATH];
  if (read_mount_image_link(title_id, image_path, sizeof(image_path)) &&
      is_usb_storage_path(image_path)) {
    return true;
  }
  return has_mount_link && is_usb_storage_path(source_path);
}

static uint64_t min_nonzero_u64(uint64_t a, uint64_t b) {
  if (a == 0)
    return b;
  if (b == 0)
    return a;
  return a < b ? a : b;
}

/*
static void bytes_to_hex(const void *src, size_t src_len, char *dst,
                         size_t dst_size) {
  static const char hex_chars[] = "0123456789abcdef";
  const uint8_t *bytes = (const uint8_t *)src;
  size_t out_pos = 0;

  if (!dst || dst_size == 0)
    return;

  for (size_t i = 0; i < src_len && out_pos + 2 < dst_size; ++i) {
    dst[out_pos++] = hex_chars[(bytes[i] >> 4) & 0x0f];
    dst[out_pos++] = hex_chars[bytes[i] & 0x0f];
  }

  dst[out_pos] = '\0';
}

static void log_app_info_response(pid_t pid, int rc, const app_info_t *appinfo) {
  if (rc != 0) {
    log_debug("  [GAME] sceKernelGetAppInfo pid=%ld failed: rc=0x%08X",
              (long)pid, (unsigned)rc);
    return;
  }

  if (!appinfo)
    return;

  char title_id[MAX_TITLE_ID];
  size_t title_len = strnlen(appinfo->title_id, sizeof(appinfo->title_id));
  if (title_len >= sizeof(title_id))
    title_len = sizeof(title_id) - 1u;
  memcpy(title_id, appinfo->title_id, title_len);
  title_id[title_len] = '\0';

  char unknown2_hex[sizeof(appinfo->unknown2) * 2u + 1u];
  bytes_to_hex(appinfo->unknown2, sizeof(appinfo->unknown2), unknown2_hex,
               sizeof(unknown2_hex));

  log_debug("  [GAME] sceKernelGetAppInfo pid=%ld app_id=0x%08X "
            "unknown1=0x%016llX title_id=\"%s\"",
            (long)pid, appinfo->app_id,
            (unsigned long long)appinfo->unknown1, title_id);
  log_debug("  [GAME] sceKernelGetAppInfo pid=%ld unknown2=%s", (long)pid,
            unknown2_hex);
}
*/

static bool resolve_game_title_id(pid_t pid, char title_id[MAX_TITLE_ID],
                                  uint32_t *app_id_out) {
  app_info_t appinfo;
  memset(&appinfo, 0, sizeof(appinfo));
  int rc = sceKernelGetAppInfo(pid, &appinfo);
  /* log_app_info_response(pid, rc, &appinfo); */
  if (rc != 0)
    return false;

  size_t title_len = strnlen(appinfo.title_id, sizeof(appinfo.title_id));
  if (title_len == 0 || title_len >= MAX_TITLE_ID)
    return false;

  memcpy(title_id, appinfo.title_id, title_len);
  title_id[title_len] = '\0';
  if (app_id_out)
    *app_id_out = appinfo.app_id;
  return true;
}

static bool is_process_alive(pid_t pid) {
  if (kill(pid, 0) == 0)
    return true;

  return errno != ESRCH;
}

static void clear_pending_game_exit(void) {
  memset(&g_pending_game_exit, 0, sizeof(g_pending_game_exit));
  atomic_store(&g_game_exit_handoff_pending, false);
}

static void queue_pending_game_exit(pid_t pid, const char *title_id,
                                    uint32_t app_id) {
  memset(&g_pending_game_exit, 0, sizeof(g_pending_game_exit));
  g_pending_game_exit.active = true;
  g_pending_game_exit.pid = pid;
  g_pending_game_exit.app_id = app_id;
  g_pending_game_exit.exit_time_us = monotonic_time_us();
  (void)strlcpy(g_pending_game_exit.title_id, title_id,
                sizeof(g_pending_game_exit.title_id));
  atomic_store(&g_game_exit_handoff_pending, true);
}

static bool same_application_identity(const char *title_id, uint32_t app_id,
                                      const char *other_title_id,
                                      uint32_t other_app_id) {
  if (!title_id || !other_title_id || strcmp(title_id, other_title_id) != 0)
    return false;
  // app_id should normally be stable across ExitSpawn. Treat zero as unknown so
  // title identity can still recover a handoff while AppInfo is incomplete.
  return app_id == 0 || other_app_id == 0 || app_id == other_app_id;
}

static bool try_game_process_handoff(int kq, pid_t pid, const char *title_id,
                                     uint32_t app_id) {
  if (pid <= 0 || !title_id || title_id[0] == '\0')
    return false;

  pid_t old_pid = 0;
  bool pending_exit = false;
  if (g_pending_game_exit.active &&
      same_application_identity(title_id, app_id,
                                g_pending_game_exit.title_id,
                                g_pending_game_exit.app_id)) {
    old_pid = g_pending_game_exit.pid;
    pending_exit = true;
  } else {
    pid_t active_pid = atomic_load(&g_active_game_pid);
    if (active_pid > 0 && active_pid != pid &&
        same_application_identity(title_id, app_id, g_active_game_title_id,
                                  g_active_game_app_id) &&
        !is_process_alive(active_pid)) {
      // NOTE_EXEC can race ahead of the old NOTE_EXIT. A dead old process with
      // the same title/app identity is the same ExitSpawn handoff.
      old_pid = active_pid;
    }
  }

  if (old_pid <= 0 || old_pid == pid)
    return false;
  if (!register_game_exit_watch(kq, pid)) {
    log_debug("  [GAME] process handoff exit watch failed: %s pid=%ld -> %ld",
              title_id, (long)old_pid, (long)pid);
    return false;
  }

  // Keep the ShellCore runtime across ExitSpawn/LoadExec, but deliberately
  // restart the per-process fakelib overlay. Holding the old nullfs overlay
  // can keep the old title sandbox busy and prevent ShellCore from removing
  // it. sm_fakelib_game_on_exec() also handles the NOTE_EXEC-before-NOTE_EXIT
  // race by cleaning any mount still owned by old_pid before mounting for pid.
  sm_shellcore_service_bind_prepared_app(title_id, app_id, pid);
  sm_fakelib_game_on_exec(pid, title_id);
  bool kstuff_rebound =
      sm_kstuff_game_handoff(old_pid, pid, title_id, app_id);
  publish_active_game(pid, title_id, app_id);
  if (pending_exit)
    clear_pending_game_exit();

  log_debug("  [GAME] process handoff: %s app_id=0x%08X pid=%ld -> pid=%ld "
            "fakelib=remounted kstuff=%s",
            title_id, app_id, (long)old_pid, (long)pid,
            kstuff_rebound ? "rebound" : "inactive");
  return true;
}

static bool dispatch_game_launch(int kq, pid_t pid, uint64_t exec_time_us,
                                 const char *title_id, uint32_t app_id) {
  if (try_game_process_handoff(kq, pid, title_id, app_id))
    return true;

  if (!sm_shellcore_ensure_title_runtime(title_id)) {
    return false;
  }
  sm_shellcore_service_bind_prepared_app(title_id, app_id, pid);

  if (!register_game_exit_watch(kq, pid)) {
    log_debug("  [GAME] skipping launch tracking for %s pid=%ld without exit watch",
              title_id, (long)pid);
    if (sm_shellcore_service_title_is_prepared(title_id) &&
        !sm_shellcore_release_title_runtime(title_id)) {
      log_debug("  [SHELLCORE] failed to roll back untracked runtime: %s",
                title_id);
    }
    return false;
  }

  log_debug("  [GAME] started: %s pid=%ld app_id=0x%08X", title_id,
            (long)pid, app_id);
  publish_active_game(pid, title_id, app_id);
  sm_kstuff_game_on_exec(pid, title_id, app_id, exec_time_us);
  sm_fakelib_game_on_exec(pid, title_id);
  return true;
}

static void clear_pending_game_launch(pending_game_launch_t *entry) {
  memset(entry, 0, sizeof(*entry));
}

static void clear_all_pending_game_launches(void) {
  memset(g_pending_game_launches, 0, sizeof(g_pending_game_launches));
}

static void close_game_lifecycle_wake_pipe(void) {
  if (g_game_lifecycle_wake_pipe[0] >= 0) {
    close(g_game_lifecycle_wake_pipe[0]);
    g_game_lifecycle_wake_pipe[0] = -1;
  }
  if (g_game_lifecycle_wake_pipe[1] >= 0) {
    close(g_game_lifecycle_wake_pipe[1]);
    g_game_lifecycle_wake_pipe[1] = -1;
  }
}

static int register_game_sandbox_watch(int kq) {
  int fd = open(GAME_SANDBOX_ROOT, O_RDONLY);
  if (fd < 0) {
    log_debug("  [GAME] sandbox watcher open failed: %s", strerror(errno));
    return -1;
  }

  struct kevent kev;
  EV_SET(&kev, (uintptr_t)fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
         NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE,
         0, NULL);
  if (kevent(kq, &kev, 1, NULL, 0, NULL) != 0) {
    log_debug("  [GAME] sandbox watcher registration failed: %s",
              strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

static void drain_game_lifecycle_wake_pipe(void) {
  char drain_buf[32];
  while (read(g_game_lifecycle_wake_pipe[0], drain_buf, sizeof(drain_buf)) > 0) {
  }
}

static bool drain_game_lifecycle_events_nowait(int kq) {
  struct kevent events[16];
  struct timespec timeout;
  memset(&timeout, 0, sizeof(timeout));

  while (true) {
    int nev = kevent(kq, NULL, 0, events, sizeof(events) / sizeof(events[0]),
                     &timeout);
    if (nev < 0) {
      if (errno == EINTR)
        continue;
      log_debug("  [GAME] stale event drain failed: %s", strerror(errno));
      return false;
    }
    if (nev == 0)
      return true;
    for (int i = 0; i < nev; ++i) {
      if (events[i].filter == EVFILT_PROC &&
          (events[i].fflags & NOTE_EXIT) != 0) {
        handle_game_exit((pid_t)events[i].ident);
      }
    }
    (void)sm_shellcore_service_release_exited_titles();
  }
}

static void set_game_lifecycle_start_result(bool success) {
  pthread_mutex_lock(&g_game_lifecycle_start_mutex);
  g_game_lifecycle_start_success = success;
  g_game_lifecycle_start_ready = true;
  pthread_cond_broadcast(&g_game_lifecycle_start_cond);
  pthread_mutex_unlock(&g_game_lifecycle_start_mutex);
}

static pending_game_launch_t *find_pending_game_launch(pid_t pid) {
  for (size_t i = 0; i < MAX_PENDING_GAME_EXEC_CANDIDATES; ++i) {
    pending_game_launch_t *entry = &g_pending_game_launches[i];
    if (entry->active && entry->pid == pid)
      return entry;
  }

  return NULL;
}

static pending_game_launch_t *reserve_pending_game_launch(uint64_t now_us) {
  pending_game_launch_t *oldest_entry = NULL;

  for (size_t i = 0; i < MAX_PENDING_GAME_EXEC_CANDIDATES; ++i) {
    pending_game_launch_t *entry = &g_pending_game_launches[i];
    if (!entry->active)
      return entry;

    if (now_us != 0 && entry->deadline_us != 0 && now_us >= entry->deadline_us)
      return entry;

    if (!oldest_entry ||
        (entry->deadline_us != 0 &&
         (oldest_entry->deadline_us == 0 ||
          entry->deadline_us < oldest_entry->deadline_us))) {
      oldest_entry = entry;
    }
  }

  return oldest_entry;
}

static pending_game_launch_t *queue_pending_game_launch(pid_t pid,
                                                        uint64_t exec_time_us,
                                                        uint64_t now_us) {
  pending_game_launch_t *entry = find_pending_game_launch(pid);
  if (!entry)
    entry = reserve_pending_game_launch(now_us);

  if (entry->active && entry->pid != pid) {
    log_debug("  [GAME] replacing pending exec pid=%ld with pid=%ld",
              (long)entry->pid, (long)pid);
  }

  uint64_t base_time_us = exec_time_us != 0 ? exec_time_us : now_us;
  entry->active = true;
  entry->pid = pid;
  entry->exec_time_us = base_time_us;
  entry->deadline_us =
      base_time_us == 0 ? 0 : base_time_us + GAME_APPINFO_LOOKUP_TIMEOUT_US;
  return entry;
}

static void defer_confirmed_game_launch_retry(pid_t pid, uint64_t exec_time_us,
                                              uint64_t now_us,
                                              const char *title_id) {
  if (now_us == 0) {
    log_debug("  [GAME] cannot schedule launch retry without monotonic clock: "
              "%s pid=%ld",
              title_id, (long)pid);
    return;
  }
  clear_all_pending_game_launches();
  queue_pending_game_launch(pid, exec_time_us, now_us);

  log_debug("  [GAME] deferring launch retry for %s pid=%ld", title_id,
            (long)pid);
}

static uint64_t next_pending_game_wake_us(uint64_t now_us) {
  uint64_t next_wake_us = 0;

  for (size_t i = 0; i < MAX_PENDING_GAME_EXEC_CANDIDATES; ++i) {
    pending_game_launch_t *entry = &g_pending_game_launches[i];
    if (!entry->active)
      continue;

    if (now_us == 0) {
      next_wake_us = min_nonzero_u64(next_wake_us, 1);
      continue;
    }

    uint64_t poll_wake_us = now_us + GAME_LIFECYCLE_POLL_INTERVAL_US;
    uint64_t entry_wake_us = min_nonzero_u64(entry->deadline_us, poll_wake_us);
    next_wake_us = min_nonzero_u64(next_wake_us, entry_wake_us);
  }

  return next_wake_us;
}

static uint64_t next_pending_exit_wake_us(uint64_t now_us) {
  if (!g_pending_game_exit.active)
    return 0;
  if (now_us == 0)
    return 1;
  return now_us + GAME_LIFECYCLE_POLL_INTERVAL_US;
}

static const struct timespec *compute_game_wait_timeout(
    struct timespec *timeout_out) {
  uint64_t now_us = monotonic_time_us();
  uint64_t next_wake_us = 0;

  next_wake_us = min_nonzero_u64(next_wake_us, next_pending_game_wake_us(now_us));
  next_wake_us = min_nonzero_u64(next_wake_us, next_pending_exit_wake_us(now_us));
  if (!g_pending_game_exit.active) {
    next_wake_us =
        min_nonzero_u64(next_wake_us, sm_kstuff_game_next_wake_us(now_us));
    next_wake_us =
        min_nonzero_u64(next_wake_us, sm_mdbg_next_wake_us(now_us));
  }
  if (next_wake_us == 0)
    return NULL;

  uint64_t wait_us = GAME_LIFECYCLE_POLL_INTERVAL_US;
  if (now_us != 0 && next_wake_us > now_us)
    wait_us = next_wake_us - now_us;
  else if (now_us != 0)
    wait_us = 0;

  timeout_out->tv_sec = (time_t)(wait_us / 1000000ull);
  timeout_out->tv_nsec = (long)((wait_us % 1000000ull) * 1000ull);
  return timeout_out;
}

static void poll_game_modules(int kq) {
  uint64_t now_us = monotonic_time_us();
  for (size_t i = 0; i < MAX_PENDING_GAME_EXEC_CANDIDATES; ++i) {
    pending_game_launch_t *entry = &g_pending_game_launches[i];
    if (!entry->active)
      continue;

    char title_id[MAX_TITLE_ID];
    uint32_t app_id = 0;
    if (resolve_game_title_id(entry->pid, title_id, &app_id)) {
      pid_t pid = entry->pid;
      uint64_t exec_time_us = entry->exec_time_us;
      if (is_supported_game_title_id(title_id)) {
        if (entry->deadline_us == 0 || now_us == 0 ||
            now_us >= entry->deadline_us) {
          log_debug("  [GAME] runtime preparation timed out for %s pid=%ld",
                    title_id, (long)pid);
          clear_pending_game_launch(entry);
          break;
        }
        if (dispatch_game_launch(kq, pid, exec_time_us, title_id, app_id)) {
          clear_pending_game_launch(entry);
          clear_all_pending_game_launches();
          break;
        }
        uint64_t retry_now_us = monotonic_time_us();
        if (retry_now_us == 0 || retry_now_us >= entry->deadline_us) {
          log_debug("  [GAME] runtime preparation timed out for %s pid=%ld",
                    title_id, (long)pid);
          clear_pending_game_launch(entry);
        } else {
          defer_confirmed_game_launch_retry(pid, exec_time_us, retry_now_us,
                                            title_id);
        }
        break;
      } else {
        clear_pending_game_launch(entry);
      }
    } else if (!is_process_alive(entry->pid)) {
      clear_pending_game_launch(entry);
    }

    if (entry->active && now_us != 0 && entry->deadline_us != 0 &&
        now_us >= entry->deadline_us) {
      log_debug("  [GAME] title_id was not available within %uus for pid=%ld",
                (unsigned)GAME_APPINFO_LOOKUP_TIMEOUT_US, (long)entry->pid);
      clear_pending_game_launch(entry);
    }
  }

  maybe_finalize_pending_game_exit();
  if (g_pending_game_exit.active)
    return;

  handle_pending_app_focus();
  sm_kstuff_game_poll();
  sm_mdbg_poll();
}

static bool register_game_exit_watch(int kq, pid_t pid) {
  struct kevent kev;
  EV_SET(&kev, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_CLEAR,
         NOTE_EXIT, 0, NULL);
  if (kevent(kq, &kev, 1, NULL, 0, NULL) != 0) {
    log_debug("  [GAME] failed to register exit watch for pid=%ld: %s",
              (long)pid, strerror(errno));
    return false;
  }
  return true;
}

static void handle_game_exec(int kq, pid_t pid) {
  if (find_pending_game_launch(pid))
    return;

  uint64_t now_us = monotonic_time_us();
  char title_id[MAX_TITLE_ID];
  uint32_t app_id = 0;
  if (resolve_game_title_id(pid, title_id, &app_id)) {
    if (is_supported_game_title_id(title_id)) {
      if (dispatch_game_launch(kq, pid, now_us, title_id, app_id))
        clear_all_pending_game_launches();
      else
        defer_confirmed_game_launch_retry(pid, now_us, now_us, title_id);
    }
    return;
  }

  if (!is_process_alive(pid))
    return;

  if (now_us != 0)
    queue_pending_game_launch(pid, now_us, now_us);
}

static void finalize_game_exit(pid_t pid, const char *fallback_title_id) {
  char owned_title_id[MAX_TITLE_ID] = {0};
  bool owned_exit =
      sm_shellcore_service_note_game_exit(pid, owned_title_id);
  sm_fakelib_game_on_exit(pid);
  sm_kstuff_game_on_exit(pid);
  if (owned_exit) {
    log_debug("  [SHELLCORE] runtime release awaiting sandbox removal: %s",
              owned_title_id);
  }

  const char *exited_title_id =
      owned_exit ? owned_title_id : fallback_title_id;
  if (exited_title_id && exited_title_id[0] != '\0') {
    int snd0_updates = normalize_snd0info_for_title(exited_title_id);
    if (snd0_updates >= 0)
      log_debug("  [DB] snd0info normalized after game exit rows=%d title=%s",
                snd0_updates, exited_title_id);
  }
}

static void finalize_pending_game_exit(void) {
  if (!g_pending_game_exit.active)
    return;

  pending_game_exit_t exited = g_pending_game_exit;
  clear_pending_game_exit();
  log_debug("  [GAME] finalizing stopped application: %s pid=%ld",
            exited.title_id, (long)exited.pid);
  finalize_game_exit(exited.pid, exited.title_id);
}

static void maybe_finalize_pending_game_exit(void) {
  if (!g_pending_game_exit.active)
    return;

  pid_t active_pid = atomic_load(&g_active_game_pid);
  if (active_pid > 0) {
    // A different application has already taken over. This cannot be a
    // same-app process handoff, so the old process may be finalized now.
    if (!same_application_identity(g_active_game_title_id,
                                   g_active_game_app_id,
                                   g_pending_game_exit.title_id,
                                   g_pending_game_exit.app_id)) {
      finalize_pending_game_exit();
    }
    return;
  }

  // ExitSpawn keeps the same title sandbox alive. A real application exit
  // removes it; wait for that signal instead of equating NOTE_EXIT with app
  // termination. The grace period protects against a brief remove/recreate
  // transition while the replacement process is being published.
  if (sm_shellcore_service_title_sandbox_exists(
          g_pending_game_exit.title_id)) {
    return;
  }

  uint64_t now_us = monotonic_time_us();
  if (g_pending_game_exit.exit_time_us != 0 && now_us != 0 &&
      now_us - g_pending_game_exit.exit_time_us <
          GAME_PROCESS_HANDOFF_GRACE_US) {
    return;
  }

  finalize_pending_game_exit();
}

static void handle_game_exit(pid_t pid) {
  pending_game_launch_t *entry = find_pending_game_launch(pid);
  if (entry)
    clear_pending_game_launch(entry);

  // Duplicate NOTE_EXIT after an active process was already moved into the
  // handoff window must not turn the application into exit-pending.
  if (g_pending_game_exit.active && g_pending_game_exit.pid == pid)
    return;

  char active_title_id[MAX_TITLE_ID] = {0};
  uint32_t active_app_id = 0;
  bool had_active_title =
      consume_active_game(pid, active_title_id, &active_app_id);
  if (had_active_title) {
    if (g_pending_game_exit.active)
      finalize_pending_game_exit();
    // Publish the pending flag before clearing the active PID so scanner/API
    // threads never observe an artificial "no game" gap during the handoff.
    queue_pending_game_exit(pid, active_title_id, active_app_id);
    publish_active_game_pid(0);

    // Match the 1.6 fakelib lifetime: release the overlay as soon as this PID
    // exits. The nullfs mount otherwise holds common/lib inside the old sandbox
    // busy and can prevent that sandbox from being removed during ExitSpawn.
    // ShellCore/KStuff ownership remains pending and may still be handed to a
    // replacement PID of the same title/app.
    sm_fakelib_game_on_exit(pid);

    log_debug("  [GAME] process exited, awaiting same-app handoff: %s "
              "pid=%ld app_id=0x%08X",
              active_title_id, (long)pid, active_app_id);
    return;
  }

  // Non-active exits (for example an outgoing title during a normal game
  // switch) retain the existing immediate per-process cleanup behavior.
  finalize_game_exit(pid, NULL);
}

static bool terminate_usb_game_for_sleep(int kq, pid_t pid,
                                         const char *title_id) {
  if (kill(pid, SIGKILL) != 0) {
    if (errno == ESRCH) {
      handle_game_exit(pid);
      if (g_pending_game_exit.active && g_pending_game_exit.pid == pid)
        finalize_pending_game_exit();
      return true;
    }
    log_debug("  [SLEEP] failed to kill USB game: %s pid=%ld: %s", title_id,
              (long)pid, strerror(errno));
    return false;
  }
  log_debug("  [SLEEP] killing USB game before suspend: %s pid=%ld", title_id,
            (long)pid);

  uint64_t now_us = monotonic_time_us();
  uint64_t deadline_us =
      now_us == 0 ? 0 : now_us + GAME_SLEEP_EXIT_TIMEOUT_US;
  while (!atomic_load_explicit(&g_game_lifecycle_stop_requested,
                               memory_order_relaxed) &&
         !should_stop_requested()) {
    struct kevent event;
    struct timespec timeout = {
        .tv_sec = 0,
        .tv_nsec = (long)GAME_LIFECYCLE_POLL_INTERVAL_US * 1000L,
    };
    int nev = kevent(kq, NULL, 0, &event, 1, &timeout);
    if (nev < 0) {
      if (errno == EINTR)
        continue;
      log_debug("  [SLEEP] NOTE_EXIT wait failed for %s pid=%ld: %s",
                title_id, (long)pid, strerror(errno));
      return false;
    }
    if (nev > 0) {
      if (event.filter == EVFILT_READ &&
          event.ident == (uintptr_t)g_game_lifecycle_wake_pipe[0]) {
        drain_game_lifecycle_wake_pipe();
      } else if (event.filter == EVFILT_PROC &&
                 (event.fflags & NOTE_EXIT) != 0) {
        pid_t exited_pid = (pid_t)event.ident;
        handle_game_exit(exited_pid);
        if (exited_pid == pid) {
          if (g_pending_game_exit.active && g_pending_game_exit.pid == pid)
            finalize_pending_game_exit();
          log_debug("  [SLEEP] USB game stopped: %s pid=%ld", title_id,
                    (long)pid);
          return true;
        }
      }
    }

    now_us = monotonic_time_us();
    if (deadline_us != 0 && now_us != 0 && now_us >= deadline_us) {
      if (!is_process_alive(pid)) {
        handle_game_exit(pid);
        if (g_pending_game_exit.active && g_pending_game_exit.pid == pid)
          finalize_pending_game_exit();
        log_debug("  [SLEEP] USB game exited without NOTE_EXIT: %s pid=%ld",
                  title_id, (long)pid);
        return true;
      }
      log_debug("  [SLEEP] timed out waiting NOTE_EXIT: %s pid=%ld", title_id,
                (long)pid);
      return false;
    }
  }
  return false;
}

static bool wait_for_shellcore_exit_cleanup_for_sleep(int kq,
                                                       bool require_no_owner) {
  uint64_t now_us = monotonic_time_us();
  uint64_t deadline_us =
      now_us == 0 ? 0 : now_us + GAME_SLEEP_EXIT_TIMEOUT_US;
  while (!atomic_load_explicit(&g_game_lifecycle_stop_requested,
                               memory_order_relaxed) &&
         !should_stop_requested()) {
    int release_status = sm_shellcore_service_release_exited_titles();
    if (release_status == 0 &&
        (!require_no_owner || !sm_shellcore_service_has_prepared_mount())) {
      return true;
    }
    if (release_status != 0 && release_status != EAGAIN) {
      log_debug("  [SLEEP] runtime release failed: %s",
                strerror(release_status));
      return false;
    }

    struct kevent event;
    struct timespec timeout = {
        .tv_sec = 0,
        .tv_nsec = (long)GAME_LIFECYCLE_POLL_INTERVAL_US * 1000L,
    };
    int nev = kevent(kq, NULL, 0, &event, 1, &timeout);
    if (nev < 0) {
      if (errno == EINTR)
        continue;
      log_debug("  [SLEEP] sandbox cleanup wait failed: %s", strerror(errno));
      return false;
    }
    if (nev > 0) {
      if (event.filter == EVFILT_READ &&
          event.ident == (uintptr_t)g_game_lifecycle_wake_pipe[0]) {
        drain_game_lifecycle_wake_pipe();
      } else if (event.filter == EVFILT_PROC &&
                 (event.fflags & NOTE_EXIT) != 0) {
        handle_game_exit((pid_t)event.ident);
      }
    }

    now_us = monotonic_time_us();
    if (deadline_us != 0 && now_us != 0 && now_us >= deadline_us) {
      log_debug("  [SLEEP] timed out waiting for sandbox cleanup");
      return false;
    }
  }
  return false;
}

static void *game_lifecycle_watcher_main(void *arg) {
  (void)arg;

  pid_t syscore_pid = find_pid_by_name("SceSysCore.elf", false);
  if (syscore_pid <= 0) {
    log_debug("  [GAME] failed to find SceSysCore.elf");
    set_game_lifecycle_start_result(false);
    return NULL;
  }

  int kq = kqueue();
  if (kq < 0) {
    log_debug("  [GAME] kqueue failed: %s", strerror(errno));
    set_game_lifecycle_start_result(false);
    return NULL;
  }

  struct kevent kev;
  EV_SET(&kev, (uintptr_t)syscore_pid, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_CLEAR,
         NOTE_FORK | NOTE_EXEC | NOTE_TRACK, 0, NULL);
  if (kevent(kq, &kev, 1, NULL, 0, NULL) != 0) {
    log_debug("  [GAME] proc watch registration failed: %s",
              strerror(errno));
    close(kq);
    set_game_lifecycle_start_result(false);
    return NULL;
  }

  EV_SET(&kev, (uintptr_t)g_game_lifecycle_wake_pipe[0], EVFILT_READ,
         EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, NULL);
  if (kevent(kq, &kev, 1, NULL, 0, NULL) != 0) {
    log_debug("  [GAME] wake pipe registration failed: %s", strerror(errno));
    close(kq);
    set_game_lifecycle_start_result(false);
    return NULL;
  }

  int sandbox_watch_fd = register_game_sandbox_watch(kq);

  set_game_lifecycle_start_result(true);
  log_debug("  [GAME] lifecycle watcher started");

  bool sleep_cleanup_done = false;
  pid_t suspended_game_pid = 0;
  while (!atomic_load_explicit(&g_game_lifecycle_stop_requested,
                               memory_order_relaxed) &&
         !should_stop_requested()) {
    if (runtime_sleep_mode_active()) {
      if (!sleep_cleanup_done && sm_scanner_usb_watches_suspended()) {
        suspended_game_pid = atomic_load(&g_active_game_pid);
        char suspended_game_title_id[MAX_TITLE_ID];
        bool active_game = snapshot_active_game(suspended_game_pid,
                                                suspended_game_title_id);
        char source_path[MAX_PATH];
        bool has_mount_link =
            active_game && read_mount_link(suspended_game_title_id, source_path,
                                           sizeof(source_path));
        bool usb_game =
            active_game && title_is_usb_backed(
                               suspended_game_title_id, source_path,
                               has_mount_link);
        clear_all_pending_game_launches();
        sm_fakelib_game_shutdown();
        sm_kstuff_sleep_enter();
        bool usb_cleanup_allowed = true;
        if (usb_game) {
          usb_cleanup_allowed = terminate_usb_game_for_sleep(
              kq, suspended_game_pid, suspended_game_title_id);
          if (usb_cleanup_allowed) {
            suspended_game_pid = 0;
          }
        }
        if (usb_cleanup_allowed) {
          usb_cleanup_allowed = wait_for_shellcore_exit_cleanup_for_sleep(
              kq, usb_game);
        }
        if (usb_cleanup_allowed) {
          runtime_mount_state_lock();
          unmount_usb_sources_for_suspend();
          runtime_mount_state_unlock();
        } else {
          log_debug("[SLEEP] USB cleanup skipped: game/sandbox cleanup "
                    "incomplete");
        }
        // Keep a live non-USB game published while it is suspended so scanner
        // work cannot race its unchanged runtime mount.
        sleep_cleanup_done = true;
      }

      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(g_game_lifecycle_wake_pipe[0], &readfds);
      int rc = select(g_game_lifecycle_wake_pipe[0] + 1, &readfds, NULL, NULL,
                      NULL);
      if (rc < 0) {
        if (errno == EINTR)
          continue;
        log_debug("  [GAME] sleep wait failed: %s", strerror(errno));
        break;
      }
      drain_game_lifecycle_wake_pipe();
      continue;
    }
    if (sleep_cleanup_done) {
      if (!drain_game_lifecycle_events_nowait(kq))
        break;
      sleep_cleanup_done = false;
      if (runtime_sleep_mode_active())
        continue;
      if (suspended_game_pid > 0 &&
          atomic_load(&g_active_game_pid) == suspended_game_pid &&
          is_process_alive(suspended_game_pid)) {
        handle_game_exec(kq, suspended_game_pid);
      }
      sm_kstuff_sleep_leave();
      suspended_game_pid = 0;
    }

    struct kevent event;
    struct timespec timeout;
    const struct timespec *timeout_ptr = compute_game_wait_timeout(&timeout);
    int nev = kevent(kq, NULL, 0, &event, 1, timeout_ptr);
    if (nev < 0) {
      if (errno == EINTR)
        continue;
      if (atomic_load_explicit(&g_game_lifecycle_stop_requested,
                               memory_order_relaxed))
        break;
      log_debug("  [GAME] kevent wait failed: %s", strerror(errno));
      break;
    }
    if (nev > 0) {
      if (event.filter == EVFILT_READ &&
          event.ident == (uintptr_t)g_game_lifecycle_wake_pipe[0]) {
        drain_game_lifecycle_wake_pipe();
      } else if (event.filter == EVFILT_PROC &&
                 (event.fflags & NOTE_TRACKERR) != 0) {
        log_debug("  [GAME] NOTE_TRACKERR for pid=%ld", (long)event.ident);
      } else if (event.filter == EVFILT_PROC) {
        if ((event.fflags & NOTE_EXEC) != 0)
          handle_game_exec(kq, (pid_t)event.ident);
        if ((event.fflags & NOTE_EXIT) != 0)
          handle_game_exit((pid_t)event.ident);
      }
    }

    if (runtime_sleep_mode_active())
      continue;
    poll_game_modules(kq);
    (void)sm_shellcore_service_release_exited_titles();
  }

  clear_all_pending_game_launches();
  clear_pending_game_exit();
  sm_fakelib_game_shutdown();
  sm_kstuff_game_shutdown();
  publish_active_game(0, NULL, 0);
  if (sandbox_watch_fd >= 0)
    close(sandbox_watch_fd);
  close(kq);
  log_debug("  [GAME] lifecycle watcher stopped");
  return NULL;
}

bool start_game_lifecycle_watcher(void) {
  if (g_game_lifecycle_thread_started)
    return true;
  if (pipe(g_game_lifecycle_wake_pipe) != 0) {
    log_debug("  [GAME] wake pipe creation failed: %s", strerror(errno));
    return false;
  }
  if (!sm_set_fd_nonblocking(g_game_lifecycle_wake_pipe[0]) ||
      !sm_set_fd_nonblocking(g_game_lifecycle_wake_pipe[1])) {
    log_debug("  [GAME] wake pipe nonblocking setup failed: %s",
              strerror(errno));
    close_game_lifecycle_wake_pipe();
    return false;
  }

  atomic_store_explicit(&g_game_lifecycle_stop_requested, false,
                        memory_order_relaxed);
  atomic_store(&g_pending_app_focus_id, 0);
  atomic_store(&g_pending_app_focus_valid, false);
  clear_pending_game_exit();
  pthread_mutex_lock(&g_game_lifecycle_start_mutex);
  g_game_lifecycle_start_ready = false;
  g_game_lifecycle_start_success = false;
  pthread_mutex_unlock(&g_game_lifecycle_start_mutex);

  int rc =
      pthread_create(&g_game_lifecycle_thread, NULL, game_lifecycle_watcher_main,
                     NULL);
  if (rc != 0) {
    log_debug("  [GAME] watcher start failed: %s", strerror(rc));
    close_game_lifecycle_wake_pipe();
    return false;
  }

  pthread_mutex_lock(&g_game_lifecycle_start_mutex);
  while (!g_game_lifecycle_start_ready)
    pthread_cond_wait(&g_game_lifecycle_start_cond, &g_game_lifecycle_start_mutex);
  bool start_success = g_game_lifecycle_start_success;
  pthread_mutex_unlock(&g_game_lifecycle_start_mutex);

  if (!start_success) {
    (void)pthread_join(g_game_lifecycle_thread, NULL);
    close_game_lifecycle_wake_pipe();
    return false;
  }

  g_game_lifecycle_thread_started = true;
  return true;
}

void wake_game_lifecycle_watcher(void) {
  if (g_game_lifecycle_wake_pipe[1] < 0)
    return;

  char wake = 'x';
  ssize_t written = write(g_game_lifecycle_wake_pipe[1], &wake, sizeof(wake));
  if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    log_debug("  [GAME] wake pipe write failed: %s", strerror(errno));
  }
}

void stop_game_lifecycle_watcher(void) {
  if (!g_game_lifecycle_thread_started)
    return;

  atomic_store_explicit(&g_game_lifecycle_stop_requested, true,
                        memory_order_relaxed);
  wake_game_lifecycle_watcher();

  (void)pthread_join(g_game_lifecycle_thread, NULL);
  g_game_lifecycle_thread_started = false;
  atomic_store_explicit(&g_game_lifecycle_stop_requested, false,
                        memory_order_relaxed);
  atomic_store(&g_pending_app_focus_id, 0);
  atomic_store(&g_pending_app_focus_valid, false);
  publish_active_game(0, NULL, 0);
  close_game_lifecycle_wake_pipe();
}

bool sm_game_lifecycle_has_active_game(void) {
  return atomic_load(&g_active_game_pid) > 0 ||
         atomic_load(&g_game_exit_handoff_pending);
}

void sm_game_lifecycle_note_app_focus(uint32_t app_id) {
  atomic_store(&g_pending_app_focus_id, app_id);
  atomic_store(&g_pending_app_focus_valid, true);
  sm_kstuff_note_app_focus(app_id);
  wake_game_lifecycle_watcher();
}

bool refresh_game_lifecycle_watcher(void) {
  if (!g_game_lifecycle_thread_started)
    return start_game_lifecycle_watcher();

  wake_game_lifecycle_watcher();
  return true;
}
