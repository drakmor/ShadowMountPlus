#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/mdioctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>


#include <ps5/kernel.h>

// --- Configuration ---
#define SCAN_INTERVAL_US 3000000
#define MAX_PENDING 512
#define MAX_UFS_MOUNTS 64
#define MAX_PATH 1024
#define MAX_TITLE_ID 32
#define MAX_TITLE_NAME 256
#define UFS_MOUNT_BASE "/data/ufsmnt"
#define LOG_DIR "/data/shadowmount"
#define LOG_FILE "/data/shadowmount/debug.log"
#define LOCK_FILE "/data/shadowmount/daemon.lock"
#define KILL_FILE "/data/shadowmount/STOP"
#define TOAST_FILE "/data/shadowmount/notify.txt"
#define IOVEC_ENTRY(x) {(void *)(x), (x) ? strlen(x) + 1 : 0}
#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))

// --- SDK Imports ---
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallTitleDir(const char *title_id,
                                     const char *install_path, void *reserved);
int sceKernelUsleep(unsigned int microseconds);
int sceUserServiceInitialize(void *);
void sceUserServiceTerminate(void);

// --- Forward Declarations ---
bool get_game_info(const char *base_path, char *out_id, char *out_name);
bool is_installed(const char *title_id);
bool is_data_mounted(const char *title_id);
void notify_system(const char *fmt, ...);
void log_debug(const char *fmt, ...);

// Standard Notification
typedef struct notify_request {
  char unused[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

// Scan Paths
const char *SCAN_PATHS[] = {
    // Internal
    "/data/homebrew", "/data/etaHEN/games",

    // Extended Storage
    "/mnt/ext0/etaHEN/homebrew", "/mnt/ext0/etaHEN/games",

    // M.2 Drive
    "/mnt/ext1/etaHEN/homebrew", "/mnt/ext1/etaHEN/games",

    // USB Subfolders
    "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew",
    "/mnt/usb3/homebrew", "/mnt/usb4/homebrew", "/mnt/usb5/homebrew",
    "/mnt/usb6/homebrew", "/mnt/usb7/homebrew",

    "/mnt/usb0/etaHEN/games", "/mnt/usb1/etaHEN/games",
    "/mnt/usb2/etaHEN/games", "/mnt/usb3/etaHEN/games",
    "/mnt/usb4/etaHEN/games", "/mnt/usb5/etaHEN/games",
    "/mnt/usb6/etaHEN/games", "/mnt/usb7/etaHEN/games",

    // USB Root Paths
    "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4",
    "/mnt/usb5", "/mnt/usb6", "/mnt/usb7", "/mnt/ext0", "/mnt/ext1",

    // UFS Mounted Images
    UFS_MOUNT_BASE,

    NULL};

struct GameCache {
  char path[MAX_PATH];
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  bool valid;
};
struct GameCache cache[MAX_PENDING];

struct UfsCache {
  char path[MAX_PATH];
  unsigned md_unit;
  bool valid;
};
struct UfsCache ufs_cache[MAX_UFS_MOUNTS];

static void copy_cstr(char *dst, size_t dst_size, const char *src) {
  if (dst_size == 0)
    return;
  if (!src)
    src = "";
  snprintf(dst, dst_size, "%s", src);
}

static void cache_game_entry(const char *path, const char *title_id,
                             const char *title_name) {
  for (int k = 0; k < MAX_PENDING; k++) {
    if (!cache[k].valid) {
      copy_cstr(cache[k].path, sizeof(cache[k].path), path);
      copy_cstr(cache[k].title_id, sizeof(cache[k].title_id), title_id);
      copy_cstr(cache[k].title_name, sizeof(cache[k].title_name), title_name);
      cache[k].valid = true;
      return;
    }
  }
}

static void cache_ufs_mount(const char *path, unsigned md_unit) {
  for (int k = 0; k < MAX_UFS_MOUNTS; k++) {
    if (!ufs_cache[k].valid) {
      copy_cstr(ufs_cache[k].path, sizeof(ufs_cache[k].path), path);
      ufs_cache[k].md_unit = md_unit;
      ufs_cache[k].valid = true;
      return;
    }
  }
}

// --- LOGGING ---
void log_to_file(const char *fmt, va_list args) {
  mkdir(LOG_DIR, 0777);
  FILE *fp = fopen(LOG_FILE, "a");
  if (fp) {
    time_t rawtime;
    struct tm *timeinfo;
    char buffer[80];
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buffer, sizeof(buffer), "%H:%M:%S", timeinfo);
    fprintf(fp, "[%s] ", buffer);
    vfprintf(fp, fmt, args);
    fprintf(fp, "\n");
    fclose(fp);
  }
}
void log_debug(const char *fmt, ...) {
  va_list args;
  va_list args_copy;
  va_start(args, fmt);
  va_copy(args_copy, args);
  vprintf(fmt, args);
  printf("\n");
  log_to_file(fmt, args_copy);
  va_end(args_copy);
  va_end(args);
}

// --- NOTIFICATIONS ---
void notify_system(const char *fmt, ...) {
  notify_request_t req;
  memset(&req, 0, sizeof(req));
  va_list args;
  va_start(args, fmt);
  vsnprintf(req.message, sizeof(req.message), fmt, args);
  va_end(args);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
  log_debug("NOTIFY: %s", req.message);
}

void trigger_rich_toast(const char *title_id, const char *game_name,
                        const char *msg) {
  FILE *f = fopen(TOAST_FILE, "w");
  if (f) {
    fprintf(f, "%s|%s|%s", title_id, game_name, msg);
    fflush(f);
    fclose(f);
  }
}

// --- FILESYSTEM ---
bool is_installed(const char *title_id) {
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "/user/app/%s", title_id);
  struct stat st;
  return (stat(path, &st) == 0);
}
bool is_data_mounted(const char *title_id) {
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "/system_ex/app/%s/sce_sys/param.json",
           title_id);
  return (access(path, F_OK) == 0);
}

// --- FAST STABILITY CHECK ---
bool wait_for_stability_fast(const char *path, const char *name) {
  struct stat st;
  time_t now = time(NULL);

  // 1. Check Root Folder Timestamp
  if (stat(path, &st) != 0)
    return false;
  double diff = difftime(now, st.st_mtime);

  // If modified > 10 seconds ago, it's stable.
  if (diff > 10.0) {
    // Double check sce_sys just to be sure
    char sys_path[MAX_PATH];
    snprintf(sys_path, sizeof(sys_path), "%s/sce_sys", path);
    if (stat(sys_path, &st) == 0) {
      if (difftime(now, st.st_mtime) > 10.0) {
        return true;
      }
    } else {
      return true; // No sce_sys? Trust root.
    }
  }

  log_debug("  [WAIT] %s modified %.0fs ago. Waiting...", name, diff);
  sceKernelUsleep(2000000); // Wait 2s
  return false;             // Force re-scan next cycle
}

static int remount_system_ex(void) {
  struct iovec iov[] = {
      IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
      IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
      IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
      IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
      IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
      IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL)};
  return nmount(iov, IOVEC_SIZE(iov), MNT_UPDATE);
}

static int mount_nullfs(const char *src, const char *dst) {
  struct iovec iov[] = {IOVEC_ENTRY("fstype"), IOVEC_ENTRY("nullfs"),
                        IOVEC_ENTRY("from"),   IOVEC_ENTRY(src),
                        IOVEC_ENTRY("fspath"), IOVEC_ENTRY(dst)};
  return nmount(iov, IOVEC_SIZE(iov), MNT_RDONLY);
}

static int copy_dir(const char *src, const char *dst) {
  mkdir(dst, 0777);
  DIR *d = opendir(src);
  if (!d)
    return -1;
  struct dirent *e;
  char ss[MAX_PATH], dd[MAX_PATH];
  struct stat st;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    snprintf(ss, sizeof(ss), "%s/%s", src, e->d_name);
    snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);
    if (stat(ss, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode))
      copy_dir(ss, dd);
    else {
      FILE *fs = fopen(ss, "rb");
      if (!fs)
        continue;
      FILE *fd = fopen(dd, "wb");
      if (!fd) {
        fclose(fs);
        continue;
      }
      char buf[8192];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), fs)) > 0)
        fwrite(buf, 1, n, fd);
      fclose(fd);
      fclose(fs);
    }
  }
  closedir(d);
  return 0;
}

int copy_file(const char *src, const char *dst) {
  char buf[8192];
  FILE *fs = fopen(src, "rb");
  if (!fs)
    return -1;
  FILE *fd = fopen(dst, "wb");
  if (!fd) {
    fclose(fs);
    return -1;
  }
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fs)) > 0)
    fwrite(buf, 1, n, fd);
  fclose(fd);
  fclose(fs);
  return 0;
}

// --- UFS2 IMAGE MOUNTING ---
static bool build_iovec(struct iovec **iov, int *iovlen, const char *name,
                        const void *val, size_t len) {
  int i = *iovlen;
  struct iovec *tmp = realloc(*iov, sizeof(**iov) * (i + 2));
  if (!tmp)
    return false;
  *iov = tmp;

  char *name_copy = strdup(name);
  if (!name_copy)
    return false;

  (*iov)[i].iov_base = name_copy;
  (*iov)[i].iov_len = strlen(name) + 1;
  i++;
  (*iov)[i].iov_base = (void *)val;
  if (len == (size_t)-1)
    len = val ? strlen((const char *)val) + 1 : 0;
  (*iov)[i].iov_len = (int)len;
  *iovlen = i + 1;
  return true;
}

static void free_iovec(struct iovec *iov, int iovlen) {
  if (!iov)
    return;
  for (int i = 0; i < iovlen; i += 2)
    free(iov[i].iov_base);
  free(iov);
}

static void detach_md_unit(unsigned md_unit) {
  int mdctl = open("/dev/mdctl", O_RDWR);
  if (mdctl < 0) {
    log_debug("  [UFS] open /dev/mdctl for detach failed: %s", strerror(errno));
    return;
  }

  struct md_ioctl mdio;
  memset(&mdio, 0, sizeof(mdio));
  mdio.md_version = MDIOVERSION;
  mdio.md_unit = md_unit;
  if (ioctl(mdctl, MDIOCDETACH, &mdio) != 0) {
    log_debug("  [UFS] MDIOCDETACH /dev/md%u failed: %s", md_unit,
              strerror(errno));
  }
  close(mdctl);
}

static bool is_ufs_image(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot)
    return false;
  return (strcasecmp(dot, ".ffpkg") == 0 || strcasecmp(dot, ".dat") == 0);
}

static void strip_extension(const char *filename, char *out, size_t out_size) {
  const char *dot = strrchr(filename, '.');
  size_t len = dot ? (size_t)(dot - filename) : strlen(filename);
  if (len >= out_size)
    len = out_size - 1;
  memcpy(out, filename, len);
  out[len] = '\0';
}

static void build_ufs_mount_point(const char *file_path, char *mount_point,
                                  size_t mount_point_size) {
  const char *filename = strrchr(file_path, '/');
  filename = filename ? filename + 1 : file_path;
  char mount_name[MAX_PATH];
  strip_extension(filename, mount_name, sizeof(mount_name));
  snprintf(mount_point, mount_point_size, "%s/%s", UFS_MOUNT_BASE, mount_name);
}

static void unmount_ufs_image(const char *file_path, unsigned md_unit) {
  char mount_point[MAX_PATH];
  build_ufs_mount_point(file_path, mount_point, sizeof(mount_point));

  if (unmount(mount_point, 0) != 0) {
    int unmount_err = errno;
    if (unmount_err != ENOENT && unmount_err != EINVAL) {
      if (unmount(mount_point, MNT_FORCE) != 0 && errno != ENOENT &&
          errno != EINVAL) {
        log_debug("  [UFS] unmount failed for %s: %s", mount_point,
                  strerror(errno));
      }
    }
  }

  if (md_unit != 0)
    detach_md_unit(md_unit);
}

static bool mount_ufs_image(const char *file_path) {
  // Check if already in UFS cache
  for (int k = 0; k < MAX_UFS_MOUNTS; k++) {
    if (ufs_cache[k].valid && strcmp(ufs_cache[k].path, file_path) == 0)
      return true;
  }

  // Extract filename for logs and mount point
  const char *filename = strrchr(file_path, '/');
  filename = filename ? filename + 1 : file_path;
  char mount_point[MAX_PATH];
  build_ufs_mount_point(file_path, mount_point, sizeof(mount_point));

  // Check if already mounted (e.g. from previous run)
  struct stat mst;
  if (stat(mount_point, &mst) == 0 && S_ISDIR(mst.st_mode)) {
    // Check if there's content inside (already mounted)
    DIR *check = opendir(mount_point);
    if (check) {
      int count = 0;
      struct dirent *ce;
      while ((ce = readdir(check)) != NULL) {
        if (ce->d_name[0] != '.') {
          count++;
          break;
        }
      }
      closedir(check);
      if (count > 0) {
        log_debug("  [UFS] Already mounted: %s", mount_point);
        // Add to cache so we don't re-check
        cache_ufs_mount(file_path, 0);
        return true;
      }
    }
  }

  // Get file size
  struct stat st;
  if (stat(file_path, &st) != 0) {
    log_debug("  [UFS] stat failed for %s: %s", file_path, strerror(errno));
    return false;
  }

  // Stability check - wait if file is still being written
  double age = difftime(time(NULL), st.st_mtime);
  if (age < 10.0) {
    log_debug("  [UFS] %s modified %.0fs ago, waiting...", filename, age);
    return false;
  }

  log_debug("  [UFS] Mounting image: %s -> %s", file_path, mount_point);

  // Create mount point
  mkdir(UFS_MOUNT_BASE, 0777);
  mkdir(mount_point, 0777);

  // Attach as md device
  int mdctl = open("/dev/mdctl", O_RDWR);
  if (mdctl < 0) {
    log_debug("  [UFS] open /dev/mdctl failed: %s", strerror(errno));
    return false;
  }

  struct md_ioctl mdio;
  memset(&mdio, 0, sizeof(mdio));
  mdio.md_version = MDIOVERSION;
  mdio.md_type = MD_VNODE;
  mdio.md_file = (char *)file_path;
  mdio.md_mediasize = st.st_size;
  mdio.md_sectorsize = 512; // was 512
  mdio.md_options = MD_AUTOUNIT | MD_READONLY | MD_ASYNC; 

  int ret = ioctl(mdctl, MDIOCATTACH, &mdio);
  if (ret != 0) {
    mdio.md_options = MD_AUTOUNIT;
    ret = ioctl(mdctl, MDIOCATTACH, &mdio);
    if (ret != 0) {
      log_debug("  [UFS] MDIOCATTACH failed: %s", strerror(errno));
      close(mdctl);
      return false;
    }
  }

  char devname[64];
  snprintf(devname, sizeof(devname), "/dev/md%u", mdio.md_unit);
  log_debug("  [UFS] Attached as %s", devname);
  close(mdctl);

  struct iovec *iov = NULL;
  int iovlen = 0;
  if (!build_iovec(&iov, &iovlen, "from", devname, (size_t)-1) ||
      !build_iovec(&iov, &iovlen, "fspath", mount_point, (size_t)-1) ||
      !build_iovec(&iov, &iovlen, "fstype", "exfatfs", (size_t)-1) ||
      !build_iovec(&iov, &iovlen, "large", "yes", (size_t)-1) ||
      !build_iovec(&iov, &iovlen, "timezone", "static", (size_t)-1) ||
      !build_iovec(&iov, &iovlen, "async", NULL, (size_t)-1) ||
      !build_iovec(&iov, &iovlen, "ignoreacl", NULL, (size_t)-1)) {
    log_debug("  [UFS] build_iovec failed");
    free_iovec(iov, iovlen);
    detach_md_unit(mdio.md_unit);
    return false;
  }

  ret = nmount(iov, iovlen, MNT_RDONLY);
  if (ret != 0) {
    log_debug("  [UFS] nmount rdonly failed: %s, trying rw...",
              strerror(errno));
    ret = nmount(iov, iovlen, 0);
    if (ret != 0) {
      log_debug("  [UFS] nmount failed: %s", strerror(errno));
      free_iovec(iov, iovlen);
      detach_md_unit(mdio.md_unit);
      return false;
    }
  }
  free_iovec(iov, iovlen);

  log_debug("  [UFS] Mounted %s -> %s", devname, mount_point);

  // Cache it
  cache_ufs_mount(file_path, mdio.md_unit);

  return true;
}

static void scan_ufs_images() {
  // UFS cache handles mount/unmount lifecycle only.
  for (int k = 0; k < MAX_UFS_MOUNTS; k++) {
    if (ufs_cache[k].valid && access(ufs_cache[k].path, F_OK) != 0) {
      log_debug("  [UFS] Source removed, unmounting: %s", ufs_cache[k].path);
      unmount_ufs_image(ufs_cache[k].path, ufs_cache[k].md_unit);
      ufs_cache[k].valid = false;
    }
  }

  for (int i = 0; SCAN_PATHS[i] != NULL; i++) {
    // Skip the UFS mount base itself to avoid recursion
    if (strcmp(SCAN_PATHS[i], UFS_MOUNT_BASE) == 0)
      continue;

    DIR *d = opendir(SCAN_PATHS[i]);
    if (!d)
      continue;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
      if (entry->d_name[0] == '.')
        continue;
      if (!is_ufs_image(entry->d_name))
        continue;

      char full_path[MAX_PATH];
      snprintf(full_path, sizeof(full_path), "%s/%s", SCAN_PATHS[i],
               entry->d_name);

      // Verify it's a regular file
      struct stat st;
      if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode))
        continue;

      mount_ufs_image(full_path);
    }
    closedir(d);
  }
}

static void shutdown_ufs_mounts(void) {
  for (int k = 0; k < MAX_UFS_MOUNTS; k++) {
    if (!ufs_cache[k].valid)
      continue;
    unmount_ufs_image(ufs_cache[k].path, ufs_cache[k].md_unit);
    ufs_cache[k].valid = false;
  }
}

// --- JSON  ---
static int extract_json_string(const char *json, const char *key, char *out,
                               size_t out_size) {
  char search[64];
  snprintf(search, sizeof(search), "\"%s\"", key);
  const char *p = strstr(json, search);
  if (!p)
    return -1;
  p = strchr(p + strlen(search), ':');
  if (!p)
    return -2;
  while (*++p && isspace(*p)) { /*skip*/
  }
  if (*p != '"')
    return -3;
  p++;
  size_t i = 0;
  while (i < out_size - 1 && p[i] && p[i] != '"') {
    out[i] = p[i];
    i++;
  }
  out[i] = '\0';
  return 0;
}

bool get_game_info(const char *base_path, char *out_id, char *out_name) {
  out_id[0] = '\0';
  out_name[0] = '\0';

  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s/sce_sys/param.json", base_path);
  FILE *f = fopen(path, "rb");
  if (f) {
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0) {
      char *buf = (char *)malloc(len + 1);
      if (buf) {
        fread(buf, 1, len, f);
        buf[len] = '\0';
        int res = extract_json_string(buf, "titleId", out_id, MAX_TITLE_ID);
        if (res != 0)
          res = extract_json_string(buf, "title_id", out_id, MAX_TITLE_ID);
        if (res == 0) {
          const char *en_ptr = strstr(buf, "\"en-US\"");
          const char *search_start = en_ptr ? en_ptr : buf;
          if (extract_json_string(search_start, "titleName", out_name,
                                  MAX_TITLE_NAME) != 0)
            extract_json_string(buf, "titleName", out_name, MAX_TITLE_NAME);
          if (strlen(out_name) == 0)
            copy_cstr(out_name, MAX_TITLE_NAME, out_id);
          free(buf);
          fclose(f);
          return true;
        }
        free(buf);
      }
    }
    fclose(f);
  }
  return false;
}

// --- COUNTING ---
int count_new_candidates() {
  int count = 0;
  for (int i = 0; SCAN_PATHS[i] != NULL; i++) {
    DIR *d = opendir(SCAN_PATHS[i]);
    if (!d)
      continue;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
      if (entry->d_name[0] == '.')
        continue;
      char full_path[MAX_PATH];
      snprintf(full_path, sizeof(full_path), "%s/%s", SCAN_PATHS[i],
               entry->d_name);

      char title_id[MAX_TITLE_ID];
      char title_name[MAX_TITLE_NAME];
      if (!get_game_info(full_path, title_id, title_name))
        continue;
      if (is_installed(title_id) && is_data_mounted(title_id))
        continue;

      bool already_seen = false;
      for (int k = 0; k < MAX_PENDING; k++) {
        if (cache[k].valid && strcmp(cache[k].path, full_path) == 0) {
          already_seen = true;
          break;
        }
      }
      if (already_seen)
        continue;

      count++;
    }
    closedir(d);
  }
  return count;
}

bool mount_and_install(const char *src_path, const char *title_id,
                       const char *title_name, bool is_remount) {
  char system_ex_app[MAX_PATH];
  char user_app_dir[MAX_PATH];
  char user_sce_sys[MAX_PATH];
  char src_sce_sys[MAX_PATH];

  // MOUNT
  snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);
  mkdir(system_ex_app, 0777);
  remount_system_ex();
  unmount(system_ex_app, 0);
  if (mount_nullfs(src_path, system_ex_app) < 0) {
    log_debug("  [MOUNT] FAIL: %s", strerror(errno));
    return false;
  }

  // COPY FILES
  if (!is_remount) {
    snprintf(user_app_dir, sizeof(user_app_dir), "/user/app/%s", title_id);
    snprintf(user_sce_sys, sizeof(user_sce_sys), "%s/sce_sys", user_app_dir);
    mkdir(user_app_dir, 0777);
    mkdir(user_sce_sys, 0777);

    snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", src_path);
    copy_dir(src_sce_sys, user_sce_sys);

    char icon_src[MAX_PATH], icon_dst[MAX_PATH];
    snprintf(icon_src, sizeof(icon_src), "%s/sce_sys/icon0.png", src_path);
    snprintf(icon_dst, sizeof(icon_dst), "/user/app/%s/icon0.png", title_id);
    copy_file(icon_src, icon_dst);
  } else {
    log_debug("  [SPEED] Skipping file copy (Assets already exist)");
  }

  // WRITE TRACKER
  char lnk_path[MAX_PATH];
  snprintf(lnk_path, sizeof(lnk_path), "/user/app/%s/mount.lnk", title_id);
  FILE *flnk = fopen(lnk_path, "w");
  if (flnk) {
    fprintf(flnk, "%s", src_path);
    fclose(flnk);
  }

  // REGISTER
  int res = sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", 0);
  sceKernelUsleep(200000);

  if (res == 0) {
    log_debug("  [REG] Installed NEW!");
    trigger_rich_toast(title_id, title_name, "Installed");
  } else if (res == 0x80990002) {
    log_debug("  [REG] Restored.");
    // Silent on restore/remount to avoid spam
  } else {
    log_debug("  [REG] FAIL: 0x%x", res);
    return false;
  }
  return true;
}

void scan_all_paths() {

  // Cache Cleaner
  for (int k = 0; k < MAX_PENDING; k++) {
    if (cache[k].valid) {
      if (access(cache[k].path, F_OK) != 0) {
        cache[k].valid = false;
      }
    }
  }

  for (int i = 0; SCAN_PATHS[i] != NULL; i++) {
    DIR *d = opendir(SCAN_PATHS[i]);
    if (!d)
      continue;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {

      if (entry->d_name[0] == '.')
        continue;
      char full_path[MAX_PATH];
      snprintf(full_path, sizeof(full_path), "%s/%s", SCAN_PATHS[i],
               entry->d_name);

      bool already_seen = false;
      for (int k = 0; k < MAX_PENDING; k++) {
        if (cache[k].valid && strcmp(cache[k].path, full_path) == 0) {
          already_seen = true;
          break;
        }
      }
      if (already_seen)
        continue;

      char title_id[MAX_TITLE_ID];
      char title_name[MAX_TITLE_NAME];
      if (!get_game_info(full_path, title_id, title_name)) {
        continue;
      }

      // 1. Skip if perfect
      bool installed = is_installed(title_id);
      if (installed && is_data_mounted(title_id)) {
        continue;
      }

      // 2. Decide Action
      bool is_remount = false;
      if (installed) {
        log_debug("  [ACTION] Remounting: %s", title_name);
        // NOTIFICATION REMOVED FOR REMOUNT
        is_remount = true;
      } else {
        log_debug("  [ACTION] Installing: %s", title_name);
        notify_system("Installing: %s...", title_name);

        // FAST CHECK
        if (!wait_for_stability_fast(full_path, title_name))
          continue;
        is_remount = false;
      }

      if (mount_and_install(full_path, title_id, title_name, is_remount)) {
        cache_game_entry(full_path, title_id, title_name);
      }
    }
    closedir(d);
  }
}

int main() {
  // Initialize services
  sceUserServiceInitialize(0);
  sceAppInstUtilInitialize();
  kernel_set_ucred_authid(-1, 0x4801000000000013L);

  remove(LOCK_FILE);
  remove(LOG_FILE);
  mkdir(LOG_DIR, 0777);

  log_debug("SHADOWMOUNT v1.4 START exFAT");

  // --- MOUNT UFS IMAGES ---
  scan_ufs_images();

  // --- STARTUP LOGIC ---
  int new_games = count_new_candidates();

  if (new_games == 0) {
    // SCENARIO A: Nothing to do.
    notify_system("ShadowMount v1.4 exFAT: Library Ready.\n- VoidWhisper");
  } else {
    // SCENARIO B: Work needed.
    notify_system("ShadowMount v1.4 exFAT: Found %d Games. Executing...", new_games);

    // Run the scan immediately to process them
    scan_all_paths();

    // Completion Message
    notify_system("Library Synchronized. - VoidWhisper");
  }

  // --- DAEMON LOOP ---
  int lock = open(LOCK_FILE, O_CREAT | O_EXCL | O_RDWR, 0666);
  if (lock < 0 && errno == EEXIST) {
    sceUserServiceTerminate();
    return 0;
  }

  while (true) {
    if (access(KILL_FILE, F_OK) == 0) {
      remove(KILL_FILE);
      break;
    }

    // Sleep FIRST since we either just finished scan above, or library was
    // ready.
    sceKernelUsleep(SCAN_INTERVAL_US);

    scan_ufs_images();
    scan_all_paths();
  }

  if (lock >= 0)
    close(lock);
  shutdown_ufs_mounts();
  remove(LOCK_FILE);
  sceUserServiceTerminate();
  return 0;
}
