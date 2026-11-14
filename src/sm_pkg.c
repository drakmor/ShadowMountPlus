// PKG parsing adapted from 0xMH/ShadowMountPlus PR #157 (GPL-3.0).
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>

#include "sm_pkg.h"
#include "sm_smb.h"
#include "sm_vfs.h"
#include "sm_gameinfo.h"

#define PKG_MAGIC_CNT 0x7F434E54u
#define PKG_MAGIC_FIH 0x7F464948u
#define PKG_MAGIC_LIH 0x7F4C4948u
#define PKG_HEADER_SIZE 0xA0u
#define PKG_ENTRY_SIZE 0x20u
#define PKG_MAX_ENTRIES 1024u
#define PKG_PARAM_MAX (1024u * 1024u)
#define PKG_ENTRY_PARAM_SFO 0x1000u
#define PKG_ENTRY_PARAM_JSON 0x2000u
#define PKG_PATCH_FLAG_MASK 0x40100000u

bool sm_pkg_is_media_remaster(const sm_pkg_info_t *info) {
  return info && info->content_type == 0x26u &&
         (info->content_flags & 0x40400000u) == 0x40400000u;
}

typedef struct { sm_pkg_read_fn read; void *ctx; int error; } pkg_reader_t;
static bool read_at(pkg_reader_t *reader, uint64_t offset, void *buffer, size_t size) {
  int status = (reader->read)(reader->ctx, offset, buffer, size);
  if (status) { reader->error = status; errno = status; }
  return status == 0;
}

static int file_read_at(void *ctx, uint64_t offset, void *buffer, size_t size) {
  int fd = *(int *)ctx;
  size_t done = 0;
  while (done < size) {
    ssize_t n = pread(fd, (char *)buffer + done, size - done,
                      (off_t)(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return n == 0 ? EIO : errno;
    done += (size_t)n;
  }
  return 0;
}

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t le64(const uint8_t *p) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; i--)
    value = (value << 8) | p[i];
  return value;
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool executable_range(uint64_t offset, uint64_t size, uint64_t limit) {
  return offset <= limit && size <= limit - offset;
}

static uint32_t read_executable_sdk(int fd, uint64_t size) {
  // Only the bounded SELF/ELF headers and the 24-byte process parameter are
  // read. In particular, do not scan a large eboot or guess from byte patterns.
  uint8_t header[8192], proc[24];
  size_t length = size < sizeof(header) ? (size_t)size : sizeof(header);
  if (length < 64 || file_read_at(&fd, 0, header, length) != 0) return 0;
  bool self = le32(header) == 0xEEF51454u;
  uint16_t segments = self ? le16(header + 24) : 0;
  uint64_t elf = self ? 32u + (uint64_t)segments * 32u : 0;
  uint64_t limit = self ? le64(header + 16) : size;
  if ((self && (!segments || segments > 64 || limit > size)) ||
      !executable_range(elf, 64, length) || !executable_range(elf, 64, limit)) return 0;
  const uint8_t *eh = header + elf;
  if (memcmp(eh, "\x7f" "ELF", 4) || eh[4] != 2 || eh[5] != 1 || eh[6] != 1 ||
      le16(eh + 18) != 62) return 0;
  uint64_t phoff = le64(eh + 32);
  uint16_t count = le16(eh + 56);
  if (le16(eh + 54) != 56 || !count || count > 64 || phoff < 64 ||
      !executable_range(phoff, (uint64_t)count * 56u, length - elf) ||
      !executable_range(phoff, (uint64_t)count * 56u, limit - elf)) return 0;
  const uint8_t *ph = eh + phoff, *param = NULL;
  for (uint16_t i = 0; i < count; ++i) {
    const uint8_t *entry = ph + (size_t)i * 56u;
    if (le32(entry) != 0x61000001u) continue;
    if (param) return 0;
    param = entry;
  }
  if (!param) return 0;
  uint64_t offset = le64(param + 8), param_size = le64(param + 32);
  if (param_size < sizeof(proc)) return 0;
  if (self) {
    bool mapped = false;
    for (uint16_t i = 0; i < count; ++i) {
      const uint8_t *load = ph + (size_t)i * 56u;
      uint64_t start = le64(load + 8), bytes = le64(load + 32);
      if (le32(load) != 1 || offset < start ||
          !executable_range(offset - start, param_size, bytes)) continue;
      for (uint16_t j = 0; j < segments; ++j) {
        const uint8_t *segment = header + 32u + (size_t)j * 32u;
        uint64_t flags = le64(segment), physical = le64(segment + 8);
        uint64_t stored = le64(segment + 16), unpacked = le64(segment + 24);
        if ((flags >> 20) != i || !(flags & 0x800u)) continue;
        if (mapped || (flags & 0xAu) || stored != unpacked ||
            physical < elf + phoff + (uint64_t)count * 56u ||
            !executable_range(physical, stored, limit) ||
            !executable_range(offset - start, param_size, stored)) return 0;
        if (file_read_at(&fd, physical + offset - start, proc, sizeof(proc)) != 0) return 0;
        mapped = true;
      }
    }
    if (!mapped) return 0;
  } else if (!executable_range(offset, param_size, size) ||
             file_read_at(&fd, offset, proc, sizeof(proc)) != 0) return 0;
  if (le32(proc + 8) != 0x4942524Fu || le64(proc) < sizeof(proc) ||
      le64(proc) > param_size) return 0;
  // PS4 uses a 32-bit SDK in process-parameter version 1; PS5 stores the
  // packed SDK in the high half of the 64-bit version field.
  uint32_t version = le32(proc + 12);
  if (!version) return 0;
  uint32_t sdk = le32(proc + (version == 1 ? 16 : 20));
  unsigned major = sdk >> 24;
  return major && (major >> 4) <= 9 && (major & 15u) <= 9 ? sdk : 0;
}

uint32_t sm_pkg_read_executable_sdk(const char *path) {
  if (!path) return 0;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return 0;
  struct stat st;
  uint32_t sdk = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0
                     ? read_executable_sdk(fd, (uint64_t)st.st_size) : 0;
  close(fd);
  return sdk;
}

static const char *pkg_filename(const char *path) {
  const char *slash = path ? strrchr(path, '/') : NULL;
  return slash ? slash + 1 : path;
}

static void copy_string(char *out, size_t out_size, const char *value) {
  if (!out || out_size == 0)
    return;
  if (!value)
    value = "";
  size_t length = strlen(value);
  if (length >= out_size)
    length = out_size - 1u;
  memcpy(out, value, length);
  out[length] = '\0';
}

static bool package_content_id_valid(const char *content_id) {
  if (!content_id[0]) return false;
  for (size_t i = 0; content_id[i]; ++i) {
    unsigned char c = (unsigned char)content_id[i];
    if (i >= MAX_CONTENT_ID - 1u || (!isalnum(c) && c != '-' && c != '_')) return false;
  }
  return true;
}

static bool pkg_extension(const char *name) {
  if (!name)
    return false;
  size_t length = strlen(name);
  return length >= 4 && strcasecmp(name + length - 4, ".pkg") == 0;
}

bool sm_pkg_is_installed_content_path(const char *path) {
  if (!path)
    return false;
  const char *rest = NULL;
  if (strncmp(path, "/user/", 6) == 0) {
    rest = path + 6;
  } else if (strncmp(path, "/mnt/ext", 8) == 0) {
    const char *p = path + 8;
    if (*p < '0' || *p > '9')
      return false;
    while (*p >= '0' && *p <= '9')
      p++;
    if (strncmp(p, "/user/", 6) == 0)
      rest = p + 6;
    else if (strncmp(p, "/ps5/user/", 10) == 0)
      rest = p + 10;
  }
  return rest && (strcmp(rest, "app") == 0 || strncmp(rest, "app/", 4) == 0 ||
                  strcmp(rest, "patch") == 0 || strncmp(rest, "patch/", 6) == 0 ||
                  strcmp(rest, "addcont") == 0 || strncmp(rest, "addcont/", 8) == 0);
}

const char *sm_pkg_kind_name(pkg_kind_t kind) {
  switch (kind) {
  case PKG_KIND_BASE:
    return "base";
  case PKG_KIND_PATCH:
    return "patch";
  case PKG_KIND_DLC:
    return "dlc";
  default:
    return "unknown";
  }
}

bool sm_pkg_kind_is_installable(pkg_kind_t kind) {
  return kind == PKG_KIND_BASE || kind == PKG_KIND_PATCH ||
         kind == PKG_KIND_DLC;
}

static bool content_title_id(const char *content_id, char out[MAX_TITLE_ID]) {
  const char *dash = strchr(content_id, '-');
  if (!dash)
    return false;
  dash++;
  const char *end = strchr(dash, '_');
  if (!end || end - dash != 9)
    return false;
  if (end - dash >= MAX_TITLE_ID)
    return false;
  for (const char *p = dash; p < end; p++) {
    if (!isalnum((unsigned char)*p))
      return false;
  }
  size_t length = (size_t)(end - dash);
  memcpy(out, dash, length);
  out[length] = '\0';
  return true;
}

static uint32_t read_pubtool_sdk(const uint8_t *text, size_t size) {
  for (size_t i = 0; i + 16u <= size; ++i) {
    if ((i && text[i - 1] != ',' && text[i - 1] != ' ') ||
        memcmp(text + i, "sdk_ver=", 8)) continue;
    uint32_t value = 0;
    for (size_t j = 0; j < 8; ++j) {
      unsigned char c = text[i + 8 + j];
      if (!isxdigit(c)) return 0;
      value = (value << 4) | (uint32_t)(c <= '9' ? c - '0' : tolower(c) - 'a' + 10);
    }
    if (i + 16u < size && text[i + 16] && text[i + 16] != ',' && text[i + 16] != ' ') return 0;
    return value;
  }
  return 0;
}

static bool parse_sfo(const uint8_t *data, size_t size, sm_pkg_info_t *candidate) {
  if (size < 20 || memcmp(data, "\0PSF", 4) != 0)
    return false;
  uint32_t key_offset = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
                        ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
  uint32_t value_offset = (uint32_t)data[12] | ((uint32_t)data[13] << 8) |
                          ((uint32_t)data[14] << 16) | ((uint32_t)data[15] << 24);
  uint32_t count = (uint32_t)data[16] | ((uint32_t)data[17] << 8) |
                  ((uint32_t)data[18] << 16) | ((uint32_t)data[19] << 24);
  if (count > (size - 20u) / 16u || key_offset >= size || value_offset >= size)
    return false;

  for (uint32_t i = 0; i < count; i++) {
    size_t entry = 20u + (size_t)i * 16u;
    uint16_t key_rel = (uint16_t)data[entry] | ((uint16_t)data[entry + 1] << 8);
    uint16_t format = (uint16_t)data[entry + 2] |
                      ((uint16_t)data[entry + 3] << 8);
    uint32_t length = (uint32_t)data[entry + 4] |
                     ((uint32_t)data[entry + 5] << 8) |
                     ((uint32_t)data[entry + 6] << 16) |
                     ((uint32_t)data[entry + 7] << 24);
    uint32_t value_rel = (uint32_t)data[entry + 12] |
                         ((uint32_t)data[entry + 13] << 8) |
                         ((uint32_t)data[entry + 14] << 16) |
                         ((uint32_t)data[entry + 15] << 24);
    size_t key_pos = (size_t)key_offset + key_rel;
    size_t value_pos = (size_t)value_offset + value_rel;
    if (key_pos >= size || value_pos > size || length > size - value_pos)
      continue;

    const uint8_t *key_end = memchr(data + key_pos, '\0', size - key_pos);
    if (!key_end)
      continue;
    size_t key_length = (size_t)(key_end - (data + key_pos));
    const char *destination = NULL;
    size_t destination_size = 0;
    if (format == 0x0004 || format == 0x0204) {
      if (key_length == 11 && memcmp(data + key_pos, "PUBTOOLINFO", 11) == 0) {
        candidate->sdk_version = read_pubtool_sdk(data + value_pos, length);
      } else if (key_length == 5 && memcmp(data + key_pos, "TITLE", 5) == 0) {
        destination = candidate->title_name;
        destination_size = sizeof(candidate->title_name);
      } else if (key_length == 8 && memcmp(data + key_pos, "TITLE_ID", 8) == 0) {
        destination = candidate->title_id;
        destination_size = sizeof(candidate->title_id);
      } else if (key_length == 8 && memcmp(data + key_pos, "CATEGORY", 8) == 0) {
        destination = candidate->category;
        destination_size = sizeof(candidate->category);
      } else if (key_length == 7 && memcmp(data + key_pos, "APP_VER", 7) == 0) {
        destination = candidate->app_ver;
        destination_size = sizeof(candidate->app_ver);
      } else if (key_length == 10 && memcmp(data + key_pos, "CONTENT_ID", 10) == 0) {
        destination = candidate->content_id;
        destination_size = sizeof(candidate->content_id);
      }
    }
    if (!destination)
      continue;
    size_t copy_length = 0;
    while (copy_length < length && data[value_pos + copy_length] != '\0')
      copy_length++;
    if (copy_length >= destination_size)
      continue;
    memcpy((char *)destination, data + value_pos, copy_length);
    ((char *)destination)[copy_length] = '\0';
  }
  return true;
}

static uint32_t parse_json_sdk(const char *text) {
  if (!text || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) return 0;
  size_t length = strlen(text + 2);
  if (length != 8 && length != 16) return 0;
  uint64_t value = 0;
  for (size_t i = 2; text[i]; ++i) {
    unsigned char c = (unsigned char)text[i];
    if (!isxdigit(c)) return 0;
    value = (value << 4) | (uint64_t)(c <= '9' ? c - '0' : tolower(c) - 'a' + 10);
  }
  return length == 16 ? (uint32_t)(value >> 32) : (uint32_t)value;
}

static bool parse_param_json(const uint8_t *data, size_t size,
                             sm_pkg_info_t *candidate) {
  while (size > 0 && data[size - 1] == '\0')
    size--;
  if (size == 0)
    return false;
  struct json_tokener *tokener = json_tokener_new_ex(8);
  if (!tokener) return false;
  json_tokener_set_flags(tokener,
                         JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
  struct json_object *root = json_tokener_parse_ex(tokener, (const char *)data, (int)size);
  bool ok = root && json_tokener_get_error(tokener) == json_tokener_success &&
            json_object_is_type(root, json_type_object);
  if (ok) {
    struct json_object *value = NULL;
    if (json_object_object_get_ex(root, "sdkVersion", &value) &&
        json_object_is_type(value, json_type_string))
      candidate->sdk_version = parse_json_sdk(json_object_get_string(value));
    if (json_object_object_get_ex(root, "contentId", &value) &&
        json_object_is_type(value, json_type_string)) {
      const char *content_id = json_object_get_string(value);
      size_t length = (size_t)json_object_get_string_len(value);
      if (length >= sizeof(candidate->content_id) || memchr(content_id, '\0', length)) ok = false;
      else copy_string(candidate->content_id, sizeof(candidate->content_id), content_id);
    }
    if (json_object_object_get_ex(root, "titleId", &value) &&
        json_object_is_type(value, json_type_string))
      copy_string(candidate->title_id, sizeof(candidate->title_id),
                  json_object_get_string(value));
    if (json_object_object_get_ex(root, "contentVersion", &value) &&
        json_object_is_type(value, json_type_string))
      copy_string(candidate->app_ver, sizeof(candidate->app_ver),
                  json_object_get_string(value));
    if (json_object_object_get_ex(root, "masterVersion", &value) &&
        json_object_is_type(value, json_type_string) &&
        candidate->app_ver[0] == '\0')
      copy_string(candidate->app_ver, sizeof(candidate->app_ver),
                  json_object_get_string(value));
    struct json_object *localized = NULL;
    if (json_object_object_get_ex(root, "localizedParameters", &localized) &&
        json_object_is_type(localized, json_type_object)) {
      struct json_object *language = NULL;
      const char *preferred = NULL;
      if (json_object_object_get_ex(localized, "defaultLanguage", &language) &&
          json_object_is_type(language, json_type_string))
        preferred = json_object_get_string(language);
      struct json_object *params = NULL;
      if (preferred)
        json_object_object_get_ex(localized, preferred, &params);
      if (!json_object_is_type(params, json_type_object)) {
        params = NULL;
        struct json_object_iter iterator;
        json_object_object_foreachC(localized, iterator) {
          if (strcmp(iterator.key, "defaultLanguage") != 0 &&
              json_object_is_type(iterator.val, json_type_object)) {
            params = iterator.val;
            break;
          }
        }
      }
      if (params && json_object_object_get_ex(params, "titleName", &value) &&
          json_object_is_type(value, json_type_string))
        copy_string(candidate->title_name, sizeof(candidate->title_name),
                    json_object_get_string(value));
    }
  }
  if (root)
    json_object_put(root);
  json_tokener_free(tokener);
  return ok;
}

static bool read_pkg_sfo(pkg_reader_t *fd, uint64_t base, const uint8_t *header,
                         uint64_t file_size, sm_pkg_info_t *candidate) {
  uint32_t count = be32(header + 0x10);
  uint32_t table = be32(header + 0x18);
  if (count == 0 || count > PKG_MAX_ENTRIES ||
      table > file_size - base ||
      (uint64_t)count > (file_size - base - table) / PKG_ENTRY_SIZE)
    return false;

  // Read the directory in bounded batches instead of issuing one pread per entry.
  uint8_t entries[32u * PKG_ENTRY_SIZE];
  for (uint32_t i = 0; i < count; i++) {
    if (i % 32u == 0) {
      size_t batch = count - i < 32u ? count - i : 32u;
      if (!read_at(fd, base + table + (uint64_t)i * PKG_ENTRY_SIZE,
                   entries, batch * PKG_ENTRY_SIZE)) return false;
    }
    const uint8_t *entry = entries + (i % 32u) * PKG_ENTRY_SIZE;
    uint32_t id = be32(entry);
    if (id == 0x1200u) {
      uint32_t offset = be32(entry + 0x10), length = be32(entry + 0x14);
      if (length >= 8 && length <= SM_PKG_ICON_MAX && offset <= file_size - base &&
          length <= file_size - base - offset) {
        candidate->icon_offset = base + offset;
        candidate->icon_size = length;
      }
      continue;
    }
    if (id != PKG_ENTRY_PARAM_SFO && id != PKG_ENTRY_PARAM_JSON)
      continue;
    uint32_t offset = be32(entry + 0x10);
    uint32_t length = be32(entry + 0x14);
    if (length == 0 || length > PKG_PARAM_MAX || offset > file_size - base ||
        (uint64_t)length > file_size - base - offset)
      continue;
    uint8_t *data = malloc(length);
    if (!data)
      return false;
    bool ok = read_at(fd, base + offset, data, length);
    if (ok && id == PKG_ENTRY_PARAM_SFO)
      ok = parse_sfo(data, length, candidate);
    else if (ok && id == PKG_ENTRY_PARAM_JSON)
      ok = parse_param_json(data, length, candidate);
    free(data);
    if (!ok && (id == PKG_ENTRY_PARAM_SFO || id == PKG_ENTRY_PARAM_JSON))
      return false;
  }
  return true;
}

static bool parse_package(const char *path, const struct stat *st,
                          pkg_reader_t *fd, sm_pkg_info_t *candidate) {
  uint64_t file_size = (uint64_t)st->st_size;
  uint8_t header[PKG_HEADER_SIZE];
  bool ok = file_size >= sizeof(header) && read_at(fd, 0, header, sizeof(header));
  if (!ok) {
    return false;
  }

  uint32_t magic = be32(header);
  uint32_t outer_content_type = be32(header + 0x74);
  uint32_t outer_content_flags = be32(header + 0x78);
  uint64_t base = 0;
  if (magic == PKG_MAGIC_FIH || magic == PKG_MAGIC_LIH) {
    uint16_t version = (uint16_t)header[6] | ((uint16_t)header[7] << 8);
    base = le64(header + (magic == PKG_MAGIC_LIH ? 0x30 : 0x58));
    if (version != (magic == PKG_MAGIC_LIH ? 1 : 3) || base < sizeof(header) || base > file_size || sizeof(header) > file_size - base ||
        !read_at(fd, base, header, sizeof(header)) || be32(header) != PKG_MAGIC_CNT) {
        return false;
    }
  } else if (magic != PKG_MAGIC_CNT) {
    return false;
  }

  memset(candidate, 0, sizeof(*candidate));
  copy_string(candidate->path, sizeof(candidate->path), path);
  candidate->source_size = file_size;
  candidate->source_device = (uint64_t)st->st_dev;
  candidate->source_inode = (uint64_t)st->st_ino;
  candidate->source_ctime = (uint64_t)st->st_ctime;
  candidate->source_mtime = st->st_mtime;
  candidate->source_mtime_ns = st->st_mtim.tv_nsec;
  candidate->source_ctime_ns = st->st_ctim.tv_nsec;
  candidate->outer_content_type = outer_content_type;
  candidate->outer_content_flags = outer_content_flags;
  size_t content_length = 0;
  while (content_length < 36 && header[0x40u + content_length] != '\0')
    content_length++;
  if (content_length == 0 || content_length >= sizeof(candidate->content_id)) {
    return false;
  }
  memcpy(candidate->content_id, header + 0x40, content_length);
  candidate->content_id[content_length] = '\0';
  if (!package_content_id_valid(candidate->content_id)) return false;
  bool parsed = read_pkg_sfo(fd, base, header, file_size, candidate);
  if (!parsed)
    return false;
  if (!package_content_id_valid(candidate->content_id)) return false;
  if (candidate->title_id[0] == '\0')
    (void)content_title_id(candidate->content_id, candidate->title_id);
  if (candidate->title_id[0] == '\0')
    return false;
  if (!is_supported_game_title_id(candidate->title_id)) {
    return false;
  }
  candidate->content_type = be32(header + 0x74);
  candidate->content_flags = be32(header + 0x78);
  uint32_t container_type = be32(header + 4);
  bool delta = magic == PKG_MAGIC_LIH || (container_type & 0xFFu) == 0x1Eu ||
               (container_type & 0xFF000000u) == 0x41000000u;
  bool patch_category = strcasecmp(candidate->category, "gp") == 0 ||
                        strcasecmp(candidate->category, "gpd") == 0 ||
                        strcasecmp(candidate->category, "ps5dp") == 0;
  candidate->pkg_classification_confident =
      candidate->content_type == 0x1Au || candidate->content_type == 0x1Bu ||
      candidate->content_type == 0x1Cu ||
      candidate->content_type == 0x20u || candidate->content_type == 0x21u ||
      candidate->content_type == 0x26u || delta;
  if (candidate->content_type == 0x1Bu ||
             candidate->content_type == 0x1Cu ||
             candidate->content_type == 0x21u) {
    candidate->pkg_kind = PKG_KIND_DLC;
    if (candidate->category[0] == '\0')
      copy_string(candidate->category, sizeof(candidate->category), "ac");
  } else if (delta || patch_category ||
             (!sm_pkg_is_media_remaster(candidate) && (candidate->content_flags & PKG_PATCH_FLAG_MASK) != 0)) {
    candidate->pkg_kind = PKG_KIND_PATCH;
    if (candidate->category[0] == '\0')
      copy_string(candidate->category, sizeof(candidate->category), "gp");
  } else {
    candidate->pkg_kind = PKG_KIND_BASE;
    if (candidate->category[0] == '\0')
      copy_string(candidate->category, sizeof(candidate->category), "gd");
  }
  if (candidate->category[0] != '\0') {
    pkg_kind_t sfo_kind = PKG_KIND_UNKNOWN;
    if (strcasecmp(candidate->category, "gd") == 0 ||
        strcasecmp(candidate->category, "gde") == 0 ||
        strcasecmp(candidate->category, "gdc") == 0 ||
        strcasecmp(candidate->category, "gdp") == 0)
      sfo_kind = PKG_KIND_BASE;
    else if (strcasecmp(candidate->category, "gp") == 0 ||
             strcasecmp(candidate->category, "gpd") == 0 ||
             strcasecmp(candidate->category, "ps5dp") == 0)
      sfo_kind = PKG_KIND_PATCH;
    else if (strcasecmp(candidate->category, "ac") == 0 ||
             strcasecmp(candidate->category, "ps5ac") == 0 ||
             strcasecmp(candidate->category, "dlc") == 0)
      sfo_kind = PKG_KIND_DLC;
    if (sfo_kind != PKG_KIND_UNKNOWN && sfo_kind != candidate->pkg_kind)
      candidate->pkg_classification_confident = false;
  }
  if (!sm_pkg_kind_is_installable(candidate->pkg_kind) ||
      !candidate->pkg_classification_confident)
    return false;
  if (candidate->title_name[0] == '\0')
    copy_string(candidate->title_name, sizeof(candidate->title_name),
                pkg_filename(path));
  return true;
}

bool sm_pkg_source_matches(const sm_pkg_info_t *info, const struct stat *st) {
  return info && st && S_ISREG(st->st_mode) &&
    (uint64_t)st->st_size == info->source_size && (uint64_t)st->st_dev == info->source_device &&
    (uint64_t)st->st_ino == info->source_inode && (uint64_t)st->st_ctime == info->source_ctime &&
    st->st_mtime == info->source_mtime && (uint32_t)st->st_mtim.tv_nsec == info->source_mtime_ns &&
    (uint32_t)st->st_ctim.tv_nsec == info->source_ctime_ns;
}

bool sm_pkg_parse_fd(const char *path, int fd, sm_pkg_info_t *candidate) {
  if (!path || fd < 0 || !candidate ||
      !pkg_extension(pkg_filename(path)) || strlen(path) >= MAX_PATH)
    return false;
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY)
    return false;
  struct stat opened;
  if (fstat(fd, &opened) != 0)
    return false;
  if (!S_ISREG(opened.st_mode) || opened.st_size <= 0) {
    errno = EINVAL;
    return false;
  }
  pkg_reader_t reader = {.read = file_read_at, .ctx = &fd};
  return parse_package(path, &opened, &reader, candidate);
}

bool sm_pkg_parse_file(const char *path, const struct stat *st,
                       sm_pkg_info_t *candidate) {
  (void)st;
  if (!path || !candidate || !pkg_extension(pkg_filename(path)) ||
      strlen(path) >= MAX_PATH)
    return false;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return false;
  bool ok = sm_pkg_parse_fd(path, fd, candidate);
  close(fd);
  return ok;
}

bool sm_pkg_parse_stream(const char *name, uint64_t size, sm_pkg_read_fn read,
                          void *ctx, sm_pkg_info_t *candidate) {
  if (!name || !candidate || !read || !size || size > INT64_MAX ||
      strlen(name) >= MAX_PATH || !pkg_extension(name)) return false;
  struct stat st = {.st_size = (off_t)size, .st_mode = S_IFREG};
  pkg_reader_t reader = {.read = read, .ctx = ctx};
  errno = 0;
  bool ok = parse_package(name, &st, &reader, candidate);
  if (!ok) errno = reader.error ? reader.error : errno == ENOMEM ? ENOMEM : EINVAL;
  return ok;
}

static int read_directory_asset(const char *path, const char *name, size_t limit,
                                 unsigned char **data, size_t *size) {
  *data = NULL;
  *size = 0;
  int fd, result;
  if (sm_smb_path(path)) {
    char asset[MAX_PATH];
    int length = snprintf(asset, sizeof(asset), "%s/sce_sys/%s", path, name);
    if (length < 0 || (size_t)length >= sizeof(asset)) return ENAMETOOLONG;
    fd = open(asset, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    result = fd < 0 ? errno : 0;
  } else {
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) return errno;
    int system_dir = openat(directory, "sce_sys", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    result = system_dir < 0 ? errno : 0;
    close(directory);
    if (result) return result;
    fd = openat(system_dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    result = fd < 0 ? errno : 0;
    close(system_dir);
  }
  if (result) return result;
  struct stat before, after;
  if (fstat(fd, &before) != 0) result = errno;
  else if (!S_ISREG(before.st_mode) || before.st_size <= 0 ||
           (uint64_t)before.st_size > limit) result = EINVAL;
  if (!result) {
    *size = (size_t)before.st_size;
    *data = malloc(*size);
    if (!*data) result = ENOMEM;
    else result = file_read_at(&fd, 0, *data, *size);
  }
  if (!result && (fstat(fd, &after) != 0 || before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)) result = ESTALE;
  close(fd);
  if (result) { free(*data); *data = NULL; *size = 0; }
  return result;
}

bool sm_pkg_read_directory_metadata(const char *path, sm_pkg_info_t *info) {
  if (!path || !info) return false;
  memset(info, 0, sizeof(*info));
  const char *names[] = {"param.json", "param.sfo"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    unsigned char *data = NULL;
    size_t size = 0;
    int result = read_directory_asset(path, names[i], PKG_PARAM_MAX, &data, &size);
    if (result == ENOENT) continue;
    if (result) return false;
    bool parsed = i == 0 ? parse_param_json(data, size, info) : parse_sfo(data, size, info);
    free(data);
    if (!parsed) memset(info, 0, sizeof(*info));
    return parsed;
  }
  return false;
}

int sm_pkg_read_directory_icon(const char *path, unsigned char **data, size_t *size) {
  if (!path || !data || !size) return EINVAL;
  int result = read_directory_asset(path, "icon0.png", SM_PKG_ICON_MAX, data, size);
  if (!result && (*size < 8 || memcmp(*data, "\x89PNG\r\n\x1a\n", 8))) {
    free(*data); *data = NULL; *size = 0; result = EINVAL;
  }
  return result;
}

int sm_pkg_read_icon(const char *path, unsigned char **data, size_t *size) {
  if (!path || !data || !size) return EINVAL;
  *data = NULL;
  *size = 0;
  int fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return errno;
  struct stat st;
  sm_pkg_info_t info;
  pkg_reader_t reader = {.read = file_read_at, .ctx = &fd};
  int result = ENOENT;
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
      parse_package(path, &st, &reader, &info) && info.icon_size) {
    unsigned char *bytes = malloc(info.icon_size);
    if (!bytes) result = ENOMEM;
    else {
      result = file_read_at(&fd, info.icon_offset, bytes, info.icon_size);
      if (!result && memcmp(bytes, "\x89PNG\r\n\x1a\n", 8)) result = EINVAL;
      if (!result) { *data = bytes; *size = info.icon_size; }
      else free(bytes);
    }
  }
  close(fd);
  return result;
}
