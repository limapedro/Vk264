# Bottom-up correctness tracking on real footage (Subaru, 2026-11-20)

Source clip: `E:\__PROJECTS__\Wx264-Vulkan-Opencode\clips\youtube\forza_original_30s_1080p60_240f.yuv` (1920x1080, 4:2:0).
Tool: `python harness/parity_check.py --input <crop.yuv> --width W --height H --frames 1 --preset P --gop 1`,
which encodes with `vk264 --backend vulkan` and libx264 at the same QP (28), strips encoder-identity SEI, and compares Annex B bytes.
Crops were extracted with `ffmpeg -f rawvideo -pix_fmt yuv420p -s 1920x1080 -i <clip> -vf crop=W:H:X:Y`.
The harness labels the first mismatch "PPS NAL" because it only matches 4-byte start codes; the differing bytes are in the slice data.

## Results

### 1. Function / shader level — byte-exact
`ctest --test-dir build -C Release` = **52/52**, including all four automated parity gates.
Every isolated x264 C function (DCT, quant, predict, deblock, MC, ME, CABAC, CAVLC, bitstream, SPS/PPS) matches its GPU shader byte for byte.

### 2. Single macroblock of real content — byte-exact
`crop=16:16:128:128`, ultrafast:
```
BYTE-EXACT MATCH (ignoring encoder-identity SEI)
decoded reconstruction: 0 / 384 bytes differ
```
(The x264 stream is 614 bytes before SEI stripping and ~71 after — x264 writes a large options SEI.)

### 3. Size threshold — divergence starts at the second macroblock row
Same location (128,128), ultrafast, square crops:

| crop | result |
|---|---|
| 16x16 | BYTE-EXACT MATCH |
| 32x32 | first mismatch at offset 76 |
| 48x48 | first mismatch at offset 78 |
| 64x64 | first mismatch at offset 81 |
| 80x80 | first mismatch at offset 85 |
| 96x96 | first mismatch at offset 86 |
| 128x128 | first mismatch at offset 93 |

Decoded reconstruction for 32x32 / 64x64:
- 32x32: first differing pixel at (16,16) = **MB(1,1)**; 164/1536 bytes differ.
- 64x64: first differing pixel at (16,16) = **MB(1,1)**; 1751/6144 bytes differ.

MB(1,1) is the first macroblock that has **both a left and a top neighbour**. The first row/column of
MBs (which have only one or no neighbour) reconstruct identically.

### 4. Location/preset dependence
32x32 crops at different locations:

| crop origin | ultrafast | fast |
|---|---|---|
| (128,128) | first recon diff MB(1,1) | — |
| (500,300) | BYTE-EXACT (recon exact) | first recon diff MB(0,0) |
| (800,500) | first recon diff MB(1,1) | first recon diff MB(0,0) |
| (100,900) | first recon diff MB(0,2) | first recon diff MB(1,1) |

### 5. Full 1080p frame
| preset | bitstream first mismatch | reconstruction |
|---|---|---|
| ultrafast | offset 829 | first diff at Y(128,128) = MB(8,8); 46.06% of bytes differ |
| fast | offset 126 | first diff at Y(270,126) = MB(16,7); 47.08% differ |
| superfast | offset 45 | first diff at Y(0,0); 48.99% differ |

Rows 0-7 and 52-66 are letterbox (black bars) and reconstruct identically; the middle rows are textured gameplay.

## Interpretation

- The **compute primitives are correct** — every isolated shader matches x264 byte for byte.
- **Single-MB frames, even with real content, are byte-exact** (ultrafast).
- Divergence appears only with **multiple macroblocks**, and specifically at the first MB whose
  prediction/context depends on both left and top neighbours (ultrafast), or immediately at MB(0,0)
  on textured content for `fast`.
- This localises the remaining parity gap to the **decision / orchestration layer**, not the shader
  primitives: intra/inter mode selection (`inter_frame.comp`, issue #3), neighbour-context derivation
  and multi-MB neighbour handling (issue #8), and I4x4 emission for subme<6 (issue #2).
- `superfast` fails from the first pixel, consistent with the already-confirmed I4x4-vs-I16x16 root cause.

## Reproduce
Artifacts are under `build/thresh-*`, `build/crop16-uf`, `build/loc-*`, and `build/real-*`.
