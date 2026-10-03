# gxtex.py: GX textures as the NL engines store them (32-byte header + data): decode C8 (RGB5A3
# palette) and CMPR, encode CMPR, plus the few image operations the converters need (RGBA lists of
# rows), and PNG output for checking.
import struct, zlib

def rgb5a3(c):
    if c & 0x8000:
        return ((c >> 10) & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, (c & 31) * 255 // 31, 255
    return ((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17, ((c >> 12) & 7) * 255 // 7

def header(blob):
    mips, fmt = struct.unpack('>II', blob[0:8])
    w, h = struct.unpack('>HH', blob[14:18])
    return mips, fmt, w, h

def decode(blob):
    mips, fmt, w, h = header(blob)
    data = blob[32:]
    px = [[(0, 0, 0, 0)] * w for _ in range(h)]
    if fmt == 8:  # C8, palette (RGB5A3, 256) after the image data
        pal = [rgb5a3(c) for c in struct.unpack('>256H', blob[-512:])]
        o = 0
        for ty in range(0, h, 4):
            for tx in range(0, w, 8):
                for y in range(4):
                    for x in range(8):
                        px[ty + y][tx + x] = pal[data[o]]; o += 1
    elif fmt == 2:  # CMPR
        o = 0
        for ty in range(0, h, 8):
            for tx in range(0, w, 8):
                for sy in (0, 4):
                    for sx in (0, 4):
                        c0, c1, bits = struct.unpack('>HHI', data[o:o + 8]); o += 8
                        pal = _cmpr_palette(c0, c1)
                        for i in range(16):
                            y, x = ty + sy + i // 4, tx + sx + i % 4
                            if y < h and x < w:
                                px[y][x] = pal[(bits >> (30 - 2 * i)) & 3]
    else:
        raise ValueError('format %d' % fmt)
    return px

def _rgb565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31

def _to565(r, g, b):
    return (min(31, (r * 31 + 127) // 255) << 11) | (min(63, (g * 63 + 127) // 255) << 5) | min(31, (b * 31 + 127) // 255)

def _cmpr_palette(c0, c1):
    a, b = _rgb565(c0), _rgb565(c1)
    if c0 > c1:
        return [a + (255,), b + (255,), tuple((2 * x + y) // 3 for x, y in zip(a, b)) + (255,),
                tuple((x + 2 * y) // 3 for x, y in zip(a, b)) + (255,)]
    return [a + (255,), b + (255,), tuple((x + y) // 2 for x, y in zip(a, b)) + (255,), (0, 0, 0, 0)]

def _encode_block(block):
    """16 RGBA pixels -> 8 bytes of CMPR (opaque blocks: 4-colour mode; any transparent pixel: 3+alpha)."""
    opaque = [p for p in block if p[3] >= 128]
    if not opaque:
        return struct.pack('>HHI', 0, 0, 0xFFFFFFFF)
    # Endpoints: the extremes along the axis of largest spread.
    axis = max(range(3), key=lambda k: max(p[k] for p in opaque) - min(p[k] for p in opaque))
    lo = min(opaque, key=lambda p: (p[axis], sum(p[:3])))
    hi = max(opaque, key=lambda p: (p[axis], sum(p[:3])))
    c_hi, c_lo = _to565(*hi[:3]), _to565(*lo[:3])
    transparent = len(opaque) < 16
    if transparent:
        c0, c1 = (c_lo, c_hi) if c_lo <= c_hi else (c_hi, c_lo)  # c0 <= c1: 3 colours + transparent
    else:
        if c_hi == c_lo:
            c_lo = c_lo - 1 if c_lo > 0 else c_lo
            if c_hi == c_lo: c_hi += 1
        c0, c1 = (c_hi, c_lo) if c_hi > c_lo else (c_lo, c_hi)  # c0 > c1: 4 colours
    pal = _cmpr_palette(c0, c1)
    bits = 0
    for i, p in enumerate(block):
        if p[3] < 128 and transparent:
            idx = 3
        else:
            choices = range(3) if transparent else range(4)
            idx = min(choices, key=lambda k: sum((p[j] - pal[k][j]) ** 2 for j in range(3)))
        bits |= idx << (30 - 2 * i)
    return struct.pack('>HHI', c0, c1, bits)

def _half(px):
    h, w = len(px) // 2, len(px[0]) // 2
    return [[tuple(sum(px[2 * y + dy][2 * x + dx][k] for dy in (0, 1) for dx in (0, 1)) // 4 for k in range(4))
             for x in range(w)] for y in range(h)]

def encode_cmpr(px, template_header, mips=1):
    """A CMPR texture of `px` (w, h multiples of 8) with `mips` levels (each halved, down to 8x8),
    header taken from a game texture of the same kind (mip count, format and size are set)."""
    h, w = len(px), len(px[0])
    out = bytearray()
    level, levels = px, 0
    while levels < mips and len(level) >= 8 and len(level[0]) >= 8:
        lh, lw = len(level), len(level[0])
        for ty in range(0, lh, 8):
            for tx in range(0, lw, 8):
                for sy in (0, 4):
                    for sx in (0, 4):
                        out += _encode_block([level[ty + sy + i // 4][tx + sx + i % 4] for i in range(16)])
        levels += 1
        level = _half(level)
    hdr = bytearray(template_header[:32])
    struct.pack_into('>II', hdr, 0, levels, 2)
    struct.pack_into('>HH', hdr, 14, w, h)
    return bytes(hdr) + bytes(out)

def resize(px, w, h):
    """Bilinear."""
    sh, sw = len(px), len(px[0])
    out = []
    for y in range(h):
        fy = (y + 0.5) * sh / h - 0.5
        y0 = max(0, min(sh - 1, int(fy))); y1 = min(sh - 1, y0 + 1); wy = min(1.0, max(0.0, fy - y0))
        row = []
        for x in range(w):
            fx = (x + 0.5) * sw / w - 0.5
            x0 = max(0, min(sw - 1, int(fx))); x1 = min(sw - 1, x0 + 1); wx = min(1.0, max(0.0, fx - x0))
            row.append(tuple(int(round(
                (px[y0][x0][k] * (1 - wx) + px[y0][x1][k] * wx) * (1 - wy) +
                (px[y1][x0][k] * (1 - wx) + px[y1][x1][k] * wx) * wy)) for k in range(4)))
        out.append(row)
    return out

def over(bottom, top, ox=0, oy=0):
    """`top` alpha-composited onto `bottom` at (ox, oy); both RGBA."""
    out = [list(r) for r in bottom]
    for y, row in enumerate(top):
        for x, (r, g, b, a) in enumerate(row):
            Y, X = y + oy, x + ox
            if 0 <= Y < len(out) and 0 <= X < len(out[0]) and a:
                br, bg, bb, ba = out[Y][X]
                t = a / 255.0
                out[Y][X] = (int(r * t + br * (1 - t)), int(g * t + bg * (1 - t)), int(b * t + bb * (1 - t)), max(ba, a))
    return out

def grey(px, scale=0.9):
    return [[(int(l), int(l), int(l), a) for (r, g, b, a) in row
             for l in [min(255, (0.299 * r + 0.587 * g + 0.114 * b) * scale)]] for row in px]

def png(path, px):
    h, w = len(px), len(px[0])
    raw = b''.join(b'\0' + bytes(c for p in row for c in p) for row in px)
    def ck(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ck(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           ck(b'IDAT', zlib.compress(raw)) + ck(b'IEND', b''))

def fe_bundle(path):
    """{hash: blob} of a front-end bundle (.Res/.Dmn)."""
    d = open(path, 'rb').read()
    n = struct.unpack('>I', d[4:8])[0]
    out = {}
    for i in range(n):
        h, o, s = struct.unpack('>III', d[0x20 + 12 * i:0x2C + 12 * i])
        out[h] = d[o * 32:o * 32 + s]
    return out

def lower_hash(s, h=0xFFFFFFFF):
    for c in s.lower().encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h
