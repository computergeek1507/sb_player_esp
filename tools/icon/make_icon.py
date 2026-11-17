#!/usr/bin/env python3
"""Draws the SB Card Builder icon (an SD card full of pixel lights) and writes every
size the three apps need. Needs Pillow:  python -m pip install pillow

  python tools/icon/make_icon.py
"""
import os
from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
S = 1024      # design size
SS = 2        # supersample factor for smooth edges

# 3 x 3 pixel lights, a rainbow running diagonally
LIGHTS = [
    ["#ff3b5c", "#ff8a1f", "#ffd60a"],
    ["#ff8a1f", "#ffd60a", "#3ddc5a"],
    ["#ffd60a", "#3ddc5a", "#22b8ff"],
]


def rgb(hex_colour):
    return tuple(int(hex_colour[i:i + 2], 16) for i in (1, 3, 5))


def gradient(size, top, bottom):
    """Vertical gradient image."""
    column = Image.new("RGB", (1, size))
    a, b = rgb(top), rgb(bottom)
    column.putdata([tuple(round(a[c] + (b[c] - a[c]) * y / (size - 1)) for c in range(3)) for y in range(size)])
    return column.resize((size, size))


def mask(draw_fn):
    m = Image.new("L", (S * SS, S * SS), 0)
    draw_fn(ImageDraw.Draw(m), SS)
    return m


def card_outline(k, grow=0):
    """SD card: a tall rounded rectangle with the top-right corner cut off."""
    left, top, right, bottom = 262 - grow, 150 - grow, 762 + grow, 874 + grow
    cut = 150
    return [(x * k, y * k) for x, y in [
        (left, top), (right - cut, top), (right, top + cut), (right, bottom), (left, bottom)]]


def draw(full_bleed=False):
    """full_bleed fills the whole square (for maskable web icons, which get cropped to a circle)."""
    big = S * SS
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))

    # background tile
    tile = mask(lambda d, k: d.rectangle([0, 0, S * k, S * k], fill=255) if full_bleed else
                d.rounded_rectangle([40 * k, 40 * k, (S - 40) * k, (S - 40) * k], radius=210 * k, fill=255))
    img.paste(gradient(big, "#27306b", "#0a0d22"), (0, 0), tile)

    # coloured glow behind the card, so the lights seem to light the tile
    glow = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    g = ImageDraw.Draw(glow)
    for (cx, cy, colour) in [(360, 420, "#ff3b5c"), (512, 600, "#ffd60a"), (680, 760, "#22b8ff")]:
        g.ellipse([(cx - 260) * SS, (cy - 260) * SS, (cx + 260) * SS, (cy + 260) * SS], fill=rgb(colour) + (110,))
    glow = glow.filter(ImageFilter.GaussianBlur(90 * SS))
    glow.putalpha(ImageChops.multiply(glow.getchannel("A"), tile))
    img.alpha_composite(glow)

    # card: soft shadow, light rim, dark body
    shadow = mask(lambda d, k: d.polygon([(x + 0, y + 26 * k) for x, y in card_outline(k)], fill=150))
    shadow = shadow.filter(ImageFilter.GaussianBlur(26 * SS))
    img.paste((0, 0, 0, 255), (0, 0), ImageChops.multiply(shadow, tile))
    round_join = dict(width=44 * SS, joint="curve")

    def card_mask(grow):
        def fn(d, k):
            pts = card_outline(k, grow)
            d.polygon(pts, fill=255)
            d.line(pts + [pts[0], pts[1]], fill=255, **round_join)   # rounds the corners
        return mask(fn)
    img.paste(gradient(big, "#aab6ff", "#5461c9"), (0, 0), card_mask(10))
    img.paste(gradient(big, "#2a3166", "#141837"), (0, 0), card_mask(0))

    d = ImageDraw.Draw(img)
    # gold contacts along the top edge
    for i in range(4):
        x = 300 + i * 78
        d.rounded_rectangle([x * SS, 176 * SS, (x + 46) * SS, 300 * SS], radius=14 * SS, fill="#f5c542")
        d.rounded_rectangle([x * SS, 176 * SS, (x + 46) * SS, 214 * SS], radius=14 * SS, fill="#ffe08a")

    # pixel lights: blurred halo, solid bulb, small highlight
    centres = [(512 + (col - 1) * 152, 600 + (row - 1) * 152) for row in range(3) for col in range(3)]
    colours = [c for row in LIGHTS for c in row]
    halo = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    h = ImageDraw.Draw(halo)
    for (cx, cy), colour in zip(centres, colours):
        h.ellipse([(cx - 76) * SS, (cy - 76) * SS, (cx + 76) * SS, (cy + 76) * SS], fill=rgb(colour) + (230,))
    img.alpha_composite(halo.filter(ImageFilter.GaussianBlur(26 * SS)))
    d = ImageDraw.Draw(img)
    for (cx, cy), colour in zip(centres, colours):
        d.ellipse([(cx - 52) * SS, (cy - 52) * SS, (cx + 52) * SS, (cy + 52) * SS], fill=colour)
        d.ellipse([(cx - 30) * SS, (cy - 34) * SS, (cx - 2) * SS, (cy - 8) * SS], fill=(255, 255, 255, 150))
    return img.resize((S, S), Image.LANCZOS)


def save(img, size, *parts):
    path = os.path.join(*parts)
    if os.path.isdir(os.path.dirname(path)):
        img.resize((size, size), Image.LANCZOS).save(path)
        print("wrote", os.path.relpath(path, TOOLS))


def main():
    icon, bleed = draw(), draw(full_bleed=True)
    ico_sizes = [(n, n) for n in (16, 24, 32, 48, 64, 128, 256)]
    icon.save(os.path.join(HERE, "sb_card_builder.png"))
    icon.save(os.path.join(HERE, "sb_card_builder.ico"), sizes=ico_sizes)
    print("wrote icon/sb_card_builder.png, icon/sb_card_builder.ico")
    save(icon, 256, TOOLS, "sb_card_builder.png")                      # Python app (next to the script)
    qt = os.path.join(TOOLS, "sb_card_builder_qt", "res")
    if os.path.isdir(qt):
        icon.save(os.path.join(qt, "app.ico"), sizes=ico_sizes)
        save(icon, 256, qt, "app.png")
        print("wrote sb_card_builder_qt/res/app.ico")
    fl = os.path.join(TOOLS, "sb_card_builder_flutter")
    win = os.path.join(fl, "windows", "runner", "resources")
    if os.path.isdir(win):
        icon.save(os.path.join(win, "app_icon.ico"), sizes=ico_sizes)
        print("wrote sb_card_builder_flutter/windows/runner/resources/app_icon.ico")
    save(icon, 32, fl, "web", "favicon.png")
    for n in (192, 512):
        save(icon, n, fl, "web", "icons", f"Icon-{n}.png")
        save(bleed, n, fl, "web", "icons", f"Icon-maskable-{n}.png")


if __name__ == "__main__":
    main()
