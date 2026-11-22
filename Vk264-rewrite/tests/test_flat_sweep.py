"""Byte-exact oracle regression: sweep flat gray values vs x264 CLI.
Compares SPS, PPS and IDR slice NALs (skips SEI)."""
import pathlib, re, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
import os
X264 = pathlib.Path(os.environ.get('VK264_X264_EXE',
    str(ROOT / 'references' / 'x264-master' / 'x264.exe')))
ORACLE = pathlib.Path(sys.argv[1])
BUILD = pathlib.Path(sys.argv[2])

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

if not X264.exists():
    print('x264.exe not found; skipping'); sys.exit(77)

bad = []
for v in range(0, 256, 8):
    src = BUILD / 'sweep.yuv'
    src.write_bytes(bytes([v])*384)
    ref, got = BUILD / 'sweep_ref.h264', BUILD / 'sweep_oracle.h264'
    subprocess.run([str(X264),'--input-res','16x16','--fps','60','--qp','28',
                    '--preset','ultrafast','--keyint','1','--frames','1',
                    '--output',str(ref),str(src)], check=True, capture_output=True)
    subprocess.run([str(ORACLE),str(src),'16','16','28','25',str(got)],
                   check=True, capture_output=True)
    a, b = nals(ref.read_bytes()), nals(got.read_bytes())
    if a != b:
        bad.append(v)

if bad:
    print(f'FAIL: {len(bad)} flat values mismatch: {bad}')
    sys.exit(1)
print('oracle flat sweep: 32/32 byte-exact vs x264 (SPS/PPS/slice)')
