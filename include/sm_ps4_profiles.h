#ifndef SM_PS4_PROFILES_H
#define SM_PS4_PROFILES_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* The hook owner controls installation/shutdown; launch changes only policy data. */
void sm_ps4_profiles_sync(pid_t pid);
void sm_ps4_profiles_stop(void);
// Read the SFO-published flags at the lifecycle's confirmed game exec.
void sm_ps4_profiles_on_exec(const char *title_id);

// Use the launch owner's resolved flags/revision for Pro, clocks and memory together.
// No ptrace or RPC while publishing.
bool sm_ps4_profiles_apply_title(const char *title_id, bool launching,
                                 uint8_t policy, uint64_t revision);

#endif
