#include "sm_platform.h"
#include "sm_runtime.h"
#include "sm_mount_device.h"
#include "sm_image_cache.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_config_mount.h"
#include "sm_mount_defs.h"
#include "sm_stability.h"

const char *attach_backend_name(attach_backend_t backend) {
  if (backend == ATTACH_BACKEND_LVD)
    return "LVD";
  if (backend == ATTACH_BACKEND_MD)
    return "MD";
  return "UNKNOWN";
}

// --- Device Node Wait and Source Stability ---
static bool wait_for_dev_node_state(const char *devname, bool should_exist) {
  for (int i = 0; i < LVD_NODE_WAIT_RETRIES; i++) {
    if ((access(devname, F_OK) == 0) == should_exist)
      return true;
    sceKernelUsleep(LVD_NODE_WAIT_US);
  }

  return false;
}

bool is_source_stable_for_mount(const char *path, const char *name,
                                const char *tag) {
  double age = 0.0;
  int st_err = 0;
  if (is_path_stable_now(path, &age, &st_err))
    return true;
  if (st_err != 0)
    return false;
  log_debug("  [%s] %s modified %.0fs ago, waiting...", tag, name, age);
  return false;
}

// --- Mounted Device Resolution (/dev/lvdN, /dev/mdN) ---
static bool parse_unit_from_dev_path(const char *dev_path, const char *prefix,
                                     int *unit_out) {
  size_t prefix_len = strlen(prefix);
  if (!dev_path || strncmp(dev_path, prefix, prefix_len) != 0)
    return false;

  char *end = NULL;
  long unit = strtol(dev_path + prefix_len, &end, 10);
  if (end == dev_path + prefix_len || *end != '\0' || unit < 0 ||
      unit > INT_MAX)
    return false;

  *unit_out = (int)unit;
  return true;
}

bool resolve_device_from_mount(const char *mount_point,
                               attach_backend_t *backend_out, int *unit_out) {
  if (!mount_point || !backend_out || !unit_out)
    return false;

  *backend_out = ATTACH_BACKEND_NONE;
  *unit_out = -1;

  if (resolve_device_from_mount_cache(mount_point, backend_out, unit_out))
    return true;

  struct statfs sfs;
  if (statfs(mount_point, &sfs) != 0)
    return false;

  if (strcmp(sfs.f_mntonname, mount_point) != 0)
    return false;

  if (parse_unit_from_dev_path(sfs.f_mntfromname, "/dev/lvd", unit_out)) {
    *backend_out = ATTACH_BACKEND_LVD;
    return true;
  }

  if (parse_unit_from_dev_path(sfs.f_mntfromname, "/dev/md", unit_out)) {
    *backend_out = ATTACH_BACKEND_MD;
    return true;
  }

  struct statfs *mntbuf = NULL;
  int mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);
  if (mntcount <= 0 || !mntbuf)
    return false;

  for (int i = 0; i < mntcount; i++) {
    if (strcmp(mntbuf[i].f_mntonname, mount_point) != 0)
      continue;
    if (parse_unit_from_dev_path(mntbuf[i].f_mntfromname, "/dev/lvd", unit_out)) {
      *backend_out = ATTACH_BACKEND_LVD;
      return true;
    }
    if (parse_unit_from_dev_path(mntbuf[i].f_mntfromname, "/dev/md", unit_out)) {
      *backend_out = ATTACH_BACKEND_MD;
      return true;
    }
  }

  return false;
}

static bool is_path_mountpoint(const char *path) {
  if (!path || path[0] == '\0')
    return false;
  struct statfs sfs;
  return (statfs(path, &sfs) == 0 && strcmp(sfs.f_mntonname, path) == 0);
}

bool is_active_image_mount_point(const char *path) {
  return is_path_mountpoint(path);
}

bool wait_for_lvd_release(void) {
  for (unsigned int waited_us = 0;; waited_us += LVD_RELEASE_WAIT_POLL_US) {
    struct statfs *mntbuf = NULL;
    int mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);
    bool mounted = false;
    for (int i = 0; i < mntcount && mntbuf; i++) {
      if (strcmp(mntbuf[i].f_mntfromname, "/dev/lvd2") != 0)
        continue;
      mounted = true;
      break;
    }
    if (!mounted) {
      if (waited_us != 0)
        log_debug("  [IMG][LVD] /dev/lvd2 released");
      return true;
    }

    if (waited_us == 0) {
      log_debug("  [IMG][LVD] waiting for /dev/lvd2 to be released...");
      for (int i = 0; i < mntcount && mntbuf; i++) {
        if (strncmp(mntbuf[i].f_mntfromname, "/dev/lvd", 8) != 0)
          continue;
        log_debug("  [IMG][LVD] mounted: from=%s path=%s type=%s "
                  "bsize=%llu iosize=%llu blocks=%llu bfree=%llu "
                  "bavail=%llu files=%llu ffree=%llu flags=0x%lX",
                  mntbuf[i].f_mntfromname, mntbuf[i].f_mntonname,
                  mntbuf[i].f_fstypename,
                  (unsigned long long)(uint64_t)mntbuf[i].f_bsize,
                  (unsigned long long)(uint64_t)mntbuf[i].f_iosize,
                  (unsigned long long)(uint64_t)mntbuf[i].f_blocks,
                  (unsigned long long)(uint64_t)mntbuf[i].f_bfree,
                  (unsigned long long)(uint64_t)mntbuf[i].f_bavail,
                  (unsigned long long)(uint64_t)mntbuf[i].f_files,
                  (unsigned long long)(uint64_t)mntbuf[i].f_ffree,
                  (unsigned long)mntbuf[i].f_flags);
      }
    }
    if (should_stop_requested())
      return false;
    if (waited_us >= LVD_RELEASE_WAIT_MAX_US) {
      log_debug("  [IMG][LVD] /dev/lvd2 wait timeout reached (%u ms), "
                "continuing startup", LVD_RELEASE_WAIT_MAX_US / 1000u);
      return true;
    }
    sceKernelUsleep(LVD_RELEASE_WAIT_POLL_US);
  }
}

// --- Device Detach Helpers ---
static bool detach_lvd_unit(int unit_id) {
  if (unit_id < 0)
    return true;

  int fd = open(LVD_CTRL_PATH, O_RDWR);
  if (fd < 0) {
    log_debug("  [IMG][%s] open %s for detach failed: %s",
              attach_backend_name(ATTACH_BACKEND_LVD), LVD_CTRL_PATH,
              strerror(errno));
    return false;
  }

  lvd_ioctl_detach_t req;
  memset(&req, 0, sizeof(req));
  req.device_id = unit_id;

  bool ok = true;
  if (ioctl(fd, SCE_LVD_IOC_DETACH, &req) != 0) {
    log_debug("  [IMG][%s] detach %d failed: %s",
              attach_backend_name(ATTACH_BACKEND_LVD), unit_id, strerror(errno));
    ok = false;
  }
  close(fd);

  char devname[64];
  snprintf(devname, sizeof(devname), "/dev/lvd%d", unit_id);
  if (!wait_for_dev_node_state(devname, false)) {
    log_debug("  [IMG][%s] device node still present after detach: /dev/lvd%d",
              attach_backend_name(ATTACH_BACKEND_LVD), unit_id);
    ok = false;
  }
  return ok;
}

static bool detach_md_unit(int unit_id) {
  if (unit_id < 0)
    return true;

  int fd = open(MD_CTRL_PATH, O_RDWR);
  if (fd < 0) {
    log_debug("  [IMG][%s] open %s for detach failed: %s",
              attach_backend_name(ATTACH_BACKEND_MD), MD_CTRL_PATH,
              strerror(errno));
    return false;
  }

  struct md_ioctl req;
  memset(&req, 0, sizeof(req));
  req.md_version = MDIOVERSION;
  req.md_unit = (unsigned int)unit_id;

  bool ok = true;
  if (ioctl(fd, MDIOCDETACH, &req) != 0) {
    int err = errno;
    req.md_options = MD_FORCE;
    if (ioctl(fd, MDIOCDETACH, &req) != 0) {
      log_debug("  [IMG][%s] detach %d failed: %s",
                attach_backend_name(ATTACH_BACKEND_MD), unit_id,
                strerror(errno));
      ok = false;
    } else {
      log_debug("  [IMG][%s] detach %d forced after error: %s",
                attach_backend_name(ATTACH_BACKEND_MD), unit_id,
                strerror(err));
    }
  }
  close(fd);

  char devname[64];
  snprintf(devname, sizeof(devname), "/dev/md%d", unit_id);
  if (!wait_for_dev_node_state(devname, false)) {
    log_debug("  [IMG][%s] device node still present after detach: /dev/md%d",
              attach_backend_name(ATTACH_BACKEND_MD), unit_id);
    ok = false;
  }
  return ok;
}

bool detach_attached_unit(attach_backend_t backend, int unit_id) {
  if (backend == ATTACH_BACKEND_MD)
    return detach_md_unit(unit_id);
  else if (backend == ATTACH_BACKEND_LVD)
    return detach_lvd_unit(unit_id);
  return true;
}
