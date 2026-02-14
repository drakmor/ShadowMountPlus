#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/mdioctl.h>
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
#define MAX_NULLFS_MOUNTS MAX_PENDING
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
// 1 = use legacy /dev/mdctl backend for .exfat images, 0 = use LVD for all.
#define EXFAT_ATTACH_USE_MDCTL 1

// --- LVD Definitions ---
// Observed parameter variants in refs/libSceFsInternalForVsh.sprx.c:
// - ioctl: ATTACH=0xC0286D00, DETACH=0xC0286D01, ATTACH2 path=0xC0286D09.
// - single-image path uses raw option flags 0x8/0x9 -> normalized 0x14/0x1C.
// - DownloadData/LWFS path (imgtype=7) uses normalized options 0x16/0x1E.
// - image_type values accepted by validator: 0..0xC (13 values total).
//   this code uses image_type=7 for UFS (DownloadData-like), 0 for generic path.
// - layer source_type observed: 1=file, 2=char/block-like source (/dev/sbram0).
// - layer entry flag bit0 is "no bitmap file specified".
#define LVD_CTRL_PATH "/dev/lvdctl"
#define MD_CTRL_PATH "/dev/mdctl"
#define SCE_LVD_IOC_ATTACH 0xC0286D00
#define SCE_LVD_IOC_DETACH 0xC0286D01
#define LVD_ATTACH_IO_VERSION 1
#define LVD_ATTACH_OPTION_FLAGS_DEFAULT 0x8
#define LVD_ATTACH_OPTION_FLAGS_RW 0x9
#define LVD_ATTACH_OPTION_NORM_DD_RO 0x16
#define LVD_ATTACH_OPTION_NORM_DD_RW 0x1E
#define LVD_SECTOR_SIZE_DEFAULT 512u
#define LVD_SECTOR_SIZE_MAX 4096u
// Raw option bits are normalized by sceFsLvdAttachCommon before validation:
// raw:0x1->norm:0x08, raw:0x2->norm:0x80, raw:0x4->norm:0x02, raw:0x8->norm:0x10.
// The normalized masks are then checked against validator constraints (0x82/0x92).
// Reference notes: docs/fs_mounting_re_notes.md ("Raw option bits -> validator bits").
#define LVD_ATTACH_IMAGE_TYPE 0
#define LVD_ATTACH_IMAGE_TYPE_UFS_DOWNLOAD_DATA 7
#define LVD_ATTACH_LAYER_COUNT 1
#define LVD_ATTACH_LAYER_ARRAY_SIZE 3
#define LVD_ENTRY_TYPE_FILE 1
#define LVD_ENTRY_FLAG_NO_BITMAP 0x1
#define LVD_NODE_WAIT_US 100000
#define LVD_NODE_WAIT_RETRIES 100
#define UFS_NMOUNT_FLAG_BASE 0x10000000u

// --- devpfs/pfs option profiles  ---
// PFS nmount key/value variants observed in refs:
// - fstype: "pfs", "transaction_pfs", "ppr_pfs"
// - mkeymode: "SD"
// - budgetid:  "game"/"system" in init paths
// - sigverify/playgo/disc: "0" or "1"
// - optional keys in specific flows: ekpfs/eekpfs, eekc, pubkey_ver, key_ver,
//   finalized, ppkg_opt, sblock_offset, maxpkgszingib
#define DEVPFS_BUDGET_GAME "game"
#define DEVPFS_BUDGET_SYSTEM "system"
#define DEVPFS_MKEYMODE_SD "SD"

// devpfs mount flag bit to nmount-key mapping (FSMP path):
#define DEVPFS_MOUNT_FLAG_CREATE_WS 0x0001u          // create workspace
#define DEVPFS_MOUNT_FLAG_MULTIMNT 0x0002u           // "multimnt"
#define DEVPFS_MOUNT_FLAG_BLKONRESLV 0x0004u         // "blkonreslv"
#define DEVPFS_MOUNT_FLAG_HOTUPDATE 0x0010u          // "hotupdate"
#define DEVPFS_MOUNT_FLAG_NOMULTILAYEREDMNT 0x0080u  // "nomultilayeredmnt"
#define DEVPFS_MOUNT_FLAG_FSMPSTAT 0x0100u           // "fsmpstat"
#define DEVPFS_MOUNT_FLAG_BLKONREAD 0x0200u          // "blkonread"
#define DEVPFS_MOUNT_FLAG_IGNORESPARSE 0x0400u       // "ignoresparse"
#define DEVPFS_MOUNT_FLAG_FORCERW 0x1000u            // "forcerw"
#define DEVPFS_MOUNT_FLAG_FGC_DEPLOYED 0x2000u       // "fgc-deployed"
#define DEVPFS_MOUNT_FLAG_WS_MGMT 0x4000u            // "ws-mgmt"
#define DEVPFS_MOUNT_FLAG_NORWMNT 0x8000u            // "norwmnt"

#define DEVPFS_FLAGS_SHELL_DEFAULT 0x0032u
#define DEVPFS_FLAGS_SHELL_FGC 0x009Fu
#define DEVPFS_FLAGS_SHELL_FGC_DISC 0x4022u
#define DEVPFS_FLAGS_SHELL_FGC_NORW 0x8022u

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

_Static_assert(offsetof(devpfs_mount_opt_t, ro) == 0x00,
               "devpfs_mount_opt_t.ro offset");
_Static_assert(offsetof(devpfs_mount_opt_t, budget_id) == 0x08,
               "devpfs_mount_opt_t.budget_id offset");
_Static_assert(offsetof(devpfs_mount_opt_t, flags) == 0x14,
               "devpfs_mount_opt_t.flags offset");
_Static_assert(offsetof(devpfs_mount_opt_t, max_pkg_gib) == 0x18,
               "devpfs_mount_opt_t.max_pkg_gib offset");
_Static_assert(sizeof(devpfs_mount_opt_t) == 0x20,
               "devpfs_mount_opt_t size");

typedef struct {
  // Human-readable profile id for logs.
  const char *name;
  // Preferred mount mode for initial nmount/attach attempt.
  bool read_only;
  // PFS nmount "budgetid" ("game", "system").
  const char *budget_id;
  // PFS nmount "mkeymode" ("SD").
  const char *mkeymode;
  // PFS nmount "sigverify" (0/1).
  bool sigverify;
  // PFS nmount "playgo" (0/1).
  bool playgo;
  // PFS nmount "disc" (0/1).
  bool disc;
} pfs_mount_profile_t;

static const pfs_mount_profile_t k_pfs_profile_shell_default = {
    .name = "shell_default",
    .read_only = true,
    .budget_id = DEVPFS_BUDGET_GAME,
    .mkeymode = DEVPFS_MKEYMODE_SD,
    .sigverify = false,
    .playgo = false,
    .disc = false,
};

typedef struct {
  // Source object class (observed: 1=file, 2=device-like source).
  uint16_t source_type;
  // Layer behavior flags (observed bit0 = no bitmap file path).
  uint8_t entry_flags;
  // Must be zero.
  uint8_t reserved0;
  // Must be zero.
  uint32_t reserved1;
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
} lvd_kernel_layer_t;

typedef struct {
  // Protocol version for /dev/lvdctl ioctl payload (valid <=1).
  uint32_t io_version;
  // Input: usually -1 for auto-assign. Output: created lvd unit id.
  int32_t device_id;
  // Sector-like size fields used by LVD attach request validation.
  // In refs these are populated from statfs and clamped to <= 4096.
  uint32_t sector_size_0;
  uint32_t sector_size_1;
  // Encoded option length derived from option flags (0x14 for 0x8, 0x1C for 0x9).
  uint16_t option_len;
  // LVD image type id (validator accepts 0..0xC; this code uses 0).
  uint16_t image_type;
  // Number of valid entries in layers_ptr.
  uint32_t layer_count;
  // Total exported virtual size (bytes).
  uint64_t device_size;
  // Pointer to layer array in user payload.
  lvd_kernel_layer_t *layers_ptr;
} lvd_ioctl_attach_t;

typedef struct {
  // Must be zero.
  uint32_t reserved0;
  // Target lvd unit id to detach.
  int32_t device_id;
  // Reserved padding required by kernel ABI.
  uint8_t reserved[0x20];
} lvd_ioctl_detach_t;

_Static_assert(sizeof(lvd_kernel_layer_t) == 0x38, "lvd_kernel_layer_t size");
_Static_assert(sizeof(lvd_ioctl_attach_t) == 0x28, "lvd_ioctl_attach_t size");
_Static_assert(sizeof(lvd_ioctl_detach_t) == 0x28, "lvd_ioctl_detach_t size");

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

typedef enum {
  ATTACH_BACKEND_NONE = 0,
  // /dev/lvdctl -> /dev/lvdN
  ATTACH_BACKEND_LVD,
  // /dev/mdctl -> /dev/mdN
  ATTACH_BACKEND_MD,
} attach_backend_t;

struct UfsCache {
  // Absolute source image path.
  char path[MAX_PATH];
  // Attached unit id (lvdN/mdN), -1 when unknown.
  int unit_id;
  // Backend used for this entry.
  attach_backend_t backend;
  // Slot occupancy flag.
  bool valid;
};
struct UfsCache ufs_cache[MAX_UFS_MOUNTS];

struct NullfsCache {
  // Mounted target path in /system_ex/app/...
  char mount_point[MAX_PATH];
  // Slot occupancy flag.
  bool valid;
};
struct NullfsCache nullfs_cache[MAX_NULLFS_MOUNTS];

static volatile sig_atomic_t g_stop_requested = 0;

typedef enum {
  IMAGE_FS_UNKNOWN = 0,
  IMAGE_FS_UFS,
  IMAGE_FS_EXFAT,
  IMAGE_FS_PFS,
} image_fs_type_t;

static const char *backend_name(attach_backend_t backend);

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

static void cache_ufs_mount(const char *path, int unit_id,
                            attach_backend_t backend) {
  for (int k = 0; k < MAX_UFS_MOUNTS; k++) {
    if (ufs_cache[k].valid && strcmp(ufs_cache[k].path, path) == 0) {
      ufs_cache[k].unit_id = unit_id;
      ufs_cache[k].backend = backend;
      return;
    }
  }

  for (int k = 0; k < MAX_UFS_MOUNTS; k++) {
    if (!ufs_cache[k].valid) {
      copy_cstr(ufs_cache[k].path, sizeof(ufs_cache[k].path), path);
      ufs_cache[k].unit_id = unit_id;
      ufs_cache[k].backend = backend;
      ufs_cache[k].valid = true;
      return;
    }
  }
}

static void cache_nullfs_mount(const char *mount_point) {
  for (int k = 0; k < MAX_NULLFS_MOUNTS; k++) {
    if (nullfs_cache[k].valid &&
        strcmp(nullfs_cache[k].mount_point, mount_point) == 0) {
      return;
    }
  }
  for (int k = 0; k < MAX_NULLFS_MOUNTS; k++) {
    if (!nullfs_cache[k].valid) {
      copy_cstr(nullfs_cache[k].mount_point, sizeof(nullfs_cache[k].mount_point),
                mount_point);
      nullfs_cache[k].valid = true;
      return;
    }
  }
}

static void invalidate_nullfs_mount(const char *mount_point) {
  for (int k = 0; k < MAX_NULLFS_MOUNTS; k++) {
    if (!nullfs_cache[k].valid)
      continue;
    if (strcmp(nullfs_cache[k].mount_point, mount_point) == 0) {
      nullfs_cache[k].valid = false;
      nullfs_cache[k].mount_point[0] = '\0';
      return;
    }
  }
}

static void on_signal(int sig) {
  (void)sig;
  g_stop_requested = 1;
}

static void install_signal_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGQUIT, &sa, NULL);
}

static bool should_stop_requested(void) {
  if (g_stop_requested)
    return true;
  if (access(KILL_FILE, F_OK) == 0) {
    remove(KILL_FILE);
    return true;
  }
  return false;
}

static bool sleep_with_stop_check(unsigned int total_us) {
  const unsigned int chunk_us = 200000;
  unsigned int slept = 0;
  while (slept < total_us) {
    if (should_stop_requested())
      return true;
    unsigned int remain = total_us - slept;
    unsigned int step = remain < chunk_us ? remain : chunk_us;
    sceKernelUsleep(step);
    slept += step;
  }
  return should_stop_requested();
}

// --- LOGGING ---
void log_to_file(const char *fmt, va_list args) {
  mkdir(LOG_DIR, 0777);
  FILE *fp = fopen(LOG_FILE, "a");
  if (fp) {
    va_list args_file;
    time_t rawtime;
    struct tm *timeinfo;
    char buffer[80];
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buffer, sizeof(buffer), "%H:%M:%S", timeinfo);
    va_copy(args_file, args);
    fprintf(fp, "[%s] ", buffer);
    vfprintf(fp, fmt, args_file);
    fprintf(fp, "\n");
    va_end(args_file);
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

static bool read_mount_link(const char *title_id, char *out, size_t out_size) {
  if (out_size == 0)
    return false;
  out[0] = '\0';

  char lnk_path[MAX_PATH];
  snprintf(lnk_path, sizeof(lnk_path), "/user/app/%s/mount.lnk", title_id);
  FILE *f = fopen(lnk_path, "r");
  if (!f)
    return false;

  if (!fgets(out, (int)out_size, f)) {
    fclose(f);
    out[0] = '\0';
    return false;
  }
  fclose(f);

  size_t len = strcspn(out, "\r\n");
  out[len] = '\0';
  return out[0] != '\0';
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

static bool unmount_nullfs_mount(const char *mount_point) {
  if (unmount(mount_point, 0) == 0) {
    invalidate_nullfs_mount(mount_point);
    return true;
  }

  int err = errno;
  if (err == ENOENT || err == EINVAL) {
    invalidate_nullfs_mount(mount_point);
    return true;
  }

  log_debug("  [MOUNT] unmount failed for %s: %s, trying force...",
            mount_point, strerror(err));
  if (unmount(mount_point, MNT_FORCE) == 0 || errno == ENOENT ||
      errno == EINVAL) {
    invalidate_nullfs_mount(mount_point);
    return true;
  }

  log_debug("  [MOUNT] force unmount failed for %s: %s", mount_point,
            strerror(errno));
  return false;
}

static int copy_dir(const char *src, const char *dst) {
  if (mkdir(dst, 0777) != 0 && errno != EEXIST)
    return -1;
  DIR *d = opendir(src);
  if (!d)
    return -1;
  int ret = 0;
  struct dirent *e;
  char ss[MAX_PATH], dd[MAX_PATH];
  struct stat st;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    snprintf(ss, sizeof(ss), "%s/%s", src, e->d_name);
    snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);
    if (stat(ss, &st) != 0) {
      ret = -1;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      if (copy_dir(ss, dd) != 0) {
        ret = -1;
        break;
      }
    }
    else {
      FILE *fs = fopen(ss, "rb");
      if (!fs) {
        ret = -1;
        break;
      }
      FILE *fd = fopen(dd, "wb");
      if (!fd) {
        fclose(fs);
        ret = -1;
        break;
      }
      char buf[8192];
      bool copy_ok = true;
      while (copy_ok) {
        size_t n = fread(buf, 1, sizeof(buf), fs);
        if (n > 0 && fwrite(buf, 1, n, fd) != n)
          copy_ok = false;
        if (n < sizeof(buf)) {
          if (ferror(fs))
            copy_ok = false;
          break;
        }
      }
      if (fflush(fd) != 0)
        copy_ok = false;
      if (fclose(fd) != 0)
        copy_ok = false;
      if (fclose(fs) != 0)
        copy_ok = false;
      if (!copy_ok) {
        ret = -1;
        break;
      }
    }
  }
  if (closedir(d) != 0)
    ret = -1;
  return ret;
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
  int ret = 0;
  while (true) {
    size_t n = fread(buf, 1, sizeof(buf), fs);
    if (n > 0 && fwrite(buf, 1, n, fd) != n) {
      ret = -1;
      break;
    }
    if (n < sizeof(buf)) {
      if (ferror(fs))
        ret = -1;
      break;
    }
  }
  if (fflush(fd) != 0)
    ret = -1;
  if (fclose(fd) != 0)
    ret = -1;
  if (fclose(fs) != 0)
    ret = -1;
  return ret;
}

// --- UFS2 IMAGE MOUNTING (LVD) ---
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

static bool wait_for_dev_node_state(const char *devname, bool should_exist) {
  for (int i = 0; i < LVD_NODE_WAIT_RETRIES; i++) {
    bool exists = (access(devname, F_OK) == 0);
    if (exists == should_exist)
      return true;
    sceKernelUsleep(LVD_NODE_WAIT_US);
  }

  return false;
}

static bool wait_for_lvd_node_state(int unit_id, bool should_exist) {
  char devname[64];
  snprintf(devname, sizeof(devname), "/dev/lvd%d", unit_id);
  return wait_for_dev_node_state(devname, should_exist);
}

static bool wait_for_md_node_state(int unit_id, bool should_exist) {
  char devname[64];
  snprintf(devname, sizeof(devname), "/dev/md%d", unit_id);
  return wait_for_dev_node_state(devname, should_exist);
}

static uint32_t get_lvd_sector_size(const char *path) {
  struct statfs sfs;
  if (path && statfs(path, &sfs) == 0) {
    uint64_t sector = (uint64_t)sfs.f_bsize;
    if (sector == 0) 
      return LVD_SECTOR_SIZE_DEFAULT;
    if (sector > LVD_SECTOR_SIZE_MAX) 
      sector = LVD_SECTOR_SIZE_MAX;
    return (uint32_t)sector;
  }
  return LVD_SECTOR_SIZE_DEFAULT;
}

static uint16_t lvd_option_len_from_flags(uint16_t options) {
  // Exact mirror of dr_lvd_attach_sub_7810 option-size derivation:
  // refs/libSceFsInternalForVsh.sprx.c (sceFsLvdAttachCommon, around +0x8295).
  // Practical values for this project:
  // - flags 0x8 (default/RO): option_len 0x14
  // - flags 0x9 (RW):         option_len 0x1C
  if ((options & 0x800E) != 0) {
    return (uint16_t)((options & 0xFFFF8000) + ((options & 2) << 6) +
                      (8 * (options & 1)) + (2 * ((options >> 2) & 1)) +
                      (2 * (options & 8)) + 4);
  }
  return (uint16_t)(8 * (options & 1) + 4);
}

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

static bool resolve_device_from_mount(const char *mount_point,
                                      attach_backend_t *backend_out,
                                      int *unit_out) {
  struct statfs sfs;
  if (statfs(mount_point, &sfs) != 0)
    return false;

  const char *from = sfs.f_mntfromname;
  if (parse_unit_from_dev_path(from, "/dev/lvd", unit_out)) {
    *backend_out = ATTACH_BACKEND_LVD;
    return true;
  }
  if (parse_unit_from_dev_path(from, "/dev/md", unit_out)) {
    *backend_out = ATTACH_BACKEND_MD;
    return true;
  }
  return false;
}

static bool is_active_image_mount_point(const char *path) {
  attach_backend_t backend = ATTACH_BACKEND_NONE;
  int unit = -1;
  return resolve_device_from_mount(path, &backend, &unit);
}

// LVD Detach Helper
static void detach_lvd_unit(int unit_id) {
  if (unit_id < 0)
    return;

  int fd = open(LVD_CTRL_PATH, O_RDWR);
  if (fd < 0) {
    log_debug("  [IMG][%s] open %s for detach failed: %s",
              backend_name(ATTACH_BACKEND_LVD), LVD_CTRL_PATH, strerror(errno));
    return;
  }

  lvd_ioctl_detach_t req;
  memset(&req, 0, sizeof(req));
  req.device_id = unit_id;

  if (ioctl(fd, SCE_LVD_IOC_DETACH, &req) != 0) {
    log_debug("  [IMG][%s] detach %d failed: %s",
              backend_name(ATTACH_BACKEND_LVD), unit_id, strerror(errno));
  }
  close(fd);

  if (!wait_for_lvd_node_state(unit_id, false)) {
    log_debug("  [IMG][%s] device node still present after detach: /dev/lvd%d",
              backend_name(ATTACH_BACKEND_LVD), unit_id);
  }
}

static void detach_md_unit(int unit_id) {
  if (unit_id < 0)
    return;

  int fd = open(MD_CTRL_PATH, O_RDWR);
  if (fd < 0) {
    log_debug("  [IMG][%s] open %s for detach failed: %s",
              backend_name(ATTACH_BACKEND_MD), MD_CTRL_PATH, strerror(errno));
    return;
  }

  struct md_ioctl req;
  memset(&req, 0, sizeof(req));
  req.md_version = MDIOVERSION;
  req.md_unit = (unsigned int)unit_id;

  if (ioctl(fd, MDIOCDETACH, &req) != 0) {
    int err = errno;
    req.md_options = MD_FORCE;
    if (ioctl(fd, MDIOCDETACH, &req) != 0) {
      log_debug("  [IMG][%s] detach %d failed: %s",
                backend_name(ATTACH_BACKEND_MD), unit_id, strerror(errno));
    } else {
      log_debug("  [IMG][%s] detach %d forced after error: %s",
                backend_name(ATTACH_BACKEND_MD), unit_id, strerror(err));
    }
  }
  close(fd);

  if (!wait_for_md_node_state(unit_id, false)) {
    log_debug("  [IMG][%s] device node still present after detach: /dev/md%d",
              backend_name(ATTACH_BACKEND_MD), unit_id);
  }
}

static void detach_attached_unit(attach_backend_t backend, int unit_id) {
  if (backend == ATTACH_BACKEND_MD)
    detach_md_unit(unit_id);
  else if (backend == ATTACH_BACKEND_LVD)
    detach_lvd_unit(unit_id);
}

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

static const char *backend_name(attach_backend_t backend) {
  switch (backend) {
  case ATTACH_BACKEND_LVD:
    return "LVD";
  case ATTACH_BACKEND_MD:
    return "MD";
  default:
    return "UNKNOWN";
  }
}

static void strip_extension(const char *filename, char *out, size_t out_size) {
  const char *dot = strrchr(filename, '.');
  size_t len = dot ? (size_t)(dot - filename) : strlen(filename);
  if (len >= out_size)
    len = out_size - 1;
  memcpy(out, filename, len);
  out[len] = '\0';
}

static const char *mount_fs_suffix(image_fs_type_t fs_type) {
  switch (fs_type) {
  case IMAGE_FS_UFS:
    return "ufs";
  case IMAGE_FS_EXFAT:
    return "exfat";
  case IMAGE_FS_PFS:
    return "pfs";
  default:
    return "unknown";
  }
}

static void build_ufs_mount_point(const char *file_path, image_fs_type_t fs_type,
                                  char *mount_point, size_t mount_point_size) {
  const char *filename = strrchr(file_path, '/');
  filename = filename ? filename + 1 : file_path;
  char mount_name[MAX_PATH];
  strip_extension(filename, mount_name, sizeof(mount_name));
  snprintf(mount_point, mount_point_size, "%s/%s-%s", UFS_MOUNT_BASE,
           mount_name, mount_fs_suffix(fs_type));
}

static void unmount_ufs_image(const char *file_path, int unit_id,
                              attach_backend_t backend) {
  char mount_point[MAX_PATH];
  image_fs_type_t fs_type = detect_image_fs_type(file_path);
  build_ufs_mount_point(file_path, fs_type, mount_point, sizeof(mount_point));
  int resolved_unit = unit_id;
  attach_backend_t resolved_backend = backend;
  bool can_detach = true;

  if (resolved_unit < 0 || resolved_backend == ATTACH_BACKEND_NONE) {
    if (!resolve_device_from_mount(mount_point, &resolved_backend,
                                   &resolved_unit)) {
      resolved_backend = ATTACH_BACKEND_NONE;
      resolved_unit = -1;
    }
  }

  // Try standard unmount
  if (unmount(mount_point, 0) != 0) {
    int unmount_err = errno;
    if (unmount_err != ENOENT && unmount_err != EINVAL) {
      if (unmount(mount_point, MNT_FORCE) != 0 && errno != ENOENT &&
          errno != EINVAL) {
        log_debug("  [IMG][%s] unmount failed for %s: %s",
                  backend_name(resolved_backend), mount_point, strerror(errno));
        can_detach = false;
      }
    }
  }

  if (!can_detach)
    return;

  if (resolved_backend == ATTACH_BACKEND_NONE) {
    log_debug("  [IMG][%s] detach skipped for %s (backend unknown)",
              backend_name(resolved_backend), mount_point);
    return;
  }

  detach_attached_unit(resolved_backend, resolved_unit);
}

static bool mount_ufs_image(const char *file_path, image_fs_type_t fs_type) {
  const pfs_mount_profile_t *pfs_profile = NULL;
  if (fs_type == IMAGE_FS_PFS)
    pfs_profile = &k_pfs_profile_shell_default;
  bool mount_read_only = true;
  if (fs_type == IMAGE_FS_PFS)
    mount_read_only = pfs_profile->read_only;

  // Check if already in UFS cache
  for (int k = 0; k < MAX_UFS_MOUNTS; k++) {
    if (ufs_cache[k].valid && strcmp(ufs_cache[k].path, file_path) == 0)
      return true;
  }

  // Extract filename for logs and mount point
  const char *filename = strrchr(file_path, '/');
  filename = filename ? filename + 1 : file_path;
  char mount_point[MAX_PATH];
  build_ufs_mount_point(file_path, fs_type, mount_point, sizeof(mount_point));

  // Check if directory exists and is populated
  struct stat mst;
  if (stat(mount_point, &mst) == 0 && S_ISDIR(mst.st_mode)) {
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
        attach_backend_t existing_backend = ATTACH_BACKEND_NONE;
        int existing_unit = -1;
        if (resolve_device_from_mount(mount_point, &existing_backend,
                                      &existing_unit)) {
          log_debug("  [IMG][%s] Already mounted: %s",
                    backend_name(existing_backend), mount_point);
          cache_ufs_mount(file_path, existing_unit, existing_backend);
          return true;
        }
        log_debug("  [IMG] Mount point exists and is non-empty but is not an "
                  "active mount, reattaching: %s",
                  mount_point);
      }
    }
  }

  // Get file size
  struct stat st;
  if (stat(file_path, &st) != 0) {
    log_debug("  [IMG] stat failed for %s: %s", file_path, strerror(errno));
    return false;
  }

  // Stability check - wait if file is still being written
  double age = difftime(time(NULL), st.st_mtime);
  if (age < 10.0) {
    log_debug("  [IMG] %s modified %.0fs ago, waiting...", filename, age);
    return false;
  }

  log_debug("  [IMG] Mounting image (%s): %s -> %s", image_fs_name(fs_type),
            file_path, mount_point);

  // Create mount point
  mkdir(UFS_MOUNT_BASE, 0777);
  mkdir(mount_point, 0777);

  attach_backend_t attach_backend = ATTACH_BACKEND_LVD;
  if (fs_type == IMAGE_FS_EXFAT && EXFAT_ATTACH_USE_MDCTL)
    attach_backend = ATTACH_BACKEND_MD;
  log_debug("  [IMG][%s] attach backend selected for %s",
            backend_name(attach_backend), file_path);

  int ret = -1;
  int unit_id = -1;
  char devname[64];
  memset(devname, 0, sizeof(devname));

  if (attach_backend == ATTACH_BACKEND_MD) {
    int md_fd = open(MD_CTRL_PATH, O_RDWR);
    if (md_fd < 0) {
      log_debug("  [IMG][%s] open %s failed: %s",
                backend_name(attach_backend), MD_CTRL_PATH, strerror(errno));
      return false;
    }

    struct md_ioctl req;
    memset(&req, 0, sizeof(req));
    req.md_version = MDIOVERSION;
    req.md_type = MD_VNODE;
    req.md_file = (char *)file_path;
    req.md_mediasize = st.st_size;
    req.md_sectorsize = 512;

    int last_errno = 0;
    const unsigned int option_flags[] = {MD_AUTOUNIT | MD_READONLY | MD_ASYNC,
                                         MD_AUTOUNIT};
    for (size_t i = 0; i < sizeof(option_flags) / sizeof(option_flags[0]);
         i++) {
      req.md_options = option_flags[i];
      log_debug("  [IMG][%s] attach try: options=0x%x",
                backend_name(attach_backend), req.md_options);
      ret = ioctl(md_fd, MDIOCATTACH, &req);
      if (ret == 0)
        break;
      last_errno = errno;
      log_debug("  [IMG][%s] attach retry with options=0x%x failed: %s",
                backend_name(attach_backend), req.md_options,
                strerror(last_errno));
    }
    close(md_fd);

    if (ret != 0) {
      errno = last_errno;
      log_debug("  [IMG][%s] attach failed: %s (ret: 0x%x)",
                backend_name(attach_backend), strerror(errno), ret);
      return false;
    }

    unit_id = (int)req.md_unit;
    if (unit_id < 0) {
      log_debug("  [IMG][%s] attach returned invalid unit: %d",
                backend_name(attach_backend), unit_id);
      return false;
    }

    if (!wait_for_md_node_state(unit_id, true)) {
      log_debug("  [IMG][%s] device node did not appear: /dev/md%d",
                backend_name(attach_backend), unit_id);
      detach_md_unit(unit_id);
      return false;
    }

    snprintf(devname, sizeof(devname), "/dev/md%d", unit_id);
    log_debug("  [IMG][%s] attach returned unit=%d",
              backend_name(attach_backend), unit_id);
  } else {
    int lvd_fd = open(LVD_CTRL_PATH, O_RDWR);
    if (lvd_fd < 0) {
      log_debug("  [IMG][%s] open %s failed: %s",
                backend_name(attach_backend), LVD_CTRL_PATH, strerror(errno));
      return false;
    }

    lvd_kernel_layer_t *layers =
        calloc(LVD_ATTACH_LAYER_ARRAY_SIZE, sizeof(*layers));
    if (!layers) {
      log_debug("  [IMG] calloc layers failed");
      close(lvd_fd);
      return false;
    }

    layers[0].source_type = LVD_ENTRY_TYPE_FILE;
    layers[0].entry_flags = LVD_ENTRY_FLAG_NO_BITMAP;
    layers[0].path = file_path;
    layers[0].offset = 0;
    layers[0].size = st.st_size;

    lvd_ioctl_attach_t req;
    memset(&req, 0, sizeof(req));
    req.io_version = LVD_ATTACH_IO_VERSION;
    req.image_type = (fs_type == IMAGE_FS_UFS) ? LVD_ATTACH_IMAGE_TYPE_UFS_DOWNLOAD_DATA
                                               : LVD_ATTACH_IMAGE_TYPE;
    req.layer_count = LVD_ATTACH_LAYER_COUNT;
    req.device_size = (uint64_t)st.st_size;
    req.layers_ptr = layers;
    req.sector_size_0 = get_lvd_sector_size(file_path);
    req.sector_size_1 = req.sector_size_0;

    int last_errno = 0;
    uint16_t attach_options[2];
    size_t option_flags_count = 0;
    bool prefer_rw = !mount_read_only;

    if (fs_type == IMAGE_FS_UFS) {
      if (prefer_rw) {
        attach_options[option_flags_count++] = LVD_ATTACH_OPTION_NORM_DD_RW;
        attach_options[option_flags_count++] = LVD_ATTACH_OPTION_NORM_DD_RO;
      } else {
        attach_options[option_flags_count++] = LVD_ATTACH_OPTION_NORM_DD_RO;
      }
    } else {
      if (prefer_rw) {
        attach_options[option_flags_count++] = LVD_ATTACH_OPTION_FLAGS_RW;
        attach_options[option_flags_count++] = LVD_ATTACH_OPTION_FLAGS_DEFAULT;
      } else {
        attach_options[option_flags_count++] = LVD_ATTACH_OPTION_FLAGS_DEFAULT;
      }
    }

    for (size_t i = 0; i < option_flags_count; i++) {
      uint16_t opt = attach_options[i];
      if (fs_type == IMAGE_FS_UFS) {
        // DownloadData/LWFS path passes normalized option mask directly.
        req.option_len = opt;
      } else {
        req.option_len = lvd_option_len_from_flags(opt);
      }
      req.device_id = -1;
      log_debug("  [IMG][%s] attach try: ver=%u sec=%u options=0x%x len=0x%x",
                backend_name(attach_backend),
                req.io_version, req.sector_size_0, opt,
                req.option_len);

      ret = ioctl(lvd_fd, SCE_LVD_IOC_ATTACH, &req);
      if (ret == 0)
        break;

      last_errno = errno;
      log_debug("  [IMG][%s] attach retry with options=0x%x option_len=0x%x "
                "failed: %s",
                backend_name(attach_backend), opt, req.option_len,
                strerror(last_errno));
    }
    close(lvd_fd);
    unit_id = req.device_id;
    free(layers);

    if (ret != 0) {
      errno = last_errno;
      log_debug("  [IMG][%s] attach failed: %s (ret: 0x%x)",
                backend_name(attach_backend), strerror(errno), ret);
      return false;
    }

    if (unit_id < 0) {
      log_debug("  [IMG][%s] attach returned invalid unit: %d",
                backend_name(attach_backend), unit_id);
      return false;
    }
    log_debug("  [IMG][%s] attach returned unit=%d",
              backend_name(attach_backend), unit_id);

    if (!wait_for_lvd_node_state(unit_id, true)) {
      log_debug("  [IMG][%s] device node did not appear: /dev/lvd%d",
                backend_name(attach_backend), unit_id);
      detach_lvd_unit(unit_id);
      return false;
    }

    snprintf(devname, sizeof(devname), "/dev/lvd%d", unit_id);
  }

  log_debug("  [IMG][%s] Attached as %s", backend_name(attach_backend), devname);

  // --- MOUNT FILESYSTEM ---
  struct iovec *iov = NULL;
  int iovlen = 0;
  bool iov_ok = false;
  char mount_errmsg[256];
  memset(mount_errmsg, 0, sizeof(mount_errmsg));

  if (fs_type == IMAGE_FS_UFS) {
    iov_ok = build_iovec(&iov, &iovlen, "fstype", "ufs", (size_t)-1) &&
             build_iovec(&iov, &iovlen, "from", devname, (size_t)-1) &&
             build_iovec(&iov, &iovlen, "fspath", mount_point, (size_t)-1) &&
             build_iovec(&iov, &iovlen, "budgetid", "game", (size_t)-1) &&
             build_iovec(&iov, &iovlen, "errmsg", mount_errmsg,
                         sizeof(mount_errmsg));
  } else if (fs_type == IMAGE_FS_EXFAT) {
    iov_ok = build_iovec(&iov, &iovlen, "from", devname, (size_t)-1) &&
             build_iovec(&iov, &iovlen, "fspath", mount_point, (size_t)-1) &&
             build_iovec(&iov, &iovlen, "fstype", "exfatfs", (size_t)-1) &&
             build_iovec(&iov, &iovlen, "large", "yes", (size_t)-1) &&
             build_iovec(&iov, &iovlen, "timezone", "static", (size_t)-1) &&
             build_iovec(&iov, &iovlen, "async", NULL, (size_t)-1) &&
             build_iovec(&iov, &iovlen, "ignoreacl", NULL, (size_t)-1);
  } else if (fs_type == IMAGE_FS_PFS) {
    const char *sigverify = pfs_profile->sigverify ? "1" : "0";
    const char *playgo = pfs_profile->playgo ? "1" : "0";
    const char *disc = pfs_profile->disc ? "1" : "0";
    // PFS mount requires this key set for shell-compatible behavior:
    // sigverify, mkeymode, budgetid, playgo, disc, errmsg.
    // Keep these explicit even when values are "0"/defaults.
    log_debug("  [IMG][%s] PFS profile=%s ro=%d budgetid=%s mkeymode=%s "
              "sigverify=%s playgo=%s disc=%s",
              backend_name(attach_backend), pfs_profile->name,
              pfs_profile->read_only ? 1 : 0, pfs_profile->budget_id,
              pfs_profile->mkeymode, sigverify, playgo, disc);

    iov_ok = build_iovec(&iov, &iovlen, "from", devname, (size_t)-1) &&
             build_iovec(&iov, &iovlen, "fspath", mount_point, (size_t)-1) &&
             build_iovec(&iov, &iovlen, "fstype", "pfs", (size_t)-1) &&
             build_iovec(&iov, &iovlen, "sigverify", sigverify, (size_t)-1) &&
             build_iovec(&iov, &iovlen, "mkeymode", pfs_profile->mkeymode,
                         (size_t)-1) &&
             build_iovec(&iov, &iovlen, "budgetid", pfs_profile->budget_id,
                         (size_t)-1) &&
             build_iovec(&iov, &iovlen, "playgo", playgo, (size_t)-1) &&
             build_iovec(&iov, &iovlen, "disc", disc, (size_t)-1) &&
             build_iovec(&iov, &iovlen, "errmsg", mount_errmsg,
                         sizeof(mount_errmsg));
  }
  if (!iov_ok) {
    log_debug("  [IMG][%s] build_iovec failed for fstype=%s",
              backend_name(attach_backend), image_fs_name(fs_type));
    free_iovec(iov, iovlen);
    detach_attached_unit(attach_backend, unit_id);
    return false;
  }

  unsigned int first_mount_flags = MNT_RDONLY;
  unsigned int second_mount_flags = 0;
  const char *first_mode = "rdonly";
  const char *second_mode = "rw";
  bool allow_mount_mode_fallback = false;

  if (fs_type == IMAGE_FS_UFS) {
    // DownloadData-style flags:
    //   0x10000000 - ((opt == 0) - 1) => 0x10000000 (RO) or 0x10000001 (RW)
    unsigned int ufs_opt = mount_read_only ? 0u : 1u;
    first_mount_flags = UFS_NMOUNT_FLAG_BASE - ((ufs_opt == 0u) - 1u);
    second_mount_flags = UFS_NMOUNT_FLAG_BASE - (((ufs_opt ^ 1u) == 0u) - 1u);
    first_mode = mount_read_only ? "dd_ro" : "dd_rw";
    second_mode = mount_read_only ? "dd_rw" : "dd_ro";
  } else if (!mount_read_only) {
    first_mount_flags = 0;
    second_mount_flags = MNT_RDONLY;
    first_mode = "rw";
    second_mode = "rdonly";
    allow_mount_mode_fallback = true;
  }

  ret = nmount(iov, iovlen, first_mount_flags);
  if (ret != 0) {
    int mount_errno = errno;
    if (mount_errmsg[0] != '\0') {
      log_debug("  [IMG][%s] nmount %s errmsg: %s",
                backend_name(attach_backend), first_mode, mount_errmsg);
    }
    if (allow_mount_mode_fallback) {
      log_debug("  [IMG][%s] nmount %s failed: %s, trying %s...",
                backend_name(attach_backend), first_mode, strerror(mount_errno),
                second_mode);
      memset(mount_errmsg, 0, sizeof(mount_errmsg));
      ret = nmount(iov, iovlen, second_mount_flags);
      if (ret != 0) {
        mount_errno = errno;
        if (mount_errmsg[0] != '\0') {
          log_debug("  [IMG][%s] nmount %s errmsg: %s",
                    backend_name(attach_backend), second_mode, mount_errmsg);
        }
      }
    }
    if (ret != 0) {
      log_debug("  [IMG][%s] nmount failed: %s", backend_name(attach_backend),
                strerror(mount_errno));
      free_iovec(iov, iovlen);
      detach_attached_unit(attach_backend, unit_id);
      return false;
    }
  }
  free_iovec(iov, iovlen);

  log_debug("  [IMG][%s] Mounted (%s) %s -> %s", backend_name(attach_backend),
            image_fs_name(fs_type), devname, mount_point);

  // Cache it
  cache_ufs_mount(file_path, unit_id, attach_backend);

  return true;
}

static void scan_ufs_images() {
  if (should_stop_requested())
    return;

  // UFS cache handles mount/unmount lifecycle only.
  for (int k = 0; k < MAX_UFS_MOUNTS; k++) {
    if (should_stop_requested())
      return;
    if (ufs_cache[k].valid && access(ufs_cache[k].path, F_OK) != 0) {
      log_debug("  [IMG][%s] Source removed, unmounting: %s",
                backend_name(ufs_cache[k].backend), ufs_cache[k].path);
      unmount_ufs_image(ufs_cache[k].path, ufs_cache[k].unit_id,
                        ufs_cache[k].backend);
      ufs_cache[k].valid = false;
    }
  }

  for (int i = 0; SCAN_PATHS[i] != NULL; i++) {
    if (should_stop_requested())
      return;
    // Skip the UFS mount base itself to avoid recursion
    if (strcmp(SCAN_PATHS[i], UFS_MOUNT_BASE) == 0)
      continue;

    DIR *d = opendir(SCAN_PATHS[i]);
    if (!d)
      continue;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
      if (should_stop_requested()) {
        closedir(d);
        return;
      }
      if (entry->d_name[0] == '.')
        continue;
      image_fs_type_t fs_type = detect_image_fs_type(entry->d_name);
      if (fs_type == IMAGE_FS_UNKNOWN)
        continue;

      char full_path[MAX_PATH];
      snprintf(full_path, sizeof(full_path), "%s/%s", SCAN_PATHS[i],
               entry->d_name);

      // Verify it's a regular file
      struct stat st;
      if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode))
        continue;

      mount_ufs_image(full_path, fs_type);
    }
    closedir(d);
  }
}

static void shutdown_ufs_mounts(void) {
  for (int k = 0; k < MAX_UFS_MOUNTS; k++) {
    if (!ufs_cache[k].valid)
      continue;
    unmount_ufs_image(ufs_cache[k].path, ufs_cache[k].unit_id,
                      ufs_cache[k].backend);
    ufs_cache[k].valid = false;
  }
}

static void shutdown_nullfs_mounts(void) {
  for (int k = 0; k < MAX_NULLFS_MOUNTS; k++) {
    if (!nullfs_cache[k].valid)
      continue;
    unmount_nullfs_mount(nullfs_cache[k].mount_point);
    nullfs_cache[k].valid = false;
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
  if (!f)
    return false;

  size_t cap = 4096;
  size_t len = 0;
  char *buf = (char *)malloc(cap + 1);
  if (!buf) {
    fclose(f);
    return false;
  }

  bool read_ok = true;
  while (read_ok) {
    if (len == cap) {
      if (cap > (SIZE_MAX / 2)) {
        read_ok = false;
        break;
      }
      size_t new_cap = cap * 2;
      char *tmp = (char *)realloc(buf, new_cap + 1);
      if (!tmp) {
        read_ok = false;
        break;
      }
      buf = tmp;
      cap = new_cap;
    }

    size_t chunk = cap - len;
    size_t n = fread(buf + len, 1, chunk, f);
    len += n;
    if (n < chunk) {
      if (ferror(f))
        read_ok = false;
      break;
    }
  }
  fclose(f);

  if (!read_ok || len == 0) {
    free(buf);
    return false;
  }

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
    return true;
  }
  free(buf);
  return false;
}

// --- COUNTING ---
int count_new_candidates() {
  int count = 0;
  size_t ufs_prefix_len = strlen(UFS_MOUNT_BASE);
  for (int i = 0; SCAN_PATHS[i] != NULL; i++) {
    if (should_stop_requested())
      break;
    DIR *d = opendir(SCAN_PATHS[i]);
    if (!d)
      continue;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
      if (should_stop_requested()) {
        closedir(d);
        return count;
      }
      if (entry->d_name[0] == '.')
        continue;
      char full_path[MAX_PATH];
      snprintf(full_path, sizeof(full_path), "%s/%s", SCAN_PATHS[i],
               entry->d_name);
      if (strncmp(full_path, UFS_MOUNT_BASE, ufs_prefix_len) == 0 &&
          (full_path[ufs_prefix_len] == '/' || full_path[ufs_prefix_len] == '\0') &&
          !is_active_image_mount_point(full_path)) {
        continue;
      }

      char title_id[MAX_TITLE_ID];
      char title_name[MAX_TITLE_NAME];
      if (!get_game_info(full_path, title_id, title_name)) {
        size_t ufs_prefix_len = strlen(UFS_MOUNT_BASE);
        if (strncmp(full_path, UFS_MOUNT_BASE, ufs_prefix_len) == 0 &&
            (full_path[ufs_prefix_len] == '/' || full_path[ufs_prefix_len] == '\0')) {
          log_debug("  [SCAN] missing/invalid param.json: %s", full_path);
        }
        continue;
      }
      bool installed = is_installed(title_id);
      bool mounted = is_data_mounted(title_id);
      if (installed && mounted) {
        char tracked_path[MAX_PATH];
        if (read_mount_link(title_id, tracked_path, sizeof(tracked_path)) &&
            strcmp(tracked_path, full_path) == 0) {
          continue;
        }
      }

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
  bool nullfs_mounted = false;

  // MOUNT
  snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);
  mkdir(system_ex_app, 0777);
  if (remount_system_ex() != 0) {
    log_debug("  [MOUNT] remount_system_ex failed: %s", strerror(errno));
    return false;
  }
  if (unmount(system_ex_app, 0) != 0 && errno != ENOENT && errno != EINVAL) {
    log_debug("  [MOUNT] pre-unmount failed for %s: %s, trying force...",
              system_ex_app, strerror(errno));
    if (unmount(system_ex_app, MNT_FORCE) != 0 && errno != ENOENT &&
        errno != EINVAL) {
      log_debug("  [MOUNT] pre-force-unmount failed for %s: %s", system_ex_app,
                strerror(errno));
      return false;
    }
  }
  if (mount_nullfs(src_path, system_ex_app) < 0) {
    log_debug("  [MOUNT] FAIL: %s", strerror(errno));
    return false;
  }
  nullfs_mounted = true;
  cache_nullfs_mount(system_ex_app);

  // COPY FILES
  if (!is_remount) {
    snprintf(user_app_dir, sizeof(user_app_dir), "/user/app/%s", title_id);
    snprintf(user_sce_sys, sizeof(user_sce_sys), "%s/sce_sys", user_app_dir);
    mkdir(user_app_dir, 0777);
    mkdir(user_sce_sys, 0777);

    snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", src_path);
    if (copy_dir(src_sce_sys, user_sce_sys) != 0) {
      log_debug("  [COPY] Failed to copy sce_sys: %s -> %s", src_sce_sys,
                user_sce_sys);
      if (nullfs_mounted)
        unmount_nullfs_mount(system_ex_app);
      return false;
    }

    char icon_src[MAX_PATH], icon_dst[MAX_PATH];
    snprintf(icon_src, sizeof(icon_src), "%s/sce_sys/icon0.png", src_path);
    snprintf(icon_dst, sizeof(icon_dst), "/user/app/%s/icon0.png", title_id);
    if (copy_file(icon_src, icon_dst) != 0) {
      log_debug("  [COPY] Failed to copy icon: %s -> %s", icon_src, icon_dst);
      if (nullfs_mounted)
        unmount_nullfs_mount(system_ex_app);
      return false;
    }
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
    if (nullfs_mounted)
      unmount_nullfs_mount(system_ex_app);
    return false;
  }
  return true;
}

void scan_all_paths() {
  if (should_stop_requested())
    return;
  size_t ufs_prefix_len = strlen(UFS_MOUNT_BASE);

  // Cache Cleaner
  for (int k = 0; k < MAX_PENDING; k++) {
    if (should_stop_requested())
      return;
    if (cache[k].valid) {
      if (access(cache[k].path, F_OK) != 0) {
        cache[k].valid = false;
      }
    }
  }

  for (int i = 0; SCAN_PATHS[i] != NULL; i++) {
    if (should_stop_requested())
      return;
    DIR *d = opendir(SCAN_PATHS[i]);
    if (!d)
      continue;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
      if (should_stop_requested()) {
        closedir(d);
        return;
      }

      if (entry->d_name[0] == '.')
        continue;
      char full_path[MAX_PATH];
      snprintf(full_path, sizeof(full_path), "%s/%s", SCAN_PATHS[i],
               entry->d_name);
      if (strncmp(full_path, UFS_MOUNT_BASE, ufs_prefix_len) == 0 &&
          (full_path[ufs_prefix_len] == '/' || full_path[ufs_prefix_len] == '\0') &&
          !is_active_image_mount_point(full_path)) {
        continue;
      }

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
        size_t ufs_prefix_len = strlen(UFS_MOUNT_BASE);
        if (strncmp(full_path, UFS_MOUNT_BASE, ufs_prefix_len) == 0 &&
            (full_path[ufs_prefix_len] == '/' || full_path[ufs_prefix_len] == '\0')) {
          log_debug("  [SCAN] missing/invalid param.json: %s", full_path);
        }
        continue;
      }

      // 1. Skip if perfect
      bool installed = is_installed(title_id);
      bool mounted = is_data_mounted(title_id);
      if (installed && mounted) {
        char tracked_path[MAX_PATH];
        if (read_mount_link(title_id, tracked_path, sizeof(tracked_path)) &&
            strcmp(tracked_path, full_path) == 0) {
          continue;
        }
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
  int lock = -1;

  // Initialize services
  sceUserServiceInitialize(0);
  sceAppInstUtilInitialize();
  kernel_set_ucred_authid(-1, 0x4801000000000013L);
  install_signal_handlers();

  mkdir(LOG_DIR, 0777);
  lock = open(LOCK_FILE, O_CREAT | O_RDWR, 0666);
  if (lock < 0) {
    printf("[LOCK] Failed to create %s: %s\n", LOCK_FILE, strerror(errno));
    sceUserServiceTerminate();
    return 1;
  }
  if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      close(lock);
      sceUserServiceTerminate();
      return 0;
    }
    printf("[LOCK] Failed to lock %s: %s\n", LOCK_FILE, strerror(errno));
    close(lock);
    sceUserServiceTerminate();
    return 1;
  }

  remove(LOG_FILE);

  log_debug("SHADOWMOUNT+ v1.4 START exFAT/UFS/LVD/MD");

  // --- MOUNT UFS IMAGES ---
  scan_ufs_images();

  // --- STARTUP LOGIC ---
  int new_games = count_new_candidates();

  if (new_games == 0) {
    // SCENARIO A: Nothing to do.
    notify_system("ShadowMount+ v1.4 exFAT/UFS: Library Ready.\n- VoidWhisper/Gezine/earthonion/Drakmor");
  } else {
    // SCENARIO B: Work needed.
    notify_system("ShadowMount+ v1.4 exFAT/UFS: Found %d Games. Executing...", new_games);

    // Run the scan immediately to process them
    scan_all_paths();

    // Completion Message
    notify_system("Library Synchronized.");
  }

  // --- DAEMON LOOP ---
  while (true) {
    if (should_stop_requested()) {
      log_debug("[SHUTDOWN] stop requested");
      break;
    }

    // Sleep FIRST since we either just finished scan above, or library was
    // ready.
    if (sleep_with_stop_check(SCAN_INTERVAL_US)) {
      log_debug("[SHUTDOWN] stop requested during sleep");
      break;
    }

    scan_ufs_images();
    scan_all_paths();
  }

  shutdown_nullfs_mounts();
  shutdown_ufs_mounts();
  if (lock >= 0) {
    close(lock);
  }
  sceUserServiceTerminate();
  return 0;
}
