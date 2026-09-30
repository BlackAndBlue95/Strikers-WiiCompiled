# Map MKW (RMCP01) data addresses to MSC (R4QE01) using anchor symbols from both decomps.
import re,bisect,json,sys
def load(p):
    out=[]
    for l in open(p):
        m=re.match(r'(\S+) = \.(\S+):0x([0-9A-Fa-f]{8}); // type:(\S+)(?:.*?size:0x([0-9A-Fa-f]+))?',l)
        if m: out.append((int(m[3],16),int(m[5] or '0',16),m[1],m[2],m[4]))
    return sorted(out)
mk=load('scripts/msc/mkw_RMCP01_symbols.txt'); ms=load('scripts/msc/msc_R4QE01_symbols.txt')
msn={}
for a,s,n,sec,t in ms: msn.setdefault(n,(a,s,sec))
msa=[x[0] for x in ms]
def msc_at(a):
    i=bisect.bisect_right(msa,a)-1
    if i<0: return '?'
    sa,sz,n,sec,t=ms[i]; return f'{n}+0x{a-sa:X}' if a-sa< max(sz,1) or a==sa else f'(after {n}+0x{a-sa:X})'
anchors=[(a,s,n,sec) for a,s,n,sec,t in mk if n in msn and not re.match(r'(lbl_|@|\.\.\.)',n) and msn[n][2]==sec and msn[n][1]==s and t=='object']
aa=[x[0] for x in anchors]
def map_addr(a,window=0x400):
    i=bisect.bisect_right(aa,a)-1
    cands=[]
    for j in (i,i+1,i-1,i+2):
        if 0<=j<len(anchors):
            ax,s,n,sec=anchors[j]
            if abs(ax-a)<=window: cands.append((msn[n][0]-ax,n,ax))
    if not cands: return None,'noanchor',[]
    deltas={}
    for d,n,ax in cands: deltas.setdefault(d,[]).append(n)
    best=max(deltas.items(),key=lambda kv:len(kv[1]))
    conf='agree' if len(deltas)==1 and len(cands)>1 else ('single' if len(cands)==1 else 'split')
    return a+best[0],conf,cands
if __name__=='__main__':
    for line in open(sys.argv[1]):
        a=int(line[:8],16)
        new,conf,c=map_addr(a)
        ctx=line.split(': ',1)[1].strip()[:55] if ': ' in line else ''
        print(f'{a:08X} -> {new and f"{new:08X}" or "--------"} {conf:8} {new and msc_at(new) or "":32} | {ctx}')
