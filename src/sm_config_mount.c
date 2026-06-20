#include "sm_platform.h"
#include "sm_config_mount.h"
#include "sm_types.h"
#include "sm_limits.h"
#include "sm_l10n.h"
#include "sm_log.h"
#include "sm_mount_defs.h"
#include "sm_mount_device.h"
#include "sm_path_utils.h"
#include "sm_paths.h"

#include <stdatomic.h>

static const char *const k_default_scan_paths[] = SM_DEFAULT_SCAN_PATHS_INITIALIZER;

typedef struct {
  char filename[MAX_PATH];
  bool mount_read_only;
  bool mount_mode_valid;
  uint32_t sector_size;
  bool sector_size_valid;
  bool valid;
} image_mode_rule_t;

typedef struct {
  runtime_config_t cfg;
  char scan_path_storage[MAX_SCAN_PATHS][MAX_PATH];
  int scan_path_count;
  image_mode_rule_t image_mode_rules[MAX_IMAGE_MODE_RULES];
} runtime_config_state_t;

typedef struct {
  bool present;
  uint64_t inode;
  uint64_t size;
  uint64_t mtime_sec;
  uint64_t mtime_nsec;
  uint64_t ctime_sec;
  uint64_t ctime_nsec;
} config_file_stamp_t;

typedef enum {
  CONFIG_LOAD_OK = 0,
  CONFIG_LOAD_MISSING,
  CONFIG_LOAD_ERROR,
} config_load_status_t;

#define RUNTIME_CONFIG_ACTIVE_SLOT_COUNT 2
#define RUNTIME_CONFIG_PARSE_SLOT RUNTIME_CONFIG_ACTIVE_SLOT_COUNT

static runtime_config_state_t
    g_runtime_state_slots[RUNTIME_CONFIG_ACTIVE_SLOT_COUNT + 1];
static _Atomic int g_runtime_state_active_index = 0;
static atomic_bool g_runtime_cfg_ready = false;
static config_file_stamp_t g_config_file_stamp;

static char *trim_ascii(char *s);
static bool parse_ini_line(char *line, char **key_out, char **value_out);
static bool normalize_title_id_value(const char *value,
                                     char out[MAX_TITLE_ID]);
static config_load_status_t load_runtime_config_state(runtime_config_state_t *state);
static bool parse_u32_ini(const char *value, uint32_t *out);
static bool is_valid_sector_size(uint32_t size);
static bool add_global_fakelib_exclude_rule(runtime_config_state_t *state,
                                            const char *value);
static bool normalize_image_filename_value(const char *value,
                                           char out[MAX_PATH]);
static bool set_image_sector_rule(runtime_config_state_t *state,
                                  const char *value);
static bool parse_image_sector_rule_value(const char *value,
                                          char filename_out[MAX_PATH],
                                          uint32_t *sector_size_out);
static bool lookup_image_sector_override_in_file(const char *path,
                                                 const char *filename,
                                                 uint32_t *sector_size_out);
static bool upsert_image_sector_override_in_file(const char *path,
                                                 const char *filename,
                                                 uint32_t sector_size);
static void apply_firmware_runtime_overrides(runtime_config_state_t *state);

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

static bool parse_ini_line(char *line, char **key_out, char **value_out) {
  if (!line || !key_out || !value_out)
    return false;

  char *s = trim_ascii(line);
  if (s[0] == '\0' || s[0] == '#' || s[0] == ';' || s[0] == '[')
    return false;

  char *eq = strchr(s, '=');
  if (!eq)
    return false;

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
    return false;

  *key_out = key;
  *value_out = value;
  return true;
}

static void apply_firmware_runtime_overrides(runtime_config_state_t *state) {
  if (!state)
    return;

  if (sm_firmware_major_version() >= 12u) {
    state->cfg.app_install_all_enabled = true;
    state->cfg.app_install_all_forced = true;
    return;
  }

  state->cfg.app_install_all_forced = false;
}

static const runtime_config_state_t *active_runtime_state(void) {
  return &g_runtime_state_slots[atomic_load_explicit(&g_runtime_state_active_index,
                                                     memory_order_acquire)];
}

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

static void clear_runtime_scan_paths(runtime_config_state_t *state) {
  state->scan_path_count = 0;
  memset(state->scan_path_storage, 0, sizeof(state->scan_path_storage));
}

static bool add_runtime_scan_path(runtime_config_state_t *state,
                                  const char *path) {
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

  for (int i = 0; i < state->scan_path_count; i++) {
    if (strcmp(state->scan_path_storage[i], normalized) == 0)
      return true;
  }

  if (state->scan_path_count >= MAX_SCAN_PATHS)
    return false;

  (void)strlcpy(state->scan_path_storage[state->scan_path_count], normalized,
                sizeof(state->scan_path_storage[state->scan_path_count]));
  state->scan_path_count++;
  return true;
}

static void add_runtime_managed_scan_paths(runtime_config_state_t *state) {
  (void)add_runtime_scan_path(state, PFSC_IMAGE_MOUNT_BASE);
  (void)add_runtime_scan_path(state, IMAGE_MOUNT_BASE);
}

static void init_runtime_scan_paths_defaults(runtime_config_state_t *state) {
  clear_runtime_scan_paths(state);
  for (int i = 0; k_default_scan_paths[i] != NULL; i++)
    (void)add_runtime_scan_path(state, k_default_scan_paths[i]);
  add_runtime_managed_scan_paths(state);
}

static void init_runtime_config_defaults(runtime_config_state_t *state) {
  memset(state, 0, sizeof(*state));
  state->cfg.debug_enabled = true;
  state->cfg.quiet_mode = false;
  state->cfg.mount_read_only = (IMAGE_MOUNT_READ_ONLY != 0);
  state->cfg.force_mount = false;
  state->cfg.app_install_all_enabled = false;
  state->cfg.app_install_all_forced = false;
  state->cfg.backport_fakelib_enabled = true;
  state->cfg.global_fakelib_enabled = true;
  state->cfg.global_fakelib_mount_first = true;
  state->cfg.legacy_recursive_scan_forced = false;
  (void)strlcpy(state->cfg.global_fakelib_path, DEFAULT_GLOBAL_FAKELIB_PATH,
                sizeof(state->cfg.global_fakelib_path));
  state->cfg.scan_depth = DEFAULT_SCAN_DEPTH;
  state->cfg.scan_interval_us = DEFAULT_SCAN_INTERVAL_US;
  state->cfg.stability_wait_seconds = DEFAULT_STABILITY_WAIT_SECONDS;
  state->cfg.language_id = SM_LANGUAGE_AUTO;
  state->cfg.exfat_backend = default_exfat_backend();
  state->cfg.ufs_backend = default_ufs_backend();
  state->cfg.lvd_sector_exfat = LVD_SECTOR_SIZE_EXFAT;
  state->cfg.lvd_sector_ufs = LVD_SECTOR_SIZE_UFS;
  state->cfg.lvd_sector_pfs = LVD_SECTOR_SIZE_PFS;
  state->cfg.md_sector_exfat = MD_SECTOR_SIZE_EXFAT;
  state->cfg.md_sector_ufs = MD_SECTOR_SIZE_UFS;
  memset(state->image_mode_rules, 0, sizeof(state->image_mode_rules));
  init_runtime_scan_paths_defaults(state);
  apply_firmware_runtime_overrides(state);
}

static config_file_stamp_t read_config_file_stamp(void) {
  config_file_stamp_t stamp;
  memset(&stamp, 0, sizeof(stamp));

  struct stat st;
  if (stat(CONFIG_FILE, &st) != 0)
    return stamp;

  stamp.present = true;
  stamp.inode = (uint64_t)st.st_ino;
  stamp.size = (uint64_t)st.st_size;
  stamp.mtime_sec = (uint64_t)st.st_mtim.tv_sec;
  stamp.mtime_nsec = (uint64_t)st.st_mtim.tv_nsec;
  stamp.ctime_sec = (uint64_t)st.st_ctim.tv_sec;
  stamp.ctime_nsec = (uint64_t)st.st_ctim.tv_nsec;
  return stamp;
}

static bool config_file_stamp_equals(const config_file_stamp_t *a,
                                     const config_file_stamp_t *b) {
  return a->present == b->present && a->inode == b->inode &&
         a->size == b->size && a->mtime_sec == b->mtime_sec &&
         a->mtime_nsec == b->mtime_nsec && a->ctime_sec == b->ctime_sec &&
         a->ctime_nsec == b->ctime_nsec;
}

static void apply_reloadable_runtime_fields(runtime_config_state_t *dst,
                                            const runtime_config_state_t *src) {
  dst->cfg = src->cfg;
  dst->scan_path_count = src->scan_path_count;
  memcpy(dst->scan_path_storage, src->scan_path_storage,
         sizeof(dst->scan_path_storage));
  memcpy(dst->image_mode_rules, src->image_mode_rules,
         sizeof(dst->image_mode_rules));
}

static bool runtime_config_states_equal(const runtime_config_state_t *a,
                                        const runtime_config_state_t *b) {
  return memcmp(a, b, sizeof(*a)) == 0;
}

static void activate_runtime_config_state(int slot_index) {
  atomic_store_explicit(&g_runtime_state_active_index, slot_index,
                        memory_order_release);
  atomic_store_explicit(&g_runtime_cfg_ready, true, memory_order_release);
}

void ensure_runtime_config_ready(void) {
  if (atomic_load_explicit(&g_runtime_cfg_ready, memory_order_acquire))
    return;

  init_runtime_config_defaults(&g_runtime_state_slots[0]);
  activate_runtime_config_state(0);
  g_config_file_stamp = read_config_file_stamp();
}

const runtime_config_t *runtime_config(void) {
  ensure_runtime_config_ready();
  return &active_runtime_state()->cfg;
}

int get_scan_path_count(void) {
  ensure_runtime_config_ready();
  return active_runtime_state()->scan_path_count;
}

const char *get_scan_path(int index) {
  ensure_runtime_config_ready();
  const runtime_config_state_t *state = active_runtime_state();
  if (index < 0 || index >= state->scan_path_count)
    return NULL;
  return state->scan_path_storage[index];
}

uint32_t get_scan_depth_for_root(const char *scan_path) {
  uint32_t scan_depth = runtime_config()->scan_depth;
  if (scan_depth < MIN_SCAN_DEPTH)
    scan_depth = MIN_SCAN_DEPTH;
  if (is_pfsc_image_mount_base_or_child(scan_path))
    scan_depth++;
  return scan_depth;
}

bool get_image_mode_override(const char *filename, bool *mount_read_only_out) {
  ensure_runtime_config_ready();
  if (!filename || !mount_read_only_out)
    return false;

  const runtime_config_state_t *state = active_runtime_state();
  char normalized[MAX_PATH];
  if (!normalize_image_filename_value(filename, normalized))
    return false;

  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (!state->image_mode_rules[k].valid)
      continue;
    if (!state->image_mode_rules[k].mount_mode_valid)
      continue;
    if (strcasecmp(state->image_mode_rules[k].filename, normalized) != 0)
      continue;
    *mount_read_only_out = state->image_mode_rules[k].mount_read_only;
    return true;
  }

  return false;
}

bool get_image_sector_size_override(const char *filename,
                                    uint32_t *sector_size_out) {
  ensure_runtime_config_ready();
  if (!filename || !sector_size_out)
    return false;

  if (lookup_image_sector_override_in_file(AUTOTUNE_FILE, filename,
                                           sector_size_out)) {
    return true;
  }

  const runtime_config_state_t *state = active_runtime_state();
  char normalized[MAX_PATH];
  if (!normalize_image_filename_value(filename, normalized))
    return false;

  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (!state->image_mode_rules[k].valid)
      continue;
    if (!state->image_mode_rules[k].sector_size_valid)
      continue;
    if (strcasecmp(state->image_mode_rules[k].filename, normalized) != 0)
      continue;
    *sector_size_out = state->image_mode_rules[k].sector_size;
    return true;
  }

  return false;
}

bool is_global_fakelib_excluded_for_title(const char *title_id) {
  ensure_runtime_config_ready();

  char normalized[MAX_TITLE_ID];
  if (!normalize_title_id_value(title_id, normalized))
    return false;

  const runtime_config_t *cfg = runtime_config();
  for (uint32_t i = 0; i < cfg->global_fakelib_exclude_title_count; ++i) {
    if (strcmp(cfg->global_fakelib_exclude_title_ids[i], normalized) == 0)
      return true;
  }

  return false;
}

bool upsert_image_sector_size_autotune(const char *filename,
                                       uint32_t sector_size,
                                       uint32_t *sector_size_out) {
  if (sector_size_out)
    *sector_size_out = 0;
  if (!is_valid_sector_size(sector_size))
    return false;

  char normalized_filename[MAX_PATH];
  if (!normalize_image_filename_value(filename, normalized_filename))
    return false;

  if (!upsert_image_sector_override_in_file(AUTOTUNE_FILE, normalized_filename,
                                            sector_size)) {
    return false;
  }

  if (sector_size_out)
    *sector_size_out = sector_size;
  return true;
}

static bool normalize_title_id_value(const char *value,
                                     char out[MAX_TITLE_ID]) {
  if (!value || !out)
    return false;

  char local[MAX_TITLE_ID];
  if (strlcpy(local, value, sizeof(local)) >= sizeof(local))
    return false;
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

static bool normalize_image_filename_value(const char *value,
                                           char out[MAX_PATH]) {
  if (!value || !out)
    return false;

  char local[MAX_PATH];
  if (strlcpy(local, value, sizeof(local)) >= sizeof(local))
    return false;
  char *trimmed = trim_ascii(local);
  const char *filename = get_filename_component(trimmed);
  size_t len = strlen(filename);
  if (len == 0 || len >= MAX_PATH)
    return false;

  (void)strlcpy(out, filename, MAX_PATH);
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

static bool parse_image_sector_rule_value(const char *value,
                                          char filename_out[MAX_PATH],
                                          uint32_t *sector_size_out) {
  if (!value || !filename_out || !sector_size_out)
    return false;

  char local[MAX_PATH];
  if (strlcpy(local, value, sizeof(local)) >= sizeof(local))
    return false;

  char *sep = strrchr(local, ':');
  if (!sep)
    return false;
  *sep = '\0';

  char *filename = trim_ascii(local);
  char *sector_value = trim_ascii(sep + 1);
  if (!normalize_image_filename_value(filename, filename_out))
    return false;
  if (!parse_u32_ini(sector_value, sector_size_out) ||
      !is_valid_sector_size(*sector_size_out)) {
    return false;
  }

  return true;
}

static bool lookup_image_sector_override_in_file(const char *path,
                                                 const char *filename,
                                                 uint32_t *sector_size_out) {
  if (!path || !filename || !sector_size_out)
    return false;

  char normalized_filename[MAX_PATH];
  if (!normalize_image_filename_value(filename, normalized_filename))
    return false;

  FILE *f = fopen(path, "r");
  if (!f)
    return false;

  bool found = false;
  uint32_t last_sector_size = 0;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char *key = NULL;
    char *value = NULL;
    if (!parse_ini_line(line, &key, &value))
      continue;
    if (strcasecmp(key, "image_sector") != 0)
      continue;

    uint32_t sector_size = 0;
    char parsed_filename[MAX_PATH];
    if (!parse_image_sector_rule_value(value, parsed_filename, &sector_size))
      continue;
    if (strcasecmp(parsed_filename, normalized_filename) != 0)
      continue;

    last_sector_size = sector_size;
    found = true;
  }

  fclose(f);
  if (!found)
    return false;

  *sector_size_out = last_sector_size;
  return true;
}

static bool upsert_image_sector_override_in_file(const char *path,
                                                 const char *filename,
                                                 uint32_t sector_size) {
  if (!path || !filename || !is_valid_sector_size(sector_size))
    return false;

  char normalized_filename[MAX_PATH];
  if (!normalize_image_filename_value(filename, normalized_filename))
    return false;

  char temp_path[MAX_PATH];
  int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
  if (written <= 0 || (size_t)written >= sizeof(temp_path))
    return false;

  FILE *in = fopen(path, "r");
  FILE *out = fopen(temp_path, "w");
  if (!out) {
    log_debug("  [CFG] autotune temp open failed: %s (%s)", temp_path,
              strerror(errno));
    if (in)
      fclose(in);
    return false;
  }

  bool found = false;
  if (in) {
    char line[512];
    while (fgets(line, sizeof(line), in)) {
      char original[sizeof(line)];
      (void)strlcpy(original, line, sizeof(original));

      char *key = NULL;
      char *value = NULL;
      if (!parse_ini_line(line, &key, &value)) {
        if (fputs(original, out) == EOF)
          goto write_failed;
        continue;
      }

      if (strcasecmp(key, "image_sector") != 0) {
        if (fputs(original, out) == EOF)
          goto write_failed;
        continue;
      }

      uint32_t parsed_sector = 0;
      char parsed_filename[MAX_PATH];
      if (!parse_image_sector_rule_value(value, parsed_filename,
                                         &parsed_sector) ||
          strcasecmp(parsed_filename, normalized_filename) != 0) {
        if (fputs(original, out) == EOF)
          goto write_failed;
        continue;
      }

      if (!found) {
        if (fprintf(out, "image_sector=%s:%u\n", normalized_filename,
                    (unsigned)sector_size) < 0) {
          goto write_failed;
        }
        found = true;
      }
    }

    fclose(in);
    in = NULL;
  }

  if (!found &&
      fprintf(out, "image_sector=%s:%u\n", normalized_filename,
              (unsigned)sector_size) < 0) {
    goto write_failed;
  }

  if (fclose(out) != 0) {
    out = NULL;
    log_debug("  [CFG] autotune temp close failed: %s (%s)", temp_path,
              strerror(errno));
    unlink(temp_path);
    return false;
  }
  out = NULL;

  if (rename(temp_path, path) != 0) {
    log_debug("  [CFG] autotune replace failed: %s -> %s (%s)", temp_path, path,
              strerror(errno));
    unlink(temp_path);
    return false;
  }

  return true;

write_failed:
  log_debug("  [CFG] autotune temp write failed: %s (%s)", temp_path,
            strerror(errno));
  if (in)
    fclose(in);
  fclose(out);
  unlink(temp_path);
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

static bool set_image_mode_rule(runtime_config_state_t *state, const char *path,
                                bool mount_read_only) {
  char normalized[MAX_PATH];
  if (!normalize_image_filename_value(path, normalized))
    return false;

  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (!state->image_mode_rules[k].valid)
      continue;
    if (strcasecmp(state->image_mode_rules[k].filename, normalized) != 0)
      continue;
    state->image_mode_rules[k].mount_read_only = mount_read_only;
    state->image_mode_rules[k].mount_mode_valid = true;
    return true;
  }

  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (state->image_mode_rules[k].valid)
      continue;
    (void)strlcpy(state->image_mode_rules[k].filename, normalized,
                  sizeof(state->image_mode_rules[k].filename));
    state->image_mode_rules[k].mount_read_only = mount_read_only;
    state->image_mode_rules[k].mount_mode_valid = true;
    state->image_mode_rules[k].valid = true;
    return true;
  }

  return false;
}

static bool set_image_sector_rule(runtime_config_state_t *state,
                                  const char *value) {
  if (!state || !value)
    return false;

  char normalized[MAX_PATH];
  uint32_t sector_size = 0;
  if (!parse_image_sector_rule_value(value, normalized, &sector_size))
    return false;

  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (!state->image_mode_rules[k].valid)
      continue;
    if (strcasecmp(state->image_mode_rules[k].filename, normalized) != 0)
      continue;
    state->image_mode_rules[k].sector_size = sector_size;
    state->image_mode_rules[k].sector_size_valid = true;
    return true;
  }

  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (state->image_mode_rules[k].valid)
      continue;
    (void)strlcpy(state->image_mode_rules[k].filename, normalized,
                  sizeof(state->image_mode_rules[k].filename));
    state->image_mode_rules[k].sector_size = sector_size;
    state->image_mode_rules[k].sector_size_valid = true;
    state->image_mode_rules[k].valid = true;
    return true;
  }

  return false;
}

static bool add_global_fakelib_exclude_rule(runtime_config_state_t *state,
                                            const char *value) {
  char normalized[MAX_TITLE_ID];
  if (!normalize_title_id_value(value, normalized))
    return false;

  for (uint32_t i = 0; i < state->cfg.global_fakelib_exclude_title_count;
       ++i) {
    if (strcmp(state->cfg.global_fakelib_exclude_title_ids[i], normalized) == 0)
      return true;
  }

  if (state->cfg.global_fakelib_exclude_title_count >=
      MAX_FAKELIB_EXCLUDE_RULES) {
    return false;
  }

  uint32_t index = state->cfg.global_fakelib_exclude_title_count++;
  (void)strlcpy(state->cfg.global_fakelib_exclude_title_ids[index],
                normalized,
                sizeof(state->cfg.global_fakelib_exclude_title_ids[index]));
  return true;
}

static config_load_status_t load_runtime_config_state(runtime_config_state_t *state) {
  init_runtime_config_defaults(state);

  FILE *f = fopen(CONFIG_FILE, "r");
  if (!f) {
    if (errno != ENOENT) {
      log_debug("  [CFG] open failed: %s (%s)", CONFIG_FILE, strerror(errno));
      return CONFIG_LOAD_ERROR;
    } else {
      log_debug("  [CFG] not found, using defaults");
      return CONFIG_LOAD_MISSING;
    }
  }

  char line[512];
  int line_no = 0;
  bool has_custom_scanpaths = false;
  bool legacy_recursive_scan_requested = false;
  while (fgets(line, sizeof(line), f)) {
    line_no++;
    char *key = NULL;
    char *value = NULL;
    if (!parse_ini_line(line, &key, &value)) {
      char *trimmed = trim_ascii(line);
      if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';' ||
          trimmed[0] == '[') {
        continue;
      }
      log_debug("  [CFG] invalid line %d (missing '=')", line_no);
      continue;
    }

    bool bval = false;
    uint32_t u32 = 0;
    attach_backend_t backend = ATTACH_BACKEND_NONE;

    if (strcasecmp(key, "debug") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.debug_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "quiet_mode") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.quiet_mode = bval;
      continue;
    }

    if (strcasecmp(key, "mount_read_only") == 0 ||
        strcasecmp(key, "read_only") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.mount_read_only = bval;
      continue;
    }

    if (strcasecmp(key, "force_mount") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.force_mount = bval;
      continue;
    }

    if (strcasecmp(key, "language") == 0 || strcasecmp(key, "lang") == 0 ||
        strcasecmp(key, "locale") == 0) {
      int32_t language_id = SM_LANGUAGE_AUTO;
      if (!sm_l10n_parse_language_id(value, &language_id)) {
        log_debug("  [CFG] invalid language at line %d: %s=%s "
                  "(use auto or a supported locale like en-US, ru-RU)",
                  line_no, key, value);
        continue;
      }
      state->cfg.language_id = language_id;
      continue;
    }

    if (strcasecmp(key, "app_install_all") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.app_install_all_enabled = bval;
      state->cfg.app_install_all_forced = false;
      continue;
    }

    if (strcasecmp(key, "image_ro") == 0 ||
        strcasecmp(key, "image_rw") == 0) {
      bool rule_read_only = (strcasecmp(key, "image_ro") == 0);
      if (!set_image_mode_rule(state, value, rule_read_only)) {
        log_debug("  [CFG] invalid image mode rule at line %d: %s=%s", line_no,
                  key, value);
      }
      continue;
    }

    if (strcasecmp(key, "image_sector") == 0) {
      if (!set_image_sector_rule(state, value)) {
        log_debug("  [CFG] invalid image sector rule at line %d: %s=%s "
                  "(format: IMAGE_FILENAME:SECTOR_SIZE)",
                  line_no, key, value);
      }
      continue;
    }

    if (strcasecmp(key, "recursive_scan") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      if (bval)
        legacy_recursive_scan_requested = true;
      continue;
    }

    if (strcasecmp(key, "scan_depth") == 0) {
      if (!parse_u32_ini(value, &u32) || u32 < MIN_SCAN_DEPTH ||
          u32 > MAX_SCAN_DEPTH) {
        log_debug("  [CFG] invalid scan depth at line %d: %s=%s (range: %u..%u)",
                  line_no, key, value, (unsigned)MIN_SCAN_DEPTH,
                  (unsigned)MAX_SCAN_DEPTH);
        continue;
      }
      state->cfg.scan_depth = u32;
      continue;
    }

    if (strcasecmp(key, "backport_fakelib") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.backport_fakelib_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "global_fakelib") == 0 ||
        strcasecmp(key, "global_fakelib_enabled") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.global_fakelib_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "global_fakelib_path") == 0) {
      char local[MAX_PATH];
      if (strlcpy(local, value, sizeof(local)) >= sizeof(local)) {
        log_debug("  [CFG] invalid global fakelib path at line %d: %s=%s",
                  line_no, key, value);
        continue;
      }
      char *trimmed = trim_ascii(local);
      size_t len = strlen(trimmed);
      if (len == 0 || len >= MAX_PATH || trimmed[0] != '/') {
        log_debug("  [CFG] invalid global fakelib path at line %d: %s=%s",
                  line_no, key, value);
        continue;
      }
      while (len > 1 && trimmed[len - 1] == '/') {
        trimmed[len - 1] = '\0';
        len--;
      }
      (void)strlcpy(state->cfg.global_fakelib_path, trimmed,
                    sizeof(state->cfg.global_fakelib_path));
      continue;
    }

    if (strcasecmp(key, "global_fakelib_priority") == 0) {
      // Lower-priority overlay must be mounted first.
      if (strcasecmp(value, "game") == 0) {
        state->cfg.global_fakelib_mount_first = true;
      } else if (strcasecmp(value, "global") == 0) {
        state->cfg.global_fakelib_mount_first = false;
      } else {
        log_debug("  [CFG] invalid global fakelib priority at line %d: %s=%s "
                  "(use game or global)",
                  line_no, key, value);
        continue;
      }
      continue;
    }

    if (strcasecmp(key, "global_fakelib_exclude") == 0 ||
        strcasecmp(key, "global_fakelib_exclude_title") == 0) {
      if (!add_global_fakelib_exclude_rule(state, value)) {
        log_debug("  [CFG] invalid global fakelib exclude rule at line %d: "
                  "%s=%s",
                  line_no, key, value);
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
      state->cfg.scan_interval_us = u32 * 1000000u;
      continue;
    }

    if (strcasecmp(key, "stability_wait_seconds") == 0 ||
        strcasecmp(key, "stability_wait_sec") == 0) {
      if (!parse_u32_ini(value, &u32) || u32 > MAX_STABILITY_WAIT_SECONDS) {
        log_debug("  [CFG] invalid stability wait at line %d: %s=%s (max: %u)",
                  line_no, key, value, (unsigned)MAX_STABILITY_WAIT_SECONDS);
        continue;
      }
      state->cfg.stability_wait_seconds = u32;
      continue;
    }

    if (strcasecmp(key, "exfat_backend") == 0) {
      if (!parse_backend_ini(value, &backend)) {
        log_debug("  [CFG] invalid backend at line %d: %s=%s", line_no, key,
                  value);
        continue;
      }
      state->cfg.exfat_backend = backend;
      continue;
    }

    if (strcasecmp(key, "ufs_backend") == 0) {
      if (!parse_backend_ini(value, &backend)) {
        log_debug("  [CFG] invalid backend at line %d: %s=%s", line_no, key,
                  value);
        continue;
      }
      state->cfg.ufs_backend = backend;
      continue;
    }

    if (strcasecmp(key, "scanpath") == 0) {
      if (!has_custom_scanpaths) {
        clear_runtime_scan_paths(state);
        has_custom_scanpaths = true;
      }
      if (!add_runtime_scan_path(state, value)) {
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
      state->cfg.lvd_sector_exfat = u32;
    } else if (strcasecmp(key, "lvd_ufs_sector_size") == 0) {
      state->cfg.lvd_sector_ufs = u32;
    } else if (strcasecmp(key, "lvd_pfs_sector_size") == 0) {
      state->cfg.lvd_sector_pfs = u32;
    } else if (strcasecmp(key, "md_exfat_sector_size") == 0) {
      state->cfg.md_sector_exfat = u32;
    } else if (strcasecmp(key, "md_ufs_sector_size") == 0) {
      state->cfg.md_sector_ufs = u32;
    }
  }

  fclose(f);

  if (has_custom_scanpaths && state->scan_path_count == 0) {
    log_debug("  [CFG] no valid scanpath entries, using defaults");
    init_runtime_scan_paths_defaults(state);
  }
  add_runtime_managed_scan_paths(state);

  if (legacy_recursive_scan_requested) {
    state->cfg.scan_depth = 2u;
    state->cfg.legacy_recursive_scan_forced = true;
    log_debug("  [CFG] recursive_scan=1 is deprecated; forcing scan_depth=2");
  }

  apply_firmware_runtime_overrides(state);

  int image_rule_count = 0;
  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (state->image_mode_rules[k].valid)
      image_rule_count++;
  }

  log_debug("  [CFG] loaded: debug=%d quiet=%d ro=%d force=%d "
            "language=%s app_install_all=%d app_install_all_forced=%d scan_depth=%u "
            "legacy_recursive_scan_forced=%d backport_fakelib=%d "
            "global_fakelib=%d global_fakelib_priority=%s "
            "global_fakelib_path=%s global_fakelib_exclude=%u "
            "exfat_backend=%s ufs_backend=%s "
            "lvd_sec(exfat=%u ufs=%u pfs=%u) md_sec(exfat=%u ufs=%u) "
            "scan_interval_s=%u stability_wait_s=%u scan_paths=%d image_rules=%d",
            state->cfg.debug_enabled ? 1 : 0, state->cfg.quiet_mode ? 1 : 0,
            state->cfg.mount_read_only ? 1 : 0,
            state->cfg.force_mount ? 1 : 0,
            sm_l10n_language_name(state->cfg.language_id),
            state->cfg.app_install_all_enabled ? 1 : 0,
            state->cfg.app_install_all_forced ? 1 : 0, state->cfg.scan_depth,
            state->cfg.legacy_recursive_scan_forced ? 1 : 0,
            state->cfg.backport_fakelib_enabled ? 1 : 0,
            state->cfg.global_fakelib_enabled ? 1 : 0,
            state->cfg.global_fakelib_mount_first ? "game" : "global",
            state->cfg.global_fakelib_path,
            state->cfg.global_fakelib_exclude_title_count,
            attach_backend_name(state->cfg.exfat_backend),
            attach_backend_name(state->cfg.ufs_backend),
            state->cfg.lvd_sector_exfat, state->cfg.lvd_sector_ufs,
            state->cfg.lvd_sector_pfs, state->cfg.md_sector_exfat,
            state->cfg.md_sector_ufs, state->cfg.scan_interval_us / 1000000u,
            state->cfg.stability_wait_seconds, state->scan_path_count,
            image_rule_count);

  return CONFIG_LOAD_OK;
}

bool load_runtime_config(void) {
  bool loaded =
      load_runtime_config_state(&g_runtime_state_slots[0]) == CONFIG_LOAD_OK;
  activate_runtime_config_state(0);
  g_config_file_stamp = read_config_file_stamp();
  return loaded;
}

bool reload_runtime_config_if_changed(bool *reloaded_out) {
  ensure_runtime_config_ready();
  if (reloaded_out)
    *reloaded_out = false;

  config_file_stamp_t new_stamp = read_config_file_stamp();
  if (config_file_stamp_equals(&new_stamp, &g_config_file_stamp))
    return true;

  const runtime_config_state_t *current = active_runtime_state();
  runtime_config_state_t *parsed = &g_runtime_state_slots[RUNTIME_CONFIG_PARSE_SLOT];
  config_load_status_t status = load_runtime_config_state(parsed);
  if (status == CONFIG_LOAD_ERROR)
    return false;

  int current_slot = atomic_load_explicit(&g_runtime_state_active_index,
                                          memory_order_acquire);
  int candidate_slot = (current_slot == 0) ? 1 : 0;
  runtime_config_state_t *candidate = &g_runtime_state_slots[candidate_slot];
  memcpy(candidate, current, sizeof(*candidate));
  apply_reloadable_runtime_fields(candidate, parsed);

  g_config_file_stamp = new_stamp;
  if (runtime_config_states_equal(candidate, current))
    return true;

  activate_runtime_config_state(candidate_slot);
  if (reloaded_out)
    *reloaded_out = true;
  return true;
}
