#include "sm_platform.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>

#include "sm_api_protocol.h"
#include "sm_config_mount.h"
#include "sm_config_file.h"
#include "sm_smb.h"
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
static const char *const k_ps4_global_keys[] = {"force_pro", "cpu_boost", "gpu_boost"};
static const char *const k_ps4_profile_keys[] = {
  "ps4_pro", "ps4_cpu_boost", "ps4_gpu_boost", "ps4_8gb", "ps4_120hz"
};

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
  sm_smb_connection_t smb[SM_SMB_CONNECTIONS_MAX];
  size_t smb_count;
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
  CONFIG_LOAD_ERROR,
} config_load_status_t;

static runtime_config_state_t g_runtime_state;
static runtime_config_state_t g_runtime_parse_state;
static pthread_once_t g_runtime_init_once = PTHREAD_ONCE_INIT;
// Never hold the state mutex across I/O or logging: those can read config.
static pthread_mutex_t g_runtime_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static config_file_stamp_t g_config_file_stamp, g_profiles_file_stamp;
static pthread_mutex_t g_config_file_mutex = PTHREAD_MUTEX_INITIALIZER;

static char *trim_ascii(char *s);
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

static config_file_stamp_t read_config_file_stamp(const char *path) {
  config_file_stamp_t stamp;
  memset(&stamp, 0, sizeof(stamp));

  struct stat st;
  if (stat(path, &st) != 0)
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
  g_config_file_stamp = read_config_file_stamp(CONFIG_FILE);
  g_profiles_file_stamp = read_config_file_stamp(PROFILES_FILE);
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

bool sm_config_debug_enabled(void) {
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_runtime_state_mutex);
  bool enabled = g_runtime_state.cfg.debug_enabled;
  pthread_mutex_unlock(&g_runtime_state_mutex);
  return enabled;
}

void sm_config_patch_sources_snapshot(sm_patch_source_config_t *sources) {
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_runtime_state_mutex);
  *sources = g_runtime_state.cfg.patch_sources;
  pthread_mutex_unlock(&g_runtime_state_mutex);
}

bool sm_config_quiet_mode(void) {
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_runtime_state_mutex);
  bool quiet = g_runtime_state.cfg.quiet_mode;
  pthread_mutex_unlock(&g_runtime_state_mutex);
  return quiet;
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

bool sm_config_is_ps4_title(const char *title_id) {
  static const char prefixes[][5] = {"CUSA", "PLAS", "PCAS", "PCJS", "PLJS", "PLJM", "PCKS"};
  if (!title_id || strlen(title_id) != 9 || strspn(title_id + 4, "0123456789") != 5)
    return false;
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
    if (memcmp(title_id, prefixes[i], 4) == 0)
      return true;
  }
  return false;
}

const char *sm_config_ps4_setting_name(sm_ps4_setting_t setting) {
  switch (setting) {
  case SM_PS4_DEFAULT: return "default";
  case SM_PS4_OFF: return "off";
  case SM_PS4_ON: return "on";
  default: return NULL;
  }
}

bool sm_config_parse_ps4_setting(const char *value, sm_ps4_setting_t *setting) {
  if (!value || !setting)
    return false;
  for (unsigned i = SM_PS4_DEFAULT; i <= SM_PS4_ON; ++i) {
    if (strcasecmp(value, sm_config_ps4_setting_name((sm_ps4_setting_t)i)) == 0) {
      *setting = (sm_ps4_setting_t)i;
      return true;
    }
  }
  return false;
}

sm_ps4_settings_t sm_config_title_ps4_override(const runtime_config_t *cfg,
                                               const char *title_id) {
  if (cfg && sm_config_is_ps4_title(title_id)) {
    for (uint32_t i = 0; i < cfg->ps4_rule_count; ++i) {
      if (strcmp(cfg->ps4_rules[i].title_id, title_id) == 0)
        return cfg->ps4_rules[i].settings;
    }
  }
  return (sm_ps4_settings_t){0};
}

uint8_t sm_config_title_ps4_policy(const runtime_config_t *cfg, const char *title_id) {
  if (!cfg || !sm_config_is_ps4_title(title_id))
    return 0;
  sm_ps4_settings_t settings = sm_config_title_ps4_override(cfg, title_id);
  bool memory = settings.memory == SM_PS4_ON;
  return ((settings.cpu == SM_PS4_ON ||
           (settings.cpu == SM_PS4_DEFAULT && cfg->ps4_cpu_boost)) ? 1u : 0u) |
         ((settings.gpu == SM_PS4_ON ||
           (settings.gpu == SM_PS4_DEFAULT && cfg->ps4_gpu_boost) || settings.gpu_mhz == 2350) ? 2u : 0u) |
         ((settings.pro == SM_PS4_ON ||
           (settings.pro == SM_PS4_DEFAULT && (cfg->ps4_force_pro || memory))) ? 4u : 0u) |
         (memory ? 8u : 0u) |
         (settings.gpu_mhz == 2350 ? 16u : 0u) |
         (settings.video_120hz == SM_PS4_ON ? 32u : 0u);
}

uint8_t sm_config_get_title_ps4_policy(const char *title_id, uint64_t *revision) {
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_runtime_state_mutex);
  uint8_t policy = sm_config_title_ps4_policy(&g_runtime_state.cfg, title_id);
  *revision = g_runtime_state.cfg.ps4_revision;
  pthread_mutex_unlock(&g_runtime_state_mutex);
  return policy;
}

bool sm_config_ps4_hooks_needed(uint64_t *revision) {
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_runtime_state_mutex);
  const runtime_config_t *cfg = &g_runtime_state.cfg;
  bool needed = cfg->ps4_force_pro || cfg->ps4_cpu_boost || cfg->ps4_gpu_boost;
  for (uint32_t i = 0; !needed && i < cfg->ps4_rule_count; ++i) {
    const sm_ps4_settings_t *settings = &cfg->ps4_rules[i].settings;
    needed = settings->pro == SM_PS4_ON || settings->cpu == SM_PS4_ON ||
             settings->gpu == SM_PS4_ON || settings->memory == SM_PS4_ON ||
             settings->video_120hz == SM_PS4_ON || settings->gpu_mhz == 2350;
  }
  *revision = cfg->ps4_revision;
  pthread_mutex_unlock(&g_runtime_state_mutex);
  return needed;
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

bool sm_config_title_global_fakelib_excluded(const runtime_config_t *cfg,
                                            const char *title_id) {
  if (!cfg || !title_id)
    return false;

  for (uint32_t i = 0; i < cfg->global_fakelib_exclude_title_count; ++i) {
    if (strcasecmp(cfg->global_fakelib_exclude_title_ids[i], title_id) == 0)
      return true;
  }
  return false;
}

bool is_global_fakelib_excluded_for_title(const char *title_id) {
  ensure_runtime_config_ready();
  char normalized[MAX_TITLE_ID];
  if (!normalize_title_id_value(title_id, normalized))
    return false;
  pthread_mutex_lock(&g_runtime_state_mutex);
  bool excluded = sm_config_title_global_fakelib_excluded(&g_runtime_state.cfg,
                                                         normalized);
  pthread_mutex_unlock(&g_runtime_state_mutex);
  return excluded;
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

static bool set_title_ps4_rule(sm_ps4_rule_t *rules, uint32_t *count,
                               const char *title_id,
                               const sm_ps4_settings_t *settings) {
  bool inherit = settings->pro == SM_PS4_DEFAULT &&
                 settings->cpu == SM_PS4_DEFAULT && settings->gpu == SM_PS4_DEFAULT &&
                 settings->memory == SM_PS4_DEFAULT &&
                 settings->video_120hz == SM_PS4_DEFAULT && !settings->gpu_mhz;
  for (uint32_t i = 0; i < *count; ++i) {
    if (strcmp(rules[i].title_id, title_id) != 0)
      continue;
    if (inherit) {
      --*count;
      memmove(&rules[i], &rules[i + 1], (*count - i) * sizeof(*rules));
      memset(&rules[*count], 0, sizeof(*rules));
    } else {
      rules[i].settings = *settings;
    }
    return true;
  }
  if (inherit)
    return true;
  if (*count >= MAX_PS4_PROFILE_RULES)
    return false;
  sm_ps4_rule_t *rule = &rules[(*count)++];
  (void)strlcpy(rule->title_id, title_id, sizeof(rule->title_id));
  rule->settings = *settings;
  return true;
}

static bool read_ps4_settings(const sm_ini_t *main, const sm_ini_t *profiles,
                               runtime_config_t *cfg) {
  bool *globals[] = {&cfg->ps4_force_pro, &cfg->ps4_cpu_boost, &cfg->ps4_gpu_boost};
  for (size_t i = 0; i < main->count; ++i) {
    const sm_ini_line_t *line = &main->lines[i];
    if (!line->key || strcasecmp(line->section, "ps4")) continue;
    size_t field = 0;
    while (field < 3 && strcasecmp(line->key, k_ps4_global_keys[field])) ++field;
    if (field == 3) {
      log_debug("  [CFG] unknown [ps4] setting: %s", line->key);
      continue;
    }
    if (sm_ini_get(main, line->section, line->key) != line->value) continue;
    if (!parse_bool_ini(line->value, globals[field])) {
      log_debug("  [CFG] invalid [ps4] %s", line->key);
      errno = EINVAL; return false;
    }
  }
  for (size_t i = 0; i < profiles->count; ++i) {
    const sm_ini_line_t *line = &profiles->lines[i];
    if (!line->header || strncasecmp(line->section, "game:", 5)) continue;
    sm_ps4_settings_t settings = {0};
    sm_ps4_setting_t *fields[] = {&settings.pro, &settings.cpu, &settings.gpu, &settings.memory, &settings.video_120hz};
    bool present = false;
    for (size_t j = 0; j < sizeof(k_ps4_profile_keys) / sizeof(k_ps4_profile_keys[0]); ++j) {
      const char *value = sm_ini_get(profiles, line->section, k_ps4_profile_keys[j]);
      if (!value) continue;
      present = true;
      if (!sm_config_parse_ps4_setting(value, fields[j])) {
        log_debug("  [CFG] invalid [%s] %s", line->section, k_ps4_profile_keys[j]);
        errno = EINVAL; return false;
      }
    }
    const char *clock = sm_ini_get(profiles, line->section, "ps4_gpu_clock");
    if (clock) {
      present = true;
      if (!strcasecmp(clock, "2350")) settings.gpu_mhz = 2350;
      else if (strcasecmp(clock, "default")) {
        log_debug("  [CFG] invalid [%s] ps4_gpu_clock", line->section);
        errno = EINVAL; return false;
      }
    }
    if (!present) continue;
    char title[MAX_TITLE_ID];
    if (!normalize_title_id_value(line->section + 5, title) || !sm_config_is_ps4_title(title) ||
        (settings.memory == SM_PS4_ON && settings.pro == SM_PS4_OFF)) {
      log_debug("  [CFG] invalid PS4 profile: [%s]", line->section);
      errno = EINVAL; return false;
    }
    if (!set_title_ps4_rule(cfg->ps4_rules, &cfg->ps4_rule_count, title, &settings)) {
      log_debug("  [CFG] PS4 profile limit reached: [%s]", line->section);
      errno = ENOSPC; return false;
    }
  }
  return true;
}

static bool read_documents(sm_ini_t *main, sm_ini_t *profiles) {
  return sm_ini_read(CONFIG_FILE, main, false) && sm_ini_version(main) &&
         sm_ini_read(PROFILES_FILE, profiles, false) &&
         (!profiles->count || sm_ini_version(profiles));
}

static bool flatten_documents(const sm_ini_t *main, const sm_ini_t *profiles, sm_ini_t *flat) {
  for (size_t i = 0; i < main->count; ++i) {
    const sm_ini_line_t *line = &main->lines[i];
    if (!line->key || !strcasecmp(line->section, "config") || !strcasecmp(line->section, "ps4") || !strcasecmp(line->section, "patches")) continue;
    if (!strncasecmp(line->section, "smb:", 4)) {
      if (strcasecmp(line->key, "path") && strcasecmp(line->key, "username") &&
          strcasecmp(line->key, "password") && strcasecmp(line->key, "active") &&
          strcasecmp(line->key, "read_only"))
        log_debug("  [CFG] unknown SMB setting [%s] %s", line->section, line->key);
      continue;
    }
    const sm_config_key_t *mapping = NULL;
    for (size_t j = 0; j < sm_config_key_count; ++j)
      if (!strcasecmp(line->section, sm_config_keys[j].section) &&
          !strcasecmp(line->key, sm_config_keys[j].key)) { mapping = &sm_config_keys[j]; break; }
    if (!mapping) {
      log_debug("  [CFG] unknown setting [%s] %s", line->section, line->key);
      continue;
    }
    if (strcmp(mapping->legacy, "scanpath") &&
        sm_ini_get(main, line->section, line->key) != line->value) continue;
    if (!sm_ini_append(flat, "runtime", mapping->legacy, line->value)) return false;
  }
  for (size_t i = 0; i < profiles->count; ++i) {
    const sm_ini_line_t *line = &profiles->lines[i];
    if (!line->key || !strcasecmp(line->section, "config")) continue;
    // Scalars use the last occurrence, including an explicit reset to default/off.
    if (sm_ini_get(profiles, line->section, line->key) != line->value) continue;
    char value[MAX_PATH + 64];
    const char *key = NULL;
    if (!strncasecmp(line->section, "game:", 5)) {
      char title[MAX_TITLE_ID];
      if (!normalize_title_id_value(line->section + 5, title) || !is_supported_game_title_id(title)) {
        errno = EINVAL; return false;
      }
      bool ps4_key = !strcasecmp(line->key, "ps4_gpu_clock");
      for (size_t j = 0; j < sizeof(k_ps4_profile_keys) / sizeof(k_ps4_profile_keys[0]); ++j)
        if (!strcasecmp(line->key, k_ps4_profile_keys[j])) ps4_key = true;
      if (ps4_key) continue; // Validated as one complete profile by read_ps4_settings().
      if (!strcasecmp(line->key, "fakelib")) {
        key = "fakelib_mode"; snprintf(value, sizeof(value), "%s:%s", title, line->value);
      } else if (!strcasecmp(line->key, "ignore_global_fakelib")) {
        bool excluded;
        if (!strcasecmp(line->value, "default")) continue;
        if (!parse_bool_ini(line->value, &excluded)) { errno = EINVAL; return false; }
        if (!excluded) continue;
        key = "global_fakelib_exclude"; strlcpy(value, title, sizeof(value));
      }
    } else if (!strncasecmp(line->section, "image:", 6)) {
      char filename[MAX_PATH];
      if (strchr(line->section + 6, '/') || !normalize_image_filename_value(line->section + 6, filename)) { errno = EINVAL; return false; }
      if (!strcasecmp(line->value, "default")) continue;
      if (!strcasecmp(line->key, "mount_mode")) {
        if (!strcasecmp(line->value, "ro")) key = "image_ro";
        else if (!strcasecmp(line->value, "rw")) key = "image_rw";
        else { errno = EINVAL; return false; }
        strlcpy(value, filename, sizeof(value));
      } else if (!strcasecmp(line->key, "sector_size")) {
        key = "image_sector"; snprintf(value, sizeof(value), "%s:%s", filename, line->value);
      }
    }
    if (!key) {
      log_debug("  [CFG] unknown profile setting [%s] %s", line->section, line->key);
      continue;
    }
    if (!sm_ini_append(flat, "runtime", key, value)) return false;
  }
  return true;
}

static bool read_smb_connections(const sm_ini_t *doc, sm_smb_connection_t *items, size_t *count) {
  *count = 0;
  memset(items, 0, SM_SMB_CONNECTIONS_MAX * sizeof(*items));
  for (size_t i = 0; i < doc->count; ++i) {
    const sm_ini_line_t *line = &doc->lines[i];
    if (!line->header || strncasecmp(line->section, "smb:", 4)) continue;
    bool seen = false;
    for (size_t j = 0; j < *count; ++j)
      if (!strcasecmp(items[j].name, line->section + 4)) seen = true;
    if (seen) continue;
    if (*count == SM_SMB_CONNECTIONS_MAX) { errno = ENOSPC; return false; }
    sm_smb_connection_t *c = &items[*count];
    if (strlcpy(c->name, line->section + 4, sizeof(c->name)) >= sizeof(c->name)) { errno = EINVAL; return false; }
    const char *path = sm_ini_get(doc, line->section, "path");
    const char *user = sm_ini_get(doc, line->section, "username");
    const char *password = sm_ini_get(doc, line->section, "password");
    const char *active = sm_ini_get(doc, line->section, "active");
    const char *ro = sm_ini_get(doc, line->section, "read_only");
    c->active = true;
    if (!path || strlcpy(c->path, path, sizeof(c->path)) >= sizeof(c->path) ||
        strlcpy(c->username, user ? user : "", sizeof(c->username)) >= sizeof(c->username) ||
        strlcpy(c->password, password ? password : "", sizeof(c->password)) >= sizeof(c->password) ||
        (active && !parse_bool_ini(active, &c->active)) || (ro && !parse_bool_ini(ro, &c->read_only)) || sm_smb_validate(c)) {
      log_debug("  [CFG] invalid SMB connection near line %zu", i + 1);
      errno = EINVAL; return false;
    }
    ++*count;
  }
  return true;
}

static bool read_patch_sources(const sm_ini_t *doc, sm_patch_source_config_t *config) {
  memset(config, 0, sizeof(*config));
  const char *period = sm_ini_get(doc, "patches", "update_interval_hours");
  if (period && (!parse_u32_ini(period, &config->interval_hours) || config->interval_hours > 8760)) {
    log_debug("  [CFG] invalid [patches] update_interval_hours (0..8760)"); errno = EINVAL; return false;
  }
  bool custom = false;
  for (size_t i = 0; i < doc->count; ++i) {
    const sm_ini_line_t *line = &doc->lines[i];
    if (!line->key || strcasecmp(line->section, "patches")) continue;
    if (strcasecmp(line->key, "source")) {
      if (strcasecmp(line->key, "update_interval_hours")) log_debug("  [CFG] unknown [patches] %s", line->key);
      continue;
    }
    custom = true;
    if (!line->value || !*line->value) continue; // Explicit empty list.
    if (config->count == SM_PATCH_SOURCE_MAX) { errno = EFBIG; return false; }
    const char *value = line->value, *separator = strchr(value, '|');
    sm_patch_source_t *source = &config->sources[config->count];
    if (!separator || (separator - value != 6 && separator - value != 7)) { errno = EINVAL; return false; }
    source->cheats = separator - value == 6 && !strncmp(value, "cheats", 6);
    if ((!source->cheats && (separator - value != 7 || strncmp(value, "patches", 7))) ||
        strlcpy(source->url, separator + 1, sizeof(source->url)) >= sizeof(source->url) || !sm_patch_source_valid(source)) {
      log_debug("  [CFG] invalid [patches] source at line %zu", i + 1); errno = EINVAL; return false;
    }
    for (size_t j = 0; j < config->count; ++j)
      if (source->cheats == config->sources[j].cheats && !strcmp(source->url, config->sources[j].url)) { errno = EEXIST; return false; }
    ++config->count;
  }
  if (!custom) { config->count = SM_PATCH_SOURCE_PRESETS; memcpy(config->sources, sm_patch_source_presets, sizeof(sm_patch_source_presets)); }
  return true;
}

static config_load_status_t parse_runtime_config_state(runtime_config_state_t *state,
    const sm_ini_t *main, const sm_ini_t *profiles, bool report_loaded) {
  init_runtime_config_defaults(state);

  sm_ini_t flat = {0};
  if (!read_ps4_settings(main, profiles, &state->cfg) ||
      !read_patch_sources(main, &state->cfg.patch_sources) ||
      !flatten_documents(main, profiles, &flat) ||
      !read_smb_connections(main, state->smb, &state->smb_count)) {
    sm_ini_free(&flat);
    return CONFIG_LOAD_ERROR;
  }
  bool has_custom_scanpaths = false;
  bool invalid = false;
  for (size_t index = 0; index < flat.count; ++index) {
    const sm_ini_line_t *entry = &flat.lines[index];
    if (!entry->key) continue;
    const char *key = entry->key, *value = entry->value;
    int line_no = (int)index + 1;

    bool bval = false;
    uint32_t u32 = 0;
    attach_backend_t backend = ATTACH_BACKEND_NONE;

    if (strcasecmp(key, "debug") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        invalid = true;
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.debug_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "quiet_mode") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        invalid = true;
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
        invalid = true;
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
        invalid = true;
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
        invalid = true;
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
        invalid = true;
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
        invalid = true;
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.mount_read_only = bval;
      continue;
    }

    if (strcasecmp(key, "force_mount") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        invalid = true;
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.force_mount = bval;
      continue;
    }

    if (strcasecmp(key, "persistent_image_mounts") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        invalid = true;
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.persistent_image_mounts = bval;
      continue;
    }

    if (strcasecmp(key, "app_install_all") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        invalid = true;
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.app_install_all_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "auto_remove_missing_games") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        invalid = true;
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.auto_remove_missing_games = bval;
      continue;
    }

    if (strcasecmp(key, "auto_remove_games_with_dlc") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        invalid = true;
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
        invalid = true;
        log_debug("  [CFG] invalid image mode rule at line %d: %s=%s", line_no,
                  key, value);
      }
      continue;
    }

    if (strcasecmp(key, "image_sector") == 0) {
      if (!set_image_sector_rule(state, value)) {
        invalid = true;
        log_debug("  [CFG] invalid image sector rule at line %d: %s=%s "
                  "(format: IMAGE_FILENAME:SECTOR_SIZE)",
                  line_no, key, value);
      }
      continue;
    }

    if (strcasecmp(key, "scan_depth") == 0) {
      if (!parse_u32_ini(value, &u32) || u32 < MIN_SCAN_DEPTH ||
          u32 > MAX_SCAN_DEPTH) {
        invalid = true;
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
        invalid = true;
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
        invalid = true;
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
        invalid = true;
        log_debug("  [CFG] invalid fakelib mode rule or limit reached "
                  "(%u) at line %d: %s=%s",
                  (unsigned)MAX_FAKELIB_EXCLUDE_RULES, line_no, key, value);
      }
      continue;
    }

    if (strcasecmp(key, "global_fakelib") == 0 ||
        strcasecmp(key, "global_fakelib_enabled") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        invalid = true;
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.global_fakelib_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "global_fakelib_path") == 0) {
      char path[MAX_PATH];
      if (!normalize_absolute_path_value(value, path)) {
        invalid = true;
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
        invalid = true;
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
        invalid = true;
        log_debug("  [CFG] invalid global fakelib exclude rule at line %d: "
                  "%s=%s",
                  line_no, key, value);
      }
      continue;
    }

    if (strcasecmp(key, "update_emulators") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        invalid = true;
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.update_emulators_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "emulators_path") == 0) {
      char path[MAX_PATH];
      if (!normalize_absolute_path_value(value, path)) {
        invalid = true;
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
        invalid = true;
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      state->cfg.auto_update_ampr_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "ampr_update_url") == 0) {
      char url[MAX_PATH];
      if (!normalize_http_url_value(value, url)) {
        invalid = true;
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
        invalid = true;
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
        invalid = true;
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
        invalid = true;
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
        invalid = true;
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
        invalid = true;
        log_debug("  [CFG] invalid backend at line %d: %s=%s", line_no, key,
                  value);
        continue;
      }
      state->cfg.exfat_backend = backend;
      continue;
    }

    if (strcasecmp(key, "ufs_backend") == 0) {
      if (!parse_backend_ini(value, &backend)) {
        invalid = true;
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
        invalid = true;
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
        invalid = true;
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
      invalid = true;
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

  sm_ini_free(&flat);
  if (invalid) { errno = EINVAL; return CONFIG_LOAD_ERROR; }

  if (has_custom_scanpaths && state->scan_path_count == 0) {
    log_debug("  [CFG] no valid scanpath entries, using defaults");
    init_runtime_scan_paths_defaults(state);
  } else if (has_custom_scanpaths) {
    state->custom_scan_path_count = state->scan_path_count;
  }
  add_runtime_managed_scan_paths(state);

  if (!report_loaded) return CONFIG_LOAD_OK;

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
            "backport_fakelib=%d "
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

static config_load_status_t load_runtime_config_state(runtime_config_state_t *state) {
  sm_ini_t main = {0}, profiles = {0};
  config_load_status_t status = read_documents(&main, &profiles)
      ? parse_runtime_config_state(state, &main, &profiles, true) : CONFIG_LOAD_ERROR;
  sm_ini_free(&main); sm_ini_free(&profiles);
  return status;
}

static bool validate_documents(const sm_ini_t *main, const sm_ini_t *profiles) {
  return parse_runtime_config_state(&g_runtime_parse_state, main, profiles, false) == CONFIG_LOAD_OK;
}

bool sm_config_prepare_files(void) {
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_config_file_mutex);
  bool ok = sm_config_migrate_files(validate_documents);
  int error = errno;
  pthread_mutex_unlock(&g_config_file_mutex);
  errno = error;
  return ok;
}

static void prepare_ps4_revision(runtime_config_t *parsed,
                                  const runtime_config_t *current) {
  bool changed = parsed->ps4_force_pro != current->ps4_force_pro ||
                 parsed->ps4_cpu_boost != current->ps4_cpu_boost ||
                 parsed->ps4_gpu_boost != current->ps4_gpu_boost ||
                 parsed->ps4_rule_count != current->ps4_rule_count ||
                 memcmp(parsed->ps4_rules, current->ps4_rules, sizeof(parsed->ps4_rules)) != 0;
  parsed->ps4_revision = current->ps4_revision + (changed ? 1u : 0u);
}

bool load_runtime_config(void) {
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_config_file_mutex);
  config_file_stamp_t main_stamp = read_config_file_stamp(CONFIG_FILE);
  config_file_stamp_t profiles_stamp = read_config_file_stamp(PROFILES_FILE);
  config_load_status_t status = load_runtime_config_state(&g_runtime_parse_state);
  if (status == CONFIG_LOAD_ERROR) {
    pthread_mutex_unlock(&g_config_file_mutex);
    return false;
  }
  bool loaded = status == CONFIG_LOAD_OK;
  pthread_mutex_lock(&g_runtime_state_mutex);
  prepare_ps4_revision(&g_runtime_parse_state.cfg, &g_runtime_state.cfg);
  memcpy(&g_runtime_state, &g_runtime_parse_state, sizeof(g_runtime_state));
  pthread_mutex_unlock(&g_runtime_state_mutex);
  g_config_file_stamp = main_stamp;
  g_profiles_file_stamp = profiles_stamp;
  pthread_mutex_unlock(&g_config_file_mutex);
  return loaded;
}

bool reload_runtime_config_if_changed(bool *reloaded_out) {
  ensure_runtime_config_ready();
  if (reloaded_out)
    *reloaded_out = false;

  pthread_mutex_lock(&g_config_file_mutex);
  config_file_stamp_t new_stamp = read_config_file_stamp(CONFIG_FILE);
  config_file_stamp_t profiles_stamp = read_config_file_stamp(PROFILES_FILE);
  if (config_file_stamp_equals(&new_stamp, &g_config_file_stamp) &&
      config_file_stamp_equals(&profiles_stamp, &g_profiles_file_stamp)) {
    pthread_mutex_unlock(&g_config_file_mutex);
    return true;
  }

  runtime_config_state_t *parsed = &g_runtime_parse_state;
  config_load_status_t status = load_runtime_config_state(parsed);
  if (status == CONFIG_LOAD_ERROR) {
    pthread_mutex_unlock(&g_config_file_mutex);
    return false;
  }

  pthread_mutex_lock(&g_runtime_state_mutex);
  prepare_ps4_revision(&parsed->cfg, &g_runtime_state.cfg);
  bool changed = !runtime_config_states_equal(&g_runtime_state, parsed);
  if (changed)
    memcpy(&g_runtime_state, parsed, sizeof(g_runtime_state));
  pthread_mutex_unlock(&g_runtime_state_mutex);

  g_config_file_stamp = new_stamp;
  g_profiles_file_stamp = profiles_stamp;
  pthread_mutex_unlock(&g_config_file_mutex);
  if (reloaded_out)
    *reloaded_out = changed;
  return true;
}

// The shared file lock also owns the loader's scratch state. Reuse its
// validators; only the modified domain is published before scanner reload.
typedef enum { CONFIG_SAVE_MAIN, CONFIG_SAVE_PROFILES, CONFIG_SAVE_PS4, CONFIG_SAVE_SMB } config_save_domain_t;

static bool save_document(sm_ini_t *main, sm_ini_t *profiles, config_save_domain_t domain) {
  bool individual = domain == CONFIG_SAVE_PROFILES || domain == CONFIG_SAVE_PS4;
  bool ok = sm_ini_set(individual ? profiles : main, "config", "version", "2") &&
      parse_runtime_config_state(&g_runtime_parse_state, main, profiles, false) == CONFIG_LOAD_OK &&
      sm_ini_write(individual ? PROFILES_FILE : CONFIG_FILE, individual ? profiles : main);
  if (ok && domain != CONFIG_SAVE_MAIN) {
    pthread_mutex_lock(&g_runtime_state_mutex);
    if (domain == CONFIG_SAVE_PROFILES) {
      runtime_config_t *dst = &g_runtime_state.cfg, *src = &g_runtime_parse_state.cfg;
      dst->fakelib_rule_count = src->fakelib_rule_count;
      memcpy(dst->fakelib_rules, src->fakelib_rules, sizeof(dst->fakelib_rules));
      dst->global_fakelib_exclude_title_count = src->global_fakelib_exclude_title_count;
      memcpy(dst->global_fakelib_exclude_title_ids, src->global_fakelib_exclude_title_ids,
             sizeof(dst->global_fakelib_exclude_title_ids));
      memcpy(g_runtime_state.image_mode_rules, g_runtime_parse_state.image_mode_rules,
             sizeof(g_runtime_state.image_mode_rules));
    }
    if (domain == CONFIG_SAVE_PS4) {
      runtime_config_t *dst = &g_runtime_state.cfg, *src = &g_runtime_parse_state.cfg;
      if (dst->ps4_rule_count != src->ps4_rule_count ||
          memcmp(dst->ps4_rules, src->ps4_rules, sizeof(dst->ps4_rules))) ++dst->ps4_revision;
      dst->ps4_rule_count = src->ps4_rule_count;
      memcpy(dst->ps4_rules, src->ps4_rules, sizeof(dst->ps4_rules));
    }
    if (domain == CONFIG_SAVE_SMB) {
      g_runtime_state.smb_count = g_runtime_parse_state.smb_count;
      memcpy(g_runtime_state.smb, g_runtime_parse_state.smb, sizeof(g_runtime_state.smb));
    }
    pthread_mutex_unlock(&g_runtime_state_mutex);
  }
  return ok;
}

static bool write_title_fakelib_policy(const char *title_id, sm_fakelib_mode_t mode, bool global_only) {
  char title[MAX_TITLE_ID], section[MAX_TITLE_ID + 8];
  if (!normalize_title_id_value(title_id, title) || !is_supported_game_title_id(title) || !sm_config_fakelib_mode_name(mode)) {
    errno = EINVAL; return false;
  }
  snprintf(section, sizeof(section), "game:%s", title);
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_config_file_mutex);
  sm_ini_t main = {0}, profiles = {0};
  const char *value = global_only ? (mode == SM_FAKELIB_DISABLED ? "on" : NULL)
      : mode == SM_FAKELIB_DEFAULT ? NULL : sm_config_fakelib_mode_name(mode);
  bool ok = read_documents(&main, &profiles) &&
      sm_ini_set(&profiles, section, global_only ? "ignore_global_fakelib" : "fakelib", value) &&
      save_document(&main, &profiles, CONFIG_SAVE_PROFILES);
  int error = errno;
  sm_ini_free(&main); sm_ini_free(&profiles);
  pthread_mutex_unlock(&g_config_file_mutex);
  errno = error; return ok;
}

bool upsert_image_sector_size_autotune(const char *filename, uint32_t sector_size, uint32_t *sector_size_out) {
  char name[MAX_PATH], section[MAX_PATH + 8], value[16];
  if (!normalize_image_filename_value(filename, name) || !is_valid_sector_size(sector_size)) { errno = EINVAL; return false; }
  snprintf(section, sizeof(section), "image:%s", name);
  snprintf(value, sizeof(value), "%u", sector_size);
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_config_file_mutex);
  sm_ini_t main = {0}, profiles = {0};
  bool ok = read_documents(&main, &profiles) && sm_ini_set(&profiles, section, "sector_size", value) &&
      save_document(&main, &profiles, CONFIG_SAVE_PROFILES);
  int error = errno;
  sm_ini_free(&main); sm_ini_free(&profiles);
  pthread_mutex_unlock(&g_config_file_mutex);
  if (ok && sector_size_out) *sector_size_out = sector_size;
  errno = error; return ok;
}

size_t sm_config_smb_snapshot(sm_smb_connection_t *out) {
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_runtime_state_mutex);
  size_t count = g_runtime_state.smb_count;
  memcpy(out, g_runtime_state.smb, count * sizeof(*out));
  pthread_mutex_unlock(&g_runtime_state_mutex);
  return count;
}

int sm_config_smb_get(const char *name, sm_smb_connection_t *out) {
  if (!name || !out) return EINVAL;
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_runtime_state_mutex);
  int status = ENOENT;
  for (size_t i = 0; i < g_runtime_state.smb_count; ++i)
    if (!strcasecmp(name, g_runtime_state.smb[i].name)) { *out = g_runtime_state.smb[i]; status = 0; break; }
  pthread_mutex_unlock(&g_runtime_state_mutex);
  return status;
}

int sm_config_smb_update(const char *previous, const sm_smb_connection_t *connection) {
  int status = connection ? sm_smb_validate(connection) : (!previous || !*previous ? EINVAL : 0);
  if (status) return status;
  if (previous && strlen(previous) >= sizeof(((sm_smb_connection_t *)0)->name)) return EINVAL;
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_config_file_mutex);
  sm_ini_t main = {0}, profiles = {0};
  bool ok = read_documents(&main, &profiles);
  char old[80], section[80];
  snprintf(old, sizeof(old), "smb:%s", previous ? previous : "");
  snprintf(section, sizeof(section), "smb:%s", connection ? connection->name : "");
  if (ok && previous && *previous && !sm_ini_get(&main, old, "path")) { errno = ENOENT; ok = false; }
  if (ok && connection && (!previous || strcasecmp(previous, connection->name)) && sm_ini_get(&main, section, "path")) {
    errno = EEXIST; ok = false;
  }
  if (ok && previous && *previous) {
    if (!connection) ok = sm_ini_remove_section(&main, old);
    else if (strcmp(previous, connection->name)) ok = sm_ini_rename_section(&main, old, section);
  }
  if (ok && connection) {
    ok = sm_ini_set(&main, section, "path", connection->path) &&
         sm_ini_set(&main, section, "username", connection->username) &&
         sm_ini_set(&main, section, "password", connection->password) &&
         sm_ini_set(&main, section, "active", connection->active ? "on" : "off") &&
         sm_ini_set(&main, section, "read_only", connection->read_only ? "on" : "off");
  }
  if (ok) ok = save_document(&main, &profiles, CONFIG_SAVE_SMB);
  status = ok ? 0 : (errno ? errno : EIO);
  sm_ini_free(&main); sm_ini_free(&profiles);
  pthread_mutex_unlock(&g_config_file_mutex);
  return status;
}

bool sm_config_set_title_fakelib_mode(const char *title_id, sm_fakelib_mode_t mode) {
  return write_title_fakelib_policy(title_id, mode, false);
}

bool sm_config_set_title_global_fakelib_excluded(const char *title_id,
                                                bool excluded) {
  return write_title_fakelib_policy(
      title_id, excluded ? SM_FAKELIB_DISABLED : SM_FAKELIB_DEFAULT, true);
}

bool sm_config_set_title_fakelib_enabled(const char *title_id, bool enabled) {
  return sm_config_set_title_fakelib_mode(
      title_id, enabled ? SM_FAKELIB_FULL : SM_FAKELIB_DISABLED);
}

bool sm_config_write_patch_sources(const sm_patch_source_config_t *config) {
  if (!config || config->count > SM_PATCH_SOURCE_MAX || config->interval_hours > 8760) { errno = EINVAL; return false; }
  for (size_t i = 0; i < config->count; ++i) if (!sm_patch_source_valid(&config->sources[i])) { errno = EINVAL; return false; }
  pthread_mutex_lock(&g_config_file_mutex);
  sm_ini_t main = {0}, profiles = {0};
  char period[16]; snprintf(period, sizeof(period), "%u", config->interval_hours);
  bool ok = read_documents(&main, &profiles) &&
      sm_ini_set(&main, "patches", "update_interval_hours", period) && sm_ini_set(&main, "patches", "source", NULL);
  if (ok && !config->count) ok = sm_ini_append(&main, "patches", "source", "");
  for (size_t i = 0; ok && i < config->count; ++i) {
    char value[SM_PATCH_SOURCE_URL_MAX + 16];
    snprintf(value, sizeof(value), "%s|%s", config->sources[i].cheats ? "cheats" : "patches", config->sources[i].url);
    ok = sm_ini_append(&main, "patches", "source", value);
  }
  if (ok) ok = save_document(&main, &profiles, CONFIG_SAVE_MAIN);
  int error = errno;
  sm_ini_free(&main); sm_ini_free(&profiles);
  pthread_mutex_unlock(&g_config_file_mutex);
  errno = error;
  return ok;
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
                                  sm_fakelib_mode_t fakelib_default_mode,
                                  bool ps4_force_pro, bool ps4_cpu_boost,
                                  bool ps4_gpu_boost) {
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

  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_config_file_mutex);
  sm_ini_t main = {0}, profiles = {0};
  char delay[16], temperature[16];
  snprintf(delay, sizeof(delay), "%u", auto_remove_missing_delay_seconds);
  if (fan_target_temperature_c == FAN_TARGET_TEMPERATURE_SYSTEM) strlcpy(temperature, "system", sizeof(temperature));
  else snprintf(temperature, sizeof(temperature), "%u", fan_target_temperature_c);
  bool ok = read_documents(&main, &profiles) &&
      sm_ini_set(&main, "ps4", "force_pro", ps4_force_pro ? "on" : "off") &&
      sm_ini_set(&main, "ps4", "cpu_boost", ps4_cpu_boost ? "on" : "off") &&
      sm_ini_set(&main, "ps4", "gpu_boost", ps4_gpu_boost ? "on" : "off") &&
      sm_ini_set(&main, "general", "debug", debug_enabled ? "on" : "off") &&
      sm_ini_set(&main, "general", "quiet_mode", quiet_mode ? "on" : "off") &&
      sm_ini_set(&main, "emulators", "update_enabled", update_emulators_enabled ? "on" : "off") &&
      sm_ini_set(&main, "emulators", "auto_update_ampr", auto_update_ampr_enabled ? "on" : "off") &&
      sm_ini_set(&main, "fakelib", "default_mode", sm_config_fakelib_mode_name(fakelib_default_mode)) &&
      sm_ini_set(&main, "library", "remove_missing", auto_remove_missing_games ? "on" : "off") &&
      sm_ini_set(&main, "library", "remove_delay_seconds", delay) &&
      sm_ini_set(&main, "api", "bind_address", allow_lan_access ? "0.0.0.0" : "127.0.0.1") &&
      sm_ini_set(&main, "fan", "target_temperature", temperature) &&
      sm_ini_set(&main, "scan", "path", NULL);
  for (size_t i = 0; ok && i < normalized_count; ++i)
    ok = sm_ini_append(&main, "scan", "path", normalized_paths[i]);
  if (ok) ok = save_document(&main, &profiles, CONFIG_SAVE_MAIN);
  int error = errno;
  free(normalized_paths);
  sm_ini_free(&main); sm_ini_free(&profiles);
  pthread_mutex_unlock(&g_config_file_mutex);
  errno = error; return ok;
}

bool sm_config_set_title_ps4_profile(const char *title_id, const sm_ps4_settings_t *settings) {
  char title[MAX_TITLE_ID], section[MAX_TITLE_ID + 8];
  if (!settings || !normalize_title_id_value(title_id, title) || !sm_config_is_ps4_title(title) ||
      (settings->memory == SM_PS4_ON && settings->pro == SM_PS4_OFF) ||
      (settings->gpu_mhz && settings->gpu_mhz != 2350)) { errno = EINVAL; return false; }
  const sm_ps4_setting_t fields[] = {settings->pro, settings->cpu, settings->gpu, settings->memory, settings->video_120hz};
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
    if (!sm_config_ps4_setting_name(fields[i])) { errno = EINVAL; return false; }
  snprintf(section, sizeof(section), "game:%s", title);
  ensure_runtime_config_ready();
  pthread_mutex_lock(&g_config_file_mutex);
  sm_ini_t main = {0}, profiles = {0};
  bool ok = read_documents(&main, &profiles);
  for (size_t i = 0; ok && i < sizeof(fields) / sizeof(fields[0]); ++i)
    ok = sm_ini_set(&profiles, section, k_ps4_profile_keys[i],
        fields[i] == SM_PS4_DEFAULT ? NULL : sm_config_ps4_setting_name(fields[i]));
  if (ok) ok = sm_ini_set(&profiles, section, "ps4_gpu_clock",
                           settings->gpu_mhz ? "2350" : NULL);
  if (ok) ok = save_document(&main, &profiles, CONFIG_SAVE_PS4);
  int error = errno;
  sm_ini_free(&main); sm_ini_free(&profiles);
  pthread_mutex_unlock(&g_config_file_mutex);
  errno = error;
  return ok;
}
