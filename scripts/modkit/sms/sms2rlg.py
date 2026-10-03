# sms2rlg.py: a Super Mario Strikers character model (.glg + .glt) as a Mario Strikers Charged one
# (.rlg + .rlt), rebound to a Charged skeleton.
#
#   sms2rlg.py <sms.glg> <sms.glt> <sms.shier> <charged.rlg> <charged.rlt> <charged.shier> <out.rlg> <out.rlt>
#
# The SMS model is rigidly skinned (each corner on one bone of its packet, through the stitching
# chunks); every vertex is taken from SMS's bind pose into the Charged skeleton's through its bone,
# bones Charged lacks folding into their nearest ancestor it has. The Charged model (a character of
# the same skeleton) is the template: its material (GXCharacterDamageMaterialProgram), stream layout,
# bone matrices and model id; its textures stay in the bundle for whatever else draws with them.
#
#   ... <out.rlg> <out.rlt> [model name, e.g. superteam/superteam] [own] [--sms-frames]
# 'own' bundles only the SMS textures. Toes 8/12, thumbs 19/35 and finger bases 22/38 (Charged
# indices) are rebound through Charged's bone frames (sanim.CHARGED_BIND_DELTA), so Charged's own
# Waluigi animations and sanim.py's converted ones drive them as SMS's animations drove SMS's mesh;
# --sms-frames keeps the old SMS-frame binding for those six bones.
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nlchunk import walk
from shier import load as load_shier
import smsglg
try:
    from sanim import CHARGED_BIND_DELTA   # Charged node index -> quaternion (x, y, z, w)
except ImportError:
    CHARGED_BIND_DELTA = None

def chunks(d):
    ch = {}
    for depth, rawid, idx, name, off, payload, size in walk(d, 0, len(d), 0, []):
        ch.setdefault(idx & 0xFFFFFF, []).append((payload, size))
    return ch

def mat_from(f):
    return [list(f[i*4:i*4+4]) for i in range(4)]
def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]
def mat_inv(m):
    n = 4
    a = [row[:] + [1.0 if i == j else 0.0 for j in range(n)] for i, row in enumerate(m)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(a[r][c]))
        a[c], a[p] = a[p], a[c]
        pv = a[c][c]
        a[c] = [x / pv for x in a[c]]
        for r in range(n):
            if r != c and a[r][c] != 0.0:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c])]
    return [row[n:] for row in a]
def xp(p, m):
    x, y, z = p
    return (x*m[0][0] + y*m[1][0] + z*m[2][0] + m[3][0], x*m[0][1] + y*m[1][1] + z*m[2][1] + m[3][1], x*m[0][2] + y*m[1][2] + z*m[2][2] + m[3][2])
def xd(p, m):
    x, y, z = p
    return (x*m[0][0] + y*m[1][0] + z*m[2][0], x*m[0][1] + y*m[1][1] + z*m[2][1], x*m[0][2] + y*m[1][2] + z*m[2][2])

def chunk(cid, payload):
    out = struct.pack('>II', cid, len(payload)) + payload
    return out + b'\0' * (-len(out) & 3)  # Charged's chunks start on 4 bytes

def read_textures(path, header):
    d = open(path, 'rb').read()
    n = struct.unpack('>I', d[4:8])[0]
    base = header + n * 16
    return [(tid, d[base+off:base+off+size]) for tid, off, size, z in (struct.unpack('>IIII', d[header+16*i:header+16*i+16]) for i in range(n))]

def nl_hash(s):
    h = 0xFFFFFFFF
    for c in s.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h

def convert(sglg, sglt, sshier, crlg, crlt, cshier, orlg, orlt, model_name=None, template_textures=True,
            delta=CHARGED_BIND_DELTA):
    """model_name: the model's id becomes nlStringHash(model_name) ("<char>/<char>", as the game's own),
    so it can't collide with the template's model in a match. template_textures: also bundle the
    template's textures (for a model replacing the template's in place)."""
    m = smsglg.read(sglg)
    d = m['data']; vo = m['vo']
    sname, shashes, sparents, _ = load_shier(sshier)
    cname, chashes, cparents, _ = load_shier(cshier)
    cset = set(chashes)
    sms_bind = {h: mat_from(f) for h, f in m['bones']}

    # Bones whose Charged frames differ from SMS's by a constant rotation: Charged's local keys are
    # DELTA * SMS's (left-multiplied quaternion, see sanim.CHARGED_BIND_DELTA). Rebind those bones'
    # vertices through the SMS bind pose expressed in Charged's frames (the bone's local bind
    # rotation with DELTA applied, parent and joint position unchanged), so the mesh follows
    # Charged-frame animations (Charged's own and sanim.py's converted ones) as SMS's did its own.
    bind_delta = {}
    if delta is not None:
        for ci, q in delta.items():
            if ci < len(chashes):
                bind_delta[chashes[ci]] = q
    sidx = {h: i for i, h in enumerate(shashes)}

    def delta_bind(src, h):
        i = sidx[h]
        par = sms_bind.get(shashes[sparents[i]]) if sparents[i] >= 0 else None
        if par is None:
            return src
        local = mat_mul(src, mat_inv(par))           # row-vector: world = local * parent
        x, y, z, w = bind_delta[h]
        dq = [[1 - 2*(y*y + z*z), 2*(x*y + z*w), 2*(x*z - y*w), 0.0],   # nlQuatToMatrix rows
              [2*(x*y - z*w), 1 - 2*(x*x + z*z), 2*(y*z + x*w), 0.0],
              [2*(x*z + y*w), 2*(y*z - x*w), 1 - 2*(x*x + y*y), 0.0],
              [0.0, 0.0, 0.0, 1.0]]
        rot = [row[:3] + [0.0] for row in local[:3]] + [[0.0, 0.0, 0.0, 1.0]]
        rot = mat_mul(rot, dq)                         # qmat(DELTA (x) q) = qmat(q) * qmat(DELTA)
        rot[3] = list(local[3])                        # joint offset unchanged
        return mat_mul(rot, par)

    # The Charged template.
    t = open(crlg, 'rb').read(); tch = chunks(t)
    tpo, tps = tch[0x1B004][0]; tso, tss = tch[0x1B005][0]; tpdo, tpds = tch[0x1B016][0]
    tmo, tms = tch[0x1B003][0]; tro, trs = tch[0x1B002][0]; tbo, tbs = tch[0x1B00A][0]
    tpk = struct.unpack('>IIHBBIIIIIIIII', t[tpo:tpo+0x30])
    tstreams = [struct.unpack('>IBBBB', t[tso+tpk[5]+s*8:tso+tpk[5]+s*8+8]) for s in range(tpk[4])]
    tparams = bytearray(t[tpdo+tpk[10]:tpdo+tpk[10]+104])
    model_id = nl_hash(model_name) if model_name else struct.unpack('>I', t[tmo:tmo+4])[0]
    charged_bind = {}
    for i in range(tbs // 0x44):
        h = struct.unpack('>I', t[tbo+i*0x44:tbo+i*0x44+4])[0]
        charged_bind[h] = mat_from(struct.unpack('>16f', t[tbo+i*0x44+4:tbo+i*0x44+68]))

    # Every SMS bone to the Charged bone that carries it: itself, or its nearest ancestor Charged has.
    fold = {}
    for i, h in enumerate(shashes):
        j = i
        while j >= 0 and shashes[j] not in cset:
            j = sparents[j]
        fold[h] = shashes[j] if j >= 0 else None
    rebind = {}
    for h in sms_bind:
        tgt = fold.get(h)
        if tgt is None or tgt not in charged_bind:
            continue
        src = sms_bind[h] if h == tgt or tgt not in sms_bind else sms_bind[tgt]
        if bind_delta and tgt in bind_delta and h == tgt:
            src = delta_bind(src, h)
        rebind[h] = (tgt, mat_mul(mat_inv(src), charged_bind[tgt]))

    def stream(pk, sid):
        for addr, i, stride in pk['streams']:
            if i == sid:
                return addr, stride
        return None

    diffuse = m['packets'][0]['diffuse']
    vertex = bytearray(); index = bytearray(); streams = bytearray(); packets = bytearray(); params = bytearray()
    maps = []
    for p, pk in enumerate(m['packets']):
        stitch = m['stitch'].get(p, b'')
        slots = {v: h for h, v in (m['maps'][p + 1] if p + 1 < len(m['maps']) else [])}
        pos = stream(pk, 0); nrm = stream(pk, 1); uv = stream(pk, 3)
        local = {}; corners = []; verts = []; bones = []
        for k, vi in enumerate(pk['idx']):
            bone = slots.get(stitch[k] if k < len(stitch) else 0)
            tgt, rm = rebind.get(bone, (None, None))
            key = (vi, tgt)
            if key not in local:
                a, st = pos
                if st == 12:
                    P = struct.unpack('>3f', d[vo+a+vi*12:vo+a+vi*12+12])
                else:
                    P = tuple(c / 256.0 for c in struct.unpack('>3h', d[vo+a+vi*6:vo+a+vi*6+6]))
                a, st = nrm
                if st == 12:
                    N = struct.unpack('>3f', d[vo+a+vi*12:vo+a+vi*12+12])
                else:
                    N = tuple(c / 64.0 for c in struct.unpack('>3b', d[vo+a+vi*3:vo+a+vi*3+3]))
                UV = d[vo+uv[0]+vi*4:vo+uv[0]+vi*4+4] if uv else b'\0\0\0\0'
                if rm is not None:
                    P = xp(P, rm); N = xd(N, rm)
                ln = (N[0]**2 + N[1]**2 + N[2]**2) ** 0.5 or 1.0
                N = tuple(c / ln for c in N)
                if tgt is not None and tgt not in bones:
                    bones.append(tgt)
                local[key] = len(verts)
                verts.append((P, N, UV, bones.index(tgt) if tgt is not None else 0))
            corners.append(local[key])
        maps.append(bones or [chashes[0]])
        # Vertex arrays, in the template's stream order: position, normal, six UV sets, bone indices, weights.
        addrs = []
        for t_addr, t_index, t_stride, t_id, t_unk in tstreams:
            while len(vertex) % 32:
                vertex.append(0)
            addrs.append(len(vertex))
            for P, N, UV, b in verts:
                if t_id == 1:
                    vertex += struct.pack('>3f', *P)
                elif t_id == 2:
                    vertex += struct.pack('>3f', *N)
                elif t_id == 4:
                    vertex += UV
                elif t_id == 7:
                    vertex += bytes([b, 0, 0, 0])
                elif t_id == 5:
                    vertex += struct.pack('>4f', 1.0, 0.0, 0.0, 0.0)
                else:
                    vertex += b'\0' * t_stride
        stoff = len(streams)
        for (t_addr, t_index, t_stride, t_id, t_unk), a in zip(tstreams, addrs):
            streams += struct.pack('>IBBBB', a, t_index, t_stride, t_id, t_unk)
        ib = len(index)
        for c in corners:
            index += struct.pack('>H', c)
        while len(index) % 4:
            index.append(0)
        poff = len(params)
        block = bytearray(tparams)
        for slot in range(6):  # diffuse, specular, mask, mega, damage 1, damage 2: the Super's art
            struct.pack_into('>I', block, slot * 8, diffuse)
        struct.pack_into('>f', block, 60, 0.0)  # specularAmount: the template's specular maps don't fit these UVs
        params += block
        packets += struct.pack('>IIHBBIIIIIIIII', ib, len(corners), len(verts), pk['prim'], len(tstreams), stoff,
                               tpk[6], tpk[7], tpk[8], tpk[9], poff, 0, 0, 0)
        print('packet %d: prim %d, %d corners, %d vertices, bones %s' % (p, pk['prim'], len(corners), len(verts), ['%08x' % b for b in bones]))

    skin = b''.join(chunk(0x1B00B, b''.join(struct.pack('>I', h) for h in mp)) for mp in maps)
    skin += chunk(0x1B00A, t[tbo:tbo+tbs])
    skin += chunk(0x1B00C, struct.pack('>III', 0, 16, len(maps)))  # no morphs
    body = chunk(0x1B016, bytes(params)) + chunk(0x1B007, bytes(index)) + chunk(0x1B006, bytes(vertex))
    body += chunk(0x1B005, bytes(streams)) + chunk(0x1B004, bytes(packets)) + chunk(0x1B002, t[tro:tro+trs])
    body += chunk(0x1B003, struct.pack('>III', model_id, len(maps), 0)) + chunk(0x8001B008, skin)
    open(orlg, 'wb').write(chunk(0x8001B000, body))

    # The bundle: the SMS textures, then the template's own (other models of the character draw with them).
    tex = read_textures(sglt, 0x20)
    have = {tid for tid, b in tex}
    if template_textures:
        tex += [(tid, b) for tid, b in read_textures(crlt, 0x10) if tid not in have]
    dict_bytes = bytearray(); data = bytearray()
    for tid, blob in tex:
        while len(data) % 32:
            data.append(0)
        dict_bytes += struct.pack('>IIII', tid, len(data), len(blob), 0)
        data += blob
    open(orlt, 'wb').write(b'PTLG' + struct.pack('>III', len(tex), 0, 0) + bytes(dict_bytes) + bytes(data))
    print('wrote %s (%d bytes), %s (%d textures)' % (os.path.basename(orlg), os.path.getsize(orlg), os.path.basename(orlt), len(tex)))

if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '--sms-frames']
    convert(*args[0:8], model_name=args[8] if len(args) > 8 else None,
            template_textures=len(args) <= 9 or args[9] != 'own',
            delta=None if '--sms-frames' in sys.argv else CHARGED_BIND_DELTA)
