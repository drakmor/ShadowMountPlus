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

The HTTP service owns and monitors its listener. If initial `socket/bind/listen`
or a later `accept` fails, it retries with an interruptible 250 ms to 5 s
backoff. Only the transition into recovery and the successful recovery are
logged. Shutdown and config reload interrupt the wait immediately.

Every API operation must use `POST`, include `Content-Type: application/json`
and contain one JSON object. Every response is a JSON object. Connections are
closed after one response; chunked request bodies are not supported. The
maximum request body is 4096 bytes.

All responses include `Access-Control-Allow-Origin: *`. Browser preflight
requests using `OPTIONS` are supported and allow `POST`, the `Content-Type`
header and Private Network Access. This permits calls from any browser origin;
the listener still needs `api_bind_address=0.0.0.0` for access from another
device.

## Routes

| Route | Request | Purpose |
| --- | --- | --- |
| `/api/v1/version` | `{}` | API and ShadowMount versions plus capabilities |
| `/api/v1/images` | `{}` | Complete image snapshot |
| `/api/v1/games` | `{}` | Complete game snapshot |
| `/api/v1/games/mount` | `{"title_id":"PPSA12345"}` | Mount a managed game |
| `/api/v1/games/unmount` | `{"title_id":"PPSA12345"}` | Unmount a managed game |

Example:

```sh
curl -sS http://127.0.0.1:10101/api/v1/version \
  -H 'Content-Type: application/json' \
  -d '{}'

curl -sS http://127.0.0.1:10101/api/v1/games \
  -H 'Content-Type: application/json' \
  -d '{}'
```

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
  "capabilities": ["list_images", "list_games", "mount_game", "unmount_game"]
}
```

List responses contain `count` and the complete `images` or `games` array.
Image items expose `path`, `mount_point`, `size`, modification time, `unit_id`,
`backend`, `complete`, `source_available`, `mapped` and `mounted`. Game items
expose `path`, `image_path`, `title_id`, `title_name`, `installed`, `managed`,
`mounted`, `image_backed` and `source_available`.

Mount mutations remain conservative. They return HTTP 409 with `status` set to
`EBUSY` while a game is active, while ShellCore owns another prepared title,
during suspend, or while a conflicting mount/release is in progress. Clients
must not retry this response in a tight loop. HTTP 404 with `ENOENT` means the
title has no ShadowMount `mount.lnk` tracker.

## Test client

Run the host-side Python client while ShadowMount is active:

```sh
python3 tools/api_test.py 192.168.1.50 10101
```

It validates CORS preflight, the version response and complete image/game
snapshots, then selects the first safe managed game for a mount/unmount cycle.
The mutation test is skipped if there is no candidate or the runtime is busy.
