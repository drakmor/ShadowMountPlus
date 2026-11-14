#ifndef SM_FPKG_H
#define SM_FPKG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "sm_pkg.h"

typedef enum {
  SM_FPKG_OK = 0,
  SM_FPKG_NOT_PS5,
  SM_FPKG_UNSUPPORTED,
  SM_FPKG_CORRUPT,
  SM_FPKG_IO,
  SM_FPKG_NOMEM,
  SM_FPKG_CANCELLED
} sm_fpkg_status_t;

typedef struct sm_fpkg sm_fpkg_t;

typedef struct {
  const char *path;
  uint64_t size;
  bool is_directory;
} sm_fpkg_file_t;

typedef struct {
  uint64_t package_offset;
  uint32_t stored_size;
  uint32_t raw_size;
  uint32_t even_size;
  uint8_t mode;
  bool kraken_encoded;
} sm_fpkg_block_t;

typedef struct {
  sm_fpkg_status_t (*begin)(void *ctx, const sm_fpkg_file_t *file,
                            void **file_ctx_out);
  sm_fpkg_status_t (*write)(void *file_ctx, uint64_t offset,
                            const void *bytes, size_t size);
  sm_fpkg_status_t (*finish)(void *file_ctx, bool commit);
  void *ctx;
} sm_fpkg_extract_sink_t;

/*
 * Open a validated FPKG reader. The read callback and its context must stay
 * alive until sm_fpkg_close; the handle retains only bounded metadata and one
 * decoded Kraken block, not the package bytes.
 */
sm_fpkg_status_t sm_fpkg_open_stream(const char *name, uint64_t size,
                                     sm_pkg_read_fn read, void *ctx,
                                     const sm_pkg_info_t *metadata,
                                     sm_fpkg_t **fpkg_out);
/* Open from a caller-owned O_RDONLY descriptor; it must stay open until close. */
sm_fpkg_status_t sm_fpkg_open_fd(const char *name, int fd,
                                 const sm_pkg_info_t *metadata,
                                 sm_fpkg_t **fpkg_out);
/* Return the retained fd for handles created by sm_fpkg_open_fd, else -1. */
int sm_fpkg_source_fd(const sm_fpkg_t *fpkg);
void sm_fpkg_close(sm_fpkg_t *fpkg);
uint64_t sm_fpkg_package_size(const sm_fpkg_t *fpkg);
size_t sm_fpkg_file_count(const sm_fpkg_t *fpkg);
bool sm_fpkg_file_at(const sm_fpkg_t *fpkg, size_t index,
                     sm_fpkg_file_t *file_out);
/* Return the AMPR-packable 256 KiB blocks for a file. A file whose data does
 * not start on a Kraken block boundary, or whose stored block spans nonadjacent
 * package extents, returns SM_FPKG_UNSUPPORTED. */
sm_fpkg_status_t sm_fpkg_file_block_count(const sm_fpkg_t *fpkg,
                                           size_t index,
                                           size_t *count_out);
sm_fpkg_status_t sm_fpkg_file_blocks(const sm_fpkg_t *fpkg, size_t index,
                                      sm_fpkg_block_t *blocks,
                                      size_t block_capacity,
                                      size_t *count_out);
/* Stream one validated file through a caller-owned staging sink. The sink
 * receives ordered ranges and must stage safely; finish(commit=false) is
 * called after any read/write failure, and finish(commit=true) publishes only
 * after the complete file has been delivered. */
sm_fpkg_status_t sm_fpkg_extract_file(sm_fpkg_t *fpkg, size_t index,
                                      const sm_fpkg_extract_sink_t *sink);
sm_fpkg_status_t sm_fpkg_read_file(sm_fpkg_t *fpkg, size_t index,
                                   uint64_t offset, void *output,
                                   size_t size);

/*
 * Probe a package already classified by sm_pkg_parse_stream/file and release
 * its validated reader state. The probe is useful for routing decisions; use
 * sm_fpkg_open_stream when callers need to enumerate or read files.
 */
sm_fpkg_status_t sm_fpkg_probe_stream(const char *name, uint64_t size,
                                      sm_pkg_read_fn read, void *ctx,
                                      const sm_pkg_info_t *metadata);

#endif
