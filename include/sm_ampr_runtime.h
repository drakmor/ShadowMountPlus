#ifndef SM_AMPR_RUNTIME_H
#define SM_AMPR_RUNTIME_H

#include "sm_limits.h"

// Resolve an installed emulators_path/ampr-emu-ng/libSceAmpr.sprx. If it is
// absent, atomically install the pinned runtime under fallback_root. Existing
// non-NG files are preserved and rejected. Returns 0 on success, -1 on error.
int sm_ampr_resolve_ng_runtime(const char *emulators_path,
                               const char *fallback_root,
                               char runtime_path[MAX_PATH]);

// Return 1 for an ampr-emu-ng binary, 0 for another file, and -1 on I/O error.
int sm_file_is_ampr_ng(const char *path);

#endif
