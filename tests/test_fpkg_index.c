/* Synthetic AMPR format boundary checks; no game package or cache writes. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sm_fpkg_index.h"

/* The index-writer fixture does not exercise fd-backed package identity. */
bool sm_pkg_source_matches(const sm_pkg_info_t *info, const struct stat *st) {
  (void)info;
  (void)st;
  return false;
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i)
    v |= (uint64_t)p[i] << (i * 8);
  return v;
}

static uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (unsigned b = 0; b < 8; ++b)
      crc = (crc >> 1) ^ (UINT32_C(0xedb88320) &
                           (uint32_t)-(int32_t)(crc & 1u));
  }
  return ~crc;
}

int main(void) {
  sm_fpkg_index_chunk_t raw = {
      .package_offset = 65536, .stored_length = 4, .raw_length = 4};
  sm_fpkg_index_file_t files[] = {
      {.path = "z.bin", .size = 2},
      {.path = "a.bin", .size = 4, .chunks = &raw, .chunk_count = 1},
  };
  sm_fpkg_index_blob_t index = {0}, manifest = {0};
  assert(sm_fpkg_index_build(files, 2, 262144, 123, &index, &manifest) ==
         SM_FPKG_OK);
  assert(index.size >= 0x30 && memcmp(index.data, "AMPRIDX3", 8) == 0);
  assert(le32(index.data + 8) == 3 && le32(index.data + 12) == 24);
  assert(le64(index.data + 16) == 2 && le64(index.data + 24) == 24);
  uint64_t slots_at = le64(index.data + 32);
  assert((slots_at & 15u) == 0 && slots_at < index.size);
  assert(memcmp(index.data + 0x30 + 48, "/app0/a.bin", 11) == 0);
  assert(memcmp(index.data + 0x30 + 48 + 12, "/app0/z.bin", 11) == 0);

  assert(manifest.size >= 128 && memcmp(manifest.data, "AMPRPAK4", 8) == 0);
  assert(le32(manifest.data + 8) == 4 && le32(manifest.data + 12) == 128);
  assert(le64(manifest.data + 40) == 2 && le64(manifest.data + 48) == 1);
  uint64_t files_at = le64(manifest.data + 72);
  uint64_t chunks_at = le64(manifest.data + 80);
  uint64_t pack_at = le64(manifest.data + 88);
  uint64_t strings_at = le64(manifest.data + 96);
  assert(files_at == 128 && chunks_at == files_at + 2 * 48);
  assert(pack_at == chunks_at + 12 && strings_at == pack_at + 32);
  assert(le64(manifest.data + chunks_at) == 65536);
  assert(le32(manifest.data + chunks_at + 8) ==
         (3u | (0x0cu << 22)));
  assert(crc32(manifest.data + 128, manifest.size - 128) ==
         le32(manifest.data + 112));
  uint32_t header_crc = le32(manifest.data + 116);
  memset(manifest.data + 116, 0, 4);
  assert(crc32(manifest.data, 128) == header_crc);
  sm_fpkg_index_blob_free(&index);
  sm_fpkg_index_blob_free(&manifest);

  sm_fpkg_index_chunk_t compressed = {
      .package_offset = 131072,
      .stored_length = 10,
      .raw_length = 1u << 18,
      .even_length = 4,
      .mode = 3,
      .kraken_encoded = true,
  };
  sm_fpkg_index_file_t compressed_file = {
      .path = "module.sprx", .size = 1, .chunks = &compressed, .chunk_count = 1};
  assert(sm_fpkg_index_build(&compressed_file, 1, 1048576, 123, &index,
                             &manifest) == SM_FPKG_OK);
  chunks_at = le64(manifest.data + 80);
  uint64_t expected_location = UINT64_C(131072) |
                               (UINT64_C(4) << 40) |
                               (UINT64_C(3) << 59);
  assert(le64(manifest.data + chunks_at) == expected_location);
  assert(le32(manifest.data + chunks_at + 8) ==
         (9u | (2u << 20) | (0x1cu << 22)));
  sm_fpkg_index_blob_free(&index);
  sm_fpkg_index_blob_free(&manifest);
  assert(sm_fpkg_index_build(&compressed_file, 1, 100000, 123, &index,
                             &manifest) == SM_FPKG_CORRUPT);

  sm_fpkg_index_file_t duplicate[] = {
      {.path = "dir/file", .size = 1},
      {.path = "DIR/FILE", .size = 1},
  };
  assert(sm_fpkg_index_build(duplicate, 2, 1, 0, &index, &manifest) ==
         SM_FPKG_CORRUPT);
  sm_fpkg_index_file_t traversal = {.path = "../escape", .size = 1};
  assert(sm_fpkg_index_build(&traversal, 1, 1, 0, &index, &manifest) ==
         SM_FPKG_CORRUPT);
  const char bad_utf8[] = {'x', '/', (char)0xff, '\0'};
  sm_fpkg_index_file_t invalid_utf8 = {.path = bad_utf8, .size = 1};
  assert(sm_fpkg_index_build(&invalid_utf8, 1, 1, 0, &index, &manifest) ==
         SM_FPKG_CORRUPT);
  puts("synthetic AMPR index layout and path boundaries passed");
  return 0;
}
