#!/usr/bin/env python3
"""Generate the sick-pill badge icons with DeskPal's exact timer palette.

Index 0 stays transparent; every other index comes from
assets/deskpal/deskpal-char-focus-128.png so the badge can share
deskpal_palette at runtime.
"""
from __future__ import annotations

import pathlib

from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets" / "deskpal"
SRC = ASSETS / "deskpal-char-focus-128.png"
OUT = ASSETS / "pet-v2"

# Indices in the shared timer palette.
OUTLINE = 31  # #311705
CREAM = 1     # #F5D07F
BRIGHT = 3    # #EE873D
ORANGE = 5    # #DE6B26
DARK = 20     # #A33D13


def draw_pill(size: int, outer_box, inner_box, outer_r: int, inner_r: int,
              highlight_cells, divider: int | None) -> Image.Image:
    source = Image.open(SRC)
    if source.mode != "P":
        raise SystemExit("palette source must be indexed")
    image = Image.new("P", (size, size), 0)
    image.putpalette(source.getpalette())
    image.info["transparency"] = 0

    outer = Image.new("L", (size, size), 0)
    inner = Image.new("L", (size, size), 0)
    ImageDraw.Draw(outer).rounded_rectangle(outer_box, radius=outer_r, fill=255)
    ImageDraw.Draw(inner).rounded_rectangle(inner_box, radius=inner_r, fill=255)
    op = outer.load()
    ip = inner.load()
    px = image.load()
    for y in range(size):
        for x in range(size):
            if op[x, y] and not ip[x, y]:
                px[x, y] = OUTLINE
            elif ip[x, y]:
                if divider is not None and x == divider:
                    px[x, y] = OUTLINE
                elif x < size // 2:
                    px[x, y] = CREAM
                else:
                    px[x, y] = ORANGE
    for x, y, color in highlight_cells:
        px[x, y] = color
    return image


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    draw_pill(32, [4, 9, 27, 22], [6, 11, 25, 20], 6, 4,
              [(18, 12, BRIGHT), (19, 12, BRIGHT), (20, 12, BRIGHT),
               (8, 19, BRIGHT), (22, 19, DARK)], divider=15).save(
        OUT / "deskpal-sick-pill-32.png")
    draw_pill(24, [1, 5, 22, 18], [3, 7, 20, 16], 7, 5,
              [(14, 8, BRIGHT), (15, 8, BRIGHT), (16, 8, BRIGHT),
               (6, 15, BRIGHT), (18, 15, DARK)], divider=12).save(
        OUT / "deskpal-sick-pill-24.png")
    draw_pill(16, [0, 3, 15, 12], [1, 4, 14, 11], 5, 4,
              [(9, 5, BRIGHT), (10, 5, BRIGHT)], divider=None).save(
        OUT / "deskpal-sick-pill-16.png")
    print("wrote 16px, 24px and 32px sick-pill icons")


if __name__ == "__main__":
    main()
