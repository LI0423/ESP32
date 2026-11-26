#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$(mktemp "${TMPDIR:-/tmp}/companion_timer_test.XXXXXX")"
trap 'rm -f "$BIN"' EXIT
# Product wall clock is CST-8; host tests must use the same POSIX TZ.
export TZ=CST-8
cc -std=c17 -Wall -Wextra -Werror -I"$ROOT/main" \
  "$ROOT/tools/test_companion_timer.c" \
  "$ROOT/main/companion_timer.c" \
  "$ROOT/main/companion_schedule.c" \
  "$ROOT/main/companion_serial.c" \
  "$ROOT/main/companion_progress.c" \
  -o "$BIN"
"$BIN"
