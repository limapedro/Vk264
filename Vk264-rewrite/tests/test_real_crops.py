"""Byte-exact oracle regression on real 1080p footage crops.
Crops several 16x16 macroblocks from the Wx264 clip set, encodes with x264
and the oracle, and compares SPS/PPS/IDR NALs byte-for-byte.
"""
import pathlib, re, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
X264 = ROOT / 'references' / 'x264-master' / 'x264.exe'
ORACLE = pathlib.Path(sys.argv[1])
BUILD = pathlib.Path(sys.argv[2])
CLIP = pathlib.Path(r'E:\__PROJECTS__\Wx264-Vulkan-Opencode\clips\youtube'
                    r'\warzone2020_verdansk_4k_src_1080p_nvenc_p7_24mbps_240f.yuv')
SW, SH = 1920, 1080

def crop(x0, y0, frame, path):
    fs = SW*SH*3//2
    data = CLIP.read_bytes()[frame*fs:(frame+1)*fs]
    out = bytearray()
    for y in range(y0, y0+16):
        out += data[y*SW+x0:y*SW+x0+16]
    for p in (1, 2):
        base = SW*SH + (p-1)*(SW*SH//4)
        for y in range(y0//2, y0//2+8):
            s = base + y*(SW//2) + x0//2
            out += data[s:s+8]
    path.write_bytes(out)

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

cases = [(100,100,0), (320,180,0), (640,360,1), (1200,600,2), (1500,900,3)]
bad = []
for i, (x0, y0, fr) in enumerate(cases):
    src = BUILD / f'real_crop_{i}.yuv'; crop(x0, y0, fr, src)
    ref, got = BUILD / f'real_ref_{i}.h264', BUILD / f'real_oracle_{i}.h264'
    subprocess.run([str(X264),'--input-res','16x16','--fps','60','--qp','28',
                    '--preset','ultrafast','--keyint','1','--frames','1',
                    '--output',str(ref),str(src)], check=True, capture_output=True)
    subprocess.run([str(ORACLE),str(src),'16','16','28','25',str(got)],
                   check=True, capture_output=True)
    a, b = nals(ref.read_bytes()), nals(got.read_bytes())
    if a != b:
        sizes = [(t, len(p)) for t, p in a], [(t, len(p)) for t, p in b]
        bad.append((x0, y0, fr, sizes))

if bad:
    print(f'FAIL: {len(bad)}/{len(cases)} crops mismatch')
    for item in bad: print(' ', item)
    sys.exit(1)
print(f'oracle real crops: {len(cases)}/{len(cases)} byte-exact vs x264')
