"""Byte-exact oracle regression: 2-frame moving-footage P-slice.

Crops a 64x64 region from real 1080p footage for frame 0 and the same region
8 pixels to the right for frame 1 (horizontal camera motion), encodes both
with x264 and the oracle at QP28/ultrafast, and compares every non-SEI NAL
byte-for-byte (SPS, PPS, IDR and the motion-compensated P-slice).
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
X0, Y0, W, H = 320, 800, 64, 64
SHIFT = 8


def crop(data, dx):
    out = bytearray()
    for y in range(Y0, Y0+H):
        out += data[y*SW + X0+dx : y*SW + X0+dx + W]
    for p in (1, 2):
        base = SW*SH + (p-1)*(SW*SH//4)
        for y in range(Y0//2, Y0//2+H//2):
            s = base + y*(SW//2) + X0//2 + dx//2
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
src = BUILD / 'pme_2frame.yuv'
src.write_bytes(crop(data, 0) + crop(data, SHIFT))

ref, got = BUILD / 'pme_ref.h264', BUILD / 'pme_oracle.h264'
subprocess.run([str(X264), '--input-res', f'{W}x{H}', '--fps', '60', '--qp', '28',
                '--preset', 'ultrafast', '--keyint', '30', '--frames', '2',
                '--threads', '1', '--output', str(ref), str(src)],
               check=True, capture_output=True)
subprocess.run([str(ORACLE), str(src), str(W), str(H), '28', '25', str(got)],
               check=True, capture_output=True)

a, b = nals(ref.read_bytes()), nals(got.read_bytes())
ok = a == b
for (ta, pa), (tb, pb) in zip(a, b):
    print(f'  NAL {ta}: {"EXACT" if pa == pb else "MISMATCH"} '
          f'ref={len(pa)} oracle={len(pb)}')
if not ok:
    print(f'  FAIL: NAL counts {len(a)} vs {len(b)}')
    sys.exit(1)
print('oracle P-slice: byte-exact vs x264 (2-frame moving footage)')
