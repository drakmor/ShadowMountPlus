#include "sm_platform.h"
#include "sm_config_file.h"
#include "sm_log.h"
#include "sm_paths.h"
#include "sm_smb.h"
#include <ctype.h>
#include <json-c/json.h>

#define INI_MAX_BYTES (1024u * 1024u)
#define INI_MAX_LINES 8192u
#define INI_LINE_MAX (MAX_PATH * 2u + 2048u)

const sm_config_key_t sm_config_keys[] = {
  {"general", "debug", "debug"}, {"general", "quiet_mode", "quiet_mode"},
  {"general", "library_sync_notification", "library_sync_notification"},
  {"general", "language", "language"},
  {"api", "enabled", "api_enabled"}, {"api", "bind_address", "api_bind_address"},
  {"api", "port", "api_port"},
  {"scan", "depth", "scan_depth"}, {"scan", "path", "scanpath"},
  {"scan", "interval_seconds", "scan_interval_seconds"},
  {"scan", "stability_wait_seconds", "stability_wait_seconds"},
  {"mount", "mode", "mount_read_only"}, {"mount", "force", "force_mount"},
  {"mount", "persistent_images", "persistent_image_mounts"},
  {"mount", "exfat_backend", "exfat_backend"}, {"mount", "ufs_backend", "ufs_backend"},
  {"mount", "nested_pfs_index_cache", "nested_pfs_index_cache"},
  {"library", "install_all", "app_install_all"},
  {"library", "remove_missing", "auto_remove_missing_games"},
  {"library", "remove_with_dlc", "auto_remove_games_with_dlc"},
  {"library", "remove_delay_seconds", "auto_remove_missing_delay_seconds"},
  {"fakelib", "enabled", "backport_fakelib"},
  {"fakelib", "default_mode", "fakelib_default_mode"},
  {"fakelib", "global_enabled", "global_fakelib"},
  {"fakelib", "global_path", "global_fakelib_path"},
  {"fakelib", "global_priority", "global_fakelib_priority"},
  {"emulators", "update_enabled", "update_emulators"},
  {"emulators", "path", "emulators_path"},
  {"emulators", "auto_update_ampr", "auto_update_ampr"},
  {"emulators", "ampr_update_url", "ampr_update_url"},
  {"emulators", "ampr_ng_update_url", "ampr_ng_update_url"},
  {"fan", "target_temperature", "fan_target_temperature"},
  {"sectors", "lvd_exfat", "lvd_exfat_sector_size"},
  {"sectors", "lvd_ufs", "lvd_ufs_sector_size"},
  {"sectors", "lvd_pfs", "lvd_pfs_sector_size"},
  {"sectors", "md_exfat", "md_exfat_sector_size"},
  {"sectors", "md_ufs", "md_ufs_sector_size"},
};
const size_t sm_config_key_count = sizeof(sm_config_keys) / sizeof(sm_config_keys[0]);

const sm_config_key_t *sm_config_find_legacy(const char *key) {
  static const char *const aliases[][2] = {
    {"lang", "language"}, {"locale", "language"},
    {"api_bind_adress", "api_bind_address"}, {"read_only", "mount_read_only"},
    {"global_fakelib_enabled", "global_fakelib"},
    {"legacy_gddr5_cache", "nested_pfs_index_cache"},
    {"scan_interval_sec", "scan_interval_seconds"},
    {"stability_wait_sec", "stability_wait_seconds"},
    {"auto_remove_missing_delay_sec", "auto_remove_missing_delay_seconds"},
  };
  for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i)
    if (!strcasecmp(key, aliases[i][0])) { key = aliases[i][1]; break; }
  for (size_t i = 0; i < sm_config_key_count; ++i)
    if (!strcasecmp(key, sm_config_keys[i].legacy)) return &sm_config_keys[i];
  return NULL;
}

static char *trim(char *s) {
  while (isspace((unsigned char)*s)) ++s;
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
  return s;
}

static void free_line(sm_ini_line_t *line) {
  if (line->raw) memset(line->raw, 0, strlen(line->raw));
  free(line->raw); free(line->section); free(line->key);
  if (line->value) memset(line->value, 0, strlen(line->value));
  free(line->value); free(line->comment);
  memset(line, 0, sizeof(*line));
}

void sm_ini_free(sm_ini_t *doc) {
  for (size_t i = 0; i < doc->count; ++i) free_line(&doc->lines[i]);
  free(doc->lines);
  memset(doc, 0, sizeof(*doc));
}

static bool insert_line(sm_ini_t *doc, size_t at, sm_ini_line_t *line) {
  if (doc->count >= INI_MAX_LINES) { errno = EFBIG; return false; }
  if (doc->count == doc->capacity) {
    size_t cap = doc->capacity ? doc->capacity * 2 : 32;
    void *next = realloc(doc->lines, cap * sizeof(*doc->lines));
    if (!next) return false;
    doc->lines = next; doc->capacity = cap;
  }
  memmove(doc->lines + at + 1, doc->lines + at, (doc->count - at) * sizeof(*doc->lines));
  doc->lines[at] = *line; ++doc->count;
  memset(line, 0, sizeof(*line));
  return true;
}

static bool decode_value(char *s, char **comment) {
  *comment = NULL;
  if (*s == '"') {
    char *out = s, *p = s + 1;
    for (; *p && *p != '"'; ++p) {
      if (*p == '\\') {
        ++p;
        if (*p != '\\' && *p != '"') return false;
      }
      if ((unsigned char)*p < 32) return false;
      *out++ = *p;
    }
    if (*p != '"') return false;
    char *tail = trim(p + 1);
    if (*tail && *tail != '#' && *tail != ';') return false;
    if (*tail) *comment = tail;
    *out = 0;
    return true;
  }
  char *p = strpbrk(s, "#;");
  if (p) { *comment = p; }
  return true;
}

bool sm_ini_read(const char *path, sm_ini_t *doc, bool legacy) {
  memset(doc, 0, sizeof(*doc));
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return errno == ENOENT;
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > INI_MAX_BYTES) {
    close(fd); errno = EINVAL; return false;
  }
  FILE *f = fdopen(fd, "r");
  if (!f) { close(fd); return false; }
  char raw[INI_LINE_MAX], section[MAX_PATH + 32] = "";
  size_t bytes = 0, number = 0;
  bool ok = true;
  while (ok) {
    size_t length = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF) {
      if (!ch || length + 1 >= sizeof(raw) || ++bytes > INI_MAX_BYTES) {
        errno = ch ? EFBIG : EINVAL; ok = false; break;
      }
      raw[length++] = (char)ch;
      if (ch == '\n') break;
    }
    if (!ok || !length) break;
    raw[length] = 0;
    ++number;
    errno = 0;
    sm_ini_line_t line = {0};
    line.raw = strdup(raw);
    char *s = trim(raw), *comment = NULL;
    if (*s == '[') {
      char *end = strchr(s, ']');
      while (end) {
        char *tail = end + 1;
        while (isspace((unsigned char)*tail)) ++tail;
        if (!*tail || *tail == '#' || *tail == ';') break;
        end = strchr(end + 1, ']');
      }
      if (!end) ok = false;
      else {
        char *tail = trim(end + 1);
        if (*tail && !(line.comment = strdup(tail))) ok = false;
        *end = 0;
        if (strlcpy(section, trim(s + 1), sizeof(section)) >= sizeof(section) || !*section) ok = false;
        line.header = true;
      }
    } else if (*s && *s != '#' && *s != ';') {
      char *eq = strchr(s, '=');
      if (!eq) ok = false;
      else {
        *eq = 0; char *key = trim(s), *value = trim(eq + 1);
        bool quoted = !legacy && *value == '"';
        ok = *key && (legacy || decode_value(value, &comment));
        if (legacy) comment = strpbrk(value, "#;");
        if (comment) {
          line.comment = strdup(comment);
          *comment = 0;
        }
        line.key = strdup(key);
        line.value = strdup(quoted ? value : trim(value));
        if (!line.key || !line.value || (comment && !line.comment)) ok = false;
      }
    }
    line.section = strdup(section);
    if (!line.raw || !line.section) ok = false;
    if (!ok && legacy && errno != ENOMEM && !line.key && line.raw && line.section) {
      // Old readers ignored malformed lines. Preserve them in the backup.
      free_line(&line); ok = true; continue;
    }
    if (ok) ok = insert_line(doc, doc->count, &line);
    free_line(&line);
  }
  int error = ok ? 0 : (errno ? errno : EINVAL);
  if (ferror(f) && !error) error = errno ? errno : EIO;
  if (fclose(f) && !error) error = errno;
  if (error) {
    sm_ini_free(doc);
    log_debug("  [CFG] cannot read %s near line %zu (status=%d)", path, number, error);
    errno = error; return false;
  }
  return true;
}

const char *sm_ini_get(const sm_ini_t *doc, const char *section, const char *key) {
  for (size_t i = doc->count; i; --i) {
    const sm_ini_line_t *line = &doc->lines[i - 1];
    if (line->key && !strcasecmp(section, line->section) && !strcasecmp(key, line->key)) return line->value;
  }
  return NULL;
}

bool sm_ini_version(const sm_ini_t *doc) {
  const char *v = sm_ini_get(doc, "config", "version");
  if (v && !strcmp(v, "2")) return true;
  errno = EPROTONOSUPPORT; return false;
}

static bool valid_text(const char *text) {
  for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
    if (*p < 32 || *p == 127) { errno = EINVAL; return false; }
  return true;
}

bool sm_ini_append(sm_ini_t *doc, const char *section, const char *key, const char *value) {
  if (!*section || !*key || !valid_text(section) || !valid_text(key) || strchr(key, '=') || !valid_text(value)) {
    errno = EINVAL; return false;
  }
  size_t at = doc->count; bool found = false;
  for (size_t i = 0; i < doc->count; ++i) {
    if (!strcasecmp(doc->lines[i].section, section)) { at = i + 1; found = true; }
  }
  if (!found) {
    sm_ini_line_t header = {.header = true, .section = strdup(section)};
    if (!header.section || !insert_line(doc, doc->count, &header)) { free_line(&header); return false; }
    at = doc->count;
  }
  sm_ini_line_t line = {.section = strdup(section), .key = strdup(key), .value = strdup(value)};
  bool ok = line.section && line.key && line.value && insert_line(doc, at, &line);
  free_line(&line); return ok;
}

bool sm_ini_set(sm_ini_t *doc, const char *section, const char *key, const char *value) {
  if (!*section || !*key || !valid_text(section) || !valid_text(key) ||
      strchr(key, '=') || (value && !valid_text(value))) { errno = EINVAL; return false; }
  bool set = false;
  for (size_t i = 0; i < doc->count; ++i) {
    sm_ini_line_t *line = &doc->lines[i];
    if (!line->key || strcasecmp(section, line->section) || strcasecmp(key, line->key)) continue;
    if (value && !set) {
      char *copy = strdup(value); if (!copy) return false;
      free(line->raw); line->raw = NULL;
      memset(line->value, 0, strlen(line->value)); free(line->value); line->value = copy;
      set = true;
    } else {
      // Keep comments when retiring duplicate keys or resetting a setting.
      free(line->raw); line->raw = line->comment; line->comment = NULL;
      free(line->key); line->key = NULL;
      memset(line->value, 0, strlen(line->value)); free(line->value); line->value = NULL;
    }
  }
  return !value || set || sm_ini_append(doc, section, key, value);
}

bool sm_ini_rename_section(sm_ini_t *doc, const char *old, const char *name) {
  if (!*name || !valid_text(name)) { errno = EINVAL; return false; }
  for (size_t i = 0; i < doc->count; ++i) {
    sm_ini_line_t *line = &doc->lines[i];
    if (strcasecmp(line->section, old)) continue;
    char *copy = strdup(name);
    if (!copy) return false;
    free(line->section); line->section = copy;
    if (line->header) { free(line->raw); line->raw = NULL; }
  }
  return true;
}

bool sm_ini_remove_section(sm_ini_t *doc, const char *section) {
  for (size_t i = 0; i < doc->count;) {
    if (strcasecmp(section, doc->lines[i].section)) { ++i; continue; }
    free_line(&doc->lines[i]);
    memmove(doc->lines + i, doc->lines + i + 1, (--doc->count - i) * sizeof(*doc->lines));
  }
  return true;
}

static bool write_value(FILE *f, const char *value) {
  // Quote strings only when needed; quoted strings escape backslash and quote.
  if (!valid_text(value)) return false;
  size_t n = strlen(value);
  bool quote = !n || isspace((unsigned char)value[0]) || isspace((unsigned char)value[n - 1]) || strpbrk(value, "#;\"\\");
  if (!quote) return fputs(value, f) >= 0;
  if (fputc('"', f) == EOF) return false;
  for (const char *p = value; *p; ++p) {
    if ((unsigned char)*p < 32 || (unsigned char)*p == 127) { errno = EINVAL; return false; }
    if ((*p == '"' || *p == '\\') && fputc('\\', f) == EOF) return false;
    if (fputc(*p, f) == EOF) return false;
  }
  return fputc('"', f) != EOF;
}

static bool sync_parent(const char *path) {
  char parent[MAX_PATH];
  if (strlcpy(parent, path, sizeof(parent)) >= sizeof(parent)) { errno = ENAMETOOLONG; return false; }
  char *slash = strrchr(parent, '/'); if (!slash) { errno = EINVAL; return false; }
  *slash = 0;
  int fd = open(*parent ? parent : "/", O_RDONLY | O_DIRECTORY);
  if (fd < 0) return false;
  int rc = fsync(fd), error = errno; close(fd); errno = error; return rc == 0;
}

bool sm_ini_write(const char *path, const sm_ini_t *doc) {
  char temp[MAX_PATH];
  int n = snprintf(temp, sizeof(temp), "%s.XXXXXX", path);
  if (n < 0 || (size_t)n >= sizeof(temp)) { errno = ENAMETOOLONG; return false; }
  int fd = mkstemp(temp); if (fd < 0) return false;
  FILE *f = fdopen(fd, "w");
  if (!f) { int e = errno; close(fd); unlink(temp); errno = e; return false; }
  bool ok = fchmod(fd, 0600) == 0;
  for (size_t i = 0; ok && i < doc->count; ++i) {
    const sm_ini_line_t *line = &doc->lines[i];
    if (line->raw) {
      ok = fputs(line->raw, f) >= 0;
      size_t len = strlen(line->raw);
      if (ok && (!len || line->raw[len - 1] != '\n')) ok = fputc('\n', f) != EOF;
    } else if (line->header) {
      ok = fprintf(f, "\n[%s]", line->section) >= 0;
      if (ok && line->comment) ok = fprintf(f, " %s", line->comment) >= 0;
      if (ok) ok = fputc('\n', f) != EOF;
    } else if (line->key) {
      ok = fprintf(f, "%s = ", line->key) >= 0 && write_value(f, line->value);
      if (ok && line->comment) ok = fprintf(f, " %s", line->comment) >= 0;
      if (ok) ok = fputc('\n', f) != EOF;
    }
  }
  if (ok) {
    off_t size = ftello(f);
    if (size < 0) ok = false;
    else if (size > INI_MAX_BYTES) { errno = EFBIG; ok = false; }
  }
  if (ok) ok = fflush(f) == 0 && fsync(fd) == 0;
  int error = ok ? 0 : (errno ? errno : EIO);
  if (fclose(f) && !error) error = errno;
  if (!error && rename(temp, path)) error = errno;
  if (!error && !sync_parent(path)) error = errno;
  if (error) unlink(temp);
  errno = error; return !error;
}

static bool read_bytes(const char *path, char **data, size_t *size) {
  *data = NULL; *size = 0;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return errno == ENOENT;
  struct stat st;
  bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 0 && st.st_size <= INI_MAX_BYTES;
  if (!ok) { close(fd); errno = EINVAL; return false; }
  *size = (size_t)st.st_size;
  *data = calloc(1, *size + 1);
  if (!*data) { close(fd); return false; }
  size_t done = 0;
  while (done < *size) {
    ssize_t n = read(fd, *data + done, *size - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { ok = false; break; }
    done += (size_t)n;
  }
  int error = ok ? 0 : (errno ? errno : EIO);
  if (close(fd) && !error) error = errno;
  if (error) { free(*data); *data = NULL; *size = 0; }
  errno = error; return !error;
}

static bool copy_backup(const char *source, const char *target) {
  char *data; size_t size;
  if (!read_bytes(source, &data, &size)) return false;
  char temp[MAX_PATH];
  snprintf(temp, sizeof(temp), "%s.XXXXXX", target);
  int fd = mkstemp(temp), error = fd < 0 ? errno : 0;
  size_t done = 0;
  while (!error && done < size) {
    ssize_t n = write(fd, data + done, size - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) error = n < 0 ? errno : EIO;
    else done += (size_t)n;
  }
  if (!error && fsync(fd)) error = errno;
  if (fd >= 0 && close(fd) && !error) error = errno;
  if (!error && rename(temp, target)) error = errno;
  if (!error && !sync_parent(target)) error = errno;
  if (error && fd >= 0) unlink(temp);
  if (data) memset(data, 0, size);
  free(data); errno = error; return !error;
}

static bool same_file(const char *a, const char *b) {
  char *x = NULL, *y = NULL; size_t nx = 0, ny = 0;
  bool ok = read_bytes(a, &x, &nx) && read_bytes(b, &y, &ny) && nx == ny && (!nx || !memcmp(x, y, nx));
  if (x) memset(x, 0, nx);
  if (y) memset(y, 0, ny);
  free(x); free(y); return ok;
}

static bool import_smb(sm_ini_t *config, const char *path) {
  char *data; size_t size;
  if (!read_bytes(path, &data, &size)) return false;
  if (!size) { free(data); return true; }
  struct json_tokener *tok = json_tokener_new();
  if (!tok) { free(data); errno = ENOMEM; return false; }
  json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
  struct json_object *array = json_tokener_parse_ex(tok, data, (int)size);
  size_t end = json_tokener_get_parse_end(tok);
  while (end < size && isspace((unsigned char)data[end])) ++end;
  bool ok = json_tokener_get_error(tok) == json_tokener_success && end == size &&
      array && json_object_is_type(array, json_type_array) && json_object_array_length(array) <= SM_SMB_CONNECTIONS_MAX;
  json_tokener_free(tok); memset(data, 0, size); free(data);
  for (size_t i = 0; ok && i < json_object_array_length(array); ++i) {
    struct json_object *item = json_object_array_get_idx(array, i), *v;
    sm_smb_connection_t c = {0};
    ok = json_object_is_type(item, json_type_object);
    const char *keys[] = {"name", "path", "username", "password"};
    char *values[] = {c.name, c.path, c.username, c.password};
    size_t sizes[] = {sizeof(c.name), sizeof(c.path), sizeof(c.username), sizeof(c.password)};
    for (size_t j = 0; ok && j < 4; ++j) {
      ok = json_object_object_get_ex(item, keys[j], &v) && json_object_is_type(v, json_type_string) &&
          strlen(json_object_get_string(v)) == (size_t)json_object_get_string_len(v) &&
          strlcpy(values[j], json_object_get_string(v), sizes[j]) < sizes[j];
    }
    if (ok) ok = json_object_object_get_ex(item, "active", &v) && json_object_is_type(v, json_type_boolean);
    if (ok) c.active = json_object_get_boolean(v);
    if (ok && json_object_object_get_ex(item, "read_only", &v)) {
      ok = json_object_is_type(v, json_type_boolean);
      c.read_only = json_object_get_boolean(v);
    }
    if (ok) ok = sm_smb_validate(&c) == 0;
    char section[80]; snprintf(section, sizeof(section), "smb:%s", c.name);
    if (ok && sm_ini_get(config, section, "path")) ok = false;
    for (size_t j = 1; ok && j < 4; ++j) ok = sm_ini_set(config, section, keys[j], values[j]);
    if (ok) ok = sm_ini_set(config, section, "active", c.active ? "on" : "off") &&
                 sm_ini_set(config, section, "read_only", c.read_only ? "on" : "off");
    memset(&c, 0, sizeof(c));
  }
  if (array) json_object_put(array);
  if (!ok) errno = EINVAL;
  return ok;
}

static bool import_rules(const sm_ini_t *old, sm_ini_t *config, sm_ini_t *profiles, bool autotune) {
  bool recursive = false;
  for (size_t i = 0; i < old->count; ++i) {
    const sm_ini_line_t *line = &old->lines[i];
    if (!line->key) continue;
    const char *key = line->key, *value = line->value;
    if (!*value) continue;
    const sm_config_key_t *mapping = sm_config_find_legacy(key);
    if (mapping && !autotune) {
      bool ok = !strcmp(mapping->legacy, "scanpath")
          ? sm_ini_append(config, mapping->section, mapping->key, value)
          : sm_ini_set(config, mapping->section, mapping->key, value);
      if (!ok) return false;
      continue;
    }
    if (!autotune && !strcasecmp(key, "recursive_scan")) {
      bool enabled = !strcasecmp(value, "on") || !strcasecmp(value, "true") ||
          !strcasecmp(value, "yes") || !strcasecmp(value, "ro") || !strcmp(value, "1");
      if (!enabled && strcasecmp(value, "off") && strcasecmp(value, "false") &&
          strcasecmp(value, "no") && strcasecmp(value, "rw") && strcmp(value, "0")) {
        errno = EINVAL; return false;
      }
      recursive |= enabled;
      continue;
    }
    char section[MAX_PATH + 32], local[MAX_PATH + 32];
    if (strlcpy(local, value, sizeof(local)) >= sizeof(local)) { errno = EFBIG; return false; }
    const char *setting = NULL, *setting_value = NULL, *prefix = NULL;
    if (!strcasecmp(key, "image_sector")) {
      char *colon = strrchr(local, ':');
      if (!colon) { errno = EINVAL; return false; }
      *colon = 0; prefix = "image"; setting = "sector_size"; setting_value = colon + 1;
    } else if (!autotune && (!strcasecmp(key, "image_ro") || !strcasecmp(key, "image_rw"))) {
      prefix = "image"; setting = "mount_mode"; setting_value = !strcasecmp(key, "image_ro") ? "ro" : "rw";
    } else if (!autotune && !strcasecmp(key, "fakelib_mode")) {
      char *colon = strchr(local, ':');
      if (!colon) { errno = EINVAL; return false; }
      *colon = 0; prefix = "game"; setting = "fakelib"; setting_value = colon + 1;
    } else if (!autotune && !strcasecmp(key, "fakelib_exclude")) {
      prefix = "game"; setting = "fakelib"; setting_value = "disabled";
    } else if (!autotune && (!strcasecmp(key, "global_fakelib_exclude") || !strcasecmp(key, "global_fakelib_exclude_title"))) {
      prefix = "game"; setting = "ignore_global_fakelib"; setting_value = "on";
    }
    if (prefix) {
      char *identity = trim(local);
      if (!strcmp(prefix, "image")) {
        char *slash = strrchr(identity, '/');
        if (slash) identity = slash + 1;
      }
      snprintf(section, sizeof(section), "%s:%s", prefix, identity);
      if (!sm_ini_set(profiles, section, setting, setting_value)) return false;
    } else {
      // Keep unsupported old values out of active sections, with their source backup intact.
      log_debug("  [CFG] legacy key not migrated: %s", key);
    }
  }
  return !recursive || sm_ini_set(config, "scan", "depth", "2");
}

bool sm_config_migrate_files(bool (*validate)(const sm_ini_t *, const sm_ini_t *)) {
  const char *sources[] = {CONFIG_FILE, LEGACY_AUTOTUNE_FILE, LEGACY_SMB_FILE};
  const char *backups[] = {CONFIG_BACKUP_DIR "/config.ini", CONFIG_BACKUP_DIR "/autotune.ini", CONFIG_BACKUP_DIR "/smb.json"};
  const char *ready = CONFIG_BACKUP_DIR "/ready.ini";
  sm_ini_t current = {0}, marker = {0}, config = {0}, profiles = {0}, old = {0}, tuned = {0};
  bool ok = sm_ini_read(CONFIG_FILE, &current, true);
  bool versioned = sm_ini_get(&current, "config", "version") != NULL;
  for (size_t i = 0; i < current.count; ++i)
    if (current.lines[i].header && !strcasecmp(current.lines[i].section, "config")) versioned = true;
  if (ok && versioned) {
    sm_ini_free(&current);
    ok = sm_ini_read(CONFIG_FILE, &current, false) && sm_ini_version(&current);
    if (ok) ok = sm_ini_read(PROFILES_FILE, &profiles, false) &&
        (!profiles.count || sm_ini_version(&profiles)) && validate(&current, &profiles);
    if (ok && access(PROFILES_FILE, F_OK) != 0 && errno == ENOENT)
      ok = sm_ini_set(&profiles, "config", "version", "2") && sm_ini_write(PROFILES_FILE, &profiles);
    if (ok && access(ready, F_OK) == 0) {
      // Finish retiring old files after a crash following the main-file commit.
      for (size_t i = 1; i < 3; ++i)
        if (same_file(sources[i], backups[i]) && unlink(sources[i]) && errno != ENOENT) ok = false;
    }
    if (ok) ok = sync_parent(CONFIG_FILE);
    goto done;
  }
  if (!ok) goto done;
  if (mkdir(CONFIG_BACKUP_DIR, 0700) && errno != EEXIST) { ok = false; goto done; }
  struct stat backup_stat;
  if (lstat(CONFIG_BACKUP_DIR, &backup_stat) || !S_ISDIR(backup_stat.st_mode)) {
    errno = ENOTDIR; ok = false; goto done;
  }
  if (!sm_ini_read(ready, &marker, false)) { ok = false; goto done; }
  if (!marker.count) {
    if (access(PROFILES_FILE, F_OK) == 0) { errno = EEXIST; ok = false; goto done; }
    for (size_t i = 0; ok && i < 3; ++i) ok = copy_backup(sources[i], backups[i]);
  } else {
    ok = sm_ini_version(&marker);
    for (size_t i = 0; ok && i < 3; ++i) {
      if (!same_file(sources[i], backups[i])) { errno = EBUSY; ok = false; }
    }
  }
  if (!ok) goto done;
  ok = sm_ini_read(backups[0], &old, true) && sm_ini_read(backups[1], &tuned, true) &&
       sm_ini_set(&config, "config", "version", "2") && sm_ini_set(&profiles, "config", "version", "2") &&
       import_rules(&old, &config, &profiles, false) &&
       import_rules(&tuned, &config, &profiles, true) && import_smb(&config, backups[2]) &&
       validate(&config, &profiles);
  if (!ok) goto done;
  for (size_t i = 0; ok && i < 3; ++i) {
    if (!same_file(sources[i], backups[i])) { errno = EBUSY; ok = false; }
  }
  if (!ok) goto done;
  if (!marker.count) {
    ok = sm_ini_set(&marker, "config", "version", "2") && sm_ini_write(ready, &marker);
    if (!ok) goto done;
  }
  // Publish the main version marker last. Until then the backup is authoritative.
  const char *candidate = CONFIG_BACKUP_DIR "/profiles-v2.ini";
  ok = sm_ini_write(candidate, &profiles);
  if (ok && access(PROFILES_FILE, F_OK) == 0 && !same_file(candidate, PROFILES_FILE)) {
    errno = EEXIST; ok = false;
  }
  if (ok) ok = sm_ini_write(PROFILES_FILE, &profiles) && sm_ini_write(CONFIG_FILE, &config);
  if (ok) {
    for (size_t i = 1; i < 3; ++i)
      if (same_file(sources[i], backups[i]) && unlink(sources[i]) && errno != ENOENT) ok = false;
    if (ok) ok = sync_parent(CONFIG_FILE);
    log_debug("  [CFG] configuration migrated to v2; originals saved in %s", CONFIG_BACKUP_DIR);
  }
done:;
  int error = ok ? 0 : (errno ? errno : EINVAL);
  sm_ini_free(&current); sm_ini_free(&marker); sm_ini_free(&config);
  sm_ini_free(&profiles); sm_ini_free(&old); sm_ini_free(&tuned);
  errno = error; return ok;
}
