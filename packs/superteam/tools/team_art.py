#!/usr/bin/env python3
"""Super Team's team art, from its logo (art/super_omega.png, "SUPER" over an omega in metal on a dark
wall) and Waluigi's ExtraTextures.rlt, whose textures Super Team shows in Waluigi's place:

- news_superteam.gxt: the Striker Times photo (the logo, 256x256);
- captain_icons_superteam.gxt: its captain picture in the HUD (the logo, 128x128);
- logos_TEAM_superteam_bg.gxt: its emblem behind its team on partner select;
- ExtraTextures.rlt: Waluigi's, with the logo for his emblem (cut out of the wall) and on his
  banners where his emblem was, and his purple (banners, Mega Strike backdrops, gloves) made Super
  Team's blue.

    team_art.py --logo <super_omega.png> --extra <Waluigi's ExtraTextures.rlt> --out-extra <ExtraTextures.rlt>
                --out-news <news_superteam.gxt> --out-icon <captain_icons_superteam.gxt>
                --out-board <logos_TEAM_superteam_bg.gxt> [--preview <dir>]

Pure Python (the image work is small): PNG in, GX CMPR textures out.
"""

import argparse
import colorsys
import os
import struct
import sys
import zlib

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "..", "..", "..", "scripts", "modkit"))
import gxtex  # noqa: E402

TEAM_BLUE = (0x3A, 0x6E, 0xA5)


# ---- PNG (8-bit greyscale, RGB or RGBA, not interlaced) ----

def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    off, idat, w = 8, bytearray(), 0
    while off < len(data):
        n = struct.unpack(">I", data[off:off + 4])[0]
        kind, body = data[off + 4:off + 8], data[off + 8:off + 8 + n]
        if kind == b"IHDR":
            w, h, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or colour not in (0, 2, 6) or interlace:
                raise ValueError("%s: needs an 8-bit, non-interlaced greyscale, RGB or RGBA PNG" % path)
            channels = {0: 1, 2: 3, 6: 4}[colour]
        elif kind == b"IDAT":
            idat += body
        off += 12 + n
    raw = zlib.decompress(bytes(idat))
    stride = w * channels
    rows, prev = [], bytearray(stride)
    for y in range(h):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if kind == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif kind == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif kind == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 0xFF
        rows.append(line)
        prev = line
    if channels == 1:
        return [[(v, v, v, 255) for v in row] for row in rows]
    if channels == 3:
        return [[(row[i], row[i + 1], row[i + 2], 255) for i in range(0, stride, 3)] for row in rows]
    return [[tuple(row[i:i + 4]) for i in range(0, stride, 4)] for row in rows]


# ---- images: rows of (r, g, b, a) ----

def luminance(p):
    return (299 * p[0] + 587 * p[1] + 114 * p[2]) // 1000


def crop_padded(img, x0, y0, x1, y1, top, bottom):
    """img[y0:y1][x0:x1]; past the picture's edges, its border bands (`top` rows at the top, `bottom`
    at the bottom, the full width at the sides) mirrored back and forth, so only plain wall repeats."""
    h, w = len(img), len(img[0])

    def bounce(v, start, n):  # v folded into [start, start + n)
        period = 2 * n
        k = (v - start) % period
        return start + (k if k < n else period - 1 - k)

    def fy(y):
        if y < 0:
            return bounce(y, 0, top)
        if y >= h:
            return bounce(y, h - bottom, bottom)
        return y

    def fx(x):
        return bounce(x, 0, w) if x < 0 or x >= w else x
    return [[img[fy(y)][fx(x)] for x in range(x0, x1)] for y in range(y0, y1)]


def shrink(img, w, h):
    """img averaged down to w x h (area-weighted; colour weighted by alpha, so transparent pixels
    don't darken edges)."""
    sx, sy = len(img[0]) / float(w), len(img) / float(h)
    out = []
    for oy in range(h):
        fy0, fy1 = oy * sy, (oy + 1) * sy
        row = []
        for ox in range(w):
            fx0, fx1 = ox * sx, (ox + 1) * sx
            r = g = b = a = area = 0.0
            for y in range(int(fy0), min(len(img), int(fy1 + 0.999999))):
                wy = min(fy1, y + 1) - max(fy0, y)
                if wy <= 0:
                    continue
                src = img[y]
                for x in range(int(fx0), min(len(src), int(fx1 + 0.999999))):
                    wgt = (min(fx1, x + 1) - max(fx0, x)) * wy
                    if wgt <= 0:
                        continue
                    p = src[x]
                    pa = p[3] * wgt
                    r += p[0] * pa
                    g += p[1] * pa
                    b += p[2] * pa
                    a += pa
                    area += wgt
            if a > 0:
                row.append((int(r / a + 0.5), int(g / a + 0.5), int(b / a + 0.5), int(a / area + 0.5)))
            else:
                row.append((0, 0, 0, 0))
        out.append(row)
    return out


def rotate_ccw(img):
    """A quarter turn anticlockwise: the image's top ends up on the left."""
    h, w = len(img), len(img[0])
    return [[img[x][w - 1 - y] for x in range(h)] for y in range(w)]


def blank(w, h, colour=(0, 0, 0, 0)):
    return [[colour] * w for _ in range(h)]


def paste(dst, src, ox, oy):
    """src alpha-composited onto dst at (ox, oy)."""
    for y, row in enumerate(src):
        Y = y + oy
        if Y < 0 or Y >= len(dst):
            continue
        out = dst[Y]
        for x, p in enumerate(row):
            X = x + ox
            if X < 0 or X >= len(out) or p[3] == 0:
                continue
            t = p[3] / 255.0
            q = out[X]
            out[X] = (int(p[0] * t + q[0] * (1 - t) + 0.5), int(p[1] * t + q[1] * (1 - t) + 0.5),
                      int(p[2] * t + q[2] * (1 - t) + 0.5), max(q[3], p[3]))


def to_blue(p):
    """Purples (and the blue-violets beside them) turned to the team's blue, keeping their shade."""
    h, s, v = colorsys.rgb_to_hsv(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0)
    deg = h * 360
    if s < 0.12 or not 225 <= deg <= 330:
        return p
    t = min(1.0, (deg - 225) / 15.0)  # the blue-violets, partly
    target = colorsys.rgb_to_hsv(*(c / 255.0 for c in TEAM_BLUE))[0] * 360
    nh = (deg + (target - deg) * t) / 360.0
    r, g, b = colorsys.hsv_to_rgb(nh, s * (1 - 0.15 * t), v)
    return (int(r * 255 + 0.5), int(g * 255 + 0.5), int(b * 255 + 0.5), p[3])


# ---- the logo, cut out of the wall ----

def glyphs(img):
    """The logo's two parts, "SUPER" and the omega, each a cut-out (alpha from how much brighter than
    the wall a pixel is), with their boxes in img."""
    h, w = len(img), len(img[0])
    lum = [[luminance(p) for p in row] for row in img]
    rows = [any(v > 100 for v in row) for row in lum]
    parts, y = [], 0
    while y < h:
        if not rows[y]:
            y += 1
            continue
        start = y
        while y < h and rows[y]:
            y += 1
        if y - start > 8:
            parts.append((start, y))
    if len(parts) != 2:
        raise ValueError("the logo should be two rows: \"SUPER\" over the omega (found %d)" % len(parts))
    out = []
    for y0, y1 in parts:
        cols = [x for x in range(w) if any(lum[y][x] > 100 for y in range(y0, y1))]
        x0, x1 = cols[0], cols[-1] + 1
        pad = 4
        x0, y0, x1, y1 = max(0, x0 - pad), max(0, y0 - pad), min(w, x1 + pad), min(h, y1 + pad)
        solid = [[lum[y][x] > 80 for x in range(x0, x1)] for y in range(y0, y1)]
        fill_specks(solid, 400)
        cut = []
        for j, y in enumerate(range(y0, y1)):
            row = []
            for i, x in enumerate(range(x0, x1)):
                p = img[y][x]
                a = 1.0 if solid[j][i] and lum[y][x] >= 105 else max(0.0, min(1.0, (lum[y][x] - 55) / 50.0))
                if solid[j][i] and lum[y][x] < 105:
                    a = max(a, 0.95)  # a dark speck in the metal, not a hole
                row.append((p[0], p[1], p[2], int(a * 255 + 0.5)))
            cut.append(row)
        out.append((cut, (x0, y0, x1, y1)))
    return out


def fill_specks(mask, largest):
    """Fills the holes in a mask smaller than `largest` pixels that the outside doesn't reach (dark
    specks in the metal; the letters' own holes, P's and R's, are larger)."""
    h, w = len(mask), len(mask[0])
    seen = [[False] * w for _ in range(h)]
    for sy in range(h):
        for sx in range(w):
            if mask[sy][sx] or seen[sy][sx]:
                continue
            region, stack, edge = [], [(sx, sy)], False
            seen[sy][sx] = True
            while stack:
                x, y = stack.pop()
                region.append((x, y))
                if x in (0, w - 1) or y in (0, h - 1):
                    edge = True
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if 0 <= nx < w and 0 <= ny < h and not seen[ny][nx] and not mask[ny][nx]:
                        seen[ny][nx] = True
                        stack.append((nx, ny))
            if not edge and len(region) < largest:
                for x, y in region:
                    mask[y][x] = True


def stacked(img, size, gap=0.06):
    """The logo's cut-out stacked into a square of `size`: "SUPER" across the top, the omega under it,
    filling the rest (as wide as at most 0.85 of "SUPER")."""
    (word, wbox), (omega, obox) = glyphs(img)
    ww, wh = wbox[2] - wbox[0], wbox[3] - wbox[1]
    ow, oh = obox[2] - obox[0], obox[3] - obox[1]
    word_w = size
    word_h = max(1, int(round(wh * word_w / float(ww))))
    space = int(round(gap * size))
    omega_h = size - word_h - space
    omega_w = int(round(ow * omega_h / float(oh)))
    if omega_w > int(size * 0.85):
        omega_w = int(size * 0.85)
        omega_h = int(round(oh * omega_w / float(ow)))
    top = (size - (word_h + space + omega_h)) // 2
    canvas = blank(size, size)
    paste(canvas, shrink(word, word_w, word_h), (size - word_w) // 2, top)
    paste(canvas, shrink(omega, omega_w, omega_h), (size - omega_w) // 2, top + word_h + space)
    return canvas


def emblem(img, size, outline=(0x0B, 0x16, 0x26), ring=TEAM_BLUE):
    """The team emblem: the stacked logo in a dark outline and a ring of the team's blue (the texture
    keeps 1-bit alpha, so its edges need them on any background)."""
    border = max(2, size // 32)
    inner = size - 4 * border
    canvas = blank(size, size)
    paste(canvas, stacked(img, inner), 2 * border, 2 * border)
    return outlined(canvas, border, outline, ring)


def outlined(img, border, outline, ring):
    """Solid where alpha >= half, then a dark line and a ring of `ring` around that."""
    h, w = len(img), len(img[0])
    solid = [[p[3] >= 128 for p in row] for row in img]

    def grow(mask, r):
        out = [row[:] for row in mask]
        offsets = [(dx, dy) for dy in range(-r, r + 1) for dx in range(-r, r + 1) if dx * dx + dy * dy <= r * r + r]
        for y in range(h):
            for x in range(w):
                if mask[y][x]:
                    for dx, dy in offsets:
                        X, Y = x + dx, y + dy
                        if 0 <= X < w and 0 <= Y < h:
                            out[Y][X] = True
        return out
    line = grow(solid, max(1, border // 2))
    halo = grow(line, max(1, border - border // 2)) if ring else line
    out = []
    for y in range(h):
        row = []
        for x in range(w):
            p = img[y][x]
            if solid[y][x]:
                row.append((p[0], p[1], p[2], 255))
            elif line[y][x]:
                row.append(outline + (255,))
            elif halo[y][x]:
                row.append(ring + (255,))
            else:
                row.append((0, 0, 0, 0))
        out.append(row)
    return out


# ---- the HUD's captain picture ----

def captain_icon(img, size=128, fill=0.84):
    """The captain picture the HUD (and the menus with it) shows: the stacked logo in silver, lifted by
    a shadow, on the team's blue fading to near black at the corners."""
    centre, edge = (0x24, 0x48, 0x74), (0x06, 0x0C, 0x16)
    half = (size - 1) / 2.0
    icon = []
    for y in range(size):
        row = []
        for x in range(size):
            t = min(1.0, ((x - half) ** 2 + (y - half) ** 2) ** 0.5 / (half * 1.41421356))
            row.append(_mix(centre, edge, t ** 0.8))
        icon.append(row)
    art = stacked(img, int(round(size * fill)))
    at = (size - len(art)) // 2
    shadow = [[(0, 0, 0, int(p[3] * 0.7)) for p in row] for row in art]
    paste(icon, shadow, at + max(1, size // 96), at + max(1, size // 64))
    paste(icon, art, at, at)
    return icon


# ---- the Striker Times photo ----

def news_photo(img, size=256, margin=0.07):
    """The logo as a photo: a square of the wall around it, the logo's width `margin` from its sides
    (where the square is taller than the picture, the wall above and below the logo continues)."""
    h, w = len(img), len(img[0])
    lum = [[luminance(p) for p in row] for row in img]
    ys = [y for y in range(h) if any(v > 100 for v in lum[y])]
    xs = [x for x in range(w) if any(lum[y][x] > 100 for y in ys)]
    cx, cy = (xs[0] + xs[-1] + 1) / 2.0, (ys[0] + ys[-1] + 1) / 2.0
    side = int(round(max(xs[-1] + 1 - xs[0], ys[-1] + 1 - ys[0]) / (1 - 2 * margin)))
    x0, y0 = int(round(cx - side / 2.0)), int(round(cy - side / 2.0))
    top, bottom = max(1, ys[0] - 24), max(1, h - (ys[-1] + 1) - 24)  # plain wall, clear of shadows
    return shrink(crop_padded(img, x0, y0, x0 + side, y0 + side, top, bottom), size, size)


# ---- textures ----

def halve(px):
    """Half size, colour weighted by alpha."""
    out = []
    for y in range(0, len(px) - 1, 2):
        a, b = px[y], px[y + 1]
        row = []
        for x in range(0, len(a) - 1, 2):
            quad = (a[x], a[x + 1], b[x], b[x + 1])
            alpha = sum(p[3] for p in quad)
            if alpha:
                row.append(tuple(int(sum(p[k] * p[3] for p in quad) / float(alpha) + 0.5) for k in range(3)) +
                           (int(alpha / 4.0 + 0.5),))
            else:
                row.append((0, 0, 0, 0))
        out.append(row)
    return out


def encode(px, template, mips):
    """A CMPR texture of px with `mips` levels; the rest of its 32-byte header is the template's."""
    data, level = bytearray(), px
    for _ in range(mips):
        h, w = len(level), len(level[0])
        for ty in range(0, h, 8):
            for tx in range(0, w, 8):
                for sy in (0, 4):
                    for sx in (0, 4):
                        data += gxtex._encode_block([level[ty + sy + i // 4][tx + sx + i % 4] for i in range(16)])
        level = halve(level)
    header = bytearray(template[:32])
    struct.pack_into(">II", header, 0, mips, 2)
    struct.pack_into(">HH", header, 14, len(px[0]), len(px))
    return bytes(header) + bytes(data)


class TextureBundle:
    """A .rlt (PTLG): textures by name hash; replaced in place, same size, so its layout stays."""

    def __init__(self, path):
        self.data = bytearray(open(path, "rb").read())
        if self.data[:4] != b"PTLG":
            raise ValueError("%s: not a texture bundle" % path)
        count = struct.unpack_from(">I", self.data, 4)[0]
        base = 0x10 + 16 * count
        self.entries = {}
        for i in range(count):
            name, offset, size = struct.unpack_from(">III", self.data, 0x10 + 16 * i)
            self.entries[name] = (base + offset, size)

    def blob(self, name):
        offset, size = self.entries[name]
        return bytes(self.data[offset:offset + size])

    def rename(self, names):
        """Entries renamed ({old hash: new hash}), the table kept sorted by hash."""
        count = struct.unpack_from(">I", self.data, 4)[0]
        rows = []
        for i in range(count):
            name, offset, size, extra = struct.unpack_from(">IIII", self.data, 0x10 + 16 * i)
            rows.append((names.get(name, name), offset, size, extra))
        rows.sort()
        base = 0x10 + 16 * count
        self.entries = {}
        for i, row in enumerate(rows):
            struct.pack_into(">IIII", self.data, 0x10 + 16 * i, *row)
            self.entries[row[0]] = (base + row[1], row[2])

    def replace(self, name, blob):
        offset, size = self.entries[name]
        if len(blob) != size:
            raise ValueError("texture %08x: %d bytes, the bundle's is %d" % (name, len(blob), size))
        self.data[offset:offset + size] = blob


# Waluigi's ExtraTextures, by name hash.
CONE, GLOVE, BANNERS, BACKDROP, LOGO, GLOVE_OPEN = 0x141D28F1, 0x4D881A94, 0x5FF7AB5A, 0xAD2596F1, 0xD04FAF02, 0xFE8B6D45

# His banners' atlas (256x128): a flag, a striped banner, a pennant (black at its staff) and a long
# pennant, beside a wooden pole, on a grey that isn't drawn. Super Team's are his in its blue, with its
# logo where his emblem was, in the box his was in: on its side (its top to the left) and squashed as
# his, so the banner's model stretches it back. Under the logo, the banner is made plain.
FLAG, STRIPES, PENNANT, LONG_PENNANT = (12, 10, 64, 92), (86, 3, 172, 49), (94, 53, 146, 90), (152, 86, 253, 125)
# The areas made plain: his emblem's boxes and the strokes that reach past them.
FLAG_PLAIN, STRIPES_PLAIN, PENNANT_PLAIN = (7, 7, 70, 92), (86, 2, 176, 50), (81, 50, 152, 97)
STRIPES_CLEAR = (176, 251)  # the striped banner's columns clear of his emblem
PENNANT_STAFF, PENNANT_TIP = 80, (148, 62, 172, 78)  # its staff's column; a patch of it clear of his emblem
WALUIGI_EMBLEM_ASPECT = 1.45  # his emblem's own width over height, which each box's squash is measured against


def _unused(p):
    return all(abs(c - 128) < 8 for c in p[:3])


def _purple(p):
    h, s, v = colorsys.rgb_to_hsv(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0)
    return s > 0.25 and 225 <= h * 360 <= 300


def _mix(a, b, t):
    return tuple(int(a[k] * (1 - t) + b[k] * t + 0.5) for k in range(3)) + (255,)


def _average(pixels):
    n = float(len(pixels))
    return tuple(int(sum(p[k] for p in pixels) / n + 0.5) for k in range(3)) + (255,)


def plain_flag(px, box):
    """The cloth, row by row, from the box's left edge to its right."""
    x0, y0, x1, y1 = box
    for y in range(y0, y1):
        left, right = _average(px[y][x0 - 3:x0]), _average(px[y][x1:x1 + 3])
        for x in range(x0, x1):
            px[y][x] = _mix(left, right, (x - x0 + 0.5) / (x1 - x0))


def plain_stripes(px, box, clear):
    """The stripes, row by row, as they run where his emblem isn't."""
    x0, y0, x1, y1 = box
    for y in range(y0, y1):
        for x in range(x0, x1):
            px[y][x] = px[y][clear[0] + (x - x0) % (clear[1] - clear[0])]


def plain_pennant(px, box, staff, tip):
    """The pennant's colour, all but the black at its staff (each row's dark run from the staff)."""
    body = sorted((px[y][x] for y in range(tip[1], tip[3]) for x in range(tip[0], tip[2]) if _purple(px[y][x])),
                  key=luminance)
    body = body[len(body) // 2]
    x0, y0, x1, y1 = box
    for y in range(y0, y1):
        black = staff
        while black < x1 and luminance(px[y][black]) < 60:
            black += 1
        for x in range(max(x0, black), x1):
            if not _unused(px[y][x]):
                px[y][x] = body


def plain_long_pennant(px, box):
    """The pattern's ground, row by row: from the colour at the box's left edge to the row's darkest,
    over the box's first quarter. Returns the pattern's own (brightest) colour."""
    x0, y0, x1, y1 = box
    marks = sorted((px[y][x] for y in range(y0, y1) for x in range(x0, x1) if _purple(px[y][x])), key=luminance)
    stroke = marks[len(marks) * 17 // 20] if marks else TEAM_BLUE + (255,)
    ramp = (x1 - x0) // 4
    for y in range(y0, y1):
        row = [p for p in px[y][x0:x1] if not _unused(p)]
        if not row:
            continue
        ground = sorted(row, key=luminance)[len(row) // 4]
        left = px[y][x0 - 1] if not _unused(px[y][x0 - 1]) else ground
        for x in range(x0, x1):
            if not _unused(px[y][x]):
                px[y][x] = _mix(left, ground, min(1.0, (x - x0 + 0.5) / ramp))
    return stroke


def logo_mark(art, box, border):
    """The logo for a box: squashed as his emblem was in it, in an outline `border` wide (0: none)."""
    x0, y0, x1, y1 = box
    bw, bh = x1 - x0, y1 - y0
    stretch = WALUIGI_EMBLEM_ASPECT / (bh / float(bw))  # how much the banner's model stretches its height
    w = bw - 2 * border
    h = int(round(w / stretch))  # the logo is square
    if h > bh - 2 * border:
        h = bh - 2 * border
        w = int(round(h * stretch))
    canvas = blank(w + 2 * border, h + 2 * border)
    paste(canvas, shrink(art, w, h), border, border)
    return outlined(canvas, border, (0x0B, 0x16, 0x26), TEAM_BLUE) if border else canvas


def stamp(px, mark, box, colour=None):
    """mark's solid pixels onto the banner, centred in box (in `colour` if given)."""
    x0, y0, x1, y1 = box
    ox, oy = x0 + (x1 - x0 - len(mark[0])) // 2, y0 + (y1 - y0 - len(mark)) // 2
    for y, row in enumerate(mark):
        for x, p in enumerate(row):
            if p[3] >= 128 and not _unused(px[oy + y][ox + x]):
                px[oy + y][ox + x] = (colour or p)[:3] + (255,)


def banners(px, logo_img):
    px = [list(row) for row in px]
    plain_flag(px, FLAG_PLAIN)
    plain_stripes(px, STRIPES_PLAIN, STRIPES_CLEAR)
    plain_pennant(px, PENNANT_PLAIN, PENNANT_STAFF, PENNANT_TIP)
    stroke = to_blue(plain_long_pennant(px, LONG_PENNANT))
    px = recolour(px)
    art = rotate_ccw(stacked(logo_img, 256))
    for box in (FLAG, STRIPES, PENNANT):
        stamp(px, logo_mark(art, box, 2), box)
    stamp(px, logo_mark(art, LONG_PENNANT, 0), LONG_PENNANT, stroke)  # in the pattern's colour, as his
    return px


def recolour(px):
    return [[to_blue(p) for p in row] for row in px]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--logo", required=True)
    parser.add_argument("--extra", required=True)
    parser.add_argument("--out-extra", required=True)
    parser.add_argument("--out-news", required=True)
    parser.add_argument("--out-icon", required=True)
    parser.add_argument("--out-board", required=True)
    parser.add_argument("--preview", help="a folder for PNGs of what was made")
    args = parser.parse_args()

    logo = read_png(args.logo)
    extra = TextureBundle(args.extra)
    made = {}
    photo = news_photo(logo)
    made["news"] = photo
    with open(args.out_news, "wb") as f:
        f.write(encode(photo, extra.blob(CONE), 6))  # the photos are 256x256 with 6 levels, like the cone
    icon = captain_icon(logo)
    made["icon"] = icon
    with open(args.out_icon, "wb") as f:
        f.write(encode(icon, extra.blob(CONE), 5))  # the captain pictures are 128x128 with 5 levels
    board = emblem(logo, 128)
    made["board"] = board
    with open(args.out_board, "wb") as f:
        f.write(encode(board, extra.blob(LOGO), 1))  # partner select's faded emblem: 128x128, one level, alpha
    for name, make in ((LOGO, lambda px: emblem(logo, len(px))), (BANNERS, lambda px: banners(px, logo)),
                       (CONE, recolour), (BACKDROP, recolour), (GLOVE, recolour), (GLOVE_OPEN, recolour)):
        blob = extra.blob(name)
        px = make(gxtex.decode(blob))
        made["%08x" % name] = px
        extra.replace(name, encode(px, blob, gxtex.header(blob)[0]))
    # Under Super Team's own name, as the game asks for its textures by its name ("<name>/<texture>").
    extra.rename({h: gxtex.lower_hash(new) for h, new in (
        (CONE, "superteam/mega_cone_colour"), (GLOVE, "superteam/mega_hand"), (BANNERS, "superteam/superteam_banners"),
        (BACKDROP, "superteam/mega_gameplay_bg"), (LOGO, "superteam/superteam_logo"), (GLOVE_OPEN, "superteam/mega_hand1"))})
    with open(args.out_extra, "wb") as f:
        f.write(extra.data)
    if args.preview:
        os.makedirs(args.preview, exist_ok=True)
        for name, px in made.items():
            gxtex.png(os.path.join(args.preview, name + ".png"), px)
    print("%s, %s, %s" % (args.out_news, args.out_icon, args.out_extra))


if __name__ == "__main__":
    main()
