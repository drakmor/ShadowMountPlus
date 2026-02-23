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
#include <sqlite3.h>

// --- Configuration ---
#define DEFAULT_SCAN_INTERVAL_US 10000000u
#define DEFAULT_STABILITY_WAIT_SECONDS 10u
#define MAX_PENDING 512
#define MAX_IMAGE_MOUNTS 64
#define MAX_IMAGE_MODE_RULES 128
#define PATH_STATE_CAPACITY MAX_PENDING
#define TITLE_STATE_CAPACITY MAX_PENDING
#define STATE_HASH_SIZE 1024u
#define MAX_SCAN_PATHS 128
#define MAX_FAILED_MOUNT_ATTEMPTS 1
#define MAX_MISSING_PARAM_SCAN_ATTEMPTS 3
#define MAX_IMAGE_MOUNT_ATTEMPTS 3
#define MAX_LAYERED_UNMOUNT_ATTEMPTS 4
#define IMAGE_MOUNT_READ_ONLY 1
#define MIN_SCAN_INTERVAL_SECONDS 1u
#define MAX_SCAN_INTERVAL_SECONDS 3600u
#define MAX_STABILITY_WAIT_SECONDS 3600u
#define APP_DB_QUERY_BUSY_RETRIES 3
#define APP_DB_UPDATE_BUSY_RETRIES 25
#define APP_DB_PREPARE_BUSY_RETRIES 25
#define APP_DB_BUSY_RETRY_SLEEP_US 200000u
#define APP_DB_BUSY_TIMEOUT_MS 5000
#define MAX_PATH 1024
#define MAX_TITLE_ID 32
#define MAX_TITLE_NAME 256
#define SHADOWMOUNT_VERSION "1.6test1"
#define PAYLOAD_NAME "shadowmountplus.elf"
#define IMAGE_MOUNT_BASE "/data/imgmnt"
#define IMAGE_MOUNT_SUBDIR_UFS "ufsmnt"
#define IMAGE_MOUNT_SUBDIR_EXFAT "exfatmnt"
#define IMAGE_MOUNT_SUBDIR_PFS "pfsmnt"
#define DEFAULT_BACKPORTS_PATH "/data/backports"
#define LOG_DIR "/data/shadowmount"
#define LOG_FILE "/data/shadowmount/debug.log"
#define LOG_FILE_PREV "/data/shadowmount/debug.log.1"
#define CONFIG_FILE "/data/shadowmount/config.ini"
#define LOCK_FILE "/data/shadowmount/daemon.lock"
#define KILL_FILE "/data/shadowmount/STOP"
#define TOAST_FILE "/data/shadowmount/notify.txt"
#define APP_DB_PATH "/system_data/priv/mms/app.db"
#define IOVEC_ENTRY(x)                                                        \
  {(void *)(x),                                                               \
   ((const char *)(x) == NULL ? 0u : (size_t)(strlen((const char *)(x)) + 1u))}
#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))
// 1 = use legacy /dev/mdctl backend for .exfat images, 0 = use LVD for all.
#define EXFAT_ATTACH_USE_MDCTL 0
// 1 = allow mounting .ffpkg images via /dev/mdctl, 0 = keep UFS on LVD.
#define UFS_ATTACH_USE_MDCTL 0

// --- LVD Definitions ---
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
#define LVD_ATTACH_OPTION_FLAGS_DEFAULT 0x9
#define LVD_ATTACH_OPTION_FLAGS_RW 0x8
#define LVD_ATTACH_OPTION_NORM_DD_RO 0x1E
#define LVD_ATTACH_OPTION_NORM_DD_RW 0x16
#define LVD_SECTOR_SIZE_EXFAT 512u
#define LVD_SECTOR_SIZE_UFS 4096u
#define LVD_SECTOR_SIZE_PFS 32768u
#define MD_SECTOR_SIZE_EXFAT 512u
#define MD_SECTOR_SIZE_UFS 512u

// Raw option bits are normalized by sceFsLvdAttachCommon before validation:
// raw:0x1->norm:0x08, raw:0x2->norm:0x80, raw:0x4->norm:0x02, raw:0x8->norm:0x10.
// The normalized masks are then checked against validator constraints (0x82/0x92).
#define LVD_ATTACH_IMAGE_TYPE 0
#define LVD_ATTACH_IMAGE_TYPE_UFS_DOWNLOAD_DATA 7
#define LVD_ATTACH_IMAGE_TYPE_PFS_SAVE_DATA 0 // also works 5
#define LVD_ATTACH_LAYER_COUNT 1
#define LVD_ATTACH_LAYER_ARRAY_SIZE 3
#define LVD_ENTRY_TYPE_FILE 1
#define LVD_ENTRY_FLAG_NO_BITMAP 0x1
#define LVD_NODE_WAIT_US 100000
#define LVD_NODE_WAIT_RETRIES 100
#define UFS_NMOUNT_FLAG_RW 0x10000000u
#define UFS_NMOUNT_FLAG_RO 0x10000001u


// --- devpfs/pfs option defaults ---
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
#define DEVPFS_MKEYMODE_GD "GD"
#define DEVPFS_MKEYMODE_AC "AC"
#define PFS_MOUNT_BUDGET_ID DEVPFS_BUDGET_GAME
#define PFS_MOUNT_MKEYMODE DEVPFS_MKEYMODE_SD
#define PFS_MOUNT_SIGVERIFY 0
#define PFS_MOUNT_PLAYGO 0
#define PFS_MOUNT_DISC 0

// 4x64-bit PFS key encoded as 64 hex chars.
#define PFS_ZERO_EKPFS_KEY_HEX \
  "0000000000000000000000000000000000000000000000000000000000000000"



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



// --- SDK Imports ---
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallTitleDir(const char *title_id,
                                     const char *install_path, void *reserved);
int sceKernelUsleep(unsigned int microseconds);
int sceUserServiceInitialize(void *);
void sceUserServiceTerminate(void);

// --- Forward Declarations ---
bool get_game_info(const char *base_path, const struct stat *param_st,
                   char *out_id, char *out_name);
bool is_installed(const char *title_id);
bool is_data_mounted(const char *title_id);
void notify_system(const char *fmt, ...);
void log_debug(const char *fmt, ...);
static void close_app_db(void);
static void shutdown_image_mounts(void);
static void ensure_runtime_config_ready(void);
static void cleanup_stale_image_mounts(void);
static bool directory_has_param_json(const char *dir_path,
                                     struct stat *param_st_out);

// Standard Notification
typedef struct notify_request {
  char unused[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

// --- Runtime Scan Sources and State Caches ---
// Scan Paths
static const char *DEFAULT_SCAN_PATHS[] = {
    // Internal
    "/data/homebrew", "/data/etaHEN/games",

    // Extended Storage
    "/mnt/ext0/homebrew",  "/mnt/ext0/etaHEN/games",

    // M.2 Drive
    "/mnt/ext1/homebrew",  "/mnt/ext1/etaHEN/games",

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

    NULL};
static const char *SCAN_PATHS[MAX_SCAN_PATHS + 1];
static char g_scan_path_storage[MAX_SCAN_PATHS][MAX_PATH];
static int g_scan_path_count = 0;

struct GameCache {
  char path[MAX_PATH];
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  bool valid;
};
struct GameCache cache[MAX_PENDING];

struct PathStateEntry {
  char path[MAX_PATH];
  uint8_t missing_param_attempts;
  uint8_t image_mount_attempts;
  bool missing_param_limit_logged;
  bool image_mount_limit_logged;
  bool game_info_cached;
  bool game_info_valid;
  time_t game_info_mtime;
  off_t game_info_size;
  ino_t game_info_ino;
  char game_title_id[MAX_TITLE_ID];
  char game_title_name[MAX_TITLE_NAME];
  bool valid;
};
struct PathStateEntry g_path_state[PATH_STATE_CAPACITY];
static uint16_t g_path_state_hash[STATE_HASH_SIZE];

struct TitleStateEntry {
  char title_id[MAX_TITLE_ID];
  uint8_t mount_reg_attempts;
  bool register_attempted_once;
  bool duplicate_notified_once;
  bool valid;
};
struct TitleStateEntry g_title_state[TITLE_STATE_CAPACITY];
static uint16_t g_title_state_hash[STATE_HASH_SIZE];

struct AppDbTitleList {
  char(*ids)[MAX_TITLE_ID];
  int count;
  int capacity;
};

typedef struct {
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

#if EXFAT_ATTACH_USE_MDCTL
#define DEFAULT_EXFAT_BACKEND ATTACH_BACKEND_MD
#else
#define DEFAULT_EXFAT_BACKEND ATTACH_BACKEND_LVD
#endif

#if UFS_ATTACH_USE_MDCTL
#define DEFAULT_UFS_BACKEND ATTACH_BACKEND_MD
#else
#define DEFAULT_UFS_BACKEND ATTACH_BACKEND_LVD
#endif

typedef struct {
  bool debug_enabled;
  bool mount_read_only;
  bool force_mount;
  bool recursive_scan;
  char backports_path[MAX_PATH];
  uint32_t scan_interval_us;
  uint32_t stability_wait_seconds;
  attach_backend_t exfat_backend;
  attach_backend_t ufs_backend;
  uint32_t lvd_sector_exfat;
  uint32_t lvd_sector_ufs;
  uint32_t lvd_sector_pfs;
  uint32_t md_sector_exfat;
  uint32_t md_sector_ufs;
} runtime_config_t;

static runtime_config_t g_runtime_cfg;
static bool g_runtime_cfg_ready = false;
static sqlite3 *g_app_db = NULL;
static sqlite3_stmt *g_app_db_stmt_update_snd0 = NULL;
static char g_last_image_mount_errmsg[256];
static struct AppDbTitleList g_app_db_title_cache = {0};
static bool g_app_db_title_cache_ready = false;
static time_t g_app_db_title_cache_mtime = 0;
static scan_candidate_t g_scan_candidates[MAX_PENDING];
static char g_scan_discovered_param_roots[MAX_PENDING][MAX_PATH];

typedef struct {
  char filename[MAX_PATH];
  bool mount_read_only;
  bool valid;
} image_mode_rule_t;
static image_mode_rule_t g_image_mode_rules[MAX_IMAGE_MODE_RULES];

struct ImageCache {
  // Absolute source image path.
  char path[MAX_PATH];
  // Mountpoint path for this image.
  char mount_point[MAX_PATH];
  // Attached unit id (lvdN/mdN), -1 when unknown.
  int unit_id;
  // Backend used for this entry.
  attach_backend_t backend;
  // Slot occupancy flag.
  bool valid;
};
struct ImageCache image_cache[MAX_IMAGE_MOUNTS];

static volatile sig_atomic_t g_stop_requested = 0;

typedef enum {
  IMAGE_FS_UNKNOWN = 0,
  IMAGE_FS_UFS,
  IMAGE_FS_EXFAT,
  IMAGE_FS_PFS,
} image_fs_type_t;

static const char *backend_name(attach_backend_t backend);

// --- Core Utilities ---
static void clear_runtime_scan_paths(void) {
  g_scan_path_count = 0;
  memset(SCAN_PATHS, 0, sizeof(SCAN_PATHS));
  memset(g_scan_path_storage, 0, sizeof(g_scan_path_storage));
}

static bool add_runtime_scan_path(const char *path) {
  if (!path)
    return false;

  while (*path && isspace((unsigned char)*path))
    path++;

  size_t len = strlen(path);
  while (len > 0 && isspace((unsigned char)path[len - 1]))
    len--;
  if (len == 0 || len >= MAX_PATH)
    return false;

  char normalized[MAX_PATH];
  memcpy(normalized, path, len);
  normalized[len] = '\0';
  while (len > 1 && normalized[len - 1] == '/') {
    normalized[len - 1] = '\0';
    len--;
  }

  for (int i = 0; i < g_scan_path_count; i++) {
    if (SCAN_PATHS[i] && strcmp(SCAN_PATHS[i], normalized) == 0)
      return true;
  }

  if (g_scan_path_count >= MAX_SCAN_PATHS)
    return false;

  (void)strlcpy(g_scan_path_storage[g_scan_path_count], normalized,
                sizeof(g_scan_path_storage[g_scan_path_count]));
  SCAN_PATHS[g_scan_path_count] = g_scan_path_storage[g_scan_path_count];
  g_scan_path_count++;
  SCAN_PATHS[g_scan_path_count] = NULL;
  return true;
}

static void init_runtime_scan_paths_defaults(void) {
  clear_runtime_scan_paths();
  for (int i = 0; DEFAULT_SCAN_PATHS[i] != NULL; i++)
    (void)add_runtime_scan_path(DEFAULT_SCAN_PATHS[i]);
  // Image mount roots must always be present.
  (void)add_runtime_scan_path(IMAGE_MOUNT_BASE "/" IMAGE_MOUNT_SUBDIR_UFS);
  (void)add_runtime_scan_path(IMAGE_MOUNT_BASE "/" IMAGE_MOUNT_SUBDIR_EXFAT);
  (void)add_runtime_scan_path(IMAGE_MOUNT_BASE "/" IMAGE_MOUNT_SUBDIR_PFS);
}

static const char *get_filename_component(const char *path) {
  if (!path)
    return "";
  const char *base = strrchr(path, '/');
  if (!base)
    base = strrchr(path, '\\');
  return base ? base + 1 : path;
}

static void cache_game_entry(const char *path, const char *title_id,
                             const char *title_name) {
  for (int k = 0; k < MAX_PENDING; k++) {
    if (!cache[k].valid) {
      (void)strlcpy(cache[k].path, path, sizeof(cache[k].path));
      (void)strlcpy(cache[k].title_id, title_id, sizeof(cache[k].title_id));
      (void)strlcpy(cache[k].title_name, title_name, sizeof(cache[k].title_name));
      cache[k].valid = true;
      return;
    }
  }
}

static bool is_under_image_mount_base(const char *path) {
  if (!path)
    return false;
  size_t image_prefix_len = strlen(IMAGE_MOUNT_BASE);
  return (strncmp(path, IMAGE_MOUNT_BASE, image_prefix_len) == 0 &&
          path[image_prefix_len] == '/');
}

// --- Fast Path/Title State Cache (hash indexed) ---
static uint32_t hash_string(const char *s) {
  uint32_t h = 2166136261u;
  while (s && *s) {
    h ^= (uint8_t)(*s++);
    h *= 16777619u;
  }
  return h;
}

static void rebuild_path_state_hash(void) {
  memset(g_path_state_hash, 0, sizeof(g_path_state_hash));
  for (int k = 0; k < PATH_STATE_CAPACITY; k++) {
    if (!g_path_state[k].valid || g_path_state[k].path[0] == '\0')
      continue;
    uint32_t slot = hash_string(g_path_state[k].path) & (STATE_HASH_SIZE - 1u);
    for (uint32_t i = 0; i < STATE_HASH_SIZE; i++) {
      if (g_path_state_hash[slot] == 0) {
        g_path_state_hash[slot] = (uint16_t)(k + 1);
        break;
      }
      slot = (slot + 1u) & (STATE_HASH_SIZE - 1u);
    }
  }
}

static void rebuild_title_state_hash(void) {
  memset(g_title_state_hash, 0, sizeof(g_title_state_hash));
  for (int k = 0; k < TITLE_STATE_CAPACITY; k++) {
    if (!g_title_state[k].valid || g_title_state[k].title_id[0] == '\0')
      continue;
    uint32_t slot = hash_string(g_title_state[k].title_id) & (STATE_HASH_SIZE - 1u);
    for (uint32_t i = 0; i < STATE_HASH_SIZE; i++) {
      if (g_title_state_hash[slot] == 0) {
        g_title_state_hash[slot] = (uint16_t)(k + 1);
        break;
      }
      slot = (slot + 1u) & (STATE_HASH_SIZE - 1u);
    }
  }
}

static struct PathStateEntry *find_path_state(const char *path) {
  if (!path || path[0] == '\0')
    return NULL;
  uint32_t slot = hash_string(path) & (STATE_HASH_SIZE - 1u);
  for (uint32_t i = 0; i < STATE_HASH_SIZE; i++) {
    uint16_t idx = g_path_state_hash[slot];
    if (idx == 0)
      return NULL;
    struct PathStateEntry *entry = &g_path_state[idx - 1u];
    if (entry->valid && strcmp(entry->path, path) == 0)
      return entry;
    slot = (slot + 1u) & (STATE_HASH_SIZE - 1u);
  }
  return NULL;
}

static struct TitleStateEntry *find_title_state(const char *title_id) {
  if (!title_id || title_id[0] == '\0')
    return NULL;
  uint32_t slot = hash_string(title_id) & (STATE_HASH_SIZE - 1u);
  for (uint32_t i = 0; i < STATE_HASH_SIZE; i++) {
    uint16_t idx = g_title_state_hash[slot];
    if (idx == 0)
      return NULL;
    struct TitleStateEntry *entry = &g_title_state[idx - 1u];
    if (entry->valid && strcmp(entry->title_id, title_id) == 0)
      return entry;
    slot = (slot + 1u) & (STATE_HASH_SIZE - 1u);
  }
  return NULL;
}

static struct PathStateEntry *create_path_state(const char *path) {
  if (!path || path[0] == '\0')
    return NULL;

  int slot_k = -1;
  for (int k = 0; k < PATH_STATE_CAPACITY; k++) {
    if (!g_path_state[k].valid) {
      slot_k = k;
      break;
    }
  }
  if (slot_k < 0) {
    int evict_k = -1;
    for (int k = 0; k < PATH_STATE_CAPACITY; k++) {
      if (!g_path_state[k].valid)
        continue;
      if (access(g_path_state[k].path, F_OK) != 0) {
        evict_k = k;
        break;
      }
    }
    if (evict_k < 0) {
      for (int k = 0; k < PATH_STATE_CAPACITY; k++) {
        if (!g_path_state[k].valid)
          continue;
        if (g_path_state[k].missing_param_attempts == 0 &&
            g_path_state[k].image_mount_attempts == 0 &&
            !g_path_state[k].game_info_cached) {
          evict_k = k;
          break;
        }
      }
    }
    if (evict_k < 0)
      evict_k = 0;
    memset(&g_path_state[evict_k], 0, sizeof(g_path_state[evict_k]));
    rebuild_path_state_hash();
    slot_k = evict_k;
  }

  memset(&g_path_state[slot_k], 0, sizeof(g_path_state[slot_k]));
  g_path_state[slot_k].valid = true;
  (void)strlcpy(g_path_state[slot_k].path, path, sizeof(g_path_state[slot_k].path));

  uint32_t slot = hash_string(path) & (STATE_HASH_SIZE - 1u);
  for (uint32_t i = 0; i < STATE_HASH_SIZE; i++) {
    if (g_path_state_hash[slot] == 0) {
      g_path_state_hash[slot] = (uint16_t)(slot_k + 1);
      return &g_path_state[slot_k];
    }
    slot = (slot + 1u) & (STATE_HASH_SIZE - 1u);
  }

  g_path_state[slot_k].valid = false;
  g_path_state[slot_k].path[0] = '\0';
  rebuild_path_state_hash();
  return NULL;
}

static struct TitleStateEntry *create_title_state(const char *title_id) {
  if (!title_id || title_id[0] == '\0')
    return NULL;

  int slot_k = -1;
  for (int k = 0; k < TITLE_STATE_CAPACITY; k++) {
    if (!g_title_state[k].valid) {
      slot_k = k;
      break;
    }
  }
  if (slot_k < 0) {
    int evict_k = -1;
    for (int k = 0; k < TITLE_STATE_CAPACITY; k++) {
      if (!g_title_state[k].valid)
        continue;
      if (g_title_state[k].mount_reg_attempts == 0 &&
          !g_title_state[k].register_attempted_once) {
        evict_k = k;
        break;
      }
    }
    if (evict_k < 0) {
      for (int k = 0; k < TITLE_STATE_CAPACITY; k++) {
        if (!g_title_state[k].valid)
          continue;
        if (g_title_state[k].mount_reg_attempts == 0) {
          evict_k = k;
          break;
        }
      }
    }
    if (evict_k < 0)
      evict_k = 0;
    memset(&g_title_state[evict_k], 0, sizeof(g_title_state[evict_k]));
    rebuild_title_state_hash();
    slot_k = evict_k;
  }

  memset(&g_title_state[slot_k], 0, sizeof(g_title_state[slot_k]));
  g_title_state[slot_k].valid = true;
  (void)strlcpy(g_title_state[slot_k].title_id, title_id,
                sizeof(g_title_state[slot_k].title_id));

  uint32_t slot = hash_string(title_id) & (STATE_HASH_SIZE - 1u);
  for (uint32_t i = 0; i < STATE_HASH_SIZE; i++) {
    if (g_title_state_hash[slot] == 0) {
      g_title_state_hash[slot] = (uint16_t)(slot_k + 1);
      return &g_title_state[slot_k];
    }
    slot = (slot + 1u) & (STATE_HASH_SIZE - 1u);
  }

  g_title_state[slot_k].valid = false;
  g_title_state[slot_k].title_id[0] = '\0';
  rebuild_title_state_hash();
  return NULL;
}

static struct PathStateEntry *get_or_create_path_state(const char *path) {
  struct PathStateEntry *entry = find_path_state(path);
  return entry ? entry : create_path_state(path);
}

static struct TitleStateEntry *get_or_create_title_state(const char *title_id) {
  struct TitleStateEntry *entry = find_title_state(title_id);
  return entry ? entry : create_title_state(title_id);
}

static void prune_path_state(void) {
  bool changed = false;
  for (int k = 0; k < PATH_STATE_CAPACITY; k++) {
    if (!g_path_state[k].valid || g_path_state[k].path[0] == '\0')
      continue;
    if (access(g_path_state[k].path, F_OK) == 0)
      continue;
    memset(&g_path_state[k], 0, sizeof(g_path_state[k]));
    changed = true;
  }
  if (changed)
    rebuild_path_state_hash();
}

static bool was_register_attempted(const char *title_id) {
  struct TitleStateEntry *entry = find_title_state(title_id);
  return entry ? entry->register_attempted_once : false;
}

static void mark_register_attempted(const char *title_id) {
  struct TitleStateEntry *entry = get_or_create_title_state(title_id);
  if (entry)
    entry->register_attempted_once = true;
}

static void notify_duplicate_title_once(const char *title_id, const char *path_a,
                                        const char *path_b) {
  struct TitleStateEntry *entry = get_or_create_title_state(title_id);
  if (!entry)
    return;
  if (entry->duplicate_notified_once)
    return;
  entry->duplicate_notified_once = true;
  notify_system("Duplicate %s detected:\n%s\n%s", title_id, path_a,
                path_b);
}

static bool is_missing_param_scan_limited(const char *path) {
  if (!is_under_image_mount_base(path))
    return false;
  struct PathStateEntry *entry = find_path_state(path);
  if (!entry)
    return false;
  return entry->missing_param_attempts >= MAX_MISSING_PARAM_SCAN_ATTEMPTS;
}

static void record_missing_param_failure(const char *path) {
  if (!is_under_image_mount_base(path))
    return;

  struct PathStateEntry *entry = get_or_create_path_state(path);
  if (!entry) {
    log_debug("  [SCAN] missing/invalid param.json: %s", path);
    notify_system("Missing/invalid param.json:\n%s", path);
    return;
  }
  if (entry->missing_param_attempts < UINT8_MAX)
    entry->missing_param_attempts++;

  log_debug("  [SCAN] missing/invalid param.json: %s", path);
  if (entry->missing_param_attempts == 1) {
    notify_system("Missing/invalid param.json:\n%s", path);
  }
  if (entry->missing_param_attempts >= MAX_MISSING_PARAM_SCAN_ATTEMPTS &&
      !entry->missing_param_limit_logged) {
    log_debug("  [SCAN] attempt limit reached (%u), skipping path: %s",
              (unsigned)MAX_MISSING_PARAM_SCAN_ATTEMPTS, path);
    entry->missing_param_limit_logged = true;
  }
}

static void clear_missing_param_entry(const char *path) {
  struct PathStateEntry *entry = find_path_state(path);
  if (!entry)
    return;
  entry->missing_param_attempts = 0;
  entry->missing_param_limit_logged = false;
}

static uint8_t get_failed_mount_attempts(const char *title_id) {
  struct TitleStateEntry *entry = find_title_state(title_id);
  return entry ? entry->mount_reg_attempts : 0;
}

static void clear_failed_mount_attempts(const char *title_id) {
  struct TitleStateEntry *entry = find_title_state(title_id);
  if (entry)
    entry->mount_reg_attempts = 0;
}

static uint8_t bump_failed_mount_attempts(const char *title_id) {
  struct TitleStateEntry *entry = get_or_create_title_state(title_id);
  if (!entry)
    return MAX_FAILED_MOUNT_ATTEMPTS;
  if (entry->mount_reg_attempts < UINT8_MAX)
    entry->mount_reg_attempts++;
  return entry->mount_reg_attempts;
}

static bool is_image_mount_limited(const char *path) {
  struct PathStateEntry *entry = find_path_state(path);
  return entry ? entry->image_mount_attempts >= MAX_IMAGE_MOUNT_ATTEMPTS : false;
}

static uint8_t bump_image_mount_attempts(const char *path) {
  struct PathStateEntry *entry = get_or_create_path_state(path);
  if (!entry)
    return MAX_IMAGE_MOUNT_ATTEMPTS;
  if (entry->image_mount_attempts < UINT8_MAX)
    entry->image_mount_attempts++;
  if (entry->image_mount_attempts >= MAX_IMAGE_MOUNT_ATTEMPTS &&
      !entry->image_mount_limit_logged) {
    log_debug("  [IMG] retry limit reached (%u/%u), skipping image: %s",
              (unsigned)entry->image_mount_attempts,
              (unsigned)MAX_IMAGE_MOUNT_ATTEMPTS, path);
    entry->image_mount_limit_logged = true;
  }
  return entry->image_mount_attempts;
}

static void clear_image_mount_attempts(const char *path) {
  struct PathStateEntry *entry = find_path_state(path);
  if (!entry)
    return;
  entry->image_mount_attempts = 0;
  entry->image_mount_limit_logged = false;
}

// --- Active Mount Session Caches ---
static void cache_image_mount(const char *path, const char *mount_point,
                              int unit_id, attach_backend_t backend) {
  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    if (image_cache[k].valid && strcmp(image_cache[k].path, path) == 0) {
      (void)strlcpy(image_cache[k].mount_point, mount_point,
                    sizeof(image_cache[k].mount_point));
      image_cache[k].unit_id = unit_id;
      image_cache[k].backend = backend;
      return;
    }
  }

  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    if (!image_cache[k].valid) {
      (void)strlcpy(image_cache[k].path, path, sizeof(image_cache[k].path));
      (void)strlcpy(image_cache[k].mount_point, mount_point,
                    sizeof(image_cache[k].mount_point));
      image_cache[k].unit_id = unit_id;
      image_cache[k].backend = backend;
      image_cache[k].valid = true;
      return;
    }
  }
}

static bool resolve_device_from_mount_cache(const char *mount_point,
                                            attach_backend_t *backend_out,
                                            int *unit_out) {
  if (!mount_point || mount_point[0] == '\0')
    return false;
  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    if (!image_cache[k].valid)
      continue;
    if (strcmp(image_cache[k].mount_point, mount_point) != 0)
      continue;
    if (image_cache[k].backend == ATTACH_BACKEND_NONE || image_cache[k].unit_id < 0)
      return false;
    *backend_out = image_cache[k].backend;
    *unit_out = image_cache[k].unit_id;
    return true;
  }
  return false;
}

// --- Shutdown and Stop Signal Handling ---
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
  sigaction(SIGABRT, &sa, NULL);
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
  if (g_runtime_cfg_ready && !g_runtime_cfg.debug_enabled)
    return;

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

static void notify_image_mount_failed(const char *path, int mount_err) {
  if (g_last_image_mount_errmsg[0] != '\0') {
    notify_system("Image mount failed: 0x%08X (%s)\n%s\n%s",
                  (uint32_t)mount_err, strerror(mount_err),
                  g_last_image_mount_errmsg, path);
    return;
  }
  notify_system("Image mount failed: 0x%08X (%s)\n%s", (uint32_t)mount_err,
                strerror(mount_err), path);
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

static bool read_mount_link_file(const char *lnk_path, char *out,
                                 size_t out_size) {
  if (!lnk_path || out_size == 0)
    return false;
  out[0] = '\0';

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

static bool read_mount_link(const char *title_id, char *out, size_t out_size) {
  if (out_size == 0)
    return false;
  out[0] = '\0';

  char lnk_path[MAX_PATH];
  snprintf(lnk_path, sizeof(lnk_path), "/user/app/%s/mount.lnk", title_id);
  return read_mount_link_file(lnk_path, out, out_size);
}

static bool path_matches_root_or_child(const char *path, const char *root) {
  if (!path || !root || root[0] == '\0')
    return false;
  size_t root_len = strlen(root);
  if (strncmp(path, root, root_len) != 0)
    return false;
  return (path[root_len] == '\0' || path[root_len] == '/');
}

static void cleanup_mount_links(const char *removed_source_root,
                                bool unmount_system_ex_bind) {
  DIR *d = opendir("/user/app");
  if (!d) {
    if (errno != ENOENT)
      log_debug("  [LINK] open /user/app failed: %s", strerror(errno));
    return;
  }

  bool tried_image_recovery = false;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (should_stop_requested())
      break;
    if (entry->d_name[0] == '.')
      continue;
    if (strlen(entry->d_name) != 9)
      continue;

    char app_dir[MAX_PATH];
    snprintf(app_dir, sizeof(app_dir), "/user/app/%s", entry->d_name);
    if (entry->d_type == DT_DIR) {
      // ok
    } else if (entry->d_type == DT_UNKNOWN) {
      struct stat st;
      if (stat(app_dir, &st) != 0 || !S_ISDIR(st.st_mode))
        continue;
    } else {
      continue;
    }

    char lnk_path[MAX_PATH];
    snprintf(lnk_path, sizeof(lnk_path), "%s/mount.lnk", app_dir);
    struct stat lst;
    if (stat(lnk_path, &lst) != 0 || !S_ISREG(lst.st_mode))
      continue;

    char source_path[MAX_PATH];
    bool should_remove = false;
    bool matches_removed_source = false;
    if (!read_mount_link_file(lnk_path, source_path, sizeof(source_path))) {
      should_remove = true;
    }

    if (!should_remove) {
      if (removed_source_root && removed_source_root[0] != '\0') {
        matches_removed_source =
            path_matches_root_or_child(source_path, removed_source_root);
        should_remove = matches_removed_source;
      } else {
        if (access(source_path, F_OK) != 0) {
          should_remove = true;
        } else if (path_matches_root_or_child(source_path, "/system_ex/app")) {
          should_remove = true;
        } else {
          char eboot_path[MAX_PATH];
          snprintf(eboot_path, sizeof(eboot_path), "%s/eboot.bin", source_path);
          if (access(eboot_path, F_OK) != 0) {
            if (!tried_image_recovery &&
                path_matches_root_or_child(source_path, IMAGE_MOUNT_BASE)) {
              cleanup_stale_image_mounts();
              tried_image_recovery = true;
            }
            if (access(eboot_path, F_OK) != 0)
              should_remove = true;
          }
        }
      }
    }

    if (!should_remove)
      continue;

    if (unlink(lnk_path) == 0 || errno == ENOENT) {
      log_debug("  [LINK] removed stale mount link: %s", lnk_path);
    } else {
      log_debug("  [LINK] remove failed for %s: %s", lnk_path, strerror(errno));
    }

    if (unmount_system_ex_bind && matches_removed_source) {
      char system_ex_path[MAX_PATH];
      snprintf(system_ex_path, sizeof(system_ex_path), "/system_ex/app/%s",
               entry->d_name);

      struct statfs mount_st;
      if (statfs(system_ex_path, &mount_st) == 0 &&
          strcmp(mount_st.f_fstypename, "nullfs") == 0 &&
          path_matches_root_or_child(mount_st.f_mntfromname, removed_source_root)) {
        if (unmount(system_ex_path, 0) != 0 && errno != ENOENT &&
            errno != EINVAL) {
          if (unmount(system_ex_path, MNT_FORCE) != 0 && errno != ENOENT &&
              errno != EINVAL) {
            log_debug("  [LINK] unmount failed for %s: %s", system_ex_path,
                      strerror(errno));
          }
        }
      }
    }
  }

  closedir(d);
}

// --- app.db Access Layer ---
static void close_app_db(void) {
  if (g_app_db_stmt_update_snd0) {
    sqlite3_finalize(g_app_db_stmt_update_snd0);
    g_app_db_stmt_update_snd0 = NULL;
  }
  if (g_app_db) {
    sqlite3_close(g_app_db);
    g_app_db = NULL;
  }
}

static bool ensure_app_db_open(void) {
  if (!g_app_db) {
    if (sqlite3_open(APP_DB_PATH, &g_app_db) != SQLITE_OK) {
      log_debug("  [DB] open failed: %s",
                (g_app_db ? sqlite3_errmsg(g_app_db) : APP_DB_PATH));
      close_app_db();
      return false;
    }
    (void)sqlite3_busy_timeout(g_app_db, APP_DB_BUSY_TIMEOUT_MS);
  }
  return true;
}

static bool app_db_wait_retry(int rc, int attempt, int max_attempts,
                              bool close_before_retry) {
  if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED)
    return false;
  if (attempt + 1 >= max_attempts || should_stop_requested())
    return false;
  if (close_before_retry)
    close_app_db();
  sceKernelUsleep(APP_DB_BUSY_RETRY_SLEEP_US);
  return true;
}

static int app_db_prepare_with_retry(const char *sql, sqlite3_stmt **stmt_out,
                                     int max_attempts, const char *label) {
  for (int attempt = 0; attempt < max_attempts; attempt++) {
    if (!ensure_app_db_open())
      return -1;

    int rc = sqlite3_prepare_v2(g_app_db, sql, -1, stmt_out, NULL);
    if (rc == SQLITE_OK)
      return SQLITE_OK;

    if (app_db_wait_retry(rc, attempt, max_attempts, true))
      continue;

    log_debug("  [DB] prepare failed for %s: rc=%d err=%s", label, rc,
              (g_app_db ? sqlite3_errmsg(g_app_db) : "unknown"));
    close_app_db();
    return rc;
  }

  close_app_db();
  return -1;
}

static int update_snd0info(const char *title_id) {
  if (!title_id || title_id[0] == '\0')
    return -1;

  if (!g_app_db_stmt_update_snd0) {
    const char *sql =
        "UPDATE tbl_contentinfo "
        "SET snd0info = '/user/appmeta/' || ?1 || '/snd0.at9' "
        "WHERE titleId = ?1;";
    int prep_rc = app_db_prepare_with_retry(sql, &g_app_db_stmt_update_snd0,
                                            APP_DB_PREPARE_BUSY_RETRIES,
                                            "snd0info update");
    if (prep_rc != SQLITE_OK)
      return -1;
  }

  for (int attempt = 0; attempt < APP_DB_UPDATE_BUSY_RETRIES; attempt++) {
    sqlite3_reset(g_app_db_stmt_update_snd0);
    sqlite3_clear_bindings(g_app_db_stmt_update_snd0);
    if (sqlite3_bind_text(g_app_db_stmt_update_snd0, 1, title_id, -1,
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      if (g_app_db) {
        log_debug("  [DB] bind failed for snd0info update: %s",
                  sqlite3_errmsg(g_app_db));
      }
      close_app_db();
      return -1;
    }

    int rc = sqlite3_step(g_app_db_stmt_update_snd0);
    if (rc == SQLITE_DONE) {
      int changes = sqlite3_changes(g_app_db);
      close_app_db();
      return changes;
    }

    if (app_db_wait_retry(rc, attempt, APP_DB_UPDATE_BUSY_RETRIES, false))
      continue;

    if (g_app_db) {
      log_debug("  [DB] step failed for snd0info update: rc=%d err=%s", rc,
                sqlite3_errmsg(g_app_db));
    }
    close_app_db();
    return -1;
  }

  close_app_db();
  return -1;
}

static void free_app_db_title_list(struct AppDbTitleList *list) {
  if (!list)
    return;
  free(list->ids);
  list->ids = NULL;
  list->count = 0;
  list->capacity = 0;
}

static bool append_app_db_title(struct AppDbTitleList *list,
                                const char *title_id) {
  if (!list || !title_id || title_id[0] == '\0')
    return true;

  if (list->count >= list->capacity) {
    int new_capacity = (list->capacity > 0) ? (list->capacity * 2) : 1024;
    char(*new_ids)[MAX_TITLE_ID] =
        realloc(list->ids, (size_t)new_capacity * sizeof(*list->ids));
    if (!new_ids)
      return false;
    list->ids = new_ids;
    list->capacity = new_capacity;
  }

  (void)strlcpy(list->ids[list->count], title_id, MAX_TITLE_ID);
  list->count++;
  return true;
}

static int compare_title_id_str(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

static bool app_db_title_list_contains(const struct AppDbTitleList *list,
                                       const char *title_id) {
  if (!list || !title_id || title_id[0] == '\0')
    return false;
  return bsearch(title_id, list->ids, (size_t)list->count, sizeof(*list->ids),
                 compare_title_id_str) != NULL;
}

static bool load_app_db_title_list(struct AppDbTitleList *list) {
  if (!list)
    return false;
  free_app_db_title_list(list);

  const char *sql =
      "SELECT DISTINCT titleId "
      "FROM tbl_contentinfo "
      "WHERE titleId != '' "
      "ORDER BY titleId;";
  sqlite3_stmt *stmt = NULL;
  int prep_rc = app_db_prepare_with_retry(sql, &stmt, APP_DB_PREPARE_BUSY_RETRIES,
                                          "title list query");
  if (prep_rc != SQLITE_OK) {
    close_app_db();
    return false;
  }

  int busy_attempts = 0;
  while (!should_stop_requested()) {
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      const char *title_id = (const char *)sqlite3_column_text(stmt, 0);
      if (title_id && title_id[0] != '\0') {
        if (!append_app_db_title(list, title_id)) {
          log_debug("  [DB] title list allocation failed");
          sqlite3_finalize(stmt);
          close_app_db();
          free_app_db_title_list(list);
          return false;
        }
      }
      continue;
    }
    if (rc == SQLITE_DONE) {
      sqlite3_finalize(stmt);
      close_app_db();
      log_debug("  [DB] loaded app.db title list: %d entries", list->count);
      return true;
    }
    if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) &&
        busy_attempts + 1 < APP_DB_QUERY_BUSY_RETRIES) {
      busy_attempts++;
      sceKernelUsleep(APP_DB_BUSY_RETRY_SLEEP_US);
      continue;
    }

    log_debug("  [DB] title list query failed: rc=%d err=%s", rc,
              (g_app_db ? sqlite3_errmsg(g_app_db) : "unknown"));
    sqlite3_finalize(stmt);
    close_app_db();
    free_app_db_title_list(list);
    return false;
  }

  sqlite3_finalize(stmt);
  close_app_db();
  free_app_db_title_list(list);
  return false;
}

static void invalidate_app_db_title_cache(void) {
  g_app_db_title_cache_ready = false;
  g_app_db_title_cache_mtime = 0;
}

static bool get_app_db_title_list_cached(
    const struct AppDbTitleList **list_out) {
  if (!list_out)
    return false;
  *list_out = NULL;

  struct stat st;
  int app_db_stat_rc = stat(APP_DB_PATH, &st);

  if (!g_app_db_title_cache_ready ||
      (app_db_stat_rc == 0 && g_app_db_title_cache_mtime != st.st_mtime)) {
    struct AppDbTitleList fresh = {0};
    if (app_db_stat_rc == 0 && load_app_db_title_list(&fresh)) {
      free_app_db_title_list(&g_app_db_title_cache);
      g_app_db_title_cache = fresh;
      g_app_db_title_cache_mtime = st.st_mtime;
      g_app_db_title_cache_ready = true;
    } else {
      free_app_db_title_list(&fresh);
      if (!g_app_db_title_cache_ready)
        return false;
    }
  }

  *list_out = &g_app_db_title_cache;
  return true;
}

// --- FAST STABILITY CHECK ---
static bool is_path_stable_now(const char *path, double *root_diff_out,
                               int *stat_errno_out) {
  struct stat st;
  time_t now = time(NULL);
  ensure_runtime_config_ready();
  if (stat_errno_out)
    *stat_errno_out = 0;
  if (stat(path, &st) != 0) {
    if (root_diff_out)
      *root_diff_out = -1.0;
    if (stat_errno_out)
      *stat_errno_out = errno;
    return false;
  }

  double root_diff = difftime(now, st.st_mtime);
  if (root_diff_out)
    *root_diff_out = root_diff;
  if (root_diff < 0.0)
    return true;
  return root_diff > (double)g_runtime_cfg.stability_wait_seconds;
}

bool wait_for_stability_fast(const char *path, const char *name) {
  double diff = 0.0;
  int st_err = 0;
  if (is_path_stable_now(path, &diff, &st_err))
    return true;

  if (st_err != 0) {
    log_debug("  [WAIT] %s stat failed for %s: %s", name, path, strerror(st_err));
  } else {
    log_debug("  [WAIT] %s modified %.0fs ago. Waiting...", name, diff);
  }
  return false;
}

// --- Mount/Copy Helpers for Install Action ---
static int copy_file(const char *src, const char *dst);

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
  struct stat lst;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    snprintf(ss, sizeof(ss), "%s/%s", src, e->d_name);
    snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);
    if (lstat(ss, &lst) != 0) {
      ret = -1;
      break;
    }
    if (S_ISLNK(lst.st_mode)) {
      if (stat(ss, &st) != 0) {
        ret = -1;
        break;
      }
      if (S_ISDIR(st.st_mode)) {
        log_debug("  [COPY] refusing symlink directory: %s", ss);
        ret = -1;
        break;
      }
    } else {
      st = lst;
    }
    if (S_ISDIR(st.st_mode)) {
      if (copy_dir(ss, dd) != 0) {
        ret = -1;
        break;
      }
    } else {
      if (copy_file(ss, dd) != 0) {
        ret = -1;
        break;
      }
    }
  }
  if (closedir(d) != 0)
    ret = -1;
  return ret;
}

static int copy_param_json_rewrite(const char *src, const char *dst) {
  FILE *fs = fopen(src, "rb");
  if (!fs)
    return -1;

  if (fseek(fs, 0, SEEK_END) != 0) {
    fclose(fs);
    return -1;
  }
  long file_size = ftell(fs);
  if (file_size < 0) {
    fclose(fs);
    return -1;
  }
  rewind(fs);

  size_t len = (size_t)file_size;
  char *buf = (char *)malloc(len + 1);
  if (!buf) {
    fclose(fs);
    return -1;
  }
  if (len > 0 && fread(buf, 1, len, fs) != len) {
    free(buf);
    fclose(fs);
    return -1;
  }
  fclose(fs);
  buf[len] = '\0';

  char *hit = strstr(buf, "upgradable");
  if (hit) {
    size_t offset = (size_t)(hit - buf);
    size_t tail = len - offset - 10u;
    memcpy(hit, "standard", 8u);
    memmove(hit + 8u, hit + 10u, tail + 1u);
    len -= 2u;
  }

  FILE *fd = fopen(dst, "wb");
  if (!fd) {
    free(buf);
    return -1;
  }

  int ret = 0;
  if (len > 0 && fwrite(buf, 1, len, fd) != len)
    ret = -1;
  if (fclose(fd) != 0)
    ret = -1;

  if (ret == 0 && hit)
    log_debug("  [COPY] param.json patched: %s", dst);

  free(buf);
  return ret;
}

static int copy_file(const char *src, const char *dst) {
  if (strstr(src, "/sce_sys/param.json")) {
    return copy_param_json_rewrite(src, dst);
  }

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

// --- Device Node Wait and Source Stability ---
static bool wait_for_dev_node_state(const char *devname, bool should_exist) {
  for (int i = 0; i < LVD_NODE_WAIT_RETRIES; i++) {
    if ((access(devname, F_OK) == 0) == should_exist)
      return true;
    sceKernelUsleep(LVD_NODE_WAIT_US);
  }

  return false;
}

static bool is_source_stable_for_mount(const char *path, const char *name,
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

// --- Runtime Config Parsing ---
static void init_runtime_config_defaults(void) {
  g_runtime_cfg.debug_enabled = true;
  g_runtime_cfg.mount_read_only = (IMAGE_MOUNT_READ_ONLY != 0);
  g_runtime_cfg.force_mount = false;
  g_runtime_cfg.recursive_scan = false;
  (void)strlcpy(g_runtime_cfg.backports_path, DEFAULT_BACKPORTS_PATH,
                sizeof(g_runtime_cfg.backports_path));
  g_runtime_cfg.scan_interval_us = DEFAULT_SCAN_INTERVAL_US;
  g_runtime_cfg.stability_wait_seconds = DEFAULT_STABILITY_WAIT_SECONDS;
  g_runtime_cfg.exfat_backend = DEFAULT_EXFAT_BACKEND;
  g_runtime_cfg.ufs_backend = DEFAULT_UFS_BACKEND;
  g_runtime_cfg.lvd_sector_exfat = LVD_SECTOR_SIZE_EXFAT;
  g_runtime_cfg.lvd_sector_ufs = LVD_SECTOR_SIZE_UFS;
  g_runtime_cfg.lvd_sector_pfs = LVD_SECTOR_SIZE_PFS;
  g_runtime_cfg.md_sector_exfat = MD_SECTOR_SIZE_EXFAT;
  g_runtime_cfg.md_sector_ufs = MD_SECTOR_SIZE_UFS;
  memset(g_image_mode_rules, 0, sizeof(g_image_mode_rules));
  init_runtime_scan_paths_defaults();
  g_runtime_cfg_ready = true;
}

static void ensure_runtime_config_ready(void) {
  if (!g_runtime_cfg_ready)
    init_runtime_config_defaults();
}

static char *trim_ascii(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
    s++;
  size_t n = strlen(s);
  while (n > 0) {
    char c = s[n - 1];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      break;
    s[n - 1] = '\0';
    n--;
  }
  return s;
}

static bool parse_bool_ini(const char *value, bool *out) {
  if (!value || !out)
    return false;
  if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
      strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0 ||
      strcasecmp(value, "ro") == 0) {
    *out = true;
    return true;
  }
  if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
      strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0 ||
      strcasecmp(value, "rw") == 0) {
    *out = false;
    return true;
  }
  return false;
}

static bool parse_backend_ini(const char *value, attach_backend_t *out) {
  if (!value || !out)
    return false;
  if (strcasecmp(value, "lvd") == 0) {
    *out = ATTACH_BACKEND_LVD;
    return true;
  }
  if (strcasecmp(value, "md") == 0 || strcasecmp(value, "mdctl") == 0) {
    *out = ATTACH_BACKEND_MD;
    return true;
  }
  return false;
}

static bool parse_u32_ini(const char *value, uint32_t *out) {
  if (!value || !out)
    return false;
  errno = 0;
  char *end = NULL;
  unsigned long v = strtoul(value, &end, 0);
  if (errno != 0 || end == value || *end != '\0' || v > UINT32_MAX)
    return false;
  *out = (uint32_t)v;
  return true;
}

static bool is_valid_sector_size(uint32_t size) {
  if (size < 512u || size > 1024u * 1024u)
    return false;
  return (size & (size - 1u)) == 0u;
}

static bool load_runtime_config(void) {
  init_runtime_config_defaults();

  FILE *f = fopen(CONFIG_FILE, "r");
  if (!f) {
    if (errno != ENOENT) {
      log_debug("  [CFG] open failed: %s (%s)", CONFIG_FILE, strerror(errno));
    } else {
      log_debug("  [CFG] not found, using defaults");
    }
    return false;
  }

  char line[512];
  int line_no = 0;
  bool has_custom_scanpaths = false;
  while (fgets(line, sizeof(line), f)) {
    line_no++;
    char *s = trim_ascii(line);
    if (s[0] == '\0' || s[0] == '#' || s[0] == ';' || s[0] == '[')
      continue;

    char *eq = strchr(s, '=');
    if (!eq) {
      log_debug("  [CFG] invalid line %d (missing '=')", line_no);
      continue;
    }
    *eq = '\0';
    char *key = trim_ascii(s);
    char *value = trim_ascii(eq + 1);

    // Allow trailing inline comments.
    char *comment = strchr(value, '#');
    if (comment) {
      *comment = '\0';
      value = trim_ascii(value);
    }
    comment = strchr(value, ';');
    if (comment) {
      *comment = '\0';
      value = trim_ascii(value);
    }

    if (key[0] == '\0' || value[0] == '\0')
      continue;

    bool bval = false;
    uint32_t u32 = 0;
    attach_backend_t backend = ATTACH_BACKEND_NONE;

    if (strcasecmp(key, "debug") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      g_runtime_cfg.debug_enabled = bval;
      continue;
    }

    if (strcasecmp(key, "mount_read_only") == 0 ||
        strcasecmp(key, "read_only") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      g_runtime_cfg.mount_read_only = bval;
      continue;
    }

    if (strcasecmp(key, "force_mount") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      g_runtime_cfg.force_mount = bval;
      continue;
    }

    if (strcasecmp(key, "image_ro") == 0 ||
        strcasecmp(key, "image_rw") == 0) {
      bool rule_read_only = (strcasecmp(key, "image_ro") == 0);
      bool applied = false;
      const char *filename = get_filename_component(value);
      if (filename[0] != '\0') {
        for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
          if (!g_image_mode_rules[k].valid)
            continue;
          if (strcasecmp(g_image_mode_rules[k].filename, filename) != 0)
            continue;
          g_image_mode_rules[k].mount_read_only = rule_read_only;
          applied = true;
          break;
        }
        if (!applied) {
          for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
            if (g_image_mode_rules[k].valid)
              continue;
            (void)strlcpy(g_image_mode_rules[k].filename, filename,
                          sizeof(g_image_mode_rules[k].filename));
            g_image_mode_rules[k].mount_read_only = rule_read_only;
            g_image_mode_rules[k].valid = true;
            applied = true;
            break;
          }
        }
      }
      if (!applied) {
        log_debug("  [CFG] invalid image mode rule at line %d: %s=%s", line_no,
                  key, value);
      }
      continue;
    }

    if (strcasecmp(key, "recursive_scan") == 0) {
      if (!parse_bool_ini(value, &bval)) {
        log_debug("  [CFG] invalid bool at line %d: %s=%s", line_no, key, value);
        continue;
      }
      g_runtime_cfg.recursive_scan = bval;
      continue;
    }

    if (strcasecmp(key, "backports_path") == 0) {
      size_t len = strlcpy(g_runtime_cfg.backports_path, value,
                           sizeof(g_runtime_cfg.backports_path));
      if (len >= sizeof(g_runtime_cfg.backports_path)) {
        log_debug("  [CFG] invalid backports path at line %d: too long", line_no);
        (void)strlcpy(g_runtime_cfg.backports_path, DEFAULT_BACKPORTS_PATH,
                      sizeof(g_runtime_cfg.backports_path));
      } else {
        while (len > 1 && g_runtime_cfg.backports_path[len - 1] == '/') {
          g_runtime_cfg.backports_path[len - 1] = '\0';
          len--;
        }
      }
      continue;
    }

    if (strcasecmp(key, "scan_interval_seconds") == 0 ||
        strcasecmp(key, "scan_interval_sec") == 0) {
      if (!parse_u32_ini(value, &u32) || u32 < MIN_SCAN_INTERVAL_SECONDS ||
          u32 > MAX_SCAN_INTERVAL_SECONDS) {
        log_debug("  [CFG] invalid scan interval at line %d: %s=%s (range: %u..%u)",
                  line_no, key, value, (unsigned)MIN_SCAN_INTERVAL_SECONDS,
                  (unsigned)MAX_SCAN_INTERVAL_SECONDS);
        continue;
      }
      g_runtime_cfg.scan_interval_us = u32 * 1000000u;
      continue;
    }

    if (strcasecmp(key, "stability_wait_seconds") == 0 ||
        strcasecmp(key, "stability_wait_sec") == 0) {
      if (!parse_u32_ini(value, &u32) || u32 > MAX_STABILITY_WAIT_SECONDS) {
        log_debug("  [CFG] invalid stability wait at line %d: %s=%s (max: %u)",
                  line_no, key, value, (unsigned)MAX_STABILITY_WAIT_SECONDS);
        continue;
      }
      g_runtime_cfg.stability_wait_seconds = u32;
      continue;
    }

    if (strcasecmp(key, "exfat_backend") == 0) {
      if (!parse_backend_ini(value, &backend)) {
        log_debug("  [CFG] invalid backend at line %d: %s=%s", line_no, key,
                  value);
        continue;
      }
      g_runtime_cfg.exfat_backend = backend;
      continue;
    }

    if (strcasecmp(key, "ufs_backend") == 0) {
      if (!parse_backend_ini(value, &backend)) {
        log_debug("  [CFG] invalid backend at line %d: %s=%s", line_no, key,
                  value);
        continue;
      }
      g_runtime_cfg.ufs_backend = backend;
      continue;
    }

    if (strcasecmp(key, "scanpath") == 0) {
      if (!has_custom_scanpaths) {
        clear_runtime_scan_paths();
        has_custom_scanpaths = true;
      }
      if (!add_runtime_scan_path(value)) {
        log_debug("  [CFG] invalid scanpath at line %d: %s=%s", line_no, key,
                  value);
      }
      continue;
    }

    bool is_sector_key =
        (strcasecmp(key, "lvd_exfat_sector_size") == 0) ||
        (strcasecmp(key, "lvd_ufs_sector_size") == 0) ||
        (strcasecmp(key, "lvd_pfs_sector_size") == 0) ||
        (strcasecmp(key, "md_exfat_sector_size") == 0) ||
        (strcasecmp(key, "md_ufs_sector_size") == 0);

    if (!is_sector_key) {
      log_debug("  [CFG] unknown key at line %d: %s", line_no, key);
      continue;
    }

    if (!parse_u32_ini(value, &u32) || !is_valid_sector_size(u32)) {
      log_debug("  [CFG] invalid sector size at line %d: %s=%s", line_no, key, value);
      continue;
    }

    if (strcasecmp(key, "lvd_exfat_sector_size") == 0) {
      g_runtime_cfg.lvd_sector_exfat = u32;
    } else if (strcasecmp(key, "lvd_ufs_sector_size") == 0) {
      g_runtime_cfg.lvd_sector_ufs = u32;
    } else if (strcasecmp(key, "lvd_pfs_sector_size") == 0) {
      g_runtime_cfg.lvd_sector_pfs = u32;
    } else if (strcasecmp(key, "md_exfat_sector_size") == 0) {
      g_runtime_cfg.md_sector_exfat = u32;
    } else if (strcasecmp(key, "md_ufs_sector_size") == 0) {
      g_runtime_cfg.md_sector_ufs = u32;
    }
  }

  fclose(f);

  if (has_custom_scanpaths && g_scan_path_count == 0) {
    log_debug("  [CFG] no valid scanpath entries, using defaults");
    init_runtime_scan_paths_defaults();
  } else {
    // Image mount roots are always required for remount scanning.
    (void)add_runtime_scan_path(IMAGE_MOUNT_BASE "/" IMAGE_MOUNT_SUBDIR_UFS);
    (void)add_runtime_scan_path(IMAGE_MOUNT_BASE "/" IMAGE_MOUNT_SUBDIR_EXFAT);
    (void)add_runtime_scan_path(IMAGE_MOUNT_BASE "/" IMAGE_MOUNT_SUBDIR_PFS);
  }

  int image_rule_count = 0;
  for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
    if (g_image_mode_rules[k].valid)
      image_rule_count++;
  }

  log_debug("  [CFG] loaded: debug=%d ro=%d force=%d recursive_scan=%d "
            "backports_path=%s "
            "exfat_backend=%s ufs_backend=%s "
            "lvd_sec(exfat=%u ufs=%u pfs=%u) md_sec(exfat=%u ufs=%u) "
            "scan_interval_s=%u stability_wait_s=%u scan_paths=%d image_rules=%d",
            g_runtime_cfg.debug_enabled ? 1 : 0,
            g_runtime_cfg.mount_read_only ? 1 : 0,
            g_runtime_cfg.force_mount ? 1 : 0,
            g_runtime_cfg.recursive_scan ? 1 : 0,
            g_runtime_cfg.backports_path,
            backend_name(g_runtime_cfg.exfat_backend),
            backend_name(g_runtime_cfg.ufs_backend),
            g_runtime_cfg.lvd_sector_exfat, g_runtime_cfg.lvd_sector_ufs,
            g_runtime_cfg.lvd_sector_pfs, g_runtime_cfg.md_sector_exfat,
            g_runtime_cfg.md_sector_ufs, g_runtime_cfg.scan_interval_us / 1000000u,
            g_runtime_cfg.stability_wait_seconds, g_scan_path_count,
            image_rule_count);

  return true;
}

// --- Backend Option Mapping and Mount Flags ---
static uint32_t get_lvd_sector_size(image_fs_type_t fs_type) {
  ensure_runtime_config_ready();
  switch (fs_type) {
  case IMAGE_FS_UFS:
    return g_runtime_cfg.lvd_sector_ufs;
  case IMAGE_FS_PFS:
    return g_runtime_cfg.lvd_sector_pfs;
  case IMAGE_FS_EXFAT:
  default:
    return g_runtime_cfg.lvd_sector_exfat;
  }
}

static uint32_t get_md_sector_size(image_fs_type_t fs_type) {
  ensure_runtime_config_ready();
  switch (fs_type) {
  case IMAGE_FS_UFS:
    return g_runtime_cfg.md_sector_ufs;
  case IMAGE_FS_EXFAT:
  default:
    return g_runtime_cfg.md_sector_exfat;
  }
}

static unsigned int get_md_attach_options(bool mount_read_only) {
  unsigned int options = MD_AUTOUNIT | MD_ASYNC;
  if (mount_read_only)
    options |= MD_READONLY;
  return options;
}

static uint16_t get_lvd_attach_option(image_fs_type_t fs_type,
                                      bool mount_read_only) {
  if (fs_type == IMAGE_FS_UFS) {
    // UFS runtime mapping: RO -> 0x1E, RW -> 0x16.
    return mount_read_only ? LVD_ATTACH_OPTION_NORM_DD_RO
                           : LVD_ATTACH_OPTION_NORM_DD_RW;
  }

  // Generic/PFS runtime mapping: RO -> 0x9, RW -> 0x8.
  return mount_read_only ? LVD_ATTACH_OPTION_FLAGS_DEFAULT
                         : LVD_ATTACH_OPTION_FLAGS_RW;
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

static uint16_t lvd_option_len_from_flags(uint16_t options) {
  // Exact mirror of dr_lvd_attach_sub_7810 option-size derivation:
  // refs/libSceFsInternalForVsh.sprx.c (sceFsLvdAttachCommon, around +0x8295).
  // Practical values for this project:
  // - flags 0x8 (default/RO): option_len 0x14
  // - flags 0x9 (RW):         option_len 0x1C
  if ((options & 0x800Eu) != 0u) {
    uint32_t raw = (uint32_t)options;
    uint32_t len = (raw & 0xFFFF8000u) + ((raw & 2u) << 6) + (8u * (raw & 1u)) +
                   (2u * ((raw >> 2) & 1u)) + (2u * (raw & 8u)) + 4u;
    return (uint16_t)len;
  }
  return (uint16_t)(8u * ((uint32_t)options & 1u) + 4u);
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

static bool resolve_device_from_mount(const char *mount_point,
                                      attach_backend_t *backend_out,
                                      int *unit_out) {
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

static bool is_active_image_mount_point(const char *path) {
  return is_path_mountpoint(path);
}

// --- Device Detach Helpers ---
static bool detach_lvd_unit(int unit_id) {
  if (unit_id < 0)
    return true;

  int fd = open(LVD_CTRL_PATH, O_RDWR);
  if (fd < 0) {
    log_debug("  [IMG][%s] open %s for detach failed: %s",
              backend_name(ATTACH_BACKEND_LVD), LVD_CTRL_PATH, strerror(errno));
    return false;
  }

  lvd_ioctl_detach_t req;
  memset(&req, 0, sizeof(req));
  req.device_id = unit_id;

  bool ok = true;
  if (ioctl(fd, SCE_LVD_IOC_DETACH, &req) != 0) {
    log_debug("  [IMG][%s] detach %d failed: %s",
              backend_name(ATTACH_BACKEND_LVD), unit_id, strerror(errno));
    ok = false;
  }
  close(fd);

  char devname[64];
  snprintf(devname, sizeof(devname), "/dev/lvd%d", unit_id);
  if (!wait_for_dev_node_state(devname, false)) {
    log_debug("  [IMG][%s] device node still present after detach: /dev/lvd%d",
              backend_name(ATTACH_BACKEND_LVD), unit_id);
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
              backend_name(ATTACH_BACKEND_MD), MD_CTRL_PATH, strerror(errno));
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
                backend_name(ATTACH_BACKEND_MD), unit_id, strerror(errno));
      ok = false;
    } else {
      log_debug("  [IMG][%s] detach %d forced after error: %s",
                backend_name(ATTACH_BACKEND_MD), unit_id, strerror(err));
    }
  }
  close(fd);

  char devname[64];
  snprintf(devname, sizeof(devname), "/dev/md%d", unit_id);
  if (!wait_for_dev_node_state(devname, false)) {
    log_debug("  [IMG][%s] device node still present after detach: /dev/md%d",
              backend_name(ATTACH_BACKEND_MD), unit_id);
    ok = false;
  }
  return ok;
}

static bool detach_attached_unit(attach_backend_t backend, int unit_id) {
  if (backend == ATTACH_BACKEND_MD)
    return detach_md_unit(unit_id);
  else if (backend == ATTACH_BACKEND_LVD)
    return detach_lvd_unit(unit_id);
  return true;
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

static void log_fs_stats(const char *tag, const char *path,
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

static const char *image_fs_subdir(image_fs_type_t fs_type) {
  if (fs_type == IMAGE_FS_UFS)
    return IMAGE_MOUNT_SUBDIR_UFS;
  if (fs_type == IMAGE_FS_EXFAT)
    return IMAGE_MOUNT_SUBDIR_EXFAT;
  if (fs_type == IMAGE_FS_PFS)
    return IMAGE_MOUNT_SUBDIR_PFS;
  return "unknown";
}

static void build_image_mount_point(const char *file_path, image_fs_type_t fs_type,
                                  char *mount_point, size_t mount_point_size) {
  const char *filename = get_filename_component(file_path);
  char mount_name[MAX_PATH];
  strip_extension(filename, mount_name, sizeof(mount_name));
  snprintf(mount_point, mount_point_size, "%s/%s/%s", IMAGE_MOUNT_BASE,
           image_fs_subdir(fs_type), mount_name);
}

static bool unmount_image(const char *file_path, int unit_id,
                              attach_backend_t backend) {
  char mount_point[MAX_PATH];
  image_fs_type_t fs_type = detect_image_fs_type(file_path);
  build_image_mount_point(file_path, fs_type, mount_point, sizeof(mount_point));
  int resolved_unit = unit_id;
  attach_backend_t resolved_backend = backend;

  if (resolved_unit < 0 || resolved_backend == ATTACH_BACKEND_NONE) {
    if (!resolve_device_from_mount(mount_point, &resolved_backend,
                                   &resolved_unit)) {
      resolved_backend = ATTACH_BACKEND_NONE;
      resolved_unit = -1;
    }
  }

  // Remove mount.lnk and unmount /system_ex/app/<titleid> that point to this
  // source before unmounting the virtual disk itself.
  cleanup_mount_links(mount_point, true);

  // Unmount stacked layers (unionfs over image fs).
  for (int i = 0; i < MAX_LAYERED_UNMOUNT_ATTEMPTS; i++) {
    if (!is_path_mountpoint(mount_point))
      break;
    if (unmount(mount_point, 0) == 0)
      continue;
    if (errno == ENOENT || errno == EINVAL)
      break;
    if (unmount(mount_point, MNT_FORCE) != 0 && errno != ENOENT &&
        errno != EINVAL) {
      log_debug("  [IMG][%s] unmount failed for %s: %s",
                backend_name(resolved_backend), mount_point, strerror(errno));
      return false;
    }
  }

  if (is_path_mountpoint(mount_point)) {
    log_debug("  [IMG][%s] unmount incomplete for %s",
              backend_name(resolved_backend), mount_point);
    return false;
  }

  bool detach_ok = true;
  if (resolved_backend != ATTACH_BACKEND_NONE && resolved_unit >= 0)
    detach_ok = detach_attached_unit(resolved_backend, resolved_unit);

  if (rmdir(mount_point) == 0) {
    log_debug("  [IMG] Removed mount directory: %s", mount_point);
    return detach_ok;
  }

  int err = errno;
  if (err == ENOENT)
    return detach_ok;
  if (err == ENOTEMPTY || err == EBUSY) {
    log_debug("  [IMG] Mount directory not removed (%s): %s",
              strerror(err), mount_point);
    return detach_ok;
  }
  log_debug("  [IMG] Failed to remove mount directory %s: %s", mount_point,
            strerror(err));
  return detach_ok;
}

// --- Image Attach + nmount Pipeline ---
static bool mount_image(const char *file_path, image_fs_type_t fs_type) {
  ensure_runtime_config_ready();
  g_last_image_mount_errmsg[0] = '\0';
  bool mount_mode_overridden = false;
  bool mount_read_only = g_runtime_cfg.mount_read_only;
  bool force_mount = g_runtime_cfg.force_mount;
  const char *filename = get_filename_component(file_path);
  if (filename[0] != '\0') {
    for (int k = 0; k < MAX_IMAGE_MODE_RULES; k++) {
      if (!g_image_mode_rules[k].valid)
        continue;
      if (strcasecmp(g_image_mode_rules[k].filename, filename) != 0)
        continue;
      mount_read_only = g_image_mode_rules[k].mount_read_only;
      mount_mode_overridden = true;
      break;
    }
  }

  // Check if already in UFS cache
  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    if (image_cache[k].valid && strcmp(image_cache[k].path, file_path) == 0)
      return true;
  }

  // Build mount point from image path.
  char mount_point[MAX_PATH];
  build_image_mount_point(file_path, fs_type, mount_point, sizeof(mount_point));

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
          cache_image_mount(file_path, mount_point, existing_unit, existing_backend);
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
  if (st.st_size < 0) {
    log_debug("  [IMG] invalid file size for %s: %lld", file_path,
              (long long)st.st_size);
    return false;
  }

  log_debug("  [IMG] Mounting image (%s): %s -> %s", image_fs_name(fs_type),
            file_path, mount_point);
  if (mount_mode_overridden) {
    log_debug("  [CFG] Image mode override: %s -> %s", file_path,
              mount_read_only ? "ro" : "rw");
  }

  // Create mount point
  char fs_mount_root[MAX_PATH];
  snprintf(fs_mount_root, sizeof(fs_mount_root), "%s/%s", IMAGE_MOUNT_BASE,
           image_fs_subdir(fs_type));
  mkdir(IMAGE_MOUNT_BASE, 0777);
  mkdir(fs_mount_root, 0777);
  mkdir(mount_point, 0777);

  attach_backend_t attach_backend = ATTACH_BACKEND_LVD;
  if (fs_type == IMAGE_FS_EXFAT)
    attach_backend = g_runtime_cfg.exfat_backend;
  else if (fs_type == IMAGE_FS_UFS)
    attach_backend = g_runtime_cfg.ufs_backend;
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
    req.md_sectorsize = get_md_sector_size(fs_type);

    int last_errno = 0;
    req.md_options = get_md_attach_options(mount_read_only);
    log_debug("  [IMG][%s] attach try: options=0x%x",
              backend_name(attach_backend), req.md_options);
    ret = ioctl(md_fd, MDIOCATTACH, &req);
    if (ret != 0)
      last_errno = errno;
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

    snprintf(devname, sizeof(devname), "/dev/md%d", unit_id);
    if (!wait_for_dev_node_state(devname, true)) {
      log_debug("  [IMG][%s] device node did not appear: %s",
                backend_name(attach_backend), devname);
      detach_md_unit(unit_id);
      return false;
    }

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
    layers[0].size = (uint64_t)st.st_size;

    lvd_ioctl_attach_t req;
    memset(&req, 0, sizeof(req));
    req.io_version = LVD_ATTACH_IO_VERSION;
    req.image_type = (fs_type == IMAGE_FS_UFS)
                         ? LVD_ATTACH_IMAGE_TYPE_UFS_DOWNLOAD_DATA
                         : (fs_type == IMAGE_FS_PFS)
                               ? LVD_ATTACH_IMAGE_TYPE_PFS_SAVE_DATA
                               : LVD_ATTACH_IMAGE_TYPE;
    req.layer_count = LVD_ATTACH_LAYER_COUNT;
    req.device_size = (uint64_t)st.st_size;
    req.layers_ptr = layers;
    req.sector_size_0 = get_lvd_sector_size(fs_type);
    req.sector_size_1 = req.sector_size_0;

    int last_errno = 0;
    uint16_t attach_option = get_lvd_attach_option(fs_type, mount_read_only);
    if (fs_type == IMAGE_FS_UFS) {
      // DownloadData/LWFS path passes normalized option mask directly.
      req.option_len = attach_option;
    } else {
      req.option_len = lvd_option_len_from_flags(attach_option);
    }
    req.device_id = -1;
    log_debug("  [IMG][%s] attach try: ver=%u sec=%u options=0x%x len=0x%x",
              backend_name(attach_backend), req.io_version, req.sector_size_0,
              attach_option, req.option_len);

    ret = ioctl(lvd_fd, SCE_LVD_IOC_ATTACH, &req);
    if (ret != 0)
      last_errno = errno;
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

    snprintf(devname, sizeof(devname), "/dev/lvd%d", unit_id);
    if (!wait_for_dev_node_state(devname, true)) {
      log_debug("  [IMG][%s] device node did not appear: %s",
                backend_name(attach_backend), devname);
      detach_lvd_unit(unit_id);
      return false;
    }
  }

  log_debug("  [IMG][%s] Attached as %s", backend_name(attach_backend), devname);

  // --- MOUNT FILESYSTEM ---
  struct iovec *iov = NULL;
  unsigned int iovlen = 0;
  char mount_errmsg[256];
  memset(mount_errmsg, 0, sizeof(mount_errmsg));
  const char *sigverify = PFS_MOUNT_SIGVERIFY ? "1" : "0";
  const char *playgo = PFS_MOUNT_PLAYGO ? "1" : "0";
  const char *disc = PFS_MOUNT_DISC ? "1" : "0";
  const char *ekpfs_key = PFS_ZERO_EKPFS_KEY_HEX;

  struct iovec iov_ufs[] = {
      IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("ufs"), IOVEC_ENTRY("from"),
      IOVEC_ENTRY(devname),     IOVEC_ENTRY("fspath"), IOVEC_ENTRY(mount_point),
      IOVEC_ENTRY("budgetid"),  IOVEC_ENTRY(DEVPFS_BUDGET_GAME),
      IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("noatime"),   IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("automounted"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("errmsg"), {(void *)mount_errmsg, sizeof(mount_errmsg)},
      IOVEC_ENTRY("force"),     IOVEC_ENTRY(NULL)};

  struct iovec iov_exfat[] = {
      IOVEC_ENTRY("from"),      IOVEC_ENTRY(devname),
      IOVEC_ENTRY("fspath"),    IOVEC_ENTRY(mount_point),
      IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
      IOVEC_ENTRY("budgetid"),  IOVEC_ENTRY(DEVPFS_BUDGET_GAME),
      IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
      IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
      IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("noatime"),   IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("automounted"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("errmsg"),    {(void *)mount_errmsg, sizeof(mount_errmsg)},
      IOVEC_ENTRY("force"),     IOVEC_ENTRY(NULL)};

  struct iovec iov_pfs[] = {
      IOVEC_ENTRY("from"),      IOVEC_ENTRY(devname),
      IOVEC_ENTRY("fspath"),    IOVEC_ENTRY(mount_point),
      IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("pfs"),
      IOVEC_ENTRY("sigverify"), IOVEC_ENTRY(sigverify),
      IOVEC_ENTRY("mkeymode"),  IOVEC_ENTRY(PFS_MOUNT_MKEYMODE),
      IOVEC_ENTRY("budgetid"),  IOVEC_ENTRY(PFS_MOUNT_BUDGET_ID),
      IOVEC_ENTRY("playgo"),    IOVEC_ENTRY(playgo),
      IOVEC_ENTRY("disc"),      IOVEC_ENTRY(disc),
      IOVEC_ENTRY("ekpfs"),     IOVEC_ENTRY(ekpfs_key),
      IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("noatime"),   IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("automounted"), IOVEC_ENTRY(NULL),
      IOVEC_ENTRY("errmsg"),    {(void *)mount_errmsg, sizeof(mount_errmsg)},
      IOVEC_ENTRY("force"),     IOVEC_ENTRY(NULL)};

  if (fs_type == IMAGE_FS_UFS) {
    iov = iov_ufs;
    iovlen = (unsigned int)IOVEC_SIZE(iov_ufs) - (force_mount ? 0u : 2u);
  } else if (fs_type == IMAGE_FS_EXFAT) {
    iov = iov_exfat;
    iovlen = (unsigned int)IOVEC_SIZE(iov_exfat) - (force_mount ? 0u : 2u);
  } else if (fs_type == IMAGE_FS_PFS) {
    log_debug("  [IMG][%s] PFS ro=%d budgetid=%s mkeymode=%s "
              "sigverify=%s playgo=%s disc=%s ekpfs=zero",
              backend_name(attach_backend), mount_read_only ? 1 : 0,
              PFS_MOUNT_BUDGET_ID, PFS_MOUNT_MKEYMODE, sigverify, playgo, disc);
    iov = iov_pfs;
    iovlen = (unsigned int)IOVEC_SIZE(iov_pfs) - (force_mount ? 0u : 2u);
  } else {
    log_debug("  [IMG][%s] unsupported fstype=%s", backend_name(attach_backend),
              image_fs_name(fs_type));
    detach_attached_unit(attach_backend, unit_id);
    return false;
  }

  const char *mount_mode = NULL;
  unsigned int mount_flags =
      get_nmount_flags(fs_type, mount_read_only, &mount_mode);
  ret = nmount(iov, iovlen, (int)mount_flags);
  if (ret != 0) {
    int mount_errno = errno;
    if (mount_errmsg[0] != '\0') {
      (void)strlcpy(g_last_image_mount_errmsg, mount_errmsg,
                    sizeof(g_last_image_mount_errmsg));
      log_debug("  [IMG][%s] nmount %s errmsg: %s",
                backend_name(attach_backend), mount_mode, mount_errmsg);
    }
    log_debug("  [IMG][%s] nmount %s failed: %s",
              backend_name(attach_backend), mount_mode, strerror(mount_errno));
    detach_attached_unit(attach_backend, unit_id);
    errno = mount_errno;
    return false;
  }

  log_debug("  [IMG][%s] Mounted (%s) %s -> %s", backend_name(attach_backend),
            image_fs_name(fs_type), devname, mount_point);
  log_fs_stats("IMG", mount_point, image_fs_name(fs_type));

  struct stat param_st;
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  title_id[0] = '\0';
  title_name[0] = '\0';
  if (directory_has_param_json(mount_point, &param_st) &&
      get_game_info(mount_point, &param_st, title_id, title_name) &&
      title_id[0] != '\0') {
    char backport_path[MAX_PATH];
    struct stat backport_st;
    snprintf(backport_path, sizeof(backport_path), "%s/%s",
             g_runtime_cfg.backports_path, title_id);
    if (stat(backport_path, &backport_st) == 0 && S_ISDIR(backport_st.st_mode)) {
      struct iovec overlay_iov[] = {
          IOVEC_ENTRY("fstype"), IOVEC_ENTRY("unionfs"), IOVEC_ENTRY("from"),
          IOVEC_ENTRY(backport_path), IOVEC_ENTRY("fspath"),
          IOVEC_ENTRY(mount_point)};
      int overlay_flags = mount_read_only ? MNT_RDONLY : 0;
      if (nmount(overlay_iov, IOVEC_SIZE(overlay_iov), overlay_flags) == 0) {
        log_debug("  [IMG] backport overlay mounted (%s): %s -> %s",
                  mount_read_only ? "ro" : "rw", backport_path, mount_point);
      } else {
        int overlay_err = errno;
        log_debug("  [IMG] backport overlay failed: %s -> %s (%s)", backport_path,
                  mount_point, strerror(overlay_err));
        notify_system("Backport overlay failed: %s\n%s\n0x%08X", title_id,
                      backport_path, (uint32_t)overlay_err);
      }
    }
  }

  // Cache it
  cache_image_mount(file_path, mount_point, unit_id, attach_backend);

  return true;
}

// --- Image Mount Lifecycle (scan/removal) ---
static void cleanup_stale_image_mounts(void) {
  if (should_stop_requested())
    return;

  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    if (should_stop_requested())
      return;
    if (image_cache[k].valid && access(image_cache[k].path, F_OK) != 0) {
      log_debug("  [IMG][%s] Source removed, unmounting: %s",
                backend_name(image_cache[k].backend), image_cache[k].path);
      if (unmount_image(image_cache[k].path, image_cache[k].unit_id,
                            image_cache[k].backend)) {
        image_cache[k].valid = false;
      }
      continue;
    }

    if (!image_cache[k].valid)
      continue;

    image_fs_type_t fs_type = detect_image_fs_type(image_cache[k].path);

    char mount_point[MAX_PATH];
    build_image_mount_point(image_cache[k].path, fs_type, mount_point,
                          sizeof(mount_point));
    if (is_active_image_mount_point(mount_point))
      continue;

    char source_path[MAX_PATH];
    (void)strlcpy(source_path, image_cache[k].path, sizeof(source_path));
    log_debug("  [IMG][%s] mount lost, retrying: %s -> %s",
              backend_name(image_cache[k].backend), source_path, mount_point);

    for (int i = 0; i < MAX_PENDING; i++) {
      if (!cache[i].valid || strcmp(cache[i].path, mount_point) != 0)
        continue;
      cache[i].valid = false;
      cache[i].path[0] = '\0';
      cache[i].title_id[0] = '\0';
      cache[i].title_name[0] = '\0';
    }
    clear_missing_param_entry(mount_point);

    image_cache[k].valid = false;
    if (mount_image(source_path, fs_type)) {
      clear_image_mount_attempts(source_path);
      continue;
    }

    int mount_err = errno;
    if (bump_image_mount_attempts(source_path) == 1) {
      notify_image_mount_failed(source_path, mount_err);
    }
  }
}

static void cleanup_mount_dirs(void) {
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
    snprintf(full_path, sizeof(full_path), "%s/%s", IMAGE_MOUNT_BASE, entry->d_name);

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

    bool is_fs_root = (strcmp(entry->d_name, IMAGE_MOUNT_SUBDIR_UFS) == 0) ||
                      (strcmp(entry->d_name, IMAGE_MOUNT_SUBDIR_EXFAT) == 0) ||
                      (strcmp(entry->d_name, IMAGE_MOUNT_SUBDIR_PFS) == 0);
    if (!is_fs_root) {
      if (rmdir(full_path) == 0) {
        log_debug("  [IMG] removed empty mount dir: %s", full_path);
        continue;
      }
      if (errno == ENOTEMPTY || errno == EBUSY || errno == ENOENT)
        continue;
      log_debug("  [IMG] failed to remove mount dir %s: %s", full_path,
                strerror(errno));
      continue;
    }

    DIR *sub = opendir(full_path);
    if (!sub) {
      if (errno != ENOENT)
        log_debug("  [IMG] open %s failed: %s", full_path, strerror(errno));
      continue;
    }
    struct dirent *sub_entry;
    while ((sub_entry = readdir(sub)) != NULL) {
      if (should_stop_requested())
        break;
      if (sub_entry->d_name[0] == '.')
        continue;

      char sub_path[MAX_PATH];
      snprintf(sub_path, sizeof(sub_path), "%s/%s", full_path, sub_entry->d_name);

      bool is_sub_dir = false;
      if (sub_entry->d_type == DT_DIR) {
        is_sub_dir = true;
      } else if (sub_entry->d_type == DT_UNKNOWN) {
        struct stat sub_st;
        if (stat(sub_path, &sub_st) == 0)
          is_sub_dir = S_ISDIR(sub_st.st_mode);
      }
      if (!is_sub_dir)
        continue;

      if (rmdir(sub_path) == 0) {
        log_debug("  [IMG] removed empty mount dir: %s", sub_path);
        continue;
      }
      if (errno == ENOTEMPTY || errno == EBUSY || errno == ENOENT)
        continue;
      log_debug("  [IMG] failed to remove mount dir %s: %s", sub_path,
                strerror(errno));
    }
    closedir(sub);
  }

  closedir(d);
}

static void maybe_mount_image_file(const char *full_path,
                                   const char *display_name) {
  image_fs_type_t fs_type = detect_image_fs_type(display_name);
  if (fs_type == IMAGE_FS_UNKNOWN)
    return;
  if (!is_source_stable_for_mount(full_path, display_name, "IMG"))
    return;
  if (is_image_mount_limited(full_path))
    return;
  if (mount_image(full_path, fs_type))
    clear_image_mount_attempts(full_path);
  else {
    int mount_err = errno;
    if (bump_image_mount_attempts(full_path) == 1) {
      notify_image_mount_failed(full_path, mount_err);
    }
  }
}

static void shutdown_image_mounts(void) {
  for (int k = 0; k < MAX_IMAGE_MOUNTS; k++) {
    if (!image_cache[k].valid)
      continue;
    (void)unmount_image(image_cache[k].path, image_cache[k].unit_id,
                            image_cache[k].backend);
    image_cache[k].valid = false;
  }
}

// --- Game Metadata Parsing (param.json) ---
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

bool get_game_info(const char *base_path, const struct stat *param_st,
                   char *out_id, char *out_name) {
  if (!out_id || !out_name)
    return false;
  out_id[0] = '\0';
  out_name[0] = '\0';
  if (!base_path || !param_st || !S_ISREG(param_st->st_mode))
    return false;

  struct PathStateEntry *path_state = get_or_create_path_state(base_path);
  if (path_state && path_state->game_info_cached &&
      path_state->game_info_mtime == param_st->st_mtime &&
      path_state->game_info_size == param_st->st_size &&
      path_state->game_info_ino == param_st->st_ino) {
    if (!path_state->game_info_valid)
      return false;
    (void)strlcpy(out_id, path_state->game_title_id, MAX_TITLE_ID);
    (void)strlcpy(out_name, path_state->game_title_name, MAX_TITLE_NAME);
    return true;
  }

  if (param_st->st_size <= 0 || param_st->st_size > 1024 * 1024) {
    if (path_state) {
      path_state->game_info_cached = true;
      path_state->game_info_valid = false;
      path_state->game_info_mtime = param_st->st_mtime;
      path_state->game_info_size = param_st->st_size;
      path_state->game_info_ino = param_st->st_ino;
      path_state->game_title_id[0] = '\0';
      path_state->game_title_name[0] = '\0';
    }
    return false;
  }

  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s/sce_sys/param.json", base_path);
  FILE *f = fopen(path, "rb");
  if (!f) {
    if (path_state) {
      path_state->game_info_cached = true;
      path_state->game_info_valid = false;
      path_state->game_info_mtime = param_st->st_mtime;
      path_state->game_info_size = param_st->st_size;
      path_state->game_info_ino = param_st->st_ino;
      path_state->game_title_id[0] = '\0';
      path_state->game_title_name[0] = '\0';
    }
    return false;
  }

  size_t len = (size_t)param_st->st_size;
  char *buf = (char *)malloc(len + 1);
  if (!buf) {
    fclose(f);
    return false;
  }
  bool read_ok = (fread(buf, 1, len, f) == len);
  fclose(f);
  if (!read_ok) {
    free(buf);
    return false;
  }
  buf[len] = '\0';

  bool valid = false;
  int res = extract_json_string(buf, "titleId", out_id, MAX_TITLE_ID);
  if (res != 0)
    res = extract_json_string(buf, "title_id", out_id, MAX_TITLE_ID);
  if (res == 0) {
    const char *en_ptr = strstr(buf, "\"en-US\"");
    const char *search_start = en_ptr ? en_ptr : buf;
    if (extract_json_string(search_start, "titleName", out_name,
                            MAX_TITLE_NAME) != 0)
      extract_json_string(buf, "titleName", out_name, MAX_TITLE_NAME);
    if (out_name[0] == '\0')
      (void)strlcpy(out_name, out_id, MAX_TITLE_NAME);
    valid = true;
  }
  free(buf);

  if (path_state) {
    path_state->game_info_cached = true;
    path_state->game_info_valid = valid;
    path_state->game_info_mtime = param_st->st_mtime;
    path_state->game_info_size = param_st->st_size;
    path_state->game_info_ino = param_st->st_ino;
    if (valid) {
      (void)strlcpy(path_state->game_title_id, out_id,
                    sizeof(path_state->game_title_id));
      (void)strlcpy(path_state->game_title_name, out_name,
                    sizeof(path_state->game_title_name));
    } else {
      path_state->game_title_id[0] = '\0';
      path_state->game_title_name[0] = '\0';
    }
  }
  return valid;
}

static void prune_game_cache(void) {
  for (int k = 0; k < MAX_PENDING; k++) {
    if (!cache[k].valid)
      continue;
    if (access(cache[k].path, F_OK) == 0)
      continue;

    if (cache[k].title_id[0] != '\0')
      log_debug("  [CACHE] source removed: %s (%s)", cache[k].title_id,
                cache[k].path);
    else
      log_debug("  [CACHE] source removed: %s", cache[k].path);

    cache[k].valid = false;
    cache[k].path[0] = '\0';
    cache[k].title_id[0] = '\0';
    cache[k].title_name[0] = '\0';
  }
}

static bool directory_has_param_json(const char *dir_path,
                                     struct stat *param_st_out) {
  if (!dir_path || dir_path[0] == '\0')
    return false;

  int dir_fd = open(dir_path, O_RDONLY | O_DIRECTORY);
  if (dir_fd < 0)
    return false;

  struct stat st;
  if (fstatat(dir_fd, "sce_sys", &st, 0) == 0 && S_ISDIR(st.st_mode) &&
      fstatat(dir_fd, "sce_sys/param.json", &st, 0) == 0 &&
      S_ISREG(st.st_mode)) {
    if (param_st_out)
      *param_st_out = st;
    close(dir_fd);
    return true;
  }

  close(dir_fd);
  return false;
}

static bool is_under_discovered_param_root(
    const char *path, char discovered_param_roots[][MAX_PATH],
    int discovered_count) {
  if (!path)
    return false;
  for (int i = 0; i < discovered_count; i++) {
    const char *root = discovered_param_roots[i];
    if (root[0] == '\0')
      continue;
    size_t root_len = strlen(root);
    if (root_len == 0)
      continue;
    if (strncmp(path, root, root_len) != 0)
      continue;
    if (path[root_len] == '\0' || path[root_len] == '/')
      return true;
  }
  return false;
}

// --- Candidate Discovery ---
static bool try_collect_candidate_for_directory(
    const char *full_path, scan_candidate_t *candidates, int max_candidates,
    int *candidate_count, const struct AppDbTitleList *app_db_titles,
    bool app_db_titles_ready, char discovered_param_roots[][MAX_PATH],
    int *discovered_param_root_count) {
  struct stat param_st;
  memset(&param_st, 0, sizeof(param_st));
  bool has_param_json = false;
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  title_id[0] = '\0';
  title_name[0] = '\0';

  if (is_under_discovered_param_root(full_path, discovered_param_roots,
                                     *discovered_param_root_count)) {
    log_debug("  [SKIP] under discovered game root: %s", full_path);
    return true;
  }

  has_param_json = directory_has_param_json(full_path, &param_st);

  if (is_under_image_mount_base(full_path) && !is_active_image_mount_point(full_path)) {
    log_debug("  [SKIP] inactive mount path: %s", full_path);
    return has_param_json;
  }

  if (!has_param_json) {
    if (is_missing_param_scan_limited(full_path)) {
      log_debug("  [SKIP] param.json retry limit reached: %s", full_path);
    } else {
      record_missing_param_failure(full_path);
    }
    return false;
  }

  if (!get_game_info(full_path, &param_st, title_id, title_name)) {
    record_missing_param_failure(full_path);
    log_debug("  [SKIP] game info unavailable: %s", full_path);
    return true;
  }

  if (!is_under_discovered_param_root(full_path, discovered_param_roots,
                                      *discovered_param_root_count) &&
      *discovered_param_root_count < MAX_PENDING) {
    (void)strlcpy(discovered_param_roots[*discovered_param_root_count], full_path,
                  MAX_PATH);
    (*discovered_param_root_count)++;
  }
  clear_missing_param_entry(full_path);

  for (int i = 0; i < *candidate_count; i++) {
    if (strcmp(candidates[i].title_id, title_id) == 0) {
      log_debug("  [SKIP] title already queued in this cycle: %s (%s)", title_name,
                title_id);
      notify_duplicate_title_once(title_id, full_path, candidates[i].path);
      return true;
    }
  }

  if (!app_db_titles_ready) {
    log_debug("  [SKIP] app.db unavailable (locked/busy), deferring: %s (%s)",
              title_name, title_id);
    return true;
  }
  bool in_app_db = app_db_title_list_contains(app_db_titles, title_id);

  if (in_app_db) {
    for (int k = 0; k < MAX_PENDING; k++) {
      if (!cache[k].valid)
        continue;
      if (strcmp(cache[k].path, full_path) == 0 ||
          (title_id[0] != '\0' && strcmp(cache[k].title_id, title_id) == 0)) {
        log_debug("  [SKIP] already cached in this session: %s (%s) path=%s",
                  title_name, title_id, full_path);
        if (title_id[0] != '\0' && strcmp(cache[k].title_id, title_id) == 0 &&
            strcmp(cache[k].path, full_path) != 0) {
          notify_duplicate_title_once(title_id, full_path, cache[k].path);
        }
        return true;
      }
    }
  }

  if (!in_app_db && was_register_attempted(title_id)) {
    log_debug("  [SKIP] register/install already attempted once: %s (%s)",
              title_name, title_id);
    return true;
  }

  // Installed status requires both app files and app.db presence.
  bool installed = is_installed(title_id) && in_app_db;
  char tracked_path[MAX_PATH];
  if (installed &&
      read_mount_link(title_id, tracked_path, sizeof(tracked_path)) &&
      strcmp(tracked_path, full_path) == 0) {
    if (is_data_mounted(title_id))
      log_debug("  [SKIP] already installed+mounted+linked: %s (%s)", title_name,
                title_id);
    else
      log_debug("  [SKIP] already installed+linked (waiting kstuff mount): %s (%s)",
                title_name, title_id);
    return true;
  }

  uint8_t failed_attempts = get_failed_mount_attempts(title_id);
  if (failed_attempts >= MAX_FAILED_MOUNT_ATTEMPTS) {
    log_debug("  [SKIP] mount/register retry limit reached (%u/%u): %s (%s)",
              (unsigned)failed_attempts, (unsigned)MAX_FAILED_MOUNT_ATTEMPTS,
              title_name, title_id);
    return true;
  }

  if (!wait_for_stability_fast(full_path, title_name)) {
    log_debug("  [SKIP] source not stable yet: %s (%s)", title_name, full_path);
    return true;
  }

  if (*candidate_count >= max_candidates) {
    log_debug("  [SKIP] candidate queue full (%d): %s (%s)", max_candidates,
              title_name, title_id);
    return true;
  }

  (void)strlcpy(candidates[*candidate_count].path, full_path,
                sizeof(candidates[*candidate_count].path));
  (void)strlcpy(candidates[*candidate_count].title_id, title_id,
                sizeof(candidates[*candidate_count].title_id));
  (void)strlcpy(candidates[*candidate_count].title_name, title_name,
                sizeof(candidates[*candidate_count].title_name));
  candidates[*candidate_count].installed = installed;
  candidates[*candidate_count].in_app_db = in_app_db;
  (*candidate_count)++;
  return true;
}

static void collect_candidates_recursively(
    const char *dir_path, scan_candidate_t *candidates, int max_candidates,
    int *candidate_count, const struct AppDbTitleList *app_db_titles,
    bool app_db_titles_ready, char discovered_param_roots[][MAX_PATH],
    int *discovered_param_root_count) {
  if (should_stop_requested() || !dir_path || dir_path[0] == '\0')
    return;

  // Once a directory has sce_sys/param.json (valid or not),
  // it is treated as a terminal game root and descendants are skipped.
  if (try_collect_candidate_for_directory(
          dir_path, candidates, max_candidates, candidate_count, app_db_titles,
          app_db_titles_ready, discovered_param_roots,
          discovered_param_root_count)) {
    return;
  }

  DIR *d = opendir(dir_path);
  if (!d)
    return;

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (should_stop_requested())
      break;
    if (entry->d_name[0] == '.')
      continue;

    char full_path[MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

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

    collect_candidates_recursively(
        full_path, candidates, max_candidates, candidate_count, app_db_titles,
        app_db_titles_ready, discovered_param_roots, discovered_param_root_count);
  }

  closedir(d);
}

// --- Unified Scan Pass (images + game candidates) ---
static void cleanup_lost_sources_before_scan(void) {
  // 1) Drop stale game cache entries for deleted sources.
  prune_game_cache();
  // 2) Drop stale/broken mount links.
  cleanup_mount_links(NULL, false);
  // 3) Unmount stale image mounts for deleted image files.
  cleanup_stale_image_mounts();
  // 4) Drop stale path-state entries.
  prune_path_state();
}

static int collect_scan_candidates(scan_candidate_t *candidates,
                                   int max_candidates,
                                   int *total_found_out) {
  int candidate_count = 0;
  const struct AppDbTitleList *app_db_titles = NULL;
  bool app_db_titles_ready = get_app_db_title_list_cached(&app_db_titles);
  char (*discovered_param_roots)[MAX_PATH] = g_scan_discovered_param_roots;
  int discovered_param_root_count = 0;
  memset(discovered_param_roots, 0, sizeof(g_scan_discovered_param_roots));

  if (!app_db_titles_ready) {
    log_debug("  [DB] app.db title list unavailable for this scan cycle");
  }

  for (int i = 0; SCAN_PATHS[i] != NULL; i++) {
    if (should_stop_requested())
      goto done;

    DIR *d = opendir(SCAN_PATHS[i]);
    if (!d)
      continue;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
      if (should_stop_requested()) {
        closedir(d);
        goto done;
      }
      if (entry->d_name[0] == '.')
        continue;

      char full_path[MAX_PATH];
      snprintf(full_path, sizeof(full_path), "%s/%s", SCAN_PATHS[i],
               entry->d_name);

      bool is_dir = false;
      bool is_regular = false;
      if (entry->d_type == DT_DIR) {
        is_dir = true;
      } else if (entry->d_type == DT_REG) {
        is_regular = true;
      } else if (entry->d_type == DT_UNKNOWN) {
        struct stat st;
        if (stat(full_path, &st) == 0) {
          is_dir = S_ISDIR(st.st_mode);
          is_regular = S_ISREG(st.st_mode);
        }
      }

      if (!path_matches_root_or_child(SCAN_PATHS[i], IMAGE_MOUNT_BASE) &&
          is_regular)
        maybe_mount_image_file(full_path, entry->d_name);
      if (!is_dir)
        continue;

      if (g_runtime_cfg.recursive_scan) {
        collect_candidates_recursively(
            full_path, candidates, max_candidates, &candidate_count,
            app_db_titles, app_db_titles_ready, discovered_param_roots,
            &discovered_param_root_count);
      } else {
        (void)try_collect_candidate_for_directory(
            full_path, candidates, max_candidates, &candidate_count,
            app_db_titles, app_db_titles_ready, discovered_param_roots,
            &discovered_param_root_count);
      }
    }
    closedir(d);
  }
done:
  if (total_found_out)
    *total_found_out = discovered_param_root_count;
  return candidate_count;
}

// --- Install/Remount Action ---
bool mount_and_install(const char *src_path, const char *title_id,
                       const char *title_name, bool is_remount,
                       bool should_register) {
  char user_app_dir[MAX_PATH];
  char user_sce_sys[MAX_PATH];
  char src_sce_sys[MAX_PATH];

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
      return false;
    }

    char icon_src[MAX_PATH], icon_dst[MAX_PATH];
    snprintf(icon_src, sizeof(icon_src), "%s/sce_sys/icon0.png", src_path);
    snprintf(icon_dst, sizeof(icon_dst), "/user/app/%s/icon0.png", title_id);
    if (copy_file(icon_src, icon_dst) != 0) {
      log_debug("  [COPY] Failed to copy icon: %s -> %s", icon_src, icon_dst);
      return false;
    }
  } else {
    log_debug("  [SPEED] Skipping file copy (Assets already exist)");
  }

  // WRITE TRACKER
  char lnk_path[MAX_PATH];
  snprintf(lnk_path, sizeof(lnk_path), "/user/app/%s/mount.lnk", title_id);
  bool link_ok = false;
  FILE *flnk = fopen(lnk_path, "w");
  if (flnk) {
    bool write_ok = (fprintf(flnk, "%s", src_path) >= 0);
    bool flush_ok = (fflush(flnk) == 0);
    bool close_ok = (fclose(flnk) == 0);
    if (write_ok && flush_ok && close_ok) {
      link_ok = true;
    } else {
      log_debug("  [LINK] write failed for %s: %s", lnk_path, strerror(errno));
    }
  } else {
    log_debug("  [LINK] open failed for %s: %s", lnk_path, strerror(errno));
  }
  if (!link_ok) {
    return false;
  }

  if (!should_register) {
    log_debug("  [REG] Skip (already present in app.db)");
    return true;
  }

  // REGISTER
  char src_snd0[MAX_PATH];
  snprintf(src_snd0, sizeof(src_snd0), "%s/sce_sys/snd0.at9", src_path);
  bool has_src_snd0 = (access(src_snd0, F_OK) == 0);

  mark_register_attempted(title_id);
  int res = sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", 0);
  sceKernelUsleep(200000);

  if (res == 0) {
    invalidate_app_db_title_cache();
    log_debug("  [REG] Installed NEW!");
    trigger_rich_toast(title_id, title_name, "Installed");
    if (has_src_snd0) {
      int snd0_updates = update_snd0info(title_id);
      if (snd0_updates >= 0)
        log_debug("  [DB] snd0info updated rows=%d", snd0_updates);
    }
  } else if ((uint32_t)res == 0x80990002u) {
    invalidate_app_db_title_cache();
    log_debug("  [REG] Restored.");
    if (has_src_snd0) {
      int snd0_updates = update_snd0info(title_id);
      if (snd0_updates >= 0)
        log_debug("  [DB] snd0info updated rows=%d", snd0_updates);
    } 
    // Silent on restore/remount to avoid spam
  } else {
    log_debug("  [REG] FAIL: 0x%x", res);
    notify_system("Register failed: %s (%s)\ncode=0x%08X", title_name, title_id,
                  (uint32_t)res);
    return false;
  }
  return true;
}

// --- Execution (per discovered candidate) ---
static void process_scan_candidates(const scan_candidate_t *candidates,
                                    int candidate_count) {
  for (int i = 0; i < candidate_count; i++) {
    if (should_stop_requested())
      return;

    const scan_candidate_t *c = &candidates[i];

    if (c->installed) {
      log_debug("  [ACTION] Remounting: %s", c->title_name);
    } else {
      log_debug("  [ACTION] Installing: %s (%s)", c->title_name, c->title_id);
      notify_system("Installing: %s (%s)...", c->title_name, c->title_id);
    }

    if (mount_and_install(c->path, c->title_id, c->title_name, c->installed,
                          !c->in_app_db)) {
      clear_failed_mount_attempts(c->title_id);
      cache_game_entry(c->path, c->title_id, c->title_name);
    } else {
      uint8_t failed_attempts = bump_failed_mount_attempts(c->title_id);
      if (failed_attempts == MAX_FAILED_MOUNT_ATTEMPTS) {
        log_debug("  [RETRY] limit reached (%u/%u): %s (%s)",
                  (unsigned)failed_attempts,
                  (unsigned)MAX_FAILED_MOUNT_ATTEMPTS, c->title_name, c->title_id);
      }
    }
  }
}

// --- Scan Orchestration ---
static int scan_all_paths_once(bool execute_actions) {
  cleanup_lost_sources_before_scan();
  int candidate_count =
      collect_scan_candidates(g_scan_candidates, MAX_PENDING, NULL);
  if (execute_actions && candidate_count > 0)
    process_scan_candidates(g_scan_candidates, candidate_count);
  return candidate_count;
}

void scan_all_paths(void) { (void)scan_all_paths_once(true); }

// --- Program Entry ---
int main(void) {
  int lock = -1;

  syscall(SYS_thr_set_name, -1, PAYLOAD_NAME);

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
      printf("[LOCK] Another instance is already running.\n");
      printf("[LOCK] Stop the first instance by creating %s and retry.\n",
             KILL_FILE);
      close(lock);
      sceUserServiceTerminate();
      return 0;
    }
    printf("[LOCK] Failed to lock %s: %s\n", LOCK_FILE, strerror(errno));
    close(lock);
    sceUserServiceTerminate();
    return 1;
  }

  (void)unlink(LOG_FILE_PREV);
  (void)rename(LOG_FILE, LOG_FILE_PREV);

  log_debug("ShadowMount+ v%s exFAT/UFS/PFS/LVD/MD. Thx to VoidWhisper/Gezine/Earthonion/EchoStretch/Drakmor", SHADOWMOUNT_VERSION);
  load_runtime_config();

  notify_system("ShadowMount+ v%s exFAT/UFS/PFS",
                  SHADOWMOUNT_VERSION);


  for (int i = 0; SCAN_PATHS[i] != NULL; i++) {
    DIR *d = opendir(SCAN_PATHS[i]);
    if (!d)
      continue;

    bool non_empty = false;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
      if ((entry->d_name[0] == '.' && entry->d_name[1] == '\0') ||
          (entry->d_name[0] == '.' && entry->d_name[1] == '.' &&
           entry->d_name[2] == '\0')) {
        continue;
      }
      non_empty = true;
      break;
    }
    closedir(d);

    if (non_empty)
      log_fs_stats("SCAN", SCAN_PATHS[i], NULL);
  }
  
  if (g_runtime_cfg.recursive_scan)
    notify_system("ShadowMount+: Recursive scan enabled.");

  cleanup_mount_dirs();

  // --- STARTUP LOGIC ---
  cleanup_lost_sources_before_scan();
  int total_found_games = 0;
  int new_games = collect_scan_candidates(g_scan_candidates, MAX_PENDING,
                                          &total_found_games);
  int notify_games = 0;
  for (int i = 0; i < new_games; i++) {
    if (!g_scan_candidates[i].installed)
      notify_games++;
  }

  if (new_games) {
    if (notify_games > 0) {
      notify_system("Found %d new games. Executing...", notify_games);
    }
    process_scan_candidates(g_scan_candidates, new_games);

    // Completion Message
    notify_system("Library Synchronized. Found %d games.", total_found_games);
  }

  // --- DAEMON LOOP ---
  while (true) {
    if (should_stop_requested()) {
      log_debug("[SHUTDOWN] stop requested");
      break;
    }

    // Sleep FIRST since we either just finished scan above, or library was
    // ready.
    if (sleep_with_stop_check(g_runtime_cfg.scan_interval_us)) {
      log_debug("[SHUTDOWN] stop requested during sleep");
      break;
    }

    scan_all_paths();
  }

  shutdown_image_mounts();
  free_app_db_title_list(&g_app_db_title_cache);
  invalidate_app_db_title_cache();
  close_app_db();
  if (lock >= 0) {
    close(lock);
  }
  sceUserServiceTerminate();
  return 0;
}
