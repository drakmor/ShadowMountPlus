# ShadowMountPlus (PS5)

**Version:** `1.5beta`

Thanks for ffpkg support: @Gezine, @earthonion and @VoidWhisper for ShadowMount


**ShadowMountPlus** is a fully automated, background "Auto-Mounter" payload for Jailbroken PlayStation 5 consoles. It streamlines the game mounting process by eliminating the need for manual configuration or external tools (such as DumpRunner or Itemzflow). ShadowMount automatically detects, mounts, and installs game dumps from both **internal and external storage**.


**Compatibility:** Supports all Jailbroken PS5 firmwares running **Kstuff v1.6.7**.


## Current image support

`PFS support is experimental.`

| Extension | Mounted FS | Attach backend | Status |
| --- | --- | --- | --- |
| `.exfat` | `exfatfs` | `LVD` or `MD` (configurable) | Stable |
| `.ffpkg` | `ufs` | `LVD` or `MD` (configurable) | High performance |
| `.ffpfs` | `pfs` | `LVD` | Experimental |

Notes:
- Backend, read-only mode, and sector size can be configured via `/data/shadowmount/config.ini`.

## Recommended FS choice

- Prefer **UFS (`.ffpkg`)** for games that work correctly on UFS: it is generally more performant.
- Use **exFAT (`.exfat`)** for games that only work from external-disk style layouts, because exFAT is case-insensitive.

## Runtime config (`/data/shadowmount/config.ini`)

This file is optional. If it does not exist, built-in defaults are used.

Supported keys (all optional):
- `mount_read_only=1|0`
- `exfat_backend=lvd|md`
- `ufs_backend=lvd|md`
- `exfat_sector_size=<value>`
- `ufs_sector_size=<value>`
- `pfs_sector_size=<value>`
- `lvd_exfat_sector_size=<value>`
- `lvd_ufs_sector_size=<value>`
- `lvd_pfs_sector_size=<value>`
- `md_exfat_sector_size=<value>`
- `md_ufs_sector_size=<value>`

Validation:
- See `config.ini.example` for a ready-to-use template.

## Mount point naming

Image mountpoints are created under:

`/data/ufsmnt/<image_name>-<fs_suffix>`

## Scan paths

ShadowMount scans these locations:
- `/data/homebrew`
- `/data/etaHEN/games`
- `/mnt/ext0/etaHEN/homebrew`
- `/mnt/ext0/etaHEN/games`
- `/mnt/ext1/etaHEN/homebrew`
- `/mnt/ext1/etaHEN/games`
- `/mnt/usb0/homebrew` .. `/mnt/usb7/homebrew`
- `/mnt/usb0/etaHEN/games` .. `/mnt/usb7/etaHEN/games`
- `/mnt/usb0` .. `/mnt/usb7`
- `/mnt/ext0`
- `/mnt/ext1`
- `/data/ufsmnt` (mounted image content scan)


## Creating an exFAT image

Linux (Ubuntu/Debian):
- `sudo apt-get install -y exfatprogs exfat-fuse fuse3 rsync`
- `truncate -s <image_size> test.exfat`
- `mkfs.exfat -c 512 test.exfat`
- `mkdir -p /mnt/exfat`
- `mount -t exfat-fuse -o loop test.exfat /mnt/exfat`
- `rsync -r --info=progress2 APPXXXX/ /mnt/exfat/`
- `umount /mnt/exfat`

Windows:
- Recommended: use `make_image.bat` (wrapper for `New-OsfExfatImage.ps1` + OSFMount).
- Requirements:
  - Install OSFMount: https://www.osforensics.com/tools/mount-disk-images.html (must provide `osfmount.com`).
  - Keep `make_image.bat` and `New-OsfExfatImage.ps1` in the same folder.
  - Run `cmd.exe` as Administrator.
- Usage:
  - `make_image.bat "C:\images\game.exfat" "C:\payload\APPXXXX"`
- Behavior:
  - Auto-sizes the image to fit source content.
  - Formats and copies the source folder into the image.
  - Overwrites existing image file (uses `-ForceOverwrite`).
- Optional (fixed size): run PowerShell script directly:
  - `powershell.exe -ExecutionPolicy Bypass -File .\New-OsfExfatImage.ps1 -ImagePath "C:\images\game.exfat" -SourceDir "C:\payload\APPXXXX" -Size 8G -ForceOverwrite`

## Installation and usage


### Method 1: Manual Payload Injection (Port 9021)
Use a payload sender (such as NetCat GUI or a web-based loader) to send the files to **Port 9021**.

1.  Send `notify.elf` (Optional).
    * *Only send this if you want graphical pop-ups. Skip if you prefer standard notifications.*
2.  Send `shadowmountplus.elf`.
3.  Wait for the notification: *"ShadowMount"*.

### Method 2: PLK Autoloader (Recommended)
Add ShadowMount to your `autoload.txt` for **plk-autoloader** to ensure it starts automatically on every boot.

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

## ⚠️ Notes
* **First Run:** If you have a large library, the initial scan may take a few seconds to register all titles.
* **Large Games:** For massive games (100GB+), allow a few extra seconds for the system to verify file integrity before the "Installed" notification appears.

## Credits
* **VoidWhisper** - Lead Developer & Logic Implementation
* **Drakmor** - Evolution
* **Special Thanks:**
    * EchoStretch
    * LightningMods
    * john-tornblom
    * PS5 R&D Community

---

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/voidwhisper)
