#!/usr/bin/env python3
"""resbun.py: parse Mario Strikers Charged (Wii) audio banks (<NAME>.resbun + <NAME>.nlxwb).

Usage:
  resbun.py CHAR_WALUIGI_Sfx[.resbun]               dump cues -> voices -> sequences -> events -> waves
  resbun.py CHAR_WALUIGI_Sfx --wav OUTDIR           also decode every wave to 16-bit mono WAV
  resbun.py CHAR_WALUIGI_Sfx --names names.txt      extra candidate names (one per line) for hash lookup
  resbun.py --check DIR                             sanity-check every *.resbun/*.nlxwb pair in DIR

Format summary (all big-endian; see the report for decomp references):
  .resbun  = NL chunk tree. chunk = u32 id, u32 size, payload. id bit31 = container;
             bits 24..27 = payload alignment as a power of two (payload starts at the next
             multiple of 1<<n after the 8-byte header, and `size` counts that padding);
             the next chunk starts at header+8+size rounded up to 4.
    0x80000001 root
      0x80023000 SoundMap           0x23001 {u32 cueCount, u32 ptr, u32 ptr}
                                    0x23003 SoundCue[cueCount] {u32 key0(name hash), key1, key2, key3, u32 cueIndex}
      0x80023300 ResourceBundle     0x23301 header (64 B), 0x23302 cues(40 B), 0x23303 voices(44 B),
                                    0x23304 sequences(12 B), 0x23305 sound events(48 B),
                                    0x23306 hit-marker events(16 B), 0x23307 parameter events(24 B),
                                    cueCount x 0x23308 (cue entries, 20 B),
                                    voiceCount x (0x23309 sequence ptrs, 0x2330C rpc group indices),
                                    sequenceCount x 0x2330A (event defs, 8 B),
                                    soundEventCount x 0x2330B (choices, 8 B)
      0x03023703 SPSoundTable       (8-byte aligned) u32 n, SPSoundEntry[n] (28 B), SPADPCM[n] (46 B)
      0x80023200 Sources            0x23201 {u32 count, u32 ?, u8 isStreamBank}
                                    0x23202 AudioSourceInfo[count] (28 B): {u32 spIndex, u32, u32,
                                    u32 waveNameHash, u32 srcChannels (only read for stream banks),
                                    u32, u32 loaderPtr (filled at runtime)}
  .nlxwb   = raw DSP-ADPCM, waves packed back to back on 8-byte frame boundaries; SP entry
             addresses are nibble addresses relative to the file start (start = byte*2+2).
"""
import os, struct, sys, wave

# ---------------------------------------------------------------- hashing
def nl_hash(s):
    """nlStringHash (NL/nlString.cpp): h=0xFFFFFFFF; h = h*33 + byte. Case-sensitive."""
    h = 0xFFFFFFFF
    for c in s.encode('latin1'):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h

def nl_lower_hash(s):
    """nlStringLowerHash: same, bytes lower-cased first."""
    return nl_hash(s.lower())

# ---------------------------------------------------------------- chunks
class Chunk:
    __slots__ = ('raw_id', 'id', 'off', 'size', 'payload', 'end', 'children')
    def __init__(self, d, off):
        self.raw_id, self.size = struct.unpack_from('>II', d, off)
        self.id = self.raw_id & 0x80FFFFFF               # nlChunk::GetID
        self.off = off
        p = off + 8
        al = (self.raw_id & 0x0F000000) >> 24            # nlChunk::GetChunkAlignment
        if al:
            a = 1 << al
            p = (p + a - 1) & ~(a - 1)
        self.payload = p
        self.end = off + 8 + self.size                   # unaligned data + size
        self.children = []
        if self.raw_id & 0x80000000:
            self.children = walk(d, off + 8, self.end)
    @property
    def data_size(self):
        return self.end - self.payload
    def next_off(self):
        e = self.end
        return e + ((4 - (e & 3)) & 3)                   # nlChunk::GetNextChunk

def walk(d, off, end):
    out = []
    while off + 8 <= end:
        c = Chunk(d, off)
        out.append(c)
        off = c.next_off()
    return out

# ---------------------------------------------------------------- structs
def u32s(d, off, n):
    return list(struct.unpack_from('>%dI' % n, d, off))

def f32(d, off):
    return struct.unpack_from('>f', d, off)[0]

SP_TYPES = {0: 'ADPCM one-shot', 1: 'ADPCM looped', 2: 'PCM16 one-shot', 3: 'PCM16 looped',
            4: 'PCM8 one-shot', 5: 'PCM8 looped'}

def dsp_nibbles(nsamples):
    """Nibbles from the start of the first frame through the last sample (headers included)."""
    full, rem = divmod(nsamples, 14)
    return full * 16 + (rem + 2 if rem else 0)

def dsp_samples_from_addrs(sa, ea):
    """Sample count for an ADPCM wave with start nibble sa (=byte*2+2) and inclusive end ea."""
    base = sa - 2
    span = ea - base + 1
    full, rem = divmod(span, 16)
    return full * 14 + (rem - 2 if rem else 0)

class Bank:
    def __init__(self, base):
        if base.endswith('.resbun') or base.endswith('.nlxwb'):
            base = base.rsplit('.', 1)[0]
        self.base = base
        self.name = os.path.basename(base)
        self.d = open(base + '.resbun', 'rb').read()
        wb = base + '.nlxwb'
        self.wb = open(wb, 'rb').read() if os.path.exists(wb) else b''
        d = self.d
        root = Chunk(d, 0)
        assert root.id == 0x80000001, hex(root.raw_id)
        self.root = root
        self.top = {c.id: c for c in root.children}
        self._parse_soundmap()
        self._parse_bundle()
        self._parse_sptable()
        self._parse_sources()

    # 0x80023000 -- SoundMap::ParseChunk (src/Game/Audio/SoundMap.cpp:32)
    def _parse_soundmap(self):
        d = self.d
        c = self.top[0x80023000]
        hdr, cues = c.children[0], c.children[1]
        assert hdr.id == 0x23001 and cues.id == 0x23003
        n = u32s(d, hdr.payload, 1)[0]
        self.soundmap = []
        for i in range(n):
            k0, k1, k2, k3, idx = u32s(d, cues.payload + 20 * i, 5)
            self.soundmap.append(dict(key=(k0, k1, k2, k3), cue=idx))

    # 0x80023300 -- ParseAudioResourceBundle (src/Game/Audio/AudioResourceBundle.cpp:19)
    def _parse_bundle(self):
        d = self.d
        ch = self.top[0x80023300].children
        it = iter(ch)
        hdr = next(it); assert hdr.id == 0x23301
        h = u32s(d, hdr.payload, 16)
        self.header = dict(field_00=h[0], bank_hash=h[1], cueCount=h[2], cues_ptr=h[3],
                           voiceCount=h[4], voices_ptr=h[5], sequenceCount=h[6], sequences_ptr=h[7],
                           soundEventCount=h[8], soundEvents_ptr=h[9], hitMarkerCount=h[10],
                           hitMarkers_ptr=h[11], paramEventCount=h[12], paramEvents_ptr=h[13],
                           extra=h[14:16])
        H = self.header
        arrs = {}
        for cid in (0x23302, 0x23303, 0x23304, 0x23305, 0x23306, 0x23307):
            c = next(it); assert c.id == cid, (hex(c.id), hex(cid)); arrs[cid] = c
        self.cues = []
        for i in range(H['cueCount']):
            o = arrs[0x23302].payload + 40 * i
            name, vcount, vptr = u32s(d, o, 3)
            use_slider = d[o + 12]
            sel_mode, slider, selidx, active, maxcount, f24 = struct.unpack_from('>iIIIII', d, o + 16)
            self.cues.append(dict(name=name, voiceCount=vcount, useSlider=use_slider, selectionMode=sel_mode,
                                  sliderIndex=slider, selectedVoiceIndex=selidx, activeCount=active,
                                  maximumCount=maxcount, entries=[]))
        self.voices = []
        for i in range(H['voiceCount']):
            o = arrs[0x23303].payload + 44 * i
            name = u32s(d, o, 1)[0]
            vol, pitch = f32(d, o + 4), f32(d, o + 8)
            slider, seqcount, seqptr, rpccount, rpcptr, dyn, mods, modptr = u32s(d, o + 12, 8)
            self.voices.append(dict(name=name, volume=vol, pitch=pitch, categoryIndex=slider,
                                    sequenceCount=seqcount, rpcGroupCount=rpccount, sequences=[], rpcGroups=[]))
        self.sequences = []
        for i in range(H['sequenceCount']):
            o = arrs[0x23304].payload + 12 * i
            self.sequences.append(dict(volumeOffset=f32(d, o), eventCount=u32s(d, o + 4, 1)[0], events=[]))
        self.sound_events = []
        for i in range(H['soundEventCount']):
            o = arrs[0x23305].payload + 48 * i
            f0, loops, nchoice, f0c, chptr = u32s(d, o, 5)
            rp, rv = d[o + 20], d[o + 21]
            pmin, pmax, vmin, vmax, dmin, drange = struct.unpack_from('>6f', d, o + 24)
            self.sound_events.append(dict(field_00=f0, loopCount=loops, choiceCount=nchoice, field_0C=f0c,
                                          randomPitch=rp, randomVolume=rv, pitchMin=pmin, pitchMax=pmax,
                                          volumeMin=vmin, volumeMax=vmax, delayMin=dmin, delayRange=drange,
                                          choices=[]))
        self.hit_markers = [u32s(d, arrs[0x23306].payload + 16 * i, 4) for i in range(H['hitMarkerCount'])]
        self.param_events = [u32s(d, arrs[0x23307].payload + 24 * i, 6) for i in range(H['paramEventCount'])]
        # per-cue voice lists: pointer - header.voices_ptr = voice byte offset
        for cue in self.cues:
            c = next(it); assert c.id == 0x23308
            for j in range(cue['voiceCount']):
                vp, = u32s(d, c.payload + 20 * j, 1)
                mn, w = f32(d, c.payload + 20 * j + 4), f32(d, c.payload + 20 * j + 8)
                selcnt, = u32s(d, c.payload + 20 * j + 12, 1)
                elig = d[c.payload + 20 * j + 16]
                off = vp - H['voices_ptr']
                assert off % 44 == 0, off
                cue['entries'].append(dict(voice=off // 44, minimumValue=mn, weight=w,
                                           selectionCount=selcnt, eligible=elig))
        for v in self.voices:
            c = next(it); assert c.id == 0x23309
            for j in range(v['sequenceCount']):
                off = u32s(d, c.payload + 4 * j, 1)[0] - H['sequences_ptr']
                assert off % 12 == 0
                v['sequences'].append(off // 12)
            c = next(it); assert c.id == 0x2330C
            v['rpcGroups'] = u32s(d, c.payload, v['rpcGroupCount'])
        for s in self.sequences:
            c = next(it); assert c.id == 0x2330A
            for j in range(s['eventCount']):
                typ, ptr = struct.unpack_from('>iI', d, c.payload + 8 * j)
                if typ == 1:
                    off = ptr - H['soundEvents_ptr']; assert off % 48 == 0; ref = off // 48
                elif typ == 3:
                    off = ptr - H['hitMarkers_ptr']; assert off % 16 == 0; ref = off // 16
                elif typ == 2:
                    off = ptr - H['paramEvents_ptr']; assert off % 24 == 0; ref = off // 24
                else:
                    ref = None
                s['events'].append(dict(type=typ, ref=ref))
        for e in self.sound_events:
            c = next(it); assert c.id == 0x2330B
            for j in range(e['choiceCount']):
                idx, w = u32s(d, c.payload + 8 * j, 2)
                e['choices'].append(dict(source=idx, weight=w))
        rest = list(it)
        assert not rest, [hex(c.id) for c in rest]

    # 0x23703 -- SPSoundTable (revolution/sp.h); fixed up by SPInitSoundTable (src/RVL_SDK/sp/sp.c)
    def _parse_sptable(self):
        d = self.d
        c = self.top[0x23703]
        p = c.payload
        n = u32s(d, p, 1)[0]
        assert 4 + n * (28 + 46) == c.data_size, (n, c.data_size)
        self.sp = []
        for i in range(n):
            typ, rate, la, lea, ea, ca, adp = u32s(d, p + 4 + 28 * i, 7)
            a = struct.unpack_from('>16h4H3H', d, p + 4 + 28 * n + 46 * i)
            ent = dict(type=typ, sampleRate=rate, loopAddr=la, loopEndAddr=lea, endAddr=ea, currentAddr=ca,
                       adpcm_ptr=adp, coefs=list(a[0:16]), gain=a[16], ps=a[17], yn1=a[18], yn2=a[19],
                       lps=a[20], lyn1=a[21], lyn2=a[22])
            if typ in (0, 1):
                ent['byteOffset'] = (ca - 2) // 2
                ent['samples'] = dsp_samples_from_addrs(ca, ea)
                ent['byteLength'] = 8 * ((ent['samples'] + 13) // 14)
            self.sp.append(ent)

    # 0x80023200 -- AudioBankLoader::ParseChunk (src/Game/Audio/AudioBankLoader.cpp)
    def _parse_sources(self):
        d = self.d
        c = self.top[0x80023200]
        hdr, ent = c.children[0], c.children[1]
        assert hdr.id == 0x23201 and ent.id == 0x23202
        n, f4 = u32s(d, hdr.payload, 2)
        self.source_hdr = dict(count=n, field_04=f4, isStream=d[hdr.payload + 8])
        self.sources = []
        for i in range(n):
            v = u32s(d, ent.payload + 28 * i, 7)
            self.sources.append(dict(sp=v[0], f04=v[1], f08=v[2], name=v[3], channels=v[4], f14=v[5], loader=v[6]))

    # -------------------------------------------------------------- audio
    def decode(self, sp_index):
        """Decode one SP entry to a list of signed 16-bit samples (ADPCM types only)."""
        e = self.sp[sp_index]
        if e['type'] not in (0, 1):
            raise ValueError('not ADPCM')
        wb = self.wb
        coefs = e['coefs']
        h1 = e['yn1'] - 65536 if e['yn1'] >= 32768 else e['yn1']
        h2 = e['yn2'] - 65536 if e['yn2'] >= 32768 else e['yn2']
        out = []
        addr = e['currentAddr'] - 2                     # frame-header nibble of first frame
        end = e['endAddr']
        while addr <= end:
            bo = addr >> 1
            hdr = wb[bo] if bo < len(wb) else 0
            scale = 1 << (hdr & 0xF)
            ci = (hdr >> 4) & 7
            c1, c2 = coefs[ci * 2], coefs[ci * 2 + 1]
            for k in range(2, 16):
                a = addr + k
                if a > end:
                    break
                b = wb[a >> 1] if (a >> 1) < len(wb) else 0
                nib = (b >> 4) if (a & 1) == 0 else (b & 0xF)
                if nib >= 8:
                    nib -= 16
                s = ((nib * scale) << 11) + 1024 + c1 * h1 + c2 * h2
                s >>= 11
                s = 32767 if s > 32767 else (-32768 if s < -32768 else s)
                out.append(s)
                h2, h1 = h1, s
            addr += 16
        return out

    def write_wav(self, sp_index, path):
        pcm = self.decode(sp_index)
        with wave.open(path, 'wb') as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(self.sp[sp_index]['sampleRate'])
            w.writeframes(struct.pack('<%dh' % len(pcm), *pcm))
        return pcm

# ---------------------------------------------------------------- names
def load_names(extra_files=()):
    """Build hash -> name from known-good strings (both hash flavours)."""
    names = {}
    here = os.path.dirname(os.path.abspath(__file__))
    files = [os.path.join(here, 'charged_sfx_names.txt')] + list(extra_files)
    for f in files:
        if not os.path.exists(f):
            continue
        for line in open(f, encoding='latin1'):
            s = line.strip()
            if not s or s.startswith('#'):
                continue
            s = s.split()[0]
            names.setdefault(nl_hash(s), s)
            names.setdefault(nl_lower_hash(s), s.lower())
    return names

# ---------------------------------------------------------------- dump
def dump(bank, names=None, out=sys.stdout):
    names = names or {}
    nm = lambda h: names.get(h, '')
    H = bank.header
    p = lambda *a: print(*a, file=out)
    p('%s  resbun %d B  nlxwb %d B' % (bank.name, len(bank.d), len(bank.wb)))
    p('bundle header: bank_hash %08x (nlStringHash(%r)=%08x)  cues %d voices %d sequences %d soundEvents %d '
      'hitMarkers %d paramEvents %d' % (H['bank_hash'], bank.name, nl_hash(bank.name), H['cueCount'],
                                        H['voiceCount'], H['sequenceCount'], H['soundEventCount'],
                                        H['hitMarkerCount'], H['paramEventCount']))
    p('sources: %d (isStream=%d)   SP entries: %d' % (bank.source_hdr['count'], bank.source_hdr['isStream'],
                                                      len(bank.sp)))
    smap = {m['cue']: m['key'] for m in bank.soundmap}
    for ci, cue in enumerate(bank.cues):
        key = smap.get(ci)
        p('\nCUE %2d  %08x %s  mode=%d maxInst=%d slider=%s  key=%s' % (
            ci, cue['name'], nm(cue['name']), cue['selectionMode'], cue['maximumCount'],
            cue['sliderIndex'] if cue['useSlider'] else '-', '/'.join('%08x' % k for k in key) if key else '?'))
        for ent in cue['entries']:
            v = bank.voices[ent['voice']]
            p('  voice %2d  %08x %s  weight=%g vol=%gdB pitch=%g cat=%d rpc=%s' % (
                ent['voice'], v['name'], nm(v['name']), ent['weight'], v['volume'], v['pitch'],
                v['categoryIndex'], v['rpcGroups']))
            for si in v['sequences']:
                s = bank.sequences[si]
                p('    seq %2d  volOffset=%g' % (si, s['volumeOffset']))
                for ev in s['events']:
                    if ev['type'] != 1:
                        p('      event type %d ref %s' % (ev['type'], ev['ref']))
                        continue
                    e = bank.sound_events[ev['ref']]
                    extra = []
                    if e['delayMin'] or e['delayRange']:
                        extra.append('delay=%g+%g' % (e['delayMin'], e['delayRange']))
                    if e['randomPitch']:
                        extra.append('pitch[%g,%g]' % (e['pitchMin'], e['pitchMax']))
                    if e['randomVolume']:
                        extra.append('vol[%g,%g]' % (e['volumeMin'], e['volumeMax']))
                    p('      soundEvent %2d  loops=%s choices=%d %s' % (
                        ev['ref'], 'inf' if e['loopCount'] in (0xFFFF, 0xFFFFFFFF) else e['loopCount'], e['choiceCount'],
                        ' '.join(extra)))
                    for ch in e['choices']:
                        src = bank.sources[ch['source']]
                        sp = bank.sp[src['sp']]
                        p('        wave src %2d -> sp %2d  %08x %-28s w=%d  %5d Hz  %6d smp  %.3fs  '
                          'nlxwb[0x%05x:+0x%05x]  ps=%02x' % (
                              ch['source'], src['sp'], src['name'], nm(src['name']), ch['weight'],
                              sp['sampleRate'], sp.get('samples', 0),
                              sp.get('samples', 0) / float(sp['sampleRate']), sp.get('byteOffset', 0),
                              sp.get('byteLength', 0), sp['ps']))
    p('\nWAVES (SP table order)')
    for i, sp in enumerate(bank.sp):
        srcs = [j for j, s in enumerate(bank.sources) if s['sp'] == i]
        p('  sp %2d  type %d (%s)  %5d Hz  sa=%06x ea=%06x  byte 0x%05x len 0x%05x  %6d smp  ps=%02x yn=%d,%d '
          'loop=%06x/%06x lps=%04x  src=%s' % (
              i, sp['type'], SP_TYPES.get(sp['type'], '?'), sp['sampleRate'], sp['currentAddr'], sp['endAddr'],
              sp.get('byteOffset', 0), sp.get('byteLength', 0), sp.get('samples', 0), sp['ps'], sp['yn1'],
              sp['yn2'], sp['loopAddr'], sp['loopEndAddr'], sp['lps'], srcs))
        p('         coefs ' + ' '.join('%d' % c for c in sp['coefs']))

def check(bank):
    """Layout invariants observed in the shipped files; returns a list of problems."""
    probs = []
    wb = bank.wb
    prev_end = 0
    for i, sp in enumerate(bank.sp):
        if sp['type'] not in (0, 1):
            probs.append('sp %d type %d' % (i, sp['type']))
            continue
        bo = sp['byteOffset']
        if (sp['currentAddr'] - 2) % 16:
            probs.append('sp %d start not frame aligned' % i)
        if bo != prev_end:
            probs.append('sp %d starts 0x%x, previous ended 0x%x' % (i, bo, prev_end))
        prev_end = bo + sp['byteLength']
        if wb and bo < len(wb) and wb[bo] != (sp['ps'] & 0xFF):
            probs.append('sp %d ps %02x != first header %02x' % (i, sp['ps'], wb[bo]))
        if sp['yn1'] or sp['yn2'] or sp['gain'] or sp['loopAddr'] or sp['loopEndAddr'] or sp['adpcm_ptr']:
            probs.append('sp %d nonzero yn/gain/loop/ptr' % i)
    if bank.sp and wb:
        last = bank.sp[-1]
        if len(wb) != last['endAddr'] // 2 + 1:
            probs.append('nlxwb size %d != last endAddr/2+1 = %d' % (len(wb), last['endAddr'] // 2 + 1))
    for j, s in enumerate(bank.sources):
        if s['sp'] != j:
            probs.append('source %d -> sp %d' % (j, s['sp']))
    if len(bank.sources) != len(bank.sp):
        probs.append('sources %d != sp %d' % (len(bank.sources), len(bank.sp)))
    if bank.header['bank_hash'] != nl_hash(bank.name):
        probs.append('bank hash mismatch')
    for i, m in enumerate(bank.soundmap):
        c = bank.cues[m['cue']]
        if m['key'][0] != c['name'] or m['key'][1:] != (0, 0, 0) or m['cue'] != i:
            probs.append('soundmap %d mismatch' % i)
    return probs

def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('bank', nargs='?')
    ap.add_argument('--wav', metavar='OUTDIR')
    ap.add_argument('--names', action='append', default=[])
    ap.add_argument('--check', metavar='DIR')
    a = ap.parse_args(argv)
    names = load_names(a.names)
    if a.check:
        for f in sorted(os.listdir(a.check)):
            if f.endswith('.resbun'):
                try:
                    b = Bank(os.path.join(a.check, f))
                    pr = check(b)
                    print('%-28s cues %3d voices %3d waves %3d  %s' % (f, len(b.cues), len(b.voices), len(b.sp),
                                                                     'OK' if not pr else '; '.join(pr[:4])))
                except Exception as ex:
                    print('%-28s ERROR %r' % (f, ex))
        return
    if not a.bank:
        ap.error('bank required')
    b = Bank(a.bank)
    dump(b, names)
    pr = check(b)
    print('\ncheck:', 'OK' if not pr else pr)
    if a.wav:
        os.makedirs(a.wav, exist_ok=True)
        for i in range(len(b.sp)):
            src = next((s for s in b.sources if s['sp'] == i), None)
            label = names.get(src['name'], '%08x' % src['name']) if src else 'nosrc'
            path = os.path.join(a.wav, '%s_%02d_%s.wav' % (b.name, i, label))
            pcm = b.write_wav(i, path)
            peak = max(abs(x) for x in pcm) if pcm else 0
            rms = (sum(x * x for x in pcm) / len(pcm)) ** 0.5 if pcm else 0
            clip = sum(1 for x in pcm if x in (32767, -32768))
            print('wrote %s  %d smp  peak %d  rms %.0f  clipped %d' % (path, len(pcm), peak, rms, clip))

if __name__ == '__main__':
    main(sys.argv[1:])
