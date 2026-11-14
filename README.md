<div align="center">
  <img src="assets/shadowkraken-header.png" alt="ShadowKraken: a luminous kraken surrounding a digital package" width="100%">
  <h1>ShadowKraken</h1>
  <p><strong>Your PS5 library, mounted your way.</strong></p>
  <p>One homebrew payload for game folders, disk images, package management and experimental native FPKG mounting.</p>
  <p>
    <a href="https://github.com/MNeroba/ShadowKraken/releases/tag/1.00-Dev.1"><img src="https://img.shields.io/badge/release-1.00%20Dev.1-8257e5?style=for-the-badge" alt="Release 1.00 Dev.1"></a>
    <a href="https://github.com/MNeroba/ShadowKraken/actions/workflows/ps5.yml"><img src="https://img.shields.io/github/actions/workflow/status/MNeroba/ShadowKraken/ps5.yml?branch=feature%2Fkraken-fpkg&amp;style=for-the-badge&amp;label=PS5%20build" alt="PS5 build status"></a>
    <img src="https://img.shields.io/badge/platform-PS5-0070d1?style=for-the-badge" alt="Platform PS5">
    <a href="https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-2b6cb0?style=for-the-badge" alt="License GPL-3.0"></a>
  </p>
  <p>
    <a href="https://github.com/MNeroba/ShadowKraken/releases/tag/1.00-Dev.1"><strong>Download</strong></a> ·
    <a href="#quick-start">Quick start</a> ·
    <a href="#formats">Formats</a> ·
    <a href="#web-interface">Web interface</a> ·
    <a href="#configuration">Configuration</a> ·
    <a href="#troubleshooting">Troubleshooting</a>
  </p>
</div>

---

**ShadowKraken** brings the library, storage and title-management features of
ShadowMountPlus together with a native C implementation of key Kraken Mount
functionality. Discover titles on console storage and external drives, work
with your library from a browser, and manage supported game formats from one
payload.

| Your library | Your storage | Your tools |
| --- | --- | --- |
| Automatic discovery and title registration | Internal, USB and extended storage | Browser-based library and installation manager |
| Game folders, disk images and experimental FPKG | SMB2/SMB3 shares for browsing and transfers | Backports, fakelib profiles, logs and settings |

> [!IMPORTANT]
> **1.00 Dev.1 is a development release.** The PS5 payload builds in CI and
> targeted host checks pass. The new native FPKG registration, mount and launch
> path has **not yet been verified on a PS5**. Existing features and individual
> games may behave differently across firmware and storage configurations.

> [!CAUTION]
> A compatible jailbroken PS5 is required. Disk mounting and file operations
> can affect data; back up important files and saves before testing. The web API
> has **no authentication**: use it only on a trusted local network and do not
> expose it to the internet.

## At a glance

- **Automatic library** — scan configured paths, discover supported games and images, and register titles for the console library.
- **Flexible formats** — use game folders, `.ffpkg` UFS2 images, `.exfat` images, `.ffpfs` PFS images and `.ffpfsc` compressed containers.
- **Native FPKG path** — experimentally prepare supported plaintext debug `.pkg` base titles for direct mounting without extracting the entire game image.
- **Browser control** — browse the library, inspect titles, manage compatible sources, track operations and adjust settings from the built-in web interface.
- **Install and transfer** — install supported packages, updates and DLC; copy or move eligible game sources; browse connected local, USB and SMB storage.
- **Compatibility tools** — manage matching backports, fakelib and emulator libraries, with global and per-title settings.
- **Designed around the console** — mount tracking, lifecycle cleanup, notifications, logs, gamepad navigation and storage safeguards are integrated into the payload.

## Quick start

### 1. Get the payload

Download **[ShadowKraken.elf](https://github.com/MNeroba/ShadowKraken/releases/download/1.00-Dev.1/ShadowKraken.elf)**
from the [1.00 Dev.1 release](https://github.com/MNeroba/ShadowKraken/releases/tag/1.00-Dev.1).
The release page also provides an archive with configuration examples, tools
and documentation. Development builds are available from
[GitHub Actions](https://github.com/MNeroba/ShadowKraken/actions/workflows/ps5.yml).

**Requirements**

- A compatible jailbroken PS5 with [kstuff-lite v1.07 or newer](https://github.com/EchoStretch/kstuff-lite).
- A payload loader and sender, or a compatible payload autoloader.
- A supported game source and enough available storage for any required cache or copy.

The active FPKG integration and release workflow are maintained in the
[`feature/kraken-fpkg`](https://github.com/MNeroba/ShadowKraken/tree/feature/kraken-fpkg)
branch. The repository's default `1.7` branch does not contain all features of
this development release.

### 2. Launch ShadowKraken

1. Start your jailbreak environment and payload loader.
2. Send `ShadowKraken.elf` to **port 9021** using your payload sender.
3. Wait for the **ShadowKraken** notification.
4. Place a supported game folder, image or eligible debug package in a [scan location](#game-locations).
5. Allow the background scanner to discover and register the title.

For a compatible autoloader, an example `autoload.txt` is:

```text
ShadowKraken.elf
!3000
kstuff.elf
```

Adapt the order and delay to your existing setup. If an older ShadowMount+
launcher is already registered, its cached name and icon may remain visible.
Removing that launcher with the console's normal library controls and
restarting the payload may be necessary; refresh behavior has not yet been
verified on PS5.

### 3. Open the web interface

Open the **ShadowKraken** launcher on the console, or visit from a device on
the same trusted network:

```text
http://<PS5_IP>:10101
```

Port `10101` is the default. The API can be restricted to the console or
configured under `[api]` in `config.ini`. Disabling the API also skips launcher
installation at startup.

## Formats

| Source | Format | How ShadowKraken handles it |
| --- | --- | --- |
| **Game folder** | Extracted game files | Discovers supported PS4/PS5 title directories in scan paths or `manual.lst`. |
| **`.ffpkg`** | UFS2 disk image | General-purpose image format; a good starting point for most games. |
| **`.exfat`** | exFAT disk image | Useful for external-drive-style compatibility; use 64 KiB clusters. |
| **`.ffpfs`** | PFS disk image | Supported image format. |
| **`.ffpfsc`** | Compressed PFS container | Holds a supported **nested image**. Compression may affect streaming performance. |
| **`.pkg`** | PS5 plaintext debug package | Experimental native FPKG path for supported base-title layouts; see [Native FPKG](#native-fpkg-experimental). |

An `.ffpkg` disk image and a `.pkg` package are different formats. Package
installation through the **Installation** tab is a separate workflow from
experimental direct FPKG mounting.

For folders and regular `.ffpkg`, `.exfat` or `.ffpfs` images, place the game
files at the root:

```text
PPSA12345/
├── eboot.bin
├── sce_sys/
│   └── param.json   # or param.sfo
└── ...
```

Avoid adding another enclosing game folder inside the image. A `.ffpfsc`
container instead holds a supported nested image, not loose game files.

## Native FPKG <sub>experimental</sub>

ShadowKraken integrates Kraken Mount's package-reading, Kraken decompression
and AMPR index concepts with the existing library lifecycle:

```text
scan → parse → prepare/cache → register → mount → launch → cleanup
```

The backend is intended for **self-created and explicitly authorized plaintext
debug FPKG base titles**. It does not decrypt content or bypass platform
signature, license or registration checks. Protected or unsupported layouts
are rejected; updates and DLC are not registered through this direct-mount
path. Their package-installation workflow remains separate.

- The original package must remain available at its scan path.
- A transactional cache under `/data/shadowmount/fpkg-cache/` holds required
  runtime files and indexes; the complete game image is not extracted.
- The package parent is exposed read-only at `/app0/kraken` for AMPR NG. A
  compatible user-provided runtime can be used, or the pinned fallback bundled
  with the payload is staged.
- Matching backport files are incorporated into the prepared indexes. Changes
  to the backport can trigger a new cache generation.
- A separate writable layer preserves game-created `/app0` files across cache
  generations. Previous layers and conflicting files are retained and use
  additional storage; unsupported special filesystem entries can stop a
  migration while preserving the previous layer.

The feature is **enabled by default**. Disable it in **Settings → Library** or
in `/data/shadowmount/config.ini`:

```ini
[fpkg]
direct_mount = off
```

> [!NOTE]
> Host tests exercise parsing, indexes, cache recovery and mocked lifecycle
> integration. Actual PS5 registration, mount behavior, AMPR loading, writable
> layer migration, launch and cleanup still need console testing. The format
> reader supports a subset of possible package layouts; see the
> [architecture notes](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/docs/fpkg-native-architecture.md)
> and [validation record](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/docs/fpkg-test-results.md).

## Game locations

Default scan roots include internal storage, USB drives and extended storage:

```text
/data/homebrew/
/data/etaHEN/games/
/mnt/usb0/homebrew/          # usb0 through usb7
/mnt/usb0/etaHEN/games/      # usb0 through usb7
/mnt/ext0/homebrew/          # ext0 and ext1
/mnt/ext0/etaHEN/games/      # ext0 and ext1
```

USB and extended-storage roots are also scanned. A library might look like:

```text
/data/homebrew/
├── PPSA12345/                 # game folder
├── CUSA12345/                 # game folder
├── PPSA67890.ffpkg            # UFS2 image
├── PPSA24680.pkg              # eligible plaintext debug FPKG
└── backports/
    └── PPSA12345/
```

Add custom locations in **Settings** or `/data/shadowmount/config.ini`:

```ini
[config]
version = 2

[scan]
depth = 2
path = /data/homebrew
path = /mnt/usb0/MyGames
interval_seconds = 15
stability_wait_seconds = 10
```

Custom `path` entries replace the built-in scan roots; managed image-mount
paths remain available. `depth = 1` scans the first level, and `depth = 2`
allows one additional level. Recently copied sources are deferred until they
are considered stable. Avoid multiple scanned copies with the same title ID.

### Add a source outside the scan roots

Add supported game folders or image paths to `/data/shadowmount/manual.lst`,
one absolute path per line:

```text
/mnt/usb0/MyGames/PPSA12345
/mnt/usb0/images/PPSA67890.ffpkg
# Lines beginning with # are ignored
```

Changes are detected automatically. A manually registered source removed from
the console library may also be removed from this list to prevent unwanted
re-registration; add its path again when needed.

## Web interface

The interface is organized around **Library**, **Installation** and **Settings**.
It works in a browser and includes TV/gamepad navigation.

### Library

Search and filter discovered games and installed titles, inspect metadata and
source details, check operation progress, and trigger a rescan. Depending on
the source and current state, actions include mounting, unmounting, copying,
moving or unpacking a game source, and managing fakelib settings. Installed
PKG titles can expose patch and DLC information and supported removal actions.

**Uninstall** and **Delete source** are different: uninstalling a registration
does not necessarily delete its source; deleting a source permanently removes
the selected physical folder or image. A source that remains in a scan path
may be discovered again.

### Installation

Browse supported console, external and configured SMB locations to:

- Copy supported game folders or images to a local destination and register them.
- Install compatible base packages, updates or DLC through the package installer.
- Stream selected PKGs from a PC browser; keep the page open until streaming finishes.
- Queue up to 32 packages and follow progress. PC batches can be ordered base game → update → DLC.
- Install, replace or remove recognized backports and manage eligible files and destinations.

The installer retains the source when copying or installing. Deleting an
installer file is separate from uninstalling an installed title. Some final
installation or deletion steps cannot be safely cancelled.

### Settings and storage

Configure scan paths, notifications, mount and library options, fakelib,
emulator updates, network access and SMB connections. Storage information and
logs are available in the interface; idle USB drives can be ejected when they
are not in use by a game or operation.

### SMB shares

1. Open **Settings → SMB** and create a named connection, such as `NAS`.
2. Enter a share such as `\\nas\games` and any required credentials.
3. Use **Check connection**, then browse `smb://NAS` in Installation or a destination picker.

SMB2 and SMB3 are supported, with up to 16 configured connections. Guest,
`DOMAIN\user`, read-only and disabled connections are available where the
server permits them. SMB sources are for browsing, transfers and installation;
they are not background scan roots or direct-launch sources. Copy games and
images to compatible console storage first. SMB credentials are stored in
plaintext in `config.ini`; protect that file and its backups.

## Configuration

ShadowKraken retains the existing `/data/shadowmount/` paths for compatibility.
Most common options can be changed in the web interface.

| Path | Purpose |
| --- | --- |
| `config.ini` | Global settings, scan paths, network shares and mount options. |
| `profiles.ini` | Per-title fakelib and per-image mount overrides. |
| `manual.lst` | Individually added game folders and images. |
| `debug.log` | Diagnostics and error messages. |
| `fpkg-cache/` | Experimental native FPKG cache generations. |

Use the [configuration template](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/config.ini.example)
and [profile template](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/profiles.ini.example)
for accepted values and defaults. Keep the `[config] version = 2` marker.

| Section | Controls |
| --- | --- |
| `[general]` | Debug logging, notifications and language. |
| `[api]` | Web/API availability, bind address and port. |
| `[scan]` | Scan roots, depth, interval and source stability. |
| `[mount]` | Image mount mode, persistence and filesystem backends. |
| `[library]` | Registration and missing-source behavior. |
| `[fpkg]` | Experimental direct FPKG path; enabled by default. |
| `[fakelib]`, `[emulators]` | Compatibility overlays and emulator libraries. |
| `[fan]`, `[smb:NAME]` | Optional fan target and named network shares. |

### Per-game and per-image profiles

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

Fakelib modes are `default`, `full`, `emulators` and `disabled`; image modes
are `default`, `ro` and `rw`. Image profiles match filenames rather than full
paths. Changes take effect at the next launch or mount, not on an active one.

### Existing configurations

Older `config.ini`, `autotune.ini` and `smb.json` settings are migrated to
configuration v2 on first launch. Originals are backed up under
`/data/shadowmount/config-v1-backup/`. Preserve the backup before downgrading;
it may contain plaintext SMB credentials.

## Backports and fakelib

Place a title's backport under `/data/homebrew/backports/`, or use a
`backports/` directory beneath a configured scan root:

```text
/data/homebrew/backports/PPSA12345/
```

Folder names may include a descriptive suffix after the title ID. The web
installer recognizes backport folders containing `fakelib` and can install,
replace or remove them. For supported titles, per-game fakelib modes can be set
in the library or `profiles.ini`. The default global fakelib location is
`/data/shadowmount/fakelib`; emulator libraries default to
`/data/shadowmount/emus`.

Optional AMPR update checks are controlled by `[emulators] auto_update_ampr`
and are off by default. Disable the standalone BackPork payload when using
ShadowKraken's fakelib integration to avoid conflicts.

## Creating images

Supply the **game root directory** as the source when creating an image.

| Platform | Tool | Output |
| --- | --- | --- |
| FreeBSD | [`mkufs2.sh`](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/mkufs2.sh) | UFS2 `.ffpkg` |
| Windows | [UFS2Tool](https://github.com/SvenGDK/UFS2Tool) | UFS2 `.ffpkg` |
| Linux | [`mkexfat.sh`](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/mkexfat.sh) | `.exfat` |
| macOS | [`mkexfat_macos.sh`](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/mkexfat_macos.sh) | `.exfat` |
| Windows | [`make_image.bat`](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/make_image.bat) with [OSFMount](https://www.osforensics.com/tools/mount-disk-images.html) | `.exfat` |

Examples:

```bash
# FreeBSD: UFS2
./mkufs2.sh ./PPSA12345-app ./PPSA12345.ffpkg

# Linux or macOS: exFAT
./mkexfat.sh ./PPSA12345-app ./PPSA12345.exfat
./mkexfat_macos.sh ./PPSA12345-app ./PPSA12345.exfat
```

The Linux script requires `exfatprogs`, `exfat-fuse`, `fuse3` and `rsync`.
On Windows, keep `make_image.bat` and `New-OsfExfatImage.ps1` together, install
OSFMount and run Command Prompt as Administrator:

```bat
make_image.bat "C:\images\PPSA12345.exfat" "C:\games\PPSA12345-app"
```

The exFAT builders use a 64 KiB allocation unit. A compressed `.ffpfsc`
container can be created with [MkPFS](https://github.com/PSBrew/MkPFS):

```bash
mkpfs pack file --verify --version PS5 --inode-bits 32 \
  ./PPSA12345.exfat ./PPSA12345.ffpfsc
```

Verify the new container before deleting its source image.

## Troubleshooting

| Symptom | Start here |
| --- | --- |
| A game does not appear | Check the scan root, depth, source stability, title ID and folder/image layout. |
| An image does not mount | Verify the image format and integrity; check `sce_sys/` is at the image root. |
| A `.ffpfsc` contains no detected game | Confirm the container holds a supported nested image. |
| A debug FPKG is rejected | Check that it is a supported plaintext base-title layout, remains at its scan path and has enough cache space. Console integration is experimental. |
| Duplicate title | Check for multiple scanned sources with the same title ID. |
| Backport or fakelib issue | Check the title ID and directory layout; disable standalone BackPork when integrated fakelib is active. |
| SMB connection fails | Check reachability, share name, credentials, SMB2/3 and server permissions. |
| Settings do not apply | Check INI values and restart the affected game or remount the image. |
| Registered game does not launch | Review notifications and `/data/shadowmount/debug.log`; platform behavior varies by firmware and title. |

Debug logging is enabled by default and can be changed with `[general] debug`
in `config.ini`. When reporting a problem, include your firmware, release or
commit SHA, source format, relevant configuration and sanitized log excerpts.
Do not share private credentials or copyrighted game files.

## Documentation and community

- [Release downloads](https://github.com/MNeroba/ShadowKraken/releases/tag/1.00-Dev.1) · [PS5 build workflow](https://github.com/MNeroba/ShadowKraken/actions/workflows/ps5.yml) · [Issue tracker](https://github.com/MNeroba/ShadowKraken/issues)
- [Configuration example](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/config.ini.example) · [Profile example](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/profiles.ini.example) · [API documentation](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/docs/api.md)
- [FPKG architecture](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/docs/fpkg-native-architecture.md) · [Cache design](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/docs/fpkg-cache-transaction.md) · [Test record](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/docs/fpkg-test-results.md)

## Credits

ShadowKraken stands on the work of both original projects and their authors:

- **[Drakmor](https://github.com/drakmor)** — [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus), the foundation for library management, mounting and the web interface. ShadowMountPlus builds on the original ShadowMount by **VoidWhisper**.
- **[quer3q](https://github.com/quer3q)** — [Kraken Mount](https://github.com/quer3q/kraken-mount), the reference for native FPKG reading, Kraken decompression and AMPR index generation.

Thanks to **BestPig**, **EchoStretch**, **Gezine**, **earthonion**,
**LightningMods**, **RenanGBarreto**, **john-tornblom** and the PS5 research
and homebrew community for their work.

See [third-party notices](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/THIRD_PARTY_NOTICES)
and the [AMPR NG notice](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/licenses/AMPR-NG-NOTICE.md)
for component attribution and licensing. Distributed under [GPL-3.0](https://github.com/MNeroba/ShadowKraken/blob/feature/kraken-fpkg/LICENSE).
