# nlchunk.py: walk an NL chunk tree (big-endian: u32 id, u32 size, payload; ids with a top-bit set hold child chunks, bits 24..30 give the payload alignment).
import struct, sys
NAMES = {0x1B001:'FILE_INFO',0x1B002:'REF_DATA',0x1B003:'MODELS',0x1B004:'PACKETS',0x1B005:'STREAMS',0x1B006:'DISPLAY_LIST/VERTEX_DATA',0x1B007:'INDEX_DATA',
         0x1B008:'SKIN',0x1B009:'SKIN_9',0x1B00A:'SKIN_BONE_MATRICES',0x1B00B:'SKIN_BONE_MAP_LIST',0x1B00C:'SKIN_MORPH',0x1B00D:'SKIN_SOFTWARE_VERTICES',
         0x1B00E:'SKIN_PAIRS',0x1B00F:'TEXTURE_ANIM/SKIN_F',0x1B010:'SKIN_STITCHING',0x1B011:'VERTEX_ANIM',0x1B012:'MATERIAL_LIST',0x1B016:'PACKET_DATA?',0x1B100:'BMD',0x1B200:'?200'}
def walk(d, off, end, depth, out, limit=None):
    while off + 8 <= end:
        cid, size = struct.unpack('>II', d[off:off+8])
        rawid = cid; idx = cid & ~0x7F000000 & 0x7FFFFFFF
        container = bool(cid & 0x80000000)
        align_bits = (cid >> 24) & 0x7F
        payload = off + 8
        if align_bits:
            a = 1 << align_bits
            payload = (payload + a - 1) & ~(a - 1)
        name = NAMES.get(idx & 0xFFFFFF, '')
        out.append((depth, rawid, idx & 0xFFFFFF, name, off, payload, size))
        if container:
            walk(d, off + 8, off + 8 + size, depth + 1, out)
        nxt = off + 8 + size
        # This game's files pack chunks back to back; Charged's start each on 4 bytes.
        if nxt & 3 and nxt + 8 <= end:
            cid2 = struct.unpack('>I', d[nxt:nxt+4])[0]
            if (cid2 & 0xFFFFFF) >> 12 != 0x1B:
                nxt = (nxt + 3) & ~3
        off = nxt
    return out
def dump(path, maxrows=80):
    d = open(path, 'rb').read()
    rows = walk(d, 0, len(d), 0, [])
    print(path.rsplit('/',1)[-1], len(d), 'bytes')
    for depth, rawid, idx, name, off, payload, size in rows[:maxrows]:
        print('  ' * depth + '%08x %-26s off %7x payload %7x size %7d' % (rawid, name, off, payload, size))
    return d, rows
if __name__ == '__main__':
    for p in sys.argv[1:]:
        dump(p)
