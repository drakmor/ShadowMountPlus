#ifndef SM_TYPES_H
#define SM_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "sm_limits.h"

typedef struct {
  // +0x00: 1 -> mount read-only, 0 -> allow write.
  uint32_t ro;
  // +0x04: reserved in observed Shell/FSMP callers.
  uint32_t reserved0;
  // +0x08: logical budget/domain string, usually "game" or "system".
  const char *budget_id;
  // +0x10: reserved in observed Shell/FSMP callers.
  uint32_t reserved1;
  // +0x14: bitmask consumed by devpfs mount logic.
  uint32_t flags;
  // +0x18: optional "maxpkgszingib" value (GiB), 0 means not set.
  uint64_t max_pkg_gib;
} devpfs_mount_opt_t;

typedef struct {
  // Human-readable profile id for logs.
  const char *name;
  // Raw option payload mapped to FSMP mount behavior.
  devpfs_mount_opt_t opt;
} devpfs_mount_profile_t;

typedef struct {
  // Source object class (observed: 1=file, 2=device-like/special source).
  uint16_t source_type;
  // Layer descriptor flags (observed bit0 = no bitmap file path).
  uint16_t flags;
  // Must be zero.
  uint32_t reserved0;
  // Backing file or device path.
  const char *path;
  // Data start offset in backing object (bytes).
  uint64_t offset;
  // Data size exposed via this layer (bytes).
  uint64_t size;
  // Optional bitmap file path.
  const char *bitmap_path;
  // Bitmap offset in bitmap file (bytes).
  uint64_t bitmap_offset;
  // Bitmap size (bytes), 0 when bitmap is unused.
  uint64_t bitmap_size;
} lvd_ioctl_layer_v0_t;

typedef struct {
  // Protocol version for /dev/lvdctl ioctl payload (0 = V0/base attach).
  uint32_t io_version;
  // Input: usually -1 for auto-assign. Output: created lvd unit id.
  int32_t device_id;
  // User-visible sector size exported by /dev/lvdN.
  uint32_t sector_size;
  // Secondary unit/granularity validated against sector_size.
  uint32_t secondary_unit;
  // Normalized attach flags produced from wrapper raw options.
  uint16_t flags;
  // LVD image type id (validator accepts 0..0xC).
  uint16_t image_type;
  // Number of valid entries in layers_ptr.
  uint32_t layer_count;
  // Total exported virtual size (bytes).
  uint64_t device_size;
  // Pointer to V0 layer array in user payload.
  lvd_ioctl_layer_v0_t *layers_ptr;
} lvd_ioctl_attach_v0_t;

typedef struct {
  // Must be zero.
  uint32_t reserved0;
  // Target lvd unit id to detach.
  int32_t device_id;
  // Reserved padding required by kernel ABI.
  uint8_t reserved[0x20];
} lvd_ioctl_detach_t;

struct AppDbTitleList {
  char(*ids)[MAX_TITLE_ID];
  int count;
  int capacity;
};

typedef struct scan_candidate {
  char path[MAX_PATH];
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  bool installed;
  bool in_app_db;
} scan_candidate_t;

typedef enum {
  ATTACH_BACKEND_NONE = 0,
  // /dev/lvdctl -> /dev/lvdN
  ATTACH_BACKEND_LVD,
  // /dev/mdctl -> /dev/mdN
  ATTACH_BACKEND_MD,
} attach_backend_t;

typedef struct runtime_config {
  bool debug_enabled;
  bool quiet_mode;
  bool mount_read_only;
  bool force_mount;
  bool app_install_all_enabled;
  bool app_install_all_forced;
  bool backport_fakelib_enabled;
  bool kstuff_game_auto_toggle;
  bool kstuff_crash_detection_enabled;
  bool legacy_recursive_scan_forced;
  uint32_t scan_depth;
  uint32_t scan_interval_us;
  uint32_t stability_wait_seconds;
  uint32_t kstuff_pause_delay_image_seconds;
  uint32_t kstuff_pause_delay_direct_seconds;
  attach_backend_t exfat_backend;
  attach_backend_t ufs_backend;
  uint32_t lvd_sector_exfat;
  uint32_t lvd_sector_ufs;
  uint32_t lvd_sector_pfs;
  uint32_t md_sector_exfat;
  uint32_t md_sector_ufs;
} runtime_config_t;

typedef enum {
  IMAGE_FS_UNKNOWN = 0,
  IMAGE_FS_UFS,
  IMAGE_FS_EXFAT,
  IMAGE_FS_PFS,
} image_fs_type_t;

typedef struct sm_error {
  int code;
  char subsystem[16];
  char message[256];
  char path[MAX_PATH];
  bool valid;
  bool notified;
} sm_error_t;

#endif
