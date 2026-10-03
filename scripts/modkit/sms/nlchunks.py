# nlchunks.py: read and write the NL chunk container both games use, as a tree.
#
# A chunk is a big-endian {u32 id, u32 size} header and a payload. Ids with the top bit set
# hold child chunks. Bits 24..27 (Charged) / 24..30 (SMS) of the id ask for the payload to
# start on a 1<<n byte boundary (absolute file offset); the leading pad counts in `size`.
#
# Layout difference between the games:
#   SMS (GameCube): the next chunk starts right after `size` bytes, always.
#   Charged (Wii):  nlChunk::GetNextChunk rounds the next chunk up to a 4-byte boundary.
#                   The pad after a leaf does NOT count in the leaf's size, but a container's
#                   size includes the pads of its children (including the last one).
import struct


class Chunk:
    __slots__ = ('id', 'data', 'children', 'off')

    def __init__(self, cid, data=b'', children=None, off=None):
        self.id = cid
        self.data = data            # payload after any alignment pad (leaf chunks)
        self.children = children    # list of Chunk for containers, else None
        self.off = off

    @property
    def type(self):
        return self.id & 0x80FFFFFF

    @property
    def is_container(self):
        return bool(self.id & 0x80000000)

    def find(self, t):
        for c in self.children or ():
            if c.type == t:
                return c
        return None

    def findall(self, t):
        return [c for c in self.children or () if c.type == t]

    def __repr__(self):
        if self.is_container:
            return 'Chunk(%08x, %d children)' % (self.id, len(self.children))
        return 'Chunk(%08x, %d bytes)' % (self.id, len(self.data))


def _align_bits(cid, charged):
    return (cid >> 24) & (0x0F if charged else 0x7F)


def parse(d, off=0, end=None, charged=False):
    """Parse chunks between off and end. charged=True uses Charged's 4-byte next-chunk rounding."""
    if end is None:
        end = len(d)
    out = []
    while off + 8 <= end:
        cid, size = struct.unpack_from('>II', d, off)
        body = off + 8
        if cid & 0x80000000:
            ch = Chunk(cid, children=parse(d, body, body + size, charged), off=off)
        else:
            a = _align_bits(cid, charged)
            start = body
            if a:
                al = 1 << a
                start = (body + al - 1) & ~(al - 1)
            ch = Chunk(cid, data=bytes(d[start:body + size]), off=off)
        out.append(ch)
        off = body + size
        if charged and (off & 3):
            off = (off + 3) & ~3
    return out


def write(chunks, charged=True, base=0):
    """Serialise a list of chunks. Offsets are taken as absolute from `base` (file start = 0)."""
    out = bytearray()

    def emit(ch):
        pos = base + len(out)
        out.extend(b'\0' * 8)
        body = base + len(out)
        if ch.is_container:
            for c in ch.children:
                emit(c)
        else:
            a = _align_bits(ch.id, charged)
            if a:
                al = 1 << a
                pad = ((body + al - 1) & ~(al - 1)) - body
                out.extend(b'\0' * pad)
            out.extend(ch.data)
        size = base + len(out) - body
        struct.pack_into('>II', out, pos - base, ch.id, size)
        if charged and (len(out) + base) & 3:
            out.extend(b'\0' * (4 - ((len(out) + base) & 3)))

    for c in chunks:
        emit(c)
    return bytes(out)


def dump(chunks, depth=0, maxbytes=16, out=None):
    lines = [] if out is None else out
    for c in chunks:
        if c.is_container:
            lines.append('  ' * depth + '%08x  (%d children)' % (c.id, len(c.children)))
            dump(c.children, depth + 1, maxbytes, lines)
        else:
            lines.append('  ' * depth + '%08x  %6d  %s' % (c.id, len(c.data), c.data[:maxbytes].hex()))
    if out is None:
        print('\n'.join(lines))
    return lines
