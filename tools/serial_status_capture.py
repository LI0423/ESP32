#!/usr/bin/env python3
"""Bounded serial capture for DeskPal acceptance runs.

Opens /dev/cu.usbmodem1401 (USB Serial/JTAG CDC), records every line to a log,
sends `status` when no valid status JSON has arrived recently, and exits once a
status JSON with time_valid true is seen (or the timeout elapses).

Usage: serial_status_capture.py --label pre --timeout 120
Exit codes: 0 = got time_valid status, 2 = timeout/no valid status, 3 = port error.
"""
import argparse
import json
import os
import sys
import time

import serial

PORT = "/dev/cu.usbmodem1401"
BAUD = 115200


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", default="capture")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--outdir", default="build")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log_path = os.path.join(args.outdir, f"deskpal_serial_{args.label}_{stamp}.log")
    json_path = os.path.join(args.outdir, f"deskpal_status_{args.label}_{stamp}.json")

    try:
        ser = serial.Serial()
        ser.port = PORT
        ser.baudrate = BAUD
        ser.timeout = 0.5
        ser.dtr = False
        ser.rts = False
        ser.open()
    except Exception as exc:  # noqa: BLE001
        print(f"PORT_ERROR: {exc}", flush=True)
        return 3

    start = time.time()
    last_valid = None
    last_send = 0.0
    lines = 0
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        log.write(f"# capture label={args.label} port={PORT} start={stamp}\n")
        log.flush()
        try:
            while time.time() - start < args.timeout:
                raw = ser.readline()
                if raw:
                    text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                    lines += 1
                    log.write(text + "\n")
                    log.flush()
                    if '"type":"status"' in text:
                        try:
                            obj = json.loads(text)
                        except json.JSONDecodeError:
                            obj = None
                        if obj and obj.get("time_valid"):
                            last_valid = obj
                            break
                now = time.time()
                if now - last_send > 5.0:
                    ser.write(b"status\n")
                    ser.flush()
                    last_send = now
        finally:
            ser.close()

    elapsed = round(time.time() - start, 1)
    print(f"capture label={args.label} elapsed={elapsed}s lines={lines}")
    print(f"log={log_path}")
    if last_valid is None:
        print("RESULT: no status with time_valid=true within timeout")
        return 2
    with open(json_path, "w", encoding="utf-8") as fh:
        json.dump(last_valid, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print(f"json={json_path}")
    print("STATUS: " + json.dumps(last_valid, sort_keys=True))
    print("RESULT: time_valid=true")
    return 0


if __name__ == "__main__":
    sys.exit(main())
