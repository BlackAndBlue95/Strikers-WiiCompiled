#!/usr/bin/env python3
"""megastrike.py: Super Team's Mega Strike cutscenes: Super Mario Strikers' Super Strike (the robot's
captain_shoot_to_score animation) played in Mario Strikers Charged's Mega Strike, shot for shot.

    python3 megastrike.py --sms-sanim SuperTeam.sanim --charged-nis <Charged Art/nis> --out <dir>
        -> <dir>/superteam_megastrike_{home,away}_{0..3}.nis, <dir>/superteam_megastrike_nis_dict.txt

Charged's Mega Strike is four cutscenes per side (NisPlayer picks "<captain>_megastrike_<side>_<n>"):
0 the captain leaps into the sky after the ball, 1 it rises, 2 it strikes in front of its backdrop,
3 the balls fly at the goal (the same file for every captain). SMS's Super Strike is one animation:
windup (keys 0-16), jump into a flip (16-24, held to 56), unfurl and float (56-96), the kick (104),
landing. Each of Super Team's cutscenes is Waluigi's (its base: same skeleton), with his cameras, props
and his path through the shot (root and bip01 translation, so the camera still frames the body), and
SMS's body animation, retimed onto the shot: every bone's rotation from SMS. SMS's kick launches the ball
on the same path relative to the body as Charged's (Charged's kick was made from it), so cutscene 2 plays
SMS's float and kick at its own speed and its ball. Waluigi's whip (cutscene 2) is left out. Cutscene 3
is Waluigi's, under Super Team's name, so that its own triggers run. The away cutscenes are made from
Waluigi's away ones (mirrored shots); SMS's body is the same on both sides.
"""
import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nis      # noqa: E402
import nlchunks  # noqa: E402
import sanim    # noqa: E402

BASE = 'waluigi'
NAME = 'superteam'
ANIM_NAME = 'SuperTeam'

# Charged key -> SMS key, piecewise linear, per cutscene.
TIMING = {
    0: [(0, 0), (8, 16), (14, 24), (20, 56), (32, 72), (34, 74)],   # windup, jump and flip, unfurl
    1: [(0, 66), (30, 92)],                                          # floating up
    2: [(0, 68), (41, 104), (52, 115)],                              # float, the kick on Charged's kick key
}
SMS_BALL = {2}   # cutscenes whose ball is SMS's (relative to the body); the others keep Waluigi's
DROP = {'WaluigiWhip'}


def sms_time(seg, k):
    knots = TIMING[seg]
    for (k0, t0), (k1, t1) in zip(knots, knots[1:]):
        if k <= k1:
            f = (k - k0) / float(k1 - k0) if k1 != k0 else 0.0
            return t0 + (t1 - t0) * max(0.0, f)
    return knots[-1][1]


def _at(track, k):
    return track[min(k, len(track) - 1)]


def nlerp(a, b, f):
    if sum(x * y for x, y in zip(a, b)) < 0:
        b = tuple(-x for x in b)
    q = tuple(x + (y - x) * f for x, y in zip(a, b))
    n = math.sqrt(sum(x * x for x in q)) or 1.0
    return tuple(x / n for x in q)


def sample_rot(node, t):
    k0 = int(math.floor(t))
    f = t - k0
    if node.rot is not None:
        return nlerp(_at(node.rot, k0), _at(node.rot, k0 + 1), f)
    return None


def sample_rotz(node, t):
    k0 = int(math.floor(t))
    f = t - k0
    a, b = _at(node.rotz, k0), _at(node.rotz, k0 + 1)
    d = (b - a + 32768) % 65536 - 32768
    return int(round(a + d * f)) % 65536


def sample_trans(node, t):
    k0 = int(math.floor(t))
    f = t - k0
    a, b = _at(node.trans, k0), _at(node.trans, k0 + 1)
    return tuple(x + (y - x) * f for x, y in zip(a, b))


def composite(wal, sms, seg):
    """Waluigi's captain animation `wal` with SMS's body (`sms`, remapped to Charged's skeleton)."""
    out = sanim.Anim()
    out.fmt = 'composite'
    out.name = ANIM_NAME
    out.hash = 0
    out.num_keys = wal.num_keys
    out.num_root_keys = wal.num_root_keys
    out.signature = sanim.CHARGED_WALUIGI_SIGNATURE
    out.root_rot = list(wal.root_rot)
    out.root_trans = list(wal.root_trans)
    out.root_trans_extra = wal.root_trans_extra
    out.morph_num_keys, out.morph_ids, out.morph_keys = [], [], b''
    times = [sms_time(seg, k) for k in range(wal.num_keys)]
    out.nodes = []
    for i in range(sanim.CHARGED_NUM_NODES):
        src = sms.nodes[i]
        n = sanim.Node()
        if src.rot is not None:
            n.rot = [sample_rot(src, t) for t in times]
        elif src.rotz is not None:
            n.rotz = [sample_rotz(src, t) for t in times]
        out.nodes.append(n)
    # The body follows Waluigi's path through the shot.
    out.nodes[2].trans = [_at(wal.nodes[2].trans, k) for k in range(wal.num_keys)]
    ball = out.nodes[1]
    if seg in SMS_BALL:
        # SMS's ball, where it is relative to the body (both are the root's children).
        ball.trans = []
        for k, t in enumerate(times):
            b, p, w = sample_trans(sms.nodes[1], t), sample_trans(sms.nodes[2], t), _at(wal.nodes[2].trans, k)
            ball.trans.append(tuple(w[j] + b[j] - p[j] for j in range(3)))
    else:
        wb = wal.nodes[1]
        ball.rot = list(wb.rot) if wb.rot is not None else None
        ball.rotz = None
        ball.trans = list(wb.trans) if wb.trans is not None else None
    return out


def captain_index(anims):
    return next(i for i, a in enumerate(anims) if a.num_nodes == sanim.CHARGED_NUM_NODES)


def build(sms_sanim, charged_nis, out_dir):
    _, smsanims = sanim.load(sms_sanim, sanim.SMS)
    sts = next(a for a in smsanims if a.name == 'captain_shoot_to_score')
    sms = sanim.remap_to_charged(sts)
    dict_path = os.path.join(charged_nis, 'nis_dict.txt')
    entries = {e['name'].lower(): e for e in nis.parse_dict(dict_path)}
    os.makedirs(out_dir, exist_ok=True)
    lines = []
    for side in ('home', 'away'):
        for seg in range(4):
            src_name = '%s_megastrike_%s_%d.nis' % (BASE, side, seg)
            name = '%s_megastrike_%s_%d.nis' % (NAME, side, seg)
            data = open(os.path.join(charged_nis, src_name), 'rb').read()
            entry = dict(entries[src_name])
            if seg in TIMING:
                chunks = nlchunks.parse(data, charged=True)
                anims = [sanim.anim_from_chunk(c, sanim.CHARGED) for c in chunks if c.type == 0x80017000]
                ci = captain_index(anims)
                kept, begin, ai = [], [], 0
                for c in chunks:
                    if c.type != 0x80017000:
                        kept.append(c)
                        continue
                    a = anims[ai]
                    if a.name in DROP:
                        ai += 1
                        continue
                    if ai == ci:
                        c = sanim.encode_charged_chunk(composite(a, sms, seg), rot_format=16)
                    if ai < len(entry['begin_pos']):
                        begin.append(entry['begin_pos'][ai])
                    kept.append(c)
                    ai += 1
                data = nlchunks.write(kept, charged=True)
                entry['num_animations'] = sum(1 for c in kept if c.type == 0x80017000)
                entry['begin_pos'] = begin
            open(os.path.join(out_dir, name), 'wb').write(data)
            entry['size'] = len(data)
            lines.append(nis.dict_entry(name, entry))
            print('%-34s %6d bytes, %d animations' % (name, len(data), entry['num_animations']))
    with open(os.path.join(out_dir, 'superteam_megastrike_nis_dict.txt'), 'wb') as f:
        f.write(''.join(lines).encode('latin1'))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--sms-sanim', required=True, help="SMS's art/animation/SuperTeam.sanim")
    ap.add_argument('--charged-nis', required=True, help="Charged's Art/nis (Waluigi's Mega Strike, nis_dict.txt)")
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    build(a.sms_sanim, a.charged_nis, a.out)


if __name__ == '__main__':
    main()
