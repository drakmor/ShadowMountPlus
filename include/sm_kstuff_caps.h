#ifndef SM_KSTUFF_CAPS_H
#define SM_KSTUFF_CAPS_H

#include <stdbool.h>
#include <stdint.h>

// Optional SceShellCore patches kstuff applied, read from its live text.
#define SM_KSTUFF_CAP_SYSDIRPATH (1u << 0)
#define SM_KSTUFF_CAP_TROPHY (1u << 1)

// Probe SceShellCore for the capability patches; *caps is only set when this
// returns true, and false means unmeasurable rather than none present.
bool sm_kstuff_probe_caps(uint32_t *caps);

#endif
