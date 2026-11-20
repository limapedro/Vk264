"""Byte-exact oracle regression on multi-MB frames (horizontal neighbours).
Crops a 32x16 (2x1 MB) region from real 1080p footage, encodes with x264
and the oracle, and compares SPS/PPS/IDR NALs byte-for-byte.
"""
import pathlib, re, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
X264 = ROOT / 'references' / 'x264-master' / 'x264.exe'
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
src = BUILD / 'multimb.yuv'
out = bytearray()
for y in range(Y0, Y0+16): out += data[y*SW+X0:y*SW+X0+32]
for p in (1, 2):
    base = SW*SH + (p-1)*(SW*SH//4)
    for y in range(Y0//2, Y0//2+8):
        s = base + y*(SW//2) + X0//2
        out += data[s:s+16]
src.write_bytes(out)

ref, got = BUILD / 'multimb_ref.h264', BUILD / 'multimb_oracle.h264'
subprocess.run([str(X264),'--input-res','32x16','--fps','60','--qp','28',
                '--preset','ultrafast','--keyint','1','--frames','1',
                '--output',str(ref),str(src)], check=True, capture_output=True)
subprocess.run([str(ORACLE),str(src),'32','16','28','25',str(got)],
               check=True, capture_output=True)
a, b = nals(ref.read_bytes()), nals(got.read_bytes())
if a != b:
    print('FAIL: multi-MB (32x16) NAL mismatch')
    for (ta,pa),(tb,pb) in zip(a,b):
        print(f'  NAL {ta}: ref {len(pa)}B oracle {len(pb)}B equal={pa==pb}')
    sys.exit(1)
print('oracle multi-MB (32x16): byte-exact vs x264 (SPS/PPS/IDR)')
