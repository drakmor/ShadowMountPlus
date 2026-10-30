// Compile the production metadata copier; emulate case-insensitive USB lookup.
#define _GNU_SOURCE
#define SM_PLATFORM_H
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sm_limits.h"
#include "sm_paths.h"

// Production APP_BASE also appears in concatenated string literals.
// Relative fixture roots keep the real staging calls inside a temporary cwd.
#undef APP_BASE
#undef APPMETA_BASE
#define APP_BASE "app"
#define APPMETA_BASE "appmeta"

static uint32_t sm_firmware_major_version(void) { return 11u; }
int sceKernelUsleep(unsigned int microseconds);

static const char *usb_source_path(const char *src, char actual[MAX_PATH]);
static int usb_stat(const char *path, struct stat *st);
static int fixture_mkdir(const char *path, mode_t mode);
#define stat(path, st) usb_stat(path, st)
#define mkdir(path, mode) fixture_mkdir(path, mode)
#include "../src/sm_install.c"
#undef stat
#undef mkdir

static bool fail_icon_copy;
static int icon_stat_error;
static const char *failed_destination;
static unsigned mount_calls;

/** Resolve the lowercase lookup that a case-insensitive USB source accepts. */
static const char *usb_source_path(const char *src, char actual[MAX_PATH]) {
  struct stat st;
  if (stat(src, &st) != 0 && errno == ENOENT && strlen(src) >= 9u &&
      strcmp(src + strlen(src) - 9u, "icon0.png") == 0) {
    assert(strlen(src) < MAX_PATH);
    strcpy(actual, src);
    strcpy(actual + strlen(actual) - 9u, "ICON0.PNG");
    return actual;
  }
  return src;
}

/** Model USB lookup and inject errors without hiding genuine stat failures. */
static int usb_stat(const char *path, struct stat *st) {
  if (icon_stat_error && strstr(path, "/icon0.png")) {
    errno = icon_stat_error;
    return -1;
  }
  char actual[MAX_PATH];
  return stat(usb_source_path(path, actual), st);
}

/** Keep privileged trophy paths out of the host filesystem. */
static bool is_trophy_path(const char *path) {
  const char prefix[] = "/system_data/priv/appmeta";
  size_t length = sizeof(prefix) - 1u;
  return strncmp(path, prefix, length) == 0 &&
         (path[length] == '\0' || path[length] == '/');
}

/** Create fixture directories without accessing the console's trophy hierarchy. */
static int fixture_mkdir(const char *path, mode_t mode) {
  return is_trophy_path(path) ? 0 : mkdir(path, mode);
}

/** Copy fixture bytes, preserve requested destination casing, and inject I/O failures. */
int copy_file(const char *src, const char *dst) {
  if (is_trophy_path(dst))
    return 0;
  if (failed_destination && strcmp(dst, failed_destination) == 0) {
    errno = EIO;
    return -1;
  }
  if (fail_icon_copy && strstr(dst, "/icon0.png")) {
    errno = EIO;
    return -1;
  }
  char actual[MAX_PATH];
  FILE *input = fopen(usb_source_path(src, actual), "rb");
  if (!input)
    return -1;
  FILE *output = fopen(dst, "wb");
  assert(output);
  int byte;
  while ((byte = fgetc(input)) != EOF)
    assert(fputc(byte, output) != EOF);
  assert(!ferror(input));
  assert(fclose(input) == 0);
  assert(fclose(output) == 0);
  return 0;
}

/** Model the generic copier's case-preserving behavior for flat sce_sys fixtures. */
int copy_dir(const char *src, const char *dst) {
  DIR *directory = opendir(src);
  assert(directory);
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;
    char input[MAX_PATH], output[MAX_PATH];
    snprintf(input, sizeof(input), "%s/%s", src, entry->d_name);
    snprintf(output, sizeof(output), "%s/%s", dst, entry->d_name);
    assert(copy_file(input, output) == 0);
  }
  assert(closedir(directory) == 0);
  return 0;
}

// Console services are stubbed; the test never mounts or registers a real title.
bool is_under_image_mount_base(const char *path) { (void)path; return false; }
bool has_appmeta_data(const char *title) { (void)title; return false; }
bool should_stop_requested(void) { return false; }
bool runtime_sleep_mode_active(void) { return false; }
void runtime_mount_state_lock(void) {}
void runtime_mount_state_unlock(void) {}
void log_debug(const char *format, ...) { (void)format; }
bool path_exists(const char *path) { struct stat st; return usb_stat(path, &st) == 0; }
bool mount_title_nullfs(const char *title, const char *src) {
  (void)title; (void)src; mount_calls++; return true;
}
bool write_mount_link(const char *title, const char *src) {
  (void)title; (void)src; return true;
}
bool rollback_title_nullfs_mount(const char *title, const char *src) {
  (void)title; (void)src; assert(false); return false;
}
size_t resolve_image_source_chain_from_mount_cache(
    const char *path, char chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH]) {
  (void)path; (void)chain; assert(false); return 0;
}
bool write_mount_image_chain(const char *title,
    const char paths[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH], size_t count) {
  (void)title; (void)paths; (void)count; assert(false); return false;
}
bool cache_image_source_mapping(const char *path, const char *mount_point) {
  (void)path; (void)mount_point; assert(false); return false;
}
bool sm_shellcore_install_bridge_enabled(void) { assert(false); return false; }
bool sm_shellcore_install_title_dir(const char *title, const char *dir, int *result) {
  (void)title; (void)dir; (void)result; assert(false); return false;
}
int sceKernelUsleep(unsigned int microseconds) { (void)microseconds; assert(false); return 0; }
int update_snd0info(const char *title) { (void)title; assert(false); return -1; }
void invalidate_app_db_title_cache(void) { assert(false); }
void mark_register_attempted(const char *title) { (void)title; assert(false); }
void clear_register_attempts(const char *title) { (void)title; assert(false); }
void notify_game_installed_rich(const char *title) { (void)title; assert(false); }
void notify_system_l10n(sm_l10n_key_t key, ...) { (void)key; assert(false); }

static void put(const char *directory, const char *name, const char *content) {
  char path[MAX_PATH];
  assert(snprintf(path, sizeof(path), "%s/%s", directory, name) > 0);
  FILE *file = fopen(path, "wb");
  assert(file);
  assert(fputs(content, file) >= 0);
  assert(fclose(file) == 0);
}

static void expect(const char *directory, const char *name, const char *content) {
  char path[MAX_PATH], actual[64] = {0};
  assert(snprintf(path, sizeof(path), "%s/%s", directory, name) > 0);
  FILE *file = fopen(path, "rb");
  assert(file);
  assert(fread(actual, 1, sizeof(actual) - 1u, file) == strlen(content));
  assert(fclose(file) == 0);
  assert(strcmp(actual, content) == 0);
}

/** Exercise the fresh-install caller so omitting its canonical copy fails the test. */
static void test_staging(const char *root) {
  char previous_cwd[MAX_PATH], source[MAX_PATH], metadata[MAX_PATH];
  assert(getcwd(previous_cwd, sizeof(previous_cwd)));
  assert(chdir(root) == 0);
  assert(mkdir("title", 0700) == 0);
  assert(mkdir("title/sce_sys", 0700) == 0);
  snprintf(source, sizeof(source), "%s/title", root);
  snprintf(metadata, sizeof(metadata), "%s/title/sce_sys", root);
  put(metadata, "ICON0.PNG", "staged-usb-icon");
  put(metadata, "param.json", "{}");

  mount_calls = 0;
  assert(mount_and_install(source, "FAKE12345", "Fixture", false, true, true, NULL));
  assert(mount_calls == 1);
  expect("app/FAKE12345", "icon0.png", "staged-usb-icon");
  expect("app/FAKE12345/sce_sys", "icon0.png", "staged-usb-icon");
  expect("appmeta/FAKE12345", "icon0.png", "staged-usb-icon");

  failed_destination = "app/FAKE12345/sce_sys/icon0.png";
  mount_calls = 0;
  assert(!mount_and_install(source, "FAKE12345", "Fixture", false, true, true, NULL));
  assert(mount_calls == 0);
  failed_destination = NULL;

  const char *files[] = {
      "title/sce_sys/ICON0.PNG", "title/sce_sys/param.json",
      "app/FAKE12345/icon0.png", "app/FAKE12345/sce_sys/ICON0.PNG",
      "app/FAKE12345/sce_sys/icon0.png", "app/FAKE12345/sce_sys/param.json",
      "appmeta/FAKE12345/icon0.png", "appmeta/FAKE12345/param.json"};
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
    assert(unlink(files[i]) == 0);
  const char *directories[] = {"title/sce_sys", "title", "app/FAKE12345/sce_sys",
      "app/FAKE12345", "app", "appmeta/FAKE12345", "appmeta"};
  for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i)
    assert(rmdir(directories[i]) == 0);
  assert(chdir(previous_cwd) == 0);
}

/** Verify canonical copies, optional icons, retry behavior, and both install paths. */
int main(void) {
  char root[] = "/tmp/shadowmount-metadata-XXXXXX";
  assert(mkdtemp(root));
  char source[128], target[128];
  snprintf(source, sizeof(source), "%s/source", root);
  snprintf(target, sizeof(target), "%s/target", root);
  assert(mkdir(source, 0700) == 0);
  assert(mkdir(target, 0700) == 0);

  put(source, "ICON0.PNG", "usb-icon");
  put(source, "param.json", "{}");
  put(source, "CustomArt.PNG", "custom-art");
  assert(copy_sce_sys_to_appmeta(source, target));
  expect(target, "icon0.png", "usb-icon");
  expect(target, "param.json", "{}");
  expect(target, "CustomArt.PNG", "custom-art");
  char alias[MAX_PATH];
  snprintf(alias, sizeof(alias), "%s/ICON0.PNG", target);
  assert(access(alias, F_OK) != 0);

  // A retry repairs old staging without deleting existing files.
  put(target, "ICON0.PNG", "old-icon");
  assert(copy_sce_sys_to_appmeta(source, target));
  expect(target, "icon0.png", "usb-icon");
  expect(target, "ICON0.PNG", "old-icon");

  // Exact lowercase lookup wins on a case-sensitive source with both spellings.
  put(source, "icon0.png", "canonical-icon");
  assert(copy_sce_sys_to_appmeta(source, target));
  expect(target, "icon0.png", "canonical-icon");
  fail_icon_copy = true;
  assert(!copy_sce_sys_to_appmeta(source, target));
  fail_icon_copy = false;
  icon_stat_error = EACCES;
  assert(!copy_sce_sys_to_appmeta(source, target));
  icon_stat_error = 0;

  // An existing title can repair appmeta even when its source icon is absent.
  char source_icon[MAX_PATH];
  snprintf(source_icon, sizeof(source_icon), "%s/icon0.png", source);
  assert(unlink(source_icon) == 0);
  snprintf(source_icon, sizeof(source_icon), "%s/ICON0.PNG", source);
  assert(unlink(source_icon) == 0);
  assert(copy_sce_sys_to_appmeta(source, target));
  expect(target, "icon0.png", "canonical-icon");

  snprintf(source_icon, sizeof(source_icon), "%s/icon0.png", source);
  assert(mkdir(source_icon, 0700) == 0);
  assert(copy_sce_sys_to_appmeta(source, target));
  assert(rmdir(source_icon) == 0);

  const char *names[] = {"ICON0.PNG", "icon0.png", "param.json", "CustomArt.PNG"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", source, names[i]);
    if (i >= 2u)
      assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/%s", target, names[i]);
    assert(unlink(path) == 0);
  }
  assert(rmdir(source) == 0);
  assert(rmdir(target) == 0);
  test_staging(root);
  assert(rmdir(root) == 0);
  puts("install metadata regression tests passed");
  return 0;
}
