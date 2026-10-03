#!/usr/bin/env python3
# smsfx2bun.py: Super Mario Strikers (GameCube) particle effects -> a Mario Strikers Charged (Wii)
# effects bundle pair (<name>Effects.bun + <name>EffectsNonRes.bun.zlib), the form Charged loads with
# EmissionManager::LoadBundle(data, nonres, pool, 1) (as CharacterLoader does for character bundles).
#
#   smsfx2bun.py build  --sms DIR --out-bun X.bun --out-nonres XNonRes.bun.zlib [--compat] [--extra FX] GROUP|PATTERN ...
#   smsfx2bun.py verify --sms DIR [--charged ART/EFFECTS] [-v] [GROUP ...]
#   smsfx2bun.py dump   BUNDLE [GROUP ...]
#
# DIR holds SMS's effect sources: scripts.fx (groups), templates.fx (templates), effects.glt (textures),
# meshes.glg. `build` compiles the named groups (fnmatch patterns allowed) and the templates they play;
# textures Charged keeps resident (art/effects/effectsNonRes.bun.zlib, same name hash) are referenced,
# the others are copied from effects.glt into the nonres bundle (CI8 converted losslessly to RGB5A3, as
# Charged stores these). `verify` compiles every SMS group and template Charged also has (same name hash)
# in --compat mode and compares them with Charged's, byte for byte with the load-time pointer words masked.
#
# ---- Charged's format (big-endian NL chunks; ids with bit 31 set hold children; Charged starts every
#      chunk on 4 bytes). EffectsBundleManager::Load walks the top container's children: 0x80024000 is
#      an effects block (fn_802EAF64), anything else goes to GLResourceChunkLoader (0x24100 textures,
#      0x8001B000/0x8001B100 models, into the pool passed to LoadBundle).
#
#   80000001                          file
#     80024000                        block = one group and its own copy of the templates it plays
#       00024001  24 bytes            EffectsBundleData {0, groupHash, numTemplates, templates*, 1, groups*}
#       00024025  4*numTemplates      template pointer table (filled at load)
#       00024026  4                   group pointer table (filled at load)
#       80024002  x numTemplates      EffectsTemplate::LoadFromChunk
#         00024003  220 bytes         EffectsTemplate up to m_cColour[25] (see TEMPLATE_FIELDS)
#         80024004  x 8               fxAnimatedRange properties (see PROP_NAMES)
#           00024005  20 bytes        {useCurve, base, range, numKeys, keys*}
#           00024006  20*numKeys      fxCurveKey {time, cubic, quadratic, linear, constant} (useCurve only)
#       80024020                      EffectsGroup::LoadFromChunk
#         00024021  28 bytes          EffectsGroup {hash, specs*, numSpecs, 0, userSpecs*, numUserSpecs, sources*}
#         00024022  0x58*numSpecs     EffectsSpec (see SPEC_FIELDS); +4 indexes this block's templates
#         00024023  8*numUserSpecs    UserEffectSource {size, data*} (then one data chunk per user spec)
#   nonres file: 80000001 { 00024100 PTLG texture bundle: {'PTLG', n, tag, 0}, n x {hash, off, size, 0}
#                sorted by hash, blobs 32-aligned from the PTLG start; GX texture = 32-byte header + data }
#   Groups go into one tree by hash (a later bundle replaces a group of the same hash); templates only
#   live in their block. Textures resolve through every pool (glx_GetTex); effect meshes only through
#   gEffectsModelInventory (the boot pool's inventory: art/objects/effectsgeometry.bun).
#
# Hashes: groups/templates nlStringLowerHash(name); textures nlStringLowerHash("effects/" + name);
# joints nlStringLowerHash("bip01 " + name with '_' -> ' ') (SMS's GetJointID; Charged's EmissionController
# looks it up with pose->GetNodeMatrixByHashID among the skeleton's node ids, the .shier's 0x18003 chunk);
# meshes nlStringHash("effects/" + name);
# terrains nlStringLowerHash(name).
#
# ---- SMS text -> Charged fields (`verify`: 240 of the 357 templates both games have come out
#      byte-identical, 20 more differ only by float rounding in the size cubic; the rest were edited
#      by Charged's artists):
#   fountainLife      0 if !fountainMode (default true), 1e10 if fountainForever, else fountainLife
#   mass              * -19.62 (SMS: z += mass * -9.81 t^2; Charged integrates mass * t)
#   inheritVelocity   / 100 (as SMS's parser); numbers parsed as doubles, rounded to float when stored
#   rotation          property 3 (spin); 'rotation 0 0.1' (SMS's idiom for a random start) -> spin 0
#   number/radius/velocity/angle/tilt -> properties 0/4/5/6/7 (constant ranges)
#   flags (+0x37)     1 infront, 2 follow (local space), 4 meshLighting, 8 matchModelFPSToParticleLife
#   blend             normal 0 / additive 1; billboard 0/1/2 (billboard, groundboard, softwarecontrolled)
#   spec              joint 1 / object 2 / emitter, ball, puck 0; delay, layer, ascend [velocity], infront,
#                     ground, light, offset, offsetx/y/z/xyz, linger_start/end; terrain -> +0x38
#   Where Charged's own compiler departed from what SMS's runtime does, --compat copies it, the default
#   follows SMS:
#                     --compat (Charged's compiler)            default (SMS's runtime)
#   hemisphere        emitter 1 (sphere)                       emitter 3 (Charged still has it)
#   start angle       random iff the spin has a range          random iff the spin is non-zero
#   size              Catmull-Rom cubic through (0,b,e,0),     straight line b -> e (one key) and the
#                     ranges dropped                           ranges as a per-particle scale (prop 2)
#   colours           SMS's 50-entry key table with the keys   SMS's own table (BlendSpan, then pairs
#                     scaled to 0..50, pairs averaged          averaged: SMS's m_cColour[25])
import argparse, copy, fnmatch, math, os, struct, sys, zlib

DEFAULT_CHARGED = os.path.expanduser('~/Library/Application Support/MSCRecomp/Game/files/Art/effects')


# ------------------------------------------------------------------------------------------ basics
def nl_hash(s):
    h = 0xFFFFFFFF
    for c in s.encode('latin-1'):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def nl_lower_hash(s):
    return nl_hash(s.lower())


def f32(x):
    return struct.unpack('>f', struct.pack('>f', x))[0]


def nz(*v):
    """Floats as Charged stores them: never -0.0 (SMS's 'mass 0' times -19.62 is +0 in its files)."""
    return [0.0 if x == 0 else x for x in v]


def atof(s):
    """A text number as a double: Charged's compiler computed in double from the decimal text and
    rounded to float only when storing (e.g. the size cubic of 0.845 -> 0.14 is f32(-1.8325), not the
    value computed from the float-rounded inputs)."""
    try:
        return float(s)
    except (TypeError, ValueError):
        return 0.0


class Chunk:
    __slots__ = ('id', 'data', 'children')

    def __init__(self, cid, data=b'', children=None):
        self.id = cid
        self.data = data
        self.children = children

    @property
    def type(self):
        return self.id & 0x80FFFFFF

    @property
    def is_container(self):
        return bool(self.id & 0x80000000)


def parse_chunks(d, off=0, end=None):
    """Charged layout: leaves' payload optionally aligned (bits 24..27), next chunk on 4 bytes."""
    end = len(d) if end is None else end
    out = []
    while off + 8 <= end:
        cid, size = struct.unpack_from('>II', d, off)
        body = off + 8
        if cid & 0x80000000:
            ch = Chunk(cid, children=parse_chunks(d, body, body + size))
        else:
            a = (cid >> 24) & 0x0F
            start = (body + (1 << a) - 1) & ~((1 << a) - 1) if a else body
            ch = Chunk(cid, data=bytes(d[start:body + size]))
        out.append(ch)
        off = (body + size + 3) & ~3
    return out


def write_chunks(chunks):
    out = bytearray()

    def emit(ch):
        pos = len(out)
        out.extend(b'\0' * 8)
        body = len(out)
        if ch.is_container:
            for c in ch.children:
                emit(c)
        else:
            out.extend(ch.data)
        struct.pack_into('>II', out, pos, ch.id, len(out) - body)
        if len(out) & 3:
            out.extend(b'\0' * (4 - (len(out) & 3)))

    for c in chunks:
        emit(c)
    return bytes(out)


def load_maybe_zlib(path):
    d = open(path, 'rb').read()
    if path.lower().endswith('.zlib'):
        n = struct.unpack('>I', d[:4])[0]
        d = zlib.decompress(d[4:])
        if len(d) != n:
            raise ValueError('%s: size prefix %d, inflated %d' % (path, n, len(d)))
    return d


def zlib_with_size(d):
    return struct.pack('>I', len(d)) + zlib.compress(d, 6)   # 78 9c, as the disc's


# ------------------------------------------------------------------------------ SMS text sources
def _lines(path):
    """SimpleParser as SMS uses it: lower-cased whitespace tokens, '#' to end of line ignored."""
    for raw in open(path, 'rb').read().decode('latin-1').splitlines():
        s = raw.split('#', 1)[0].strip().lower()
        if s:
            yield s.split()


class SmsTemplate:
    def __init__(self, name):
        self.name = name
        self.fountainLife = 0.0
        self.fountain = True          # fountainMode defaults to true in SMS's parser
        self.forever = False
        self.r = {}                   # number sizebegin sizeend mass particlelife rotation radius
        self.bounce = False           # inheritvelocity velocity acceleration particlefps angle tilt
        self.infront = False
        self.follow = False
        self.lit = False
        self.matchLife = False
        self.emitter = 'circle'
        self.blend = 'normal'
        self.billboard = 'billboard'
        self.texture = None
        self.model = None
        self.nFrames = 0
        self.keys = {}                # channel index -> [(frame, value)]

    def rng(self, k):
        return self.r.get(k, (0.0, 0.0))


RANGES = ('number', 'sizebegin', 'sizeend', 'mass', 'particlelife', 'rotation', 'radius',
          'inheritvelocity', 'velocity', 'acceleration', 'particlefps', 'angle', 'tilt')
CHANNELS = {'red': 0, 'green': 1, 'blue': 2, 'alpha': 3}


def template_token(cur, k, a):
    """One line of a template definition (templates.fx syntax) applied to `cur`."""
    if k == 'fountainlife':
        cur.fountainLife = atof(a[0])
    elif k == 'fountainmode':
        cur.fountain = a[:1] == ['true']
    elif k == 'fountainforever':
        cur.forever = a[:1] == ['true']
    elif k in RANGES:
        cur.r[k] = (atof(a[0]) if a else 0.0, atof(a[1]) if len(a) > 1 else 0.0)
    elif k in ('bounce', 'meshlighting', 'infront', 'follow', 'matchmodelfpstoparticlelife'):
        v = a[:1] == ['true']
        setattr(cur, {'bounce': 'bounce', 'meshlighting': 'lit', 'infront': 'infront', 'follow': 'follow',
                      'matchmodelfpstoparticlelife': 'matchLife'}[k], v)
    elif k == 'texturenumframes':
        cur.nFrames = int(atof(a[0]))
    elif k in ('emitter', 'blendmode', 'billboardmode'):
        setattr(cur, {'emitter': 'emitter', 'blendmode': 'blend', 'billboardmode': 'billboard'}[k], a[0])
    elif k == 'texture':
        cur.texture = a[0] if a else None
    elif k == 'model':
        cur.model = a[0] if a else None
    elif k in CHANNELS:
        keys = []
        for tok in a:
            i, _, v = tok.partition(':')
            keys.append((int(i), int(v)))
        cur.keys[CHANNELS[k]] = keys
    elif k == 'link':
        pass
    else:
        print('warning: %s: unknown template token %r' % (cur.name, k), file=sys.stderr)


def parse_templates(path):
    out = {}
    cur = None
    for t in _lines(path):
        k = t[0]
        if cur is None:
            if k == 'begin' and len(t) > 1:
                cur = SmsTemplate(t[1])
            continue
        if k == 'end':
            out[cur.name] = cur
            cur = None
        else:
            template_token(cur, k, t[1:])
    return out


class SmsSpec:
    def __init__(self, template):
        self.template = template
        self.attach = 0
        self.joint = None
        self.jointID = 0
        self.delay = 0.0
        self.layer = 0
        self.jbind = 0
        self.jvel = 0.0
        self.infront = False
        self.ground = False
        self.light = False
        self.offset = 0.0
        self.local = [0.0, 0.0, 0.0]
        self.lingerStart = -1.0
        self.lingerEnd = -1.0
        self.terrain = None


def joint_id(name):
    return nl_lower_hash('bip01 ' + name.replace('_', ' '))


def parse_spec(t, group):
    s = SmsSpec(t[1])
    i = 2
    while i < len(t):
        k = t[i]
        i += 1
        nxt = t[i] if i < len(t) else None
        if k in ('at', 'on'):
            b = nxt
            i += 1
            if b == 'emitter':
                s.attach = 0
            elif b == 'joint':
                s.attach = 1
                s.joint = t[i]
                s.jointID = joint_id(t[i])
                i += 1
                if i < len(t) and t[i] == 'ascend':
                    i += 1
                    s.jbind = 1
                    s.jvel = 1.0
                    if i < len(t):
                        s.jvel = atof(t[i])
                        i += 1
            elif b == 'object':
                s.attach = 2
            elif b in ('ball', 'puck'):
                s.attach = 0       # SMS: FXBind_Object; Charged's compiler wrote 0 (same position code path)
            else:
                print('warning: %s: unknown binding %r' % (group, b), file=sys.stderr)
        elif k == 'linger_start':
            s.lingerStart = s.lingerEnd = atof(nxt)
            i += 1
        elif k == 'linger_end':
            s.lingerEnd = atof(nxt)
            i += 1
        elif k == 'layer':
            s.layer = int(atof(nxt))
            i += 1
        elif k == 'infront':
            s.infront = True
        elif k == 'delay':
            s.delay = atof(nxt)
            i += 1
        elif k == 'ground':
            s.ground = True
        elif k == 'light':
            s.light = True
        elif k == 'offset':
            s.offset = atof(nxt)
            i += 1
        elif k in ('offsetx', 'offsety', 'offsetz'):
            s.local['xyz'.index(k[-1])] = atof(nxt)
            i += 1
        elif k == 'offsetxyz':
            s.local = [atof(x) for x in t[i:i + 3]]
            i += 3
        else:
            print('warning: %s: unknown spec token %r' % (group, k), file=sys.stderr)
    return s


def parse_scripts(path):
    """{group: {'specs': [SmsSpec], 'user': [token lines]}}"""
    out = {}
    cur = None
    terrain = None
    for t in _lines(path):
        k = t[0]
        if cur is None:
            if k == 'begin' and len(t) > 1:
                cur = {'name': t[1], 'specs': [], 'user': []}
            continue
        if k == 'end':
            if terrain is not None:
                terrain = None
                continue
            out[cur['name']] = cur
            cur = None
        elif k == 'play':
            s = parse_spec(t, cur['name'])
            s.terrain = terrain
            cur['specs'].append(s)
        elif k == 'terrain':
            terrain = t[1:]
        else:
            cur['user'].append(t)
    return out


def parse_extra(path, templates, scripts):
    """A pack's own templates and groups, in SMS's syntax, made of SMS's (adds to `templates`/`scripts`):

        template <name> [from <SMS template>]     a template: SMS's, with the lines that follow changed
            <templates.fx lines>
        end
        group <name> [from <SMS group>]           a group: SMS's specs, plus the plays that follow
            play <template> <scripts.fx binding...>
        end
    """
    cur = kind = None
    for t in _lines(path):
        k = t[0]
        if cur is None:
            if k in ('template', 'group') and len(t) > 1:
                kind, name = k, t[1]
                base = t[3] if len(t) > 3 and t[2] == 'from' else None
                if kind == 'template':
                    if base is not None and base not in templates:
                        sys.exit('%s: %s: no SMS template %r' % (path, name, base))
                    cur = copy.deepcopy(templates[base]) if base else SmsTemplate(name)
                    cur.name = name
                else:
                    if base is not None and base not in scripts:
                        sys.exit('%s: %s: no SMS group %r' % (path, name, base))
                    cur = {'name': name, 'specs': [copy.copy(x) for x in scripts[base]['specs']] if base else [],
                           'user': list(scripts[base]['user']) if base else []}
            continue
        if k == 'end':
            if kind == 'template':
                templates[cur.name] = cur
            else:
                scripts[cur['name']] = cur
            cur = None
        elif kind == 'template':
            template_token(cur, k, t[1:])
        elif k == 'play':
            cur['specs'].append(parse_spec(t, cur['name']))
        else:
            print('warning: %s: %s: unknown group line %r' % (path, cur['name'], ' '.join(t)), file=sys.stderr)


# ------------------------------------------------------------------------------------- compiling
EMITTERS = {'circle': 0, 'sphere': 1, 'spindle': 2, 'hemisphere': 3, 'disc': 4}
BLENDS = {'normal': 0, 'additive': 1}
BILLBOARDS = {'billboard': 0, 'groundboard': 1, 'softwarecontrolled': 2}


EPS = f32(0.00001)


def colours25(t, compat=True):
    """The 25 colours from SMS's channel keys (frames 0..49).

    SMS's own parser (GetColourComponent/BlendSpan, InterpolateColours): a 50-entry table filled
    span by span from key to key with a float step accumulated in a float, each entry
    (u8)(current + 0.00001f), then averaged in pairs into 25 (m_cColour[25] in both games).
    Charged's compiler (compat) did the same with the key frames scaled to 0..50 (int(frame * 50 / 49):
    only the final key at 49 moves, to 50) and the first 50 of the 51 entries averaged; that matches
    every colour channel of the shared templates Charged's artists left alone (1319 of 1468; the rest
    differ by tens of levels: recoloured). Without compat: SMS's own table."""
    out = [[0, 0, 0, 0] for _ in range(25)]
    for ch in range(4):
        keys = [((i * 50 // 49) if compat else i, v) for i, v in t.keys.get(ch, [])] or [(0, 0)]
        table = [None] * 51
        for (i0, v0), (i1, v1) in zip(keys, keys[1:]):
            if 0 <= i1 <= 50:
                table[i1] = v1 & 255
            if i1 <= i0:
                continue
            step = f32(f32(v1 - v0) / f32(i1 - i0))
            cur = f32(v0)
            for i in range(i0, i1):
                if 0 <= i <= 50:
                    # Charged's compiler (x86) added the epsilon in extended precision; SMS on the
                    # GameCube adds in single precision (fadds) before the conversion
                    table[i] = int(cur + EPS if compat else f32(cur + EPS)) & 255
                cur = f32(cur + step)
        # entries no span reached (keys not starting at 0 / ending at 49) are stale memory in SMS;
        # hold the nearest key's value instead
        first = keys[0][1] & 255
        for i in range(51):
            if table[i] is None:
                table[i] = first if i < keys[0][0] else (table[i - 1] if i else first)
        for k in range(25):
            out[k][ch] = (table[2 * k] + table[2 * k + 1]) // 2
    return [bytes(c) for c in out]


def prop_const(base, rng):
    return Chunk(0x80024004, children=[Chunk(0x24005, struct.pack('>IffII', 0, *nz(base, rng), 0, 0))])


def prop_size(t, compat=True):
    """Properties 1 (size over the particle's life) and 2 (per-particle size scale)."""
    sb, sbr = t.rng('sizebegin')
    se, ser = t.rng('sizeend')
    if sb == se:
        return prop_const(sb, sbr), prop_const(1.0, 0.0)
    if compat:
        # Charged's compiler: the Catmull-Rom segment through (0, sb, se, 0), tangents se/2 and -sb/2,
        # computed in double; ranges dropped.
        a, b, c = 1.5 * sb - 1.5 * se, -2.5 * sb + 2.0 * se, 0.5 * se
        scale = prop_const(1.0, 0.0)
    else:
        # SMS: size = lerp(sizeBegin +- range/2, sizeEnd +- range/2, life fraction): the straight line,
        # and the ranges as one per-particle scale of about the same relative spread.
        a, b, c = 0.0, 0.0, se - sb
        spread = (abs(sbr) + abs(ser)) / (abs(sb) + abs(se))
        scale = prop_const(1.0, min(spread, 2.0))
    key = struct.pack('>5f', *nz(0.0, a, b, c, sb))
    return (Chunk(0x80024004, children=[Chunk(0x24005, struct.pack('>IffII', 1, 0.0, 0.0, 1, 0)),
                                        Chunk(0x24006, key)]), scale)


def compile_template(t, tex_hash, opts):
    b = bytearray(0xDC)
    fl = t.fountainLife
    if not t.fountain:
        fl = 0.0
    elif t.forever:
        fl = 1.0e10
    struct.pack_into('>If', b, 0, nl_lower_hash(t.name), *nz(fl))
    mass = t.rng('mass')
    struct.pack_into('>2f', b, 0x08, *nz(mass[0] * -19.62, mass[1] * -19.62))
    struct.pack_into('>2f', b, 0x10, *nz(*t.rng('particlelife')))
    iv = t.rng('inheritvelocity')
    struct.pack_into('>2f', b, 0x18, *nz(iv[0] / 100.0, iv[1] / 100.0))
    struct.pack_into('>2f', b, 0x20, *nz(*t.rng('acceleration')))
    rot = t.rng('rotation')
    # start angle: Charged's compiler randomised it when the spin has a range; SMS's runtime whenever
    # the particle's spin is non-zero
    spin = (rot[1] != 0.0) if opts.compat else (rot != (0.0, 0.0))
    struct.pack_into('>2f', b, 0x28, 0.0, 360.0 if spin else 0.0)
    struct.pack_into('>f', b, 0x30, 0.0)       # chance (%) of a mirrored texture: SMS has none
    em = EMITTERS[t.emitter]
    if em == 3 and opts.compat:
        em = 1          # Charged's compiler wrote every SMS 'Hemisphere' as Sphere
    flags = (1 if t.infront else 0) | (2 if t.follow else 0) | (4 if t.lit else 0) | (8 if t.matchLife else 0)
    b[0x34:0x38] = bytes([em, BLENDS[t.blend], BILLBOARDS[t.billboard], flags])
    struct.pack_into('>Ii', b, 0x38, tex_hash, max(1, t.nFrames))
    struct.pack_into('>2f', b, 0x4C, *nz(*t.rng('particlefps')))
    struct.pack_into('>I', b, 0x54, nl_hash('effects/' + t.model) if t.model else 0xFFFFFFFF)
    for k, c in enumerate(colours25(t, opts.compat)):
        b[0x78 + 4 * k:0x7C + 4 * k] = c
    if rot[0] == 0.0 and abs(rot[1]) <= 0.1:
        rot = (0.0, 0.0)    # SMS's idiom 'rotation 0 0.1' (random start, no visible spin): Charged keeps the start only
    props = [prop_const(*t.rng('number')), *prop_size(t, opts.compat), prop_const(*rot),
             prop_const(*t.rng('radius')), prop_const(*t.rng('velocity')), prop_const(*t.rng('angle')),
             prop_const(*t.rng('tilt'))]
    return Chunk(0x80024002, children=[Chunk(0x24003, bytes(b))] + props)


# --face-joints: per joint ID, the spec "axis" (+0x48: 1..6 = +X +Y +Z -X -Y -Z of the joint's frame) whose
# direction in the bind pose is the character's forward. EmissionController::fn_802E5164 then aims the
# particle system's forward along it every frame, so its frame (local Y up, local Z backwards) follows
# the body as SMS's did (SMS faced cutscene effects with the character; Charged's cutscene effects
# keep the default straight-up direction, which turns SMS's "offsety" (up) into a sideways shift).
FACE_AXIS = {}


def face_axes(rlg_path):
    """{joint ID: axis} from a Charged model's bone matrices (bone -> world, row vectors)."""
    import sms2rlg as m
    t = open(rlg_path, 'rb').read()
    tch = m.chunks(t)
    tbo, tbs = tch[0x1B00A][0]
    binds = {}
    for i in range(tbs // 0x44):
        h = struct.unpack('>I', t[tbo + i * 0x44:tbo + i * 0x44 + 4])[0]
        binds[h] = m.mat_from(struct.unpack('>16f', t[tbo + i * 0x44 + 4:tbo + i * 0x44 + 68]))
    foot, toe = binds[joint_id('r_foot')][3], binds[joint_id('r_toe0')][3]
    fwd = (toe[0] - foot[0], toe[1] - foot[1], 0.0)
    n = math.hypot(fwd[0], fwd[1])
    fwd = (fwd[0] / n, fwd[1] / n, 0.0)
    out = {}
    for h, w in binds.items():
        dots = [sum(w[k][c] * fwd[c] for c in range(3)) for k in range(3)]
        k = max(range(3), key=lambda i: abs(dots[i]))
        out[h] = (k + 1) if dots[k] > 0 else (k + 4)
    return out


def compile_spec(s, tindex):
    """EffectsSpec records for one 'play' line: one per terrain it is limited to (Charged keeps a single
    terrain hash per spec, +0x38, compared with fxGetTerrain(); SMS lists several per spec)."""
    out = []
    for terrain in (s.terrain or [None]):
        b = bytearray(0x58)
        struct.pack_into('>IIIIfIfIIIf3fIffIi', b, 0, 0, tindex, s.attach, s.jointID, *nz(s.delay), s.jbind,
                         *nz(s.jvel), int(s.infront), int(s.ground), int(s.light), *nz(s.offset, *s.local),
                         nl_lower_hash(terrain) if terrain else 0, *nz(s.lingerStart, s.lingerEnd), s.layer,
                         FACE_AXIS.get(s.jointID, 0) if s.attach == 1 else 0)
        out.append(bytes(b))
    return out


def compile_group(g, templates, tex_hash_of, opts):
    """One 0x80024000 block: the group and its templates (each once, in order of first use)."""
    order = []
    specs = []
    for s in g['specs']:
        if s.template not in templates:
            raise KeyError('group %s: no template %r' % (g['name'], s.template))
        if s.template not in order:
            order.append(s.template)
        specs += compile_spec(s, order.index(s.template))
    if g['user']:
        print('warning: %s: user effects not converted: %s' % (g['name'], [' '.join(u) for u in g['user']]),
              file=sys.stderr)
    gh = nl_lower_hash(g['name'])
    hdr = struct.pack('>6I', 0, gh, len(order), 0, 1, 0)
    grp = struct.pack('>7I', gh, 0, len(specs), 0, 0, 0, 0)
    kids = [Chunk(0x24001, hdr), Chunk(0x24025, b'\0' * 4 * len(order)), Chunk(0x24026, b'\0' * 4)]
    kids += [compile_template(templates[n], tex_hash_of(templates[n]), opts) for n in order]
    kids.append(Chunk(0x80024020, children=[Chunk(0x24021, grp), Chunk(0x24022, b''.join(specs)),
                                            Chunk(0x24023, b'')]))
    return Chunk(0x80024000, children=kids)


# -------------------------------------------------------------------------------------- textures
def read_ptlg(b, header):
    n = struct.unpack('>I', b[4:8])[0]
    base = header + 16 * n
    out = {}
    for i in range(n):
        h, off, size, _ = struct.unpack_from('>IIII', b, header + 16 * i)
        out[h] = b[base + off:base + off + size]
    return out


def resident_textures(paths):
    have = {}
    for p in paths:
        if not os.path.exists(p):
            print('warning: resident bundle %s not found' % p, file=sys.stderr)
            continue
        for top in parse_chunks(load_maybe_zlib(p)):
            for c in top.children or ():
                if c.type == 0x24100:
                    have.update(read_ptlg(c.data, 0x10))
    return have


def ci8_to_rgb5a3(blob):
    """CI8 + RGB5A3 palette (SMS) -> RGB5A3 (as Charged stores these effect textures); lossless."""
    mips, fmt = struct.unpack('>II', blob[:8])
    w, h = struct.unpack('>HH', blob[14:18])
    entries = struct.unpack('>I', blob[20:24])[0]
    pal = struct.unpack('>%dH' % entries, blob[len(blob) - 2 * entries:])
    src = 32
    out = bytearray()
    lw, lh = w, h
    for _ in range(mips):
        img = [[0] * lw for _ in range(lh)]
        for ty in range(0, lh, 4):
            for tx in range(0, lw, 8):
                for y in range(4):
                    for x in range(8):
                        if ty + y < lh and tx + x < lw:
                            img[ty + y][tx + x] = pal[blob[src]]
                        src += 1
        for ty in range(0, lh, 4):
            for tx in range(0, lw, 4):
                for y in range(4):
                    for x in range(4):
                        v = img[ty + y][tx + x] if ty + y < lh and tx + x < lw else 0
                        out += struct.pack('>H', v)
        lw, lh = max(1, lw // 2), max(1, lh // 2)
    hdr = bytearray(blob[:32])
    struct.pack_into('>I', hdr, 4, 1)          # GXTex_RGB5A3
    hdr[8:12] = bytes([5, 5, 5, 3])            # numBits as Charged's RGB5A3 textures
    struct.pack_into('>I', hdr, 20, 0)         # no palette
    return bytes(hdr) + bytes(out)


def build_ptlg(textures, tag):
    items = sorted(textures.items())
    n = len(items)
    data = bytearray()
    dict_ = bytearray()
    base = 0x10 + 16 * n
    for h, blob in items:
        while (base + len(data)) % 32:
            data.append(0)
        dict_ += struct.pack('>IIII', h, len(data), len(blob), 0)
        data += blob
    return b'PTLG' + struct.pack('>III', n, tag, 0) + bytes(dict_) + bytes(data)


# ------------------------------------------------------------------------------------------ build
CHARGED_FILES = os.path.dirname(DEFAULT_CHARGED.rstrip('/'))   # .../files/Art


def effects_model_ids(path):
    """Model ids in a model bundle (art/objects/effectsgeometry.bun): what gEffectsModelInventory holds."""
    ids = set()

    def walk(chs):
        for c in chs:
            if c.is_container:
                walk(c.children)
            elif c.type == 0x1B003 and len(c.data) >= 4:
                ids.add(struct.unpack_from('>I', c.data, 0)[0])
    if os.path.exists(path):
        walk(parse_chunks(open(path, 'rb').read()))
    return ids


def select_groups(scripts, patterns):
    names = []
    for pat in patterns:
        pat = pat.lower()
        hits = sorted(n for n in scripts if fnmatch.fnmatchcase(n, pat)) if any(c in pat for c in '*?[') \
            else ([pat] if pat in scripts else [])
        if not hits:
            sys.exit('no SMS group matches %r' % pat)
        names += [n for n in hits if n not in names]
    return names


def build(args):
    if args.face_joints:
        FACE_AXIS.update(face_axes(args.face_joints))
    templates = parse_templates(os.path.join(args.sms, 'templates.fx'))
    scripts = parse_scripts(os.path.join(args.sms, 'scripts.fx'))
    for path in args.extra or ():
        parse_extra(path, templates, scripts)
    patterns = list(args.groups)
    for p in args.groups_file or ():
        patterns += [l.split('#')[0].strip() for l in open(p) if l.split('#')[0].strip()]
    names = select_groups(scripts, patterns)

    resident = resident_textures(args.resident)
    sms_tex = read_ptlg(open(os.path.join(args.sms, 'effects.glt'), 'rb').read(), 0x20)
    model_ids = effects_model_ids(args.effects_geometry)
    shipped = {}
    notes = {}

    def tex_hash_of(t):
        if not t.texture:
            return 0xFFFFFFFF                    # LoadFromChunk substitutes global/white
        h = nl_lower_hash('effects/' + t.texture)
        if h in resident and not args.ship_all_textures:
            notes[t.texture] = (h, 'resident in Charged (%s)' % ', '.join(os.path.basename(p) for p in args.resident))
            return h
        blob = sms_tex.get(h)
        if blob is None:
            notes[t.texture] = (h, 'MISSING: in neither the resident bundles nor effects.glt')
            return h
        fmt = struct.unpack('>I', blob[4:8])[0]
        if fmt == 8 and not args.keep_ci8:
            blob, how = ci8_to_rgb5a3(blob), 'shipped, SMS CI8 -> RGB5A3'
        else:
            how = 'shipped as is (GX format %d)' % fmt
        shipped[h] = blob
        mips, _, = struct.unpack('>II', blob[:8])
        w, hh = struct.unpack('>HH', blob[14:18])
        notes[t.texture] = (h, '%s, %dx%d, %d mips, %d bytes' % (how, w, hh, mips, len(blob)))
        return h

    blocks = []
    for n in names:
        g = scripts[n]
        blocks.append(compile_group(g, templates, tex_hash_of, args))
        for s in g['specs']:
            t = templates[s.template]
            if t.model and nl_hash('effects/' + t.model) not in model_ids:
                print('warning: %s: mesh %r (%08x) is not in %s; Charged falls back to sprites' % (
                    t.name, t.model, nl_hash('effects/' + t.model), os.path.basename(args.effects_geometry)),
                      file=sys.stderr)
            if s.attach == 1:
                print('  %-38s %-34s joint %-10s %08x' % (n, s.template, s.joint, s.jointID))
            else:
                print('  %-38s %-34s %s' % (n, s.template, ['emitter', 'joint', 'object'][s.attach]))

    bun = write_chunks([Chunk(0x80000001, children=blocks)])
    tag = nl_lower_hash(os.path.basename(args.out_nonres).split('.')[0])   # header word the loader ignores
    nonres = write_chunks([Chunk(0x80000001, children=[Chunk(0x24100, build_ptlg(shipped, tag))])])
    open(args.out_bun, 'wb').write(bun)
    open(args.out_nonres, 'wb').write(zlib_with_size(nonres) if args.out_nonres.lower().endswith('.zlib')
                                      else nonres)
    for name, (h, how) in sorted(notes.items()):
        print('texture effects/%-30s %08x  %s' % (name, h, how))
    print('wrote %s (%d bytes, %d groups, %d templates), %s (%d textures, %d bytes inflated)' % (
        args.out_bun, len(bun), len(blocks), sum(len(b.children) - 4 for b in blocks), args.out_nonres,
        len(shipped), len(nonres)))
    check_bundle(args.out_bun, args.out_nonres)


def check_bundle(bun_path, nonres_path):
    """Walk the pair the way EffectsBundleManager::Load / fn_802EAF64 / glEndLoadTextureBundle do."""
    d = load_maybe_zlib(bun_path)
    top = parse_chunks(d)
    assert len(top) == 1 and top[0].id == 0x80000001 and struct.unpack_from('>I', d, 4)[0] == len(d) - 8
    for bd in top[0].children:
        assert bd.type == 0x80024000
        k = bd.children
        _, gh, nt, _, ng, _ = struct.unpack('>6I', k[0].data)
        assert k[0].type == 0x24001 and k[1].type == 0x24025 and len(k[1].data) >= 4 * nt
        assert k[2].type == 0x24026 and len(k[2].data) >= 4 * ng and ng == 1 and len(k) == 3 + nt + ng
        for t in k[3:3 + nt]:
            assert t.type == 0x80024002 and t.children[0].type == 0x24003 and len(t.children[0].data) == 0xDC
            assert len(t.children) == 9
            for pc in t.children[1:]:
                use, _, _, nk, _ = struct.unpack('>IffII', pc.children[0].data)
                assert pc.type == 0x80024004 and pc.children[0].type == 0x24005
                assert not use or (len(pc.children) == 2 and len(pc.children[1].data) == 20 * nk and nk >= 1)
        g = k[3 + nt]
        h, _, nspec, _, _, nuser, _ = struct.unpack('>7I', g.children[0].data)
        assert g.type == 0x80024020 and h == gh and len(g.children[1].data) == 0x58 * nspec
        assert len(g.children[2].data) == 8 * nuser and len(g.children) == 3 + nuser
        for i in range(nspec):
            assert struct.unpack_from('>I', g.children[1].data, 0x58 * i + 4)[0] < nt
    n = load_maybe_zlib(nonres_path)
    top = parse_chunks(n)
    assert len(top) == 1 and top[0].id == 0x80000001
    for c in top[0].children:
        assert c.type == 0x24100 and c.data[:4] == b'PTLG'
        cnt = struct.unpack_from('>I', c.data, 4)[0]
        hashes = [struct.unpack_from('>I', c.data, 16 + 16 * i)[0] for i in range(cnt)]
        assert hashes == sorted(hashes)
        for i in range(cnt):
            h, off, size, _ = struct.unpack_from('>IIII', c.data, 16 + 16 * i)
            assert (16 + 16 * cnt + off) % 32 == 0 and 16 + 16 * cnt + off + size <= len(c.data)
    print('check: %s and %s walk cleanly as Charged loads them' % (os.path.basename(bun_path),
                                                                    os.path.basename(nonres_path)))


# ----------------------------------------------------------------------------------------- reading
SPEC_FIELDS = [('hash', 'I'), ('template', 'I'), ('attach', 'I'), ('joint', 'I'), ('delay', 'f'),
               ('jointBinding', 'I'), ('jointVelocity', 'f'), ('infront', 'I'), ('ground', 'I'),
               ('light', 'I'), ('offset', 'f'), ('offsetx', 'f'), ('offsety', 'f'), ('offsetz', 'f'),
               ('terrain', 'I'), ('lingerStart', 'f'), ('lingerEnd', 'f'), ('layer', 'I'), ('axis', 'i'),
               ('pad0', 'I'), ('pad1', 'I'), ('pad2', 'I')]
TEMPLATE_FIELDS = [(0x00, 'hash', 'I'), (0x04, 'fountainLife', 'f'), (0x08, 'mass', '2f'),
                   (0x10, 'particleLife', '2f'), (0x18, 'inheritVelocity', '2f'), (0x20, 'acceleration', '2f'),
                   (0x28, 'startRotation', '2f'), (0x30, 'mirrorChance', 'f'), (0x34, 'emitter', 'B'),
                   (0x35, 'blend', 'B'), (0x36, 'billboard', 'B'), (0x37, 'flags', 'B'), (0x38, 'texture', 'I'),
                   (0x3C, 'frames', 'i'), (0x40, 'onEmitterDeath', 'I'), (0x44, 'onParticleCreation', 'I'),
                   (0x48, 'onParticleDeath', 'I'), (0x4C, 'fps', '2f'), (0x54, 'model', 'I')]
PROP_NAMES = ['number', 'size', 'sizeScale', 'spin', 'radius', 'velocity', 'angle', 'tilt']


def decode_template(c):
    b = c.children[0].data
    o = {}
    for off, name, fmt in TEMPLATE_FIELDS:
        v = struct.unpack_from('>' + fmt, b, off)
        o[name] = v if len(v) > 1 else v[0]
    o['colours'] = [b[0x78 + 4 * i:0x7C + 4 * i].hex() for i in range((len(b) - 0x78) // 4)]
    for name, pc in zip(PROP_NAMES, c.children[1:]):
        use, base, rng, nk, _ = struct.unpack('>IffII', pc.children[0].data)
        if use:
            kd = pc.children[1].data
            o[name] = ('curve', [struct.unpack_from('>5f', kd, i) for i in range(0, len(kd), 20)][:nk])
        else:
            o[name] = (base, rng)
    return o


def decode_block(bd):
    hdr = struct.unpack('>6I', bd.children[0].data)
    nt = hdr[2]
    tm = bd.children[3:3 + nt]
    g = bd.children[3 + nt]
    gd = struct.unpack('>7I', g.children[0].data)
    sp = g.children[1].data
    specs = []
    for i in range(0, len(sp), 0x58):
        v = struct.unpack_from('>IIIIfIfIIIf3fIffIi3I', sp, i)
        specs.append(dict(zip([n for n, _ in SPEC_FIELDS], v)))
    return {'hash': gd[0], 'numSpecs': gd[2], 'numUser': gd[5], 'templates': [decode_template(t) for t in tm],
            'specs': specs, 'raw_templates': tm, 'raw_specs': sp}


def charged_blocks(paths):
    out = {}
    for p in paths:
        for top in parse_chunks(load_maybe_zlib(p)):
            for bd in top.children or ():
                if bd.type == 0x80024000:
                    blk = decode_block(bd)
                    out.setdefault(blk['hash'], []).append((os.path.basename(p), blk, bd))
    return out


def _fmt(v):
    if isinstance(v, float):
        return '%g' % v
    if isinstance(v, int):
        return '%08x' % v if v > 0xFFFF else str(v)
    if isinstance(v, tuple):
        return '(' + ', '.join(_fmt(x) for x in v) + ')'
    return str(v)


def dump(args):
    names = {nl_lower_hash(n): n for n in args.groups}
    for h, lst in charged_blocks([args.bundle]).items():
        if names and h not in names:
            continue
        for fn, blk, _ in lst:
            print('group %08x %s: %d specs, %d user specs, %d templates' % (
                h, names.get(h, ''), blk['numSpecs'], blk['numUser'], len(blk['templates'])))
            for s in blk['specs']:
                print('  spec ' + ' '.join('%s=%s' % (k, _fmt(v)) for k, v in s.items()
                                          if v not in (0, 0.0) or k == 'template'))
            for t in blk['templates']:
                print('  template %08x' % t['hash'])
                for k, v in t.items():
                    if k != 'hash':
                        print('    %-18s %s' % (k, _fmt(v) if k != 'colours' else ' '.join(v)))


# ------------------------------------------------------------------------------------------ verify
def masked(ch):
    """A copy of a chunk tree with the fields the loader overwrites (pointers) or never reads (the
    numKeys/keys words of a constant property: stale memory in Charged's files) zeroed."""
    if ch.is_container:
        return Chunk(ch.id, children=[masked(c) for c in ch.children])
    d = bytearray(ch.data)
    t = ch.type
    if t == 0x24001:
        d[12:16] = d[20:24] = b'\0' * 4
    elif t in (0x24025, 0x24026):
        d[:] = b'\0' * len(d)
    elif t == 0x24003:
        d[0x58:0x78] = b'\0' * 0x20
    elif t == 0x24005:
        if struct.unpack_from('>I', d, 0)[0]:
            d[16:20] = b'\0' * 4
        else:
            d[12:20] = b'\0' * 8
    elif t == 0x24021:
        d[4:8] = d[16:20] = d[24:28] = b'\0' * 4
    return Chunk(ch.id, bytes(d))


def _bits(x):
    return struct.unpack('>i', struct.pack('>f', x))[0]


def ulp_close(a, b, n=64):
    """Two size properties equal but for float rounding (Charged's compiler computed a few of its
    cubics in another float order: <= n ulps per coefficient)."""
    if a[0] != 'curve' or b[0] != 'curve' or len(a[1]) != len(b[1]):
        return False
    return all(abs(_bits(x) - _bits(y)) <= n for ka, kb in zip(a[1], b[1]) for x, y in zip(ka, kb))


def verify(args):
    templates = parse_templates(os.path.join(args.sms, 'templates.fx'))
    scripts = parse_scripts(os.path.join(args.sms, 'scripts.fx'))
    files = sorted(os.path.join(args.charged, f) for f in os.listdir(args.charged)
                   if f.lower().endswith('.bun') and 'nonres' not in f.lower())
    charged = charged_blocks(files)
    args.compat = True
    want = set(n.lower() for n in args.groups)
    ref_templates = {}                       # hash -> (decoded, raw chunk), first occurrence
    for lst in charged.values():
        for fn, blk, raw in lst:
            for d, rc in zip(blk['templates'], blk['raw_templates']):
                ref_templates.setdefault(d['hash'], (d, rc, fn))

    def tex_hash_of(t):
        return nl_lower_hash('effects/' + t.texture) if t.texture else 0xFFFFFFFF

    tres = {}                                # hash -> (name, byte-identical, differing fields)
    groups = []
    for name, g in sorted(scripts.items()):
        if want and name not in want:
            continue
        h = nl_lower_hash(name)
        if h not in charged:
            continue
        try:
            block = compile_group(g, templates, tex_hash_of, args)
        except KeyError as e:
            print('skip %s: %s' % (name, e))
            continue
        mine = decode_block(block)
        for d, rc in zip(mine['templates'], mine['raw_templates']):
            if d['hash'] in tres or d['hash'] not in ref_templates:
                continue
            rd, rr, _ = ref_templates[d['hash']]
            same = write_chunks([masked(rc)]) == write_chunks([masked(rr)])
            diffs = [k for k in d if d[k] != rd[k]]
            if 'size' in diffs and ulp_close(d['size'], rd['size']):
                diffs[diffs.index('size')] = 'size~ulp'
            tres[d['hash']] = (next(s.template for s in g['specs'] if nl_lower_hash(s.template) == d['hash']),
                               same, diffs)
        fn, theirs, raw = charged[h][0]
        if write_chunks([masked(block)]) == write_chunks([masked(raw)]):
            groups.append((name, fn, 'identical', ''))
            continue
        if mine['numSpecs'] != theirs['numSpecs'] or len(mine['templates']) != len(theirs['templates']):
            groups.append((name, fn, 'reauthored', '%d specs/%d templates here, %d/%d in Charged' % (
                mine['numSpecs'], len(mine['templates']), theirs['numSpecs'], len(theirs['templates']))))
            continue
        why = []
        for i, (a, b) in enumerate(zip(mine['specs'], theirs['specs'])):
            why += ['spec%d.%s %s/%s' % (i, k, _fmt(a[k]), _fmt(b[k])) for k in a if a[k] != b[k]]
        for a, b in zip(mine['templates'], theirs['templates']):
            if a['hash'] != b['hash']:
                why.append('template %08x/%08x' % (a['hash'], b['hash']))
            elif a['hash'] in tres and not tres[a['hash']][1]:
                why.append('%s(%s)' % (tres[a['hash']][0], ','.join(tres[a['hash']][2]) or 'bytes'))
        groups.append((name, fn, 'differs', '; '.join(why)))

    ident = [v for v in tres.values() if v[1]]
    ulps = [v for v in tres.values() if v[2] == ['size~ulp']]
    print('templates (same name in both games): %d compiled, %d byte-identical to Charged\'s '
          '(load-time pointer words masked), %d more equal but for float rounding in the size cubic'
          % (len(tres), len(ident), len(ulps)))
    count = {}
    for name, same, diffs in tres.values():
        for k in diffs:
            count[k] = count.get(k, 0) + 1
    print('  others differ in: ' + ', '.join('%s %d' % kv for kv in sorted(count.items(), key=lambda x: -x[1])))
    for kind in ('identical', 'differs', 'reauthored'):
        print('groups %s: %d' % (kind, sum(1 for g in groups if g[2] == kind)))
    if args.verbose:
        for name, fn, kind, why in groups:
            print('  %-10s %-40s %-24s %s' % (kind, name, fn, why))
        for h, (name, same, diffs) in sorted(tres.items(), key=lambda x: x[1][0]):
            if not same:
                d, rd = None, ref_templates[h][0]
                print('  template %-40s differs: %s' % (name, ', '.join(diffs) or 'bytes only'))
    return tres, groups


def main():
    ap = argparse.ArgumentParser(description='SMS particle effects -> a Charged effects bundle pair.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build', help='compile SMS groups (names or fnmatch patterns) into a bundle pair')
    b.add_argument('--sms', required=True, help='directory with scripts.fx, templates.fx, effects.glt')
    b.add_argument('--out-bun', required=True)
    b.add_argument('--out-nonres', required=True, help='*.zlib: 4-byte BE size + zlib stream, as the disc')
    b.add_argument('--groups-file', action='append')
    b.add_argument('--extra', action='append', metavar='FX',
                   help="templates and groups of a pack's own, made of SMS's (see parse_extra); their groups "
                        "are built when named like any other")
    b.add_argument('--resident', action='append',
                   help="texture bundle(s) Charged keeps loaded (default: its art/effects/effectsNonRes.bun.zlib)")
    b.add_argument('--effects-geometry', default=os.path.join(CHARGED_FILES, 'objects', 'effectsgeometry.bun'))
    b.add_argument('--ship-all-textures', action='store_true', help='convert every texture, resident or not')
    b.add_argument('--keep-ci8', action='store_true', help='ship SMS CI8 textures as CI8 instead of RGB5A3')
    b.add_argument('--compat', action='store_true',
                   help="exactly what Charged's own compiler wrote for SMS sources (hemisphere -> sphere, "
                        "start angle only when the spin has a range, colour keys scaled to 0..50); "
                        "default: SMS's runtime semantics")
    b.add_argument('--face-joints', metavar='RLG', help='aim joint-attached effects along the joint\'s forward axis '
                   '(from this Charged model\'s bind pose): SMS cutscene effects, which Charged starts facing up')
    b.add_argument('groups', nargs='*')
    v = sub.add_parser('verify', help="compile every SMS group Charged also has and diff against Charged's")
    v.add_argument('--sms', required=True)
    v.add_argument('--charged', default=DEFAULT_CHARGED, help='Charged art/effects directory')
    v.add_argument('-v', '--verbose', action='store_true')
    v.add_argument('groups', nargs='*')
    d = sub.add_parser('dump', help='decode a Charged effects bundle')
    d.add_argument('bundle')
    d.add_argument('groups', nargs='*')
    args = ap.parse_args()
    if args.cmd == 'build':
        if not args.resident:
            args.resident = [os.path.join(DEFAULT_CHARGED, 'effectsnonres.bun.zlib')]
        build(args)
    elif args.cmd == 'verify':
        verify(args)
    else:
        dump(args)


if __name__ == '__main__':
    main()
