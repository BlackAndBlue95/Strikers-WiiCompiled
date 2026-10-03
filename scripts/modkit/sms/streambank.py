#!/usr/bin/env python3
"""streambank.py: Mario Strikers Charged (Wii) *stream* audio banks (<NAME>.resbun + <NAME>.nlxwb):
STREAM_GEN_NIS, STREAM_GEN_Music, STREAM_GEN_Crowd, FE_GEN_Music (STREAM_GEN_LoadScreen / *_Sfx are
resident banks: see resbun.py).

  streambank.py dump   BANK[.resbun] [--names FILE] [--nlxwb PATH] [--append BLOB --nlxwb-size N]
  streambank.py wav    BANK CUE OUT.wav [--nlxwb PATH] [--append BLOB --nlxwb-size N]
  streambank.py encode IN.wav OUT.idsp [--interleave N | --bank BANK]     (WAV rate kept; mono -> 2 ch)
  streambank.py blobwav IN.idsp OUT.wav
  streambank.py merge  BANK OUT_DIR --nlxwb-size N [--template CUE] CUE[,ALIAS...]=BLOB ...
  streambank.py verify BANK MERGED.resbun --append BLOB [--nlxwb ORIG.nlxwb] [--ref CUE=WAV ...]
  streambank.py selftest

CUE is a cue name (hashed with nlStringLowerHash, as NisPlayer::PlayNisCue does) or 0xHASH.

Format (big-endian; decomp references in the report and below)
---------------------------------------------------------------
.resbun = NL chunk tree (see resbun.py for the chunk header rules) with three top-level chunks
  (no 0x23703 SP sound table, unlike a resident bank):
  0x80023000 SoundMap          0x23001 {u32 cueCount, u32 ptr, u32 ptr}
                               0x23003 SoundCue[cueCount] {u32 nameHash, 0, 0, 0, u32 cueIndex}
                               (inserted into an AVL tree at load: SoundMap::ParseChunk; order free)
  0x80023300 ResourceBundle    header(64) + cue(40)/voice(44)/sequence(12)/soundEvent(48)/hit(16)/
                               param(24) arrays + per-entry pointer chunks (AudioResourceBundle.cpp);
                               pointers are relocated against the header's array base pointers.
                               Every NIS cue: 1 voice -> 1 sequence -> 1 sound event -> 1 choice.
  0x80023200 Sources           0x23201 {u32 count, u32 interleave, u8 isStream(=1), 3 junk bytes}
                               0x23202 AudioSourceInfo[count] (28 B):
                                 +00 u32 index (= position)   +04 u32 offset in .nlxwb (32-aligned)
                                 +08 u32 size (bytes)          +0C u32 name hash (= cue name hash)
                                 +10 u32 channels (1 or 2; CreateSource picks the 1-/2-ch reader)
                                 +14 u32 0                     +18 loader ptr (filled at load)
.nlxwb  = the streams back to back (offset/size from 0x23202; contiguous, sizes multiples of 32;
  the file ends at the last stream's end). One stereo stream:
  +00 'IDSP'  +04 u32 interleave (= the bank's)  +08 u32 ~per-channel bytes; none of +00..+0B is read:
      the streamer takes the interleave from the bank's 0x23201 header
  +0C DSP header ch0 (0x60: num_samples, num_nibbles, rate, u16 loop 0, u16 fmt 0, sa 2,
      ea num_nibbles-1, ca 2, coefs[16], gain 0, ps, yn1 0, yn2 0, lps 0, lyn1 0, lyn2 0, pad[11])
  +6C DSP header ch1
  +CC rows x (ch0 block[interleave], ch1 block[interleave]); rows = ceil(num_nibbles/2 / interleave);
      each channel is one continuous DSP-ADPCM stream (one coef set, history carried across blocks:
      AX_VOICE_STREAM); the unused tail of the last blocks is padding (zeros here)
  +CC + rows*2*interleave: 0x14 bytes of '0' (0x30) padding  -> size = 0xE0 + rows*2*interleave
  The game (AudioReadState::Prepare/Update + fn_8035FE6C/fn_80360120 in the DOL) reads each 0x60
  header at offset+12+ch*0x60 (offset+ch*0x60 for a mono stream), takes per-channel bytes =
  num_nibbles/2, then streams interleave-sized reads (the last one rounded up to 32 bytes) from
  offset+12+nch*0x60 + done*nch + ch*interleave into a 2*interleave ARAM ring per channel; the
  AX voice runs at rate/32000 and loops the ring; loop count comes from the sound event (1 = once).
  Mono streams (none shipped): header at offset+0, data at offset+0x60 per the reader -- unverified,
  so build_stream writes stereo only.
"""
import array, ctypes, hashlib, json, os, struct, subprocess, sys, tempfile, wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import resbun
from resbun import nl_hash, nl_lower_hash

ID_ROOT, ID_SMAP, ID_BUNDLE, ID_SOURCES = 0x80000001, 0x80023000, 0x80023300, 0x80023200
REC_SIZE = {0x23302: 40, 0x23303: 44, 0x23304: 12, 0x23305: 48, 0x23306: 16, 0x23307: 24}
HDR_SIZE = 0x60
TAIL = b'0' * 0x14


# =============================================================== chunk tree (lossless)
class Node:
    """One NL chunk: raw id (alignment bits kept) and either payload bytes or children."""
    __slots__ = ('raw_id', 'data', 'children')

    def __init__(self, raw_id, data=None, children=None):
        self.raw_id, self.data, self.children = raw_id, data, children

    @property
    def id(self):
        return self.raw_id & 0x80FFFFFF

    def __repr__(self):
        return '<Node %08x %s>' % (self.raw_id, ('%d children' % len(self.children)) if self.children is not None
                                   else ('%d bytes' % len(self.data)))


def parse_tree(d, off=0, end=None):
    end = len(d) if end is None else end
    out = []
    while off + 8 <= end:
        c = resbun.Chunk(d, off)
        if c.raw_id & 0x80000000:
            out.append(Node(c.raw_id, children=parse_tree(d, off + 8, c.end)))
        else:
            out.append(Node(c.raw_id, data=bytes(d[c.payload:c.end])))
        off = c.next_off()
    return out


def serialize(node, pos=0):
    al = (node.raw_id >> 24) & 0xF
    pad = (-(pos + 8)) % (1 << al) if al else 0
    if node.children is not None:
        body = bytearray()
        start = pos + 8 + pad
        for i, ch in enumerate(node.children):
            body += serialize(ch, start + len(body))
            if i < len(node.children) - 1:
                body += b'\0' * (-(start + len(body)) % 4)
    else:
        body = node.data
    return struct.pack('>II', node.raw_id, pad + len(body)) + b'\0' * pad + bytes(body)


def find(nodes, cid):
    return next(n for n in nodes if n.id == cid)


# =============================================================== DSP-ADPCM (pure Python)
def _clamp16(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def _cdiv(a, b):
    """C integer division (truncates toward zero)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def py_decode(data, nsamples, coefs, h1=0, h2=0):
    out = array.array('h')
    for f in range(0, len(data), 8):
        hdr = data[f]
        scale = 1 << (hdr & 0xF)
        ci = (hdr >> 4) & 7
        c1, c2 = coefs[ci * 2], coefs[ci * 2 + 1]
        for i in range(14):
            if len(out) >= nsamples:
                return out
            b = data[f + 1 + (i >> 1)]
            nib = (b >> 4) if not (i & 1) else (b & 0xF)
            if nib >= 8:
                nib -= 16
            v = _clamp16((((nib * scale) << 11) + 1024 + c1 * h1 + c2 * h2) >> 11)
            out.append(v)
            h2, h1 = h1, v
    return out


# --- encoder: a port of the GameCube SDK-equivalent DSPADPCM encoder (DSPCorrelateCoefs /
#     DSPEncodeFrame, as in jackoalan's DSPTool / vgmstream's tools), so the output matches what
#     Nintendo's dspadpcm tool would produce for the same input.
def _inner_product(buf):           # buf: 16 samples, buf[2:] = frame, buf[0:2] = previous two
    return [-sum(buf[2 + x - i] * buf[2 + x] for x in range(14)) for i in range(3)]


def _outer_product(buf):
    m = [[0.0] * 3 for _ in range(3)]
    for x in (1, 2):
        for y in (1, 2):
            m[x][y] = float(sum(buf[2 + z - x] * buf[2 + z - y] for z in range(14)))
    return m


def _analyze_ranges(mtx, idx):
    recips = [0.0] * 3
    for x in (1, 2):
        val = max(abs(mtx[x][1]), abs(mtx[x][2]))
        if val < 2.220446049250313e-16:
            return True
        recips[x] = 1.0 / val
    max_index = 0
    for i in (1, 2):
        for x in range(1, i):
            tmp = mtx[x][i]
            for y in range(1, x):
                tmp -= mtx[x][y] * mtx[y][i]
            mtx[x][i] = tmp
        val = 0.0
        for x in range(i, 3):
            tmp = mtx[x][i]
            for y in range(1, i):
                tmp -= mtx[x][y] * mtx[y][i]
            mtx[x][i] = tmp
            tmp = abs(tmp) * recips[x]
            if tmp >= val:
                val = tmp
                max_index = x
        if max_index != i:
            for y in (1, 2):
                mtx[max_index][y], mtx[i][y] = mtx[i][y], mtx[max_index][y]
            recips[max_index] = recips[i]
        idx[i] = max_index
        if mtx[i][i] == 0.0:
            return True
        if i != 2:
            tmp = 1.0 / mtx[i][i]
            for x in range(i + 1, 3):
                mtx[x][i] *= tmp
    mn, mx = 1.0e10, 0.0
    for i in (1, 2):
        tmp = abs(mtx[i][i])
        mn = min(mn, tmp)
        mx = max(mx, tmp)
    return mn / mx < 1.0e-10


def _bidirectional_filter(mtx, idx, vec):
    x = 0
    for i in (1, 2):
        index = idx[i]
        tmp = vec[index]
        vec[index] = vec[i]
        if x != 0:
            for y in range(x, i):
                tmp -= vec[y] * mtx[i][y]
        elif tmp != 0.0:
            x = i
        vec[i] = tmp
    for i in (2, 1):
        tmp = vec[i]
        for y in range(i + 1, 3):
            tmp -= vec[y] * mtx[i][y]
        vec[i] = tmp / mtx[i][i]
    vec[0] = 1.0


def _quadratic_merge(vec):
    v2 = vec[2]
    tmp = 1.0 - v2 * v2
    if tmp == 0.0:
        return True
    v0 = (vec[0] - v2 * v2) / tmp
    v1 = (vec[1] - vec[1] * v2) / tmp
    vec[0], vec[1] = v0, v1
    return abs(v1) > 1.0


def _finish_record(inp, out):
    for z in (1, 2):
        if inp[z] >= 1.0:
            inp[z] = 0.9999999999
        elif inp[z] <= -1.0:
            inp[z] = -0.9999999999
    out[0] = 1.0
    out[1] = inp[2] * inp[1] + inp[1]
    out[2] = inp[2]


def _matrix_filter(src, dst):
    mtx = [[0.0] * 3 for _ in range(3)]
    mtx[2][0] = 1.0
    for i in (1, 2):
        mtx[2][i] = -src[i]
    for i in (2, 1):
        val = 1.0 - mtx[i][i] * mtx[i][i]
        for y in range(1, i + 1):
            mtx[i - 1][y] = (mtx[i][i] * mtx[i][y] + mtx[i][y]) / val
    dst[0] = 1.0
    for i in (1, 2):
        dst[i] = 0.0
        for y in range(1, i + 1):
            dst[i] += mtx[i][y] * dst[i - y]


def _merge_finish_record(src, dst):
    tmp = [0.0] * 3
    val = src[0]
    dst[0] = 1.0
    for i in (1, 2):
        v2 = 0.0
        for y in range(1, i):
            v2 += dst[y] * src[i - y]
        dst[i] = -(v2 + src[i]) / val if val > 0.0 else 0.0
        tmp[i] = dst[i]
        for y in range(1, i):
            dst[y] += dst[i] * dst[i - y]
        val *= 1.0 - dst[i] * dst[i]
    _finish_record(tmp, dst)


def _contrast(s1, s2):
    val = (s2[2] * s2[1] + -s2[1]) / (1.0 - s2[2] * s2[2])
    val1 = s1[0] * s1[0] + s1[1] * s1[1] + s1[2] * s1[2]
    val2 = s1[0] * s1[1] + s1[1] * s1[2]
    val3 = s1[0] * s1[2]
    return val1 + 2.0 * val * val2 + 2.0 * (-s2[1] * val + -s2[2]) * val3


def _filter_records(best, exp, records):
    for _ in range(2):
        counts = [0] * exp
        acc = [[0.0] * 3 for _ in range(exp)]
        buf = [0.0] * 3
        for r in records:
            index, value = 0, 1.0e30
            for i in range(exp):
                t = _contrast(best[i], r)
                if t < value:
                    value, index = t, i
            counts[index] += 1
            _matrix_filter(r, buf)
            for i in range(3):
                acc[index][i] += buf[i]
        for i in range(exp):
            if counts[i] > 0:
                for y in range(3):
                    acc[i][y] /= counts[i]
        for i in range(exp):
            _merge_finish_record(acc[i], best[i])


def _lround(d):
    return int(d + 0.5) if d >= 0 else -int(-d + 0.5)


def py_correlate_coefs(src):
    n = len(src)
    records = []
    hist = [0, 0]
    for f in range(0, n, 14):
        frame = list(src[f:f + 14])
        frame += [0] * (14 - len(frame))
        buf = hist + frame
        hist = frame[12:14]
        vec1 = _inner_product(buf)
        if abs(vec1[0]) > 10.0:
            mtx = _outer_product(buf)
            idx = [0, 0, 0]
            if not _analyze_ranges(mtx, idx):
                vec1 = [float(v) for v in vec1]
                _bidirectional_filter(mtx, idx, vec1)
                if not _quadratic_merge(vec1):
                    rec = [0.0] * 3
                    _finish_record(vec1, rec)
                    records.append(rec)
    best = [[0.0] * 3 for _ in range(8)]
    vec1 = [1.0, 0.0, 0.0]
    tmpv = [0.0] * 3
    for r in records:
        _matrix_filter(r, tmpv)
        for y in (1, 2):
            vec1[y] += tmpv[y]
    for y in (1, 2):
        vec1[y] /= max(len(records), 1)
    _merge_finish_record(vec1, best[0])
    exp = 1
    w = 0
    while w < 3:
        vec2 = [0.0, -1.0, 0.0]
        for i in range(exp):
            for y in range(3):
                best[exp + i][y] = 0.01 * vec2[y] + best[i][y]
        w += 1
        exp = 1 << w
        _filter_records(best, exp, records)
    out = []
    for z in range(8):
        for k in (1, 2):
            d = -best[z][k] * 2048.0
            out.append(max(-32768, min(32767, _lround(d))))
    return out


_HALF = struct.unpack('>f', struct.pack('>f', 0.4999999))[0]     # the reference encoder's 0.4999999f


def _encode_frame(pcm, count, coefs):
    """pcm: 16 ints (2 history + 14); returns (8 bytes, decoded 14 samples)."""
    best_dist, best = None, None
    for i in range(8):
        c1, c2 = coefs[2 * i], coefs[2 * i + 1]
        ins = [pcm[0], pcm[1]] + [0] * 14
        distance = 0
        for s in range(count):
            v1 = _cdiv(pcm[s] * c2 + pcm[s + 1] * c1, 2048)
            ins[s + 2] = v1
            v3 = _clamp16(pcm[s + 2] - v1)
            if abs(v3) > abs(distance):
                distance = v3
        scale = 0
        while scale <= 12 and (distance > 7 or distance < -8):
            scale += 1
            distance = _cdiv(distance, 2)
        scale = -1 if scale <= 1 else scale - 2
        while True:
            scale += 1
            dist_acc = 0.0
            index = 0
            outs = [0] * 14
            for s in range(count):
                v1 = ins[s] * c2 + ins[s + 1] * c1
                v2 = (pcm[s + 2] << 11) - v1
                v3 = int(v2 / (1 << scale) / 2048 + _HALF) if v2 > 0 else int(v2 / (1 << scale) / 2048 - _HALF)
                if v3 < -8:
                    if index < -8 - v3:
                        index = -8 - v3
                    v3 = -8
                elif v3 > 7:
                    if index < v3 - 7:
                        index = v3 - 7
                    v3 = 7
                outs[s] = v3
                v1 = (v1 + ((v3 * (1 << scale)) << 11) + 1024) >> 11
                v2 = _clamp16(v1)
                ins[s + 2] = v2
                v3 = pcm[s + 2] - v2
                dist_acc += v3 * float(v3)
            x = index + 8
            while x > 256:
                scale += 1
                if scale >= 12:
                    scale = 11
                x >>= 1
            if not (scale < 12 and index > 1):
                break
        if best_dist is None or dist_acc < best_dist:
            best_dist, best = dist_acc, (i, scale, outs, ins)
    i, scale, outs, ins = best
    b = bytes([(i << 4) | (scale & 0xF)] + [((outs[2 * y] << 4) | (outs[2 * y + 1] & 0xF)) & 0xFF for y in range(7)])
    return b, ins[2:2 + count]


def py_encode(src, coefs):
    out = bytearray()
    h = [0, 0]
    n = len(src)
    for f in range(0, n, 14):
        frame = list(src[f:f + 14])
        cnt = len(frame)
        frame += [0] * (14 - cnt)
        b, dec = _encode_frame(h + frame, cnt, coefs)
        out += b
        full = h + dec + frame[cnt:]
        h = [full[cnt], full[cnt + 1]]
    return bytes(out)


# =============================================================== DSP-ADPCM (optional C build)
C_SRC = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
typedef double tvec[3];
static void inner(tvec o, const short* p){for(int i=0;i<=2;i++){o[i]=0.0;for(int x=0;x<14;x++)o[i]-=(double)p[x-i]*p[x];}}
static void outer(tvec m[3], const short* p){for(int x=1;x<=2;x++)for(int y=1;y<=2;y++){m[x][y]=0.0;for(int z=0;z<14;z++)m[x][y]+=(double)p[z-x]*p[z-y];}}
static int analyze(tvec m[3], int* vi){double r[3],val,tmp,mn,mx;for(int x=1;x<=2;x++){val=fmax(fabs(m[x][1]),fabs(m[x][2]));if(val<DBL_EPSILON)return 1;r[x]=1.0/val;}
 int mi=0;for(int i=1;i<=2;i++){for(int x=1;x<i;x++){tmp=m[x][i];for(int y=1;y<x;y++)tmp-=m[x][y]*m[y][i];m[x][i]=tmp;}
 val=0.0;for(int x=i;x<=2;x++){tmp=m[x][i];for(int y=1;y<i;y++)tmp-=m[x][y]*m[y][i];m[x][i]=tmp;tmp=fabs(tmp)*r[x];if(tmp>=val){val=tmp;mi=x;}}
 if(mi!=i){for(int y=1;y<=2;y++){tmp=m[mi][y];m[mi][y]=m[i][y];m[i][y]=tmp;}r[mi]=r[i];}
 vi[i]=mi;if(m[i][i]==0.0)return 1;if(i!=2){tmp=1.0/m[i][i];for(int x=i+1;x<=2;x++)m[x][i]*=tmp;}}
 mn=1.0e10;mx=0.0;for(int i=1;i<=2;i++){tmp=fabs(m[i][i]);if(tmp<mn)mn=tmp;if(tmp>mx)mx=tmp;}return mn/mx<1.0e-10;}
static void bidir(tvec m[3], int* vi, tvec v){double tmp;for(int i=1,x=0;i<=2;i++){int ix=vi[i];tmp=v[ix];v[ix]=v[i];if(x!=0)for(int y=x;y<=i-1;y++)tmp-=v[y]*m[i][y];else if(tmp!=0.0)x=i;v[i]=tmp;}
 for(int i=2;i>0;i--){tmp=v[i];for(int y=i+1;y<=2;y++)tmp-=v[y]*m[i][y];v[i]=tmp/m[i][i];}v[0]=1.0;}
static int quad(tvec v){double v0,v1,v2=v[2];double tmp=1.0-(v2*v2);if(tmp==0.0)return 1;v0=(v[0]-(v2*v2))/tmp;v1=(v[1]-(v[1]*v2))/tmp;v[0]=v0;v[1]=v1;return fabs(v1)>1.0;}
static void finish(tvec in, tvec out){for(int z=1;z<=2;z++){if(in[z]>=1.0)in[z]=0.9999999999;else if(in[z]<=-1.0)in[z]=-0.9999999999;}out[0]=1.0;out[1]=(in[2]*in[1])+in[1];out[2]=in[2];}
static void mfilt(tvec s, tvec d){tvec m[3];m[2][0]=1.0;for(int i=1;i<=2;i++)m[2][i]=-s[i];for(int i=2;i>0;i--){double val=1.0-(m[i][i]*m[i][i]);for(int y=1;y<=i;y++)m[i-1][y]=((m[i][i]*m[i][y])+m[i][y])/val;}
 d[0]=1.0;for(int i=1;i<=2;i++){d[i]=0.0;for(int y=1;y<=i;y++)d[i]+=m[i][y]*d[i-y];}}
static void mfinish(tvec s, tvec d){tvec tmp;double val=s[0];d[0]=1.0;for(int i=1;i<=2;i++){double v2=0.0;for(int y=1;y<i;y++)v2+=d[y]*s[i-y];if(val>0.0)d[i]=-(v2+s[i])/val;else d[i]=0.0;tmp[i]=d[i];for(int y=1;y<i;y++)d[y]+=d[i]*d[i-y];val*=1.0-(d[i]*d[i]);}finish(tmp,d);}
static double contrast(tvec a, tvec b){double val=(b[2]*b[1]+-b[1])/(1.0-b[2]*b[2]);double v1=(a[0]*a[0])+(a[1]*a[1])+(a[2]*a[2]);double v2=(a[0]*a[1])+(a[1]*a[2]);double v3=a[0]*a[2];return v1+(2.0*val*v2)+(2.0*(-b[1]*val+-b[2])*v3);}
static void frecs(tvec best[8], int exp, tvec* recs, int n){tvec bl[8];int b1[8];tvec b2;for(int x=0;x<2;x++){for(int y=0;y<exp;y++){b1[y]=0;for(int i=0;i<=2;i++)bl[y][i]=0.0;}
 for(int z=0;z<n;z++){int index=0;double value=1.0e30;for(int i=0;i<exp;i++){double t=contrast(best[i],recs[z]);if(t<value){value=t;index=i;}}b1[index]++;mfilt(recs[z],b2);for(int i=0;i<=2;i++)bl[index][i]+=b2[i];}
 for(int i=0;i<exp;i++)if(b1[i]>0)for(int y=0;y<=2;y++)bl[i][y]/=b1[i];for(int i=0;i<exp;i++)mfinish(bl[i],best[i]);}}
void dsp_correlate_coefs(const short* src, int n, short* out){
 int nf=(n+13)/14;tvec* recs=(tvec*)calloc(sizeof(tvec),nf*2+2);int rc=0;short hb[2][14];memset(hb,0,sizeof hb);tvec v1,v2,m[3],best[8];int vi[3];
 for(int f=0;f<n;f+=14){for(int z=0;z<14;z++)hb[0][z]=hb[1][z];for(int z=0;z<14;z++)hb[1][z]=(f+z<n)?src[f+z]:0;
  inner(v1,hb[1]);if(fabs(v1[0])>10.0){outer(m,hb[1]);if(!analyze(m,vi)){bidir(m,vi,v1);if(!quad(v1)){finish(v1,recs[rc]);rc++;}}}}
 v1[0]=1.0;v1[1]=0.0;v1[2]=0.0;for(int z=0;z<rc;z++){mfilt(recs[z],best[0]);for(int y=1;y<=2;y++)v1[y]+=best[0][y];}for(int y=1;y<=2;y++)v1[y]/=(rc?rc:1);
 mfinish(v1,best[0]);int exp=1;for(int w=0;w<3;){v2[0]=0.0;v2[1]=-1.0;v2[2]=0.0;for(int i=0;i<exp;i++)for(int y=0;y<=2;y++)best[exp+i][y]=(0.01*v2[y])+best[i][y];++w;exp=1<<w;frecs(best,exp,recs,rc);}
 for(int z=0;z<8;z++){for(int k=1;k<=2;k++){double d=-best[z][k]*2048.0;long r=(d>=0)?(long)(d+0.5):-(long)(-d+0.5);if(r>32767)r=32767;if(r<-32768)r=-32768;out[z*2+k-1]=(short)r;}}free(recs);}
static void encframe(int* pcm, int cnt, unsigned char* o, const short* cf){int ins[8][16];int outs[8][14];int bi=0;int sc[8];double da[8];
 for(int i=0;i<8;i++){int v1,v2,v3,dist,index;ins[i][0]=pcm[0];ins[i][1]=pcm[1];dist=0;
  for(int s=0;s<cnt;s++){ins[i][s+2]=v1=((pcm[s]*cf[i*2+1])+(pcm[s+1]*cf[i*2]))/2048;v2=pcm[s+2]-v1;v3=(v2>=32767)?32767:(v2<=-32768)?-32768:v2;if(abs(v3)>abs(dist))dist=v3;}
  for(sc[i]=0;(sc[i]<=12)&&((dist>7)||(dist<-8));sc[i]++,dist/=2){}sc[i]=(sc[i]<=1)?-1:sc[i]-2;
  do{sc[i]++;da[i]=0;index=0;for(int s=0;s<cnt;s++){v1=((ins[i][s]*cf[i*2+1])+(ins[i][s+1]*cf[i*2]));v2=(pcm[s+2]<<11)-v1;
    v3=(v2>0)?(int)((double)v2/(1<<sc[i])/2048+0.4999999f):(int)((double)v2/(1<<sc[i])/2048-0.4999999f);
    if(v3<-8){if(index<(v3=-8-v3))index=v3;v3=-8;}else if(v3>7){if(index<(v3-=7))index=v3;v3=7;}
    outs[i][s]=v3;v1=(v1+((v3*(1<<sc[i]))<<11)+1024)>>11;ins[i][s+2]=v2=(v1>=32767)?32767:(v1<=-32768)?-32768:v1;v3=pcm[s+2]-v2;da[i]+=v3*(double)v3;}
   for(int x=index+8;x>256;x>>=1)if(++sc[i]>=12)sc[i]=11;}while((sc[i]<12)&&(index>1));}
 double mn=DBL_MAX;for(int i=0;i<8;i++){if(da[i]<mn){mn=da[i];bi=i;}}
 for(int s=0;s<cnt;s++)pcm[s+2]=ins[bi][s+2];o[0]=(unsigned char)((bi<<4)|(sc[bi]&0xF));for(int s=cnt;s<14;s++)outs[bi][s]=0;
 for(int y=0;y<7;y++)o[y+1]=(unsigned char)((outs[bi][y*2]<<4)|(outs[bi][y*2+1]&0xF));}
void dsp_encode(const short* src, int n, const short* cf, unsigned char* out){int pcm[16];pcm[0]=pcm[1]=0;
 for(int f=0,o=0;f<n;f+=14,o+=8){int cnt=(n-f<14)?(n-f):14;for(int z=0;z<14;z++)pcm[2+z]=(z<cnt)?src[f+z]:0;encframe(pcm,cnt,out+o,cf);
  if(cnt==14){pcm[0]=pcm[14];pcm[1]=pcm[15];}else if(cnt==1){pcm[0]=pcm[1];pcm[1]=pcm[2];}else{pcm[0]=pcm[cnt];pcm[1]=pcm[cnt+1];}}}
void dsp_decode(const unsigned char* d, int n, const short* cf, int h1, int h2, short* out){int k=0;for(int f=0;k<n;f+=8){int hd=d[f];int sc=1<<(hd&0xF);int ci=(hd>>4)&7;int c1=cf[ci*2],c2=cf[ci*2+1];
 for(int i=0;i<14&&k<n;i++){int b=d[f+1+(i>>1)];int nib=(i&1)?(b&0xF):(b>>4);if(nib>=8)nib-=16;int v=(((nib*sc)<<11)+1024+c1*h1+c2*h2)>>11;if(v<-32768)v=-32768;if(v>32767)v=32767;out[k++]=(short)v;h2=h1;h1=v;}}}
'''

_LIB = None


def _lib():
    """Compile C_SRC once (cached in __pycache__ beside this file); None if no compiler -> pure Python."""
    global _LIB
    if _LIB is not None:
        return _LIB or None
    if os.environ.get('STREAMBANK_PURE_PYTHON'):
        _LIB = False
        return None
    tag = hashlib.sha1(C_SRC.encode()).hexdigest()[:12]
    cache = os.environ.get('STREAMBANK_CACHE') or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                                 '__pycache__')
    try:
        os.makedirs(cache, exist_ok=True)
    except OSError:
        cache = tempfile.gettempdir()
    path = os.path.join(cache, 'streambank_dsp_%s.so' % tag)
    try:
        if not os.path.exists(path):
            src = path[:-3] + '.c'
            open(src, 'w').write(C_SRC)
            subprocess.check_call(['cc', '-O2', '-shared', '-fPIC', '-o', path, src],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        lib = ctypes.CDLL(path)
        lib.dsp_correlate_coefs.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
        lib.dsp_encode.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p]
        lib.dsp_decode.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_void_p]
        _LIB = lib
    except Exception:
        _LIB = False
    return _LIB or None


def _buf(a):
    return (ctypes.c_char * (len(a) * a.itemsize)).from_buffer(a)


def correlate_coefs(pcm):
    pcm = array.array('h', pcm)
    lib = _lib()
    if lib and len(pcm):
        out = array.array('h', [0] * 16)
        lib.dsp_correlate_coefs(ctypes.addressof(_buf(pcm)), len(pcm), ctypes.addressof(_buf(out)))
        return list(out)
    return py_correlate_coefs(pcm)


def encode_adpcm(pcm, coefs):
    pcm = array.array('h', pcm)
    lib = _lib()
    if lib and len(pcm):
        out = bytearray(8 * ((len(pcm) + 13) // 14))
        cf = array.array('h', coefs)
        lib.dsp_encode(ctypes.addressof(_buf(pcm)), len(pcm), ctypes.addressof(_buf(cf)),
                       ctypes.addressof((ctypes.c_char * len(out)).from_buffer(out)))
        return bytes(out)
    return py_encode(pcm, coefs)


def decode_adpcm(data, nsamples, coefs, h1=0, h2=0):
    lib = _lib()
    if lib and nsamples:
        need = 8 * ((nsamples + 13) // 14)
        data = bytes(data[:need]).ljust(need, b'\0')
        out = array.array('h', [0] * nsamples)
        cf = array.array('h', coefs)
        src = ctypes.create_string_buffer(data, len(data))
        lib.dsp_decode(src, nsamples, ctypes.addressof(_buf(cf)), h1, h2, ctypes.addressof(_buf(out)))
        return out
    return py_decode(data, nsamples, coefs, h1, h2)


def nibbles_for(nsamples):
    full, rem = divmod(nsamples, 14)
    return full * 16 + (rem + 2 if rem else 0)


# =============================================================== stream blobs
def dsp_header(nsamples, rate, coefs, ps):
    nn = nibbles_for(nsamples)
    return struct.pack('>IIIHHIII16hHHhhHhh22x', nsamples, nn, rate, 0, 0, 2, nn - 1, 2, *coefs, 0, ps, 0, 0, 0, 0, 0)


def parse_dsp_header(b):
    v = struct.unpack_from('>IIIHHIII16hHHhhHhh', b, 0)
    return dict(num_samples=v[0], num_nibbles=v[1], rate=v[2], loop_flag=v[3], format=v[4], sa=v[5], ea=v[6],
                ca=v[7], coefs=list(v[8:24]), gain=v[24], ps=v[25], yn1=v[26], yn2=v[27], lps=v[28], lyn1=v[29],
                lyn2=v[30])


def build_stream(channels_pcm, rate, interleave, progress=None):
    """channels_pcm: list (1 or 2) of int16 sequences, equal length -> the IDSP stream bytes."""
    nch = len(channels_pcm)
    if nch != 2:
        raise ValueError('only stereo streams are written (every shipped stream is stereo; the mono layout '
                         'is unverified) -- duplicate a mono channel')
    if interleave % 32:
        raise ValueError('interleave must be a multiple of 32')
    n = len(channels_pcm[0])
    assert all(len(c) == n for c in channels_pcm)
    coded, hdrs = [], []
    for ci, pcm in enumerate(channels_pcm):
        coefs = correlate_coefs(pcm)
        adp = encode_adpcm(pcm, coefs)
        coded.append(adp)
        hdrs.append(dsp_header(n, rate, coefs, adp[0] if adp else 0))
        if progress:
            progress(ci)
    bpc = nibbles_for(n) // 2 + (nibbles_for(n) & 1)       # bytes holding num_nibbles nibbles
    rows = max(1, -(-bpc // interleave))
    out = bytearray(b'IDSP' + struct.pack('>II', interleave, (n // 14) * 8))
    for h in hdrs:
        out += h
    for r in range(rows):
        for adp in coded:
            blk = adp[r * interleave:(r + 1) * interleave]
            out += blk + b'\0' * (interleave - len(blk))
    out += TAIL
    assert len(out) == 0xE0 + rows * nch * interleave and len(out) % 32 == 0
    return bytes(out)


def read_stream(read, offset, nch, interleave):
    """Decode one stream given read(off, n) over the (virtual) .nlxwb -> (rate, [pcm per channel], headers)."""
    base = offset + (12 if nch == 2 else 0)
    hdrs = [parse_dsp_header(read(base + c * HDR_SIZE, HDR_SIZE)) for c in range(nch)]
    data0 = base + nch * HDR_SIZE
    bpc = (hdrs[0]['num_nibbles'] + 1) // 2
    rows = -(-bpc // interleave)
    pcm = []
    for c, h in enumerate(hdrs):
        buf = bytearray()
        for r in range(rows):
            buf += read(data0 + r * nch * interleave + c * interleave, interleave)
        pcm.append(decode_adpcm(bytes(buf[:bpc]), h['num_samples'], h['coefs'], h['yn1'], h['yn2']))
    return hdrs[0]['rate'], pcm, hdrs


def parse_blob(blob):
    magic, il, f8 = struct.unpack_from('>4sII', blob, 0)
    assert magic == b'IDSP', magic
    return read_stream(lambda o, n: blob[o:o + n], 0, 2, il)


# =============================================================== WAV helpers
def read_wav(path):
    with wave.open(path, 'rb') as w:
        nch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if sw != 2:
        raise ValueError('%s: only 16-bit PCM WAV' % path)
    a = array.array('h', raw)
    if sys.byteorder == 'big':
        a.byteswap()
    return rate, [a[c::nch] for c in range(nch)]


def write_wav(path, rate, chans):
    n = min(len(c) for c in chans)
    inter = array.array('h', [0] * (n * len(chans)))
    for c, ch in enumerate(chans):
        inter[c::len(chans)] = array.array('h', ch[:n])
    if sys.byteorder == 'big':
        inter.byteswap()
    with wave.open(path, 'wb') as w:
        w.setnchannels(len(chans))
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(inter.tobytes())


# =============================================================== the bank
class VirtualFile:
    """The original .nlxwb (optional: only needed to read original streams) + an appended blob that
    starts at file offset `orig_size` -- what the runtime has to serve as one file."""

    def __init__(self, nlxwb=None, orig_size=None, append=None):
        self.f = open(nlxwb, 'rb') if nlxwb else None
        self.orig_size = orig_size if orig_size is not None else (os.path.getsize(nlxwb) if nlxwb else 0)
        self.app = open(append, 'rb').read() if isinstance(append, str) else (append or b'')

    def read(self, off, n):
        out = bytearray()
        if off < self.orig_size:
            if self.f is None:
                raise IOError('offset 0x%x is in the original .nlxwb (not given)' % off)
            k = min(n, self.orig_size - off)
            self.f.seek(off)
            out += self.f.read(k)
            off += k
            n -= k
        if n > 0:
            a = off - self.orig_size
            out += self.app[a:a + n]
        return bytes(out)


class StreamBank:
    def __init__(self, resbun_path):
        self.path = resbun_path
        self.d = open(resbun_path, 'rb').read()
        self.root = parse_tree(self.d)[0]
        assert self.root.id == ID_ROOT
        if serialize(self.root) != self.d:
            raise ValueError('%s: chunk tree does not re-serialize identically' % resbun_path)
        top = self.root.children
        self.smap = find(top, ID_SMAP)
        self.bundle = find(top, ID_BUNDLE)
        self.srcnode = find(top, ID_SOURCES)
        if any(n.id == 0x23703 for n in top):
            raise ValueError('%s has an SP sound table: a resident bank (use resbun.py)' % resbun_path)
        self._parse()

    def _parse(self):
        sm_hdr, sm_cues = self.smap.children
        n = struct.unpack_from('>I', sm_hdr.data)[0]
        self.soundmap = [struct.unpack_from('>5I', sm_cues.data, 20 * i) for i in range(n)]
        ch = self.bundle.children
        self.hdr = list(struct.unpack('>16I', ch[0].data))
        H = self.hdr
        self.counts = dict(cues=H[2], voices=H[4], seqs=H[6], events=H[8], hits=H[10], params=H[12])
        self.base = dict(cues=H[3], voices=H[5], seqs=H[7], events=H[9], hits=H[11], params=H[13])
        self.arr = {c.id: c for c in ch[1:7]}
        rest = ch[7:]
        k = 0
        self.cue_entries = rest[k:k + H[2]]; k += H[2]
        self.voice_chunks = [(rest[k + 2 * i], rest[k + 2 * i + 1]) for i in range(H[4])]; k += 2 * H[4]
        self.seq_chunks = rest[k:k + H[6]]; k += H[6]
        self.event_chunks = rest[k:k + H[8]]; k += H[8]
        assert k == len(rest), (k, len(rest))
        assert all(c.id == 0x23308 for c in self.cue_entries) and all(c.id == 0x2330A for c in self.seq_chunks)
        assert all(a.id == 0x23309 and b.id == 0x2330C for a, b in self.voice_chunks)
        assert all(c.id == 0x2330B for c in self.event_chunks)
        sh, se = self.srcnode.children
        self.src_hdr = sh.data
        cnt, self.interleave = struct.unpack_from('>II', sh.data)
        self.is_stream = sh.data[8]
        self.sources = [dict(zip(('index', 'offset', 'size', 'name', 'channels', 'f14', 'loader'),
                                 struct.unpack_from('>7I', se.data, 28 * i))) for i in range(cnt)]

    # ---- resolution: cue -> voices -> sequences -> sound events -> choices -> sources
    def rec(self, cid, i):
        sz = REC_SIZE[cid]
        return self.arr[cid].data[sz * i:sz * (i + 1)]

    def cue_name(self, ci):
        return struct.unpack_from('>I', self.rec(0x23302, ci))[0]

    def cue_voices(self, ci):
        nv = struct.unpack_from('>I', self.rec(0x23302, ci), 4)[0]
        e = self.cue_entries[ci].data
        out = []
        for j in range(nv):
            vp = struct.unpack_from('>I', e, 20 * j)[0]
            off = vp - self.base['voices']
            assert off % 44 == 0
            out.append(off // 44)
        return out

    def voice_sources(self, vi):
        seqp = self.voice_chunks[vi][0].data
        out = []
        for j in range(len(seqp) // 4):
            si = (struct.unpack_from('>I', seqp, 4 * j)[0] - self.base['seqs']) // 12
            nev = struct.unpack_from('>I', self.rec(0x23304, si), 4)[0]
            evd = self.seq_chunks[si].data
            for k in range(nev):
                typ, ptr = struct.unpack_from('>iI', evd, 8 * k)
                if typ != 1:
                    continue
                ei = (ptr - self.base['events']) // 48
                nch = struct.unpack_from('>I', self.rec(0x23305, ei), 8)[0]
                chd = self.event_chunks[ei].data
                for m in range(nch):
                    out.append(struct.unpack_from('>I', chd, 8 * m)[0])
        return out

    def cue_index(self, key):
        h = key if isinstance(key, int) else (int(key, 16) if key.lower().startswith('0x') else nl_lower_hash(key))
        for k0, k1, k2, k3, ci in self.soundmap:
            if k0 == h:
                return ci
        return None

    def cue_sources(self, ci):
        return [s for v in self.cue_voices(ci) for s in self.voice_sources(v)]

    def resolve_all(self):
        """{cue hash: [(source fields)]} -- what verify compares."""
        out = {}
        for k0, k1, k2, k3, ci in self.soundmap:
            out[k0] = [tuple(self.sources[s][k] for k in ('offset', 'size', 'name', 'channels'))
                       for s in self.cue_sources(ci)]
        return out

    # ---- building
    def nlxwb_end(self):
        return max((s['offset'] + s['size'] for s in self.sources), default=0)

    def merged(self, new_cues, nlxwb_size, align=32, template=None):
        """new_cues: list of (names [primary, aliases...], blob bytes). Returns (resbun bytes, append bytes,
        manifest). The new streams start at nlxwb_size rounded up to `align`; the append blob begins at
        file offset nlxwb_size (it includes that rounding padding). The new cue/voice/sequence/event
        records copy `template`'s (a cue name or hash; default: the first cue)."""
        known = {k0 for k0, *_ in self.soundmap}
        H = list(self.hdr)
        tmpl_ci = self.cue_index(template) if template is not None else self.soundmap[0][4]
        if tmpl_ci is None:
            raise ValueError('template cue %s not in the bank' % template)
        v0 = self.cue_voices(tmpl_ci)[0]
        cue_t, voice_t = self.rec(0x23302, tmpl_ci), self.rec(0x23303, v0)
        seq_i = (struct.unpack_from('>I', self.voice_chunks[v0][0].data)[0] - self.base['seqs']) // 12
        seq_t = self.rec(0x23304, seq_i)
        ev_i = (struct.unpack_from('>iI', self.seq_chunks[seq_i].data)[1] - self.base['events']) // 48
        ev_t = self.rec(0x23305, ev_i)
        entry_t = self.cue_entries[tmpl_ci].data
        rpc_t = self.voice_chunks[v0][1].data
        choice_t = self.event_chunks[ev_i].data

        app = bytearray(b'\0' * ((-nlxwb_size) % align))
        smap_add, cues_add, voices_add, seqs_add, evs_add = [], [], [], [], []
        entries_add, vchunks_add, seqch_add, evch_add, srcs_add = [], [], [], [], []
        nc, nv, ns, ne, nsrc = H[2], H[4], H[6], H[8], len(self.sources)
        manifest = []
        for names, blob in new_cues:
            magic, il = struct.unpack_from('>4sI', blob)
            if magic != b'IDSP' or il != self.interleave:
                raise ValueError('%s: blob interleave 0x%x != bank interleave 0x%x' % (names[0], il, self.interleave))
            if len(blob) % 32:
                raise ValueError('%s: blob size not a multiple of 32' % names[0])
            off = nlxwb_size + len(app)
            assert off % align == 0
            app += blob
            app += b'\0' * ((-len(app)) % align)
            prim = nl_lower_hash(names[0])
            src_i, vi, si, ei = nsrc, nv, ns, ne
            srcs_add.append(struct.pack('>7I', src_i, off, len(blob), prim, 2, 0, 0))
            nsrc += 1
            voices_add.append(struct.pack('>I', prim) + voice_t[4:])
            vchunks_add.append((Node(0x23309, data=struct.pack('>I', self.base['seqs'] + 12 * si)),
                                Node(0x2330C, data=rpc_t)))
            nv += 1
            seqs_add.append(seq_t)
            seqch_add.append(Node(0x2330A, data=struct.pack('>iI', 1, self.base['events'] + 48 * ei)))
            ns += 1
            evs_add.append(ev_t)
            evch_add.append(Node(0x2330B, data=struct.pack('>I', src_i) + choice_t[4:8]))
            ne += 1
            cue_names = []
            for nm in names:
                h = nl_lower_hash(nm)
                if h in known:
                    raise ValueError('cue %s (%08x) already exists in the bank' % (nm, h))
                known.add(h)
                cues_add.append(struct.pack('>I', h) + cue_t[4:])
                entries_add.append(Node(0x23308, data=struct.pack('>I', self.base['voices'] + 44 * vi) + entry_t[4:]))
                smap_add.append(struct.pack('>5I', h, 0, 0, 0, nc))
                cue_names.append((nm, '%08x' % h, nc))
                nc += 1
            manifest.append(dict(cues=cue_names, source=src_i, voice=vi, offset=off, size=len(blob),
                                 append_offset=off - nlxwb_size))
        H[2], H[4], H[6], H[8] = nc, nv, ns, ne

        def cp(n, data=None, children=None):
            return Node(n.raw_id, data=data, children=children)
        sm_hdr, sm_cues = self.smap.children
        smap = cp(self.smap, children=[cp(sm_hdr, data=struct.pack('>I', nc) + sm_hdr.data[4:]),
                                       cp(sm_cues, data=sm_cues.data + b''.join(smap_add))])
        ch = self.bundle.children
        arrs = [cp(ch[1], data=ch[1].data + b''.join(cues_add)), cp(ch[2], data=ch[2].data + b''.join(voices_add)),
                cp(ch[3], data=ch[3].data + b''.join(seqs_add)), cp(ch[4], data=ch[4].data + b''.join(evs_add)),
                ch[5], ch[6]]
        kids = [cp(ch[0], data=struct.pack('>16I', *H))] + arrs
        kids += self.cue_entries + entries_add
        for a, b in self.voice_chunks:
            kids += [a, b]
        for a, b in vchunks_add:
            kids += [a, b]
        kids += self.seq_chunks + seqch_add + self.event_chunks + evch_add
        bundle = cp(self.bundle, children=kids)
        sh, se = self.srcnode.children
        srcn = cp(self.srcnode, children=[cp(sh, data=struct.pack('>I', nsrc) + sh.data[4:]),
                                          cp(se, data=se.data + b''.join(srcs_add))])
        top = []
        for n in self.root.children:
            top.append({ID_SMAP: smap, ID_BUNDLE: bundle, ID_SOURCES: srcn}.get(n.id, n))
        return serialize(Node(self.root.raw_id, children=top)), bytes(app), manifest


# =============================================================== commands
def load_names(extra=()):
    names = {}
    here = os.path.dirname(os.path.abspath(__file__))
    files = [os.path.join(here, 'charged_nis_cues.txt')] + list(extra)
    for f in files:
        if not os.path.exists(f):
            continue
        for line in open(f, encoding='latin1'):
            s = line.strip().split()[0] if line.strip() else ''
            if not s or s.startswith('#'):
                continue
            for v in (s, s + '_nocrowd'):
                names.setdefault(nl_lower_hash(v), v.lower())
    return names


def cmd_dump(a):
    b = StreamBank(a.bank if a.bank.endswith('.resbun') else a.bank + '.resbun')
    names = load_names(a.names)
    nlx = a.nlxwb or (b.path[:-7] + '.nlxwb')
    vf = VirtualFile(nlx, a.nlxwb_size, a.append) if os.path.exists(nlx) else None
    print('%s: %d cues, %d voices, %d sequences, %d sound events, %d sources; interleave 0x%x; isStream %d; '
          'nlxwb end 0x%x%s' % (os.path.basename(b.path), b.counts['cues'], b.counts['voices'], b.counts['seqs'],
                                b.counts['events'], len(b.sources), b.interleave, b.is_stream, b.nlxwb_end(),
                                (' (file %d B)' % vf.orig_size) if vf else ''))
    for k0, k1, k2, k3, ci in b.soundmap:
        for si in b.cue_sources(ci):
            s = b.sources[si]
            extra = ''
            hb = vf.read(s['offset'] + 12, HDR_SIZE) if vf else b''
            if len(hb) == HDR_SIZE:
                h = parse_dsp_header(hb)
                extra = '  %5d Hz %8d smp %7.3fs' % (h['rate'], h['num_samples'], h['num_samples'] / h['rate'])
            elif vf:
                extra = '  (beyond the .nlxwb given: pass --append)'
            print('cue %3d %08x %-44s voice %3d src %3d %08x off 0x%08x size 0x%07x ch %d%s' % (
                ci, k0, names.get(k0, '?'), b.cue_voices(ci)[0], si, s['name'], s['offset'], s['size'],
                s['channels'], extra))


def cmd_wav(a):
    b = StreamBank(a.bank if a.bank.endswith('.resbun') else a.bank + '.resbun')
    nlx = a.nlxwb or (os.path.splitext(a.bank)[0] + '.nlxwb')
    vf = VirtualFile(nlx if os.path.exists(nlx) else None, a.nlxwb_size, a.append)
    ci = b.cue_index(a.cue)
    if ci is None:
        sys.exit('no cue %s' % a.cue)
    s = b.sources[b.cue_sources(ci)[0]]
    rate, pcm, hdrs = read_stream(vf.read, s['offset'], s['channels'], b.interleave)
    write_wav(a.out, rate, pcm)
    print('%s: %d ch %d Hz %d samples (%.3fs) -> %s' % (a.cue, len(pcm), rate, len(pcm[0]), len(pcm[0]) / rate, a.out))


def cmd_encode(a):
    il = a.interleave
    if a.bank:
        il = StreamBank(a.bank if a.bank.endswith('.resbun') else a.bank + '.resbun').interleave
    rate, ch = read_wav(a.wav)
    if len(ch) == 1:
        ch = [ch[0], ch[0]]
    blob = build_stream(ch[:2], rate, il)
    open(a.out, 'wb').write(blob)
    print('%s: %d Hz %d samples -> %s (%d bytes, interleave 0x%x)' % (a.wav, rate, len(ch[0]), a.out, len(blob), il))


def cmd_blobwav(a):
    rate, pcm, hdrs = parse_blob(open(a.blob, 'rb').read())
    write_wav(a.out, rate, pcm)
    print('%s -> %s (%d Hz, %d samples)' % (a.blob, a.out, rate, len(pcm[0])))


def cmd_merge(a):
    b = StreamBank(a.bank if a.bank.endswith('.resbun') else a.bank + '.resbun')
    new = []
    for spec in a.cues:
        names, path = spec.split('=', 1)
        new.append((names.split(','), open(path, 'rb').read()))
    res, app, man = b.merged(new, a.nlxwb_size, template=a.template)
    os.makedirs(a.out, exist_ok=True)
    stem = os.path.join(a.out, os.path.basename(b.path)[:-7])
    open(stem + '.resbun', 'wb').write(res)
    open(stem + '.nlxwb.append', 'wb').write(app)
    json.dump(dict(bank=os.path.basename(b.path), original_nlxwb_size=a.nlxwb_size, append_size=len(app),
                   interleave=b.interleave, streams=man), open(stem + '.manifest.json', 'w'), indent=1)
    print('wrote %s.resbun (%d B), %s.nlxwb.append (%d B, served at offset %d), %s.manifest.json' % (
        stem, len(res), stem, len(app), a.nlxwb_size, stem))


def snr(ref, test):
    n = min(len(ref), len(test))
    sig = sum(x * x for x in ref[:n]) or 1
    err = sum((x - y) ** 2 for x, y in zip(ref[:n], test[:n])) or 1
    import math
    return 10 * math.log10(sig / err)


def game_reads(hdrs, offset, size, nch, interleave):
    """The reads AudioReadState::Prepare/Update + fn_8035FE6C issue for one stream: (offset, length)
    list -- 0x60-byte headers, then per channel interleave-sized reads, the last one rounded up to
    32 bytes. Raises if any read leaves [offset, offset+size)."""
    base = offset + (12 if nch == 2 else 0)
    reads = [(base + c * HDR_SIZE, HDR_SIZE) for c in range(nch)]
    total = hdrs[0]['num_nibbles'] >> 1                    # m_Unknown18 = num_adpcm_nibbles >> 1
    done = 0
    data0 = base + nch * HDR_SIZE
    while done < total:
        rem = total - done
        n = interleave if rem > interleave else (rem + 31) & ~31
        for c in range(nch):
            reads.append((data0 + done * nch + c * interleave, n))
        done += interleave
    for o, n in reads:
        if o < offset or o + n > offset + size:
            raise ValueError('read 0x%x+0x%x outside the stream 0x%x+0x%x' % (o, n, offset, size))
        if o % 4:
            raise ValueError('read offset 0x%x not 4-byte aligned' % o)
    return reads


def cmd_verify(a):
    orig = StreamBank(a.bank if a.bank.endswith('.resbun') else a.bank + '.resbun')
    m = StreamBank(a.merged)
    ok = True
    o, n = orig.resolve_all(), m.resolve_all()
    for h, v in o.items():
        if n.get(h) != v:
            print('MISMATCH original cue %08x: %s -> %s' % (h, v, n.get(h)))
            ok = False
    print('original cues: %d; every one resolves to the same source (offset/size/name/channels): %s' % (len(o), ok))
    for cid in REC_SIZE:
        sz = len(orig.arr[cid].data)
        if m.arr[cid].data[:sz] != orig.arr[cid].data:
            print('MISMATCH array %x prefix' % cid)
            ok = False
    if m.interleave != orig.interleave or m.src_hdr[4:] != orig.src_hdr[4:]:
        print('MISMATCH source header')
        ok = False
    if m.sources[:len(orig.sources)] != orig.sources:
        print('MISMATCH original sources')
        ok = False
    offs = [(s['offset'], s['size']) for s in m.sources]
    contiguous = all(offs[i][0] + offs[i][1] <= offs[i + 1][0] for i in range(len(offs) - 1))
    print('original records/sources/headers unchanged: %s; sources sorted, non-overlapping: %s' % (ok, contiguous))
    ok &= contiguous
    size = a.nlxwb_size or (os.path.getsize(a.nlxwb) if a.nlxwb else orig.nlxwb_end())
    vf = VirtualFile(a.nlxwb, size, a.append)
    refs = dict(r.split('=', 1) for r in a.ref)
    added = [h for h in n if h not in o]
    print('new cues: %d (virtual .nlxwb = %d original + %d appended bytes)' % (len(added), size, len(vf.app)))
    for h in added:
        name = next((c for c in refs if nl_lower_hash(c) == h), '%08x' % h)
        for (off, sz, srcname, nch) in n[h]:
            if off < size or off % 32 or sz % 32 or off + sz > size + len(vf.app):
                print('  %s: bad placement 0x%x+0x%x' % (name, off, sz))
                ok = False
                continue
            rate, pcm, hdrs = read_stream(vf.read, off, nch, m.interleave)
            reads = game_reads(hdrs, off, sz, nch, m.interleave)
            line = '  %-38s %08x off 0x%x size 0x%x %d Hz %d smp %.3fs, %d game reads in bounds' % (
                name, h, off, sz, rate, len(pcm[0]), len(pcm[0]) / rate, len(reads))
            if name in refs:
                rr, rch = read_wav(refs[name])
                line += ', SNR vs source %s dB' % '/'.join('%.1f' % snr(rch[min(c, len(rch) - 1)], pcm[c])
                                                          for c in range(len(pcm)))
            print(line)
    if a.nlxwb and a.check_original_data:
        bad = 0
        for s in orig.sources:
            rate, pcm, hdrs = read_stream(vf.read, s['offset'], s['channels'], orig.interleave)
            game_reads(hdrs, s['offset'], s['size'], s['channels'], orig.interleave)
        print('original streams: all %d decode and every game read stays in bounds' % len(orig.sources))
    print('VERIFY', 'OK' if ok else 'FAILED')
    return 0 if ok else 1


def cmd_selftest(a):
    import math, random, time
    rnd = random.Random(1)
    n = 14 * 300 + 5
    pcm = [int(8000 * math.sin(i * 0.05) + 3000 * math.sin(i * 0.31) + rnd.randint(-300, 300)) for i in range(n)]
    t = time.time(); pc = py_correlate_coefs(pcm); pe = py_encode(pcm, pc); tp = time.time() - t
    lib = _lib()
    if lib:
        cc = correlate_coefs(pcm); ce = encode_adpcm(pcm, cc)
        print('C build: coefs equal %s, encoded equal %s' % (cc == pc, ce == pe))
    dec = py_decode(pe, n, pc)
    print('pure-Python encode %.2fs; round-trip SNR %.1f dB; coefs %s' % (tp, snr(pcm, dec), pc))
    blob = build_stream([pcm, pcm[::-1]], 32000, 0x6b40)
    r, ch, hd = parse_blob(blob)
    print('blob %d B, decoded %d/%d samples, SNR %.1f / %.1f dB' % (len(blob), len(ch[0]), n, snr(pcm, ch[0]),
                                                                    snr(pcm[::-1], ch[1])))


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd')
    p = sp.add_parser('dump'); p.add_argument('bank'); p.add_argument('--names', action='append', default=[])
    p.add_argument('--nlxwb'); p.add_argument('--append'); p.add_argument('--nlxwb-size', type=int)
    p = sp.add_parser('wav'); p.add_argument('bank'); p.add_argument('cue'); p.add_argument('out')
    p.add_argument('--nlxwb'); p.add_argument('--append'); p.add_argument('--nlxwb-size', type=int)
    p = sp.add_parser('encode'); p.add_argument('wav'); p.add_argument('out')
    p.add_argument('--interleave', type=lambda s: int(s, 0), default=0x6b40); p.add_argument('--bank')
    p = sp.add_parser('blobwav'); p.add_argument('blob'); p.add_argument('out')
    p = sp.add_parser('merge'); p.add_argument('bank'); p.add_argument('out')
    p.add_argument('--nlxwb-size', type=int, required=True); p.add_argument('--template')
    p.add_argument('cues', nargs='+')
    p = sp.add_parser('verify'); p.add_argument('bank'); p.add_argument('merged'); p.add_argument('--append', required=True)
    p.add_argument('--nlxwb'); p.add_argument('--nlxwb-size', type=int); p.add_argument('--ref', action='append', default=[])
    p.add_argument('--check-original-data', action='store_true', help='also decode every original stream')
    sp.add_parser('selftest')
    a = ap.parse_args(argv)
    f = dict(dump=cmd_dump, wav=cmd_wav, encode=cmd_encode, blobwav=cmd_blobwav, merge=cmd_merge,
             verify=cmd_verify, selftest=cmd_selftest).get(a.cmd)
    if not f:
        ap.print_help()
        return 1
    return f(a) or 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
