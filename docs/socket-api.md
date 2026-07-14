# ShadowMount socket API v1

ShadowMount exposes a public Unix stream socket at
`/system_tmp/shadowmount-api.sock`. The internal
`/system_tmp/shadowmount.sock` endpoint is reserved for the SceShellCore
bridge and must not be used by third-party applications.

The ABI is defined in `include/sm_api_protocol.h`. All integers use the native
PS5 little-endian byte order. A client opens one connection, writes exactly one
64-byte `sm_api_request_t`, reads one 32-byte `sm_api_response_t` followed by
the advertised payload, and closes the connection.

Validate `magic`, `version`, `operation`, `size`, `item_count`, and `item_size`
before consuming a response. `status` is zero on success or a positive errno
value. Strings are NUL-terminated fixed-size byte arrays.

## Operations

- `SM_API_GET_VERSION` returns one `sm_api_version_info_t`.
- `SM_API_LIST_IMAGES` returns `sm_api_image_info_t` entries. `offset` is the
  zero-based entry offset; `limit` is clamped to 16 and zero means 16.
- `SM_API_LIST_GAMES` uses the same pagination and returns
  `sm_api_game_info_t` entries.
- `SM_API_MOUNT_GAME` mounts the managed runtime stack identified by
  `title_id`.
- `SM_API_UNMOUNT_GAME` releases that runtime stack and its backing image
  chain.

List responses contain the full current count in `total_count` and the current
page size in `item_count`. Continue with `offset += item_count` until the offset
reaches `total_count`.

Mount mutations are deliberately conservative. They return `EBUSY` while a
game is active, while ShellCore owns another prepared title, during suspend,
or while a conflicting mount/release is in progress. `ENOENT` means the title
does not have a ShadowMount `mount.lnk` tracker. Clients should not retry
`EBUSY` in a tight loop.

The socket is created only after startup synchronization completes and is
removed before shutdown mount cleanup begins.

## Test payload

Build `api-test.elf` and run it while ShadowMount is active. It validates the
version response, walks every image and game page, then selects the first
managed game that is not already mounted and performs a mount/unmount cycle.
The mutation test is skipped when no safe candidate exists or the runtime is
busy; an existing mounted game is never selected.

Clients should call `SM_API_GET_VERSION` first and check its capability bits.
Unknown operations return `ENOTSUP`, malformed requests return `EPROTO`, and a
future incompatible structure layout will increment `SM_API_VERSION`.
