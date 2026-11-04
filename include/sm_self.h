#ifndef SM_SELF_H
#define SM_SELF_H

#include <stdbool.h>
#include "sm_limits.h"

// Read the original filename from ELF or readable, uncompressed SELF metadata.
// Missing, encrypted, compressed and malformed metadata returns false.
bool sm_self_read_original_filename(const char *path, char out[MAX_PATH]);

#endif
