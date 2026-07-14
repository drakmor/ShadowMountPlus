#include "sm_platform.h"

#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/vnode.h>

#include "sm_log.h"
#include "sm_mount_diag.h"
#include "sm_path_utils.h"

#define MOUNT_DIAG_MAX_ROOTS 16u
#define MOUNT_DIAG_MAX_VM_HITS 32u
#define MOUNT_DIAG_MAX_VNODE_HITS 16u

typedef struct {
  char paths[MOUNT_DIAG_MAX_ROOTS][MAX_PATH];
  size_t count;
} mount_diag_roots_t;

static void *read_sysctl_snapshot(const int *mib, u_int mib_count,
                                  size_t *size_out) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    size_t needed = 0;
    if (sysctl(mib, mib_count, NULL, &needed, NULL, 0) != 0)
      return NULL;
    if (needed == 0) {
      *size_out = 0;
      return NULL;
    }

    size_t capacity = needed + needed / 8u + 4096u;
    uint8_t *buffer = malloc(capacity);
    if (!buffer)
      return NULL;

    size_t used = capacity;
    if (sysctl(mib, mib_count, buffer, &used, NULL, 0) == 0) {
      *size_out = used;
      return buffer;
    }

    int snapshot_errno = errno;
    free(buffer);
    if (snapshot_errno != ENOMEM) {
      errno = snapshot_errno;
      return NULL;
    }
  }

  errno = ENOMEM;
  return NULL;
}

static bool copy_record_path(char out[MAX_PATH], const uint8_t *record,
                             size_t record_size, size_t path_offset) {
  out[0] = '\0';
  if (record_size <= path_offset)
    return false;

  const char *path = (const char *)(record + path_offset);
  size_t available = record_size - path_offset;
  size_t length = strnlen(path, available);
  if (length == 0 || length == available)
    return false;

  (void)strlcpy(out, path, MAX_PATH);
  return true;
}

static void copy_process_name(char out[TDNAMLEN + 1], const uint8_t *record,
                              size_t record_size) {
  size_t name_offset = offsetof(struct kinfo_proc, ki_tdname);
  out[0] = '\0';
  if (record_size <= name_offset)
    return;

  const char *name = (const char *)(record + name_offset);
  size_t available = record_size - name_offset;
  size_t length = strnlen(name, available);
  if (length == available)
    return;
  if (length > TDNAMLEN)
    length = TDNAMLEN;
  memcpy(out, name, length);
  out[length] = '\0';
}

static bool add_diag_root(mount_diag_roots_t *roots, const char *path) {
  if (!path || path[0] == '\0')
    return false;
  for (size_t i = 0; i < roots->count; ++i) {
    if (strcmp(roots->paths[i], path) == 0)
      return true;
  }
  if (roots->count >= MOUNT_DIAG_MAX_ROOTS)
    return false;
  (void)strlcpy(roots->paths[roots->count++], path, MAX_PATH);
  return true;
}

static bool path_matches_diag_roots(const mount_diag_roots_t *roots,
                                    const char *path) {
  for (size_t i = 0; i < roots->count; ++i) {
    if (path_matches_root_or_child(path, roots->paths[i]))
      return true;
  }
  return false;
}

static const char *normalized_mount_source(const char *source) {
  return strncmp(source, "<above>:", 8) == 0 ? source + 8 : source;
}

static void collect_mount_dependencies(const char *mount_point,
                                       mount_diag_roots_t *roots,
                                       size_t *dependency_count_out) {
  (void)add_diag_root(roots, mount_point);

  struct statfs *mounts = NULL;
  int mount_count = getmntinfo(&mounts, MNT_NOWAIT);
  if (mount_count <= 0 || !mounts) {
    log_debug("  [MOUNT-BUSY] mount table unavailable for %s: %s",
              mount_point, strerror(errno));
    return;
  }

  size_t dependency_count = 0;
  for (int i = 0; i < mount_count; ++i) {
    const char *source = normalized_mount_source(mounts[i].f_mntfromname);
    const char *target = mounts[i].f_mntonname;
    bool consumes_mount = path_matches_root_or_child(source, mount_point);
    bool child_mount = strcmp(target, mount_point) != 0 &&
                       path_matches_root_or_child(target, mount_point);
    if (!consumes_mount && !child_mount)
      continue;

    dependency_count++;
    log_debug("  [MOUNT-BUSY] dependent type=%s from=%s on=%s flags=0x%lX",
              mounts[i].f_fstypename, source, target,
              (unsigned long)mounts[i].f_flags);
    if (consumes_mount)
      (void)add_diag_root(roots, target);
  }
  *dependency_count_out = dependency_count;
}

static void log_process_vm_refs(pid_t pid, const char *name,
                                const mount_diag_roots_t *roots,
                                size_t *hit_count) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, pid};
  size_t data_size = 0;
  uint8_t *data = read_sysctl_snapshot(mib, 4, &data_size);
  if (!data)
    return;

  uint8_t *cursor = data;
  uint8_t *end = data + data_size;
  while ((size_t)(end - cursor) >= sizeof(int)) {
    int record_size = 0;
    memcpy(&record_size, cursor, sizeof(record_size));
    if (record_size <= 0 || (size_t)record_size > (size_t)(end - cursor))
      break;

    const struct kinfo_vmentry *entry =
        (const struct kinfo_vmentry *)cursor;
    if ((size_t)record_size > offsetof(struct kinfo_vmentry, kve_path) &&
        entry->kve_type == KVME_TYPE_VNODE) {
      char path[MAX_PATH];
      if (copy_record_path(path, cursor, (size_t)record_size,
                           offsetof(struct kinfo_vmentry, kve_path)) &&
          path_matches_diag_roots(roots, path)) {
        if (*hit_count < MOUNT_DIAG_MAX_VM_HITS) {
          log_debug("  [MOUNT-BUSY] pid=%ld name=%s vm=0x%llX-0x%llX "
                    "prot=0x%X path=%s",
                    (long)pid, name, (unsigned long long)entry->kve_start,
                    (unsigned long long)entry->kve_end,
                    entry->kve_protection, path);
        }
        (*hit_count)++;
      }
    }
    cursor += record_size;
  }

  free(data);
}

static void log_all_process_vm_refs(const mount_diag_roots_t *roots,
                                    size_t *hit_count_out) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t data_size = 0;
  uint8_t *data = read_sysctl_snapshot(mib, 4, &data_size);
  if (!data) {
    log_debug("  [MOUNT-BUSY] process list unavailable: %s", strerror(errno));
    return;
  }

  size_t hit_count = 0;
  uint8_t *cursor = data;
  uint8_t *end = data + data_size;
  while ((size_t)(end - cursor) >= sizeof(int)) {
    int record_size = 0;
    memcpy(&record_size, cursor, sizeof(record_size));
    if (record_size <= 0 || (size_t)record_size > (size_t)(end - cursor))
      break;
    if ((size_t)record_size <= offsetof(struct kinfo_proc, ki_tdname))
      break;

    const struct kinfo_proc *process = (const struct kinfo_proc *)cursor;
    char process_name[TDNAMLEN + 1];
    copy_process_name(process_name, cursor, (size_t)record_size);
    const char *name = process_name[0] ? process_name : "?";
    if (process->ki_pid > 0)
      log_process_vm_refs(process->ki_pid, name, roots, &hit_count);
    cursor += record_size;
  }

  free(data);
  *hit_count_out = hit_count;
  if (hit_count > MOUNT_DIAG_MAX_VM_HITS) {
    log_debug("  [MOUNT-BUSY] %zu additional VM references suppressed",
              hit_count - MOUNT_DIAG_MAX_VM_HITS);
  }
}

static void log_vnode_refs(const char *mount_point, size_t *hit_count_out) {
  struct stat root_stat;
  if (stat(mount_point, &root_stat) != 0)
    return;

  int mib[2] = {CTL_KERN, KERN_VNODE};
  size_t data_size = 0;
  uint8_t *data = read_sysctl_snapshot(mib, 2, &data_size);
  if (!data)
    return;

  void *mount_address = NULL;
  uint8_t *cursor = data;
  uint8_t *end = data + data_size;
  while ((size_t)(end - cursor) >= sizeof(size_t)) {
    const struct xvnode *vnode = (const struct xvnode *)cursor;
    if (vnode->xv_size < sizeof(struct xvnode) ||
        vnode->xv_size > (size_t)(end - cursor)) {
      break;
    }
    if (vnode->xv_dev == root_stat.st_dev &&
        vnode->xv_ino == root_stat.st_ino) {
      mount_address = vnode->xv_mount;
      break;
    }
    cursor += vnode->xv_size;
  }

  size_t hit_count = 0;
  size_t hold_only_count = 0;
  if (mount_address) {
    cursor = data;
    while ((size_t)(end - cursor) >= sizeof(size_t)) {
      const struct xvnode *vnode = (const struct xvnode *)cursor;
      if (vnode->xv_size < sizeof(struct xvnode) ||
          vnode->xv_size > (size_t)(end - cursor)) {
        break;
      }
      if (vnode->xv_mount == mount_address) {
        bool active = vnode->xv_usecount > 0 || vnode->xv_writecount > 0 ||
                      vnode->xv_numoutput > 0;
        if (active) {
          if (hit_count < MOUNT_DIAG_MAX_VNODE_HITS) {
            log_debug("  [MOUNT-BUSY] vnode=%p dev=0x%llX ino=0x%llX "
                      "use=%d hold=%d write=%d output=%ld",
                      vnode->xv_vnode, (unsigned long long)vnode->xv_dev,
                      (unsigned long long)vnode->xv_ino, vnode->xv_usecount,
                      vnode->xv_holdcnt, vnode->xv_writecount,
                      vnode->xv_numoutput);
          }
          hit_count++;
        } else if (vnode->xv_holdcnt > 0) {
          hold_only_count++;
        }
      }
      cursor += vnode->xv_size;
    }
  }

  free(data);
  *hit_count_out = hit_count;
  if (hit_count > MOUNT_DIAG_MAX_VNODE_HITS) {
    log_debug("  [MOUNT-BUSY] %zu additional active vnodes suppressed",
              hit_count - MOUNT_DIAG_MAX_VNODE_HITS);
  }
  if (hold_only_count > 0) {
    log_debug("  [MOUNT-BUSY] hold-only cached vnodes=%zu", hold_only_count);
  }
}

void sm_mount_diag_log_busy(const char *mount_point) {
  if (!mount_point || mount_point[0] == '\0')
    return;

  int saved_errno = errno;
  log_debug("  [MOUNT-BUSY] diagnostic begin: %s", mount_point);

  mount_diag_roots_t roots;
  memset(&roots, 0, sizeof(roots));
  size_t dependency_count = 0;
  collect_mount_dependencies(mount_point, &roots, &dependency_count);

  size_t vm_hit_count = 0;
  log_all_process_vm_refs(&roots, &vm_hit_count);

  size_t vnode_hit_count = 0;
  log_vnode_refs(mount_point, &vnode_hit_count);

  if (dependency_count == 0 && vm_hit_count == 0 &&
      vnode_hit_count == 0) {
    log_debug("  [MOUNT-BUSY] no user-visible owner found; likely transient "
              "VFS, buffer-cache, or backing-device reference");
  }
  log_debug("  [MOUNT-BUSY] diagnostic end: dependencies=%zu vm_refs=%zu "
            "active_vnodes=%zu",
            dependency_count, vm_hit_count, vnode_hit_count);
  errno = saved_errno;
}
