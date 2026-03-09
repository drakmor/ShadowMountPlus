#ifndef SM_STABILITY_H
#define SM_STABILITY_H

#include <stdbool.h>

// Check whether a path is old enough to be treated as stable.
bool is_path_stable_now(const char *path, double *root_diff_out,
                        int *stat_errno_out);
// Wait briefly for a path to become stable before giving up.
bool wait_for_stability_fast(const char *path, const char *name);

#endif
