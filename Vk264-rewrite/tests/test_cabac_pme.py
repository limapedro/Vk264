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
CASES = [(320, 800, 8), (1080, 600, 12), (1200, 540, 12)]

if not X264.exists() or not CLIP.exists():
    print('x264.exe or clip missing; skipping'); sys.exit(77)

data = CLIP.read_bytes()[:SW*SH*3//2]


def crop(x0, y0, dx):
    out = bytearray()
    for y in range(y0, y0+H):
        out += data[y*SW + x0+dx : y*SW + x0+dx + W]
    for p in (1, 2):
        base = SW*SH + (p-1)*(SW*SH//4)
        for y in range(y0//2, y0//2+H//2):
            s = base + y*(SW//2) + x0//2 + dx//2
            out += data[s : s + W//2]
    return bytes(out)


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


all_ok = True
for X0, Y0, SHIFT in CASES:
    src = BUILD / f'cabac_pme_{X0}_{Y0}_{SHIFT}.yuv'
    src.write_bytes(crop(X0, Y0, 0) + crop(X0, Y0, SHIFT))
    ref = BUILD / f'cabac_pme_{X0}_{Y0}_{SHIFT}_ref.h264'
    got = BUILD / f'cabac_pme_{X0}_{Y0}_{SHIFT}_ora.h264'
    subprocess.run([str(X264), '--input-res', f'{W}x{H}', '--fps', '60', '--qp', '28',
                    '--preset', 'ultrafast', '--keyint', '30', '--frames', '2', '--cabac',
                    '--threads', '1', '--output', str(ref), str(src)], check=True, capture_output=True)
    subprocess.run([str(ORACLE), str(src), str(W), str(H), '28', '25', str(got)],
                   check=True, capture_output=True)
    a, b = nals(ref.read_bytes()), nals(got.read_bytes())
    ok = len(a) == len(b)
    for i, (x, y) in enumerate(zip(a, b)):
        same = x == y
        ok &= same
        print('  (%d,%d,+%d) NAL %d: %s ref=%d oracle=%d' %
              (X0, Y0, SHIFT, i, 'EXACT' if same else 'MISMATCH', len(x), len(y)))
    all_ok &= ok

if not all_ok:
    sys.exit(1)
print(f'CABAC moving P-slice: {len(CASES)}/{len(CASES)} byte-exact vs x264 --cabac')
