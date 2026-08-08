#include "sm_platform.h"

#include "sm_config_mount.h"
#include "sm_fan.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_types.h"

#define FAN_DEVICE "/dev/icc_fan"
#define FAN_GET_AUTOSERVO 0xC01C8F08UL
#define FAN_SET_AUTOSERVO 0xC01C8F07UL
#define FAN_CONFIG_SIZE 28u
#define FAN_SELECTOR 0u
#define FAN_TARGET_OFFSET 5u

static int get_fan_config(int fd, uint8_t config[FAN_CONFIG_SIZE]) {
  memset(config, 0, FAN_CONFIG_SIZE);
  config[2] = FAN_SELECTOR;
  return ioctl(fd, FAN_GET_AUTOSERVO, config);
}

static int set_fan_target(int fd, uint8_t target_c,
                          uint8_t *previous_target_out) {
  uint8_t current[FAN_CONFIG_SIZE];
  uint8_t verified[FAN_CONFIG_SIZE];

  if (get_fan_config(fd, current) != 0)
    return -1;

  *previous_target_out = current[FAN_TARGET_OFFSET];
  if (*previous_target_out == target_c)
    return 0;

  current[FAN_TARGET_OFFSET] = target_c;
  if (ioctl(fd, FAN_SET_AUTOSERVO, current) != 0)
    return -1;
  if (get_fan_config(fd, verified) != 0)
    return -1;
  if (verified[FAN_TARGET_OFFSET] == target_c)
    return 0;

  errno = EIO;
  return -1;
}

static int apply_fan_target(uint8_t target_c,
                            uint8_t *previous_target_out) {
  int fd = open(FAN_DEVICE, O_RDWR | O_DIRECT);
  if (fd < 0)
    return -1;

  int result = set_fan_target(fd, target_c, previous_target_out);
  int saved_errno = errno;
  close(fd);
  errno = saved_errno;
  return result;
}

void sm_fan_apply_game_target(const char *title_id) {
  uint32_t target_c = runtime_config()->fan_target_temperature_c;
  if (target_c == FAN_TARGET_TEMPERATURE_AUTO)
    return;

  uint8_t previous_target = 0;
  if (apply_fan_target((uint8_t)target_c, &previous_target) != 0) {
    log_debug("  [FAN] target apply failed for %s: %s", title_id,
              strerror(errno));
    return;
  }

  if (previous_target == target_c) {
    log_debug("  [FAN] target already %u C for %s", (unsigned)target_c,
              title_id);
  } else {
    log_debug("  [FAN] target applied for %s: %u C -> %u C", title_id,
              (unsigned)previous_target, (unsigned)target_c);
  }
}
