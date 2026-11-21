"""Byte-exact CABAC P-slice regression on moving footage.

Crops a 64x64 region from real footage for frame 0 and the same region
shifted horizontally for frame 1, encodes both with x264 --cabac, then
compares every non-SEI Annex-B NAL against the oracle's production path.
"""
import pathlib, re, subprocess, sys, os

ROOT = pathlib.Path(__file__).resolve().parents[2]
X264 = pathlib.Path(os.environ.get('VK264_X264_EXE',
    str(ROOT / 'references' / 'x264-master' / 'x264.exe')))
ORACLE = pathlib.Path(sys.argv[1])
BUILD = pathlib.Path(sys.argv[2])
CLIP = pathlib.Path(r'E:\__PROJECTS__\Wx264-Vulkan-Opencode\clips\youtube'
                    r'\warzone2020_verdansk_4k_src_1080p_nvenc_p7_24mbps_240f.yuv')
SW, SH = 1920, 1080
W, H = 64, 64
X0, Y0, SHIFT = 320, 800, 8

if not X264.exists() or not CLIP.exists():
    print('x264.exe or clip missing; skipping'); sys.exit(77)

data = CLIP.read_bytes()[:SW*SH*3//2]

def crop(dx):
    out = bytearray()
    for y in range(Y0, Y0+H):
        out += data[y*SW + X0+dx : y*SW + X0+dx + W]
    for p in (1, 2):
        base = SW*SH + (p-1)*(SW*SH//4)
        for y in range(Y0//2, Y0//2+H//2):
            s = base + y*(SW//2) + X0//2 + dx//2
            out += data[s : s + W//2]
    return bytes(out)

src = BUILD / 'cabac_pme.yuv'
src.write_bytes(crop(0) + crop(SHIFT))
ref, got = BUILD / 'cabac_pme_ref.h264', BUILD / 'cabac_pme_ora.h264'
subprocess.run([str(X264), '--input-res', f'{W}x{H}', '--fps', '60', '--qp', '28',
                '--preset', 'ultrafast', '--keyint', '30', '--frames', '2', '--cabac',
                '--threads', '1', '--output', str(ref), str(src)], check=True, capture_output=True)
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
ok = len(a) == len(b)
for i, (x, y) in enumerate(zip(a, b)):
    same = x == y
    ok &= same
    print('  NAL %d: %s ref=%d oracle=%d' % (i, 'EXACT' if same else 'MISMATCH', len(x), len(y)))
if not ok:
    sys.exit(1)
print('CABAC moving P-slice: byte-exact vs x264 --cabac (IDR + P, 16 MBs)')
