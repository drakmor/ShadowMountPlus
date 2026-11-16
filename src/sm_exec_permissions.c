#include "sm_platform.h"
#include "sm_exec_permissions.h"

int sm_add_exec_permission(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0)
    return errno == ENOENT ? 0 : -1;
  if (!S_ISREG(st.st_mode))
    return 0;

  mode_t mode = st.st_mode & 07777;
  if ((mode & SM_EXEC_PERMISSION_BITS) == SM_EXEC_PERMISSION_BITS)
    return 0;
  if (chmod(path, mode | SM_EXEC_PERMISSION_BITS) != 0)
    return -1;
  return 1;
}
