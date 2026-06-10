#ifndef SM_MOUNT_REGISTRY_H
#define SM_MOUNT_REGISTRY_H

#include <stdbool.h>
#include "sm_limits.h"

typedef enum {
  SM_MOUNT_KIND_NULLFS_TITLE = 0,
  SM_MOUNT_KIND_NULLFS_BACKPORT,
  SM_MOUNT_KIND_NULLFS_FAKELIB,
  SM_MOUNT_KIND_IMAGE,
} sm_mount_kind_t;

void sm_mount_registry_push(const char *mount_point, const char *source_path,
                            sm_mount_kind_t kind);
void sm_mount_registry_remove(const char *mount_point);
void sm_mount_registry_shutdown(void);

#endif
