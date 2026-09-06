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
static char test_config_path[1024];
static char test_autotune_path[1024];
#undef CONFIG_FILE
#undef AUTOTUNE_FILE
#define CONFIG_FILE test_config_path
#define AUTOTUNE_FILE test_autotune_path

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

#include "../src/sm_config_mount.c"

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
          "api_port=%u\n"
          "global_fakelib_path=/data/version%u\n"
          "emulators_path=/data/version%u\n"
          "global_fakelib_exclude=PPSA12345\n"
          "scanpath=/data/games%u\n"
          "image_ro=game.exfat\n"
          "image_sector=game.exfat:%u\n"
          "kstuff_no_pause=PPSA12345\n"
          "kstuff_delay=PPSA12345:%u\n",
          9100u + version, version, version, version,
          version == 1u ? 512u : 4096u, version);
  assert(fclose(file) == 0);
  assert(rename(next_path, CONFIG_FILE) == 0);
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
    uint32_t sector = 0, delay = 0;
    assert(get_image_mode_override("game.exfat", &read_only) && read_only);
    assert(get_image_sector_size_override("game.exfat", &sector));
    assert(sector == 512u || sector == 4096u);
    assert(is_kstuff_pause_disabled_for_title("PPSA12345"));
    assert(is_global_fakelib_excluded_for_title("PPSA12345"));
    assert(get_kstuff_pause_delay_override_for_title("PPSA12345", &delay));
    assert(delay == 1u || delay == 2u);
    atomic_fetch_add(&read_count, 1u);
  } while (!atomic_load(&reader_stop));
  return NULL;
}

int main(void) {
  char directory[] = "/tmp/shadowmount-config-test-XXXXXX";
  assert(mkdtemp(directory));
  snprintf(test_config_path, sizeof(test_config_path), "%s/config.ini", directory);
  snprintf(test_autotune_path, sizeof(test_autotune_path), "%s/autotune.ini", directory);

  pthread_t readers[4];
  assert(pthread_barrier_init(&reader_barrier, NULL, 5) == 0);
  for (size_t i = 0; i < 4; i++)
    assert(pthread_create(&readers[i], NULL, init_reader, NULL) == 0);
  (void)pthread_barrier_wait(&reader_barrier);
  for (size_t i = 0; i < 4; i++)
    assert(pthread_join(readers[i], NULL) == 0);

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
  assert(!is_kstuff_pause_disabled_for_title("PPSA99999"));
  assert(!is_global_fakelib_excluded_for_title("PPSA99999"));
  assert(!get_kstuff_pause_delay_override_for_title("PPSA99999", &value));
  write_config(1);
  assert(reload_runtime_config_if_changed(&changed) && !changed);
  assert(unlink(CONFIG_FILE) == 0);
  assert(reload_runtime_config_if_changed(&changed) && changed);
  assert(runtime_config().api_port == SM_API_DEFAULT_PORT);
  assert(rmdir(directory) == 0);
  assert(pthread_barrier_destroy(&reader_barrier) == 0);
  printf("config: 1000 reloads, %u concurrent reads, snapshots preserved\n",
         atomic_load(&read_count));
  return 0;
}
