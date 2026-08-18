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
| `.ffpfs` | `pfs` | `LVD` | Experimental |
| `.ffpfsc` | `pfs` container | `LVD` | Experimental container for nested images |

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

## Runtime config (`/data/shadowmount/config.ini`)

This file is optional. If it does not exist, ShadowMountPlus creates it from the bundled `config.ini.example` template on startup and uses built-in defaults until you uncomment overrides.

Supported keys (all optional):
- `debug=1|0` (`1` enables `log_debug` output to console + `/data/shadowmount/debug.log`; default is `1`)
- `quiet_mode=1|0` (`1` suppresses plain informational popups but keeps rich toasts; default is `0`)
- `language=auto|<locale>` (notification language; `auto` uses the console system language, e.g. `en-US`, `ru-RU`; default: `auto`)
- `api_bind_address=<IPv4>` (HTTP/JSON API bind address; default: `127.0.0.1`; use `0.0.0.0` for network access)
- `api_port=<1..65535>` (HTTP/JSON API port; default: `10101`)
- `mount_read_only=1|0` (default: `1`)
- `force_mount=1|0` (mounting even damaged file systems; default: `0`)
- `persistent_image_mounts=1|0` (`1` keeps discovered images mounted between game launches; default: `0`)
- `app_install_all=1|0` (`1` stages new titles and submits them through the stock batch `sceAppInstUtilAppInstallAll`; default: `0` on every firmware. On FW `12.00+`, the default per-title path is provided by the SceShellCore TitleDir bridge.)
- `auto_remove_missing_games=1|0` (`1` automatically removes games from the system library when their source is no longer available; default: `0`)
- `auto_remove_games_with_dlc=1|0` (`1` allows automatic removal of games that have installed DLC; default: `0`)
- `auto_remove_missing_delay_seconds=<1..86400>` (how long a source must remain unavailable before removal; default: `300`)
- `image_ro=<image_filename>` (repeatable; force read-only mode for this image filename)
- `image_rw=<image_filename>` (repeatable; force read-write mode for this image filename)
- `image_sector=<image_filename>:<sector_size>` (repeatable; force the logical device sector size, not the filesystem cluster size)
- `scan_depth=<1..2>` (`1` = scan only first-level subfolders, `2` = also scan one additional nested level; default: `1`)
- `recursive_scan=1|0` (deprecated compatibility key; `1` forces `scan_depth=2`)
- `scan_interval_seconds=<1..3600>` (full scan loop interval; default: `15`)
- `stability_wait_seconds=<0..3600>` (minimum source age before processing; default: `10`)
- `exfat_backend=lvd|md` (default: `lvd`)
- `ufs_backend=lvd|md` (default: `lvd`)
- `legacy_mount_ufs=1|0` (`.ffpkg`; `1` selects the version 1.6 mount profile; default: `1`)
- `legacy_mount_exfat=1|0` (`.exfat`; `1` selects the version 1.6 mount profile; default: `1`)
- `legacy_mount_pfs=1|0` (`.ffpfs` and nested `pfs_image.dat`; `1` selects the version 1.6 mount profile; default: `1`)
- `legacy_mount_pfsc=1|0` (outer `.ffpfsc`; `1` selects the version 1.6 mount profile; default: `1`)
- `legacy_gddr5_cache=1|0` (request the PFS compressed-offset cache for nested images in legacy mode; default: `0`; optimized mode always requests it)
- `backport_fakelib=1|0` (`1` mounts sandbox `fakelib` overlays for running games; default: `1`)
- `update_emulators=1|0` (`1` updates all emulators with matching files in a game's own fakelib; default: `1`)
- `emulators_path=<absolute_path>` (folder containing emulator update files; default: `/data/shadowmount/emus`)
- `auto_update_ampr=1|0` (check for a new `libSceAmpr.sprx` 30 seconds after startup and every four hours; default: `0`)
- `ampr_update_url=<http_or_https_url>` (AMPR emulator download URL; default: `https://github.com/drakmor/ampr_emu/releases/latest/download/libSceAmpr.sprx`)
- `global_fakelib=1|0` (`1` enables the global fakelib overlay when the folder exists; default: `1`)
- `global_fakelib_path=<absolute_path>` (global fakelib folder; default: `/data/shadowmount/fakelib`)
- `global_fakelib_priority=game|global` (overlay priority when both global and game fakelib exist; default: `game`)
- `global_fakelib_exclude=<TITLE_ID>` (repeatable; disables the global fakelib overlay for matching titles)
- `kstuff_game_auto_toggle=1|0` (`1` pauses kstuff after tracked game launches and resumes it on stop; default: `1`)
- `kstuff_crash_detection=1|0` (`1` enables crash monitoring and pause-delay autotune updates; default: `1`)
- `fan_target_temperature=system|<50..91>` (automatic fan-controller target in degrees Celsius, applied when a game starts; `system` leaves fan control to the console and is the default)
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
- `lvd_pfs_sector_size=<value>` (default: `4096`; the optimized profile uses a `65536`-byte LVD mapping unit)
- `md_exfat_sector_size=<value>` (default: `512`)
- `md_ufs_sector_size=<value>` (default: `512` with the legacy UFS profile, `4096` with the optimized profile)

Supported notification languages:

| Language | Locale |
| --- | --- |
| Arabic (Saudi Arabia) | `ar-SA` |
| Chinese (Simplified) | `zh-CN` |
| Chinese (Traditional) | `zh-TW` |
| Czech | `cs-CZ` |
| Danish | `da-DK` |
| Dutch | `nl-NL` |
| English (United Kingdom) | `en-GB` |
| English (United States) | `en-US` |
| Finnish | `fi-FI` |
| French (Canada) | `fr-CA` |
| French (France) | `fr-FR` |
| German | `de-DE` |
| Greek | `el-GR` |
| Hungarian | `hu-HU` |
| Indonesian | `id-ID` |
| Italian | `it-IT` |
| Japanese | `ja-JP` |
| Korean | `ko-KR` |
| Norwegian | `no-NO` |
| Polish | `pl-PL` |
| Portuguese (Brazil) | `pt-BR` |
| Portuguese (Portugal) | `pt-PT` |
| Romanian | `ro-RO` |
| Russian | `ru-RU` |
| Spanish (Mexico) | `es-MX` |
| Spanish (Spain) | `es-ES` |
| Swedish | `sv-SE` |
| Thai | `th-TH` |
| Turkish | `tr-TR` |
| Vietnamese | `vi-VN` |

The public API routes and JSON schemas are documented in [docs/socket-api.md](docs/socket-api.md)
and available as an [OpenAPI manifest](docs/openapi.yaml).

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
image_sector=legacy-512-sector.exfat:512
```

Persistent image mount behavior:
- With `persistent_image_mounts=0` (default), backing images are mounted when a game starts and released after it exits.
- With `persistent_image_mounts=1`, images discovered during scans remain mounted between launches. Per-title runtime and backport layers are still mounted only when needed.
- USB-backed images are always released during suspend and mounted again by the resume scan.
- Persistent mode consumes one backing device per mounted image layer and is intended for libraries that fit the available LVD/MD device capacity.

Per-image sector override behavior:
- Match is done by image file name (without path).
- `image_sector` in `/data/shadowmount/autotune.ini` has the highest priority for matching image files.
- If no per-image rule matches, the backend-specific global sector size defaults are used.
- When image validation fails because the mounted file-system cluster size is smaller than the selected device sector size, ShadowMountPlus writes `image_sector=<image_filename>:<cluster_size>` into `/data/shadowmount/autotune.ini` and asks you to try mounting again.
- For the exFAT LVD/BFS fast path, leave the logical sector at its default `512`. The required `65536` values are the exFAT allocation unit and the internal LVD `secondary_unit`; do not set `image_sector=...:65536` to request this mode.

Scan path behavior:
- If at least one `scanpath=...` is present, only those custom paths are used.
- `/mnt/shadowmnt/pfsc` and `/mnt/shadowmnt` are always added automatically, even with custom paths.
- With `scan_depth=1` (default), only first-level subfolders are checked.
- With `scan_depth=2`, one additional nested level is checked.
- If `recursive_scan=1` is set, ShadowMount+ forces `scan_depth=2`.
- Full scan loop runs every `scan_interval_seconds` (default: `15`).
- Sources newer than `stability_wait_seconds` are deferred until stable (default: `10`).
- With `auto_remove_missing_games=1`, a game is removed from the system library only after its source remains unavailable for `auto_remove_missing_delay_seconds`. If the source becomes available again before removal, the pending removal is cancelled.
- Direct folder installs use `<game>/sce_sys` for this check; image and backport sources use the target path itself.

Backport overlay behavior:
- For each `scanpath`, use:
  - `<scanpath>/backports/<TITLE_ID>/`
- The `backports` folder is ignored during normal game scanning.
- A backport is applied automatically to the matching mounted game from any configured scan path.
- If multiple scan paths provide the same title backport, the game's own scan path wins; otherwise scan path order is used.
- ShadowMount+ checks the selected backport for `fakelib2` and then `fakelib`; if neither exists, it uses only `fakelib` from the original game source. The selected directory is mounted into the running game's sandbox `common/lib`.
- If `update_emulators=1`, matching files from `emulators_path` replace files in the selected game fakelib. The cache is refreshed when its sources change and expires after seven days without a game launch.
- With `auto_update_ampr=1`, ShadowMount+ checks for AMPR updates 30 seconds after startup and every four hours. It downloads a missing or newer emulator and displays a notification after a successful update.
- The backport notification adds `Emulators updated` when emulator files are updated for the launched game.
- If `global_fakelib=1` and `global_fakelib_path` exists as a directory, ShadowMount+ also mounts that folder into the same sandbox `common/lib`.
- When both global and per-game fakelib exist, the default gives the game's own `fakelib` priority by mounting `/data/shadowmount/fakelib` first and then the game-specific fakelib.
- `global_fakelib_priority=global` reverses that priority.
- Use repeatable `global_fakelib_exclude=<TITLE_ID>` entries to skip the global fakelib for specific games without disabling per-game fakelib.
- `backport_fakelib=0` disables the sandbox `fakelib` watcher, including global fakelib and emulator updates.
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
- When crash monitoring detects an app crash before kstuff was paused, ShadowMountPlus only notifies that the app crashed.
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
- A nested `pfs_image.dat` file inside a PFSC container is treated as a PFS image.
- `.ffpfsc` uses the nested outer PFS profile (`img_type=0x02`); `.ffpfs` and `pfs_image.dat` files mounted from inside it use the nested inner profile (`img_type=0x82`). Signature verification remains disabled for unsigned images; nested inner images request the PFSC compressed-offset cache before LVD attach.

## Compressed PFS containers (`.ffpfsc`)

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

### Packing an uncompressed PFS image into compressed PFS

First create an uncompressed nested PFS image:

```bash
mkpfs pack folder --verify --no-compress --no-adjust-output-file-extension --version PS5 --inode-bits 32 \
  './PPSA07923/PPSA07923-app' \
  './pfs_image.dat'
```

Then pack the nested image (**pfs_image.dat**) into a compressed PFS container:

```bash
mkpfs pack file --verify --version PS5 --inode-bits 32 \
  './pfs_image.dat' \
  './PPSA12345.ffpfsc'
```

After successful packing, the temporary nested image can be removed:

```bash
rm './pfs_image.dat'
```

### Packing an exFAT image into compressed PFS

First create a normal exFAT image using one of the methods from
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

Then pack the exFAT image into a compressed PFS container:

```bash
mkpfs pack file --verify --version PS5 --inode-bits 32 \
  './PPSA12345.exfat' \
  './PPSA12345.ffpfsc'
```

After successful packing, the temporary exFAT image can be removed:

```bash
rm './PPSA12345.exfat'
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

### LVD type-5 / BFS fast-path requirements

With `legacy_mount_exfat=0`, ShadowMount attaches `.exfat` through LVD as
`img_type=5 (Sv)`, with a 512-byte
logical sector and a 64 KiB `secondary_unit`. This lets the LVD worker collect
up to 31 queued, 64-KiB-aligned requests of exactly 64 KiB and
submit them through `bfs_iosession_rw_sdimg`/`BfsSdimg`. Non-matching requests
continue through the normal vnode path.

All of the following are required for the kernel to consider that fast path:

- Use `exfat_backend=lvd` (the default). `/dev/mdctl` has no LVD image type or
  BFS sdimg batching.
- Set `legacy_mount_exfat=0`; the default version 1.6 profile uses
  `img_type=0` and does not request this fast path.
- Store the final `.exfat` file directly on the internal BFS, normally below
  `/data`. An image on USB/UFS/exFAT/PFS, or an exFAT image nested in compressed
  PFS, uses the normal path.
- Format exFAT with a fixed 64 KiB allocation unit. The supplied Linux, macOS,
  and Windows builders now always use this value, including small-file images.
- Keep `lvd_exfat_sector_size=512` and do not add an
  `image_sector=<name>.exfat:65536` override. A logical sector and an exFAT
  allocation unit are different geometries.
- Make the image size a multiple of 64 KiB; the supplied builders round it to
  1 MiB and therefore satisfy this automatically.
- Copy the completed image to BFS in one sequential operation when practical.
  This does not control eligibility, but avoids needless fragmentation of the
  backing file. The default read-only mount is preferred when writes are not
  required.

The 64 KiB cluster improves eligibility but cannot force every access through
the batch path: exFAT metadata and small or unaligned application I/O still use
the safe fallback. In the ShadowMount debug log, an eligible profile request is
shown by `img=5 sec=512 sec2=65536`; the kernel still makes the final BFS and
platform checks internally.

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
  - Always formats with `mkfs.exfat -c 64K` for the LVD/BFS profile.
  - For a manual build, use an image size divisible by 64 KiB and run `mkfs.exfat -c 64K <image>`.

macOS:

- Script: `mkexfat_macos.sh` (uses the built-in exFAT, disk-image and `rsync` tools).
- Example: `chmod +x mkexfat_macos.sh && ./mkexfat_macos.sh ./APPXXXX ./PPSA12345.exfat`.
- The script attaches the raw image as a device and formats it with
  `newfs_exfat -b 65536` before copying the game-root contents.

Windows:
- Recommended: use `make_image.bat` (wrapper for `New-OsfExfatImage.ps1` + OSFMount).
- Requirements:
  - Install OSFMount: https://www.osforensics.com/tools/mount-disk-images.html.
  - Keep `make_image.bat` and `New-OsfExfatImage.ps1` in the same folder.
  - Run `cmd.exe` as Administrator.
  - If you build an exFAT image manually, format it with a `64K` allocation unit, for example `format X: /FS:exFAT /A:64K /Q`.
- Usage:
  - `make_image.bat "C:\images\game.exfat" "C:\payload\APPXXXX"`
- Behavior:
  - Auto-sizes the image to fit source content.
  - Source folder must be the game root and contain `eboot.bin`.
  - Formats and copies source folder contents into image root.
  - Uses a fixed 64 KiB allocation unit for both large-file and small/mixed-file images.
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
