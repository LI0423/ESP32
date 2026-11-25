#!/usr/bin/env python3
"""Test Desktop Companion OTA while both devices are on the same LAN.

The board must have CONFIG_COMPANION_OTA_ALLOW_HTTP enabled for this local
HTTP test. Production firmware should use HTTPS OTA.
"""

from __future__ import annotations

import argparse
import http.server
import ipaddress
import json
import os
from pathlib import Path
import socket
import ssl
import sys
import threading
import time
import urllib.error
import urllib.request
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[1]


def local_ip_for(device_ip: str) -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect((device_ip, 80))
        return sock.getsockname()[0]


def same_lan(host_ip: str, device_ip: str, prefix: int = 24) -> bool:
    return ipaddress.ip_interface(f"{host_ip}/{prefix}").network == ipaddress.ip_interface(
        f"{device_ip}/{prefix}"
    ).network


def next_partition(initial: str) -> str:
    if initial == "ota_0":
        return "ota_1"
    if initial == "ota_1":
        return "ota_0"
    raise ValueError(f"unexpected running partition: {initial}")


def request_json(url: str, *, method: str = "GET", body: dict | None = None,
                 timeout: float = 5.0, token: str | None = None) -> Any:
    data = None if body is None else json.dumps(body, separators=(",", ":")).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=timeout,
                                    context=ssl._create_unverified_context()) as response:
            payload = response.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        raise RuntimeError(f"HTTP {exc.code} from {url}: {detail}") from exc
    return json.loads(payload) if payload else None


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format: str, *args: Any) -> None:
        print(f"[http] {format % args}", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-ip", required=True, help="Desktop Companion IPv4 address on the LAN")
    parser.add_argument("--firmware", type=Path, default=PROJECT_ROOT / "build" / "desktop_companion.bin")
    parser.add_argument("--token", default=os.environ.get("DESKPAL_ADMIN_TOKEN"),
                        help="Admin token (or set DESKPAL_ADMIN_TOKEN)")
    parser.add_argument("--allow-http", action="store_true",
                        help="Acknowledge that this test uses plaintext HTTP OTA")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--timeout", type=float, default=180.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    firmware = args.firmware.resolve()
    if not firmware.is_file() or firmware.stat().st_size == 0:
        print(f"lan_ota_test: firmware not found: {firmware}", file=sys.stderr)
        return 2
    if not args.token:
        print("lan_ota_test: admin token required (--token or DESKPAL_ADMIN_TOKEN)", file=sys.stderr)
        return 2
    if not args.allow_http:
        print("lan_ota_test: this local test uses HTTP; pass --allow-http to continue", file=sys.stderr)
        return 2
    try:
        host_ip = local_ip_for(args.device_ip)
    except OSError as exc:
        print(f"lan_ota_test: cannot determine local LAN IP: {exc}", file=sys.stderr)
        return 1
    if not same_lan(host_ip, args.device_ip):
        print(
            f"lan_ota_test: host {host_ip} and board {args.device_ip} are not on the same /24 LAN",
            file=sys.stderr,
        )
        return 1

    handler = lambda *a, **kw: QuietHandler(*a, directory=str(firmware.parent), **kw)
    try:
        server = http.server.ThreadingHTTPServer((host_ip, args.port), handler)
    except OSError as exc:
        print(f"lan_ota_test: cannot start HTTP server on {host_ip}:{args.port}: {exc}", file=sys.stderr)
        return 1
    thread = threading.Thread(target=server.serve_forever, name="ota-http", daemon=True)
    thread.start()

    base = f"https://{args.device_ip}"
    try:
        config = request_json(base + "/api/config", timeout=10.0)
        status = request_json(base + "/api/status", timeout=10.0)
        initial = status.get("ota", {}).get("running_partition", "unknown")
        expected = next_partition(initial)
        print(f"Device: {args.device_ip} mode={status.get('wifi_mode')} ip={status.get('ip')}")
        print(f"Config: SSID={config.get('wifi_ssid')} password_set={config.get('wifi_password_set')}")
        print(f"OTA before: running_partition={initial}; expected_after={expected}")

        url = f"http://{host_ip}:{args.port}/{firmware.name}"
        reply = request_json(base + "/api/ota", method="POST", body={"url": url},
                             timeout=10.0, token=args.token)
        print(f"OTA start: {json.dumps(reply, ensure_ascii=False)}")
        if not isinstance(reply, dict) or reply.get("accepted") is not True:
            raise RuntimeError("board did not accept OTA request")

        deadline = time.monotonic() + args.timeout
        last_state = None
        while time.monotonic() < deadline:
            try:
                current = request_json(base + "/api/status", timeout=2.0)
                ota = current.get("ota", {})
                state = ota.get("state")
                running = ota.get("running_partition")
                if state != last_state:
                    print(
                        f"OTA state={state} written={ota.get('bytes_written')} "
                        f"total={ota.get('total_bytes')} running={running}"
                    )
                    last_state = state
                if running == expected:
                    health_deadline = time.monotonic() + 30.0
                    while time.monotonic() < health_deadline:
                        try:
                            healthy = request_json(base + "/api/status", timeout=2.0)
                            timer = healthy.get("timer", {})
                            if (healthy.get("ota", {}).get("state") == "idle"
                                and timer.get("phase") in {"focus", "celebrate", "break"}
                                and isinstance(timer.get("remaining_seconds"), int)):
                                print(f"LAN OTA passed and healthy: {initial} -> {running}")
                                return 0
                        except (urllib.error.URLError, TimeoutError, ConnectionError,
                                json.JSONDecodeError):
                            pass
                        time.sleep(0.5)
                    raise RuntimeError("new partition booted but application health check failed")
                if state == "failed":
                    raise RuntimeError(f"OTA failed with error {ota.get('error')}")
            except (urllib.error.URLError, TimeoutError, ConnectionError, json.JSONDecodeError):
                # The device is expected to disappear briefly while rebooting.
                pass
            time.sleep(1.0)
        raise TimeoutError(f"OTA did not switch to {expected} within {args.timeout:.0f}s")
    except (urllib.error.URLError, TimeoutError, ConnectionError, RuntimeError, ValueError) as exc:
        print(f"lan_ota_test: {exc}", file=sys.stderr)
        return 1
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


if __name__ == "__main__":
    raise SystemExit(main())
