#ifndef SM_KSTUFF_CAPS_H
#define SM_KSTUFF_CAPS_H

#include <stdbool.h>
#include <stdint.h>

// Which optional SceShellCore patches the loaded kstuff actually applied.
// Measured from ShellCore's live text, not inferred from a version number.
#define SM_KSTUFF_CAP_SYSDIRPATH (1u << 0)
#define SM_KSTUFF_CAP_TROPHY (1u << 1)

// Probe SceShellCore for the capability patches and cache the result.
// -> false when the answer is UNKNOWN (unmapped firmware, ShellCore not found,
// or the read failed); *caps is only meaningful when this returns true.
bool sm_kstuff_probe_caps(uint32_t *caps);

#endif
