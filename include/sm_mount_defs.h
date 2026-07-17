#ifndef SM_MOUNT_DEFS_H
#define SM_MOUNT_DEFS_H

#include <stdint.h>

#define IMAGE_MOUNT_READ_ONLY 1

// Build-time backend defaults. runtime config.ini can override both values.
// 1 = legacy /dev/mdctl, 0 = /dev/lvdctl.
#define EXFAT_ATTACH_USE_MDCTL 0
#define UFS_ATTACH_USE_MDCTL 0

/*
 * Mount profiles and cache layers
 * ================================================================
 *
 * LVD geometry
 * ------------
 * sector_size is the logical sector exported by /dev/lvdN. It controls BIO
 * alignment and is not a filesystem allocation block. ShadowMount defaults to
 * 512 bytes for exFAT and 4096 bytes for UFS/PFS.
 *
 * secondary_unit is the LVD mapping/split granularity. ShadowMount uses 64 KiB
 * for all supported image filesystems. It matches the exFAT/UFS allocation
 * profile and PFS/PFSC block boundary, must be divisible by sector_size, and is
 * required by the type-5 BFS sdimg path.
 *
 * LVD attach ABI and image types
 * ------------------------------
 * The kernel exposes base attach V0, extended attach V1/Attach2 and detach.
 * ShadowMount builds one complete V0 layer and marks it NO_BITMAP. source_type
 * 1 is a regular file; source_type 2 is a device/special source.
 *
 * Accepted image types and kernel names:
 *   0 Dfl, 1 GmI, 2 GmO, 3 PtcI, 4 PtcO, 5 Sv, 6 Trp, 7 Dwl,
 *   8 AcI, 9 AcO, 10 MaI, 11 MaO, 12 Pcdwl.
 *
 * Types 0/5/6/7/12 use lvdstart_vnode_direct. Types 1-4/8-11 use
 * lvdstart_vnode and resolve each secondary unit through layered bitmap and
 * optional OTBL metadata. Current ShadowMount profiles are:
 *
 *   .exfat                         type 5  Sv,  direct + BFS sdimg batching
 *   .ffpkg (UFS)                  type 5  Sv,  direct + BFS sdimg batching
 *   standalone .ffpfs             type 5  Sv,  direct
 *   outer .ffpfsc                 type 9  AcO, layered package outer
 *   inner .ffpfs/pfs_image.dat    type 8  AcI, layered package inner
 *
 * ShadowMount deliberately uses type 5 with single/save attach flags for UFS
 * images so a standalone .ffpkg on BFS can use sdimg batching. Type 7 with
 * Download Data instead favors HDD request isolation. UFS nmount has
 * no separate download-data flag: 0x10000000 is the generic MNT_NOATIME bit,
 * not an UFS or DD mode. Do not substitute GmO/PtcO only to request the UFS
 * block-map cache: that replaces direct dispatch with layered dispatch. The
 * kernel requests that cache only for type 2/4, an HDD-class regular backing
 * vnode, and a backing filesystem that is UFS.
 *
 * Type-5 BFS sdimg fast path
 * -------------------------
 * Type 5 labels the LVD provider "Ssd" and may enable BFS sdimg when the image
 * is a regular file on internal BFS and secondary_unit is exactly 64 KiB. The
 * kernel also applies platform/storage-mode and lower-provider guards.
 *
 * When enabled, lvd_worker accepts only requests whose length is exactly
 * 64 KiB and whose backing-file offset is 64-KiB aligned. It collects the
 * initial request plus up to 30 matching queued requests and submits them via
 * bfs_iosession_rw_sdimg/BfsSdimg. All other requests fall back to the normal
 * vnode path. This is block-I/O batching, not an exFAT cache or decompressor.
 *
 * For exFAT keep sector_size at 512 and create the filesystem with a 64-KiB
 * allocation unit. Setting sector_size to 65536 does not request this mode;
 * the relevant LVD value is secondary_unit. The path is unavailable through
 * MD or when the image is backed by USB/UFS/PFS/exFAT instead of BFS.
 *
 * Raw flags and validator
 * -----------------------
 * Download-data flags exist only in the LVD attach ABI. ShadowMount calls
 * /dev/lvdctl directly and normalizes wrapper-style presets:
 * raw 0x8/0x9 -> single/save-data family 0x14/0x1c;
 * raw 0xc/0xd -> download-data family 0x16/0x1e.
 * Firmware pairs raw bit 0 with read-only variants; it is not an independent
 * LVD read-only flag. The normalized validator masks are:
 *
 *   type 0,5,6,8-11: (flags & 0x92) == 0x10
 *   type 1:           (flags & 0x82) == 0x80
 *   type 2,3,4:       (flags & 0x82) == 0
 *   type 7,12:        (flags & 0x92) == 0x12
 *
 * Download Data HDD worker policy
 * -------------------------------
 * Download Data is normalized LVD bit 0x02 and is required by types 7 and 12.
 * On an HDD-class provider it makes LVD create two request workers instead of
 * one: worker 0 is labelled System (lvdhdsw), worker 1 Game (lvdhdgw). The LVD
 * start routine routes each BIO using the kernel classification byte at
 * BIO + 0x172: value 1 selects Game and every other value selects System. LVD
 * also tracks and drains the workers separately during shutdown. The flag is
 * therefore an HDD queue-isolation/QoS policy, not an on-disk-format property.
 *
 * Download Data does not select direct BIO dispatch (image_type does), enable
 * bitmap/OTBL interpretation, populate the UFS block-map cache (types 2/4),
 * unlock BFS sdimg batching (type 5), or enable PFS/compression caches. For a
 * standalone image, type 7 + Download Data is the HDD-oriented choice when
 * System/Game queue separation matters. Type 5 + Single/Save is preferable for
 * a BFS sdimg-backed .ffpkg when maximum sequential/batched throughput matters
 * and the two-queue HDD policy is not needed.
 *
 * Layer metadata and independent caches
 * -------------------------------------
 * LVD bitmap says whether a logical unit is present in a layer. V1 OTBL maps a
 * logical unit to a physical unit inside the layer file. It is unrelated to a
 * PFSC compressed-offset table.
 *
 *   LVD bitmap/OTBL       image-layer presence/remapping
 *   UFS block-map cache   backing UFS file blocks -> physical UFS blocks
 *   PFS GDDR5 ioctl       PFSC compressed offsets and optional full ICV
 *
 * The UFS cache depends on the filesystem containing the image, not UFS stored
 * inside .ffpkg, and is unavailable for BFS/PFS/exFAT backing files. The PFS
 * metadata ioctl applies to every nested image backed by pfs, ppr_pfs,
 * transaction_pfs, or a forwarding overlay; the filesystem inside the image
 * does not need to be PFS.
 *
 * Backend and nmount policy
 * -------------------------
 * LVD is the default for every format. MD is a compatibility backend for
 * UFS/exFAT; it uses AUTOUNIT|ASYNC and native logical sectors (UFS 4096,
 * exFAT 512). Read-only state is applied to the backing
 * device and nmount. Current nmount profiles are:
 *
 *   UFS   ufs + budgetid + async + noatime + automounted
 *   exFAT exfatfs + large + static-timezone + async + noatime + ignoreacl
 *   PFS   pfs + AC + game + sigverify/playgo/disc=0 + zero ekpfs
 *         + async + noatime
 *
 * ShadowMount passes only MNT_RDONLY (0x1) in nmount's numeric flags argument.
 * Generic named options are decoded by the kernel as async=MNT_ASYNC (0x40),
 * noatime=MNT_NOATIME (0x10000000), and automounted=MNT_AUTOMOUNTED
 * (0x200000000). UFS options compressedfile, iochunk, largewrite and related
 * zone-aware modes are separate filesystem features and are not DD flags.
 *
 * force is appended only for explicit recovery. Unsigned PFS keeps signature
 * verification disabled; every nested image on PFS primes the containing
 * file's compressed-offset cache before LVD attach.
 */

#define LVD_CTRL_PATH "/dev/lvdctl"
#define MD_CTRL_PATH "/dev/mdctl"
#define SCE_LVD_IOC_ATTACH_V0 0xC0286D00
#define SCE_LVD_IOC_ATTACH_V1 0xC0286D09
#define SCE_LVD_IOC_DETACH 0xC0286D01
#define LVD_ATTACH_IO_VERSION_V0 0u
#define LVD_ATTACH_IO_VERSION_V1 1u
#define LVD_ATTACH_RAW_BIT_RO_VARIANT 0x0001u
#define LVD_ATTACH_RAW_BIT_INNER_PROFILE 0x0002u
#define LVD_ATTACH_RAW_BIT_DOWNLOAD_DATA_PROFILE 0x0004u
#define LVD_ATTACH_RAW_BIT_SINGLE_SAVE_PROFILE 0x0008u
#define LVD_ATTACH_RAW_PASSTHROUGH_MASK 0x8000u

#define LVD_ATTACH_FLAG_DOWNLOAD_DATA_PROFILE 0x0002u
#define LVD_ATTACH_FLAG_BASE 0x0004u
#define LVD_ATTACH_FLAG_RO_VARIANT 0x0008u
#define LVD_ATTACH_FLAG_SINGLE_SAVE_PROFILE 0x0010u
#define LVD_ATTACH_FLAG_INNER_PROFILE 0x0080u

#define LVD_ATTACH_RAW_FLAGS_SINGLE_RW LVD_ATTACH_RAW_BIT_SINGLE_SAVE_PROFILE
#define LVD_ATTACH_RAW_FLAGS_SINGLE_RO                                      \
  (LVD_ATTACH_RAW_FLAGS_SINGLE_RW | LVD_ATTACH_RAW_BIT_RO_VARIANT)
#define LVD_ATTACH_RAW_FLAGS_DD_RW                                          \
  (LVD_ATTACH_RAW_BIT_SINGLE_SAVE_PROFILE |                                \
   LVD_ATTACH_RAW_BIT_DOWNLOAD_DATA_PROFILE)
#define LVD_ATTACH_RAW_FLAGS_DD_RO                                          \
  (LVD_ATTACH_RAW_FLAGS_DD_RW | LVD_ATTACH_RAW_BIT_RO_VARIANT)

#define LVD_ATTACH_FLAGS_SINGLE_RW                                          \
  (LVD_ATTACH_FLAG_BASE | LVD_ATTACH_FLAG_SINGLE_SAVE_PROFILE)
#define LVD_ATTACH_FLAGS_SINGLE_RO                                          \
  (LVD_ATTACH_FLAGS_SINGLE_RW | LVD_ATTACH_FLAG_RO_VARIANT)
#define LVD_ATTACH_FLAGS_DD_RW                                              \
  (LVD_ATTACH_FLAGS_SINGLE_RW | LVD_ATTACH_FLAG_DOWNLOAD_DATA_PROFILE)
#define LVD_ATTACH_FLAGS_DD_RO                                              \
  (LVD_ATTACH_FLAGS_DD_RW | LVD_ATTACH_FLAG_RO_VARIANT)
#define LVD_SECTOR_SIZE_EXFAT 512u
#define LVD_SECTOR_SIZE_UFS 4096u
#define LVD_SECTOR_SIZE_PFS 4096u
#define LVD_SECONDARY_UNIT_IMAGE_IO 0x10000u
#define MD_SECTOR_SIZE_EXFAT 512u
#define MD_SECTOR_SIZE_UFS 4096u
_Static_assert(LVD_SECONDARY_UNIT_IMAGE_IO % LVD_SECTOR_SIZE_EXFAT == 0,
               "exFAT LVD geometry must be aligned");
_Static_assert(LVD_SECONDARY_UNIT_IMAGE_IO % LVD_SECTOR_SIZE_UFS == 0,
               "UFS LVD geometry must be aligned");
_Static_assert(LVD_SECONDARY_UNIT_IMAGE_IO % LVD_SECTOR_SIZE_PFS == 0,
               "PFS LVD geometry must be aligned");
#define LVD_ATTACH_IMAGE_TYPE_SINGLE 0
#define LVD_ATTACH_IMAGE_TYPE_SAVE_DATA 5
#define PFS_NESTED_OUTER_IMG_TYPE 0x02u
#define PFS_NESTED_INNER_IMG_TYPE 0x82u
#define LVD_ATTACH_LAYER_COUNT 1
#define LVD_ENTRY_TYPE_FILE 1
#define LVD_ENTRY_TYPE_SPECIAL 2
#define LVD_ENTRY_FLAG_NO_BITMAP 0x1
#define LVD_NODE_WAIT_US 100000
#define LVD_NODE_WAIT_RETRIES 100
// PFS nmount values used by ShadowMount. Other firmware flows also expose
// transaction_pfs/ppr_pfs, SD/GD key modes and system budget IDs.
#define DEVPFS_BUDGET_GAME "game"
#define DEVPFS_BUDGET_SYSTEM "system"
#define DEVPFS_MKEYMODE_SD "SD"
#define DEVPFS_MKEYMODE_GD "GD"
#define DEVPFS_MKEYMODE_AC "AC"
#define PFS_MOUNT_BUDGET_ID DEVPFS_BUDGET_GAME
#define PFS_MOUNT_MKEYMODE DEVPFS_MKEYMODE_AC
#define PFS_MOUNT_SIGVERIFY 0
#define PFS_MOUNT_PLAYGO 0
#define PFS_MOUNT_DISC 0
#define PFS_GDDR5_CACHE_IOCTL 0x80209168

// 0x80209168 is an IOC_IN request with a 0x20-byte payload. Despite the
// userland "GDDR5 cache" name, the PFS vnode handler uses it to populate two
// metadata caches for a nested-image backing file: its PFSC offset table and,
// optionally, the containing signed PFS mount's full ICV table. A zero unit
// count asks the kernel to size the corresponding cache automatically.
typedef struct pfs_gddr5_cache_request {
  uint8_t cache_cmp_offsets;
  uint8_t reserved01[7];
  uint64_t cmp_cache_units;
  uint8_t cache_full_icv;
  uint8_t reserved11[7];
  uint64_t icv_cache_units;
} pfs_gddr5_cache_request_t;

_Static_assert(sizeof(pfs_gddr5_cache_request_t) == 0x20,
               "PFS GDDR5 cache request must be 0x20 bytes");

// 4x64-bit PFS key encoded as 64 hex chars.
#define PFS_ZERO_EKPFS_KEY_HEX                                                \
  "0000000000000000000000000000000000000000000000000000000000000000"

#endif
