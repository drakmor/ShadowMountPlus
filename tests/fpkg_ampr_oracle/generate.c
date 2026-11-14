/* Writes a disposable synthetic AMPR index/manifest and pack payload. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utime.h>

#include "sm_fpkg_index.h"

#define PACKAGE_SIZE 0x100000u
#define BLOCK_SIZE 0x40000u
#define EBOOT_OFFSET 0x20000u
#define PARAM_OFFSET 0x60000u
#define STREAM_HEAD_OFFSET 0x80000u
#define STREAM_TAIL_OFFSET 0xc0000u
#define PARAM_SIZE 257u
#define STREAM_TAIL_SIZE 37u

/* This writer-only fixture never opens a source package through sm_fpkg.c. */
bool sm_pkg_source_matches(const sm_pkg_info_t *info, const struct stat *st) {
  (void)info;
  (void)st;
  return true;
}

static uint8_t pattern_eboot(size_t index) {
  return (uint8_t)((index * 29u + index / 113u) & 0xffu);
}

static uint8_t pattern_stream_head(size_t index) {
  return (uint8_t)((index * 7u + index / 17u) & 0xffu);
}

static uint8_t pattern_stream_tail(size_t index) {
  return (uint8_t)((index * 13u + index / 41u) & 0xffu);
}

static int write_file(const char *path, const void *bytes, size_t size) {
  FILE *file = fopen(path, "wb");
  if (!file)
    return -1;
  int result = fwrite(bytes, 1, size, file) == size ? 0 : -1;
  if (fclose(file) != 0)
    result = -1;
  return result;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s OUTPUT_DIRECTORY\n", argv[0]);
    return 2;
  }
  char index_path[4096], manifest_path[4096], package_path[4096];
  int index_len = snprintf(index_path, sizeof(index_path), "%s/ampr_emu.index", argv[1]);
  int manifest_len = snprintf(manifest_path, sizeof(manifest_path), "%s/ampr_assets.index", argv[1]);
  int package_len = snprintf(package_path, sizeof(package_path), "%s/kraken.pkg", argv[1]);
  if (index_len < 0 || (size_t)index_len >= sizeof(index_path) ||
      manifest_len < 0 || (size_t)manifest_len >= sizeof(manifest_path) ||
      package_len < 0 || (size_t)package_len >= sizeof(package_path)) {
    fprintf(stderr, "output path formatting failed\n");
    return 2;
  }

  sm_fpkg_index_chunk_t chunks[4] = {
      {
          .package_offset = EBOOT_OFFSET,
          .stored_length = BLOCK_SIZE,
          .raw_length = BLOCK_SIZE,
          .even_length = 0x20000u,
          .mode = 0,
          .kraken_encoded = true,
      },
      {
          .package_offset = PARAM_OFFSET,
          .stored_length = PARAM_SIZE,
          .raw_length = PARAM_SIZE,
          .kraken_encoded = false,
      },
      {
          .package_offset = STREAM_HEAD_OFFSET,
          .stored_length = BLOCK_SIZE,
          .raw_length = BLOCK_SIZE,
          .kraken_encoded = false,
      },
      {
          .package_offset = STREAM_TAIL_OFFSET,
          .stored_length = BLOCK_SIZE,
          .raw_length = BLOCK_SIZE,
          .even_length = 0x20000u,
          .mode = 0,
          .kraken_encoded = true,
      },
  };
  sm_fpkg_index_file_t files[3] = {
      {
          .path = "eboot.bin",
          .size = BLOCK_SIZE,
          .chunks = &chunks[0],
          .chunk_count = 1,
      },
      {
          .path = "sce_sys/param.json",
          .size = PARAM_SIZE,
          .chunks = &chunks[1],
          .chunk_count = 1,
      },
      {
          .path = "assets/stream.bin",
          .size = BLOCK_SIZE + STREAM_TAIL_SIZE,
          .chunks = &chunks[2],
          .chunk_count = 2,
      },
  };
  sm_fpkg_index_blob_t index = {0}, manifest = {0};
  sm_fpkg_status_t status = sm_fpkg_index_build(
      files, sizeof(files) / sizeof(files[0]), PACKAGE_SIZE, 123, &index,
      &manifest);
  if (status != SM_FPKG_OK) {
    fprintf(stderr, "AMPR index generation failed: %d\n", status);
    return 1;
  }
  uint8_t *package = calloc(PACKAGE_SIZE, 1);
  if (!package) {
    sm_fpkg_index_blob_free(&index);
    sm_fpkg_index_blob_free(&manifest);
    return 1;
  }
  for (size_t i = 0; i < BLOCK_SIZE; ++i) {
    package[EBOOT_OFFSET + i] = pattern_eboot(i);
    package[STREAM_HEAD_OFFSET + i] = pattern_stream_head(i);
    package[STREAM_TAIL_OFFSET + i] = pattern_stream_tail(i);
  }
  for (size_t i = 0; i < PARAM_SIZE; ++i)
    package[PARAM_OFFSET + i] = (uint8_t)(i ^ 0x5au);
  int result = write_file(index_path, index.data, index.size) |
               write_file(manifest_path, manifest.data, manifest.size) |
               write_file(package_path, package, PACKAGE_SIZE);
  if (result == 0) {
    struct utimbuf fixed_time = {.actime = 123, .modtime = 123};
    if (utime(package_path, &fixed_time) != 0)
      result = -1;
  }
  if (result != 0)
    fprintf(stderr, "failed writing oracle inputs: %s\n", strerror(errno));
  free(package);
  sm_fpkg_index_blob_free(&index);
  sm_fpkg_index_blob_free(&manifest);
  return result == 0 ? 0 : 1;
}
