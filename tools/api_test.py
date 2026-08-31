#!/usr/bin/env python3
"""Validate a running ShadowMount HTTP/JSON API endpoint."""

import argparse
import concurrent.futures
import errno
import http.client
import json
import socket
import sys
from typing import Any, Dict, Optional, Tuple


API_VERSION = 1
MAX_RESPONSE_SIZE = 16 * 1024 * 1024

ROUTE_INDEX = "/"
ROUTE_VERSION = "/api/v1/version"
ROUTE_STORAGE = "/api/v1/storage"
ROUTE_IMAGES = "/api/v1/images"
ROUTE_SCAN = "/api/v1/scan"
ROUTE_MANUAL_LIST = "/api/v1/manual/list"
ROUTE_MANUAL_ADD = "/api/v1/manual/add"
ROUTE_MANUAL_REMOVE = "/api/v1/manual/remove"
ROUTE_GAMES = "/api/v1/games"
ROUTE_GAME_INFO = "/api/v1/games/info"
ROUTE_MOUNT = "/api/v1/games/mount"
ROUTE_UNMOUNT = "/api/v1/games/unmount"
ROUTE_UNINSTALL = "/api/v1/games/uninstall"
ROUTE_MOVE = "/api/v1/games/move"
ROUTE_COPY = "/api/v1/games/copy"
ROUTE_DELETE = "/api/v1/games/delete"


class ApiTestError(RuntimeError):
    pass


class ApiClient:
    def __init__(self, address: str, port: int, timeout: float) -> None:
        self.address = address
        self.port = port
        self.timeout = timeout

    def request(
        self,
        method: str,
        route: str,
        body: bytes = b"",
        headers: Optional[Dict[str, str]] = None,
    ) -> Tuple[int, Dict[str, str], bytes]:
        connection = http.client.HTTPConnection(
            self.address, self.port, timeout=self.timeout
        )
        try:
            connection.request(method, route, body=body, headers=headers or {})
            response = connection.getresponse()
            response_body = response.read(MAX_RESPONSE_SIZE + 1)
            response_headers = {
                name.lower(): value.strip() for name, value in response.getheaders()
            }
            response_status = response.status
        finally:
            connection.close()

        if len(response_body) > MAX_RESPONSE_SIZE:
            raise ApiTestError("response body exceeds 16 MiB")
        if response_headers.get("access-control-allow-origin") != "*":
            raise ApiTestError(f"{method} {route}: missing CORS allow-origin header")

        content_length = response_headers.get("content-length")
        if response_status == 204:
            if response_body:
                raise ApiTestError(f"{method} {route}: HTTP 204 response has a body")
            return response_status, response_headers, response_body
        if content_length is None:
            raise ApiTestError(f"{method} {route}: missing Content-Length header")
        try:
            declared_size = int(content_length, 10)
        except ValueError as error:
            raise ApiTestError(
                f"{method} {route}: invalid Content-Length header"
            ) from error
        if declared_size != len(response_body):
            raise ApiTestError(
                f"{method} {route}: Content-Length is {declared_size}, "
                f"received {len(response_body)} bytes"
            )
        return response_status, response_headers, response_body

    def post(self, route: str, payload: Dict[str, Any]) -> Tuple[int, Dict[str, Any]]:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        status, headers, response_body = self.request(
            "POST",
            route,
            body,
            {
                "Content-Type": "application/json",
                "Content-Length": str(len(body)),
                "Connection": "close",
            },
        )
        if not headers.get("content-type", "").lower().startswith(
            "application/json"
        ):
            raise ApiTestError(f"POST {route}: response is not JSON")
        try:
            response = json.loads(response_body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ApiTestError(f"POST {route}: invalid JSON response") from error
        if not isinstance(response, dict):
            raise ApiTestError(f"POST {route}: response must be a JSON object")
        return status, response


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ApiTestError(message)


def integer_field(response: Dict[str, Any], key: str) -> int:
    value = response.get(key)
    if type(value) is not int:
        raise ApiTestError(f"response field {key!r} must be an integer")
    return value


def test_preflight(client: ApiClient) -> None:
    status, headers, body = client.request(
        "OPTIONS",
        ROUTE_VERSION,
        headers={
            "Origin": "http://example.test",
            "Access-Control-Request-Method": "POST",
            "Access-Control-Request-Headers": "Content-Type",
            "Access-Control-Request-Private-Network": "true",
            "Connection": "close",
        },
    )
    require(status == 204, f"CORS preflight returned HTTP {status}")
    require(not body, "CORS preflight returned a response body")
    require(
        headers.get("access-control-allow-methods") == "GET, POST, OPTIONS",
        "CORS preflight returned invalid allowed methods",
    )
    require(
        headers.get("access-control-allow-headers") == "Content-Type",
        "CORS preflight returned invalid allowed headers",
    )
    require(
        headers.get("access-control-allow-private-network") == "true",
        "CORS private-network access is not enabled",
    )
    print("[API-TEST] CORS preflight=PASS")


def test_index_page(client: ApiClient) -> str:
    status, headers, body = client.request("GET", ROUTE_INDEX)
    content_type = headers.get("content-type", "").lower()
    if status == 404:
        require(
            content_type.startswith("application/json"),
            "missing web UI response is not JSON",
        )
        try:
            error = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exception:
            raise ApiTestError("missing web UI returned invalid JSON") from exception
        require(
            integer_field(error, "status") == errno.ENOENT,
            "missing web UI did not return ENOENT",
        )
        print("[API-TEST] web UI=SKIP (index.html is not installed)")
        return "SKIP"
    require(status == 200, f"web UI returned HTTP {status}")
    require(content_type.startswith("text/html"), "web UI response is not HTML")
    require(bool(body.strip()), "web UI response is empty")
    print(f"[API-TEST] web UI=PASS ({len(body)} bytes)")
    return "PASS"


def test_version(client: ApiClient) -> None:
    http_status, response = client.post(ROUTE_VERSION, {})
    require(http_status == 200, f"version request returned HTTP {http_status}")
    require(integer_field(response, "status") == 0, "version request failed")
    require(
        integer_field(response, "api_version") == API_VERSION,
        "unsupported API version",
    )
    version = response.get("shadowmount_version")
    require(isinstance(version, str), "missing ShadowMount version")
    capabilities = response.get("capabilities")
    require(isinstance(capabilities, list), "missing API capabilities")
    required_capabilities = {
        "web_ui",
        "storage_space",
        "rescan",
        "uninstall_game",
        "game_info",
        "game_icon",
        "move_game_source",
        "copy_game_source",
        "delete_game_source",
        "list_manual_sources",
        "add_manual_source",
        "remove_manual_source",
    }
    require(
        required_capabilities.issubset(capabilities),
        "API does not advertise all mutation capabilities",
    )
    print(f"[API-TEST] ShadowMount={version} API={API_VERSION}")


def test_storage(client: ApiClient) -> int:
    http_status, response = client.post(ROUTE_STORAGE, {})
    require(http_status == 200, f"storage request returned HTTP {http_status}")
    require(integer_field(response, "status") == 0, "storage request failed")
    count = integer_field(response, "count")
    mounts = response.get("mounts")
    require(count >= 0, "storage mount count is negative")
    require(isinstance(mounts, list), "storage mounts field must be an array")
    require(len(mounts) == count, "storage mount count does not match array length")

    required_fields = {
        "source",
        "mount_point",
        "filesystem",
        "total_bytes",
        "free_bytes",
        "available_bytes",
        "used_bytes",
        "read_only",
    }
    for mount in mounts:
        require(isinstance(mount, dict), "storage mounts contains a non-object item")
        require(required_fields.issubset(mount), "storage mount lacks fields")
        require(
            isinstance(mount["source"], str)
            and isinstance(mount["mount_point"], str)
            and mount["mount_point"].startswith("/")
            and isinstance(mount["filesystem"], str),
            "storage mount contains invalid identity fields",
        )
        total = integer_field(mount, "total_bytes")
        free = integer_field(mount, "free_bytes")
        available = integer_field(mount, "available_bytes")
        used = integer_field(mount, "used_bytes")
        require(
            0 <= available <= free <= total and used == total - available,
            "storage mount byte counters are inconsistent",
        )
        require(type(mount["read_only"]) is bool, "storage read_only must be boolean")
    print(f"[API-TEST] storage mounts={count}")
    return count


def test_list(client: ApiClient, route: str, key: str) -> list:
    http_status, response = client.post(route, {})
    require(http_status == 200, f"{key} request returned HTTP {http_status}")
    require(integer_field(response, "status") == 0, f"{key} request failed")
    count = integer_field(response, "count")
    items = response.get(key)
    require(count >= 0, f"{key} count is negative")
    require(isinstance(items, list), f"{key} field must be an array")
    require(len(items) == count, f"{key} count does not match array length")
    if key == "games":
        require(response.get("size_included") is False, "game size was enabled")
    require(
        all(isinstance(item, dict) for item in items),
        f"{key} array contains a non-object item",
    )
    print(f"[API-TEST] {key}={count}")
    return items


def test_game_details(client: ApiClient, games: list) -> None:
    if not games:
        print("[API-TEST] game details=SKIP (no games)")
        return

    required_fields = {
        "path",
        "runtime_path",
        "source_type",
        "image_type",
        "platform",
        "title_id",
        "content_id",
        "title_name",
        "last_access_time",
        "install_time",
        "icon_url",
        "app_db_size_bytes",
    }
    for game in games:
        require(required_fields.issubset(game), "game list item lacks metadata")
        require("size_bytes" not in game, "game list calculated size by default")

    title_id = games[0].get("title_id")
    require(isinstance(title_id, str) and title_id, "game has invalid title_id")
    http_status, detail = client.post(ROUTE_GAME_INFO, {"title_id": title_id})
    require(http_status == 200, f"game info returned HTTP {http_status}")
    require(integer_field(detail, "status") == 0, "game info failed")
    size_status = integer_field(detail, "size_status")
    require(size_status >= 0, "game size status is invalid")
    if size_status == 0:
        require(integer_field(detail, "size_bytes") >= 0, "game size is invalid")

    icon_url = detail.get("icon_url")
    if isinstance(icon_url, str) and icon_url:
        icon_status, headers, icon = client.request("GET", icon_url)
        require(icon_status == 200, f"game icon returned HTTP {icon_status}")
        require(
            headers.get("content-type", "").lower().startswith("image/png"),
            "game icon response is not PNG",
        )
        require(bool(icon), "game icon is empty")
    print(f"[API-TEST] game details=PASS ({title_id}, size_status={size_status})")


def find_mount_candidate(games: list, mode: str) -> str:
    for game in games:
        title_id = game.get("title_id")
        source_path = game.get("path")
        if (
            game.get("managed") is True
            and game.get("source_available") is True
            and game.get("mounted") is False
            and (not mode or game.get("image_backed") is True)
            and (
                mode != "rw"
                or not isinstance(source_path, str)
                or not source_path.lower().endswith(".ffpfsc")
            )
            and isinstance(title_id, str)
            and title_id
        ):
            return title_id
    return ""


def title_operation(
    client: ApiClient, route: str, title_id: str, mode: str = ""
) -> Tuple[int, int]:
    payload = {"title_id": title_id}
    if mode:
        payload["mode"] = mode
    http_status, response = client.post(route, payload)
    return http_status, integer_field(response, "status")


def test_mount_cycle(client: ApiClient, title_id: str, mode: str) -> str:
    if not title_id:
        print("[API-TEST] mount cycle=SKIP (no candidate)")
        return "SKIP"

    http_status, status = title_operation(client, ROUTE_MOUNT, title_id, mode)
    if http_status == 409 and status == errno.EBUSY:
        print(f"[API-TEST] mount cycle=SKIP ({title_id} is busy)")
        return "SKIP"
    if http_status == 501 and status == errno.ENOTSUP:
        print(f"[API-TEST] mount cycle=SKIP ({title_id} rejects mode={mode})")
        return "SKIP"
    require(
        http_status == 200 and status == 0,
        f"mount {title_id} failed: HTTP {http_status}, status {status}",
    )

    http_status, status = title_operation(client, ROUTE_UNMOUNT, title_id)
    require(
        http_status == 200 and status == 0,
        f"unmount {title_id} failed: HTTP {http_status}, status {status}",
    )
    mode_text = mode or "default"
    print(f"[API-TEST] mount cycle=PASS ({title_id}, mode={mode_text})")
    return "PASS"


def test_rescan(client: ApiClient) -> None:
    http_status, response = client.post(ROUTE_SCAN, {})
    require(http_status == 200, f"rescan request returned HTTP {http_status}")
    require(integer_field(response, "status") == 0, "rescan request failed")
    require(response.get("queued") is True, "rescan was not queued")
    require(
        response.get("reset_attempts") is False,
        "rescan reset attempts without an explicit option",
    )
    print("[API-TEST] full rescan=QUEUED")


def test_manual_list(client: ApiClient) -> int:
    http_status, response = client.post(ROUTE_MANUAL_LIST, {})
    require(http_status == 200, f"manual list returned HTTP {http_status}")
    require(integer_field(response, "status") == 0, "manual list failed")
    count = integer_field(response, "count")
    paths = response.get("paths")
    require(count >= 0, "manual path count is negative")
    require(isinstance(paths, list), "manual paths field must be an array")
    require(len(paths) == count, "manual path count does not match array length")
    require(
        all(isinstance(path, str) and path.startswith("/") for path in paths),
        "manual paths contains an invalid path",
    )
    print(f"[API-TEST] manual paths={count}")
    return count


def test_parallel_requests(client: ApiClient) -> None:
    stalled = socket.create_connection(
        (client.address, client.port), timeout=client.timeout
    )
    try:
        stalled.sendall(
            (
                f"POST {ROUTE_VERSION} HTTP/1.1\r\n"
                f"Host: {client.address}:{client.port}\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 2\r\n"
                "Connection: close\r\n\r\n"
                "{"
            ).encode("ascii")
        )

        def get_version(_: int) -> None:
            http_status, response = client.post(ROUTE_VERSION, {})
            require(
                http_status == 200 and integer_field(response, "status") == 0,
                f"parallel version request returned HTTP {http_status}",
            )

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            list(executor.map(get_version, range(16)))
    finally:
        stalled.close()
    print("[API-TEST] parallel requests=PASS (16 requests + stalled client)")


def test_mutation_validation(client: ApiClient) -> None:
    invalid_requests = (
        (ROUTE_SCAN, {"reset_attempts": "yes"}),
        (ROUTE_MANUAL_ADD, {"path": "relative/path"}),
        (ROUTE_MANUAL_REMOVE, {"path": "/"}),
        (ROUTE_UNINSTALL, {"title_id": "INVALID"}),
        (ROUTE_MOVE, {"title_id": "INVALID", "destination_dir": "/mnt"}),
        (ROUTE_COPY, {"title_id": "INVALID", "destination_dir": "/mnt"}),
        (ROUTE_DELETE, {"title_id": "INVALID", "confirm": True}),
    )
    for route, payload in invalid_requests:
        http_status, response = client.post(route, payload)
        require(http_status == 400, f"{route} validation returned HTTP {http_status}")
        require(
            integer_field(response, "status") == errno.EINVAL,
            f"{route} validation did not return EINVAL",
        )
    print("[API-TEST] mutation validation=PASS")


def valid_port(value: str) -> int:
    try:
        port = int(value, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError("port must be an integer") from error
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("port must be in range 1..65535")
    return port


def positive_timeout(value: str) -> float:
    try:
        timeout = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("timeout must be a number") from error
    if timeout <= 0:
        raise argparse.ArgumentTypeError("timeout must be positive")
    return timeout


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("address", help="PS5 IPv4 address or hostname")
    parser.add_argument("port", type=valid_port, help="ShadowMount HTTP API port")
    parser.add_argument(
        "--timeout", type=positive_timeout, default=60.0, help="request timeout"
    )
    parser.add_argument(
        "--mount-mode",
        choices=("default", "ro", "rw"),
        default="default",
        help="request-scoped mode for the optional mount cycle",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    client = ApiClient(args.address, args.port, args.timeout)
    try:
        test_preflight(client)
        web_result = test_index_page(client)
        test_version(client)
        storage_count = test_storage(client)
        images = test_list(client, ROUTE_IMAGES, "images")
        games = test_list(client, ROUTE_GAMES, "games")
        test_game_details(client, games)
        manual_count = test_manual_list(client)
        test_parallel_requests(client)
        mount_mode = "" if args.mount_mode == "default" else args.mount_mode
        mount_result = test_mount_cycle(
            client, find_mount_candidate(games, mount_mode), mount_mode
        )
        test_mutation_validation(client)
        test_rescan(client)
    except (ApiTestError, http.client.HTTPException, OSError) as error:
        print(f"[API-TEST] FAIL: {error}", file=sys.stderr)
        return 1

    print(
        f"[API-TEST] PASS: images={len(images)} games={len(games)} "
        f"storage={storage_count} manual={manual_count} web={web_result} "
        f"mount={mount_result}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
