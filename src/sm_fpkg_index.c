/* Native AMPRIDX3 / AMPRPAK4 writer, based on the GPL-3.0-or-later
 * Kraken Mount and pinned ampr-emu-ng v0.4.0 format implementations. */
#include "sm_fpkg_index.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define IDX_MAGIC "AMPRIDX3"
#define IDX_HEADER 0x30u
#define IDX_RECORD 24u
#define IDX_SLOT 16u
#define IDX_SLOT_SHARED 1u
#define PAK_MAGIC "AMPRPAK4"
#define PAK_HEADER 128u
#define PAK_FILE_RECORD 48u
#define PAK_CHUNK_RECORD 12u
#define PAK_PACK_RECORD 32u
#define PAK_PAYLOAD_CRC_AT 112u
#define PAK_HEADER_CRC_AT 116u
#define PAK_ENDIAN 0x01020304u
#define PAK_PAGE 65536u
#define PAK_BLOCK (1u << 18)
#define PAK_OFFSET_LIMIT (UINT64_C(1) << 48)
#define PAK_KRAKEN_OFFSET_LIMIT (UINT64_C(1) << 40)
#define PAK_CODEC_RAW 0u
#define PAK_CODEC_KRAKEN 2u
#define PAK_FLAG_FULL 0x10u
#define PAK_FLAG_PAGE_CONTAINED 0x04u
#define PAK_FLAG_PAGE_ALIGNED 0x08u
#define PAK_FILE_PACKED 1u
#define PAK_PACK_IO_PAGE_LAYOUT 2u
#define MAX_INDEX_FILES 1000000u
#define MAX_INDEX_CHUNKS 16000000u
#define MAX_INDEX_PATH_BYTES (256u * 1024u * 1024u)
#define MAX_PATH_BYTES 1024u
#define MAX_COMPONENT_BYTES 255u

typedef struct {
  const sm_fpkg_index_file_t *file;
  char *path;
  size_t path_length;
  uint64_t folded_hash;
} index_row_t;

static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= (uint64_t)p[i] << (i * 8u);
  return value;
}

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i)
    p[i] = (uint8_t)(v >> (i * 8));
}

static bool add_size(size_t a, size_t b, size_t *out) {
  if (b > SIZE_MAX - a)
    return false;
  *out = a + b;
  return true;
}

static bool mul_size(size_t a, size_t b, size_t *out) {
  if (a != 0 && b > SIZE_MAX / a)
    return false;
  *out = a * b;
  return true;
}

static bool round_up_size(size_t value, size_t alignment, size_t *out) {
  size_t rem = value % alignment;
  return add_size(value, rem ? alignment - rem : 0, out);
}

static uint8_t fold_byte(uint8_t b) {
  if (b >= 'A' && b <= 'Z')
    return (uint8_t)(b + ('a' - 'A'));
  return b;
}

static bool valid_utf8(const uint8_t *s, size_t len) {
  size_t i = 0;
  while (i < len) {
    uint8_t c = s[i++];
    if (c < 0x80)
      continue;
    unsigned tail;
    uint32_t value;
    if (c >= 0xc2 && c <= 0xdf) {
      tail = 1;
      value = c & 0x1fu;
    } else if (c >= 0xe0 && c <= 0xef) {
      tail = 2;
      value = c & 0x0fu;
    } else if (c >= 0xf0 && c <= 0xf4) {
      tail = 3;
      value = c & 0x07u;
    } else {
      return false;
    }
    if (tail > len - i)
      return false;
    uint8_t first = s[i];
    if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
        (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90))
      return false;
    for (unsigned j = 0; j < tail; ++j) {
      uint8_t b = s[i++];
      if ((b & 0xc0u) != 0x80u)
        return false;
      value = (value << 6) | (b & 0x3fu);
    }
    if (value > 0x10ffffu || (value >= 0xd800u && value <= 0xdfffu))
      return false;
  }
  return true;
}

static uint64_t fnv_path(const char *path, size_t len, uint64_t basis) {
  uint64_t hash = basis;
  for (size_t i = 0; i < len; ++i) {
    hash ^= fold_byte((uint8_t)path[i]);
    hash *= UINT64_C(1099511628211);
  }
  return hash ? hash : 1;
}

static uint64_t idx_hash(const char *path, size_t len) {
  return fnv_path(path, len, UINT64_C(1469598103934665603));
}

static uint64_t pak_hash(const char *path, size_t len) {
  return fnv_path(path, len, UINT64_C(14695981039346656037));
}

static int row_compare(const void *left, const void *right) {
  const index_row_t *a = left;
  const index_row_t *b = right;
  size_t n = a->path_length < b->path_length ? a->path_length : b->path_length;
  for (size_t i = 0; i < n; ++i) {
    uint8_t ac = fold_byte((uint8_t)a->path[i]);
    uint8_t bc = fold_byte((uint8_t)b->path[i]);
    if (ac != bc)
      return ac < bc ? -1 : 1;
  }
  return (a->path_length > b->path_length) -
         (a->path_length < b->path_length);
}

static bool valid_relative_path(const char *path, size_t len) {
  if (!path || len == 0 || len >= MAX_PATH_BYTES - 6u || path[0] == '/' ||
      !valid_utf8((const uint8_t *)path, len))
    return false;
  size_t component = 0;
  size_t component_start = 0;
  for (size_t i = 0; i <= len; ++i) {
    if (i < len && (path[i] == '\0' || path[i] == '\\'))
      return false;
    if (i == len || path[i] == '/') {
      component = i - component_start;
      if (component == 0 || component > MAX_COMPONENT_BYTES ||
          (component == 1 && path[component_start] == '.') ||
          (component == 2 && path[component_start] == '.' &&
           path[component_start + 1] == '.'))
        return false;
      component_start = i + 1;
    }
  }
  return true;
}

static uint32_t crc32_bytes(const uint8_t *bytes, size_t len) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < len; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (UINT32_C(0xedb88320) &
                           (uint32_t)-(int32_t)(crc & 1u));
  }
  return ~crc;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *bytes, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (UINT32_C(0xedb88320) &
                           (uint32_t)-(int32_t)(crc & 1u));
  }
  return crc;
}

static bool read_exact_at(int fd, uint64_t offset, void *buffer, size_t size) {
  if (offset > (uint64_t)INT64_MAX || size > (uint64_t)INT64_MAX - offset)
    return false;
  uint8_t *bytes = buffer;
  size_t done = 0;
  while (done < size) {
    ssize_t count = pread(fd, bytes + done, size - done,
                          (off_t)(offset + done));
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    done += (size_t)count;
  }
  return true;
}

static bool validate_idx_fd(int fd, uint64_t file_size) {
  uint8_t header[IDX_HEADER];
  if (file_size < sizeof(header) || !read_exact_at(fd, 0, header,
                                                   sizeof(header)) ||
      memcmp(header, IDX_MAGIC, 8) != 0 || read_le32(header + 8) != 3u ||
      read_le32(header + 12) != IDX_RECORD)
    return false;
  uint64_t count = read_le64(header + 16);
  uint64_t path_bytes = read_le64(header + 24);
  uint64_t slots_offset = read_le64(header + 32);
  uint32_t slot_size = read_le32(header + 40);
  uint32_t slot_count = read_le32(header + 44);
  uint64_t records_size;
  if (count == 0 || count > MAX_INDEX_FILES ||
      path_bytes > MAX_INDEX_PATH_BYTES ||
      slot_size != IDX_SLOT || slot_count < 2u ||
      (slot_count & (slot_count - 1u)) != 0 ||
      (uint64_t)slot_count < count * 2u ||
      count > UINT64_MAX / IDX_RECORD)
    return false;
  records_size = count * IDX_RECORD;
  if (records_size > UINT64_MAX - IDX_HEADER ||
      path_bytes > UINT64_MAX - IDX_HEADER - records_size)
    return false;
  uint64_t payload_end = IDX_HEADER + records_size + path_bytes;
  uint64_t slots_size = (uint64_t)slot_count * IDX_SLOT;
  if (slots_offset < payload_end || (slots_offset & (IDX_SLOT - 1u)) != 0 ||
      slots_size > UINT64_MAX - slots_offset ||
      slots_offset + slots_size != file_size)
    return false;
  uint8_t padding[IDX_SLOT - 1u];
  size_t padding_size = (size_t)(slots_offset - payload_end);
  if (padding_size > sizeof(padding) ||
      (padding_size &&
       !read_exact_at(fd, payload_end, padding, padding_size)))
    return false;
  for (size_t i = 0; i < padding_size; ++i)
    if (padding[i] != 0)
      return false;

  uint8_t record[IDX_RECORD];
  for (uint64_t i = 0; i < count; ++i) {
    if (!read_exact_at(fd, IDX_HEADER + i * IDX_RECORD, record,
                       sizeof(record)))
      return false;
    uint32_t path_at = read_le32(record);
    uint32_t path_length = read_le32(record + 4);
    if (path_length == 0 || path_length > MAX_PATH_BYTES ||
        path_at > path_bytes || (uint64_t)path_length + 1u >
                                    path_bytes - path_at)
      return false;
    char path[MAX_PATH_BYTES + 1u];
    if (!read_exact_at(fd, IDX_HEADER + records_size + path_at, path,
                       (size_t)path_length + 1u) ||
        path[path_length] != '\0' ||
        memchr(path, '\0', path_length) != NULL || path_length < 7u ||
        memcmp(path, "/app0/", 6u) != 0)
      return false;
  }

  uint8_t *seen_records = calloc((size_t)count + 1u, 1u);
  if (!seen_records)
    return false;
  uint32_t used = 0;
  uint8_t slot[IDX_SLOT];
  for (uint32_t i = 0; i < slot_count; ++i) {
    if (!read_exact_at(fd, slots_offset + (uint64_t)i * IDX_SLOT, slot,
                       sizeof(slot))) {
      free(seen_records);
      return false;
    }
    uint64_t hash = read_le64(slot);
    uint32_t record_plus_one = read_le32(slot + 8);
    uint32_t flags = read_le32(slot + 12);
    if (record_plus_one == 0) {
      if (hash != 0 || flags != 0) {
        free(seen_records);
        return false;
      }
      continue;
    }
    if (record_plus_one > count || hash == 0 ||
        (flags & ~IDX_SLOT_SHARED) != 0 || seen_records[record_plus_one]) {
      free(seen_records);
      return false;
    }
    uint32_t probe = (uint32_t)hash & (slot_count - 1u);
    while (probe != i) {
      uint8_t previous_slot[IDX_SLOT];
      if (!read_exact_at(fd, slots_offset + (uint64_t)probe * IDX_SLOT,
                         previous_slot, sizeof(previous_slot)) ||
          read_le32(previous_slot + 8) == 0) {
        free(seen_records);
        return false;
      }
      probe = (probe + 1u) & (slot_count - 1u);
    }
    uint8_t record[IDX_RECORD];
    uint64_t record_offset = IDX_HEADER +
                             (uint64_t)(record_plus_one - 1u) * IDX_RECORD;
    if (!read_exact_at(fd, record_offset, record, sizeof(record))) {
      free(seen_records);
      return false;
    }
    uint32_t path_at = read_le32(record);
    uint32_t path_length = read_le32(record + 4);
    char path[MAX_PATH_BYTES + 1u];
    if (path_length == 0 || path_length > MAX_PATH_BYTES ||
        path_at > path_bytes || (uint64_t)path_length + 1u >
                                    path_bytes - path_at ||
        !read_exact_at(fd, IDX_HEADER + records_size + path_at, path,
                       (size_t)path_length + 1u) ||
        idx_hash(path, path_length) != hash) {
      free(seen_records);
      return false;
    }
    seen_records[record_plus_one] = 1u;
    ++used;
  }
  bool all_seen = used == count;
  for (uint64_t i = 1; all_seen && i <= count; ++i)
    all_seen = seen_records[i] != 0;
  free(seen_records);
  return all_seen;
}

static bool validate_pak_fd(int fd, uint64_t file_size) {
  uint8_t header[PAK_HEADER];
  if (file_size < sizeof(header) || !read_exact_at(fd, 0, header,
                                                   sizeof(header)) ||
      memcmp(header, PAK_MAGIC, 8) != 0 || read_le32(header + 8) != 4u ||
      read_le32(header + 12) != PAK_HEADER ||
      read_le32(header + 20) != PAK_ENDIAN ||
      read_le32(header + 56) != 1u ||
      read_le32(header + 60) != PAK_FILE_RECORD ||
      read_le32(header + 64) != PAK_CHUNK_RECORD ||
      read_le32(header + 68) != PAK_PACK_RECORD)
    return false;
  uint64_t file_count = read_le64(header + 40);
  uint64_t chunk_count = read_le64(header + 48);
  uint64_t files_at = read_le64(header + 72);
  uint64_t chunks_at = read_le64(header + 80);
  uint64_t pack_at = read_le64(header + 88);
  uint64_t strings_at = read_le64(header + 96);
  uint64_t strings_size = read_le64(header + 104);
  if (file_count == 0 || file_count > MAX_INDEX_FILES ||
      chunk_count > MAX_INDEX_CHUNKS ||
      files_at != PAK_HEADER ||
      file_count > (UINT64_MAX - files_at) / PAK_FILE_RECORD ||
      chunks_at != files_at + file_count * PAK_FILE_RECORD ||
      chunk_count > (UINT64_MAX - chunks_at) / PAK_CHUNK_RECORD ||
      pack_at != chunks_at + chunk_count * PAK_CHUNK_RECORD ||
      pack_at > UINT64_MAX - PAK_PACK_RECORD ||
      strings_at != pack_at + PAK_PACK_RECORD ||
      strings_size > UINT64_MAX - strings_at ||
      strings_at + strings_size != file_size)
    return false;

  uint8_t pack[PAK_PACK_RECORD];
  if (!read_exact_at(fd, pack_at, pack, sizeof(pack)) ||
      read_le64(pack) == 0 || read_le64(pack + 8) < read_le64(pack) ||
      read_le32(pack + 24) != PAK_PACK_IO_PAGE_LAYOUT ||
      read_le32(pack + 28) != PAK_PAGE)
    return false;
  uint32_t pack_name_at = read_le32(pack + 16);
  uint32_t pack_name_size = read_le32(pack + 20);
  if ((uint64_t)pack_name_at + pack_name_size + 1u > strings_size ||
      pack_name_size != sizeof("kraken.pkg") - 1u)
    return false;
  char pack_name[sizeof("kraken.pkg")];
  if (!read_exact_at(fd, strings_at + pack_name_at, pack_name,
                     sizeof(pack_name)) ||
      memcmp(pack_name, "kraken.pkg", sizeof(pack_name)) != 0)
    return false;

  uint8_t file_record[PAK_FILE_RECORD];
  for (uint64_t i = 0; i < file_count; ++i) {
    if (!read_exact_at(fd, files_at + i * PAK_FILE_RECORD, file_record,
                       sizeof(file_record)))
      return false;
    uint32_t first_chunk = read_le32(file_record + 24);
    uint32_t file_chunks = read_le32(file_record + 28);
    uint32_t path_at = read_le32(file_record + 32);
    uint32_t path_length = read_le32(file_record + 36);
    if (first_chunk > chunk_count || file_chunks > chunk_count - first_chunk ||
        path_length == 0 || path_length > MAX_PATH_BYTES ||
        (uint64_t)path_at + path_length + 1u > strings_size)
      return false;
    char path[MAX_PATH_BYTES + 1u];
    if (!read_exact_at(fd, strings_at + path_at, path,
                       (size_t)path_length + 1u) ||
        path[path_length] != '\0' || memchr(path, '\0', path_length) != NULL ||
        pak_hash(path, path_length) != read_le64(file_record))
      return false;
  }

  uint32_t expected_header_crc = read_le32(header + PAK_HEADER_CRC_AT);
  memset(header + PAK_HEADER_CRC_AT, 0, 4u);
  if (crc32_bytes(header, sizeof(header)) != expected_header_crc)
    return false;
  uint8_t chunk[64u * 1024u];
  uint64_t offset = PAK_HEADER;
  uint32_t crc = UINT32_MAX;
  while (offset < file_size) {
    size_t amount = file_size - offset > sizeof(chunk)
                        ? sizeof(chunk)
                        : (size_t)(file_size - offset);
    if (!read_exact_at(fd, offset, chunk, amount))
      return false;
    crc = crc32_update(crc, chunk, amount);
    offset += amount;
  }
  return ~crc == read_le32(header + PAK_PAYLOAD_CRC_AT);
}

bool sm_fpkg_index_validate_fd(int fd, bool manifest) {
  if (fd < 0)
    return false;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
    return false;
  uint64_t file_size = (uint64_t)st.st_size;
  return manifest ? validate_pak_fd(fd, file_size)
                   : validate_idx_fd(fd, file_size);
}

bool sm_fpkg_index_validate_pair_fds(int index_fd, int manifest_fd) {
  if (!sm_fpkg_index_validate_fd(index_fd, false) ||
      !sm_fpkg_index_validate_fd(manifest_fd, true))
    return false;
  uint8_t index_header[IDX_HEADER], manifest_header[PAK_HEADER];
  if (!read_exact_at(index_fd, 0, index_header, sizeof(index_header)) ||
      !read_exact_at(manifest_fd, 0, manifest_header,
                     sizeof(manifest_header)))
    return false;
  uint64_t count = read_le64(index_header + 16);
  if (count != read_le64(manifest_header + 40))
    return false;
  uint64_t index_paths_at = IDX_HEADER + count * IDX_RECORD;
  uint64_t manifest_files_at = read_le64(manifest_header + 72);
  uint64_t manifest_strings_at = read_le64(manifest_header + 96);
  uint8_t index_record[IDX_RECORD], manifest_record[PAK_FILE_RECORD];
  for (uint64_t i = 0; i < count; ++i) {
    if (!read_exact_at(index_fd, IDX_HEADER + i * IDX_RECORD, index_record,
                       sizeof(index_record)) ||
        !read_exact_at(manifest_fd,
                       manifest_files_at + i * PAK_FILE_RECORD,
                       manifest_record, sizeof(manifest_record)))
      return false;
    uint32_t index_path_at = read_le32(index_record);
    uint32_t index_path_length = read_le32(index_record + 4);
    uint32_t manifest_path_at = read_le32(manifest_record + 32);
    uint32_t manifest_path_length = read_le32(manifest_record + 36);
    if (index_path_length != manifest_path_length ||
        read_le64(index_record + 8) != read_le64(manifest_record + 8) ||
        read_le64(index_record + 16) != read_le64(manifest_record + 16))
      return false;
    char index_path[MAX_PATH_BYTES + 1u];
    char manifest_path[MAX_PATH_BYTES + 1u];
    if (!read_exact_at(index_fd, index_paths_at + index_path_at, index_path,
                       (size_t)index_path_length + 1u) ||
        !read_exact_at(manifest_fd,
                       manifest_strings_at + manifest_path_at,
                       manifest_path, (size_t)manifest_path_length + 1u) ||
        memcmp(index_path, manifest_path,
               (size_t)index_path_length + 1u) != 0)
      return false;
  }
  return true;
}

static uint32_t crc32_parts(const uint8_t *a, size_t a_len,
                            const uint8_t *b, size_t b_len,
                            const uint8_t *c, size_t c_len) {
  uint32_t crc = UINT32_MAX;
  const uint8_t *parts[] = {a, b, c};
  size_t lengths[] = {a_len, b_len, c_len};
  for (size_t p = 0; p < 3; ++p) {
    for (size_t i = 0; i < lengths[p]; ++i) {
      crc ^= parts[p][i];
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ (UINT32_C(0xedb88320) &
                             (uint32_t)-(int32_t)(crc & 1u));
    }
  }
  return ~crc;
}

static sm_fpkg_status_t prepare_rows(const sm_fpkg_index_file_t *files,
                                     size_t count, index_row_t **rows_out,
                                     size_t *path_bytes_out) {
  if (!files || count == 0 || count > MAX_INDEX_FILES)
    return SM_FPKG_UNSUPPORTED;
  index_row_t *rows = calloc(count, sizeof(*rows));
  if (!rows)
    return SM_FPKG_NOMEM;
  size_t path_bytes = 0;
  sm_fpkg_status_t status = SM_FPKG_OK;
  for (size_t i = 0; i < count; ++i) {
    if (!files[i].path) {
      status = SM_FPKG_CORRUPT;
      break;
    }
    size_t rel_len = strlen(files[i].path);
    if (!valid_relative_path(files[i].path, rel_len)) {
      status = SM_FPKG_CORRUPT;
      break;
    }
    size_t full_len;
    if (!add_size(6u, rel_len, &full_len) ||
        !add_size(path_bytes, full_len + 1u, &path_bytes) ||
        path_bytes > MAX_INDEX_PATH_BYTES || full_len > UINT32_MAX) {
      status = SM_FPKG_UNSUPPORTED;
      break;
    }
    rows[i].path = malloc(full_len + 1u);
    if (!rows[i].path) {
      status = SM_FPKG_NOMEM;
      break;
    }
    memcpy(rows[i].path, "/app0/", 6u);
    memcpy(rows[i].path + 6u, files[i].path, rel_len);
    rows[i].path[full_len] = '\0';
    rows[i].path_length = full_len;
    rows[i].file = &files[i];
    rows[i].folded_hash = idx_hash(rows[i].path, full_len);
  }
  if (status == SM_FPKG_OK) {
    qsort(rows, count, sizeof(*rows), row_compare);
    for (size_t i = 1; i < count; ++i) {
      if (row_compare(&rows[i - 1], &rows[i]) == 0) {
        status = SM_FPKG_CORRUPT;
        break;
      }
    }
  }
  if (status != SM_FPKG_OK) {
    for (size_t i = 0; i < count; ++i)
      free(rows[i].path);
    free(rows);
    return status;
  }
  *rows_out = rows;
  *path_bytes_out = path_bytes;
  return SM_FPKG_OK;
}

static void free_rows(index_row_t *rows, size_t count) {
  if (!rows)
    return;
  for (size_t i = 0; i < count; ++i)
    free(rows[i].path);
  free(rows);
}

static sm_fpkg_status_t build_index(const index_row_t *rows, size_t count,
                                    size_t path_bytes, int64_t mtime,
                                    sm_fpkg_index_blob_t *out) {
  size_t records_size, path_start, slots_offset, slot_count = 2;
  if (!mul_size(count, IDX_RECORD, &records_size) ||
      !add_size(IDX_HEADER, records_size, &path_start) ||
      !add_size(path_start, path_bytes, &slots_offset) ||
      !round_up_size(slots_offset, IDX_SLOT, &slots_offset))
    return SM_FPKG_UNSUPPORTED;
  while (slot_count < count * 2u) {
    if (slot_count > SIZE_MAX / 2u)
      return SM_FPKG_UNSUPPORTED;
    slot_count *= 2u;
  }
  size_t slots_size, total;
  if (!mul_size(slot_count, IDX_SLOT, &slots_size) ||
      !add_size(slots_offset, slots_size, &total) ||
      slot_count > UINT32_MAX || slots_offset > UINT64_MAX)
    return SM_FPKG_UNSUPPORTED;
  uint8_t *buf = calloc(1, total);
  if (!buf)
    return SM_FPKG_NOMEM;
  memcpy(buf, IDX_MAGIC, 8);
  put_le32(buf + 8, 3);
  put_le32(buf + 12, IDX_RECORD);
  put_le64(buf + 16, count);
  put_le64(buf + 24, path_bytes);
  put_le64(buf + 32, slots_offset);
  put_le32(buf + 40, IDX_SLOT);
  put_le32(buf + 44, (uint32_t)slot_count);

  size_t path_at = 0;
  for (size_t i = 0; i < count; ++i) {
    uint8_t *record = buf + IDX_HEADER + i * IDX_RECORD;
    put_le32(record, (uint32_t)path_at);
    put_le32(record + 4, (uint32_t)rows[i].path_length);
    put_le64(record + 8, rows[i].file->size);
    put_le64(record + 16, (uint64_t)mtime);
    memcpy(buf + path_start + path_at, rows[i].path, rows[i].path_length);
    path_at += rows[i].path_length;
    buf[path_start + path_at++] = 0;
  }
  for (size_t i = 0; i < count; ++i) {
    uint64_t hash = rows[i].folded_hash;
    size_t at = (size_t)hash & (slot_count - 1u);
    bool shared = false;
    while (read_le32(buf + slots_offset + at * IDX_SLOT + 8u) != 0) {
      uint8_t *slot = buf + slots_offset + at * IDX_SLOT;
      uint64_t old_hash = (uint64_t)slot[0] | ((uint64_t)slot[1] << 8) |
                          ((uint64_t)slot[2] << 16) |
                          ((uint64_t)slot[3] << 24) |
                          ((uint64_t)slot[4] << 32) |
                          ((uint64_t)slot[5] << 40) |
                          ((uint64_t)slot[6] << 48) |
                          ((uint64_t)slot[7] << 56);
      if (old_hash == hash) {
        put_le32(slot + 12, read_le32(slot + 12) | IDX_SLOT_SHARED);
        shared = true;
      }
      at = (at + 1u) & (slot_count - 1u);
    }
    uint8_t *slot = buf + slots_offset + at * IDX_SLOT;
    put_le64(slot, hash);
    put_le32(slot + 8, (uint32_t)i + 1u);
    put_le32(slot + 12, shared ? IDX_SLOT_SHARED : 0);
  }
  out->data = buf;
  out->size = total;
  return SM_FPKG_OK;
}

static sm_fpkg_status_t chunk_descriptor(const sm_fpkg_index_chunk_t *chunk,
                                         uint64_t wanted, uint64_t package_size,
                                         uint64_t volume_size,
                                         uint64_t *location_out,
                                         uint32_t *descriptor_out) {
  uint64_t stored;
  uint32_t codec;
  uint8_t flags = 0;
  uint64_t location = chunk->package_offset;
  if (chunk->raw_length < wanted || chunk->raw_length > PAK_BLOCK)
    return SM_FPKG_CORRUPT;
  if (!chunk->kraken_encoded) {
    if (chunk->stored_length != chunk->raw_length)
      return SM_FPKG_CORRUPT;
    stored = wanted;
    codec = PAK_CODEC_RAW;
  } else {
    stored = chunk->stored_length;
    if (stored == 0 || stored > PAK_BLOCK || chunk->even_length > stored ||
        chunk->mode >= 32 ||
        (chunk->raw_length > wanted && chunk->raw_length != PAK_BLOCK))
      return SM_FPKG_CORRUPT;
    codec = PAK_CODEC_KRAKEN;
    if (chunk->raw_length > wanted)
      flags |= PAK_FLAG_FULL;
    location |= (uint64_t)chunk->even_length << 40;
    location |= (uint64_t)chunk->mode << 59;
  }
  uint64_t limit = codec == PAK_CODEC_KRAKEN ? PAK_KRAKEN_OFFSET_LIMIT
                                               : PAK_OFFSET_LIMIT;
  if (chunk->package_offset < PAK_PAGE ||
      chunk->package_offset >= limit || stored > volume_size ||
      stored > package_size ||
      chunk->package_offset > package_size - stored ||
      chunk->package_offset > volume_size - stored ||
      chunk->package_offset + stored >= limit)
    return SM_FPKG_CORRUPT;
  bool contained = stored <= PAK_PAGE &&
                   chunk->package_offset / PAK_PAGE ==
                       (chunk->package_offset + stored - 1u) / PAK_PAGE;
  bool aligned = chunk->package_offset % PAK_PAGE == 0 &&
                 (stored > PAK_PAGE || contained);
  if (contained)
    flags |= PAK_FLAG_PAGE_CONTAINED;
  if (aligned)
    flags |= PAK_FLAG_PAGE_ALIGNED;
  *location_out = location;
  *descriptor_out = (uint32_t)(stored - 1u) | (codec << 20) |
                    ((uint32_t)flags << 22);
  return SM_FPKG_OK;
}

static sm_fpkg_status_t build_manifest(const index_row_t *rows, size_t count,
                                       size_t path_bytes, size_t chunk_count,
                                       uint64_t pkg_size, int64_t mtime,
                                       sm_fpkg_index_blob_t *out) {
  if (pkg_size > UINT64_MAX - (PAK_PAGE - 1u))
    return SM_FPKG_UNSUPPORTED;
  uint64_t volume_size = (pkg_size + PAK_PAGE - 1u) / PAK_PAGE * PAK_PAGE;
  if (volume_size < 2u * PAK_PAGE)
    volume_size = 2u * PAK_PAGE;
  size_t files_size, chunks_size, strings_size, payload_size, total;
  if (!mul_size(count, PAK_FILE_RECORD, &files_size) ||
      !mul_size(chunk_count, PAK_CHUNK_RECORD, &chunks_size) ||
      !add_size(path_bytes, sizeof("kraken.pkg"), &strings_size) ||
      !add_size(files_size, chunks_size, &payload_size) ||
      !add_size(payload_size, PAK_PACK_RECORD, &payload_size) ||
      !add_size(payload_size, strings_size, &payload_size) ||
      !add_size(PAK_HEADER, payload_size, &total) ||
      chunk_count > UINT32_MAX)
    return SM_FPKG_UNSUPPORTED;
  uint8_t *buf = calloc(1, total);
  if (!buf)
    return SM_FPKG_NOMEM;
  uint8_t *file_records = buf + PAK_HEADER;
  uint8_t *chunk_records = file_records + files_size;
  uint8_t *pack_record = chunk_records + chunks_size;
  uint8_t *strings = pack_record + PAK_PACK_RECORD;
  size_t string_at = 0, chunk_at = 0;

  for (size_t i = 0; i < count; ++i) {
    const sm_fpkg_index_file_t *file = rows[i].file;
    uint8_t *rec = file_records + i * PAK_FILE_RECORD;
    put_le64(rec, pak_hash(rows[i].path, rows[i].path_length));
    put_le64(rec + 8, file->size);
    put_le64(rec + 16, (uint64_t)mtime);
    if (string_at > UINT32_MAX || rows[i].path_length > UINT32_MAX) {
      free(buf);
      return SM_FPKG_UNSUPPORTED;
    }
    put_le32(rec + 32, (uint32_t)string_at);
    put_le32(rec + 36, (uint32_t)rows[i].path_length);
    memcpy(strings + string_at, rows[i].path, rows[i].path_length);
    string_at += rows[i].path_length;
    strings[string_at++] = 0;
    if (!file->chunks)
      continue;
    put_le32(rec + 24, (uint32_t)chunk_at);
    put_le32(rec + 28, (uint32_t)file->chunk_count);
    put_le32(rec + 40, PAK_FILE_PACKED);
    rec[44] = 18;
    for (size_t j = 0; j < file->chunk_count; ++j) {
      uint64_t logical_offset;
      if (j > UINT64_MAX / PAK_BLOCK) {
        free(buf);
        return SM_FPKG_UNSUPPORTED;
      }
      logical_offset = (uint64_t)j * PAK_BLOCK;
      if (logical_offset >= file->size) {
        free(buf);
        return SM_FPKG_CORRUPT;
      }
      uint64_t wanted = file->size - logical_offset;
      if (wanted > PAK_BLOCK)
        wanted = PAK_BLOCK;
      uint64_t location;
      uint32_t descriptor;
      sm_fpkg_status_t status = chunk_descriptor(
          &file->chunks[j], wanted, pkg_size, volume_size, &location,
          &descriptor);
      if (status != SM_FPKG_OK) {
        free(buf);
        return status;
      }
      uint8_t *chunk = chunk_records + chunk_at * PAK_CHUNK_RECORD;
      put_le64(chunk, location);
      put_le32(chunk + 8, descriptor);
      ++chunk_at;
    }
  }
  if (string_at != path_bytes) {
    free(buf);
    return SM_FPKG_CORRUPT;
  }
  put_le64(pack_record, volume_size - PAK_PAGE);
  put_le64(pack_record + 8, volume_size);
  put_le32(pack_record + 16, (uint32_t)string_at);
  put_le32(pack_record + 20, (uint32_t)(sizeof("kraken.pkg") - 1u));
  put_le32(pack_record + 24, PAK_PACK_IO_PAGE_LAYOUT);
  put_le32(pack_record + 28, PAK_PAGE);
  memcpy(strings + string_at, "kraken.pkg", sizeof("kraken.pkg"));

  memcpy(buf, PAK_MAGIC, 8);
  put_le32(buf + 8, 4);
  put_le32(buf + 12, PAK_HEADER);
  put_le32(buf + 20, PAK_ENDIAN);
  uint32_t seed = crc32_parts(file_records, files_size, chunk_records,
                              chunks_size, strings, strings_size);
  for (unsigned i = 0; i < 4; ++i) {
    uint8_t seed_bytes[4], size_bytes[8], index_byte = (uint8_t)i;
    put_le32(seed_bytes, seed);
    put_le64(size_bytes, pkg_size);
    uint32_t id = crc32_parts(seed_bytes, sizeof(seed_bytes), size_bytes,
                              sizeof(size_bytes), &index_byte, 1);
    put_le32(buf + 24 + i * 4u, id);
  }
  put_le64(buf + 40, count);
  put_le64(buf + 48, chunk_count);
  put_le32(buf + 56, 1);
  put_le32(buf + 60, PAK_FILE_RECORD);
  put_le32(buf + 64, PAK_CHUNK_RECORD);
  put_le32(buf + 68, PAK_PACK_RECORD);
  uint64_t offset = PAK_HEADER;
  put_le64(buf + 72, offset);
  offset += files_size;
  put_le64(buf + 80, offset);
  offset += chunks_size;
  put_le64(buf + 88, offset);
  offset += PAK_PACK_RECORD;
  put_le64(buf + 96, offset);
  put_le64(buf + 104, strings_size);
  uint32_t payload_crc = crc32_bytes(buf + PAK_HEADER, total - PAK_HEADER);
  put_le32(buf + PAK_PAYLOAD_CRC_AT, payload_crc);
  const uint8_t zero_crc[4] = {0};
  uint32_t header_crc = crc32_parts(buf, PAK_HEADER_CRC_AT, zero_crc,
                                    sizeof(zero_crc),
                                    buf + PAK_HEADER_CRC_AT + 4u,
                                    PAK_HEADER - PAK_HEADER_CRC_AT - 4u);
  put_le32(buf + PAK_HEADER_CRC_AT, header_crc);
  out->data = buf;
  out->size = total;
  return SM_FPKG_OK;
}

sm_fpkg_status_t sm_fpkg_index_build(
    const sm_fpkg_index_file_t *files, size_t file_count,
    uint64_t package_size, int64_t package_mtime,
    sm_fpkg_index_blob_t *index_out, sm_fpkg_index_blob_t *manifest_out) {
  if (!index_out || !manifest_out || index_out == manifest_out)
    return SM_FPKG_CORRUPT;
  index_out->data = NULL;
  index_out->size = 0;
  manifest_out->data = NULL;
  manifest_out->size = 0;
  if (package_size == 0)
    return SM_FPKG_CORRUPT;
  index_row_t *rows = NULL;
  size_t path_bytes = 0;
  sm_fpkg_status_t status =
      prepare_rows(files, file_count, &rows, &path_bytes);
  if (status != SM_FPKG_OK)
    return status;
  size_t total_chunks = 0;
  for (size_t i = 0; i < file_count; ++i) {
    const sm_fpkg_index_file_t *file = rows[i].file;
    uint64_t expected = file->size / PAK_BLOCK +
                        (file->size % PAK_BLOCK != 0);
    if (file->chunks) {
      if (file->chunk_count != expected ||
          !add_size(total_chunks, file->chunk_count, &total_chunks) ||
          total_chunks > MAX_INDEX_CHUNKS) {
        status = SM_FPKG_CORRUPT;
        goto done;
      }
    } else if (file->chunk_count != 0) {
      status = SM_FPKG_CORRUPT;
      goto done;
    }
  }
  status = build_index(rows, file_count, path_bytes, package_mtime, index_out);
  if (status != SM_FPKG_OK)
    goto done;
  status = build_manifest(rows, file_count, path_bytes, total_chunks,
                          package_size, package_mtime, manifest_out);
  if (status != SM_FPKG_OK) {
    free(index_out->data);
    index_out->data = NULL;
    index_out->size = 0;
  }
done:
  free_rows(rows, file_count);
  return status;
}

typedef struct {
  const sm_fpkg_index_file_t *file;
  bool used;
} package_override_t;

static int compare_folded_paths(const char *a, const char *b) {
  while (*a && *b) {
    uint8_t ac = fold_byte((uint8_t)*a++);
    uint8_t bc = fold_byte((uint8_t)*b++);
    if (ac != bc)
      return ac < bc ? -1 : 1;
  }
  return ((unsigned char)*a > (unsigned char)*b) -
         ((unsigned char)*a < (unsigned char)*b);
}

static int compare_package_overrides(const void *a, const void *b) {
  const package_override_t *left = a, *right = b;
  return compare_folded_paths(left->file->path, right->file->path);
}

sm_fpkg_status_t sm_fpkg_index_build_package_with_overrides(
    const sm_fpkg_t *fpkg, sm_fpkg_index_extract_fn should_extract_fn,
    void *extract_ctx, sm_fpkg_index_extract_fn should_skip_fn,
    void *skip_ctx, const sm_fpkg_index_file_t *overrides,
    size_t override_count, int64_t package_mtime,
    sm_fpkg_index_blob_t *index_out, sm_fpkg_index_blob_t *manifest_out) {
  if (!index_out || !manifest_out || index_out == manifest_out)
    return SM_FPKG_CORRUPT;
  index_out->data = NULL;
  index_out->size = 0;
  manifest_out->data = NULL;
  manifest_out->size = 0;
  if (!fpkg || (override_count && !overrides))
    return SM_FPKG_CORRUPT;
  size_t entry_count = sm_fpkg_file_count(fpkg);
  if (entry_count == 0 || entry_count > MAX_INDEX_FILES ||
      override_count > MAX_INDEX_FILES - entry_count ||
      sm_fpkg_package_size(fpkg) == 0)
    return SM_FPKG_UNSUPPORTED;

  size_t capacity = entry_count + override_count;
  sm_fpkg_index_file_t *files = calloc(capacity, sizeof(*files));
  sm_fpkg_index_chunk_t **owned_chunks =
      calloc(capacity, sizeof(*owned_chunks));
  package_override_t *sorted = override_count
      ? calloc(override_count, sizeof(*sorted)) : NULL;
  sm_fpkg_block_t *blocks = NULL;
  if (!files || !owned_chunks || (override_count && !sorted)) {
    free(files);
    free(owned_chunks);
    free(sorted);
    return SM_FPKG_NOMEM;
  }
  static const sm_fpkg_index_chunk_t empty_chunks[1] = {{0}};
  size_t file_count = 0, total_chunks = 0;
  sm_fpkg_status_t status = SM_FPKG_OK;
  for (size_t i = 0; i < override_count; ++i) {
    if (!overrides[i].path || overrides[i].chunks || overrides[i].chunk_count ||
        !valid_relative_path(overrides[i].path, strlen(overrides[i].path))) {
      status = SM_FPKG_CORRUPT;
      break;
    }
    sorted[i].file = &overrides[i];
  }
  if (status == SM_FPKG_OK && override_count > 1) {
    qsort(sorted, override_count, sizeof(*sorted), compare_package_overrides);
    for (size_t i = 1; i < override_count; ++i) {
      if (compare_folded_paths(sorted[i - 1].file->path,
                               sorted[i].file->path) == 0) {
        status = SM_FPKG_CORRUPT;
        break;
      }
    }
  }
  for (size_t i = 0; status == SM_FPKG_OK && i < entry_count; ++i) {
    sm_fpkg_file_t entry;
    if (!sm_fpkg_file_at(fpkg, i, &entry)) {
      status = SM_FPKG_CORRUPT;
      break;
    }
    if (entry.is_directory)
      continue;
    if (should_skip_fn && should_skip_fn(entry.path, skip_ctx))
      continue;
    sm_fpkg_index_file_t *file = &files[file_count];
    size_t left = 0, right = override_count;
    while (left < right) {
      size_t mid = left + (right - left) / 2u;
      int order = compare_folded_paths(sorted[mid].file->path, entry.path);
      if (order < 0)
        left = mid + 1u;
      else
        right = mid;
    }
    if (left < override_count &&
        compare_folded_paths(sorted[left].file->path, entry.path) == 0) {
      *file = *sorted[left].file;
      sorted[left].used = true;
      goto next_file;
    }
    file->path = entry.path;
    file->size = entry.size;
    if (should_extract_fn && should_extract_fn(entry.path, extract_ctx))
      goto next_file;

    size_t block_count = 0;
    status = sm_fpkg_file_block_count(fpkg, i, &block_count);
    if (status != SM_FPKG_OK)
      break;
    if (block_count > MAX_INDEX_CHUNKS - total_chunks) {
      status = SM_FPKG_UNSUPPORTED;
      break;
    }
    if (block_count == 0) {
      file->chunks = empty_chunks;
      goto next_file;
    }
    if (block_count > SIZE_MAX / sizeof(*blocks) ||
        block_count > SIZE_MAX / sizeof(*owned_chunks[file_count])) {
      status = SM_FPKG_UNSUPPORTED;
      break;
    }
    blocks = malloc(block_count * sizeof(*blocks));
    owned_chunks[file_count] =
        malloc(block_count * sizeof(*owned_chunks[file_count]));
    if (!blocks || !owned_chunks[file_count]) {
      status = SM_FPKG_NOMEM;
      break;
    }
    size_t actual_count = 0;
    status = sm_fpkg_file_blocks(fpkg, i, blocks, block_count,
                                 &actual_count);
    if (status != SM_FPKG_OK)
      break;
    if (actual_count != block_count) {
      status = SM_FPKG_CORRUPT;
      break;
    }
    for (size_t j = 0; j < block_count; ++j) {
      owned_chunks[file_count][j] = (sm_fpkg_index_chunk_t){
          .package_offset = blocks[j].package_offset,
          .stored_length = blocks[j].stored_size,
          .raw_length = blocks[j].raw_size,
          .even_length = blocks[j].even_size,
          .mode = blocks[j].mode,
          .kraken_encoded = blocks[j].kraken_encoded,
      };
    }
    file->chunks = owned_chunks[file_count];
    file->chunk_count = block_count;
    total_chunks += block_count;
    free(blocks);
    blocks = NULL;
next_file:
    ++file_count;
  }
  if (status == SM_FPKG_OK) {
    for (size_t i = 0; i < override_count; ++i)
      if (!sorted[i].used)
        files[file_count++] = *sorted[i].file;
  }
  if (status == SM_FPKG_OK)
    status = sm_fpkg_index_build(files, file_count,
                                 sm_fpkg_package_size(fpkg), package_mtime,
                                 index_out, manifest_out);
  free(blocks);
  for (size_t i = 0; i < capacity; ++i)
    free(owned_chunks[i]);
  free(owned_chunks);
  free(sorted);
  free(files);
  if (status != SM_FPKG_OK) {
    sm_fpkg_index_blob_free(index_out);
    sm_fpkg_index_blob_free(manifest_out);
  }
  return status;
}

sm_fpkg_status_t sm_fpkg_index_build_package(
    const sm_fpkg_t *fpkg, sm_fpkg_index_extract_fn should_extract_fn,
    void *extract_ctx, int64_t package_mtime,
    sm_fpkg_index_blob_t *index_out, sm_fpkg_index_blob_t *manifest_out) {
  return sm_fpkg_index_build_package_with_overrides(
      fpkg, should_extract_fn, extract_ctx, NULL, NULL, NULL, 0, package_mtime,
      index_out, manifest_out);
}

void sm_fpkg_index_blob_free(sm_fpkg_index_blob_t *blob) {
  if (!blob)
    return;
  free(blob->data);
  blob->data = NULL;
  blob->size = 0;
}
