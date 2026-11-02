#ifndef SM_PATCH_SOURCES_H
#define SM_PATCH_SOURCES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define SM_PATCH_SOURCE_MAX 12u
#define SM_PATCH_SOURCE_URL_MAX 1024u
#define SM_PATCH_SOURCE_ROOT "/data/shadowmount/catalogs"
#define SM_PATCH_ARCHIVE_MAX (512u * 1024u * 1024u)

typedef struct {
  bool cheats;
  char url[SM_PATCH_SOURCE_URL_MAX];
} sm_patch_source_t;

#define SM_PATCH_SOURCE_PRESETS 6u
extern const sm_patch_source_t sm_patch_source_presets[SM_PATCH_SOURCE_PRESETS];

typedef struct {
  uint32_t interval_hours; // Zero disables automatic updates.
  size_t count;
  sm_patch_source_t sources[SM_PATCH_SOURCE_MAX];
} sm_patch_source_config_t;

typedef struct {
  bool queued, running;
  int error;
  time_t last_check;
  uint64_t sources_hash;
} sm_patch_update_status_t;

bool sm_patch_source_valid(const sm_patch_source_t *source);
uint64_t sm_patch_source_hash(const sm_patch_source_config_t *config);
void sm_patch_source_directory(const sm_patch_source_t *source, char out[128]);
void sm_patch_sources_read_lock(void);
void sm_patch_sources_read_unlock(void);
void sm_patch_sources_write_lock(void);
void sm_patch_sources_write_unlock(void);
// Snapshot current's immutable directory under the read lock.
bool sm_patch_source_current(const sm_patch_source_t *source, char out[192]);
bool sm_patch_sources_last_status(const sm_patch_source_config_t *config, sm_patch_update_status_t *status);
uint32_t sm_patch_sources_delay(const sm_patch_source_config_t *config);
typedef int (*sm_patch_fetch_t)(const char *url, const char *path, void *context);
typedef bool (*sm_patch_cancel_t)(void *context);
int sm_patch_sources_update(const sm_patch_source_config_t *config,
                            sm_patch_fetch_t fetch, sm_patch_cancel_t cancelled,
                            void *context);
// Scheduled by the existing network update worker.
int sm_patch_updater_request(void);
void sm_patch_updater_status(sm_patch_update_status_t *out);

#endif
