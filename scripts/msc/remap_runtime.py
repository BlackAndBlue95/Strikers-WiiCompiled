#!/usr/bin/env python3
"""Re-key WiiCompiled's runtime from Mario Kart Wii (RMCP01) guest addresses to
Mario Strikers Charged (R4QE01). Run once on a pristine runtime/ tree:

    python3 scripts/msc/remap_runtime.py

Functions are matched by name (MKW MAP.txt -> MSC decomp symbols). SDK globals use
the hand-checked DATA table below (derived from both decomps' symbols.txt; see
scripts/msc/datamap.py). Hooks whose target doesn't exist in MSC are commented out
with an MSC-UNMAPPED marker; other unmapped uses are reported."""
import re, os, sys, collections

MKW_MAP = 'projects/mkwii/MAP.txt'
MSC_SYMS = 'scripts/msc/msc_R4QE01_symbols.txt'

# MKW data address -> MSC data address (base, span): addresses in [base, base+span)
# shift by the same delta.
DATA = [
    (0x801938FC, 1,     0x803A8B58),  # IOS_Open+4 (NAND_IOS_OpenBody)
    (0x80244DE0, 1,     0x804DB4E0),  # _ctors start
    (0x80244EA0, 1,     0x804DBBC0),  # _dtors start (ctor end)
    (0x8027F820, 1,     0x80537E00),  # axDspSlave (AX IRAM image)
    (0x8029CEB0, 1,     0x80554AD8),  # SC product area table
    (0x802F81A0, 1,     0x805A7360),  # __AXDSPTask
    (0x802F8200, 1,     0x805A73C0),  # __AXDramImage
    (0x80336278, 1,     0x80607F78),  # btm_cb
    (0x80343230, 1,     0x805BD750),  # WaitingQueue
    (0x803434E0, 1,     0x805BD960),  # dvdContexts
    (0x80343740, 1,     0x805BDB80),  # FifoObj
    (0x803437C0, 1,     0x805BDC00),  # gxData
    (0x80343DC0, 1,     0x805BE200),  # GPFifo
    (0x80343DE4, 1,     0x805BE224),  # CPUFifo
    (0x80344090, 0x700, 0x805BE248),  # DisplayListFifo / __savedGXdata / OldCPUFifo
    (0x80346D20, 1,     0x80619440),  # s_homeDir
    (0x80347130, 1,     0x805C4410),  # __OSErrorTable[OS_ERROR_FPE]
    (0x80347498, 1,     0x805D4ED8),  # DefaultThread
    (0x803477B0, 1,     0x805D51F0),  # RunQueue
    (0x803478B0, 1,     0x805D52F0),  # IdleContext
    (0x80350860, 0x58,  0x805D60A0),  # HorVer
    (0x80385AA8, 1,     0x806DFD00),  # __OSFpscrEnableBits
    (0x80385AE0, 1,     0x806DFD28),  # SwitchThreadCallback
    (0x80386298, 1,     0x806E3038),  # sRFLManager
    (0x80386448, 1,     0x806E24E8),  # __AI_init_flag
    (0x8038644C, 1,     0x806E24EC),  # __AID_Active
    (0x8038647C, 1,     0x806E251C),  # __CallbackStack
    (0x80386480, 1,     0x806E2520),  # __AID_Callback
    (0x80386608, 0x20,  0x806E2668),  # __DSP_init_flag .. __DSP_curr_task
    (0x80386664, 1,     0x806E26AC),  # PauseFlag
    (0x80386668, 1,     0x806E26B0),  # PausingFlag
    (0x80386670, 1,     0x806E26B8),  # Canceling
    (0x8038667C, 1,     0x806E26C4),  # FirstTimeInBootrom
    (0x803866A0, 1,     0x806E26E4),  # DVDInitialized
    (0x803866A8, 1,     0x806E26EC),  # Prepared
    (0x803866F0, 1,     0x806E2728),  # executing
    (0x80386720, 6,     0x806E2748),  # freeDvdContext / dvdContextsInited / DVDLowInitCalled
    (0x803867B0, 0x2C,  0x806E27D0),  # CPUFifoReady .. DrawDone (GX sbss)
    (0x80386848, 1,     0x806E2CB8),  # s_libState (NAND)
    (0x803868F8, 1,     0x806E2920),  # InterruptHandlerTable
    (0x80386918, 0xC,   0x806E2940),  # Reschedule / RunQueueHint / RunQueueBits
    (0x803869E0, 1,     0x806E2CA8),  # SCGetProductCode buf
    (0x80386B38, 1,     0x806E2B08),  # VI IsInitialized
    (0x80386BA0, 1,     0x806E2B74),  # NextBufAddr
    (0x80386BA8, 1,     0x806E2B78),  # CurrTvMode
    (0x80386BB4, 1,     0x806E2B84),  # PostCB
    (0x80386BB8, 1,     0x806E2B88),  # PreCB
    (0x80386BC0, 1,     0x806E2B90),  # retraceQueue
    (0x80386BE4, 1,     0x806E2BB4),  # retraceCount
    (0x803886C8, 1,     0x806E7378),  # __GXData
    (0x80399180, 1,     0x806F7BE8),  # default MEM1 arena lo
]

def load_msc():
    msc = {}
    for l in open(MSC_SYMS):
        m = re.match(r'(\S+) = \.(\S+):0x([0-9A-Fa-f]{8}); // type:(\S+)', l)
        if m: msc.setdefault(m[1], int(m[3], 16))
    return msc

msc = load_msc()
mkw = {}
for l in open(MKW_MAP):
    a, n = l.split(None, 1); mkw[int(a, 16)] = n.strip()

def cands(n):
    out = [n]
    if '::' in n:
        ns, rest = n.split('::', 1); rest = rest.replace('::', '')
        if ns == 'RVL': out.append(rest)
        out += [ns + rest, ns + '_' + rest]
        if rest.startswith('__'): out.append('__' + ns + rest[2:])
    return out

def resolve(a):
    for base, span, new in DATA:
        if base <= a < base + span: return 'data', new + (a - base), 'ok'
    n = mkw.get(a)
    if not n: return None, None, 'nomap'
    if re.search(r'_(caseD_|switch)', n): return n, None, 'caselabel'
    for c in cands(n):
        if c in msc: return n, msc[c], 'ok'
    return n, None, 'missing'

LIT = re.compile(r'(?<![0-9A-Za-z])(0x)?(8[01][0-9A-Fa-f]{6})(u?)(?![0-9A-Za-z])|\bfunc_(8[01][0-9A-Fa-f]{6})\b')

def in_scope(a):
    return 0x80004000 <= a < 0x81800000 and (a in mkw or any(b <= a < b + s for b, s, _ in DATA))

def main():
    report = collections.defaultdict(list)
    for root in ('runtime/src', 'runtime/include'):
        for dp, _, fs in os.walk(root):
            if 'third_party' in dp: continue
            for f in fs:
                p = os.path.join(dp, f)
                lines = open(p, errors='ignore').read().split('\n'); changed = False; i = 0
                while i < len(lines):
                    l = lines[i]; bad = []
                    def sub(m):
                        a = int(m[2] or m[4], 16)
                        if not in_scope(a): return m[0]
                        n, new, how = resolve(a)
                        if new is None: bad.append((a, n, how)); return m[0]
                        s = f'{new:08X}'
                        return ('func_' + s) if m[4] else (m[1] or '') + s + (m[3] or '')
                    nl = LIT.sub(sub, l)
                    if bad and re.search(r'PPC_NATIVE_OVERRIDE|REGISTER_NATIVE_FUNCTION|MKW_KNOWN_NATIVE_CPU_CALL', l) \
                            and not l.lstrip().startswith('#define'):
                        j = i; depth = 0
                        while j < len(lines):
                            depth += lines[j].count('(') - lines[j].count(')')
                            lines[j] = f'// MSC-UNMAPPED({bad[0][1]}) ' + lines[j]
                            if depth <= 0 or j - i > 8: break
                            j += 1
                        report['dropped_hook'].append(f'{p}:{i+1} {bad[0][1]}')
                        changed = True; i = j + 1; continue
                    for a, n, how in bad:
                        report['unmapped_use'].append(f'{p}:{i+1} {a:08X} {n} [{how}] :: {l.strip()[:100]}')
                    if nl != l: lines[i] = nl; changed = True
                    i += 1
                if changed: open(p, 'w').write('\n'.join(lines))
    for k, v in report.items(): print(k, len(v))
    open('scripts/msc/remap_report.txt', 'w').write('\n'.join(report['dropped_hook'] + [''] + report['unmapped_use']) + '\n')

if __name__ == '__main__':
    main()
