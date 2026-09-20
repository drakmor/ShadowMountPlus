# ShadowMountPlus 1.7

Version `1.7` is a major ShadowMountPlus update compared with `1.6`.
It focuses on the game lifecycle, performance with large libraries, safe
release of mount resources, and new management features.

> [!WARNING]
> Mounting images can still cause shutdown problems and data corruption on
> internal drives, especially on older firmware versions. Back up important
> data and configuration before updating.

## Highlights

### New SceShellCore based runtime

- Game registration and runtime mount preparation now use the SceShellCore bridge.
- By default, images are mounted on demand before a game starts and released
  after the application exits.
- Games can be switched without leaving stale mount layers behind.
- Improved handling of `ExitSpawn`/`LoadExec`, PID changes, game exit,
  suspend/resume, and payload shutdown.
- On FW 12.00+, individual games are registered through the TitleDir bridge;
  forcing `app_install_all` is no longer required.
- Firmware versions up to 13.60 are supported.

### Large libraries and faster scanning

- A reusable image index supports up to **8,192 games across 2,048 images**.
- Faster metadata reading, staging, and game registration.
- Fewer file descriptors are open at the same time.
- Faster USB library recovery after rest mode.
- The new `persistent_image_mounts=1` mode keeps discovered images mounted
  between game launches. It is disabled by default.
- Optional removal of unavailable games from the system library includes a
  delay and cancels pending removal if the source returns.
- Games with installed DLC are protected from automatic removal unless
  `auto_remove_games_with_dlc=1` is explicitly enabled.

### Web interface and HTTP/JSON API

- The built-in library interface offers search, filters, game cards, source
  management, scanning, settings, and log viewing.
- A Shadow Mount+ launcher is installed in the PS5 menu and opens the local
  web interface.
- HTTP/JSON API v1 provides access to images, games, and storage devices, as
  well as game mounting, source management, settings, and scanning.
- Background jobs can copy, move, unpack an image, or delete a source. Their
  progress is available, and they can be cancelled before an irreversible stage.
- The default address is `127.0.0.1:10101`, so the API is accessible only to
  applications running on the PS5.
- Configure `api_bind_address` and `api_port` to allow access from the local
  network.
- The release includes the [API documentation](docs/api.md),
  [OpenAPI schema](docs/openapi.yaml), [web page](web/index.html), and
  `tools/api_test.py` test client.

> [!CAUTION]
> The API has no authentication. Do not use `api_bind_address=0.0.0.0` on an
> untrusted network.

### Mount profiles and image performance

- For `.exfat`, `.ffpkg`, and standalone `.ffpfs`, the optimized profile is used
  only for direct paths under `/data/...` or `/user/...`. Other direct paths use
  the 1.6 profile. Outer `.ffpfsc` images and nested
  `.ffpfs`/`pfs_image.dat` images always use the new profile.
- The optimized exFAT profile supports LVD `img_type=5` and the BFS fast path
  for images on internal storage.
- The fast path requires the `lvd` backend, a 512-byte logical sector, and
  64 KiB exFAT clusters.
- The Linux, macOS, and Windows exFAT image creation scripts now use a fixed
  64 KiB cluster size.
- For nested PFS images, `nested_pfs_index_cache` requests the compressed
  offset index cache before attachment.
- Improved handling of asynchronous LVD/MD detach, deferred unmounts for busy
  resources, and cleanup of incomplete attachments after mount failures.
- Forced detach of MD devices was removed to avoid disrupting mounts that
  are still in use.

### fakelib and emulator updates

- Global and per-game `fakelib` are now combined in a per-title cache with
  configurable `game` or `global` priority.
- Matching game emulator files can be replaced with files from
  `emulators_path` without changing the original game folder.
- Unused fakelib caches are automatically removed seven days after the last
  game launch.
- Optional automatic checks and downloads for `libSceAmpr.sprx` updates.
- `fakelib2` always has the highest priority: it is mounted directly and is
  not combined with global `fakelib` or emulator updates.
- Fixed fakelib remounting across `ExitSpawn`/`LoadExec` transitions.
- For installed PKGs, their own `fakelib` is mounted before the game process
  starts.

### Localization, cooling, and diagnostics

- System notifications are localized into 31 languages.
- `language=auto` selects the console's system language; a language can also
  be selected explicitly.
- Added `fan_target_temperature=system|50..91`. The default `system` value
  leaves fan control to the console.
- Startup notifications show the active temperature and the result of
  backport and emulator updates more clearly.
- Expanded diagnostics for busy mounts, fatal game signals, and the reasons
  behind runtime state transitions.
- Improved the reliability of automatic Kstuff pause/resume and recovery
  after a game exits early.

## New configuration options

Key options added in 1.7:

```ini
# Notification language
language=auto

# HTTP/JSON API
api_enabled=1
api_bind_address=127.0.0.1
api_port=10101

# Runtime and library maintenance
persistent_image_mounts=0
auto_remove_missing_games=0
auto_remove_games_with_dlc=0
auto_remove_missing_delay_seconds=300

# fakelib and emulators
update_emulators=1
emulators_path=/data/shadowmount/emus
auto_update_ampr=0

# Cooling
fan_target_temperature=system
```

See `config.ini.example` for the full list of options and valid values.

## Upgrading from 1.6beta16

- An existing `/data/shadowmount/config.ini` remains compatible and does not
  have to be recreated.
- New options are not added to an existing configuration file automatically.
  If needed, copy the relevant lines from the new `config.ini.example`.
- Back up `config.ini` and `autotune.ini` before updating.
- Mount profiles are selected by source path. The old `legacy_mount_*` options
  are no longer supported and should be removed from the configuration.
  `nested_pfs_index_cache` remains available; `legacy_gddr5_cache` is accepted
  as its old name.
- The default `lvd_pfs_sector_size` changed from `32768` to `4096`.
- The default `md_ufs_sector_size` is `4096`.
- For the exFAT fast path, do not set `image_sector=...:65536`: 64 KiB is the
  cluster/secondary-unit size, while the logical sector must remain 512 bytes.
- `auto_remove_missing_games`, `persistent_image_mounts`, `auto_update_ampr`,
  and network access to the API are disabled by default.

## Compatibility

- Supports jailbroken PS5 systems with **Kstuff-lite v1.07 or newer**.
- UFS (`.ffpkg`) is the recommended format for normal use.
- PFS (`.ffpfs`/`.ffpfsc`) remains experimental.
- Running ShadowMountPlus alongside the separate BackPork payload is still
  unsupported because their fakelib mounts conflict.
