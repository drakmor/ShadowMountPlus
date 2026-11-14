/* A synthetic single-block Kraken descriptor; not a game FPKG fixture. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sm_fpkg_internal.h"

#define BLOCK 0x40000u
#define RECORD_AT 59u
#define DESCRIPTOR_SIZE 95u
#define OVERLAP_DESCRIPTOR_SIZE 113u

static void put_le64(uint8_t *p, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i)
    p[i] = (uint8_t)(v >> (8 * i));
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

static void make_descriptor(uint8_t bytes[DESCRIPTOR_SIZE]) {
  memset(bytes, 0, DESCRIPTOR_SIZE);
  /* comp=Kraken, four file boundaries, one 256 KiB ublock, four records. */
  put_le64(bytes, (UINT64_C(2) << 24) | 3u | (UINT64_C(1) << 32));
  put_le64(bytes + 8, UINT64_C(2) << 24);
  put_boundary(bytes + 16, 0, 0);
  put_boundary(bytes + 22, BLOCK, 0);
  put_boundary(bytes + 28, BLOCK, 0);
  put_boundary(bytes + 34, BLOCK, 0x40);
  bytes[RECORD_AT - 1u] = 4;
  /* Start anchor at record 0. */
  /* One full raw block at logical/stored offset zero. */
  uint8_t *block = bytes + RECORD_AT + 9u;
  put_bits(block, 14, 17, (1u << 17) - 1u);
  put_bits(block, 66, 1, 1);
  put_bits(block, 68, 4, 4); /* next anchor's stored offset is 4 * 64 KiB */
  /* Terminator anchor at stored offset 256 KiB. */
  uint8_t *terminator = bytes + RECORD_AT + 18u;
  put_bits(terminator, 26, 22, 1);
  put_bits(terminator, 67, 1, 1);
  /* Last record is the logical end sentinel: (mount % window) >> 4 == 0. */
}

static void make_overlapping_descriptor(
    uint8_t bytes[OVERLAP_DESCRIPTOR_SIZE]) {
  memset(bytes, 0, OVERLAP_DESCRIPTOR_SIZE);
  /* Two logical blocks; the second stored range reuses 62 prior bytes. */
  put_le64(bytes, (UINT64_C(2) << 24) | 3u | (UINT64_C(2) << 32));
  put_le64(bytes + 8, UINT64_C(4) << 24); /* Six records. */
  put_boundary(bytes + 16, 0, 0);
  put_boundary(bytes + 22, BLOCK, 0);
  put_boundary(bytes + 28, 2u * BLOCK, 0);
  put_boundary(bytes + 34, 2u * BLOCK, 0x40);
  bytes[RECORD_AT - 1u] = 4;

  uint8_t *first = bytes + RECORD_AT + 9u;
  put_bits(first, 14, 17, (1u << 17) - 1u);
  put_bits(first, 66, 1, 1); /* Record 2 anchors the second block. */
  put_bits(first, 68, 4, 3); /* Its start is just below the 256 KiB edge. */

  uint8_t *second_anchor = bytes + RECORD_AT + 18u;
  put_bits(second_anchor, 48, 18, BLOCK - 62u);

  uint8_t *second = bytes + RECORD_AT + 27u;
  put_bits(second, 14, 17, (1u << 17) - 1u);
  put_bits(second, 48, 18, BLOCK - 62u);
  put_bits(second, 66, 1, 1); /* Record 4 terminates the stored stream. */

  uint8_t *terminator = bytes + RECORD_AT + 36u;
  put_bits(terminator, 26, 22, 1);
  put_bits(terminator, 48, 18, BLOCK - 62u);
}

int main(void) {
  uint8_t descriptor[DESCRIPTOR_SIZE];
  sm_fpkg_layout_t layout = {0};
  make_descriptor(descriptor);
  assert(sm_fpkg_layout_parse(descriptor, sizeof(descriptor), BLOCK, &layout) ==
         SM_FPKG_OK);
  assert(layout.mount_size == BLOCK && layout.block_count == 1);
  assert(layout.blocks[0].logical_offset == 0);
  assert(layout.blocks[0].stored_offset == 0);
  assert(layout.blocks[0].logical_length == BLOCK);
  assert(layout.blocks[0].stored_length == BLOCK);
  assert(layout.blocks[0].even_length == BLOCK / 2u);
  assert(layout.blocks[0].mode == 0 && layout.blocks[0].raw);
  sm_fpkg_layout_free(&layout);

  uint8_t overlapping[OVERLAP_DESCRIPTOR_SIZE];
  make_overlapping_descriptor(overlapping);
  assert(sm_fpkg_layout_parse(overlapping, sizeof(overlapping), 2u * BLOCK,
                              &layout) == SM_FPKG_OK);
  assert(layout.mount_size == 2u * BLOCK && layout.block_count == 2);
  assert(layout.blocks[0].logical_offset == 0);
  assert(layout.blocks[0].stored_offset == 0);
  assert(layout.blocks[1].logical_offset == BLOCK);
  assert(layout.blocks[1].stored_offset == BLOCK - 62u);
  assert(layout.blocks[1].stored_length == BLOCK);
  sm_fpkg_layout_free(&layout);

  assert(sm_fpkg_layout_parse(descriptor, sizeof(descriptor) - 1u, BLOCK,
                              &layout) == SM_FPKG_CORRUPT);
  uint8_t invalid[DESCRIPTOR_SIZE];
  memcpy(invalid, descriptor, sizeof(invalid));
  put_le64(invalid, 3u | (UINT64_C(1) << 32));
  assert(sm_fpkg_layout_parse(invalid, sizeof(invalid), BLOCK, &layout) ==
         SM_FPKG_UNSUPPORTED);
  memcpy(invalid, descriptor, sizeof(invalid));
  put_le64(invalid, (UINT64_C(2) << 24) | 3u | (UINT64_C(1) << 32) |
                         (UINT64_C(1) << 28));
  assert(sm_fpkg_layout_parse(invalid, sizeof(invalid), BLOCK, &layout) ==
         SM_FPKG_UNSUPPORTED);
  memcpy(invalid, descriptor, sizeof(invalid));
  put_boundary(invalid + 16, 0, 0x40);
  assert(sm_fpkg_layout_parse(invalid, sizeof(invalid), BLOCK, &layout) ==
         SM_FPKG_UNSUPPORTED);
  assert(sm_fpkg_layout_parse(descriptor, sizeof(descriptor), BLOCK - 1u,
                              &layout) == SM_FPKG_CORRUPT);
  puts("synthetic Kraken layout bounds passed");
  return 0;
}
