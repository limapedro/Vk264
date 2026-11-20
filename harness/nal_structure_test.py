"""Regression: gop>1 must emit P-slices (NAL type 1), not all-IDR (type 5).

Bug fixed in cavlc_packet.comp: the IDR flag is words[13] bit0; bits 1/2 carry
ultrafast/SAD analysis flags that are always set for subme<=1 presets, so the
old `words[13] != 0` test forced every frame to IDR.

Usage: python harness/nal_structure_test.py <vk264.exe> <build-dir>
"""
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check import make_source

exe, out_dir = sys.argv[1], pathlib.Path(sys.argv[2])
out_dir.mkdir(parents=True, exist_ok=True)
source = out_dir / "source.yuv"
make_source(source, 64, 64, 3, "flat-grid")
out = out_dir / "nal_structure.h264"
subprocess.run([exe, str(source), str(out), "64", "64", "3", "60",
                "--qp", "28", "--gop", "3", "--backend", "vulkan",
                "--preset", "ultrafast"], check=True, capture_output=True)
data = out.read_bytes()
types = [m.group(1)[0] & 0x1F for m in re.finditer(b"\x00\x00\x01(.)", data)]
expected = [7, 8, 5, 1, 1]  # SPS, PPS, IDR, P, P
if types != expected:
    raise SystemExit(f"NAL structure mismatch: {types} != {expected} "
                     "(gop=3 must emit P-slices, not all-IDR)")
print(f"NAL structure OK for gop=3: {types} (SPS, PPS, IDR, P, P)")
