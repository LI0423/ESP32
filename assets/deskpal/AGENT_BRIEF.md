# DeskPal character-layer implementation (do this now)

You are the local Cursor CLI Agent. Implement this in the existing DesktopCompanion firmware repo. The coordinator will not edit source; you must make the code changes.

## Goal
Replace the current procedurally drawn placeholder character with the frozen 128x128 indexed pixel sprites, and draw HUD in firmware on top, per the agreed entry point.

This is a separate character-style track. Do not rewrite the 2026-11-25 v1 schedule/timer behavior (work window 08:00-22:00 CST-8, STANDBY, no 7-day fast-forward, offline face not grayed, etc.). Do not commit, do not push, do not flash.

## Assets already on disk
Directory: `assets/deskpal/`

- `deskpal_sprites.h` / `deskpal_sprites.c`: ready-to-compile shared 32-color RGB565 palette, four 128x128 8-bit index maps (`deskpal_sprite_focus/break/complete/standby`), transparent index 0, layout constants, phase background colors, helper `deskpal_sprite_data()`.
- `deskpal-hud-layout.json`: source of truth for positions (also encoded in the header).
- Reference PNGs and `deskpal-shared-palette.png`.

Move or add `deskpal_sprites.c/.h` into `main/` (or another compiled location) and update `main/CMakeLists.txt` so they are built. You may keep copies under `assets/deskpal/`. Do not invent new art.

## Layout (240x240)
- Character: 128x128 at (56, 43). Blit with index 0 transparent.
- Stage label centered at (120, 20), 8px pixel font, text FOCUS | BREAK | COMPLETE | STANDBY according to phase.
- Time `MM:SS` centered at (120, 198), 16px pixel font.
- `Today N` centered at (120, 219), 8px pixel font.
- Online/network dot at (222, 18), radius 5, online color #2ED66C. Offline should still only affect this dot, not the face.
- Backgrounds (fill screen first): FOCUS #492A15, BREAK #437059, COMPLETE #E3A22C, STANDBY #0F1629.
- STANDBY dimming must use the existing backlight / brightness path. Do not bake dim colors into the sprite.

## Font
Use Press Start 2P or an equivalent monospaced pixel font. If the exact TTF cannot be embedded, use a small embedded bitmap glyph set covering: 0-9, `:`, space, `FOCUS`, `BREAK`, `COMPLETE`, `STANDBY`, `Today`. Keep it readable at 8px and 16px on 240x240.

## Implementation notes
- COMPLETE sparkles are already in the character sprite; do not redraw extra sparkles unless the current UI already has a harmless overlay you must preserve.
- Update `companion_ui.c` (and related files) to select the sprite by phase, including STANDBY.
- Host tests: do not break existing timer tests. Add UI-related tests only if there is already a host-test pattern for UI; otherwise keep tests focused on anything you can unit-test without the LCD.
- If you can run `bash tools/run_tests.sh`, do so. If Shell is denied, leave tests unrun and say so; do not use `--force`.
- Do not run `idf.py flash`. You may try `idf.py build` if the environment is available and Shell is allowed.

## Done when
- Placeholder geometric face is gone for the four phases.
- Sprites blit at the specified rect; HUD is firmware-drawn at the specified positions.
- Palette/transparency works (no black box around the character unless the art itself has it).
- Existing schedule/timer behavior is unchanged.
- Summarize files changed, what you verified, and what you could not run.
