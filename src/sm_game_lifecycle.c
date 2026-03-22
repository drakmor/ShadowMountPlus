#include "sm_platform.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/event.h>

#include "sm_fakelib.h"
#include "sm_game_lifecycle.h"
#include "sm_kstuff.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_runtime.h"
#include "sm_time.h"

#define MAX_PENDING_GAME_EXEC_CANDIDATES 32

typedef struct {
  bool active;
  pid_t pid;
  uint64_t exec_time_us;
  uint64_t deadline_us;
} pending_game_launch_t;

// SysCore can emit multiple child exec events before the actual game title_id
// becomes visible, so keep a small raw exec candidate buffer here.
static pending_game_launch_t
    g_pending_game_launches[MAX_PENDING_GAME_EXEC_CANDIDATES];
static pthread_t g_game_lifecycle_thread;
static bool g_game_lifecycle_thread_started = false;
static volatile sig_atomic_t g_game_lifecycle_stop_requested = 0;
static int g_game_lifecycle_wake_pipe[2] = {-1, -1};
static pthread_mutex_t g_game_lifecycle_start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_game_lifecycle_start_cond = PTHREAD_COND_INITIALIZER;
static bool g_game_lifecycle_start_ready = false;
static bool g_game_lifecycle_start_success = false;

static bool register_game_exit_watch(int kq, pid_t pid);

static bool is_supported_title_id(const char *title_id) {
  return strncmp(title_id, "PPSA", 4) == 0 || strncmp(title_id, "CUSA", 4) == 0;
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

static bool dispatch_game_launch(int kq, pid_t pid, uint64_t exec_time_us,
                                 const char *title_id, uint32_t app_id) {
  if (!register_game_exit_watch(kq, pid)) {
    log_debug("  [GAME] skipping launch tracking for %s pid=%ld without exit watch",
              title_id, (long)pid);
    return false;
  }

  log_debug("  [GAME] started: %s pid=%ld app_id=0x%08X", title_id,
            (long)pid, app_id);
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

static bool set_fd_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return false;

  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
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

static const struct timespec *compute_game_wait_timeout(
    struct timespec *timeout_out) {
  uint64_t now_us = monotonic_time_us();
  uint64_t next_wake_us = 0;

  next_wake_us = min_nonzero_u64(next_wake_us, next_pending_game_wake_us(now_us));
  next_wake_us = min_nonzero_u64(next_wake_us, sm_kstuff_game_next_wake_us(now_us));
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
      if (is_supported_title_id(title_id)) {
        if (dispatch_game_launch(kq, pid, exec_time_us, title_id, app_id)) {
          clear_pending_game_launch(entry);
          clear_all_pending_game_launches();
          break;
        }
        defer_confirmed_game_launch_retry(pid, exec_time_us, now_us, title_id);
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

  sm_kstuff_game_poll();
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
    if (is_supported_title_id(title_id)) {
      if (dispatch_game_launch(kq, pid, now_us, title_id, app_id))
        clear_all_pending_game_launches();
      else
        defer_confirmed_game_launch_retry(pid, now_us, now_us, title_id);
    }
    return;
  }

  if (!is_process_alive(pid))
    return;

  queue_pending_game_launch(pid, now_us, now_us);
}

static void handle_game_exit(pid_t pid) {
  pending_game_launch_t *entry = find_pending_game_launch(pid);
  if (entry)
    clear_pending_game_launch(entry);
  sm_fakelib_game_on_exit(pid);
  sm_kstuff_game_on_exit(pid);
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

  set_game_lifecycle_start_result(true);
  log_debug("  [GAME] lifecycle watcher started");

  while (!g_game_lifecycle_stop_requested && !should_stop_requested()) {
    struct kevent event;
    struct timespec timeout;
    const struct timespec *timeout_ptr = compute_game_wait_timeout(&timeout);
    int nev = kevent(kq, NULL, 0, &event, 1, timeout_ptr);
    if (nev < 0) {
      if (errno == EINTR)
        continue;
      if (g_game_lifecycle_stop_requested)
        break;
      log_debug("  [GAME] kevent wait failed: %s", strerror(errno));
      break;
    }
    if (nev > 0) {
      if (event.filter == EVFILT_READ &&
          event.ident == (uintptr_t)g_game_lifecycle_wake_pipe[0]) {
        char drain_buf[32];
        (void)read(g_game_lifecycle_wake_pipe[0], drain_buf,
                   sizeof(drain_buf));
      } else if ((event.fflags & NOTE_TRACKERR) != 0) {
        log_debug("  [GAME] NOTE_TRACKERR for pid=%ld", (long)event.ident);
      } else {
        if ((event.fflags & NOTE_EXEC) != 0)
          handle_game_exec(kq, (pid_t)event.ident);
        if ((event.fflags & NOTE_EXIT) != 0)
          handle_game_exit((pid_t)event.ident);
      }
    }

    poll_game_modules(kq);
  }

  clear_all_pending_game_launches();
  sm_fakelib_game_shutdown();
  sm_kstuff_game_shutdown();
  close(kq);
  log_debug("  [GAME] lifecycle watcher stopped");
  return NULL;
}

bool start_game_lifecycle_watcher(void) {
  if (!sm_fakelib_game_feature_enabled() && !sm_kstuff_game_feature_enabled())
    return true;
  if (g_game_lifecycle_thread_started)
    return true;
  if (pipe(g_game_lifecycle_wake_pipe) != 0) {
    log_debug("  [GAME] wake pipe creation failed: %s", strerror(errno));
    return false;
  }
  if (!set_fd_nonblocking(g_game_lifecycle_wake_pipe[0]) ||
      !set_fd_nonblocking(g_game_lifecycle_wake_pipe[1])) {
    log_debug("  [GAME] wake pipe nonblocking setup failed: %s",
              strerror(errno));
    close_game_lifecycle_wake_pipe();
    return false;
  }

  g_game_lifecycle_stop_requested = 0;
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

  g_game_lifecycle_stop_requested = 1;
  wake_game_lifecycle_watcher();

  (void)pthread_join(g_game_lifecycle_thread, NULL);
  g_game_lifecycle_thread_started = false;
  g_game_lifecycle_stop_requested = 0;
  close_game_lifecycle_wake_pipe();
}

bool refresh_game_lifecycle_watcher(void) {
  bool should_run =
      sm_fakelib_game_feature_enabled() || sm_kstuff_game_feature_enabled();

  if (!should_run) {
    if (g_game_lifecycle_thread_started)
      stop_game_lifecycle_watcher();
    return true;
  }

  if (!g_game_lifecycle_thread_started)
    return start_game_lifecycle_watcher();

  wake_game_lifecycle_watcher();
  return true;
}
