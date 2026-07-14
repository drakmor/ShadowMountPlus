#ifndef SM_MOUNT_DIAG_H
#define SM_MOUNT_DIAG_H

// Log read-only evidence explaining why unmount(path) returned EBUSY.
void sm_mount_diag_log_busy(const char *mount_point);

#endif
