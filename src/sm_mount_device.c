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
bool wait_for_dev_node_state(const char *devname, bool should_exist) {
  for (int i = 0; i < LVD_NODE_WAIT_RETRIES; i++) {
    struct stat st;
    if (stat(devname, &st) == 0) {
      if (should_exist)
        return true;
    } else if (!should_exist && (errno == ENOENT || errno == ENOTDIR)) {
      return true;
    }
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
  if (strncmp(dev_path, prefix, prefix_len) != 0)
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
  struct statfs sfs;
  if (statfs(path, &sfs) == 0 && strcmp(sfs.f_mntonname, path) == 0)
    return true;

  // An unreadable filesystem can make statfs(path) fail while it is still in
  // the mount table. Treat it as active until unmount removes that entry.
  struct statfs *mntbuf = NULL;
  int mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);
  for (int i = 0; i < mntcount && mntbuf; ++i) {
    if (strcmp(mntbuf[i].f_mntonname, path) == 0)
      return true;
  }
  return false;
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
static void build_attached_unit_devname(attach_backend_t backend, int unit_id,
                                        char devname[64]) {
  const char *prefix = backend == ATTACH_BACKEND_MD ? "/dev/md" : "/dev/lvd";
  snprintf(devname, 64, "%s%d", prefix, unit_id);
}

static bool read_device_node_identity(
    const char *devname, attached_unit_detach_state_t *state) {
  struct stat st;
  if (stat(devname, &st) != 0)
    return false;

  state->node_identity_valid = true;
  state->node_device = (uint64_t)st.st_dev;
  state->node_inode = (uint64_t)st.st_ino;
  state->node_rdev = (uint64_t)st.st_rdev;
  return true;
}

static bool device_node_is_mounted(const char *devname,
                                   char mount_point[MNAMELEN]) {
  struct statfs *mounts = NULL;
  int mount_count = getmntinfo(&mounts, MNT_NOWAIT);
  for (int i = 0; i < mount_count && mounts; ++i) {
    if (strcmp(mounts[i].f_mntfromname, devname) != 0)
      continue;
    if (mount_point)
      (void)strlcpy(mount_point, mounts[i].f_mntonname, MNAMELEN);
    return true;
  }
  return false;
}

static bool detach_target_is_released_or_reused(
    attach_backend_t backend, int unit_id, const char *devname,
    const attached_unit_detach_state_t *state) {
  struct stat st;
  if (stat(devname, &st) != 0) {
    if (errno != ENOENT && errno != ENOTDIR)
      return false;
    log_debug("  [IMG][%s] detach target released: unit=%d node removed",
              attach_backend_name(backend), unit_id);
    return true;
  }

  if (state && state->node_identity_valid &&
      (state->node_device != (uint64_t)st.st_dev ||
       state->node_inode != (uint64_t)st.st_ino ||
       state->node_rdev != (uint64_t)st.st_rdev)) {
    log_debug("  [IMG][%s] detach target released: unit=%d node identity "
              "changed",
              attach_backend_name(backend), unit_id);
    return true;
  }

  char mount_point[MNAMELEN];
  if (device_node_is_mounted(devname, mount_point)) {
    log_debug("  [IMG][%s] detach target released: unit=%d reused at %s",
              attach_backend_name(backend), unit_id, mount_point);
    return true;
  }

  return false;
}

typedef enum {
  ATTACHED_UNIT_DETACH_COMPLETE = 0,
  ATTACHED_UNIT_DETACH_REQUEST,
  ATTACHED_UNIT_DETACH_PENDING,
} attached_unit_detach_prepare_t;

static attached_unit_detach_prepare_t prepare_attached_unit_detach(
    attach_backend_t backend, int unit_id,
    const attached_unit_detach_state_t *state, char devname[64],
    attached_unit_detach_state_t *request_state) {
  build_attached_unit_devname(backend, unit_id, devname);
  if (detach_target_is_released_or_reused(backend, unit_id, devname, state))
    return ATTACHED_UNIT_DETACH_COMPLETE;

  if (state && state->requested) {
    errno = EINPROGRESS;
    return ATTACHED_UNIT_DETACH_PENDING;
  }

  if (state)
    *request_state = *state;
  else
    memset(request_state, 0, sizeof(*request_state));
  if (!request_state->node_identity_valid)
    (void)read_device_node_identity(devname, request_state);
  return ATTACHED_UNIT_DETACH_REQUEST;
}

bool wait_for_attached_unit_release(
    attach_backend_t backend, int unit_id,
    const attached_unit_detach_state_t *state) {
  char devname[64];
  build_attached_unit_devname(backend, unit_id, devname);
  if (wait_for_dev_node_state(devname, false))
    return true;
  if (detach_target_is_released_or_reused(backend, unit_id, devname, state))
    return true;

  log_debug("  [IMG][%s] detach pending after timeout: unit=%d node=%s",
            attach_backend_name(backend), unit_id, devname);
  errno = EINPROGRESS;
  return false;
}

static bool finish_accepted_detach(
    attach_backend_t backend, int unit_id,
    attached_unit_detach_state_t *state,
    attached_unit_detach_state_t *request_state) {
  request_state->requested = true;
  if (state) {
    *state = *request_state;
    errno = EINPROGRESS;
    return false;
  }

  return wait_for_attached_unit_release(backend, unit_id, request_state);
}

static bool finish_failed_detach(attach_backend_t backend, int unit_id,
                                 const char *devname,
                                 const attached_unit_detach_state_t *state,
                                 int detach_errno) {
  if (detach_target_is_released_or_reused(backend, unit_id, devname, state))
    return true;
  errno = detach_errno;
  return false;
}

static bool detach_lvd_unit(int unit_id,
                            attached_unit_detach_state_t *state) {
  if (unit_id < 0)
    return true;

  char devname[64];
  attached_unit_detach_state_t request_state;
  attached_unit_detach_prepare_t prepare = prepare_attached_unit_detach(
      ATTACH_BACKEND_LVD, unit_id, state, devname, &request_state);
  if (prepare == ATTACHED_UNIT_DETACH_COMPLETE)
    return true;
  if (prepare == ATTACHED_UNIT_DETACH_PENDING)
    return false;

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

  if (ioctl(fd, SCE_LVD_IOC_DETACH, &req) != 0) {
    int detach_errno = errno;
    log_debug("  [IMG][%s] detach %d failed: %s",
              attach_backend_name(ATTACH_BACKEND_LVD), unit_id,
              strerror(detach_errno));
    close(fd);
    return finish_failed_detach(ATTACH_BACKEND_LVD, unit_id, devname,
                                &request_state, detach_errno);
  }
  close(fd);

  return finish_accepted_detach(ATTACH_BACKEND_LVD, unit_id, state,
                                &request_state);
}

static bool detach_md_unit(int unit_id,
                           attached_unit_detach_state_t *state) {
  if (unit_id < 0)
    return true;

  char devname[64];
  attached_unit_detach_state_t request_state;
  attached_unit_detach_prepare_t prepare = prepare_attached_unit_detach(
      ATTACH_BACKEND_MD, unit_id, state, devname, &request_state);
  if (prepare == ATTACHED_UNIT_DETACH_COMPLETE)
    return true;
  if (prepare == ATTACHED_UNIT_DETACH_PENDING)
    return false;

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

  if (ioctl(fd, MDIOCDETACH, &req) != 0) {
    int detach_errno = errno;
    log_debug("  [IMG][%s] detach %d failed: %s",
              attach_backend_name(ATTACH_BACKEND_MD), unit_id,
              strerror(detach_errno));
    close(fd);
    return finish_failed_detach(ATTACH_BACKEND_MD, unit_id, devname,
                                &request_state, detach_errno);
  }
  close(fd);

  return finish_accepted_detach(ATTACH_BACKEND_MD, unit_id, state,
                                &request_state);
}

bool detach_attached_unit(attach_backend_t backend, int unit_id,
                          attached_unit_detach_state_t *state) {
  if (backend == ATTACH_BACKEND_MD)
    return detach_md_unit(unit_id, state);
  else if (backend == ATTACH_BACKEND_LVD)
    return detach_lvd_unit(unit_id, state);
  return true;
}
