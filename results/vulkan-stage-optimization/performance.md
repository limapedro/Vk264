# Wx264 Vulkan CAVLC performance

Benchmarked at 1920×1080, 60 fps, GOP 120 and a 6,000 kb/s video target on a GTX 1660 SUPER and Core i5-12400F. Wx264 used the GPU-resident CAVLC prototype with single-reference P-pictures and average-rate feedback. x264 used its Fast preset with VBV enabled.

## 600-frame end-to-end runs

| Clip | Wx264 before | Wx264 after | Gain | x264 Fast | Wx264 VMAF | x264 Fast VMAF |
|---|---:|---:|---:|---:|---:|---:|
| Warzone | 18.46 fps / 32.50 s | 25.13 fps / 23.88 s | 1.36× | 106.14 fps / 5.65 s | 65.13 | 92.20 |
| Fortnite | 16.93 fps / 35.44 s | 22.77 fps / 26.35 s | 1.34× | 98.28 fps / 6.11 s | 52.11 | 87.78 |

Process timings include encoder startup, raw input and output writes. The Wx264 columns use the earlier baseline and the final build. The x264 Fast reference is from the comparison run after the shader and staging updates. Disk caching affects elapsed times.

The final Wx264 bitstreams were byte-identical to the original 600-frame outputs. Both 60 fps clips decoded all 600 frames. VMAF was measured against each source, and the final Wx264 bitstreams match the streams used for those measurements.

- Warzone [Wx264](final-warzone-600/wx264-vulkan-cavlc.mp4) and [x264 Fast](final-warzone-600/x264-fast.mp4).
- Fortnite [Wx264](final-fortnite-600/wx264-vulkan-cavlc.mp4) and [x264 Fast](final-fortnite-600/x264-fast.mp4).

## GPU intervals

Vulkan queue timestamps on the first 120 Warzone frames. The P-frame columns average 119 frames. The IDR columns show one frame. Residual stage timings sum all dispatch batches within each frame. The baseline graph was timestamped from after upload to the assembled packet. The final total also includes the two transfer intervals. The packet shader writes the compressed bitstream directly into mapped memory.

| Stage | P before (ms) | P after (ms) | IDR before (ms) | IDR after (ms) |
|---|---:|---:|---:|---:|
| Staging upload | — | 0.278 | — | 0.280 |
| Reference interpolation | 0.090 | 0.096 | — | — |
| Motion estimation and compensation | 22.108 | 13.512 | — | — |
| Residual setup and transform selection | 4.385 | 3.043 | 29.852 | 28.630 |
| 4×4 luma residual | 1.718 | 1.744 | 51.710 | 49.964 |
| 8×8 luma residual | 3.320 | 1.780 | — | — |
| Chroma residual | 0.928 | 0.909 | 25.014 | 27.177 |
| Coefficient gathering and reconstruction | 2.577 | 1.187 | 21.592 | 12.555 |
| CAVLC and slice assembly | 4.294 | 1.524 | 8.168 | 2.107 |
| In-loop deblocking | 1.939 | 1.928 | 2.762 | 3.631 |
| NAL packet assembly | 0.854 | 0.859 | 8.361 | 8.373 |
| Final metadata copy | — | 0.006 | — | 0.007 |
| **Total resident graph** | 42.213 | 26.866 | 147.459 | 132.723 |

Motion search consumed 50% of the final P-frame GPU time. That stage alone averaged 13.51 ms. A full encode must fit within 16.67 ms per frame at 60 fps and roughly 9–10 ms per frame to reach the x264 Fast measurements on these clips.

## CPU work and transfers

| Measurement | Before | After |
|---|---:|---:|
| Host time for the resident P call, including waiting and GPU work (ms/P-frame) | 44.742 | 27.647 |
| Raw input and padding (ms/frame) | 7.928 | 2.269 |
| Copy into mapped staging (ms/frame) | — | 0.229 |
| Rate-control update and slice header shaders (combined GPU ms/frame) | 0.023 | 0.023 |
| Host NAL output and file writes (ms/frame) | 0.279 | 0.165 |

Host measurements include potentially overlapping or nested scopes and disk-cache effects. One-time shader setup and file open are reflected in process throughput. Each picture uploads about 3.0 MiB. P-pictures averaged about 12 KiB of compressed bytes and metadata read by the host. The reference picture stays in device memory.

## Changes

- Luma motion estimation loads four pixels at a time for SAD and SATD. The packed path also reuses those loads for luma compensation.
- Transform selection uses each 4×4 Hadamard result for both the 4×4 and 8×8 costs. Four GPU lanes process the four 8×8 luma blocks of each macroblock.
- Coefficient gathering and CAVLC skip uncoded residual data.
- Slice assembly finds the bit offsets, then merges independent macroblock payloads in parallel.
- Input planes go straight into mapped staging, avoiding intermediate plane copies. The file harness reads each contiguous full-width plane in one call.

Stage traces are saved in [before.log](before.log) and [contiguous-profile.log](contiguous-profile.log).

Final timing data and hashes are saved in [contiguous-throughput.json](contiguous-throughput.json).

All 45 tests passed after the shader and staging changes. After the contiguous input change, all three reconstruction tests passed again. [Full suite](full-ctest.log) and [final reconstruction](final-reconstruction-ctest.log).
