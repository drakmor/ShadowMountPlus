#include <ctype.h>
#include <errno.h>
#include <expat.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sm_patch_internal.h"

#define PATCH_MAX_PATTERN 1024u
#define PATCH_MAX_SCAN (512u * 1024u * 1024u)
#define PATCH_SCAN_CHUNK 65536u

void sm_patch_free_line(sm_patch_line_t *line) {
  free(line->bytes);
  free(line->pattern);
  free(line->mask);
  free(line->target);
  free(line->off);
  free(line->expected);
  line->bytes = line->pattern = line->mask = line->target = line->off = line->expected = NULL;
}

void sm_patch_free_entry(sm_game_patch_t *patch) {
  for (size_t i = 0; i < patch->line_count; ++i) sm_patch_free_line(&patch->lines[i]);
  free(patch->lines);
  free(patch->source_path);
}

void sm_patch_truncate(sm_game_patch_list_t *list, size_t count) {
  while (list->count > count) sm_patch_free_entry(&list->entries[--list->count]);
}

void sm_game_patches_free(sm_game_patch_list_t *list) {
  sm_patch_truncate(list, 0);
  free(list->entries);
  memset(list, 0, sizeof(*list));
}

static int hex_digit(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool number(const char *text, uint64_t *value, bool signed_value) {
  if (!text || !*text || isspace((unsigned char)*text)) return false;
  bool negative = *text == '-';
  if (negative && !signed_value) return false;
  if (negative || *text == '+') ++text;
  int base = 10;
  if (*text == '#' || *text == '$') { base = 16; ++text; }
  else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { base = 16; text += 2; }
  if (!*text || hex_digit((unsigned char)*text) < 0) return false;
  errno = 0;
  char *end;
  unsigned long long parsed = strtoull(text, &end, base);
  if (errno || *end || (negative && parsed > UINT64_C(0x8000000000000000))) return false;
  *value = negative ? 0u - (uint64_t)parsed : (uint64_t)parsed;
  return true;
}

static bool hex_bytes(const char *text, bool wildcards, uint8_t **bytes,
                       uint8_t **mask, size_t *size) {
  size_t capacity = strlen(text) / 2 + 1;
  if (capacity > PATCH_MAX_VALUE) return false;
  *bytes = malloc(capacity);
  if (!*bytes) return false;
  if (wildcards) {
    *mask = malloc(capacity);
    if (!*mask) return false;
  }
  *size = 0;
  while (*text) {
    if (isspace((unsigned char)*text)) { ++text; continue; }
    int high = hex_digit((unsigned char)*text++), low;
    bool wildcard = high < 0 && text[-1] == '?' && wildcards;
    if (wildcard) {
      if (*text == '?') ++text;
      (*bytes)[*size] = 0;
      (*mask)[*size] = 0;
    } else {
      if (high < 0 || !*text || (low = hex_digit((unsigned char)*text++)) < 0) return false;
      (*bytes)[*size] = (uint8_t)((high << 4) | low);
      if (wildcards) (*mask)[*size] = 0xff;
    }
    ++*size;
  }
  return *size != 0;
}

static bool string_bytes(const char *text, bool utf16, uint8_t **bytes, size_t *size) {
  size_t length = strlen(text);
  if (length > PATCH_MAX_VALUE / 2) return false;
  uint8_t *decoded = malloc(length + 1);
  *bytes = malloc(length * 2 + 2);
  if (!decoded || !*bytes) { free(decoded); return false; }
  size_t count = 0;
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)text[i];
    if (c == '\\') {
      if (++i == length) { free(decoded); return false; }
      c = (unsigned char)text[i];
      if (c == 'n') c = '\n';
      else if (c == 't') c = '\t';
      else if (c == 'r') c = '\r';
      else if (c == '0') c = 0;
      else if (c == 'x') {
        if (i + 2 >= length || hex_digit((unsigned char)text[i+1]) < 0 ||
            hex_digit((unsigned char)text[i+2]) < 0) { free(decoded); return false; }
        c = (unsigned char)((hex_digit((unsigned char)text[i+1]) << 4) | hex_digit((unsigned char)text[i+2]));
        i += 2;
      } else if (c != '\\') { free(decoded); return false; }
    }
    decoded[count++] = c;
  }
  *size = 0;
  if (!utf16) { memcpy(*bytes, decoded, count); *size = count; }
  for (size_t i = 0; utf16 && i < count;) {
    uint32_t cp = decoded[i++], minimum = 0;
    unsigned tail = 0;
    if (cp >= 0xc2 && cp <= 0xdf) { cp &= 31; tail = 1; minimum = 0x80; }
    else if (cp >= 0xe0 && cp <= 0xef) { cp &= 15; tail = 2; minimum = 0x800; }
    else if (cp >= 0xf0 && cp <= 0xf4) { cp &= 7; tail = 3; minimum = 0x10000; }
    else if (cp >= 0x80) { free(decoded); return false; }
    if (tail > count - i) { free(decoded); return false; }
    while (tail--) {
      if ((decoded[i] & 0xc0) != 0x80) { free(decoded); return false; }
      cp = (cp << 6) | (decoded[i++] & 63);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) { free(decoded); return false; }
    if (cp >= 0x10000) {
      uint32_t high = 0xd800 + ((cp - 0x10000) >> 10);
      (*bytes)[(*size)++] = (uint8_t)high;
      (*bytes)[(*size)++] = (uint8_t)(high >> 8);
      cp = 0xdc00 + ((cp - 0x10000) & 1023);
    }
    (*bytes)[(*size)++] = (uint8_t)cp;
    (*bytes)[(*size)++] = (uint8_t)(cp >> 8);
  }
  free(decoded);
  return *size != 0;
}

static const char *attribute(const XML_Char **attributes, const char *name) {
  for (size_t i = 0; attributes[i]; i += 2)
    if (!strcmp(attributes[i], name)) return attributes[i+1];
  return "";
}

static bool copy_attribute(char *out, size_t size, const XML_Char **attributes,
                            const char *name) {
  const char *value = attribute(attributes, name);
  if (strlen(value) >= size) return false;
  memcpy(out, value, strlen(value) + 1);
  return true;
}

static bool module_name(const char *name) {
  return *name && strlen(name) < 128 && !strchr(name, '/') && !strchr(name, '\\') &&
         strcmp(name, ".") && strcmp(name, "..");
}

static bool compile_line(const XML_Char **a, sm_patch_line_t *line, uint64_t image_base) {
  const char *type = attribute(a, "Type"), *value = attribute(a, "Value");
  const char *address = attribute(a, "Address"), *module = attribute(a, "Module");
  if (!*value || strlen(value) > PATCH_MAX_VALUE * 2u || strlen(address) > PATCH_MAX_PATTERN * 3u)
    return false;
  line->image_base = image_base;
  if (*module) {
    if (!module_name(module) || !copy_attribute(line->module, sizeof(line->module), a, "Module")) return false;
    // Per-line Module addresses are module-relative (NeoEnabler extension).
    line->image_base = 0;
  }
  if (!strcmp(type, "mask") || !strcmp(type, "mask_jump32")) {
    if (!hex_bytes(address, true, &line->pattern, &line->mask, &line->pattern_size) ||
        line->pattern_size > PATCH_MAX_PATTERN) return false;
    bool concrete = false;
    for (size_t i = 0; i < line->pattern_size; ++i) concrete |= line->mask[i] != 0;
    if (!concrete) return false;
    uint64_t offset = 0;
    if (*attribute(a, "Offset") && !number(attribute(a, "Offset"), &offset, true)) return false;
    if (offset > INT64_MAX && attribute(a, "Offset")[0] != '-') return false;
    line->offset = (int64_t)offset;
    if (!hex_bytes(value, false, &line->bytes, NULL, &line->size)) return false;
    if (!strcmp(type, "mask_jump32")) {
      uint64_t size = 5;
      if ((*attribute(a, "Size") && !number(attribute(a, "Size"), &size, false)) ||
          size < 5 || size > 4096 ||
          !hex_bytes(attribute(a, "Target"), false, &line->target, NULL, &line->target_size) ||
          line->target_size > PATCH_MAX_PATTERN) return false;
      line->jump_size = (unsigned)size;
    }
    return true;
  }
  if (!number(address, &line->address, false) || line->address < line->image_base) return false;
  if (!strcmp(type, "bytes")) return hex_bytes(value, false, &line->bytes, NULL, &line->size);
  if (!strcmp(type, "utf8") || !strcmp(type, "utf16"))
    return string_bytes(value, !strcmp(type, "utf16"), &line->bytes, &line->size);
  uint64_t bits = 0;
  if (!strcmp(type, "byte")) line->size = 1;
  else if (!strcmp(type, "bytes16")) line->size = 2;
  else if (!strcmp(type, "bytes32") || !strcmp(type, "float32")) line->size = 4;
  else if (!strcmp(type, "bytes64") || !strcmp(type, "float64")) line->size = 8;
  else return false;
  if (!strcmp(type, "float32") || !strcmp(type, "float64")) {
    char *end;
    errno = 0;
    if (line->size == 4) {
      float v = strtof(value, &end);
      if (errno || end == value || *end || !__builtin_isfinite(v)) return false;
      memcpy(&bits, &v, 4);
    } else {
      double v = strtod(value, &end);
      if (errno || end == value || *end || !__builtin_isfinite(v)) return false;
      memcpy(&bits, &v, 8);
    }
  } else {
    if (!number(value, &bits, true)) return false;
    if (line->size < 8) {
      uint64_t maximum = (UINT64_C(1) << (line->size * 8)) - 1;
      if (value[0] == '-') {
        if (0u - bits > (maximum + 1) / 2) return false;
      } else if (bits > maximum) return false;
    }
  }
  line->bytes = malloc(line->size);
  if (!line->bytes) return false;
  for (size_t i = 0; i < line->size; ++i) line->bytes[i] = (uint8_t)(bits >> (8 * i));
  return true;
}

static bool version_matches(const char *versions, const char *version, bool allow_mask) {
  // Exact dotted components, allowing conventional zero padding; no prefixes.
  while (*versions) {
    while (isspace((unsigned char)*versions) || *versions == ',') ++versions;
    const char *end = versions;
    while (*end && !isspace((unsigned char)*end) && *end != ',') ++end;
    if (allow_mask && end - versions == 4 && !memcmp(versions, "mask", 4)) return true;
    const char *a = versions, *b = version;
    bool same = *version != 0;
    while (same && a < end && *b) {
      if (!isdigit((unsigned char)*a) || !isdigit((unsigned char)*b)) { same = false; break; }
      while (a + 1 < end && *a == '0' && isdigit((unsigned char)a[1])) ++a;
      while (*b == '0' && isdigit((unsigned char)b[1])) ++b;
      const char *aa = a, *bb = b;
      while (a < end && isdigit((unsigned char)*a)) ++a;
      while (isdigit((unsigned char)*b)) ++b;
      same = a - aa == b - bb && !memcmp(aa, bb, (size_t)(a - aa));
      if (a < end || *b) {
        if (a == end || *a != '.' || *b != '.') same = false;
        else { ++a; ++b; if (a == end || !*b) same = false; }
      }
    }
    if (same && a == end && !*b) return true;
    versions = end;
  }
  return false;
}

void sm_patch_set_compatibility(sm_game_patch_t *patch, const char *version) {
  patch->signature_based = patch->supported && patch->line_count && patch->lines;
  for (size_t i = 0; patch->signature_based && i < patch->line_count; ++i)
    if (!patch->lines[i].pattern_size || patch->lines[i].absolute)
      patch->signature_based = false;
  patch->version_matched = version_matches(patch->app_ver, version, patch->signature_based);
  patch->compatible = patch->version_matched || patch->signature_based;
}

typedef struct {
  XML_Parser parser;
  sm_game_patch_list_t *list;
  const char *title_id, *version, *path;
  size_t first, depth, total_lines, total_bytes, elements;
  bool ids, matched, in_id, in_metadata, in_lines;
  char id[64];
  size_t id_size;
  int error;
} xml_context_t;

static void xml_error(xml_context_t *ctx, int error) {
  ctx->error = error;
  XML_StopParser(ctx->parser, XML_FALSE);
}

static void XMLCALL xml_start(void *user, const XML_Char *name, const XML_Char **a) {
  xml_context_t *ctx = user;
  if (++ctx->depth > 5 || ++ctx->elements > 65536) { xml_error(ctx, EFBIG); return; }
  if (ctx->depth == 1) { if (strcmp(name, "Patch")) xml_error(ctx, EINVAL); return; }
  if (ctx->depth == 2 && !strcmp(name, "TitleID")) { ctx->ids = true; return; }
  if (ctx->depth == 3 && !strcmp(name, "ID") && !ctx->in_metadata) {
    ctx->in_id = true; ctx->id_size = 0; return;
  }
  if (ctx->depth == 2 && !strcmp(name, "Metadata")) {
    if (ctx->list->count == SM_PATCH_MAX_ENTRIES) { xml_error(ctx, EFBIG); return; }
    void *entries = realloc(ctx->list->entries, (ctx->list->count + 1) * sizeof(*ctx->list->entries));
    if (!entries) { xml_error(ctx, ENOMEM); return; }
    ctx->list->entries = entries;
    sm_game_patch_t *p = &ctx->list->entries[ctx->list->count++];
    memset(p, 0, sizeof(*p));
    p->supported = true;
    memcpy(p->format, "XML", 4);
    ctx->in_metadata = true;
    if (!copy_attribute(p->name, sizeof(p->name), a, "Name") || !*p->name ||
        !copy_attribute(p->author, sizeof(p->author), a, "Author") ||
        !copy_attribute(p->note, sizeof(p->note), a, "Note") ||
        !copy_attribute(p->app_ver, sizeof(p->app_ver), a, "AppVer") || !*p->app_ver ||
        !copy_attribute(p->app_elf, sizeof(p->app_elf), a, "AppElf") || !module_name(p->app_elf) ||
        !copy_attribute(p->requires, sizeof(p->requires), a, "NeoRequires")) {
      xml_error(ctx, EINVAL); return;
    }
    p->image_base = UINT64_C(0x400000);
    const char *extension = strrchr(p->app_elf, '.');
    if (extension && (!strcmp(extension, ".prx") || !strcmp(extension, ".sprx")))
      p->image_base = 0;
    if (*attribute(a, "ImageBase") && !number(attribute(a, "ImageBase"), &p->image_base, false)) {
      xml_error(ctx, EINVAL); return;
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    const char *base = strrchr(ctx->path, '/');
    const char *fields[] = {base ? base + 1 : ctx->path, attribute(a, "Title"), p->name, p->app_ver, p->app_elf, attribute(a, "ImageBase")};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
      for (const unsigned char *s = (const unsigned char *)fields[i]; *s; ++s) { hash ^= *s; hash *= UINT64_C(1099511628211); }
      hash ^= 0xff; hash *= UINT64_C(1099511628211);
    }
    snprintf(p->id, sizeof(p->id), "%016llx", (unsigned long long)hash);
    return;
  }
  if (ctx->depth == 3 && ctx->in_metadata && !strcmp(name, "PatchList")) { ctx->in_lines = true; return; }
  if (ctx->depth == 4 && ctx->in_lines && !strcmp(name, "Line")) {
    if (++ctx->total_lines > PATCH_MAX_LINES) { xml_error(ctx, EFBIG); return; }
    sm_game_patch_t *p = &ctx->list->entries[ctx->list->count-1];
    sm_patch_line_t line = {0};
    if (!compile_line(a, &line, p->image_base)) {
      p->supported = false;
      snprintf(p->error, sizeof(p->error), "Invalid or unsupported Line type/value: %.48s", attribute(a, "Type"));
      sm_patch_free_line(&line);
      return;
    }
    ctx->total_bytes += line.size + line.pattern_size * 2 + line.target_size;
    if (ctx->total_bytes > PATCH_MAX_BYTES) { sm_patch_free_line(&line); xml_error(ctx, EFBIG); return; }
    void *lines = realloc(p->lines, (p->line_count + 1) * sizeof(*p->lines));
    if (!lines) { sm_patch_free_line(&line); xml_error(ctx, ENOMEM); return; }
    p->lines = lines;
    p->lines[p->line_count++] = line;
    return;
  }
  xml_error(ctx, EINVAL);
}

static void XMLCALL xml_end(void *user, const XML_Char *name) {
  xml_context_t *ctx = user;
  if (ctx->in_id && !strcmp(name, "ID")) {
    ctx->id[ctx->id_size] = 0;
    if (!strcmp(ctx->id, ctx->title_id)) ctx->matched = true;
    ctx->in_id = false;
  }
  if (ctx->depth == 3 && !strcmp(name, "PatchList")) ctx->in_lines = false;
  if (ctx->depth == 2 && !strcmp(name, "Metadata")) {
    sm_game_patch_t *p = &ctx->list->entries[ctx->list->count-1];
    if (!p->line_count) { p->supported = false; snprintf(p->error, sizeof(p->error), "Empty PatchList"); }
    sm_patch_set_compatibility(p, ctx->version);
    ctx->in_metadata = false;
  }
  --ctx->depth;
}

static void XMLCALL xml_text(void *user, const XML_Char *text, int length) {
  xml_context_t *ctx = user;
  for (int i = 0; i < length; ++i) {
    if (ctx->in_id) {
      if (ctx->id_size + 1 >= sizeof(ctx->id)) { xml_error(ctx, EFBIG); return; }
      ctx->id[ctx->id_size++] = text[i];
    } else if (!isspace((unsigned char)text[i])) { xml_error(ctx, EINVAL); return; }
  }
}

static void XMLCALL xml_doctype(void *user, const XML_Char *name,
                                const XML_Char *system, const XML_Char *public_id, int internal) {
  (void)name; (void)system; (void)public_id; (void)internal;
  xml_error(user, ENOTSUP); // No DTD, entity expansion or external I/O.
}

int sm_patch_xml_load(const char *path, const char *title_id, const char *version,
                      sm_game_patch_list_t *list) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return errno;
  struct stat st;
  if (fstat(fd, &st)) { int error = errno; close(fd); return error; }
  if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > PATCH_MAX_FILE) {
    close(fd); return EFBIG;
  }
  FILE *file = fdopen(fd, "rb");
  if (!file) { int error = errno; close(fd); return error; }
  xml_context_t ctx = {.list = list, .title_id = title_id, .version = version,
                       .path = path, .first = list->count};
  for (size_t i = 0; i < list->count; ++i) {
    ctx.total_lines += list->entries[i].line_count;
    for (size_t j = 0; j < list->entries[i].line_count; ++j) {
      const sm_patch_line_t *line = &list->entries[i].lines[j];
      ctx.total_bytes += line->size * (1 + !!line->off + !!line->expected) + line->pattern_size * 2 + line->target_size;
    }
  }
  ctx.parser = XML_ParserCreate(NULL);
  if (!ctx.parser) { fclose(file); return ENOMEM; }
  XML_SetUserData(ctx.parser, &ctx);
  XML_SetElementHandler(ctx.parser, xml_start, xml_end);
  XML_SetCharacterDataHandler(ctx.parser, xml_text);
  XML_SetStartDoctypeDeclHandler(ctx.parser, xml_doctype);
  XML_SetParamEntityParsing(ctx.parser, XML_PARAM_ENTITY_PARSING_NEVER);
  char buffer[8192];
  size_t total = 0;
  bool done = false;
  while (!done && !ctx.error) {
    size_t size = fread(buffer, 1, sizeof(buffer), file);
    total += size;
    done = size < sizeof(buffer);
    if (ferror(file)) ctx.error = EIO;
    else if (total > PATCH_MAX_FILE) ctx.error = EFBIG;
    else if (XML_Parse(ctx.parser, buffer, (int)size, done) == XML_STATUS_ERROR)
      ctx.error = ctx.error ? ctx.error : EINVAL;
  }
  XML_ParserFree(ctx.parser);
  fclose(file);
  if (ctx.error || (ctx.ids && !ctx.matched)) sm_patch_truncate(list, ctx.first);
  if (!ctx.error) {
    for (size_t i = ctx.first; i < list->count; ++i)
      for (size_t j = 0; j < i; ++j)
        if (!strcmp(list->entries[i].id, list->entries[j].id)) {
          sm_patch_truncate(list, ctx.first);
          return EEXIST;
        }
  }
  return ctx.error;
}

typedef struct {
  uintptr_t address;
  size_t size;
  const uint8_t *value;
  uint8_t *original, *allocated;
} patch_write_t;

struct sm_patch_recovery {
  patch_write_t *writes;
  size_t count, attempted;
  uint8_t *verify;
};

static void free_recovery(sm_patch_recovery_t *recovery) {
  for (size_t i = 0; i < recovery->count; ++i) {
    free(recovery->writes[i].original);
    free(recovery->writes[i].allocated);
  }
  free(recovery->writes);
  free(recovery->verify);
  free(recovery);
}

void sm_patch_forget_recovery(sm_patch_recovery_t **recovery) {
  if (*recovery) free_recovery(*recovery);
  *recovery = NULL;
}

bool sm_patch_recover(sm_patch_recovery_t **recovery, const sm_patch_memory_t *memory) {
  sm_patch_recovery_t *r = *recovery;
  if (!r) return true;
  bool restored = true;
  for (size_t i = r->attempted; i > 0; --i) {
    patch_write_t *w = &r->writes[i-1];
    if (!memory->write(memory->context, w->address, w->original, w->size) ||
        !memory->read(memory->context, w->address, r->verify, w->size) ||
        memcmp(r->verify, w->original, w->size)) restored = false;
  }
  if (restored) { free_recovery(r); *recovery = NULL; }
  return restored;
}

static bool module_range(uintptr_t base, size_t size, uintptr_t address, size_t bytes) {
  return base >= 0x10000 && base < UINT64_C(0x800000000000) &&
         size <= UINT64_C(0x800000000000) - base && bytes &&
         address >= base && address - base <= size && bytes <= size - (address - base);
}

typedef struct {
  uintptr_t base, address, cave;
  size_t size;
} patch_location_t;

typedef struct {
  const uint8_t *bytes, *mask;
  size_t size, anchor;
  uintptr_t *found;
} pattern_request_t;

static int scan_patterns(const sm_patch_memory_t *memory, uintptr_t base, size_t size,
                          pattern_request_t *requests, size_t count) {
  if (size > PATCH_MAX_SCAN) return EFBIG;
  size_t maximum = 0;
  for (size_t i = 0; i < count; ++i) {
    pattern_request_t *r = &requests[i];
    if (!r->size || r->size > size) return ENOENT;
    if (r->size > maximum) maximum = r->size;
    while (r->mask && !r->mask[r->anchor]) ++r->anchor;
  }
  uint8_t *data = malloc(PATCH_SCAN_CHUNK + maximum);
  if (!data) return ENOMEM;
  size_t carry = 0;
  // One remote pass per module, shared by all masks and code-cave targets.
  for (size_t at = 0; at < size;) {
    if (memory->cancelled && memory->cancelled(memory->context)) { free(data); return ETIMEDOUT; }
    size_t bytes = size - at;
    if (bytes > PATCH_SCAN_CHUNK) bytes = PATCH_SCAN_CHUNK;
    bool readable = memory->read(memory->context, base + at, data + carry, bytes);
    if (!readable && bytes > 0x4000) {
      bytes = 0x4000;
      readable = memory->read(memory->context, base + at, data + carry, bytes);
    }
    if (!readable) {
      // Unmapped ELF gaps do not join the surrounding pattern fragments.
      carry = 0;
      at += bytes;
      continue;
    }
    size_t available = carry + bytes;
    for (size_t n = 0; n < count; ++n) {
      if (!(n & 15u) && memory->cancelled && memory->cancelled(memory->context)) {
        free(data); return ETIMEDOUT;
      }
      pattern_request_t *r = &requests[n];
      // A short pattern inside the overlap was already examined last time.
      size_t i = carry >= r->size ? carry - r->size + 1 : 0;
      while (i + r->size <= available) {
        const uint8_t *candidate = memchr(data + i + r->anchor, r->bytes[r->anchor],
                                          available - r->size - i + 1);
        if (!candidate) break;
        i = (size_t)(candidate - data) - r->anchor;
        size_t j = 0;
        while (j < r->size && ((r->mask && !r->mask[j]) || data[i+j] == r->bytes[j])) ++j;
        if (j == r->size) {
          if (*r->found) { free(data); return EEXIST; }
          *r->found = base + at - carry + i;
        }
        ++i;
      }
    }
    carry = maximum - 1;
    if (carry > available) carry = available;
    memmove(data, data + available - carry, carry);
    at += bytes;
  }
  free(data);
  for (size_t i = 0; i < count; ++i) if (!*requests[i].found) return ENOENT;
  return 0;
}

static int resolve_locations(const sm_game_patch_t *patch, const sm_patch_memory_t *memory,
                              patch_location_t *locations) {
  for (size_t i = 0; i < patch->line_count; ++i) {
    const sm_patch_line_t *line = &patch->lines[i];
    patch_location_t *location = &locations[i];
    const char *name = *line->module ? line->module : patch->app_elf;
    if (line->absolute) {
      location->base = 0x10000;
      location->size = UINT64_C(0x800000000000) - location->base;
    } else if (!memory->module(memory->context, name, &location->base, &location->size)) return EAGAIN;
    if (!module_range(location->base, location->size, location->base, 1)) return EFAULT;
  }
  pattern_request_t *requests = calloc(patch->line_count * 2, sizeof(*requests));
  if (!requests) return ENOMEM;
  int error = 0;
  for (size_t i = 0; i < patch->line_count && !error; ++i) {
    bool scanned = false;
    for (size_t j = 0; j < i; ++j)
      if (locations[j].base == locations[i].base && locations[j].size == locations[i].size) scanned = true;
    if (scanned) continue;
    size_t count = 0;
    for (size_t j = i; j < patch->line_count; ++j) {
      if (locations[j].base != locations[i].base || locations[j].size != locations[i].size) continue;
      const sm_patch_line_t *line = &patch->lines[j];
      if (line->pattern_size)
        requests[count++] = (pattern_request_t){line->pattern, line->mask, line->pattern_size, 0, &locations[j].address};
      if (line->target_size)
        requests[count++] = (pattern_request_t){line->target, NULL, line->target_size, 0, &locations[j].cave};
    }
    if (count) error = scan_patterns(memory, locations[i].base, locations[i].size, requests, count);
  }
  free(requests);
  return error;
}

static bool relative_jump(uint8_t *bytes, uintptr_t source, uintptr_t target) {
  int64_t delta = (int64_t)target - (int64_t)source - 5;
  if (delta < INT32_MIN || delta > INT32_MAX) return false;
  bytes[0] = 0xe9;
  uint32_t bits = (uint32_t)(int32_t)delta;
  for (size_t i = 0; i < 4; ++i) bytes[i+1] = (uint8_t)(bits >> (i*8));
  return true;
}

int sm_patch_apply(const sm_game_patch_t *patch, const sm_patch_memory_t *memory,
                   sm_patch_recovery_t **recovery) {
  *recovery = NULL;
  if (!patch->supported || !patch->compatible || !patch->line_count) return EINVAL;
  sm_patch_recovery_t *r = calloc(1, sizeof(*r));
  if (!r) return ENOMEM;
  size_t capacity = patch->line_count * 2;
  patch_write_t *writes = calloc(capacity, sizeof(*writes));
  if (!writes) { free(r); return ENOMEM; }
  r->writes = writes;
  size_t count = 0, attempted = 0;
  patch_location_t *locations = calloc(patch->line_count, sizeof(*locations));
  int error = locations ? resolve_locations(patch, memory, locations) : ENOMEM;
  for (size_t i = 0; i < patch->line_count && !error; ++i) {
    if (memory->cancelled && memory->cancelled(memory->context)) { error = ETIMEDOUT; break; }
    const sm_patch_line_t *line = &patch->lines[i];
    uintptr_t base = locations[i].base, address = locations[i].address;
    size_t size = locations[i].size;
    if (line->pattern_size) {
      if ((line->offset < 0 && (uint64_t)(0u - (uint64_t)line->offset) > address - base) ||
          (line->offset >= 0 && (uint64_t)line->offset > size - (address - base))) { error = EFAULT; break; }
      address = (uintptr_t)((uint64_t)address + (uint64_t)line->offset);
    } else {
      uint64_t offset = line->address - line->image_base;
      if (!line->absolute && offset > size) { error = EFAULT; break; }
      address = line->absolute ? offset : base + offset;
    }
    if (!line->jump_size) {
      writes[count++] = (patch_write_t){.address = address, .size = line->size, .value = line->bytes};
    } else {
      uintptr_t cave = locations[i].cave;
      uint8_t *payload = malloc(line->size + 5), *jump = malloc(line->jump_size);
      if (!payload || !jump) { free(payload); free(jump); error = ENOMEM; break; }
      memcpy(payload, line->bytes, line->size);
      memset(jump, 0x90, line->jump_size);
      if (!relative_jump(payload + line->size, cave + line->size, address + line->jump_size) ||
          !relative_jump(jump, address, cave)) { free(payload); free(jump); error = ERANGE; break; }
      writes[count++] = (patch_write_t){.address = cave, .size = line->size + 5, .value = payload, .allocated = payload};
      writes[count++] = (patch_write_t){.address = address, .size = line->jump_size, .value = jump, .allocated = jump};
    }
    for (size_t n = count - (line->jump_size ? 2 : 1); n < count; ++n) {
      patch_write_t *w = &writes[n];
      if (!module_range(base, size, w->address, w->size)) { error = EFAULT; break; }
      w->original = malloc(w->size);
      if (!w->original) { error = ENOMEM; break; }
      if (!memory->read(memory->context, w->address, w->original, w->size)) { error = EFAULT; break; }
      if (line->expected && memcmp(w->original, w->value, w->size) &&
          memcmp(w->original, line->expected, w->size)) { error = ESTALE; break; }
      for (size_t j = 0; j < n; ++j) {
        patch_write_t *previous = &writes[j];
        if (previous->address < w->address + w->size && w->address < previous->address + previous->size) {
          uintptr_t start = previous->address > w->address ? previous->address : w->address;
          uintptr_t end = previous->address + previous->size < w->address + w->size
                            ? previous->address + previous->size : w->address + w->size;
          if (memcmp(previous->value + (start - previous->address),
                      w->value + (start - w->address), end - start)) { error = EEXIST; break; }
        }
      }
    }
  }
  free(locations);
  uint8_t *verify = NULL;
  if (!error) {
    verify = malloc(PATCH_MAX_VALUE + 5);
    if (!verify) error = ENOMEM;
  }
  for (size_t i = 0; i < count && !error; ++i) {
    if (memory->cancelled && memory->cancelled(memory->context)) { error = ETIMEDOUT; break; }
    patch_write_t *w = &writes[i];
    if (!memcmp(w->original, w->value, w->size)) continue;
    attempted = i + 1; // A reported write failure can still have changed bytes.
    if (!memory->write(memory->context, w->address, w->value, w->size) ||
        !memory->read(memory->context, w->address, verify, w->size) || memcmp(verify, w->value, w->size))
      error = EIO;
  }
  if (!error && memory->cancelled && memory->cancelled(memory->context)) error = ETIMEDOUT;
  r->count = count;
  r->attempted = error ? attempted : 0;
  r->verify = verify;
  *recovery = r;
  return sm_patch_recover(recovery, memory) ? error : ECANCELED;
}

bool sm_patch_selection_ready(const sm_game_patch_list_t *list,
                               const sm_patch_memory_t *memory) {
  for (size_t i = 0; i < list->count; ++i) {
    const sm_game_patch_t *p = &list->entries[i];
    if (!p->enabled) continue;
    for (size_t j = 0; j < p->line_count; ++j) {
      const char *name = *p->lines[j].module ? p->lines[j].module : p->app_elf;
      uintptr_t base;
      size_t size;
      if (!p->lines[j].absolute && !memory->module(memory->context, name, &base, &size)) return false;
    }
  }
  return true;
}

int sm_patch_apply_selected(const sm_game_patch_list_t *list,
                            const sm_patch_memory_t *memory,
                            sm_patch_recovery_t **recovery) {
  *recovery = NULL;
  if (!sm_patch_selection_ready(list, memory)) return EAGAIN;
  sm_game_patch_t combined = {.supported = true, .compatible = true};
  for (size_t i = 0; i < list->count; ++i) {
    const sm_game_patch_t *p = &list->entries[i];
    if (!p->enabled) continue;
    if (!p->compatible || !p->supported || p->line_count > PATCH_MAX_LINES - combined.line_count) return EINVAL;
    combined.line_count += p->line_count;
  }
  if (!combined.line_count) return 0;
  combined.lines = malloc(combined.line_count * sizeof(*combined.lines));
  if (!combined.lines) return ENOMEM;
  size_t count = 0;
  for (size_t i = 0; i < list->count; ++i) {
    const sm_game_patch_t *p = &list->entries[i];
    if (!p->enabled) continue;
    for (size_t j = 0; j < p->line_count; ++j) {
      combined.lines[count] = p->lines[j];
      if (!*combined.lines[count].module)
        memcpy(combined.lines[count].module, p->app_elf, strlen(p->app_elf) + 1);
      ++count;
    }
  }
  int error = sm_patch_apply(&combined, memory, recovery);
  free(combined.lines);
  return error;
}

int sm_cheat_append_line(sm_game_patch_t *patch, sm_patch_line_t *line) {
  for (size_t i = 0; i < patch->line_count;) {
    sm_patch_line_t *old = &patch->lines[i];
    if (old->absolute != line->absolute || strcmp(old->module, line->module) ||
        old->address + old->size <= line->address || line->address + line->size <= old->address) { ++i; continue; }
    uint64_t start = old->address < line->address ? old->address : line->address;
    uint64_t end = old->address + old->size > line->address + line->size ? old->address + old->size : line->address + line->size;
    if (end - start > PATCH_MAX_VALUE || (!!old->expected != !!line->expected)) return EINVAL;
    size_t size = (size_t)(end - start);
    uint8_t *on = malloc(size), *off = malloc(size), *expected = old->expected ? malloc(size) : NULL;
    if (!on || !off || (old->expected && !expected)) { free(on); free(off); free(expected); return ENOMEM; }
    memcpy(on + old->address - start, old->bytes, old->size);
    memcpy(on + line->address - start, line->bytes, line->size);
    memcpy(off + line->address - start, line->off, line->size);
    memcpy(off + old->address - start, old->off, old->size);
    if (expected) {
      memcpy(expected + line->address - start, line->expected, line->size);
      memcpy(expected + old->address - start, old->expected, old->size);
    }
    sm_patch_free_line(old);
    memmove(old, old + 1, (patch->line_count - i - 1) * sizeof(*old));
    --patch->line_count;
    sm_patch_free_line(line);
    line->bytes = on; line->off = off; line->expected = expected;
    line->address = start; line->size = size;
    i = 0;
  }
  if (patch->line_count >= PATCH_MAX_LINES) return EFBIG;
  void *lines = realloc(patch->lines, (patch->line_count + 1) * sizeof(*patch->lines));
  if (!lines) return ENOMEM;
  patch->lines = lines;
  patch->lines[patch->line_count++] = *line;
  memset(line, 0, sizeof(*line));
  return 0;
}

int sm_patch_dependency_chain(const sm_game_patch_list_t *list, size_t index,
                               size_t chain[SM_PATCH_MAX_ENTRIES], size_t *count) {
  *count = 0;
  if (list->count > SM_PATCH_MAX_ENTRIES || index >= list->count) return EINVAL;
  for (;;) {
    for (size_t i = 0; i < *count; ++i)
      if (chain[i] == index) return ELOOP;
    const sm_game_patch_t *p = &list->entries[index];
    if (!p->supported || !p->compatible) return EINVAL;
    chain[(*count)++] = index;
    if (!*p->master_id) return 0;
    size_t next = 0;
    while (next < list->count && strcmp(list->entries[next].id, p->master_id)) ++next;
    if (next == list->count) return ENOENT;
    index = next;
  }
}

int sm_cheat_apply(sm_game_patch_list_t *list, size_t index, bool enabled,
                    const sm_patch_memory_t *memory, sm_patch_recovery_t **recovery) {
  *recovery = NULL;
  size_t chain[SM_PATCH_MAX_ENTRIES], count = 0;
  int error = sm_patch_dependency_chain(list, index, chain, &count);
  if (error) return error;
  const sm_game_patch_t *p = &list->entries[index];
  bool dependency[SM_PATCH_MAX_ENTRIES] = {0};
  for (size_t i = 0; i < count; ++i) {
    if (!strcmp(list->entries[chain[i]].format, "XML")) return EINVAL;
    dependency[chain[i]] = true;
  }
  for (size_t i = 0; i < list->count; ++i) {
    const sm_game_patch_t *other = &list->entries[i];
    if ((!enabled || p->button) && other->applied && !strcmp(other->master_id, p->id)) return EBUSY;
  }
  sm_game_patch_t combined = {.supported = true, .compatible = true};
  for (size_t part = enabled ? count : 1; part > 0 && !error; --part) {
    const sm_game_patch_t *source = &list->entries[chain[part - 1]];
    if (part > 1 && source->applied) continue;
    for (size_t j = 0; j < source->line_count && !error; ++j) {
      const sm_patch_line_t *original = &source->lines[j];
      if (!original->off || original->pattern || original->mask || original->target || original->pattern_size || original->target_size) {
        error = EINVAL;
        break;
      }
      sm_patch_line_t line = *original;
      if (!*line.module) memcpy(line.module, source->app_elf, strlen(source->app_elf) + 1);
      line.bytes = malloc(line.size); line.off = malloc(line.size); line.expected = NULL;
      if (!line.bytes || !line.off) error = ENOMEM;
      else {
        memcpy(line.bytes, enabled ? original->bytes : original->off, line.size);
        memcpy(line.off, original->off, line.size);
        if (enabled && original->expected) {
          line.expected = malloc(line.size);
          if (!line.expected) error = ENOMEM;
          else memcpy(line.expected, original->expected, line.size);
        }
      }
      if (!error) error = sm_cheat_append_line(&combined, &line);
      sm_patch_free_line(&line);
    }
  }
  // Refuse overlap with another live selection rather than silently undoing it.
  for (size_t i = 0; i < list->count && !error; ++i) {
    const sm_game_patch_t *other = &list->entries[i];
    if (!other->applied || dependency[i]) continue;
    for (size_t a = 0; a < combined.line_count && !error; ++a) {
      const sm_patch_line_t *x = &combined.lines[a];
      uintptr_t xb = 0; size_t xs;
      if (!x->absolute && !memory->module(memory->context, x->module, &xb, &xs)) { error = EAGAIN; break; }
      uint64_t xa = xb + x->address;
      for (size_t b = 0; b < other->line_count; ++b) {
        const sm_patch_line_t *y = &other->lines[b];
        uintptr_t yb = 0; size_t ys;
        if (y->pattern_size) { error = EBUSY; break; }
        if (!y->absolute && !memory->module(memory->context, *y->module ? y->module : other->app_elf, &yb, &ys)) { error = EAGAIN; break; }
        uint64_t ya = yb + y->address - y->image_base;
        if (xa < ya + y->size && ya < xa + x->size) { error = EEXIST; break; }
      }
    }
  }
  if (!error) error = sm_patch_apply(&combined, memory, recovery);
  if (!error) {
    if (enabled) {
      for (size_t i = 0; i < count; ++i) list->entries[chain[i]].applied = true;
    } else list->entries[index].applied = false;
  }
  for (size_t i = 0; i < combined.line_count; ++i) sm_patch_free_line(&combined.lines[i]);
  free(combined.lines);
  return error;
}
