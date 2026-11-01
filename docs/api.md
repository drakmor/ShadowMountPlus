# ShadowMount HTTP/JSON API v1

The machine-readable OpenAPI 3.1 description is available in
[`openapi.yaml`](openapi.yaml).

ShadowMount exposes its public API over HTTP/TCP. By default it listens on
`0.0.0.0:10101`, allowing clients on the local network to connect through the
PS5's IPv4 address. Listener configuration is described in the
[runtime configuration reference](../README.md#runtime-configuration).

The API has no authentication. Bind it to a network-facing address only on a
trusted network.

The service accepts up to 32 concurrent connections and closes idle
connections after 15 seconds. The listener resumes automatically after suspend.

Every JSON API operation uses `POST`, includes `Content-Type: application/json`
and contains one JSON object. `GET /` returns the web interface; icon endpoints
return PNG or JPEG bytes. Ordinary API connections close after one response;
the streaming endpoints described below support persistent connections and
binary uploads. Chunked request bodies are not supported. The maximum JSON
request body is 4096 bytes; the request-header limit is 8192 bytes.

All responses include `Access-Control-Allow-Origin: *`. Browser preflight
requests using `OPTIONS` are supported and allow `GET`, `POST`, the `Content-Type`
header and Private Network Access. This permits calls from any browser origin;
local network access must still be enabled for connections from another device.

## Routes

| Route | Request | Purpose |
| --- | --- | --- |
| `/` | GET | Serve the web interface |
| `/api/v1/version` | `{}` | API and ShadowMount versions plus capabilities |
| `/api/v1/storage` | `{}` or `{"path":"smb://NAS"}` | Local storage listing and active SMB destinations, or capacity for one SMB path |
| `/api/v1/storage/eject` | `{"path":"/mnt/usb0","source":"/dev/da1p1","mount_id":"1122334455667788"}` | Safely unmount an external USB drive; returns an asynchronous storage job |
| `/api/v1/images` | `{}` | Complete image snapshot |
| `/api/v1/scan` | `{"reset_attempts":false}` | Queue an immediate full rescan; optionally reset title and image retry counters |
| `/api/v1/manual/list` | `{}` | List manually registered game folders or images |
| `/api/v1/manual/add` | `{"path":"/mnt/usb0/games/PPSA12345"}` | Idempotently register one game source |
| `/api/v1/manual/remove` | `{"path":"/mnt/usb0/games/PPSA12345"}` | Remove a manually registered source |
| `/api/v1/settings` | `{}` | Read API-managed runtime settings and scan paths |
| `/api/v1/smb` | `{"action":"list"}` | List, save, remove or check named SMB2/SMB3 connections |
| `/api/v1/sources/delete` | `{"path":"smb://NAS/Game.pkg","confirm":true}` | Start asynchronous deletion of a permitted folder, image or PKG |
| `/api/v1/files/mkdir` | `{"path":"smb://NAS","name":"New folder"}` | Create one directory beneath an existing local or SMB parent; return its full path |
| `/api/v1/settings/update` | `{"debug":true,"quiet_mode":false,"update_emulators":true,"fakelib_default_mode":"full","auto_update_ampr":false,"auto_remove_missing_games":false,"auto_remove_missing_delay_seconds":300,"allow_lan_access":true,"fan_target_temperature":0,"scan_paths":["/mnt/usb0/games"]}` | Persist API-managed runtime settings |
| `/api/v1/debug-log` | `{"max_bytes":131072}` | Read a bounded tail of `debug.log` |
| `/api/v1/kernel-log` | `{"max_bytes":131072}` | Read a bounded tail of the kernel log |
| `/api/v1/games` | `{"include_size":false}` | Detailed game snapshot; optional physical source-size calculation |
| `/api/v1/games/info` | `{"title_id":"PPSA12345"}` | Detailed installed-game and source information for one game; size is always calculated |
| `/api/v1/games/ps4-profile` | `{"title_id":"CUSA12345","pro":"default","cpu":"default","gpu":"default"}` | Save per-game PS4 profile overrides for the next launch |
| `/api/v1/games/patches` | `{"title_id":"CUSA12345"}` | List XML patch IDs, names, versions, modules, requirements, compatibility and selection state |
| `/api/v1/games/patches` | `{"title_id":"CUSA12345","id":"0123456789abcdef","enabled":true}` | Persist one XML patch selection for the next launch; incompatible/unsupported entries cannot be enabled |
| `/api/v1/games/fakelib` | `{"title_id":"PPSA12345","mode":"emulators"}` | Persist the PS5 game's fakelib policy for its next launch |
| `/api/v1/games/fakelib` | `{"title_id":"PPSA12345","ignore_global_fakelib":true}` | Ignore the global fakelib overlay for this PS5 game on its next launch |
| `/api/v1/games/icon?title_id=PPSA12345[&size=thumb\|card\|cover]` | GET | Stream the original PNG or a cached icon variant |
| `/api/v1/games/mount` | `{"title_id":"PPSA12345","mode":"ro"}` | Mount a managed game, optionally overriding its image mode with `ro`/`rw` |
| `/api/v1/games/unmount` | `{"title_id":"PPSA12345"}` | Unmount a managed game |
| `/api/v1/games/uninstall` | `{"title_id":"PPSA12345"}` | Request game uninstallation |
| `/api/v1/games/content/uninstall` | `{"title_id":"PPSA12345","kind":"patch"}` or `{"title_id":"PPSA12345","kind":"dlc","content_id":"…"}` | Remove only the patch or the specified installed DLC |
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

## Files and SMB connections

`/packages` includes `root_storage` records with available and total filesystem
capacity for local roots. Folders on the same filesystem share its capacity;
these values are not recursive folder-size measurements.

`POST /api/v1/files/mkdir` accepts `path` for an existing parent and `name` for
one new directory, and returns its full path. `/files` reports
`can_create_directory` for the selected parent. The name must fit 255 UTF-8
bytes and cannot contain path separators, control characters or reserved
characters. Existing paths are not overwritten. Read-only and system parents
are rejected; writable scan roots remain valid parents. Creating a directory
does not register a game or request a scan.

Installation listings include `can_delete` for each entry.
`POST /api/v1/sources/delete` requires an exact `path` and boolean
`confirm:true`; it accepts local browser roots and SMB aliases and returns
HTTP 202 with the storage-job response. The root itself cannot be deleted.
Local system and read-only paths are protected. Local folders and images in
scan roots, and folders containing scan roots, cannot be deleted here; PKGs in
scan roots are allowed. Active/prepared-game protection applies. Deletion of
registered local sources or failed runtime cleanup requests a recovery scan;
unregistered-file and SMB-only deletion do not. Cancellation is available
during preparation/measurement and closes when deletion starts.

`POST /api/v1/storage` with `{"path":"smb://NAS"}` queries capacity for one
active saved SMB connection or a directory beneath it. It returns `status`,
`path`, `filesystem`, `remote`, `total_bytes`, `free_bytes`, `available_bytes`,
`used_bytes` and `read_only`. Available space uses the server's `f_bavail`,
which can reflect the connected user's quota. Errors affect only that request.
The default `{}` storage listing does not contact SMB servers.

`POST /api/v1/smb` accepts `action=list|save|remove|check` (default `list`).
`save` and `check` accept `name`, `path`, `username`, `password`, `active`, `read_only` and
optional `previous_name`. `name` is a unique alias of at most 63 UTF-8 bytes;
slashes, control characters and `: @ ? # %` are forbidden. `path` is an actual
`smb://server/share[/subdirectory]` address, optionally with `server:port`.
An empty user name uses Guest; `DOMAIN\user` is accepted. New connections are
active by default. `previous_name` selects a saved connection for editing or
checking; omitted fields retain their saved values except `name` and `path`,
which remain required. An omitted password preserves it; `"password":""`
explicitly clears it. `remove` requires only `name`.
`read_only` defaults to `false`; when enabled, all mutations through that alias
are rejected with `EROFS`. Browsing, copying from SMB and PKG streaming remain available.

Successful responses contain `status=0`, `dialect` (zero except after `check`),
and `connections`: objects containing `name`, `path`, `username`, `active`, `read_only` and
`password_set`. Passwords are never returned. `check` tests the submitted
settings without saving, including an inactive connection, and reports the
negotiated SMB2/SMB3 dialect (for example 0x0210 or 0x0311). It verifies directory
access, not write permission. `save` validates syntax and persists up to 16
connections.
Save/remove return `EBUSY` while a storage job or PKG queue is active.

Active aliases are exposed as `smb://<name>[/relative/path]` in `/packages`,
`/files`, and `/storage.destinations`. Remote destinations have `remote=true`
and `filesystem=smb`; capacity fields are omitted to avoid network polling.
`/packages` and `/files` return a `limit` field: 32 remote entries or 256 native
entries per page. Advance by this limit when `truncated=true`.
`/games/copy`, `/games/move` and `/games/unpack` accept an SMB destination.
`/sources/install` accepts SMB folders and images as sources with a local
destination. `/packages/install` and the package queue accept SMB PKGs; their
source must remain available until installation completes or is cancelled.
No SMB path is added to scanner roots or `manual.lst`;
installed folder/image copies are registered using their local destination.

SMB operations use SMB2/SMB3. Cross-filesystem moves delete the source only
after the complete destination has been copied and flushed. Failed or
cancelled copies preserve their source. If the share becomes unreachable,
partial destination cleanup may require manual removal after reconnecting.
Files copied from SMB to local storage receive permissions `0777`; failure
to set them fails the copy.

## Responses

JSON responses include `X-ShadowMount-Local-Time` when the console's local-time
conversion succeeds. Its integer value is Unix seconds shifted by the console's
current timezone and daylight-saving offset. Treat it as local wall-clock fields
(decode with UTC calendar functions), not as an absolute UTC timestamp.
Older payloads omit this optional header.

JSON API responses contain numeric `status`. It is zero on success, or a positive
errno value on failure. Error responses also contain `error` and use an
appropriate HTTP status such as 400, 404, 409, 413 or 415.

A successful version response has this shape:

```json
{
  "status": 0,
  "api_version": 1,
  "shadowmount_version": "1.7",
  "capabilities": ["web_ui", "storage_space", "list_images", "list_games", "game_info", "game_data_sizes", "game_icon", "mount_game", "unmount_game", "uninstall_game", "uninstall_game_content", "move_game_source", "copy_game_source", "delete_game_source", "unpack_game_image", "storage_job_status", "storage_job_cancel", "list_manual_sources", "add_manual_source", "remove_manual_source", "manage_settings", "read_debug_log", "read_kernel_log", "rescan"]
}
```

List responses contain `count` and the complete `images` or `games` array.
Homebrew IDs contain four uppercase ASCII letters and five digits, excluding
`CUSA`, `PPSA`, and `NPXS` prefixes. Confirmed running homebrew does not defer scans or block
source copy/move/delete operations and uninstalls for unrelated titles. This
applies to folder, image and installed-PKG applications. Ordinary PS4/PS5 games
allow the same work regardless of AppFocus, including while in the foreground. The
live application's own title, source, backing image chain, mounted fakelib
overlay source and retained NSFS backport sources remain protected;
operations affecting them return `game_active`. Pending launches/exits and
unknown runtime ownership defer new work. Scan requests use the same protection.

Image items expose `path`, `mount_point`, `size`, modification time, `unit_id`,
`backend`, `complete`, `source_available`, `mapped` and `mounted`. Game items expose
the physical `path`, `runtime_path`, `source_type` (`folder` or `image`), image
filesystem type, PS4/PS5 platform, title/content IDs, name, last-launch and
install timestamps, game `version`, relative `icon_url`, app.db size, and runtime state.
`backport_path` contains the matching backport directory,
or an empty string when absent. This reports presence independently of
fakelib policy or whether an overlay is currently mounted.

Icon requests without `size` return the original PNG. `size=thumb` returns a
PNG capped at 128 pixels. `size=card` and `size=cover` are capped at 256 and 512
pixels and may return JPEG or PNG, as indicated by `Content-Type`. Aspect ratio
is preserved and small sources are never enlarged. If a variant is unavailable,
the original PNG is returned. Icon URLs are versioned when the source changes.

`version` is an empty string when unavailable.

`POST /api/v1/games` does not walk game directories by default. Set
`include_size=true` to add `size_status` and, on success, `size_bytes` to every
item. `POST /api/v1/games/info` always performs this calculation. An image uses
its file size; a folder uses the sum of its regular-file sizes. Details measure
the physical base `app.pkg` for installed packages. The library list can use
the recorded installed total for those titles, while `app_db_size_bytes`
remains the system reference in both responses.

`games/info` also returns `save_data_size_status`, `download_size_status`,
`patch_size_status` and `dlc_size_status`.
On success (status `0`), the matching `save_data_size_bytes`,
`download_size_bytes`, `patch_size_bytes` or `dlc_size_bytes` field contains
the sum of regular-file lengths. Missing
directories are successful zero sizes; errors omit the corresponding byte count.
Saves are summed across local
profiles and include backups within the title directory, but exclude cloud,
metadata and profile-level backup folders. Patch and DLC totals include matching
internal and extended-storage installations without counting aliases twice.
Any access, cancellation or measurement failure returns that category's
errno-style status and omits its bytes, without publishing a partial total.
Symlink directories are not followed. Measurements stop on shutdown or rest mode.
Separate patch files are measured;
updates merged into the base package remain part of the game's main size.

Details also return `patch_info_status`, a `patch` object (`installed`,
`version`, `size_bytes`), `dlc_info_status`, a `dlc` array and
`dlc_info_truncated`. The DLC list is capped at 256 records. Records contain
`content_id`, `title_name`, `version`, `size_status` and, when valid,
`size_bytes`. Metadata errors leave filesystem size results intact.
Library entries include `patch_info_status` and `dlc_info_status`, with
`has_patch` and `dlc_count` present only when their respective query succeeds.

`base_version` reports the original base-PKG version in game details, or an
empty string if unavailable. Patch detection and `base_version` apply only to
PKG games.

Game entries and details include `active` for the currently running game.
Optional `play_time_seconds` reports the foreground console user's system
play-time total. It is omitted when that statistic is unavailable.
Unpopulated zero counters are treated as unavailable, rather than zero time played.

`POST /api/v1/games/content/uninstall` removes only the patch (`kind=patch`) or
the DLC identified by `content_id` (`kind=dlc`). Runtime protections and
conflicting storage operations can block the request. The
`uninstall_game_content` capability advertises this route.

`system_content_size_status` and, on success, `system_content_size_bytes`
report the system-accounted installed total, separately from physical source
size. The `game_data_sizes` capability advertises the size fields.

`POST /api/v1/storage` with `{}` returns capacity-bearing mounted filesystems and omits
virtual mounts such as `devfs`, `nullfs` and `tmpfs`. Each item contains the
device `source`, `mount_point`, `filesystem`, `total_bytes`, `free_bytes`,
`available_bytes`, `used_bytes` and `read_only`. `available_bytes` is the space
available for new files; `used_bytes` is calculated against that value.
Local entries in the `destinations` array contain writable, currently available managed
`/data/homebrew`, `/mnt/ext*/homebrew` and `/mnt/usb*/homebrew` paths. Scanned
`/mnt/ext*` and `/mnt/usb*` roots are included as additional destinations. Each
item adds the selectable `path` to the same filesystem metadata. Writable nullfs
aliases such as `/data/homebrew` are valid destinations but remain omitted from
the mount overview to avoid duplicates. Active SMB aliases are additional remote
destinations; their capacity is queried separately as described above.
Their `source` is the configured server/share path.
`/mnt/ext1/homebrew` is also advertised
when its writable ext1 mount is available and only the homebrew folder is absent;
listing storage does not create it. The folder is created on an explicit
copy/move/unpack request. Other missing destination directories remain invalid.

`storage_eject` advertises `POST /api/v1/storage/eject`. Mount items add
`ejectable`: true for `/mnt/usb0..7` and extended USB storage `/mnt/ext0`;
internal storage, M.2 (`/mnt/ext1`), and SMB destinations are excluded. Send the
`mount_point` as `path`, its current `source`, and the opaque 16-character
hexadecimal `mount_id` from the same listing unchanged. It identifies the
mounted filesystem so a replacement disk cannot inherit an old request.
Missing or malformed IDs return HTTP 400; a stale
source or ID returns HTTP 409 (`usb_mount_changed`). Identity is checked again
immediately before issuing the system command. The accepted response is HTTP 202 with `operation=eject`;
poll the existing `/games/storage/status` route. Cancellation is possible only
before finalizing starts. `completed` means the selected filesystem has been
unmounted, not merely that the system accepted the request. `usb_eject_unconfirmed`
means keep the drive connected and refresh storage; no automatic retry occurs.

Ejection is blocked when the selected volume contains the live game's source
or backing chain, or its native PKG storage cannot be resolved. Unrelated
volumes may be ejected during gameplay. Pending installation, launch/exit
transitions, prepared inactive runtime mounts, attached images,
ShadowMount-owned title mounts and other mounted partitions of the same
physical device also block ejection. Dependency failures distinguish
`usb_mount_busy`, `usb_partition_busy` and `usb_image_busy`. A failed job
includes `blocking_source` and `blocking_target`: a physical image path, or the
source and target of one blocking mount. Empty fields mean no dependency path
is available. Idle applications may retain mounts or attached images, so these
paths do not imply that an application is running. Release the listed
dependency and retry explicitly. Dependency details are cleared when a new
job starts. `usb_disk_busy` identifies other dependency/precondition failures;
`usb_eject_failed` indicates a system error.

`copy` and `move` preserve the source basename and require an existing
destination under a configured non-runtime scan root (with the ext1 homebrew
creation exception above). They and `delete` return
HTTP 202 with a `job_id`; only one storage job can be active.
A second start returns HTTP 409/`EBUSY`.
`delete` still requires `confirm=true`. If a cached source is already absent
(`ENOENT` or `ENOTDIR`), it instead returns HTTP 200 with `source_missing=true`
and `scan_queued=true`, without creating a job or directly uninstalling the title.
An absent source can also mean disconnected storage; permission and I/O errors
remain failures. Unknown titles still return HTTP 404.
The status response contains the phase,
byte-based percentage, processed/total bytes,
processed/total files, average processed-byte rate, elapsed time and final
errno-style result. The initial `measuring` phase discovers the totals and has
zero percent until they are known. Only the active or most recently finished
job is retained; an older explicit `job_id` returns HTTP 404.

`scan`, successful `uninstall`, and missing-source delete responses include
`scan_queued`, `scan_deferred`, and `scan_deferred_reason`. The reason is
`game_active`, `runtime_prepared`, `rest_mode`, or an empty string. These fields
describe the runtime at response time; queue acceptance is not scan completion.
A successful uninstall request queues a full synchronization only when its
known source was already absent before uninstall. Retained sources do not
trigger immediate rediscovery; `scan_queued=false` reports this case. Uninstall
does not exclude or delete the source, so a later scan can still discover it.
Busy uninstall/storage responses add a stable `error_reason` code; storage jobs
expose the corresponding `result_error_reason`. The English `error` and
`result_error` remain available for clients without localized catalogs. Reasons distinguish
game activity, pending installation, mutation-gate contention, prepared runtime,
and busy mount release. Close a game fully before retrying; returning to the
home screen alone does not terminate it.
If the source root disappears after a delete job is accepted, an absent root is
treated as already deleted. A missing child in an existing tree, I/O errors,
permission errors, and cancellation remain failures.

Cancellation is cooperative during `preparing`, `measuring` and `transferring`.
A partial copy destination is removed. A delete job becomes non-cancellable
when it enters `deleting`, before its first physical removal. Likewise, once a
cross-filesystem move enters
`finalizing` and starts deleting the complete source, cancellation is rejected
to avoid leaving the only source tree partially removed. A rescan is queued
after success, cancellation or failure. Cross-filesystem moves delete the
source only after copying the complete destination. A backing image
shared by several titles is handled as one physical source.
`unpack` uses the same job status and cancellation endpoints. It mounts the requested
image read-only, copies the mounted game tree to `<destination_dir>/<TITLE_ID>`,
and always releases the runtime mount afterward. `delete_source=true` removes
the image only after a complete copy and successful unmount; this irreversible
final phase is non-cancellable. Source deletion is rejected when one image is
shared by several titles.
Changing the API bind address or port restarts only the HTTP listener; an
active storage job continues and remains available through the new listener.

The game list combines discovered folder/image sources with installed PS4 and
PS5 PKGs from `app.db`, identified by an existing `app.pkg`. Title IDs appear
once; managed folder/image sources retain their normal actions. Installed PKGs
have `source_type: "pkg"`, `installed_pkg: true`, `managed: false` and
`mounted: false`. Their `path` points to `app.pkg`; `runtime_path` is empty.
They support information, icons and uninstall, plus fakelib configuration for
PS5. Mount/unmount and source copy/move/delete/unpack reject PKGs with HTTP 403
and `EPERM`. Library PKG size uses the installed size in `app.db`, falling back to the
package file size; folder/image measurement remains unchanged.

Game responses also provide `can_uninstall`, `can_manage_source`,
`can_toggle_fakelib`, `fakelib_enabled` and `fakelib_effective_enabled`.
`fakelib_mode` is the resolved mode (`full`, `emulators`, `disabled`),
`fakelib_mode_override` is the title's explicit mode or `default`, and
`fakelib_default_mode` is the current global default.
`ignore_global_fakelib` reports whether the title ignores the global overlay.
Post `title_id` and a boolean `ignore_global_fakelib` to update only that setting;
do not combine it with `mode` or `enabled`. False removes the exclusion.
The game's own fakelib mode stays
unchanged. Changes apply on the next launch and preserve active mounts.
The fakelib route accepts only known PS5 games, including PKGs, images and
folders. `PPSA` identifies PS5; homebrew uses its `app.db` platform.
Send `mode: "full"`, `"emulators"` or `"disabled"` to persist an explicit override,
or `mode: "default"` to remove it and inherit `fakelib_default_mode`.
Legacy `enabled: true|false` requests remain supported and select explicit
`full` or `disabled`; `mode` takes precedence when both fields are present.
Other title overrides and global settings are preserved.
The response includes the three mode fields, `saved: true` and
`applies_on_next_launch: true`. Existing mounts stay until game exit.
`fakelib_enabled` is false when the resolved mode is `disabled`;
`fakelib_effective_enabled` also respects the global `backport_fakelib` switch.
Reaching the 128-title override limit fails without changing saved settings;
removing an override remains possible at the limit.

Mount mutations return HTTP 409 with `status` set to
`EBUSY` when they affect a live title, its source or a shared backing image,
while an inactive title owns a prepared runtime, during suspend, or while a
conflicting scan, storage operation, launch or release is in progress. An
independent title may be mounted/unmounted during gameplay without replacing
the live runtime owner; scanner cleanup may release these transient mounts. Clients
must not retry this response in a tight loop. HTTP 404 with `ENOENT` means the
title has no managed runtime mount.
The optional mount `mode` is request-scoped and does not change saved settings.
Accepted values are `ro`, `rw`, `r/o` and `r/w`; responses normalize them to
`ro` or `rw`. It applies to every image layer needed for that title. A direct
directory source or an image profile that is inherently read-only rejects an
explicit mode it cannot support with HTTP 501 and `ENOTSUP`. An already mounted
image in the other mode returns HTTP 409 and `EBUSY`.
With `persistent_image_mounts=1`, unmounting a game releases its per-title
runtime layers but keeps the backing image mounted.

The scan command queues a full scan. If the runtime is busy, the request
remains pending until scanning is safe. By default retry counters are preserved.
With `reset_attempts=true`, title registration, installation and image mount
retry counters are cleared before the requested scan starts. If pending scan
requests are combined, one `true` request keeps the reset enabled for that scan.

Manual entries are direct paths to individual game directories or image files;
they are not recursive scan roots. Updates preserve unrelated entries. Adding
an existing normalized path and removing a missing path
both succeed with `changed: false`. A real change queues a scan. Removing a
manual source does not uninstall its game; use the uninstall route separately
when both operations are desired.
The list route returns `{status, count, paths}`. Paths use the same trimming and
trailing-slash normalization as the scanner; comments and empty lines are not
included. An unconfigured manual source list is returned as an empty list.

The settings update owns only `debug`, `quiet_mode`, `update_emulators`,
`fakelib_default_mode`, `ps4_force_pro`, `ps4_cpu_boost`, `ps4_gpu_boost`,
`auto_update_ampr`, `auto_remove_missing_games`,
`auto_remove_missing_delay_seconds`, `allow_lan_access`,
`fan_target_temperature`, and `scan_paths`. The
`allow_lan_access` field maps to `0.0.0.0` when enabled and `127.0.0.1` when
disabled. Updates preserve unrelated settings and apply through runtime reload.
The listener's enabled state and port cannot be changed through this endpoint.
A fan target of `0` means system control; explicit
values must be 50 through 91 degrees Celsius. `scan_paths` defines recursive
library roots. The settings response contains only explicit custom roots. Saving
an empty array removes the custom override and restores built-in defaults.

`POST /api/v1/settings` returns `fakelib_default_mode` (`full`, `emulators`, or
`disabled`; initial default `full`). The settings update accepts the same
optional field; omission preserves its current value for older clients.
Per-title overrides take priority, and mode changes apply on the next launch.

The debug-log route returns at most 256 KiB and never modifies or rotates the log.
The kernel-log route returns a kernel-log snapshot.

Uninstall returns success after the system accepts the request; the on-disk
removal itself is asynchronous. It returns `EBUSY` for the live title, during
launch/exit transitions, while a conflicting inactive runtime is prepared, or
while batch install work is pending. Other titles may be uninstalled while a
game runs. An unchanged discoverable source may be installed again by a
later scan, so remove its manual entry or source first when that is not wanted.

## Game installation and browser streaming

The `pkg_install`, `pkg_browser_stream` and `install_game_source` capabilities
advertise package installation, browser streaming and folder/image installation.

`POST /api/v1/packages` accepts `{"path":"/data/pkgs","offset":0}` and returns
`path`, `parent`, available `roots`, `entries`, `offset` and `truncated`.
An empty path lists available roots. Each page contains up to 256 local entries
or 32 SMB entries; advance `offset` by the returned `limit` when `truncated`
is true. Browsing is non-recursive and
restricted to internal `/data`, mounted `/mnt/usb0..7` and `/mnt/ext0..1`.
`/data/pkgs` remains a shortcut. Symlinks and installed content trees are excluded. Valid package entries expose
`title_id`, `title_name`, `content_id`, `version`, `kind` (base/patch/dlc) and
`size_bytes`. Invalid packages remain visible with `installable=false`.
Game folders with `sce_sys/param.json` or `param.sfo` and `eboot.bin` or
`eboot2.bin`, and supported image files, expose `source_type` (`folder` or
`image`) and `installable=true`. Folders include their title ID and name,
`version` and `sdk_version` from their own param.json/param.sfo, plus `has_icon`
and `icon_revision` for `sce_sys/icon0.png`;
unmounted images use their filename. Folder sizes are not recursively measured
during browsing (`size_bytes=0`); installation measures the whole source.

`POST /api/v1/sources/install` accepts `path` and `destination_dir`. It copies
the source directory or intact image under its original name into the selected
directory in a background storage job. Destinations must be existing
directories under `/data` or mounted USB/ext storage; native installed-content
trees and runtime image mounts are excluded. The optional ext1 homebrew preset
can be created using the existing storage destination rules. Existing targets,
the source's own parent, and destinations inside the source are rejected.
The source is preserved. After copying, the destination is added to `manual.lst`
and a scan is requested, so custom destination directories are discovered too.
These installations have no queue support and serialize against active jobs and
PKG batches. Active-title/source protection and cancellation apply.
Status reports `operation=install` with
`source_type=folder|image`; native PKG dependency checks do not treat this as a
queued base PKG.

Valid entries also include `installed` (base packages installed as native PKGs),
`base_installed`, `base_queued`, `has_icon` and `icon_revision`. A folder/image
registration counts as `base_installed` but not `installed`: its base PKG can
be installed or queued. Direct patch/DLC installation requires an installed base.
PS5 media application PKGs (`content_type=0x26`) are supported. A full media
remaster is classified as a base source and has `replaceable=true`: it can be
installed standalone or queued even when the native app is installed. The
replacement reports patch progress and supports cancellation without uninstalling
the existing base app.

`GET /api/v1/packages/icon?path=...&v=...` returns the embedded icon0.png from a
permitted PKG, limited to 4 MiB. For a game directory, add `source_type=folder`
and pass the directory as `path` to read its `sce_sys/icon0.png`, even before
registration. Use `icon_revision` for the `v` query parameter to invalidate
cached artwork when the source changes. Missing or unreadable version fields
are empty; unavailable `sdk_version` is zero.

`POST /api/v1/files` lists paths for scan roots, individual game sources and
storage destinations. It accepts `path` (empty for drives), `offset`
and `include_files` (boolean, default false). The response has the same listing
envelope as `/packages`: `roots`, `path`, `parent`, `offset`, `truncated` and
`entries` containing `name`, `path`, `directory`, `size_bytes` and
`installable=false`. It returns directories only unless files are requested,
never reads package metadata, and uses the same drive restrictions and
pagination limits as `/packages`.

Listings recognise backport directories named `^(PPSA|CUSA)[0-9]{5}.*`,
containing a real `fakelib` subdirectory and no `sce_sys/keystone` entry.
These entries expose `backport=true`, `backport_installed`, their nine-character
`title_id`, and installed-game metadata matched by title ID: `title_name`,
`version` and a versioned `icon_url`. Missing games return empty metadata.
`sdk_version` describes the backport's own executable independently of the
installed game, or zero if unavailable.

`POST /api/v1/backports/install` accepts `{"path":"/mnt/usb0/PPSA12345 Game"}`
and copies the whole directory to `/data/homebrew/backports/<original name>`.
It returns HTTP 202 with the shared storage-job response (`operation=copy`,
`source_type=backport`). Cancellation, progress and active-game protections
apply. Existing backports
can be replaced; failed or cancelled copies leave the installed version intact.
Cancellation closes during finalization. A failed replacement restores the
previous directory. The source is preserved.
Installing a destination over itself or an ancestor is rejected.
No game registration is required.

`POST /api/v1/backports/delete` accepts the same `path` and removes exactly the
selected backport directory, including its contents. It does not substitute the
corresponding internal installation path. The directory must still satisfy the
backport rules at request time and when the worker starts. The shared job uses
`operation=delete`, `source_type=backport` and an empty destination; progress is
available through the storage-status endpoint. Active-game protections apply.
Cancellation is
available during preparation/measurement; recursive deletion is irreversible
once it starts.

`POST /api/v1/packages/queue` accepts `action` (`status`, `add`, `remove`, `clear`
or `start`), and `path` for add/remove. The payload owns up to 32 remaining local
packages. A queued or currently installing base permits enqueueing its patches
and DLC, which still require an installed base when their turn begins. Duplicate
packages and bases already installed as PKGs cannot be queued. Removing a queued base
also removes its dependents if the base is not installed. The active entry stays
at the head until installation completes successfully and cannot be removed.
Clear removes waiting entries and preserves an active installation. Errors,
cancellation and rest mode stop the batch, retaining the interrupted entry and
all remaining entries. Completed entries are removed. Restart is manual; no
files are deleted or retried automatically.
For source switching, `clear` also accepts `only_if_idle=true`: a busy or stopping
storage worker returns `storage_busy` without changing any queue entries. The
default is false, preserving the normal clear-waiting-items behavior.
Starting returns HTTP 202 with StorageJobResponse; other actions return HTTP 200
with `queue_running`, `queue_completed`, `queue_total` and remaining `entries`
(including the current entry while the batch is running).
Entries include `size_bytes`, `has_icon` and `icon_revision` for queue cards;
artwork uses the same `/packages/icon` endpoint as package browsing.
The queue runs sequentially, survives browser closure and requests a scan
after successful installations. It is kept in memory until payload shutdown.
Storage status adds `queue_count`, `queue_completed`, `queue_total`,
`queue_running` and the current `package_kind`; its job ID covers the batch.

`POST /api/v1/packages/install` accepts `{"path":"/mnt/usb0/packages/game.pkg"}`.
There is one shared job slot for installation and file operations.
Patches and DLC require an installed base game. Sources are not deleted or
automatically retried.
Local file identity, size and modification times are checked before and during
installation. System errors preserve the raw `native_status` code.

For a file on a computer, `POST /api/v1/packages/stream` accepts
`{"name":"game.pkg","size":123456}` and returns the job plus a private `token`.
The response advertises `chunk_bytes` (1 MiB) and `stream_port`. Connect to
`ws://<console>:<stream_port>/<token>/ws` on the package stream server.
One source connection is accepted per job. Server text messages are
`{"op":"ready","window":2,"max_window":4}`, `{"op":"range","id":1,"offset":0,"size":1048576}`
and `{"op":"ack","id":1}`. Reply to each range with a binary message containing
a little-endian 64-bit range ID followed by exactly the requested bytes.
The initial window permits two outstanding transfers. When `max_window` is 4,
the client can enable four by sending the exact text `{"op":"window","size":4}`.
An acknowledgement releases its window slot. If `max_window` is absent,
clients must keep the initial window of two outstanding transfers.
The server closes the connection when the operation ends; disconnecting the
source fails pending reads immediately with `pkg_stream_lost`. Explicit cancellation
remains a separate cancelled job. Keep the source connection available until
completion. A stream whose native installation completes before cancellation
takes effect reports `completed` with `cancel_requested=true`.

Compatibility clients can use two concurrent
`POST /api/v1/packages/stream/request` clients, with
`{"token":"...","client":1}` or client 2. Each returns an `id`, `offset` and
`size` immediately; id=0 means no pending range. Idle clients wait 16-100 ms
before polling again. Reply with
`POST /api/v1/packages/stream/chunk?token=...&id=...`, Content-Type
`application/octet-stream`, and exactly that range's bytes. Successful range
and chunk responses keep the HTTP connection alive. Stable client IDs reclaim
an unacknowledged range after a lost response. Repeated buffered replies are
accepted only if their bytes match; an evicted range returns HTTP 409.
Expired sessions return 404/410.

Both modes publish `operation=install` and `state=preparing|installing` through
`/api/v1/games/storage/status` and use its existing cancellation endpoint.
Status includes package name, processed/total bytes, speed, elapsed time and
system state. Progress is indeterminate until system counters are available;
transferring all source bytes alone does not imply installation completion.
Each queued package starts with fresh progress.

After submission, the job allows up to 30 seconds for system status to appear.
A later visibility gap also has a 30-second deadline. No package is resubmitted
automatically. System errors fail immediately and preserve `native_status`.
Malformed system replies fail with `EPROTO`, separately from
`pkg_stream_lost` source failures. A 15-minute inactivity deadline follows
progress rather than total installation duration.

Cancellation stops the current installation; cancelling a patch or DLC does
not uninstall its base game. Sleep stops the current installation and batch,
retaining the interrupted and waiting queue entries. The interrupted installation must be restarted; a new
browser stream receives a new token. Closing the browser disconnects its stream,
whereas local-file and queued installations survive browser closure.

Package list and queue entries also include `sdk_version`, an unsigned packed
SDK value (for example `0x07508000` displays as `SDK 7.50`), or zero if unavailable.
A zero or missing value remains unknown and never prevents installation.
Minimum firmware requirements are separate fields.

## PS4 game profiles

`POST /api/v1/settings` returns three global boolean defaults:
`ps4_force_pro`, `ps4_cpu_boost`, and `ps4_gpu_boost`. All default to `false`.
`POST /api/v1/settings/update` accepts these fields optionally; omitted values
remain unchanged. Changes apply at the next game launch.

The `game_ps4_profiles` capability advertises
`POST /api/v1/games/ps4-profile`. It accepts a known PS4 game's `title_id`
and the following string fields:

| Field | Required | Values |
| --- | --- | --- |
| `pro` | Yes | `default`, `on`, `off` |
| `cpu` | Yes | `default`, `on`, `off` |
| `gpu` | Yes | `default`, `on`, `off` |
| `memory` | No | `default`, `on`, `off` |
| `video_120hz` | No | `default`, `on`, `off` |
| `gpu_clock` | No | `default`, `2350` |

Omitted optional fields preserve their saved values. `default` restores
inheritance for `pro`, `cpu`, and `gpu`; memory and 120 Hz default to off.
Explicit `off` overrides an enabled global default. `gpu_clock=2350` selects
that GPU target even when `gpu=off`. To disable GPU boost completely, send
`gpu=off` and `gpu_clock=default`. Setting every profile field to `default`
removes its overrides while preserving the title's other settings.

Example:

```json
{"title_id":"CUSA12345","pro":"default","cpu":"on","gpu":"on","gpu_clock":"2350"}
```

PS5/system titles, unknown games and invalid modes are rejected. Up to 128
explicit PS4 title profiles are supported. `memory=on` requires
`ps4_memory_supported=true` and Pro mode: `pro=default` implicitly enables Pro,
while explicit `pro=off` is rejected. The `game_ps4_memory` capability advertises
the optional memory field.

PS4 library/detail entries set `can_configure_ps4_profile=true` and include:

- `ps4_profile`: the six saved override strings listed above.
- `ps4_profile_default`: default booleans for `pro`, `cpu`, `gpu`, `memory`,
  and `video_120hz`, plus `gpu_clock=0` (no explicit target override).
- `ps4_profile_effective`: resolved booleans for those five switches and a
  numeric `gpu_clock` in MHz: `0` for stock, `2200`, or `2350`.
- `ps4_memory_supported`: whether the console firmware supports the memory option.
- `ps4_memory_status`: current memory state, `off`, `pending`, `active`, or `blocked`.

Successful saves return these fields with `status=0`, `title_id`, `saved=true`
and `applies_on_next_launch=true`. This confirms persistence; it does not confirm
display-mode activation or the hardware's actual clock frequency.
