#include "sm_platform.h"

#include <expat.h>
#include <sqlite3.h>
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/sha.h>
#include <strings.h>

#include "sm_gameinfo.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_patch_index.h"
#include "sm_patch_internal.h"
#include "sm_runtime.h"

#define INDEX_MAX_FILES 20000u
#define INDEX_MAX_BYTES (128u * 1024u * 1024u)
#define INDEX_MAX_ROWS 262144u

static const char *const local_xml_roots[] = {
  SM_PATCH_DIRECTORY, "/data/user/patches/xml_orbis", "/data/user/patches/xml_prospero"
};
static const char *const local_cheat_roots[] = {
  SM_CHEAT_DIRECTORY, "/data/cheatrunner/cheats", "/data/etaHEN/cheats", "/data/elf-arsenal/cheats"
};
static const char *const local_cheat_formats[] = {"json", "shn", "mc4"};

typedef struct {
  uint64_t device, inode, size;
  int64_t modified, changed, modified_ns, changed_ns;
} index_stamp_t;

typedef struct { char *name; index_stamp_t stamp; } index_file_t;
typedef struct { sm_patch_cancel_t callback; void *context; } index_cancel_t;

static bool index_cancelled(index_cancel_t *cancel) {
  return cancel->callback && cancel->callback(cancel->context);
}

static int index_progress(void *context) { return index_cancelled(context); }

static int index_error(int code) {
  if (code == SQLITE_OK || code == SQLITE_DONE || code == SQLITE_ROW) return 0;
  if (code == SQLITE_NOMEM) return ENOMEM;
  if (code == SQLITE_BUSY || code == SQLITE_LOCKED) return EAGAIN;
  return code == SQLITE_INTERRUPT ? ECANCELED : EIO;
}

static int index_open(sqlite3 **db) {
  *db = NULL;
  char parent[MAX_PATH];
  strlcpy(parent, SM_PATCH_INDEX_PATH, sizeof(parent));
  char *slash = strrchr(parent, '/');
  if (!slash) return EINVAL;
  *slash = 0;
  if (mkdir(parent, 0777) && errno != EEXIST) return errno;
  struct stat st;
  if (!lstat(SM_PATCH_INDEX_PATH, &st) && !S_ISREG(st.st_mode)) return EINVAL;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
#ifdef SQLITE_OPEN_NOFOLLOW
  flags |= SQLITE_OPEN_NOFOLLOW;
#endif
  int error = index_error(sqlite3_open_v2(SM_PATCH_INDEX_PATH, db, flags, NULL));
  if (!error) {
    sqlite3_busy_timeout(*db, 1000);
    error = index_error(sqlite3_exec(*db,
      "PRAGMA temp_store=MEMORY;"
      "CREATE TABLE IF NOT EXISTS directories(path TEXT,kind INTEGER,stamp BLOB,identity BLOB,PRIMARY KEY(path,kind));"
      "CREATE TABLE IF NOT EXISTS files(dir TEXT,kind INTEGER,name TEXT,stamp BLOB,PRIMARY KEY(dir,kind,name));"
      "CREATE TABLE IF NOT EXISTS entries(dir TEXT,kind INTEGER,name TEXT,title TEXT,version TEXT,PRIMARY KEY(dir,kind,title,name,version));"
      "CREATE TABLE IF NOT EXISTS badges_v2(title TEXT PRIMARY KEY,version TEXT,stamp BLOB,flags INTEGER,entries BLOB);",
      NULL, NULL, NULL));
  }
  if (error) { if (*db) sqlite3_close(*db); *db = NULL; }
  return error;
}

static void file_stamp(const struct stat *st, index_stamp_t *stamp) {
  memset(stamp, 0, sizeof(*stamp));
  stamp->device = st->st_dev; stamp->inode = st->st_ino; stamp->size = st->st_size;
  stamp->modified = st->st_mtim.tv_sec; stamp->modified_ns = st->st_mtim.tv_nsec;
  stamp->changed = st->st_ctim.tv_sec; stamp->changed_ns = st->st_ctim.tv_nsec;
}

static bool index_extension(const char *name, bool cheats) {
  const char *ext = strrchr(name, '.');
  return ext && (cheats ? !strcasecmp(ext, ".json") || !strcasecmp(ext, ".shn") || !strcasecmp(ext, ".mc4") : !strcasecmp(ext, ".xml"));
}

static void free_files(index_file_t *files, size_t count) {
  for (size_t i = 0; i < count; ++i) free(files[i].name);
  free(files);
}

static int compare_files(const void *a, const void *b) {
  return strcmp(((const index_file_t *)a)->name, ((const index_file_t *)b)->name);
}

static int scan_files(const char *directory, bool cheats, index_file_t **out,
                       size_t *count, uint8_t digest[SHA256_DIGEST_LENGTH], index_cancel_t *cancel) {
  *out = NULL; *count = 0;
  DIR *dir = opendir(directory);
  if (!dir) return errno;
  int error = 0;
  size_t capacity = 0;
  uint64_t bytes = 0;
  for (;;) {
    if (index_cancelled(cancel)) { error = ECANCELED; break; }
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) { error = errno; break; }
    if (!index_extension(entry->d_name, cheats)) continue;
    struct stat st;
    if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) { error = errno; break; }
    if (!S_ISREG(st.st_mode)) continue;
    if (*count == INDEX_MAX_FILES || st.st_size < 0 || (uint64_t)st.st_size > PATCH_MAX_FILE ||
        (bytes += (uint64_t)st.st_size) > INDEX_MAX_BYTES) { error = EFBIG; break; }
    if (*count == capacity) {
      size_t next = capacity ? capacity * 2 : 64;
      index_file_t *files = realloc(*out, next * sizeof(**out));
      if (!files) { error = ENOMEM; break; }
      *out = files; capacity = next;
    }
    index_file_t *file = &(*out)[*count];
    file->name = strdup(entry->d_name);
    if (!file->name) { error = ENOMEM; break; }
    file_stamp(&st, &file->stamp); ++*count;
  }
  closedir(dir);
  if (error) { free_files(*out, *count); *out = NULL; *count = 0; return error; }
  if (*count > 1) qsort(*out, *count, sizeof(**out), compare_files);
  SHA256_CTX hash; SHA256_Init(&hash);
  for (size_t i = 0; i < *count; ++i) {
    SHA256_Update(&hash, (*out)[i].name, strlen((*out)[i].name) + 1);
    SHA256_Update(&hash, &(*out)[i].stamp, sizeof((*out)[i].stamp));
  }
  SHA256_Final(digest, &hash);
  return 0;
}

typedef struct {
  XML_Parser parser;
  unsigned depth, elements;
  bool has_ids, in_id;
  size_t used, id_count, version_count;
  char id[16], ids[256][10], versions[256][128];
  int error;
} index_xml_t;

static void metadata_error(index_xml_t *xml, int error) {
  xml->error = error; XML_StopParser(xml->parser, XML_FALSE);
}

static void XMLCALL metadata_start(void *context, const XML_Char *name, const XML_Char **attributes) {
  index_xml_t *xml = context;
  if (++xml->depth > 5 || ++xml->elements > 65536) { metadata_error(xml, EFBIG); return; }
  if (xml->depth == 1 && strcmp(name, "Patch")) { metadata_error(xml, EINVAL); return; }
  if (xml->depth == 2 && !strcmp(name, "TitleID")) xml->has_ids = true;
  if (xml->depth == 3 && !strcmp(name, "ID")) { xml->in_id = true; xml->used = 0; }
  if (xml->depth != 2 || strcmp(name, "Metadata")) return;
  const char *version = NULL;
  for (size_t i = 0; attributes[i]; i += 2)
    if (!strcmp(attributes[i], "AppVer")) version = attributes[i + 1];
  if (!version || !*version || strlen(version) >= sizeof(xml->versions[0])) { metadata_error(xml, EINVAL); return; }
  for (size_t i = 0; i < xml->version_count; ++i)
    if (!strcmp(xml->versions[i], version)) return;
  if (xml->version_count == 256) { metadata_error(xml, EFBIG); return; }
  strcpy(xml->versions[xml->version_count++], version);
}

static void XMLCALL metadata_text(void *context, const XML_Char *text, int length) {
  index_xml_t *xml = context;
  if (!xml->in_id) return;
  if (length < 0 || (size_t)length >= sizeof(xml->id) - xml->used) { metadata_error(xml, EFBIG); return; }
  memcpy(xml->id + xml->used, text, (size_t)length); xml->used += (size_t)length;
}

static void XMLCALL metadata_end(void *context, const XML_Char *name) {
  index_xml_t *xml = context;
  if (xml->in_id && !strcmp(name, "ID")) {
    xml->id[xml->used] = 0; xml->in_id = false;
    if (is_supported_game_title_id(xml->id)) {
      bool duplicate = false;
      for (size_t i = 0; i < xml->id_count; ++i) duplicate |= !strcmp(xml->ids[i], xml->id);
      if (!duplicate) {
        if (xml->id_count == 256) { metadata_error(xml, EFBIG); return; }
        strcpy(xml->ids[xml->id_count++], xml->id);
      }
    }
  }
  --xml->depth;
}

static void XMLCALL metadata_doctype(void *context, const XML_Char *name,
                                      const XML_Char *system, const XML_Char *public_id, int internal) {
  (void)name; (void)system; (void)public_id; (void)internal;
  metadata_error(context, ENOTSUP);
}

static void filename_titles(const char *name, index_xml_t *xml) {
  for (size_t i = 0; name[i] && xml->id_count < 256; ++i) {
    if (strncmp(name + i, "CUSA", 4) && strncmp(name + i, "PPSA", 4)) continue;
    size_t remaining = strlen(name + i);
    if (remaining < 9 || (remaining > 9 && isdigit((unsigned char)name[i + 9]))) continue;
    char title[10]; memcpy(title, name + i, 9); title[9] = 0;
    if (!is_supported_game_title_id(title)) continue;
    bool duplicate = false;
    for (size_t j = 0; j < xml->id_count; ++j) duplicate |= !strcmp(xml->ids[j], title);
    if (!duplicate) strcpy(xml->ids[xml->id_count++], title);
  }
}

static int xml_metadata(const char *path, index_xml_t *xml, index_cancel_t *cancel) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return errno;
  struct stat st;
  int error = fstat(fd, &st) ? errno : !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > PATCH_MAX_FILE ? EFBIG : 0;
  xml->parser = !error ? XML_ParserCreate(NULL) : NULL;
  if (!error && !xml->parser) error = ENOMEM;
  if (!error) {
    XML_SetUserData(xml->parser, xml);
    XML_SetElementHandler(xml->parser, metadata_start, metadata_end);
    XML_SetCharacterDataHandler(xml->parser, metadata_text);
    XML_SetStartDoctypeDeclHandler(xml->parser, metadata_doctype);
    XML_SetParamEntityParsing(xml->parser, XML_PARAM_ENTITY_PARSING_NEVER);
    char buffer[8192]; size_t total = 0;
    for (;;) {
      if (index_cancelled(cancel)) { error = ECANCELED; break; }
      ssize_t n = read(fd, buffer, sizeof(buffer));
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) { error = errno; break; }
      total += (size_t)n;
      if (total > PATCH_MAX_FILE) { error = EFBIG; break; }
      if (XML_Parse(xml->parser, buffer, (int)n, !n) == XML_STATUS_ERROR) { error = xml->error ? xml->error : EINVAL; break; }
      if (!n) break;
    }
  }
  if (xml->parser) XML_ParserFree(xml->parser);
  close(fd);
  return error ? error : !xml->version_count ? EINVAL : 0;
}

static int index_metadata(const char *path, const char *name, bool cheats,
                            sqlite3_stmt *insert, size_t *rows, index_cancel_t *cancel) {
  index_xml_t *metadata = calloc(1, sizeof(*metadata));
  if (!metadata) return ENOMEM;
  int error;
  if (cheats) {
    char title[10], version[128];
    error = sm_cheat_metadata(path, title, version);
    if (!error) {
      metadata->has_ids = !!*title;
      if (*title && is_supported_game_title_id(title)) strcpy(metadata->ids[metadata->id_count++], title);
      strcpy(metadata->versions[metadata->version_count++], version);
    }
  } else error = xml_metadata(path, metadata, cancel);
  if (!error && !metadata->has_ids) filename_titles(name, metadata);
  for (size_t i = 0; !error && i < metadata->id_count; ++i) {
    for (size_t j = 0; !error && j < metadata->version_count; ++j) {
      char version[128]; strlcpy(version, metadata->versions[j], sizeof(version));
      if (cheats && !*version) {
        const char *id = strstr(name, metadata->ids[i]);
        const char *start = id ? id + 9 : name;
        if (*start == '_' && isdigit((unsigned char)start[1])) {
          const char *end = ++start;
          while (isdigit((unsigned char)*end) || *end == '.') ++end;
          while (end > start && end[-1] == '.') --end;
          if ((size_t)(end - start) < sizeof(version)) { memcpy(version, start, (size_t)(end - start)); version[end - start] = 0; }
        }
      }
      if (++*rows > INDEX_MAX_ROWS) { error = EOVERFLOW; break; }
      sqlite3_bind_text(insert, 4, metadata->ids[i], -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(insert, 5, version, -1, SQLITE_TRANSIENT);
      error = index_error(sqlite3_step(insert)); sqlite3_reset(insert);
    }
  }
  free(metadata); return error;
}

static int prepare(sqlite3 *db, sqlite3_stmt **stmt, const char *sql, const char *directory, bool cheats) {
  int error = index_error(sqlite3_prepare_v2(db, sql, -1, stmt, NULL));
  if (!error) { sqlite3_bind_text(*stmt, 1, directory, -1, SQLITE_TRANSIENT); sqlite3_bind_int(*stmt, 2, cheats); }
  return error;
}

int sm_patch_index_build(const char *directory, bool cheats, bool immutable,
                           sm_patch_cancel_t cancelled, void *context) {
  index_cancel_t cancel = {cancelled, context};
  if (index_cancelled(&cancel)) return ECANCELED;
  struct stat directory_stat;
  if (stat(directory, &directory_stat)) return errno;
  if (!S_ISDIR(directory_stat.st_mode)) return ENOTDIR;
  index_stamp_t identity; file_stamp(&directory_stat, &identity);
  sqlite3 *db; int error = index_open(&db);
  if (error) return error;
  sqlite3_progress_handler(db, 1000, index_progress, &cancel);
  sqlite3_stmt *header = NULL;
  error = prepare(db, &header, "SELECT stamp,identity FROM directories WHERE path=?1 AND kind=?2;", directory, cheats);
  int status = !error ? sqlite3_step(header) : SQLITE_ERROR;
  uint8_t previous[SHA256_DIGEST_LENGTH];
  bool have_index = status == SQLITE_ROW && sqlite3_column_bytes(header, 0) == (int)sizeof(previous) &&
                    sqlite3_column_bytes(header, 1) == (int)sizeof(identity) &&
                    !memcmp(sqlite3_column_blob(header, 1), &identity, sizeof(identity));
  if (have_index) memcpy(previous, sqlite3_column_blob(header, 0), sizeof(previous));
  if (!error) error = index_error(status);
  sqlite3_finalize(header);
  index_file_t *files = NULL; size_t count = 0; uint8_t stamp[SHA256_DIGEST_LENGTH];
  if (!error && !(immutable && have_index)) error = scan_files(directory, cheats, &files, &count, stamp, &cancel);
  if (!error && have_index && (immutable || !memcmp(previous, stamp, sizeof(stamp)))) goto done;
  if (error) goto done;
  error = index_error(sqlite3_exec(db, "BEGIN IMMEDIATE; CREATE TEMP TABLE seen(name TEXT PRIMARY KEY);", NULL, NULL, NULL));
  sqlite3_stmt *lookup = NULL, *save = NULL, *seen = NULL, *erase = NULL, *insert = NULL;
  if (!error) error = prepare(db, &lookup, "SELECT stamp FROM files WHERE dir=?1 AND kind=?2 AND name=?3;", directory, cheats);
  if (!error) error = prepare(db, &save, "INSERT OR REPLACE INTO files VALUES(?1,?2,?3,?4);", directory, cheats);
  if (!error) error = index_error(sqlite3_prepare_v2(db, "INSERT INTO seen VALUES(?1);", -1, &seen, NULL));
  if (!error) error = prepare(db, &erase, "DELETE FROM entries WHERE dir=?1 AND kind=?2 AND name=?3;", directory, cheats);
  if (!error) error = prepare(db, &insert, "INSERT OR IGNORE INTO entries VALUES(?1,?2,?3,?4,?5);", directory, cheats);
  size_t rows = 0, parsed = 0, rejected = 0;
  for (size_t i = 0; !error && i < count; ++i) {
    if (index_cancelled(&cancel)) { error = ECANCELED; break; }
    const index_file_t *file = &files[i];
    sqlite3_bind_text(seen, 1, file->name, -1, SQLITE_STATIC);
    error = index_error(sqlite3_step(seen)); sqlite3_reset(seen);
    if (error) break;
    sqlite3_bind_text(lookup, 3, file->name, -1, SQLITE_STATIC);
    status = sqlite3_step(lookup);
    bool same = status == SQLITE_ROW && sqlite3_column_bytes(lookup, 0) == (int)sizeof(file->stamp) &&
                !memcmp(sqlite3_column_blob(lookup, 0), &file->stamp, sizeof(file->stamp));
    error = index_error(status); sqlite3_reset(lookup);
    if (error || same) continue;
    char path[MAX_PATH];
    int n = snprintf(path, sizeof(path), "%s/%s", directory, file->name);
    if (n < 0 || (size_t)n >= sizeof(path)) { error = ENAMETOOLONG; break; }
    sqlite3_bind_text(erase, 3, file->name, -1, SQLITE_STATIC);
    error = index_error(sqlite3_step(erase)); sqlite3_reset(erase);
    if (error) break;
    sqlite3_bind_text(insert, 3, file->name, -1, SQLITE_STATIC);
    error = index_metadata(path, file->name, cheats, insert, &rows, &cancel); ++parsed;
    if (error == EINVAL || error == ENOTSUP || error == EFBIG) {
      // Invalid files have no identity; do not repeatedly decode them until changed.
      ++rejected; error = index_error(sqlite3_step(erase)); sqlite3_reset(erase);
    }
    struct stat st; index_stamp_t current;
    if (!error) {
      if (lstat(path, &st)) error = errno;
      else { file_stamp(&st, &current); if (memcmp(&current, &file->stamp, sizeof(current))) error = EAGAIN; }
    }
    if (error) { log_debug("  [PATCH-INDEX] file failed: %s status=%d", path, error); break; }
    sqlite3_bind_text(save, 3, file->name, -1, SQLITE_STATIC);
    sqlite3_bind_blob(save, 4, &file->stamp, sizeof(file->stamp), SQLITE_STATIC);
    error = index_error(sqlite3_step(save)); sqlite3_reset(save);
  }
  sqlite3_finalize(lookup); sqlite3_finalize(save); sqlite3_finalize(seen); sqlite3_finalize(erase); sqlite3_finalize(insert);
  const char *cleanup[] = {
    "DELETE FROM entries WHERE dir=?1 AND kind=?2 AND name NOT IN(SELECT name FROM seen);",
    "DELETE FROM files WHERE dir=?1 AND kind=?2 AND name NOT IN(SELECT name FROM seen);"
  };
  for (size_t i = 0; !error && i < 2; ++i) {
    sqlite3_stmt *stmt = NULL; error = prepare(db, &stmt, cleanup[i], directory, cheats);
    if (!error) error = index_error(sqlite3_step(stmt));
    sqlite3_finalize(stmt);
  }
  if (!error) {
    index_file_t *verify = NULL; size_t verify_count = 0; uint8_t verify_stamp[SHA256_DIGEST_LENGTH];
    error = scan_files(directory, cheats, &verify, &verify_count, verify_stamp, &cancel);
    free_files(verify, verify_count);
    if (!error && memcmp(stamp, verify_stamp, sizeof(stamp))) error = EAGAIN;
  }
  if (!error) {
    sqlite3_stmt *total = NULL;
    error = prepare(db, &total, "SELECT count(*) FROM entries WHERE dir=?1 AND kind=?2;", directory, cheats);
    if (!error) {
      status = sqlite3_step(total); error = index_error(status);
      if (!error && (status != SQLITE_ROW || sqlite3_column_int64(total, 0) > INDEX_MAX_ROWS)) error = EFBIG;
    }
    sqlite3_finalize(total);
    if (!error) {
      if (stat(directory, &directory_stat)) error = errno;
      else { index_stamp_t current; file_stamp(&directory_stat, &current); if (memcmp(&identity, &current, sizeof(identity))) error = EAGAIN; }
    }
  }
  header = NULL;
  if (!error) error = prepare(db, &header, "INSERT OR REPLACE INTO directories VALUES(?1,?2,?3,?4);", directory, cheats);
  if (!error) {
    sqlite3_bind_blob(header, 3, stamp, sizeof(stamp), SQLITE_STATIC);
    sqlite3_bind_blob(header, 4, &identity, sizeof(identity), SQLITE_STATIC);
    error = index_error(sqlite3_step(header));
  }
  sqlite3_finalize(header);
  if (!error && index_cancelled(&cancel)) error = ECANCELED;
  // Rollback must remain possible after the cancellation callback starts firing.
  sqlite3_progress_handler(db, 0, NULL, NULL);
  if (!error) error = index_error(sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL));
  if (error) sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
  if (!error && parsed) log_debug("  [PATCH-INDEX] indexed: %s files=%zu parsed=%zu rejected=%zu", directory, count, parsed, rejected);
done:
  free_files(files, count); sqlite3_close(db); return error;
}

static bool lookup_cancelled(void *context) {
  (void)context; return should_stop_requested() || runtime_sleep_mode_active();
}

static int collect_indexed(sqlite3 *db, const char *directory, const char *title, bool cheats,
                             char **paths, size_t *count) {
  sqlite3_stmt *query = NULL;
  int error = prepare(db, &query, "SELECT DISTINCT name FROM entries WHERE dir=?1 AND kind=?2 AND title=?3 ORDER BY name;", directory, cheats);
  if (!error) sqlite3_bind_text(query, 3, title, -1, SQLITE_STATIC);
  while (!error) {
    int status = sqlite3_step(query);
    if (status != SQLITE_ROW) { error = index_error(status); break; }
    const char *name = (const char *)sqlite3_column_text(query, 0);
    if (!name || !*name || strchr(name, '/') || strchr(name, '\\') ||
        !strcmp(name, ".") || !strcmp(name, "..") || (size_t)sqlite3_column_bytes(query, 0) != strlen(name)) { error = EINVAL; break; }
    bool duplicate = false;
    for (size_t i = 0; i < *count; ++i) {
      const char *base = strrchr(paths[i], '/');
      if (base && !strcmp(base + 1, name)) duplicate = true;
    }
    if (duplicate) continue;
    if (*count == SM_PATCH_MAX_FILES) { error = EFBIG; break; }
    char path[MAX_PATH]; int n = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (n < 0 || (size_t)n >= sizeof(path)) { error = ENAMETOOLONG; break; }
    paths[*count] = strdup(path);
    if (!paths[*count]) { error = ENOMEM; break; }
    ++*count;
  }
  sqlite3_finalize(query); return error;
}

int sm_patch_index_collect(const char *directory, const char *title, bool cheats,
                             bool immutable, char **paths, size_t *count) {
  int error = sm_patch_index_build(directory, cheats, immutable, lookup_cancelled, NULL);
  if (error == ENOENT) return 0;
  if (error) return error;
  sqlite3 *db; error = index_open(&db);
  if (error) return error;
  error = collect_indexed(db, directory, title, cheats, paths, count);
  sqlite3_close(db); return error;
}

static int collect_local(sqlite3 *db, const char *title, char **paths, size_t *count) {
  if (!is_supported_game_title_id(title)) return EINVAL;
  size_t before = *count;
  int error = db ? collect_indexed(db, local_xml_roots[0], title, false, paths, count) :
                  sm_patch_index_collect(local_xml_roots[0], title, false, false, paths, count);
  if (!error && *count == before)
    error = db ? collect_indexed(db, local_xml_roots[!strncmp(title, "CUSA", 4) ? 1 : 2], title, false, paths, count) :
                 sm_patch_index_collect(local_xml_roots[!strncmp(title, "CUSA", 4) ? 1 : 2], title, false, false, paths, count);
  before = *count;
  for (size_t i = 0; !error && *count == before && i < sizeof(local_cheat_roots) / sizeof(local_cheat_roots[0]); ++i) {
    error = db ? collect_indexed(db, local_cheat_roots[i], title, true, paths, count) :
                 sm_patch_index_collect(local_cheat_roots[i], title, true, false, paths, count);
    for (size_t j = 0; !error && j < 3; ++j) {
      char directory[MAX_PATH];
      snprintf(directory, sizeof(directory), "%s/%s", local_cheat_roots[i], local_cheat_formats[j]);
      error = db ? collect_indexed(db, directory, title, true, paths, count) :
                   sm_patch_index_collect(directory, title, true, false, paths, count);
    }
  }
  return error;
}

int sm_patch_index_collect_local(const char *title, char **paths, size_t *count) {
  return collect_local(NULL, title, paths, count);
}

static int compare_paths(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

typedef struct { char id[17]; uint8_t kind; } badge_entry_t;

static unsigned enabled_badges(const sm_ini_t *selection, const char *title, const badge_entry_t *entries, size_t count) {
  if (!selection) return 0;
  char section[48]; snprintf(section, sizeof(section), "game:%s", title);
  unsigned enabled = 0;
  for (size_t i = 0; i < count; ++i) {
    const char *value = sm_ini_get(selection, section, entries[i].id);
    if (value && (!strcmp(value, "on") || !strcmp(value, "1"))) enabled |= entries[i].kind;
  }
  return enabled;
}

static int indexed_badges(sqlite3 *db, sm_patch_badges_t *item, const sm_patch_source_config_t *sources,
                            const sm_ini_t *selection) {
  char *paths[SM_PATCH_MAX_FILES] = {0}; size_t count = 0;
  int error = collect_local(db, item->title_id, paths, &count);
  for (size_t i = 0; !error && i < sources->count; ++i) {
    char directory[192];
    if (sm_patch_source_current(&sources->sources[i], directory))
      error = collect_indexed(db, directory, item->title_id, sources->sources[i].cheats, paths, &count);
  }
  if (!error && !count) return 0;
  qsort(paths, count, sizeof(paths[0]), compare_paths);
  SHA256_CTX hash; SHA256_Init(&hash);
  for (size_t i = 0; !error && i < count; ++i) {
    struct stat st;
    if (lstat(paths[i], &st)) { error = errno; break; }
    index_stamp_t stamp; file_stamp(&st, &stamp);
    SHA256_Update(&hash, paths[i], strlen(paths[i]) + 1);
    SHA256_Update(&hash, &stamp, sizeof(stamp));
  }
  uint8_t digest[SHA256_DIGEST_LENGTH]; SHA256_Final(digest, &hash);
  sqlite3_stmt *query = NULL;
  if (!error) error = index_error(sqlite3_prepare_v2(db, "SELECT version,stamp,flags,entries FROM badges_v2 WHERE title=?1;", -1, &query, NULL));
  bool cached = false;
  if (!error) {
    sqlite3_bind_text(query, 1, item->title_id, -1, SQLITE_STATIC);
    int status = sqlite3_step(query); error = index_error(status);
    const char *version = status == SQLITE_ROW ? (const char *)sqlite3_column_text(query, 0) : NULL;
    cached = version && !strcmp(version, item->version) && sqlite3_column_bytes(query, 1) == (int)sizeof(digest) &&
             !memcmp(sqlite3_column_blob(query, 1), digest, sizeof(digest));
    int bytes = cached ? sqlite3_column_bytes(query, 3) : 0;
    const badge_entry_t *entries = cached ? sqlite3_column_blob(query, 3) : NULL;
    if (bytes < 0 || bytes > (int)(SM_PATCH_MAX_ENTRIES * sizeof(*entries)) || bytes % sizeof(*entries)) cached = false;
    size_t entry_count = (size_t)bytes / sizeof(*entries);
    if (entry_count && !entries) cached = false;
    unsigned available = 0;
    for (size_t i = 0; cached && i < entry_count; ++i) {
      if (entries[i].id[16] || strspn(entries[i].id, "0123456789abcdef") != 16 ||
          (entries[i].kind != SM_PATCH_BADGE_PATCHES && entries[i].kind != SM_PATCH_BADGE_CHEATS)) cached = false;
      available |= entries[i].kind;
    }
    if (cached && available != (unsigned)sqlite3_column_int(query, 2)) cached = false;
    if (cached) {
      item->available = available;
      item->enabled = enabled_badges(selection, item->title_id, entries, entry_count);
    }
  }
  sqlite3_finalize(query);
  sm_game_patch_list_t list = {0};
  for (size_t i = 0; !error && !cached && i < count; ++i) {
    if (lookup_cancelled(NULL)) { error = ECANCELED; break; }
    const char *ext = strrchr(paths[i], '.');
    error = ext && !strcasecmp(ext, ".xml") ? sm_patch_xml_load(paths[i], item->title_id, item->version, &list) :
             sm_cheat_load(paths[i], item->title_id, item->version, &list);
  }
  if (!error && !cached) {
    badge_entry_t *entries = calloc(list.count ? list.count : 1, sizeof(*entries));
    size_t entry_count = 0;
    if (!entries) error = ENOMEM;
    for (size_t i = 0; !error && i < list.count; ++i) {
      size_t chain[SM_PATCH_MAX_ENTRIES], length;
      if (!sm_patch_dependency_chain(&list, i, chain, &length)) {
        badge_entry_t *entry = &entries[entry_count++];
        memcpy(entry->id, list.entries[i].id, sizeof(entry->id));
        entry->kind = !strcmp(list.entries[i].format, "XML") ? SM_PATCH_BADGE_PATCHES : SM_PATCH_BADGE_CHEATS;
        item->available |= entry->kind;
      }
    }
    if (!error) item->enabled = enabled_badges(selection, item->title_id, entries, entry_count);
    query = NULL;
    if (!error) error = index_error(sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO badges_v2 VALUES(?1,?2,?3,?4,?5);", -1, &query, NULL));
    if (!error) {
      sqlite3_bind_text(query, 1, item->title_id, -1, SQLITE_STATIC);
      sqlite3_bind_text(query, 2, item->version, -1, SQLITE_STATIC);
      sqlite3_bind_blob(query, 3, digest, sizeof(digest), SQLITE_STATIC);
      sqlite3_bind_int(query, 4, (int)item->available);
      sqlite3_bind_blob(query, 5, entries, (int)(entry_count * sizeof(*entries)), SQLITE_STATIC);
      error = index_error(sqlite3_step(query));
    }
    sqlite3_finalize(query);
    free(entries);
  }
  sm_game_patches_free(&list);
  for (size_t i = 0; i < count; ++i) free(paths[i]);
  return error;
}

int sm_patch_index_badges(sm_patch_badges_t *items, size_t count, const sm_patch_source_config_t *sources,
                           const sm_ini_t *selection) {
  for (size_t i = 0; i < count; ++i) { items[i].available = 0; items[i].enabled = 0; }
  if (!count) return 0;
  int error = sm_patch_index_warm_sources(sources, lookup_cancelled, NULL);
  if (error) return error;
  sm_patch_sources_read_lock();
  sqlite3 *db;
  error = index_open(&db);
  int first_error = error;
  for (size_t i = 0; !error && i < count; ++i) {
    if (lookup_cancelled(NULL)) { first_error = ECANCELED; break; }
    if (!is_supported_game_title_id(items[i].title_id) || !items[i].version) continue;
    int result = indexed_badges(db, &items[i], sources, selection);
    if (result && !first_error) first_error = result;
  }
  if (db) sqlite3_close(db);
  sm_patch_sources_read_unlock();
  return first_error;
}

void sm_patch_index_remove(const char *directory) {
  if (access(SM_PATCH_INDEX_PATH, F_OK)) return;
  sqlite3 *db; if (index_open(&db)) return;
  int error = index_error(sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL));
  const char *sql[] = {"DELETE FROM entries WHERE dir=?1;", "DELETE FROM files WHERE dir=?1;", "DELETE FROM directories WHERE path=?1;"};
  for (size_t i = 0; !error && i < 3; ++i) {
    sqlite3_stmt *stmt = NULL;
    error = index_error(sqlite3_prepare_v2(db, sql[i], -1, &stmt, NULL));
    if (!error) { sqlite3_bind_text(stmt, 1, directory, -1, SQLITE_STATIC); error = index_error(sqlite3_step(stmt)); }
    sqlite3_finalize(stmt);
  }
  sqlite3_exec(db, error ? "ROLLBACK;" : "COMMIT;", NULL, NULL, NULL); sqlite3_close(db);
}

int sm_patch_index_warm_sources(const sm_patch_source_config_t *sources,
                                  sm_patch_cancel_t cancelled, void *context) {
  int first_error = 0;
  for (size_t i = 0; i < sizeof(local_xml_roots) / sizeof(local_xml_roots[0]); ++i) {
    int error = sm_patch_index_build(local_xml_roots[i], false, false, cancelled, context);
    if (error == ECANCELED) return error;
    if (error && error != ENOENT && !first_error) first_error = error;
  }
  for (size_t i = 0; i < sizeof(local_cheat_roots) / sizeof(local_cheat_roots[0]); ++i) {
    for (size_t j = 0; j < 4; ++j) {
      char directory[MAX_PATH];
      snprintf(directory, sizeof(directory), "%s%s%s", local_cheat_roots[i], j ? "/" : "", j ? local_cheat_formats[j - 1] : "");
      int error = sm_patch_index_build(directory, true, false, cancelled, context);
      if (error == ECANCELED) return error;
      if (error && error != ENOENT && !first_error) first_error = error;
    }
  }
  sm_patch_sources_read_lock();
  for (size_t i = 0; i < sources->count; ++i) {
    char directory[192];
    if (!sm_patch_source_current(&sources->sources[i], directory)) continue;
    int error = sm_patch_index_build(directory, sources->sources[i].cheats, true, cancelled, context);
    if (error && !first_error) first_error = error;
    if (error == ECANCELED) { first_error = error; break; }
  }
  sm_patch_sources_read_unlock(); return first_error;
}
