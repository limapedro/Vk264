"""Byte-exact oracle regression: 2-frame moving-footage P-slices.

For each case a 64x64 region is cropped from real 1080p footage for frame 0
and the same region shifted horizontally for frame 1 (camera motion). Both
are encoded with x264 and the oracle at QP28/ultrafast, and every non-SEI
NAL is compared byte-for-byte (SPS, PPS, IDR and the P-slice).
"""
import pathlib, re, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
import os
X264 = pathlib.Path(os.environ.get('VK264_X264_EXE',
    str(ROOT / 'references' / 'x264-master' / 'x264.exe')))
ORACLE = pathlib.Path(sys.argv[1])
BUILD = pathlib.Path(sys.argv[2])
CLIP = pathlib.Path(r'E:\__PROJECTS__\Wx264-Vulkan-Opencode\clips\youtube'
                    r'\warzone2020_verdansk_4k_src_1080p_nvenc_p7_24mbps_240f.yuv')
SW, SH = 1920, 1080
W, H = 64, 64
CASES = [(320, 800, 8), (320, 720, 8), (1760, 480, 8),
         (1280, 960, 8), (0, 0, 8), (640, 360, 8),
         (960, 840, 4), (1560, 960, 4), (1800, 900, 4), (640, 360, 16),
         (360, 0, 8), (1680, 960, 16), (1680, 120, 2), (0, 120, 2),
         (1200, 720, 8), (480, 120, 8), (720, 480, 16), (1200, 720, 16),
         (720, 0, 2), (240, 240, 12), (1680, 120, 12), (1200, 120, 16),
         (720, 120, 8), (960, 240, 2), (1200, 0, 16), (960, 360, 12),
         (960, 0, 4), (1680, 0, 16), (1680, 540, 8),
         (960, 0, 12), (720, 240, 12), (0, 360, 12), (1680, 840, 12),
         (1560, 240, 16), (1680, 240, 16), (840, 360, 16),
         (1680, 900, 16), (720, 120, 16), (960, 60, 2),
         # post-decimation chroma-AC neighbour nnz regression cases
         (0, 420, 16), (120, 960, 4), (120, 960, 12), (120, 960, 16), (960, 0, 16),
         # I16 luma-AC count_cat regression cases (total_zeros must be absent at 15)
         (0, 900, 16), (120, 360, 12), (120, 960, 8), (1800, 960, 16), (480, 960, 16)]


def crop(data, x0, y0, dx):
    out = bytearray()
    for y in range(y0, y0+H):
        out += data[y*SW + x0+dx : y*SW + x0+dx + W]
    for p in (1, 2):
        base = SW*SH + (p-1)*(SW*SH//4)
        for y in range(y0//2, y0//2+H//2):
            s = base + y*(SW//2) + x0//2 + dx//2
            out += data[s:s+W//2]
    return bytes(out)


def nals(data):
    out = []
    for m in re.finditer(b'\x00\x00\x01(.)', data):
        s = m.start()+3
        e = data.find(b'\x00\x00\x01', s)
        if e < 0: e = len(data)
        p = data[s:e]
        while p.endswith(b'\x00'): p = p[:-1]
        if p: out.append((p[0] & 0x1f, p))
    return [(t, p) for t, p in out if t != 6]


if not X264.exists() or not CLIP.exists():
    print('x264.exe or clip missing; skipping'); sys.exit(77)

data = CLIP.read_bytes()[:SW*SH*3//2]
all_ok = True
for x0, y0, shift in CASES:
    src = BUILD / f'pme_{x0}_{y0}_{shift}.yuv'
    src.write_bytes(crop(data, x0, y0, 0) + crop(data, x0, y0, shift))
    ref = BUILD / f'pme_{x0}_{y0}_{shift}_ref.h264'
    got = BUILD / f'pme_{x0}_{y0}_{shift}_ora.h264'
    subprocess.run([str(X264), '--input-res', f'{W}x{H}', '--fps', '60', '--qp', '28',
                    '--preset', 'ultrafast', '--keyint', '30', '--frames', '2',
                    '--threads', '1', '--output', str(ref), str(src)],
                   check=True, capture_output=True)
    subprocess.run([str(ORACLE), str(src), str(W), str(H), '28', '25', str(got)],
                   check=True, capture_output=True)
    a, b = nals(ref.read_bytes()), nals(got.read_bytes())
    ok = a == b
    all_ok &= ok
    pl = [len(p) for t, p in a if t == 1]
    print(f'  ({x0},{y0},+{shift}): {"EXACT" if ok else "MISMATCH"} Plen={pl}')

if not all_ok:
    sys.exit(1)
print(f'oracle P-slices: {len(CASES)}/{len(CASES)} byte-exact vs x264 (moving footage)')
