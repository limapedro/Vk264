"""Compare NAL units of x264 reference and oracle H.264 files.
Skips SEI (type 6). Reports first differing bit/byte per matching NAL type.
Usage: compare_nals.py <ref.h264> <oracle.h264>
"""
import re, sys
from pathlib import Path

def nals(path):
    b = Path(path).read_bytes()
    out = []
    for m in re.finditer(b'\x00\x00\x01(.)', b):
        start = m.start()+3
        nxt = b.find(b'\x00\x00\x01', start)
        if nxt < 0: nxt = len(b)
        payload = b[start:nxt]
        # trim trailing zeros belonging to next start code
        while payload.endswith(b'\x00'): payload = payload[:-1]
        out.append((payload[0] & 0x1f, payload))
    return out

a, b = nals(sys.argv[1]), nals(sys.argv[2])
fa = [x for x in a if x[0] != 6]
fb = [x for x in b if x[0] != 6]
print(f"ref NALs  : {[t for t,_ in a]}")
print(f"oracle NALs: {[t for t,_ in b]}")
if len(fa) != len(fb):
    print("NAL count mismatch (after skipping SEI)")
    sys.exit(1)
rc = 0
for (ta, pa), (tb, pb) in zip(fa, fb):
    if ta != tb:
        print(f"type mismatch {ta} vs {tb}"); rc = 1; continue
    n = min(len(pa), len(pb))
    diff = next((i for i in range(n) if pa[i] != pb[i]), None)
    if len(pa) != len(pb) or diff is not None:
        print(f"NAL {ta}: MISMATCH len ref={len(pa)} oracle={len(pb)} first_diff={diff}")
        if diff is not None:
            print(f"  ref    : {pa[max(0,diff-4):diff+12].hex(' ')}")
            print(f"  oracle : {pb[max(0,diff-4):diff+12].hex(' ')}")
        rc = 1
    else:
        print(f"NAL {ta}: EXACT ({len(pa)} bytes)")
sys.exit(rc)
