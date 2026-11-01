#ifndef SM_GAME_PATCHES_H
#define SM_GAME_PATCHES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SM_PATCH_DIRECTORY "/data/shadowmount/patches"
#define SM_PATCH_SETTINGS "/data/shadowmount/patches.ini"
#define SM_PATCH_MAX_ENTRIES 256u

typedef struct sm_patch_line sm_patch_line_t;
typedef struct {
  char id[17], name[192], author[128], note[2048];
  char app_ver[128], app_elf[128], requires[32], error[128];
  bool compatible, supported, enabled;
  uint64_t image_base;
  size_t line_count;
  sm_patch_line_t *lines;
} sm_game_patch_t;

typedef struct {
  sm_game_patch_t *entries;
  size_t count;
} sm_game_patch_list_t;

// Bounded XML parsing. A rejected file never publishes a partial catalog.
int sm_patch_xml_load(const char *path, const char *title_id, const char *version,
                      sm_game_patch_list_t *list);
void sm_game_patches_free(sm_game_patch_list_t *list);
// Listing and selection share the atomic INI writer; all entries default off.
int sm_game_patches_list(const char *title_id, const char *version,
                         sm_game_patch_list_t *list);
int sm_game_patches_set_enabled(const char *title_id, const char *id, bool enabled);

typedef struct {
  void *context;
  bool (*module)(void *context, const char *name, uintptr_t *base, size_t *size);
  bool (*read)(void *context, uintptr_t address, void *data, size_t size);
  bool (*write)(void *context, uintptr_t address, const void *data, size_t size);
  bool (*cancelled)(void *context);
} sm_patch_memory_t;

typedef struct sm_patch_recovery sm_patch_recovery_t;

// Caller holds the target process stopped. Preflight every write, verify it and
// roll back in reverse order on failure. Return EAGAIN for an absent module;
// ECANCELED means rollback was not confirmed and the process must stay stopped.
int sm_patch_apply(const sm_game_patch_t *patch, const sm_patch_memory_t *memory,
                   sm_patch_recovery_t **recovery);
// Selected patches are one transaction, including conflict detection between
// entries. An absent module returns EAGAIN before any reads/writes.
int sm_patch_apply_selected(const sm_game_patch_list_t *list,
                            const sm_patch_memory_t *memory,
                            sm_patch_recovery_t **recovery);
bool sm_patch_selection_ready(const sm_game_patch_list_t *list,
                               const sm_patch_memory_t *memory);
bool sm_patch_recover(sm_patch_recovery_t **recovery, const sm_patch_memory_t *memory);
// Only after the target process has exited or its identity changed.
void sm_patch_forget_recovery(sm_patch_recovery_t **recovery);

// Owned by the existing lifecycle watcher, including cleanup after exit/stop.
void sm_game_patches_on_exec(pid_t pid, const char *title_id);
void sm_game_patches_poll(void);
void sm_game_patches_reset(void);
uint64_t sm_game_patches_next_wake_us(void);

#endif
