#!/usr/bin/env python3
"""superbank.py: the Super Team's voice from Super Mario Strikers as a Mario Strikers Charged
character sound bank (<name>.resbun + <name>.nlxwb).

  superbank.py <sms audio dir> <super manifest.json> <out dir> [bank name]

SMS's samples are DSP-ADPCM already, so each wave is SMS's frames and coefficients as they are.
The bank answers both the captain and the sidekick cue of each event (a Super can be either in a
captain-only team).
"""
import json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from musyx import read_sdir
from resbun import nl_hash

# Each cue: (hashes it answers, [voice]); voice: (volume dB, category, [sequence]);
# sequence: (delay s, pitch semitones, [SFX names, one picked at random]).
VOICE, OTHER = 3, 4  # category sliders: vocal efforts, the rest (as Waluigi's bank)
CUES = [
    ((0xBD8AB76A,), [(0.0, OTHER, [(0, 0, ['SFXCHAR_SUPER_CALL_Wo_01', 'SFXCHAR_SUPER_CALL_Hey_02'])])]),  # power-up got
    ((0x1313EE94,), [(-96.0, OTHER, [(0, 0, ['SFXCHAR_SUPER_CALL_Wo_01'])])]),  # power-up used (muted, as every captain)
    ((0x8A9FCF66,), [(0.0, OTHER, [(0, -3, ['SFXCHAR_MYST_NIS_Rocket_Boots_01'])])]),  # CHAR_CAPTAIN_TankOn
    ((0xA91D4914, 0xDEA5F49B), [(0.0, OTHER, [(0, 0, ['SFXCHAR_SUPER_Deek_Left', 'SFXCHAR_SUPER_Deek_Right'])])]),  # deke
    ((0x5BF8E132,), [(0.0, OTHER, [(0, 0, ['SFXCHAR_SUPER_EFFORTS_Attack_01', 'SFXCHAR_SUPER_EFFORTS_Attack_02',
                                            'SFXCHAR_SUPER_EFFORTS_Attack_03'])])]),  # CHAR_CAPTAIN_DekeImpact
    ((0x270203ED, 0x2E95FBD4), [(-9.0, VOICE, [(0, 0, ['SFXCHAR_SUPER_CALL_Hey_01', 'SFXCHAR_SUPER_CALL_Hey_02',
                                                        'SFXCHAR_SUPER_CALL_Wo_01', 'SFXCHAR_SUPER_CALL_Wo_02',
                                                        'SFXCHAR_SUPER_EFFORTS_Attack_02'])])]),  # voice preview
    ((0x1602CA52, 0xBADF0EF9), [(-9.0, VOICE, [(0, 0, ['SFXCHAR_SUPER_EFFORTS_Electrocute_01',
                                                        'SFXCHAR_SUPER_EFFORTS_Electrocute_02',
                                                        'SFXCHAR_SUPER_EFFORTS_Electrocute_03'])])]),  # electrocuted
    ((0xFDE0C69B, 0x1CEC5A02), [(-9.0, VOICE, [(0, 0, ['SFXCHAR_SUPER_EFFORTS_Kick_01', 'SFXCHAR_SUPER_EFFORTS_Kick_02',
                                                        'SFXCHAR_SUPER_EFFORTS_Kick_03'])])]),  # shot
    ((0xBD539FB8, 0xBDD19FFF), [(-9.0, VOICE, [(0, 0, ['SFXCHAR_SUPER_EFFORTS_Pain_02', 'SFXCHAR_SUPER_EFFORTS_Pain_03',
                                                        'SFXCHAR_SUPER_EFFORTS_Hit_03'])])]),  # hit by a sidekick
    ((0x3642C41B, 0x00E606A2), [(-9.0, VOICE, [(0, 0, ['SFXCHAR_SUPER_EFFORTS_Hit_01', 'SFXCHAR_SUPER_EFFORTS_Hit_02',
                                                        'SFXCHAR_SUPER_EFFORTS_Pain_01'])])]),  # hit by a captain
    ((0xFDC268FB, 0x1CCDFC62), [(-9.0, VOICE, [(0, 0, ['SFXCHAR_SUPER_EFFORTS_Dazed'])])]),  # dazed
    ((0xFDEC8E0F, 0x1CF82176), [(-6.0, VOICE, [(0, 0, ['SFXCHAR_SUPER_EFFORTS_Pain_01', 'SFXCHAR_SUPER_EFFORTS_Hit_01'])])]),  # knocked flying
    ((0x790F135F,), [(0.0, OTHER, [(0, 0, ['SFXCHAR_SUPER_EFFORTS_Kick_Super_01']),
                                   (0, -3, ['SFXCHAR_MYST_NIS_Energy_Blast_01'])])]),  # megastrike
    ((0xD73B11EC,), [(0.0, VOICE, [(0, 0, ['SFXCHAR_SUPER_EFFORTS_Dazed']), (0.8, 0, ['SFXCHAR_SUPER_CALL_Wo_02'])]),
                     (0.0, VOICE, [(0, 0, ['SFXCHAR_SUPER_CALL_Hey_02'])]),
                     (0.0, VOICE, [(0, 0, ['SFXCHAR_SUPER_EFFORTS_Dazed'])])]),  # CHAR_CAPTAIN_WarioReact (confused)
]

def ser(node, pos):
    """An NL chunk (id, bytes or [children]) placed at file offset pos."""
    cid, body = node
    al = (cid >> 24) & 0xF
    pad = (-(pos + 8)) % (1 << al) if al else 0
    if isinstance(body, list):
        data = b''
        start = pos + 8 + pad
        for i, child in enumerate(body):
            data += ser(child, start + len(data))
            if i < len(body) - 1:
                data += b'\0' * (-(start + len(data)) % 4)
    else:
        data = body
    return struct.pack('>II', cid, pad + len(data)) + b'\0' * pad + data

def build(audio, manifest, out_dir, name='CHAR_WALUIGI_Sfx'):
    sdir = read_sdir(os.path.join(audio, 'sebring.sdir'))
    samp = open(os.path.join(audio, 'sebring.samp'), 'rb').read()
    sample_of = {e['name']: e['variants'][0]['files'][0]['sample'] for e in json.load(open(manifest))}

    waves = []  # SMS sample ids, in bank order
    def wave(sfx):
        sid = sample_of[sfx]
        if sid not in waves:
            waves.append(sid)
        return waves.index(sid)

    # Flatten the cues (one per hash) into the bundle's arrays.
    cues, voices, seqs, events = [], [], [], []
    for hashes, cue_voices in CUES:
        for h in hashes:
            first = len(voices)
            for vol, cat, sequences in cue_voices:
                seq_ids = []
                for delay, pitch, choices in sequences:
                    events.append((delay, pitch, [wave(c) for c in choices]))
                    seqs.append(len(events) - 1)
                    seq_ids.append(len(seqs) - 1)
                voices.append((vol, cat, seq_ids))
            cues.append((h, list(range(first, len(voices)))))

    VB, SB, EB, CB, HB, PB = 0x00100000, 0x00200000, 0x00300000, 0x00400000, 0x00500000, 0x00600000  # pointer bases
    header = struct.pack('>16I', 0, nl_hash(name), len(cues), CB, len(voices), VB, len(seqs), SB,
                         len(events), EB, 0, HB, 0, PB, 0, 0)
    cue_arr = b''.join(struct.pack('>IIIBxxxiIIIII', h, len(v), 0, 0, 3, 0xFF, 0xFFFF, 0, 255, 0) for h, v in cues)
    voice_arr = b''.join(struct.pack('>IffIIIII12x', nl_hash('%s_%d' % (name, i)), vol, 0.0, cat, len(s), 0, 0, 0)
                         for i, (vol, cat, s) in enumerate(voices))
    seq_arr = b''.join(struct.pack('>fII', 0.0, 1, 0) for _ in seqs)
    ev_arr = b''.join(struct.pack('>IIIIIBB2xffffff', 0x1001FBBC, 1, len(ch), 0, 0, 1 if pitch else 0, 0,
                                  float(pitch), float(pitch), 0.0, 0.0, float(delay), 0.0)
                      for delay, pitch, ch in events)
    bundle = [(0x23301, header), (0x23302, cue_arr), (0x23303, voice_arr), (0x23304, seq_arr), (0x23305, ev_arr),
              (0x23306, b''), (0x23307, b'')]
    bundle += [(0x23308, b''.join(struct.pack('>Iff I B3x'.replace(' ', ''), VB + v * 44, 0.0, 255.0, 0, 1) for v in vs))
               for h, vs in cues]
    for vol, cat, s in voices:
        bundle += [(0x23309, b''.join(struct.pack('>I', SB + q * 12) for q in s)), (0x2330C, b'')]
    bundle += [(0x2330A, struct.pack('>iI', 1, EB + e * 48)) for e in seqs]
    bundle += [(0x2330B, b''.join(struct.pack('>II', w, 255) for w in ch)) for delay, pitch, ch in events]
    soundmap = [(0x23001, struct.pack('>III', len(cues), 0, 0)),
                (0x23003, b''.join(struct.pack('>IIIII', h, 0, 0, 0, i) for i, (h, v) in enumerate(cues)))]

    # The waves: SMS's ADPCM frames back to back, and the SDK sound table pointing at them.
    wb = bytearray(); entries = b''; adpcm = b''; sources = b''
    for i, sid in enumerate(waves):
        s = sdir[sid]
        assert s.format == 0 and s.loop_len == 0, (hex(sid), s.format, s.loop_len)
        frames = samp[s.offset:s.offset + s.nbytes]
        assert frames[0] == s.ps
        off = len(wb)
        wb += frames
        n = s.num_samples
        nib = n // 14 * 16 + (n % 14 + 2 if n % 14 else 0)
        entries += struct.pack('>7I', 0, s.rate, 0, 0, off * 2 + nib - 1, off * 2 + 2, 0)
        adpcm += struct.pack('>16h4H3H', *s.coefs, 0, s.ps, 0, 0, 0, 0, 0)
        sources += struct.pack('>7I', i, 0, 0, nl_hash('%s_wave_%d' % (name, i)), 1, 0, 0)
    last_end = struct.unpack('>I', entries[-28 + 16:-28 + 20])[0]
    wb = bytes(wb[:last_end // 2 + 1])

    root = (0x80000001, [(0x80023000, soundmap), (0x80023300, bundle),
                         (0x03023703, struct.pack('>I', len(waves)) + entries + adpcm),
                         (0x80023200, [(0x23201, struct.pack('>III', len(waves), 0, 0)), (0x23202, sources)])])
    os.makedirs(out_dir, exist_ok=True)
    open(os.path.join(out_dir, name + '.resbun'), 'wb').write(ser(root, 0))
    open(os.path.join(out_dir, name + '.nlxwb'), 'wb').write(wb)
    print('%s: %d cues, %d voices, %d waves, %d + %d bytes' % (name, len(cues), len(voices), len(waves),
                                                               os.path.getsize(os.path.join(out_dir, name + '.resbun')), len(wb)))

if __name__ == '__main__':
    build(*sys.argv[1:5])
