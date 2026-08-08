#ifndef SM_FAN_H
#define SM_FAN_H

// Apply the configured automatic fan-controller target for a confirmed running
// game. Disabled (auto) configuration is a no-op.
void sm_fan_apply_game_target(const char *title_id);

#endif
