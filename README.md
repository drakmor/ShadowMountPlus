# ShadowMountPlus (PS5)

**Repository:** https://github.com/drakmor/shadowMountPlus

**Discord:** https://discord.gg/x2Ppvzwjhm

**Warning! Mounting images can cause shutdown problems and data corruption on internal drives! This depends on many factors, but is more common with older firmware versions. Please take this into account when testing.**

**ShadowMountPlus** is a fully automated, background "Auto-Mounter" payload for Jailbroken PlayStation 5 consoles. It streamlines the game mounting process by eliminating the need for manual configuration or external tools (such as DumpRunner or Itemzflow). ShadowMountPlus automatically detects, mounts, and installs game dumps from both **internal and external storage**.

**Compatibility:** Supports all Jailbroken PS5 firmwares running **[Kstuff-lite v1.07+](https://github.com/EchoStretch/kstuff-lite)**.


## 💜 Support Development

 If you want to support this project, you can donate
 - USDT (TRC-20):  **`TKaUGEwMm9KBXzEoiaaKYBX2yCHAKASW3p`**
 - USDT (ERC-20):  **`0x313dD245dBA957A5560618eA882d08e66aaFb430`**
 - USDC (Solana):  **`5kv7j2RbUGaSP1kU1cZWj9jHH7d6rfvxmK6YXTYbH4um`**

## Current image support

`PFS support is experimental.`

| Extension | Mounted FS | Attach backend | Status |
| --- | --- | --- | --- |
| `.ffpkg` | `ufs` | `LVD` or `MD` (configurable) | Recommended |
| `.exfat` | `exfatfs` | `LVD` or `MD` (configurable) | Compatibility / external-drive-only titles |
| `.ffpfs` | `pfs` (direct) | `LVD` | Experimental direct game layout |
| `.ffpfsc` | `pfs` container | `LVD` | Experimental nested-image container |

Notes:
- Backend, read-only mode, and sector size can be configured via `/data/shadowmount/config.ini`.
- Debug logging is enabled by default (`debug=1`) and writes to console plus `/data/shadowmount/debug.log` (set `debug=0` to disable).
- **UFS (`.ffpkg`) is the recommended image format for normal use.**
- **Use exFAT (`.exfat`) only for titles that need external-drive-style compatibility.**
- **When building exFAT images manually, keep the cluster size at `64 KB`; smaller clusters can reduce performance.**

## Recommended FS choice

- Prefer **UFS (`.ffpkg`)** in most cases: this is the recommended default image format for ShadowMountPlus.
- Use **exFAT (`.exfat`)** only for games that do not work correctly unless they are handled like external-drive content.
- If you create an **exFAT (`.exfat`)** image manually, use a **`64 KB` cluster size**. Smaller clusters can cause a noticeable performance loss.
- Use **compressed PFS containers (`.ffpfsc`)** if you want to experiment with compressed formats; prefer the recommended `.ffpfsc` container layout with a nested exFAT image.

## Runtime config (`/data/shadowmount/config.ini`)

This file is optional. If it does not exist, ShadowMountPlus creates it from the bundled `config.ini.example` template on startup and uses built-in defaults until you uncomment overrides.

Supported keys (all optional):
- `debug=1|0` (`1` enables `log_debug` output to console + `/data/shadowmount/debug.log`; default is `1`)
- `quiet_mode=1|0` (`1` suppresses plain informational popups but keeps rich toasts; default is `0`)
- `mount_read_only=1|0` (default: `1`)
- `force_mount=1|0` (mounting even damaged file systems; default: `0`)
- `app_install_all=1|0` (`1` stages new titles and submits them through `sceAppInstUtilAppInstallAll`; default: `0` on FW below `12.00`, forced `1` on FW `12.00+`)
- `image_ro=<image_filename>` (repeatable; force read-only mode for this image filename)
- `image_rw=<image_filename>` (repeatable; force read-write mode for this image filename)
- `image_sector=<image_filename>:<sector_size>` (repeatable; force sector size for this image filename)
- `scan_depth=<1..2>` (`1` = scan only first-level subfolders, `2` = also scan one additional nested level; default: `1`)
- `recursive_scan=1|0` (deprecated compatibility key; `1` forces `scan_depth=2`)
- `scan_interval_seconds=<1..3600>` (full scan loop interval; default: `15`)
- `stability_wait_seconds=<0..3600>` (minimum source age before processing; default: `10`)
- `exfat_backend=lvd|md` (default: `lvd`)
- `ufs_backend=lvd|md` (default: `lvd`)
- `backport_fakelib=1|0` (`1` mounts sandbox `fakelib` overlays for running games; default: `1`)
- `global_fakelib=1|0` (`1` enables the global fakelib overlay when the folder exists; default: `1`)
- `global_fakelib_path=<absolute_path>` (global fakelib folder; default: `/data/shadowmount/fakelib`)
- `global_fakelib_priority=game|global` (overlay priority when both global and game fakelib exist; default: `game`)
- `global_fakelib_exclude=<TITLE_ID>` (repeatable; disables the global fakelib overlay for matching titles)
- `kstuff_game_auto_toggle=1|0` (`1` pauses kstuff after tracked game launches and resumes it on stop; default: `1`)
- `kstuff_crash_detection=1|0` (`1` enables crash monitoring and pause-delay autotune updates; default: `1`)
- `kstuff_pause_delay_image_seconds=<0..3600>` (delay before pausing kstuff for image-backed launches; default: `25`)
- `kstuff_pause_delay_direct_seconds=<0..3600>` (delay before pausing kstuff for direct/non-image launches; default: `15`)
- `kstuff_no_pause=<TITLE_ID>` (repeatable; keeps kstuff enabled for matching titles)
- `kstuff_delay=<TITLE_ID>:<0..3600>` (repeatable; per-title pause delay override, last matching rule wins)
- `/data/shadowmount/autotune.ini` may also provide per-title pause-delay overrides with highest priority:
  - `kstuff_delay=<TITLE_ID>:<0..3600>`
  - `<TITLE_ID>=<0..3600>`
  - `image_sector=<image_filename>:<sector_size>`
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
image_rw=PPSA1234-my-image.ffpfs
image_rw=MYGame 123.exfat
image_ro=legacy_dump.ffpkg
image_sector=MYGame 123.exfat:65536
```

Per-image sector override behavior:
- Match is done by image file name (without path).
- `image_sector` in `/data/shadowmount/autotune.ini` has the highest priority for matching image files.
- If no per-image rule matches, the backend-specific global sector size defaults are used.
- When image validation fails because the mounted file-system cluster size is smaller than the selected device sector size, ShadowMountPlus writes `image_sector=<image_filename>:<cluster_size>` into `/data/shadowmount/autotune.ini` and asks you to try mounting again.

Scan path behavior:
- If at least one `scanpath=...` is present, only those custom paths are used.
- `/mnt/shadowmnt/pfsc` and `/mnt/shadowmnt` are always added automatically, even with custom paths.
- With `scan_depth=1` (default), only first-level subfolders are checked.
- With `scan_depth=2`, one additional nested level is checked.
- If `recursive_scan=1` is set, ShadowMount+ forces `scan_depth=2`.
- Full scan loop runs every `scan_interval_seconds` (default: `15`).
- Sources newer than `stability_wait_seconds` are deferred until stable (default: `10`).
- Direct folder installs use `<game>/sce_sys` for this check; image and backport sources use the target path itself.

Backport overlay behavior:
- For each `scanpath`, use:
  - `<scanpath>/backports/<TITLE_ID>/`
- The `backports` folder is ignored during normal game scanning.
- A backport is applied automatically to the matching mounted game from any configured scan path.
- If multiple scan paths provide the same title backport, the game's own scan path wins; otherwise scan path order is used.
- If `/mnt/sandbox/<TITLE_ID>_XXX/app0/fakelib2` exists while the game is running, ShadowMount+ mounts it into that game's sandbox `common/lib`; otherwise it falls back to `app0/fakelib`.
- If `global_fakelib=1` and `global_fakelib_path` exists as a directory, ShadowMount+ also mounts that folder into the same sandbox `common/lib`.
- When both global and per-game fakelib exist, the default gives the game's own `fakelib` priority by mounting `/data/shadowmount/fakelib` first and then the game-specific fakelib.
- `global_fakelib_priority=global` reverses that priority.
- Use repeatable `global_fakelib_exclude=<TITLE_ID>` entries to skip the global fakelib for specific games without disabling per-game fakelib.
- `backport_fakelib=0` disables the sandbox `fakelib` watcher, including global fakelib.
- For `backport_fakelib` to work correctly, the standalone `BackPork` payload must be disabled. Running both at the same time will conflict.

Kstuff game lifecycle behavior:
- When `kstuff_game_auto_toggle=1`, ShadowMount watches game `exec/exit` events in the background.
- Image-backed launches use `kstuff_pause_delay_image_seconds`; direct/non-image launches use `kstuff_pause_delay_direct_seconds`.
- `kstuff_crash_detection=0` disables crash monitoring and the automatic pause-delay tuning logic, while leaving normal kstuff auto-pause/auto-resume behavior intact.
- `kstuff_no_pause` skips auto-pause entirely for matching title IDs.
- `kstuff_delay` overrides the pause delay for matching title IDs, regardless of image/direct launch type.
- `/data/shadowmount/autotune.ini` overrides both `config.ini` and `autopause.txt` for matching title IDs.
- `/data/shadowmount/autotune.ini` also overrides `image_sector` rules from `config.ini` for matching image file names.
- A game source folder may optionally contain `autopause.txt`; it is read once at launch time.
- Priority order is: `autotune.ini` -> `kstuff_delay` from `config.ini` -> `autopause.txt` -> global direct/image default delay.
- If `autopause.txt` contains only a number, that value is used for direct launches and doubled for image-backed launches.
- `autopause.txt` may also use:
  - `direct=<seconds>`
  - `image=<seconds>`
- If both kinds of rule target the same title, `kstuff_no_pause` takes priority.
- When crash monitoring detects an app crash before kstuff was paused, ShadowMountPlus only notifies that the app crashed and kstuff is not to blame.
- When crash monitoring detects an app crash within 2 minutes after kstuff auto-pause, ShadowMountPlus doubles the applied pause delay for that title and upserts it into `/data/shadowmount/autotune.ini` (up to `3600` seconds), then prompts you to launch the game again.
- When the last tracked game stops, ShadowMount immediately enables kstuff again if it was the component that disabled it.


Validation:
- See `config.ini.example` for a ready-to-use template.

## Mount point naming

Image mountpoints are created under:

`/mnt/shadowmnt/<image_name>_<hash>`

PFSC container mountpoints are created under:

`/mnt/shadowmnt/pfsc/<image_name>_<hash>`

Image layout requirement (`.ffpkg`, `.exfat`, `.ffpfs`):
- Game files must be placed at the image root.
- Do not add an extra top-level folder inside the image.
- Valid example: `/sce_sys/param.json` exists directly from image root.
- Invalid example: `/GAME_FOLDER/sce_sys/param.json` (extra nesting level).

PFSC container layout requirement (`.ffpfsc`):
- Do not place game files directly in the container root.
- Place supported nested image files inside the container; ShadowMountPlus mounts those nested images and scans them for the game.
- A nested `pfs_image.dat` file is treated as a PFS image only when it is located inside a mounted PFSC container.
- `.ffpfsc` uses the nested outer PFS profile (`img_type=0x02`); `.ffpfs` and `pfs_image.dat` files mounted from inside it use the nested inner profile (`img_type=0x82`). Signature verification and GDDR5 cache setup are kept in code but currently disabled.

## Compressed PFS Containers (`.ffpfsc`)

Compressed PFS mode is intended only for nested images. During packing, data is
zero-padded to a `64 KB` sector boundary, so the compressed PFS should be used
as an outer container for another image rather than as a direct game-file
layout.

Recommended layouts:
- exFAT image inside compressed PFS.
- Uncompressed PFS image inside compressed PFS.

MkPFS uses zLib compression. Decompression is hardware-assisted, but throughput
is limited to roughly `150-250 MB/s`. This is about one third of the speed of an
external USB drive, FFPKG/exFAT images, or about one tenth of the internal drive
speed. Keep this in mind when choosing which games to pack. Games that read large
amounts of data or stream textures continuously may stutter.

Use the official [PSBrew/MkPFS](https://github.com/PSBrew/MkPFS) tool to pack
PFS images.


### Packing an exFAT image into a Compressed PFS _(exfat->ffpfsc)_ (Recommended)

First create an exFAT image using one of the methods from
`Creating an exFAT image`. The nested exFAT image name must keep the `.exfat`
extension, for example `PPSA12345.exfat`.

Linux:

```bash
chmod +x mkexfat.sh
./mkexfat.sh ./PPSA12345-app ./PPSA12345.exfat
```

Windows:

```cmd
make_image.bat "C:\images\PPSA12345.exfat" "C:\payload\PPSA12345-app"
```

The exFAT image must contain the game files at the image root, without an extra
top-level folder.

Then pack the exFAT image into a compressed PFS container (.ffpfsc):

```bash
# Install/Update mkpfs using pip
python -m pip install -U "mkpfs"

# Convert the .exfat image into a .ffpfsc compressed PFS container. 
python -m mkpfs pack file --verify './PPSA12345.exfat' './PPSA12345.ffpfsc'

# OPTIONAL: After successful packing, the temporary exFAT image can be removed
rm './PPSA12345.exfat'
```


### Packing an uncompressed PFS image into compressed PFS _(folder->dat->ffpfsc)_

First create an uncompressed nested PFS image and then compress it on a second layer.

```bash
# Install/Update mkpfs using pip
python -m pip install -U "mkpfs"

# Create an uncompressed PFS image with the nested image name `pfs_image.dat`.
python -m mkpfs pack folder --verify --no-compress --no-adjust-output-file-extension './BREW1234-app' './pfs_image.dat'

# Pack the pfs_image.dat into a compressed PFS container with the desired output name.
# IMPORTANT: The internal file name MUST be exactly `pfs_image.dat`.
python -m mkpfs pack file --verify './pfs_image.dat' './BREW1234.ffpfsc'

# Remove the temporary inner file.
rm './pfs_image.dat'
```

## Direct Compressed PFS images (`.ffpfs`)

Using a direct compressed PFS image, without the nested container layer, is
still experimental and might not work for several games.

Unlike `.ffpfsc`, this format mounts as a normal image and expects the game
files at the image root. It does not act as a container for nested images.

There are limits related to file size and count. If you want a more stable
compressed format, prefer the recommended `.ffpfsc` container layout with a
nested exFAT image.

```bash
# Install/Update mkpfs using pip
python -m pip install -U "mkpfs"

# Create a direct .ffpfs image from a game folder (single-pass, experimental)
python -m mkpfs pack folder --verify './BREW1234-app/' './BREW1234.ffpfs'
```


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
- `/mnt/shadowmnt/pfsc` (mounted PFSC container scan)
- `/mnt/shadowmnt` (mounted image content scan)

You can override scan roots with `scanpath=...` entries in `/data/shadowmount/config.ini`.

## Manual install list

For games that should not live under the normal scan paths, ShadowMountPlus also
checks:

`/data/shadowmount/manual.lst`

Add one source per line:
- Path to a game folder, where `sce_sys/param.json` exists inside that folder.
- Path to a supported image file: `.ffpkg`, `.exfat`, `.ffpfs`, or `.ffpfsc`.
- Empty lines and lines starting with `#` are ignored.

Example:
```text
/mnt/usb0/MyGames/PPSA12345
/mnt/usb0/images/PPSA54321.ffpkg
# /mnt/usb0/disabled/PPSA00000
```

`manual.lst` is watched for changes. When you add a new line, ShadowMountPlus
rescans shortly after the write, mounts images when needed, and installs the
game through the same pipeline as normal scan path discoveries.

Manual install state is tracked in:

`/data/shadowmount/manual.status`

This status file is managed by ShadowMountPlus. If a manually installed game is
later removed by the user and disappears from `app.db`, ShadowMountPlus marks it
as deleted in `manual.status` and removes the matching source line from
`manual.lst` so it is not installed again automatically. If you later add the
same source path back to `manual.lst`, it will be processed again and the status
will be updated after installation.

You can use **Dump Installer** to install or prepare game dumps for this manual
workflow, then add the resulting game folder path or image file path to
`manual.lst`.

Recommended folder structure:
- Default mode (`scan_depth=1`):
  - `/data/homebrew/<TITLE_ID>/`
  - `/data/etaHEN/games/<TITLE_ID>/`
  - `/data/homebrew/backports/<TITLE_ID>/`
  - `/data/etaHEN/games/backports/<TITLE_ID>/`
   
- Nested mode (`scan_depth=2`):
  - `/data/homebrew/PS5/<AnyFolder>/<TITLE_ID>/`
  - `/mnt/ext0/etaHEN/games/<Collection>/<TITLE_ID>/`
  - `/mnt/ext0/etaHEN/games/backports/<TITLE_ID>/`


## Creating an exFAT image

Recommended only for titles that need external-drive-style compatibility. For general use, prefer `.ffpkg`.

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
  - For manual exFAT builds, keep the cluster size at `64 KB` or you may lose performance.
  - Automatically selects exFAT cluster profile:
  - Large-file profile: `64K`
  - Small/mixed-file profile: `32K`

Windows:
- Recommended: use `make_image.bat` (wrapper for `New-OsfExfatImage.ps1` + OSFMount).
- Requirements:
  - Install OSFMount: https://www.osforensics.com/tools/mount-disk-images.html.
  - Keep `make_image.bat` and `New-OsfExfatImage.ps1` in the same folder.
  - Run `cmd.exe` as Administrator.
  - If you build an exFAT image manually instead of using the script, keep the cluster size at `64 KB` or you may lose performance.
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
  - `newfs -O 2 -b 65536 -f 65536 -m 0 -S 4096`
  - `mkufs2.sh` keeps this fixed block/fragment/sector profile and auto-tunes `-i` based on source file/directory count.
  - Rough manual `-i` estimate for manual builds:
  - `target_inodes ~= file_count + dir_count + 2048`
  - `bytes_per_inode ~= image_size_bytes / target_inodes`
  - Round `bytes_per_inode` down to a multiple of `4096`, then keep it in the practical range `65536..262144`.
  - Practical rule of thumb: use `262144` for normal game dumps, `131072` for tens of thousands of files, and `65536` only for very file-dense images.
  - Example: for an `8 GiB` image with `60000` files and `4000` directories, `-i ~= 8*1024^3 / (60000 + 4000 + 2048) ~= 130312`, so use `-i 131072`.

Windows:
- You can create UFS2 images with **UFS2Tool** https://github.com/SvenGDK/UFS2Tool.
- Example:
  - `UFS2Tool.exe newfs -O 2 -b 65536 -f 65536 -m 0 -S 4096 -i 262144 -D ./APPXXXX ./PPSA12345.ffpkg`
  - For manual builds, use `-i 262144` as the baseline and lower it for images with many small files.


## Installation and usage


### Method 1: Manual Payload Injection (Port 9021)
Use a payload sender (such as NetCat GUI or a web-based loader) to send the files to **Port 9021**.

1.  Send `shadowmountplus.elf`.
2.  Wait for the notification: *"ShadowMount+"*.

### Method 2: PLK Autoloader (Recommended)
Add ShadowMountPlus to your `autoload.txt` for **plk-autoloader** to ensure it starts automatically on every boot.

**Sample Configuration:**
```ini
shadowmountplus.elf
!3000
kstuff.elf
```

---

## Troubleshooting

If a game is not mounted:
- Debug log is enabled by default; if disabled, set `debug=1` in `/data/shadowmount/config.ini`.
- Check `/data/shadowmount/debug.log` and system notifications from ShadowMount+.
- Verify scan roots:
  - if `scanpath=...` is set, only these paths are scanned;
  - `/mnt/shadowmnt/pfsc` and `/mnt/shadowmnt` are always scanned.
- Verify scan depth:
  - `scan_depth=1` scans only first-level subfolders;
  - `scan_depth=2` scans one additional nested level;
  - `recursive_scan=1` is treated as deprecated compatibility mode and forces `scan_depth=2`.
- If logs show `source not stable yet`, adjust `stability_wait_seconds` (or wait for source copy/write to finish).
- Verify game structure:
  - folder game: `<GAME_DIR>/sce_sys/param.json`;
  - image game (`.ffpkg` / `.exfat` / `.ffpfs`): `sce_sys/param.json` must be at image root (no extra top-level folder);
  - PFSC container (`.ffpfsc`): nested supported image files are scanned; direct game files inside the container are ignored.
- If you see `missing/invalid param.json` for an image, check via FTP that files are present under `/mnt/shadowmnt/<image_name>_<hash>/` and include `sce_sys/param.json`.
- If you see image mount failure, check image integrity and filesystem type (`.ffpkg`=UFS, `.exfat`=exFAT, `.ffpfs`=PFS, `.ffpfsc`=PFS container).
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
    * VoidWhisper for ShadowMount
    * BestPig for BackPort
    * EchoStretch for kstuff-toggle and etc
    * Gezine
    * earthonion
    * LightningMods
    * RenanGBarreto for his excellent https://github.com/PSBrew/MkPFS
    * john-tornblom for SDK
    * PS5 R&D Community
