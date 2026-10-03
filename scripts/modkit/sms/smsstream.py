#!/usr/bin/env python3
"""smsstream.py: Super Mario Strikers (GC) stream files (audio/data/Streams/**.idsp) -> WAV.

  smsstream.py IN.idsp OUT.wav

Same 'IDSP' container as Charged's .nlxwb streams (+04 interleave, +0C/+6C DSP headers, data at
+0xCC, channel blocks interleaved), except that SMS does not pad the last row: each channel's last
block is only its remaining bytes rounded up to 32, and there is no 0x14-byte tail. (Charged's
streamer needs the padded layout; streambank.build_stream writes that.)
"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from streambank import parse_dsp_header, decode_adpcm, write_wav


def read_sms_idsp(blob):
    magic, il = struct.unpack_from('>4sI', blob)
    assert magic == b'IDSP', magic
    nch = 2
    hdrs = [parse_dsp_header(blob[0x0C + 0x60 * c:0x6C + 0x60 * c]) for c in range(nch)]
    bpc = (hdrs[0]['num_nibbles'] + 1) // 2
    full, rem = divmod(bpc, il)
    last = (rem + 31) & ~31
    pcm = []
    for c, h in enumerate(hdrs):
        buf = bytearray()
        for r in range(full):
            o = 0xCC + r * nch * il + c * il
            buf += blob[o:o + il]
        if rem:
            o = 0xCC + full * nch * il + c * last
            buf += blob[o:o + last]
        pcm.append(decode_adpcm(bytes(buf[:bpc]), h['num_samples'], h['coefs'], h['yn1'], h['yn2']))
    expect = 0xCC + full * nch * il + (nch * last if rem else 0)
    return hdrs[0]['rate'], pcm, dict(interleave=il, expected_size=expect, size=len(blob), headers=hdrs)


if __name__ == '__main__':
    rate, pcm, info = read_sms_idsp(open(sys.argv[1], 'rb').read())
    write_wav(sys.argv[2], rate, pcm)
    print('%s: %d Hz, %d samples (%.2fs), interleave 0x%x, size %d (layout predicts %d) -> %s' % (
        sys.argv[1], rate, len(pcm[0]), len(pcm[0]) / rate, info['interleave'], info['size'], info['expected_size'],
        sys.argv[2]))
