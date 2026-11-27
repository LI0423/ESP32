#!/usr/bin/env python3
"""Generate a pixel-art rice-bowl hunger badge with DeskPal's timer palette."""
from __future__ import annotations

import pathlib

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets" / "deskpal"
SRC = ASSETS / "deskpal-char-focus-128.png"
OUT = ASSETS / "pet-v2"

# Shared timer-palette indices.
OUTLINE = 31  # dark brown #311705
CREAM = 1     # rice #F5D07F
BRIGHT = 3    # highlight #EE873D
ORANGE = 5    # bowl #DE6B26
DARK = 20     # bowl shade #A33D13


def make_base(size: int) -> Image.Image:
    source = Image.open(SRC)
    image = Image.new("P", (size, size), 0)
    image.putpalette(source.getpalette())
    image.info["transparency"] = 0

    filled = [[False] * size for _ in range(size)]
    colors = [[0] * size for _ in range(size)]

    for y in range(size):
        for x in range(size):
            nx = (x + 0.5) / size
            ny = (y + 0.5) / size
            # Rice mound.
            rx = (nx - 0.5) / 0.29
            ry = (ny - 0.42) / 0.20
            if rx * rx + ry * ry <= 1.0:
                filled[y][x] = True
                colors[y][x] = CREAM
            # Bowl body.
            if 0.52 <= ny <= 0.88:
                t = (ny - 0.52) / 0.36
                half = 0.36 - 0.17 * t
                if abs(nx - 0.5) <= half:
                    filled[y][x] = True
                    colors[y][x] = DARK if ny > 0.78 else ORANGE
            # Bowl rim.
            if 0.51 <= ny <= 0.55 and abs(nx - 0.5) <= 0.37:
                filled[y][x] = True
                colors[y][x] = OUTLINE

    px = image.load()
    for y in range(size):
        for x in range(size):
            if filled[y][x]:
                px[x, y] = colors[y][x]
            elif any(
                0 <= y + dy < size and 0 <= x + dx < size and filled[y + dy][x + dx]
                for dy in (-1, 0, 1)
                for dx in (-1, 0, 1)
            ):
                px[x, y] = OUTLINE
    return image


def make_detailed() -> Image.Image:
    """32px icon with rice grains, a rim highlight and steam wisps."""
    image = make_base(32)
    px = image.load()

    # Individual rice grains on the mound.
    for x, y in ((9, 10), (12, 8), (15, 10), (18, 8), (21, 10),
                 (11, 13), (14, 12), (17, 13), (20, 12), (23, 13)):
        px[x, y] = BRIGHT

    # Bright rim just above the dark bowl lip.
    for x in range(7, 25):
        px[x, 15] = CREAM

    # Steam wisps above the rice.
    for x, y in ((6, 5), (8, 3), (6, 1)):
        px[x, y] = BRIGHT
    return image


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    big = make_detailed()
    big.save(OUT / "deskpal-hungry-rice-32.png")
    for size in (16, 24):
        big.resize((size, size), Image.Resampling.NEAREST).save(
            OUT / f"deskpal-hungry-rice-{size}.png")

    sheet = Image.new("RGB", (3 * 96, 96), (0x0F, 0x16, 0x29))
    for i, size in enumerate((16, 24, 32)):
        icon = Image.open(OUT / f"deskpal-hungry-rice-{size}.png").convert("RGBA")
        icon = icon.resize((96, 96), Image.Resampling.NEAREST)
        sheet.paste(icon, (i * 96, 0), icon)
    sheet.save(OUT / "deskpal-hungry-rice-preview.png")

    screen = Image.new("RGBA", (240, 240), (0x49, 0x2A, 0x15, 255))
    screen.alpha_composite(Image.open(ASSETS / "deskpal-char-focus-128.png").convert("RGBA"), (56, 43))
    screen.alpha_composite(big.convert("RGBA"), (40, 28))
    screen.convert("RGB").resize((480, 480), Image.Resampling.NEAREST).save(
        OUT / "deskpal-hungry-rice-on-screen-preview.png")
    print("wrote detailed 16/24/32 rice-bowl icons and previews")


if __name__ == "__main__":
    main()
