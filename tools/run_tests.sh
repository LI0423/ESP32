#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$(mktemp "${TMPDIR:-/tmp}/companion_timer_test.XXXXXX")"
trap 'rm -f "$BIN"' EXIT
cc -std=c17 -Wall -Wextra -Werror -I"$ROOT/main" \
  "$ROOT/tools/test_companion_timer.c" "$ROOT/main/companion_timer.c" -o "$BIN"
"$BIN"
