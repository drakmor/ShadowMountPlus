#include <ctype.h>
#include <errno.h>
#include <expat.h>
#include <fcntl.h>
#include <json-c/json.h>
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/aes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sm_patch_internal.h"

static const char *json_text(struct json_object *object, const char *key) {
  struct json_object *v = NULL;
  if (!json_object_object_get_ex(object, key, &v)) return "";
  if (!json_object_is_type(v, json_type_string)) return NULL;
  const char *s = json_object_get_string(v);
  return strlen(s) == (size_t)json_object_get_string_len(v) ? s : NULL;
}

static bool copy_text(char *out, size_t capacity, const char *text) {
  if (!text || strlen(text) >= capacity) return false;
  memcpy(out, text, strlen(text) + 1);
  return true;
}

static bool cheat_module(const char *name) {
  if (!name || !*name || strlen(name) >= 128 || strchr(name, '/') || strchr(name, '\\')) return false;
  for (const unsigned char *s = (const unsigned char *)name; *s; ++s)
    if (*s < 0x20 || *s == 0x7f || *s == '@') return false;
  return true;
}

static bool offset_hex(const char *text, uint64_t *offset) {
  if (!text || !isxdigit((unsigned char)*text)) return false;
  errno = 0;
  char *end;
  *offset = strtoull(text, &end, 16);
  return !errno && !*end && *offset < UINT64_C(0x800000000000);
}

static int digit(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool cheat_bytes(const char *s, uint8_t **out, size_t *size) {
  if (!s || strlen(s) > PATCH_MAX_VALUE * 3u) return false;
  *out = malloc(strlen(s) / 2 + 1);
  if (!*out) return false;
  *size = 0;
  while (*s) {
    if (isspace((unsigned char)*s) || *s == '-') { ++s; continue; }
    int hi = digit((unsigned char)*s++);
    if (hi < 0 || !*s) return false;
    int lo = digit((unsigned char)*s++);
    if (lo < 0 || *size == PATCH_MAX_VALUE) return false;
    (*out)[(*size)++] = (uint8_t)((hi << 4) | lo);
  }
  return *size != 0;
}

static bool compile_memory(struct json_object *m, const char *module, sm_patch_line_t *line) {
  if (!json_object_is_type(m, json_type_object)) return false;
  const char *name = json_text(m, "module_name");
  if (!name || (*name && !cheat_module(name)) || !offset_hex(json_text(m, "offset"), &line->address)) return false;
  if (!copy_text(line->module, sizeof(line->module), *name ? name : module)) return false;
  struct json_object *v = NULL;
  if (json_object_object_get_ex(m, "absolute", &v)) {
    if (!json_object_is_type(v, json_type_boolean)) return false;
    line->absolute = json_object_get_boolean(v);
  }
  if (json_object_object_get_ex(m, "section", &v)) {
    if (!json_object_is_type(v, json_type_int)) return false;
    int64_t handle = json_object_get_int64(v);
    if (handle < 0 || handle > UINT32_MAX) return false;
    if (handle && !line->absolute)
      snprintf(line->module, sizeof(line->module), "@handle:%u", (unsigned)handle);
  }
  size_t off_size = 0, expected_size = 0;
  if (!cheat_bytes(json_text(m, "on"), &line->bytes, &line->size) ||
      !cheat_bytes(json_text(m, "off"), &line->off, &off_size) || off_size != line->size ||
      line->size > UINT64_C(0x800000000000) - line->address) return false;
  const char *expected = json_text(m, "expected");
  if (!expected || (*expected && (!cheat_bytes(expected, &line->expected, &expected_size) || expected_size != line->size))) return false;
  return true;
}

static uint64_t hash_text(uint64_t hash, const char *s) {
  for (; *s; ++s) { hash ^= (unsigned char)*s; hash *= UINT64_C(1099511628211); }
  return (hash ^ 0xff) * UINT64_C(1099511628211);
}

static bool master_name(const char *name) {
  for (; *name; ++name)
    if (!strncasecmp(name, "mastercode", 10) || !strncasecmp(name, "master code", 11)) return true;
  return false;
}

static bool resolve_master_offset(const sm_game_patch_list_t *list, const sm_game_patch_t *patch,
                                    sm_patch_line_t *line) {
  if (!*patch->master_id || !strstr(patch->name, "MC")) return true;
  const sm_game_patch_t *master = NULL;
  for (size_t i = 0; i < list->count; ++i)
    if (!strcmp(list->entries[i].id, patch->master_id)) master = &list->entries[i];
  if (!master || !master->supported) return false;
  const char *module = *line->module ? line->module : patch->app_elf;
  size_t matches = 0;
  uint64_t found = 0;
  for (size_t i = 0; i < master->line_count; ++i) {
    const sm_patch_line_t *m = &master->lines[i];
    if (m->absolute != line->absolute || strcmp(*m->module ? m->module : master->app_elf, module)) continue;
    if (line->address >= m->address && line->address - m->address <= m->size &&
        line->size <= m->size - (line->address - m->address)) return true;
    if (m->size < 16 || line->size > m->size) continue;
    for (size_t j = 0; j <= m->size - line->size; ++j)
      if (!memcmp(m->bytes + j, line->off, line->size)) { found = m->address + j; if (++matches > 1) return false; }
  }
  if (matches != 1) return false;
  line->address = found;
  return true;
}

static int compile_cheats(struct json_object *root, const char *path, const char *format,
                            const char *title, const char *version, sm_game_patch_list_t *list) {
  const char *id = json_text(root, "id"), *ver = json_text(root, "version"), *process = json_text(root, "process");
  if (!id || !ver || !process || (*id && strcmp(id, title))) return EINVAL;
  // Legacy packs may keep identity/version only in the filename.
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  char file_version[128] = {0};
  if (!*ver) {
    const char *id_in_name = strstr(base, title);
    const char *start = id_in_name ? id_in_name + strlen(title) : base;
    if (*start == '_' && isdigit((unsigned char)start[1])) {
      ++start;
      const char *end = start;
      while (isdigit((unsigned char)*end) || *end == '.') ++end;
      while (end > start && end[-1] == '.') --end;
      if (end > start && (size_t)(end - start) < sizeof(file_version)) {
        memcpy(file_version, start, (size_t)(end - start));
        ver = file_version;
      }
    }
  }
  if (!*process) process = "eboot.bin";
  if (!cheat_module(process)) return EINVAL;
  struct json_object *mods = NULL, *credits = NULL;
  if (!json_object_object_get_ex(root, "mods", &mods) || !json_object_is_type(mods, json_type_array)) return EINVAL;
  size_t nmods = json_object_array_length(mods);
  if (!nmods || nmods > SM_PATCH_MAX_ENTRIES - list->count) return EFBIG;
  char author[128] = {0};
  if (json_object_object_get_ex(root, "credits", &credits) && json_object_is_type(credits, json_type_array) && json_object_array_length(credits)) {
    struct json_object *a = json_object_array_get_idx(credits, 0);
    if (json_object_is_type(a, json_type_string)) snprintf(author, sizeof(author), "%s", json_object_get_string(a));
  }
  char master[17] = {0};
  size_t total_lines = 0, total_bytes = 0;
  for (size_t i = 0; i < list->count; ++i)
    for (size_t j = 0; j < list->entries[i].line_count; ++j) {
      ++total_lines;
      const sm_patch_line_t *l = &list->entries[i].lines[j];
      total_bytes += l->size * (1 + !!l->off + !!l->expected) + l->pattern_size * 2 + l->target_size;
    }
  for (size_t i = 0; i < nmods; ++i) {
    struct json_object *m = json_object_array_get_idx(mods, i), *memory = NULL;
    if (!json_object_is_type(m, json_type_object)) return EINVAL;
    const char *module = json_text(m, "module_name"), *type = json_text(m, "type");
    void *entries = realloc(list->entries, (list->count + 1) * sizeof(*list->entries));
    if (!entries) return ENOMEM;
    list->entries = entries;
    sm_game_patch_t *p = &list->entries[list->count++];
    memset(p, 0, sizeof(*p));
    if (!copy_text(p->name, sizeof(p->name), json_text(m, "name")) || !*p->name ||
        !copy_text(p->app_ver, sizeof(p->app_ver), ver) || !module || (*module && !cheat_module(module)) || !type) return EINVAL;
    const char *hint = json_text(m, "hint");
    struct json_object *hint_value = NULL;
    // JSON null is commonly used for absent hints.
    if (!hint && json_object_object_get_ex(m, "hint", &hint_value) && !hint_value) hint = "";
    if (!copy_text(p->note, sizeof(p->note), hint)) return EINVAL;
    strcpy(p->app_elf, process); strcpy(p->format, format); strcpy(p->author, author);
    strcpy(p->master_id, master);
    p->supported = true;
    p->button = !strcasecmp(type, "button");
    if (*type && strcasecmp(type, "button") && strcasecmp(type, "checkbox")) p->supported = false;
    if (!json_object_object_get_ex(m, "memory", &memory)) json_object_object_get_ex(m, "patches", &memory);
    if (!json_object_is_type(memory, json_type_array)) return EINVAL;
    size_t nlines = json_object_array_length(memory);
    if (!nlines || nlines > PATCH_MAX_LINES - total_lines) return EFBIG;
    total_lines += nlines;
    for (size_t j = 0; j < nlines; ++j) {
      sm_patch_line_t line = {0};
      bool valid = compile_memory(json_object_array_get_idx(memory, j), module, &line);
      if (valid && !master_name(p->name) && !resolve_master_offset(list, p, &line)) {
        valid = false;
        strcpy(p->error, "Ambiguous or missing master-code address");
      }
      total_bytes += line.size * (2 + !!line.expected);
      if (total_bytes > PATCH_MAX_BYTES) { sm_patch_free_line(&line); return EFBIG; }
      int error = valid ? sm_cheat_append_line(p, &line) : EINVAL;
      sm_patch_free_line(&line);
      if (error == ENOMEM || error == EFBIG) return error;
      if (error) p->supported = false;
    }
    if (!p->supported && !*p->error) strcpy(p->error, "Invalid or unsupported cheat memory/type");
    if (!*ver) strcpy(p->error, "Game version is missing in file and filename");
    sm_patch_set_compatibility(p, version);
    // Include the payload: edited cheat files cannot silently reuse a live ID.
    uint64_t hash = hash_text(UINT64_C(14695981039346656037), base);
    const char *serialized = json_object_to_json_string_ext(m, JSON_C_TO_STRING_PLAIN);
    if (!serialized) return ENOMEM;
    hash = hash_text(hash, serialized);
    hash = hash_text(hash, ver); hash = hash_text(hash, process); hash = hash_text(hash, master);
    snprintf(p->id, sizeof(p->id), "%016llx", (unsigned long long)hash);
    for (size_t j = 0; j + 1 < list->count; ++j)
      if (!strcmp(list->entries[j].id, p->id)) return EEXIST;
    bool cave = false;
    for (size_t j = 0; j < p->line_count; ++j) cave |= p->lines[j].size >= 16;
    if (master_name(p->name) && cave) { *p->master_id = 0; strcpy(master, p->id); }
  }
  return 0;
}

typedef struct {
  XML_Parser parser;
  struct json_object *root, *mod, *line;
  unsigned depth, field_depth, elements, mods, lines;
  const char *field;
  char *text;
  size_t used;
  int error;
} shn_parser_t;

static const char *xml_attr(const XML_Char **a, const char *key) {
  for (size_t i = 0; a[i]; i += 2) if (!strcmp(a[i], key)) return a[i+1];
  return "";
}

static void shn_error(shn_parser_t *p, int error) {
  p->error = error;
  XML_StopParser(p->parser, XML_FALSE);
}

static bool json_string(struct json_object *object, const char *key, const char *value) {
  struct json_object *v = json_object_new_string(value);
  if (!v) return false;
  if (!json_object_object_add(object, key, v)) return true;
  json_object_put(v);
  return false;
}

static void XMLCALL shn_start(void *user, const XML_Char *name, const XML_Char **a) {
  shn_parser_t *p = user;
  if (++p->depth > 5 || ++p->elements > 65536) { shn_error(p, EFBIG); return; }
  if (p->depth == 1) {
    if (strcmp(name, "Trainer")) { shn_error(p, EINVAL); return; }
    if (!json_string(p->root, "id", xml_attr(a, "Cusa")) ||
        !json_string(p->root, "version", xml_attr(a, "Version")) ||
        !json_string(p->root, "process", xml_attr(a, "Process"))) { shn_error(p, ENOMEM); return; }
    struct json_object *credits = json_object_new_array();
    struct json_object *author = json_object_new_string(xml_attr(a, "Moder"));
    if (!credits || !author) { if (credits) json_object_put(credits); if (author) json_object_put(author); shn_error(p, ENOMEM); return; }
    if (json_object_array_add(credits, author)) {
      json_object_put(author); json_object_put(credits); shn_error(p, ENOMEM); return;
    }
    if (json_object_object_add(p->root, "credits", credits)) {
      json_object_put(credits); shn_error(p, ENOMEM); return;
    }
  } else if (p->depth == 2 && !strcmp(name, "Cheat")) {
    if (++p->mods > SM_PATCH_MAX_ENTRIES) { shn_error(p, EFBIG); return; }
    const char *label = xml_attr(a, "Text");
    if (!*label) label = xml_attr(a, "CheatName");
    if (!*label) label = xml_attr(a, "Name");
    p->mod = json_object_new_object();
    struct json_object *memory = json_object_new_array();
    if (!p->mod || !memory) { if (memory) json_object_put(memory); shn_error(p, ENOMEM); return; }
    if (json_object_object_add(p->mod, "memory", memory)) {
      json_object_put(memory); shn_error(p, ENOMEM); return;
    }
    if (!json_string(p->mod, "name", label) || !json_string(p->mod, "hint", xml_attr(a, "Description")) ||
        !json_string(p->mod, "type", xml_attr(a, "Type"))) shn_error(p, ENOMEM);
  } else if (p->depth == 3 && p->mod && !strcmp(name, "Cheatline")) {
    if (++p->lines > PATCH_MAX_LINES) { shn_error(p, EFBIG); return; }
    p->line = json_object_new_object();
    if (!p->line) shn_error(p, ENOMEM);
  } else if (p->depth == 3 && !strcmp(name, "Cheatline")) {
    // Unknown code containers (for example StartUP) must not be silently
    // ignored or misreported as an allocator failure when they close.
    shn_error(p, ENOTSUP);
  } else if (p->depth == 4 && p->line) {
    static const char *const fields[][2] = {{"Offset","offset"},{"ValueOn","on"},{"ValueOff","off"},
      {"Expected","expected"},{"ModuleName","module_name"},{"Absolute","absolute"},{"Section","section"}};
    p->field = NULL;
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); ++i)
      if (!strcmp(name, fields[i][0])) p->field = fields[i][1];
    if (p->field) { p->field_depth = p->depth; p->used = 0; }
    else if (strcmp(name, "PS5Offset")) shn_error(p, ENOTSUP);
  } else if (p->depth > 2 && p->mod) shn_error(p, EINVAL);
}

static void XMLCALL shn_text(void *user, const XML_Char *text, int length) {
  shn_parser_t *p = user;
  if (!p->field) return;
  if (length < 0 || (size_t)length > PATCH_MAX_VALUE * 3u - p->used) { shn_error(p, EFBIG); return; }
  memcpy(p->text + p->used, text, (size_t)length);
  p->used += (size_t)length;
}

static void XMLCALL shn_end(void *user, const XML_Char *name) {
  shn_parser_t *p = user;
  if (p->field && p->depth == p->field_depth) {
    p->text[p->used] = 0;
    char *s = p->text;
    while (isspace((unsigned char)*s)) ++s;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    struct json_object *v = NULL, *previous = NULL;
    if (json_object_object_get_ex(p->line, p->field, &previous)) { shn_error(p, EINVAL); return; }
    if (!strcmp(p->field, "absolute")) {
      if (strcmp(s, "true") && strcmp(s, "false") && strcmp(s, "1") && strcmp(s, "0")) { shn_error(p, EINVAL); return; }
      v = json_object_new_boolean(!strcmp(s, "true") || !strcmp(s, "1"));
    } else if (!strcmp(p->field, "section")) {
      errno = 0; char *tail;
      unsigned long handle = strtoul(s, &tail, 10);
      if (!isdigit((unsigned char)*s) || *tail || errno || handle > UINT32_MAX) { shn_error(p, EINVAL); return; }
      v = json_object_new_int64(handle);
    } else v = json_object_new_string(s);
    if (!v || json_object_object_add(p->line, p->field, v)) { if (v) json_object_put(v); shn_error(p, ENOMEM); return; }
    p->field = NULL;
  }
  if (p->depth == 3 && !strcmp(name, "Cheatline")) {
    struct json_object *memory = NULL;
    json_object_object_get_ex(p->mod, "memory", &memory);
    if (!memory || !p->line) { shn_error(p, EINVAL); return; }
    if (json_object_array_add(memory, p->line)) { shn_error(p, ENOMEM); return; }
    p->line = NULL;
  }
  if (p->depth == 2 && !strcmp(name, "Cheat")) {
    struct json_object *mods = NULL;
    json_object_object_get_ex(p->root, "mods", &mods);
    if (!p->mod || json_object_array_add(mods, p->mod)) { shn_error(p, ENOMEM); return; }
    p->mod = NULL;
  }
  --p->depth;
}

static void XMLCALL shn_doctype(void *user, const XML_Char *name,
                                const XML_Char *system, const XML_Char *public_id, int internal) {
  (void)name; (void)system; (void)public_id; (void)internal;
  shn_error(user, EINVAL);
}

static struct json_object *parse_shn(const char *data, size_t size, const char *encoding) {
  shn_parser_t p = {0};
  p.parser = XML_ParserCreate(encoding);
  p.root = json_object_new_object();
  p.text = malloc(PATCH_MAX_VALUE * 3u + 1);
  struct json_object *mods = json_object_new_array();
  if (!p.parser || !p.root || !p.text || !mods) p.error = ENOMEM;
  if (p.root && mods) {
    if (json_object_object_add(p.root, "mods", mods)) p.error = ENOMEM;
    else mods = NULL;
  }
  if (!p.error) {
    XML_SetUserData(p.parser, &p);
    XML_SetElementHandler(p.parser, shn_start, shn_end);
    XML_SetCharacterDataHandler(p.parser, shn_text);
    XML_SetStartDoctypeDeclHandler(p.parser, shn_doctype);
    XML_SetParamEntityParsing(p.parser, XML_PARAM_ENTITY_PARSING_NEVER);
    if (XML_Parse(p.parser, data, (int)size, XML_TRUE) == XML_STATUS_ERROR && !p.error) p.error = EINVAL;
  }
  if (p.parser) XML_ParserFree(p.parser);
  free(p.text);
  if (mods) json_object_put(mods);
  if (p.mod) json_object_put(p.mod);
  if (p.line) json_object_put(p.line);
  if (p.error) { if (p.root) json_object_put(p.root); errno = p.error; return NULL; }
  return p.root;
}

static int decrypt_mc4(char *data, size_t *size) {
  // MC4 uses these literal ASCII key/IV bytes, not a hex-decoded key.
  static const unsigned char key[] = "304c6528f659c766110239a51cl5dd9c";
  static const unsigned char iv[] = "u@}kzW2u[u(8DWar";
  size_t n = 0;
  for (size_t i = 0; i < *size; ++i) if (!isspace((unsigned char)data[i])) data[n++] = data[i];
  if (!n || n % 4) return EINVAL;
  size_t padding = data[n-1] == '=' ? (data[n-2] == '=' ? 2u : 1u) : 0;
  for (size_t i = 0; i < n - padding; ++i) {
    unsigned char c = (unsigned char)data[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/')) return EINVAL;
  }
  unsigned char *decoded = malloc(n / 4 * 3 + 1);
  if (!decoded) return ENOMEM;
  size_t length = 0;
  uint32_t bits = 0;
  unsigned available = 0;
  for (size_t i = 0; i < n - padding; ++i) {
    unsigned char c = (unsigned char)data[i];
    unsigned value = c >= 'A' && c <= 'Z' ? c - (unsigned)'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26u :
                     c >= '0' && c <= '9' ? c - '0' + 52u : c == '+' ? 62u : 63u;
    bits = (bits << 6) | value;
    available += 6;
    if (available >= 8) { available -= 8; decoded[length++] = (uint8_t)(bits >> available); }
  }
  int error = EINVAL;
  if (length && !(length % AES_BLOCK_SIZE) && (!available || !(bits & ((1u << available) - 1)))) {
    AES_KEY schedule;
    unsigned char chain[AES_BLOCK_SIZE];
    memcpy(chain, iv, sizeof(chain));
    if (!AES_set_decrypt_key(key, 256, &schedule)) {
      AES_cbc_encrypt(decoded, (unsigned char *)data, length, &schedule, chain, AES_DECRYPT);
      unsigned pad = (unsigned char)data[length - 1];
      bool valid = pad && pad <= AES_BLOCK_SIZE && pad <= length;
      for (size_t i = 0; valid && i < pad; ++i) valid = (unsigned char)data[length - 1 - i] == pad;
      if (valid) { *size = length - pad; data[*size] = 0; error = 0; }
    }
  }
  free(decoded);
  return error;
}

static const char *cheat_format(const char *path) {
  const char *extension = strrchr(path, '.');
  return extension && !strcasecmp(extension, ".json") ? "JSON" :
                       extension && !strcasecmp(extension, ".shn") ? "SHN" :
                       extension && !strcasecmp(extension, ".mc4") ? "MC4" : NULL;
}

static int read_cheat_document(const char *path, const char *format, struct json_object **document) {
  *document = NULL;
  if (!format) return EINVAL;
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return errno;
  struct stat st;
  int error = fstat(fd, &st) ? errno : !S_ISREG(st.st_mode) ? EINVAL :
              st.st_size <= 0 || st.st_size > PATCH_MAX_FILE ? EFBIG : 0;
  char *data = !error ? malloc((size_t)st.st_size + 1) : NULL;
  if (!error && !data) error = ENOMEM;
  size_t size = 0;
  while (!error && size < (size_t)st.st_size) {
    ssize_t n = read(fd, data + size, (size_t)st.st_size - size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) error = n < 0 ? errno : EIO;
    else size += (size_t)n;
  }
  close(fd);
  if (!error && !strcmp(format, "SHN") && size) {
    // Some SHN exports retain the MC4 container's PKCS#7 tail.
    unsigned pad = (unsigned char)data[size - 1];
    bool valid = pad && pad <= 16 && pad <= size;
    for (size_t i = 0; valid && i < pad; ++i) valid = (unsigned char)data[size - 1 - i] == pad;
    if (valid) size -= pad;
  }
  if (!error) {
    data[size] = 0;
    if (strcmp(format, "SHN") && memchr(data, 0, size)) error = EINVAL;
  }
  if (!error && !strcmp(format, "MC4")) {
    error = decrypt_mc4(data, &size);
    if (!error && memchr(data, 0, size)) error = EINVAL;
  }
  struct json_object *root = NULL;
  if (!error && !strcmp(format, "JSON")) {
    struct json_tokener *tok = json_tokener_new_ex(16);
    if (!tok) error = ENOMEM;
    else {
      json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
      root = json_tokener_parse_ex(tok, data, (int)size);
      size_t end = json_tokener_get_parse_end(tok);
      while (end < size && isspace((unsigned char)data[end])) ++end;
      if (json_tokener_get_error(tok) != json_tokener_success || end != size) error = EINVAL;
      json_tokener_free(tok);
    }
  } else if (!error) {
    // Some MC4 packs encode the complete XML document as XML entities.
    char *s = data;
    while (isspace((unsigned char)*s)) ++s;
    if (!strncmp(s, "&lt;", 4)) {
      static const char *const entities[][2] = {{"&lt;","<"},{"&gt;",">"},{"&quot;","\""},{"&apos;","'"},{"&amp;","&"}};
      char *out = data;
      while (*s) {
        if (*s == '\\' && !strncmp(s + 1, "&quot;", 6)) ++s;
        size_t i;
        for (i = 0; i < sizeof(entities)/sizeof(entities[0]); ++i) {
          size_t n = strlen(entities[i][0]);
          if (!strncmp(s, entities[i][0], n)) { *out++ = *entities[i][1]; s += n; break; }
        }
        if (i == sizeof(entities)/sizeof(entities[0])) *out++ = *s++;
      }
      *out = 0; size = (size_t)(out - data);
    }
    // Legacy MC4 escapes UTF-8 text while retaining a UTF-16 XML declaration.
    root = parse_shn(data, size, !strcmp(format, "MC4") ? "UTF-8" : NULL);
    if (!root) error = errno ? errno : EINVAL;
  }
  if (!error && !json_object_is_type(root, json_type_object)) error = EINVAL;
  free(data);
  if (error) { if (root) json_object_put(root); }
  else *document = root;
  return error;
}

int sm_cheat_metadata(const char *path, char title[10], char version[128]) {
  struct json_object *root = NULL;
  int error = read_cheat_document(path, cheat_format(path), &root);
  if (error) return error;
  if (!copy_text(title, 10, json_text(root, "id")) ||
      !copy_text(version, 128, json_text(root, "version"))) error = EINVAL;
  struct json_object *mods = NULL;
  if (!json_object_object_get_ex(root, "mods", &mods) || !json_object_is_type(mods, json_type_array) ||
      !json_object_array_length(mods)) error = EINVAL;
  json_object_put(root);
  return error;
}

int sm_cheat_load(const char *path, const char *title_id, const char *version,
                   sm_game_patch_list_t *list) {
  const char *format = cheat_format(path);
  struct json_object *root = NULL;
  int error = read_cheat_document(path, format, &root);
  size_t first = list->count;
  if (!error) error = compile_cheats(root, path, format, title_id, version, list);
  if (root) json_object_put(root);
  if (error) sm_patch_truncate(list, first);
  return error;
}
