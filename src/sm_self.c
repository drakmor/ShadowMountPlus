#include "sm_platform.h"
#include "sm_self.h"

static uint16_t le16(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const unsigned char *p) {
  return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static bool executable_range(uint64_t offset, uint64_t size, uint64_t limit) {
  return offset <= limit && size <= limit - offset;
}

static int file_read_at(int *fd, uint64_t offset, void *buffer, size_t size) {
  unsigned char *out = buffer;
  while (size) {
    ssize_t count = pread(*fd, out, size, (off_t)offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return -1;
    out += count;
    size -= (size_t)count;
    offset += (uint64_t)count;
  }
  return 0;
}

typedef struct {
  int fd;
  uint64_t size, elf, phoff;
  uint16_t segments, phnum;
  unsigned char header[8192];
} executable_reader_t;

static const unsigned char *executable_phdr(const executable_reader_t *reader,
                                            unsigned index) {
  return reader->header + reader->elf + reader->phoff + (size_t)index * 56u;
}

static bool read_executable_bytes(executable_reader_t *reader, uint64_t offset,
                                   void *out, size_t size) {
  if (!reader->segments)
    return executable_range(offset, size, reader->size) &&
           file_read_at(&reader->fd, offset, out, size) == 0;
  for (unsigned i = 0; i < reader->phnum; ++i) {
    const unsigned char *ph = executable_phdr(reader, i);
    uint64_t start = le64(ph + 8), bytes = le64(ph + 32);
    if (offset < start || !executable_range(offset - start, size, bytes))
      continue;
    for (unsigned j = 0; j < reader->segments; ++j) {
      const unsigned char *segment = reader->header + 32u + (size_t)j * 32u;
      uint64_t flags = le64(segment), physical = le64(segment + 8);
      uint64_t stored = le64(segment + 16), unpacked = le64(segment + 24);
      if (((flags >> 20) & 0xffffu) != i || !(flags & 0x800u) ||
          (flags & 0xau) || stored != unpacked ||
          physical < reader->elf + reader->phoff + reader->phnum * 56u ||
          !executable_range(physical, stored, reader->size) ||
          !executable_range(offset - start, size, stored))
        continue;
      return file_read_at(&reader->fd, physical + offset - start, out, size) == 0;
    }
  }
  return false;
}

static bool read_executable_filename(executable_reader_t *reader,
                                      char out[MAX_PATH]) {
  size_t length = reader->size < sizeof(reader->header)
                      ? (size_t)reader->size : sizeof(reader->header);
  unsigned char *header = reader->header;
  if (length < 64 || file_read_at(&reader->fd, 0, header, length) != 0)
    return false;
  uint32_t magic = le32(header);
  if (magic == 0xEEF51454u || magic == 0x1D3D154Fu) {
    reader->segments = le16(header + 24);
    reader->elf = 32u + (uint64_t)reader->segments * 32u;
    uint64_t limit = le64(header + 16);
    if (!reader->segments || reader->segments > 64)
      return false;
    // Some FSELF files omit the trailing alignment padding in FileSize.
    // Segment reads must fit both the declared and the physical file size.
    if (limit < reader->size)
      reader->size = limit;
  }
  if (!executable_range(reader->elf, 64, length) ||
      !executable_range(reader->elf, 64, reader->size))
    return false;
  const unsigned char *eh = header + reader->elf;
  if (memcmp(eh, "\x7f" "ELF", 4) || eh[4] != 2 || eh[5] != 1 ||
      eh[6] != 1 || (le16(eh + 16) != 3 && le16(eh + 16) != 0xfe18u) ||
      le16(eh + 18) != 62 || le16(eh + 54) != 56)
    return false;
  reader->phoff = le64(eh + 32);
  reader->phnum = le16(eh + 56);
  if (!reader->phnum || reader->phnum > 64 || reader->phoff < 64 ||
      !executable_range(reader->phoff, reader->phnum * 56u, length - reader->elf) ||
      !executable_range(reader->phoff, reader->phnum * 56u, reader->size - reader->elf))
    return false;
  const unsigned char *dynamic = NULL, *dynlib = NULL;
  for (unsigned i = 0; i < reader->phnum; ++i) {
    const unsigned char *ph = executable_phdr(reader, i);
    if (le32(ph) == 2) dynamic = ph;
    if (le32(ph) == 0x61000000u) dynlib = ph;
  }
  if (!dynamic)
    return false;
  uint64_t dyn_size = le64(dynamic + 32), dyn_offset = le64(dynamic + 8);
  if (dyn_size > 65536u || dyn_size % 16u || dyn_offset > UINT64_MAX - dyn_size)
    return false;
  uint64_t name = UINT64_MAX, strtab = UINT64_MAX, strsz = 0;
  uint64_t sce_strtab = UINT64_MAX, sce_strsz = 0;
  bool terminated = false;
  for (uint64_t offset = 0; offset < dyn_size && !terminated;) {
    unsigned char entries[512];
    size_t bytes = dyn_size - offset < sizeof(entries)
                       ? (size_t)(dyn_size - offset) : sizeof(entries);
    if (!read_executable_bytes(reader, dyn_offset + offset, entries, bytes))
      return false;
    for (size_t i = 0; i < bytes; i += 16u) {
      uint64_t tag = le64(entries + i), value = le64(entries + i + 8u);
      if (!tag) { terminated = true; break; }
      if (tag == 0x61000009u || tag == 0x61000041u) name = value;
      if (tag == 5) strtab = value;
      if (tag == 10) strsz = value;
      if (tag == 0x61000035u) sce_strtab = value;
      if (tag == 0x61000037u) sce_strsz = value;
    }
    offset += bytes;
  }
  if (!terminated || name == UINT64_MAX)
    return false;
  uint64_t string_offset = 0;
  bool mapped = false;
  if (strtab != UINT64_MAX && strsz && name < strsz) {
    for (unsigned i = 0; i < reader->phnum; ++i) {
      const unsigned char *ph = executable_phdr(reader, i);
      uint64_t base = le64(ph + 16), size = le64(ph + 32);
      uint64_t file_offset = le64(ph + 8);
      if (le32(ph) != 1 || strtab < base ||
          !executable_range(strtab - base, strsz, size) ||
          file_offset > UINT64_MAX - (strtab - base) - name)
        continue;
      string_offset = file_offset + strtab - base + name;
      mapped = true;
      break;
    }
  } else if (dynlib && sce_strtab != UINT64_MAX && name < sce_strsz) {
    uint64_t base = le64(dynlib + 8), size = le64(dynlib + 32);
    if (executable_range(sce_strtab, sce_strsz, size) &&
        base <= UINT64_MAX - sce_strtab - name) {
      string_offset = base + sce_strtab + name;
      strsz = sce_strsz;
      mapped = true;
    }
  }
  if (!mapped)
    return false;
  size_t bytes = strsz - name < MAX_PATH ? (size_t)(strsz - name) : MAX_PATH;
  return read_executable_bytes(reader, string_offset, out, bytes) &&
         memchr(out, '\0', bytes) != NULL && out[0];
}

bool sm_self_read_original_filename(const char *path, char out[MAX_PATH]) {
  out[0] = '\0';
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return false;
  struct stat st, after;
  executable_reader_t reader = {.fd = fd};
  bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
  if (valid) {
    reader.size = (uint64_t)st.st_size;
    valid = read_executable_filename(&reader, out) && fstat(fd, &after) == 0 &&
            st.st_size == after.st_size && st.st_mtim.tv_sec == after.st_mtim.tv_sec &&
            st.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
            st.st_ctim.tv_sec == after.st_ctim.tv_sec &&
            st.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
  }
  close(fd);
  if (!valid)
    out[0] = '\0';
  return valid;
}
