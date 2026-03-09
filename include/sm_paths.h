#ifndef SM_PATHS_H
#define SM_PATHS_H

#define IMAGE_MOUNT_BASE "/data/imgmnt"
#define IMAGE_MOUNT_SUBDIR_UFS "ufsmnt"
#define IMAGE_MOUNT_SUBDIR_EXFAT "exfatmnt"
#define IMAGE_MOUNT_SUBDIR_PFS "pfsmnt"

#define DEFAULT_BACKPORTS_DIR_NAME "backports"
#define LOG_DIR "/data/shadowmount"
#define LOG_FILE "/data/shadowmount/debug.log"
#define LOG_FILE_PREV "/data/shadowmount/debug.log.1"
#define CONFIG_FILE "/data/shadowmount/config.ini"
#define KILL_FILE "/data/shadowmount/STOP"
#define TOAST_FILE "/data/shadowmount/notify.txt"
#define NOTIFY_ICON_DIR "/user/data/shadowmount"
#define NOTIFY_ICON_FILE "/user/data/shadowmount/smp_icon.png"
#define APP_DB_PATH "/system_data/priv/mms/app.db"

/* Compile-time default scan roots (used when config has no scanpath entries). */
#define SM_DEFAULT_SCAN_PATHS_INITIALIZER                                      \
  {                                                                            \
    /* Internal */                                                             \
    "/data/homebrew", "/data/etaHEN/games",                                   \
    /* Extended Storage */                                                     \
    "/mnt/ext0/homebrew", "/mnt/ext0/etaHEN/games",                           \
    /* M.2 Drive */                                                            \
    "/mnt/ext1/homebrew", "/mnt/ext1/etaHEN/games",                           \
    /* USB Subfolders */                                                       \
    "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew",         \
        "/mnt/usb3/homebrew", "/mnt/usb4/homebrew", "/mnt/usb5/homebrew",     \
        "/mnt/usb6/homebrew", "/mnt/usb7/homebrew",                           \
        "/mnt/usb0/etaHEN/games", "/mnt/usb1/etaHEN/games",                   \
        "/mnt/usb2/etaHEN/games", "/mnt/usb3/etaHEN/games",                   \
        "/mnt/usb4/etaHEN/games", "/mnt/usb5/etaHEN/games",                   \
        "/mnt/usb6/etaHEN/games", "/mnt/usb7/etaHEN/games",                   \
    /* USB Root Paths */                                                       \
    "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4",          \
        "/mnt/usb5", "/mnt/usb6", "/mnt/usb7", "/mnt/ext0", "/mnt/ext1", NULL \
  }

#endif
