#!/usr/bin/env python3
"""supernisaudio.py: the Super Team's cutscene audio, as Super Mario Strikers (GC) composed it at
run time, rendered to Mario Strikers Charged NIS-cue streams.

  supernisaudio.py [--out DIR] [--sms-audio DIR] [--sms-streams DIR] [--only CUE]

SMS has no per-captain cutscene recording. During a Super Team cutscene it plays, live:
  * music on the stream "Music" track, chosen by soundevents.byte_code, not by captain:
      goal  -> event MusicGoal (raised by presentation.byte_code right before the goal NIS plays):
               STAD_Goal_<Rand4>_32k.idsp, volume 0.6, 500 ms fade-in
      intro -> event MusicStadiumIntro (raised by the stadium flyby NIS): STAD_<stadium>_Intro_32k.idsp,
               volume 0.5 (the Super Team gets STAD_Super_Intro here: its stadium in SMS)
      end of game -> CrowdWin<side>: STAD_EoG_<Rand3>_32k.idsp, volume 0.6
  * the NIS triggers of the mystery_* NIS (nis_triggers.byte_code): character SFX by name
    (servos, land, jump, turn, breathing, energy blast, rocket boots, electrocute), random
    "dialogue" efforts (CharDialogueType -> cCharacterSFX charDialogueSFX table) and footsteps,
    world SFX (force field, fireworks, camera zoom, crowd cheer), each through the Super's
    sound-property tables (supergen / supergrass / world / crowd / stadgen) and volume groups.
    SMS has SFXCHAR_SUPER_NIS_* "voice lines", but they are aliases of the effort samples and the
    mystery NIS never trigger them; the robot's voice in its cutscenes is these efforts.
  * crowd: the AudioScript crowd events around the NIS (CrowdGoalHome, CrowdIntroHome/Away)
    -> only in the "<cue>" variant; "<cue>_nocrowd" leaves them out (Charged's own split).
Charged instead plays one pre-mixed stream per cue (NisPlayer::PlayNisCue), so this renders that
mix with SMS's levels, timed from the cue start (= the first NIS's Play()), at 32 kHz stereo.
"""
import argparse, array, json, math, os, random, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH = os.getcwd()  # default inputs and outputs: sms/, smsdecomp/, out/ under the working folder
sys.path.insert(0, HERE)
import musyx
import streambank
from smsstream import read_sms_idsp
from streambank import nl_lower_hash

RATE = 32000
FPS = 30.0                                 # NIS keys / trigger frames per second (cAnimCamera, Nis::UpdateTriggers)
VOLUME_GROUPS = {1: 0.9, 2: 0.6, 4: 1.0, 15: 0.9}   # audio/VolumeGroups.ini (15 = Super, x All Ingame Dialogue 1.0)
CHAR_CLASS_MYSTERY = 12
TERRAIN = 'supergrass'                     # CHARSFX_WALK/LAND/JUMP/TURN are per-surface; a grass pitch

# cCharacterSFX::PlayRandomCharDialogue: charDialogueSFX[] and charDialogueSFXInfo[] (CharacterAudio.cpp)
DIALOGUE_SFX = ['CHARSFX_EFFORTS_ATTACK_01', 'CHARSFX_EFFORTS_ATTACK_02', 'CHARSFX_EFFORTS_ATTACK_03',
                'CHARSFX_EFFORTS_GET_HIT_01', 'CHARSFX_EFFORTS_GET_HIT_02', 'CHARSFX_EFFORTS_GET_HIT_03',
                'CHARSFX_EFFORTS_HIT_01', 'CHARSFX_EFFORTS_HIT_02', 'CHARSFX_EFFORTS_HIT_03',
                'CHARSFX_EFFORTS_PAIN_01', 'CHARSFX_EFFORTS_PAIN_02', 'CHARSFX_EFFORTS_PAIN_03',
                'CHARSFX_EFFORTS_PAIN_04', 'CHARSFX_EFFORTS_PAIN_05', 'CHARSFX_EFFORTS_ELECTROCUTE_01',
                'CHARSFX_EFFORTS_PERFECT_PASS', 'CHARSFX_BREATH_WITH_BALL', 'CHARSFX_CALL_HEY_01',
                'CHARSFX_CALL_HEY_02', 'CHARSFX_CALL_WO_01', 'CHARSFX_EFFORTS_ELECTROCUTE_02',
                'CHARSFX_EFFORTS_ELECTROCUTE_03', 'CHARSFX_EFFORTS_EXERT_01', 'CHARSFX_EFFORTS_EXERT_02',
                'CHARSFX_EFFORTS_EXERT_03', 'CHARSFX_EFFORTS_KICK_01', 'CHARSFX_GEN_STOS_FLOAT',
                'CHARSFX_GEN_STOS_FLOAT_HYPER', 'CHARSFX_NIS_CLAP_01']
DIALOGUE_INFO = [(0, 3), (3, 3), (6, 3), (9, 3), (12, 3), (15, 5), (20, 3), (23, 3), (26, 3)]
FOOTSTEP_WALK = ['CHARSFX_WALK_0%d' % i for i in range(1, 6)]

# soundevents.byte_code crowd events used around these NIS (delay s, crowd event); see the report
CROWD_GOAL_HOME = [(0.25, 'CROWDSFX_GOAL_HOME'), (1.3, 'CROWDSFX_EVENT_YEAH_BIG'), (3.0, 'CROWDSFX_EVENT_CLAP_BIG')]
CROWD_INTRO_HOME = [(0.0, 'CROWDSFX_EVENT_YEAH_BIG'), (2.0, 'CROWDSFX_EVENT_CLAP_BIG')]
CROWD_INTRO_AWAY = [(0.0, 'BOO_BIG_RANDOM'), (1.0, 'CROWDSFX_EVENT_JEER_BIG')]

# Charged's away-intro cues carry the theme only for the away NIS sequence (~4 s for Waluigi) and then
# just the crowd bed (silence in _nocrowd) to 26.19 s; music_until_parts_end does the same here.
# Charged cue -> SMS parts. Each part: (SMS NIS, Charged NIS it became, NIS keys). Cue lengths follow
# Waluigi's equivalent cues (goal 26.19 s, home intro 15.66 s, away intro 26.19 s).
CUES = {
    'superteam_goal_winner_high_0': dict(parts=['mystery_goal_winner_high_0'], music=('STAD_Goal_1_32k', 0.6),
                                         length=26.192, crowd=CROWD_GOAL_HOME, bed='CROWDSFX_LOOP_POS', fade_out=3.0),
    'superteam_goal_winner_high_1': dict(parts=['mystery_goal_winner_high_1'], music=('STAD_Goal_2_32k', 0.6),
                                         length=26.192, crowd=CROWD_GOAL_HOME, bed='CROWDSFX_LOOP_POS', fade_out=3.0),
    'superteam_goal_winner_low_0': dict(parts=['mystery_goal_winner_low_0'], music=('STAD_Goal_3_32k', 0.6),
                                        length=26.192, crowd=CROWD_GOAL_HOME, bed='CROWDSFX_LOOP_POS', fade_out=3.0),
    # NisPlayer prepares a cue only while none is pending and the presentation script stops the cue
    # (native 77) only before *_capt_intro_1: the part-1 cue plays through parts 2 and 3.
    'superteam_home_capt_intro_1': dict(parts=['mystery_enter_stadium_home_0', 'mystery_run_to_center_0',
                                               'mystery_attitude_home_0'],
                                        music=('STAD_Super_Intro_32k', 0.5), length=15.661, crowd=CROWD_INTRO_HOME,
                                        bed='CROWDSFX_LOOP_POS', fade_out=2.0),
    'superteam_away_capt_intro_1': dict(parts=['mystery_enter_stadium_away_0', 'mystery_attitude_home_0'],
                                        music=('STAD_Super_Intro_32k', 0.5), length=26.192, crowd=CROWD_INTRO_AWAY,
                                        bed='CROWDSFX_LOOP_NEG', fade_out=3.0, music_until_parts_end=1.0),
    # Never requested by Charged (the holotron NIS is loaded with param5 = 1, so PlayNisCue is skipped,
    # and cue 0x625F1997 plays instead). Rendered for completeness; not registered by default.
    'superteam_end_of_game_holotron_home': dict(parts=['mystery_end_of_game_home_0'], music=('STAD_EoG_1_32k', 0.6),
                                                length=26.192, crowd=[], fade_out=3.0, optional=True),
}
NIS_OF = {'mystery_goal_winner_high_0': 'superteam_goal_winner_high_0',
          'mystery_goal_winner_high_1': 'superteam_goal_winner_high_1',
          'mystery_goal_winner_low_0': 'superteam_goal_winner_low_0',
          'mystery_enter_stadium_home_0': 'superteam_home_capt_intro_1',
          'mystery_run_to_center_0': 'superteam_home_capt_intro_2',
          'mystery_attitude_home_0': 'superteam_home_capt_intro_3 / superteam_away_capt_intro_2_home',
          'mystery_enter_stadium_away_0': 'superteam_away_capt_intro_1',
          'mystery_end_of_game_home_0': 'superteam_end_of_game_holotron_home'}


# ---------------------------------------------------------------- SMS NIS triggers
def sms_nis_triggers(path, nis_name):
    """Trigger records of one SMS NIS from nis_triggers.byte_code (SMS InterpreterCore format)."""
    from smsbc import BC, nlh
    bc = BC(open(path, 'rb').read())
    by_hash = {h: i for i, (h, o) in enumerate(bc.funcs)}

    def walk(idx, out):
        h, off = bc.funcs[idx]
        offs = sorted(set(f[1] & ~1 for f in bc.funcs) | {bc.csz})
        end = next(e for e in offs if e > (off & ~1))
        pc, st = off & ~1, []
        while pc < end:
            ins = struct.unpack_from('>H', bc.d, bc.coff + pc)[0]
            hi, lo = ins & 0xC000, ins & 0x3FFF
            if hi == 0:
                v = bc.data[lo]
                st.append(('d', v, struct.unpack('>f', struct.pack('>I', v))[0]))
            elif hi == 0x4000:
                st.append(('s', bc.s(lo)))
            elif hi == 0x8000:
                out.append((lo, st))
                st = []
            else:
                sub, a = (lo >> 8) & 0xFF, lo & 0xFF
                if sub == 9:
                    st.append(('i', a))
                elif sub == 0xA:
                    walk(a, out)
                    st = []
            pc += 2
        return out

    idx = by_hash.get(nlh(nis_name, True), by_hash.get(nlh(nis_name)))
    if idx is None:
        return []
    raw = walk(idx, [])

    def num(x):
        if x[0] == 'i':
            return x[1]
        return x[2] if (0x30000000 < x[1] < 0x50000000) else (x[1] if x[1] < 0x80000000 else x[1] - (1 << 32))

    def s(x):
        return x[1] if x[0] == 's' else ''
    trig = []
    for nat, a in raw:
        if nat == 1:      # PlaySound(frame, name, charClass, target, useStopAtNisEnd)
            trig.append(dict(kind='char', frame=num(a[0]), name=s(a[1]), cls=num(a[2]), stop_at_end=num(a[4]) != 1))
        elif nat == 2:
            trig.append(dict(kind='char', frame=num(a[0]), name=s(a[1]), cls=num(a[2]), volume=num(a[3]),
                             stop_at_end=num(a[5]) != 1))
        elif nat == 3:    # PlayRandomDialogue(frame, dialogueType, charClass, target, useNameAsTarget)
            trig.append(dict(kind='dialogue', frame=num(a[0]), type=num(a[1]), cls=num(a[2]), stop_at_end=num(a[4]) != 1))
        elif nat == 5:    # PlaySound world (frame, name, target, useStopAtNisEnd)
            trig.append(dict(kind='world', frame=num(a[0]), name=s(a[1]), target=s(a[2]), stop_at_end=num(a[3]) != 1))
        elif nat == 6:
            trig.append(dict(kind='world', frame=num(a[0]), name=s(a[1]), volume=num(a[2]), target=s(a[3]),
                             stop_at_end=num(a[4]) != 1))
        elif nat in (11, 13):  # StopSound(frame, name, ...)
            trig.append(dict(kind='stop', frame=num(a[0]), name=s(a[1])))
        elif nat == 7:    # RaiseEvent(frame, name, target)
            trig.append(dict(kind='event', frame=num(a[0]), name=s(a[1]), target=s(a[2])))
    return trig


# ---------------------------------------------------------------- SFX rendering
class SFX:
    def __init__(self, audio_dir, decomp):
        self.bank = musyx.Bank(audio_dir)
        self.ids, _ = musyx.read_defines(decomp)
        self.props = {}
        for stem in ('supergen', TERRAIN, 'world', 'crowd', 'stadgen'):
            for r in musyx.read_soundprops(decomp, stem):
                self.props.setdefault(stem, {})[r['event']] = r
        self.cache = {}

    def event(self, name, tables):
        for t in tables:
            r = self.props[t].get(name)
            if r:
                return r
        return None

    def render(self, sfx_name, rng, length=None):
        """-> list of floats at RATE (mono, +-32768 scale). Loops (play-until-key-off) are tiled to
        `length` samples; random pitch drawn from rng as MusyX's RndNote would."""
        sid = self.ids.get(sfx_name)
        if sid is None or sid not in self.bank.sfx:
            return None, False
        e, paths = self.bank.resolve(sid)
        v = paths[rng.randrange(len(paths))] if len(paths) > 1 else paths[0]
        starts = list(v.starts)
        for lay in v.layers:
            starts += musyx.flatten(lay)
        if not starts:
            return None, False
        mix = []
        looping = v.loops_until_keyoff
        base_vol = starts[0]['vol'] or 1.0
        for st in starts:
            s = self.bank.samples[st['sample']]
            semis = st['key'] - s.root_key + st['detune'] / 100.0
            if st['key_rand'] != [0, 0]:
                semis += rng.uniform(st['key_rand'][0], st['key_rand'][1])
            if st['detune_rand']:
                semis += rng.uniform(-st['detune_rand'], st['detune_rand']) / 100.0
            pcm = self.bank.pcm(s.id)
            if looping and s.loop_len and length:
                need = int(length * (2 ** (semis / 12)) * s.rate / RATE) + 2
                ls, le = s.loop_start, s.loop_start + s.loop_len
                body = list(pcm[:le])
                while len(body) < need:
                    body += pcm[ls:le]
                pcm = body[:need]
            r = musyx.resample(pcm, 2 ** (semis / 12) * s.rate / RATE)
            g = st['vol']
            if len(r) > len(mix):
                mix += [0.0] * (len(r) - len(mix))
            for i, x in enumerate(r):
                mix[i] += x * g
        return mix, looping


# ---------------------------------------------------------------- mixing
def nis_keys(sms_nis_dir, name):
    import nis
    n = nis.load(os.path.join(sms_nis_dir, name + '.nis'), nis.SMS)
    cams = [c.count for c in n.cameras]
    return max(cams) if cams else max(a.num_keys for a in n.anims)


def build_cue(cue, spec, a, sfx, with_crowd):
    """-> (buses {music, sfx, crowd: [left, right]}, music_rms, log). Gains are SMS's (stream volume,
    sound-property volume x volume group); mastering scales all buses together."""
    rng = random.Random(nl_lower_hash(cue))            # character SFX picks / pitch
    crng = random.Random(nl_lower_hash(cue) ^ 0x5A5A)  # crowd picks (keeps rng identical across variants)
    L = int(spec['length'] * RATE)
    bus = {k: ([0.0] * L, [0.0] * L) for k in ('music', 'sfx', 'crowd')}
    log = []

    def add(name, pcm, start, gain, stop=None):
        if pcm is None:
            return
        left, right = bus[name]
        i0 = int(round(start * RATE))
        end = len(pcm) if stop is None else min(len(pcm), int(round((stop - start) * RATE)))
        fade = int(0.012 * RATE)
        for k in range(max(0, end)):
            j = i0 + k
            if j >= L:
                break
            if j < 0:
                continue
            g = gain
            if stop is not None and k > end - fade:
                g *= max(0.0, (end - k) / fade)
            x = pcm[k] * g
            left[j] += x
            right[j] += x

    # NIS part timing (cue t=0 = the first NIS's Play(); later parts follow back to back)
    parts = []
    t = 0.0
    for part in spec['parts']:
        keys = nis_keys(a.sms_nis, part)
        parts.append((part, t, keys, keys / FPS))
        t += keys / FPS
    parts_end = t

    # music bus
    mname, mvol = spec['music']
    rate, mus, info = read_sms_idsp(open(os.path.join(a.sms_streams, mname + '.idsp'), 'rb').read())
    assert rate == RATE, rate
    fin = int(0.5 * RATE)
    mend = L
    mfade = int(spec['fade_out'] * RATE)
    if spec.get('music_until_parts_end'):
        mfade = int(spec['music_until_parts_end'] * RATE)
        mend = min(L, int(parts_end * RATE) + mfade)
    for c, dst in enumerate(bus['music']):
        ch = mus[c]
        for j in range(min(mend, len(ch))):
            g = mvol * (j / fin if j < fin else 1.0)
            if j >= mend - mfade:
                g *= (mend - j) / mfade
            dst[j] = ch[j] * g
    # loudness reference: the whole SMS music file at its stream volume (the same for every cue that
    # uses it, whatever slice of it the cue holds)
    music_rms = mvol * math.sqrt((sum(x * x for x in mus[0][::7]) + sum(x * x for x in mus[1][::7])) /
                                 (2 * len(mus[0][::7])))
    log.append('music %s x%.2f, 0.5 s fade-in, %.2f s%s' % (mname, mvol, mend / RATE,
               ' (ends with the NIS parts + %.1f s fade)' % spec['music_until_parts_end']
               if spec.get('music_until_parts_end') else ''))

    for part, t0, keys, dur in parts:
        trig = sms_nis_triggers(a.sms_nis_triggers, part)
        log.append('part %s (-> %s) at %.3f s, %d keys = %.3f s, %d triggers' % (part, NIS_OF.get(part, '?'), t0,
                                                                                keys, dur, len(trig)))
        plays = []
        stops = {}
        for tr in trig:
            if tr['kind'] == 'stop':
                stops.setdefault(tr['name'].upper(), []).append(tr['frame'] / FPS)
        last_walk = None
        dlg = []
        for tr in sorted(trig, key=lambda x: x['frame']):
            ts = tr['frame'] / FPS
            if tr['kind'] == 'char':
                ev = sfx.event(tr['name'], ('supergen', TERRAIN))
                if not ev:
                    log.append('   ! no table entry for %s' % tr['name'])
                    continue
                stop = next((x for x in stops.get(tr['name'].upper(), []) if x > ts), None)
                vol = (tr.get('volume', 100.0) or 100.0) / 100.0
                plays.append(['sfx', ts, ev, vol, stop, tr['stop_at_end'], tr['name']])
            elif tr['kind'] == 'dialogue':
                if tr['type'] in (9, 10):
                    nm = rng.choice([x for x in FOOTSTEP_WALK if x != last_walk])
                    last_walk = nm
                    ev = sfx.event(nm, (TERRAIN, 'supergen'))
                    if ev:
                        plays.append(['sfx', ts, ev, 1.0, None, tr['stop_at_end'], 'footstep ' + nm])
                    continue
                base, cnt = DIALOGUE_INFO[tr['type']]
                nm = DIALOGUE_SFX[base + rng.randrange(cnt)]
                ev = sfx.event(nm, ('supergen', TERRAIN))
                if dlg and (dlg[-1][4] is None or dlg[-1][4] > ts):
                    dlg[-1][4] = ts               # StopPlayingAllCharDialogue()
                dlg.append(['sfx', ts, ev, 1.0, None, tr['stop_at_end'], 'dialogue %d %s' % (tr['type'], nm)])
                if not ev:
                    log.append('   ! dialogue type %d picked %s: no Super table entry (silent in SMS too)' % (
                        tr['type'], nm))
            elif tr['kind'] == 'world':
                crowd = tr['name'].startswith('CROWDSFX')
                if crowd and not with_crowd:
                    continue
                ev = sfx.event(tr['name'], ('world', 'crowd', 'stadgen'))
                if not ev:
                    log.append('   ! no world entry %s' % tr['name'])
                    continue
                vol = 1.0 if not tr['target'] else (tr.get('volume', 100.0) or 100.0) / 100.0
                stop = next((x for x in stops.get(tr['name'].upper(), []) if x > ts), None)
                plays.append(['crowd' if crowd else 'sfx', ts, ev, vol, stop, tr['stop_at_end'], tr['name']])
            elif tr['kind'] == 'event':
                if with_crowd and tr['name'] in ('EnterStadiumHome', 'EnterStadiumAway') and spec.get('crowd'):
                    for d, nm in spec['crowd']:
                        if nm == 'BOO_BIG_RANDOM':
                            nm = 'CROWDSFX_EVENT_BOO_BIG%d' % crng.randint(1, 3)
                        ev = sfx.event(nm, ('crowd',))
                        plays.append(['crowd', ts + d, ev, 1.0, None, False, 'Crowd%s -> %s' % (
                            tr['name'].replace('EnterStadium', 'Intro'), nm)])
        plays += [r for r in dlg if r[2]]
        for busname, ts, ev, vol, stop, stop_at_end, label in sorted(plays, key=lambda r: r[1]):
            end = stop
            if stop_at_end and (end is None or end > dur):
                end = dur
            gain = ev['volume'] * VOLUME_GROUPS.get(ev['volume_group'], 0.9) * vol
            length = int(((end if end is not None else spec['length'] - t0) - ts) * RATE) + 16
            pcm, looping = sfx.render(ev['sfx'], crng if busname == 'crowd' else rng, length)
            if pcm is None:
                log.append('   ! %s: SFX %s missing' % (label, ev['sfx']))
                continue
            if looping and end is None:
                end = spec['length'] - t0
            add(busname, pcm, t0 + ts, gain, None if end is None else t0 + end)
            log.append('   %6.3f s %-5s %-42s %-36s x%.3f%s' % (t0 + ts, busname, label, ev['sfx'], gain,
                                                              '' if end is None else '  (stops %.3f s)' % (t0 + end)))

    if with_crowd:
        if spec.get('crowd') is CROWD_GOAL_HOME:
            # CrowdGoal<side> fires at the goal, just before this cue starts
            for d, nm in spec['crowd']:
                ev = sfx.event(nm, ('crowd',))
                pcm, _ = sfx.render(ev['sfx'], crng)
                gain = ev['volume'] * VOLUME_GROUPS.get(ev['volume_group'], VOLUME_GROUPS[2])
                add('crowd', pcm, d, gain)
                log.append('   %6.3f s crowd CrowdGoalHome %-28s %-36s x%.3f' % (d, nm, ev['sfx'], gain))
        if spec.get('bed'):
            ev = sfx.event(spec['bed'], ('crowd',))
            pcm, _ = sfx.render(ev['sfx'], crng, L + 16)
            if pcm is not None and len(pcm) < L:          # tile non-key-off loops too
                base = list(pcm)
                while len(pcm) < L:
                    pcm += base
            gain = ev['volume'] * VOLUME_GROUPS.get(ev['volume_group'], VOLUME_GROUPS[2])
            fi = int(0.3 * RATE)
            pcm = [x * (k / fi if k < fi else 1.0) for k, x in enumerate(pcm[:L])]
            add('crowd', pcm, 0.0, gain)
            log.append('   crowd bed %s (%s) x%.3f, whole cue (SMS CrowdMood loop)' % (spec['bed'], ev['sfx'], gain))

    fo = int(spec['fade_out'] * RATE)
    for b in bus.values():
        for ch in b:
            for j in range(L - fo, L):
                ch[j] *= (L - j) / fo
    return bus, music_rms, log


def limiter(left, right, thr, look=0.004, release=0.120):
    """Look-ahead peak limiter (stereo-linked): the gain ramps down over `look` s before a peak so
    |out| <= thr, then recovers with a `release` s time constant. Returns new lists and the number of
    samples whose gain was reduced."""
    n = len(left)
    need = [1.0] * n
    for j in range(n):
        p = max(abs(left[j]), abs(right[j]))
        if p > thr:
            need[j] = thr / p
    W = max(1, int(look * RATE))
    g = need[:]
    for j in range(n - 2, -1, -1):          # attack: reach each dip linearly over W samples
        lim = g[j + 1] + (1.0 - g[j + 1]) / W if g[j + 1] < 1.0 else 1.0
        if lim < g[j]:
            g[j] = lim
    rel = 1.0 / max(1, release * RATE)
    cur = 1.0
    reduced = 0
    for j in range(n):                       # release
        cur = min(g[j], cur + (1.0 - cur) * rel * 4)
        g[j] = cur
        if cur < 0.999:
            reduced += 1
    gmin = min(g) if g else 1.0
    return [x * k for x, k in zip(left, g)], [x * k for x, k in zip(right, g)], (reduced, round(-20 * math.log10(gmin), 1))


def master(bus, gains):
    """music x gain; SFX x gain then peak-limited to -3 dBFS; crowd x gain; sum limited to -1 dBFS."""
    g = {k: 10 ** (v / 20) for k, v in gains.items()}
    m = [[x * g['music'] for x in ch] for ch in bus['music']]
    sl, sr, sred = limiter([x * g['sfx'] for x in bus['sfx'][0]], [x * g['sfx'] for x in bus['sfx'][1]],
                           10 ** (-3 / 20) * 32767)
    c = [[x * g['crowd'] for x in ch] for ch in bus['crowd']]
    L = [a + b + d for a, b, d in zip(m[0], sl, c[0])]
    R = [a + b + d for a, b, d in zip(m[1], sr, c[1])]
    L, R, fred = limiter(L, R, 10 ** (-1 / 20) * 32767)
    out = []
    for ch in (L, R):
        out.append(array.array('h', [int(round(max(-32768, min(32767, x)))) for x in ch]))
    return out, dict(sfx_limited_samples=sred[0], sfx_max_reduction_db=sred[1], mix_limited_samples=fred[0],
                     mix_max_reduction_db=fred[1])


def bus_peak(ch_pair):
    return max(max((abs(x) for x in ch), default=0.0) for ch in ch_pair)


SFX_GAIN_MAX_DB = 2.0   # SMS absolute SFX level + 2 dB (the goal music needs ~+4): keeps the robot audible, limits less


def plan_gains(bus, music_rms, music_target_db, crowd_rel_db=-16.0, crowd_peak_db=-6.0):
    """Music to the target loudness; SFX at SMS's absolute level x the SMS->Charged gain the goal music
    needs (so a quieter SMS music file is raised without raising the SFX with it), peaks limited in
    master(); the crowd bus sits crowd_rel_db under the music (Charged's cues: ~-20 dB)."""
    gm = music_target_db - 20 * math.log10(max(music_rms, 1e-9) / 32768)
    gc = gm
    cr = bus['crowd']
    crms = math.sqrt((sum(x * x for x in cr[0][::5]) + sum(x * x for x in cr[1][::5])) / (2 * len(cr[0][::5])))
    if crms > 0:
        gc = (music_target_db + crowd_rel_db) - 20 * math.log10(crms / 32768)
        gc = min(gc, crowd_peak_db - 20 * math.log10(bus_peak(cr) / 32768))
    return dict(music=gm, sfx=min(gm, SFX_GAIN_MAX_DB), crowd=gc)


def bus_rms_db(ch_pair, gain_db):
    l, r = ch_pair
    v = math.sqrt((sum(x * x for x in l[::5]) + sum(x * x for x in r[::5])) / (2 * len(l[::5]))) * 10 ** (gain_db / 20)
    return 20 * math.log10(max(v, 1e-3) / 32768)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=os.path.join(SCRATCH, 'out', 'audio', 'superteam'))
    ap.add_argument('--sms-audio', default=os.path.join(SCRATCH, 'sms', 'audio'), help='sebring.* (MusyX)')
    ap.add_argument('--sms-decomp', default=os.path.join(SCRATCH, 'smsdecomp'))
    ap.add_argument('--sms-streams', default=os.path.join(SCRATCH, 'sms', 'streams'), help='SMS .idsp streams')
    ap.add_argument('--sms-nis', default=os.path.join(SCRATCH, 'sms', 'nis'))
    ap.add_argument('--sms-nis-triggers', default=os.path.join(SCRATCH, 'sms', 'nis_triggers.byte_code'))
    ap.add_argument('--music-rms', type=float, default=-14.0,
                    help='dBFS the music bed is mastered to (Charged goal cues: ~-14; SFX/crowd keep SMS balance)')
    ap.add_argument('--interleave', type=lambda s: int(s, 0), default=0x6b40, help="STREAM_GEN_NIS's")
    ap.add_argument('--only', action='append')
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    sfx = SFX(a.sms_audio, a.sms_decomp)
    manifest = {}
    for cue, spec in CUES.items():
        if a.only and cue not in a.only:
            continue
        variants = [(cue, True), (cue + '_nocrowd', False)]
        if not spec.get('crowd') and not spec.get('bed'):
            variants = [(cue, False)]
        rendered = {}
        for name, crowd in variants:
            bus, mrms, log = build_cue(cue, spec, a, sfx, crowd)
            gains = plan_gains(bus, mrms, a.music_rms)
            pcm, lim = master(bus, gains)
            wav = os.path.join(a.out, name + '.wav')
            streambank.write_wav(wav, RATE, pcm)
            key = zlib.crc32(pcm[0].tobytes() + pcm[1].tobytes())
            same = next((k for k, v in rendered.items() if v == key), None)
            rendered[name] = key
            levels = {k: round(bus_rms_db(bus[k], gains[k]), 1) for k in bus}
            ent = dict(wav=os.path.basename(wav), seconds=round(len(pcm[0]) / RATE, 3), crowd=crowd,
                       bus_gain_db={k: round(v, 2) for k, v in gains.items()}, bus_rms_dbfs=levels, limited_samples=lim, log=log,
                       registered_by_default=not spec.get('optional'))
            if same:
                ent['same_audio_as'] = same
            else:
                blob = streambank.build_stream(pcm, RATE, a.interleave)
                bpath = os.path.join(a.out, name + '.idsp')
                open(bpath, 'wb').write(blob)
                ent['blob'] = os.path.basename(bpath)
                ent['blob_size'] = len(blob)
            manifest[name] = ent
            print('%-40s %.2fs gains %s, bus RMS %s, limited %s%s' % (
                name, len(pcm[0]) / RATE, {k: round(v, 1) for k, v in gains.items()}, levels, lim,
                ('  = ' + same) if same else ''))
            for line in log:
                print('    ' + line)
    old = {}
    mpath = os.path.join(a.out, 'superteam_cues.json')
    if a.only and os.path.exists(mpath):
        old = json.load(open(mpath))
    old.update(manifest)
    json.dump(old, open(mpath, 'w'), indent=1)


if __name__ == '__main__':
    main(sys.argv[1:])
