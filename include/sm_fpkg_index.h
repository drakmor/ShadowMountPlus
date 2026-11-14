#ifndef SM_FPKG_INDEX_H
#define SM_FPKG_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sm_fpkg.h"

typedef struct {
  uint64_t package_offset;
  uint32_t stored_length;
  uint32_t raw_length;
  uint32_t even_length;
  uint8_t mode;
  bool kraken_encoded;
} sm_fpkg_index_chunk_t;

typedef struct {
  /* Relative app0 path, for example "sce_sys/param.json". */
  const char *path;
  uint64_t size;
  /* NULL for loose files; otherwise one record per 256 KiB logical chunk. */
  const sm_fpkg_index_chunk_t *chunks;
  size_t chunk_count;
} sm_fpkg_index_file_t;

typedef struct {
  uint8_t *data;
  size_t size;
} sm_fpkg_index_blob_t;

typedef bool (*sm_fpkg_index_extract_fn)(const char *path, void *ctx);

/* Build deterministic AMPRIDX3 and AMPRPAK4 byte images. */
sm_fpkg_status_t sm_fpkg_index_build(
    const sm_fpkg_index_file_t *files, size_t file_count,
    uint64_t package_size, int64_t package_mtime,
    sm_fpkg_index_blob_t *index_out, sm_fpkg_index_blob_t *manifest_out);
/* Build indexes directly from a validated package. Files selected by
 * should_extract_fn are represented as loose app0 files and must be staged by
 * the caller before publication. Other files must map to contiguous package
 * blocks or the call fails closed with SM_FPKG_UNSUPPORTED. */
sm_fpkg_status_t sm_fpkg_index_build_package(
    const sm_fpkg_t *fpkg, sm_fpkg_index_extract_fn should_extract_fn,
    void *extract_ctx,
    int64_t package_mtime,
    sm_fpkg_index_blob_t *index_out, sm_fpkg_index_blob_t *manifest_out);
/* Overlay loose files by case-folded path. Matching package files are replaced;
 * new paths are added. Every override must have chunks == NULL. */
sm_fpkg_status_t sm_fpkg_index_build_package_with_overrides(
    const sm_fpkg_t *fpkg, sm_fpkg_index_extract_fn should_extract_fn,
    void *extract_ctx, sm_fpkg_index_extract_fn should_skip_fn,
    void *skip_ctx, const sm_fpkg_index_file_t *overrides,
    size_t override_count, int64_t package_mtime,
    sm_fpkg_index_blob_t *index_out, sm_fpkg_index_blob_t *manifest_out);
void sm_fpkg_index_blob_free(sm_fpkg_index_blob_t *blob);

/* Validate the bounded AMPRIDX3 or AMPRPAK4 layout in an already-open file.
 * AMPRIDX3 has no checksum field; its offsets, records and hash slots are
 * checked structurally. AMPRPAK4 header and payload CRCs are verified. */
bool sm_fpkg_index_validate_fd(int fd, bool manifest);
bool sm_fpkg_index_validate_pair_fds(int index_fd, int manifest_fd);

#endif
