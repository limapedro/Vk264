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

W = H = 64
all_ok = True
for name, luma, chroma in (('flat128', 100, 128), ('flat100', 42, 100),
                           ('gradient', None, None)):
    frame = bytearray()
    if luma is None:  # gradient: luma = (x*4+y*2)%256, chroma = 128
        for y in range(H):
            frame += bytes([(x*4+y*2) % 256 for x in range(W)])
        for p in (1, 2):
            frame += bytes([128]*(W//2)*(H//2))
    else:
        for y in range(H):
            frame += bytes([luma]*W)
        for p in (1, 2):
            frame += bytes([chroma]*(W//2)*(H//2))
    src = BUILD / f'cabac_{name}.yuv'
    src.write_bytes(frame)
    ref, got = BUILD / f'cabac_{name}_ref.h264', BUILD / f'cabac_{name}_ora.h264'
    subprocess.run([str(X264), '--input-res', f'{W}x{H}', '--fps', '60', '--qp', '28',
                    '--preset', 'ultrafast', '--keyint', '1', '--frames', '1', '--cabac',
                    '--output', str(ref), str(src)], check=True, capture_output=True)
    subprocess.run([str(ORACLE), str(src), str(W), str(H), '28', '25', str(got)],
                   check=True, capture_output=True)
    a, b = nals(ref.read_bytes()), nals(got.read_bytes())
    ok = a == b
    all_ok &= ok
    for i, (x, y) in enumerate(zip(a, b)):
        print('  %s NAL %d: %s ref=%d oracle=%d' %
              (name, i, 'EXACT' if x == y else 'MISMATCH', len(x), len(y)))
    if len(a) != len(b): all_ok = False
if not all_ok:
    sys.exit(1)
print('CABAC production: byte-exact vs x264 --cabac')
