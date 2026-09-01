#ifndef SM_ICON_THUMB_H
#define SM_ICON_THUMB_H

#include <stdbool.h>
#include <sys/stat.h>

#include "sm_limits.h"

#define SM_ICON_THUMB_SIZE 128u

// Return a cached thumbnail path, creating it atomically when necessary.
bool sm_icon_thumbnail_path(const char *title_id, const char *source_path,
                            const struct stat *source_st,
                            char out[MAX_PATH]);

#endif
