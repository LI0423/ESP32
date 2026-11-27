#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$(mktemp "${TMPDIR:-/tmp}/companion_timer_test.XXXXXX")"
PET_BIN="$(mktemp "${TMPDIR:-/tmp}/companion_pet_test.XXXXXX")"
trap 'rm -f "$BIN" "$PET_BIN"' EXIT
# Product wall clock is CST-8; host tests must use the same POSIX TZ.
export TZ=CST-8
python3 "$ROOT/tools/gen_deskpal_sprites.py" --check
cc -std=c17 -Wall -Wextra -Werror -I"$ROOT/main" \
   "$ROOT/tools/test_companion_timer.c" \
   "$ROOT/main/companion_timer.c" \
   "$ROOT/main/companion_schedule.c" \
   "$ROOT/main/companion_serial.c" \
   "$ROOT/main/companion_progress.c" \
   "$ROOT/assets/deskpal/deskpal_sprites.c" \
   -o "$BIN"
cc -std=c17 -Wall -Wextra -Werror -I"$ROOT/main" \
   "$ROOT/tools/test_companion_pet.c" \
   "$ROOT/main/companion_pet.c" \
   "$ROOT/main/companion_schedule.c" \
   -o "$PET_BIN"
"$BIN"
"$PET_BIN"
