"""Byte-exact oracle regression: all-skip P-slice for a static 2-frame sequence."""
import pathlib, re, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
X264 = pathlib.Path(sys.argv[1])
ORACLE = pathlib.Path(sys.argv[2])
BUILD = pathlib.Path(sys.argv[3])
CLIP = pathlib.Path(r'E:\__PROJECTS__\Wx264-Vulkan-Opencode\clips\youtube'
                    r'\warzone2020_verdansk_4k_src_1080p_nvenc_p7_24mbps_240f.yuv')
SW, SH, X0, Y0, W, H = 1920, 1080, 640, 360, 64, 64

def nals(data):
    out = []
    for m in re.finditer(b'\x00\x00\x01(.)', data):
        s = m.start()+3
        e = data.find(b'\x00\x00\x01', s)
        if e < 0: e = len(data)
        p = data[s:e]
        while p.endswith(b'\x00'): p = p[:-1]
        if p: out.append((p[0] & 0x1f, p))
    return out

if not X264.exists() or not CLIP.exists():
    print('x264.exe or clip missing; skipping'); sys.exit(77)

fs = SW*SH*3//2
data = CLIP.read_bytes()[:fs]
out = bytearray()
for y in range(Y0, Y0+H): out += data[y*SW+X0:y*SW+X0+W]
for p in (1, 2):
    base = SW*SH + (p-1)*(SW*SH//4)
    for y in range(Y0//2, Y0//2+H//2):
        s = base + y*(SW//2) + X0//2
        out += data[s:s+W//2]
src = BUILD/'p2_static.yuv'
src.write_bytes(bytes(out)*2)

ref, got = BUILD/'p2_static_ref.h264', BUILD/'p2_static_oracle.h264'
subprocess.run([str(X264),'--input-res',f'{W}x{H}','--fps','60','--qp','28',
                '--preset','ultrafast','--keyint','30','--frames','2',
                '--output',str(ref),str(src)], check=True, capture_output=True)
subprocess.run([str(ORACLE),str(src),str(W),str(H),'28','28',str(got)],
               check=True, capture_output=True)

a = [p for t,p in nals(ref.read_bytes()) if t == 1]
b = [p for t,p in nals(got.read_bytes()) if t == 1]
if len(a) != 1 or len(b) != 1:
    print(f'FAIL: expected one P NAL, got ref={len(a)} oracle={len(b)}'); sys.exit(1)
print(f'  P NAL: ref {len(a[0])}B oracle {len(b[0])}B')
if a[0] != b[0]:
    print('FAIL: P-slice mismatch')
    print('  ref   ', a[0].hex(' '))
    print('  oracle', b[0].hex(' '))
    sys.exit(1)
print('oracle P-skip slice: byte-exact vs x264')
