# ShadowMount HTTP/JSON API v1

The machine-readable OpenAPI 3.1 description is available in
[`openapi.yaml`](openapi.yaml).

ShadowMount exposes its public API over HTTP/TCP. The default endpoint is
`127.0.0.1:10101`, so only software running on the PS5 can connect. Set
`api_bind_address=0.0.0.0` to allow clients on the network, or select a
specific PS5 IPv4 address. `api_port` accepts values from 1 through 65535.
Both settings are applied on runtime config reload.

The API has no authentication. Bind it to a network-facing address only on a
trusted network.

The internal `/system_tmp/shadowmount.sock` Unix socket remains private to the
SceShellCore bridge and must not be used by third-party applications.

The HTTP service uses libmicrohttpd with four fixed polling workers. It accepts
up to 16 concurrent connections, uses a listen backlog of 32 and closes idle
connections after 5 seconds. Slow clients therefore do not serialize unrelated
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
| `/` | GET | Serve `/data/shadowmount/index.html` when it exists |
| `/api/v1/version` | `{}` | API and ShadowMount versions plus capabilities |
| `/api/v1/storage` | `{}` | Mounted storage filesystems with total, free and available space |
| `/api/v1/images` | `{}` | Complete image snapshot |
| `/api/v1/scan` | `{"reset_attempts":false}` | Queue an immediate full rescan; optionally reset title and image retry counters |
| `/api/v1/manual/list` | `{}` | List normalized source paths from `manual.lst` |
| `/api/v1/manual/add` | `{"path":"/mnt/usb0/games/PPSA12345"}` | Idempotently add a source to `manual.lst` |
| `/api/v1/manual/remove` | `{"path":"/mnt/usb0/games/PPSA12345"}` | Remove matching source lines from `manual.lst` |
| `/api/v1/games` | `{"include_size":false}` | Detailed game snapshot; optional physical source-size calculation |
| `/api/v1/games/info` | `{"title_id":"PPSA12345"}` | Detailed app.db and source information for one game; size is always calculated |
| `/api/v1/games/icon?title_id=PPSA12345` | GET | Stream the PNG referenced by app.db `icon0Info` |
| `/api/v1/games/mount` | `{"title_id":"PPSA12345","mode":"ro"}` | Mount a managed game, optionally overriding its image mode with `ro`/`rw` |
| `/api/v1/games/unmount` | `{"title_id":"PPSA12345"}` | Unmount a managed game |
| `/api/v1/games/uninstall` | `{"title_id":"PPSA12345"}` | Request uninstallation through AppInstUtil |
| `/api/v1/games/move` | `{"title_id":"PPSA12345","destination_dir":"/mnt/usb1/games"}` | Move the physical game folder or backing image, with cross-filesystem fallback |
| `/api/v1/games/copy` | `{"title_id":"PPSA12345","destination_dir":"/mnt/usb1/games"}` | Copy the physical game folder or backing image |
| `/api/v1/games/delete` | `{"title_id":"PPSA12345","confirm":true}` | Permanently delete the physical game folder or backing image |

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
  "capabilities": ["web_ui", "list_images", "list_games", "game_info", "game_icon", "mount_game", "unmount_game", "uninstall_game", "move_game_source", "copy_game_source", "delete_game_source", "list_manual_sources", "add_manual_source", "remove_manual_source", "rescan"]
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

Storage operations preserve the source basename and require an existing
destination under a configured non-runtime scan root. They are synchronous,
hold the scanner/ShellCore mutation gates, release affected runtime mounts, and
queue a rescan after success. `move` uses `rename()` on one filesystem and exact
copy-plus-delete across filesystems. `delete` requires `confirm=true`. A backing
image shared by several titles is handled as one physical source.

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

Manual source updates atomically replace `manual.lst` and preserve comments and
unrelated lines. Adding an existing normalized path and removing a missing path
both succeed with `changed: false`. A real change queues a scan. Removing a
manual source does not uninstall its game; use the uninstall route separately
when both operations are desired.
The list route returns `{status, count, paths}`. Paths use the same trimming and
trailing-slash normalization as the scanner; comments and empty lines are not
included. A missing `manual.lst` is reported as an empty list.

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
