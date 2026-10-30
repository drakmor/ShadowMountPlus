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

static uint32_t sm_firmware_major_version(void) { return 11u; }
int sceKernelUsleep(unsigned int microseconds);

static const char *usb_source_path(const char *src, char actual[MAX_PATH]);
static int usb_stat(const char *path, struct stat *st);
#define stat(path, st) usb_stat(path, st)
#include "../src/sm_install.c"
#undef stat

static bool fail_icon_copy;
static int icon_stat_error;

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

static int usb_stat(const char *path, struct stat *st) {
  if (icon_stat_error && strstr(path, "/icon0.png")) {
    errno = icon_stat_error;
    return -1;
  }
  char actual[MAX_PATH];
  return stat(usb_source_path(path, actual), st);
}

int copy_file(const char *src, const char *dst) {
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
  assert(rmdir(root) == 0);
  puts("install metadata regression tests passed");
  return 0;
}
