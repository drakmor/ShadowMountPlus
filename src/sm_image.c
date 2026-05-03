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

static uint32_t get_lvd_sector_size_fallback(image_fs_type_t fs_type) {
  const runtime_config_t *cfg = runtime_config();
  switch (fs_type) {
  case IMAGE_FS_UFS:
    return cfg->lvd_sector_ufs;
  case IMAGE_FS_PFS:
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
  uint32_t fallback = get_image_sector_size_override_or_default(
      path, get_lvd_sector_size_fallback(fs_type));
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

static uint32_t get_lvd_secondary_unit(const char *path,
                                       image_fs_type_t fs_type) {
  if (fs_type == IMAGE_FS_EXFAT)
    return LVD_SECONDARY_UNIT_SINGLE_IMAGE;
  return get_lvd_sector_size(path, fs_type);
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
                                         bool mount_read_only) {
    if (fs_type == IMAGE_FS_UFS) {
    return mount_read_only ? LVD_ATTACH_RAW_FLAGS_DD_RO
                           : LVD_ATTACH_RAW_FLAGS_DD_RW;
  }
  return mount_read_only ? LVD_ATTACH_RAW_FLAGS_SINGLE_RO
                         : LVD_ATTACH_RAW_FLAGS_SINGLE_RW;
}

static unsigned int get_nmount_flags(image_fs_type_t fs_type,
                                     bool mount_read_only,
                                     const char **mount_mode_out) {
  if (fs_type == IMAGE_FS_UFS) {
    if (mount_mode_out)
      *mount_mode_out = mount_read_only ? "dd_ro" : "dd_rw";
    return mount_read_only ? UFS_NMOUNT_FLAG_RO : UFS_NMOUNT_FLAG_RW;
  }
  if (mount_mode_out)
    *mount_mode_out = mount_read_only ? "rdonly" : "rw";
  return mount_read_only ? MNT_RDONLY : 0;
}

static uint16_t normalize_lvd_raw_flags(uint16_t raw_flags) {
  if ((raw_flags & 0x800Eu) != 0u) {
    uint32_t raw = (uint32_t)raw_flags;
    uint32_t len = (raw & 0xFFFF8000u) + ((raw & 2u) << 6) +
                   (8u * (raw & 1u)) + (2u * ((raw >> 2) & 1u)) +
                   (2u * (raw & 8u)) + 4u;
    return (uint16_t)len;
  }
  return (uint16_t)(8u * ((uint32_t)raw_flags & 1u) + 4u);
}

static uint16_t get_lvd_image_type(image_fs_type_t fs_type) {
  if (fs_type == IMAGE_FS_UFS)
    return LVD_ATTACH_IMAGE_TYPE_UFS_DOWNLOAD_DATA;
  if (fs_type == IMAGE_FS_PFS)
    return LVD_ATTACH_IMAGE_TYPE_PFS_SAVE_DATA;
  return LVD_ATTACH_IMAGE_TYPE_SINGLE;
}

static uint16_t get_lvd_source_type(const char *path) {
  if (strncmp(path, "/dev/sbram0", strlen("/dev/sbram0")) == 0)
    return LVD_ENTRY_TYPE_SPECIAL;

  struct stat st;
  if (stat(path, &st) == 0) {
    if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))
      return LVD_ENTRY_TYPE_SPECIAL;
  }
  return LVD_ENTRY_TYPE_FILE;
}

// --- Image Path and Naming Helpers ---
static image_fs_type_t detect_image_fs_type(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot)
    return IMAGE_FS_UNKNOWN;
  if (strcasecmp(dot, ".ffpkg") == 0)
    return IMAGE_FS_UFS;
  if (strcasecmp(dot, ".exfat") == 0)
    return IMAGE_FS_EXFAT;
  if (strcasecmp(dot, ".ffpfs") == 0)
    return IMAGE_FS_PFS;
  return IMAGE_FS_UNKNOWN;
}

bool is_supported_image_file_name(const char *name) {
  return detect_image_fs_type(name) != IMAGE_FS_UNKNOWN;
}

static const char *image_fs_name(image_fs_type_t fs_type) {
  switch (fs_type) {
  case IMAGE_FS_UFS:
    return "ufs";
  case IMAGE_FS_EXFAT:
    return "exfatfs";
  case IMAGE_FS_PFS:
    return "pfs";
  default:
    return "unknown";
  }
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

static void build_image_mount_point(const char *file_path,
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

  snprintf(mount_point, MAX_PATH, "%s/%s", IMAGE_MOUNT_BASE, mount_name);
}

typedef bool (*image_attach_fn)(const char *file_path, image_fs_type_t fs_type,
                                bool mount_read_only, off_t file_size,
                                int *unit_id_out, char *devname_out,
                                size_t devname_size);

typedef struct {
  attach_backend_t id;
  image_attach_fn attach;
} image_backend_ops_t;

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
  if (!wait_for_dev_node_state(devname_out, true)) {
    log_debug("  [IMG][%s] device node did not appear: %s",
              attach_backend_name(ATTACH_BACKEND_MD), devname_out);
    (void)detach_attached_unit(ATTACH_BACKEND_MD, unit_id);
    return false;
  }

  log_debug("  [IMG][%s] attach returned unit=%d",
            attach_backend_name(ATTACH_BACKEND_MD), unit_id);
  *unit_id_out = unit_id;
  return true;
}

static bool attach_lvd_backend(const char *file_path, image_fs_type_t fs_type,
                               bool mount_read_only, off_t file_size,
                               int *unit_id_out, char *devname_out,
                               size_t devname_size) {
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
  uint32_t secondary_unit = get_lvd_secondary_unit(file_path, fs_type);
  uint16_t raw_flags = get_lvd_attach_raw_flags(fs_type, mount_read_only);
  uint16_t normalized_flags = normalize_lvd_raw_flags(raw_flags);

  lvd_ioctl_attach_v0_t req;
  memset(&req, 0, sizeof(req));
  req.io_version = LVD_ATTACH_IO_VERSION_V0;
  req.image_type = get_lvd_image_type(fs_type);
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
  if (!wait_for_dev_node_state(devname_out, true)) {
    log_debug("  [IMG][%s] device node did not appear: %s",
              attach_backend_name(ATTACH_BACKEND_LVD), devname_out);
    (void)detach_attached_unit(ATTACH_BACKEND_LVD, unit_id);
    return false;
  }

  *unit_id_out = unit_id;
  return true;
}

static const image_backend_ops_t *get_image_backend_ops(attach_backend_t backend) {
  static const image_backend_ops_t md_ops = {
      .id = ATTACH_BACKEND_MD, .attach = attach_md_backend};
  static const image_backend_ops_t lvd_ops = {
      .id = ATTACH_BACKEND_LVD, .attach = attach_lvd_backend};
  if (backend == ATTACH_BACKEND_MD)
    return &md_ops;
  if (backend == ATTACH_BACKEND_LVD)
    return &lvd_ops;
  return NULL;
}

static bool is_cached_image_path(const char *file_path) {
  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    image_cache_entry_t cached_entry;
    if (get_image_cache_entry(k, &cached_entry) &&
        strcmp(cached_entry.path, file_path) == 0) {
      return true;
    }
  }
  return false;
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
                                    attach_backend_t attach_backend, int unit_id,
                                    const char *devname,
                                    const char *mount_point, int io_err,
                                    const char *stage) {
  sm_error_set("IMG", io_err, file_path, "Mounted image is unreadable or damaged");
  log_debug("  [IMG][%s] unreadable or damaged mount (%s -> %s, %s): %s",
            attach_backend_name(attach_backend), devname, mount_point, stage,
            strerror(io_err));
  (void)unmount_image(file_path, unit_id, attach_backend);
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
    if (!unmount_image(source_path, cached_entry->unit_id, cached_entry->backend))
      return false;
  } else {
    log_debug("  [IMG][%s] mount lost, retrying: %s -> %s",
              attach_backend_name(cached_entry->backend), source_path,
              mount_point);
    clear_cached_game(mount_point);
  }

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
      if (!unmount_image(file_path, existing_unit, existing_backend)) {
        if (cache_failed_out)
          *cache_failed_out = true;
        return false;
      }
      return false;
    }
    if (!cache_image_mount(file_path, mount_point, existing_unit,
                           existing_backend)) {
      sm_error_set("IMG", ENOSPC, file_path,
                   "Image cache full (%u entries), cannot track mount %s",
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

static bool attach_image_device(const char *file_path, image_fs_type_t fs_type,
                                bool mount_read_only, off_t file_size,
                                attach_backend_t attach_backend, int *unit_id_out,
                                char *devname_out, size_t devname_size) {
  const image_backend_ops_t *backend_ops = get_image_backend_ops(attach_backend);
  if (!backend_ops) {
    log_debug("  [IMG] unsupported attach backend for %s", file_path);
    errno = EINVAL;
    return false;
  }

  if (!backend_ops->attach(file_path, fs_type, mount_read_only, file_size,
                           unit_id_out, devname_out, devname_size)) {
    return false;
  }

  log_debug("  [IMG][%s] Attached as %s", attach_backend_name(attach_backend),
            devname_out);
  return true;
}

static bool perform_image_nmount(const char *file_path, image_fs_type_t fs_type,
                                 attach_backend_t attach_backend, int unit_id,
                                 const char *devname, const char *mount_point,
                                 bool mount_read_only, bool force_mount) {
  struct iovec *iov = NULL;
  unsigned int iovlen = 0;
  char mount_errmsg[256];
  memset(mount_errmsg, 0, sizeof(mount_errmsg));
  const char *sigverify = PFS_MOUNT_SIGVERIFY ? "1" : "0";
  const char *playgo = PFS_MOUNT_PLAYGO ? "1" : "0";
  const char *disc = PFS_MOUNT_DISC ? "1" : "0";
  const char *ekpfs_key = PFS_ZERO_EKPFS_KEY_HEX;

  struct iovec iov_ufs[] = {
      IOVEC_ENTRY("fstype"),     IOVEC_ENTRY("ufs"), IOVEC_ENTRY("from"),
      IOVEC_ENTRY(devname),      IOVEC_ENTRY("fspath"),
      IOVEC_ENTRY(mount_point),  IOVEC_ENTRY("budgetid"),
      IOVEC_ENTRY(DEVPFS_BUDGET_GAME),
      IOVEC_ENTRY("async"),      IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("noatime"),    IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("automounted"), IOVEC_ENTRY(NULL),
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

  if (fs_type == IMAGE_FS_UFS) {
    iov = iov_ufs;
    iovlen = (unsigned int)IOVEC_SIZE(iov_ufs) - (force_mount ? 0u : 2u);
  } else if (fs_type == IMAGE_FS_EXFAT) {
    iov = iov_exfat;
    iovlen = (unsigned int)IOVEC_SIZE(iov_exfat) - (force_mount ? 0u : 2u);
  } else if (fs_type == IMAGE_FS_PFS) {
    log_debug("  [IMG][%s] PFS ro=%d budgetid=%s mkeymode=%s "
              "sigverify=%s playgo=%s disc=%s ekpfs=zero",
              attach_backend_name(attach_backend), mount_read_only ? 1 : 0,
              PFS_MOUNT_BUDGET_ID, PFS_MOUNT_MKEYMODE, sigverify, playgo, disc);
    iov = iov_pfs;
    iovlen = (unsigned int)IOVEC_SIZE(iov_pfs) - (force_mount ? 0u : 2u);
  } else {
    log_debug("  [IMG][%s] unsupported fstype=%s",
              attach_backend_name(attach_backend), image_fs_name(fs_type));
    (void)detach_attached_unit(attach_backend, unit_id);
    errno = EINVAL;
    return false;
  }

  const char *mount_mode = NULL;
  unsigned int mount_flags =
      get_nmount_flags(fs_type, mount_read_only, &mount_mode);
  if (nmount(iov, iovlen, (int)mount_flags) == 0)
    return true;

  int mount_errno = errno;
  if (mount_errmsg[0] != '\0') {
    sm_error_set("IMG", mount_errno, file_path, "%s", mount_errmsg);
    log_debug("  [IMG][%s] nmount %s errmsg: %s",
              attach_backend_name(attach_backend), mount_mode, mount_errmsg);
  }
  log_debug("  [IMG][%s] nmount %s failed: %s",
            attach_backend_name(attach_backend), mount_mode,
            strerror(mount_errno));
  (void)detach_attached_unit(attach_backend, unit_id);
  errno = mount_errno;
  return false;
}

static bool validate_mounted_image(const char *file_path, image_fs_type_t fs_type,
                                   attach_backend_t attach_backend, int unit_id,
                                   const char *devname,
                                   const char *mount_point) {
  struct statfs mounted_sfs;
  if (statfs(mount_point, &mounted_sfs) != 0) {
    return reject_mounted_image_io(file_path, attach_backend, unit_id, devname,
                                   mount_point, errno, "statfs");
  }

  uint32_t min_device_sector =
      (attach_backend == ATTACH_BACKEND_MD) ? get_md_sector_size_for_path(file_path, fs_type)
                                            : get_lvd_sector_size(file_path,
                                                                  fs_type);
  uint64_t fs_block_size = (uint64_t)mounted_sfs.f_bsize;
  if (fs_block_size < (uint64_t)min_device_sector) {
    uint32_t tuned_sector_size = 0;
    bool autotuned =
        fs_block_size <= UINT32_MAX &&
        upsert_image_sector_size_autotune(get_filename_component(file_path),
                                          (uint32_t)fs_block_size,
                                          &tuned_sector_size);
    sm_error_set("IMG", EINVAL, file_path,
                 "Filesystem cluster size (%llu) is smaller than device "
                 "sector size (%u) for %s",
                 (unsigned long long)fs_block_size, min_device_sector, devname);
    log_debug("  [IMG][%s] %s", attach_backend_name(attach_backend),
              sm_last_error()->message);
    if (autotuned) {
      log_debug("  [CFG] image sector autotuned: %s=%u",
                get_filename_component(file_path), tuned_sector_size);
      notify_system("Image mount rejected:\n%s\nCluster size is too small "
                    "(%llu < %u).\nSaved image_sector override: %u.\nTry "
                    "mounting again.",
                    file_path, (unsigned long long)fs_block_size,
                    min_device_sector, tuned_sector_size);
    } else {
      notify_system("Image mount rejected:\n%s\nCluster size is too small "
                    "(%llu < %u).\nUse a larger cluster size in the image or "
                    "lower sector size in config.",
                    file_path, (unsigned long long)fs_block_size,
                    min_device_sector);
    }
    sm_error_mark_notified();
    (void)unmount_image(file_path, unit_id, attach_backend);
    errno = EINVAL;
    return false;
  }

  int root_err = 0;
  if (!is_image_mount_root_accessible(mount_point, &root_err)) {
    return reject_mounted_image_io(file_path, attach_backend, unit_id, devname,
                                   mount_point, root_err, "root access");
  }

  return true;
}

// --- Image Attach + nmount Pipeline ---
bool mount_image(const char *file_path, image_fs_type_t fs_type) {
  sm_error_clear();
  if (should_pause_work()) {
    errno = EINTR;
    return false;
  }

  const runtime_config_t *cfg = runtime_config();
  bool mount_read_only = cfg->mount_read_only;
  bool mount_mode_overridden = false;
  bool force_mount = cfg->force_mount;
  const char *filename = get_filename_component(file_path);
  if (filename[0] != '\0')
    mount_mode_overridden =
        get_image_mode_override(filename, &mount_read_only);

  if (is_cached_image_path(file_path))
    return true;

  char mount_point[MAX_PATH];
  build_image_mount_point(file_path, mount_point);
  struct stat st;
  bool cache_failed = false;
  if (reuse_existing_image_mount(file_path, mount_point, &cache_failed))
    return true;
  if (cache_failed)
    return false;
  if (!stat_image_file(file_path, &st))
    return false;

  log_debug("  [IMG] Mounting image (%s): %s -> %s", image_fs_name(fs_type),
            file_path, mount_point);
  if (mount_mode_overridden) {
    log_debug("  [CFG] Image mode override: %s -> %s", file_path,
              mount_read_only ? "ro" : "rw");
  }

  ensure_mount_dirs(mount_point);

  if (should_pause_work()) {
    errno = EINTR;
    return false;
  }

  attach_backend_t attach_backend = select_image_backend(cfg, fs_type);
  log_debug("  [IMG][%s] attach backend selected for %s",
            attach_backend_name(attach_backend), file_path);

  int unit_id = -1;
  char devname[64];
  memset(devname, 0, sizeof(devname));
  if (!attach_image_device(file_path, fs_type, mount_read_only, st.st_size,
                           attach_backend, &unit_id, devname, sizeof(devname))) {
    return false;
  }
  if (should_pause_work()) {
    (void)detach_attached_unit(attach_backend, unit_id);
    errno = EINTR;
    return false;
  }
  if (!perform_image_nmount(file_path, fs_type, attach_backend, unit_id, devname,
                            mount_point, mount_read_only, force_mount)) {
    return false;
  }
  if (should_pause_work()) {
    (void)unmount_image(file_path, unit_id, attach_backend);
    errno = EINTR;
    return false;
  }

  if (!validate_mounted_image(file_path, fs_type, attach_backend, unit_id,
                              devname, mount_point)) {
    return false;
  }

  log_debug("  [IMG][%s] Mounted (%s) %s -> %s",
            attach_backend_name(attach_backend), image_fs_name(fs_type),
            devname, mount_point);
  log_fs_stats("IMG", mount_point, image_fs_name(fs_type));

  if (!cache_image_mount(file_path, mount_point, unit_id, attach_backend)) {
    sm_error_set("IMG", ENOSPC, file_path,
                 "Image cache full (%u entries), rolling back mount",
                 (unsigned)MAX_IMAGE_MOUNTS);
    log_debug("  [IMG] image cache full, rolling back mount: %s -> %s",
              file_path, mount_point);
    (void)unmount_image(file_path, unit_id, attach_backend);
    errno = ENOSPC;
    return false;
  }
  return true;
}

bool unmount_image(const char *file_path, int unit_id, attach_backend_t backend) {
  char mount_point[MAX_PATH];
  build_image_mount_point(file_path, mount_point);
  int resolved_unit = unit_id;
  attach_backend_t resolved_backend = backend;

  if (resolved_unit < 0 || resolved_backend == ATTACH_BACKEND_NONE) {
    if (!resolve_device_from_mount(mount_point, &resolved_backend,
                                   &resolved_unit)) {
      resolved_backend = ATTACH_BACKEND_NONE;
      resolved_unit = -1;
    }
  }

  log_debug("  [IMG][%s] unmount start: source=%s mount=%s unit=%d",
            attach_backend_name(resolved_backend), file_path, mount_point,
            resolved_unit);

  // Remove mount.lnk and unmount /system_ex/app/<titleid> that point to this
  // source before unmounting the virtual disk itself.
  cleanup_mount_links(mount_point, true);
  clear_cached_game(mount_point);

  // Unmount stacked layers (unionfs over image fs).
  for (int i = 0; i < MAX_LAYERED_UNMOUNT_ATTEMPTS; i++) {
    if (!is_active_image_mount_point(mount_point))
      break;
    if (unmount(mount_point, 0) == 0)
      continue;
    if (errno == ENOENT || errno == EINVAL)
      break;
    if (unmount(mount_point, MNT_FORCE) != 0 && errno != ENOENT &&
        errno != EINVAL) {
      log_debug("  [IMG][%s] unmount failed for %s: %s",
                attach_backend_name(resolved_backend), mount_point,
                strerror(errno));
      return false;
    }
  }

  if (is_active_image_mount_point(mount_point)) {
    log_debug("  [IMG][%s] unmount incomplete for %s",
              attach_backend_name(resolved_backend), mount_point);
    return false;
  }

  bool detach_ok = true;
  if (resolved_backend != ATTACH_BACKEND_NONE && resolved_unit >= 0)
    detach_ok = detach_attached_unit(resolved_backend, resolved_unit);

  if (rmdir(mount_point) == 0) {
    log_debug("  [IMG] Removed mount directory: %s", mount_point);
    log_debug("  [IMG][%s] unmount complete: source=%s mount=%s unit=%d",
              attach_backend_name(resolved_backend), file_path, mount_point,
              resolved_unit);
    return detach_ok;
  }

  int err = errno;
  if (err == ENOENT) {
    log_debug("  [IMG][%s] unmount complete: source=%s mount=%s unit=%d",
              attach_backend_name(resolved_backend), file_path, mount_point,
              resolved_unit);
    return detach_ok;
  }
  if (err == ENOTEMPTY || err == EBUSY) {
    log_debug("  [IMG] Mount directory not removed (%s): %s",
              strerror(err), mount_point);
    if (detach_ok) {
      log_debug("  [IMG][%s] unmount complete with leftover dir: source=%s "
                "mount=%s unit=%d",
                attach_backend_name(resolved_backend), file_path, mount_point,
                resolved_unit);
    }
    return detach_ok;
  }
  log_debug("  [IMG] Failed to remove mount directory %s: %s", mount_point,
            strerror(err));
  return detach_ok;
}

void cleanup_stale_image_mounts(void) {
  if (should_stop_requested())
    return;

  log_debug("  [IMG] stale image cleanup begin");
  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    image_cache_entry_t cached_entry;
    if (should_stop_requested())
      return;
    if (!get_image_cache_entry(k, &cached_entry))
      continue;

    log_debug("  [IMG][%s] stale cleanup check: slot=%d source=%s mount=%s",
              attach_backend_name(cached_entry.backend), k, cached_entry.path,
              cached_entry.mount_point);

    if (!path_exists(cached_entry.path)) {
      log_debug("  [IMG][%s] Source removed, unmounting: %s",
                attach_backend_name(cached_entry.backend), cached_entry.path);
      if (unmount_image(cached_entry.path, cached_entry.unit_id,
                        cached_entry.backend))
        invalidate_image_cache_entry(k);
      continue;
    }

    image_fs_type_t fs_type = detect_image_fs_type(cached_entry.path);
    char mount_point[MAX_PATH];
    char source_path[MAX_PATH];
    if (!prepare_image_mount_retry(&cached_entry, mount_point, source_path))
      continue;

    invalidate_image_cache_entry(k);
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
    if (!path_matches_root_or_child(cached_entry.path, root) &&
        !path_matches_root_or_child(cached_entry.mount_point, root)) {
      continue;
    }

    if (!path_exists(cached_entry.path)) {
      log_debug("  [IMG][%s] Source removed, unmounting: %s",
                attach_backend_name(cached_entry.backend), cached_entry.path);
      if (unmount_image(cached_entry.path, cached_entry.unit_id,
                        cached_entry.backend))
        invalidate_image_cache_entry(k);
      continue;
    }

    image_fs_type_t fs_type = detect_image_fs_type(cached_entry.path);
    char mount_point[MAX_PATH];
    char source_path[MAX_PATH];
    if (!prepare_image_mount_retry(&cached_entry, mount_point, source_path))
      continue;

    invalidate_image_cache_entry(k);
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

void cleanup_mount_dirs(void) {
  DIR *d = opendir(IMAGE_MOUNT_BASE);
  if (!d) {
    if (errno != ENOENT)
      log_debug("  [IMG] open %s failed: %s", IMAGE_MOUNT_BASE, strerror(errno));
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (should_stop_requested())
      break;
    if (entry->d_name[0] == '.')
      continue;

    char full_path[MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s", IMAGE_MOUNT_BASE,
             entry->d_name);

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

void maybe_mount_image_file(const char *full_path, const char *display_name,
                            bool *unstable_out) {
  image_fs_type_t fs_type = detect_image_fs_type(display_name);
  if (fs_type == IMAGE_FS_UNKNOWN)
    return;
  if (!is_source_stable_for_mount(full_path, display_name, "IMG")) {
    if (unstable_out)
      *unstable_out = true;
    return;
  }
  if (is_image_mount_limited(full_path))
    return;

  if (mount_image(full_path, fs_type)) {
    clear_image_mount_attempts(full_path);
    return;
  }

  int mount_err = errno;
  if (bump_image_mount_attempts(full_path) == 1 && !sm_error_notified()) {
    notify_image_mount_failed(full_path, mount_err);
  }
}

bool shutdown_image_mounts(void) {
  for (int pass = 0; pass < MAX_LAYERED_UNMOUNT_ATTEMPTS; pass++) {
    bool any_remaining = false;
    bool progress = false;

    for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
      image_cache_entry_t cached_entry;
      if (!get_image_cache_entry(k, &cached_entry))
        continue;

      if (unmount_image(cached_entry.path, cached_entry.unit_id,
                        cached_entry.backend)) {
        invalidate_image_cache_entry(k);
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
