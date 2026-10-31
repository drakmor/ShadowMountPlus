// Host regression test: compile the production config implementation with
// console-only dependencies stubbed below. No console files are accessed.
#define _GNU_SOURCE
#define SM_PLATFORM_H
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sm_paths.h"
#undef CONFIG_FILE
#undef PROFILES_FILE
#undef LEGACY_AUTOTUNE_FILE
#undef LEGACY_SMB_FILE
#undef CONFIG_BACKUP_DIR
#define CONFIG_FILE "./config.ini"
#define PROFILES_FILE "./profiles.ini"
#define LEGACY_AUTOTUNE_FILE "./autotune.ini"
#define LEGACY_SMB_FILE "./smb.json"
#define CONFIG_BACKUP_DIR "./backup"

static size_t test_strlcpy(char *dst, const char *src, size_t size) {
  size_t length = strlen(src);
  if (size > 0) {
    size_t copied = length < size - 1u ? length : size - 1u;
    memcpy(dst, src, copied);
    dst[copied] = '\0';
  }
  return length;
}
#define strlcpy test_strlcpy

#include "title_id_helpers.inc"
#include "sm_smb.h"
#include "smb_validation.inc"
static bool fail_main_rename;
static int test_rename(const char *source, const char *target) {
  if (fail_main_rename && !strcmp(target, CONFIG_FILE)) { errno = EIO; return -1; }
  return rename(source, target);
}
#define rename test_rename
#include "../src/sm_config_file.c"
#include "../src/sm_config_mount.c"
bool is_under_image_mount_base(const char *path) { (void)path; return false; }

void log_debug(const char *fmt, ...) {
  (void)fmt;
  // Production logging reads config, including while parsing a reload.
  (void)runtime_config();
}

bool sm_l10n_parse_language_id(const char *value, int32_t *out) {
  (void)value;
  *out = SM_LANGUAGE_AUTO;
  return true;
}

const char *sm_l10n_language_name(int32_t language_id) {
  (void)language_id;
  return "auto";
}

const char *attach_backend_name(attach_backend_t backend) {
  (void)backend;
  return "test";
}

const char *get_filename_component(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static atomic_bool reader_stop;
static atomic_uint read_count;
static pthread_barrier_t reader_barrier;

static void write_config(unsigned version) {
  char next_path[MAX_PATH];
  assert(snprintf(next_path, sizeof(next_path), "%s.next", CONFIG_FILE) > 0);
  FILE *file = fopen(next_path, "w");
  assert(file);
  fprintf(file,
          "[config]\nversion=2\n[api]\nport=%u\n"
          "[fakelib]\nglobal_path=/data/version%u\n"
          "[emulators]\npath=/data/version%u\n"
          "[scan]\npath=/data/games%u\n",
          9100u + version, version, version, version);
  assert(fclose(file) == 0);
  assert(rename(next_path, CONFIG_FILE) == 0);
  file = fopen(PROFILES_FILE, "w");
  assert(file);
  fprintf(file, "[config]\nversion=2\n[game:PPSA12345]\nignore_global_fakelib=on\n"
                "[image:game.exfat]\nmount_mode=ro\nsector_size=%u\n", version == 1 ? 512u : 4096u);
  assert(!fclose(file));
}

static void *init_reader(void *unused) {
  (void)unused;
  (void)pthread_barrier_wait(&reader_barrier);
  runtime_config_t cfg = runtime_config();
  assert(cfg.api_port == SM_API_DEFAULT_PORT);
  char path[MAX_PATH];
  assert(get_scan_path(0, path));
  assert(path[0] == '/');
  return NULL;
}

static void *reload_reader(void *unused) {
  (void)unused;
  (void)pthread_barrier_wait(&reader_barrier);
  do {
    runtime_config_t cfg = runtime_config();
    unsigned version = cfg.api_port - 9100u;
    assert(version == 1u || version == 2u);
    char expected[MAX_PATH];
    snprintf(expected, sizeof(expected), "/data/version%u", version);
    assert(strcmp(cfg.global_fakelib_path, expected) == 0);
    assert(strcmp(cfg.emulators_path, expected) == 0);
    assert(cfg.global_fakelib_exclude_title_count == 1u);
    assert(strcmp(cfg.global_fakelib_exclude_title_ids[0], "PPSA12345") == 0);

    char path[MAX_PATH];
    assert(get_scan_path_count() == 3);
    assert(get_custom_scan_path_count() == 1);
    assert(get_scan_path(0, path));
    assert(strcmp(path, "/data/games1") == 0 ||
           strcmp(path, "/data/games2") == 0);
    assert(get_custom_scan_path(0, path));
    assert(strcmp(path, "/data/games1") == 0 ||
           strcmp(path, "/data/games2") == 0);
    bool read_only = false;
    uint32_t sector = 0;
    assert(get_image_mode_override("game.exfat", &read_only) && read_only);
    assert(get_image_sector_size_override("game.exfat", &sector));
    assert(sector == 512u || sector == 4096u);
    assert(is_global_fakelib_excluded_for_title("PPSA12345"));
    atomic_fetch_add(&read_count, 1u);
  } while (!atomic_load(&reader_stop));
  return NULL;
}

static void test_logging_flags(void) {
  for (unsigned mask = 0; mask < 4; ++mask) {
    FILE *file = fopen(CONFIG_FILE, "w");
    assert(file);
    fprintf(file, "[config]\nversion=2\n[general]\ndebug=%u\nquiet_mode=%u\n", mask & 1u, (mask >> 1) & 1u);
    assert(fclose(file) == 0 && load_runtime_config());
    assert(sm_config_debug_enabled() == !!(mask & 1u));
    assert(sm_config_quiet_mode() == !!(mask & 2u));
  }
}

static void put_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

static char *get_file(const char *path) {
  char *data = NULL;
  size_t size = 0;
  assert(read_bytes(path, &data, &size));
  assert(data);
  return data;
}

static void reset_files(void) {
  const char *paths[] = {CONFIG_FILE, PROFILES_FILE, LEGACY_AUTOTUNE_FILE, LEGACY_SMB_FILE,
    CONFIG_BACKUP_DIR "/config.ini", CONFIG_BACKUP_DIR "/autotune.ini", CONFIG_BACKUP_DIR "/smb.json",
    CONFIG_BACKUP_DIR "/ready.ini", CONFIG_BACKUP_DIR "/profiles-v2.ini"};
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
    assert(!unlink(paths[i]) || errno == ENOENT);
  assert(!rmdir(CONFIG_BACKUP_DIR) || errno == ENOENT);
}

static void test_migration(void) {
  const char *legacy = "# preserved original\napi_port=11111\nrecursive_scan=1\nscan_depth=1\n"
      "scanpath=/data/first\nscanpath=/data/second\nimage_ro=/mnt/usb0/Game [v1].exfat\n"
      "image_sector=Game [v1].exfat:4096\nfakelib_exclude=PPSA13197\n"
      "fakelib_mode=PPSA13197:emulators\nglobal_fakelib_exclude=PPSA13197\n";
  put_file(CONFIG_FILE, legacy);
  put_file(LEGACY_AUTOTUNE_FILE, "image_sector=Game [v1].exfat:512\n");
  put_file(LEGACY_SMB_FILE, "[{\"name\":\"NAS\",\"path\":\"smb://server/games\","
      "\"username\":\"DOMAIN\\\\user\",\"password\":\" #;\\\"secret\\\\ \",\"active\":true,\"read_only\":true}]");
  // Simulate a crash/error after profiles publication but before the main commit.
  fail_main_rename = true;
  assert(!sm_config_prepare_files());
  char *data = get_file(CONFIG_FILE);
  assert(!strcmp(data, legacy)); free(data);
  assert(access(PROFILES_FILE, F_OK) == 0 && access(LEGACY_SMB_FILE, F_OK) == 0);
  fail_main_rename = false;
  assert(sm_config_prepare_files() && load_runtime_config());
  assert(runtime_config().api_port == 11111 && runtime_config().scan_depth == 2);
  assert(get_custom_scan_path_count() == 2);
  assert(get_fakelib_mode_for_title("PPSA13197") == SM_FAKELIB_EMULATORS);
  assert(is_global_fakelib_excluded_for_title("PPSA13197"));
  uint32_t sector;
  bool ro;
  assert(get_image_sector_size_override("Game [v1].exfat", &sector) && sector == 512);
  assert(get_image_mode_override("game [v1].exfat", &ro) && ro);
  sm_smb_connection_t c;
  assert(!sm_config_smb_get("nas", &c));
  assert(!strcmp(c.password, " #;\"secret\\ ") && !strcmp(c.username, "DOMAIN\\user"));
  assert(c.read_only && c.active);
  data = get_file(CONFIG_BACKUP_DIR "/config.ini");
  assert(!strcmp(data, legacy)); free(data);
  assert(access(LEGACY_AUTOTUNE_FILE, F_OK) < 0 && errno == ENOENT);
  assert(access(LEGACY_SMB_FILE, F_OK) < 0 && errno == ENOENT);
  struct stat before, after;
  assert(!stat(CONFIG_FILE, &before) && !(before.st_mode & 077));
  assert(sm_config_prepare_files() && !stat(CONFIG_FILE, &after));
  assert(before.st_ino == after.st_ino); // No rewrite on subsequent startups.
  // Even after the commit, matching legacy leftovers can be retired safely.
  assert(copy_backup(CONFIG_BACKUP_DIR "/smb.json", LEGACY_SMB_FILE));
  assert(sm_config_prepare_files() && access(LEGACY_SMB_FILE, F_OK) < 0);
  reset_files();

  // Never overwrite user-created profiles or accept a newer schema.
  put_file(CONFIG_FILE, legacy);
  put_file(PROFILES_FILE, "[config]\nversion=2\n");
  assert(!sm_config_prepare_files() && errno == EEXIST);
  reset_files();
  put_file(CONFIG_FILE, "[config]\nversion=3\n");
  assert(!sm_config_prepare_files() && errno == EPROTONOSUPPORT);
  reset_files();
  put_file(CONFIG_FILE, "api_port=99999\n");
  assert(!sm_config_prepare_files() && access(PROFILES_FILE, F_OK) < 0);
  data = get_file(CONFIG_FILE); assert(!strcmp(data, "api_port=99999\n")); free(data);
  put_file(CONFIG_FILE, "api_port=12345\n");
  assert(sm_config_prepare_files() && load_runtime_config());
  assert(runtime_config().api_port == 12345);
  reset_files();
  // Truncated strings and malformed SMB must leave every source intact.
  put_file(CONFIG_FILE, "api_port=10101\n");
  put_file(LEGACY_SMB_FILE, "[{\"password\":");
  assert(!sm_config_prepare_files() && access(LEGACY_SMB_FILE, F_OK) == 0);
  reset_files();
  // A fresh minimal install and a v2 main file without profiles both work.
  assert(sm_config_prepare_files() && load_runtime_config());
  assert(runtime_config().api_port == SM_API_DEFAULT_PORT);
  reset_files();
  put_file(CONFIG_FILE, "[config]\nversion=2\n");
  assert(sm_config_prepare_files() && load_runtime_config());
  reset_files();
}

static void test_editing(void) {
  put_file(CONFIG_FILE, "# keep main comment\n[config]\nversion=2\n[general]\ndebug=on # debug comment\n"
      "[custom] # keep [header comment]\nuntouched = value # custom comment\n");
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:PPSA13197]\nfakelib=disabled # game comment\n"
      "ignore_global_fakelib=on\n[image:Game [v1].exfat]\nmount_mode=rw # mount comment\n");
  assert(load_runtime_config());
  assert(sm_config_set_title_fakelib_mode("PPSA13197", SM_FAKELIB_EMULATORS));
  assert(upsert_image_sector_size_autotune("Game [v1].exfat", 512, NULL));
  char *data = get_file(PROFILES_FILE);
  assert(strstr(data, "# game comment") && strstr(data, "# mount comment")); free(data);
  assert(sm_config_set_title_fakelib_mode("PPSA13197", SM_FAKELIB_DEFAULT));
  assert(sm_config_set_title_global_fakelib_excluded("PPSA13197", false));
  assert(get_fakelib_mode_for_title("PPSA13197") == SM_FAKELIB_FULL);
  assert(!is_global_fakelib_excluded_for_title("PPSA13197"));
  assert(sm_config_write_web_settings(false, true, true, false, false, 300,
      true, 0, NULL, 0, SM_FAKELIB_FULL, false, false, false));
  data = get_file(CONFIG_FILE);
  assert(strstr(data, "# keep main comment") && strstr(data, "# debug comment") &&
      strstr(data, "untouched = value # custom comment") && strstr(data, "# keep [header comment]")); free(data);
  sm_smb_connection_t c = {.name="NAS", .path="smb://server/games", .active=true};
  strlcpy(c.password, "  #;a\\b\"  ", sizeof(c.password));
  assert(!sm_config_smb_update(NULL, &c));
  assert(sm_config_smb_update(NULL, &c) == EEXIST);
  assert(!sm_config_smb_update("NAS", &(sm_smb_connection_t){.name="nas", .path="smb://server/games"}));
  assert(!sm_config_smb_get("nas", &c) && !strcmp(c.name, "nas"));
  assert(!sm_config_smb_update("nas", NULL));
  assert(sm_config_smb_get("nas", &c) == ENOENT);
  assert(sm_config_smb_update("missing", NULL) == ENOENT);

  bool changed;
  assert(reload_runtime_config_if_changed(&changed));
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:PPSA13197]\nfakelib=disabled\n"
      "[game:ppsa13197]\nfakelib=default\nignore_global_fakelib=on\nignore_global_fakelib=off\n"
      "[image:demo.exfat]\nmount_mode=rw\nmount_mode=default\nsector_size=4096\nsector_size=default\n");
  assert(reload_runtime_config_if_changed(&changed) && changed);
  assert(get_fakelib_mode_for_title("PPSA13197") == SM_FAKELIB_FULL);
  assert(!is_global_fakelib_excluded_for_title("PPSA13197"));
  bool ro; uint32_t sector;
  assert(!get_image_mode_override("demo.exfat", &ro) && !get_image_sector_size_override("demo.exfat", &sector));
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:PPSA13197]\nfakelib=emulators\n");
  assert(reload_runtime_config_if_changed(&changed) && changed);
  assert(get_fakelib_mode_for_title("PPSA13197") == SM_FAKELIB_EMULATORS);
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:PPSA13197]\nfakelib=invalid\n");
  assert(!reload_runtime_config_if_changed(&changed));
  assert(get_fakelib_mode_for_title("PPSA13197") == SM_FAKELIB_EMULATORS);
  assert(!sm_config_set_title_fakelib_mode("PPSA12345", SM_FAKELIB_FULL));
  // Embedded NUL, overlong input, and newline injection are rejected.
  FILE *f = fopen(PROFILES_FILE, "wb");
  const char nul[] = "[config]\nversion=2\0bad=1\n";
  assert(f && fwrite(nul, 1, sizeof(nul)-1, f) == sizeof(nul)-1 && !fclose(f));
  assert(!reload_runtime_config_if_changed(&changed));
  sm_ini_t doc = {0};
  assert(sm_ini_set(&doc, "section", "key", "value"));
  assert(!sm_ini_set(&doc, "section", "key", "value\nnew=1"));
  sm_ini_free(&doc);
  reset_files();
}

static void *settings_writer(void *arg) {
  uintptr_t kind = (uintptr_t)arg;
  (void)pthread_barrier_wait(&reader_barrier);
  sm_smb_connection_t connection = {.name="NAS", .path="smb://server/games", .active=true};
  for (unsigned i = 0; i < 8; ++i) {
    if (kind == 0)
      assert(sm_config_write_web_settings(i & 1, false, true, false, false, 300,
          true, 0, NULL, 0, SM_FAKELIB_FULL, false, false, false));
    else if (kind == 1) {
      assert(sm_config_set_title_fakelib_mode("PPSA13197", SM_FAKELIB_EMULATORS));
      assert(sm_config_set_title_ps4_profile("CUSA27145",
          &(sm_ps4_settings_t){SM_PS4_DEFAULT, SM_PS4_ON, SM_PS4_OFF, SM_PS4_ON, SM_PS4_DEFAULT, 0}));
    }
    else if (kind == 2)
      assert(upsert_image_sector_size_autotune("Game.exfat", 512, NULL));
    else {
      snprintf(connection.password, sizeof(connection.password), " #secret%u;\\\" ", i);
      assert(!sm_config_smb_update(i ? "NAS" : NULL, &connection));
    }
  }
  return NULL;
}

static void test_concurrent_writers(void) {
  put_file(CONFIG_FILE, "[config]\nversion=2\n");
  put_file(PROFILES_FILE, "[config]\nversion=2\n");
  assert(load_runtime_config());
  pthread_t writers[4];
  for (uintptr_t i = 0; i < 4; ++i)
    assert(!pthread_create(&writers[i], NULL, settings_writer, (void *)i));
  (void)pthread_barrier_wait(&reader_barrier);
  for (size_t i = 0; i < 4; ++i) assert(!pthread_join(writers[i], NULL));
  assert(load_runtime_config() && sm_config_debug_enabled());
  assert(get_fakelib_mode_for_title("PPSA13197") == SM_FAKELIB_EMULATORS);
  uint32_t sector;
  assert(get_image_sector_size_override("Game.exfat", &sector) && sector == 512);
  sm_smb_connection_t c;
  assert(!sm_config_smb_get("NAS", &c) && !strcmp(c.password, " #secret7;\\\" "));
  uint64_t revision;
  assert(sm_config_get_title_ps4_policy("CUSA27145", &revision) == 13);
  reset_files();
  puts("config migration: recovery, credentials, validation, manual edits and concurrent writers passed");
}

static uint8_t ps4_policy(const char *title) {
  uint64_t revision;
  uint8_t policy = sm_config_get_title_ps4_policy(title, &revision);
  assert(revision == runtime_config().ps4_revision);
  return policy;
}

static void test_ps4_settings(void) {
  put_file(CONFIG_FILE, "[config]\nversion=2\n");
  put_file(PROFILES_FILE, "[config]\nversion=2\n");
  assert(load_runtime_config());
  uint64_t default_revision;
  assert(!sm_config_ps4_hooks_needed(&default_revision));
  assert(ps4_policy("CUSA27145") == 0 && ps4_policy("PPSA13197") == 0);
  put_file(CONFIG_FILE, "[config]\nversion=2\n[api]\nport=10102\n[ps4]\nforce_pro=off\n");
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:CUSA27145] # keep game comment\n"
      "fakelib=emulators\nignore_global_fakelib=on\nps4_pro=off # keep Pro comment\n"
      "ps4_cpu_boost=off\nps4_gpu_boost=on\n"
      "[image:Game.exfat]\nsector_size=512\n");
  assert(load_runtime_config());
  for (unsigned mask = 0; mask < 8; ++mask) {
    char *profiles_before = get_file(PROFILES_FILE);
    assert(sm_config_write_web_settings(false, false, true, false, false, 300,
        false, 0, NULL, 0, SM_FAKELIB_FULL, mask & 1, mask & 2, mask & 4));
    char *profiles_after = get_file(PROFILES_FILE);
    assert(!strcmp(profiles_before, profiles_after)); free(profiles_before); free(profiles_after);
    bool changed;
    assert(reload_runtime_config_if_changed(&changed));
    assert(runtime_config().api_port == 10102);
    assert(ps4_policy("CUSA27145") == 2); // Explicit off overrides all globals.
    assert(ps4_policy("CUSA00001") == ((mask & 1 ? 4 : 0) | (mask & 2 ? 1 : 0) | (mask & 4 ? 2 : 0)));
  }
  char *main_before = get_file(CONFIG_FILE);
  sm_ps4_settings_t settings = {SM_PS4_DEFAULT, SM_PS4_OFF, SM_PS4_ON, SM_PS4_ON, SM_PS4_DEFAULT, 0};
  uint64_t revision = runtime_config().ps4_revision;
  assert(sm_config_set_title_ps4_profile("cusa27145", &settings));
  assert(ps4_policy("CUSA27145") == 14 && runtime_config().ps4_revision == revision + 1);
  revision = runtime_config().ps4_revision;
  assert(sm_config_set_title_ps4_profile("CUSA27145", &settings));
  assert(runtime_config().ps4_revision == revision);
  assert(get_fakelib_mode_for_title("CUSA27145") == SM_FAKELIB_EMULATORS);
  assert(is_global_fakelib_excluded_for_title("CUSA27145"));
  char *main_after = get_file(CONFIG_FILE);
  assert(!strcmp(main_before, main_after)); free(main_before); free(main_after);
  char *profiles = get_file(PROFILES_FILE);
  assert(strstr(profiles, "# keep game comment") && strstr(profiles, "# keep Pro comment") &&
      strstr(profiles, "[image:Game.exfat]")); free(profiles);
  settings.pro = SM_PS4_OFF;
  assert(!sm_config_set_title_ps4_profile("CUSA27145", &settings) && errno == EINVAL);
  settings.pro = SM_PS4_DEFAULT;
  assert(!sm_config_set_title_ps4_profile("PPSA13197", &settings) && errno == EINVAL);
  settings.gpu = (sm_ps4_setting_t)99;
  assert(!sm_config_set_title_ps4_profile("CUSA27145", &settings) && errno == EINVAL);
  assert(runtime_config().ps4_revision == revision && ps4_policy("CUSA27145") == 14);
  settings = (sm_ps4_settings_t){0};
  assert(sm_config_set_title_ps4_profile("CUSA27145", &settings));
  assert(runtime_config().ps4_rule_count == 0 && ps4_policy("CUSA27145") == 7);
  assert(is_global_fakelib_excluded_for_title("CUSA27145"));

  // A fakelib save must not publish a pending manual PS4 edit without hook refresh.
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:CUSA27145]\nps4_cpu_boost=off\n");
  revision = runtime_config().ps4_revision;
  assert(sm_config_set_title_fakelib_mode("CUSA27145", SM_FAKELIB_FULL));
  assert(runtime_config().ps4_revision == revision && ps4_policy("CUSA27145") == 7);
  bool changed;
  assert(reload_runtime_config_if_changed(&changed) && changed);
  assert(runtime_config().ps4_revision == revision + 1 && ps4_policy("CUSA27145") == 6);
  // Conversely PS4 saves must leave unrelated runtime settings for the scanner.
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:CUSA27145]\nfakelib=disabled\n");
  settings = (sm_ps4_settings_t){SM_PS4_ON, SM_PS4_OFF, SM_PS4_OFF, SM_PS4_OFF, SM_PS4_DEFAULT, 0};
  assert(sm_config_set_title_ps4_profile("CUSA27145", &settings));
  assert(get_fakelib_mode_for_title("CUSA27145") == SM_FAKELIB_FULL);
  assert(reload_runtime_config_if_changed(&changed) && changed);
  assert(get_fakelib_mode_for_title("CUSA27145") == SM_FAKELIB_DISABLED);
  assert(ps4_policy("CUSA27145") == 4);
  revision = runtime_config().ps4_revision;
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:CUSA27145]\nps4_pro=off\nps4_8gb=on\n");
  assert(!reload_runtime_config_if_changed(&changed) && runtime_config().ps4_revision == revision);
  assert(ps4_policy("CUSA27145") == 4);
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:PPSA13197]\nps4_pro=on\n");
  assert(!reload_runtime_config_if_changed(&changed));
  // Split duplicate sections still resolve each field by its last occurrence.
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:CUSA27145]\nps4_pro=off\nps4_8gb=on\n"
      "[game:cusa27145]\nps4_pro=default\nps4_cpu_boost=off\nps4_gpu_boost=off\n");
  assert(reload_runtime_config_if_changed(&changed) && changed);
  assert(ps4_policy("CUSA27145") == 12);
  // New options survive saves/reloads, force GPU independently, and reject invalid MHz.
  settings = (sm_ps4_settings_t){.cpu = SM_PS4_OFF, .gpu = SM_PS4_OFF, .video_120hz = SM_PS4_ON, .gpu_mhz = 2350};
  assert(sm_config_set_title_ps4_profile("CUSA27145", &settings));
  assert(ps4_policy("CUSA27145") == (4 | 2 | 16 | 32));
  assert(load_runtime_config() && ps4_policy("CUSA27145") == (4 | 2 | 16 | 32));
  settings.gpu_mhz = 2400;
  assert(!sm_config_set_title_ps4_profile("CUSA27145", &settings) && errno == EINVAL);
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:CUSA27145]\nps4_gpu_clock=2351\n");
  assert(!load_runtime_config() && ps4_policy("CUSA27145") == (4 | 2 | 16 | 32));
  put_file(PROFILES_FILE, "[config]\nversion=2\n[game:CUSA27145]\nps4_120hz=on\n");
  assert(load_runtime_config() && ps4_policy("CUSA27145") == (7 | 32));
  settings = (sm_ps4_settings_t){0};
  settings.pro = SM_PS4_ON;
  // The 128-rule limit must reject a new entry without changing files or live state.
  FILE *file = fopen(PROFILES_FILE, "w"); assert(file);
  fputs("[config]\nversion=2\n", file);
  for (unsigned i = 0; i < MAX_PS4_PROFILE_RULES; ++i)
    fprintf(file, "[game:CUSA%05u]\nps4_pro=on\n", i);
  assert(!fclose(file) && load_runtime_config());
  profiles = get_file(PROFILES_FILE);
  assert(!sm_config_set_title_ps4_profile("CUSA99999", &settings) && errno == ENOSPC);
  char *unchanged = get_file(PROFILES_FILE); assert(!strcmp(profiles, unchanged)); free(profiles); free(unchanged);
  assert(runtime_config().ps4_rule_count == MAX_PS4_PROFILE_RULES);
  revision = runtime_config().ps4_revision;
  char *valid_main = get_file(CONFIG_FILE);
  put_file(CONFIG_FILE, "[config]\nversion=2\n[ps4]\nforce_pro=invalid\n");
  assert(!reload_runtime_config_if_changed(&changed) && runtime_config().ps4_revision == revision);
  put_file(CONFIG_FILE, valid_main); free(valid_main);
  assert(load_runtime_config() && runtime_config().ps4_revision == revision);
  reset_files();
  puts("PS4 config v2: defaults, inheritance, 8 GB, limits and revision publication passed");
}

int main(void) {
  char directory[] = "/tmp/shadowmount-config-test-XXXXXX";
  assert(mkdtemp(directory));
  assert(!chdir(directory));

  pthread_t readers[4];
  assert(pthread_barrier_init(&reader_barrier, NULL, 5) == 0);
  for (size_t i = 0; i < 4; i++)
    assert(pthread_create(&readers[i], NULL, init_reader, NULL) == 0);
  (void)pthread_barrier_wait(&reader_barrier);
  for (size_t i = 0; i < 4; i++)
    assert(pthread_join(readers[i], NULL) == 0);

  test_migration();
  test_editing();
  test_concurrent_writers();
  test_logging_flags();
  test_ps4_settings();
  write_config(1);
  assert(load_runtime_config());
  runtime_config_t retained = runtime_config();
  char retained_path[MAX_PATH];
  assert(get_scan_path(0, retained_path));
  bool changed = true;
  assert(reload_runtime_config_if_changed(&changed) && !changed);

  for (size_t i = 0; i < 4; i++)
    assert(pthread_create(&readers[i], NULL, reload_reader, NULL) == 0);
  (void)pthread_barrier_wait(&reader_barrier);
  for (unsigned i = 0; i < 1000; i++) {
    write_config(i % 2u == 0 ? 2u : 1u);
    assert(reload_runtime_config_if_changed(&changed) && changed);
  }
  atomic_store(&reader_stop, true);
  for (size_t i = 0; i < 4; i++)
    assert(pthread_join(readers[i], NULL) == 0);
  assert(atomic_load(&read_count) > 0u);
  assert(retained.api_port == 9101u);
  assert(strcmp(retained.global_fakelib_path, "/data/version1") == 0);
  assert(strcmp(retained_path, "/data/games1") == 0);

  char invalid_path[MAX_PATH] = "stale";
  assert(!get_scan_path(-1, invalid_path) && invalid_path[0] == '\0');
  assert(!get_scan_path(MAX_SCAN_PATHS, invalid_path));
  assert(!get_custom_scan_path(1, invalid_path));
  bool read_only = false;
  uint32_t value = 0;
  assert(!get_image_mode_override("missing.exfat", &read_only));
  assert(!get_image_sector_size_override("missing.exfat", &value));
  assert(!is_global_fakelib_excluded_for_title("PPSA99999"));
  write_config(1);
  assert(reload_runtime_config_if_changed(&changed) && !changed);
  assert(unlink(CONFIG_FILE) == 0);
  assert(!reload_runtime_config_if_changed(&changed));
  assert(runtime_config().api_port == 9101u);
  reset_files();
  assert(!chdir("/tmp"));
  assert(rmdir(directory) == 0);
  assert(pthread_barrier_destroy(&reader_barrier) == 0);
  printf("config: 1000 reloads, %u concurrent reads, snapshots preserved\n",
         atomic_load(&read_count));
  return 0;
}
