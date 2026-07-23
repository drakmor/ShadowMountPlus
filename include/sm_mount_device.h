#ifndef SM_MOUNT_DEVICE_H
#define SM_MOUNT_DEVICE_H

#include <stdbool.h>

#include "sm_types.h"

// Return a printable name for an attach backend enum value.
const char *attach_backend_name(attach_backend_t backend);
// Check whether a source path has aged enough to be mounted safely.
bool is_source_stable_for_mount(const char *path, const char *name,
                                const char *tag);
// Wait until a device node appears or disappears.
bool wait_for_dev_node_state(const char *devname, bool should_exist);
// Resolve backend and unit ID for a mounted image path.
bool resolve_device_from_mount(const char *mount_point,
                               attach_backend_t *backend_out, int *unit_out);
// Return true if a path is an active mount point.
bool is_active_image_mount_point(const char *path);
// Wait until /dev/lvd2 is no longer mounted during startup.
bool wait_for_lvd_release(void);
// Detach a previously attached MD or LVD unit. A non-NULL state makes an
// accepted asynchronous detach idempotent across later cleanup attempts.
bool detach_attached_unit(attach_backend_t backend, int unit_id,
                          attached_unit_detach_state_t *state);
// Wait for an accepted detach to remove its node, or detect unit reuse.
bool wait_for_attached_unit_release(
    attach_backend_t backend, int unit_id,
    const attached_unit_detach_state_t *state);

#endif
