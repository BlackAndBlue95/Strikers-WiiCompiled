# gcextract.py <ciso> <outdir> <prefix...>: extract disc files whose path starts with a prefix.
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gcdisc import CISO, files

if __name__ == '__main__':
    d = CISO(sys.argv[1]); out = sys.argv[2]; prefixes = sys.argv[3:]
    n = 0
    for p, o, s in files(d):
        if any(p.lower().startswith(pre.lower()) for pre in prefixes):
            dst = os.path.join(out, p); os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, 'wb').write(d.read(o, s)); n += 1
    print(n, 'files')
