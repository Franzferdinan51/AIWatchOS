#!/usr/bin/env python3
"""Bake the AIWatchOS UI fonts and sprites into committed binary assets.

Implements docs/ui/IMPLEMENTATION_PLAN.md Phase 2: Inter (OFL 1.1, by
Rasmus Andersson — see assets/FONTS-LICENSE.txt) rendered with FreeType
(via Pillow) into 4-bit (16-level) anti-aliased coverage bitmaps, plus
the hand-drawn icon faces and the blurred bubble shadow sprites the
Muse Gloss recipes (DESIGN_TOKENS section 4) are built from.

Everything here is a BUILD-TIME input. The watch never rasterizes: it
blits pre-baked coverage with a tiny interpreter (font.cpp). Output
goes to assets/ and is COMMITTED; this script is the reproducible way
to regenerate it:

    python3 tools/gen_font.py [--ttf-dir DIR]

Inter TTFs are looked up in: --ttf-dir, tools/fonts/, /tmp/interdl/
inter/extras/ttf (dev machine), and finally downloaded from the
official rsms/inter GitHub release (v4.1) into tools/fonts/.

Asset budget (RESOURCE_BUDGET.md): fonts + sprites <= 256 KB total,
verified at the end of the run (hard fail above the cap). At runtime
the assets live in the aiwatchos_assets FAT partition and are loaded
into PSRAM — never in the app image, never in SRAM.

Font .bin format ("AIF1", little-endian, all fields read by offset):
    char[4]  magic "AIF1"          offset 0
    u16      version = 1           offset 4
    u16      glyph_count           offset 6
    i16      ascent                offset 8   (px above baseline)
    i16      descent               offset 10  (px below baseline, +)
    u16      line_height           offset 12
    u32      data_bytes            offset 14  (packed coverage bytes)
    header size = 18 bytes
    glyph_count x GlyphRec (14 bytes each):
        u32  codepoint             offset 0
        u8   w                     offset 4   (0 = no ink, e.g. space)
        u8   h                     offset 5
        i8   xoff                  offset 6   (bitmap left rel. pen)
        i8   yoff                  offset 7   (bitmap top rel. baseline,
                                               negative = above)
        u8   advance               offset 8
        u8   reserved = 0          offset 9
        u32  data_offset           offset 10  (into the coverage blob)
    coverage blob: per glyph, rows top-to-bottom, each row packed
    2 px/byte (high nibble first), rows padded to a whole byte.
    Coverage nibble n blends with alpha n*17.

sprites.bin format ("AIS1", little-endian):
    char[4] magic "AIS1", u16 version=1, u16 sprite_count
    per sprite: u16 w, u16 h, i16 anchor_x, i16 anchor_y,
                u16 cap_top, u16 cap_bot (vstretch caps), u32 data_bytes
    then all sprites' coverage blobs (same packing as fonts).
"""

import io
import math
import os
import struct
import sys
import urllib.request
import zipfile

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(REPO, "assets")
INTER_ZIP_URL = "https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip"
BUDGET_BYTES = 256 * 1024

BASIC = [chr(c) for c in range(0x20, 0x7F)]  # printable ASCII
EXTRA = ["…", "°", "·", "—", "–", "‘", "’", "“", "”", "✓"]
FULL_CHARSET = BASIC + EXTRA

# (output name, ttf file, pixel size, charset)
FACES = [
    ("font_micro15.bin",   "Inter-SemiBold.ttf", 15, FULL_CHARSET),
    ("font_caption16.bin", "Inter-Regular.ttf",  16, FULL_CHARSET),
    ("font_body18.bin",    "Inter-Regular.ttf",  18, FULL_CHARSET),
    ("font_body18sb.bin",  "Inter-SemiBold.ttf", 18, FULL_CHARSET),
    ("font_headline20.bin", "Inter-SemiBold.ttf", 20, FULL_CHARSET),
    ("font_title28.bin",   "Inter-SemiBold.ttf", 28, FULL_CHARSET),
    ("font_pair40.bin",    "Inter-Bold.ttf",     40,
     list("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ")),
    ("font_display96.bin", "Inter-Bold.ttf",     96,
     list("0123456789:.") + [" ", "A", "P", "M", "°"]),
]


def find_ttfs(ttf_dir_arg):
    candidates = []
    if ttf_dir_arg:
        candidates.append(ttf_dir_arg)
    candidates += [
        os.path.join(REPO, "tools", "fonts"),
        "/tmp/interdl/inter/extras/ttf",
        os.path.expanduser("~/Downloads/Inter-4.1/extras/ttf"),
    ]
    for cand in candidates:
        if os.path.exists(os.path.join(cand, "Inter-Regular.ttf")):
            return cand
    # Download the official release into tools/fonts/.
    dest = os.path.join(REPO, "tools", "fonts")
    os.makedirs(dest, exist_ok=True)
    print("downloading Inter 4.1 from", INTER_ZIP_URL)
    data = urllib.request.urlopen(INTER_ZIP_URL).read()
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        for name in zf.namelist():
            if name.startswith("extras/ttf/Inter-") and name.endswith(".ttf"):
                with zf.open(name) as src, open(
                    os.path.join(dest, os.path.basename(name)), "wb"
                ) as out:
                    out.write(src.read())
    return dest


def pack_coverage(img):
    """Quantize an L-mode image to 4-bit coverage, 2 px/byte, rows
    padded to a whole byte, high nibble first."""
    w, h = img.size
    px = img.load()
    out = bytearray()
    for y in range(h):
        for x in range(0, w, 2):
            hi = (px[x, y] >> 4) & 0xF
            lo = ((px[x + 1, y] >> 4) & 0xF) if x + 1 < w else 0
            out.append((hi << 4) | lo)
    return bytes(out)


def serialize_font(glyphs, ascent, descent, line_height):
    """glyphs: list of (codepoint, w, h, xoff, yoff, advance, blob)."""
    out = bytearray()
    out += struct.pack("<4sHHhhHI", b"AIF1", 1, len(glyphs), ascent,
                       descent, line_height,
                       sum(len(g[6]) for g in glyphs))
    offset = 0
    for cp, w, h, xoff, yoff, advance, blob in glyphs:
        out += struct.pack("<IBBbbBBI", cp, w, h, xoff, yoff, advance, 0,
                           offset)
        offset += len(blob)
    for g in glyphs:
        out += g[6]
    return bytes(out)


def bake_face(ttf_path, size, charset):
    font = ImageFont.truetype(ttf_path, size)
    ascent, descent = font.getmetrics()
    glyphs = []
    digit_advances = set()
    for ch in charset:
        cp = ord(ch)
        advance = round(font.getlength(ch))
        if ch.isdigit():
            digit_advances.add(advance)
        if ch == " ":
            glyphs.append((cp, 0, 0, 0, 0, advance, b""))
            continue
        # Render with the baseline placed a full ascent below the
        # canvas top (a smaller pad clips tall glyphs), then crop to
        # the ink box so the record describes exactly the stored bitmap.
        pad = size // 4 + 8
        baseline_y = pad + ascent
        img = Image.new("L", (size * 2 + pad * 2,
                              ascent + descent + pad * 2), 0)
        d = ImageDraw.Draw(img)
        d.text((pad, baseline_y), ch, fill=255, font=font, anchor="ls")
        bbox = img.getbbox()
        if bbox is None:
            glyphs.append((cp, 0, 0, 0, 0, advance, b""))
            continue
        ink = img.crop(bbox)
        # Bitmap left rel. pen = bbox.x0 - pad; top rel. baseline =
        # bbox.y0 - baseline_y (negative above the baseline).
        glyphs.append((cp, ink.size[0], ink.size[1], bbox[0] - pad,
                       bbox[1] - baseline_y, advance, pack_coverage(ink)))
    if len(digit_advances) > 1:
        zero_adv = next(g[5] for g in glyphs if g[0] == ord("0"))
        fixed = []
        for cp, w, h, xoff, yoff, advance, blob in glyphs:
            ch = chr(cp)
            target = None
            if ch.isdigit():
                target = zero_adv
            elif size >= 90 and ch in ":.":
                target = zero_adv // 2
            if target is not None and advance != target:
                xoff += (target - advance) // 2
                advance = target
            fixed.append((cp, w, h, xoff, yoff, advance, blob))
        glyphs = fixed
    return serialize_font(glyphs, ascent, descent,
                          ascent + descent), len(glyphs)


# ---------------------------------------------------------------------------
# Icons: hand-drawn on a 24-unit grid (Lucide-style geometry), stroked
# with PIL. Codepoints are Private Use Area U+E001.. in the order of
# ICON_NAMES; icons.hpp on the C++ side mirrors this table.
# ---------------------------------------------------------------------------

ICON_NAMES = [
    "mic", "feather", "sparkle", "gear", "bell", "clock", "sun",
    "cloud", "cloudsun", "cloudrain", "cloudsnow", "check", "chev_r",
    "wifi", "wifi_off", "moon", "volume", "info", "alert", "x",
    "arrow_up", "arrow_down", "heart", "play", "pause", "music",
    "send", "dot",
]
ICON_BASE = 0xE001
WX_ICONS = ["sun", "cloud", "cloudsun", "cloudrain", "cloudsnow"]


def _pts(pts, s):
    return [(x * s, y * s) for x, y in pts]


def _line(d, pts, s, w=2.0):
    d.line(_pts(pts, s), fill=255, width=max(1, round(w * s)),
           joint="curve")


def _circle(d, cx, cy, r, s, w=2.0, fill=None):
    box = [(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s]
    if fill is not None:
        d.ellipse(box, fill=fill)
    else:
        d.ellipse(box, outline=255, width=max(1, round(w * s)))


def _arc(d, cx, cy, r, start, end, s, w=2.0):
    box = [(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s]
    d.arc(box, start, end, fill=255, width=max(1, round(w * s)))


def _cloud_path(d, s, ox=0.0, oy=0.0, k=1.0):
    """Solid cloud: union of three circles + a base rectangle."""
    def C(cx, cy, r):
        d.ellipse([(ox + (cx - r) * k) * s, (oy + (cy - r) * k) * s,
                   (ox + (cx + r) * k) * s, (oy + (cy + r) * k) * s],
                  fill=255)
    C(9.0, 14.5, 3.6)
    C(13.8, 11.0, 4.6)
    C(17.8, 14.5, 3.4)
    d.rectangle([(ox + 5.4 * k) * s, (oy + 13.0 * k) * s,
                 (ox + 21.2 * k) * s, (oy + 18.0 * k) * s], fill=255)


def draw_icon(name, S):
    """Render one icon at em size S; returns an L-mode square image."""
    img = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(img)
    s = S / 24.0

    if name == "mic":
        d.rounded_rectangle([9 * s, 2 * s, 15 * s, 13 * s], radius=3 * s,
                            outline=255, width=max(1, round(2 * s)))
        _arc(d, 12, 12.5, 6.5, 0, 180, s)
        _line(d, [(12, 19), (12, 22)], s)
    elif name == "feather":
        _arc(d, 11.5, 11.5, 8.5, 185, 355, s)
        _line(d, [(19.5, 4.5), (4.8, 19.2)], s)
        _line(d, [(4.8, 19.2), (3.5, 21.5)], s)
        _line(d, [(8.2, 15.2), (13.5, 13.5)], s, 1.6)
        _line(d, [(7.2, 11.0), (12.8, 9.8)], s, 1.6)
    elif name == "sparkle":
        d.polygon(_pts([(12, 2), (14.3, 9.7), (22, 12), (14.3, 14.3),
                        (12, 22), (9.7, 14.3), (2, 12), (9.7, 9.7)], s),
                  fill=255)
    elif name == "gear":
        for k in range(8):
            a = math.radians(k * 45)
            _line(d, [(12 + 6.0 * math.cos(a), 12 + 6.0 * math.sin(a)),
                      (12 + 9.4 * math.cos(a), 12 + 9.4 * math.sin(a))],
                  s, 3.4)
        _circle(d, 12, 12, 5.6, s)
        _circle(d, 12, 12, 2.1, s, fill=0)
    elif name == "bell":
        _arc(d, 12, 9.5, 5.5, 180, 360, s)
        _line(d, [(6.5, 9.5), (5.2, 17.5), (18.8, 17.5), (17.5, 9.5)], s)
        _circle(d, 12, 20.2, 1.6, s, fill=255)
    elif name == "clock":
        _circle(d, 12, 12, 9.2, s)
        _line(d, [(12, 12), (12, 6.8)], s)
        _line(d, [(12, 12), (15.8, 13.8)], s)
    elif name == "sun":
        _circle(d, 12, 12, 4.3, s)
        for k in range(8):
            a = math.radians(k * 45)
            _line(d, [(12 + 6.3 * math.cos(a), 12 + 6.3 * math.sin(a)),
                      (12 + 8.7 * math.cos(a), 12 + 8.7 * math.sin(a))], s)
    elif name == "cloud":
        _cloud_path(d, s)
    elif name == "cloudsun":
        sub = Image.new("L", (S, S), 0)
        sd = ImageDraw.Draw(sub)
        _circle(sd, 8.5, 8.0, 3.2, s, 1.8)
        for k in range(8):
            a = math.radians(k * 45)
            _line(sd, [(8.5 + 4.8 * math.cos(a), 8.0 + 4.8 * math.sin(a)),
                       (8.5 + 6.4 * math.cos(a), 8.0 + 6.4 * math.sin(a))],
                  s, 1.6)
        img = ImageChops.lighter(img, sub)
        d = ImageDraw.Draw(img)
        _cloud_path(d, s, ox=3.5, oy=4.5, k=0.82)
    elif name == "cloudrain":
        _cloud_path(d, s, oy=-2.0)
        for x in (8.5, 13.0, 17.5):
            _line(d, [(x, 19.0), (x - 1.2, 22.2)], s, 1.8)
    elif name == "cloudsnow":
        _cloud_path(d, s, oy=-2.0)
        for x in (8.5, 13.0, 17.5):
            _circle(d, x, 20.5, 1.0, s, fill=255)
    elif name == "check":
        _line(d, [(4, 12.8), (9.3, 18), (20, 6.5)], s, 2.4)
    elif name == "chev_r":
        _line(d, [(9, 4.5), (16.5, 12), (9, 19.5)], s, 2.2)
    elif name in ("wifi", "wifi_off"):
        for r in (11.5, 8.0, 4.6):
            _arc(d, 12, 16.5, r, 228, 312, s, 2.2)
        _circle(d, 12, 17.8, 1.5, s, fill=255)
        if name == "wifi_off":
            _line(d, [(4.5, 4.5), (19.5, 19.5)], s, 2.2)
    elif name == "moon":
        _circle(d, 12, 12, 9.0, s, fill=255)
        _circle(d, 16.8, 9.2, 7.4, s, fill=0)
    elif name == "volume":
        d.polygon(_pts([(3.5, 9.5), (7.5, 9.5), (13, 5), (13, 19),
                        (7.5, 14.5), (3.5, 14.5)], s), fill=255)
        _arc(d, 15.0, 12, 3.6, 295, 65, s)
        _arc(d, 15.0, 12, 6.8, 295, 65, s)
    elif name == "info":
        _circle(d, 12, 12, 9.0, s)
        _line(d, [(12, 11), (12, 16.2)], s, 2.2)
        _circle(d, 12, 7.8, 1.3, s, fill=255)
    elif name == "alert":
        d.line(_pts([(12, 3.5), (21.5, 20), (2.5, 20), (12, 3.5)], s),
               fill=255, width=max(1, round(2 * s)), joint="curve")
        _line(d, [(12, 10), (12, 14.6)], s, 2.2)
        _circle(d, 12, 17.3, 1.2, s, fill=255)
    elif name == "x":
        _line(d, [(5, 5), (19, 19)], s, 2.2)
        _line(d, [(19, 5), (5, 19)], s, 2.2)
    elif name == "arrow_up":
        _line(d, [(12, 20.5), (12, 4.5)], s, 2.2)
        _line(d, [(5.5, 11), (12, 4.5), (18.5, 11)], s, 2.2)
    elif name == "arrow_down":
        _line(d, [(12, 3.5), (12, 19.5)], s, 2.2)
        _line(d, [(5.5, 13), (12, 19.5), (18.5, 13)], s, 2.2)
    elif name == "heart":
        _circle(d, 8.2, 8.8, 3.7, s, fill=255)
        _circle(d, 15.8, 8.8, 3.7, s, fill=255)
        d.polygon(_pts([(4.7, 10.2), (19.3, 10.2), (12, 20.8)], s),
                  fill=255)
    elif name == "play":
        d.polygon(_pts([(8, 5), (19.5, 12), (8, 19)], s), fill=255)
    elif name == "pause":
        d.rectangle([6.8 * s, 5 * s, 10.4 * s, 19 * s], fill=255)
        d.rectangle([13.6 * s, 5 * s, 17.2 * s, 19 * s], fill=255)
    elif name == "music":
        d.polygon(_pts([(10, 5.2), (18.5, 3.6), (18.5, 7.2), (10, 8.8)],
                       s), fill=255)
        _line(d, [(10, 5.2), (10, 17.5)], s)
        _line(d, [(18.5, 3.6), (18.5, 15.8)], s)
        _circle(d, 7.6, 18.2, 2.6, s, fill=255)
        _circle(d, 16.1, 16.5, 2.6, s, fill=255)
    elif name == "send":
        d.line(_pts([(21.5, 2.5), (11.5, 12.8), (3, 9.8), (7, 16.8),
                     (11.5, 12.8)], s), fill=255,
               width=max(1, round(1.9 * s)), joint="curve")
        d.line(_pts([(11.5, 12.8), (21.5, 2.5)], s), fill=255,
               width=max(1, round(1.9 * s)))
    elif name == "dot":
        _circle(d, 12, 12, 4.5, s, fill=255)
    return img


def bake_icon_face(names, size):
    """An AIF1 font whose glyphs are the named icons at `size` px em.
    Baseline = em bottom, so draw order math matches text faces."""
    glyphs = []
    for name in names:
        cp = ICON_BASE + ICON_NAMES.index(name)
        img = draw_icon(name, size)
        bbox = img.getbbox()
        if bbox is None:
            glyphs.append((cp, 0, 0, 0, 0, size, b""))
            continue
        ink = img.crop(bbox)
        # Canvas origin = em top-left; baseline sits at em bottom.
        glyphs.append((cp, ink.size[0], ink.size[1], bbox[0],
                       bbox[1] - size, size, pack_coverage(ink)))
    return serialize_font(glyphs, size, 0, size), len(glyphs)


# ---------------------------------------------------------------------------
# Shadow sprites: bubble silhouettes blurred at build time (recipe G4).
# ---------------------------------------------------------------------------

def _shadow_sprite(w, h, r, canvas_w, canvas_h, shape_x, shape_y):
    img = Image.new("L", (canvas_w, canvas_h), 0)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([shape_x, shape_y, shape_x + w, shape_y + h],
                        radius=r, fill=255)
    return img.filter(ImageFilter.GaussianBlur(8))


def bake_sprites():
    # (image, anchor_x, anchor_y, cap_top, cap_bot)
    sprites = [
        # tile shadow: 96x96 r24 shape; sprite extends 16px past the
        # tile on the sides/top and 24px below (shape dropped 8px).
        (_shadow_sprite(96, 96, 24, 128, 128, 16, 24), -16, -8, 0, 0),
        # pill shadow: 64x32 r16 (battery pill, chips).
        (_shadow_sprite(64, 32, 16, 96, 64, 16, 24), -16, -8, 0, 0),
        # card shadow: 392x152 r22 reference; vstretch caps cover the
        # corner + blur region so taller/shorter cards keep true corners.
        (_shadow_sprite(392, 152, 22, 424, 184, 16, 24), -16, -8, 54, 40),
    ]
    out = bytearray()
    out += struct.pack("<4sHH", b"AIS1", 1, len(sprites))
    blobs = bytearray()
    for img, ax, ay, cap_t, cap_b in sprites:
        blob = pack_coverage(img)
        out += struct.pack("<HHhhHHI", img.size[0], img.size[1], ax, ay,
                           cap_t, cap_b, len(blob))
        blobs += blob
    return bytes(out + blobs)


def main():
    ttf_arg = None
    if "--ttf-dir" in sys.argv:
        ttf_arg = sys.argv[sys.argv.index("--ttf-dir") + 1]
    ttf_dir = find_ttfs(ttf_arg)
    print("TTF dir:", ttf_dir)
    os.makedirs(ASSETS, exist_ok=True)
    total = 0

    for fname, ttf, size, charset in FACES:
        data, n = bake_face(os.path.join(ttf_dir, ttf), size, charset)
        with open(os.path.join(ASSETS, fname), "wb") as f:
            f.write(data)
        total += len(data)
        print(f"  {fname}: {len(data)} B ({n} glyphs, {ttf} {size}px)")

    for fname, size in (("font_icon20.bin", 20), ("font_icon40.bin", 40)):
        data, n = bake_icon_face(ICON_NAMES, size)
        with open(os.path.join(ASSETS, fname), "wb") as f:
            f.write(data)
        total += len(data)
        print(f"  {fname}: {len(data)} B ({n} icons @ {size}px)")

    data, n = bake_icon_face(WX_ICONS, 64)
    with open(os.path.join(ASSETS, "font_wx64.bin"), "wb") as f:
        f.write(data)
    total += len(data)
    print(f"  font_wx64.bin: {len(data)} B ({n} icons @ 64px)")

    data = bake_sprites()
    with open(os.path.join(ASSETS, "sprites.bin"), "wb") as f:
        f.write(data)
    total += len(data)
    print(f"  sprites.bin: {len(data)} B")

    lic_src = os.path.join(os.path.dirname(ttf_dir), "..", "LICENSE.txt")
    lic_fallback = os.path.join(os.path.dirname(ttf_dir), "LICENSE.txt")
    lic_text = None
    for cand in (lic_src, lic_fallback):
        if os.path.exists(cand):
            lic_text = open(cand, encoding="utf-8").read()
            break
    if lic_text:
        with open(os.path.join(ASSETS, "FONTS-LICENSE.txt"), "w") as f:
            f.write(lic_text)
    else:
        print("  NOTE: Inter LICENSE.txt not found next to the TTFs; "
              "FONTS-LICENSE.txt not written")

    print(f"TOTAL assets: {total} B of {BUDGET_BYTES} B budget")
    if total > BUDGET_BYTES:
        print("OVER BUDGET — cut order (plan 2.1): display96 -> title28 "
              "fallback, then micro15")
        sys.exit(1)


if __name__ == "__main__":
    main()
