#!/usr/bin/env python3
"""Validate a running ShadowMount HTTP/JSON API endpoint."""

import argparse
import errno
import http.client
import json
import sys
from typing import Any, Dict, Optional, Tuple


API_VERSION = 1
MAX_RESPONSE_SIZE = 16 * 1024 * 1024

ROUTE_VERSION = "/api/v1/version"
ROUTE_IMAGES = "/api/v1/images"
ROUTE_GAMES = "/api/v1/games"
ROUTE_MOUNT = "/api/v1/games/mount"
ROUTE_UNMOUNT = "/api/v1/games/unmount"


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
        headers.get("access-control-allow-methods") == "POST, OPTIONS",
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
    print(f"[API-TEST] ShadowMount={version} API={API_VERSION}")


def test_list(client: ApiClient, route: str, key: str) -> list:
    http_status, response = client.post(route, {})
    require(http_status == 200, f"{key} request returned HTTP {http_status}")
    require(integer_field(response, "status") == 0, f"{key} request failed")
    count = integer_field(response, "count")
    items = response.get(key)
    require(count >= 0, f"{key} count is negative")
    require(isinstance(items, list), f"{key} field must be an array")
    require(len(items) == count, f"{key} count does not match array length")
    require(
        all(isinstance(item, dict) for item in items),
        f"{key} array contains a non-object item",
    )
    print(f"[API-TEST] {key}={count}")
    return items


def find_mount_candidate(games: list) -> str:
    for game in games:
        title_id = game.get("title_id")
        if (
            game.get("managed") is True
            and game.get("source_available") is True
            and game.get("mounted") is False
            and isinstance(title_id, str)
            and title_id
        ):
            return title_id
    return ""


def title_operation(client: ApiClient, route: str, title_id: str) -> Tuple[int, int]:
    http_status, response = client.post(route, {"title_id": title_id})
    return http_status, integer_field(response, "status")


def test_mount_cycle(client: ApiClient, title_id: str) -> str:
    if not title_id:
        print("[API-TEST] mount cycle=SKIP (no candidate)")
        return "SKIP"

    http_status, status = title_operation(client, ROUTE_MOUNT, title_id)
    if http_status == 409 and status == errno.EBUSY:
        print(f"[API-TEST] mount cycle=SKIP ({title_id} is busy)")
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
    print(f"[API-TEST] mount cycle=PASS ({title_id})")
    return "PASS"


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
        "--timeout", type=positive_timeout, default=5.0, help="request timeout"
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    client = ApiClient(args.address, args.port, args.timeout)
    try:
        test_preflight(client)
        test_version(client)
        images = test_list(client, ROUTE_IMAGES, "images")
        games = test_list(client, ROUTE_GAMES, "games")
        mount_result = test_mount_cycle(client, find_mount_candidate(games))
    except (ApiTestError, http.client.HTTPException, OSError) as error:
        print(f"[API-TEST] FAIL: {error}", file=sys.stderr)
        return 1

    print(
        f"[API-TEST] PASS: images={len(images)} games={len(games)} "
        f"mount={mount_result}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
