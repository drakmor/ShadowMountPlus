#include "sm_platform.h"

#include <pthread.h>
#include <stdatomic.h>
#include <sys/sysctl.h>

#include "sm_runtime.h"
#include "sm_ampr_updater.h"
#include "sm_api_service.h"
#include "sm_types.h"
#include "sm_log.h"
#include "sm_shellcore_flags.h"
#include "sm_shellcore_hooks.h"
#include "sm_shellcore_service.h"
#include "sm_config_mount.h"
#include "sm_game_lifecycle.h"
#include "sm_kstuff.h"
#include "sm_mount_device.h"
#include "sm_filesystem.h"
#include "sm_image.h"
#include "sm_path_utils.h"
#include "sm_scan.h"
#include "sm_scanner.h"
#include "sm_time.h"
#include "sm_install.h"
#include "sm_appdb.h"
#include "sm_limits.h"
#include "sm_mdbg.h"
#include "sm_paths.h"

#ifndef SHADOWMOUNT_VERSION
#define SHADOWMOUNT_VERSION "unknown"
#endif

#ifndef SHADOWMOUNT_BUILD_TIME
#define SHADOWMOUNT_BUILD_TIME __DATE__ " " __TIME__
#endif

#define PAYLOAD_NAME "shadowmountplus.elf"
#define BACKPORK_PROCESS_NAME "backpork.elf"
#define BACKPORK_PROCESS_NAME_ALT "ps5-backpork.elf"
#define RESTART_WAIT_POLL_US 200000u
#define RESTART_WAIT_MAX_US 60000000u
#define STOP_FILE_POLL_INTERVAL_US 3000000ull
#define KINFO_PID_OFFSET 72
#define KINFO_TDNAME_OFFSET 447

//#define AUTHID_BASE 0x4801000000000013L
#define AUTHID_BASE 0x4800000000000006ull


static volatile sig_atomic_t g_stop_requested = 0;
static atomic_bool g_shutdown_on_going_stop_requested = false;
static atomic_bool g_runtime_sleep_mode_active = false;
static atomic_uint_fast64_t g_runtime_resume_grace_deadline_us = 0;
static _Atomic(uintptr_t) g_shutdown_stop_reason_bits = 0;
static atomic_uint_fast64_t g_next_stop_file_poll_us = 0;
static pthread_mutex_t g_runtime_mount_state_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
  pthread_mutex_t reason_mutex;
  char reason[128];
  bool reset_attempts;
} immediate_scan_request_t;

static immediate_scan_request_t g_scan_now = {
    .reason_mutex = PTHREAD_MUTEX_INITIALIZER,
    .reason = {0},
};

extern unsigned char config_ini_example[];
extern unsigned int config_ini_example_len;

static void on_signal(int sig) {
  (void)sig;
  g_stop_requested = 1;
  sm_scanner_wake();
}

void install_signal_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGQUIT, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, NULL);
}

bool should_stop_requested(void) {
  if (g_stop_requested)
    return true;

  uint64_t now_us = monotonic_time_us();
  if (now_us != 0) {
    uint64_t next_poll_us =
        atomic_load_explicit(&g_next_stop_file_poll_us, memory_order_acquire);
    if (next_poll_us != 0 && now_us < next_poll_us)
      return false;
    atomic_store_explicit(&g_next_stop_file_poll_us,
                          now_us + STOP_FILE_POLL_INTERVAL_US,
                          memory_order_release);
  }

  if (remove(KILL_FILE) == 0) {
    g_stop_requested = 1;
    return true;
  }
  return false;
}

void request_shutdown_stop(const char *reason) {
  const char *resolved_reason =
      (reason && reason[0] != '\0') ? reason : "unknown shutdown source";
  static char g_shutdown_stop_reason[128];
  bool already_requested =
      atomic_exchange_explicit(&g_shutdown_on_going_stop_requested, true,
                               memory_order_acq_rel);
  if (!already_requested) {
    (void)strlcpy(g_shutdown_stop_reason, resolved_reason,
                  sizeof(g_shutdown_stop_reason));
    atomic_store_explicit(&g_shutdown_stop_reason_bits,
                          (uintptr_t)g_shutdown_stop_reason,
                          memory_order_release);
    log_debug("[SHUTDOWN] requested by %s", g_shutdown_stop_reason);
  }
  g_stop_requested = 1;
  sm_scanner_wake();
  wake_game_lifecycle_watcher();
}

bool runtime_sleep_mode_active(void) {
  return atomic_load_explicit(&g_runtime_sleep_mode_active,
                              memory_order_acquire);
}

bool runtime_resume_grace_active(void) {
  uint64_t deadline_us = atomic_load_explicit(
      &g_runtime_resume_grace_deadline_us, memory_order_acquire);
  uint64_t now_us = monotonic_time_us();
  return deadline_us != 0 && now_us != 0 && now_us < deadline_us;
}

static void clear_scan_now_request(void) {
  pthread_mutex_lock(&g_scan_now.reason_mutex);
  g_scan_now.reason[0] = '\0';
  g_scan_now.reset_attempts = false;
  pthread_mutex_unlock(&g_scan_now.reason_mutex);
}

bool request_runtime_sleep_mode(bool active, const char *reason) {
  if (active) {
    bool previous = atomic_exchange_explicit(&g_runtime_sleep_mode_active, true,
                                             memory_order_acq_rel);
    if (previous)
      return false;
    clear_scan_now_request();
    atomic_store_explicit(&g_runtime_resume_grace_deadline_us, 0,
                          memory_order_release);
  } else {
    if (!runtime_sleep_mode_active())
      return false;
    uint64_t now_us = monotonic_time_us();
    uint64_t deadline_us =
        now_us != 0 ? now_us + RUNTIME_RESUME_GRACE_US : 0;
    // Publish the deadline before allowing launch requests to observe that
    // sleep mode ended.
    atomic_store_explicit(&g_runtime_resume_grace_deadline_us, deadline_us,
                          memory_order_release);
    bool previous = atomic_exchange_explicit(&g_runtime_sleep_mode_active,
                                             false, memory_order_acq_rel);
    if (!previous)
      return false;
  }

  const char *resolved_reason =
      (reason && reason[0] != '\0') ? reason : "unknown sleep source";
  log_debug("[SLEEP] %s by %s", active ? "entered" : "left",
            resolved_reason);
  sm_api_service_on_sleep_change(active);
  sm_shellcore_service_on_sleep_change(active);
  sm_ampr_updater_on_sleep_change(active);
  sm_scanner_wake();
  wake_game_lifecycle_watcher();
  return true;
}

void runtime_mount_state_lock(void) {
  pthread_mutex_lock(&g_runtime_mount_state_mutex);
}

void runtime_mount_state_unlock(void) {
  pthread_mutex_unlock(&g_runtime_mount_state_mutex);
}

void request_scan_now(const char *reason) {
  request_scan_now_with_options(reason, false);
}

void request_scan_now_with_options(const char *reason, bool reset_attempts) {
  const char *resolved_reason =
      (reason && reason[0] != '\0') ? reason : "unknown scan source";
  if (runtime_sleep_mode_active())
    return;

  char log_reason[sizeof(g_scan_now.reason)];
  bool should_log = false;

  pthread_mutex_lock(&g_scan_now.reason_mutex);
  if (g_scan_now.reason[0] == '\0') {
    (void)strlcpy(g_scan_now.reason, resolved_reason, sizeof(g_scan_now.reason));
    (void)strlcpy(log_reason, g_scan_now.reason, sizeof(log_reason));
    should_log = true;
  }
  if (reset_attempts)
    g_scan_now.reset_attempts = true;
  pthread_mutex_unlock(&g_scan_now.reason_mutex);

  if (should_log)
    log_debug("[SCAN] immediate scan requested by %s", log_reason);
  sm_scanner_wake();
}

bool consume_scan_now_request(char *reason_out, size_t reason_out_size,
                              bool *reset_attempts_out) {
  if (reason_out && reason_out_size > 0)
    reason_out[0] = '\0';
  if (reset_attempts_out)
    *reset_attempts_out = false;
  pthread_mutex_lock(&g_scan_now.reason_mutex);
  if (g_scan_now.reason[0] == '\0') {
    pthread_mutex_unlock(&g_scan_now.reason_mutex);
    return false;
  }
  if (reason_out && reason_out_size > 0)
    (void)strlcpy(reason_out, g_scan_now.reason, reason_out_size);
  if (reset_attempts_out)
    *reset_attempts_out = g_scan_now.reset_attempts;
  g_scan_now.reason[0] = '\0';
  g_scan_now.reset_attempts = false;
  pthread_mutex_unlock(&g_scan_now.reason_mutex);
  return true;
}

bool sleep_with_stop_check(unsigned int total_us) {
  const unsigned int chunk_us = 200000;
  unsigned int slept = 0;
  while (slept < total_us) {
    if (should_stop_requested())
      return true;
    unsigned int remain = total_us - slept;
    unsigned int step = remain < chunk_us ? remain : chunk_us;
    sceKernelUsleep(step);
    slept += step;
  }
  return should_stop_requested();
}

static void get_firmware_version_string(char out[32]) {
  uint32_t fw = kernel_get_fw_version();
  uint32_t major_bcd = (fw >> 24) & 0xFFu;
  uint32_t minor_bcd = (fw >> 16) & 0xFFu;
  uint32_t major =
      ((major_bcd >> 4) & 0xFu) * 10u + (major_bcd & 0xFu);
  uint32_t minor =
      ((minor_bcd >> 4) & 0xFu) * 10u + (minor_bcd & 0xFu);

  if (major == 0 && minor == 0) {
    (void)strlcpy(out, "unknown", 32);
    return;
  }

  snprintf(out, 32, "%u.%02u", major, minor);
}

pid_t find_pid_by_name(const char *name, bool exclude_self) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t buf_size = 0;
  if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) != 0)
    return -1;
  if (buf_size == 0)
    return 0;

  uint8_t *buf = malloc(buf_size);
  if (!buf)
    return -1;

  if (sysctl(mib, 4, buf, &buf_size, NULL, 0) != 0) {
    free(buf);
    return -1;
  }

  pid_t mypid = exclude_self ? getpid() : -1;
  pid_t found_pid = 0;
  uint8_t *ptr = buf;
  uint8_t *end = buf + buf_size;
  while ((size_t)(end - ptr) >= sizeof(int)) {
    int ki_structsize = 0;
    memcpy(&ki_structsize, ptr, sizeof(ki_structsize));
    if (ki_structsize <= KINFO_TDNAME_OFFSET ||
        (size_t)ki_structsize > (size_t)(end - ptr)) {
      found_pid = -1;
      break;
    }

    pid_t ki_pid = 0;
    memcpy(&ki_pid, &ptr[KINFO_PID_OFFSET], sizeof(ki_pid));
    const char *ki_tdname = (const char *)&ptr[KINFO_TDNAME_OFFSET];
    size_t tdname_size = (size_t)ki_structsize - KINFO_TDNAME_OFFSET;
    ptr += ki_structsize;
    if ((!exclude_self || ki_pid != mypid) &&
        strnlen(ki_tdname, tdname_size) < tdname_size &&
        strcmp(ki_tdname, name) == 0) {
      found_pid = ki_pid;
      break;
    }
  }

  free(buf);
  return found_pid;
}

static bool wait_for_existing_instance_exit(pid_t target_pid) {
  pid_t last_signaled_pid = 0;
  for (unsigned int waited_us = 0; waited_us <= RESTART_WAIT_MAX_US;
       waited_us += RESTART_WAIT_POLL_US) {
    if (target_pid != last_signaled_pid) {
      if (kill(target_pid, SIGTERM) == 0) {
        printf("[RESTART] Requested shutdown of running instance pid=%ld.\n",
               (long)target_pid);
        last_signaled_pid = target_pid;
      } else if (errno != ESRCH) {
        printf("[RESTART] Failed to signal pid=%ld: %s\n", (long)target_pid,
               strerror(errno));
        return false;
      }
    }

    target_pid = find_pid_by_name(PAYLOAD_NAME, true);
    if (target_pid == 0)
      return true;
    if (target_pid < 0) {
      printf("[RESTART] Failed to enumerate running processes.\n");
      return false;
    }
    if (sleep_with_stop_check(RESTART_WAIT_POLL_US))
      return false;
  }

  printf("[RESTART] Timed out waiting for previous instance to exit.\n");
  return false;
}

static void log_non_empty_scan_paths(void) {
  for (int i = 0; i < get_scan_path_count(); i++) {
    const char *scan_path = get_scan_path(i);
    DIR *d = opendir(scan_path);
    if (!d)
      continue;

    bool non_empty = false;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
      if ((entry->d_name[0] == '.' && entry->d_name[1] == '\0') ||
          (entry->d_name[0] == '.' && entry->d_name[1] == '.' &&
           entry->d_name[2] == '\0')) {
        continue;
      }
      non_empty = true;
      break;
    }
    closedir(d);

    if (non_empty)
      log_fs_stats("SCAN", scan_path, NULL);
  }
}

static bool ensure_kstuff_noautomount_file(void) {
  if (path_exists(KSTUFF_NOAUTOMOUNT_FILE))
    return true;

  int fd = open(KSTUFF_NOAUTOMOUNT_FILE, O_WRONLY | O_CREAT, 0666);
  if (fd < 0) {
    printf("[KSTUFF] Failed to create %s: %s\n", KSTUFF_NOAUTOMOUNT_FILE,
           strerror(errno));
    return false;
  }
  (void)close(fd);

  printf("[KSTUFF] Created automount sentinel: %s\n",
         KSTUFF_NOAUTOMOUNT_FILE);
  return true;
}

static bool write_buffer_to_fd(int fd, const unsigned char *buf, size_t size) {
  size_t offset = 0;
  while (offset < size) {
    ssize_t written = write(fd, buf + offset, size - offset);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (written == 0) {
      errno = EIO;
      return false;
    }
    offset += (size_t)written;
  }
  return true;
}

static void ensure_runtime_config_file(void) {
  int fd = open(CONFIG_FILE, O_WRONLY | O_CREAT | O_EXCL, 0666);
  if (fd < 0) {
    if (errno == EEXIST)
      return;
    printf("[CFG] Failed to create %s: %s\n", CONFIG_FILE, strerror(errno));
    return;
  }

  size_t template_size = (size_t)config_ini_example_len;
  int saved_errno = 0;
  if (!write_buffer_to_fd(fd, config_ini_example, template_size))
    saved_errno = errno;
  if (close(fd) != 0 && saved_errno == 0)
    saved_errno = errno;

  if (saved_errno != 0) {
    errno = saved_errno;
    printf("[CFG] Failed to write %s: %s\n", CONFIG_FILE, strerror(errno));
    (void)unlink(CONFIG_FILE);
    return;
  }

  printf("[CFG] Created default config from template: %s\n", CONFIG_FILE);
}

static void cleanup_kstuff_noautomount_file(void) {
  pid_t replacement_pid = find_pid_by_name(PAYLOAD_NAME, true);
  if (replacement_pid > 0) {
    log_debug("[KSTUFF] replacement instance pid=%ld; keeping %s",
              (long)replacement_pid, KSTUFF_NOAUTOMOUNT_FILE);
    return;
  }
  if (replacement_pid < 0) {
    log_debug("[KSTUFF] process enumeration unavailable; keeping %s",
              KSTUFF_NOAUTOMOUNT_FILE);
    return;
  }

  if (unlink(KSTUFF_NOAUTOMOUNT_FILE) == 0) {
    log_debug("[KSTUFF] removed shutdown sentinel: %s",
              KSTUFF_NOAUTOMOUNT_FILE);
  } else if (errno != ENOENT) {
    log_debug("[KSTUFF] failed to remove %s: %s", KSTUFF_NOAUTOMOUNT_FILE,
              strerror(errno));
  }
}

static void stop_conflicting_backpork(void) {
  if (!runtime_config()->backport_fakelib_enabled)
    return;

  const char *names[] = {BACKPORK_PROCESS_NAME, BACKPORK_PROCESS_NAME_ALT};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    while (true) {
      pid_t pid = find_pid_by_name(names[i], false);
      if (pid <= 0)
        break;

      if (kill(pid, SIGKILL) != 0) {
        if (errno != ESRCH) {
          log_debug("  [FAKELIB] failed to stop %s pid=%ld: %s", names[i],
                    (long)pid, strerror(errno));
        }
        break;
      }

      log_debug("  [FAKELIB] stopped conflicting %s pid=%ld", names[i],
                (long)pid);
      sceKernelUsleep(100000);
    }
  }
}

int main(void) {
  bool restarted_previous_instance = false;
  pid_t existing_pid = 0;

  sceUserServiceInitialize(0);
  sceAppInstUtilInitialize();
  kernel_set_ucred_authid(-1, AUTHID_BASE);
  install_signal_handlers();

  mkdir(LOG_DIR, 0777);
  ensure_runtime_config_file();
  existing_pid = find_pid_by_name(PAYLOAD_NAME, true);
  if (existing_pid < 0) {
    printf("[RESTART] Failed to enumerate running processes.\n");
    sceUserServiceTerminate();
    return 1;
  }
  if (!ensure_kstuff_noautomount_file()) {
    sceUserServiceTerminate();
    return 1;
  }
  if (existing_pid > 0) {
    printf("[RESTART] Another instance is already running.\n");
    if (!wait_for_existing_instance_exit(existing_pid)) {
      printf("[KSTUFF] Keeping automount sentinel after failed handoff.\n");
      sceUserServiceTerminate();
      return 0;
    }
    restarted_previous_instance = true;
  }
  // Older builds removed the sentinel unconditionally during shutdown.
  // Recreate it after handoff before starting any mount work.
  if (!ensure_kstuff_noautomount_file()) {
    sceUserServiceTerminate();
    return 1;
  }
  syscall(SYS_thr_set_name, -1, PAYLOAD_NAME);

  if (remove(KILL_FILE) == 0) {
    printf("[STOP] Cleared stale stop flag at startup: %s\n", KILL_FILE);
  } else if (errno != ENOENT) {
    printf("[STOP] Could not clear %s: %s\n", KILL_FILE, strerror(errno));
  }

  (void)unlink(LOG_FILE_PREV);
  (void)rename(LOG_FILE, LOG_FILE_PREV);
  if (!sm_scanner_init())
    log_debug("  [SCAN] scanner service init incomplete; steady-state scanner will stop if initialization cannot be completed");

  char firmware_version[32];
  get_firmware_version_string(firmware_version);
  log_debug(
      "ShadowMount+ v%s exFAT/UFS/PFS/LVD/MD. "
      "FW: %s. "
      "Build: %s. "
      "Thx to VoidWhisper/Gezine/Earthonion/EchoStretch/Drakmor",
      SHADOWMOUNT_VERSION, firmware_version, SHADOWMOUNT_BUILD_TIME);
  if (restarted_previous_instance)
    log_debug("[RESTART] Previous instance stopped, continuing startup");
  load_runtime_config();
  sm_notifications_init();
  stop_conflicting_backpork();
  sm_mdbg_init();
  sm_kstuff_init();
  if (!sm_shellcore_service_start())
    log_debug("  [SHELLCORE] Unix socket service unavailable: %s",
              strerror(errno));
  else if (!sm_shellcore_hooks_start()) {
    log_debug("  [SHELLCORE] lifecycle hooks unavailable; stock behavior kept");
    notify_system_l10n(SM_L10N_SHELLCORE_HOOKS_FAILED);
  }
  if (!refresh_game_lifecycle_watcher())
    log_debug("  [GAME] lifecycle watcher unavailable");
  // Publish the initial AppFocus only after its lifecycle/kstuff consumers.
  if (!sm_shellcore_flags_start())
    log_debug("  [SHELLFLAG] monitor unavailable");
  if (!sm_ampr_updater_start())
    log_debug("  [AMPR] update service unavailable: %s", strerror(errno));

  if (mkdir("/system_ex/app", 0777) != 0 && errno != EEXIST) {
    log_debug("  [MOUNT] failed to create /system_ex/app: %s", strerror(errno));
  }
  if (remount_system_ex() != 0) {
    log_debug("  [MOUNT] remount_system_ex failed: %s", strerror(errno));
  }

  const runtime_config_t *startup_cfg = runtime_config();
  if (startup_cfg->api_enabled) {
    notify_system_l10n(SM_L10N_STARTUP_WEB, SHADOWMOUNT_VERSION,
                       startup_cfg->api_bind_address, startup_cfg->api_port);
  } else {
    notify_system_l10n(SM_L10N_STARTUP, SHADOWMOUNT_VERSION);
  }
  log_non_empty_scan_paths();

  if (runtime_config()->legacy_recursive_scan_forced) {
    notify_system_info_l10n(SM_L10N_RECURSIVE_SCAN_DEPRECATED);
  } else if (runtime_config()->scan_depth > 1u) {
    notify_system_info_l10n(SM_L10N_SCAN_DEPTH_ENABLED,
                            runtime_config()->scan_depth);
  }

  cleanup_mount_dirs();
  if (!wait_for_lvd_release()) {
    log_debug("[SHUTDOWN] stop requested while waiting /dev/lvd2 release");
    goto shutdown;
  }

  log_debug("[STARTUP] cleanup_staged_mount_links begin");
  runtime_mount_state_lock();
  cleanup_staged_mount_links();
  log_debug("[STARTUP] cleanup_duplicate_title_mounts begin");
  cleanup_duplicate_title_mounts();
  runtime_mount_state_unlock();
  if (!app_db_run_startup_maintenance())
    log_debug("  [DB] startup snd0info maintenance unavailable");
  log_debug("[STARTUP] scanner startup sync begin");
  if (!sm_scanner_run_startup_sync()) {
    log_debug("[STARTUP] scanner startup sync aborted");
    goto shutdown;
  }
  log_debug("[STARTUP] scanner startup sync done");
  if (!sm_api_service_start())
    log_debug("  [API] HTTP/JSON service unavailable: %s", strerror(errno));
  sm_scanner_run_loop();

shutdown:
  sm_ampr_updater_stop();
  sm_api_service_stop();
  // Stop ShellCore producers before their lifecycle and mount-state owners.
  sm_shellcore_flags_stop();
  sm_shellcore_hooks_stop();
  stop_game_lifecycle_watcher();
  sm_shellcore_service_stop();
  sm_scanner_shutdown();
  sm_kstuff_shutdown();
  sm_mdbg_shutdown();
  bool title_mounts_released = shutdown_title_mounts();
  bool image_mounts_released = false;
  if (title_mounts_released) {
    image_mounts_released = shutdown_image_mounts();
  } else {
    log_debug("[SHUTDOWN] image teardown skipped while title layers remain");
  }
  if (!image_mounts_released) {
    log_debug("[SHUTDOWN] some image mounts or devices were not fully released");
  }
  if (title_mounts_released && image_mounts_released) {
    cleanup_kstuff_noautomount_file();
  } else {
    log_debug("[KSTUFF] keeping automount disabled after incomplete teardown");
  }
  shutdown_app_db();

  if (atomic_load_explicit(&g_shutdown_on_going_stop_requested,
                           memory_order_acquire)) {
    const char *shutdown_reason =
        (const char *)atomic_load_explicit(&g_shutdown_stop_reason_bits,
                                           memory_order_acquire);
    log_debug("[SHUTDOWN] cleanup complete for %s",
              shutdown_reason ? shutdown_reason : "unknown shutdown source");
  }

  sm_log_shutdown();
  sceUserServiceTerminate();
  return 0;
}
