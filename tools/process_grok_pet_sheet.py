#!/usr/bin/env python3
"""Split the Grok growth-stage sheet into firmware-ready indexed concepts.

The raw Grok image remains untouched. Output uses the exact 32-color timer
palette, index 0 transparency, and a 128x128 canvas.
"""
from __future__ import annotations

import pathlib

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets" / "deskpal"
SOURCE = ASSETS / "pet-v2" / "grok-growth-stages.jpg"
PALETTE_SOURCE = ASSETS / "deskpal-char-focus-128.png"
OUT = ASSETS / "pet-v2"
SIZE = 128
STAGES = ("baby", "child", "teen", "adult")
TARGET_HEIGHTS = {"baby": 88, "child": 100, "teen": 110, "adult": 118}


def load_palette() -> list[tuple[int, int, int]]:
    image = Image.open(PALETTE_SOURCE)
    raw = image.getpalette() or []
    return [tuple(raw[i * 3 : i * 3 + 3]) for i in range(32)]  # type: ignore[return-value]


def find_characters(image: Image.Image) -> list[tuple[int, int, int, int]]:
    width, height = image.size
    pixels = image.load()
    columns = [any(max(pixels[x, y]) > 24 for y in range(height)) for x in range(width)]
    groups: list[tuple[int, int]] = []
    start = None
    for x, present in enumerate(columns + [False]):
        if present and start is None:
            start = x
        elif not present and start is not None:
            if x - start > 20:
                groups.append((start, x - 1))
            start = None
    if len(groups) != 4:
        raise SystemExit(f"expected 4 character groups, found {groups}")
    boxes = []
    for left, right in groups:
        rows = [y for y in range(height)
                if any(max(pixels[x, y]) > 24 for x in range(left, right + 1))]
        boxes.append((left, min(rows), right + 1, max(rows) + 1))
    return boxes


def nearest_palette(image: Image.Image, palette: list[tuple[int, int, int]]) -> Image.Image:
    rgba = image.convert("RGBA")
    result = Image.new("P", rgba.size, 0)
    flat = [channel for color in palette for channel in color]
    result.putpalette(flat)
    out = result.load()
    for y in range(rgba.height):
        for x in range(rgba.width):
            r, g, b, a = rgba.getpixel((x, y))
            if a < 96 or max(r, g, b) < 24:
                out[x, y] = 0
                continue
            best_index = 1
            best_distance = 1 << 30
            for index in range(1, 32):
                pr, pg, pb = palette[index]
                distance = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
                if distance < best_distance:
                    best_distance = distance
                    best_index = index
            out[x, y] = best_index
    result.info["transparency"] = 0
    return result


def make_sprite(crop: Image.Image, target_height: int,
                palette: list[tuple[int, int, int]]) -> Image.Image:
    target_width = max(1, round(crop.width * target_height / crop.height))
    resized = crop.resize((target_width, target_height), Image.Resampling.NEAREST)
    indexed = nearest_palette(resized, palette)
    sheet = Image.new("P", (SIZE, SIZE), 0)
    sheet.putpalette([channel for color in palette for channel in color])
    sheet.paste(indexed, ((SIZE - target_width) // 2, SIZE - 3 - target_height))
    sheet.info["transparency"] = 0
    return sheet


def contact_sheet(sprites: dict[str, Image.Image], path: pathlib.Path) -> None:
    scale = 4
    cell = 160 * scale
    sheet = Image.new("RGBA", (cell * 2, cell * 2), (15, 22, 36, 255))
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default(size=20)
    for index, (name, sprite) in enumerate(sprites.items()):
        x = (index % 2) * cell
        y = (index // 2) * cell
        image = sprite.convert("RGBA").resize((SIZE * scale, SIZE * scale), Image.Resampling.NEAREST)
        sheet.alpha_composite(image, (x + 16 * scale, y + 4 * scale))
        draw.text((x + 20, y + 140 * scale), name.upper(), font=font, fill=(245, 208, 127, 255))
    sheet.save(path)


def main() -> None:
    source = Image.open(SOURCE).convert("RGB")
    boxes = find_characters(source)
    palette = load_palette()
    sprites: dict[str, Image.Image] = {}
    for stage, box in zip(STAGES, boxes):
        crop = source.crop(box)
        sprite = make_sprite(crop, TARGET_HEIGHTS[stage], palette)
        path = OUT / f"deskpal-pet-{stage}-grok-128.png"
        sprite.save(path, transparency=0)
        sprites[stage] = sprite
        print(f"{stage}: source={box} output={path.name} bbox={sprite.convert('RGBA').getchannel('A').getbbox()}")
    contact_sheet(sprites, OUT / "deskpal-pet-grok-contact-sheet.png")
    print("wrote Grok-derived indexed stage concepts")


if __name__ == "__main__":
    main()
