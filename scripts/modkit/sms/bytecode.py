#!/usr/bin/env python3
"""bytecode.py: list / disassemble Charged (and SMS-era) InterpreterCore .byte_code files.

    python3 bytecode.py <file.byte_code> [function-name-or-substring ...]

Header (ByteCodeHeader, 0x48 bytes, big-endian): signature, numFunctions, tweakDataSize,
globalDataSize, dataSegmentSize, codeSegmentSize, stringSegmentSize, numGlobals, firstFloatTweak,
firstBoolTweak, numVariables, numStringRefs, then 6 runtime pointers. Then the function table
(12 bytes each: nlStringHash(name), code offset, u16 frameSize, u8 numArgs, u8 flags), tweak data,
global init data, data segment (u32 constants), code segment (u16: opcode = ins>>11,
operand = ins & 0x7FF), string segment.
NIS trigger natives (NisPlayer::DoFunctionCall): 2 = Effect(frame, name, target, param),
3 = PlaySound(frame, slot, cue), 4 = RaiseEvent(frame, name, target), 10 = TimeDilation(frame, f),
0/1/5..9 = misc (crowd, rumble, dirt, stadium event).
"""
import struct, sys

NAMES = {0: 'push_data', 1: 'push_str_data', 2: 'push_imm', 3: 'push_str', 4: 'jnz_fwd', 5: 'jmp_fwd',
         6: 'jnz_back', 7: 'jmp_back', 8: 'native', 9: 'call', 10: 'ret', 11: 'push_local',
         12: 'pop_local', 13: 'op', 14: 'push_global', 15: 'pop_global', 16: 'sp_add', 17: 'dup_local',
         18: 'mov_local', 19: 'set_local', 20: 'push_local2', 21: 'push_local3'}
NIS_NATIVES = {0: 'Unk8', 1: 'Unk9', 2: 'Effect', 3: 'PlaySound', 4: 'RaiseEvent', 5: 'Dirt', 6: 'Crowd',
               7: 'Rumble', 8: 'StadiumEvent', 9: 'Unk10', 10: 'TimeDilation'}


def nl_string_hash(s):
    h = 0xFFFFFFFF
    for c in s.encode('latin1'):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


class ByteCode:
    def __init__(self, d):
        self.d = d
        h = struct.unpack_from('>12I', d, 0)
        (self.sig, self.nfunc, self.tweak_size, self.global_size, self.data_size, self.code_size,
         self.str_size, self.nglobals, _, _, _, self.nstrrefs) = h
        self.funcs = [struct.unpack_from('>IIHBB', d, 0x48 + 12 * i) for i in range(self.nfunc)]
        o = 0x48 + 12 * self.nfunc
        self.tweak_off = o; o += self.tweak_size
        self.global_off = o; o += self.global_size
        self.data_off = o; o += self.data_size
        self.code_off = o; o += self.code_size
        self.str_off = o
        self.data = struct.unpack_from('>%dI' % (self.data_size // 4), d, self.data_off)

    def string(self, off):
        e = self.d.index(b'\0', self.str_off + off)
        return self.d[self.str_off + off:e].decode('latin1')

    def find(self, name):
        h = nl_string_hash(name)
        for i, f in enumerate(self.funcs):
            if f[0] == h:
                return i
        return None

    def disasm(self, idx, natives=NIS_NATIVES):
        """Yield (pc, mnemonic, operand, comment) until the function's final ret."""
        h, off, frame, nargs, flags = self.funcs[idx]
        ends = sorted(set(f[1] for f in self.funcs) | {self.code_size})
        end = next(e for e in ends if e > off)
        pc = off
        stack = []
        out = []
        while pc < end:
            ins = struct.unpack_from('>H', self.d, self.code_off + pc)[0]
            op, arg = ins >> 11, ins & 0x7FF
            com = ''
            if op == 0:
                v = self.data[arg]
                f = struct.unpack('>f', struct.pack('>I', v))[0]
                com = '%d / %g' % (v, f)
                stack.append(('data', v, f))
            elif op == 1:
                s = self.string(self.data[arg]); com = repr(s); stack.append(('str', s))
            elif op == 2:
                com = str(arg); stack.append(('imm', arg))
            elif op == 3:
                s = self.string(arg); com = repr(s); stack.append(('str', s))
            elif op == 8:
                com = natives.get(arg, '?')
                stack = []
            elif op == 9:
                com = '%08x' % self.funcs[arg][0]
            out.append((pc, NAMES.get(op, '?%d' % op), arg, com))
            pc += 2
        return out

    def calls(self, idx):
        """Decode native calls of a straight-line trigger function as (native, args)."""
        res = []
        stack = []
        for pc, m, arg, com in self.disasm(idx):
            if m == 'push_data':
                v = self.data[arg]
                stack.append(struct.unpack('>f', struct.pack('>I', v))[0] if 0x3f000000 <= v <= 0x47000000 or v == 0 else v)
            elif m in ('push_str', 'push_str_data'):
                stack.append(eval(com))
            elif m == 'push_imm':
                stack.append(arg)
            elif m == 'native':
                res.append((NIS_NATIVES.get(arg, arg), list(stack)))
                stack = []
            elif m == 'call':
                res.append(('call', com, list(stack)))
                stack = []
        return res


def main(argv):
    bc = ByteCode(open(argv[1], 'rb').read())
    print('%s: %d functions, data %d, code %d, strings %d' % (argv[1], bc.nfunc, bc.data_size, bc.code_size, bc.str_size))
    for name in argv[2:]:
        i = bc.find(name)
        if i is None:
            print(name, ': no function')
            continue
        print(name, 'function #%d' % i, bc.funcs[i])
        for c in bc.calls(i):
            print('   ', c)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
