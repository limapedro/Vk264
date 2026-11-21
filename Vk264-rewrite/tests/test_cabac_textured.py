"""Byte-exact CABAC I-frame regression on textured real footage.

Crops 64x64 regions from real 1080p footage and compares single-frame IDR
NALs from the oracle's production CABAC path against x264 byte-for-byte.
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
CASES = [(320, 800), (320, 720), (1760, 480), (640, 360), (1680, 960)]

if not X264.exists() or not CLIP.exists():
    print('x264.exe or clip missing; skipping'); sys.exit(77)

data = CLIP.read_bytes()[:SW*SH*3//2]

def crop(x0, y0):
    out = bytearray()
    for y in range(y0, y0+H):
        out += data[y*SW+x0 : y*SW+x0+W]
    for p in (1, 2):
        base = SW*SH + (p-1)*(SW*SH//4)
        for y in range(y0//2, y0//2+H//2):
            s = base + y*(SW//2) + x0//2
            out += data[s : s+W//2]
    return bytes(out)

def nal5(data):
    for m in re.finditer(b'\x00\x00\x01(.)', data):
        if m.group(1)[0] & 0x1f == 5:
            s = m.start()+3
            e = data.find(b'\x00\x00\x01', s)
            if e < 0: e = len(data)
            return data[s:e].rstrip(b'\x00')
    return b''

all_ok = True
for x0, y0 in CASES:
    src = BUILD / 'cabac_tex.yuv'
    src.write_bytes(crop(x0, y0))
    ref, got = BUILD / 'cabac_tex_ref.h264', BUILD / 'cabac_tex_ora.h264'
    subprocess.run([str(X264), '--input-res', f'{W}x{H}', '--fps', '60', '--qp', '28',
                    '--preset', 'ultrafast', '--keyint', '1', '--frames', '1', '--cabac',
                    '--threads', '1', '--output', str(ref), str(src)], check=True, capture_output=True)
    subprocess.run([str(ORACLE), str(src), str(W), str(H), '28', '25', str(got)],
                   check=True, capture_output=True)
    a, b = nal5(ref.read_bytes()), nal5(got.read_bytes())
    ok = a == b
    all_ok &= ok
    print(f'  ({x0},{y0}): {"EXACT" if ok else "MISMATCH"} ref={len(a)} oracle={len(b)}')

if not all_ok:
    sys.exit(1)
print(f'CABAC textured I-frames: {len(CASES)}/{len(CASES)} byte-exact vs x264 --cabac')
