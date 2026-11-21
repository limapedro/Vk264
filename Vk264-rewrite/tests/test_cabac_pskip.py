"""Byte-exact CABAC P-slice regression (static two-frame sequence).

Generates a flat 64x64 2-frame sequence with identical frames, encodes it
with x264 --cabac, then encodes it with the oracle's production CABAC path
and compares every non-SEI Annex-B NAL byte-for-byte.
"""
import pathlib, re, subprocess, sys, os

ROOT = pathlib.Path(__file__).resolve().parents[2]
X264 = pathlib.Path(os.environ.get('VK264_X264_EXE',
    str(ROOT / 'references' / 'x264-master' / 'x264.exe')))
ORACLE = pathlib.Path(sys.argv[1])
BUILD = pathlib.Path(sys.argv[2])

if not X264.exists():
    print('x264.exe missing; skipping'); sys.exit(77)

W = H = 64
frame = bytearray()
for y in range(H):
    frame += bytes([100]*W)
for p in (1, 2):
    frame += bytes([128]*(W//2)*(H//2))
src = BUILD / 'cabac_pskip.yuv'
src.write_bytes(frame + frame)

ref, got = BUILD / 'cabac_pskip_ref.h264', BUILD / 'cabac_pskip_ora.h264'
subprocess.run([str(X264), '--input-res', f'{W}x{H}', '--fps', '60', '--qp', '28',
                '--preset', 'ultrafast', '--keyint', '30', '--frames', '2', '--cabac',
                '--output', str(ref), str(src)], check=True, capture_output=True)
subprocess.run([str(ORACLE), str(src), str(W), str(H), '28', '25', str(got)],
               check=True, capture_output=True)

def nals(data):
    out = []
    for m in re.finditer(b'\x00\x00\x01(.)', data):
        s = m.start()+3
        e = data.find(b'\x00\x00\x01', s)
        if e < 0: e = len(data)
        p = data[s:e]
        while p.endswith(b'\x00'): p = p[:-1]
        if p and (p[0] & 0x1f) != 6: out.append(p)
    return out

a, b = nals(ref.read_bytes()), nals(got.read_bytes())
ok = a == b
for i, (x, y) in enumerate(zip(a, b)):
    print('  NAL %d: %s ref=%d oracle=%d' %
          (i, 'EXACT' if x == y else 'MISMATCH', len(x), len(y)))
if len(a) != len(b): ok = False
if not ok:
    sys.exit(1)
print('CABAC P-skip slice: byte-exact vs x264 --cabac')
