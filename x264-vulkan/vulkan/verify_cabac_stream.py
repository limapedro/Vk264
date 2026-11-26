# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode the Vulkan CABAC smoke stream and compare every sample."""

from pathlib import Path
from fractions import Fraction
import json
import subprocess
import sys
import tempfile

test, ffmpeg, parameters, slice_header, luma, chroma, motion, predictor, search, cabac, nal, output, reconstruction, *probe = (
    sys.argv[1:]
)
raw = bool(probe and probe[-1] == "--raw")
if raw:
    probe.pop()
width, height, pictures = (100, 70, 18) if raw else (30, 30, 5)
stages = [parameters, slice_header, luma, chroma, motion, predictor, search, cabac, nal, output, reconstruction]

def raw_fixture(path):
    source_width, source_height, left, top, first = 122, 90, 12, 8, 1
    with path.open("wb") as source:
        for frame in range(first + pictures):
            for plane in range(3):
                shift = int(plane != 0)
                for y in range(source_height >> shift):
                    row = bytearray([16] * (source_width >> shift))
                    if (top >> shift) <= y < ((top + height) >> shift):
                        x = left >> shift
                        row[x:x + (width >> shift)] = bytes([68 + 20 * plane + 7 * frame]) * (width >> shift)
                    source.write(row)
    return [str(value) for value in (path, source_width, source_height, left, top, width, height, first, pictures)]

if raw:
    with tempfile.TemporaryDirectory(dir=Path(output).parent) as directory:
        source = raw_fixture(Path(directory) / "source.yuv")
        subprocess.run([test, "cabac_raw", *stages, *source], check=True)
else:
    subprocess.run([test, "cabac_smoke", *stages], check=True)
if not Path(output).stat().st_size:
    raise SystemExit("Vulkan CABAC smoke stream is empty")

decoded = subprocess.run(
    [ffmpeg, "-v", "error", "-xerror", "-i", output,
     "-map", "0:v:0", "-pix_fmt", "yuv420p", "-f", "rawvideo", "pipe:1"],
    check=True, stdout=subprocess.PIPE,
).stdout

expected = Path(reconstruction).read_bytes()
plane_size = width * height
frame_size = plane_size * 3 // 2
if len(expected) != pictures * frame_size:
    raise SystemExit("GPU reconstruction has the wrong number of samples")

if len(decoded) != len(expected):
    raise SystemExit(f"Expected {len(expected)} decoded bytes, got {len(decoded)}")
for index, (actual, reference) in enumerate(zip(decoded, expected)):
    if actual != reference:
        raise SystemExit(f"Decoded byte {index} is {actual}, expected {reference}")
if raw:
    for frame in range(pictures):
        offset = frame * frame_size
        for plane, length in enumerate((plane_size, plane_size // 4, plane_size // 4)):
            target = 75 + 20 * plane + 7 * frame
            if any(abs(sample - target) > 4 for sample in decoded[offset:offset + length]):
                raise SystemExit(f"Frame {frame} plane {plane} differs from the requested crop or source frame")
            offset += length
else:
    if expected[:frame_size] != bytes([128]) * frame_size:
        raise SystemExit("The first synthetic picture must be neutral gray")
    if len(set(expected[2 * frame_size:2 * frame_size + plane_size])) < 16:
        raise SystemExit("The reconstructed texture does not exercise a range of luma values")
    if decoded[2 * frame_size:3 * frame_size] != decoded[3 * frame_size:4 * frame_size]:
        raise SystemExit("The P-skip picture differs from its reference")
    for start, end in ((0, plane_size), (plane_size, frame_size)):
        if decoded[4 * frame_size + start:4 * frame_size + end] == decoded[3 * frame_size + start:3 * frame_size + end]:
            raise SystemExit("Inter residuals must change both the reconstructed luma and chroma")
print(f"FFmpeg decoded all {pictures} CABAC pictures with exact GPU reconstruction matches.")

if probe:
    properties = json.loads(subprocess.check_output(
        [probe[0], "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height,color_range,color_space,color_transfer,color_primaries,time_base:packet=duration",
         "-of", "json", output],
    ))
    stream, = properties["streams"]
    expected_metadata = dict(width=width, height=height, color_range="tv", color_space="bt709",
                             color_transfer="bt709", color_primaries="bt709")
    if any(stream.get(key) != value for key, value in expected_metadata.items()):
        raise SystemExit(f"Unexpected stream metadata: {stream}")
    durations = [int(packet["duration"]) * Fraction(stream["time_base"]) for packet in properties["packets"]]
    if durations != [Fraction(1, 60)] * pictures:
        raise SystemExit(f"Unexpected packet durations: {durations}")
    frames = json.loads(subprocess.check_output(
        [probe[0], "-v", "error", "-select_streams", "v:0", "-show_entries",
         "frame=pict_type", "-of", "json", output],
    ))["frames"]
    types = ["I"] + ["P"] * (pictures - 1) if raw else ["I", "I", "I", "P", "P"]
    if [frame["pict_type"] for frame in frames] != types:
        raise SystemExit(f"Unexpected decoded picture types: {frames}")
    print(f"FFprobe confirmed {'/'.join(types)}, {width}x{height}, limited-range BT.709, and 60 fps.")
