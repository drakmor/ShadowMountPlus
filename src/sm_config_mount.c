#include "sm_platform.h"
#include "sm_config_mount.h"
#include "sm_types.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_mount_defs.h"
#include "sm_mount_device.h"
#include "sm_path_utils.h"
#include "sm_paths.h"

static const char *const k_default_scan_paths[] = SM_DEFAULT_SCAN_PATHS_INITIALIZER;

typedef struct {
  char filename[MAX_PATH];
  bool mount_read_only;
  bool valid;
} image_mode_rule_t;

typedef struct {
  char title_id[MAX_TITLE_ID];
  uint32_t delay_seconds;
  bool valid;
} kstuff_delay_rule_t;

static runtime_config_t g_runtime_cfg;
static bool g_runtime_cfg_ready = false;
static const char *g_scan_paths[MAX_SCAN_PATHS + 1];
static char g_scan_path_storage[MAX_SCAN_PATHS][MAX_PATH];
static int g_scan_path_count = 0;
static image_mode_rule_t g_image_mode_rules[MAX_IMAGE_MODE_RULES];
static char g_kstuff_no_pause_title_ids[MAX_KSTUFF_TITLE_RULES][MAX_TITLE_ID];
static int g_kstuff_no_pause_title_count = 0;
static kstuff_delay_rule_t g_kstuff_delay_rules[MAX_KSTUFF_TITLE_RULES];

static char *trim_ascii(char *s);
static bool normalize_title_id_value(const char *value,
                                     char out[MAX_TITLE_ID]);

static attach_backend_t default_exfat_backend(void) {
#if EXFAT_ATTACH_USE_MDCTL
  return ATTACH_BACKEND_MD;
#else
  return ATTACH_BACKEND_LVD;
#endif
}

static attach_backend_t default_ufs_backend(void) {
#if UFS_ATTACH_USE_MDCTL
  return ATTACH_BACKEND_MD;
#else
  return ATTACH_BACKEND_LVD;
#endif
}

static void clear_runtime_scan_paths(void) {
  g_scan_path_count = 0;
  memset(g_scan_paths, 0, sizeof(g_scan_paths));
  memset(g_scan_path_storage, 0, sizeof(g_scan_path_storage));
}

static bool add_runtime_scan_path(const char *path) {
  if (!path)
    return false;

  while (*path && isspace((unsigned char)*path))
    path++;

  size_t len = strlen(path);
  while (len > 0 && isspace((unsigned char)path[len - 1]))
    len--;
  if (len == 0 || len >= MAX_PATH)
    return false;

  char normalized[MAX_PATH];
  memcpy(normalized, path, len);
  normalized[len] = '\0';
  while (len > 1 && normalized[len - 1] == '/') {
    normalized[len - 1] = '\0';
    len--;
  }

  for (int i = 0; i < g_scan_path_count; i++) {
    if (g_scan_paths[i] && strcmp(g_scan_paths[i], normalized) == 0)
      return true;
  }

  if (g_scan_path_count >= MAX_SCAN_PATHS)
    return false;

  (void)strlcpy(g_scan_path_storage[g_scan_path_count], normalized,
                sizeof(g_scan_path_storage[g_scan_path_count]));
  g_scan_paths[g_scan_path_count] = g_scan_path_storage[g_scan_path_count];
  g_scan_path_count++;
  g_scan_paths[g_scan_path_count] = NULL;
  return true;
}

static void init_runtime_scan_paths_defaults(void) {
  clear_runtime_scan_paths();
  for (int i = 0; k_default_scan_paths[i] != NULL; i++)
    (void)add_runtime_scan_path(k_default_scan_paths[i]);
  (void)add_runtime_scan_path(IMAGE_MOUNT_BASE "/" IMAGE_MOUNT_SUBDIR_UFS);
  (void)add_runtime_scan_path(IMAGE_MOUNT_BASE "/" IMAGE_MOUNT_SUBDIR_EXFAT);
  (void)add_runtime_scan_path(IMAGE_MOUNT_BASE "/" IMAGE_MOUNT_SUBDIR_PFS);
}

static void clear_kstuff_title_rules(void) {
  g_kstuff_no_pause_title_count = 0;
  memset(g_kstuff_no_pause_title_ids, 0, sizeof(g_kstuff_no_pause_title_ids));
  memset(g_kstuff_delay_rules, 0, sizeof(g_kstuff_delay_rules));
}

static void init_runtime_config_defaults(void) {
  g_runtime_cfg.debug_enabled = true;
  g_runtime_cfg.quiet_mode = false;
  g_runtime_cfg.mount_read_only = (IMAGE_MOUNT_READ_ONLY != 0);
  g_runtime_cfg.force_mount = false;
  g_runtime_cfg.recursive_scan = false;
  g_runtime_cfg.backport_fakelib_enabled = true;
  g_runtime_cfg.kstuff_game_auto_toggle = true;
  g_runtime_cfg.scan_interval_us = DEFAULT_SCAN_INTERVAL_US;
  g_runtime_cfg.stability_wait_seconds = DEFAULT_STABILITY_WAIT_SECONDS;
  g_runtime_cfg.kstuff_pause_delay_image_seconds =
      DEFAULT_KSTUFF_PAUSE_DELAY_IMAGE_SECONDS;
  g_runtime_cfg.kstuff_pause_delay_direct_seconds =
      DEFAULT_KSTUFF_PAUSE_DELAY_DIRECT_SECONDS;
  g_runtime_cfg.exfat_backend = default_exfat_backend();
  g_runtime_cfg.ufs_backend = default_ufs_backend();
  g_runtime_cfg.lvd_sector_exfat = LVD_SECTOR_SIZE_EXFAT;
  g_runtime_cfg.lvd_sector_ufs = LVD_SECTOR_SIZE_UFS;
  g_runtime_cfg.lvd_sector_pfs = LVD_SECTOR_SIZE_PFS;
  g_runtime_cfg.md_sector_exfat = MD_SECTOR_SIZE_EXFAT;
  g_runtime_cfg.md_sector_ufs = MD_SECTOR_SIZE_UFS;
  memset(g_image_mode_rules, 0, sizeof(g_image_mode_rules));
  clear_kstuff_title_rules();
  init_runtime_scan_paths_defaults();
  g_runtime_cfg_ready = true;
}

void ensure_runtime_config_ready(void) {
  if (!g_runtime_cfg_ready)
    init_runtime_config_defaults();
}

const runtime_config_t *runtime_config(void) {
  ensure_runtime_config_ready();
  return &g_runtime_cfg;
}

int get_scan_path_count(void) {
  ensure_runtime_config_ready();
  return g_scan_path_count;
}

const char *get_scan_path(int index) {
  ensure_runtime_config_ready();
  if (index < 0 || index >= g_scan_path_count)
    return NULL;
  return g_scan_paths[index];
}

bool get_image_mode_override(const char *filename, bool *mount_read_only_out) {
  ensure_runtime_config_ready();
  if (!filename || !mount_read_only_out)
    return false;

  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (!g_image_mode_rules[k].valid)
      continue;
    if (strcasecmp(g_image_mode_rules[k].filename, filename) != 0)
      continue;
    *mount_read_only_out = g_image_mode_rules[k].mount_read_only;
    return true;
  }

  return false;
}

bool is_kstuff_pause_disabled_for_title(const char *title_id) {
  ensure_runtime_config_ready();

  char normalized[MAX_TITLE_ID];
  if (!normalize_title_id_value(title_id, normalized))
    return false;

  for (int i = 0; i < g_kstuff_no_pause_title_count; ++i) {
    if (strcmp(g_kstuff_no_pause_title_ids[i], normalized) == 0)
      return true;
  }

  return false;
}

bool get_kstuff_pause_delay_override_for_title(const char *title_id,
                                               uint32_t *delay_seconds_out) {
  ensure_runtime_config_ready();
  if (!delay_seconds_out)
    return false;

  char normalized[MAX_TITLE_ID];
  if (!normalize_title_id_value(title_id, normalized))
    return false;

  for (int i = 0; i < MAX_KSTUFF_TITLE_RULES; ++i) {
    if (!g_kstuff_delay_rules[i].valid)
      continue;
    if (strcmp(g_kstuff_delay_rules[i].title_id, normalized) != 0)
      continue;
    *delay_seconds_out = g_kstuff_delay_rules[i].delay_seconds;
    return true;
  }

  return false;
}

static char *trim_ascii(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
    s++;
  size_t n = strlen(s);
  while (n > 0) {
    char c = s[n - 1];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      break;
    s[n - 1] = '\0';
    n--;
  }
  return s;
}

static bool normalize_title_id_value(const char *value,
                                     char out[MAX_TITLE_ID]) {
  if (!value || !out)
    return false;

  char local[MAX_TITLE_ID];
  (void)strlcpy(local, value, sizeof(local));
  char *trimmed = trim_ascii(local);
  size_t len = strlen(trimmed);
  if (len == 0 || len >= MAX_TITLE_ID)
    return false;

  for (size_t i = 0; i < len; ++i) {
    unsigned char ch = (unsigned char)trimmed[i];
    if (!isalnum(ch))
      return false;
    out[i] = (char)toupper(ch);
  }

  out[len] = '\0';
  return true;
}

static bool parse_bool_ini(const char *value, bool *out) {
  if (!value || !out)
    return false;
  if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
      strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0 ||
      strcasecmp(value, "ro") == 0) {
    *out = true;
    return true;
  }
  if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
      strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0 ||
      strcasecmp(value, "rw") == 0) {
    *out = false;
    return true;
  }
  return false;
}

static bool parse_backend_ini(const char *value, attach_backend_t *out) {
  if (!value || !out)
    return false;
  if (strcasecmp(value, "lvd") == 0) {
    *out = ATTACH_BACKEND_LVD;
    return true;
  }
  if (strcasecmp(value, "md") == 0 || strcasecmp(value, "mdctl") == 0) {
    *out = ATTACH_BACKEND_MD;
    return true;
  }
  return false;
}

static bool parse_u32_ini(const char *value, uint32_t *out) {
  if (!value || !out)
    return false;
  errno = 0;
  char *end = NULL;
  unsigned long v = strtoul(value, &end, 0);
  if (errno != 0 || end == value || *end != '\0' || v > UINT32_MAX)
    return false;
  *out = (uint32_t)v;
  return true;
}

static bool is_valid_sector_size(uint32_t size) {
  if (size < 512u || size > 1024u * 1024u)
    return false;
  return (size & (size - 1u)) == 0u;
}

static bool set_image_mode_rule(const char *path, bool mount_read_only) {
  const char *filename = get_filename_component(path);
  if (filename[0] == '\0')
    return false;

  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (!g_image_mode_rules[k].valid)
      continue;
    if (strcasecmp(g_image_mode_rules[k].filename, filename) != 0)
      continue;
    g_image_mode_rules[k].mount_read_only = mount_read_only;
    return true;
  }

  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (g_image_mode_rules[k].valid)
      continue;
    (void)strlcpy(g_image_mode_rules[k].filename, filename,
                  sizeof(g_image_mode_rules[k].filename));
    g_image_mode_rules[k].mount_read_only = mount_read_only;
    g_image_mode_rules[k].valid = true;
    return true;
  }

  return false;
}

static bool add_kstuff_no_pause_title_rule(const char *value) {
  char normalized[MAX_TITLE_ID];
  if (!normalize_title_id_value(value, normalized))
    return false;

  for (int i = 0; i < g_kstuff_no_pause_title_count; ++i) {
    if (strcmp(g_kstuff_no_pause_title_ids[i], normalized) == 0)
      return true;
  }

  if (g_kstuff_no_pause_title_count >= MAX_KSTUFF_TITLE_RULES)
    return false;

  (void)strlcpy(g_kstuff_no_pause_title_ids[g_kstuff_no_pause_title_count],
                normalized,
                sizeof(g_kstuff_no_pause_title_ids[g_kstuff_no_pause_title_count]));
  g_kstuff_no_pause_title_count++;
  return true;
}

static bool set_kstuff_pause_delay_override_rule(const char *value) {
  if (!value)
    return false;

  char local[128];
  (void)strlcpy(local, value, sizeof(local));

  char *sep = strchr(local, ':');
  if (!sep)
    return false;
  *sep = '\0';

  char *title_id = trim_ascii(local);
  char *delay_value = trim_ascii(sep + 1);
  char normalized[MAX_TITLE_ID];
  uint32_t delay_seconds = 0;

  if (!normalize_title_id_value(title_id, normalized))
    return false;
  if (!parse_u32_ini(delay_value, &delay_seconds) ||
      delay_seconds > MAX_KSTUFF_PAUSE_DELAY_SECONDS) {
    return false;
  }

  for (int i = 0; i < MAX_KSTUFF_TITLE_RULES; ++i) {
    if (!g_kstuff_delay_rules[i].valid)
      continue;
    if (strcmp(g_kstuff_delay_rules[i].title_id, normalized) != 0)
      continue;
    g_kstuff_delay_rules[i].delay_seconds = delay_seconds;
    return true;
  }

  for (int i = 0; i < MAX_KSTUFF_TITLE_RULES; ++i) {
    if (g_kstuff_delay_rules[i].valid)
      continue;
    (void)strlcpy(g_kstuff_delay_rules[i].title_id, normalized,
                  sizeof(g_kstuff_delay_rules[i].title_id));
    g_kstuff_delay_rules[i].delay_seconds = delay_seconds;
    g_kstuff_delay_rules[i].valid = true;
    return true;
  }

  return false;
}

bool load_runtime_config(void) {
  init_runtime_config_defaults();

  FILE *f = fopen(CONFIG_FILE, "r");
  if (!f) {
    if (errno != ENOENT) {
      log_debug("  [CFG] open failed: %s (%s)", CONFIG_FILE, strerror(errno));
    } else {
      log_debug("  [CFG] not found, using defaults");
    }
    return false;
  }

  char line[512];
  int line_no = 0;
  bool has_custom_scanpaths = false;
  while (fgets(line, sizeof(line), f)) {
    line_no++;
    char *s = trim_ascii(line);
    if (s[0] == '\0' || s[0] == '#' || s[0] == ';' || s[0] == '[')
      continue;

    char *eq = strchr(s, '=');
    if (!eq) {
      log_debug("  [CFG] invalid line %d (missing '=')", line_no);
      continue;
    }
    *eq = '\0';
    char *key = trim_ascii(s);
    char *value = trim_ascii(eq + 1);

    char *comment = strchr(value, '#');
    if (comment) {
      *comment = '\0';
      value = trim_ascii(value);
    }
    comment = strchr(value, ';');
    if (comment) {
      *comment = '\0';
      value = trim_ascii(value);
    }

    if (key[0] == '\0' || value[0] == '\0')
      continue;

    bool bval = false;
    uint32_t u32 = 0;
    attach_backend_t backend = ATTACH_BACKEND_NONE;

    if (strcasecmp(key, "debug") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      g_runtime_cfg.debug_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "quiet_mode") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      g_runtime_cfg.quiet_mode = bval;
      continue;
    }

    if (strcasecmp(key, "mount_read_only") == 0 ||
        strcasecmp(key, "read_only") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      g_runtime_cfg.mount_read_only = bval;
      continue;
    }

    if (strcasecmp(key, "force_mount") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      g_runtime_cfg.force_mount = bval;
      continue;
    }

    if (strcasecmp(key, "image_ro") == 0 ||
        strcasecmp(key, "image_rw") == 0) {
      bool rule_read_only = (strcasecmp(key, "image_ro") == 0);
      if (!set_image_mode_rule(value, rule_read_only)) {
        log_debug("  [CFG] invalid image mode rule at line %d: %s=%s", line_no,
                  key, value);
      }
      continue;
    }

    if (strcasecmp(key, "recursive_scan") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      g_runtime_cfg.recursive_scan = bval;
      continue;
    }

    if (strcasecmp(key, "backport_fakelib") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      g_runtime_cfg.backport_fakelib_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "kstuff_game_auto_toggle") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      g_runtime_cfg.kstuff_game_auto_toggle = bval;
      continue;
    }

    if (strcasecmp(key, "kstuff_no_pause") == 0) {
      if (!add_kstuff_no_pause_title_rule(value)) {
        log_debug("  [CFG] invalid kstuff no-pause title rule at line %d: "
                  "%s=%s", line_no, key, value);
      }
      continue;
    }

    if (strcasecmp(key, "kstuff_delay") == 0) {
      if (!set_kstuff_pause_delay_override_rule(value)) {
        log_debug("  [CFG] invalid kstuff pause override at line %d: %s=%s "
                  "(format: TITLEID:SECONDS, max: %u)",
                  line_no, key, value,
                  (unsigned)MAX_KSTUFF_PAUSE_DELAY_SECONDS);
      }
      continue;
    }

    if (strcasecmp(key, "scan_interval_seconds") == 0 ||
        strcasecmp(key, "scan_interval_sec") == 0) {
      if (!parse_u32_ini(value, &u32) || u32 < MIN_SCAN_INTERVAL_SECONDS ||
          u32 > MAX_SCAN_INTERVAL_SECONDS) {
        log_debug("  [CFG] invalid scan interval at line %d: %s=%s (range: %u..%u)",
                  line_no, key, value, (unsigned)MIN_SCAN_INTERVAL_SECONDS,
                  (unsigned)MAX_SCAN_INTERVAL_SECONDS);
        continue;
      }
      g_runtime_cfg.scan_interval_us = u32 * 1000000u;
      continue;
    }

    if (strcasecmp(key, "stability_wait_seconds") == 0 ||
        strcasecmp(key, "stability_wait_sec") == 0) {
      if (!parse_u32_ini(value, &u32) || u32 > MAX_STABILITY_WAIT_SECONDS) {
        log_debug("  [CFG] invalid stability wait at line %d: %s=%s (max: %u)",
                  line_no, key, value, (unsigned)MAX_STABILITY_WAIT_SECONDS);
        continue;
      }
      g_runtime_cfg.stability_wait_seconds = u32;
      continue;
    }

    if (strcasecmp(key, "kstuff_pause_delay_image_seconds") == 0 ||
        strcasecmp(key, "kstuff_pause_delay_image_sec") == 0) {
      if (!parse_u32_ini(value, &u32) || u32 > MAX_KSTUFF_PAUSE_DELAY_SECONDS) {
        log_debug("  [CFG] invalid image kstuff pause delay at line %d: %s=%s "
                  "(max: %u)", line_no, key, value,
                  (unsigned)MAX_KSTUFF_PAUSE_DELAY_SECONDS);
        continue;
      }
      g_runtime_cfg.kstuff_pause_delay_image_seconds = u32;
      continue;
    }

    if (strcasecmp(key, "kstuff_pause_delay_direct_seconds") == 0 ||
        strcasecmp(key, "kstuff_pause_delay_direct_sec") == 0) {
      if (!parse_u32_ini(value, &u32) || u32 > MAX_KSTUFF_PAUSE_DELAY_SECONDS) {
        log_debug("  [CFG] invalid direct kstuff pause delay at line %d: %s=%s "
                  "(max: %u)", line_no, key, value,
                  (unsigned)MAX_KSTUFF_PAUSE_DELAY_SECONDS);
        continue;
      }
      g_runtime_cfg.kstuff_pause_delay_direct_seconds = u32;
      continue;
    }

    if (strcasecmp(key, "exfat_backend") == 0) {
      if (!parse_backend_ini(value, &backend)) {
        log_debug("  [CFG] invalid backend at line %d: %s=%s", line_no, key,
                  value);
        continue;
      }
      g_runtime_cfg.exfat_backend = backend;
      continue;
    }

    if (strcasecmp(key, "ufs_backend") == 0) {
      if (!parse_backend_ini(value, &backend)) {
        log_debug("  [CFG] invalid backend at line %d: %s=%s", line_no, key,
                  value);
        continue;
      }
      g_runtime_cfg.ufs_backend = backend;
      continue;
    }

    if (strcasecmp(key, "scanpath") == 0) {
      if (!has_custom_scanpaths) {
        clear_runtime_scan_paths();
        has_custom_scanpaths = true;
      }
      if (!add_runtime_scan_path(value)) {
        log_debug("  [CFG] invalid scanpath at line %d: %s=%s", line_no, key,
                  value);
      }
      continue;
    }

    bool is_sector_key =
        (strcasecmp(key, "lvd_exfat_sector_size") == 0) ||
        (strcasecmp(key, "lvd_ufs_sector_size") == 0) ||
        (strcasecmp(key, "lvd_pfs_sector_size") == 0) ||
        (strcasecmp(key, "md_exfat_sector_size") == 0) ||
        (strcasecmp(key, "md_ufs_sector_size") == 0);

    if (!is_sector_key) {
      log_debug("  [CFG] unknown key at line %d: %s", line_no, key);
      continue;
    }

    if (!parse_u32_ini(value, &u32) || !is_valid_sector_size(u32)) {
      log_debug("  [CFG] invalid sector size at line %d: %s=%s", line_no, key,
                value);
      continue;
    }

    if (strcasecmp(key, "lvd_exfat_sector_size") == 0) {
      g_runtime_cfg.lvd_sector_exfat = u32;
    } else if (strcasecmp(key, "lvd_ufs_sector_size") == 0) {
      g_runtime_cfg.lvd_sector_ufs = u32;
    } else if (strcasecmp(key, "lvd_pfs_sector_size") == 0) {
      g_runtime_cfg.lvd_sector_pfs = u32;
    } else if (strcasecmp(key, "md_exfat_sector_size") == 0) {
      g_runtime_cfg.md_sector_exfat = u32;
    } else if (strcasecmp(key, "md_ufs_sector_size") == 0) {
      g_runtime_cfg.md_sector_ufs = u32;
    }
  }

  fclose(f);

  if (has_custom_scanpaths && g_scan_path_count == 0) {
    log_debug("  [CFG] no valid scanpath entries, using defaults");
    init_runtime_scan_paths_defaults();
  } else {
    (void)add_runtime_scan_path(IMAGE_MOUNT_BASE "/" IMAGE_MOUNT_SUBDIR_UFS);
    (void)add_runtime_scan_path(IMAGE_MOUNT_BASE "/" IMAGE_MOUNT_SUBDIR_EXFAT);
    (void)add_runtime_scan_path(IMAGE_MOUNT_BASE "/" IMAGE_MOUNT_SUBDIR_PFS);
  }

  int image_rule_count = 0;
  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (g_image_mode_rules[k].valid)
      image_rule_count++;
  }

  int kstuff_delay_rule_count = 0;
  for (int k = 0; k < MAX_KSTUFF_TITLE_RULES; k++) {
    if (g_kstuff_delay_rules[k].valid)
      kstuff_delay_rule_count++;
  }

  log_debug("  [CFG] loaded: debug=%d quiet=%d ro=%d force=%d recursive_scan=%d "
            "backport_fakelib=%d kstuff_game_auto_toggle=%d "
            "kstuff_pause_delay_image_s=%u kstuff_pause_delay_direct_s=%u "
            "exfat_backend=%s ufs_backend=%s "
            "lvd_sec(exfat=%u ufs=%u pfs=%u) md_sec(exfat=%u ufs=%u) "
            "scan_interval_s=%u stability_wait_s=%u scan_paths=%d image_rules=%d "
            "kstuff_no_pause=%d kstuff_delay_rules=%d",
            g_runtime_cfg.debug_enabled ? 1 : 0,
            g_runtime_cfg.quiet_mode ? 1 : 0,
            g_runtime_cfg.mount_read_only ? 1 : 0,
            g_runtime_cfg.force_mount ? 1 : 0,
            g_runtime_cfg.recursive_scan ? 1 : 0,
            g_runtime_cfg.backport_fakelib_enabled ? 1 : 0,
            g_runtime_cfg.kstuff_game_auto_toggle ? 1 : 0,
            g_runtime_cfg.kstuff_pause_delay_image_seconds,
            g_runtime_cfg.kstuff_pause_delay_direct_seconds,
            attach_backend_name(g_runtime_cfg.exfat_backend),
            attach_backend_name(g_runtime_cfg.ufs_backend),
            g_runtime_cfg.lvd_sector_exfat, g_runtime_cfg.lvd_sector_ufs,
            g_runtime_cfg.lvd_sector_pfs, g_runtime_cfg.md_sector_exfat,
            g_runtime_cfg.md_sector_ufs,
            g_runtime_cfg.scan_interval_us / 1000000u,
            g_runtime_cfg.stability_wait_seconds, g_scan_path_count,
            image_rule_count, g_kstuff_no_pause_title_count,
            kstuff_delay_rule_count);

  return true;
}
