/* Synthetic envelope checks only; this is not a game FPKG compatibility fixture. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sm_fpkg.h"
#include "sm_fpkg_index.h"

#define OUTER_BLOCK 0x10000u
#define OUTER_BLOCK_COUNT 24u
#define PFS_SIZE (OUTER_BLOCK * OUTER_BLOCK_COUNT)
#define PFS_END (OUTER_BLOCK + PFS_SIZE)
#define CNT_OFFSET PFS_END
#define FILE_SIZE (CNT_OFFSET + 0x2000u)
#define DINODE_SIZE 0x2C8u
#define INNER_INODE_SIZE 0xA8u
#define DESCRIPTOR_SIZE 113u
#define DESCRIPTOR_RECORD_AT 59u

typedef struct {
  uint8_t bytes[FILE_SIZE];
  uint64_t fail_offset;
  int fail_read;
} memory_pkg_t;

/* This fixture opens only callback-backed readers, never an fd-backed source. */
#ifndef SM_TEST_FPKG_HEADER_EXTERNAL_SOURCE_MATCHES
bool sm_pkg_source_matches(const sm_pkg_info_t *info, const struct stat *st) {
  (void)info;
  (void)st;
  return false;
}
#endif

static void put_le32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i)
    p[i] = (uint8_t)(v >> (i * 8));
}

static void put_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put_le64(uint8_t *p, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i)
    p[i] = (uint8_t)(v >> (i * 8));
}

static uint64_t get_le64(const uint8_t *p) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i)
    v |= (uint64_t)p[i] << (i * 8u);
  return v;
}

static uint32_t get_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_bits(uint8_t *record, unsigned low, unsigned width,
                     uint64_t value) {
  for (unsigned i = 0; i < width; ++i) {
    if ((value >> i) & 1u)
      record[(low + i) / 8u] |= (uint8_t)(1u << ((low + i) % 8u));
  }
}

static void put_boundary(uint8_t *p, uint64_t value, uint8_t kind) {
  for (unsigned i = 0; i < 5; ++i)
    p[i] = (uint8_t)(value >> (8 * i));
  p[5] = kind;
}

static void make_layout_descriptor(uint8_t *bytes) {
  memset(bytes, 0, DESCRIPTOR_SIZE);
  put_le64(bytes, (UINT64_C(2) << 24) | 3u | (UINT64_C(3) << 32));
  put_le64(bytes + 8, UINT64_C(4) << 24);
  put_boundary(bytes + 16, 0, 0);
  put_boundary(bytes + 22, 0x40000u, 0);
  put_boundary(bytes + 28, 0x80000u, 0);
  put_boundary(bytes + 34, 0xC0000u, 0);
  put_boundary(bytes + 40, 0xC0000u, 0x40);
  bytes[DESCRIPTOR_RECORD_AT - 1u] = 6;
  for (unsigned i = 0; i < 3; ++i) {
    uint8_t *block = bytes + DESCRIPTOR_RECORD_AT + 9u + i * 9u;
    put_bits(block, 14, 17, (1u << 17) - 1u);
    if (i == 2) {
      put_bits(block, 66, 1, 1);
      put_bits(block, 68, 4, 12);
    }
  }
  put_bits(bytes + DESCRIPTOR_RECORD_AT + 36u, 26, 22, 3);
}

static void put_inner_image(memory_pkg_t *pkg, uint64_t offset,
                            const uint8_t *bytes, size_t size) {
  static const uint32_t outer_blocks[] = {4, 5, 6, 7, 8, 9,
                                           10, 11, 12, 13, 14, 15};
  size_t done = 0;
  while (done < size) {
    uint64_t at = offset + done;
    size_t map_index = (size_t)(at / OUTER_BLOCK);
    size_t within = (size_t)(at % OUTER_BLOCK);
    size_t take = OUTER_BLOCK - within;
    if (take > size - done) take = size - done;
    uint64_t package_offset = OUTER_BLOCK +
                              (uint64_t)outer_blocks[map_index] * OUTER_BLOCK +
                              within;
    memcpy(pkg->bytes + package_offset, bytes + done, take);
    done += take;
  }
}

static int memory_read(void *ctx, uint64_t offset, void *out, size_t size) {
  memory_pkg_t *pkg = ctx;
  if (pkg->fail_read && offset == pkg->fail_offset)
    return 5;
  if (offset > sizeof(pkg->bytes) || size > sizeof(pkg->bytes) - offset)
    return 5;
  memcpy(out, pkg->bytes + offset, size);
  return 0;
}

static void set_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void make_dirent(uint8_t *out, uint32_t inode, const char *name) {
  size_t name_length = strlen(name);
  put_le32(out, inode);
  put_le32(out + 8, (uint32_t)name_length);
  put_le32(out + 12, (uint32_t)(16u + name_length));
  memcpy(out + 16, name, name_length);
}

static void set_dinode(uint8_t *out, uint64_t size, uint32_t first_block) {
  put_le64(out + 8, size);
  put_le32(out + 0x64 + 32, first_block);
}

static void valid_envelope(memory_pkg_t *pkg) {
  memset(pkg, 0, sizeof(*pkg));
  uint8_t *fih = pkg->bytes;
  memcpy(fih, "\x7f" "FIH", 4);
  fih[5] = 0;
  fih[6] = 3;
  put_le32(fih + 0x60, OUTER_BLOCK);
  put_le64(fih + 0x10, OUTER_BLOCK);
  put_le64(fih + 0x18, PFS_SIZE);
  put_le64(fih + 0x20, OUTER_BLOCK);
  put_le32(fih + 0x50, 1);
  put_le64(fih + 0x58, CNT_OFFSET);
  set_be32(pkg->bytes + CNT_OFFSET, 0x7F434E54u);
  put_le64(pkg->bytes + OUTER_BLOCK, 2);
  put_le64(pkg->bytes + OUTER_BLOCK + 8, 20130315u);
  memcpy(pkg->bytes + OUTER_BLOCK + 0x370, "PPRPLAIN-NOAUTH!", 16);
  put_le64(pkg->bytes + OUTER_BLOCK + 0x30, 4);
  put_le32(pkg->bytes + OUTER_BLOCK + 0xD8, 1);

  uint8_t *inodes = pkg->bytes + 2u * OUTER_BLOCK;
  set_dinode(inodes + 0u * DINODE_SIZE, 21, 2);
  set_dinode(inodes + 1u * DINODE_SIZE, 64, 3);
  set_dinode(inodes + 2u * DINODE_SIZE, 12u * OUTER_BLOCK, 4);
  for (unsigned i = 1; i < 12; ++i)
    put_le32(inodes + 2u * DINODE_SIZE + 0x64u + i * 36u + 32u,
             i + 4u);
  set_dinode(inodes + 3u * DINODE_SIZE, DESCRIPTOR_SIZE, 22);

  make_dirent(pkg->bytes + 3u * OUTER_BLOCK, 1, "uroot");
  uint8_t *uroot = pkg->bytes + 4u * OUTER_BLOCK;
  make_dirent(uroot, 2, "pfs_image.dat");
  make_dirent(uroot + 29, 3, "naps_pkg_layout.dat");
  make_layout_descriptor(pkg->bytes + 23u * OUTER_BLOCK);

  uint8_t inner_superblock[0x40] = {0};
  put_le64(inner_superblock, 2);
  put_le64(inner_superblock + 8, 20130315u);
  put_le32(inner_superblock + 0x20, 0x40000u);
  put_le64(inner_superblock + 0x30, 6);
  put_inner_image(pkg, 0x10000u, inner_superblock, sizeof(inner_superblock));
  uint8_t root_inode[INNER_INODE_SIZE] = {0};
  put_le16(root_inode, 0x4000u);
  put_le64(root_inode + 8, 25);
  put_le64(root_inode + 0x60, 0xA0000u);
  put_inner_image(pkg, 0x50000u + 4u * INNER_INODE_SIZE, root_inode,
                  sizeof(root_inode));
  uint8_t file_inode[INNER_INODE_SIZE] = {0};
  put_le16(file_inode, 0x8000u);
  put_le64(file_inode + 8, 13);
  put_le64(file_inode + 0x60, 0);
  put_inner_image(pkg, 0x50000u + 5u * INNER_INODE_SIZE, file_inode,
                  sizeof(file_inode));
  uint8_t dirent[25] = {0};
  make_dirent(dirent, 5, "eboot.bin");
  put_le32(dirent + 4, 2);
  put_inner_image(pkg, 0xA0000u, dirent, sizeof(dirent));
  static const uint8_t payload[] = "hello native!";
  put_inner_image(pkg, 0, payload, sizeof(payload) - 1u);
}

static void set_indirect_image_file(memory_pkg_t *pkg, uint32_t indirect_data) {
  static const uint32_t original_map[] = {4, 5, 6, 7, 8, 9,
                                           10, 11, 12, 13, 14, 15};
  uint8_t image[12u * OUTER_BLOCK];
  for (unsigned i = 0; i < 12; ++i)
    memcpy(image + (size_t)i * OUTER_BLOCK,
           pkg->bytes + OUTER_BLOCK +
               (size_t)original_map[i] * OUTER_BLOCK,
           OUTER_BLOCK);
  uint8_t *dinode = pkg->bytes + 2u * OUTER_BLOCK + DINODE_SIZE * 2u;
  put_le64(dinode + 8, 13u * OUTER_BLOCK);
  for (unsigned i = 0; i < 12; ++i)
    put_le32(dinode + 0x64u + i * 36u + 32u, 8u + i);
  put_le32(dinode + 0x64u + 12u * 36u + 32u, 20);
  put_le32(pkg->bytes + 21u * OUTER_BLOCK + 32u, indirect_data);
  for (unsigned i = 0; i < 12; ++i)
    memcpy(pkg->bytes + OUTER_BLOCK + (size_t)(8u + i) * OUTER_BLOCK,
           image + (size_t)i * OUTER_BLOCK, OUTER_BLOCK);
}

static sm_pkg_info_t metadata(void) {
  sm_pkg_info_t info;
  memset(&info, 0, sizeof(info));
  strcpy(info.title_id, "PPSA00001");
  info.pkg_kind = PKG_KIND_BASE;
  info.pkg_classification_confident = true;
  return info;
}

static bool extract_eboot(const char *path, void *ctx) {
  (void)ctx;
  return strcmp(path, "eboot.bin") == 0;
}

typedef struct {
  const char *expected_path;
  uint8_t bytes[64];
  size_t size;
  size_t written;
  bool begun;
  bool finished;
  bool committed;
  bool cancel_write;
} memory_sink_t;

static sm_fpkg_status_t sink_begin(void *ctx, const sm_fpkg_file_t *file,
                                   void **file_ctx_out) {
  memory_sink_t *sink = ctx;
  assert(strcmp(file->path, sink->expected_path) == 0);
  assert(!file->is_directory && file->size <= sizeof(sink->bytes));
  sink->size = (size_t)file->size;
  sink->written = 0;
  sink->begun = true;
  *file_ctx_out = sink;
  return SM_FPKG_OK;
}

static sm_fpkg_status_t sink_write(void *file_ctx, uint64_t offset,
                                   const void *bytes, size_t size) {
  memory_sink_t *sink = file_ctx;
  if (sink->cancel_write)
    return SM_FPKG_CANCELLED;
  assert(offset == sink->written && size <= sink->size - sink->written);
  memcpy(sink->bytes + sink->written, bytes, size);
  sink->written += size;
  return SM_FPKG_OK;
}

static sm_fpkg_status_t sink_finish(void *file_ctx, bool commit) {
  memory_sink_t *sink = file_ctx;
  sink->finished = true;
  sink->committed = commit;
  if (commit)
    assert(sink->written == sink->size);
  return SM_FPKG_OK;
}

int main(void) {
  memory_pkg_t pkg;
  sm_pkg_info_t info = metadata();
  valid_envelope(&pkg);
  sm_fpkg_t *fpkg = NULL;
  assert(sm_fpkg_open_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                             &pkg, &info, &fpkg) == SM_FPKG_OK);
  assert(fpkg && sm_fpkg_file_count(fpkg) == 1);
  sm_fpkg_file_t file;
  assert(sm_fpkg_file_at(fpkg, 0, &file));
  assert(strcmp(file.path, "eboot.bin") == 0 && file.size == 13 &&
         !file.is_directory);
  size_t block_count = 0;
  assert(sm_fpkg_file_block_count(fpkg, 0, &block_count) == SM_FPKG_OK);
  assert(block_count == 1);
  assert(sm_fpkg_file_blocks(fpkg, 0, NULL, 0, &block_count) ==
         SM_FPKG_UNSUPPORTED);
  assert(block_count == 1);
  sm_fpkg_block_t block;
  assert(sm_fpkg_file_blocks(fpkg, 0, &block, 1, &block_count) ==
         SM_FPKG_OK);
  assert(block_count == 1 && block.package_offset == 5u * OUTER_BLOCK &&
         block.stored_size == 0x40000u && block.raw_size == 0x40000u &&
         !block.kraken_encoded);
  sm_fpkg_index_blob_t generated_index = {0}, generated_manifest = {0};
  assert(sm_fpkg_index_build_package(fpkg, NULL, NULL, 123,
                                     &generated_index, &generated_manifest) ==
         SM_FPKG_OK);
  assert(memcmp(generated_index.data, "AMPRIDX3", 8) == 0);
  assert(memcmp(generated_manifest.data, "AMPRPAK4", 8) == 0);
  assert(get_le64(generated_manifest.data + 40) == 1);
  assert(get_le64(generated_manifest.data + 48) == 1);
  sm_fpkg_index_blob_free(&generated_index);
  sm_fpkg_index_blob_free(&generated_manifest);
  assert(sm_fpkg_index_build_package(fpkg, extract_eboot, NULL, 123,
                                     &generated_index, &generated_manifest) ==
         SM_FPKG_OK);
  assert(get_le64(generated_manifest.data + 48) == 0);
  assert(get_le32(generated_manifest.data + 128u + 40u) == 0);
  sm_fpkg_index_blob_free(&generated_index);
  sm_fpkg_index_blob_free(&generated_manifest);
  sm_fpkg_index_file_t overrides[] = {
      {.path = "EBOOT.BIN", .size = 7},
      {.path = "patch/new.dat", .size = 4},
  };
  sm_fpkg_index_blob_t expected_index = {0}, expected_manifest = {0};
  assert(sm_fpkg_index_build_package_with_overrides(
             fpkg, NULL, NULL, NULL, NULL, overrides, 2, 123,
             &generated_index, &generated_manifest) == SM_FPKG_OK);
  assert(sm_fpkg_index_build(overrides, 2, sizeof(pkg.bytes), 123,
                             &expected_index, &expected_manifest) == SM_FPKG_OK);
  assert(generated_index.size == expected_index.size &&
         memcmp(generated_index.data, expected_index.data,
                expected_index.size) == 0);
  assert(generated_manifest.size == expected_manifest.size &&
         memcmp(generated_manifest.data, expected_manifest.data,
                expected_manifest.size) == 0);
  sm_fpkg_index_blob_free(&generated_index);
  sm_fpkg_index_blob_free(&generated_manifest);
  sm_fpkg_index_blob_free(&expected_index);
  sm_fpkg_index_blob_free(&expected_manifest);
  assert(sm_fpkg_index_build_package_with_overrides(
             fpkg, NULL, NULL, extract_eboot, NULL, &overrides[1], 1, 123,
             &generated_index, &generated_manifest) == SM_FPKG_OK);
  assert(get_le64(generated_manifest.data + 40) == 1u &&
         get_le64(generated_manifest.data + 48) == 0u);
  sm_fpkg_index_blob_free(&generated_index);
  sm_fpkg_index_blob_free(&generated_manifest);
  overrides[1].path = "eboot.bin";
  assert(sm_fpkg_index_build_package_with_overrides(
             fpkg, NULL, NULL, NULL, NULL, overrides, 2, 123,
             &generated_index, &generated_manifest) == SM_FPKG_CORRUPT);
  uint8_t payload[13];
  assert(sm_fpkg_read_file(fpkg, 0, 0, payload, sizeof(payload)) ==
         SM_FPKG_OK);
  assert(memcmp(payload, "hello native!", sizeof(payload)) == 0);
  memory_sink_t sink = {.expected_path = "eboot.bin"};
  sm_fpkg_extract_sink_t extract_sink = {
      .begin = sink_begin,
      .write = sink_write,
      .finish = sink_finish,
      .ctx = &sink,
  };
  assert(sm_fpkg_extract_file(fpkg, 0, &extract_sink) == SM_FPKG_OK);
  assert(sink.begun && sink.finished && sink.committed &&
         sink.written == sizeof(payload));
  assert(memcmp(sink.bytes, "hello native!", sizeof(payload)) == 0);
  memset(&sink, 0, sizeof(sink));
  sink.expected_path = "eboot.bin";
  sink.cancel_write = true;
  assert(sm_fpkg_extract_file(fpkg, 0, &extract_sink) ==
         SM_FPKG_CANCELLED);
  assert(sink.begun && sink.finished && !sink.committed);
  assert(sm_fpkg_read_file(fpkg, 0, 12, payload, 2) == SM_FPKG_CORRUPT);
  sm_fpkg_close(fpkg);
  valid_envelope(&pkg);
  set_be32(pkg.bytes + CNT_OFFSET + 0x10u, 1);
  set_be32(pkg.bytes + CNT_OFFSET + 0x18u, 0x1000u);
  uint8_t *cnt_row = pkg.bytes + CNT_OFFSET + 0x1000u;
  set_be32(cnt_row, 0x1200u);
  set_be32(cnt_row + 16u, 0x1800u);
  set_be32(cnt_row + 20u, 14u);
  memcpy(pkg.bytes + CNT_OFFSET + 0x1800u, "CNT icon data!", 14u);
  assert(sm_fpkg_open_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                             &pkg, &info, &fpkg) == SM_FPKG_OK);
  assert(sm_fpkg_file_count(fpkg) == 3);
  size_t icon_index = SIZE_MAX;
  for (size_t i = 0; i < sm_fpkg_file_count(fpkg); ++i) {
    assert(sm_fpkg_file_at(fpkg, i, &file));
    if (strcmp(file.path, "sce_sys/icon0.png") == 0)
      icon_index = i;
  }
  assert(icon_index != SIZE_MAX);
  uint8_t icon[14];
  assert(sm_fpkg_read_file(fpkg, icon_index, 0, icon, sizeof(icon)) ==
         SM_FPKG_OK);
  assert(memcmp(icon, "CNT icon data!", sizeof(icon)) == 0);
  assert(sm_fpkg_file_block_count(fpkg, icon_index, &block_count) ==
         SM_FPKG_OK);
  assert(block_count == 1);
  sm_fpkg_block_t cnt_block;
  assert(sm_fpkg_file_blocks(fpkg, icon_index, &cnt_block, 1, &block_count) ==
         SM_FPKG_OK);
  assert(cnt_block.package_offset == CNT_OFFSET + 0x1800u &&
         cnt_block.stored_size == 14u && cnt_block.raw_size == 14u &&
         !cnt_block.kraken_encoded);
  sm_fpkg_close(fpkg);
  valid_envelope(&pkg);
  uint8_t unaligned_offset[8];
  put_le64(unaligned_offset, 0x2000u);
  put_inner_image(&pkg, 0x50000u + 5u * INNER_INODE_SIZE + 0x60u,
                  unaligned_offset, sizeof(unaligned_offset));
  assert(sm_fpkg_open_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                             &pkg, &info, &fpkg) == SM_FPKG_OK);
  assert(sm_fpkg_file_block_count(fpkg, 0, &block_count) ==
         SM_FPKG_UNSUPPORTED);
  sm_fpkg_close(fpkg);
  valid_envelope(&pkg);
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_OK);

  pkg.bytes[5] = 1;
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_UNSUPPORTED);
  valid_envelope(&pkg);
  memcpy(pkg.bytes + OUTER_BLOCK + 0x370, "NOT-PLAINTEXT!!", 16);
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_UNSUPPORTED);
  valid_envelope(&pkg);
  memcpy(pkg.bytes, "\x7f" "LIH", 4);
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_UNSUPPORTED);
  valid_envelope(&pkg);
  put_le64(pkg.bytes + 0x18, UINT64_MAX);
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_CORRUPT);
  valid_envelope(&pkg);
  put_le64(pkg.bytes + OUTER_BLOCK + 0x30, UINT64_MAX);
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_CORRUPT);
  valid_envelope(&pkg);
  pkg.bytes[3u * OUTER_BLOCK + 16u] = 'x';
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_CORRUPT);
  valid_envelope(&pkg);
  put_le32(pkg.bytes + 3u * OUTER_BLOCK + 12u, 15);
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_CORRUPT);
  valid_envelope(&pkg);
  put_le32(pkg.bytes + 3u * OUTER_BLOCK, 4);
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_CORRUPT);
  valid_envelope(&pkg);
  pkg.fail_read = 1;
  pkg.fail_offset = OUTER_BLOCK;
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_IO);
  valid_envelope(&pkg);
  set_be32(pkg.bytes + CNT_OFFSET + 0x10u, 4097);
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_CORRUPT);
  valid_envelope(&pkg);
  set_be32(pkg.bytes + CNT_OFFSET + 0x10u, 1);
  set_be32(pkg.bytes + CNT_OFFSET + 0x18u, 0x1000);
  set_be32(pkg.bytes + CNT_OFFSET + 0x1000u + 16u, 0x2000);
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_OK);
  set_be32(pkg.bytes + CNT_OFFSET + 0x1000u + 20u, 1);
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_CORRUPT);
  valid_envelope(&pkg);
  set_indirect_image_file(&pkg, 21);
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_OK);
  valid_envelope(&pkg);
  set_indirect_image_file(&pkg, 8);
  assert(sm_fpkg_probe_stream("game.pkg", sizeof(pkg.bytes), memory_read,
                              &pkg, &info) == SM_FPKG_CORRUPT);

  puts("synthetic FIH/PFS descriptor mapping and malformed-input checks passed");
  return 0;
}
