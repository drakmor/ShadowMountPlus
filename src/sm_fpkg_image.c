/* Virtual inner-image reads through naps_pkg_layout.dat and pfs_image.dat. */
#include "sm_fpkg_internal.h"
#include "sm_fpkg_decoder.h"

#include <stdlib.h>
#include <string.h>

#define IMAGE_OUTER_BLOCK 0x10000u
#define IMAGE_MAX_STORED_BLOCK (2u * 0x40000u)
#define IMAGE_MAX_LOGICAL_BLOCK 0x40000u

struct sm_fpkg_image_reader {
  sm_pkg_read_fn read;
  void *read_ctx;
  uint64_t pfs_offset;
  uint64_t outer_block_count;
  const uint32_t *image_map;
  uint64_t image_map_count;
  uint64_t image_size;
  const sm_fpkg_layout_t *layout;
  sm_fpkg_decoder_t *decoder;
  uint8_t *stored;
  uint8_t *decoded;
  size_t cached_block;
};

static bool range_fits(uint64_t offset, uint64_t length, uint64_t total) {
  return offset <= total && length <= total - offset;
}

static sm_fpkg_status_t read_stored_image(sm_fpkg_image_reader_t *reader,
                                          uint64_t offset, uint8_t *output,
                                          size_t size) {
  if (!range_fits(offset, size, reader->image_size))
    return SM_FPKG_CORRUPT;
  size_t done = 0;
  while (done < size) {
    uint64_t at = offset + done;
    uint64_t map_index = at / IMAGE_OUTER_BLOCK;
    uint64_t within = at % IMAGE_OUTER_BLOCK;
    if (map_index >= reader->image_map_count)
      return SM_FPKG_CORRUPT;
    uint32_t outer_block = reader->image_map[map_index];
    if ((uint64_t)outer_block >= reader->outer_block_count)
      return SM_FPKG_CORRUPT;
    size_t amount = IMAGE_OUTER_BLOCK - (size_t)within;
    if (amount > size - done) amount = size - done;
    uint64_t absolute = reader->pfs_offset +
                        (uint64_t)outer_block * IMAGE_OUTER_BLOCK + within;
    if (reader->read(reader->read_ctx, absolute, output + done, amount) != 0)
      return SM_FPKG_IO;
    done += amount;
  }
  return SM_FPKG_OK;
}

static size_t find_block(const sm_fpkg_layout_t *layout, uint64_t offset) {
  size_t low = 0, high = layout->block_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2u;
    const sm_fpkg_layout_block_t *block = &layout->blocks[middle];
    uint64_t end = block->logical_offset + block->logical_length;
    if (end <= offset)
      low = middle + 1u;
    else
      high = middle;
  }
  return low;
}

static sm_fpkg_status_t decode_block(sm_fpkg_image_reader_t *reader,
                                     size_t index) {
  if (index >= reader->layout->block_count)
    return SM_FPKG_CORRUPT;
  if (reader->cached_block == index)
    return SM_FPKG_OK;
  const sm_fpkg_layout_block_t *block = &reader->layout->blocks[index];
  if (block->stored_length == 0 ||
      block->stored_length > IMAGE_MAX_STORED_BLOCK ||
      block->logical_length == 0 ||
      block->logical_length > IMAGE_MAX_LOGICAL_BLOCK)
    return SM_FPKG_CORRUPT;
  sm_fpkg_status_t status = read_stored_image(
      reader, block->stored_offset, reader->stored, block->stored_length);
  if (status != SM_FPKG_OK)
    return status;
  reader->cached_block = SIZE_MAX;
  status = sm_fpkg_decode_block(reader->decoder, reader->stored,
                                block->stored_length, block->even_length,
                                block->mode, reader->decoded,
                                block->logical_length);
  if (status == SM_FPKG_OK)
    reader->cached_block = index;
  return status;
}

sm_fpkg_image_reader_t *sm_fpkg_image_reader_create(
    sm_pkg_read_fn read, void *read_ctx, uint64_t pfs_offset,
    uint64_t outer_block_count, const uint32_t *image_map,
    uint64_t image_map_count, uint64_t image_size,
    const sm_fpkg_layout_t *layout) {
  if (!read || !image_map || image_map_count == 0 || image_size == 0 ||
      !layout || !layout->blocks || layout->block_count == 0 ||
      layout->mount_size == 0 ||
      image_map_count < image_size / IMAGE_OUTER_BLOCK +
                            (image_size % IMAGE_OUTER_BLOCK != 0))
    return NULL;
  sm_fpkg_image_reader_t *reader = calloc(1, sizeof(*reader));
  if (!reader)
    return NULL;
  reader->read = read;
  reader->read_ctx = read_ctx;
  reader->pfs_offset = pfs_offset;
  reader->outer_block_count = outer_block_count;
  reader->image_map = image_map;
  reader->image_map_count = image_map_count;
  reader->image_size = image_size;
  reader->layout = layout;
  reader->cached_block = SIZE_MAX;
  reader->decoder = sm_fpkg_decoder_create();
  reader->stored = malloc(IMAGE_MAX_STORED_BLOCK);
  reader->decoded = malloc(IMAGE_MAX_LOGICAL_BLOCK);
  if (!reader->decoder || !reader->stored || !reader->decoded) {
    sm_fpkg_image_reader_free(reader);
    return NULL;
  }
  return reader;
}

void sm_fpkg_image_reader_free(sm_fpkg_image_reader_t *reader) {
  if (!reader)
    return;
  sm_fpkg_decoder_free(reader->decoder);
  free(reader->stored);
  free(reader->decoded);
  free(reader);
}

sm_fpkg_status_t sm_fpkg_image_read(sm_fpkg_image_reader_t *reader,
                                    uint64_t offset, void *output,
                                    size_t size) {
  if (!reader || (!output && size != 0) ||
      !range_fits(offset, size, reader->layout->mount_size))
    return SM_FPKG_CORRUPT;
  size_t done = 0;
  while (done < size) {
    uint64_t at = offset + done;
    size_t index = find_block(reader->layout, at);
    if (index >= reader->layout->block_count)
      return SM_FPKG_CORRUPT;
    const sm_fpkg_layout_block_t *block = &reader->layout->blocks[index];
    if (at < block->logical_offset ||
        at - block->logical_offset >= block->logical_length)
      return SM_FPKG_CORRUPT;
    sm_fpkg_status_t status = decode_block(reader, index);
    if (status != SM_FPKG_OK)
      return status;
    size_t within = (size_t)(at - block->logical_offset);
    size_t amount = block->logical_length - within;
    if (amount > size - done) amount = size - done;
    memcpy((uint8_t *)output + done, reader->decoded + within, amount);
    done += amount;
  }
  return SM_FPKG_OK;
}
