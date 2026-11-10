# SNAPSHOT.md

Architecture blueprint of ShadowMountPlus (zstd fork). Read this first instead of scanning
the repo. Last updated: 2026-11-10 (Trail-code map added).

## What it is
Background payload (`shadowmountplus.elf`) for jailbroken PS5: discovers PS4/PS5 game
folders and disk images (`.ffpkg` UFS, `.exfat`, `.ffpfs`/`.ffpfsc` PFS) on internal/USB/M.2
storage, mounts and registers them, prepares fakelib/backports at launch, installs PKGs,
and serves a web UI + JSON API. ~46k lines of C in `src/`, headers in `include/`.

## Repository layout
| Path | Contents |
| --- | --- |
| `src/main.c` | Startup: config load/migration, services, main loop |
| `src/sm_*.c` / `include/sm_*.h` | Modules (one header per module) |
| `src/sm_shellcore_bridge.S` | Position-independent SceShellCore hook bridge |
| `web/index.html` | Single-file web UI (embedded as asset) |
| `docs/api.md`, `docs/openapi.yaml` | HTTP API docs |
| `config.ini.example`, `profiles.ini.example` | Config templates (new INI format, `[config] version=2`) |
| `tests/`, `tools/test_*.py` | Host regression tests (Linux), Python drivers compile C tests |
| `tools/` | Offset generators, l10n tools, json-c build script |
| `include/lang/*.inc` | Localization catalogs |
| `.github/workflows/ps5.yml` | CI build with PS5 SDK |
| `.trail/`, `.claude/settings.json` | Trail-code learnings + Claude Code SessionStart hook |
| `graft/` (git-ignored) | Trail-code code map cache, rebuilt with `trail build` |
| `mkexfat*.sh`, `mkufs2.sh`, `New-OsfExfatImage.ps1`, `make_image.bat` | Image creation helpers |

## Module map (by area)
- **Config:** `sm_config_file` (INI read/write, `sm_config_keys[]` mapping, legacy migration),
  `sm_config_mount` (runtime_config snapshots, parsing, reload/watch), `sm_types.h`
  (`runtime_config_t`), `sm_paths.h` (default paths/URLs), `sm_limits.h`.
- **Scanning / library:** `sm_scanner` (service, watches), `sm_scan` (scan + stale cleanup),
  `sm_scan_tree`, `sm_stability`, `sm_game_cache`, `sm_path_state`, `sm_title_state`,
  `sm_gameinfo`, `sm_appdb` (app.db via sqlite), `sm_manual` (manual.lst).
- **Mounting:** `sm_image` (attach/mount images), `sm_image_cache`, `sm_image_index`,
  `sm_mount_device` (LVD/MD backends), `sm_mount_diag`, `sm_filesystem` (mount table,
  copy helpers, `sm_file_is_ampr_ng`), `sm_usb`, `sm_storage`.
- **Install / PKG:** `sm_install`, `sm_install_queue`, `sm_pkg`, `sm_pkg_server` (HTTP range
  server), `sm_pkg_stream`, `sm_pkg_ws`, `sm_pkg_backport`, `pkg_helper_asset.c`.
- **Game launch / runtime:** `sm_game_lifecycle`, `sm_fakelib` (backport fakelib overlay,
  global fakelib, emulator replacement cache), `sm_ampr_updater` (AMPR + ampr-emu-ng
  auto-update over HTTPS), `sm_fan`, `sm_mdbg` (game error monitoring).
- **ShellCore integration:** `sm_shellcore_hooks`, `sm_shellcore_remote`,
  `sm_shellcore_service`, `sm_shellcore_flags`, `sm_shellcore_offsets`, `sm_self`, `sm_kstuff`.
- **Network / UI:** `sm_api_service` (largest file: HTTP API via libmicrohttpd),
  `sm_icon_thumb`, `sm_shell_icon`, `sm_smb` + `sm_vfs` (SMB2/3 via libsmb2), `sm_socket_io`.
- **Misc:** `sm_log`, `sm_l10n`, `sm_runtime` (signals), `sm_time`, `sm_hash`, `sm_path_utils`.

## AMPR / zstd flow
- `libSceAmpr.sprx` in a game's `fakelib/` is the AMPR emulator. The upstream one is
  `drakmor/ampr_emu`; `quer3q/ampr-emu-ng` reads zstd/LZ4 packs (`/app0/ampr_zstd.index`).
- `sm_fakelib` detects ng builds via `sm_file_is_ampr_ng()` and only replaces them from
  `<emulators_path>/ampr-emu-ng/libSceAmpr.sprx`; upstream files from `<emulators_path>/`.
- `sm_ampr_updater` with `auto_update_ampr` updates both files; the ng file is only upgraded
  when it already exists (`[emulators] ampr_ng_update_url`).

## Build
`make` with `PS5_PAYLOAD_SDK` (default `/opt/ps5-payload-sdk`); builds private json-c 0.19
into `.deps/`. Links libmicrohttpd, libpng, turbojpeg, zlib, libsmb2, sqlite3, Sce* libs.
