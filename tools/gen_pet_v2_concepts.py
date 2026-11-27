#!/usr/bin/env python3
"""Draw original DeskPal pet-v2 pixel concepts in the frozen sprite style.

Outputs indexed 128x128 PNGs using the exact timer palette from the current
focus sprite. These are concepts only; they are not embedded in firmware.
"""
from __future__ import annotations

import math
import pathlib
from typing import Callable

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets" / "deskpal"
OUT = ASSETS / "pet-v2"
SOURCE = ASSETS / "deskpal-char-focus-128.png"
SIZE = 128
TRANSPARENT = 0
OUTLINE = 31
DEEP = 30
STEM = 28
BAYER = (
    (0, 8, 2, 10),
    (12, 4, 14, 6),
    (3, 11, 1, 9),
    (15, 7, 13, 5),
)

STAGES = {
    "baby": {"maxw": 36, "top": 45, "bottom": 119, "pear": 0.00, "leaf": 0.78},
    "child": {"maxw": 40, "top": 40, "bottom": 120, "pear": 0.05, "leaf": 0.92},
    "teen": {"maxw": 42, "top": 34, "bottom": 120, "pear": 0.30, "leaf": 1.05},
    "adult": {"maxw": 46, "top": 31, "bottom": 121, "pear": 0.43, "leaf": 1.16},
}


def palette() -> list[int]:
    source = Image.open(SOURCE)
    if source.mode != "P":
        raise SystemExit("source sprite must be indexed")
    raw = source.getpalette() or []
    return [raw[i * 3 : i * 3 + 3] for i in range(32)]  # type: ignore[list-item]


def canvas() -> Image.Image:
    image = Image.new("P", (SIZE, SIZE), TRANSPARENT)
    flat = [channel for rgb in palette() for channel in rgb]
    image.putpalette(flat)
    image.info["transparency"] = TRANSPARENT
    return image


def mask_ellipse(box: tuple[int, int, int, int]) -> Image.Image:
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).ellipse(box, fill=255)
    return mask


def mask_polygon(points: list[tuple[int, int]]) -> Image.Image:
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).polygon(points, fill=255)
    return mask


def add_outlined_shape(image: Image.Image, mask: Image.Image, fill: int,
                       outline: int = OUTLINE, width: int = 2) -> None:
    expanded = mask.filter(ImageFilter.MaxFilter(width * 2 + 1))
    edge = ImageChops.subtract(expanded, mask)
    image.paste(outline, (0, 0), edge)
    image.paste(fill, (0, 0), mask)


def body_mask(config: dict[str, float]) -> tuple[Image.Image, tuple[int, int, int, int]]:
    top = int(config["top"])
    bottom = int(config["bottom"])
    height = bottom - top
    maxw = float(config["maxw"])
    pear = float(config["pear"])
    mask = Image.new("L", (SIZE, SIZE), 0)
    pixels = mask.load()
    for y in range(top, bottom + 1):
        t = (y - top) / height
        ellipse = max(0.0, 1.0 - (2.0 * t - 1.0) ** 2)
        half = maxw * math.sqrt(ellipse) * (1.0 + pear * (t - 0.48))
        left = int(round(64 - half))
        right = int(round(64 + half))
        for x in range(max(0, left), min(SIZE, right + 1)):
            pixels[x, y] = 255
    return mask, (64 - int(maxw) - 4, top - 4, 64 + int(maxw) + 5, bottom + 5)


def shade_body(image: Image.Image, mask: Image.Image) -> None:
    pix = image.load()
    mask_pix = mask.load()
    bbox = mask.getbbox()
    if not bbox:
        return
    left, top, right, bottom = bbox
    cy = (top + bottom - 1) / 2
    ry = max(1.0, (bottom - top - 1) / 2)
    for y in range(top, bottom):
        for x in range(left, right):
            if not mask_pix[x, y]:
                continue
            nx = (x - 63.5) / max(1.0, (right - left) / 2)
            ny = (y - cy) / ry
            light = 0.56 - 0.30 * nx - 0.34 * ny
            noise = (BAYER[y & 3][x & 3] / 15.0 - 0.5) * 0.16
            value = light + noise
            if value > 0.69:
                color = 1
            elif value > 0.53:
                color = 3
            elif value > 0.34:
                color = 5
            elif value > 0.14:
                color = 6
            elif value > -0.04:
                color = 10
            elif value > -0.22:
                color = 15
            elif value > -0.40:
                color = 20
            else:
                color = 24
            pix[x, y] = color

    # Soft pixel highlight on the upper-left shoulder.
    highlight = mask_ellipse((30, 43, 57, 69))
    highlight = ImageChops.multiply(highlight, mask)
    hp = highlight.load()
    for y in range(43, 69):
        for x in range(30, 57):
            if hp[x, y] and BAYER[y & 3][x & 3] < 7:
                pix[x, y] = 3 if BAYER[y & 3][x & 3] < 3 else 5


def draw_appendages(image: Image.Image, stage: str, config: dict[str, float]) -> None:
    top = int(config["top"])
    bottom = int(config["bottom"])
    maxw = int(config["maxw"])
    draw = ImageDraw.Draw(image)

    # Crown nubs are part of the original citrus silhouette.
    nub_y = top + (8 if stage == "baby" else 5)
    nub_r = 6 if stage in ("baby", "child") else 7
    for cx in (64 - maxw + 11, 64 + maxw - 11):
        add_outlined_shape(image, mask_ellipse((cx - nub_r, nub_y - nub_r,
                                                cx + nub_r, nub_y + nub_r)), 5)

    if stage in ("teen", "adult"):
        arm_y = top + int((bottom - top) * 0.56)
        reach = 8 if stage == "teen" else 10
        for cx in (64 - maxw - 2, 64 + maxw + 2):
            box = (cx - 6, arm_y, cx + 6, arm_y + 18)
            add_outlined_shape(image, mask_ellipse(box), 6)
            if cx < 64:
                draw.line((cx - reach + 2, arm_y + 5, cx - 2, arm_y + 7), fill=OUTLINE, width=2)
            else:
                draw.line((cx + 2, arm_y + 7, cx + reach - 2, arm_y + 5), fill=OUTLINE, width=2)

    if stage == "adult":
        for cx in (43, 85):
            add_outlined_shape(image, mask_ellipse((cx - 8, bottom - 4, cx + 8, bottom + 5)), 6)


def draw_leaf(image: Image.Image, stage: str, config: dict[str, float]) -> None:
    top = int(config["top"])
    scale = float(config["leaf"])
    lean = 4 if stage in ("baby", "child") else 7
    stem_top = top - int(12 * scale)
    stem_bottom = top + 7
    draw = ImageDraw.Draw(image)
    draw.line((64, stem_bottom, 64 + lean, stem_top), fill=STEM, width=4)
    draw.line((64, stem_bottom - 1, 64 + lean, stem_top + 1), fill=2, width=2)

    def leaf(points: list[tuple[float, float]]) -> None:
        scaled = [(int(round(64 + lean + x * scale)), int(round(stem_top + y * scale)))
                  for x, y in points]
        add_outlined_shape(image, mask_polygon(scaled), 2, STEM, 1)
        draw.line((scaled[0][0], scaled[0][1], scaled[-1][0], scaled[-1][1]), fill=1, width=1)

    leaf([(0, 1), (-15, -10), (-12, -22), (1, -17), (8, -6)])
    if stage in ("teen", "adult"):
        leaf([(1, 0), (13, -10), (18, -18), (8, -21), (-1, -10)])


def draw_eye(draw: ImageDraw.ImageDraw, cx: int, cy: int, width: int, height: int,
             expression: str) -> None:
    if expression in ("clean", "sleep"):
        draw.line((cx - width // 2, cy + 2, cx, cy, cx + width // 2, cy + 2),
                  fill=OUTLINE, width=2)
        return
    draw.rounded_rectangle((cx - width // 2, cy - height // 2,
                            cx + width // 2, cy + height // 2),
                           radius=2, fill=OUTLINE)
    draw.rectangle((cx - width // 2 + 1, cy - 1, cx + width // 2 - 1, cy + 2), fill=DEEP)
    draw.rectangle((cx - width // 2 + 1, cy - height // 2 + 1,
                    cx - width // 2 + 2, cy - height // 2 + 2), fill=1)
    if expression in ("happy", "play"):
        draw.rectangle((cx - 1, cy - height // 2 + 1, cx, cy - height // 2 + 2), fill=1)


def draw_face(image: Image.Image, stage: str, config: dict[str, float], expression: str) -> None:
    top = int(config["top"])
    bottom = int(config["bottom"])
    eye_y = top + int((bottom - top) * 0.47)
    spacing = 13 if stage == "baby" else 15
    eye_w = 7 if stage == "baby" else 6
    eye_h = 9 if stage == "baby" else 8
    draw = ImageDraw.Draw(image)
    draw_eye(draw, 64 - spacing, eye_y, eye_w, eye_h, expression)
    draw_eye(draw, 64 + spacing, eye_y, eye_w, eye_h, expression)

    # Cheek pixels keep the face warm without adding new palette colors.
    if stage != "adult" or expression == "happy":
        draw.point((48, eye_y + 7), fill=3)
        draw.point((80, eye_y + 7), fill=3)

    mouth_y = eye_y + 13
    if expression in ("happy", "play"):
        draw.line((58, mouth_y, 61, mouth_y + 2, 64, mouth_y + 3,
                   67, mouth_y + 2, 70, mouth_y), fill=OUTLINE, width=2)
        draw.point((62, mouth_y + 1), fill=1)
    elif expression == "feed":
        draw.ellipse((59, mouth_y - 1, 69, mouth_y + 6), fill=OUTLINE)
        draw.ellipse((62, mouth_y + 1, 66, mouth_y + 4), fill=5)
    elif expression == "sleep":
        draw.line((61, mouth_y + 2, 67, mouth_y + 2), fill=OUTLINE, width=1)
    else:
        draw.line((60, mouth_y + 2, 68, mouth_y + 2), fill=OUTLINE, width=2)
        if stage in ("teen", "adult"):
            draw.point((69, mouth_y + 1), fill=OUTLINE)


def sparkle(draw: ImageDraw.ImageDraw, cx: int, cy: int, size: int) -> None:
    draw.line((cx - size, cy, cx + size, cy), fill=1, width=1)
    draw.line((cx, cy - size, cx, cy + size), fill=1, width=1)
    draw.point((cx - 1, cy - 1), fill=2)
    draw.point((cx + 1, cy + 1), fill=2)


def draw_action_prop(image: Image.Image, action: str, config: dict[str, float]) -> None:
    if not action:
        return
    draw = ImageDraw.Draw(image)
    top = int(config["top"])
    bottom = int(config["bottom"])
    y = top + int((bottom - top) * 0.64)
    if action == "feed":
        add_outlined_shape(image, mask_ellipse((84, y - 7, 99, y + 8)), 1, OUTLINE, 1)
        draw.line((88, y - 3, 94, y + 3), fill=2, width=1)
        draw.point((89, y - 4), fill=3)
    elif action == "medicine":
        add_outlined_shape(image, mask_ellipse((84, y - 10, 99, y + 10)), 1, OUTLINE, 1)
        draw.rectangle((88, y - 13, 95, y - 9), fill=OUTLINE)
        draw.rectangle((89, y - 12, 94, y - 10), fill=2)
        draw.rectangle((90, y - 4, 93, y + 3), fill=5)
        draw.rectangle((88, y - 2, 95, y + 1), fill=5)
    elif action == "play":
        add_outlined_shape(image, mask_ellipse((91, y + 4, 108, y + 19)), 1, OUTLINE, 1)
        draw.line((94, y + 7, 104, y + 16), fill=2, width=2)
        draw.line((83, y - 8, 89, y - 11), fill=1, width=1)
        draw.line((85, y - 2, 91, y - 3), fill=1, width=1)
    elif action == "clean":
        sparkle(draw, 91, y - 8, 4)
        sparkle(draw, 101, y + 6, 3)
        draw.ellipse((82, y + 5, 91, y + 14), outline=1, width=1)
        draw.ellipse((96, y - 2, 103, y + 5), outline=1, width=1)


def make_sprite(stage: str, expression: str = "neutral", action: str = "") -> Image.Image:
    config = STAGES[stage]
    image = canvas()
    draw_appendages(image, stage, config)
    mask, _ = body_mask(config)
    shade_body(image, mask)
    expanded = mask.filter(ImageFilter.MaxFilter(5))
    edge = ImageChops.subtract(expanded, mask)
    image.paste(OUTLINE, (0, 0), edge)
    draw_leaf(image, stage, config)
    draw_face(image, stage, config, expression)
    draw_action_prop(image, action, config)
    return image


def contact_sheet(images: dict[str, Image.Image], path: pathlib.Path) -> None:
    scale = 4
    cell_w = 180 * scale
    cell_h = 176 * scale
    sheet = Image.new("RGBA", (cell_w * 2, cell_h * 2), (15, 22, 36, 255))
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default(size=20)
    for index, (name, image) in enumerate(images.items()):
        x = (index % 2) * cell_w
        y = (index // 2) * cell_h
        sprite = image.convert("RGBA").resize((SIZE * scale, SIZE * scale), Image.Resampling.NEAREST)
        sheet.alpha_composite(sprite, (x + 26 * scale, y + 8 * scale))
        draw.text((x + 20, y + 146 * scale), name.upper(), font=font, fill=(245, 208, 127, 255))
    sheet.save(path)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    stages = {stage: make_sprite(stage) for stage in STAGES}
    for stage, image in stages.items():
        image.save(OUT / f"deskpal-pet-{stage}-128.png", transparency=TRANSPARENT)
    actions = {
        "feed": make_sprite("child", "feed", "feed"),
        "medicine": make_sprite("teen", "neutral", "medicine"),
        "play": make_sprite("teen", "play", "play"),
        "clean": make_sprite("adult", "clean", "clean"),
    }
    for action, image in actions.items():
        image.save(OUT / f"deskpal-pet-action-{action}-128.png", transparency=TRANSPARENT)
    contact_sheet({**stages, **actions}, OUT / "deskpal-pet-v2-contact-sheet.png")
    print(f"wrote {len(stages) + len(actions)} indexed sprites and contact sheet to {OUT}")


if __name__ == "__main__":
    main()
