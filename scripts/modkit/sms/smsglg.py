# smsglg.py: read a Super Mario Strikers model (.glg): packets, streams, indices, rigid (stitched) skinning.
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nlchunk import walk

def chunks(d):
    ch = {}
    for depth, rawid, idx, name, off, payload, size in walk(d, 0, len(d), 0, []):
        ch.setdefault(idx & 0xFFFFFF, []).append((payload, size))
    return ch

def read(path):
    d = open(path, 'rb').read()
    ch = chunks(d)
    mo, ms = ch[0x1B003][0]
    npk, model_id, pad, off = struct.unpack('>IIII', d[mo:mo+16])
    po, ps = ch[0x1B004][0]; so, ss = ch[0x1B005][0]; vo, vs = ch[0x1B006][0]; io, isz = ch[0x1B007][0]
    packets = []
    for p in range(npk):
        b = po + p * 0x4A
        u0, ib, nidx, prim, ns, stoff = struct.unpack('>IIHBBI', d[b:b+0x10])
        state = d[b+0x10:b+0x46]
        diffuse = struct.unpack('>I', state[0x18:0x1C])[0]
        streams = [struct.unpack('>IBB', d[so+stoff+s*6:so+stoff+s*6+6]) for s in range(ns)]
        idx = struct.unpack('>%dH' % nidx, d[io+ib:io+ib+2*nidx])
        packets.append(dict(prim=prim, idx=idx, streams=streams, diffuse=diffuse, state=state, raw=d[b:b+0x4A]))
    bones = []
    for o, s in ch.get(0x1B00A, []):
        for i in range(s // 0x44):
            bid = struct.unpack('>I', d[o+i*0x44:o+i*0x44+4])[0]
            bones.append((bid, struct.unpack('>16f', d[o+i*0x44+4:o+i*0x44+68])))
    maps = []
    for o, s in ch.get(0x1B00B, []):
        maps.append([struct.unpack('>II', d[o+i*8:o+i*8+8]) for i in range(s // 8)])
    stitch = {}
    for o, s in ch.get(0x1B010, []):
        n, pk = struct.unpack('>ii', d[o:o+8])
        stitch[pk] = d[o+8:o+s]
    return dict(data=d, vo=vo, model_id=model_id, packets=packets, bones=bones, maps=maps, stitch=stitch, ch=ch)

if __name__ == '__main__':
    m = read(sys.argv[1])
    print('model %08x packets %d bones %d maps %d stitch %d' % (m['model_id'], len(m['packets']), len(m['bones']), len(m['maps']), len(m['stitch'])))
    for i, pk in enumerate(m['packets']):
        st = m['stitch'].get(i, b'')
        mp = m['maps'][i + 1] if i + 1 < len(m['maps']) else []
        print(i, 'prim', pk['prim'], 'idx', len(pk['idx']), 'maxidx', max(pk['idx']), 'streams', [(hex(a), sid, stride) for a, sid, stride in pk['streams']],
              'diffuse %08x' % pk['diffuse'], 'stitch', len(st), 'slots', sorted(set(st)), 'map', [(hex(h), v) for h, v in mp])
