#ifndef SM_SHELLCORE_OFFSETS_H
#define SM_SHELLCORE_OFFSETS_H

#include <stdint.h>

typedef enum {
  SM_SHELLCORE_TARGET_LAUNCH_APP = 0,
  SM_SHELLCORE_TARGET_UNMOUNT_WORKSPACE,
  SM_SHELLCORE_TARGET_INSTALL_TITLE_DIR,
  SM_SHELLCORE_TARGET_INSTALL_ALL,
  SM_SHELLCORE_TARGET_COUNT
} sm_shellcore_target_t;

typedef struct {
  uintptr_t offset;
  uint8_t patch_size;
} sm_shellcore_target_offset_t;

typedef struct {
  uint16_t firmware;
  const char *name;
  sm_shellcore_target_offset_t targets[SM_SHELLCORE_TARGET_COUNT];
} sm_shellcore_firmware_offsets_t;

const sm_shellcore_firmware_offsets_t *
sm_shellcore_offsets_for_firmware(uint32_t raw_firmware);
const char *sm_shellcore_target_name(sm_shellcore_target_t target);

#endif
