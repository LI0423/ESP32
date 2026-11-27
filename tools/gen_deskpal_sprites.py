#!/usr/bin/env python3
"""Regenerate assets/deskpal/deskpal_sprites.c from the indexed PNG artwork.

The four timer faces keep the shared warm palette they were delivered with.
The vitality faces (hungry/sick) share a second 32-color palette of their own,
so the timer-face palette never changes.

Usage:
    python3 tools/gen_deskpal_sprites.py           # rewrite the .c file
    python3 tools/gen_deskpal_sprites.py --check   # fail if the file is stale

Requires Pillow (PIL).
"""
from __future__ import annotations

import argparse
import pathlib
import sys

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets" / "deskpal"
OUT = ASSETS / "deskpal_sprites.c"

SPRITE = 128
PALETTE_LEN = 32
TRANSPARENT = 0

# (C symbol, PNG name, palette group)
SPRITES = [
    ("deskpal_sprite_focus", "deskpal-char-focus-128.png", "timer"),
    ("deskpal_sprite_break", "deskpal-char-break-128.png", "timer"),
    ("deskpal_sprite_complete", "deskpal-char-complete-128.png", "timer"),
    ("deskpal_sprite_standby", "deskpal-char-standby-128.png", "timer"),
    ("deskpal_sprite_hungry", "deskpal-vitality-hungry-128.png", "vitality"),
    ("deskpal_sprite_sick", "deskpal-vitality-sick-128.png", "vitality"),
    ("deskpal_sprite_baby", "pet-v2/deskpal-pet-baby-grok-128.png", "timer"),
    ("deskpal_sprite_child", "pet-v2/deskpal-pet-child-grok-128.png", "timer"),
    ("deskpal_sprite_teen", "pet-v2/deskpal-pet-teen-grok-128.png", "timer"),
    ("deskpal_sprite_adult", "pet-v2/deskpal-pet-adult-grok-128.png", "timer"),
    ("deskpal_sprite_action_feed", "pet-v2/deskpal-pet-action-feed-grok-128.png", "timer"),
    ("deskpal_sprite_action_medicine", "pet-v2/deskpal-pet-action-medicine-grok-128.png", "timer"),
    ("deskpal_sprite_action_play", "pet-v2/deskpal-pet-action-play-grok-128.png", "timer"),
    ("deskpal_sprite_action_clean", "pet-v2/deskpal-pet-action-clean-grok-128.png", "timer"),
]

# Small HUD icons use the same timer palette but their own canvas size.
SMALL_ICONS = [
    ("deskpal_sick_pill", "pet-v2/deskpal-sick-pill-32.png", 32),
    ("deskpal_hungry_rice", "pet-v2/deskpal-hungry-rice-32.png", 32),
]

# Palette source per group. All four timer PNGs carry the identical palette, and
# hungry/sick carry the identical vitality palette, so one file defines each.
PALETTE_SOURCE = {
    "timer": "deskpal-char-focus-128.png",
    "vitality": "deskpal-vitality-sick-128.png",
}


def rgb565(rgb: tuple[int, int, int]) -> int:
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def load_palette(png: str) -> list[int]:
    image = Image.open(ASSETS / png)
    if image.mode != "P":
        raise SystemExit(f"{png}: expected indexed PNG, got mode {image.mode}")
    raw = image.getpalette() or []
    palette = [rgb565(tuple(raw[i * 3 : i * 3 + 3])) for i in range(PALETTE_LEN)]
    if palette[TRANSPARENT] != 0:
        raise SystemExit(f"{png}: index {TRANSPARENT} must be transparent black")
    return palette


def load_indices(png: str, size: int = SPRITE) -> list[int]:
    image = Image.open(ASSETS / png)
    if image.mode != "P" or image.size != (size, size):
        raise SystemExit(f"{png}: expected a {size}x{size} indexed PNG")
    if image.info.get("transparency") not in (None, TRANSPARENT):
        raise SystemExit(f"{png}: transparency index must be {TRANSPARENT}")
    if hasattr(image, "get_flattened_data"):
        return list(image.get_flattened_data())
    return list(image.getdata())


def format_palette(name: str, palette: list[int]) -> str:
    values = ", ".join(f"0x{value:04X}" for value in palette)
    return f"const uint16_t {name}[{PALETTE_LEN}] = {{ {values} }};\n"


def format_sprite(name: str, indices: list[int], size: int = SPRITE) -> str:
    lines = [f"const uint8_t {name}[{size} * {size}] = {{"]
    for start in range(0, len(indices), 16):
        chunk = ", ".join(str(value) for value in indices[start : start + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines) + "\n"


def render() -> str:
    timer_palette = load_palette(PALETTE_SOURCE["timer"])
    vitality_palette = load_palette(PALETTE_SOURCE["vitality"])

    total = SPRITE * SPRITE

    parts = ['#include "deskpal_sprites.h"\n\n']
    parts.append(format_palette("deskpal_palette", timer_palette))
    parts.append("\n")
    parts.append(format_palette("deskpal_vitality_palette", vitality_palette))
    parts.append("\n")

    for name, png, group in SPRITES:
        indices = load_indices(png)
        if len(indices) != total:
            raise SystemExit(f"{png}: wrong pixel count {len(indices)}")
        if group == "vitality":
            source = Image.open(ASSETS / png)
            raw = source.getpalette() or []
            colors = [tuple(raw[i * 3 : i * 3 + 3]) for i in range(PALETTE_LEN)]
            if [rgb565(color) for color in colors] != vitality_palette:
                raise SystemExit(f"{png}: palette differs from {PALETTE_SOURCE['vitality']}")
        else:
            source = Image.open(ASSETS / png)
            raw = source.getpalette() or []
            colors = [tuple(raw[i * 3 : i * 3 + 3]) for i in range(PALETTE_LEN)]
            if [rgb565(color) for color in colors] != timer_palette:
                raise SystemExit(f"{png}: palette differs from {PALETTE_SOURCE['timer']}")
        parts.append(format_sprite(name, indices))
        parts.append("\n")

    for name, png, size in SMALL_ICONS:
        indices = load_indices(png, size)
        source = Image.open(ASSETS / png)
        raw = source.getpalette() or []
        colors = [tuple(raw[i * 3 : i * 3 + 3]) for i in range(PALETTE_LEN)]
        if [rgb565(color) for color in colors] != timer_palette:
            raise SystemExit(f"{png}: palette differs from {PALETTE_SOURCE['timer']}")
        parts.append(format_sprite(name, indices, size))
        parts.append("\n")

    parts.append(
        "const uint8_t *deskpal_sprite_data(deskpal_sprite_id_t id)\n"
        "{\n"
        "    switch (id) {\n"
        "    case DESKPAL_SPRITE_FOCUS:\n"
        "        return deskpal_sprite_focus;\n"
        "    case DESKPAL_SPRITE_BREAK:\n"
        "        return deskpal_sprite_break;\n"
        "    case DESKPAL_SPRITE_COMPLETE:\n"
        "        return deskpal_sprite_complete;\n"
        "    case DESKPAL_SPRITE_STANDBY:\n"
        "        return deskpal_sprite_standby;\n"
        "    case DESKPAL_SPRITE_HUNGRY:\n"
        "        return deskpal_sprite_hungry;\n"
        "    case DESKPAL_SPRITE_SICK:\n"
        "        return deskpal_sprite_sick;\n"
        "    case DESKPAL_SPRITE_BABY:\n"
        "        return deskpal_sprite_baby;\n"
        "    case DESKPAL_SPRITE_CHILD:\n"
        "        return deskpal_sprite_child;\n"
        "    case DESKPAL_SPRITE_TEEN:\n"
        "        return deskpal_sprite_teen;\n"
        "    case DESKPAL_SPRITE_ADULT:\n"
        "        return deskpal_sprite_adult;\n"
        "    case DESKPAL_SPRITE_ACTION_FEED:\n"
        "        return deskpal_sprite_action_feed;\n"
        "    case DESKPAL_SPRITE_ACTION_MEDICINE:\n"
        "        return deskpal_sprite_action_medicine;\n"
        "    case DESKPAL_SPRITE_ACTION_PLAY:\n"
        "        return deskpal_sprite_action_play;\n"
        "    case DESKPAL_SPRITE_ACTION_CLEAN:\n"
        "        return deskpal_sprite_action_clean;\n"
        "    default:\n"
        "        return deskpal_sprite_focus;\n"
        "    }\n"
        "}\n"
        "\n"
        "const uint16_t *deskpal_sprite_palette(deskpal_sprite_id_t id)\n"
        "{\n"
        "    switch (id) {\n"
        "    case DESKPAL_SPRITE_HUNGRY:\n"
        "    case DESKPAL_SPRITE_SICK:\n"
        "        return deskpal_vitality_palette;\n"
        "    default:\n"
        "        return deskpal_palette;\n"
        "    }\n"
        "}\n"
    )
    return "".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    generated = render()
    if args.check:
        if OUT.read_text() != generated:
            print(f"stale: {OUT.relative_to(ROOT)}", file=sys.stderr)
            return 1
        print("deskpal_sprites.c is up to date")
        return 0
    OUT.write_text(generated)
    print(f"wrote {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
