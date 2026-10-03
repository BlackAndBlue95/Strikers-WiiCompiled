# shier.py: a skeleton hierarchy file: name, bone hashes, parent indices, flags.
import struct, sys
sys.path.insert(0, __file__.rsplit('/',1)[0])
from nlchunk import walk
def load(path):
    d=open(path,'rb').read()
    rows=walk(d,0,len(d),0,[])
    ch={}
    for depth,rawid,idx,name,off,payload,size in rows: ch.setdefault(idx&0xFFFFFF,[]).append((payload,size))
    o,s=ch[0x18003][0]; hashes=list(struct.unpack('>%dI'%(s//4), d[o:o+s]))
    o,s=ch[0x18009][0]; parents=list(struct.unpack('>%di'%(s//4), d[o:o+s]))
    o,s=ch[0x18002][0]; name=d[o:o+s].split(b'\0')[0].decode()
    return name, hashes, parents, ch
if __name__=='__main__':
    a=load(sys.argv[1]); b=load(sys.argv[2])
    print(a[0], len(a[1]), 'bones;', b[0], len(b[1]), 'bones')
    sa=set(a[1]); sb=set(b[1])
    print('in both', len(sa&sb), 'only first', ['%08x'%h for h in a[1] if h not in sb], 'only second', ['%08x'%h for h in b[1] if h not in sa])
    print('chunks first', sorted('%x'%k for k in a[3]), 'second', sorted('%x'%k for k in b[3]))
