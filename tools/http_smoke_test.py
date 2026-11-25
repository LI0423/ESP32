#!/usr/bin/env python3
"""HTTP authentication and parser regression test for Desktop Companion."""
from __future__ import annotations

import argparse
import json
import os
import ssl
import urllib.error
import urllib.request


def request(base: str, path: str, method: str = "GET", body: dict | None = None,
            token: str | None = None, timeout: float = 5.0) -> tuple[int, dict | None]:
    data = None if body is None else json.dumps(body, separators=(",", ":")).encode()
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(base + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout,
                                    context=ssl._create_unverified_context()) as response:
            payload = response.read()
            return response.status, json.loads(payload) if payload else None
    except urllib.error.HTTPError as exc:
        payload = exc.read()
        return exc.code, json.loads(payload) if payload else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="deskpal.local")
    parser.add_argument("--token", default=os.environ.get("DESKPAL_ADMIN_TOKEN"))
    args = parser.parse_args()
    if not args.token:
        raise SystemExit("admin token required: --token or DESKPAL_ADMIN_TOKEN")
    base = f"https://{args.host}"

    status, state = request(base, "/api/status")
    assert status == 200 and state and "timer" in state, "status endpoint failed"
    status, config = request(base, "/api/config")
    assert status == 200 and config and config.get("admin_token_set"), "config endpoint failed"

    status, _ = request(base, "/api/config", "POST", {"wifi_ssid": config["wifi_ssid"]})
    assert status == 401, f"unauthenticated config write returned {status}"

    payload = {
        "wifi_ssid": config["wifi_ssid"],
        "wifi_password": "",
        "clear_wifi_password": False,
        "brightness": config["brightness"],
        "ota_url": config.get("ota_url", ""),
        "reboot": False,
    }
    status, reply = request(base, "/api/config", "POST", payload, args.token)
    assert status == 200 and reply and reply.get("saved"), f"valid string JSON failed: {status} {reply}"

    status, _ = request(base, "/api/config", "POST", {"brightness": 999}, args.token)
    assert status == 400, f"invalid brightness returned {status}"

    status, _ = request(base, "/api/ota", "POST", {"url": "file:///etc/passwd"}, args.token)
    assert status == 400, f"invalid OTA URL returned {status}"

    status, _ = request(base, "/api/restart", "POST", {})
    assert status == 401, f"unauthenticated restart returned {status}"

    print("HTTP auth and JSON parser smoke test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
