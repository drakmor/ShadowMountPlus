#ifndef SM_MOUNT_DEFS_H
#define SM_MOUNT_DEFS_H

#define IMAGE_MOUNT_READ_ONLY 1

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
#define PFS_ZERO_EKPFS_KEY_HEX                                                \
  "0000000000000000000000000000000000000000000000000000000000000000"

#endif
