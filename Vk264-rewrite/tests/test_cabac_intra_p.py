"""Byte-exact CABAC P-slice regression with intra (I16) macroblocks.

Crops a 64x64 region from real footage for frame 0 and the same region
shifted for frame 1; x264 --cabac codes some P-slice MBs as I16. Compares
every non-SEI Annex-B NAL against the oracle's production path.
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
CASES = [(640, 960, 16), (320, 800, 8), (1120, 960, 16), (320, 720, 2)]

if not X264.exists() or not CLIP.exists():
    print('x264.exe or clip missing; skipping'); sys.exit(77)

data = CLIP.read_bytes()[:SW*SH*3//2]

def crop(x0, y0, dx):
    out = bytearray()
    for y in range(y0, y0+H):
        out += data[y*SW+x0+dx : y*SW+x0+dx+W]
    for p in (1, 2):
        base = SW*SH + (p-1)*(SW*SH//4)
        for y in range(y0//2, y0//2+H//2):
            s = base + y*(SW//2) + x0//2 + dx//2
            out += data[s : s+W//2]
    return bytes(out)

def nals(d):
    out = []
    for m in re.finditer(b'\x00\x00\x01(.)', d):
        s = m.start()+3
        e = d.find(b'\x00\x00\x01', s)
        if e < 0: e = len(d)
        p = d[s:e]
        while p.endswith(b'\x00'): p = p[:-1]
        if p and (p[0] & 0x1f) != 6: out.append(p)
    return out

all_ok = True
for x0, y0, dx in CASES:
    src = BUILD / 'cabac_intra_p.yuv'
    src.write_bytes(crop(x0, y0, 0) + crop(x0, y0, dx))
    ref, got = BUILD / 'cabac_intra_p_ref.h264', BUILD / 'cabac_intra_p_ora.h264'
    subprocess.run([str(X264), '--input-res', f'{W}x{H}', '--fps', '60', '--qp', '28',
                    '--preset', 'ultrafast', '--keyint', '30', '--frames', '2', '--cabac',
                    '--threads', '1', '--output', str(ref), str(src)], check=True, capture_output=True)
    subprocess.run([str(ORACLE), str(src), str(W), str(H), '28', '25', str(got)],
                   check=True, capture_output=True)
    a, b = nals(ref.read_bytes()), nals(got.read_bytes())
    ok = a == b
    all_ok &= ok
    print(f'  ({x0},{y0},+{dx}): {"EXACT" if ok else "MISMATCH"} P={len(a[-1]) if a else 0}/{len(b[-1]) if b else 0}')

if not all_ok:
    sys.exit(1)
print(f'CABAC intra-in-P: {len(CASES)}/{len(CASES)} byte-exact vs x264 --cabac')
