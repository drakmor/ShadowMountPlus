#include "sm_platform.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>

#include "sm_api_protocol.h"
#include "sm_config_mount.h"
#include "sm_gameinfo.h"
#include "sm_types.h"
#include "sm_limits.h"
#include "sm_l10n.h"
#include "sm_log.h"
#include "sm_mount_defs.h"
#include "sm_mount_device.h"
#include "sm_path_utils.h"
#include "sm_paths.h"

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
  int custom_scan_path_count;
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

static runtime_config_state_t g_runtime_state;
static runtime_config_state_t g_runtime_parse_state;
static pthread_once_t g_runtime_init_once = PTHREAD_ONCE_INIT;
// Never hold the state mutex across I/O or logging: those can read config.
static pthread_mutex_t g_runtime_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_runtime_load_mutex = PTHREAD_MUTEX_INITIALIZER;
static config_file_stamp_t g_config_file_stamp;
static pthread_mutex_t g_autotune_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_config_file_mutex = PTHREAD_MUTEX_INITIALIZER;

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
static bool normalize_absolute_path_value(const char *value,
                                          char out[MAX_PATH]);
static bool normalize_http_url_value(const char *value, char out[MAX_PATH]);
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

static bool web_managed_config_key(const char *key) {
  return strcasecmp(key, "debug") == 0 ||
         strcasecmp(key, "quiet_mode") == 0 ||
         strcasecmp(key, "update_emulators") == 0 ||
         strcasecmp(key, "fakelib_default_mode") == 0 ||
         strcasecmp(key, "auto_update_ampr") == 0 ||
         strcasecmp(key, "auto_remove_missing_games") == 0 ||
         strcasecmp(key, "auto_remove_missing_delay_seconds") == 0 ||
         strcasecmp(key, "auto_remove_missing_delay_sec") == 0 ||
         strcasecmp(key, "fan_target_temperature") == 0 ||
         strcasecmp(key, "api_bind_address") == 0 ||
         strcasecmp(key, "api_bind_adress") == 0 ||
         strcasecmp(key, "scanpath") == 0;
}

static int config_io_error(void) {
  return errno != 0 ? errno : EIO;
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
  state->custom_scan_path_count = 0;
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
  state->cfg.api_enabled = true;
  state->cfg.debug_enabled = true;
  state->cfg.quiet_mode = false;
  state->cfg.mount_read_only = (IMAGE_MOUNT_READ_ONLY != 0);
  state->cfg.force_mount = false;
  state->cfg.persistent_image_mounts = false;
  state->cfg.app_install_all_enabled = false;
  state->cfg.auto_remove_missing_games = false;
  state->cfg.auto_remove_games_with_dlc = false;
  state->cfg.backport_fakelib_enabled = true;
  state->cfg.fakelib_default_mode = SM_FAKELIB_FULL;
  state->cfg.global_fakelib_enabled = true;
  state->cfg.global_fakelib_game_priority = true;
  state->cfg.update_emulators_enabled = true;
  state->cfg.auto_update_ampr_enabled = false;
  state->cfg.legacy_recursive_scan_forced = false;
  (void)strlcpy(state->cfg.api_bind_address, SM_API_DEFAULT_BIND_ADDRESS,
                sizeof(state->cfg.api_bind_address));
  state->cfg.api_port = SM_API_DEFAULT_PORT;
  (void)strlcpy(state->cfg.global_fakelib_path, DEFAULT_GLOBAL_FAKELIB_PATH,
                sizeof(state->cfg.global_fakelib_path));
  (void)strlcpy(state->cfg.emulators_path, DEFAULT_EMULATORS_PATH,
                sizeof(state->cfg.emulators_path));
  (void)strlcpy(state->cfg.ampr_update_url, DEFAULT_AMPR_UPDATE_URL,
                sizeof(state->cfg.ampr_update_url));
  state->cfg.scan_depth = DEFAULT_SCAN_DEPTH;
  state->cfg.scan_interval_us = DEFAULT_SCAN_INTERVAL_US;
  state->cfg.stability_wait_seconds = DEFAULT_STABILITY_WAIT_SECONDS;
  state->cfg.auto_remove_missing_delay_seconds =
      DEFAULT_AUTO_REMOVE_MISSING_DELAY_SECONDS;
  state->cfg.fan_target_temperature_c = FAN_TARGET_TEMPERATURE_SYSTEM;
  state->cfg.language_id = SM_LANGUAGE_AUTO;
  state->cfg.exfat_backend = default_exfat_backend();
  state->cfg.ufs_backend = default_ufs_backend();
  state->cfg.nested_pfs_index_cache_enabled = false;
  state->cfg.lvd_sector_exfat = LVD_SECTOR_SIZE_EXFAT;
  state->cfg.lvd_sector_ufs = LVD_SECTOR_SIZE_UFS;
  state->cfg.lvd_sector_pfs = LVD_SECTOR_SIZE_PFS;
  state->cfg.md_sector_exfat = MD_SECTOR_SIZE_EXFAT;
  state->cfg.md_sector_ufs = MD_SECTOR_SIZE_UFS_OPTIMIZED;
  memset(state->image_mode_rules, 0, sizeof(state->image_mode_rules));
  init_runtime_scan_paths_defaults(state);
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

static bool runtime_config_states_equal(const runtime_config_state_t *a,
                                        const runtime_config_state_t *b) {
  return memcmp(a, b, sizeof(*a)) == 0;
}

static void init_runtime_config(void) {
  init_runtime_config_defaults(&g_runtime_state);
  g_config_file_stamp = read_config_file_stamp();
}

void ensure_runtime_config_ready(void) {
  (void)pthread_once(&g_runtime_init_once, init_runtime_config);
}

runtime_config_t runtime_config(void) {
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_runtime_state_mutex);
  runtime_config_t cfg = g_runtime_state.cfg;
  pthread_mutex_unlock(&g_runtime_state_mutex);
  return cfg;
}

int get_scan_path_count(void) {
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_runtime_state_mutex);
  int count = g_runtime_state.scan_path_count;
  pthread_mutex_unlock(&g_runtime_state_mutex);
  return count;
}

bool get_scan_path(int index, char path_out[MAX_PATH]) {
  ensure_runtime_config_ready();
  path_out[0] = '\0';
  pthread_mutex_lock(&g_runtime_state_mutex);
  bool valid = index >= 0 && index < g_runtime_state.scan_path_count;
  if (valid)
    (void)strlcpy(path_out, g_runtime_state.scan_path_storage[index], MAX_PATH);
  pthread_mutex_unlock(&g_runtime_state_mutex);
  return valid;
}

int get_backport_scan_path_count(void) {
  return get_scan_path_count() + 1;
}

bool get_backport_scan_path(int index, char path_out[MAX_PATH]) {
  ensure_runtime_config_ready();
  path_out[0] = '\0';
  pthread_mutex_lock(&g_runtime_state_mutex);
  bool valid = index >= 0 && index <= g_runtime_state.scan_path_count;
  if (valid) {
    const char *path = index == g_runtime_state.scan_path_count
                           ? DEFAULT_BACKPORT_SCAN_PATH
                           : g_runtime_state.scan_path_storage[index];
    (void)strlcpy(path_out, path, MAX_PATH);
  }
  pthread_mutex_unlock(&g_runtime_state_mutex);
  return valid;
}

int get_custom_scan_path_count(void) {
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_runtime_state_mutex);
  int count = g_runtime_state.custom_scan_path_count;
  pthread_mutex_unlock(&g_runtime_state_mutex);
  return count;
}

bool get_custom_scan_path(int index, char path_out[MAX_PATH]) {
  ensure_runtime_config_ready();
  path_out[0] = '\0';
  pthread_mutex_lock(&g_runtime_state_mutex);
  bool valid = index >= 0 && index < g_runtime_state.custom_scan_path_count;
  if (valid)
    (void)strlcpy(path_out, g_runtime_state.scan_path_storage[index], MAX_PATH);
  pthread_mutex_unlock(&g_runtime_state_mutex);
  return valid;
}

uint32_t get_scan_depth_for_root(const char *scan_path) {
  uint32_t scan_depth = runtime_config().scan_depth;
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

  char normalized[MAX_PATH];
  if (!normalize_image_filename_value(filename, normalized))
    return false;

  pthread_mutex_lock(&g_runtime_state_mutex);
  const runtime_config_state_t *state = &g_runtime_state;
  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (!state->image_mode_rules[k].valid)
      continue;
    if (!state->image_mode_rules[k].mount_mode_valid)
      continue;
    if (strcasecmp(state->image_mode_rules[k].filename, normalized) != 0)
      continue;
    *mount_read_only_out = state->image_mode_rules[k].mount_read_only;
    pthread_mutex_unlock(&g_runtime_state_mutex);
    return true;
  }

  pthread_mutex_unlock(&g_runtime_state_mutex);
  return false;
}

bool get_image_sector_size_override(const char *filename,
                                    uint32_t *sector_size_out) {
  ensure_runtime_config_ready();
  if (!filename || !sector_size_out)
    return false;

  pthread_mutex_lock(&g_autotune_file_mutex);
  bool autotuned = lookup_image_sector_override_in_file(
      AUTOTUNE_FILE, filename, sector_size_out);
  pthread_mutex_unlock(&g_autotune_file_mutex);
  if (autotuned) {
    return true;
  }

  char normalized[MAX_PATH];
  if (!normalize_image_filename_value(filename, normalized))
    return false;

  pthread_mutex_lock(&g_runtime_state_mutex);
  const runtime_config_state_t *state = &g_runtime_state;
  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (!state->image_mode_rules[k].valid)
      continue;
    if (!state->image_mode_rules[k].sector_size_valid)
      continue;
    if (strcasecmp(state->image_mode_rules[k].filename, normalized) != 0)
      continue;
    *sector_size_out = state->image_mode_rules[k].sector_size;
    pthread_mutex_unlock(&g_runtime_state_mutex);
    return true;
  }

  pthread_mutex_unlock(&g_runtime_state_mutex);
  return false;
}

const char *sm_config_fakelib_mode_name(sm_fakelib_mode_t mode) {
  switch (mode) {
  case SM_FAKELIB_FULL:
    return "full";
  case SM_FAKELIB_EMULATORS:
    return "emulators";
  case SM_FAKELIB_DISABLED:
    return "disabled";
  case SM_FAKELIB_DEFAULT:
    return "default";
  default:
    return NULL;
  }
}

bool sm_config_parse_fakelib_mode(const char *value, sm_fakelib_mode_t *mode) {
  if (!value || !mode)
    return false;
  for (int i = SM_FAKELIB_FULL; i <= SM_FAKELIB_DEFAULT; ++i) {
    if (strcasecmp(value, sm_config_fakelib_mode_name((sm_fakelib_mode_t)i)) == 0) {
      *mode = (sm_fakelib_mode_t)i;
      return true;
    }
  }
  return false;
}

sm_fakelib_mode_t sm_config_title_fakelib_override(const runtime_config_t *cfg,
                                                 const char *title_id) {
  if (title_id) {
    for (uint32_t i = 0; i < cfg->fakelib_rule_count; ++i) {
      if (strcasecmp(cfg->fakelib_rules[i].title_id, title_id) == 0)
        return cfg->fakelib_rules[i].mode;
    }
  }
  return SM_FAKELIB_DEFAULT;
}

sm_fakelib_mode_t sm_config_title_fakelib_mode(const runtime_config_t *cfg,
                                             const char *title_id) {
  sm_fakelib_mode_t mode = sm_config_title_fakelib_override(cfg, title_id);
  return mode == SM_FAKELIB_DEFAULT ? cfg->fakelib_default_mode : mode;
}

sm_fakelib_mode_t get_fakelib_mode_for_title(const char *title_id) {
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_runtime_state_mutex);
  sm_fakelib_mode_t mode =
      sm_config_title_fakelib_mode(&g_runtime_state.cfg, title_id);
  pthread_mutex_unlock(&g_runtime_state_mutex);
  return mode;
}

bool is_fakelib_excluded_for_title(const char *title_id) {
  return get_fakelib_mode_for_title(title_id) == SM_FAKELIB_DISABLED;
}

bool is_global_fakelib_excluded_for_title(const char *title_id) {
  ensure_runtime_config_ready();

  char normalized[MAX_TITLE_ID];
  if (!normalize_title_id_value(title_id, normalized))
    return false;

  pthread_mutex_lock(&g_runtime_state_mutex);
  const runtime_config_t *cfg = &g_runtime_state.cfg;
  for (uint32_t i = 0; i < cfg->global_fakelib_exclude_title_count; ++i) {
    if (strcmp(cfg->global_fakelib_exclude_title_ids[i], normalized) == 0) {
      pthread_mutex_unlock(&g_runtime_state_mutex);
      return true;
    }
  }

  pthread_mutex_unlock(&g_runtime_state_mutex);
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

  pthread_mutex_lock(&g_autotune_file_mutex);
  bool updated = upsert_image_sector_override_in_file(
      AUTOTUNE_FILE, normalized_filename, sector_size);
  pthread_mutex_unlock(&g_autotune_file_mutex);
  if (!updated) {
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

static bool normalize_absolute_path_value(const char *value,
                                          char out[MAX_PATH]) {
  if (!value || !out)
    return false;

  char local[MAX_PATH];
  if (strlcpy(local, value, sizeof(local)) >= sizeof(local))
    return false;

  char *trimmed = trim_ascii(local);
  size_t len = strlen(trimmed);
  if (len == 0 || len >= MAX_PATH || trimmed[0] != '/')
    return false;
  for (size_t i = 0; i < len; ++i) {
    if (iscntrl((unsigned char)trimmed[i]) || trimmed[i] == '#')
      return false;
  }

  while (len > 1 && trimmed[len - 1] == '/') {
    trimmed[len - 1] = '\0';
    len--;
  }

  (void)strlcpy(out, trimmed, MAX_PATH);
  return true;
}

static bool normalize_http_url_value(const char *value, char out[MAX_PATH]) {
  if (!value || !out)
    return false;

  char local[MAX_PATH];
  if (strlcpy(local, value, sizeof(local)) >= sizeof(local))
    return false;

  char *trimmed = trim_ascii(local);
  size_t len = strlen(trimmed);
  const char *authority = NULL;
  if (strncasecmp(trimmed, "https://", 8) == 0)
    authority = trimmed + 8;
  else if (strncasecmp(trimmed, "http://", 7) == 0)
    authority = trimmed + 7;
  else
    return false;
  if (*authority == '\0' || *authority == '/' || *authority == '?' ||
      *authority == '#') {
    return false;
  }

  for (size_t i = 0; i < len; ++i) {
    if (iscntrl((unsigned char)trimmed[i]) ||
        isspace((unsigned char)trimmed[i])) {
      return false;
    }
  }

  (void)strlcpy(out, trimmed, MAX_PATH);
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

  bool read_failed = ferror(f) != 0;
  if (fclose(f) != 0)
    read_failed = true;
  if (read_failed || !found)
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
  if (!in && errno != ENOENT) {
    log_debug("  [CFG] autotune open failed: %s (%s)", path, strerror(errno));
    return false;
  }
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

    if (ferror(in))
      goto write_failed;
    int close_result = fclose(in);
    in = NULL;
    if (close_result != 0)
      goto write_failed;
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

static bool parse_title_fakelib_rule(const char *key, const char *value,
                                     char title_id[MAX_TITLE_ID],
                                     sm_fakelib_mode_t *mode) {
  if (strcasecmp(key, "fakelib_exclude") == 0) {
    *mode = SM_FAKELIB_DISABLED;
    return normalize_title_id_value(value, title_id) &&
           is_supported_game_title_id(title_id);
  }
  if (strcasecmp(key, "fakelib_mode") != 0)
    return false;
  const char *separator = strchr(value, ':');
  if (!separator || (size_t)(separator - value) >= MAX_TITLE_ID)
    return false;
  char raw_title[MAX_TITLE_ID];
  memcpy(raw_title, value, (size_t)(separator - value));
  raw_title[separator - value] = '\0';
  return normalize_title_id_value(raw_title, title_id) &&
         is_supported_game_title_id(title_id) &&
         sm_config_parse_fakelib_mode(separator + 1, mode);
}

static bool set_title_fakelib_rule(sm_fakelib_rule_t *rules, uint32_t *count,
                                  const char *title_id, sm_fakelib_mode_t mode) {
  for (uint32_t i = 0; i < *count; ++i) {
    if (strcmp(rules[i].title_id, title_id) != 0)
      continue;
    if (mode == SM_FAKELIB_DEFAULT) {
      --*count;
      memmove(&rules[i], &rules[i + 1u], (*count - i) * sizeof(*rules));
      memset(&rules[*count], 0, sizeof(*rules));
    } else {
      rules[i].mode = mode;
    }
    return true;
  }
  if (mode == SM_FAKELIB_DEFAULT)
    return true;
  if (*count >= MAX_FAKELIB_EXCLUDE_RULES)
    return false;
  sm_fakelib_rule_t *rule = &rules[(*count)++];
  (void)strlcpy(rule->title_id, title_id, sizeof(rule->title_id));
  rule->mode = mode;
  return true;
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

    if (strcasecmp(key, "api_enabled") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key,
                  value);
        continue;
      }
      state->cfg.api_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "api_bind_address") == 0 ||
        strcasecmp(key, "api_bind_adress") == 0) {
      struct in_addr address;
      if (strlen(value) >= sizeof(state->cfg.api_bind_address) ||
          inet_pton(AF_INET, value, &address) != 1) {
        log_debug("  [CFG] invalid API IPv4 address at line %d: %s=%s",
                  line_no, key, value);
        continue;
      }
      if (strcasecmp(key, "api_bind_adress") == 0)
        log_debug("  [CFG] legacy api_bind_adress at line %d; use "
                  "api_bind_address", line_no);
      (void)strlcpy(state->cfg.api_bind_address, value,
                    sizeof(state->cfg.api_bind_address));
      continue;
    }

    if (strcasecmp(key, "api_port") == 0) {
      if (!parse_u32_ini(value, &u32) || u32 == 0 || u32 > 65535u) {
        log_debug("  [CFG] invalid API port at line %d: %s=%s (range: 1..65535)",
                  line_no, key, value);
        continue;
      }
      state->cfg.api_port = u32;
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

    if (strcasecmp(key, "persistent_image_mounts") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.persistent_image_mounts = bval;
      continue;
    }

    if (strcasecmp(key, "app_install_all") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.app_install_all_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "auto_remove_missing_games") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.auto_remove_missing_games = bval;
      continue;
    }

    if (strcasecmp(key, "auto_remove_games_with_dlc") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.auto_remove_games_with_dlc = bval;
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

    if (strcasecmp(key, "fakelib_default_mode") == 0) {
      sm_fakelib_mode_t mode;
      if (sm_config_parse_fakelib_mode(value, &mode) && mode != SM_FAKELIB_DEFAULT) {
        state->cfg.fakelib_default_mode = mode;
      } else {
        log_debug("  [CFG] invalid fakelib_default_mode at line %d: %s",
                  line_no, value);
      }
      continue;
    }

    if (strcasecmp(key, "fakelib_exclude") == 0 ||
        strcasecmp(key, "fakelib_mode") == 0) {
      char title_id[MAX_TITLE_ID];
      sm_fakelib_mode_t mode;
      if (!parse_title_fakelib_rule(key, value, title_id, &mode) ||
          !set_title_fakelib_rule(state->cfg.fakelib_rules,
                                 &state->cfg.fakelib_rule_count, title_id, mode)) {
        log_debug("  [CFG] invalid fakelib mode rule or limit reached "
                  "(%u) at line %d: %s=%s",
                  (unsigned)MAX_FAKELIB_EXCLUDE_RULES, line_no, key, value);
      }
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
      char path[MAX_PATH];
      if (!normalize_absolute_path_value(value, path)) {
        log_debug("  [CFG] invalid global fakelib path at line %d: %s=%s",
                  line_no, key, value);
        continue;
      }
      (void)strlcpy(state->cfg.global_fakelib_path, path,
                    sizeof(state->cfg.global_fakelib_path));
      continue;
    }

    if (strcasecmp(key, "global_fakelib_priority") == 0) {
      if (strcasecmp(value, "game") == 0) {
        state->cfg.global_fakelib_game_priority = true;
      } else if (strcasecmp(value, "global") == 0) {
        state->cfg.global_fakelib_game_priority = false;
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

    if (strcasecmp(key, "update_emulators") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.update_emulators_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "emulators_path") == 0) {
      char path[MAX_PATH];
      if (!normalize_absolute_path_value(value, path)) {
        log_debug("  [CFG] invalid emulator path at line %d: %s=%s",
                  line_no, key, value);
        continue;
      }
      (void)strlcpy(state->cfg.emulators_path, path,
                    sizeof(state->cfg.emulators_path));
      continue;
    }

    if (strcasecmp(key, "auto_update_ampr") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.auto_update_ampr_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "ampr_update_url") == 0) {
      char url[MAX_PATH];
      if (!normalize_http_url_value(value, url)) {
        log_debug("  [CFG] invalid AMPR update URL at line %d: %s=%s "
                  "(HTTP or HTTPS required)",
                  line_no, key, value);
        continue;
      }
      (void)strlcpy(state->cfg.ampr_update_url, url,
                    sizeof(state->cfg.ampr_update_url));
      continue;
    }

    if (strcasecmp(key, "fan_target_temperature") == 0) {
      if (strcasecmp(value, "system") == 0 ||
          strcasecmp(value, "auto") == 0) {
        state->cfg.fan_target_temperature_c = FAN_TARGET_TEMPERATURE_SYSTEM;
      } else if (!parse_u32_ini(value, &u32) ||
                 u32 < MIN_FAN_TARGET_TEMPERATURE_C ||
                 u32 > MAX_FAN_TARGET_TEMPERATURE_C) {
        log_debug("  [CFG] invalid fan target at line %d: %s=%s "
                  "(use system or %u..%u C)",
                  line_no, key, value,
                  (unsigned)MIN_FAN_TARGET_TEMPERATURE_C,
                  (unsigned)MAX_FAN_TARGET_TEMPERATURE_C);
        continue;
      } else {
        state->cfg.fan_target_temperature_c = u32;
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

    if (strcasecmp(key, "auto_remove_missing_delay_seconds") == 0 ||
        strcasecmp(key, "auto_remove_missing_delay_sec") == 0) {
      if (!parse_u32_ini(value, &u32) ||
          u32 < MIN_AUTO_REMOVE_MISSING_DELAY_SECONDS ||
          u32 > MAX_AUTO_REMOVE_MISSING_DELAY_SECONDS) {
        log_debug("  [CFG] invalid auto-remove delay at line %d: %s=%s "
                  "(range: %u..%u)",
                  line_no, key, value,
                  (unsigned)MIN_AUTO_REMOVE_MISSING_DELAY_SECONDS,
                  (unsigned)MAX_AUTO_REMOVE_MISSING_DELAY_SECONDS);
        continue;
      }
      state->cfg.auto_remove_missing_delay_seconds = u32;
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

    if (strcasecmp(key, "nested_pfs_index_cache") == 0 ||
        strcasecmp(key, "legacy_gddr5_cache") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key,
                  value);
        continue;
      }
      state->cfg.nested_pfs_index_cache_enabled = bval;
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

  int read_error = ferror(f) ? (errno != 0 ? errno : EIO) : 0;
  if (fclose(f) != 0 && read_error == 0)
    read_error = errno;
  if (read_error != 0) {
    log_debug("  [CFG] read failed: %s (%s)", CONFIG_FILE,
              strerror(read_error));
    errno = read_error;
    return CONFIG_LOAD_ERROR;
  }

  if (has_custom_scanpaths && state->scan_path_count == 0) {
    log_debug("  [CFG] no valid scanpath entries, using defaults");
    init_runtime_scan_paths_defaults(state);
  } else if (has_custom_scanpaths) {
    state->custom_scan_path_count = state->scan_path_count;
  }
  add_runtime_managed_scan_paths(state);

  if (legacy_recursive_scan_requested) {
    state->cfg.scan_depth = 2u;
    state->cfg.legacy_recursive_scan_forced = true;
    log_debug("  [CFG] recursive_scan=1 is deprecated; forcing scan_depth=2");
  }

  int image_rule_count = 0;
  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (state->image_mode_rules[k].valid)
      image_rule_count++;
  }

  log_debug("  [CFG] loaded: api_enabled=%d debug=%d quiet=%d language=%s "
            "ro=%d force=%d "
            "persistent_image_mounts=%d app_install_all=%d "
            "auto_remove_missing_games=%d "
            "auto_remove_games_with_dlc=%d auto_remove_missing_delay_s=%u "
            "api=%s:%u scan_depth=%u "
            "legacy_recursive_scan_forced=%d backport_fakelib=%d "
            "fakelib_default_mode=%s fakelib_rules=%u "
            "global_fakelib=%d global_fakelib_priority=%s "
            "global_fakelib_path=%s global_fakelib_exclude=%u "
            "update_emulators=%d emulators_path=%s auto_update_ampr=%d "
            "ampr_update_url=%s "
            "fan_target_temperature=%u (0=system) "
            "exfat_backend=%s ufs_backend=%s "
            "nested_pfs_index_cache=%d "
            "lvd_sec(exfat=%u ufs=%u pfs=%u) md_sec(exfat=%u ufs=%u) "
            "scan_interval_s=%u stability_wait_s=%u scan_paths=%d image_rules=%d",
            state->cfg.api_enabled ? 1 : 0,
            state->cfg.debug_enabled ? 1 : 0, state->cfg.quiet_mode ? 1 : 0,
            sm_l10n_language_name(state->cfg.language_id),
            state->cfg.mount_read_only ? 1 : 0,
            state->cfg.force_mount ? 1 : 0,
            state->cfg.persistent_image_mounts ? 1 : 0,
            state->cfg.app_install_all_enabled ? 1 : 0,
            state->cfg.auto_remove_missing_games ? 1 : 0,
            state->cfg.auto_remove_games_with_dlc ? 1 : 0,
            state->cfg.auto_remove_missing_delay_seconds,
            state->cfg.api_bind_address, state->cfg.api_port,
            state->cfg.scan_depth,
            state->cfg.legacy_recursive_scan_forced ? 1 : 0,
            state->cfg.backport_fakelib_enabled ? 1 : 0,
            sm_config_fakelib_mode_name(state->cfg.fakelib_default_mode),
            state->cfg.fakelib_rule_count,
            state->cfg.global_fakelib_enabled ? 1 : 0,
            state->cfg.global_fakelib_game_priority ? "game" : "global",
            state->cfg.global_fakelib_path,
            state->cfg.global_fakelib_exclude_title_count,
            state->cfg.update_emulators_enabled ? 1 : 0,
            state->cfg.emulators_path,
            state->cfg.auto_update_ampr_enabled ? 1 : 0,
            state->cfg.ampr_update_url,
            state->cfg.fan_target_temperature_c,
            attach_backend_name(state->cfg.exfat_backend),
            attach_backend_name(state->cfg.ufs_backend),
            state->cfg.nested_pfs_index_cache_enabled ? 1 : 0,
            state->cfg.lvd_sector_exfat, state->cfg.lvd_sector_ufs,
            state->cfg.lvd_sector_pfs, state->cfg.md_sector_exfat,
            state->cfg.md_sector_ufs, state->cfg.scan_interval_us / 1000000u,
            state->cfg.stability_wait_seconds, state->scan_path_count,
            image_rule_count);

  return CONFIG_LOAD_OK;
}

bool load_runtime_config(void) {
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_runtime_load_mutex);
  config_load_status_t status = load_runtime_config_state(&g_runtime_parse_state);
  if (status == CONFIG_LOAD_ERROR) {
    pthread_mutex_unlock(&g_runtime_load_mutex);
    return false;
  }
  bool loaded = status == CONFIG_LOAD_OK;
  pthread_mutex_lock(&g_runtime_state_mutex);
  memcpy(&g_runtime_state, &g_runtime_parse_state, sizeof(g_runtime_state));
  pthread_mutex_unlock(&g_runtime_state_mutex);
  g_config_file_stamp = read_config_file_stamp();
  pthread_mutex_unlock(&g_runtime_load_mutex);
  return loaded;
}

bool reload_runtime_config_if_changed(bool *reloaded_out) {
  ensure_runtime_config_ready();
  if (reloaded_out)
    *reloaded_out = false;

  pthread_mutex_lock(&g_runtime_load_mutex);
  config_file_stamp_t new_stamp = read_config_file_stamp();
  if (config_file_stamp_equals(&new_stamp, &g_config_file_stamp)) {
    pthread_mutex_unlock(&g_runtime_load_mutex);
    return true;
  }

  runtime_config_state_t *parsed = &g_runtime_parse_state;
  config_load_status_t status = load_runtime_config_state(parsed);
  if (status == CONFIG_LOAD_ERROR) {
    pthread_mutex_unlock(&g_runtime_load_mutex);
    return false;
  }

  pthread_mutex_lock(&g_runtime_state_mutex);
  bool changed = !runtime_config_states_equal(&g_runtime_state, parsed);
  if (changed)
    memcpy(&g_runtime_state, parsed, sizeof(g_runtime_state));
  pthread_mutex_unlock(&g_runtime_state_mutex);

  g_config_file_stamp = new_stamp;
  pthread_mutex_unlock(&g_runtime_load_mutex);
  if (reloaded_out)
    *reloaded_out = changed;
  return true;
}

bool sm_config_set_title_fakelib_mode(const char *title_id, sm_fakelib_mode_t mode) {
  char normalized[MAX_TITLE_ID];
  if (!normalize_title_id_value(title_id, normalized) ||
      !is_supported_game_title_id(normalized) || !sm_config_fakelib_mode_name(mode)) {
    errno = EINVAL;
    return false;
  }
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_config_file_mutex);
  (void)mkdir(LOG_DIR, 0777);
  FILE *in = fopen(CONFIG_FILE, "r");
  if (!in && errno != ENOENT) {
    int saved_errno = config_io_error();
    pthread_mutex_unlock(&g_config_file_mutex);
    errno = saved_errno;
    return false;
  }
  char temp_path[MAX_PATH];
  int written = snprintf(temp_path, sizeof(temp_path), "%s.fakelib.XXXXXX",
                          CONFIG_FILE);
  int temp_fd = written > 0 && (size_t)written < sizeof(temp_path)
                    ? mkstemp(temp_path) : -1;
  FILE *out = NULL;
  if (temp_fd >= 0) {
    struct stat st;
    if (!in || (fstat(fileno(in), &st) == 0 &&
                fchmod(temp_fd, st.st_mode & 0777) == 0)) {
      out = fdopen(temp_fd, "w");
    }
  }
  if (!out) {
    int saved_errno = written <= 0 || (size_t)written >= sizeof(temp_path)
                          ? ENAMETOOLONG : config_io_error();
    if (temp_fd >= 0) {
      close(temp_fd);
      (void)unlink(temp_path);
    }
    if (in)
      fclose(in);
    pthread_mutex_unlock(&g_config_file_mutex);
    errno = saved_errno;
    return false;
  }

  sm_fakelib_rule_t rules[MAX_FAKELIB_EXCLUDE_RULES] = {0};
  uint32_t count = 0;
  int saved_errno = 0;
  int last_written = '\n';
  char line[MAX_PATH + 256u];
  while (in && fgets(line, sizeof(line), in)) {
    bool truncated = strchr(line, '\n') == NULL && !feof(in);
    char parsed[sizeof(line)];
    (void)strlcpy(parsed, line, sizeof(parsed));
    char *key = NULL;
    char *value = NULL;
    char parsed_title[MAX_TITLE_ID];
    sm_fakelib_mode_t parsed_mode;
    bool rule = parse_ini_line(parsed, &key, &value) &&
                parse_title_fakelib_rule(key, value, parsed_title, &parsed_mode);
    bool skip = rule && strcmp(parsed_title, normalized) == 0;
    if (rule && !skip)
      (void)set_title_fakelib_rule(rules, &count, parsed_title, parsed_mode);
    if (!skip && fputs(line, out) == EOF) {
      saved_errno = config_io_error();
      break;
    }
    if (!skip && line[0] != '\0')
      last_written = (unsigned char)line[strlen(line) - 1u];
    if (!truncated)
      continue;
    int ch;
    while ((ch = fgetc(in)) != EOF) {
      if (!skip && fputc(ch, out) == EOF && saved_errno == 0)
        saved_errno = config_io_error();
      if (!skip)
        last_written = ch;
      if (ch == '\n')
        break;
    }
    if (saved_errno != 0)
      break;
  }
  if (in && ferror(in) && saved_errno == 0)
    saved_errno = config_io_error();
  if (mode != SM_FAKELIB_DEFAULT && count == MAX_FAKELIB_EXCLUDE_RULES && saved_errno == 0)
    saved_errno = ENOSPC;
  if (saved_errno == 0 && last_written != '\n' && fputc('\n', out) == EOF)
    saved_errno = config_io_error();
  if (mode != SM_FAKELIB_DEFAULT && saved_errno == 0) {
    int result = mode == SM_FAKELIB_DISABLED
                     ? fprintf(out, "fakelib_exclude=%s\n", normalized)
                     : fprintf(out, "fakelib_mode=%s:%s\n", normalized,
                               sm_config_fakelib_mode_name(mode));
    if (result < 0)
      saved_errno = config_io_error();
    else
      (void)set_title_fakelib_rule(rules, &count, normalized, mode);
  }
  if (fflush(out) != 0 && saved_errno == 0)
    saved_errno = config_io_error();
  if (saved_errno == 0 && fsync(fileno(out)) != 0)
    saved_errno = config_io_error();
  if (fclose(out) != 0 && saved_errno == 0)
    saved_errno = config_io_error();
  if (in && fclose(in) != 0 && saved_errno == 0)
    saved_errno = config_io_error();
  if (saved_errno == 0 && rename(temp_path, CONFIG_FILE) != 0)
    saved_errno = config_io_error();
  if (saved_errno == 0) {
    // Publish just this policy. Leave the file stamp for the scanner so it
    // still applies any other config edits through its normal reload path.
    pthread_mutex_lock(&g_runtime_load_mutex);
    pthread_mutex_lock(&g_runtime_state_mutex);
    g_runtime_state.cfg.fakelib_rule_count = count;
    memcpy(g_runtime_state.cfg.fakelib_rules, rules, sizeof(rules));
    pthread_mutex_unlock(&g_runtime_state_mutex);
    pthread_mutex_unlock(&g_runtime_load_mutex);
  } else {
    (void)unlink(temp_path);
  }
  pthread_mutex_unlock(&g_config_file_mutex);
  if (saved_errno != 0) {
    errno = saved_errno;
    return false;
  }
  log_debug("  [CFG] fakelib for %s %s; applies on next launch", normalized,
            sm_config_fakelib_mode_name(mode));
  return true;
}

bool sm_config_set_title_fakelib_enabled(const char *title_id, bool enabled) {
  return sm_config_set_title_fakelib_mode(
      title_id, enabled ? SM_FAKELIB_FULL : SM_FAKELIB_DISABLED);
}

bool sm_config_write_web_settings(bool debug_enabled, bool quiet_mode,
                                  bool update_emulators_enabled,
                                  bool auto_update_ampr_enabled,
                                  bool auto_remove_missing_games,
                                  uint32_t auto_remove_missing_delay_seconds,
                                  bool allow_lan_access,
                                  uint32_t fan_target_temperature_c,
                                  const char *const *scan_paths,
                                  size_t scan_path_count,
                                  sm_fakelib_mode_t fakelib_default_mode) {
  if (auto_remove_missing_delay_seconds <
          MIN_AUTO_REMOVE_MISSING_DELAY_SECONDS ||
      auto_remove_missing_delay_seconds >
          MAX_AUTO_REMOVE_MISSING_DELAY_SECONDS ||
      (fan_target_temperature_c != FAN_TARGET_TEMPERATURE_SYSTEM &&
       (fan_target_temperature_c < MIN_FAN_TARGET_TEMPERATURE_C ||
        fan_target_temperature_c > MAX_FAN_TARGET_TEMPERATURE_C)) ||
      scan_path_count > MAX_SCAN_PATHS ||
      (scan_path_count > 0 && !scan_paths) ||
      !sm_config_fakelib_mode_name(fakelib_default_mode) ||
      fakelib_default_mode == SM_FAKELIB_DEFAULT) {
    errno = EINVAL;
    return false;
  }

  char(*normalized_paths)[MAX_PATH] =
      scan_path_count > 0 ? calloc(scan_path_count, MAX_PATH) : NULL;
  if (scan_path_count > 0 && !normalized_paths) {
    errno = ENOMEM;
    return false;
  }
  size_t normalized_count = 0;
  for (size_t i = 0; i < scan_path_count; ++i) {
    char path[MAX_PATH];
    if (!normalize_absolute_path_value(scan_paths[i], path) ||
        strcmp(path, "/") == 0 ||
        is_under_image_mount_base(path)) {
      free(normalized_paths);
      errno = EINVAL;
      return false;
    }
    bool duplicate = false;
    for (size_t j = 0; j < normalized_count; ++j) {
      if (strcmp(normalized_paths[j], path) == 0) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      (void)strlcpy(normalized_paths[normalized_count], path, MAX_PATH);
      normalized_count++;
    }
  }

  pthread_mutex_lock(&g_config_file_mutex);
  (void)mkdir(LOG_DIR, 0777);
  FILE *in = fopen(CONFIG_FILE, "r");
  if (!in && errno != ENOENT) {
    int saved_errno = config_io_error();
    pthread_mutex_unlock(&g_config_file_mutex);
    free(normalized_paths);
    errno = saved_errno;
    return false;
  }

  char temp_path[MAX_PATH];
  int temp_written = snprintf(temp_path, sizeof(temp_path), "%s.web.tmp",
                              CONFIG_FILE);
  if (temp_written < 0 || (size_t)temp_written >= sizeof(temp_path)) {
    if (in)
      (void)fclose(in);
    pthread_mutex_unlock(&g_config_file_mutex);
    free(normalized_paths);
    errno = ENAMETOOLONG;
    return false;
  }
  (void)unlink(temp_path);
  FILE *out = fopen(temp_path, "w");
  if (!out) {
    int saved_errno = config_io_error();
    if (in)
      (void)fclose(in);
    pthread_mutex_unlock(&g_config_file_mutex);
    free(normalized_paths);
    errno = saved_errno;
    return false;
  }

  errno = 0;
  int saved_errno = 0;
  int last_written = '\n';
  char line[MAX_PATH + 256u];
  while (in && fgets(line, sizeof(line), in)) {
    bool truncated = strchr(line, '\n') == NULL && !feof(in);
    char parsed[sizeof(line)];
    (void)strlcpy(parsed, line, sizeof(parsed));
    char *key = NULL;
    char *value = NULL;
    char *trimmed = trim_ascii(parsed);
    bool skip = strcmp(trimmed,
                       "# Managed by the ShadowMount web interface.") == 0;
    if (!skip)
      skip = parse_ini_line(parsed, &key, &value) &&
             web_managed_config_key(key);
    if (!skip && fputs(line, out) == EOF) {
      saved_errno = config_io_error();
      break;
    }
    if (!skip && line[0] != '\0')
      last_written = (unsigned char)line[strlen(line) - 1u];
    if (!truncated)
      continue;
    int ch;
    while ((ch = fgetc(in)) != EOF) {
      if (!skip && fputc(ch, out) == EOF && saved_errno == 0)
        saved_errno = config_io_error();
      if (!skip)
        last_written = ch;
      if (ch == '\n')
        break;
    }
    if (saved_errno != 0)
      break;
  }
  if (in && ferror(in) && saved_errno == 0)
    saved_errno = config_io_error();
  if (saved_errno == 0 && last_written != '\n' && fputc('\n', out) == EOF)
    saved_errno = config_io_error();
  if (saved_errno == 0 &&
      fprintf(out,
              "\n# Managed by the ShadowMount web interface.\n"
              "debug=%u\nquiet_mode=%u\nupdate_emulators=%u\n"
              "auto_update_ampr=%u\nfakelib_default_mode=%s\n"
              "auto_remove_missing_games=%u\n"
              "auto_remove_missing_delay_seconds=%u\n"
              "api_bind_address=%s\n",
              debug_enabled ? 1u : 0u, quiet_mode ? 1u : 0u,
              update_emulators_enabled ? 1u : 0u,
              auto_update_ampr_enabled ? 1u : 0u,
              sm_config_fakelib_mode_name(fakelib_default_mode),
              auto_remove_missing_games ? 1u : 0u,
              auto_remove_missing_delay_seconds,
              allow_lan_access ? "0.0.0.0" : "127.0.0.1") < 0) {
    saved_errno = config_io_error();
  }
  if (saved_errno == 0) {
    if (fan_target_temperature_c == FAN_TARGET_TEMPERATURE_SYSTEM) {
      if (fputs("fan_target_temperature=system\n", out) == EOF)
        saved_errno = config_io_error();
    } else if (fprintf(out, "fan_target_temperature=%u\n",
                       fan_target_temperature_c) < 0) {
      saved_errno = config_io_error();
    }
  }
  for (size_t i = 0; saved_errno == 0 && i < normalized_count; ++i) {
    if (fprintf(out, "scanpath=%s\n", normalized_paths[i]) < 0)
      saved_errno = config_io_error();
  }
  if (fflush(out) != 0 && saved_errno == 0)
    saved_errno = config_io_error();
  if (saved_errno == 0 && fsync(fileno(out)) != 0)
    saved_errno = config_io_error();
  if (fclose(out) != 0 && saved_errno == 0)
    saved_errno = config_io_error();
  if (in && fclose(in) != 0 && saved_errno == 0)
    saved_errno = config_io_error();
  if (saved_errno == 0 && rename(temp_path, CONFIG_FILE) != 0)
    saved_errno = config_io_error();
  if (saved_errno != 0)
    (void)unlink(temp_path);
  pthread_mutex_unlock(&g_config_file_mutex);
  free(normalized_paths);
  if (saved_errno != 0) {
    errno = saved_errno;
    return false;
  }
  return true;
}
