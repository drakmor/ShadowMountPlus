#include "sm_platform.h"

#include <pthread.h>

#include "sm_game_lifecycle.h"
#include "sm_kstuff.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_runtime.h"
#include "sm_shellcore_flags.h"

#define SHELLCORE_FLAG_WAITMODE_OR 2u
#define SHELLCORE_FLAG_ALL_BITS UINT64_MAX

typedef intptr_t sm_kernel_event_flag_t;

int sceKernelOpenEventFlag(sm_kernel_event_flag_t *ef, const char *name);
int sceKernelPollEventFlag(sm_kernel_event_flag_t ef, uint64_t bit_pattern,
                           unsigned int wait_mode, uint64_t *result_pattern);
int sceKernelCloseEventFlag(sm_kernel_event_flag_t ef);

typedef struct {
  const char *name;
  sm_kernel_event_flag_t handle;
  uint64_t last_pattern;
  int last_rc;
  bool is_open;
  bool has_last_pattern;
  bool has_last_rc;
} shellcore_flag_monitor_t;

typedef struct {
  uint64_t mask;
  const char *name;
} shellcore_flag_bit_desc_t;

static shellcore_flag_monitor_t g_shellcore_flags[] = {
    {.name = "SceShellCoreUtilAppFocus", .handle = -1},
    {.name = "SceSystemStateMgrInfo", .handle = -1},
/*    {.name = "SceSystemStateMgrStatus", .handle = -1},
    {.name = "SceSysCoreSuspend", .handle = -1},
    {.name = "SceLncUtilSystemStatus", .handle = -1},
    {.name = "SceShellCoreUtilCtrlFocus", .handle = -1},
    {.name = "SceShellCoreUtilPowerControl", .handle = -1},
    {.name = "SceShellCoreUtilRunLevel", .handle = -1},
    {.name = "SceShellCoreUtilUIStatus", .handle = -1},
    {.name = "SceAutoMountUsbMass", .handle = -1},
    {.name = "SceVshctlInternalUserFlag", .handle = -1},
    {.name = "SceBootStatusFlags", .handle = -1},
    {.name = "SceSysCoreReboot", .handle = -1},
     */
};

static pthread_t g_shellcore_flag_thread;
static pthread_mutex_t g_shellcore_flag_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_shellcore_flag_cond = PTHREAD_COND_INITIALIZER;
static bool g_shellcore_flag_thread_started = false;
static bool g_shellcore_flag_start_ready = false;
static bool g_shellcore_flag_start_success = false;
static volatile sig_atomic_t g_shellcore_flag_stop_requested = 0;

#if 0
static const shellcore_flag_bit_desc_t g_lnc_util_system_status_bits[] = {
    {0x0000000000000001ULL, "EXTRA_AUDIO_CPU_BUDGET_AVAILABLE"},
    {0x0000000000000002ULL, "SHELLUI_FG_GAME_BG_CPU_MODE"},
};

static const shellcore_flag_bit_desc_t g_power_control_bits[] = {
    {0x0000000000020000ULL, "SCREEN_SAVER_ON"},
    {0x0000000000040000ULL, "POWER_TRANSITION"},
    {0x0000000000080000ULL, "POWER_LOCK"},
    {0x0000000000100000ULL, "SOCIAL_SCREEN_STATUS"},
    {0x0000000000200000ULL, "REQUEST_CAMERA_CALIBRATION"},
    {0x0000000002000000ULL, "USER_INPUT_PULSE"},
    {0x0000000004000000ULL, "IMPOSE_OVERLAY"},
    {0x0000000008000000ULL, "AUTO_POWER_DOWN_TIMER_RESET"},
    {0x0000000010000000ULL, "REMOTE_PLAY_STATUS"},
    {0x0000000020000000ULL, "SHARE_PLAY_STATUS"},
    {0x0000000040000000ULL, "GAME_LIVE_STREAMING"},
    {0x0000000080000000ULL, "BGM_CPU_BUDGET"},
    {0x0000000100000000ULL, "PRENOTIFY_REMOTE_PLAY"},
    {0x0000000200000000ULL, "PRENOTIFY_SHARE_PLAY"},
    {0x0000000400000000ULL, "PRENOTIFY_GAME_LIVE_STREAMING"},
    {0x0000000800000000ULL, "REMOTE_PLAY_CPU_BUDGET"},
    {0x0000001000000000ULL, "SHARE_PLAY_CPU_BUDGET"},
    {0x0000100000000000ULL, "EXT_STORAGE_APP_MOVE_IN_PROGRESS"},
    {0x0000200000000000ULL, "STATE_0x2000000000000"},
};

static const shellcore_flag_bit_desc_t g_shellcore_ui_status_bits[] = {
    {0x0000000000000001ULL, "LOW_BIT_0x1"},
    {0x0000000000000010ULL, "LOW_BIT_0x10"},
    {0x0000000000018000ULL, "BASIC_PRODUCT_SHAPE_FIELD"},
    {0x0000000000020000ULL, "EXT_HDD_MOUNT_UNBLOCK"},
    {0x0000000400000000ULL, "HIGH32_BIT2"},
};

static const shellcore_flag_bit_desc_t g_vshctl_internal_user_bits[] = {
    {0x0000000000000001ULL, "INTERNAL_CRONOS_USER"},
};

static const shellcore_flag_bit_desc_t g_system_state_mgr_status_bits[] = {
    {0x0000000000000001ULL, "CEC_ONE_TOUCH_PLAY_COMMAND"},
    {0x0000000000000002ULL, "MEDIA_PLAYBACK_MODE"},
    {0x0000000000000004ULL, "BD_DRIVE_READY"},
    {0x0000000000000008ULL, "TICK_MUSIC_PLAYBACK"},
    {0x0000000000000010ULL, "TICK_VIDEO_PLAYBACK"},
    {0x0000000000000020ULL, "TICK_0x20"},
    {0x0000000000000040ULL, "TICK_PARTY_CHAT"},
    {0x0000000000000100ULL, "TURN_OFF_REQUEST"},
    {0x0000000000000200ULL, "REBOOT_REQUEST"},
    {0x0000000000000400ULL, "ENTER_STANDBY_REQUEST"},
    {0x0000000000010000ULL, "START_SHUTDOWN_TIMER"},
    {0x0000000000020000ULL, "START_REBOOT_TIMER"},
    {0x0000000000040000ULL, "START_STANDBY_TIMER"},
    {0x0000000000080000ULL, "EXTEND_SHUTDOWN_TIMER"},
    {0x0000000000100000ULL, "CANCEL_SHUTDOWN_TIMER"},
    {0x0000000000200000ULL, "SHELLUI_SHUTDOWN_IN_PROGRESS"},
    {0x0000000000400000ULL, "EXTEND_SHUTDOWN_TIMER_POST_AUTOUPDATE"},
    {0x0000000001000000ULL, "BIGAPP_LAUNCH_READY"},
    {0x0000000004000000ULL, "VSH_AUTO_UPDATE_VERIFY_DONE"},
    {0x0000000008000000ULL, "START_REBOOT_TIMER_FAST"},
};

static const shellcore_flag_bit_desc_t g_boot_status_bits[] = {
    {0x0000000000000002ULL, "BIT_0x2"},
    {0x0000000000000040ULL, "BIT_0x40"},
    {0x0000000000000080ULL, "BIT_0x80"},
    {0x0000000000000200ULL, "BIT_0x200"},
    {0x0000000000004000ULL, "BIT_0x4000"},
    {0x0000000001000000ULL, "BIGAPP_LAUNCH_READY"},
};

static const shellcore_flag_bit_desc_t g_syscore_reboot_bits[] = {
    {0x0000000000000040ULL, "BIT_0x40"},
    {0x0000000000004000ULL, "BIT_0x4000"},
    {0x0000000100000000ULL, "REBOOT_FLAG"},
};

static const shellcore_flag_bit_desc_t g_syscore_suspend_bits[] = {
    {0x0000000000000001ULL, "SUSPEND_ACK"},
    {0x0000000000000002ULL, "SUSPEND_REQ_BIT1"},
    {0x0000000000000004ULL, "SUSPEND_REQ_BIT2"},
    {0x0000000000000008ULL, "SUSPEND_REQ_BIT3"},
};
#endif

enum {
  SYSTEM_STATE_MGR_STATE_INVALID = 0u,
  SYSTEM_STATE_MGR_STATE_INITIALIZING = 10u,
  SYSTEM_STATE_MGR_STATE_SHUTDOWN_ON_GOING = 100u,
  SYSTEM_STATE_MGR_STATE_POWER_SAVING = 200u,
  SYSTEM_STATE_MGR_STATE_SUSPEND_ON_GOING = 300u,
  SYSTEM_STATE_MGR_STATE_MAIN_ON_STANDBY = 500u,
  SYSTEM_STATE_MGR_STATE_WORKING = 1000u,
};

enum {
  SYSTEM_STATE_MGR_REBOOT_CAUSE_NONE = 0u,
  SYSTEM_STATE_MGR_REBOOT_CAUSE_CFF = 1u,
};

static const shellcore_flag_bit_desc_t *get_shellcore_flag_bits(
    const shellcore_flag_monitor_t *flag, size_t *count_out) {
  if (count_out)
    *count_out = 0;
  if (!flag || !flag->name)
    return NULL;

#if 0
  if (strcmp(flag->name, "SceLncUtilSystemStatus") == 0) {
    if (count_out) {
      *count_out = sizeof(g_lnc_util_system_status_bits) /
                   sizeof(g_lnc_util_system_status_bits[0]);
    }
    return g_lnc_util_system_status_bits;
  }

  if (strcmp(flag->name, "SceShellCoreUtilPowerControl") == 0) {
    if (count_out)
      *count_out = sizeof(g_power_control_bits) / sizeof(g_power_control_bits[0]);
    return g_power_control_bits;
  }

  if (strcmp(flag->name, "SceShellCoreUtilUIStatus") == 0) {
    if (count_out) {
      *count_out = sizeof(g_shellcore_ui_status_bits) /
                   sizeof(g_shellcore_ui_status_bits[0]);
    }
    return g_shellcore_ui_status_bits;
  }

  if (strcmp(flag->name, "SceVshctlInternalUserFlag") == 0) {
    if (count_out) {
      *count_out = sizeof(g_vshctl_internal_user_bits) /
                   sizeof(g_vshctl_internal_user_bits[0]);
    }
    return g_vshctl_internal_user_bits;
  }

  if (strcmp(flag->name, "SceSystemStateMgrStatus") == 0) {
    if (count_out) {
      *count_out = sizeof(g_system_state_mgr_status_bits) /
                   sizeof(g_system_state_mgr_status_bits[0]);
    }
    return g_system_state_mgr_status_bits;
  }

  if (strcmp(flag->name, "SceBootStatusFlags") == 0) {
    if (count_out)
      *count_out = sizeof(g_boot_status_bits) / sizeof(g_boot_status_bits[0]);
    return g_boot_status_bits;
  }

  if (strcmp(flag->name, "SceSysCoreReboot") == 0) {
    if (count_out)
      *count_out = sizeof(g_syscore_reboot_bits) / sizeof(g_syscore_reboot_bits[0]);
    return g_syscore_reboot_bits;
  }

  if (strcmp(flag->name, "SceSysCoreSuspend") == 0) {
    if (count_out)
      *count_out = sizeof(g_syscore_suspend_bits) /
                   sizeof(g_syscore_suspend_bits[0]);
    return g_syscore_suspend_bits;
  }
#endif

  return NULL;
}

static const char *get_system_state_mgr_state_name(unsigned state) {
  switch (state) {
  case SYSTEM_STATE_MGR_STATE_INVALID:
    return "INVALID";
  case SYSTEM_STATE_MGR_STATE_INITIALIZING:
    return "INITIALIZING";
  case SYSTEM_STATE_MGR_STATE_SHUTDOWN_ON_GOING:
    return "SHUTDOWN_ON_GOING";
  case SYSTEM_STATE_MGR_STATE_POWER_SAVING:
    return "POWER_SAVING";
  case SYSTEM_STATE_MGR_STATE_SUSPEND_ON_GOING:
    return "SUSPEND_ON_GOING";
  case SYSTEM_STATE_MGR_STATE_MAIN_ON_STANDBY:
    return "MAIN_ON_STANDBY";
  case SYSTEM_STATE_MGR_STATE_WORKING:
    return "WORKING";
  default:
    return "UNKNOWN";
  }
}

static const char *get_system_state_mgr_reboot_cause_name(unsigned cause) {
  switch (cause) {
  case SYSTEM_STATE_MGR_REBOOT_CAUSE_NONE:
    return "NONE";
  case SYSTEM_STATE_MGR_REBOOT_CAUSE_CFF:
    return "CFF";
  default:
    return "UNKNOWN";
  }
}

static void append_shellcore_flag_token(char *dst, size_t dst_size,
                                        const char *token) {
  size_t used;

  if (!dst || dst_size == 0 || !token || token[0] == '\0')
    return;

  used = strnlen(dst, dst_size);
  if (used >= dst_size - 1)
    return;

  (void)snprintf(dst + used, dst_size - used, "%s%s", used == 0 ? "" : "|",
                 token);
}

static bool format_shellcore_flag_special_known_masks(
    const shellcore_flag_monitor_t *flag, char *dst, size_t dst_size) {
  if (!flag || !flag->name || !dst || dst_size == 0)
    return false;

#if 0
  if (strcmp(flag->name, "SceShellCoreUtilRunLevel") == 0) {
    append_shellcore_flag_token(dst, dst_size, "RUN_LEVEL_NUMERIC");
    append_shellcore_flag_token(dst, dst_size, "POLL_MASK=0xFFFFFFF");
    append_shellcore_flag_token(dst, dst_size, "INITIAL=10");
    append_shellcore_flag_token(dst, dst_size, "READY=100");
    return true;
  }

  if (strcmp(flag->name, "SceAutoMountUsbMass") == 0) {
    append_shellcore_flag_token(dst, dst_size, "USB_MOUNT_BITS=0xFFFFFFF");
    return true;
  }
#endif

  if (strcmp(flag->name, "SceSystemStateMgrInfo") == 0) {
    append_shellcore_flag_token(dst, dst_size,
                                "CURRENT_STATE=bits[0..15]");
    append_shellcore_flag_token(dst, dst_size,
                                "TRIGGER_CODE=bits[32..47]");
    append_shellcore_flag_token(dst, dst_size,
                                "REBOOT_CAUSE=bits[48..63]");
    append_shellcore_flag_token(dst, dst_size, "STATE_INVALID=0");
    append_shellcore_flag_token(dst, dst_size, "STATE_INITIALIZING=10");
    append_shellcore_flag_token(dst, dst_size,
                                "STATE_SHUTDOWN_ON_GOING=100");
    append_shellcore_flag_token(dst, dst_size, "STATE_POWER_SAVING=200");
    append_shellcore_flag_token(dst, dst_size,
                                "STATE_SUSPEND_ON_GOING=300");
    append_shellcore_flag_token(dst, dst_size,
                                "STATE_MAIN_ON_STANDBY=500");
    append_shellcore_flag_token(dst, dst_size, "STATE_WORKING=1000");
    append_shellcore_flag_token(dst, dst_size, "REBOOT_CAUSE_NONE=0");
    append_shellcore_flag_token(dst, dst_size, "REBOOT_CAUSE_CFF=1");
    append_shellcore_flag_token(dst, dst_size, "INITIAL=0x10000000A");
    return true;
  }

  return false;
}

static void format_shellcore_flag_known_masks(const shellcore_flag_monitor_t *flag,
                                              char *dst, size_t dst_size) {
  size_t bit_count = 0;
  const shellcore_flag_bit_desc_t *bits = get_shellcore_flag_bits(flag, &bit_count);

  if (!dst || dst_size == 0)
    return;
  dst[0] = '\0';

  (void)format_shellcore_flag_special_known_masks(flag, dst, dst_size);

  for (size_t i = 0; i < bit_count; ++i) {
    char token[96];
    (void)snprintf(token, sizeof(token), "%s=0x%llX", bits[i].name,
                   (unsigned long long)bits[i].mask);
    append_shellcore_flag_token(dst, dst_size, token);
  }

  if (strcmp(flag->name, "SceShellCoreUtilAppFocus") == 0) {
    append_shellcore_flag_token(dst, dst_size, "APP_ID");
  }
}

static bool format_shellcore_flag_special_value(const shellcore_flag_monitor_t *flag,
                                                uint64_t pattern, char *dst,
                                                size_t dst_size) {
  if (!flag || !flag->name || !dst || dst_size == 0)
    return false;

#if 0

  if (strcmp(flag->name, "SceShellCoreUtilRunLevel") == 0) {
    char token[96];
    (void)snprintf(token, sizeof(token), "RUN_LEVEL=%u",
                   (unsigned)(pattern & 0x0FFFFFFFULL));
    append_shellcore_flag_token(dst, dst_size, token);
    if ((pattern & 0x0FFFFFFFULL) == 10u) {
      append_shellcore_flag_token(dst, dst_size, "INITIAL");
    } else if ((pattern & 0x0FFFFFFFULL) == 100u) {
      append_shellcore_flag_token(dst, dst_size, "READY");
    }
    if ((pattern & ~0x0FFFFFFFULL) != 0) {
      (void)snprintf(token, sizeof(token), "UNKNOWN=0x%llX",
                     (unsigned long long)(pattern & ~0x0FFFFFFFULL));
      append_shellcore_flag_token(dst, dst_size, token);
    }
    return true;
  }

  if (strcmp(flag->name, "SceAutoMountUsbMass") == 0) {
    uint64_t usb_bits = pattern & 0x0FFFFFFFULL;
    for (unsigned i = 0; i < 28; ++i) {
      if ((usb_bits & (1ULL << i)) == 0)
        continue;
      char token[32];
      (void)snprintf(token, sizeof(token), "USB%u", i);
      append_shellcore_flag_token(dst, dst_size, token);
    }
    if ((pattern & ~0x0FFFFFFFULL) != 0) {
      char token[64];
      (void)snprintf(token, sizeof(token), "UNKNOWN=0x%llX",
                     (unsigned long long)(pattern & ~0x0FFFFFFFULL));
      append_shellcore_flag_token(dst, dst_size, token);
    }
    if (dst[0] == '\0')
      (void)snprintf(dst, dst_size, "0");
    return true;
  }
#endif

  if (strcmp(flag->name, "SceSystemStateMgrInfo") == 0) {
    unsigned current_state = (unsigned)(pattern & 0xFFFFu);
    unsigned mid16 = (unsigned)((pattern >> 16) & 0xFFFFu);
    unsigned trigger_code = (unsigned)((pattern >> 32) & 0xFFFFu);
    unsigned reboot_cause = (unsigned)((pattern >> 48) & 0xFFFFu);
    char token[128];

    (void)snprintf(token, sizeof(token), "STATE=%s(0x%04X)",
                   get_system_state_mgr_state_name(current_state), current_state);
    append_shellcore_flag_token(dst, dst_size, token);

    (void)snprintf(token, sizeof(token), "TRIGGER=0x%04X", trigger_code);
    append_shellcore_flag_token(dst, dst_size, token);

    (void)snprintf(token, sizeof(token), "REBOOT_CAUSE=%s(0x%04X)",
                   get_system_state_mgr_reboot_cause_name(reboot_cause),
                   reboot_cause);
    append_shellcore_flag_token(dst, dst_size, token);

    if (mid16 != 0) {
      (void)snprintf(token, sizeof(token), "MID16=0x%04X", mid16);
      append_shellcore_flag_token(dst, dst_size, token);
    }
    return true;
  }

  if (strcmp(flag->name, "SceShellCoreUtilAppFocus") == 0) {
    unsigned low32 = (unsigned)(pattern & 0xFFFFFFFFu);
    unsigned high32 = (unsigned)(pattern >> 32);
    char token[96];

    if (low32 != 0) {
      (void)snprintf(token, sizeof(token), "APP_ID=0x%08X", low32);
      append_shellcore_flag_token(dst, dst_size, token);
    }
    if (high32 != 0) {
      (void)snprintf(token, sizeof(token), "HIGH32=0x%08X", high32);
      append_shellcore_flag_token(dst, dst_size, token);
    }
    if (dst[0] == '\0')
      (void)snprintf(dst, dst_size, "0");
    return true;
  }

#if 0
  if (strcmp(flag->name, "SceShellCoreUtilUIStatus") == 0) {
    unsigned shape_field = (unsigned)((pattern >> 15) & 0x3u);
    if (shape_field != 0) {
      char token[96];
      (void)snprintf(token, sizeof(token), "BASIC_PRODUCT_SHAPE=%u",
                     shape_field - 1u);
      append_shellcore_flag_token(dst, dst_size, token);
    }
    return false;
  }
#endif

  return false;
}

static void format_shellcore_flag_set_bits(const shellcore_flag_monitor_t *flag,
                                           uint64_t pattern, char *dst,
                                           size_t dst_size) {
  size_t bit_count = 0;
  uint64_t known_mask = 0;
  const shellcore_flag_bit_desc_t *bits = get_shellcore_flag_bits(flag, &bit_count);

  if (!dst || dst_size == 0)
    return;
  dst[0] = '\0';

  if (format_shellcore_flag_special_value(flag, pattern, dst, dst_size) &&
      dst[0] != '\0') {
    return;
  }

  for (size_t i = 0; i < bit_count; ++i) {
    known_mask |= bits[i].mask;
    if ((pattern & bits[i].mask) == 0)
      continue;

    char token[96];
    (void)snprintf(token, sizeof(token), "%s", bits[i].name);
    append_shellcore_flag_token(dst, dst_size, token);
  }

  if (strcmp(flag->name, "SceShellCoreUtilAppFocus") == 0 &&
      (pattern >> 32) == 0 && pattern != 0) {
    char token[64];
    (void)snprintf(token, sizeof(token), "APP_ID=0x%08X", (unsigned)pattern);
    append_shellcore_flag_token(dst, dst_size, token);
  }

  if ((pattern & ~known_mask) != 0) {
    char token[64];
    (void)snprintf(token, sizeof(token), "UNKNOWN=0x%llX",
                   (unsigned long long)(pattern & ~known_mask));
    append_shellcore_flag_token(dst, dst_size, token);
  }

  if (dst[0] == '\0')
    (void)snprintf(dst, dst_size, "0");
}

static void set_shellcore_flag_start_result(bool success) {
  pthread_mutex_lock(&g_shellcore_flag_mutex);
  g_shellcore_flag_start_success = success;
  g_shellcore_flag_start_ready = true;
  pthread_cond_broadcast(&g_shellcore_flag_cond);
  pthread_mutex_unlock(&g_shellcore_flag_mutex);
}

static void reset_shellcore_flag_state(shellcore_flag_monitor_t *flag) {
  if (!flag)
    return;

  flag->handle = -1;
  flag->is_open = false;
  flag->has_last_pattern = false;
  flag->has_last_rc = false;
  flag->last_pattern = 0;
  flag->last_rc = 0;
}

static void log_shellcore_flag_pattern(const shellcore_flag_monitor_t *flag,
                                       uint64_t pattern) {
  char decoded[512];

  if (!flag)
    return;

  format_shellcore_flag_set_bits(flag, pattern, decoded, sizeof(decoded));

  if (!flag->has_last_pattern) {
    log_debug("  [SHELLFLAG] %s initial=0x%016llX low32=0x%08X high32=0x%08X "
              "decoded=%s",
              flag->name, (unsigned long long)pattern, (unsigned)(pattern),
              (unsigned)(pattern >> 32), decoded);
    return;
  }

  log_debug("  [SHELLFLAG] %s value=0x%016llX delta=0x%016llX low32=0x%08X "
            "high32=0x%08X decoded=%s",
            flag->name, (unsigned long long)pattern,
            (unsigned long long)(flag->last_pattern ^ pattern),
            (unsigned)(pattern), (unsigned)(pattern >> 32), decoded);
}

static void close_shellcore_flag(shellcore_flag_monitor_t *flag) {
  if (!flag || !flag->is_open)
    return;

  int rc = sceKernelCloseEventFlag(flag->handle);
  if (rc < 0) {
    log_debug("  [SHELLFLAG] close failed: %s handle=0x%016llX rc=0x%08X",
              flag->name, (unsigned long long)flag->handle, (unsigned)rc);
  }

  reset_shellcore_flag_state(flag);
}

static void close_shellcore_flags(void) {
  for (size_t i = 0; i < sizeof(g_shellcore_flags) / sizeof(g_shellcore_flags[0]);
       ++i) {
    close_shellcore_flag(&g_shellcore_flags[i]);
  }
}

static size_t open_shellcore_flags(void) {
  size_t opened_count = 0;

  for (size_t i = 0; i < sizeof(g_shellcore_flags) / sizeof(g_shellcore_flags[0]);
       ++i) {
    shellcore_flag_monitor_t *flag = &g_shellcore_flags[i];
    sm_kernel_event_flag_t handle = -1;
    int rc = sceKernelOpenEventFlag(&handle, flag->name);
    if (rc < 0) {
      log_debug("  [SHELLFLAG] open failed: %s rc=0x%08X", flag->name,
                (unsigned)rc);
      reset_shellcore_flag_state(flag);
      continue;
    }

    flag->handle = handle;
    flag->is_open = true;
    log_debug("  [SHELLFLAG] opened %s handle=0x%016llX", flag->name,
              (unsigned long long)flag->handle);

    {
      char known_masks[1024];
      format_shellcore_flag_known_masks(flag, known_masks, sizeof(known_masks));
      if (known_masks[0] != '\0') {
        log_debug("  [SHELLFLAG] known %s masks: %s", flag->name, known_masks);
      }
    }

    opened_count++;
  }

  return opened_count;
}

static void poll_shellcore_flag(shellcore_flag_monitor_t *flag) {
  uint64_t result_pattern = 0;
  int rc;
  bool changed = false;
  bool entered_shutdown_on_going = false;
  bool entered_resume_working = false;
  bool is_system_state_mgr_info = false;
  unsigned current_state = 0;
  unsigned previous_state = 0;

  if (!flag || !flag->is_open)
    return;

  rc = sceKernelPollEventFlag(flag->handle, SHELLCORE_FLAG_ALL_BITS,
                              SHELLCORE_FLAG_WAITMODE_OR, &result_pattern);
  if (rc < 0) {
    if (!flag->has_last_rc || flag->last_rc != rc) {
      log_debug("  [SHELLFLAG] %s poll rc=0x%08X", flag->name,
                (unsigned)rc);
    }
    flag->last_rc = rc;
    flag->has_last_rc = true;
    return;
  }

  if (!flag->has_last_rc || flag->last_rc != 0 || !flag->has_last_pattern ||
      flag->last_pattern != result_pattern) {
    log_shellcore_flag_pattern(flag, result_pattern);
    changed = flag->has_last_pattern && flag->last_pattern != result_pattern;
  }

  is_system_state_mgr_info = strcmp(flag->name, "SceSystemStateMgrInfo") == 0;
  if (is_system_state_mgr_info) {
    current_state = (unsigned)(result_pattern & 0xFFFFu);
    previous_state =
        flag->has_last_pattern ? (unsigned)(flag->last_pattern & 0xFFFFu) : 0;

    entered_shutdown_on_going =
        current_state == SYSTEM_STATE_MGR_STATE_SHUTDOWN_ON_GOING &&
        (!flag->has_last_pattern ||
         previous_state != SYSTEM_STATE_MGR_STATE_SHUTDOWN_ON_GOING);
    entered_resume_working =
        flag->has_last_pattern &&
        current_state == SYSTEM_STATE_MGR_STATE_WORKING &&
        previous_state != SYSTEM_STATE_MGR_STATE_WORKING;
  }

  flag->last_pattern = result_pattern;
  flag->has_last_pattern = true;
  flag->last_rc = 0;
  flag->has_last_rc = true;

  if (changed && strcmp(flag->name, "SceShellCoreUtilAppFocus") == 0) {
    sm_kstuff_note_app_focus((uint32_t)result_pattern);
    wake_game_lifecycle_watcher();
  }
  if (entered_shutdown_on_going) {
    request_shutdown_stop("SceSystemStateMgrInfo=SHUTDOWN_ON_GOING");
  }
  if (entered_resume_working) {
    request_scan_now("SceSystemStateMgrInfo=WORKING");
  }
}

static void *shellcore_flag_thread_main(void *arg) {
  (void)arg;

  size_t opened_count = open_shellcore_flags();
  // The active monitor currently depends on both configured flags opening:
  // AppFocus plus SceSystemStateMgrInfo.
  if (opened_count < 2) {
    if (opened_count != 0)
      close_shellcore_flags();
    set_shellcore_flag_start_result(false);
    return NULL;
  }

  for (size_t i = 0; i < sizeof(g_shellcore_flags) / sizeof(g_shellcore_flags[0]);
       ++i) {
    poll_shellcore_flag(&g_shellcore_flags[i]);
  }

  log_debug("  [SHELLFLAG] monitor started (%zu/%zu flags opened)", opened_count,
            sizeof(g_shellcore_flags) / sizeof(g_shellcore_flags[0]));
  set_shellcore_flag_start_result(true);

  while (!g_shellcore_flag_stop_requested && !should_stop_requested()) {
    for (size_t i = 0; i < sizeof(g_shellcore_flags) / sizeof(g_shellcore_flags[0]);
         ++i) {
      poll_shellcore_flag(&g_shellcore_flags[i]);
    }

    sceKernelUsleep(SHELLCORE_FLAG_POLL_INTERVAL_US);
  }

  close_shellcore_flags();
  return NULL;
}

bool sm_shellcore_flags_start(void) {
  int rc;

  if (g_shellcore_flag_thread_started)
    return g_shellcore_flag_start_success;

  g_shellcore_flag_stop_requested = 0;
  g_shellcore_flag_start_ready = false;
  g_shellcore_flag_start_success = false;
  close_shellcore_flags();

  rc = pthread_create(&g_shellcore_flag_thread, NULL, shellcore_flag_thread_main,
                      NULL);
  if (rc != 0) {
    log_debug("  [SHELLFLAG] pthread_create failed: rc=%d", rc);
    return false;
  }

  pthread_mutex_lock(&g_shellcore_flag_mutex);
  while (!g_shellcore_flag_start_ready)
    pthread_cond_wait(&g_shellcore_flag_cond, &g_shellcore_flag_mutex);
  pthread_mutex_unlock(&g_shellcore_flag_mutex);

  if (!g_shellcore_flag_start_success) {
    pthread_join(g_shellcore_flag_thread, NULL);
    close_shellcore_flags();
    return false;
  }

  g_shellcore_flag_thread_started = true;
  return true;
}

void sm_shellcore_flags_stop(void) {
  if (!g_shellcore_flag_thread_started)
    return;

  g_shellcore_flag_stop_requested = 1;
  pthread_join(g_shellcore_flag_thread, NULL);
  g_shellcore_flag_thread_started = false;
  g_shellcore_flag_start_ready = false;
  g_shellcore_flag_start_success = false;
  close_shellcore_flags();
}
