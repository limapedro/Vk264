# Tiled Vulkan motion search

Measured on the GTX 1660 SUPER using the resident batched CAVLC path at 1920x1080, 60 fps, a 120-frame GOP, and a 6,000 kb/s target.

## GPU timings

Warzone timings average the 119 P-frames in a 120-frame sample. All samples produced the same H.264 bytes. The 2x2 row comes from the first build of the same two-row tiling schedule. The other timings come from the paired comparison in the second build.

| Schedule | Motion dispatches per P-frame | Motion (ms/P-frame) | GPU graph (ms/P-frame) |
|---|---:|---:|---:|
| baseline | 254 | 11.44 | 25.20 |
| 2x1 | 161 | 13.04 | 26.26 |
| 4x1 | 114 | 18.25 | 32.23 |
| 2x2 | 127 | 14.82 | 28.07 |
| 4x2 | 80 | 15.61 | 29.23 |
| baseline-repeat | 254 | 12.20 | 26.66 |

## 600-frame throughput

The comparison uses the 2x1 candidate from the paired GPU profile. Process timing includes startup and raw file I/O.

| Clip | Default | 2x1 tiles | SHA-256 match |
|---|---:|---:|---|
| Warzone | 24.72 fps | 22.13 fps | Yes |
| Fortnite | 23.51 fps | 23.04 fps | Yes |

The tiling uses (x+y, y) coordinates so that H.264 motion predictors stay on earlier tile diagonals or earlier steps inside a tile. Single-row tiles carry the left predictor within a subgroup. Two-row tiles exchange internal predictors through shared memory. The 4x2 schedule cuts dispatches by 69%. At 1080p, the default schedule has about 32 active macroblocks per wave on average, each assigned to one subgroup. The 2x1 schedule averages about 26 active tiles per wave and processes the two macroblocks in each tile in sequence. Two-row tiles also introduce workgroup barriers inside each dispatch.

The tiling modes are opt-in using WX264_MOTION_TILE. The default remains the batched wave schedule.

Raw results [first tile comparison](profile-runs.json), [paired GPU profile](paired-runs.json), and [full clip comparison](full-runs.json).

The tiled shader passed SPIR-V validation. Motion-search parity and default CAVLC reconstruction passed. CAVLC reconstruction also passed with all four tile sizes.
