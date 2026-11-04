#ifndef SM_CONFIG_FILE_H
#define SM_CONFIG_FILE_H
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  char *raw, *section, *key, *value, *comment;
  bool header;
} sm_ini_line_t;
typedef struct {
  sm_ini_line_t *lines;
  size_t count, capacity;
} sm_ini_t;
typedef struct {
  const char *section, *key, *legacy;
} sm_config_key_t;
extern const sm_config_key_t sm_config_keys[];
extern const size_t sm_config_key_count;

// Missing files are empty documents. All other I/O/parse errors are failures.
bool sm_ini_read(const char *path, sm_ini_t *doc, bool legacy);
void sm_ini_free(sm_ini_t *doc);
const char *sm_ini_get(const sm_ini_t *doc, const char *section, const char *key);
bool sm_ini_set(sm_ini_t *doc, const char *section, const char *key, const char *value);
bool sm_ini_append(sm_ini_t *doc, const char *section, const char *key, const char *value);
bool sm_ini_rename_section(sm_ini_t *doc, const char *old, const char *name);
bool sm_ini_remove_section(sm_ini_t *doc, const char *section);
bool sm_ini_write(const char *path, const sm_ini_t *doc);
bool sm_ini_version(const sm_ini_t *doc);
const sm_config_key_t *sm_config_find_legacy(const char *key);
// Startup only, before any service opens configuration files.
bool sm_config_migrate_files(bool (*validate)(const sm_ini_t *, const sm_ini_t *));
#endif
