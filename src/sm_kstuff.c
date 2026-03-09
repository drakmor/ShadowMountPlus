#include "sm_platform.h"

#include "sm_config_mount.h"
#include "sm_filesystem.h"
#include "sm_kstuff.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_path_utils.h"
#include "sm_time.h"
#include "sm_types.h"

#include <stdatomic.h>

#define KSTUFF_SYSENTVEC_TOGGLE_OFFSET 14
#define KSTUFF_SYSENTVEC_ENABLED 0xdeb7u
#define KSTUFF_SYSENTVEC_DISABLED 0xffffu
typedef struct {
  bool active;
  bool image_backed;
  bool pause_applied;
  bool focus_override_active;
  pid_t pid;
  uint32_t app_id;
  uint64_t pause_deadline_us;
  char title_id[MAX_TITLE_ID];
} kstuff_game_entry_t;

typedef struct {
  intptr_t sysentvec_ps5;
  intptr_t sysentvec_ps4;
  bool supported;
  bool restore_on_empty;
  bool current_app_focus_valid;
  uint32_t current_app_focus_id;
  uint64_t restore_retry_us;
  kstuff_game_entry_t game;
} kstuff_state_t;

static kstuff_state_t g_kstuff;
static _Atomic uint32_t g_pending_app_focus_id;
static _Atomic bool g_pending_app_focus_valid;

static bool resolve_kstuff_sysentvec_addrs(intptr_t *ps5_out, intptr_t *ps4_out) {
  if (!ps5_out || !ps4_out)
    return false;

  *ps5_out = 0;
  *ps4_out = 0;

  switch (kernel_get_fw_version() & 0xffff0000u) {
  case 0x1000000:
  case 0x1010000:
  case 0x1020000:
  case 0x1050000:
  case 0x1100000:
  case 0x1110000:
  case 0x1120000:
  case 0x1130000:
  case 0x1140000:
  case 0x2000000:
  case 0x2200000:
  case 0x2250000:
  case 0x2260000:
  case 0x2300000:
  case 0x2500000:
  case 0x2700000:
    // Older jailbreak chains usually rely on Byepervisor instead of kstuff.
    return false;

  case 0x3000000:
  case 0x3100000:
  case 0x3200000:
  case 0x3210000:
    *ps5_out = KERNEL_ADDRESS_DATA_BASE + 0xca0cd8;
    *ps4_out = KERNEL_ADDRESS_DATA_BASE + 0xca0e50;
    return true;

  case 0x4000000:
  case 0x4020000:
  case 0x4030000:
  case 0x4500000:
  case 0x4510000:
    *ps5_out = KERNEL_ADDRESS_DATA_BASE + 0xd11bb8;
    *ps4_out = KERNEL_ADDRESS_DATA_BASE + 0xd11d30;
    return true;

  case 0x5000000:
  case 0x5020000:
  case 0x5100000:
  case 0x5500000:
    *ps5_out = KERNEL_ADDRESS_DATA_BASE + 0xe00be8;
    *ps4_out = KERNEL_ADDRESS_DATA_BASE + 0xe00d60;
    return true;

  case 0x6000000:
  case 0x6020000:
  case 0x6500000:
    *ps5_out = KERNEL_ADDRESS_DATA_BASE + 0xe210a8;
    *ps4_out = KERNEL_ADDRESS_DATA_BASE + 0xe21220;
    return true;

  case 0x7000000:
  case 0x7010000:
    *ps5_out = KERNEL_ADDRESS_DATA_BASE + 0xe21ab8;
    *ps4_out = KERNEL_ADDRESS_DATA_BASE + 0xe21c30;
    return true;

  case 0x7200000:
  case 0x7400000:
  case 0x7600000:
  case 0x7610000:
    *ps5_out = KERNEL_ADDRESS_DATA_BASE + 0xe21b78;
    *ps4_out = KERNEL_ADDRESS_DATA_BASE + 0xe21cf0;
    return true;

  case 0x8000000:
  case 0x8200000:
  case 0x8400000:
  case 0x8600000:
    *ps5_out = KERNEL_ADDRESS_DATA_BASE + 0xe21ca8;
    *ps4_out = KERNEL_ADDRESS_DATA_BASE + 0xe21e20;
    return true;

  case 0x9000000:
  case 0x9050000:
  case 0x9200000:
  case 0x9400000:
  case 0x9600000:
    *ps5_out = KERNEL_ADDRESS_DATA_BASE + 0xdba648;
    *ps4_out = KERNEL_ADDRESS_DATA_BASE + 0xdba7c0;
    return true;

  case 0x10000000:
  case 0x10010000:
  case 0x10200000:
  case 0x10400000:
  case 0x10600000:
    *ps5_out = KERNEL_ADDRESS_DATA_BASE + 0xdba6d8;
    *ps4_out = KERNEL_ADDRESS_DATA_BASE + 0xdba850;
    return true;

  default:
    return false;
  }
}

static bool kstuff_sysentvec_is_enabled(intptr_t sysentvec_addr) {
  return kernel_getshort(sysentvec_addr + KSTUFF_SYSENTVEC_TOGGLE_OFFSET) !=
         KSTUFF_SYSENTVEC_DISABLED;
}

static void set_kstuff_sysentvec_enabled(intptr_t sysentvec_addr, bool enabled) {
  (void)kernel_setshort(sysentvec_addr + KSTUFF_SYSENTVEC_TOGGLE_OFFSET,
                        enabled ? KSTUFF_SYSENTVEC_ENABLED
                                : KSTUFF_SYSENTVEC_DISABLED);
}

static bool read_kstuff_enabled_state(bool *ps5_out, bool *ps4_out) {
  if (!g_kstuff.supported)
    return false;

  bool ps5_enabled = kstuff_sysentvec_is_enabled(g_kstuff.sysentvec_ps5);
  bool ps4_enabled = kstuff_sysentvec_is_enabled(g_kstuff.sysentvec_ps4);
  if (ps5_out)
    *ps5_out = ps5_enabled;
  if (ps4_out)
    *ps4_out = ps4_enabled;
  return ps5_enabled && ps4_enabled;
}

static bool kstuff_state_matches(bool enabled, bool ps5_enabled,
                                 bool ps4_enabled) {
  return ps5_enabled == enabled && ps4_enabled == enabled;
}

static bool apply_kstuff_enabled_state(bool enabled, bool notify_user,
                                       bool *fully_applied_out);

static void mark_restore_needed(void) {
  g_kstuff.restore_on_empty = true;
  g_kstuff.restore_retry_us = 0;
}

static uint32_t get_pause_delay_seconds_for_title(const char *title_id,
                                                  bool *image_backed_out) {
  char source_path[MAX_PATH];
  bool image_backed =
      read_mount_link(title_id, source_path, sizeof(source_path)) &&
      is_under_image_mount_base(source_path);

  if (image_backed_out)
    *image_backed_out = image_backed;

  uint32_t delay_seconds =
      image_backed ? runtime_config()->kstuff_pause_delay_image_seconds
                   : runtime_config()->kstuff_pause_delay_direct_seconds;
  uint32_t override_delay_seconds = 0;
  if (get_kstuff_pause_delay_override_for_title(title_id,
                                                &override_delay_seconds)) {
    return override_delay_seconds;
  }

  return delay_seconds;
}

static void restore_kstuff_if_needed(const char *reason) {
  if (g_kstuff.game.active && g_kstuff.game.pause_applied)
    return;

  if (!g_kstuff.restore_on_empty)
    return;

  uint64_t now_us = monotonic_time_us();
  if (now_us != 0 && g_kstuff.restore_retry_us != 0 &&
      now_us < g_kstuff.restore_retry_us) {
    return;
  }

  if (g_kstuff.game.active && !g_kstuff.game.pause_applied) {
    if (g_kstuff.game.pause_deadline_us == 0 ||
        (now_us != 0 && now_us >= g_kstuff.game.pause_deadline_us)) {
      return;
    }
  }

  bool enabled = sm_kstuff_is_enabled();
  if (!enabled)
    enabled = sm_kstuff_set_enabled(true, true);
  if (enabled) {
    log_debug("  [KSTUFF] auto-resumed (%s)%s",
              reason ? reason : "no reason",
              g_kstuff.game.active ? ", pending delay for another tracked game"
                                   : "");
  } else {
    log_debug("  [KSTUFF] failed to auto-resume (%s)",
              reason ? reason : "no reason");
    g_kstuff.restore_retry_us =
        now_us == 0 ? 0 : now_us + KSTUFF_RESTORE_RETRY_US;
    return;
  }

  g_kstuff.restore_on_empty = false;
  g_kstuff.restore_retry_us = 0;
}

static void maybe_apply_kstuff_pause_for_slot(kstuff_game_entry_t *entry) {
  if (!entry || !entry->active || entry->pause_applied)
    return;

  uint64_t now_us = monotonic_time_us();
  if (now_us != 0 && now_us < entry->pause_deadline_us)
    return;

  if (g_kstuff.current_app_focus_valid && entry->app_id != 0 &&
      g_kstuff.current_app_focus_id != entry->app_id) {
    return;
  }

  bool was_enabled = sm_kstuff_is_enabled();
  bool fully_disabled = false;
  (void)apply_kstuff_enabled_state(false, was_enabled, &fully_disabled);
  if (!fully_disabled) {
    log_debug("  [KSTUFF] failed to auto-pause for %s pid=%ld",
              entry->title_id, (long)entry->pid);
    return;
  }

  if (was_enabled) {
    mark_restore_needed();
    log_debug("  [KSTUFF] auto-paused for %s pid=%ld (%s launch)",
              entry->title_id, (long)entry->pid,
              entry->image_backed ? "image" : "direct");
  } else {
    log_debug("  [KSTUFF] pause deadline reached for %s pid=%ld but kstuff "
              "was already disabled", entry->title_id, (long)entry->pid);
  }

  entry->pause_applied = true;
}

static void handle_kstuff_app_focus_change(uint32_t focused_app_id) {
  g_kstuff.current_app_focus_valid = true;
  g_kstuff.current_app_focus_id = focused_app_id;

  if (!g_kstuff.game.active || g_kstuff.game.app_id == 0)
    return;

  if (focused_app_id != g_kstuff.game.app_id) {
    if (!g_kstuff.game.pause_applied || g_kstuff.game.focus_override_active)
      return;

    bool enabled = sm_kstuff_is_enabled();
    if (!enabled)
      enabled = sm_kstuff_set_enabled(true, false);
    if (!enabled) {
      log_debug("  [KSTUFF] failed to enable on app focus leave "
                "(tracked=0x%08X current=0x%08X)",
                g_kstuff.game.app_id, focused_app_id);
      return;
    }

    g_kstuff.game.focus_override_active = true;
    log_debug("  [KSTUFF] temporarily enabled on app focus leave "
              "(tracked=0x%08X current=0x%08X)",
              g_kstuff.game.app_id, focused_app_id);
    return;
  }

  if (!g_kstuff.game.focus_override_active)
    return;

  bool fully_disabled = false;
  (void)apply_kstuff_enabled_state(false, false, &fully_disabled);
  if (!fully_disabled) {
    log_debug("  [KSTUFF] failed to restore disabled state on app focus return "
              "(app_id=0x%08X)",
              focused_app_id);
    return;
  }

  g_kstuff.game.focus_override_active = false;
  log_debug("  [KSTUFF] restored disabled state on app focus return "
            "(app_id=0x%08X)",
            focused_app_id);
}

uint64_t sm_kstuff_game_next_wake_us(uint64_t now_us) {
  if (!sm_kstuff_game_feature_enabled())
    return 0;

  uint64_t next_wake_us = 0;
  if (g_kstuff.game.active && !g_kstuff.game.pause_applied) {
    if (g_kstuff.current_app_focus_valid && g_kstuff.game.app_id != 0 &&
        g_kstuff.current_app_focus_id != g_kstuff.game.app_id) {
      next_wake_us = 0;
    } else if (g_kstuff.game.pause_deadline_us != 0) {
      next_wake_us = g_kstuff.game.pause_deadline_us;
    } else if (now_us == 0) {
      next_wake_us = 1;
    } else {
      next_wake_us = now_us + GAME_LIFECYCLE_POLL_INTERVAL_US;
    }
  }

  if (g_kstuff.restore_on_empty) {
    uint64_t restore_wake_us = g_kstuff.restore_retry_us;
    if (restore_wake_us == 0) {
      if (g_kstuff.game.active && !g_kstuff.game.pause_applied) {
        if (g_kstuff.game.pause_deadline_us != 0) {
          restore_wake_us = g_kstuff.game.pause_deadline_us;
        } else if (now_us == 0) {
          restore_wake_us = 1;
        } else {
          restore_wake_us = now_us + GAME_LIFECYCLE_POLL_INTERVAL_US;
        }
      } else {
        restore_wake_us = now_us == 0 ? 1 : now_us;
      }
    }

    if (next_wake_us == 0 || restore_wake_us < next_wake_us)
      next_wake_us = restore_wake_us;
  }

  return next_wake_us;
}

bool sm_kstuff_is_supported(void) {
  return g_kstuff.supported;
}

bool sm_kstuff_is_enabled(void) {
  return read_kstuff_enabled_state(NULL, NULL);
}

static bool apply_kstuff_enabled_state(bool enabled, bool notify_user,
                                       bool *fully_applied_out) {
  if (!g_kstuff.supported)
    return false;

  bool ps5_enabled = false;
  bool ps4_enabled = false;
  bool is_enabled = read_kstuff_enabled_state(&ps5_enabled, &ps4_enabled);
  bool fully_applied = kstuff_state_matches(enabled, ps5_enabled, ps4_enabled);
  if (fully_applied) {
    if (fully_applied_out)
      *fully_applied_out = true;
    return is_enabled;
  }

  if (ps5_enabled != enabled)
    set_kstuff_sysentvec_enabled(g_kstuff.sysentvec_ps5, enabled);
  if (ps4_enabled != enabled)
    set_kstuff_sysentvec_enabled(g_kstuff.sysentvec_ps4, enabled);

  is_enabled = read_kstuff_enabled_state(&ps5_enabled, &ps4_enabled);
  fully_applied = kstuff_state_matches(enabled, ps5_enabled, ps4_enabled);
  if (fully_applied_out)
    *fully_applied_out = fully_applied;

  if (fully_applied) {
    log_debug("  [KSTUFF] %s both sysentvecs", enabled ? "enabled" : "disabled");
    if (notify_user) {
      notify_system_info(enabled ? "KStuff active again"
                                 : "KStuff paused while the game is running");
    }
  } else {
    log_debug("  [KSTUFF] %s request incomplete (ps5=%s ps4=%s)",
              enabled ? "enable" : "disable", ps5_enabled ? "on" : "off",
              ps4_enabled ? "on" : "off");
  }

  return is_enabled;
}

bool sm_kstuff_set_enabled(bool enabled, bool notify_user) {
  return apply_kstuff_enabled_state(enabled, notify_user, NULL);
}

bool sm_kstuff_game_feature_enabled(void) {
  return runtime_config()->kstuff_game_auto_toggle && sm_kstuff_is_supported();
}

void sm_kstuff_game_on_exec(pid_t pid, const char *title_id, uint32_t app_id,
                            uint64_t exec_time_us) {
  if (!sm_kstuff_game_feature_enabled())
    return;

  if (g_kstuff.game.active) {
    if (g_kstuff.game.pid == pid) {
      log_debug("  [KSTUFF] already tracking pid=%ld for %s", (long)pid,
                title_id);
      return;
    }

    log_debug("  [KSTUFF] handoff tracked game pid=%ld (%s) -> pid=%ld (%s)",
              (long)g_kstuff.game.pid, g_kstuff.game.title_id, (long)pid,
              title_id);
    memset(&g_kstuff.game, 0, sizeof(g_kstuff.game));
    mark_restore_needed();
    restore_kstuff_if_needed("tracked game handoff");
  }

  if (is_kstuff_pause_disabled_for_title(title_id)) {
    log_debug("  [KSTUFF] auto-pause disabled by config for %s pid=%ld "
              "app_id=0x%08X", title_id, (long)pid, app_id);
    return;
  }

  bool image_backed = false;
  uint32_t delay_seconds =
      get_pause_delay_seconds_for_title(title_id, &image_backed);
  uint64_t now_us = monotonic_time_us();
  uint64_t base_time_us = exec_time_us != 0 ? exec_time_us : now_us;

  g_kstuff.game.active = true;
  g_kstuff.game.image_backed = image_backed;
  g_kstuff.game.pause_applied = false;
  g_kstuff.game.focus_override_active = false;
  g_kstuff.game.pid = pid;
  g_kstuff.game.app_id = app_id;
  g_kstuff.game.pause_deadline_us =
      base_time_us == 0 ? 0 : base_time_us + (uint64_t)delay_seconds * 1000000ull;
  (void)strlcpy(g_kstuff.game.title_id, title_id, sizeof(g_kstuff.game.title_id));

  log_debug("  [KSTUFF] tracking game launch: %s pid=%ld app_id=0x%08X source=%s "
            "pause_delay=%us", title_id, (long)pid, app_id,
            image_backed ? "image" : "direct", delay_seconds);
}

void sm_kstuff_note_app_focus(uint32_t app_id) {
  atomic_store(&g_pending_app_focus_id, app_id);
  atomic_store(&g_pending_app_focus_valid, true);
}

void sm_kstuff_game_on_exit(pid_t pid) {
  if (!g_kstuff.game.active || g_kstuff.game.pid != pid)
    return;

  log_debug("  [KSTUFF] game stopped: %s pid=%ld",
            g_kstuff.game.title_id, (long)pid);
  memset(&g_kstuff.game, 0, sizeof(g_kstuff.game));
  if (!g_kstuff.game.active)
    mark_restore_needed();
  restore_kstuff_if_needed("tracked game exit");
}

void sm_kstuff_game_poll(void) {
  if (!sm_kstuff_game_feature_enabled())
    return;

  if (atomic_exchange(&g_pending_app_focus_valid, false)) {
    handle_kstuff_app_focus_change(atomic_load(&g_pending_app_focus_id));
  }

  restore_kstuff_if_needed("pending auto-resume");
  maybe_apply_kstuff_pause_for_slot(&g_kstuff.game);
}

void sm_kstuff_game_shutdown(void) {
  memset(&g_kstuff.game, 0, sizeof(g_kstuff.game));
  restore_kstuff_if_needed("watcher shutdown");
}

void sm_kstuff_init(void) {
  memset(&g_kstuff, 0, sizeof(g_kstuff));
  atomic_store(&g_pending_app_focus_id, 0);
  atomic_store(&g_pending_app_focus_valid, false);

  if (!resolve_kstuff_sysentvec_addrs(&g_kstuff.sysentvec_ps5,
                                      &g_kstuff.sysentvec_ps4)) {
    log_debug("  [KSTUFF] runtime control unavailable on firmware 0x%08X",
              kernel_get_fw_version());
    return;
  }

  g_kstuff.supported = true;
  log_debug("  [KSTUFF] runtime control ready (image_delay=%u "
            "direct_delay=%u)",
            runtime_config()->kstuff_pause_delay_image_seconds,
            runtime_config()->kstuff_pause_delay_direct_seconds);
}

void sm_kstuff_shutdown(void) {
  sm_kstuff_game_shutdown();
  atomic_store(&g_pending_app_focus_id, 0);
  atomic_store(&g_pending_app_focus_valid, false);
  memset(&g_kstuff, 0, sizeof(g_kstuff));
}
