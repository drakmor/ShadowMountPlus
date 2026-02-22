# ShadowMountPlus (PS5)

**Version:** `1.5beta6`

**Repository:** https://github.com/drakmor/shadowMountPlus

**Warning! Mounting images can cause shutdown problems and data corruption on internal drives! This depends on many factors, but is more common with older firmware versions. Please take this into account when testing.**


**ShadowMountPlus** is a fully automated, background "Auto-Mounter" payload for Jailbroken PlayStation 5 consoles. It streamlines the game mounting process by eliminating the need for manual configuration or external tools (such as DumpRunner or Itemzflow). ShadowMountPlus automatically detects, mounts, and installs game dumps from both **internal and external storage**.


**Compatibility:** Supports all Jailbroken PS5 firmwares running **Kstuff v1.6.7**.


## Current image support

`PFS support is experimental.`

| Extension | Mounted FS | Attach backend | Status |
| --- | --- | --- | --- |
| `.exfat` | `exfatfs` | `LVD` or `MD` (configurable) | Optimal |
| `.ffpkg` | `ufs` | `LVD` or `MD` (configurable) | Legacy |
| `.ffpfs` | `pfs` | `LVD` | Experimental |

Notes:
- Backend, read-only mode, and sector size can be configured via `/data/shadowmount/config.ini`.
- Debug logging is enabled by default (`debug=1`) and writes to console plus `/data/shadowmount/debug.log` (set `debug=0` to disable).
- **exFAT is the preferred image filesystem, including on 4.xx firmware, with no known reboot/shutdown issues in typical use.**

## Recommended FS choice

- Prefer **exFAT (`.exfat`)** in most cases: it is generally more performant, case-insensitive, and does not have reboot/shutdown issues in typical use.
- Use **UFS (`.ffpkg`)** only when specifically needed for compatibility with your game/setup.

## Runtime config (`/data/shadowmount/config.ini`)

This file is optional. If it does not exist, built-in defaults are used.

Supported keys (all optional):
- `debug=1|0` (`1` enables `log_debug` output to console + `/data/shadowmount/debug.log`; default is `1`)
- `mount_read_only=1|0` (default: `1`)
- `image_ro=<image_filename>` (repeatable; force read-only mode for this image filename)
- `image_rw=<image_filename>` (repeatable; force read-write mode for this image filename)
- `recursive_scan=1|0` (`0` = scan only first-level subfolders, `1` = recursive scan without depth limit; default: `0`)
- `scan_interval_seconds=<1..3600>` (full scan loop interval; default: `10`)
- `stability_wait_seconds=<0..3600>` (minimum source age before processing; default: `10`)
- `exfat_backend=lvd|md` (default: `lvd`)
- `ufs_backend=lvd|md` (default: `lvd`)
- `scanpath=<absolute_path>` (can be repeated on multiple lines; default: built-in scan path list below)
- `lvd_exfat_sector_size=<value>` (default: `512`)
- `lvd_ufs_sector_size=<value>` (default: `4096`)
- `lvd_pfs_sector_size=<value>` (default: `32768`)
- `md_exfat_sector_size=<value>` (default: `512`)
- `md_ufs_sector_size=<value>` (default: `512`)

Per-image mode override behavior:
- Match is done by image file name (without path).
- File names with spaces are supported.
- If multiple rules target the same file name, the last one in config wins.
- If no rule matches, global `mount_read_only` is used.
- Example:
```ini
mount_read_only=1
image_rw=PPSA08766-Cocoon-01.004.000.ffpfs
image_rw=DIRT 5.exfat
image_ro=legacy_dump.ffpkg
```

Scan path behavior:
- If at least one `scanpath=...` is present, only those custom paths are used.
- `/data/imgmnt` is always added automatically, even with custom paths.
- With `recursive_scan=0` (default), only first-level subfolders are checked.
- With `recursive_scan=1`, subfolders are scanned recursively.
- Full scan loop runs every `scan_interval_seconds` (default: `10`).
- Sources newer than `stability_wait_seconds` are deferred until stable (default: `10`).

Validation:
- See `config.ini.example` for a ready-to-use template.

## Mount point naming

Image mountpoints are created under:

`/data/imgmnt/<image_name>-<fs_suffix>`

Image layout requirement (`.ffpkg`, `.exfat`, `.ffpfs`):
- Game files must be placed at the image root.
- Do not add an extra top-level folder inside the image.
- Valid example: `/sce_sys/param.json` exists directly from image root.
- Invalid example: `/GAME_FOLDER/sce_sys/param.json` (extra nesting level).

## Scan paths

Default scan locations:
- `/data/homebrew`
- `/data/etaHEN/games`
- `/mnt/ext0/homebrew`
- `/mnt/ext0/etaHEN/games`
- `/mnt/ext1/homebrew`
- `/mnt/ext1/etaHEN/games`
- `/mnt/usb0/homebrew` .. `/mnt/usb7/homebrew`
- `/mnt/usb0/etaHEN/games` .. `/mnt/usb7/etaHEN/games`
- `/mnt/usb0` .. `/mnt/usb7`
- `/mnt/ext0`
- `/mnt/ext1`
- `/data/imgmnt` (mounted image content scan)

You can override scan roots with `scanpath=...` entries in `/data/shadowmount/config.ini`.

Recommended folder structure:
- Default mode (`recursive_scan=0`):
  - `/data/homebrew/<TITLE_ID>/`
  - `/data/etaHEN/games/<TITLE_ID>/`
   
- Recursive mode (`recursive_scan=1`):
  - `/data/homebrew/PS5/<AnyFolder>/<TITLE_ID>/`
  - `/mnt/ext0/etaHEN/games/<Collection>/<TITLE_ID>/`


## Creating an exFAT image

Linux (Ubuntu/Debian):
- Required components installation:
  - `sudo apt-get update && sudo apt-get install -y exfatprogs exfat-fuse fuse3 rsync`
- Script: `mkexfat.sh`
- Usage: `./mkexfat.sh <game_root_dir> [output_file]`
- Example:
  - `chmod +x mkexfat.sh`
  - `./mkexfat.sh ./APPXXXX ./PPSA12345.exfat`
- Notes:
  - Source folder must be the game root and contain `eboot.bin`.
  - Auto-calculates image size using rounded file allocation + metadata + safety margin.
  - Automatically selects exFAT cluster profile:
  - Large-file profile: `64K`
  - Small/mixed-file profile: `32K`

Windows:
- Recommended: use `make_image.bat` (wrapper for `New-OsfExfatImage.ps1` + OSFMount).
- Requirements:
  - Install OSFMount: https://www.osforensics.com/tools/mount-disk-images.html.
  - Keep `make_image.bat` and `New-OsfExfatImage.ps1` in the same folder.
  - Run `cmd.exe` as Administrator.
- Usage:
  - `make_image.bat "C:\images\game.exfat" "C:\payload\APPXXXX"`
- Behavior:
  - Auto-sizes the image to fit source content.
  - Source folder must be the game root and contain `eboot.bin`.
  - Formats and copies source folder contents into image root.
- Optional (fixed size): run PowerShell script directly:
  - `powershell.exe -ExecutionPolicy Bypass -File .\New-OsfExfatImage.ps1 -ImagePath "C:\images\game.exfat" -SourceDir "C:\payload\APPXXXX" -Size 8G -ForceOverwrite`

## Creating a UFS2 image (`.ffpkg`)

FreeBSD:
- Script: `mkufs2.sh`
- Usage: `./mkufs2.sh <game_root_dir> [output_file]`
- Example:
  - `chmod +x mkufs2.sh`
  - `./mkufs2.sh ./APPXXXX ./PPSA12345.ffpkg`
- Notes:
  - Source folder must be the game root and contain `eboot.bin`.
  - The script auto-calculates image size using rounded file allocation + metadata + safety margin.
  - Recommended `newfs` parameters for UFS2:
  - Large-file profile: `newfs -O 2 -b 65536 -f 4096 -m 0 -i 262144`
  - Small/mixed-file profile: `newfs -O 2 -b 32768 -f 4096 -m 0 -i 262144`
  - `mkufs2.sh` selects between these two profiles automatically (based on average file size).

Windows:
- You can create UFS2 images with **UFS2Tool** https://github.com/SvenGDK/UFS2Tool.
- Example:
  - `UFS2Tool.exe newfs  -O 2 -b 32768 -f 4096 -m 0 -i 262144 -D ./APPXXXX ./PPSA12345.ffpkg`


## Installation and usage


### Method 1: Manual Payload Injection (Port 9021)
Use a payload sender (such as NetCat GUI or a web-based loader) to send the files to **Port 9021**.

1.  Send `notify.elf` (Optional).
    * *Only send this if you want graphical pop-ups. Skip if you prefer standard notifications.*
2.  Send `shadowmountplus.elf`.
3.  Wait for the notification: *"ShadowMount+"*.

### Method 2: PLK Autoloader (Recommended)
Add ShadowMountPlus to your `autoload.txt` for **plk-autoloader** to ensure it starts automatically on every boot.

**Sample Configuration:**
```ini
!1000
kstuff.elf
!1000
notify.elf  ; Optional - Remove this line if you do not want Rich Toasts
!1000
shadowmountplus.elf
```

---

## Troubleshooting

If a game is not mounted:
- Debug log is enabled by default; if disabled, set `debug=1` in `/data/shadowmount/config.ini`.
- Check `/data/shadowmount/debug.log` and system notifications from ShadowMount+.
- Verify scan roots:
  - if `scanpath=...` is set, only these paths are scanned;
  - `/data/imgmnt` is always scanned.
- Verify scan depth:
  - `recursive_scan=0` scans only first-level subfolders;
  - `recursive_scan=1` scans recursively.
- If logs show `source not stable yet`, adjust `stability_wait_seconds` (or wait for source copy/write to finish).
- Verify game structure:
  - folder game: `<GAME_DIR>/sce_sys/param.json`;
  - image game (`.ffpkg` / `.exfat` / `.ffpfs`): `sce_sys/param.json` must be at image root (no extra top-level folder).
- If you see `missing/invalid param.json` for an image, check via FTP that `/data/imgmnt/<TITLE_ID>/` contains full game files and `sce_sys/param.json`.
- If you see image mount failure, check image integrity and filesystem type (`.ffpkg`=UFS, `.exfat`=exFAT, `.ffpfs`=PFS).
- If you see duplicate titleId notification, keep only one source per `<TITLE_ID>`.

If a game is mounted but does not start:
- Check registration notifications (`Register failed ...`).
- If the game is not registered, try removing its launcher icon and removing it from Itemzflow.
- If this does not help, remove the game data from system settings and retry (this will delete game saves).

## ⚠️ Notes
* **First Run:** If you have a large library, the initial scan may take a few seconds to register all titles.
* **Large Games:** For massive games (100GB+), allow a few extra seconds for the system to verify file integrity before the "Installed" notification appears.

## Credits
* **Drakmor** - Evolution of ShadowMount to ShadowMountPlus

* **Special Thanks:**
    * VoidWhisper for ShadowMount [![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/voidwhisper)
    * EchoStretch
    * Gezine
    * earthonion
    * LightningMods
    * john-tornblom
    * PS5 R&D Community
