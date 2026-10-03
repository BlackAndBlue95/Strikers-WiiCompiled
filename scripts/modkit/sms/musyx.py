#!/usr/bin/env python3
"""MusyX (Factor 5) GameCube sound bank reader and SFX extractor.

Reads a <name>.proj / .pool / .sdir / .samp set (big-endian GameCube layout, as
used by Super Mario Strikers' audio/data/sebring.*), resolves SFX ids through
their sound macros to the DSP-ADPCM samples they play, and writes 16-bit PCM
.wav files named by SFX name.

    python3 musyx.py                 # Super Team + robot Kritter (defaults below)
    python3 musyx.py --list-groups
    python3 musyx.py --group GRPChar_Mystery_SFX --out /tmp/x
    python3 musyx.py --render        # also write as-heard versions (pitch + layers)

File layouts (all big-endian, verified against sebring.* and the game's
MusyX runtime in main.dol):

proj: a chain of groups, each starting with a 0x28-byte header
    u32 nextGroupOff   (absolute; 0xFFFFFFFF ends the chain)
    u16 groupId        (the MusyX id the game passes to sndPushGroup)
    u16 type           (1 = SFX group, 0 = song group)
    u32 macroIdsOff, sampleIdsOff, tableIdsOff, keymapIdsOff, layerIdsOff
    u32 sfxTableOff    (song groups: page table)
    u32 drumTableOff, midiSetupsOff
  id lists: u16 ids, 0xFFFF-terminated; an id with bit 15 set starts a range
  (id & 0x7FFF) .. next u16 (inclusive).
  SFX table: u16 count, u16 pad, then count x 10-byte entries
    u16 sfxId, u16 objId (macro id), u8 maxVoices, u8 priority,
    u8 defVel, u8 defPan, u8 defKey, u8 vGroup  (MusyX FX_TAB)

pool: u32 macrosOff, tablesOff, keymapsOff, layersOff (0 = none; sebring.pool
  has macros only). Macros: chain of {u32 size, u16 macroId, u16 pad,
  (size-8)/8 commands}, ended by size 0xFFFFFFFF. Each command is two u32
  words p0, p1; opcode = p0 & 0x7F. Byte numbering below is b[0..3] = p0
  little-end first (b0 = opcode), b[4..7] = p1 little-end first.

sdir: 0x20-byte entries, ended by u16 0xFFFF (4 bytes FF)
    u16 sampleId, u16 pad, u32 sampOffset, u32 unk(0), u8 rootKey, u8 pad,
    u16 sampleRate, u32 (format<<24 | numSamples), u32 loopStart,
    u32 loopLength, u32 adpcmParamOff
  ADPCM params (at adpcmParamOff): u16 bytesPerFrame(8), u8 ps, u8 loopPs,
    s16 hist2, s16 hist1, s16 coefs[16]. Format 1 entries carry an extra
    6 bytes per frame after that (per-frame seek context); decoding is the
    same.

samp: raw DSP-ADPCM, 8-byte frames (header ps = pred<<4 | scale, 14 nibbles).
"""
import argparse
import copy
import json
import math
import os
import re
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH = os.getcwd()  # default inputs and outputs: sms/, smsdecomp/, out/ under the working folder
DEF_AUDIO = os.path.join(SCRATCH, 'sms', 'audio')
DEF_DECOMP = os.path.join(SCRATCH, 'smsdecomp')

# --------------------------------------------------------------------------
# proj


def _idlist(d, o):
    out = []
    if o == 0:
        return out
    while True:
        v = struct.unpack_from('>H', d, o)[0]
        o += 2
        if v == 0xFFFF:
            return out
        if v & 0x8000:
            e = struct.unpack_from('>H', d, o)[0]
            o += 2
            out.extend(range(v & 0x7FFF, e + 1))
        else:
            out.append(v)


class SfxEntry:
    __slots__ = ('sfx_id', 'obj_id', 'priority', 'max_voices', 'def_vel', 'def_pan', 'def_key', 'vgroup', 'group')

    def __init__(self, raw, group):
        # Field order checked in main.dol synthFXStart: macro=+2, +4 -> synthStartSound's
        # maxVoices arg, +5 -> priority, key=+8, vol=+6, pan=+7, +9 read too (vGroup).
        (self.sfx_id, self.obj_id, self.max_voices, self.priority,
         self.def_vel, self.def_pan, self.def_key, self.vgroup) = struct.unpack('>HHBBBBBB', raw)
        self.group = group


class Group:
    def __init__(self, d, off):
        (self.next, self.id, self.type, mo, so, to, ko, lo, self.sfx_off, self.drum_off,
         self.midi_off) = struct.unpack_from('>IHHIIIIIIII', d, off)
        self.off = off
        self.macros = _idlist(d, mo)
        self.samples = _idlist(d, so)
        self.tables = _idlist(d, to)
        self.keymaps = _idlist(d, ko)
        self.layers = _idlist(d, lo)
        self.sfx = []
        if self.type == 1:
            n = struct.unpack_from('>H', d, self.sfx_off)[0]
            for i in range(n):
                o = self.sfx_off + 4 + i * 10
                self.sfx.append(SfxEntry(d[o:o + 10], self.id))


def read_proj(path):
    d = open(path, 'rb').read()
    groups, off = [], 0
    while off + 4 <= len(d):
        if struct.unpack_from('>I', d, off)[0] == 0xFFFFFFFF:
            break
        g = Group(d, off)
        groups.append(g)
        off = g.next
    return groups

# --------------------------------------------------------------------------
# pool


def read_pool(path):
    d = open(path, 'rb').read()
    mac_off, tab_off, key_off, lay_off = struct.unpack_from('>IIII', d, 0)
    if tab_off or key_off or lay_off:
        print('note: pool has tables/keymaps/layers; only macros are parsed', file=sys.stderr)
    macros = {}
    o = mac_off
    while o and o + 4 <= len(d):
        size = struct.unpack_from('>I', d, o)[0]
        if size == 0xFFFFFFFF:
            break
        mid = struct.unpack_from('>H', d, o + 4)[0]
        macros[mid] = [struct.unpack_from('>II', d, o + 8 + i * 8) for i in range((size - 8) // 8)]
        o += size
    return macros

# --------------------------------------------------------------------------
# sdir / samp


class Sample:
    def __init__(self, raw, sdir):
        (self.id, _p, self.offset, self.unk, self.root_key, _p2, self.rate, fmt_n,
         self.loop_start, self.loop_len, self.param_off) = struct.unpack('>HHIIBBHIIII', raw)
        self.format = fmt_n >> 24
        self.num_samples = fmt_n & 0xFFFFFF
        p = sdir[self.param_off:self.param_off + 40]
        self.bytes_per_frame, self.ps, self.loop_ps, self.hist2, self.hist1 = struct.unpack_from('>HBBhh', p, 0)
        self.coefs = struct.unpack_from('>16h', p, 8)

    @property
    def duration(self):
        return self.num_samples / self.rate

    @property
    def nbytes(self):
        return (self.num_samples + 13) // 14 * 8


def read_sdir(path):
    d = open(path, 'rb').read()
    out, o = {}, 0
    while struct.unpack_from('>H', d, o)[0] != 0xFFFF:
        s = Sample(d[o:o + 32], d)
        out[s.id] = s
        o += 32
    return out


def decode_dsp(samp, s):
    """Nintendo DSP-ADPCM -> list of int16."""
    if s.format not in (0, 1):
        raise ValueError(f'sample {s.id:#x}: unsupported format {s.format}')
    data = samp[s.offset:s.offset + s.nbytes]
    if data[0] != s.ps:
        print(f'warning: sample {s.id:#x} first frame ps {data[0]:#x} != sdir ps {s.ps:#x}', file=sys.stderr)
    c = s.coefs
    h1, h2 = s.hist1, s.hist2
    out = []
    n = s.num_samples
    for f in range(0, len(data), 8):
        hdr = data[f]
        scale = 1 << (hdr & 0xF)
        pi = (hdr >> 4) & 7
        c1, c2 = c[pi * 2], c[pi * 2 + 1]
        for i in range(14):
            if len(out) >= n:
                return out
            b = data[f + 1 + (i >> 1)]
            nib = (b >> 4) if (i & 1) == 0 else (b & 0xF)
            if nib >= 8:
                nib -= 16
            v = ((nib * scale) << 11) + c1 * h1 + c2 * h2 + 1024 >> 11
            v = -32768 if v < -32768 else 32767 if v > 32767 else v
            out.append(v)
            h2, h1 = h1, v
    return out


def write_wav(path, pcm, rate, loop=None):
    """16-bit mono PCM. loop=(start, end_inclusive) adds a RIFF 'smpl' chunk."""
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(int(round(rate)))
        w.writeframes(struct.pack('<%dh' % len(pcm), *pcm))
    if loop:
        smpl = struct.pack('<9I', 0, 0, int(1e9 / rate), 60, 0, 0, 0, 1, 0)
        smpl += struct.pack('<6I', 0, 0, loop[0], loop[1], 0, 0)
        with open(path, 'r+b') as f:
            f.seek(0, 2)
            f.write(b'smpl' + struct.pack('<I', len(smpl)) + smpl)
            size = f.tell() - 8
            f.seek(4)
            f.write(struct.pack('<I', size))

# --------------------------------------------------------------------------
# sound macros


def cmd_bytes(p0, p1):
    return struct.pack('<II', p0, p1)


def s8(x):
    return x - 256 if x >= 128 else x


OPS = {
    0x00: 'End', 0x01: 'Stop', 0x02: 'SplitKey', 0x03: 'SplitVel', 0x04: 'WaitTicks', 0x05: 'Loop',
    0x06: 'Goto', 0x07: 'WaitMs', 0x08: 'PlayMacro', 0x09: 'SendKeyOff', 0x0A: 'SplitMod',
    0x0B: 'PianoPan', 0x0C: 'SetAdsr', 0x0D: 'ScaleVolume', 0x0E: 'Panning', 0x0F: 'Envelope',
    0x10: 'StartSample', 0x11: 'StopSample', 0x12: 'KeyOff', 0x13: 'SplitRnd', 0x14: 'FadeIn',
    0x15: 'Spanning', 0x16: 'SetAdsrCtrl', 0x17: 'RndNote', 0x18: 'AddNote', 0x19: 'SetNote',
    0x1A: 'LastNote', 0x1B: 'Portamento', 0x1C: 'Vibrato', 0x1D: 'PitchSweep1', 0x1E: 'PitchSweep2',
    0x1F: 'SetPitch', 0x20: 'SetPitchAdsr', 0x21: 'ScaleVolumeDLS', 0x22: 'Mod2Vibrange',
    0x23: 'SetupTremolo', 0x24: 'Return', 0x25: 'GoSub', 0x28: 'TrapEvent', 0x29: 'UntrapEvent',
    0x2A: 'SendMessage', 0x2B: 'GetMessage', 0x2C: 'GetVid', 0x30: 'AddAgeCount', 0x31: 'SetAgeCount',
    0x32: 'SendFlag', 0x33: 'PitchWheelR', 0x34: 'Op34', 0x35: 'Op35', 0x36: 'SetPriority',
    0x37: 'AddPriority', 0x38: 'AgeCntSpeed', 0x39: 'AgeCntVel', 0x40: 'VolSelect', 0x41: 'PanSelect',
    0x42: 'PitchWheelSelect', 0x43: 'ModWheelSelect', 0x44: 'PedalSelect', 0x45: 'PortamentoSelect',
    0x46: 'ReverbSelect', 0x47: 'SpanSelect', 0x48: 'DopplerSelect', 0x49: 'TremoloSelect',
    0x4A: 'PreASelect', 0x4B: 'PreBSelect', 0x4C: 'PostBSelect', 0x4D: 'AuxAFXSelect',
    0x4E: 'AuxBFXSelect', 0x50: 'SetupLFO', 0x58: 'ModeSelect', 0x59: 'SetKeygroup',
    0x5A: 'SRCmodeSelect', 0x5E: 'FilterSwitchSelect', 0x5F: 'FilterParameterSelect',
    0x60: 'AddVars', 0x61: 'SubVars', 0x62: 'MulVars', 0x63: 'DivVars', 0x64: 'AddIVars',
    0x65: 'SetVar', 0x70: 'IfEqual', 0x71: 'IfLess',
}
# Commands that only set up controller routing / voice ageing: hidden in summaries.
BOILERPLATE = {0x31, 0x33, 0x38, 0x42, 0x5E, 0x5F, 0x11, 0x00}


def describe(p0, p1):
    b = cmd_bytes(p0, p1)
    op = b[0] & 0x7F
    name = OPS.get(op, f'Op{op:02x}')
    u16 = lambda i: b[i] | b[i + 1] << 8
    if op == 0x10:
        mode = {0: 'abs', 1: 'vel-neg', 2: 'vel-pos'}.get(b[3], b[3])
        return f'StartSample sample={u16(1):#x} offset={struct.unpack_from("<I", b, 4)[0]} mode={mode}'
    if op in (0x04, 0x07):
        t = u16(6)
        return (f'{name} {"forever" if t == 0xFFFF else t} keyOff={b[1]} random={b[2]} '
                f'sampleEnd={b[3]} absolute={b[4]} msSwitch={b[5]}')
    if op == 0x08:
        return f'PlayMacro macro={u16(2):#x} step={u16(4)} addNote={s8(b[1])} prio={b[6]} maxVoices={b[7]}'
    if op in (0x02, 0x03, 0x13, 0x0A):
        what = {0x02: 'key>=', 0x03: 'vel>=', 0x13: 'rnd(0..255)>=', 0x0A: 'mod>='}[op]
        return f'{name} if {what}{b[1]} goto macro={u16(2):#x} step={u16(4)}'
    if op in (0x06, 0x25):
        return f'{name} macro={u16(2):#x} step={u16(4)}'
    if op == 0x18:
        return (f'AddNote {"orig" if b[3] else "cur"}Key{s8(b[1]):+d} detune={s8(b[2])}c '
                f'wait={u16(6)}{"ms" if b[5] else "ticks"}')
    if op == 0x19:
        return f'SetNote key={b[1] & 0x7F} detune={s8(b[2])}c wait={u16(6)}{"ms" if b[5] else "ticks"}'
    if op == 0x17:
        rng = f'key-{b[1]}..key+{b[3]}' if b[5] else f'{b[1]}..{b[3]}'
        det = 'random +-100c' if b[4] else f'{s8(b[2])}c'
        return f'RndNote {rng} detune={det}'
    if op == 0x0D:
        return (f'ScaleVolume vol={"orig" if b[5] else "cur"}*{b[1]}/127+{s8(b[2])} '
                f'curve={u16(3):#x}')
    if op in (0x0F, 0x14):
        return (f'{name} to {b[1]}/127 (+{s8(b[2])}) curve={u16(3):#x} '
                f'over {u16(6)}{"ms" if b[5] else "ticks"}')
    if op == 0x0E:
        return f'Panning pos={b[1]} time={u16(2)}ms width={s8(b[4])}'
    if op == 0x15:
        return f'Spanning pos={b[1]} time={u16(2)}ms width={s8(b[4])}'
    if op == 0x1C:
        return f'Vibrato depth={s8(b[1])}st{s8(b[2]):+d}c period={u16(6)}{"ms" if b[5] else "ticks"} modScale={b[3]}'
    if op == 0x1F:
        return f'SetPitch hz={p0 >> 8} fine={u16(4)}'
    if op == 0x30:
        return f'AddAgeCount {struct.unpack_from("<h", b, 2)[0]}'
    if op == 0x38:
        return f'AgeCntSpeed time={struct.unpack_from("<I", b, 4)[0]}'
    return f'{name} {b.hex()}'


class Voice:
    """One voice's static trace: the samples it starts and the pitch/volume at that point."""

    def __init__(self, macro, step, key, orig_key, vel):
        self.macro, self.step = macro, step
        self.key, self.orig_key, self.detune = key, orig_key, 0
        self.key_rand = (0, 0)       # semitone range around key
        self.detune_rand = 0         # +- cents
        self.vol = vel / 127.0       # linear, before ScaleVolume
        self.events = []             # human-readable command log
        self.starts = []             # dicts per StartSample
        self.layers = []             # Voice objects spawned by PlayMacro
        self.loops_until_keyoff = False


def trace(macros, macro, step, key, vel, depth=0, orig_key=None):
    """Follow a macro statically. Returns a list of alternative Voice paths
    (more than one only when the macro branches randomly / conditionally)."""
    if orig_key is None:
        orig_key = key
    paths = []

    def run(v, mid, st, stack, budget):
        cmds = macros.get(mid)
        if cmds is None:
            v.events.append(f'missing macro {mid:#x}')
            paths.append(v)
            return
        while budget > 0:
            budget -= 1
            if st >= len(cmds):
                break
            p0, p1 = cmds[st]
            b = cmd_bytes(p0, p1)
            op = b[0] & 0x7F
            u16 = lambda i: b[i] | b[i + 1] << 8
            if op not in BOILERPLATE:
                v.events.append(f'{mid:#x}[{st}] ' + describe(p0, p1))
            st += 1
            if op in (0x00, 0x01):
                break
            elif op == 0x24:            # Return
                if stack:
                    mid, st = stack.pop()
                    cmds = macros[mid]
                    continue
                break
            elif op == 0x06:            # Goto
                mid, st = u16(2), u16(4)
                cmds = macros.get(mid, [])
            elif op == 0x25:            # GoSub
                stack.append((mid, st))
                mid, st = u16(2), u16(4)
                cmds = macros.get(mid, [])
            elif op in (0x02, 0x03, 0x0A):
                # SplitKey / SplitVel / SplitMod: branch when value >= b1 (runtime does
                # `if (value < b1) continue;`). Deterministic here: SFX start with
                # key=defKey, vel=defVel and the mod wheel at 0.
                val = {0x02: v.key, 0x03: vel, 0x0A: 0}[op]
                if val >= b[1]:
                    mid, st = u16(2), u16(4)
                    cmds = macros.get(mid, [])
            elif op == 0x13:
                # SplitRnd: runtime branches when (sndRand() & 0xFF) >= b1, i.e. with
                # probability (256-b1)/256. Explore the taken side as a separate variant.
                alt = copy.deepcopy(v)
                alt.events.append(f'  (variant: branch taken, p={(256 - b[1]) / 256:.3f})')
                v.events.append(f'  (variant: branch not taken, p={b[1] / 256:.3f})')
                run(alt, u16(2), u16(4), list(stack), budget)
            elif op in (0x70, 0x71):
                v.events.append('  (variable compare not evaluated; fall-through followed)')
            elif op == 0x08:            # PlayMacro -> separate voice
                k = max(0, min(127, v.orig_key + s8(b[1])))
                sub = trace(macros, u16(2), u16(4), k, vel, depth + 1)
                v.layers.append(sub)
            elif op == 0x18:            # AddNote
                v.key = (v.orig_key if b[3] else v.key) + s8(b[1])
                v.key = max(0, min(127, v.key))
                v.detune = s8(b[2])
                v.key_rand = (0, 0)
            elif op == 0x19:            # SetNote
                v.key, v.detune, v.key_rand = b[1] & 0x7F, s8(b[2]), (0, 0)
            elif op == 0x17:            # RndNote
                if b[5]:
                    lo, hi = v.key - b[1], v.key + b[3]
                else:
                    lo, hi = sorted((b[1], b[3]))
                lo, hi = max(0, lo), min(127, hi)
                v.key = (lo + hi) / 2
                v.key_rand = (lo - v.key, hi - v.key)
                if b[4]:
                    v.detune, v.detune_rand = 0, 100
                else:
                    v.detune, v.detune_rand = s8(b[2]), 0
            elif op == 0x0D:            # ScaleVolume (orig flag ignored: same at start)
                v.vol = min(1.0, v.vol * b[1] / 127.0 + s8(b[2]) / 127.0)
            elif op == 0x10:            # StartSample
                v.starts.append({'sample': u16(1), 'key': v.key, 'detune': v.detune,
                                 'key_rand': list(v.key_rand), 'detune_rand': v.detune_rand,
                                 'vol': round(v.vol, 4), 'offset': struct.unpack_from('<I', b, 4)[0]})
            elif op in (0x04, 0x07):
                if b[1] and not b[3] and u16(6) == 0xFFFF:
                    v.loops_until_keyoff = True
        paths.append(v)

    run(Voice(macro, step, key, orig_key, vel), macro, step, [], 512)
    return paths

# --------------------------------------------------------------------------
# game-side tables from the decompilation


def read_defines(decomp):
    src = open(os.path.join(decomp, 'src/Game/Audio/SebringSoundDefines.cpp')).read()
    sfx, grp = {}, {}
    for m in re.finditer(r'\{\s*0x([0-9A-Fa-f]+),\s*"(\w+)"', src):
        (grp if m.group(2).startswith('GRP') else sfx)[m.group(2)] = int(m.group(1), 16)
    return sfx, grp


def read_soundprops(decomp, stem):
    path = os.path.join(decomp, 'src/Game/SoundProps', stem + 'soundproperties.cpp')
    src = open(path).read()
    rows = []
    for m in re.finditer(r'\{\s*"(\w+)",\s*"(\w+)",\s*([-\d.]+)f,\s*([-\d.]+)f,\s*([-\d.]+)f,\s*(-?\d+),\s*(-?\d+)\s*\}', src):
        rows.append({'event': m.group(1), 'sfx': m.group(2), 'volume': float(m.group(3)),
                     'delay': float(m.group(4)), 'reverb': float(m.group(5)),
                     'volume_group': int(m.group(6)), 'priority': int(m.group(7))})
    return rows

# --------------------------------------------------------------------------
# rendering (as-heard: pitch from the macro's key, layers mixed)


def resample(pcm, ratio):
    """Linear-interpolated resample so the result plays `ratio` times faster."""
    if abs(ratio - 1.0) < 1e-9:
        return [float(x) for x in pcm]
    n = int(len(pcm) / ratio)
    out = []
    last = len(pcm) - 1
    for i in range(n):
        x = i * ratio
        j = int(x)
        f = x - j
        a = pcm[j]
        bb = pcm[j + 1] if j < last else pcm[last]
        out.append(a + (bb - a) * f)
    return out


def flatten(paths):
    """All (start, depth) pairs of one alternative, walking first-path layers."""
    v = paths[0]
    out = [s for s in v.starts]
    for lay in v.layers:
        out += flatten(lay)
    return out

# --------------------------------------------------------------------------


class Bank:
    def __init__(self, audio_dir, stem='sebring'):
        p = lambda ext: os.path.join(audio_dir, f'{stem}.{ext}')
        self.groups = read_proj(p('proj'))
        self.macros = read_pool(p('pool'))
        self.samples = read_sdir(p('sdir'))
        self.samp = open(p('samp'), 'rb').read()
        self._pcm = {}
        self.sfx = {}
        for g in self.groups:
            for e in g.sfx:
                self.sfx[e.sfx_id] = e

    def group(self, gid):
        for g in self.groups:
            if g.id == gid:
                return g
        raise KeyError(gid)

    def pcm(self, sid):
        if sid not in self._pcm:
            self._pcm[sid] = decode_dsp(self.samp, self.samples[sid])
        return self._pcm[sid]

    def resolve(self, sfx_id):
        e = self.sfx[sfx_id]
        return e, trace(self.macros, e.obj_id, 0, e.def_key, e.def_vel)


def sample_info(bank, sid):
    s = bank.samples[sid]
    d = {'sample': sid, 'rate': s.rate, 'num_samples': s.num_samples,
         'duration_s': round(s.duration, 4), 'root_key': s.root_key, 'format': s.format}
    if s.loop_len:
        d['loop'] = [s.loop_start, s.loop_start + s.loop_len - 1]
    return d


def export(bank, names_by_id, sfx_ids, out_dir, render=False, label=''):
    os.makedirs(out_dir, exist_ok=True)
    if render:
        os.makedirs(os.path.join(out_dir, 'rendered'), exist_ok=True)
    manifest = []
    for sid in sfx_ids:
        name = names_by_id.get(sid, f'SFX_{sid:04x}')
        e, paths = bank.resolve(sid)
        variants = paths  # alternatives (random/conditional branches)
        rec = {'name': name, 'sfx_id': sid, 'group': e.group, 'macro': e.obj_id,
               'priority': e.priority, 'max_voices': e.max_voices, 'def_vel': e.def_vel,
               'def_pan': e.def_pan, 'def_key': e.def_key, 'vgroup': e.vgroup, 'variants': []}
        for vi, v in enumerate(variants):
            suffix = f'_{vi + 1}' if len(variants) > 1 else ''
            vrec = {'commands': v.events, 'loops_until_keyoff': v.loops_until_keyoff, 'files': []}
            starts = [(st, 0) for st in v.starts]
            for li, lay in enumerate(v.layers):
                starts += [(st, li + 1) for st in flatten(lay)]
            for st, layer in starts:
                s = bank.samples[st['sample']]
                fn = f'{name}{suffix}' + (f'_layer{layer}' if layer else '') + '.wav'
                loop = (s.loop_start, s.loop_start + s.loop_len - 1) if s.loop_len else None
                write_wav(os.path.join(out_dir, fn), bank.pcm(s.id), s.rate, loop)
                semis = st['key'] - s.root_key + st['detune'] / 100.0
                info = sample_info(bank, s.id)
                info.update({'file': fn, 'layer': layer, 'key': st['key'],
                             'pitch_semitones': round(semis, 3),
                             'pitch_ratio': round(2 ** (semis / 12), 5),
                             'random_semitones': st['key_rand'], 'random_cents': st['detune_rand'],
                             'macro_volume': st['vol']})
                vrec['files'].append(info)
            if render and starts:
                base_rate = bank.samples[starts[0][0]['sample']].rate
                mix = []
                for st, layer in starts:
                    s = bank.samples[st['sample']]
                    semis = st['key'] - s.root_key + st['detune'] / 100.0
                    ratio = 2 ** (semis / 12) * s.rate / base_rate
                    r = resample(bank.pcm(s.id), ratio)
                    g = st['vol'] / starts[0][0]['vol']
                    if len(r) > len(mix):
                        mix += [0.0] * (len(r) - len(mix))
                    for i, x in enumerate(r):
                        mix[i] += x * g
                peak = max(abs(x) for x in mix) if mix else 0
                gain = 32767.0 / peak if peak > 32767 else 1.0   # layered mixes can exceed full scale
                pcm = [max(-32768, min(32767, int(round(x * gain)))) for x in mix]
                fn = f'{name}{suffix}.wav'
                write_wav(os.path.join(out_dir, 'rendered', fn), pcm, base_rate)
                vrec['rendered'] = 'rendered/' + fn
                vrec['rendered_gain_db'] = round(20 * math.log10(gain), 2)
            rec['variants'].append(vrec)
        manifest.append(rec)
    with open(os.path.join(out_dir, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    return manifest


def stats(pcm):
    if not pcm:
        return 0, 0.0, 0.0
    peak = max(abs(x) for x in pcm)
    rms = math.sqrt(sum(x * x for x in pcm) / len(pcm))
    clip = sum(1 for x in pcm if x in (32767, -32768)) / len(pcm)
    return peak, rms, clip


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--audio', default=DEF_AUDIO, help='dir with sebring.proj/.pool/.sdir/.samp')
    ap.add_argument('--decomp', default=DEF_DECOMP, help='Super Mario Strikers decomp root')
    ap.add_argument('--out', help='output dir (default: <audio>/super and <audio>/robot)')
    ap.add_argument('--group', action='append', help='group name (GRP...) or MusyX id; repeatable')
    ap.add_argument('--list-groups', action='store_true')
    ap.add_argument('--render', action='store_true', help='also write pitch-applied, layer-mixed renders')
    ap.add_argument('--verbose', '-v', action='store_true')
    a = ap.parse_args()

    bank = Bank(a.audio)
    sfx_names, grp_names = read_defines(a.decomp)
    names_by_id = {v: k for k, v in sfx_names.items()}
    grp_by_id = {v: k for k, v in grp_names.items()}

    if a.list_groups:
        for g in bank.groups:
            print(f'group {g.id:#04x} {grp_by_id.get(g.id, "?"):30s} type={g.type} sfx={len(g.sfx):3d} '
                  f'macros={len(g.macros):3d} samples={len(g.samples):3d}')
        return

    jobs = []
    if a.group:
        ids = []
        for gname in a.group:
            gid = grp_names[gname] if gname in grp_names else int(gname, 0)
            ids += [e.sfx_id for e in bank.group(gid).sfx]
        jobs.append((a.out or os.path.join(a.audio, 'custom'), ids, 'custom'))
    else:
        # Super Team: every SFXCHAR_SUPER_* / SFXCHAR_MYST_* id. They live in two groups:
        # GRPChar_Mystery_SFX (0x2B, captain group) and GRPStad_Mystery_SFX (0x00, loaded
        # whenever either captain is the Super Team).
        super_ids = sorted(v for k, v in sfx_names.items()
                           if k.startswith('SFXCHAR_SUPER_') or k.startswith('SFXCHAR_MYST_'))
        jobs.append((a.out or os.path.join(a.audio, 'super'), super_ids, 'super'))
        # Generic SFX the Super Team's tables also point at (shared with other characters:
        # SFXCHAR_GEN_*, per-surface run/slide/jump, power-ups, ball) -> super/shared/
        shared = set()
        for stem in ('supergen', 'supergrass', 'supermetal', 'superconcrete', 'superrubber', 'superwood'):
            shared |= {r['sfx'] for r in read_soundprops(a.decomp, stem)}
        shared_ids = sorted(sfx_names[n] for n in shared if sfx_names[n] not in super_ids)
        jobs.append((os.path.join(a.out, 'shared') if a.out else os.path.join(a.audio, 'super', 'shared'),
                     shared_ids, 'super-shared'))
        robot = read_soundprops(a.decomp, 'critterrobot')
        robot_ids = sorted({sfx_names[r['sfx']] for r in robot})
        jobs.append((os.path.join(a.out, 'robot') if a.out else os.path.join(a.audio, 'robot'),
                     robot_ids, 'robot'))

    if not a.group:
        # CHARSFX_* event -> SFX tables (volume etc.) for the Super Team and the robot goalie
        ev = {stem: read_soundprops(a.decomp, stem) for stem in
              ('supergen', 'supergrass', 'supermetal', 'superconcrete', 'superrubber', 'superwood',
               'critterrobot')}
        for out_dir, _ids, _l in jobs:
            os.makedirs(out_dir, exist_ok=True)
            with open(os.path.join(out_dir, 'events.json'), 'w') as f:
                json.dump(ev, f, indent=1)

    for out_dir, ids, label in jobs:
        man = export(bank, names_by_id, ids, out_dir, a.render, label)
        print(f'== {label}: {len(man)} SFX -> {out_dir}')
        for rec in man:
            grp = grp_by_id.get(rec['group'], hex(rec['group']))
            print(f"{rec['sfx_id']:#05x} {rec['name']:40s} {grp:20s} macro={rec['macro']:#x} "
                  f"vel={rec['def_vel']} key={rec['def_key']}")
            for v in rec['variants']:
                for f in v['files']:
                    rnd = ''
                    if f['random_semitones'] != [0, 0] or f['random_cents']:
                        rnd = f" rnd={f['random_semitones']}st+-{f['random_cents']}c"
                    lp = f" loop={f['loop']}" if 'loop' in f else ''
                    print(f"      {f['file']:52s} smp={f['sample']:#05x} {f['rate']}Hz "
                          f"{f['duration_s']:.3f}s pitch={f['pitch_semitones']:+g}st{rnd} "
                          f"vol={f['macro_volume']}{lp}")
                if a.verbose:
                    for c in v['commands']:
                        print('         ', c)


if __name__ == '__main__':
    main()
