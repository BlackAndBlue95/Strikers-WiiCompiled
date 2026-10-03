#!/usr/bin/env python3
"""sanim.py: Super Mario Strikers (GC) / Mario Strikers Charged (Wii) skeletal animations.

    python3 sanim.py list <file.sanim | file.sanim.zlib | file.nis> [--sms|--charged] [-v]
    python3 sanim.py compare <sms.sanim> <charged.sanim.zlib> <name>      # convert + diff one anim
    python3 sanim.py verify <sms.sanim> <charged.sanim.zlib> [--sms-frames]  # every shared name
    python3 sanim.py build <charged base.sanim.zlib> <sms donor.sanim> <out.sanim.zlib> name|sms=charged ...
        (build: Charged bundle order and every other chunk kept byte-for-byte; the named SMS
         animations converted with 12-bit rotations, Charged's Waluigi signature 0x7D05EA3B)

Verified (see verify): Charged's own gameplay animations re-encode byte-identically from their
decoded keys (97/97 waluigi, mario, birdo; all 837 NIS animation chunks except the garbage
pointer words of the header). SMS -> Charged on the 9 animations Charged carried over
unchanged: 98.8% of non-leg rotation keys bit-identical to Charged's file (max 0.097 deg);
toes/thumbs/finger bases need CHARGED_BIND_DELTA; thighs/feet (5,7,9,11) were re-solved by
Charged (median 1.3 deg, up to ~5 deg) and root/pelvis split re-extracted (world pelvis within
0.02-0.12 m). Quantisation truncates toward zero, as Charged's exporter does.

Both games store one animation per 0x80017000 chunk (the same chunk is what a .nis file
holds per actor). Format (all big-endian):

  SMS  (cSAnim, SAnim.cpp, header 0x48)             Charged (cSAnim, SAnim.cpp, header 0x58)
  ---------------------------------------------    ----------------------------------------------
  17001 header                                      17001 header
  17002 name string                                 17002 name string
                                                    17110 u32[numNodes]  (Unknown18: weight-key counts, 0)
                                                    17113 u32[numNodes]  (Unknown1C, 0)
  17004 ptr[numNodes] rot   (filled at load)        17004 ptr[numNodes] rot
  17005 ptr[numNodes] trans                         17005 ptr[numNodes] trans
  17006 ptr[numNodes] scale                         17006 ptr[numNodes] scale
                                                    17111 ptr[numNodes]  (Unknown2C: 17112 weight tracks)
                                                    17114 ptr[numNodes]  (Unknown30: 17115 tracks)
  17007 u16[numRootKeys] root Z angle (65536=360)    17007 same
  02017008 f32[numRootKeys][3] root translation     02017008 same
  80017100 x numNodes: 17101 rot, 17102 trans,      80017100 x numNodes: + optional 17112 (u8/255),
           17103 scale (each optional)                       17115
  17009 u32[numMorph] morph key counts              17009 same
  1700a u32[numMorph] morph target hashes           1700a same
  1700b u8[] morph weights (/255)                   1700b same
  17003 u32[numNodes] node properties               17003 same

  header: +0 name ptr, +4 nlStringHash(name)        +0 name, +4 hash, +8 numKeys, +C numNodes,
  (case-sensitive; 0 in NIS files), +8 numKeys,     +10 numMorph, +14..+30 pointers (garbage in
  +C numNodes, +10 numMorph, +14..+20 ptrs,         file), +34 numRootKeys, +38..+4C ptrs,
  +24 numRootKeys, +28..+3C ptrs, +40 linear        +50 linear speed (recomputed at load),
  speed (recomputed), +44 hierarchy signature       +54 hierarchy signature

  node properties: 1 = rotation is a u16 Z angle; 2 = rotation constant (1 key); 4 = translation
  constant; 8 = scale constant. Charged adds rotation encodings 0x10 = s16 x4 /32768 (all NIS
  files), 0x20 = 12-bit x4 packed in 6 bytes (all gameplay .sanim), neither = s8 x4 /128.
  SMS quaternions are s16 x4 /16384. Translation f32 x3 in both. Scale s16 x3: /4096 SMS,
  /2048 Charged. 30 keys per second, every frame stored, keys sampled at t*(numKeys-1).

  chunk layout: SMS packs chunks back to back (some leaves carry their own pad in `size`);
  Charged rounds every next chunk up to 4 bytes (see nlchunks.py). Charged .sanim.zlib =
  u32 uncompressed size + zlib stream (AnimInventory loads the whole bundle; Find() is by
  nlStringHash(name), case-sensitive, first match in list order = LAST chunk in the file
  wins because AddStart prepends).

Bone remap SMS Waluigi/SuperTeam skeleton (38 bones) -> Charged waluigi.shier (44 bones),
by bone-name hash: 0-24 same; 25,26 -> 28,29; 27 (9b953858) -> 30 (4eee263c, same place in
the hierarchy, renamed); 28-37 -> 31-40. Charged-only bones 25-27 (third finger, left) and
41-43 (third finger, right) copy the tracks of 22-24 and 38-40 (what Charged's own Waluigi
animations do for the frames converted here: see the verify report).
"""
import math
import struct
import sys
import zlib

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import nlchunks
from nlchunks import Chunk

SMS, CHARGED = 'sms', 'charged'

# SMS node -> Charged node (Waluigi skeleton)
SMS_TO_CHARGED = list(range(25)) + [28, 29, 30] + list(range(31, 41))
# Charged nodes with no SMS bone: copy from (Charged index)
CHARGED_COPIES = {25: 22, 26: 23, 27: 24, 41: 38, 42: 39, 43: 40}
CHARGED_WALUIGI_SIGNATURE = 0x7D05EA3B   # every animation in Charged waluigi.sanim + Waluigi NIS
# Charged's re-rigged Waluigi: constant rotation offsets between SMS and Charged local keys,
# q_charged = DELTA * q_sms (left-multiplied), measured over 21 animation pairs that Charged
# carried over unchanged (~700 keys each; per-key scatter median 0.03 deg, i.e. quantisation).
# Toes 8/12: -21.06 deg about Z; thumbs 19/35: 26.97 deg; finger bases 22/38: 0.59 deg.
CHARGED_BIND_DELTA = {
    8:  (0.000115, 0.000040, -0.182735, 0.983162),
    12: (-0.000094, -0.000101, -0.182727, 0.983164),
    19: (-0.023865, 0.225696, 0.053567, 0.972431),
    35: (0.023845, -0.225688, 0.053554, 0.972434),
    22: (0.000204, -0.001068, 0.005054, 0.999987),
    38: (-0.000187, 0.001027, 0.005002, 0.999987),
}
CHARGED_NUM_NODES = 44

P_ROTZ, P_ROTCONST, P_TRANSCONST, P_SCALECONST, P_ROT16, P_ROT12 = 1, 2, 4, 8, 0x10, 0x20


def nl_string_hash(s):
    """nlStringHash: case-sensitive, h = h*33 + c, seed 0xFFFFFFFF (anim name hash)."""
    h = 0xFFFFFFFF
    for c in s.encode('latin1'):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def nl_string_lower_hash(s):
    return nl_string_hash(s.lower())


def _s16(v):
    return v - 0x10000 if v & 0x8000 else v


def _clamp_round(v, lo, hi):
    r = int(math.floor(v + 0.5))
    return lo if r < lo else hi if r > hi else r


class Node:
    __slots__ = ('rot', 'rotz', 'trans', 'scale', 'props', 'extra')

    def __init__(self):
        self.rot = None      # list of (x, y, z, w) floats
        self.rotz = None     # list of u16 Z angles (props & 1)
        self.trans = None    # list of (x, y, z)
        self.scale = None    # list of (x, y, z)
        self.props = 0
        self.extra = []      # other child chunks kept verbatim (Charged 17112/17115)

    def copy(self):
        n = Node()
        n.rot = list(self.rot) if self.rot is not None else None
        n.rotz = list(self.rotz) if self.rotz is not None else None
        n.trans = list(self.trans) if self.trans is not None else None
        n.scale = list(self.scale) if self.scale is not None else None
        n.props = self.props
        n.extra = list(self.extra)
        return n

    def empty(self):
        return self.rot is None and self.rotz is None and self.trans is None and self.scale is None


class Anim:
    def __init__(self):
        self.fmt = None
        self.name = ''
        self.hash = 0
        self.num_keys = 0
        self.num_root_keys = 0
        self.signature = 0
        self.linear_speed = 0.0
        self.root_rot = []
        self.root_trans = []
        self.nodes = []
        self.morph_num_keys = []
        self.morph_ids = []
        self.morph_keys = b''
        self.tables = {}        # Charged 17110 / 17113 payloads
        self.root_trans_extra = b''
        self.header = b''

    @property
    def num_nodes(self):
        return len(self.nodes)

    @property
    def num_morph(self):
        return len(self.morph_ids)

    def __repr__(self):
        return '<Anim %s %r hash %08x keys %d nodes %d morph %d sig %08x>' % (
            self.fmt, self.name, self.hash, self.num_keys, self.num_nodes, self.num_morph, self.signature)

    # ---- sampling (decoded floats) ----
    def node_rot(self, i, k):
        """Quaternion (x,y,z,w) of node i at key k, or None when the node has no rotation."""
        n = self.nodes[i]
        if n.rot is not None:
            return n.rot[min(k, len(n.rot) - 1)]
        if n.rotz is not None:
            a = n.rotz[min(k, len(n.rotz) - 1)] * (2 * math.pi / 65536.0)
            return (0.0, 0.0, math.sin(a / 2), math.cos(a / 2))
        return None


# ---------------------------------------------------------------- parse

def _decode_rot(data, props, count, fmt):
    out = []
    if props & P_ROTZ:
        return None, list(struct.unpack_from('>%dH' % count, data))
    if fmt == SMS:
        v = struct.unpack_from('>%dh' % (4 * count), data)
        s = 1.0 / 16384
        out = [(v[4*i]*s, v[4*i+1]*s, v[4*i+2]*s, v[4*i+3]*s) for i in range(count)]
    elif props & P_ROT16:
        v = struct.unpack_from('>%dh' % (4 * count), data)
        s = 1.0 / 32768
        out = [(v[4*i]*s, v[4*i+1]*s, v[4*i+2]*s, v[4*i+3]*s) for i in range(count)]
    elif props & P_ROT12:
        s = 1.0 / 32768
        for i in range(count):
            b = data[6*i:6*i+6]
            x = _s16((b[0] << 8) | (b[1] & 0xF0))
            y = _s16((b[2] << 8) | ((b[1] << 4) & 0xF0))
            z = _s16((b[3] << 8) | (b[4] & 0xF0))
            w = _s16((b[5] << 8) | ((b[4] << 4) & 0xF0))
            out.append((x*s, y*s, z*s, w*s))
    else:
        v = struct.unpack_from('>%db' % (4 * count), data)
        s = 1.0 / 128
        out = [(v[4*i]*s, v[4*i+1]*s, v[4*i+2]*s, v[4*i+3]*s) for i in range(count)]
    return out, None


def anim_from_chunk(ch, fmt):
    a = Anim()
    a.fmt = fmt
    kids = ch.children
    hdr = kids[0].data
    a.header = hdr
    a.hash, a.num_keys, nn, nm = struct.unpack_from('>4I', hdr, 4)
    if fmt == SMS:
        a.num_root_keys = struct.unpack_from('>I', hdr, 0x24)[0]
        a.linear_speed = struct.unpack_from('>f', hdr, 0x40)[0]
        a.signature = struct.unpack_from('>I', hdr, 0x44)[0]
    else:
        a.num_root_keys = struct.unpack_from('>I', hdr, 0x34)[0]
        a.linear_speed = struct.unpack_from('>f', hdr, 0x50)[0]
        a.signature = struct.unpack_from('>I', hdr, 0x54)[0]
    a.name = kids[1].data.split(b'\0')[0].decode('latin1')
    for c in kids:
        if c.type in (0x17110, 0x17113):
            a.tables[c.type] = c.data
    rr = ch.find(0x17007)
    a.root_rot = list(struct.unpack_from('>%dH' % a.num_root_keys, rr.data)) if a.num_root_keys else []
    rt = ch.find(0x17008)
    v = struct.unpack_from('>%df' % (3 * a.num_root_keys), rt.data) if a.num_root_keys else ()
    a.root_trans = [tuple(v[3*i:3*i+3]) for i in range(a.num_root_keys)]
    a.root_trans_extra = rt.data[12 * a.num_root_keys:]   # Charged sometimes stores more keys
    props = struct.unpack_from('>%dI' % nn, ch.find(0x17003).data)
    nodes = ch.findall(0x80017100)
    assert len(nodes) == nn, (len(nodes), nn)
    for i, nc in enumerate(nodes):
        n = Node()
        p = n.props = props[i]
        for c in nc.children:
            if c.type == 0x17101:
                cnt = 1 if p & P_ROTCONST else a.num_keys
                n.rot, n.rotz = _decode_rot(c.data, p, cnt, fmt)
            elif c.type == 0x17102:
                cnt = 1 if p & P_TRANSCONST else a.num_keys
                v = struct.unpack_from('>%df' % (3 * cnt), c.data)
                n.trans = [tuple(v[3*k:3*k+3]) for k in range(cnt)]
            elif c.type == 0x17103:
                cnt = 1 if p & P_SCALECONST else a.num_keys
                v = struct.unpack_from('>%dh' % (3 * cnt), c.data)
                s = 1.0 / (4096 if fmt == SMS else 2048)
                n.scale = [(v[3*k]*s, v[3*k+1]*s, v[3*k+2]*s) for k in range(cnt)]
            else:
                n.extra.append(c)
        a.nodes.append(n)
    mk = ch.find(0x17009)
    if nm:
        a.morph_num_keys = list(struct.unpack_from('>%dI' % nm, mk.data))
        a.morph_ids = list(struct.unpack_from('>%dI' % nm, ch.find(0x1700A).data))
        a.morph_keys = ch.find(0x1700B).data[:sum(a.morph_num_keys)]
    return a


def load_bytes(path):
    d = open(path, 'rb').read()
    if path.endswith('.zlib'):
        n = struct.unpack('>I', d[:4])[0]
        d = zlib.decompress(d[4:])
        assert len(d) == n
    return d


def guess_fmt(path, d):
    """Charged headers are 0x58 bytes, SMS 0x48 (first chunk of a .sanim / .nis is an animation)."""
    if d[:4] == b'\x80\x01\x70\x00' and struct.unpack('>I', d[12:16])[0] in (0x58, 0x48):
        return CHARGED if struct.unpack('>I', d[12:16])[0] == 0x58 else SMS
    return CHARGED if path.endswith('.zlib') else SMS


def load(path, fmt=None):
    """Return (fmt, [Anim]) for a .sanim, .sanim.zlib or the animation chunks of a .nis."""
    d = load_bytes(path)
    fmt = fmt or guess_fmt(path, d)
    chunks = nlchunks.parse(d, charged=(fmt == CHARGED))
    return fmt, [anim_from_chunk(c, fmt) for c in chunks if c.type == 0x80017000]


# ---------------------------------------------------------------- encode (Charged)

def _trunc(v, lo, hi):
    r = int(v)          # toward zero, like both games' exporters (measured: 99.85% of Charged's
    return lo if r < lo else hi if r > hi else r   # 12-bit keys = trunc(sms*2048); SMS/Charged s16 likewise)


def _q16(v):
    return _trunc(v * 32768.0, -32768, 32767)


def _q12(v):
    # 12-bit signed fraction of 2048 (= the high 12 bits of an s16 /32768)
    return _trunc(v * 2048.0, -2048, 2047) & 0xFFF


def _enc_rot16(q):
    return struct.pack('>4h', *(_q16(c) for c in q))


def _enc_rot12(q):
    x, y, z, w = (_q12(c) for c in q)
    return bytes([x >> 4, ((x & 0xF) << 4) | (y & 0xF), y >> 4,
                  z >> 4, ((z & 0xF) << 4) | (w & 0xF), w >> 4])


def _enc_rot8(q):
    return struct.pack('>4b', *(_trunc(c * 128.0, -128, 127) for c in q))


def _canon(q, prev=None):
    """Normalise; keep the sign continuous with the previous key (SLERP-free lerp in the game)."""
    x, y, z, w = q
    l = math.sqrt(x*x + y*y + z*z + w*w) or 1.0
    q = (x/l, y/l, z/l, w/l)
    if prev is not None and sum(a*b for a, b in zip(q, prev)) < 0:
        q = tuple(-c for c in q)
    return q


def encode_charged_chunk(a, rot_format=16, detect_const=True, header_template=None):
    """Build the Charged 0x80017000 chunk for Anim `a` (decoded floats, any source format).

    rot_format: 16 (props 0x10, what Charged NIS files use) or 12 (0x20, gameplay .sanim).
    """
    nn = a.num_nodes
    nk = a.num_keys
    props = []
    node_chunks = []
    for n in a.nodes:
        p = 0
        kids = []
        if n.rotz is not None:
            keys = n.rotz
            if detect_const and len(set(keys)) == 1:
                keys = keys[:1]
            p |= P_ROTZ | P_ROT16
            if len(keys) == 1:
                p |= P_ROTCONST
            kids.append(Chunk(0x17101, struct.pack('>%dH' % len(keys), *keys)))
        elif n.rot is not None:
            enc = {16: _enc_rot16, 12: _enc_rot12, 8: _enc_rot8}[rot_format]
            p |= {16: P_ROT16, 12: P_ROT12, 8: 0}[rot_format]
            blobs = [enc(q) for q in n.rot]
            if detect_const and len(set(blobs)) == 1:
                blobs = blobs[:1]
            if len(blobs) == 1:
                p |= P_ROTCONST
            kids.append(Chunk(0x17101, b''.join(blobs)))
        if n.trans is not None:
            keys = n.trans
            packed = [struct.pack('>3f', *t) for t in keys]
            if detect_const and len(set(packed)) == 1:
                packed = packed[:1]
            if len(packed) == 1:
                p |= P_TRANSCONST
            kids.append(Chunk(0x17102, b''.join(packed)))
        else:
            p |= n.props & P_TRANSCONST     # absent track: keep the source exporter's bit
        if n.scale is not None:
            packed = [struct.pack('>3h', *(_clamp_round(c * 2048.0, -32768, 32767) for c in s)) for s in n.scale]
            if detect_const and len(set(packed)) == 1:
                packed = packed[:1]
            if len(packed) == 1:
                p |= P_SCALECONST
            kids.append(Chunk(0x17103, b''.join(packed)))
        else:
            p |= n.props & P_SCALECONST
        if n.empty():
            p = 0   # Charged's exporter writes 0 for a node with no tracks at all
        kids.extend(n.extra)
        props.append(p)
        node_chunks.append(Chunk(0x80017100, children=kids))

    hdr = bytearray(0x58)
    if header_template is not None and len(header_template) == 0x58:
        hdr[:] = header_template
    struct.pack_into('>5I', hdr, 0, 0, a.hash, nk, nn, a.num_morph)
    for off in range(0x14, 0x34, 4):
        struct.pack_into('>I', hdr, off, 0)
    struct.pack_into('>I', hdr, 0x34, a.num_root_keys)
    for off in range(0x38, 0x50, 4):
        struct.pack_into('>I', hdr, off, 0)
    struct.pack_into('>fI', hdr, 0x50, 0.0, a.signature)

    name = a.name.encode('latin1') + b'\0'
    name += b'\0' * ((-len(name)) % 4)
    zeros = b'\0' * (4 * nn)
    kids = [
        Chunk(0x17001, bytes(hdr)),
        Chunk(0x17002, name),
        Chunk(0x17110, a.tables.get(0x17110, zeros)),
        Chunk(0x17113, a.tables.get(0x17113, zeros)),
        Chunk(0x17004, zeros), Chunk(0x17005, zeros), Chunk(0x17006, zeros),
        Chunk(0x17111, zeros), Chunk(0x17114, zeros),
        Chunk(0x17007, struct.pack('>%dH' % len(a.root_rot), *a.root_rot)),
        Chunk(0x02017008, b''.join(struct.pack('>3f', *t) for t in a.root_trans) + a.root_trans_extra),
    ] + node_chunks + [
        Chunk(0x17009, struct.pack('>%dI' % a.num_morph, *a.morph_num_keys)),
        Chunk(0x1700A, struct.pack('>%dI' % a.num_morph, *a.morph_ids)),
        Chunk(0x1700B, bytes(a.morph_keys)),
        Chunk(0x17003, struct.pack('>%dI' % nn, *props)),
    ]
    return Chunk(0x80017000, children=kids)


# ---------------------------------------------------------------- SMS -> Charged

def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw*bx + ax*bw + ay*bz - az*by,
            aw*by - ax*bz + ay*bw + az*bx,
            aw*bz + ax*by - ay*bx + az*bw,
            aw*bw - ax*bx - ay*by - az*bz)


def remap_to_charged(a, remap=SMS_TO_CHARGED, copies=CHARGED_COPIES, num_nodes=CHARGED_NUM_NODES,
                     signature=CHARGED_WALUIGI_SIGNATURE, name=None, delta=CHARGED_BIND_DELTA):
    """Return a new Anim with SMS node order remapped onto Charged's Waluigi skeleton.
    SMS nodes beyond the 38 skeleton bones (prop/locator nodes some NIS files carry; SMS's pose
    accumulator ignores them too) are dropped. `delta` (Charged node -> quaternion) is
    left-multiplied into those nodes' rotation keys to land in Charged's bone frames; pass
    delta=None to keep SMS's frames (see the report: which one looks right depends on how the
    mesh was rebound)."""
    b = Anim()
    b.fmt = 'remapped'
    b.name = a.name if name is None else name
    b.hash = a.hash if name is None else (nl_string_hash(name) if a.hash else 0)
    b.num_keys = a.num_keys
    b.num_root_keys = a.num_root_keys
    b.signature = signature
    b.root_rot = list(a.root_rot)
    b.root_trans = list(a.root_trans)
    b.root_trans_extra = a.root_trans_extra
    b.morph_num_keys = list(a.morph_num_keys)
    b.morph_ids = list(a.morph_ids)
    b.morph_keys = a.morph_keys
    b.nodes = [Node() for _ in range(num_nodes)]
    for si, n in enumerate(a.nodes):
        if si < len(remap):
            b.nodes[remap[si]] = n.copy()
    if delta:
        for ci, d in delta.items():
            n = b.nodes[ci]
            if n.rot is not None:
                n.rot = [qmul(d, q) for q in n.rot]
            elif n.rotz is not None:
                n.rot = [qmul(d, (0.0, 0.0, math.sin(z * math.pi / 65536), math.cos(z * math.pi / 65536)))
                         for z in n.rotz]
                n.rotz = None
    for dst, src in copies.items():
        b.nodes[dst] = b.nodes[src].copy()
    return b


def sms_to_charged_chunk(a, rot_format, name=None, signature=CHARGED_WALUIGI_SIGNATURE,
                         delta=CHARGED_BIND_DELTA):
    """SMS Anim -> Charged 0x80017000 chunk on the Waluigi skeleton."""
    b = remap_to_charged(a, name=name, signature=signature, delta=delta)
    return encode_charged_chunk(b, rot_format=rot_format)


# ---------------------------------------------------------------- bundles

def chunks_from_file(path, fmt):
    d = load_bytes(path)
    return nlchunks.parse(d, charged=(fmt == CHARGED))


def write_sanim_zlib(path, anim_chunks):
    raw = nlchunks.write(anim_chunks, charged=True)
    with open(path, 'wb') as f:
        f.write(struct.pack('>I', len(raw)))
        f.write(zlib.compress(raw, 9))
    return raw


def build_bundle(base_path, replacements, out_path):
    """Charged bundle = base bundle's chunks in order, with chunk for each name in `replacements`
    (dict name -> Charged Chunk) swapped in place. Returns the raw bytes."""
    chunks = chunks_from_file(base_path, CHARGED)
    names = []
    for i, c in enumerate(chunks):
        nm = c.children[1].data.split(b'\0')[0].decode('latin1')
        names.append(nm)
        if nm in replacements:
            chunks[i] = replacements[nm]
    missing = set(replacements) - set(names)
    if missing:
        raise KeyError('not in base bundle: %s' % sorted(missing))
    return write_sanim_zlib(out_path, chunks)


# ---------------------------------------------------------------- comparison

def quat_angle(q1, q2):
    """Angle (degrees) between two rotations given as possibly non-unit quaternions."""
    def n(q):
        l = math.sqrt(sum(c*c for c in q)) or 1.0
        return tuple(c / l for c in q)
    q1, q2 = n(q1), n(q2)
    d = abs(sum(a*b for a, b in zip(q1, q2)))
    return math.degrees(2 * math.acos(min(1.0, d)))


def compare(a, b, nodes=None, rot_tol_deg=None):
    """Per-node max differences between two decoded anims (same node order, same key count).
    Returns dict with max rotation angle (deg), max translation, max scale, root diffs."""
    res = {'rot': 0.0, 'trans': 0.0, 'scale': 0.0, 'root_trans': 0.0, 'root_rot': 0, 'per_node': {}}
    nk = min(a.num_keys, b.num_keys)
    for i in (nodes if nodes is not None else range(min(a.num_nodes, b.num_nodes))):
        na, nb = a.nodes[i], b.nodes[i]
        r = t = s = 0.0
        for k in range(nk):
            qa, qb = a.node_rot(i, k), b.node_rot(i, k)
            if qa is not None or qb is not None:
                r = max(r, quat_angle(qa or (0, 0, 0, 1), qb or (0, 0, 0, 1)))
            if na.trans is not None or nb.trans is not None:
                ta = na.trans[min(k, len(na.trans)-1)] if na.trans else (0, 0, 0)
                tb = nb.trans[min(k, len(nb.trans)-1)] if nb.trans else (0, 0, 0)
                t = max(t, max(abs(x - y) for x, y in zip(ta, tb)))
            if na.scale is not None or nb.scale is not None:
                sa = na.scale[min(k, len(na.scale)-1)] if na.scale else (1, 1, 1)
                sb = nb.scale[min(k, len(nb.scale)-1)] if nb.scale else (1, 1, 1)
                s = max(s, max(abs(x - y) for x, y in zip(sa, sb)))
        res['per_node'][i] = (r, t, s)
        res['rot'] = max(res['rot'], r)
        res['trans'] = max(res['trans'], t)
        res['scale'] = max(res['scale'], s)
    for k in range(min(a.num_root_keys, b.num_root_keys)):
        res['root_trans'] = max(res['root_trans'], max(abs(x - y) for x, y in zip(a.root_trans[k], b.root_trans[k])))
        dr = abs(_s16((a.root_rot[k] - b.root_rot[k]) & 0xFFFF))
        res['root_rot'] = max(res['root_rot'], dr)
    return res


LEG_NODES = (5, 7, 9, 11)   # thigh/foot: Charged re-solved these (longer legs), ~1.3 deg median


def verify_gameplay(sms_path, charged_path, delta=CHARGED_BIND_DELTA, out=print):
    """Convert every SMS animation that Charged's bundle also has (same name, same key count) and
    compare with Charged's own encoding. Returns rows (name, max rot excl. legs, max leg rot,
    max trans, root trans, root rot units, median rot)."""
    import statistics
    _, sms = load(sms_path, SMS)
    _, ch = load(charged_path, CHARGED)
    cb = {x.name: x for x in ch}
    rows = []
    for sa in sms:
        ca = cb.get(sa.name)
        if ca is None:
            continue
        if ca.num_keys != sa.num_keys:
            rows.append((sa.name, None, None, None, None, None, None, 'keys %d vs %d' % (sa.num_keys, ca.num_keys)))
            continue
        conv = anim_from_chunk(sms_to_charged_chunk(sa, rot_format=12, delta=delta), CHARGED)
        body = [i for i in range(CHARGED_NUM_NODES) if i not in LEG_NODES]
        r = compare(conv, ca, nodes=body)
        rl = compare(conv, ca, nodes=LEG_NODES)
        errs = []
        for i in body:
            for k in range(ca.num_keys):
                qa, qb = conv.node_rot(i, k), ca.node_rot(i, k)
                if qa is not None or qb is not None:
                    errs.append(quat_angle(qa or (0, 0, 0, 1), qb or (0, 0, 0, 1)))
        rows.append((sa.name, r['rot'], rl['rot'], max(r['trans'], rl['trans']), r['root_trans'], r['root_rot'],
                     statistics.median(errs) if errs else 0.0, ''))
    return rows


def describe(a):
    kinds = []
    for i, n in enumerate(a.nodes):
        r = ('z%d' % len(n.rotz)) if n.rotz is not None else ('q%d' % len(n.rot)) if n.rot is not None else '-'
        t = ('t%d' % len(n.trans)) if n.trans is not None else '-'
        s = ('s%d' % len(n.scale)) if n.scale is not None else '-'
        kinds.append('%d:%s/%s/%s/%x' % (i, r, t, s, n.props))
    return ' '.join(kinds)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd = argv[1]
    if cmd == 'list':
        fmt = CHARGED if '--charged' in argv else SMS if '--sms' in argv else None
        fmt, anims = load(argv[2], fmt)
        print(argv[2], fmt, len(anims), 'animations')
        for a in anims:
            print('  %08x %-40s keys %4d nodes %2d morph %2d sig %08x' % (
                a.hash, a.name, a.num_keys, a.num_nodes, a.num_morph, a.signature))
            if '-v' in argv:
                print('    ' + describe(a))
        return 0
    if cmd == 'compare':
        _, sms = load(argv[2], SMS)
        _, ch = load(argv[3], CHARGED)
        name = argv[4]
        sa = next(x for x in sms if x.name == name)
        ca = next(x for x in ch if x.name == name)
        conv = anim_from_chunk(sms_to_charged_chunk(sa, rot_format=12), CHARGED)
        r = compare(conv, ca)
        print('converted vs charged %s: max rot %.4f deg, trans %.6f, scale %.6f, root trans %.6f, root rot %d'
              % (name, r['rot'], r['trans'], r['scale'], r['root_trans'], r['root_rot']))
        return 0
    if cmd == 'verify':
        rows = verify_gameplay(argv[2], argv[3], delta=None if '--sms-frames' in argv else CHARGED_BIND_DELTA)
        print('%-34s %9s %9s %9s %9s %6s %8s' % ('animation', 'maxrot', 'legs', 'trans', 'roott', 'rootr', 'median'))
        for nm, mr, ml, mt, rt, rr, med, note in rows:
            if mr is None:
                print('%-34s %s' % (nm, note))
            else:
                print('%-34s %9.3f %9.3f %9.5f %9.5f %6d %8.4f' % (nm, mr, ml, mt, rt, rr, med))
        return 0
    if cmd == 'build':
        base, donor, out = argv[2], argv[3], argv[4]
        _, sms = load(donor, SMS)
        by = {a.name: a for a in sms}
        delta = None if '--sms-frames' in argv else CHARGED_BIND_DELTA
        names = [n for n in argv[5:] if not n.startswith('--')]
        rep = {}
        for n in names:   # "name" or "sms_name=charged_name" (renamed: hash = nlStringHash(charged_name))
            src, _, dst = n.partition('=')
            dst = dst or src
            rep[dst] = sms_to_charged_chunk(by[src], rot_format=12, delta=delta,
                                            name=None if dst == src else dst)
        raw = build_bundle(base, rep, out)
        print('wrote', out, len(raw), 'bytes uncompressed;', len(rep), 'replaced')
        return 0
    print(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
