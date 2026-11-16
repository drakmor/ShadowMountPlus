#ifndef SM_EXEC_PERMISSIONS_H
#define SM_EXEC_PERMISSIONS_H

// Owner, group and other execute bits. The PS5 loader rejects eboot.bin and
// PRX/SPRX modules that lack them with a "mount flag / attribute error".
#define SM_EXEC_PERMISSION_BITS 0111

// Add any missing execute bits to a regular file, keeping all other bits.
// Returns 1 when the mode changed, 0 when nothing was needed (including
// missing files and non-regular files), or -1 with errno set on failure.
int sm_add_exec_permission(const char *path);

#endif
