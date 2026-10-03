import struct, sys
class CISO:
    def __init__(s, path):
        s.f = open(path, 'rb'); h = s.f.read(0x8000)
        assert h[:4] == b'CISO', h[:4]
        s.bs = struct.unpack('<I', h[4:8])[0]; m = h[8:0x8000]
        s.idx = []; n = 0
        for b in m:
            s.idx.append(n if b else None); n += 1 if b else 0
    def read(s, off, size):
        out = bytearray()
        while size > 0:
            blk, bo = divmod(off, s.bs); take = min(size, s.bs - bo)
            i = s.idx[blk]
            if i is None: out += b'\0' * take
            else:
                s.f.seek(0x8000 + i * s.bs + bo); out += s.f.read(take)
            off += take; size -= take
        return bytes(out)
def files(d):
    fst_off, fst_size = struct.unpack('>II', d.read(0x424, 8))
    fst = d.read(fst_off, fst_size)
    n = struct.unpack('>I', fst[8:12])[0]; st = 12 * n
    def name(o):
        e = fst.index(b'\0', st + o); return fst[st + o:e].decode('latin1')
    out = []; stack = [(n, '')]
    i = 1
    while i < n:
        while stack and i >= stack[-1][0]: stack.pop()
        w, a, b = struct.unpack('>III', fst[12*i:12*i+12])
        isdir = w >> 24; nm = name(w & 0xFFFFFF)
        prefix = stack[-1][1] if stack else ''
        if isdir:
            stack.append((b, prefix + nm + '/'))
        else:
            out.append((prefix + nm, a, b))
        i += 1
    return out
if __name__ == '__main__':
    d = CISO(sys.argv[1])
    for p, o, s in files(d): print(s, p)
