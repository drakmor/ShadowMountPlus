#ifndef SM_PATCH_INTERNAL_H
#define SM_PATCH_INTERNAL_H

#include "sm_game_patches.h"

#define PATCH_MAX_FILE SM_PATCH_MAX_FILE_SIZE
#define PATCH_MAX_LINES 4096u
#define PATCH_MAX_BYTES (1024u * 1024u)
#define PATCH_MAX_VALUE 65536u

struct sm_patch_line {
  char module[128];
  uint64_t address, image_base;
  int64_t offset;
  unsigned jump_size;
  bool absolute;
  size_t size, pattern_size, target_size;
  uint8_t *bytes, *pattern, *mask, *target, *off, *expected;
};

// Derive applicability from compiled writes; fixed/mixed writes stay version-bound.
void sm_patch_set_compatibility(sm_game_patch_t *patch, const char *version);
void sm_patch_truncate(sm_game_patch_list_t *list, size_t count);
void sm_patch_free_entry(sm_game_patch_t *patch);
void sm_patch_free_line(sm_patch_line_t *line);
// Collapse successive writes inside one cheat, retaining its initial OFF bytes.
int sm_cheat_append_line(sm_game_patch_t *patch, sm_patch_line_t *line);
// Reuse the bounded JSON/SHN/MC4 document decoder for the content index.
int sm_cheat_metadata(const char *path, char title[10], char version[128]);

#endif
