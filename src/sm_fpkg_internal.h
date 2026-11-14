#ifndef SM_FPKG_INTERNAL_H
#define SM_FPKG_INTERNAL_H

#include "sm_fpkg.h"

#include <stdbool.h>

typedef struct {
  uint64_t logical_offset;
  uint64_t stored_offset;
  uint32_t logical_length;
  uint32_t stored_length;
  uint32_t even_length;
  uint8_t mode;
  bool raw;
} sm_fpkg_layout_block_t;

typedef struct {
  sm_fpkg_layout_block_t *blocks;
  size_t block_count;
  uint64_t mount_size;
} sm_fpkg_layout_t;

typedef struct sm_fpkg_image_reader sm_fpkg_image_reader_t;
typedef sm_fpkg_status_t (*sm_fpkg_mount_read_fn)(void *ctx, uint64_t offset,
                                                   void *output, size_t size);

typedef struct {
  char *path;
  uint64_t size;
  uint64_t logical_offset;
  uint64_t package_offset;
  bool is_dir;
  bool from_package;
} sm_fpkg_tree_entry_t;

typedef struct {
  sm_fpkg_tree_entry_t *entries;
  size_t count;
} sm_fpkg_tree_t;

/* Parse a bounded naps_pkg_layout.dat image for pfs_image.dat of image_size. */
sm_fpkg_status_t sm_fpkg_layout_parse(const uint8_t *descriptor,
                                      size_t descriptor_size,
                                      uint64_t image_size,
                                      sm_fpkg_layout_t *layout_out);
void sm_fpkg_layout_free(sm_fpkg_layout_t *layout);

/* Read the decoded inner PFS mount through the Kraken block map. */
sm_fpkg_image_reader_t *sm_fpkg_image_reader_create(
    sm_pkg_read_fn read, void *read_ctx, uint64_t pfs_offset,
    uint64_t outer_block_count, const uint32_t *image_map,
    uint64_t image_map_count, uint64_t image_size,
    const sm_fpkg_layout_t *layout);
void sm_fpkg_image_reader_free(sm_fpkg_image_reader_t *reader);
sm_fpkg_status_t sm_fpkg_image_read(sm_fpkg_image_reader_t *reader,
                                    uint64_t offset, void *output,
                                    size_t size);

/* Validate inner PFS metadata and bounded directory traversal. */
sm_fpkg_status_t sm_fpkg_tree_validate(sm_fpkg_mount_read_fn read,
                                       void *read_ctx, uint64_t mount_size,
                                       uint64_t meta_base);
sm_fpkg_status_t sm_fpkg_tree_open(sm_fpkg_mount_read_fn read, void *read_ctx,
                                   uint64_t mount_size, uint64_t meta_base,
                                   sm_fpkg_tree_t *tree_out);
void sm_fpkg_tree_free(sm_fpkg_tree_t *tree);

#endif
