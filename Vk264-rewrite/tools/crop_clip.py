"""Crop WxH pixels at (x0,y0) from frame n of a raw YUV420 clip.
Usage: crop_clip.py <input.yuv> <src_width> <out.yuv> <x0> <y0> <w> <h> <frame>
"""
import sys
from pathlib import Path

src, sw, out, x0, y0, w, h, frame = sys.argv[1], int(sys.argv[2]), sys.argv[3], \
    int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]), int(sys.argv[7]), int(sys.argv[8])
assert w % 16 == 0 and h % 16 == 0, "crop must be macroblock aligned"
fs = sw * (sw*3//2) if False else 0  # frame size uses source width/height below
# source height is not passed; infer from file size assuming 1080p unless overridable
sh = int(sys.argv[9]) if len(sys.argv) > 9 else 1080
fs = sw * sh * 3 // 2
data = Path(src).read_bytes()[frame*fs:(frame+1)*fs]
planes = []
off = 0
for scale in (1, 2):
    pw, ph = (w, h) if scale == 1 else (w//2, h//2)
    px, py = (x0, y0) if scale == 1 else (x0//2, y0//2)
    buf = bytearray()
    for row in range(ph):
        s = off + (py+row)*sw//scale + px
        buf += data[s:s+pw]
    planes.append(bytes(buf))
    off += sw * sh // (scale*scale)
Path(out).write_bytes(b''.join(planes))
print(f"wrote {out}: {w}x{h} frame {frame} at ({x0},{y0}), {Path(out).stat().st_size} bytes")
