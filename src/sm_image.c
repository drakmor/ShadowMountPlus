#include "sm_platform.h"
#include "sm_runtime.h"
#include "sm_image.h"
#include "sm_hash.h"
#include "sm_image_cache.h"
#include "sm_game_cache.h"
#include "sm_log.h"
#include "sm_config_mount.h"
#include "sm_limits.h"
#include "sm_mount_defs.h"
#include "sm_mount_device.h"
#include "sm_filesystem.h"
#include "sm_path_state.h"
#include "sm_path_utils.h"
#include "sm_paths.h"

static bool release_runtime_image_attachment(const char *file_path,
                                             uint64_t generation);

static bool unmount_completed(unmount_result_t result) {
  return result.filesystem_released && result.device_released &&
         result.directory_removed;
}

static bool invalidate_matching_image_cache_entry(
    int index, const image_cache_entry_t *expected) {
  image_cache_entry_t current;
  if (!expected || !get_image_cache_entry(index, &current) ||
      current.generation != expected->generation ||
      strcmp(current.path, expected->path) != 0 ||
      strcmp(current.mount_point, expected->mount_point) != 0 ||
      current.unit_id != expected->unit_id ||
      current.backend != expected->backend ||
      (expected->detach_state.node_identity_valid &&
       (!current.detach_state.node_identity_valid ||
        current.detach_state.node_device != expected->detach_state.node_device ||
        current.detach_state.node_inode != expected->detach_state.node_inode ||
        current.detach_state.node_rdev != expected->detach_state.node_rdev))) {
    errno = ESTALE;
    return false;
  }
  return invalidate_image_cache_entry(index, &current);
}

static void rollback_unpublished_attached_unit(attach_backend_t backend,
                                               int unit_id,
                                               const char *devname) {
  attached_unit_detach_state_t detach_state = {0};
  bool detached = detach_attached_unit(backend, unit_id, &detach_state);
  if (!detached && !detach_state.node_identity_valid &&
      wait_for_dev_node_state(devname, true)) {
    detached = detach_attached_unit(backend, unit_id, &detach_state);
  }
  if (!detached && detach_state.requested)
    detached = wait_for_attached_unit_release(backend, unit_id, &detach_state);
  if (!detached) {
    log_debug("  [IMG][%s] unpublished unit rollback pending: unit=%d error=%s",
              attach_backend_name(backend), unit_id, strerror(errno));
  }
}

static bool image_fs_type_is_pfs(image_fs_type_t fs_type) {
  return fs_type == IMAGE_FS_PFS || fs_type == IMAGE_FS_PFSC_CONTAINER;
}

static bool pfs_path_is_nested_inner(const char *path, image_fs_type_t fs_type) {
  return fs_type == IMAGE_FS_PFS && is_pfsc_image_mount_base_or_child(path);
}

static bool pfs_path_uses_nested_profile(const char *path,
                                         image_fs_type_t fs_type) {
  return fs_type == IMAGE_FS_PFSC_CONTAINER ||
         pfs_path_is_nested_inner(path, fs_type);
}

static uint8_t get_nested_pfs_img_type(const char *path,
                                       image_fs_type_t fs_type) {
  if (pfs_path_is_nested_inner(path, fs_type))
    return PFS_NESTED_INNER_IMG_TYPE;
  return PFS_NESTED_OUTER_IMG_TYPE;
}

static uint16_t pfs_lvd_game_image_type_from_selector(uint8_t selector) {
  static const uint16_t table[8] = {1, 2, 3, 4, 8, 9, 10, 11};
  uint32_t idx = (((uint32_t)selector >> 7) & 1u) +
                 2u * ((uint32_t)selector & 0x1Fu);

  idx ^= 1u;
  if (idx >= 8u)
    return LVD_ATTACH_IMAGE_TYPE_SAVE_DATA;
  return table[idx];
}

typedef struct {
  bool legacy;
  bool nested_backing_cache;
} image_mount_profile_t;

static image_mount_profile_t
get_image_mount_profile(const runtime_config_t *cfg, const char *file_path,
                        image_fs_type_t fs_type) {
  bool optimized_source = path_matches_root_or_child(file_path, "/data") ||
                          path_matches_root_or_child(file_path, "/user");
  bool path_selected_profile =
      fs_type == IMAGE_FS_UFS || fs_type == IMAGE_FS_EXFAT ||
      (fs_type == IMAGE_FS_PFS &&
       !pfs_path_is_nested_inner(file_path, fs_type));
  bool legacy = path_selected_profile && !optimized_source;

  return (image_mount_profile_t){
      .legacy = legacy,
      .nested_backing_cache = cfg->nested_pfs_index_cache_enabled,
  };
}

static bool should_prepare_nested_backing_cache(
    const image_mount_profile_t *profile, const char *path) {
  return profile->nested_backing_cache &&
         is_pfsc_image_mount_base_or_child(path);
}

static uint32_t get_lvd_sector_size_fallback(image_fs_type_t fs_type) {
  const runtime_config_t *cfg = runtime_config();
  switch (fs_type) {
  case IMAGE_FS_UFS:
    return cfg->lvd_sector_ufs;
  case IMAGE_FS_PFS:
  case IMAGE_FS_PFSC_CONTAINER:
    return cfg->lvd_sector_pfs;
  case IMAGE_FS_EXFAT:
  default:
    return cfg->lvd_sector_exfat;
  }
}

static uint32_t get_image_sector_size_override_or_default(
    const char *path, uint32_t fallback) {
  uint32_t override = 0;
  const char *filename = get_filename_component(path);
  if (filename && filename[0] != '\0' &&
      get_image_sector_size_override(filename, &override)) {
    return override;
  }
  return fallback;
}

static uint32_t get_lvd_sector_size(const char *path, image_fs_type_t fs_type) {
  bool nested_pfs_profile = pfs_path_uses_nested_profile(path, fs_type);
  uint32_t default_sector =
      nested_pfs_profile ? LVD_SECTOR_SIZE_PFS
                         : get_lvd_sector_size_fallback(fs_type);
  uint32_t fallback = get_image_sector_size_override_or_default(
      path, default_sector);
  if (nested_pfs_profile)
    return fallback;

  struct statfs sfs;
  if (statfs(path, &sfs) != 0)
    return fallback;

  uint64_t fs_cluster_size = (uint64_t)sfs.f_bsize;
  if (fs_cluster_size == 0)
    fs_cluster_size = (uint64_t)sfs.f_iosize;
  if (fs_cluster_size == 0 || fs_cluster_size >= (uint64_t)fallback)
    return fallback;

  return (uint32_t)fs_cluster_size;
}

static uint32_t get_lvd_secondary_unit(image_fs_type_t fs_type,
                                       uint32_t sector_size,
                                       bool legacy_mount) {
  if (legacy_mount && fs_type != IMAGE_FS_EXFAT)
    return sector_size;
  return LVD_SECONDARY_UNIT_IMAGE_IO % sector_size == 0u
             ? LVD_SECONDARY_UNIT_IMAGE_IO
             : sector_size;
}

static uint32_t get_md_sector_size(image_fs_type_t fs_type) {
  const runtime_config_t *cfg = runtime_config();
  uint32_t fallback = 0;
  switch (fs_type) {
  case IMAGE_FS_UFS:
    fallback = cfg->md_sector_ufs;
    break;
  case IMAGE_FS_EXFAT:
  default:
    fallback = cfg->md_sector_exfat;
    break;
  }
  return fallback;
}

static uint32_t get_md_sector_size_for_path(const char *path,
                                            image_fs_type_t fs_type) {
  return get_image_sector_size_override_or_default(path,
                                                   get_md_sector_size(fs_type));
}

static unsigned int get_md_attach_options(bool mount_read_only) {
  unsigned int options = MD_AUTOUNIT | MD_ASYNC;
  if (mount_read_only)
    options |= MD_READONLY;
  return options;
}

static uint16_t get_lvd_attach_raw_flags(image_fs_type_t fs_type,
                                         bool mount_read_only,
                                         bool legacy_mount) {
  if (legacy_mount && fs_type == IMAGE_FS_UFS) {
    return mount_read_only ? LVD_ATTACH_RAW_FLAGS_DD_RO
                           : LVD_ATTACH_RAW_FLAGS_DD_RW;
  }
  return mount_read_only ? LVD_ATTACH_RAW_FLAGS_SINGLE_RO
                         : LVD_ATTACH_RAW_FLAGS_SINGLE_RW;
}

static unsigned int get_nmount_flags(image_fs_type_t fs_type,
                                     bool mount_read_only, bool legacy_mount,
                                     const char **mount_mode_out) {
  if (legacy_mount && fs_type == IMAGE_FS_UFS) {
    if (mount_mode_out)
      *mount_mode_out = mount_read_only ? "legacy_dd_ro" : "legacy_dd_rw";
    return (mount_read_only ? MNT_RDONLY : 0u) | MNT_NOATIME;
  }
  if (mount_mode_out)
    *mount_mode_out = mount_read_only ? "rdonly" : "rw";
  return mount_read_only ? MNT_RDONLY : 0;
}

static uint16_t normalize_lvd_raw_flags(uint16_t raw_flags) {
  uint16_t flags = LVD_ATTACH_FLAG_BASE;

  flags |= raw_flags & LVD_ATTACH_RAW_PASSTHROUGH_MASK;
  if ((raw_flags & LVD_ATTACH_RAW_BIT_RO_VARIANT) != 0u)
    flags |= LVD_ATTACH_FLAG_RO_VARIANT;
  if ((raw_flags & LVD_ATTACH_RAW_BIT_INNER_PROFILE) != 0u)
    flags |= LVD_ATTACH_FLAG_INNER_PROFILE;
  if ((raw_flags & LVD_ATTACH_RAW_BIT_DOWNLOAD_DATA_PROFILE) != 0u)
    flags |= LVD_ATTACH_FLAG_DOWNLOAD_DATA_PROFILE;
  if ((raw_flags & LVD_ATTACH_RAW_BIT_SINGLE_SAVE_PROFILE) != 0u)
    flags |= LVD_ATTACH_FLAG_SINGLE_SAVE_PROFILE;

  return flags;
}

static uint16_t get_lvd_image_type(const char *path, image_fs_type_t fs_type,
                                   bool legacy_mount) {
  if (pfs_path_uses_nested_profile(path, fs_type)) {
    return pfs_lvd_game_image_type_from_selector(
        get_nested_pfs_img_type(path, fs_type));
  }

  if (fs_type == IMAGE_FS_UFS) {
    return legacy_mount ? LVD_ATTACH_IMAGE_TYPE_DOWNLOAD_DATA
                        : LVD_ATTACH_IMAGE_TYPE_SAVE_DATA;
  }
  if (fs_type == IMAGE_FS_EXFAT) {
    return legacy_mount ? LVD_ATTACH_IMAGE_TYPE_SINGLE
                        : LVD_ATTACH_IMAGE_TYPE_SAVE_DATA;
  }
  if (fs_type == IMAGE_FS_PFS)
    return LVD_ATTACH_IMAGE_TYPE_SAVE_DATA;
  return LVD_ATTACH_IMAGE_TYPE_SINGLE;
}

static uint16_t get_lvd_source_type(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))
      return LVD_ENTRY_TYPE_SPECIAL;
  }
  return LVD_ENTRY_TYPE_FILE;
}

static bool backing_fs_supports_pfs_metadata_cache(const char *fs_name) {
  if (!fs_name)
    return false;
  return strcmp(fs_name, "pfs") == 0 || strcmp(fs_name, "ppr_pfs") == 0 ||
         strcmp(fs_name, "transaction_pfs") == 0 ||
         strcmp(fs_name, "nullfs") == 0 || strcmp(fs_name, "unionfs") == 0;
}

static void prepare_nested_image_backing_cache(const char *file_path) {
  int fd = open(file_path, O_RDONLY);
  if (fd < 0) {
    log_debug("  [IMG][LVD] PFS backing cache open failed for %s: %s",
              file_path, strerror(errno));
    return;
  }

  struct statfs sfs;
  memset(&sfs, 0, sizeof(sfs));
  if (fstatfs(fd, &sfs) != 0) {
    log_debug("  [IMG][LVD] PFS backing cache statfs failed for %s: %s",
              file_path, strerror(errno));
    close(fd);
    return;
  }

  if (!backing_fs_supports_pfs_metadata_cache(sfs.f_fstypename)) {
    log_debug("  [IMG][LVD] PFS backing cache skipped for %s: backing fs=%s",
              file_path,
              sfs.f_fstypename[0] != '\0' ? sfs.f_fstypename : "unknown");
    close(fd);
    return;
  }

  pfs_gddr5_cache_request_t request;
  memset(&request, 0, sizeof(request));
  request.cache_cmp_offsets = 1;
  request.cache_full_icv = PFS_MOUNT_SIGVERIFY ? 1 : 0;

  if (ioctl(fd, PFS_GDDR5_CACHE_IOCTL, &request) != 0) {
    int cache_errno = errno;
    close(fd);
    if (cache_errno == EBUSY) {
      log_debug("  [IMG][LVD] PFS backing metadata cache busy for %s "
                "(already active or no free slot); continuing",
                file_path);
      return;
    }
    log_debug("  [IMG][LVD] PFS backing metadata cache unavailable for %s "
              "(fs=%s): %s",
              file_path, sfs.f_fstypename, strerror(cache_errno));
    return;
  }

  close(fd);
  log_debug("  [IMG][LVD] PFS backing metadata cache request accepted for %s "
            "(fs=%s cmp=1 icv=%d)",
            file_path, sfs.f_fstypename, PFS_MOUNT_SIGVERIFY ? 1 : 0);
}

// --- Image Path and Naming Helpers ---
static image_fs_type_t detect_image_fs_type(const char *name) {
  if (!name || name[0] == '\0')
    return IMAGE_FS_UNKNOWN;

  const char *dot = strrchr(name, '.');
  if (!dot)
    return IMAGE_FS_UNKNOWN;
  if (strcasecmp(dot, ".ffpkg") == 0)
    return IMAGE_FS_UFS;
  if (strcasecmp(dot, ".exfat") == 0)
    return IMAGE_FS_EXFAT;
  if (strcasecmp(dot, ".ffpfs") == 0)
    return IMAGE_FS_PFS;
  if (strcasecmp(dot, ".ffpfsc") == 0)
    return IMAGE_FS_PFSC_CONTAINER;
  return IMAGE_FS_UNKNOWN;
}

static bool is_nested_pfs_image_file_name(const char *name) {
  return name && strcasecmp(name, "pfs_image.dat") == 0;
}

static image_fs_type_t detect_image_fs_type_for_path(const char *path,
                                                     const char *name) {
  const char *filename = name;
  if ((!filename || filename[0] == '\0') && path)
    filename = get_filename_component(path);

  image_fs_type_t fs_type = detect_image_fs_type(filename);
  if (fs_type != IMAGE_FS_UNKNOWN)
    return fs_type;

  if (is_nested_pfs_image_file_name(filename) &&
      is_pfsc_image_mount_base_or_child(path)) {
    return IMAGE_FS_PFS;
  }

  return IMAGE_FS_UNKNOWN;
}

bool is_supported_image_file_name(const char *name) {
  return detect_image_fs_type(name) != IMAGE_FS_UNKNOWN;
}

bool is_supported_image_file_path(const char *full_path, const char *name) {
  return detect_image_fs_type_for_path(full_path, name) != IMAGE_FS_UNKNOWN;
}

static const char *image_fs_name(image_fs_type_t fs_type) {
  switch (fs_type) {
  case IMAGE_FS_UFS:
    return "ufs";
  case IMAGE_FS_EXFAT:
    return "exfatfs";
  case IMAGE_FS_PFS:
    return "pfs";
  case IMAGE_FS_PFSC_CONTAINER:
    return "pfsc";
  default:
    return "unknown";
  }
}

const char *image_fs_type_name_for_path(const char *path) {
  return image_fs_name(detect_image_fs_type_for_path(path, NULL));
}

void log_fs_stats(const char *tag, const char *path,
                  const char *type_hint) {
  struct statfs sfs;
  if (statfs(path, &sfs) != 0) {
    log_debug("  [%s] FS stats read failed for %s: %s", tag, path,
              strerror(errno));
    return;
  }

  const char *type_name = type_hint;
  if (sfs.f_fstypename[0] != '\0')
    type_name = sfs.f_fstypename;
  if (!type_name)
    type_name = "unknown";

  uint64_t bsize = (uint64_t)sfs.f_bsize;
  uint64_t iosize = (uint64_t)sfs.f_iosize;
  uint64_t blocks = (uint64_t)sfs.f_blocks;
  uint64_t bfree = (uint64_t)sfs.f_bfree;
  uint64_t bavail = (uint64_t)sfs.f_bavail;
  uint64_t files = (uint64_t)sfs.f_files;
  uint64_t ffree = (uint64_t)sfs.f_ffree;
  uint64_t total_bytes = blocks * bsize;
  uint64_t free_bytes = bfree * bsize;
  uint64_t avail_bytes = bavail * bsize;

  log_debug("  [%s] FS stats: path=%s type=%s bsize=%llu iosize=%llu "
            "blocks=%llu bfree=%llu bavail=%llu files=%llu ffree=%llu "
            "flags=0x%lX total=%lluB free=%lluB avail=%lluB",
            tag, path, type_name, (unsigned long long)bsize,
            (unsigned long long)iosize, (unsigned long long)blocks,
            (unsigned long long)bfree, (unsigned long long)bavail,
            (unsigned long long)files, (unsigned long long)ffree,
            (unsigned long)sfs.f_flags, (unsigned long long)total_bytes,
            (unsigned long long)free_bytes, (unsigned long long)avail_bytes);
}

static void strip_extension(const char *filename, char *out, size_t out_size) {
  const char *dot = strrchr(filename, '.');
  size_t len = dot ? (size_t)(dot - filename) : strlen(filename);
  if (len >= out_size)
    len = out_size - 1;
  memcpy(out, filename, len);
  out[len] = '\0';
}

static void build_image_mount_point_for_fs(const char *file_path,
                                           image_fs_type_t fs_type,
                                           char mount_point[MAX_PATH]) {
  const char *filename = get_filename_component(file_path);
  char base_name[MAX_PATH];
  char mount_name[MAX_PATH];
  strip_extension(filename, base_name, sizeof(base_name));

  size_t base_len = strlen(base_name);
  size_t max_base_len = sizeof(mount_name) - 1u - 9u;
  if (base_len > max_base_len)
    base_len = max_base_len;
  memcpy(mount_name, base_name, base_len);
  mount_name[base_len] = '\0';
  snprintf(mount_name + base_len, sizeof(mount_name) - base_len, "_%08x",
           sm_fnv1a32(file_path));

  const char *mount_base =
      fs_type == IMAGE_FS_PFSC_CONTAINER ? PFSC_IMAGE_MOUNT_BASE
                                         : IMAGE_MOUNT_BASE;
  snprintf(mount_point, MAX_PATH, "%s/%s", mount_base, mount_name);
}

static void build_image_mount_point(const char *file_path,
                                    char mount_point[MAX_PATH]) {
  build_image_mount_point_for_fs(
      file_path, detect_image_fs_type_for_path(file_path, NULL), mount_point);
}

void get_image_mount_point_for_source(const char *file_path,
                                      char mount_point[MAX_PATH]) {
  build_image_mount_point(file_path, mount_point);
}

static bool attach_md_backend(const char *file_path, image_fs_type_t fs_type,
                              bool mount_read_only, off_t file_size,
                              int *unit_id_out, char *devname_out,
                              size_t devname_size) {
  int md_fd = open(MD_CTRL_PATH, O_RDWR);
  if (md_fd < 0) {
    log_debug("  [IMG][%s] open %s failed: %s",
              attach_backend_name(ATTACH_BACKEND_MD), MD_CTRL_PATH,
              strerror(errno));
    return false;
  }

  struct md_ioctl req;
  memset(&req, 0, sizeof(req));
  req.md_version = MDIOVERSION;
  req.md_type = MD_VNODE;
  req.md_file = (char *)file_path;
  req.md_mediasize = file_size;
  req.md_sectorsize = get_md_sector_size_for_path(file_path, fs_type);
  req.md_options = get_md_attach_options(mount_read_only);

  int last_errno = 0;
  log_debug("  [IMG][%s] attach try: options=0x%x",
            attach_backend_name(ATTACH_BACKEND_MD), req.md_options);
  int ret = ioctl(md_fd, MDIOCATTACH, &req);
  if (ret != 0)
    last_errno = errno;
  close(md_fd);

  if (ret != 0) {
    errno = last_errno;
    log_debug("  [IMG][%s] attach failed: %s (ret: 0x%x)",
              attach_backend_name(ATTACH_BACKEND_MD), strerror(errno), ret);
    return false;
  }

  int unit_id = (int)req.md_unit;
  if (unit_id < 0) {
    log_debug("  [IMG][%s] attach returned invalid unit: %d",
              attach_backend_name(ATTACH_BACKEND_MD), unit_id);
    return false;
  }

  snprintf(devname_out, devname_size, "/dev/md%d", unit_id);
  log_debug("  [IMG][%s] attach returned unit=%d",
            attach_backend_name(ATTACH_BACKEND_MD), unit_id);
  *unit_id_out = unit_id;
  return true;
}

static bool attach_lvd_backend_as(
    const char *file_path, image_fs_type_t fs_type, bool mount_read_only,
    off_t file_size, int *unit_id_out, char *devname_out,
    size_t devname_size, const image_mount_profile_t *profile,
    uint16_t image_type) {
  // This ioctl caches metadata of the PFS vnode containing the image file.
  // The filesystem stored inside that file can be PFS, UFS, or exFAT.
  if (should_prepare_nested_backing_cache(profile, file_path)) {
    prepare_nested_image_backing_cache(file_path);
  }

  int lvd_fd = open(LVD_CTRL_PATH, O_RDWR);
  if (lvd_fd < 0) {
    log_debug("  [IMG][%s] open %s failed: %s",
              attach_backend_name(ATTACH_BACKEND_LVD), LVD_CTRL_PATH,
              strerror(errno));
    return false;
  }

  lvd_ioctl_layer_v0_t layers[LVD_ATTACH_LAYER_COUNT];
  memset(layers, 0, sizeof(layers));
  layers[0].source_type = get_lvd_source_type(file_path);
  layers[0].flags = LVD_ENTRY_FLAG_NO_BITMAP;
  layers[0].path = file_path;
  layers[0].offset = 0;
  layers[0].size = (uint64_t)file_size;

  uint32_t sector_size = get_lvd_sector_size(file_path, fs_type);
  uint32_t secondary_unit =
      get_lvd_secondary_unit(fs_type, sector_size, profile->legacy);
  uint16_t raw_flags =
      get_lvd_attach_raw_flags(fs_type, mount_read_only, profile->legacy);
  uint16_t normalized_flags = normalize_lvd_raw_flags(raw_flags);

  lvd_ioctl_attach_v0_t req;
  memset(&req, 0, sizeof(req));
  req.io_version = LVD_ATTACH_IO_VERSION_V0;
  req.image_type = image_type;
  req.layer_count = LVD_ATTACH_LAYER_COUNT;
  req.device_size = (uint64_t)file_size;
  req.layers_ptr = layers;
  req.sector_size = sector_size;
  req.secondary_unit = secondary_unit;
  req.flags = normalized_flags;
  req.device_id = -1;

  int last_errno = 0;
  log_debug("  [IMG][%s] attach try: ver=%u sec=%u sec2=%u raw=0x%x "
            "flags=0x%x img=%u",
            attach_backend_name(ATTACH_BACKEND_LVD), req.io_version,
            req.sector_size, req.secondary_unit, raw_flags, req.flags,
            req.image_type);
  int ret = ioctl(lvd_fd, SCE_LVD_IOC_ATTACH_V0, &req);
  if (ret != 0)
    last_errno = errno;
  close(lvd_fd);
  int unit_id = req.device_id;

  if (ret != 0) {
    errno = last_errno;
    log_debug("  [IMG][%s] attach failed: %s (ret: 0x%x)",
              attach_backend_name(ATTACH_BACKEND_LVD), strerror(errno), ret);
    return false;
  }

  if (unit_id < 0) {
    log_debug("  [IMG][%s] attach returned invalid unit: %d",
              attach_backend_name(ATTACH_BACKEND_LVD), unit_id);
    return false;
  }
  log_debug("  [IMG][%s] attach returned unit=%d",
            attach_backend_name(ATTACH_BACKEND_LVD), unit_id);

  snprintf(devname_out, devname_size, "/dev/lvd%d", unit_id);
  *unit_id_out = unit_id;
  return true;
}

static bool get_cached_image_mount(const char *file_path,
                                   image_cache_entry_t *entry_out,
                                   int *index_out) {
  image_cache_entry_t entry;
  int index = -1;
  if (!find_image_cache_entry(file_path, &entry, &index) ||
      entry.backend == ATTACH_BACKEND_NONE || entry.unit_id < 0) {
    return false;
  }
  if (entry_out)
    *entry_out = entry;
  if (index_out)
    *index_out = index;
  return true;
}

static bool directory_has_visible_entries(const char *path) {
  DIR *dir = opendir(path);
  if (!dir)
    return false;

  bool found = false;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;
    found = true;
    break;
  }
  closedir(dir);
  return found;
}

static bool is_image_mount_root_accessible(const char *mount_point,
                                           int *error_out) {
  if (error_out)
    *error_out = 0;

  DIR *dir = opendir(mount_point);
  if (!dir) {
    if (error_out)
      *error_out = errno;
    return false;
  }
  closedir(dir);
  return true;
}

static bool reject_mounted_image_io(const char *file_path,
                                    uint64_t generation,
                                    attach_backend_t attach_backend,
                                    const char *devname,
                                    const char *mount_point, int io_err,
                                    const char *stage) {
  sm_error_set_l10n("IMG", io_err, file_path,
                    SM_L10N_IMAGE_UNREADABLE_DAMAGED);
  log_debug("  [IMG][%s] unreadable or damaged mount (%s -> %s, %s): %s",
            attach_backend_name(attach_backend), devname, mount_point, stage,
            strerror(io_err));
  (void)release_runtime_image_attachment(file_path, generation);
  errno = io_err;
  return false;
}

static bool prepare_image_mount_retry(const image_cache_entry_t *cached_entry,
                                      char mount_point[MAX_PATH],
                                      char source_path[MAX_PATH]) {
  build_image_mount_point(cached_entry->path, mount_point);
  (void)strlcpy(source_path, cached_entry->path, MAX_PATH);

  if (is_active_image_mount_point(mount_point)) {
    int root_err = 0;
    if (is_image_mount_root_accessible(mount_point, &root_err))
      return false;

    log_debug("  [IMG][%s] mount unreadable, retrying: %s -> %s: %s",
              attach_backend_name(cached_entry->backend), source_path,
              mount_point, strerror(root_err));
  } else {
    log_debug("  [IMG][%s] mount lost, retrying: %s -> %s",
              attach_backend_name(cached_entry->backend), source_path,
              mount_point);
    clear_cached_game(mount_point);
  }

  if (!release_runtime_image_attachment(cached_entry->path,
                                        cached_entry->generation))
    return false;
  clear_missing_param_entry(mount_point);
  return true;
}

static bool reuse_existing_image_mount(const char *file_path,
                                       const char *mount_point,
                                       bool *cache_failed_out) {
  if (cache_failed_out)
    *cache_failed_out = false;

  struct stat mount_st;
  if (stat(mount_point, &mount_st) != 0 || !S_ISDIR(mount_st.st_mode))
    return false;

  attach_backend_t existing_backend = ATTACH_BACKEND_NONE;
  int existing_unit = -1;
  if (resolve_device_from_mount(mount_point, &existing_backend, &existing_unit)) {
    int root_err = 0;
    if (!is_image_mount_root_accessible(mount_point, &root_err)) {
      log_debug("  [IMG][%s] Existing mount unreadable, reattaching: %s -> %s: %s",
                attach_backend_name(existing_backend), file_path, mount_point,
                strerror(root_err));
      if (!cache_image_mount(file_path, mount_point, existing_unit,
                             existing_backend) ||
          !release_runtime_image_mount(file_path)) {
        if (cache_failed_out)
          *cache_failed_out = true;
        return false;
      }
      return false;
    }
    if (!cache_image_mount(file_path, mount_point, existing_unit,
                           existing_backend)) {
      sm_error_set_l10n("IMG", ENOSPC, file_path,
                        SM_L10N_IMAGE_CACHE_FULL_TRACK,
                        (unsigned)MAX_IMAGE_MOUNTS, mount_point);
      log_debug("  [IMG] image cache full, refusing unmanaged mount reuse: %s",
                mount_point);
      errno = ENOSPC;
      if (cache_failed_out)
        *cache_failed_out = true;
      return false;
    }
    log_debug("  [IMG][%s] Already mounted: %s",
              attach_backend_name(existing_backend), mount_point);
    return true;
  }

  if (!directory_has_visible_entries(mount_point))
    return false;

  log_debug("  [IMG] Mount point exists and is non-empty but is not an active "
            "mount, reattaching: %s", mount_point);
  return false;
}

static bool handle_cached_or_existing_image_mount(
    const char *file_path, const char *mount_point, bool *success_out) {
  if (success_out)
    *success_out = false;

  if (runtime_sleep_mode_active())
    return true;

  image_cache_entry_t cached_entry;
  if (get_cached_image_mount(file_path, &cached_entry, NULL)) {
    if (cached_entry.state == ATTACHED_DEVICE_DETACH_REQUESTED) {
      log_debug("  [IMG][%s] detach still pending, mount deferred: %s",
                attach_backend_name(cached_entry.backend), file_path);
      (void)release_runtime_image_mount(file_path);
      return true;
    }
    if (cached_entry.state == ATTACHED_DEVICE_ATTACHING)
      return true;

    int root_err = 0;
    if (is_active_image_mount_point(mount_point) &&
        is_image_mount_root_accessible(mount_point, &root_err)) {
      if (success_out)
        *success_out = true;
      return true;
    }

    log_debug("  [IMG][%s] cached mount is not active, reattaching: %s -> "
              "%s",
              attach_backend_name(cached_entry.backend), file_path,
              mount_point);
    if (!release_runtime_image_attachment(cached_entry.path,
                                          cached_entry.generation)) {
      return true;
    }
  }

  bool cache_failed = false;
  if (reuse_existing_image_mount(file_path, mount_point, &cache_failed)) {
    if (success_out)
      *success_out = !runtime_sleep_mode_active();
    return true;
  }

  return cache_failed;
}

static bool stat_image_file(const char *file_path, struct stat *st_out) {
  if (stat(file_path, st_out) != 0) {
    log_debug("  [IMG] stat failed for %s: %s", file_path, strerror(errno));
    return false;
  }
  if (st_out->st_size < 0) {
    log_debug("  [IMG] invalid file size for %s: %lld", file_path,
              (long long)st_out->st_size);
    errno = EINVAL;
    return false;
  }
  return true;
}

static void ensure_mount_dirs(const char *mount_point) {
  mkdir(IMAGE_MOUNT_BASE, 0777);
  if (is_pfsc_image_mount_base_or_child(mount_point))
    mkdir(PFSC_IMAGE_MOUNT_BASE, 0777);
  mkdir(mount_point, 0777);
}

static attach_backend_t select_image_backend(const runtime_config_t *cfg,
                                             image_fs_type_t fs_type) {
  if (fs_type == IMAGE_FS_EXFAT)
    return cfg->exfat_backend;
  if (fs_type == IMAGE_FS_UFS)
    return cfg->ufs_backend;
  return ATTACH_BACKEND_LVD;
}

static bool attach_image_device(
    const char *file_path, image_fs_type_t fs_type, bool mount_read_only,
    off_t file_size, attach_backend_t attach_backend, int *unit_id_out,
    char *devname_out, size_t devname_size,
    const image_mount_profile_t *profile, uint16_t lvd_image_type,
    uint64_t generation) {
  bool attached = false;
  if (attach_backend == ATTACH_BACKEND_MD) {
    attached = attach_md_backend(file_path, fs_type, mount_read_only, file_size,
                                 unit_id_out, devname_out, devname_size);
  } else if (attach_backend == ATTACH_BACKEND_LVD) {
    attached = attach_lvd_backend_as(
        file_path, fs_type, mount_read_only, file_size, unit_id_out,
        devname_out, devname_size, profile, lvd_image_type);
  } else {
    log_debug("  [IMG] unsupported attach backend for %s", file_path);
    errno = EINVAL;
  }

  if (!attached) {
    int attach_errno = errno;
    (void)cancel_image_attachment(file_path, generation);
    errno = attach_errno;
    return false;
  }

  if (!publish_image_attachment(file_path, generation, *unit_id_out,
                                attach_backend)) {
    log_debug("  [IMG][%s] failed to publish attached unit: unit=%d source=%s",
              attach_backend_name(attach_backend), *unit_id_out, file_path);
    (void)cancel_image_attachment(file_path, generation);
    rollback_unpublished_attached_unit(attach_backend, *unit_id_out,
                                       devname_out);
    errno = EIO;
    return false;
  }

  if (!wait_for_dev_node_state(devname_out, true)) {
    log_debug("  [IMG][%s] device node did not appear: %s",
              attach_backend_name(attach_backend), devname_out);
    (void)release_runtime_image_attachment(file_path, generation);
    errno = ETIMEDOUT;
    return false;
  }

  log_debug("  [IMG][%s] Attached as %s", attach_backend_name(attach_backend),
            devname_out);
  return true;
}

static bool verify_exfat_nmount_result(const char *file_path,
                                       uint64_t generation,
                                       attach_backend_t attach_backend,
                                       const char *devname,
                                       const char *mount_point) {
  struct statfs sfs;
  int verify_errno = EINVAL;
  if (statfs(mount_point, &sfs) == 0) {
    if (strcmp(sfs.f_fstypename, "exfatfs") == 0 &&
        strcmp(sfs.f_mntfromname, devname) == 0 &&
        strcmp(sfs.f_mntonname, mount_point) == 0) {
      return true;
    }

    log_debug("  [IMG][%s] nmount did not produce the expected exFAT mount: "
              "from=%s path=%s type=%s",
              attach_backend_name(attach_backend), sfs.f_mntfromname,
              sfs.f_mntonname, sfs.f_fstypename);
  } else {
    verify_errno = errno;
    log_debug("  [IMG][%s] exFAT mount verification failed for %s -> %s: %s",
              attach_backend_name(attach_backend), devname, mount_point,
              strerror(verify_errno));
  }

  bool released = release_runtime_image_attachment(file_path, generation);
  int result_errno = (!released && errno != 0) ? errno : verify_errno;
  errno = result_errno;
  return false;
}

static bool perform_image_nmount(const char *file_path, uint64_t generation,
                                 image_fs_type_t fs_type,
                                 attach_backend_t attach_backend,
                                 const char *devname, const char *mount_point,
                                 bool mount_read_only, bool force_mount,
                                 const image_mount_profile_t *profile) {
  struct iovec *iov = NULL;
  unsigned int iovlen = 0;
  char mount_errmsg[256];
  memset(mount_errmsg, 0, sizeof(mount_errmsg));
  const char *sigverify = PFS_MOUNT_SIGVERIFY ? "1" : "0";
  const char *playgo = PFS_MOUNT_PLAYGO ? "1" : "0";
  const char *disc = PFS_MOUNT_DISC ? "1" : "0";
  const char *finalized = "0";
  const char *pubkey_ver = "0";
  const char *key_ver = "0";
  const char *ekpfs_key = PFS_ZERO_EKPFS_KEY_HEX;
  uint8_t nested_img_type = 0;
  bool nested_pfs_profile = pfs_path_uses_nested_profile(file_path, fs_type);
  if (nested_pfs_profile) {
    nested_img_type = get_nested_pfs_img_type(file_path, fs_type);
    // Temporarily disabled: keep nested PFS signature option wired but do not
    // force sigverify=1.
    // sigverify = "1";
    disc = (nested_img_type & 0x40u) ? "1" : "0";
    finalized = (nested_img_type & 0x20u) ? "1" : "0";
  }

  struct iovec iov_ufs[] = {
      IOVEC_ENTRY("fstype"),     IOVEC_ENTRY("ufs"), 
      IOVEC_ENTRY("from"),       IOVEC_ENTRY(devname),
      IOVEC_ENTRY("fspath"),     IOVEC_ENTRY(mount_point),  
      IOVEC_ENTRY("budgetid"),   IOVEC_ENTRY(DEVPFS_BUDGET_GAME),
      IOVEC_ENTRY("async"),      IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("noatime"),    IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("automounted"), IOVEC_ENTRY(NULL),
//      IOVEC_ENTRY("compressedfile"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("errmsg"),     {(void *)mount_errmsg, sizeof(mount_errmsg)},
      IOVEC_ENTRY("force"),      IOVEC_ENTRY(NULL)};

  struct iovec iov_exfat[] = {
      IOVEC_ENTRY("from"),       IOVEC_ENTRY(devname),
      IOVEC_ENTRY("fspath"),     IOVEC_ENTRY(mount_point),
      IOVEC_ENTRY("fstype"),     IOVEC_ENTRY("exfatfs"),
      IOVEC_ENTRY("budgetid"),   IOVEC_ENTRY(DEVPFS_BUDGET_GAME),
      IOVEC_ENTRY("large"),      IOVEC_ENTRY("yes"),
      IOVEC_ENTRY("timezone"),   IOVEC_ENTRY("static"),
      IOVEC_ENTRY("async"),      IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("noatime"),    IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("ignoreacl"),  IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("automounted"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("errmsg"),     {(void *)mount_errmsg, sizeof(mount_errmsg)},
      IOVEC_ENTRY("force"),      IOVEC_ENTRY(NULL)};

  struct iovec iov_pfs[] = {
      IOVEC_ENTRY("from"),       IOVEC_ENTRY(devname),
      IOVEC_ENTRY("fspath"),     IOVEC_ENTRY(mount_point),
      IOVEC_ENTRY("fstype"),     IOVEC_ENTRY("pfs"),
      IOVEC_ENTRY("sigverify"),  IOVEC_ENTRY(sigverify),
      IOVEC_ENTRY("mkeymode"),   IOVEC_ENTRY(PFS_MOUNT_MKEYMODE),
      IOVEC_ENTRY("budgetid"),   IOVEC_ENTRY(PFS_MOUNT_BUDGET_ID),
      IOVEC_ENTRY("playgo"),     IOVEC_ENTRY(playgo),
      IOVEC_ENTRY("disc"),       IOVEC_ENTRY(disc),
      IOVEC_ENTRY("ekpfs"),      IOVEC_ENTRY(ekpfs_key),
      IOVEC_ENTRY("async"),      IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("noatime"),    IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("automounted"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("errmsg"),     {(void *)mount_errmsg, sizeof(mount_errmsg)},
      IOVEC_ENTRY("force"),      IOVEC_ENTRY(NULL)};

  struct iovec iov_nested_pfs[] = {
      IOVEC_ENTRY("from"),       IOVEC_ENTRY(devname),
      IOVEC_ENTRY("fspath"),     IOVEC_ENTRY(mount_point),
      IOVEC_ENTRY("fstype"),     IOVEC_ENTRY("pfs"),
      IOVEC_ENTRY("sigverify"),  IOVEC_ENTRY(sigverify),
      IOVEC_ENTRY("mkeymode"),   IOVEC_ENTRY(PFS_MOUNT_MKEYMODE),
      IOVEC_ENTRY("budgetid"),   IOVEC_ENTRY(PFS_MOUNT_BUDGET_ID),
      IOVEC_ENTRY("playgo"),     IOVEC_ENTRY(playgo),
      IOVEC_ENTRY("disc"),       IOVEC_ENTRY(disc),
//      IOVEC_ENTRY("pubkey_ver"), IOVEC_ENTRY(pubkey_ver),
//      IOVEC_ENTRY("key_ver"),    IOVEC_ENTRY(key_ver),
//      IOVEC_ENTRY("finalized"),  IOVEC_ENTRY(finalized),
      IOVEC_ENTRY("ekpfs"),      IOVEC_ENTRY(ekpfs_key),
      IOVEC_ENTRY("async"),      IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("noatime"),    IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("automounted"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("errmsg"),     {(void *)mount_errmsg, sizeof(mount_errmsg)},
      IOVEC_ENTRY("force"),      IOVEC_ENTRY(NULL)};

  if (fs_type == IMAGE_FS_UFS) {
    iov = iov_ufs;
    iovlen = (unsigned int)IOVEC_SIZE(iov_ufs) - (force_mount ? 0u : 2u);
  } else if (fs_type == IMAGE_FS_EXFAT) {
    iov = iov_exfat;
    iovlen = (unsigned int)IOVEC_SIZE(iov_exfat) - (force_mount ? 0u : 2u);
  } else if (image_fs_type_is_pfs(fs_type)) {
    if (nested_pfs_profile) {
      log_debug("  [IMG][%s] PFS ro=%d budgetid=%s mkeymode=%s "
                "sigverify=%s playgo=%s disc=%s finalized=%s "
                "pubkey_ver=%s key_ver=%s",
                attach_backend_name(attach_backend), mount_read_only ? 1 : 0,
                PFS_MOUNT_BUDGET_ID, PFS_MOUNT_MKEYMODE, sigverify, playgo,
                disc, finalized, pubkey_ver, key_ver);
      log_debug("  [IMG][%s] nested PFS profile: selector=0x%02x "
                "lvd_img=%u gddr5=%d",
                attach_backend_name(attach_backend), nested_img_type,
                get_lvd_image_type(file_path, fs_type, profile->legacy),
                should_prepare_nested_backing_cache(profile, file_path));
      iov = iov_nested_pfs;
      iovlen =
          (unsigned int)IOVEC_SIZE(iov_nested_pfs) - (force_mount ? 0u : 2u);
    } else {
      log_debug("  [IMG][%s] PFS ro=%d budgetid=%s mkeymode=%s "
                "sigverify=%s playgo=%s disc=%s ekpfs=zero",
                attach_backend_name(attach_backend), mount_read_only ? 1 : 0,
                PFS_MOUNT_BUDGET_ID, PFS_MOUNT_MKEYMODE, sigverify, playgo,
                disc);
      iov = iov_pfs;
      iovlen = (unsigned int)IOVEC_SIZE(iov_pfs) - (force_mount ? 0u : 2u);
    }
  } else {
    log_debug("  [IMG][%s] unsupported fstype=%s",
              attach_backend_name(attach_backend), image_fs_name(fs_type));
    (void)release_runtime_image_attachment(file_path, generation);
    errno = EINVAL;
    return false;
  }

  const char *mount_mode = NULL;
  unsigned int mount_flags =
      get_nmount_flags(fs_type, mount_read_only, profile->legacy, &mount_mode);
  if (nmount(iov, iovlen, (int)mount_flags) == 0) {
    return fs_type != IMAGE_FS_EXFAT ||
           verify_exfat_nmount_result(file_path, generation, attach_backend,
                                      devname, mount_point);
  }

  int mount_errno = errno;
  if (mount_errmsg[0] != '\0') {
    sm_error_set("IMG", mount_errno, file_path, "%s", mount_errmsg);
    log_debug("  [IMG][%s] nmount %s errmsg: %s",
              attach_backend_name(attach_backend), mount_mode, mount_errmsg);
  }
  log_debug("  [IMG][%s] nmount %s failed: %s",
            attach_backend_name(attach_backend), mount_mode,
            strerror(mount_errno));
  (void)release_runtime_image_attachment(file_path, generation);
  errno = mount_errno;
  return false;
}

static bool validate_mounted_image(const char *file_path, uint64_t generation,
                                   image_fs_type_t fs_type,
                                   attach_backend_t attach_backend,
                                   const char *devname,
                                   const char *mount_point) {
  struct statfs mounted_sfs;
  if (statfs(mount_point, &mounted_sfs) != 0) {
    return reject_mounted_image_io(file_path, generation, attach_backend,
                                   devname, mount_point, errno, "statfs");
  }

  uint32_t min_device_sector =
      (attach_backend == ATTACH_BACKEND_MD)
          ? get_md_sector_size_for_path(file_path, fs_type)
          : get_lvd_sector_size(file_path, fs_type);
  uint64_t fs_block_size = (uint64_t)mounted_sfs.f_bsize;
  if (fs_block_size < (uint64_t)min_device_sector) {
    uint32_t tuned_sector_size = 0;
    bool autotuned =
        fs_block_size <= UINT32_MAX &&
        upsert_image_sector_size_autotune(get_filename_component(file_path),
                                          (uint32_t)fs_block_size,
                                          &tuned_sector_size);
    sm_error_set_l10n("IMG", EINVAL, file_path, SM_L10N_FS_CLUSTER_SMALL,
                      (unsigned long long)fs_block_size, min_device_sector,
                      devname);
    log_debug("  [IMG][%s] %s", attach_backend_name(attach_backend),
              sm_last_error()->message);
    if (autotuned) {
      log_debug("  [CFG] image sector autotuned: %s=%u",
                get_filename_component(file_path), tuned_sector_size);
      notify_system_l10n(SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED, file_path,
                         (unsigned long long)fs_block_size, min_device_sector,
                         tuned_sector_size);
    } else {
      notify_system_l10n(SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG, file_path,
                         (unsigned long long)fs_block_size, min_device_sector);
    }
    sm_error_mark_notified();
    (void)release_runtime_image_attachment(file_path, generation);
    errno = EINVAL;
    return false;
  }

  int root_err = 0;
  if (!is_image_mount_root_accessible(mount_point, &root_err)) {
    return reject_mounted_image_io(file_path, generation, attach_backend,
                                   devname, mount_point, root_err,
                                   "root access");
  }

  return true;
}

// --- Image Attach + nmount Pipeline ---
bool mount_image_with_mode(const char *file_path, image_fs_type_t fs_type,
                           const bool *mount_read_only_override) {
  sm_error_clear();
  const runtime_config_t *cfg = runtime_config();
  bool mount_read_only = cfg->mount_read_only;
  bool mount_mode_overridden = false;
  bool request_mode_overridden = mount_read_only_override != NULL;
  bool force_mount = cfg->force_mount;
  attach_backend_t attach_backend = select_image_backend(cfg, fs_type);
  const char *filename = get_filename_component(file_path);
  if (filename[0] != '\0')
    mount_mode_overridden =
        get_image_mode_override(filename, &mount_read_only);
  bool mount_mode_forced = pfs_path_uses_nested_profile(file_path, fs_type);
  if (mount_read_only_override && mount_mode_forced &&
      !*mount_read_only_override) {
    errno = ENOTSUP;
    return false;
  }
  if (mount_read_only_override) {
    mount_read_only = *mount_read_only_override;
    mount_mode_overridden = true;
  }
  if (mount_mode_forced)
    mount_read_only = true;

  if (runtime_sleep_mode_active())
    return false;

  char mount_point[MAX_PATH];
  build_image_mount_point_for_fs(file_path, fs_type, mount_point);

  bool success = false;
  bool handled =
      handle_cached_or_existing_image_mount(file_path, mount_point, &success);
  if (handled && success && mount_read_only_override) {
    struct statfs mounted_fs;
    if (statfs(mount_point, &mounted_fs) != 0 ||
        ((mounted_fs.f_flags & MNT_RDONLY) != 0) != mount_read_only) {
      errno = EBUSY;
      return false;
    }
  }
  if (handled && !success && mount_read_only_override && errno != ENOSPC)
    errno = EBUSY;
  if (handled)
    return success;

  struct stat st;
  if (!stat_image_file(file_path, &st))
    return false;

  image_mount_profile_t profile =
      get_image_mount_profile(cfg, file_path, fs_type);
  uint16_t image_type =
      get_lvd_image_type(file_path, fs_type, profile.legacy);

  uint64_t attachment_generation = 0;
  runtime_mount_state_lock();
  if (runtime_sleep_mode_active()) {
    runtime_mount_state_unlock();
    return false;
  }

  image_cache_entry_t current_entry;
  if (find_image_cache_entry(file_path, &current_entry, NULL) &&
      current_entry.state == ATTACHED_DEVICE_MOUNTED) {
    runtime_mount_state_unlock();
    if (mount_read_only_override) {
      struct statfs mounted_fs;
      if (statfs(mount_point, &mounted_fs) != 0 ||
          ((mounted_fs.f_flags & MNT_RDONLY) != 0) != mount_read_only) {
        errno = EBUSY;
        return false;
      }
    }
    return true;
  }
  if (!begin_image_attachment(file_path, mount_point,
                              &attachment_generation)) {
    int reserve_errno = errno;
    runtime_mount_state_unlock();
    log_debug("  [IMG] cannot reserve attachment record for %s: %s",
              file_path, strerror(reserve_errno));
    errno = reserve_errno;
    return false;
  }
  runtime_mount_state_unlock();

  log_debug("  [IMG] Mounting image (%s, %s): %s -> %s",
            image_fs_name(fs_type),
            profile.legacy ? "legacy-1.6" : "optimized-1.7", file_path,
            mount_point);
  if (mount_mode_overridden) {
    log_debug("  [%s] Image mode override: %s -> %s",
              request_mode_overridden ? "API" : "CFG", file_path,
              mount_read_only ? "ro" : "rw");
  }

  ensure_mount_dirs(mount_point);

  log_debug("  [IMG][%s] attach backend selected for %s",
            attach_backend_name(attach_backend), file_path);

  int unit_id = -1;
  char devname[64];
  memset(devname, 0, sizeof(devname));
  if (!attach_image_device(file_path, fs_type, mount_read_only, st.st_size,
                           attach_backend, &unit_id, devname, sizeof(devname),
                           &profile, image_type, attachment_generation)) {
    return false;
  }
  if (runtime_sleep_mode_active()) {
    (void)release_runtime_image_attachment(file_path, attachment_generation);
    return false;
  }
  if (!perform_image_nmount(file_path, attachment_generation, fs_type,
                            attach_backend, devname, mount_point,
                            mount_read_only, force_mount, &profile)) {
    return false;
  }
  if (runtime_sleep_mode_active()) {
    (void)release_runtime_image_attachment(file_path, attachment_generation);
    return false;
  }

  if (!validate_mounted_image(file_path, attachment_generation, fs_type,
                              attach_backend, devname, mount_point)) {
    return false;
  }

  runtime_mount_state_lock();
  bool sleep_active = runtime_sleep_mode_active();
  bool committed =
      !sleep_active &&
      complete_image_attachment(file_path, attachment_generation, unit_id,
                                attach_backend);
  runtime_mount_state_unlock();
  if (!committed) {
    log_debug("  [IMG][%s] failed to publish mounted attachment: unit=%d "
              "source=%s",
              attach_backend_name(attach_backend), unit_id, file_path);
    (void)release_runtime_image_attachment(file_path, attachment_generation);
    errno = sleep_active ? ECANCELED : EAGAIN;
    return false;
  }

  log_debug("  [IMG][%s] Mounted (%s) %s -> %s",
            attach_backend_name(attach_backend), image_fs_name(fs_type),
            devname, mount_point);
  log_fs_stats("IMG", mount_point, image_fs_name(fs_type));
  return true;
}

bool mount_image(const char *file_path, image_fs_type_t fs_type) {
  return mount_image_with_mode(file_path, fs_type, NULL);
}

static bool unmount_child_image_mounts_for_container(
    const char *mount_point, bool cleanup_source_state) {
  bool all_unmounted = true;

  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    image_cache_entry_t cached_entry;
    if (!get_image_cache_entry(k, &cached_entry))
      continue;
    if (!path_matches_root_or_child(cached_entry.path, mount_point))
      continue;
    if (cached_entry.state == ATTACHED_DEVICE_ATTACHING) {
      all_unmounted = false;
      errno = EBUSY;
      continue;
    }
    if (cached_entry.backend == ATTACH_BACKEND_NONE ||
        cached_entry.unit_id < 0) {
      continue;
    }
    log_debug("  [IMG] container child unmount: %s source=%s",
              mount_point, cached_entry.path);
    unmount_result_t result =
        cleanup_source_state
            ? unmount_image(cached_entry.path, cached_entry.unit_id,
                            cached_entry.backend)
            : unmount_runtime_image(cached_entry.path, cached_entry.unit_id,
                                    cached_entry.backend);
    if (unmount_completed(result)) {
      (void)invalidate_matching_image_cache_entry(k, &cached_entry);
      continue;
    }

    all_unmounted = false;
    if (result.error != 0)
      errno = result.error;
    log_debug("  [IMG] container child unmount pending: %s source=%s",
              mount_point, cached_entry.path);
  }

  return all_unmounted;
}

static bool detach_cached_image_device(const char *file_path,
                                       uint64_t generation,
                                       attach_backend_t backend, int unit_id) {
  image_cache_entry_t cached_entry;
  if (!claim_image_cache_detach(file_path, generation, unit_id, backend,
                                &cached_entry))
    return false;

  attached_unit_detach_state_t detach_state = cached_entry.detach_state;
  bool detach_was_requested = detach_state.requested;

  bool detached = detach_attached_unit(backend, unit_id, &detach_state);
  int detach_errno = errno;

  if (!finish_image_cache_detach_attempt(
          file_path, cached_entry.generation, unit_id, backend,
          &detach_state)) {
    log_debug("  [IMG][%s] failed to finish detach attempt: unit=%d "
              "source=%s",
              attach_backend_name(backend), unit_id, file_path);
    errno = ESTALE;
    return false;
  }

  if (detached)
    return true;
  if (!detach_was_requested && detach_state.requested) {
    return wait_for_attached_unit_release(backend, unit_id, &detach_state);
  }

  errno = detach_errno;
  return false;
}

static void log_image_unmount_result(const char *file_path,
                                     const char *mount_point,
                                     attach_backend_t backend, int unit_id,
                                     unmount_result_t result) {
  const char *status = "unmount complete";
  if (!result.filesystem_released)
    status = "filesystem release pending";
  else if (!result.device_released)
    status = "filesystem released; device detach pending";
  else if (!result.directory_removed)
    status = "filesystem and device released; mount directory remains";
  log_debug("  [IMG][%s] %s: source=%s mount=%s unit=%d error=%d",
            attach_backend_name(backend), status, file_path, mount_point,
            unit_id, result.error);
}

static unmount_result_t unmount_image_impl(const char *file_path, int unit_id,
                                           attach_backend_t backend,
                                           bool cleanup_source_state,
                                           uint64_t expected_generation) {
  unmount_result_t result = {0};

  char mount_point[MAX_PATH];
  image_fs_type_t fs_type = detect_image_fs_type_for_path(file_path, NULL);
  build_image_mount_point_for_fs(file_path, fs_type, mount_point);
  int resolved_unit = unit_id;
  attach_backend_t resolved_backend = backend;
  uint64_t tracked_generation = 0;

  if (resolved_unit < 0 || resolved_backend == ATTACH_BACKEND_NONE) {
    if (!resolve_device_from_mount(mount_point, &resolved_backend,
                                   &resolved_unit)) {
      resolved_backend = ATTACH_BACKEND_NONE;
      resolved_unit = -1;
    }
  }

  image_cache_entry_t tracked_entry;
  bool has_tracked_device =
      find_image_cache_entry(file_path, &tracked_entry, NULL) &&
      tracked_entry.backend != ATTACH_BACKEND_NONE &&
      tracked_entry.unit_id >= 0;
  if (has_tracked_device) {
    if ((expected_generation != 0 &&
         tracked_entry.generation != expected_generation) ||
        tracked_entry.unit_id != resolved_unit ||
        tracked_entry.backend != resolved_backend) {
      result.error = ESTALE;
      errno = result.error;
      return result;
    }
    if (tracked_entry.state == ATTACHED_DEVICE_ATTACHING &&
        expected_generation == 0) {
      result.error = EBUSY;
      errno = result.error;
      return result;
    }
    if (!mark_image_cache_detach_requested(
            tracked_entry.path, tracked_entry.generation,
            tracked_entry.unit_id, tracked_entry.backend)) {
      result.error = errno != 0 ? errno : ESTALE;
      log_debug("  [IMG][%s] unmount deferred before detach reservation: "
                "source=%s unit=%d error=%s",
                attach_backend_name(tracked_entry.backend), tracked_entry.path,
                tracked_entry.unit_id, strerror(result.error));
      errno = result.error;
      return result;
    }
    tracked_generation = tracked_entry.generation;
  } else if (resolved_backend != ATTACH_BACKEND_NONE && resolved_unit >= 0) {
    result.error = ESTALE;
    errno = result.error;
    return result;
  }

  log_debug("  [IMG][%s] unmount start: source=%s mount=%s unit=%d",
            attach_backend_name(resolved_backend), file_path, mount_point,
            resolved_unit);

  if (fs_type == IMAGE_FS_PFSC_CONTAINER &&
      !unmount_child_image_mounts_for_container(mount_point,
                                                cleanup_source_state)) {
    result.error = errno != 0 ? errno : EBUSY;
    errno = result.error;
    return result;
  }

  if (cleanup_source_state) {
    // Permanent source cleanup removes title links before releasing the image.
    cleanup_mount_links_for_source_unmount(mount_point);
    clear_cached_game(mount_point);
  }

  // Unmount stacked layers (unionfs over image fs).
  for (int i = 0; i < MAX_LAYERED_UNMOUNT_ATTEMPTS; i++) {
    if (!is_active_image_mount_point(mount_point))
      break;
    if (unmount(mount_point, 0) == 0)
      continue;
    int unmount_errno = errno;
    if (unmount_errno == ENOENT || unmount_errno == EINVAL)
      break;
    log_debug("  [IMG][%s] unmount deferred for %s: %s",
              attach_backend_name(resolved_backend), mount_point,
              strerror(unmount_errno));
    result.error = unmount_errno;
    errno = result.error;
    return result;
  }

  if (is_active_image_mount_point(mount_point)) {
    log_debug("  [IMG][%s] unmount incomplete for %s",
              attach_backend_name(resolved_backend), mount_point);
    result.error = EBUSY;
    errno = result.error;
    return result;
  }
  result.filesystem_released = true;

  result.device_released = true;
  if (resolved_backend != ATTACH_BACKEND_NONE && resolved_unit >= 0) {
    result.device_released = detach_cached_image_device(
        file_path, tracked_generation, resolved_backend, resolved_unit);
    if (!result.device_released)
      result.error = errno != 0 ? errno : EIO;
  }

  if (rmdir(mount_point) == 0) {
    result.directory_removed = true;
    log_debug("  [IMG] Removed mount directory: %s", mount_point);
  } else {
    int directory_error = errno;
    if (directory_error == ENOENT) {
      result.directory_removed = true;
    } else {
      log_debug("  [IMG] Mount directory not removed (%s): %s",
                strerror(directory_error), mount_point);
      if (result.error == 0)
        result.error = directory_error;
    }
  }

  log_image_unmount_result(file_path, mount_point, resolved_backend,
                           resolved_unit, result);
  if (result.error != 0)
    errno = result.error;
  return result;
}

unmount_result_t unmount_image(const char *file_path, int unit_id,
                               attach_backend_t backend) {
  return unmount_image_impl(file_path, unit_id, backend, true, 0);
}

unmount_result_t unmount_runtime_image(const char *file_path, int unit_id,
                                       attach_backend_t backend) {
  return unmount_image_impl(file_path, unit_id, backend, false, 0);
}

static bool release_runtime_image_attachment(const char *file_path,
                                             uint64_t generation) {
  image_cache_entry_t entry;
  int index = -1;
  if (!find_image_cache_entry(file_path, &entry, &index) ||
      entry.generation != generation) {
    errno = ESTALE;
    return false;
  }
  if (entry.backend == ATTACH_BACKEND_NONE || entry.unit_id < 0)
    return cancel_image_attachment(file_path, generation);
  unmount_result_t result = unmount_image_impl(
      entry.path, entry.unit_id, entry.backend, false, generation);
  if (!unmount_completed(result))
    return false;
  return invalidate_matching_image_cache_entry(index, &entry);
}

bool release_runtime_image_mount(const char *file_path) {
  image_cache_entry_t entry;
  int index = -1;
  if (!find_image_cache_entry(file_path, &entry, &index))
    return true;
  if (entry.state == ATTACHED_DEVICE_ATTACHING) {
    errno = EBUSY;
    return false;
  }
  if (entry.backend == ATTACH_BACKEND_NONE || entry.unit_id < 0)
    return true;
  unmount_result_t result =
      unmount_runtime_image(entry.path, entry.unit_id, entry.backend);
  if (!unmount_completed(result))
    return false;
  (void)invalidate_matching_image_cache_entry(index, &entry);
  return true;
}

bool release_runtime_image_mounts(void) {
  bool all_released = true;
  for (int pass = 0; pass < MAX_LAYERED_UNMOUNT_ATTEMPTS; ++pass) {
    bool any_remaining = false;
    bool progress = false;
    for (int k = 0; k < MAX_IMAGE_MOUNTS; ++k) {
      image_cache_entry_t entry;
      if (!get_image_cache_entry(k, &entry))
        continue;
      if (entry.state == ATTACHED_DEVICE_ATTACHING) {
        any_remaining = true;
        all_released = false;
        continue;
      }
      if (entry.backend == ATTACH_BACKEND_NONE || entry.unit_id < 0)
        continue;
      unmount_result_t result =
          unmount_runtime_image(entry.path, entry.unit_id, entry.backend);
      if (unmount_completed(result)) {
        (void)invalidate_matching_image_cache_entry(k, &entry);
        progress = true;
      } else {
        any_remaining = true;
        all_released = false;
      }
    }
    if (!any_remaining)
      return true;
    if (!progress)
      break;
  }
  return all_released;
}

static bool service_pending_image_detach(
    int cache_index, const image_cache_entry_t *entry) {
  if (entry->state != ATTACHED_DEVICE_DETACH_REQUESTED)
    return false;

  unmount_result_t result =
      unmount_runtime_image(entry->path, entry->unit_id, entry->backend);
  if (unmount_completed(result))
    (void)invalidate_matching_image_cache_entry(cache_index, entry);
  return true;
}

void cleanup_stale_image_mounts(void) {
  if (should_stop_requested())
    return;

  bool has_cached_mount = false;
  for (int k = 0; k < MAX_IMAGE_MOUNTS; ++k) {
    image_cache_entry_t entry;
    if (get_image_cache_entry(k, &entry) &&
        entry.backend != ATTACH_BACKEND_NONE && entry.unit_id >= 0) {
      has_cached_mount = true;
      break;
    }
  }
  if (!has_cached_mount)
    return;

  log_debug("  [IMG] stale image cleanup begin");
  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    image_cache_entry_t cached_entry;
    if (should_stop_requested())
      return;
    if (!get_image_cache_entry(k, &cached_entry))
      continue;
    if (cached_entry.backend == ATTACH_BACKEND_NONE ||
        cached_entry.unit_id < 0) {
      continue;
    }

    log_debug("  [IMG][%s] stale cleanup check: slot=%d source=%s mount=%s",
              attach_backend_name(cached_entry.backend), k, cached_entry.path,
              cached_entry.mount_point);

    if (service_pending_image_detach(k, &cached_entry))
      continue;
    if (cached_entry.state == ATTACHED_DEVICE_ATTACHING)
      continue;
    if (!path_exists(cached_entry.path)) {
      log_debug("  [IMG][%s] Source removed, unmounting: %s",
                attach_backend_name(cached_entry.backend), cached_entry.path);
      unmount_result_t result =
          unmount_image(cached_entry.path, cached_entry.unit_id,
                        cached_entry.backend);
      if (unmount_completed(result))
        (void)invalidate_matching_image_cache_entry(k, &cached_entry);
      continue;
    }

    image_fs_type_t fs_type =
        detect_image_fs_type_for_path(cached_entry.path, NULL);
    char mount_point[MAX_PATH];
    char source_path[MAX_PATH];
    if (!prepare_image_mount_retry(&cached_entry, mount_point, source_path))
      continue;

    if (mount_image(source_path, fs_type)) {
      clear_image_mount_attempts(source_path);
      continue;
    }

    int mount_err = errno;
    if (bump_image_mount_attempts(source_path) == 1 && !sm_error_notified()) {
      notify_image_mount_failed(source_path, mount_err);
    }
  }
  log_debug("  [IMG] stale image cleanup done");
}

void cleanup_stale_image_mounts_for_root(const char *root) {
  if (!root || root[0] == '\0') {
    cleanup_stale_image_mounts();
    return;
  }

  if (should_stop_requested())
    return;

  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    image_cache_entry_t cached_entry;
    if (should_stop_requested())
      return;
    if (!get_image_cache_entry(k, &cached_entry))
      continue;
    if (cached_entry.backend == ATTACH_BACKEND_NONE ||
        cached_entry.unit_id < 0) {
      continue;
    }
    if (!path_matches_root_or_child(cached_entry.path, root) &&
        !path_matches_root_or_child(cached_entry.mount_point, root)) {
      continue;
    }

    if (service_pending_image_detach(k, &cached_entry))
      continue;
    if (cached_entry.state == ATTACHED_DEVICE_ATTACHING)
      continue;
    if (!path_exists(cached_entry.path)) {
      log_debug("  [IMG][%s] Source removed, unmounting: %s",
                attach_backend_name(cached_entry.backend), cached_entry.path);
      unmount_result_t result =
          unmount_image(cached_entry.path, cached_entry.unit_id,
                        cached_entry.backend);
      if (unmount_completed(result))
        (void)invalidate_matching_image_cache_entry(k, &cached_entry);
      continue;
    }

    image_fs_type_t fs_type =
        detect_image_fs_type_for_path(cached_entry.path, NULL);
    char mount_point[MAX_PATH];
    char source_path[MAX_PATH];
    if (!prepare_image_mount_retry(&cached_entry, mount_point, source_path))
      continue;

    if (mount_image(source_path, fs_type)) {
      clear_image_mount_attempts(source_path);
      continue;
    }

    int mount_err = errno;
    if (bump_image_mount_attempts(source_path) == 1 && !sm_error_notified()) {
      notify_image_mount_failed(source_path, mount_err);
    }
  }
}

bool unmount_usb_image_mounts_for_suspend(void) {
  bool all_unmounted = true;

  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    image_cache_entry_t cached_entry;
    if (!get_image_cache_entry(k, &cached_entry))
      continue;
    if (!is_usb_storage_path(cached_entry.path))
      continue;
    if (cached_entry.state == ATTACHED_DEVICE_ATTACHING) {
      all_unmounted = false;
      log_debug("  [IMG][%s] USB suspend unmount deferred while attaching: %s",
                attach_backend_name(cached_entry.backend), cached_entry.path);
      continue;
    }
    if (cached_entry.backend == ATTACH_BACKEND_NONE ||
        cached_entry.unit_id < 0) {
      continue;
    }

    log_debug("  [IMG][%s] USB suspend unmount: %s",
              attach_backend_name(cached_entry.backend), cached_entry.path);
    unmount_result_t result = unmount_runtime_image(
        cached_entry.path, cached_entry.unit_id, cached_entry.backend);
    if (unmount_completed(result)) {
      log_debug("  [IMG][%s] USB suspend unmounted: %s",
                attach_backend_name(cached_entry.backend), cached_entry.path);
      (void)invalidate_matching_image_cache_entry(k, &cached_entry);
      continue;
    }

    all_unmounted = false;
    log_debug("  [IMG][%s] USB suspend unmount pending: %s",
              attach_backend_name(cached_entry.backend), cached_entry.path);
  }

  return all_unmounted;
}

static void cleanup_mount_dirs_under(const char *base, const char *skip_name) {
  DIR *d = opendir(base);
  if (!d) {
    if (errno != ENOENT)
      log_debug("  [IMG] open %s failed: %s", base, strerror(errno));
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (should_stop_requested())
      break;
    if (entry->d_name[0] == '.')
      continue;
    if (skip_name && strcmp(entry->d_name, skip_name) == 0)
      continue;

    char full_path[MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s", base, entry->d_name);

    bool is_dir = false;
    if (entry->d_type == DT_DIR) {
      is_dir = true;
    } else if (entry->d_type == DT_UNKNOWN) {
      struct stat st;
      if (stat(full_path, &st) == 0)
        is_dir = S_ISDIR(st.st_mode);
    }
    if (!is_dir)
      continue;

    if (rmdir(full_path) == 0) {
      log_debug("  [IMG] removed empty mount dir: %s", full_path);
      continue;
    }
    if (errno == ENOTEMPTY || errno == EBUSY || errno == ENOENT)
      continue;
    log_debug("  [IMG] failed to remove mount dir %s: %s", full_path,
              strerror(errno));
  }

  closedir(d);
}

void cleanup_mount_dirs(void) {
  cleanup_mount_dirs_under(PFSC_IMAGE_MOUNT_BASE, NULL);
  cleanup_mount_dirs_under(IMAGE_MOUNT_BASE, "pfsc");
}

bool maybe_mount_image_file_with_mode(
    const char *full_path, const char *display_name, bool *unstable_out,
    const bool *mount_read_only_override) {
  image_fs_type_t fs_type =
      detect_image_fs_type_for_path(full_path, display_name);
  if (fs_type == IMAGE_FS_UNKNOWN)
    return false;
  if (!is_source_stable_for_mount(full_path, display_name, "IMG")) {
    if (unstable_out)
      *unstable_out = true;
    return false;
  }
  if (is_image_mount_limited(full_path))
    return false;

  if (mount_image_with_mode(full_path, fs_type, mount_read_only_override)) {
    clear_image_mount_attempts(full_path);
    return true;
  }

  int mount_err = errno;
  if (mount_read_only_override &&
      (mount_err == EBUSY || mount_err == ENOTSUP)) {
    errno = mount_err;
    return false;
  }
  if (bump_image_mount_attempts(full_path) == 1 && !sm_error_notified()) {
    notify_image_mount_failed(full_path, mount_err);
  }
  errno = mount_err;
  return false;
}

bool maybe_mount_image_file(const char *full_path, const char *display_name,
                            bool *unstable_out) {
  return maybe_mount_image_file_with_mode(full_path, display_name,
                                          unstable_out, NULL);
}

bool shutdown_image_mounts(void) {
  for (int pass = 0; pass < MAX_LAYERED_UNMOUNT_ATTEMPTS; pass++) {
    bool any_remaining = false;
    bool progress = false;

    for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
      image_cache_entry_t cached_entry;
      if (!get_image_cache_entry(k, &cached_entry))
        continue;
      if (cached_entry.backend == ATTACH_BACKEND_NONE ||
          cached_entry.unit_id < 0) {
        continue;
      }

      unmount_result_t result = unmount_runtime_image(
          cached_entry.path, cached_entry.unit_id, cached_entry.backend);
      if (unmount_completed(result)) {
        (void)invalidate_matching_image_cache_entry(k, &cached_entry);
        progress = true;
        continue;
      }

      any_remaining = true;
      log_debug("  [IMG] shutdown unmount pending (%d/%d): %s", pass + 1,
                MAX_LAYERED_UNMOUNT_ATTEMPTS, cached_entry.path);
    }

    if (!any_remaining)
      return true;
    if (!progress)
      break;
  }

  return false;
}
