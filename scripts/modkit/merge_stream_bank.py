#!/usr/bin/env python3
"""Adds streams to one of Mario Strikers Charged's stream banks (audio/<bank>.resbun + .nlxwb).

A stream bank is an index (.resbun: an NL chunk tree with the bank's cues, voices, sequences, sound
events and its source table) and the streams themselves (.nlxwb: IDSP audio, one after another). This
writes a new index with the added streams' cues, and the bytes to append to the .nlxwb, which stays
the game's own: a pack ships the index as a file patch and the bytes as a patch at the .nlxwb's end
(Riivolution <file offset="...">). Each new cue copies a template cue's settings.

    merge_stream_bank.py --resbun <game .resbun> --nlxwb-size <bytes> --template <cue>
                         --out-resbun <file> --out-append <file> <stream.idsp>...

A stream's cue is its file name ("superteam_goal_winner_high_0.idsp" -> superteam_goal_winner_high_0);
--alias stream=cue adds more cue names for one stream.
"""

import argparse
import struct
import sys


def be32(data, offset):
    return struct.unpack_from(">I", data, offset)[0]


def put32(data, offset, value):
    struct.pack_into(">I", data, offset, value & 0xFFFFFFFF)


def lower_hash(text):  # nlStringLowerHash
    h = 0xFFFFFFFF
    for c in text.lower().encode("latin-1"):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


class Chunk:
    """An NL chunk, losslessly: its raw id (alignment bits kept) and its data or children."""

    def __init__(self, raw_id, data=b"", children=None):
        self.raw_id = raw_id
        self.data = bytearray(data)
        self.children = children if children is not None else []

    def id(self):
        return self.raw_id & 0x80FFFFFF

    def container(self):
        return (self.raw_id & 0x80000000) != 0


def parse_chunks(data, off, end, depth=0):
    if depth > 8:
        raise ValueError("chunks nested too deep")
    out = []
    while off + 8 <= end:
        raw_id = be32(data, off)
        chunk_end = off + 8 + be32(data, off + 4)
        if chunk_end > end:
            raise ValueError("chunk past its parent's end")
        payload = off + 8
        align = (raw_id >> 24) & 0xF
        if align:
            payload = (payload + (1 << align) - 1) & ~((1 << align) - 1)
        chunk = Chunk(raw_id)
        if chunk.container():
            chunk.children = parse_chunks(data, off + 8, chunk_end, depth + 1)
        else:
            if payload > chunk_end:
                raise ValueError("chunk payload past its end")
            chunk.data = bytearray(data[payload:chunk_end])
        out.append(chunk)
        off = chunk_end + ((4 - (chunk_end & 3)) & 3)
    return out


def serialize(chunk, out):
    at = len(out)
    align = (chunk.raw_id >> 24) & 0xF
    pad = (((1 << align) - ((at + 8) & ((1 << align) - 1))) & ((1 << align) - 1)) if align else 0
    out.extend(b"\0" * (8 + pad))
    body = len(out)
    if chunk.container():
        for i, child in enumerate(chunk.children):
            serialize(child, out)
            if i + 1 < len(chunk.children):
                out.extend(b"\0" * (((len(out) + 3) & ~3) - len(out)))
    else:
        out.extend(chunk.data)
    put32(out, at, chunk.raw_id)
    put32(out, at + 4, pad + len(out) - body)


def merge(resbun, nlxwb_size, streams, template_cue):
    """streams: [(names, blob)]. Returns (new resbun, bytes to append to the .nlxwb)."""
    CUE, VOICE, SEQ, EVENT, SOURCE = 40, 44, 12, 48, 28
    top = parse_chunks(resbun, 0, len(resbun))
    if len(top) != 1 or top[0].raw_id != 0x80000001:
        raise ValueError("not a sound bank")
    again = bytearray()
    serialize(top[0], again)
    if bytes(again) != bytes(resbun):
        raise ValueError("the bank doesn't re-serialize identically")
    root = top[0]
    smap = bundle = sources = None
    for c in root.children:
        if c.id() == 0x80023000:
            smap = c
        elif c.id() == 0x80023300:
            bundle = c
        elif c.id() == 0x80023200:
            sources = c
        elif c.id() == 0x23703:
            raise ValueError("a resident bank (it has a sample table), not a stream bank")
    if (not smap or not bundle or not sources or len(smap.children) != 2 or len(sources.children) != 2
            or len(bundle.children) < 7 or len(bundle.children[0].data) < 64):
        raise ValueError("not a stream bank")
    sm_header, sm_cues = smap.children
    h = bundle.children[0].data  # ResourceBundle header: counts and base pointers
    cues, voices, seqs, events = be32(h, 8), be32(h, 16), be32(h, 24), be32(h, 32)
    voice_base, seq_base, event_base = be32(h, 20), be32(h, 28), be32(h, 36)
    kids = bundle.children
    rest_first = 7
    voice_first = rest_first + cues
    seq_first = voice_first + 2 * voices
    event_first = seq_first + seqs
    if len(kids) != event_first + events or be32(sm_header.data, 0) * 20 != len(sm_cues.data):
        raise ValueError("unexpected resource bundle layout")

    def record(array, size, index):
        a = kids[1 + array].data
        return bytes(a[index * size:(index + 1) * size]) if (index + 1) * size <= len(a) else b""

    # The template cue's chain: cue -> its first voice -> sequence -> sound event (-> choice).
    template_index = be32(sm_cues.data, 16) if sm_cues.data else 0xFFFFFFFF
    if template_cue:
        template_index = 0xFFFFFFFF
        for i in range(0, len(sm_cues.data) - 19, 20):
            if be32(sm_cues.data, i) == lower_hash(template_cue):
                template_index = be32(sm_cues.data, i + 16)
    if template_index >= cues or len(kids[rest_first + template_index].data) < 20:
        raise ValueError("template cue %s not in the bank" % template_cue)
    cue_t = record(0, CUE, template_index)
    entry_t = bytes(kids[rest_first + template_index].data)
    v0 = (be32(entry_t, 0) - voice_base) // VOICE
    if v0 >= voices:
        raise ValueError("template cue has no voice")
    voice_t = record(1, VOICE, v0)
    rpc_t = bytes(kids[voice_first + 2 * v0 + 1].data)
    s0 = (be32(kids[voice_first + 2 * v0].data, 0) - seq_base) // SEQ
    if s0 >= seqs or len(kids[seq_first + s0].data) < 8:
        raise ValueError("template cue has no sequence")
    seq_t = record(2, SEQ, s0)
    e0 = (be32(kids[seq_first + s0].data, 4) - event_base) // EVENT
    if e0 >= events or len(kids[event_first + e0].data) < 8 or not cue_t or not voice_t or not seq_t:
        raise ValueError("template cue has no sound event")
    event_t = record(3, EVENT, e0)
    choice_t = bytes(kids[event_first + e0].data)
    source_header, source_list = sources.children[0].data, sources.children[1].data
    if len(source_header) < 9 or len(source_list) != be32(source_header, 0) * SOURCE:
        raise ValueError("unexpected source table")
    interleave = be32(source_header, 4)

    known = {be32(sm_cues.data, i) for i in range(0, len(sm_cues.data) - 19, 20)}
    append = bytearray(b"\0" * ((32 - nlxwb_size % 32) % 32))
    source_count = be32(source_header, 0)
    new_entries, new_voices, new_seqs, new_events = [], [], [], []
    for names, blob in streams:
        if (len(blob) < 0xE0 or len(blob) % 32 or be32(blob, 0) != 0x49445350 or be32(blob, 4) != interleave):
            raise ValueError("%s: not a stream in this bank's format (IDSP, interleave %d, 32-byte size)"
                             % (names[0], interleave))
        offset = nlxwb_size + len(append)
        append.extend(blob)
        source, voice, seq, event = source_count, voices, seqs, events
        source_count, voices, seqs, events = source_count + 1, voices + 1, seqs + 1, events + 1
        primary = lower_hash(names[0])
        source_list.extend(struct.pack(">7I", source, offset, len(blob), primary, 2, 0, 0))
        kids[2].data.extend(struct.pack(">I", primary) + voice_t[4:])  # voice record: the template's, named
        new_voices.append(Chunk(0x23309, struct.pack(">I", seq_base + SEQ * seq)))
        new_voices.append(Chunk(0x2330C, rpc_t))
        kids[3].data.extend(seq_t)
        new_seqs.append(Chunk(0x2330A, struct.pack(">II", 1, event_base + EVENT * event)))
        kids[4].data.extend(event_t)
        new_events.append(Chunk(0x2330B, struct.pack(">I", source) + choice_t[4:8]))
        for name in names:
            cue_hash = lower_hash(name)
            if cue_hash in known:
                raise ValueError("cue %s is already in the bank" % name)
            known.add(cue_hash)
            kids[1].data.extend(struct.pack(">I", cue_hash) + cue_t[4:])
            new_entries.append(Chunk(0x23308, struct.pack(">I", voice_base + VOICE * voice) + entry_t[4:]))
            sm_cues.data.extend(struct.pack(">5I", cue_hash, 0, 0, 0, cues))
            cues += 1
    put32(sm_header.data, 0, cues)
    put32(h, 8, cues)
    put32(h, 16, voices)
    put32(h, 24, seqs)
    put32(h, 32, events)
    put32(source_header, 0, source_count)
    # Entry chunks in the bundle's order: cues', voices' pairs, sequences', sound events'.
    kids[event_first + (events - len(new_events)):event_first + (events - len(new_events))] = new_events
    kids[event_first:event_first] = new_seqs
    kids[seq_first:seq_first] = new_voices
    kids[voice_first:voice_first] = new_entries
    merged = bytearray()
    serialize(root, merged)
    return bytes(merged), bytes(append)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--resbun", required=True)
    parser.add_argument("--nlxwb-size", required=True, type=int)
    parser.add_argument("--template", default="")
    parser.add_argument("--alias", action="append", default=[], help="stream=cue")
    parser.add_argument("--out-resbun", required=True)
    parser.add_argument("--out-append", required=True)
    parser.add_argument("streams", nargs="+")
    args = parser.parse_args()
    aliases = {}
    for item in args.alias:
        stream, _, cue = item.partition("=")
        aliases.setdefault(stream.strip(), []).append(cue.strip())
    streams = []
    for path in sorted(args.streams):
        stem = path.rsplit("/", 1)[-1].rsplit(".", 1)[0]
        with open(path, "rb") as f:
            streams.append(([stem] + aliases.get(stem, []), f.read()))
    with open(args.resbun, "rb") as f:
        resbun = f.read()
    try:
        merged, append = merge(resbun, args.nlxwb_size, streams, args.template)
    except ValueError as error:
        sys.exit("merge_stream_bank: %s" % error)
    with open(args.out_resbun, "wb") as f:
        f.write(merged)
    with open(args.out_append, "wb") as f:
        f.write(append)
    print("%s: %d stream(s), %d bytes to append at %d" % (args.out_resbun, len(streams), len(append), args.nlxwb_size))


if __name__ == "__main__":
    main()
