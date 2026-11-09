# ShadowMountPlus (PS5)

**ShadowMountPlus** is an automated background game mounter and library manager for jailbroken PlayStation 5 consoles. It discovers and registers PS4/PS5 game folders and supported disk images on internal or external storage, then prepares them when launched. A built-in web interface provides library management, installation, file transfers, and configuration.

**[Download releases](https://github.com/drakmor/ShadowMountPlus/releases)** · **[Discord community](https://discord.gg/x2Ppvzwjhm)** · **[Report an issue](https://github.com/drakmor/ShadowMountPlus/issues)**

> [!WARNING]
> Mounting disk images can cause shutdown issues or data corruption, particularly on older firmware. Back up important files and saves before testing. This project requires a jailbroken PS5 and is not intended for an unmodified console.

## 💜 Support development

If ShadowMountPlus is useful to you, you can support continued development:

| Method | Address / link |
| --- | --- |
| **Card / Boosty** | **[Donate via Boosty](https://boosty.to/drkamor/donate)** |
| USDT (TRC-20) | `TKaUGEwMm9KBXzEoiaaKYBX2yCHAKASW3p` |
| USDT (ERC-20) | `0x313dD245dBA957A5560618eA882d08e66aaFb430` |
| USDT (BEP20) | `0x313dD245dBA957A5560618eA882d08e66aaFb430` |
| USDC (Solana) | `5kv7j2RbUGaSP1kU1cZWj9jHH7d6rfvxmK6YXTYbH4um` |
| BTC (Bitcoin) | `bc1q58czpsrhz783hs8878tzlt4gatrjall9ulqduu` |

Please double-check the currency and network before sending a donation.

## Features

- **Automatic game discovery:** scan configured folders, register supported titles, and keep the library up to date.
- **Folder and image support:** run games stored as extracted folders, UFS2, exFAT, PFS images, and compressed PFS containers.
- **Internal and external storage:** work with the PS5's internal storage and supported USB/external drives.
- **Web interface:** search and filter the library, manage games and their files, install PKGs, updates, DLC and backports, and monitor operations.
- **SMB2/SMB3 file management:** connect NAS or PC shares, browse their contents, transfer files, and install content to the console.
- **Backports and fakelib:** automatically apply matching backports, install/replace/remove backports through the web interface, and configure per-game fakelib modes.
- **Emulator libraries:** manage supported emulator-library replacements and optionally check for AMPR updates.
- **Per-game and per-image profiles:** override selected options without changing global defaults.
- **Background operation:** continue detecting and managing other titles while a supported game is running, with safeguards for active game files.
- **Notifications and logs:** view startup, installation, mount, and error messages on the console and review diagnostics from the web interface.

## Requirements

- Jailbroken PlayStation 5 with **[kstuff-lite v1.07 or newer](https://github.com/EchoStretch/kstuff-lite)** (including supported firmware 2.00 configurations).
- The latest compatible `shadowmountplus.elf` from [Releases](https://github.com/drakmor/ShadowMountPlus/releases).
- A payload sender or a compatible autoloader.

## Quick start

### Option 1: Manual payload launch

1. Start your jailbreak environment and payload loader.
2. Send `shadowmountplus.elf` to **port 9021** using a payload sender.
3. Wait for the **ShadowMount+** notification.
4. Place a compatible game folder or image in a [scan location](#game-locations-and-scanning).
5. Allow the background scanner to discover and register it.

### Option 2: PLK autoloader (recommended)

Add the following to your autoloader's `autoload.txt`, adjusting paths and entries to your existing setup:

```text
shadowmountplus.elf
!3000
kstuff.elf
```

This example starts ShadowMountPlus before kstuff, with a delay between them. Keep other payloads in the order required by your jailbreak environment.

### Web interface

Open **ShadowMount+** on the PS5, or use a browser on a device on the same local network:

```text
http://<PS5_IP>:10101
```

`10101` is the default port; it can be changed under `[api]` in `config.ini`. See [Web interface: library and installation](#web-interface-library-and-installation) for the available actions. **The web API has no authentication:** use a trusted LAN only, and never expose it directly to the internet.

## Supported game formats

| Extension | Filesystem / content | Recommendation |
| --- | --- | --- |
| **`.ffpkg`** | UFS2 disk image | **Preferred for most games** |
| `.exfat` | exFAT disk image | For games requiring external-drive-style compatibility |
| `.ffpfs` | PFS disk image | Supported |
| `.ffpfsc` | Compressed PFS container holding another supported image | Supported; compression can affect loading performance |
| Game folder | Extracted game files | Supported in scanned or manually specified paths |

**Choose `.ffpkg` for general use.** Use `.exfat` when a game needs external-drive compatibility; create it with a **64 KB allocation unit**. Compressed `.ffpfsc` containers are best reserved for games without demanding continuous data streaming.

### Correct directory layout

For an extracted game folder or a normal `.ffpkg`, `.exfat`, or `.ffpfs` image, the game files must be at the root:

```text
PPSA12345/
├── eboot.bin
├── sce_sys/
│   └── param.json   # or param.sfo
└── ...
```

Do **not** add another enclosing game directory inside the image. A `.ffpfsc` file is different: it contains a supported **nested image**, not loose game files at the container root.

## Game locations and scanning

ShadowMountPlus checks several built-in locations, including:

```text
/data/homebrew/
/data/etaHEN/games/
/mnt/usb0/homebrew/          # through usb7
/mnt/usb0/etaHEN/games/      # through usb7
/mnt/ext0/homebrew/          # ext0 and ext1
/mnt/ext0/etaHEN/games/      # ext0 and ext1
```

USB and external-drive roots are also scanned. Image mount locations are managed automatically.

A typical library looks like this:

```text
/data/homebrew/
├── PPSA12345/
├── CUSA12345/
├── PPSA67890.ffpkg
└── backports/
    └── PPSA12345/
```

To scan different directories, add repeatable `[scan] path` entries in `/data/shadowmount/config.ini`:

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

**Important scanning rules:**

- Setting one or more custom `path` values replaces the normal built-in scan roots (managed mounted-image paths remain available).
- `depth = 1` checks immediate subfolders; `depth = 2` checks one additional nested level.
- `interval_seconds` controls periodic rescans. New or recently copied content is deferred until it is considered stable.
- Avoid two copies of the same title ID within the scanned library to prevent duplicate registration conflicts.

### Add a game outside scan locations

Create or edit `/data/shadowmount/manual.lst` with **one absolute path per line**:

```text
/mnt/usb0/MyGames/PPSA12345
/mnt/usb0/images/PPSA67890.ffpkg
# Lines beginning with # are ignored
```

This works for supported game folders and image files. Updates to the list are detected automatically. If you delete a manually registered game from the console library, ShadowMountPlus may remove its entry to avoid installing it again; add the path back when you want to register it again.

## Web interface: library and installation

The built-in web interface is a game library and installation manager, not just a configuration page. It is available through the **ShadowMount+** launcher on the PS5 or at `http://<PS5_IP>:10101` on a trusted local network. The main tabs are **Library**, **Installation**, and **Settings**. The interface also supports TV screens and gamepad navigation.

### Library: browse and manage games

The **Library** combines discovered PS4/PS5 game folders and images with installed PKG titles. You can:

- **Find games:** search and filter titles; switch between compact and detailed game cards with covers, names, title IDs, versions, platforms, and source information.
- **Inspect a game:** open details for its source path, file size, version, installation state, and available backport. Installed PKGs also show available update/patch and DLC information, including individual DLC entries; supported data-size information includes saved data and installed content.
- **Manage folder/image games:** mount or unmount supported sources, copy or move a game to another permitted location, unpack an image into a game folder, or permanently delete the source when the operation is allowed.
- **Manage installed PKGs:** uninstall a game, or remove an installed patch or an individual DLC without uninstalling the entire base game. Installed PKGs cannot be copied, moved or unpacked through the managed-source actions.
- **Control compatibility settings:** choose **Default**, **Full**, **Emulators only**, or **Disabled** fakelib mode for a PS5 title, and optionally ignore the global fakelib. These changes take effect the next time the game starts.
- **Maintain the library:** initiate a rescan, add or remove individually registered manual sources, and inspect operation progress, errors, and logs.

**Uninstall and Delete source are different operations.** Uninstalling a registered game does not necessarily remove its folder or image. If the source remains in a scanned location, it may be discovered again. **Delete source** permanently removes the selected managed game's physical folder or image, subject to confirmation and safety checks. Back up important files before using destructive actions.

### Installation: games, updates and DLC

Open **Installation** to browse available PS5 storage, USB/external drives, and configured SMB shares. The browser shows compatible game folders, images, and PKG files, and storage destinations show available and total capacity.

- **Install from a folder or image:** select a supported PS4/PS5 game folder or `.ffpkg`, `.exfat`, `.ffpfs`, or `.ffpfsc` image, choose a local console destination, and start the installation. ShadowMountPlus copies the source and registers the copied game; the original source is retained.
- **Install PKG files:** install base games, game updates (patches), or DLC packages from console storage, external drives, or an SMB share. The required base game must be present or included in the installation workflow.
- **Install directly from a PC browser:** select PKG files on your computer and send them to the PS5 installer without first manually copying the complete packages to console storage. **Keep the browser page open until streaming completes.**
- **Use the installation queue:** collect up to 32 PKGs from supported sources and follow installation progress. PC batches can be ordered automatically as **base game → update → DLC**, so dependent content is installed in the correct sequence. Queued files are not removed from their source storage merely because they have been installed.
- **Manage files and destinations:** copy or move supported game sources, choose a writable destination, create a folder where permitted, and delete eligible files from the Installation browser with confirmation.
- **Monitor operations:** view transfer or installation progress and cancel an active operation while it remains cancellable. Some final installation or deletion steps cannot be safely interrupted.

For installed games, use the **Library** details to review updates and DLC and remove them individually. Deleting an installer `.pkg` file in **Installation** removes that file, **not** the already installed game. Conversely, uninstalling a game from **Library** does not automatically delete the original PKG installer.

### Backports: install, replace and remove

Backports can be managed from **Installation** without FTP or manual directory copying:

1. Browse to a backport folder on local, USB/external or configured SMB storage. Supported backport directories start with a PS4/PS5 title ID such as `PPSA12345` or `CUSA12345` and contain a `fakelib` directory.
2. Choose **Install backport**. The backport is copied to `/data/homebrew/backports/` using its existing folder name; the source folder is retained.
3. If a backport with that destination name is already installed, choose **Replace backport** to update it. A failed or cancelled copy preserves the previous installed version.
4. Use the backport's **Delete** action to remove the selected backport directory. Confirm the full path carefully: this is separate from uninstalling or deleting the game itself.

The Installation browser identifies supported backport folders and, where metadata is available, displays their associated game information. ShadowMountPlus automatically applies a matching backport when the game starts; there is no need to merge backport files into the base game. See [Backports, fakelib, and emulator libraries](#backports-fakelib-and-emulator-libraries) for the manual directory layout and fakelib settings.

### Settings and storage

**Settings** provides commonly used options without editing INI files, including scan paths, notifications, web access, emulator updates, fakelib defaults, and SMB connections. The interface also provides storage usage and diagnostic logs. Available USB drives can be safely ejected when they are not in use by a game or operation.

### Connect a NAS or Windows share

1. Open **Settings → SMB** in the web interface.
2. Create a connection with a recognizable name, such as `NAS`.
3. Enter the share path, for example `\\nas\games`, and any required username/password.
4. Use **Check connection** to verify access.
5. Open **Installation** or the file destination picker and select `smb://NAS`.

You can browse shares, copy or move game folders/images between the console and the share, and install compatible content from SMB to local console storage. PKG installation from a share is supported through the installer workflow.

**SMB limitations and safety:**

- **SMB2/SMB3 only**; SMB1 is unsupported.
- SMB shares are **not** used for automatic background game scanning or direct game launches. Games/images must be copied to compatible console storage first.
- Up to **16 SMB connections** can be configured.
- Guest access and `DOMAIN\user` usernames are supported where permitted by the server.
- Connections can be disabled or configured read-only.
- Saved SMB credentials are stored in `config.ini` in plaintext; protect that file and its backups.
- File deletion is available only for permitted locations and requires confirmation. Check the selected source and destination before moving or deleting files.

## Configuration

Settings live under `/data/shadowmount/`:

| File | Purpose |
| --- | --- |
| `config.ini` | Global settings, scan paths, SMB, web service, mounting, and library options |
| `profiles.ini` | Per-title fakelib preferences and per-image mount overrides |
| `manual.lst` | Manually added game folders or images |
| `debug.log` | Debug information and errors |

Most common options are available through the web interface. For complete accepted values, use [`config.ini.example`](config.ini.example) and [`profiles.ini.example`](profiles.ini.example).

### Useful global options

| Section | Examples | What they control |
| --- | --- | --- |
| `[general]` | `debug`, `quiet_mode`, `language` | Logs, notifications, localization |
| `[api]` | `enabled`, `bind_address`, `port` | Web interface/API access |
| `[scan]` | `path`, `depth`, `interval_seconds` | Library discovery |
| `[mount]` | `mode`, `persistent_images`, `exfat_backend`, `ufs_backend` | Default image-mount behavior |
| `[library]` | `install_all`, `remove_missing`, `remove_delay_seconds` | Registration and missing-title cleanup |
| `[fakelib]` | `enabled`, `default_mode`, `global_enabled`, `global_path` | Fakelib overlays |
| `[emulators]` | `update_enabled`, `path`, `auto_update_ampr` | Optional emulator-library updates |
| `[fan]` | `target_temperature` | Optional fan target adjustment |
| `[smb:NAME]` | `path`, `username`, `password`, `active` | Named network shares |

New configurations use `[config]` with `version = 2`. If a setting is omitted, its default applies. **Do not remove the version marker.** Profile changes take effect on the next game launch or image mount, rather than interrupting a running game.

### Per-game and per-image overrides

Use `profiles.ini` when one title needs different settings from the global defaults:

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

Game fakelib modes include `default`, `full`, `emulators`, and `disabled`. Image mount modes include `default`, `ro` (read-only), and `rw` (read-write). Image rules match **filenames**, not full paths, so images with the same filename on different drives share a profile.

### Upgrading from older configurations

**Configuration v2** automatically migrates older `config.ini`, `autotune.ini`, and `smb.json` settings on first launch. The originals are backed up to:

```text
/data/shadowmount/config-v1-backup/
```

Global settings and SMB shares move into `config.ini`, while individual game and image overrides move into `profiles.ini`. **Do not overwrite converted files with an old configuration.** Preserve the backup before downgrading; it may contain plaintext SMB credentials. For migration problems, review the error log and the [configuration templates](config.ini.example).

## Backports, fakelib, and emulator libraries

ShadowMountPlus can apply matching backport files to a game without requiring you to combine all files into the game folder. You can **install, replace, or remove** recognized backport folders directly from **Web interface → Installation**, as described [above](#backports-install-replace-and-remove). For manual installation, place a backport for title `PPSA12345` in:

```text
/data/homebrew/backports/PPSA12345/
```

You can also use a `backports/` directory under a configured scan root. Backport folder names may begin with the complete title ID followed by a descriptive suffix (for example `PPSA12345 Game Name`). The backport folder is not registered as a separate game. The web installer recognizes backport folders containing `fakelib` and places them under `/data/homebrew/backports/`.

For compatible titles, fakelib can be controlled globally or per game. In the web interface, select **Default**, **Full**, **Emulators only**, or **Disabled** from the game's fakelib settings. Global fakelib defaults to `/data/shadowmount/fakelib`; the emulator library directory defaults to `/data/shadowmount/emus`.

Optional AMPR auto-update is controlled by `[emulators] auto_update_ampr` and is **off by default**. You may enable it when you want automatic update checks rather than managing the library manually.

> [!IMPORTANT]
> If ShadowMountPlus fakelib integration is enabled, **disable the standalone BackPork payload**. Running both can cause conflicts. Not every game needs a backport or fakelib override; start with defaults and change only what is needed.

## Creating game images

The repository includes scripts for the recommended image formats. Supply the **game root directory** as the source, not its parent folder.

| Platform | Recommended tool | Format |
| --- | --- | --- |
| FreeBSD | [`mkufs2.sh`](mkufs2.sh) | `.ffpkg` / UFS2 |
| Windows | [UFS2Tool](https://github.com/SvenGDK/UFS2Tool) | `.ffpkg` / UFS2 |
| Linux | [`mkexfat.sh`](mkexfat.sh) | `.exfat` |
| macOS | [`mkexfat_macos.sh`](mkexfat_macos.sh) | `.exfat` |
| Windows | [`make_image.bat`](make_image.bat) with [OSFMount](https://www.osforensics.com/tools/mount-disk-images.html) | `.exfat` |

### UFS2 (`.ffpkg`)

FreeBSD:

```bash
chmod +x mkufs2.sh
./mkufs2.sh ./PPSA12345-app ./PPSA12345.ffpkg
```

Windows users can use [UFS2Tool](https://github.com/SvenGDK/UFS2Tool). In most cases, **UFS2 is the best first choice**.

### exFAT (`.exfat`)

Linux (requires `exfatprogs`, `exfat-fuse`, `fuse3`, and `rsync`):

```bash
chmod +x mkexfat.sh
./mkexfat.sh ./PPSA12345-app ./PPSA12345.exfat
```

macOS:

```bash
chmod +x mkexfat_macos.sh
./mkexfat_macos.sh ./PPSA12345-app ./PPSA12345.exfat
```

Windows (install OSFMount and run Command Prompt as Administrator; keep `make_image.bat` and `New-OsfExfatImage.ps1` together):

```bat
make_image.bat "C:\images\PPSA12345.exfat" "C:\games\PPSA12345-app"
```

These builders use the recommended **64 KB exFAT allocation unit**. If creating images manually, use the same allocation unit and place game files at the image root.

### Compressed PFS (`.ffpfsc`)

A `.ffpfsc` file is a compressed container for a nested image, created with [PSBrew/MkPFS](https://github.com/PSBrew/MkPFS). For example, once you have a normal `.exfat` image:

```bash
mkpfs pack file --verify --version PS5 --inode-bits 32 \
  ./PPSA12345.exfat ./PPSA12345.ffpfsc
```

An uncompressed PFS image can also be nested. This format can save space, but decompression adds overhead and may cause stuttering in streaming-heavy games. Verify the container before deleting the source image.

## Troubleshooting

| Problem | What to check |
| --- | --- |
| Game does not appear | Confirm the scan path, scan depth, source stability, supported title ID, and directory layout. |
| Image cannot mount | Verify its format and integrity, and check that `sce_sys/` is at the image root. |
| `.ffpfsc` does not detect a game | The container must hold a supported nested image, not loose game files. |
| Duplicate title notification | Remove duplicate registrations/sources with the same title ID. |
| Backport or fakelib causes errors | Check the matching title ID and file layout; disable standalone BackPork if ShadowMountPlus fakelib is active. |
| NAS connection fails | Verify the share name, credentials, SMB2/3 support, permissions, and that the NAS is reachable. |
| Game is registered but will not start | Check PS5 notifications and log messages; remove stale library entries before retrying. |
| Configuration changes do not apply | Check for invalid INI values and restart the affected game/image; existing active mounts are not changed immediately. |

To inspect the log, open:

```text
/data/shadowmount/debug.log
```

Debug logging is enabled by default. You can change it with `[general] debug = on` or `off` in `config.ini`. For more detailed diagnostics, see [GitHub Issues](https://github.com/drakmor/ShadowMountPlus/issues) or the [Discord community](https://discord.gg/x2Ppvzwjhm).

> [!CAUTION]
> Removing a game's data from the PS5's system settings may also delete related saves. Back up your data before using destructive troubleshooting steps.

## Additional documentation

- [Configuration example](config.ini.example) — global settings and defaults.
- [Profile example](profiles.ini.example) — per-game and per-image options.
- [API documentation](docs/api.md) — web/API endpoints and integration reference.
- [Releases](https://github.com/drakmor/ShadowMountPlus/releases) — downloads and release notes.

## Credits

**Drakmor** — development and evolution of ShadowMount into ShadowMountPlus.

Special thanks to **VoidWhisper** (original ShadowMount), **BestPig** (BackPort), **EchoStretch**, **Gezine**, **earthonion**, **LightningMods**, **RenanGBarreto** ([MkPFS](https://github.com/PSBrew/MkPFS)), **john-tornblom** (SDK), and the **PS5 R&D community**.

## License

[GPL-3.0](LICENSE)
