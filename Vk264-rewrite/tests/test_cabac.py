"""Byte-exact CABAC production-path regression.

Generates a flat 64x64 I-frame, encodes it with x264 --cabac, then encodes
it with the oracle's production CABAC path (no captured decisions) and
compares the complete Annex-B stream byte-for-byte. This validates the
ported coder, context tables, I16 decision generation, residual coefficient
coding, slice-header alignment and flush.
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
src = BUILD / 'cabac_flat.yuv'
src.write_bytes(frame)

ref, got = BUILD / 'cabac_ref.h264', BUILD / 'cabac_ora.h264'
subprocess.run([str(X264), '--input-res', f'{W}x{H}', '--fps', '60', '--qp', '28',
                '--preset', 'ultrafast', '--keyint', '1', '--frames', '1', '--cabac',
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
print('  NAL count ref=%d oracle=%d' % (len(a), len(b)))
ok = True
for i, (x, y) in enumerate(zip(a, b)):
    same = x == y
    ok &= same
    print('  NAL %d: %s ref=%d oracle=%d' % (i, 'EXACT' if same else 'MISMATCH', len(x), len(y)))
if len(a) != len(b): ok = False
if not ok:
    sys.exit(1)
print('CABAC replay: byte-exact vs x264 --cabac')
