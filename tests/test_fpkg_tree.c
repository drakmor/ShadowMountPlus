/* Synthetic inner-PFS tree validation; no game data or package fixture. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sm_fpkg_internal.h"

#define MOUNT_SIZE 0xc0000u
#define META_BASE 0u
#define INNER_BLOCK 0x10000u
#define INODE_SIZE 0xa8u
#define INODE_TABLE_AT INNER_BLOCK
#define DIR_AT 0x80000u

typedef struct { uint8_t bytes[MOUNT_SIZE]; } memory_mount_t;

static void put_le16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8u * i));
}

static void put_le64(uint8_t *p, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(value >> (8u * i));
}

static sm_fpkg_status_t mount_read(void *ctx, uint64_t offset, void *output,
                                   size_t size) {
  memory_mount_t *mount = ctx;
  if (offset > sizeof(mount->bytes) ||
      size > sizeof(mount->bytes) - (size_t)offset)
    return SM_FPKG_CORRUPT;
  memcpy(output, mount->bytes + offset, size);
  return SM_FPKG_OK;
}

static size_t make_dirent(uint8_t *out, uint32_t inode, uint32_t kind,
                          const uint8_t *name, size_t name_length) {
  put_le32(out, inode);
  put_le32(out + 4u, kind);
  put_le32(out + 8u, (uint32_t)name_length);
  put_le32(out + 12u, (uint32_t)(16u + name_length));
  memcpy(out + 16u, name, name_length);
  return 16u + name_length;
}

static void set_inode(uint8_t *table, uint32_t inode, uint16_t mode,
                      uint64_t size, uint64_t logical) {
  uint8_t *raw = table + (size_t)inode * INODE_SIZE;
  put_le16(raw, mode);
  put_le64(raw + 8u, size);
  put_le64(raw + 0x60u, logical);
}

static void valid_mount(memory_mount_t *mount) {
  memset(mount, 0, sizeof(*mount));
  put_le64(mount->bytes, 2);
  put_le64(mount->bytes + 8u, 20130315u);
  put_le32(mount->bytes + 0x20u, INNER_BLOCK);
  put_le64(mount->bytes + 0x30u, 6);
  uint8_t *table = mount->bytes + INODE_TABLE_AT;
  set_inode(table, 4, 0x4000u, 25u, DIR_AT);
  set_inode(table, 5, 0x8000u, 0, 0);
  size_t dir_size = make_dirent(mount->bytes + DIR_AT, 5, 2,
                                (const uint8_t *)"eboot.bin", 9);
  put_le64(table + 4u * INODE_SIZE + 8u, dir_size);
}

int main(void) {
  memory_mount_t mount;
  valid_mount(&mount);
  sm_fpkg_tree_t tree = {0};
  assert(sm_fpkg_tree_open(mount_read, &mount, sizeof(mount.bytes),
                           META_BASE, &tree) == SM_FPKG_OK);
  assert(tree.count == 1 && strcmp(tree.entries[0].path, "eboot.bin") == 0);
  assert(!tree.entries[0].is_dir && tree.entries[0].logical_offset == 0);
  sm_fpkg_tree_free(&tree);

  memcpy(mount.bytes + 0x40000u, mount.bytes + INODE_TABLE_AT, INNER_BLOCK);
  memset(mount.bytes + INODE_TABLE_AT, 0, INNER_BLOCK);
  put_le32(mount.bytes + 0x20u, 0x40000u);
  assert(sm_fpkg_tree_open(mount_read, &mount, sizeof(mount.bytes),
                           META_BASE, &tree) == SM_FPKG_OK);
  assert(tree.count == 1 && strcmp(tree.entries[0].path, "eboot.bin") == 0);
  assert(!tree.entries[0].is_dir && tree.entries[0].logical_offset == 0);
  sm_fpkg_tree_free(&tree);
  assert(sm_fpkg_tree_validate(mount_read, &mount, sizeof(mount.bytes),
                               META_BASE) == SM_FPKG_OK);

  valid_mount(&mount);
  put_le32(mount.bytes + 0x20u, 0x40000u);
  assert(sm_fpkg_tree_validate(mount_read, &mount, sizeof(mount.bytes),
                               META_BASE) == SM_FPKG_CORRUPT);

  valid_mount(&mount);
  make_dirent(mount.bytes + DIR_AT, 5, 2,
              (const uint8_t *)"../escape", 9);
  assert(sm_fpkg_tree_validate(mount_read, &mount, sizeof(mount.bytes),
                               META_BASE) == SM_FPKG_CORRUPT);

  valid_mount(&mount);
  uint8_t invalid_name[] = {0xff};
  make_dirent(mount.bytes + DIR_AT, 5, 2, invalid_name,
              sizeof(invalid_name));
  assert(sm_fpkg_tree_validate(mount_read, &mount, sizeof(mount.bytes),
                               META_BASE) == SM_FPKG_CORRUPT);

  valid_mount(&mount);
  make_dirent(mount.bytes + DIR_AT, 4, 3, (const uint8_t *)"cycle", 5);
  put_le64(mount.bytes + INODE_TABLE_AT + 4u * INODE_SIZE + 8u, 21u);
  assert(sm_fpkg_tree_validate(mount_read, &mount, sizeof(mount.bytes),
                               META_BASE) == SM_FPKG_CORRUPT);

  valid_mount(&mount);
  put_le64(mount.bytes + 0x30u, 4000001u);
  assert(sm_fpkg_tree_validate(mount_read, &mount, sizeof(mount.bytes),
                               META_BASE) == SM_FPKG_CORRUPT);
  puts("synthetic inner-PFS tree bounds, paths and cycle checks passed");
  return 0;
}
