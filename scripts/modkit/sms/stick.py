#!/usr/bin/env python3
"""stick.py: stick-figure contact sheets of Charged-skeleton (Waluigi, 44 nodes) animations.

    stick.py out.png  label=path.nis[@anim][:k0-k1/step] ...   (Charged NIS/sanim)
                      label=sms:path.sanim@name[:k0-k1/step]   (SMS, remapped to Charged)

Each row: one animation; each cell: one key, front view (x right, z up) and side view (y right, z up)
side by side, framed on the body's bip01. Bone offsets: Waluigi's NIS constant translations.
"""
import math, os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sanim  # noqa: E402

# The extracted game's files (bone offsets come from Waluigi's Mega Strike cutscene): $STRIKERS_GAME, or
# the macOS data folder's copy.
GAME = os.environ.get('STRIKERS_GAME') or os.path.expanduser('~/Library/Application Support/MSCRecomp/Game/files')
PARENTS = [-1, 0, 0, 2, 2, 4, 5, 6, 7, 4, 9, 10, 11, 2, 13, 14, 15, 16, 17, 18, 19, 20, 18, 22, 23, 18, 25, 26, 14,
           28, 29, 14, 31, 32, 33, 34, 35, 36, 34, 38, 39, 34, 41, 42]


def qmul(a, b):
    return sanim.qmul(a, b)


def qrot(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    tx = 2 * (y * vz - z * vy)
    ty = 2 * (z * vx - x * vz)
    tz = 2 * (x * vy - y * vx)
    return (vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx))


_bind = None


def bind_offsets():
    global _bind
    if _bind is None:
        _, anims = sanim.load(os.path.join(GAME, 'Art/nis/waluigi_megastrike_home_0.nis'), sanim.CHARGED)
        w = anims[0]
        _bind = [w.nodes[i].trans[0] if w.nodes[i].trans and i > 3 else (0.0, 0.0, 0.0) for i in range(44)]
    return _bind


def pose(a, k):
    """World joint positions (44) at key k."""
    bind = bind_offsets()
    k = min(k, a.num_keys - 1)
    rr = a.root_rot[min(k, len(a.root_rot) - 1)] if a.root_rot else 0
    ang = rr * 2 * math.pi / 65536
    root_q = (0.0, 0.0, math.sin(ang / 2), math.cos(ang / 2))
    root_t = a.root_trans[min(k, len(a.root_trans) - 1)] if a.root_trans else (0.0, 0.0, 0.0)
    wq = [None] * 44
    wp = [None] * 44
    for i in range(44):
        n = a.nodes[i] if i < len(a.nodes) else None
        q = a.node_rot(i, k) if n is not None else None
        q = q or (0.0, 0.0, 0.0, 1.0)
        if n is not None and n.trans:
            t = n.trans[min(k, len(n.trans) - 1)]
        else:
            t = bind[i]
        p = PARENTS[i]
        if p < 0:
            wq[i] = root_q
            wp[i] = root_t
        else:
            wq[i] = qmul(wq[p], q)
            o = qrot(wq[p], t)
            wp[i] = (wp[p][0] + o[0], wp[p][1] + o[1], wp[p][2] + o[2])
    return wp


def load_spec(spec):
    label, _, src = spec.partition('=')
    rng = None
    if ':' in src.split('@')[-1] and not src.startswith('sms:') or src.count(':') > 1:
        src, _, rng = src.rpartition(':')
    sms = src.startswith('sms:')
    if sms:
        src = src[4:]
    path, _, name = src.partition('@')
    fmt, anims = sanim.load(path, sanim.SMS if sms else None)
    a = next(x for x in anims if x.name == name) if name else anims[0]
    if sms:
        a = sanim.remap_to_charged(a)
    if rng:
        r, _, step = rng.partition('/')
        k0, _, k1 = r.partition('-')
        keys = list(range(int(k0), int(k1) + 1, int(step or 1)))
    else:
        step = max(1, a.num_keys // 8)
        keys = list(range(0, a.num_keys, step))
    return label, a, keys


# ---- raster
W_CELL, H_CELL = 120, 140


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray(b'\xff' * (w * h * 3))

    def dot(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            o = (y * self.w + x) * 3
            self.px[o:o + 3] = bytes(c)

    def line(self, x0, y0, x1, y1, c):
        n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        for i in range(n + 1):
            t = i / n
            self.dot(int(round(x0 + (x1 - x0) * t)), int(round(y0 + (y1 - y0) * t)), c)

    def png(self, path):
        raw = b''.join(b'\0' + bytes(self.px[y * self.w * 3:(y + 1) * self.w * 3]) for y in range(self.h))

        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
        with open(path, 'wb') as f:
            f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', self.w, self.h, 8, 2, 0, 0, 0))
                    + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


FONT = {}  # digits only, 3x5
for ch, rows in {'0': '111101101101111', '1': '010110010010111', '2': '111001111100111', '3': '111001111001111',
                 '4': '101101111001001', '5': '111100111001111', '6': '111100111101111', '7': '111001001001001',
                 '8': '111101111101111', '9': '111101111001111'}.items():
    FONT[ch] = rows


def text(cv, x, y, s, c=(0, 0, 0)):
    for ch in s:
        rows = FONT.get(ch)
        if rows:
            for i, b in enumerate(rows):
                if b == '1':
                    for dx in range(2):
                        for dy in range(2):
                            cv.dot(x + (i % 3) * 2 + dx, y + (i // 3) * 2 + dy, c)
        x += 8


LEFT = {5, 6, 7, 8, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27}
RIGHT = {9, 10, 11, 12, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43}


def draw(cv, ox, oy, joints, scale=55.0):
    c = joints[2]
    for view, x0 in ((0, ox), (1, ox + W_CELL // 2)):
        for i in range(3, 44):
            p = PARENTS[i]
            if p < 2 or i == 3:  # the root, the ball; footsteps (a biped helper)
                continue
            col = (200, 30, 30) if i in LEFT else (30, 30, 200) if i in RIGHT else (0, 0, 0)
            a, b = joints[p], joints[i]
            ax = (a[view] - c[view]) * scale + x0 + W_CELL // 4
            bx = (b[view] - c[view]) * scale + x0 + W_CELL // 4
            ay = oy + H_CELL * 0.55 - (a[2] - c[2]) * scale
            by = oy + H_CELL * 0.55 - (b[2] - c[2]) * scale
            cv.line(ax, ay, bx, by, col)
        # the ball
        bpos = joints[1]
        bx = (bpos[view] - c[view]) * scale + x0 + W_CELL // 4
        by = oy + H_CELL * 0.55 - (bpos[2] - c[2]) * scale
        for dx in range(-2, 3):
            for dy in range(-2, 3):
                cv.dot(int(bx) + dx, int(by) + dy, (0, 160, 0))
        # head marker
        h = joints[29]
        hx = (h[view] - c[view]) * scale + x0 + W_CELL // 4
        hy = oy + H_CELL * 0.55 - (h[2] - c[2]) * scale
        for dx in range(-1, 2):
            for dy in range(-1, 2):
                cv.dot(int(hx) + dx, int(hy) + dy, (255, 140, 0))


def main(argv):
    out = argv[1]
    rows = [load_spec(s) for s in argv[2:]]
    cols = max(len(k) for _, _, k in rows)
    cv = Canvas(cols * W_CELL, len(rows) * H_CELL)
    for r, (label, a, keys) in enumerate(rows):
        for ci, k in enumerate(keys):
            ox, oy = ci * W_CELL, r * H_CELL
            cv.line(ox, oy, ox, oy + H_CELL - 1, (220, 220, 220))
            cv.line(ox + W_CELL // 2, oy + 12, ox + W_CELL // 2, oy + H_CELL - 1, (235, 235, 235))
            draw(cv, ox, oy, pose(a, k))
            text(cv, ox + 3, oy + 3, str(k))
        cv.line(0, r * H_CELL, cv.w - 1, r * H_CELL, (160, 160, 160))
        print('row %d: %s (%s, %d keys)' % (r, label, a.name, a.num_keys))
    cv.png(out)
    print(out)


if __name__ == '__main__':
    main(sys.argv)
