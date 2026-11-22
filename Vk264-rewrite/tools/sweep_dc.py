"""Sweep flat gray values, report x264's decoded luma DC coefficient[0]."""
import subprocess, pathlib, sys, re
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from decode_block import BR, decode, unescape

def first_coef(path):
    data = pathlib.Path(path).read_bytes()
    for m in re.finditer(b'\x00\x00\x01(.)', data):
        nal = data[m.start()+3:]
        if (nal[0] & 0x1f) != 5: continue
        r = BR(unescape(nal[1:]), 24)
        mt = r.ue(); cp = r.ue(); qd = r.se()
        dc = decode(r, 0, 0, 16)
        return mt, dc[0], dc[:4]
    return None

for v in range(120, 137):
    pathlib.Path('build/flat.yuv').write_bytes(bytes([v])*384)
    subprocess.run(['references/x264-master/x264.exe','--input-res','16x16','--fps','60',
                    '--qp','28','--preset','ultrafast','--keyint','1','--frames','1',
                    '--output','build/f_ref.h264','build/flat.yuv'], capture_output=True)
    print(v, first_coef('build/f_ref.h264'))
