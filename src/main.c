#include "sm_platform.h"

#include <stdatomic.h>
#include <sys/sysctl.h>

#include "sm_runtime.h"
#include "sm_types.h"
#include "sm_log.h"
#include "sm_shellcore_flags.h"
#include "sm_config_mount.h"
#include "sm_game_lifecycle.h"
#include "sm_kstuff.h"
#include "sm_mount_device.h"
#include "sm_filesystem.h"
#include "sm_image.h"
#include "sm_scan.h"
#include "sm_install.h"
#include "sm_appdb.h"
#include "sm_limits.h"
#include "sm_paths.h"

#ifndef SHADOWMOUNT_VERSION
#define SHADOWMOUNT_VERSION "unknown"
#endif

#define PAYLOAD_NAME "shadowmountplus.elf"
#define BACKPORK_PROCESS_NAME "backpork.elf"
#define BACKPORK_PROCESS_NAME_ALT "ps5-backpork.elf"
#define RESTART_WAIT_POLL_US 200000u
#define RESTART_WAIT_MAX_US 60000000u
#define KINFO_PID_OFFSET 72
#define KINFO_TDNAME_OFFSET 447

static volatile sig_atomic_t g_stop_requested = 0;
static atomic_bool g_shutdown_on_going_stop_requested = false;
static atomic_bool g_scan_now_requested = false;
static _Atomic(uintptr_t) g_shutdown_stop_reason_bits = 0;
static _Atomic(uintptr_t) g_scan_now_reason_bits = 0;

static void on_signal(int sig) {
  (void)sig;
  g_stop_requested = 1;
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
}

bool should_stop_requested(void) {
  if (g_stop_requested)
    return true;
  if (access(KILL_FILE, F_OK) == 0) {
    remove(KILL_FILE);
    return true;
  }
  return false;
}

void request_shutdown_stop(const char *reason) {
  const char *resolved_reason =
      (reason && reason[0] != '\0') ? reason : "unknown shutdown source";
  bool already_requested =
      atomic_exchange_explicit(&g_shutdown_on_going_stop_requested, true,
                               memory_order_acq_rel);
  if (!already_requested) {
    atomic_store_explicit(&g_shutdown_stop_reason_bits,
                          (uintptr_t)resolved_reason, memory_order_release);
    log_debug("[SHUTDOWN] requested by %s", resolved_reason);
  }
  g_stop_requested = 1;
}

void request_scan_now(const char *reason) {
  const char *resolved_reason =
      (reason && reason[0] != '\0') ? reason : "unknown scan source";
  bool already_requested =
      atomic_exchange_explicit(&g_scan_now_requested, true, memory_order_acq_rel);
  if (!already_requested) {
    atomic_store_explicit(&g_scan_now_reason_bits, (uintptr_t)resolved_reason,
                          memory_order_release);
    log_debug("[SCAN] immediate scan requested by %s", resolved_reason);
  }
}

bool consume_scan_now_request(const char **reason_out) {
  if (!atomic_load_explicit(&g_scan_now_requested, memory_order_acquire))
    return false;

  if (reason_out)
    *reason_out = (const char *)atomic_load_explicit(&g_scan_now_reason_bits,
                                                     memory_order_acquire);
  atomic_store_explicit(&g_scan_now_reason_bits, (uintptr_t)0,
                        memory_order_release);
  atomic_store_explicit(&g_scan_now_requested, false, memory_order_release);
  return true;
}

bool sleep_with_stop_check(unsigned int total_us) {
  const unsigned int chunk_us = 200000;
  unsigned int slept = 0;
  while (slept < total_us) {
    if (should_stop_requested())
      return true;
    if (atomic_load_explicit(&g_scan_now_requested, memory_order_acquire))
      return false;
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
  if (!name || name[0] == '\0')
    return -1;

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
  while (ptr < end) {
    int ki_structsize = *(int *)ptr;
    pid_t ki_pid = *(pid_t *)&ptr[KINFO_PID_OFFSET];
    const char *ki_tdname = (const char *)&ptr[KINFO_TDNAME_OFFSET];
    ptr += ki_structsize;
    if ((!exclude_self || ki_pid != mypid) && strcmp(ki_tdname, name) == 0) {
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
    if (!scan_path)
      continue;

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
  bool kstuff_runtime_enabled = false;
  pid_t existing_pid = 0;
  const char *scan_reason = NULL;
  scan_candidate_t candidates[MAX_PENDING];
  memset(candidates, 0, sizeof(candidates));

  sceUserServiceInitialize(0);
  sceAppInstUtilInitialize();
  kernel_set_ucred_authid(-1, 0x4801000000000013L);
  install_signal_handlers();

  mkdir(LOG_DIR, 0777);
  existing_pid = find_pid_by_name(PAYLOAD_NAME, true);
  if (existing_pid < 0) {
    printf("[RESTART] Failed to enumerate running processes.\n");
    sceUserServiceTerminate();
    return 1;
  }
  if (existing_pid > 0) {
    printf("[RESTART] Another instance is already running.\n");
    if (!wait_for_existing_instance_exit(existing_pid)) {
      sceUserServiceTerminate();
      return 0;
    }
    restarted_previous_instance = true;
  }
  syscall(SYS_thr_set_name, -1, PAYLOAD_NAME);

  if (remove(KILL_FILE) == 0) {
    printf("[STOP] Cleared stale stop flag at startup: %s\n", KILL_FILE);
  } else if (errno != ENOENT) {
    printf("[STOP] Could not clear %s: %s\n", KILL_FILE, strerror(errno));
  }

  (void)unlink(LOG_FILE_PREV);
  (void)rename(LOG_FILE, LOG_FILE_PREV);

  char firmware_version[32];
  get_firmware_version_string(firmware_version);
  log_debug(
      "ShadowMount+ v%s exFAT/UFS/PFS/LVD/MD. "
      "FW: %s. "
      "Build: %s %s. "
      "Thx to VoidWhisper/Gezine/Earthonion/EchoStretch/Drakmor",
      SHADOWMOUNT_VERSION, firmware_version, __DATE__, __TIME__);
  if (restarted_previous_instance)
    log_debug("[RESTART] Previous instance stopped, continuing startup");
  load_runtime_config();
  sm_notifications_init();
  stop_conflicting_backpork();
  if (!sm_shellcore_flags_start())
    log_debug("  [SHELLFLAG] monitor unavailable");
  kstuff_runtime_enabled = runtime_config()->kstuff_game_auto_toggle;
  if (kstuff_runtime_enabled)
    sm_kstuff_init();
  if (!start_game_lifecycle_watcher())
    log_debug("  [GAME] lifecycle watcher unavailable");

  if (mkdir("/system_ex/app", 0777) != 0 && errno != EEXIST) {
    log_debug("  [MOUNT] failed to create /system_ex/app: %s", strerror(errno));
  }
  if (remount_system_ex() != 0) {
    log_debug("  [MOUNT] remount_system_ex failed: %s", strerror(errno));
  }

  notify_system("ShadowMount+ v%s exFAT/UFS/PFS", SHADOWMOUNT_VERSION);
  log_non_empty_scan_paths();

  if (runtime_config()->recursive_scan)
    notify_system_info("ShadowMount+: Recursive scan enabled.");

  cleanup_mount_dirs();
  if (!wait_for_lvd_release()) {
    log_debug("[SHUTDOWN] stop requested while waiting /dev/lvd2 release");
    goto shutdown;
  }

  cleanup_staged_mount_links();
  cleanup_duplicate_title_mounts();
  cleanup_lost_sources_before_scan();
  int total_found_games = 0;
  int candidate_count =
      collect_scan_candidates(candidates, MAX_PENDING, &total_found_games);
  if (candidate_count > 0) {
    int new_games = 0;
    for (int i = 0; i < candidate_count; i++) {
      if (!candidates[i].installed)
        new_games++;
    }
    if (new_games > 0)
      notify_system_info("Found %d new games. Executing...", new_games);
    process_scan_candidates(candidates, candidate_count);
  }
  mount_backport_overlays();
  notify_system_rich(true, "Library Synchronized.\nFound %d games.",
                     total_found_games);

  while (true) {
    if (should_stop_requested()) {
      log_debug("[SHUTDOWN] stop requested");
      break;
    }

    if (!consume_scan_now_request(&scan_reason)) {
      if (sleep_with_stop_check(runtime_config()->scan_interval_us)) {
        log_debug("[SHUTDOWN] stop requested during sleep");
        break;
      }
      (void)consume_scan_now_request(&scan_reason);
    }

    if (scan_reason) {
      log_debug("[SCAN] running immediate scan (%s)", scan_reason);
      scan_reason = NULL;
    }

    cleanup_lost_sources_before_scan();
    candidate_count = collect_scan_candidates(candidates, MAX_PENDING, NULL);
    if (candidate_count > 0)
      process_scan_candidates(candidates, candidate_count);
    mount_backport_overlays();
  }

shutdown:
  sm_shellcore_flags_stop();
  stop_game_lifecycle_watcher();
  if (kstuff_runtime_enabled)
    sm_kstuff_shutdown();
  shutdown_title_mounts();
  if (!shutdown_image_mounts()) {
    log_debug("[SHUTDOWN] some image mounts or devices were not fully released");
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

  sceUserServiceTerminate();
  return 0;
}
