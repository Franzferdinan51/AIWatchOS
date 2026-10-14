#!/usr/bin/env python3
"""Bake the Muse boot sprite from the brand webp.

Crops the blue "M" mark, resizes to watch width, and writes
assets/boot_muse.bin: magic "AIBM", u16 LE w/h, then per-pixel
u8 alpha + u16 LE rgb565. Alpha comes from inkiness (white
background goes transparent), so the mark composites cleanly over
the black boot screen with no halo.

Usage: python3 tools/gen_boot_sprite.py [input.webp]
"""
import struct
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
    "/Users/duckets/Downloads/muselogo.webp")
DST = ROOT / "assets" / "boot_muse.bin"
TARGET_W = 300
MARGIN = 28


def main() -> None:
    im = Image.open(SRC).convert("RGB")
    w, h = im.size
    px = im.load()

    def is_blue(p):
        r, g, b = p
        return b > 120 and b > r + 40 and b > g + 10

    # M-only rows: the Meta mark at the bottom is separated by a gap.
    rows = []
    for y in range(h):
        n = sum(1 for x in range(0, w, 3) if is_blue(px[x, y]))
        if n > 3:
            rows.append(y)
    seg = [y for y in rows if y < 1000]
    xs = []
    for y in seg:
        for x in range(0, w, 3):
            if is_blue(px[x, y]):
                xs.append(x)
    x0, x1 = max(0, min(xs) - MARGIN), min(w, max(xs) + MARGIN)
    y0, y1 = max(0, min(seg) - MARGIN), min(h, max(seg) + MARGIN)

    crop = im.crop((x0, y0, x1, y1))
    scale = TARGET_W / crop.width
    small = crop.resize((TARGET_W, round(crop.height * scale)), Image.LANCZOS)
    sw, sh = small.size
    spx = small.load()

    def rgb565(r, g, b):
        return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

    out = bytearray()
    out += b"AIBM"
    out += struct.pack("<HH", sw, sh)
    ink = 0
    for y in range(sh):
        for x in range(sw):
            r, g, b = spx[x, y]
            # Inkiness: white (255,255,255) -> transparent, saturated
            # color -> opaque. min() keeps bright-blue AA edges smooth.
            lum = min(r, g, b)
            a = max(0, min(255, 255 - int(lum * 255 / 235)))
            if a < 12:
                a = 0
            else:
                ink += 1
            out.append(a)
            out += struct.pack("<H", rgb565(r, g, b))
    DST.write_bytes(bytes(out))
    print(f"{DST.name}: {sw}x{sh}, {len(out)} bytes, {ink} inked px")


if __name__ == "__main__":
    main()
