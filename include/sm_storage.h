#ifndef SM_STORAGE_H
#define SM_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

// Sum regular-file sizes under a file or directory without following links.
int sm_storage_measure_path(const char *path, uint64_t *size_out);
// Copy a regular file or directory tree exactly. Destination must not exist.
int sm_storage_copy_path(const char *source, const char *destination);
// Move a file or directory, falling back to copy+delete across filesystems.
int sm_storage_move_path(const char *source, const char *destination);
// Recursively remove a regular file or directory without following links.
int sm_storage_delete_path(const char *path);

#endif
