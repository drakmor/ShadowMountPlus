/* Native parser for Kraken's naps_pkg_layout.dat descriptor.
 * Format behavior follows Kraken Mount and ps5-dump-forge (GPL-3.0-or-later). */
#include "sm_fpkg_internal.h"

#include <stdlib.h>
#include <string.h>

#define LAYOUT_MAX_DESCRIPTOR (384u * 1024u * 1024u)
#define LAYOUT_MAX_BLOCKS (1u << 22)
#define LAYOUT_MAX_BOUNDARIES (1u << 22)
#define LAYOUT_UBLOCK UINT64_C(0x40000)
#define LAYOUT_WINDOW UINT64_C(0x40000)
#define LAYOUT_HALF UINT64_C(0x20000)
#define LAYOUT_RECORD_SIZE 9u
#define LAYOUT_BOUNDARY_SIZE 6u
#define LAYOUT_MAX_IMAGE_SIZE (UINT64_C(1) << 40)

static uint64_t read_le64(const uint8_t *p) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= (uint64_t)p[i] << (i * 8);
  return value;
}

static bool add_u64(uint64_t a, uint64_t b, uint64_t *out) {
  if (b > UINT64_MAX - a)
    return false;
  *out = a + b;
  return true;
}

static bool mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
  if (a != 0 && b > UINT64_MAX / a)
    return false;
  *out = a * b;
  return true;
}

static uint32_t field(unsigned __int128 value, unsigned low,
                      unsigned width) {
  unsigned __int128 mask = (((unsigned __int128)1) << width) - 1u;
  return (uint32_t)((value >> low) & mask);
}

static unsigned __int128 read_record(const uint8_t *p) {
  unsigned __int128 value = 0;
  for (unsigned i = 0; i < LAYOUT_RECORD_SIZE; ++i)
    value |= (unsigned __int128)p[i] << (i * 8);
  return value;
}

static int compare_u64(const void *left, const void *right) {
  uint64_t a = *(const uint64_t *)left;
  uint64_t b = *(const uint64_t *)right;
  return (a > b) - (a < b);
}

static sm_fpkg_status_t append_block(sm_fpkg_layout_t *layout,
                                     size_t *capacity, uint64_t logical,
                                     uint64_t stored_at, uint64_t len,
                                     uint64_t stored_len, uint32_t even_len,
                                     uint8_t mode) {
  if (layout->block_count >= LAYOUT_MAX_BLOCKS || len == 0 ||
      len > LAYOUT_UBLOCK || stored_len == 0 || stored_len > 2 * LAYOUT_UBLOCK ||
      stored_at > LAYOUT_MAX_IMAGE_SIZE || stored_len > LAYOUT_MAX_IMAGE_SIZE - stored_at ||
      len > UINT32_MAX || stored_len > UINT32_MAX)
    return SM_FPKG_CORRUPT;
  if (layout->block_count == *capacity) {
    size_t next = *capacity ? *capacity * 2 : 256;
    if (next > LAYOUT_MAX_BLOCKS)
      next = LAYOUT_MAX_BLOCKS;
    if (next <= *capacity || next > SIZE_MAX / sizeof(*layout->blocks))
      return SM_FPKG_UNSUPPORTED;
    void *grown = realloc(layout->blocks, next * sizeof(*layout->blocks));
    if (!grown)
      return SM_FPKG_NOMEM;
    layout->blocks = grown;
    *capacity = next;
  }
  sm_fpkg_layout_block_t *b = &layout->blocks[layout->block_count++];
  b->logical_offset = logical;
  b->stored_offset = stored_at;
  b->logical_length = (uint32_t)len;
  b->stored_length = (uint32_t)stored_len;
  b->even_length = even_len;
  b->mode = mode;
  b->raw = stored_len == len && (mode & 0x1bu) == 0 &&
           even_len == (len < LAYOUT_HALF ? len : LAYOUT_HALF);
  return SM_FPKG_OK;
}

static uint64_t next_boundary(const uint64_t *bounds, size_t count,
                              uint64_t logical, uint64_t mount_size) {
  size_t low = 0, high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (bounds[middle] <= logical)
      low = middle + 1;
    else
      high = middle;
  }
  return low < count ? bounds[low] : mount_size;
}

void sm_fpkg_layout_free(sm_fpkg_layout_t *layout) {
  if (!layout)
    return;
  free(layout->blocks);
  memset(layout, 0, sizeof(*layout));
}

sm_fpkg_status_t sm_fpkg_layout_parse(const uint8_t *descriptor,
                                      size_t descriptor_size,
                                      uint64_t image_size,
                                      sm_fpkg_layout_t *layout_out) {
  if (!layout_out)
    return SM_FPKG_CORRUPT;
  memset(layout_out, 0, sizeof(*layout_out));
  if (!descriptor || descriptor_size < 16 ||
      descriptor_size > LAYOUT_MAX_DESCRIPTOR || image_size == 0 ||
      image_size > LAYOUT_MAX_IMAGE_SIZE)
    return SM_FPKG_CORRUPT;

  uint64_t w0 = read_le64(descriptor);
  uint64_t w1 = read_le64(descriptor + 8);
  uint32_t compression = (uint32_t)((w0 >> 24) & 3u);
  uint64_t files = (w0 & 0xffffffu) + 1u;
  uint64_t ublocks = (w0 >> 32) & 0xffffffu;
  uint64_t outer = w1 & 0xffffffu;
  uint64_t shuffles = (w0 >> 28) & 0xfu;
  uint64_t records = ((w1 >> 24) & 0xffffffu) + 2u;
  if (compression != 2)
    return SM_FPKG_UNSUPPORTED;
  if (shuffles != 0)
    return SM_FPKG_UNSUPPORTED;
  if (files == 0 || files > LAYOUT_MAX_BOUNDARIES ||
      records < 2 || records > (1u << 24) + 1u)
    return files > LAYOUT_MAX_BOUNDARIES ? SM_FPKG_UNSUPPORTED
                                         : SM_FPKG_CORRUPT;

  uint64_t fidx_at = 16, part;
  if (!mul_u64(outer, 8, &part) || !add_u64(fidx_at, part, &fidx_at))
    return SM_FPKG_CORRUPT;
  uint64_t u2c_end = fidx_at;
  if (!mul_u64(files, LAYOUT_BOUNDARY_SIZE, &part) ||
      !add_u64(u2c_end, part, &u2c_end))
    return SM_FPKG_CORRUPT;
  uint64_t bitmap_entries = ublocks / 8u + 1u;
  if (!mul_u64(bitmap_entries, 10u, &part) ||
      !add_u64(u2c_end, part, &u2c_end))
    return SM_FPKG_CORRUPT;
  uint64_t rec_at;
  if (!add_u64(u2c_end, 7u, &rec_at))
    return SM_FPKG_CORRUPT;
  rec_at = (rec_at & ~UINT64_C(7)) + 3u;
  uint64_t records_size, rec_end;
  if (!mul_u64(records, LAYOUT_RECORD_SIZE, &records_size) ||
      !add_u64(rec_at, records_size, &rec_end) || rec_end > descriptor_size ||
      rec_at < 3 || fidx_at > descriptor_size)
    return SM_FPKG_CORRUPT;

  uint64_t *bounds = malloc((size_t)files * sizeof(*bounds));
  if (!bounds)
    return SM_FPKG_NOMEM;
  sm_fpkg_status_t result = SM_FPKG_CORRUPT;
  uint64_t sparse_at = UINT64_MAX;
  for (uint64_t i = 0; i < files; ++i) {
    uint64_t at = fidx_at + i * LAYOUT_BOUNDARY_SIZE;
    uint64_t boundary = 0;
    for (unsigned j = 0; j < 5; ++j)
      boundary |= (uint64_t)descriptor[at + j] << (j * 8);
    uint8_t kind = descriptor[at + 5];
    bounds[i] = boundary;
    if (kind == 0x40 && boundary < sparse_at)
      sparse_at = boundary;
  }
  uint64_t mount_size = bounds[files - 1u];
  if (sparse_at < mount_size) {
    result = SM_FPKG_UNSUPPORTED;
    goto done;
  }
  if (mount_size == 0 || (mount_size + LAYOUT_UBLOCK - 1u) / LAYOUT_UBLOCK != ublocks)
    goto done;
  for (uint64_t i = 0; i < files; ++i) {
    if (bounds[i] > mount_size)
      goto done;
  }
  qsort(bounds, (size_t)files, sizeof(*bounds), compare_u64);
  size_t unique = 0;
  for (uint64_t i = 0; i < files; ++i) {
    if (unique == 0 || bounds[unique - 1] != bounds[i])
      bounds[unique++] = bounds[i];
  }
  bool strict = descriptor[rec_at - 3u] == 0 &&
                descriptor[rec_at - 2u] == 0 &&
                descriptor[rec_at - 1u] == 4;

  sm_fpkg_layout_t parsed = {0};
  size_t capacity = 0;
  uint64_t cursor = 0, logical = 0, carried_nibble = 0;
  bool next_is_anchor = true;
  for (uint64_t i = 0; i + 1u < records; ++i) {
    unsigned __int128 record =
        read_record(descriptor + rec_at + i * LAYOUT_RECORD_SIZE);
    uint64_t nibble = carried_nibble;
    carried_nibble = field(record, 68, 4);
    bool anchor = next_is_anchor;
    next_is_anchor = field(record, 66, 1) == 1 && !anchor;
    if (anchor) {
      cursor = ((uint64_t)field(record, 26, 22) << 18) |
               field(record, 48, 18);
      bool terminator = i + 2u == records;
      if (strict && i > 0 && !terminator && nibble != ((cursor >> 16) & 0xfu)) {
        sm_fpkg_layout_free(&parsed);
        goto done;
      }
      continue;
    }
    if ((strict && i > 0 && nibble != (logical & 0xfu)) || logical >= mount_size ||
        ((logical % LAYOUT_WINDOW) >> 4) != field(record, 0, 14)) {
      sm_fpkg_layout_free(&parsed);
      goto done;
    }
    uint64_t bound = next_boundary(bounds, unique, logical, mount_size);
    if (bound < logical) {
      sm_fpkg_layout_free(&parsed);
      goto done;
    }
    uint64_t room = bound - logical;
    uint64_t cap = logical + (room < LAYOUT_UBLOCK ? room : LAYOUT_UBLOCK);
    uint64_t next = field(record, 66, 1) == 1 ? i + 2u : i + 1u;
    uint64_t len;
    if (next + 1u < records) {
      unsigned __int128 next_record =
          read_record(descriptor + rec_at + next * LAYOUT_RECORD_SIZE);
      uint64_t at16 = (logical & ~(LAYOUT_WINDOW - 1u)) |
                      ((uint64_t)field(next_record, 0, 14) << 4);
      if (at16 <= (logical & ~UINT64_C(0xf))) {
        if (!add_u64(at16, LAYOUT_WINDOW, &at16)) {
          sm_fpkg_layout_free(&parsed);
          goto done;
        }
      }
      unsigned __int128 before_next =
          read_record(descriptor + rec_at + (next - 1u) * LAYOUT_RECORD_SIZE);
      uint64_t low = strict ? field(before_next, 68, 4) : (logical & 0xfu);
      uint64_t exact = at16 | low;
      uint64_t at16_plus;
      if (!add_u64(at16, 16u, &at16_plus)) {
        sm_fpkg_layout_free(&parsed);
        goto done;
      }
      len = (at16_plus <= cap || (strict && exact < cap)) ? exact - logical
                                                          : cap - logical;
    } else {
      len = cap - logical;
    }
    if (len == 0 || len > LAYOUT_UBLOCK || len > mount_size - logical) {
      sm_fpkg_layout_free(&parsed);
      goto done;
    }
    uint64_t end = (cursor & ~(LAYOUT_WINDOW - 1u)) | field(record, 48, 18);
    if (end < cursor && !add_u64(end, LAYOUT_WINDOW, &end)) {
      sm_fpkg_layout_free(&parsed);
      goto done;
    }
    uint64_t even_size = (uint64_t)field(record, 14, 17) + 1u;
    bool even_raw = field(record, 33, 1) == 0 &&
                    even_size == (len < LAYOUT_HALF ? len : LAYOUT_HALF);
    bool odd_raw = field(record, 36, 1) == 0;
    if (end == cursor && even_raw && odd_raw && len > LAYOUT_HALF &&
        !add_u64(end, LAYOUT_WINDOW, &end)) {
      sm_fpkg_layout_free(&parsed);
      goto done;
    }
    if (end <= cursor || field(record, 44, 4) != 0) {
      sm_fpkg_layout_free(&parsed);
      result = field(record, 44, 4) != 0 ? SM_FPKG_UNSUPPORTED
                                          : SM_FPKG_CORRUPT;
      goto done;
    }
    uint64_t stored_len = end - cursor;
    uint64_t stored_end;
    /* Kraken descriptors may reuse the tail of a prior stored extent. Each
     * block is decoded independently, so only the individual range bounds
     * below matter; requiring globally monotonic stored offsets rejects legal
     * layouts accepted by the pinned reader. */
    if (even_size > stored_len) {
      sm_fpkg_layout_free(&parsed);
      goto done;
    }
    sm_fpkg_status_t append_status = append_block(
        &parsed, &capacity, logical, cursor, len, stored_len,
        (uint32_t)even_size, (uint8_t)field(record, 32, 5));
    if (stored_len > 2u * LAYOUT_UBLOCK ||
        !add_u64(cursor, stored_len, &stored_end) || stored_end > image_size ||
        append_status != SM_FPKG_OK) {
      sm_fpkg_layout_free(&parsed);
      if (append_status == SM_FPKG_NOMEM ||
          append_status == SM_FPKG_UNSUPPORTED)
        result = append_status;
      goto done;
    }
    cursor = end;
    logical += len;
  }
  if (logical != mount_size || parsed.block_count == 0) {
    sm_fpkg_layout_free(&parsed);
    goto done;
  }
  parsed.mount_size = mount_size;
  *layout_out = parsed;
  result = SM_FPKG_OK;
done:
  free(bounds);
  return result;
}
