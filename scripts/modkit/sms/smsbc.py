"""Disassemble SMS (GameCube) InterpreterCore .byte_code (header 0x24, 8-byte function entries)."""
import struct, sys
def nlh(s, lower=False):
    h = 0xFFFFFFFF
    for c in (s.lower() if lower else s).encode('latin1'):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h
class BC:
    def __init__(self, d):
        self.d = d
        self.sig, self.nf, self.dsz, self.csz, self.ssz = struct.unpack_from('>5I', d, 0)
        self.funcs = [struct.unpack_from('>II', d, 0x24 + 8*i) for i in range(self.nf)]
        o = 0x24 + 8*self.nf
        self.doff = o; self.coff = o + self.dsz; self.soff = self.coff + self.csz
        self.data = struct.unpack_from('>%dI' % (self.dsz//4), d, self.doff)
    def s(self, off):
        e = self.d.index(b'\0', self.soff + off); return self.d[self.soff+off:e].decode('latin1')
    def run(self, idx, natives=None):
        h, off = self.funcs[idx]
        offs = sorted(set(f[1] & ~1 for f in self.funcs) | {self.csz})
        end = next(e for e in offs if e > (off & ~1))
        pc = off & ~1; st = []; out = []
        while pc < end:
            ins = struct.unpack_from('>H', self.d, self.coff + pc)[0]
            hi, lo = ins & 0xC000, ins & 0x3FFF
            if hi == 0:
                v = self.data[lo]; f = struct.unpack('>f', struct.pack('>I', v))[0]
                st.append(('%g' % f) if 0x30000000 < v < 0x50000000 or 0xb0000000 < v < 0xd0000000 else '0x%x' % v)
            elif hi == 0x4000:
                st.append(repr(self.s(lo)))
            elif hi == 0x8000:
                out.append('native%d(%s)' % (lo, ', '.join(st))); st = []
            else:
                sub, a = (lo >> 8) & 0xFF, lo & 0xFF
                if sub == 9: st.append(str(a))
                elif sub == 0xA: out.append('call f%d %08x(%s)' % (a, self.funcs[a][0], ', '.join(st))); st = []
                elif sub == 2: out.append('ret'); 
                elif sub in (0, 1): pass
                else: out.append('op%x/%d' % (sub, a))
            pc += 2
        return out
if __name__ == '__main__':
    bc = BC(open(sys.argv[1], 'rb').read())
    names = {}
    for f in sys.argv[2:]:
        for l in open(f):
            l = l.strip()
            if l: names[nlh(l)] = l; names.setdefault(nlh(l, True), l + '(lower)')
    print('funcs', bc.nf, 'data', bc.dsz, 'code', bc.csz, 'str', bc.ssz)
    for i, (h, off) in enumerate(bc.funcs):
        print('f%d %08x %s @%x' % (i, h, names.get(h, '?'), off))
        for l in bc.run(i): print('    ' + l)
