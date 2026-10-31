# ShadowMountPlus (PS5)

> **Configuration format changed (v2).** On the first launch, ShadowMountPlus converts the old `config.ini`, `autotune.ini` and `smb.json`. Global settings and SMB connections now live in `config.ini`; individual game/image settings live in `profiles.ini`. Originals are kept in `/data/shadowmount/config-v1-backup/`. Do not copy an old configuration over the new files. See [configuration migration](#configuration-migration) before downgrading.

**Repository:** https://github.com/drakmor/shadowMountPlus

**Discord:** https://discord.gg/x2Ppvzwjhm


**Warning! Mounting images can cause shutdown problems and data corruption on internal drives! This depends on many factors, but is more common with older firmware versions. Please take this into account when testing.**


**ShadowMountPlus** is a fully automated, background "Auto-Mounter" payload for Jailbroken PlayStation 5 consoles. It streamlines the game mounting process by eliminating the need for manual configuration or external tools (such as DumpRunner or Itemzflow). ShadowMountPlus automatically detects, mounts, and installs game dumps from both **internal and external storage**.


**Compatibility:** Supports Jailbroken PS5 firmwares running **[Kstuff-lite v1.07+](https://github.com/EchoStretch/kstuff-lite)**, including firmware 2.00.


## 💜 Support Development

 If you want to support this project, you can donate
 - USDT (TRC-20):  **`TKaUGEwMm9KBXzEoiaaKYBX2yCHAKASW3p`**
 - USDT (ERC-20):  **`0x313dD245dBA957A5560618eA882d08e66aaFb430`**
 - USDT (BEP20):   **`0x313dD245dBA957A5560618eA882d08e66aaFb430`**
 - USDC (Solana):  **`5kv7j2RbUGaSP1kU1cZWj9jHH7d6rfvxmK6YXTYbH4um`**
 - BTC (Bitcoin):  **`bc1q58czpsrhz783hs8878tzlt4gatrjall9ulqduu`**
 - Card payments:  **`https://boosty.to/drkamor/donate`**
 

## Current image support

`PFS support is experimental.`

| Extension | Mounted FS | Attach backend | Status |
| --- | --- | --- | --- |
| `.ffpkg` | `ufs` | `LVD` or `MD` (configurable) | Recommended |
| `.exfat` | `exfatfs` | `LVD` or `MD` (configurable) | Compatibility / external-drive-only titles |
| `.ffpfs` | `pfs` | `LVD` | Experimental |
| `.ffpfsc` | `pfs` container | `LVD` | Experimental container for nested images |

Notes:
- Homebrew IDs contain four uppercase ASCII letters and five digits, excluding
  the `CUSA`, `PPSA`, and `NPXS` prefixes. Confirmed running homebrew allows background scans,
  registration, and source copy/move/delete operations on other titles, whether
  the application uses a folder, image, or installed PKG. Its own runtime, source
  and all backing image layers remain protected. Operations that would release
  a shared image are refused. Ordinary PS4/PS5 games allow the same work while
  running, regardless of AppFocus. Forced and scheduled scans keep the live
  title and its backing chain protected, and pace directory traversal while a
  game is active. Pending launch/exit transitions and unknown runtime ownership
  still defer work.
- Backend, read-only mode, and sector size can be configured via `/data/shadowmount/config.ini`.
- Debug logging is enabled by default (`debug=1`) and writes to console plus `/data/shadowmount/debug.log` (set `debug=0` to disable).
- **UFS (`.ffpkg`) is the recommended image format for normal use.**
- **Use exFAT (`.exfat`) only for titles that need external-drive-style compatibility.**
- **When building exFAT images manually, keep the cluster size at `64 KB`; smaller clusters can reduce performance.**

## SMB file transfers and installation

Open **Settings → SMB** in the web interface to add a named SMB2/SMB3
connection. Enter a name such as `NAS`, a share path such as
`\\nas.home.loc\homebrew` (an optional subfolder is supported), user name and
password. **Check connection** verifies authentication, access to the configured
directory and the negotiated SMB dialect. Leading `\\` are added automatically
if omitted; the UI converts the UNC path to the client's SMB URL internally.
An empty user name uses Guest;
`DOMAIN\user` is supported.

Active connections appear as `smb://NAS` in **Installation** and the copy/move
destination picker. Copy or move game images and folders from console storage
to the share, or install folders and intact images from the share onto console
storage. PKGs on SMB are streamed to the system installer through an on-demand
HTTP range server. Local base PKGs use native file paths directly; patches and
DLC use HTTP streaming, including local files, to allow safe cancellation.
Images and folders are copied locally before registration and launching.
Files copied from SMB to console storage receive permissions `0777`.
Active SMB connections also appear in the storage overview and summary with
free space, total capacity and a usage meter. Capacity is loaded separately
from local disks; an unavailable share does not delay the local storage list.
The installation browser has trash icons for folders, images and PKGs on SMB
and local drives. Deletion requires confirmation. Icons are disabled for
read-only/system paths and local scanned folders/images; PKGs may be deleted
from scan folders. The shared storage worker rechecks permissions and policy.
The custom folder picker also offers **New folder** on writable local and SMB
paths. Enter a name to create and open that folder, then select it as usual.
**Installation → Drives and folders** shows available and total filesystem
capacity for each path. It refreshes after copy, move, delete and install jobs,
including cancelled or failed operations that may have changed disk usage.

SMB-to-local and local-to-SMB copies from 512 KiB overlap reading and writing
with two buffers, sized for the file and bounded at 4 MiB each. The reader
thread exists only for the current file and is joined before cleanup.
Directory transfers reuse an authenticated connection while the directory or
its child files remain open, including directory creation and removal.
Uploads overlap server flushes with subsequent files using a bounded window
of four destination files. Every flush and close must succeed before the
directory copy completes or a move removes its source. Single-file transfers
and destinations that deny directory listing retain synchronous completion.
Small reads stop at the expected remaining size,
with a one-byte EOF check; a growing source continues to stream normally.
Bulk SMB requests use a bounded, continuously refilled window independent of
the file's total size. Reads use requests up to 1 MiB; writes keep up to 4 MiB
contiguous to avoid concurrent small file extensions on Windows destinations.
Both respect the server's negotiated limit; SMB2.0.2 retains its 64 KiB limit.
Small socket buffers are increased to 512 KiB without shrinking larger system defaults.
Large copies log complete file timing, including open, flush and close.

File-backed PKG HTTP responses of at least 4 MiB overlap source reads with
ordered socket sends using two buffers. Native files use 2 MiB per buffer;
SMB ranges of at least 8 MiB use 4 MiB per buffer to widen the SMB request window.
The reader is joined before response cleanup; resource failures fall back to
sequential delivery. Short ranges stay sequential, and browser sources retain
their existing range-cache prefetch. HTTP logs identify SMB sources separately
and include `wait_ms` for time the sender waits for source data. Timings are
summed across connections; read/send times can overlap.
Browser streaming negotiates up to four outstanding 1 MiB uploads within its
existing 32 MiB cache. HTTP sends each ready segment immediately, including
unaligned ranges. Older pages and servers retain the two-upload window.

Up to 16 connections are saved as `[smb:NAME]` sections in `/data/shadowmount/config.ini`, including
credentials, with owner-only file permissions. API responses never include
passwords. Leaving the password empty when editing preserves it; use **Clear
saved password** to remove it. Connection changes are refused while a storage
job or PKG queue is active. Disabling a connection hides its paths without
deleting its settings. SMB1 and background scanning/mounting of shares are not
supported. Shares are contacted only for explicit browsing, checks and transfers.

Building requires the PS5 payload SDK's `libsmb2` headers and static library,
in addition to the existing dependencies. See [SMB notices](licenses/SMB-NOTICES.txt).
The first `make` also downloads and builds a checksum-pinned json-c 0.19 in
`.deps/json-c` (Python 3, CMake and patch are required). This private library
includes upstream fixes for container leaks with `NDEBUG`; the SDK's existing
json-c archive is not used. Local builds and CI use the same dependency.
PS5's placeholder locale APIs are disabled for JSON parsing to avoid leaking
locale handles and sharing mutable locale state between API workers.

## Recommended FS choice

- Prefer **UFS (`.ffpkg`)** in most cases: this is the recommended default image format for ShadowMountPlus.
- Use **exFAT (`.exfat`)** only for games that do not work correctly unless they are handled like external-drive content.
- If you create an **exFAT (`.exfat`)** image manually, use a **`64 KB` cluster size**. Smaller clusters can cause a noticeable performance loss.

## Runtime configuration

Files under `/data/shadowmount/`:

| File | Contents |
| --- | --- |
| `config.ini` | Global settings, scan roots, SMB connections |
| `profiles.ini` | Individual game settings by title ID; image settings by filename |

Both files start with `[config]` and `version = 2`. Keep this marker. Settings are grouped into INI sections; an absent setting uses its default. Boolean values accept `on/off`, `true/false`, `yes/no` or `1/0`. Section/key names and image/title identities are case-insensitive. Scalar duplicates use the last value; `[scan] path` is repeatable. Quote values containing `#`, `;`, double quotes, backslashes or leading/trailing spaces; inside quotes use `\\` for a backslash and `\"` for a double quote.

Fresh installations get the bundled [config.ini.example](config.ini.example) and an empty versioned `profiles.ini`. See [profiles.ini.example](profiles.ini.example) for individual overrides. Web settings, SMB edits and automatic sector corrections use the same files and preserve unrelated settings and comments. Manual changes are reloaded by file events; invalid recognized values reject the reload and retain the last valid configuration. Unknown keys are logged and preserved by edits. Game/image changes take effect at the next launch/mount, without disrupting active mounts.

Main sections (see the commented template for defaults and ranges):

| Section | Keys |
| --- | --- |
| `[general]` | `debug`, `quiet_mode`, `language` |
| `[api]` | `enabled`, `bind_address`, `port` |
| `[scan]` | `depth`, repeatable `path`, `interval_seconds`, `stability_wait_seconds` |
| `[mount]` | `mode` (`ro/rw`), `force`, `persistent_images`, `exfat_backend`, `ufs_backend`, `nested_pfs_index_cache` |
| `[library]` | `install_all`, `remove_missing`, `remove_with_dlc`, `remove_delay_seconds` |
| `[fakelib]` | `enabled`, `default_mode`, `global_enabled`, `global_path`, `global_priority` |
| `[emulators]` | `update_enabled`, `path`, `auto_update_ampr`, `ampr_update_url` |
| `[fan]` | `target_temperature` (`system` or `50..91`) |
| `[sectors]` | `lvd_exfat`, `lvd_ufs`, `lvd_pfs`, `md_exfat`, `md_ufs` |
| `[smb:NAME]` | `path`, `username`, `password`, `active`, `read_only` |

Example `profiles.ini`:

```ini
[config]
version = 2

[game:PPSA13197]
fakelib = emulators
ignore_global_fakelib = on

[image:My Game.exfat]
mount_mode = ro
sector_size = 512
```

Game `fakelib` accepts `default`, `full`, `emulators`, `disabled`; `ignore_global_fakelib` accepts a boolean or `default` (off). Image `mount_mode` accepts `default`, `ro`, `rw`; `sector_size` accepts `default` or a power of two from 512 to 1048576. Up to 128 explicit fakelib rules, 128 global exclusions and 128 image rules are supported. Images are matched **only by filename**, so matching names on different drives share the same profile. Paths are not valid image profile identities.

### Configuration migration

1. The old instance is stopped before migration. Original files are copied, byte for byte, into `config-v1-backup/`, and the converted settings are validated before publication.
2. Old global keys become sectioned main settings. `scanpath` becomes repeatable `[scan] path`; `recursive_scan=1` becomes `[scan] depth=2`. Game/image rules move to `profiles.ini`; SMB credentials move from JSON to `[smb:NAME]` sections.
3. Old autotune sector overrides retain their priority over old manual overrides. Future automatic corrections update `profiles.ini` directly. Active `autotune.ini` and `smb.json` are removed only after successful migration; their backups remain.
4. `profiles.ini` is published first and `config.ini` with `version=2` last. An interrupted migration resumes from its prepared backup on the next launch. Existing unrelated profiles and source changes during an interrupted migration cause an error instead of being overwritten.

If conversion fails, startup stops with a configuration error. Fix invalid legacy input and retry. After an interrupted publication, preserve the entire directory before making changes; to restart migration from edited legacy sources, move aside the generated `profiles.ini` and `config-v1-backup/` together. Unsupported old keys remain in the original backup and are reported in the log. Version numbers newer than this build supports are rejected.

To downgrade, stop SM+, save both current v2 files separately, then restore the original `config.ini`, `autotune.ini` and `smb.json` from `config-v1-backup/` before starting an old build. Settings changed after migration are not automatically converted back. Backups include SMB passwords: new files use owner-only permissions, but credentials are plaintext, so protect copies too.

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
| Ukrainian | `uk-UA` |
| Vietnamese | `vi-VN` |

The public API routes and JSON schemas are documented in [docs/api.md](docs/api.md)
and available as an [OpenAPI manifest](docs/openapi.yaml).

Per-image mode override behavior:
- Use `[image:FILENAME] mount_mode` in `profiles.ini`; spaces and brackets in names are supported.
- The last value for the same filename wins. Without an override, `[mount] mode` applies.

Persistent image mount behavior:
- With `[mount] persistent_images=0` (default), backing images are mounted when a game starts and released after it exits.
- With `[mount] persistent_images=1`, images discovered during scans remain mounted between launches. Per-title runtime and backport layers are still mounted only when needed.
- USB-backed images are always released during suspend and mounted again by the resume scan.
- Persistent mode consumes one backing device per mounted image layer and is intended for libraries that fit the available LVD/MD device capacity.

Per-image sector override behavior:
- Match is done by image file name (without path).
- `[image:FILENAME] sector_size` in `profiles.ini` supplies the individual override.
- If no per-image rule matches, the backend-specific global sector size defaults are used.
- When image validation fails because the mounted file-system cluster size is smaller than the selected device sector size, ShadowMountPlus writes `sector_size=<cluster_size>` into `[image:FILENAME]` in `profiles.ini` and asks you to try mounting again.
- For the exFAT LVD/BFS fast path, leave the logical sector at its default `512`. The required `65536` values are the exFAT allocation unit and the internal LVD `secondary_unit`; do not set `sector_size=65536` to request this mode.

Scan path behavior:
- If at least one `[scan] path=...` is present, only those custom paths are used.
- `/mnt/shadowmnt/pfsc` and `/mnt/shadowmnt` are always added automatically, even with custom paths.
- With `[scan] depth=1` (default), only first-level subfolders are checked.
- With `[scan] depth=2`, one additional nested level is checked.
- Full scan loop runs every `[scan] interval_seconds` (default: `15`).
- Sources newer than `[scan] stability_wait_seconds` are deferred until stable (default: `10`).
- With `[library] remove_missing=1`, a game is removed from the system library only after its source remains unavailable for `[library] remove_delay_seconds`. If the source becomes available again before removal, the pending removal is cancelled.
- Direct folder installs use `<game>/sce_sys` for this check; image and backport sources use the target path itself.

Backport overlay behavior:
- For each `scanpath`, use:
  - `<scanpath>/backports/<TITLE_ID>/`
  - `<scanpath>/backports/<TITLE_ID><suffix>/` (for example, `PPSA12345 Game Name`)
- The `backports` folder is ignored during normal game scanning.
- A backport is applied automatically to the matching mounted game from any configured scan path.
- `/data/homebrew/backports/<TITLE_ID>/` remains available as a fallback even with custom scan paths; this does not add `/data/homebrew` to normal game scanning.
- For an installed PKG, a matching external `backports/<TITLE_ID>` directory is added to the ready package `app0` through NSFS immediately before process spawn. Files use plain redirects and missing directory subtrees use directory overlays. Up to 256 redirect entries are supported per package sandbox. Redirect state follows the NSFS mount identity, so redirects are rebuilt when ShellCore recreates a sandbox under the same path. Missing backports and non-NSFS sandboxes are harmless skips. The package's own or external-backport `fakelib2`/`fakelib` remains available for the later `common/lib` pre-spawn overlay.
- If multiple scan paths provide the same title backport, the game's own scan path wins; otherwise scan path order is used, followed by the internal fallback.
- Within each scan path, the exact `<TITLE_ID>` directory wins. Otherwise, the first directory name beginning with the full title ID in byte order is selected. Files and nested directories are not candidates.
- ShadowMount+ checks the selected backport for `fakelib2` and then `fakelib`; if neither exists, it checks the original game source in the same order. The selected directory is mounted into the running game's sandbox `common/lib`. A selected `fakelib2` remains exclusive: neither emulator updates nor the global fakelib can replace or supplement it. Per-game filtering and removal of `libkernel.sprx` still apply, using a cache when needed.
- For folder/image games, `[emulators] update_enabled=1` makes matching files from `[emulators] path` replace files in the selected game fakelib, except when `fakelib2` is selected. Installed PKGs never compose `[emulators] path` into their fakelib. The cache is refreshed when its sources change and expires after seven days without a game launch.
- Before preparing and mounting fakelib for an installed PPR PKG, ShadowMount+ deletes `libSceAmpr.sprx` and `libScePlayGo.sprx` from the selected `/data/homebrew/backports/<TITLE_ID><suffix>/fakelib` (the suffix may be empty). Missing files are harmless skips; a deletion failure stops the fakelib mount.
- With `[emulators] auto_update_ampr=1`, ShadowMount+ checks for AMPR updates 30 seconds after startup and every four hours. It downloads a missing or newer emulator and displays a notification after a successful update.
- When the mounted fakelib contains only the supported emulator libraries, the notification says `Emulators applied`. Otherwise the backport notification adds `Emulators updated` when emulator files were replaced.
- If both global and per-game fakelib exist, they are combined in the game cache according to `[fakelib] global_priority`, unless the selected backport contains `fakelib2`. Without a per-game fakelib, the global folder is used on its own, with a filtered cache when required.
- Use `[game:TITLE_ID] ignore_global_fakelib=on` in `profiles.ini` to skip the global fakelib for specific games without disabling per-game fakelib.
- The game card in the web interface offers `Default`, `Full`, `Emulators only` and `Disabled` fakelib modes. `Default` inherits `[fakelib] default_mode` from web settings or `config.ini` (initially `full`). An explicit per-game mode overrides that default, including `full`. Modes apply to the selected game fakelib (including `fakelib2`) and global fakelib on the next launch; active mounts stay unchanged. Set `fakelib=emulators` or `fakelib=full` under `[game:PPSA12345]` in `profiles.ini`; `default` restores inheritance.
- `Emulators only` mounts available regular files named `libSceAppContent.sprx`, `libSceGameUpdate.sprx` and `libSceNpEntitlementAccess.sprx` for installed PKGs. Folder/image games also allow `libSceAmpr.sprx` and `libScePlayGo.sprx`. Missing files are silently skipped; an empty selection creates no fakelib overlay. Emulator updates obey the same allowlist.
- With fakelib enabled, `libkernel.sprx` is removed from the selected writable folder fakelib and participating global fakelib. For image games and installed PKGs, source files remain unchanged and a cache omitting this library is mounted instead. If a required filtered cache cannot be created, the unfiltered source is never mounted.
- Use `[game:TITLE_ID] fakelib=disabled` in `profiles.ini` to disable per-game `fakelib`, `fakelib2`, global fakelib and cache preparation for specific games. `/data`, sandbox storage aliases and PKG `app0` backports remain enabled. Reloaded exclusions apply on the next launch; existing mounts remain until game exit. Title IDs are case-insensitive and must have a supported prefix followed by five digits; invalid entries and entries beyond the 128-title limit reject the reload, retaining the last valid settings.
- `/data` is mounted through writable `nullfs` for PS5 (`PPSA*`), PS4 (`CUSA*`) and homebrew titles. Sandbox `/mnt` remains a local directory: only mounted volumes at `/mnt/usb0` through `/mnt/usb7`, `/mnt/ext0` through `/mnt/ext1` and `/mnt/disc` receive individual writable `nullfs` aliases. Empty mount directories and other host `/mnt` entries are skipped. Storage aliases follow filesystem mount/unmount events, including USB hotplug and disc insertion/ejection. All aliases are removed on game process exit.
- `[fakelib] enabled=0` disables the sandbox `fakelib` watcher, including global fakelib and emulator updates.
- For `[fakelib] enabled` to work correctly, the standalone `BackPork` payload must be disabled. Running both at the same time will conflict.

Game error monitoring:

- Supported game launches are monitored for up to 60 seconds from the original
  launch event. Same-game process replacements keep the original deadline.
- Fatal signals and module-load errors are matched to the game's PID and shown
  with their details. An exception-stop flag provides a generic crash notification
  when no detailed error is available. Incomplete fatal reports wait for up to
  two seconds without report progress, then show the available PID-matched
  details or fall back to the tracked process's exception flag.
- Monitoring stops after the first reported error, the deadline, game exit,
  runtime sleep or watcher shutdown. It does not control kstuff or tune delays.

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
- Valid example: `/sce_sys/param.json` or `/sce_sys/param.sfo` exists directly from image root.
- Invalid example: `/GAME_FOLDER/sce_sys/param.json` (extra nesting level).

PFSC container layout requirement (`.ffpfsc`):
- Do not place game files directly in the container root.
- Place supported nested image files inside the container; ShadowMountPlus mounts those nested images and scans them for the game.
- A nested `pfs_image.dat` file inside a PFSC container is treated as a PFS image.
- `.ffpfsc` and its nested `.ffpfs`/`pfs_image.dat` images use the version 1.6 LVD mapping unit, equal to the logical sector size (4096 bytes by default). The outer selector remains `img_type=0x02`, and the inner selector remains `img_type=0x82`. A standalone `.ffpfs` source uses the optimized profile only under `/data/...` or `/user/...`; other standalone sources use the version 1.6 parameters. Signature verification remains disabled for unsigned images; set `nested_pfs_index_cache=1` to request the PFSC compressed-offset cache before attaching nested images.

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

You can override scan roots with `[scan] path=...` entries in `/data/shadowmount/config.ini`.

## Manual install list

For games that should not live under the normal scan paths, ShadowMountPlus also
checks:

`/data/shadowmount/manual.lst`

Add one source per line:
- Path to a game folder containing `eboot.bin` or `eboot2.bin` and `sce_sys/param.json` or `sce_sys/param.sfo`.
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
- Default mode (`[scan] depth=1`):
  - `/data/homebrew/<TITLE_ID>/`
  - `/data/etaHEN/games/<TITLE_ID>/`
  - `/data/homebrew/backports/<TITLE_ID>/`
  - `/data/etaHEN/games/backports/<TITLE_ID>/`
   
- Nested mode (`[scan] depth=2`):
  - `/data/homebrew/PS5/<AnyFolder>/<TITLE_ID>/`
  - `/mnt/ext0/etaHEN/games/<Collection>/<TITLE_ID>/`
  - `/mnt/ext0/etaHEN/games/backports/<TITLE_ID>/`


## Creating an exFAT image

Recommended only for titles that need external-drive-style compatibility. For general use, prefer `.ffpkg`.

### LVD type-5 / BFS fast-path requirements

For direct source paths under `/data/...` or `/user/...`, ShadowMount attaches
`.exfat` through LVD as `img_type=5 (Sv)`, with a 512-byte logical sector and a
64 KiB `secondary_unit`. This lets the LVD worker collect
up to 31 queued, 64-KiB-aligned requests of exactly 64 KiB and
submit them through `bfs_iosession_rw_sdimg`/`BfsSdimg`. Non-matching requests
continue through the normal vnode path.

All of the following are required for the kernel to consider that fast path:

- Use `exfat_backend=lvd` (the default). `/dev/mdctl` has no LVD image type or
  BFS sdimg batching.
- A `.exfat` source outside `/data/...` and `/user/...` uses the `img_type=0`
  profile directly. No second mount attempt is made after failure.
- Store the final `.exfat` file directly on the internal BFS, normally below
  `/data`. An image on USB/UFS/exFAT/PFS, or an exFAT image nested in compressed
  PFS, uses the normal path.
- Format exFAT with a fixed 64 KiB allocation unit. The supplied Linux, macOS,
  and Windows builders now always use this value, including small-file images.
- Keep `lvd_exfat_sector_size=512` and do not add an
  `[image:<name>.exfat] sector_size=65536` override. A logical sector and an exFAT
  allocation unit are different geometries.
- Make the image size a multiple of 64 KiB; the supplied builders round it to
  1 MiB and therefore satisfy this automatically.
- Copy the completed image to BFS in one sequential operation when practical.
  This does not control eligibility, but avoids needless fragmentation of the
  backing file. The default read-only mount is preferred when writes are not
  required.

The 64 KiB cluster improves eligibility but cannot force every access through
the batch path: exFAT metadata and small or unaligned application I/O still use
the normal vnode path. In the ShadowMount debug log, an eligible profile request is
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
- Debug log is enabled by default; if disabled, set `[general] debug=on` in `/data/shadowmount/config.ini`.
- Check `/data/shadowmount/debug.log` and system notifications from ShadowMount+.
- Verify scan roots:
  - if `[scan] path=...` is set, only these paths are scanned;
  - `/mnt/shadowmnt/pfsc` and `/mnt/shadowmnt` are always scanned.
- Verify scan depth:
  - `[scan] depth=1` scans only first-level subfolders;
  - `[scan] depth=2` scans one additional nested level;
  - `recursive_scan=1` is treated as deprecated compatibility mode and forces `[scan] depth=2`.
- If logs show `source not stable yet`, adjust `[scan] stability_wait_seconds` (or wait for source copy/write to finish).
- Verify game structure:
  - folder game: `<GAME_DIR>/sce_sys/param.json` or `<GAME_DIR>/sce_sys/param.sfo`;
  - image game (`.ffpkg` / `.exfat` / `.ffpfs`): `sce_sys/param.json` or `sce_sys/param.sfo` must be at image root (no extra top-level folder);
  - PFSC container (`.ffpfsc`): nested supported image files are scanned; direct game files inside the container are ignored.
- If you see `missing/invalid param.json/param.sfo` for an image, check via FTP that files are present under `/mnt/shadowmnt/<image_name>_<hash>/` and include `sce_sys/param.json` or `sce_sys/param.sfo`.
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
