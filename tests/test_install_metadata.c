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

static uint32_t sm_firmware_major_version(void) { return 11u; }
int sceKernelUsleep(unsigned int microseconds);

#include "../src/sm_install.c"

static bool fail_icon_copy;

int copy_file(const char *src, const char *dst) {
  if (fail_icon_copy && strstr(dst, "/icon0.png")) {
    errno = EIO;
    return -1;
  }
  FILE *input = fopen(src, "rb");
  if (!input && strlen(src) >= 9u &&
      strcmp(src + strlen(src) - 9u, "icon0.png") == 0) {
    char actual[MAX_PATH];
    assert(strlen(src) < sizeof(actual));
    strcpy(actual, src);
    strcpy(actual + strlen(actual) - 9u, "ICON0.PNG");
    input = fopen(actual, "rb");
  }
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

  const char *names[] = {"ICON0.PNG", "icon0.png", "param.json", "CustomArt.PNG"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", source, names[i]);
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
