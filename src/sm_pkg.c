#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>

#include "sm_pkg.h"

#define PKG_MAGIC_CNT 0x7F434E54u
#define PKG_MAGIC_FIH 0x7F464948u
#define PKG_HEADER_SIZE 0xA0u
#define PKG_ENTRY_SIZE 0x20u
#define PKG_MAX_ENTRIES 1024u
#define PKG_PARAM_MAX (1024u * 1024u)
#define PKG_ENTRY_PARAM_SFO 0x1000u
#define PKG_ENTRY_PARAM_JSON 0x2000u
#define PKG_PATCH_FLAG_MASK 0x40100000u
#define PKG_FINGERPRINT_SAMPLE 4096u

static bool read_at(int fd, uint64_t offset, void *buffer, size_t size);

static uint64_t fingerprint_bytes(uint64_t hash, const uint8_t *data,
                                  size_t size) {
  for (size_t i = 0; i < size; i++) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

static uint64_t package_fingerprint(int fd, uint64_t file_size) {
  uint8_t sample[PKG_FINGERPRINT_SAMPLE];
  uint64_t hash = 1469598103934665603ull;
  hash = fingerprint_bytes(hash, (const uint8_t *)&file_size,
                           sizeof(file_size));
  size_t first = file_size < sizeof(sample) ? (size_t)file_size : sizeof(sample);
  if (first > 0 && !read_at(fd, 0, sample, first))
    return 0;
  hash = fingerprint_bytes(hash, sample, first);
  if (file_size > first) {
    size_t last = file_size < sizeof(sample) ? (size_t)file_size : sizeof(sample);
    if (!read_at(fd, file_size - last, sample, last))
      return 0;
    hash = fingerprint_bytes(hash, sample, last);
  }
  return hash;
}

static bool read_at(int fd, uint64_t offset, void *buffer, size_t size) {
  size_t done = 0;
  while (done < size) {
    ssize_t n = pread(fd, (char *)buffer + done, size - done,
                      (off_t)(offset + done));
    if (n <= 0)
      return false;
    done += (size_t)n;
  }
  return true;
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

static bool pkg_extension(const char *name) {
  if (!name)
    return false;
  size_t length = strlen(name);
  return length >= 4 && strcasecmp(name + length - 4, ".pkg") == 0;
}

bool sm_pkg_path_is_candidate(const char *path, const char *name) {
  if (!path || !pkg_extension(name ? name : pkg_filename(path)))
    return false;
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
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
  return rest && (strncmp(rest, "app/", 4) == 0 ||
                  strncmp(rest, "patch/", 6) == 0 ||
                  strncmp(rest, "addcont/", 8) == 0);
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

int sm_pkg_compare_versions(const char *left, const char *right) {
  const char *lp = left ? left : "";
  const char *rp = right ? right : "";
  for (;;) {
    unsigned long l = 0;
    unsigned long r = 0;
    while (*lp >= '0' && *lp <= '9')
      l = l * 10u + (unsigned long)(*lp++ - '0');
    while (*rp >= '0' && *rp <= '9')
      r = r * 10u + (unsigned long)(*rp++ - '0');
    if (l != r)
      return l < r ? -1 : 1;
    while (*lp == '.')
      lp++;
    while (*rp == '.')
      rp++;
    bool ld = *lp >= '0' && *lp <= '9';
    bool rd = *rp >= '0' && *rp <= '9';
    if (!ld && !rd)
      return 0;
    if (!ld)
      return -1;
    if (!rd)
      return 1;
  }
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

static bool parse_sfo(const uint8_t *data, size_t size, scan_candidate_t *candidate) {
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
      if (key_length == 5 && memcmp(data + key_pos, "TITLE", 5) == 0) {
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

static bool parse_param_json(const uint8_t *data, size_t size,
                             scan_candidate_t *candidate) {
  while (size > 0 && data[size - 1] == '\0')
    size--;
  if (size == 0)
    return false;
  char *text = malloc(size + 1u);
  if (!text)
    return false;
  memcpy(text, data, size);
  text[size] = '\0';
  struct json_tokener *tokener = json_tokener_new_ex(8);
  if (!tokener) {
    free(text);
    return false;
  }
  json_tokener_set_flags(tokener,
                         JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
  struct json_object *root = json_tokener_parse_ex(tokener, text, (int)size);
  bool ok = root && json_tokener_get_error(tokener) == json_tokener_success &&
            json_object_is_type(root, json_type_object);
  if (ok) {
    struct json_object *value = NULL;
    if (json_object_object_get_ex(root, "contentId", &value) &&
        json_object_is_type(value, json_type_string))
      copy_string(candidate->content_id, sizeof(candidate->content_id),
                  json_object_get_string(value));
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
      if (!params) {
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
  free(text);
  return ok;
}

static bool read_pkg_sfo(int fd, uint64_t base, const uint8_t *header,
                         uint64_t file_size, scan_candidate_t *candidate) {
  uint32_t count = be32(header + 0x10);
  uint32_t table = be32(header + 0x18);
  if (count == 0 || count > PKG_MAX_ENTRIES ||
      table > file_size - base ||
      (uint64_t)count > (file_size - base - table) / PKG_ENTRY_SIZE)
    return false;

  uint8_t entry[PKG_ENTRY_SIZE];
  for (uint32_t i = 0; i < count; i++) {
    uint64_t entry_offset = base + table + (uint64_t)i * PKG_ENTRY_SIZE;
    if (!read_at(fd, entry_offset, entry, sizeof(entry)))
      return false;
    uint32_t id = be32(entry);
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

bool sm_pkg_parse_file(const char *path, const struct stat *st,
                       scan_candidate_t *candidate) {
  if (!path || !st || !candidate || !S_ISREG(st->st_mode) ||
      !pkg_extension(pkg_filename(path)))
    return false;
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return false;
  uint64_t file_size = (uint64_t)st->st_size;
  uint8_t header[PKG_HEADER_SIZE];
  bool ok = file_size >= sizeof(header) && read_at(fd, 0, header, sizeof(header));
  if (!ok) {
    close(fd);
    return false;
  }

  uint32_t magic = be32(header);
  uint32_t outer_content_type = be32(header + 0x74);
  uint32_t outer_content_flags = be32(header + 0x78);
  uint64_t base = 0;
  if (magic == PKG_MAGIC_FIH) {
    uint16_t version = (uint16_t)header[6] | ((uint16_t)header[7] << 8);
    base = le64(header + 0x58);
    if (version != 3 || base > file_size || sizeof(header) > file_size - base ||
        !read_at(fd, base, header, sizeof(header)) || be32(header) != PKG_MAGIC_CNT) {
      close(fd);
      return false;
    }
  } else if (magic != PKG_MAGIC_CNT) {
    close(fd);
    return false;
  }

  memset(candidate, 0, sizeof(*candidate));
  copy_string(candidate->path, sizeof(candidate->path), path);
  candidate->kind = SCAN_CANDIDATE_PKG;
  candidate->source_size = file_size;
  candidate->source_device = (uint64_t)st->st_dev;
  candidate->source_inode = (uint64_t)st->st_ino;
  candidate->source_ctime = (uint64_t)st->st_ctime;
  candidate->source_mtime = st->st_mtime;
  candidate->source_fingerprint = package_fingerprint(fd, file_size);
  if (candidate->source_fingerprint == 0) {
    close(fd);
    return false;
  }
  candidate->outer_content_type = outer_content_type;
  candidate->outer_content_flags = outer_content_flags;
  size_t content_length = 0;
  while (content_length < 36 && header[0x40u + content_length] != '\0')
    content_length++;
  if (content_length == 0 || content_length >= sizeof(candidate->content_id)) {
    close(fd);
    return false;
  }
  memcpy(candidate->content_id, header + 0x40, content_length);
  candidate->content_id[content_length] = '\0';
  bool parsed = read_pkg_sfo(fd, base, header, file_size, candidate);
  close(fd);
  if (!parsed)
    return false;
  if (candidate->title_id[0] == '\0')
    (void)content_title_id(candidate->content_id, candidate->title_id);
  if (candidate->title_id[0] == '\0')
    return false;
  bool supported_title_id =
      strlen(candidate->title_id) == 9u &&
      (strncmp(candidate->title_id, "PPSA", 4u) == 0 ||
       strncmp(candidate->title_id, "CUSA", 4u) == 0 ||
       strncmp(candidate->title_id, "LAPY", 4u) == 0);
  if (supported_title_id) {
    for (size_t i = 4; i < 9; i++) {
      if (candidate->title_id[i] < '0' || candidate->title_id[i] > '9') {
        supported_title_id = false;
        break;
      }
    }
  }
  if (!supported_title_id) {
    return false;
  }
  candidate->content_type = be32(header + 0x74);
  candidate->content_flags = be32(header + 0x78);
  candidate->pkg_classification_confident =
      candidate->content_type == 0x1Au || candidate->content_type == 0x1Bu ||
      candidate->content_type == 0x20u || candidate->content_type == 0x21u;
  if (candidate->content_type == 0x26u) {
    candidate->pkg_kind = PKG_KIND_UNKNOWN;
    candidate->pkg_classification_confident = false;
  } else if (candidate->content_type == 0x1Bu ||
             candidate->content_type == 0x21u) {
    candidate->pkg_kind = PKG_KIND_DLC;
    if (candidate->category[0] == '\0')
      copy_string(candidate->category, sizeof(candidate->category), "ac");
  } else if ((candidate->content_flags & PKG_PATCH_FLAG_MASK) != 0) {
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
