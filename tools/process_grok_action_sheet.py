#!/usr/bin/env python3
"""Split Grok's 2x2 action sheet into indexed 128x128 DeskPal concepts."""
from __future__ import annotations

import pathlib

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets" / "deskpal"
SOURCE = ASSETS / "pet-v2" / "grok-action-sheet.jpg"
PALETTE_SOURCE = ASSETS / "deskpal-char-focus-128.png"
OUT = ASSETS / "pet-v2"
SIZE = 128
ACTIONS = ("feed", "medicine", "play", "clean")


def load_palette() -> list[tuple[int, int, int]]:
    image = Image.open(PALETTE_SOURCE)
    raw = image.getpalette() or []
    return [tuple(raw[i * 3 : i * 3 + 3]) for i in range(32)]  # type: ignore[return-value]


def content_box(image: Image.Image) -> tuple[int, int, int, int]:
    pixels = image.load()
    width, height = image.size
    points = [(x, y) for y in range(height) for x in range(width)
              if max(pixels[x, y]) > 24]
    if not points:
        raise SystemExit("empty action panel")
    xs = [point[0] for point in points]
    ys = [point[1] for point in points]
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def warm_recolor(image: Image.Image) -> Image.Image:
    """Map Grok's cool bubble/bottle accents into the frozen warm palette."""
    image = image.convert("RGBA")
    pixels = image.load()
    for y in range(image.height):
        for x in range(image.width):
            r, g, b, a = pixels[x, y]
            if a < 96 or max(r, g, b) < 24:
                pixels[x, y] = (0, 0, 0, 0)
            elif b > r * 1.12 and b > g * 1.02:
                pixels[x, y] = (245, 208, 127, a) if (r + g + b) > 360 else (196, 160, 68, a)
    return image


def nearest_palette(image: Image.Image, palette: list[tuple[int, int, int]]) -> Image.Image:
    rgba = image.convert("RGBA")
    result = Image.new("P", rgba.size, 0)
    result.putpalette([channel for color in palette for channel in color])
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


def make_sprite(panel: Image.Image, palette: list[tuple[int, int, int]]) -> Image.Image:
    crop = warm_recolor(panel.crop(content_box(panel)))
    scale = min(116 / crop.height, 116 / crop.width)
    size = (max(1, round(crop.width * scale)), max(1, round(crop.height * scale)))
    indexed = nearest_palette(crop.resize(size, Image.Resampling.NEAREST), palette)
    sheet = Image.new("P", (SIZE, SIZE), 0)
    sheet.putpalette([channel for color in palette for channel in color])
    sheet.paste(indexed, ((SIZE - size[0]) // 2, SIZE - 3 - size[1]))
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
    width, height = source.size
    panels = [
        source.crop((0, 0, width // 2, height // 2)),
        source.crop((width // 2, 0, width, height // 2)),
        source.crop((0, height // 2, width // 2, height)),
        source.crop((width // 2, height // 2, width, height)),
    ]
    palette = load_palette()
    sprites: dict[str, Image.Image] = {}
    for action, panel in zip(ACTIONS, panels):
        sprite = make_sprite(panel, palette)
        path = OUT / f"deskpal-pet-action-{action}-grok-128.png"
        sprite.save(path, transparency=0)
        sprites[action] = sprite
        print(f"{action}: output={path.name} bbox={sprite.convert('RGBA').getchannel('A').getbbox()}")
    contact_sheet(sprites, OUT / "deskpal-pet-actions-grok-contact-sheet.png")
    print("wrote Grok-derived indexed action concepts")


if __name__ == "__main__":
    main()
