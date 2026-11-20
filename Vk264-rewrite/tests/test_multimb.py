"""Byte-exact oracle regression on multi-MB I-frames.
Crops 32x16, 16x32, 32x32, 64x64 and 128x128 regions from real 1080p footage, encodes each
with x264 and the oracle, and compares SPS/PPS/IDR NALs byte-for-byte.
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
SW, SH, X0, Y0 = 1920, 1080, 640, 360

def nals(data):
    out = []
    for m in re.finditer(b'\x00\x00\x01(.)', data):
        s = m.start()+3
        e = data.find(b'\x00\x00\x01', s)
        if e < 0: e = len(data)
        p = data[s:e]
        while p.endswith(b'\x00'): p = p[:-1]
        if p: out.append((p[0]&0x1f, p))
    return [(t, p) for t, p in out if t != 6]

if not X264.exists() or not CLIP.exists():
    print('x264.exe or clip missing; skipping'); sys.exit(77)

fs = SW*SH*3//2
data = CLIP.read_bytes()[:fs]

cases = []
for w, h in ((32,16), (16,32), (32,32), (64,64), (128,128)):
    src = BUILD / f'multimb_{w}x{h}.yuv'
    out = bytearray()
    for y in range(Y0, Y0+h): out += data[y*SW+X0:y*SW+X0+w]
    for p in (1, 2):
        base = SW*SH + (p-1)*(SW*SH//4)
        for y in range(Y0//2, Y0//2+h//2):
            s = base + y*(SW//2) + X0//2
            out += data[s:s+w//2]
    src.write_bytes(out)

    ref, got = BUILD / f'mb_{w}x{h}_ref.h264', BUILD / f'mb_{w}x{h}_oracle.h264'
    subprocess.run([str(X264),'--input-res',f'{w}x{h}','--fps','60','--qp','28',
                    '--preset','ultrafast','--keyint','1','--frames','1',
                    '--output',str(ref),str(src)], check=True, capture_output=True)
    subprocess.run([str(ORACLE),str(src),str(w),str(h),'28','25',str(got)],
                   check=True, capture_output=True)
    a, b = nals(ref.read_bytes()), nals(got.read_bytes())
    cases.append((f'{w}x{h}', a == b, [(t,len(p)) for t,p in a], [(t,len(p)) for t,p in b]))

bad = [c for c in cases if not c[1]]
for name, ok, sa, sb in cases:
    print(f'  {name}: {"EXACT" if ok else "MISMATCH"} ref={sa} oracle={sb}')
if bad:
    sys.exit(1)
print(f'oracle multi-MB: {len(cases)}/{len(cases)} byte-exact vs x264')
