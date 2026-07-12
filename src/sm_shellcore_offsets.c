#include "sm_shellcore_offsets.h"

#include <stddef.h>

static const sm_shellcore_firmware_offsets_t g_shellcore_offsets[] = {
#include "sm_shellcore_offsets.inc"
};

const sm_shellcore_firmware_offsets_t *
sm_shellcore_offsets_for_firmware(uint32_t raw_firmware) {
  uint16_t firmware = (uint16_t)((raw_firmware >> 16) & 0xffffu);
  for (size_t i = 0;
       i < sizeof(g_shellcore_offsets) / sizeof(g_shellcore_offsets[0]); ++i) {
    if (g_shellcore_offsets[i].firmware == firmware)
      return &g_shellcore_offsets[i];
  }
  return NULL;
}

const char *sm_shellcore_target_name(sm_shellcore_target_t target) {
  switch (target) {
  case SM_SHELLCORE_TARGET_LAUNCH_APP:
    return "launchApp";
  case SM_SHELLCORE_TARGET_UNMOUNT_WORKSPACE:
    return "unmountWorkspace";
  case SM_SHELLCORE_TARGET_INSTALL_TITLE_DIR:
    return "AppInstallTitleDirMain";
  case SM_SHELLCORE_TARGET_INSTALL_ALL:
    return "AppInstallAll";
  default:
    return "unknown";
  }
}
