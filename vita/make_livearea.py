#!/usr/bin/env python3
"""Generate the Vita LiveArea images (original art, no game assets).

A dusk-orange planet rim against a deep starfield, with plain text. The Vita
needs 8-bit indexed PNGs at fixed sizes:
  sce_sys/icon0.png                        128x128
  sce_sys/livearea/contents/bg.png         840x500
  sce_sys/livearea/contents/startup.png    280x158
Run once; the PNGs are committed so a build needs no Python imaging.
"""
import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"


def scene(w, h, seed, planet_cx, planet_cy, planet_r):
    rnd = random.Random(seed)
    img = Image.new("RGB", (w, h))
    px = img.load()
    # Vertical gradient: near-black navy to deep violet.
    for y in range(h):
        t = y / max(1, h - 1)
        r = int(6 + 22 * t)
        g = int(8 + 6 * t)
        b = int(26 + 34 * t)
        for x in range(w):
            px[x, y] = (r, g, b)
    d = ImageDraw.Draw(img)
    # Stars.
    for _ in range(int(w * h / 900)):
        x, y = rnd.randrange(w), rnd.randrange(h)
        v = rnd.randint(120, 255)
        d.point((x, y), fill=(v, v, min(255, v + 20)))
    for _ in range(int(w * h / 12000)):
        x, y = rnd.randrange(w), rnd.randrange(h)
        d.ellipse((x - 1, y - 1, x + 1, y + 1), fill=(230, 235, 255))
    # Atmosphere glow behind the planet.
    glow = Image.new("RGB", (w, h), (0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse((planet_cx - planet_r * 1.12, planet_cy - planet_r * 1.12,
                planet_cx + planet_r * 1.12, planet_cy + planet_r * 1.12),
               fill=(255, 120, 40))
    glow = glow.filter(ImageFilter.GaussianBlur(planet_r * 0.12))
    img = Image.blend(img, glow, 0.0)
    img = Image.composite(glow, img, glow.convert("L").point(lambda v: int(v * 0.75)))
    # Planet body: dark, lit from the upper left.
    d = ImageDraw.Draw(img)
    pr = planet_r
    body = Image.new("RGB", (w, h))
    bp = body.load()
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).ellipse((planet_cx - pr, planet_cy - pr,
                                  planet_cx + pr, planet_cy + pr), fill=255)
    lx, ly = -0.6, -0.7
    for y in range(max(0, int(planet_cy - pr)), min(h, int(planet_cy + pr) + 1)):
        for x in range(max(0, int(planet_cx - pr)), min(w, int(planet_cx + pr) + 1)):
            nx, ny = (x - planet_cx) / pr, (y - planet_cy) / pr
            if nx * nx + ny * ny > 1:
                continue
            nz = math.sqrt(max(0.0, 1 - nx * nx - ny * ny))
            lit = max(0.0, -(nx * lx + ny * ly) + 0.25 * nz)
            band = 0.5 + 0.5 * math.sin(ny * 9.0 + nx * 1.5)
            r = int(30 + 120 * lit + 18 * band)
            g = int(22 + 60 * lit + 10 * band)
            b = int(40 + 35 * lit)
            bp[x, y] = (min(r, 255), min(g, 255), min(b, 255))
    img.paste(body, (0, 0), mask)
    return img


def text(img, xy, s, size, bold=True, fill=(240, 236, 228), anchor="la"):
    d = ImageDraw.Draw(img)
    font = ImageFont.truetype(FONT_BOLD if bold else FONT, size)
    x, y = xy
    d.text((x + 2, y + 2), s, font=font, fill=(0, 0, 0), anchor=anchor)
    d.text((x, y), s, font=font, fill=fill, anchor=anchor)


def save_indexed(img, rel):
    path = os.path.join(HERE, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT,
                                dither=Image.Dither.FLOYDSTEINBERG).save(path, optimize=True)
    print("wrote", rel)


def main():
    icon = scene(128, 128, 7, 98, 112, 70)
    text(icon, (10, 12), "SM", 40)
    text(icon, (12, 56), "RECOMP", 16, bold=True, fill=(255, 170, 90))
    save_indexed(icon, "sce_sys/icon0.png")

    bg = scene(840, 500, 11, 690, 520, 300)
    text(bg, (48, 60), "Super Metroid", 58)
    text(bg, (52, 132), "Recompiled for PS Vita", 26, bold=False, fill=(255, 170, 90))
    text(bg, (52, 420), "Double-tap the left side of the screen for 16:9", 20,
         bold=False, fill=(200, 205, 225))
    save_indexed(bg, "sce_sys/livearea/contents/bg.png")

    st = scene(280, 158, 5, 230, 175, 110)
    text(st, (18, 40), "Super Metroid", 26)
    text(st, (20, 78), "Recomp", 18, bold=False, fill=(255, 170, 90))
    save_indexed(st, "sce_sys/livearea/contents/startup.png")


if __name__ == "__main__":
    sys.exit(main())
