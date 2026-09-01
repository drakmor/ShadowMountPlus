# ShadowMount HTTP/JSON API v1

The machine-readable OpenAPI 3.1 description is available in
[`openapi.yaml`](openapi.yaml).

ShadowMount exposes its public API over HTTP/TCP. The default endpoint is
`127.0.0.1:10101`, so only software running on the PS5 can connect. Set
`api_bind_address=0.0.0.0` to allow clients on the network, or select a
specific PS5 IPv4 address. `api_port` accepts values from 1 through 65535.
These settings are applied on runtime config reload. `api_enabled` defaults to
`1`; setting it to `0` stops the listener and joins the libmicrohttpd service
and worker threads. Re-enabling it restarts the HTTP service without restarting
the payload. An already-running storage job is allowed to finish, but no new
jobs can be submitted while the API is disabled.

`/data/shadowmount/index.html` overrides the web interface bundled into the
payload. If the file is absent, the same `web/index.html` built into the ELF is
served automatically, so enabling the API always provides a landing page.

The API has no authentication. Bind it to a network-facing address only on a
trusted network.

The internal `/system_tmp/shadowmount.sock` Unix socket remains private to the
SceShellCore bridge and must not be used by third-party applications.

The HTTP service uses libmicrohttpd with eight fixed polling workers. It accepts
up to 32 concurrent connections, uses a listen backlog of 32 and closes idle
connections after 15 seconds. Slow clients therefore do not serialize unrelated
requests or create an unbounded number of threads. If daemon startup fails, the
service retries with an interruptible 250 ms to 5 s backoff. Suspend, shutdown
and config reload stop the daemon cleanly; resume recreates it.

Every JSON API operation uses `POST`, includes `Content-Type: application/json`
and contains one JSON object. `GET /` returns the optional web interface and the
icon endpoint returns PNG bytes. Connections are closed after one response;
chunked request bodies are not supported. The maximum request body is 4096 bytes.
The parsed request-header limit remains 8192 bytes.

All responses include `Access-Control-Allow-Origin: *`. Browser preflight
requests using `OPTIONS` are supported and allow `GET`, `POST`, the `Content-Type`
header and Private Network Access. This permits calls from any browser origin;
the listener still needs `api_bind_address=0.0.0.0` for access from another
device.

## Routes

| Route | Request | Purpose |
| --- | --- | --- |
| `/` | GET | Serve `/data/shadowmount/index.html`, or the embedded page when it is absent |
| `/api/v1/version` | `{}` | API and ShadowMount versions plus capabilities |
| `/api/v1/storage` | `{}` | Mounted storage filesystems with total, free and available space |
| `/api/v1/images` | `{}` | Complete image snapshot |
| `/api/v1/scan` | `{"reset_attempts":false}` | Queue an immediate full rescan; optionally reset title and image retry counters |
| `/api/v1/manual/list` | `{}` | List direct paths to individual game folders or images from `manual.lst` |
| `/api/v1/manual/add` | `{"path":"/mnt/usb0/games/PPSA12345"}` | Idempotently add one game source to `manual.lst` |
| `/api/v1/manual/remove` | `{"path":"/mnt/usb0/games/PPSA12345"}` | Remove matching source lines from `manual.lst` |
| `/api/v1/settings` | `{}` | Read web-managed runtime settings and scan paths |
| `/api/v1/settings/update` | `{"debug":true,"quiet_mode":false,"update_emulators":true,"allow_lan_access":true,"fan_target_temperature":0,"scan_paths":["/mnt/usb0/games"]}` | Atomically update the web-managed part of `config.ini` |
| `/api/v1/debug-log` | `{"max_bytes":131072}` | Read a bounded tail of `debug.log` for the web log dialog |
| `/api/v1/kernel-log` | `{"max_bytes":131072}` | Read a bounded tail of the SDK kernel-log stream used by crash detection |
| `/api/v1/games` | `{"include_size":false}` | Detailed game snapshot; optional physical source-size calculation |
| `/api/v1/games/info` | `{"title_id":"PPSA12345"}` | Detailed app.db and source information for one game; size is always calculated |
| `/api/v1/games/icon?title_id=PPSA12345[&size=thumb]` | GET | Stream the full PNG or a cached 128x128 thumbnail |
| `/api/v1/games/mount` | `{"title_id":"PPSA12345","mode":"ro"}` | Mount a managed game, optionally overriding its image mode with `ro`/`rw` |
| `/api/v1/games/unmount` | `{"title_id":"PPSA12345"}` | Unmount a managed game |
| `/api/v1/games/uninstall` | `{"title_id":"PPSA12345"}` | Request uninstallation through AppInstUtil |
| `/api/v1/games/move` | `{"title_id":"PPSA12345","destination_dir":"/mnt/usb1/games"}` | Start an asynchronous move job |
| `/api/v1/games/copy` | `{"title_id":"PPSA12345","destination_dir":"/mnt/usb1/games"}` | Start an asynchronous copy job |
| `/api/v1/games/unpack` | `{"title_id":"PPSA12345","destination_dir":"/mnt/usb1/games","delete_source":false}` | Mount an image read-only and asynchronously copy the game into a folder |
| `/api/v1/games/storage/status` | `{"job_id":1}` | Get current or last storage job status; `job_id` is optional |
| `/api/v1/games/storage/cancel` | `{"job_id":1}` | Request cancellation while the active job is still cancellable |
| `/api/v1/games/delete` | `{"title_id":"PPSA12345","confirm":true}` | Start asynchronous permanent deletion of the physical source |

Example:

```sh
curl -sS http://127.0.0.1:10101/api/v1/version \
  -H 'Content-Type: application/json' \
  -d '{}'

curl -sS http://127.0.0.1:10101/api/v1/games \
  -H 'Content-Type: application/json' \
  -d '{}'

curl -sS http://127.0.0.1:10101/api/v1/scan \
  -H 'Content-Type: application/json' \
  -d '{}'

curl -sS http://127.0.0.1:10101/api/v1/games/mount \
  -H 'Content-Type: application/json' \
  -d '{"title_id":"PPSA12345","mode":"rw"}'
```

An autonomous example library UI is provided as `web/index.html`. Copy it to
`/data/shadowmount/index.html` on the PS5 and open the configured API listener
in a browser, for example `http://192.168.1.50:10101/`. The file is read for
each request, so it can be replaced without restarting ShadowMount. If it is
missing, `GET /` returns HTTP 404 with the normal JSON error response.

## Responses

Every response contains numeric `status`. It is zero on success, or a positive
errno value on failure. Error responses also contain `error` and use an
appropriate HTTP status such as 400, 404, 409, 413 or 415.

A successful version response has this shape:

```json
{
  "status": 0,
  "api_version": 1,
  "shadowmount_version": "1.7",
  "capabilities": ["web_ui", "storage_space", "list_images", "list_games", "game_info", "game_icon", "mount_game", "unmount_game", "uninstall_game", "move_game_source", "copy_game_source", "delete_game_source", "unpack_game_image", "storage_job_status", "storage_job_cancel", "list_manual_sources", "add_manual_source", "remove_manual_source", "manage_settings", "read_debug_log", "read_kernel_log", "rescan"]
}
```

List responses contain `count` and the complete `images` or `games` array.
Image items expose `path`, `mount_point`, `size`, modification time, `unit_id`,
`backend`, `complete`, `source_available`, `mapped` and `mounted`. Game items
combine ShadowMount's game cache with `tbl_contentinfo` from app.db. They expose
the physical `path`, `runtime_path`, `source_type` (`folder` or `image`), image
filesystem type, PS4/PS5 platform, title/content IDs, name, last-launch and
install timestamps, relative `icon_url`, app.db size, and runtime state.
The timestamp values from `AppInfoJson` (`#_last_access_time` and
`#_install_time`) take precedence over the stale top-level columns when they
are present.

`POST /api/v1/games` does not walk game directories by default. Set
`include_size=true` to add `size_status` and, on success, `size_bytes` to every
item. `POST /api/v1/games/info` always performs this calculation. An image uses
its file size; a folder uses the sum of its regular-file sizes.

`POST /api/v1/storage` returns capacity-bearing mounted filesystems and omits
virtual mounts such as `devfs`, `nullfs` and `tmpfs`. Each item contains the
device `source`, `mount_point`, `filesystem`, `total_bytes`, `free_bytes`,
`available_bytes`, `used_bytes` and `read_only`. `available_bytes` is the space
available for new files; `used_bytes` is calculated against that value.

`copy` and `move` preserve the source basename and require an existing
destination under a configured non-runtime scan root. They and `delete` return
HTTP 202 with a `job_id` and continue in one background worker;
only one of these jobs can be active. A second start returns HTTP 409/`EBUSY`.
`delete` still requires `confirm=true`. The status response contains the phase,
byte-based percentage, processed/total bytes,
processed/total files, average processed-byte rate, elapsed time and final
errno-style result. The initial `measuring` phase discovers the totals and has
zero percent until they are known. Only the active or most recently finished
job is retained; an older explicit `job_id` returns HTTP 404.

Cancellation is cooperative during `preparing`, `measuring` and `transferring`.
A partial copy destination is removed. A delete job becomes non-cancellable
when it enters `deleting`, before its first physical removal. Likewise, once a
cross-filesystem move enters
`finalizing` and starts deleting the complete source, cancellation is rejected
to avoid leaving the only source tree partially removed. The scanner and
ShellCore mutation gates remain held for the whole job and a rescan is queued
after success, cancellation or failure. `move` uses `rename()` on one
filesystem and exact copy-plus-delete across filesystems. A backing image
shared by several titles is handled as one physical source.
`unpack` uses the same worker and progress endpoint. It mounts the requested
image read-only, copies the mounted game tree to `<destination_dir>/<TITLE_ID>`,
and always releases the runtime mount afterward. `delete_source=true` removes
the image only after a complete copy and successful unmount; this irreversible
final phase is non-cancellable. Source deletion is rejected when one image is
shared by several titles.
Changing the API bind address or port restarts only the HTTP listener; an
active storage job continues and remains available through the new listener.

Mount mutations remain conservative. They return HTTP 409 with `status` set to
`EBUSY` while a game is active, while ShellCore owns another prepared title,
during suspend, or while a conflicting mount/release is in progress. Clients
must not retry this response in a tight loop. HTTP 404 with `ENOENT` means the
title has no ShadowMount `mount.lnk` tracker.
The optional mount `mode` is request-scoped and does not change `config.ini`.
Accepted values are `ro`, `rw`, `r/o` and `r/w`; responses normalize them to
`ro` or `rw`. It applies to every image layer needed for that title. A direct
directory source or an image profile that is inherently read-only rejects an
explicit mode it cannot support with HTTP 501 and `ENOTSUP`. An already mounted
image in the other mode returns HTTP 409 and `EBUSY`.
With `persistent_image_mounts=1`, unmounting a game releases its per-title
runtime layers but keeps the backing image mounted.

The scan command queues the existing full-scan path and wakes the scanner. If a
game runtime mount is active, the request remains pending until the lifecycle
watcher reports that scanning is safe; it does not create a parallel scan.
By default retry counters are preserved. With `reset_attempts=true`, immediately
before the requested full scan starts it clears all title registration and
install/remount counters plus all path-owned image-mount counters. If several
scan requests merge while one is pending, one `true` request keeps the reset
enabled for that scan.

Manual entries are direct paths to individual game directories or image files;
they are not recursive scan roots. Updates atomically replace `manual.lst` and preserve comments and
unrelated lines. Adding an existing normalized path and removing a missing path
both succeed with `changed: false`. A real change queues a scan. Removing a
manual source does not uninstall its game; use the uninstall route separately
when both operations are desired.
The list route returns `{status, count, paths}`. Paths use the same trimming and
trailing-slash normalization as the scanner; comments and empty lines are not
included. A missing `manual.lst` is reported as an empty list.

The settings update owns only `debug`, `quiet_mode`, `update_emulators`,
`api_bind_address`, `fan_target_temperature`, and repeated `scanpath` keys. The
`allow_lan_access` field maps to `0.0.0.0` when enabled and `127.0.0.1` when
disabled. It atomically rewrites those keys while preserving every unrelated
`config.ini` line and comment, then wakes the scanner so its existing
runtime-reload path applies the values and rebuilds scan watches. `api_enabled`
is deliberately config-only, so the web interface cannot disable the service it
needs to restore access. A fan target of `0` means system control; explicit
values must be 50 through 91 degrees Celsius. Unlike `manual.lst`, `scan_paths`
are recursive library roots. The settings response contains only explicit
custom roots, never compile-time defaults or internal image-mount roots. Saving
an empty array removes the custom override and restores compile-time defaults.
The debug-log route returns at most 256 KiB and never modifies or rotates the log.
The kernel-log route reads the same `sceKernelDebugGetSdkLogText` snapshot used
by `kstuff_crash_detection`.

Uninstall returns success after `sceAppInstUtilAppUnInstall` accepts the
request; the on-disk removal itself is asynchronous. It returns `EBUSY` while a
game is active, another title owns a prepared runtime mount, or batch install
work is pending. An unchanged discoverable source may be installed again by a
later scan, so remove its manual entry or source first when that is not wanted.

## Test client

Run the host-side Python client while ShadowMount is active:

```sh
python3 tools/api_test.py 192.168.1.50 10101
```

It validates CORS preflight, the version response and complete image/game
snapshots. It also runs 16 parallel version requests while one client holds an
incomplete request open, then selects the first safe managed game for a
mount/unmount cycle. The mutation test is skipped if there is no candidate or
the runtime is busy.
