#!/usr/bin/env python3
"""nis.py: Super Mario Strikers (GC) / Mario Strikers Charged (Wii) cutscene (.nis) files.

    python3 nis.py list <file.nis> [--sms|--charged]
    python3 nis.py convert <sms.nis> <out charged.nis> [--name <dict name>] [--anim-name SuperTeam]
                   [--sms-frames]                       # also prints the nis_dict.txt entry
    python3 nis.py dict <charged-or-sms.nis> [name]      # nis_dict.txt entry for an existing file
    python3 nis.py superteam <out dir>                   # the Super Team set (see SUPERTEAM_MAP), from ./sms/nis

A .nis is a flat list of NL chunks (nlchunks.py): one 0x80017000 animation per actor (the sanim
chunk of sanim.py; SMS header 0x48, Charged 0x58) followed by the cameras.

Camera chunk                SMS 0x80015501           Charged 0x8002500B (cAnimCamera loader)
  name hash (nlStringHash)  15502                    25001  (not read)
  name                      15503                    25000
  64-byte block             15504 (stale memory)     25002  camera matrix at key 0: rows of
                                                            quat->matrix(camRot[0]), row 3 = camPos[0]
  64-byte block             15505 (stale memory)     2500D  same for target: targetRot[0], targetPos[0]
  initial FOV               15506                    2500E  (copied unconverted)
  initial focus distance    15507                    2500F
  key count                 15508                    2500C
  camera position  f32x3    03015509                 03025003
  camera rotation  f32x4    03015511                 03025004
  camera scale     f32x3    0301550B (1,1,1)         03025005  Charged writes the position again
  target position  f32x3    0301550C                 03025006
  target rotation  f32x4    03015512                 03025007
  target scale     f32x3    0301550E (1,1,1)         03025008  Charged writes target position again
  FOV (deg)        f32      0301550F (horizontal)    03025009  vertical: 2*atan(0.75*tan(h/2))
  focus distance   f32      03015510                 0302500A
(03xxxxxx = payload aligned to 8 bytes, pad counted in the size.)

Charged binds animation chunks in file order (Render/Nis.cpp): an animation whose name starts
with an NPC template name (Art/NPCList, case-insensitive prefix: NIS_ball, Podium, FlyingCamera,
effects_mesh, ...) becomes a prop; every other one goes to the NIS target character (scorer /
captain / ...), then the home captain, the away captain, the first free slot. The name is not
otherwise used (SMS names them all "nis"; Charged names them after the character). Played with
no retarget: node i drives bone i of the character's .shier.

nis_dict.txt entry (NisPlayer::fn_8027B880; all fields required, CRLF, tab-indented):
  size            file size in bytes (bytes read into the 0x70800 NIS pool)
  has_ball        balls attached to the first character animation (NisHeader.numBalls)
  num_animations  number of 0x80017000 chunks (props included); max 4 begin_pos stored
  num_cameras     number of camera chunks (max 10 loaded)
  center          world pelvis (node 2) of the first animation at key 0:
                  rootTrans[0] + Rz(rootRot[0]) * node2.trans[0]  (root angles of +-1 unit read as 0)
                  Reproduces 155/311 single-animation Charged entries to 1e-6 and 123 more to
                  <=2e-3 (root-angle quantisation); multi-actor files derive it differently.
  min/max_bounds  min/max over keys of (world pelvis[k] - center)
  begin_pos       rootTrans[0] of each animation (0,0,0 when it has no root keys)
  num_anim_proxies + name/position/direction triples (scene locators, not in the .nis)
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nlchunks
import sanim
from nlchunks import Chunk

SMS, CHARGED = sanim.SMS, sanim.CHARGED
FOV_TAN_SCALE = 0.75          # SMS horizontal FOV (4:3) -> Charged vertical FOV
NIS_POOL_SIZE = 0x70800       # NisPlayer::mMemory, shared by every NIS loaded at once (+0x20 each)

SMS_CAM, CHARGED_CAM = 0x80015501, 0x8002500B
SMS_CAM_IDS = dict(hash=0x15502, name=0x15503, blk0=0x15504, blk1=0x15505, fov0=0x15506, focal0=0x15507,
                   count=0x15508, cam_pos=0x15509, cam_rot=0x15511, cam_scale=0x1550B, tgt_pos=0x1550C,
                   tgt_rot=0x15512, tgt_scale=0x1550E, fov=0x1550F, focal=0x15510)
CHARGED_CAM_IDS = dict(hash=0x25001, name=0x25000, blk0=0x25002, blk1=0x2500D, fov0=0x2500E, focal0=0x2500F,
                       count=0x2500C, cam_pos=0x25003, cam_rot=0x25004, cam_scale=0x25005, tgt_pos=0x25006,
                       tgt_rot=0x25007, tgt_scale=0x25008, fov=0x25009, focal=0x2500A)
CHARGED_CAM_ORDER = ['hash', 'name', 'blk0', 'blk1', 'fov0', 'focal0', 'count', 'cam_pos', 'cam_rot',
                     'cam_scale', 'tgt_pos', 'tgt_rot', 'tgt_scale', 'fov', 'focal']
_VEC = {'cam_pos': 3, 'cam_rot': 4, 'cam_scale': 3, 'tgt_pos': 3, 'tgt_rot': 4, 'tgt_scale': 3, 'fov': 1, 'focal': 1}


class Camera:
    def __init__(self):
        self.name = ''
        self.hash = 0
        self.count = 0
        self.fov0 = 0.0
        self.focal0 = 0.0
        self.blk0 = b''
        self.blk1 = b''
        self.tracks = {}      # key -> list of tuples

    def __repr__(self):
        return '<Camera %r keys %d>' % (self.name, self.count)


def camera_from_chunk(ch, fmt):
    ids = SMS_CAM_IDS if fmt == SMS else CHARGED_CAM_IDS
    by = {c.type: c for c in ch.children}
    c = Camera()
    c.name = by[ids['name']].data.split(b'\0')[0].decode('latin1')
    c.hash = struct.unpack('>I', by[ids['hash']].data[:4])[0]
    c.count = struct.unpack('>I', by[ids['count']].data[:4])[0]
    c.fov0 = struct.unpack('>f', by[ids['fov0']].data[:4])[0]
    c.focal0 = struct.unpack('>f', by[ids['focal0']].data[:4])[0]
    c.blk0 = by[ids['blk0']].data
    c.blk1 = by[ids['blk1']].data
    for k, n in _VEC.items():
        d = by[ids[k]].data
        v = struct.unpack_from('>%df' % (n * c.count), d)
        c.tracks[k] = [tuple(v[n*i:n*i+n]) for i in range(c.count)]
    return c


def qmat(q):
    x, y, z, w = q
    return [[1 - 2*(y*y + z*z), 2*(x*y + z*w), 2*(x*z - y*w)],
            [2*(x*y - z*w), 1 - 2*(x*x + z*z), 2*(y*z + x*w)],
            [2*(x*z + y*w), 2*(y*z - x*w), 1 - 2*(x*x + y*y)]]


def _matrix_block(q, p):
    r = qmat(q)
    m = [r[0][0], r[0][1], r[0][2], 0.0, r[1][0], r[1][1], r[1][2], 0.0,
         r[2][0], r[2][1], r[2][2], 0.0, p[0], p[1], p[2], 1.0]
    return struct.pack('>16f', *m)


def convert_fov(h_deg):
    return math.degrees(2 * math.atan(FOV_TAN_SCALE * math.tan(math.radians(h_deg) / 2)))


def camera_to_charged_chunk(c, convert_from_sms=True):
    """Charged 0x8002500B chunk. From an SMS camera: FOV converted, scale tracks replaced by
    the position tracks and the two matrix blocks rebuilt from key 0, as Charged's exporter does."""
    ids = CHARGED_CAM_IDS
    fov = [(convert_fov(f[0]),) for f in c.tracks['fov']] if convert_from_sms else c.tracks['fov']
    name = c.name.encode('latin1') + b'\0'
    name += b'\0' * ((-len(name)) % 4)
    if convert_from_sms:
        blk0 = _matrix_block(c.tracks['cam_rot'][0], c.tracks['cam_pos'][0])
        blk1 = _matrix_block(c.tracks['tgt_rot'][0], c.tracks['tgt_pos'][0])
        cam_scale, tgt_scale = c.tracks['cam_pos'], c.tracks['tgt_pos']
    else:
        blk0, blk1 = c.blk0, c.blk1
        cam_scale, tgt_scale = c.tracks['cam_scale'], c.tracks['tgt_scale']

    def track(v):
        return b''.join(struct.pack('>%df' % len(t), *t) for t in v)

    payload = {
        'hash': struct.pack('>I', sanim.nl_string_hash(c.name) if convert_from_sms else c.hash),
        'name': name,
        'blk0': blk0, 'blk1': blk1,
        'fov0': struct.pack('>f', c.fov0), 'focal0': struct.pack('>f', c.focal0),
        'count': struct.pack('>I', c.count),
        'cam_pos': track(c.tracks['cam_pos']), 'cam_rot': track(c.tracks['cam_rot']),
        'cam_scale': track(cam_scale), 'tgt_pos': track(c.tracks['tgt_pos']),
        'tgt_rot': track(c.tracks['tgt_rot']), 'tgt_scale': track(tgt_scale),
        'fov': track(fov), 'focal': track(c.tracks['focal']),
    }
    kids = []
    for k in CHARGED_CAM_ORDER:
        cid = ids[k] | (0x03000000 if k in _VEC else 0)
        kids.append(Chunk(cid, payload[k]))
    return Chunk(CHARGED_CAM, children=kids)


class NisFile:
    def __init__(self):
        self.fmt = None
        self.items = []   # ('anim', sanim.Anim) / ('camera', Camera), file order
        self.size = 0

    @property
    def anims(self):
        return [x for k, x in self.items if k == 'anim']

    @property
    def cameras(self):
        return [x for k, x in self.items if k == 'camera']


def guess_fmt(d):
    for ch in nlchunks.parse(d, charged=True):
        if ch.type == CHARGED_CAM:
            return CHARGED
        if ch.type == SMS_CAM:
            return SMS
        if ch.type == 0x80017000:
            hl = struct.unpack('>I', d[ch.off + 12:ch.off + 16])[0]
            return CHARGED if hl == 0x58 else SMS
    return SMS


def load(path, fmt=None):
    d = open(path, 'rb').read()
    fmt = fmt or guess_fmt(d)
    n = NisFile()
    n.fmt = fmt
    n.size = len(d)
    for ch in nlchunks.parse(d, charged=(fmt == CHARGED)):
        if ch.type == 0x80017000:
            n.items.append(('anim', sanim.anim_from_chunk(ch, fmt)))
        elif ch.type in (SMS_CAM, CHARGED_CAM):
            n.items.append(('camera', camera_from_chunk(ch, fmt)))
        else:
            n.items.append(('raw', ch))
    return n


def convert_sms_nis(src, anim_name='SuperTeam', delta=sanim.CHARGED_BIND_DELTA):
    """SMS NisFile -> bytes of a Charged .nis (Waluigi skeleton, 44 nodes)."""
    out = []
    for kind, x in src.items:
        if kind == 'anim':
            out.append(sanim.sms_to_charged_chunk(x, rot_format=16, name=anim_name, delta=delta))
        elif kind == 'camera':
            out.append(camera_to_charged_chunk(x, convert_from_sms=True))
        else:
            raise ValueError('unknown chunk %08x in SMS nis' % x.id)
    return nlchunks.write(out, charged=True)


# ---------------------------------------------------------------- dictionary

def _pelvis_world(a, k, use_root_rot=True):
    r = a.root_trans[min(k, len(a.root_trans) - 1)] if a.root_trans else (0.0, 0.0, 0.0)
    n = a.nodes[2] if len(a.nodes) > 2 else None
    t = n.trans[min(k, len(n.trans) - 1)] if n is not None and n.trans else (0.0, 0.0, 0.0)
    u = a.root_rot[min(k, len(a.root_rot) - 1)] if a.root_rot else 0
    u = u - 65536 if u >= 32768 else u
    if abs(u) <= 1:
        u = 0   # +-1 unit is float noise around 0 in the exporter's source; it used 0 there
    ang = u * (2 * math.pi / 65536) if use_root_rot else 0.0
    c, s = math.cos(ang), math.sin(ang)
    return (r[0] + c*t[0] - s*t[1], r[1] + s*t[0] + c*t[1], r[2] + t[2])


def dict_fields(nis, has_ball=0, use_root_rot=True):
    anims = nis.anims
    a = anims[0]
    c0 = _pelvis_world(a, 0, use_root_rot)
    rel = [tuple(p - q for p, q in zip(_pelvis_world(a, k, use_root_rot), c0)) for k in range(a.num_keys)]
    mn = tuple(min(r[i] for r in rel) for i in range(3))
    mx = tuple(max(r[i] for r in rel) for i in range(3))
    begins = [x.root_trans[0] if x.root_trans else (0.0, 0.0, 0.0) for x in anims]
    return dict(size=nis.size, has_ball=has_ball, num_animations=len(anims), num_cameras=len(nis.cameras),
                center=c0, min_bounds=mn, max_bounds=mx, begin_pos=begins, proxies=[])


def _f(v):
    s = '%f' % v
    return '0.000000' if s == '-0.000000' else s


def dict_entry(name, fields):
    L = ['name %s' % name,
         '\tsize %d' % fields['size'],
         '\thas_ball %d' % fields['has_ball'],
         '\tnum_animations %d' % fields['num_animations'],
         '\tnum_cameras %d' % fields['num_cameras'],
         '\tcenter %s' % ', '.join(_f(x) for x in fields['center']),
         '\tmin_bounds %s' % ', '.join(_f(x) for x in fields['min_bounds']),
         '\tmax_bounds %s' % ', '.join(_f(x) for x in fields['max_bounds'])]
    for b in fields['begin_pos'][:4]:
        L.append('\tbegin_pos %s' % ', '.join(_f(x) for x in b))
    L.append('\tnum_anim_proxies %d' % len(fields['proxies']))
    for pname, pos, direction in fields['proxies']:
        L.append('\tanim_proxy_name %s' % pname)
        L.append('\tanim_proxy_position %s %s' % (_f(pos[0]), _f(pos[1])))
        L.append('\tanim_proxy_direction %d' % direction)
    return '\r\n'.join(L) + '\r\n'


def parse_dict(path):
    txt = open(path, 'rb').read().decode('latin1').replace('\r', '')
    ents = []
    cur = None
    for line in txt.split('\n'):
        if line.startswith('name '):
            cur = {'name': line[5:].strip(), 'begin_pos': [], 'proxies': []}
            ents.append(cur)
            continue
        if cur is None or not line.strip():
            continue
        k, _, v = line.strip().partition(' ')
        if k in ('size', 'has_ball', 'num_animations', 'num_cameras', 'num_anim_proxies'):
            cur[k] = int(v)
        elif k in ('center', 'min_bounds', 'max_bounds'):
            cur[k] = tuple(float(x) for x in v.split(','))
        elif k == 'begin_pos':
            cur['begin_pos'].append(tuple(float(x) for x in v.split(',')))
        elif k == 'anim_proxy_name':
            cur['proxies'].append([v])
        elif k == 'anim_proxy_position':
            cur['proxies'][-1].append(tuple(float(x) for x in v.split()))
        elif k == 'anim_proxy_direction':
            cur['proxies'][-1].append(int(v))
    return ents


def dict_error(entry, fields):
    """Max abs difference over center/bounds/begin_pos, plus exact-field mismatches."""
    e = 0.0
    for k in ('center', 'min_bounds', 'max_bounds'):
        e = max(e, max(abs(a - b) for a, b in zip(entry[k], fields[k])))
    for a, b in zip(entry['begin_pos'], fields['begin_pos']):
        e = max(e, max(abs(x - y) for x, y in zip(a, b)))
    bad = [k for k in ('size', 'num_animations', 'num_cameras') if entry[k] != fields[k]]
    return e, bad


# ---------------------------------------------------------------- the Super Team set

# SMS file -> Charged NIS name(s). Charged type names from presentation.byte_code / NisPlayer::Load
# (prefix "<team or CharacterInfo name>_<type>"). "_home" in a name makes IsMirrored() flip it for
# the away captain, the way SMS mirrored attitude_home for the away side.
SUPERTEAM_MAP = [
    ('mystery_goal_winner_high_0', ['superteam_goal_winner_high_0']),
    ('mystery_goal_winner_high_1', ['superteam_goal_winner_high_1']),
    ('mystery_goal_winner_low_0', ['superteam_goal_winner_low_0']),
    ('mystery_end_of_game_home_0', ['superteam_end_of_game_holotron_home']),
    ('mystery_enter_stadium_home_0', ['superteam_home_capt_intro_1']),
    ('mystery_run_to_center_0', ['superteam_home_capt_intro_2']),
    ('mystery_attitude_home_0', ['superteam_home_capt_intro_3', 'superteam_away_capt_intro_2_home']),
    ('mystery_enter_stadium_away_0', ['superteam_away_capt_intro_1']),
]


def build_superteam(sms_nis_dir, out_dir, sms_dict=None, anim_name='SuperTeam', delta=sanim.CHARGED_BIND_DELTA):
    os.makedirs(out_dir, exist_ok=True)
    smsd = {e['name']: e for e in parse_dict(sms_dict)} if sms_dict else {}
    entries = []
    report = []
    for src_name, outs in SUPERTEAM_MAP:
        src = load(os.path.join(sms_nis_dir, src_name + '.nis'), SMS)
        data = convert_sms_nis(src, anim_name=anim_name, delta=delta)
        has_ball = smsd.get(src_name + '.nis', {}).get('has_ball', 0)
        for o in outs:
            path = os.path.join(out_dir, o + '.nis')
            open(path, 'wb').write(data)
            conv = load(path, CHARGED)
            f = dict_fields(conv, has_ball=has_ball)
            entries.append(dict_entry(o + '.nis', f))
            report.append((src_name, o, len(data), src.anims[0].num_keys, len(src.cameras)))
    with open(os.path.join(out_dir, 'superteam_nis_dict.txt'), 'wb') as fh:
        fh.write(''.join(entries).encode('latin1'))
    return report


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd = argv[1]
    fmt = CHARGED if '--charged' in argv else SMS if '--sms' in argv else None
    delta = None if '--sms-frames' in argv else sanim.CHARGED_BIND_DELTA
    if cmd == 'list':
        n = load(argv[2], fmt)
        print(argv[2], n.fmt, n.size, 'bytes')
        for kind, x in n.items:
            print('  ', kind, x)
        return 0
    if cmd == 'dict':
        n = load(argv[2], fmt)
        name = argv[3] if len(argv) > 3 and not argv[3].startswith('--') else os.path.basename(argv[2]).lower()
        sys.stdout.write(dict_entry(name, dict_fields(n)))
        return 0
    if cmd == 'convert':
        src = load(argv[2], SMS)
        anim_name = argv[argv.index('--anim-name') + 1] if '--anim-name' in argv else 'SuperTeam'
        data = convert_sms_nis(src, anim_name=anim_name, delta=delta)
        open(argv[3], 'wb').write(data)
        name = argv[argv.index('--name') + 1] if '--name' in argv else os.path.basename(argv[3]).lower()
        sys.stdout.write(dict_entry(name, dict_fields(load(argv[3], CHARGED))))
        return 0
    if cmd == 'superteam':
        smsdir = os.path.join(os.getcwd(), 'sms', 'nis')  # SMS's cutscenes, under the working folder
        rep = build_superteam(smsdir, argv[2], os.path.join(smsdir, 'nis_dict.txt'), delta=delta)
        for r in rep:
            print('%-30s -> %-38s %6d bytes, %3d keys, %d cameras' % r)
        return 0
    print(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
