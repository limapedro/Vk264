# CAVLC packet assembly and frame dependencies

GTX 1660 SUPER, 1920x1080 at 60 fps, 6,000 kb/s target, 120-frame GOP. The paired Warzone profile alternated the old serial packet algorithm and the new one in the same encoder build, using the same dispatch sequence. Each run covered one IDR and 119 P-frames.

| GPU interval | Serial | Parallel |
|---|---:|---:|
| NAL assembly per P-frame | 0.989 ms | 0.028 ms |
| NAL assembly per IDR | 8.884 ms | 0.187 ms |
| Full P-frame graph | 25.018 ms | 23.283 ms |
| Motion stage per P-frame | 11.355 ms | 10.964 ms |

The parallel shader computes the emulation-prevention counts for 64-byte chunks, assigns their output positions, then emits the chunks in parallel. Bytes at a chunk boundary retain the zero-run state of the earlier chunk.

## 600-frame process throughput

The same encoder build ran each clip with both packet shaders. The Warzone pair ran parallel first. The Fortnite pair ran serial first. Each pair used matching inputs and settings.

| Clip | Serial | Parallel | Gain | Bitstream match |
|---|---:|---:|---:|---|
| Warzone | 26.61 fps | 27.39 fps | 2.9% | Yes |
| Fortnite | 25.95 fps | 26.56 fps | 2.3% | Yes |

The initial before-and-after builds measured lower overall throughput with the new shader while other GPU stages also ran slower. Timing varies with startup, file I/O, and system load. The paired data gives a cleaner comparison, with one measured pair per clip.

## What a four-frame queue can overlap

| Stage | Inputs that control when it can run |
|---|---|
| Raw input and staging | Source pixels for that frame. Can run ahead into separate slots. |
| P-frame rate control and header | The previous frame's actual bit count and rate-control state. |
| Reference filtering and motion | The preceding frame's deblocked reconstruction. Motion-vector predictors also impose dependencies within the frame. |
| Transforms and reconstruction | Motion predictions and the selected QP. P-frame residual work can run in parallel across macroblocks. |
| Deblocking | Reconstructed pixels and block metadata. It makes this frame available as the next reference. |
| Entropy and packet assembly | The current frame's coefficients and slice header. This branch supplies the bit count for rate control. |

A four-slot queue could overlap source reads and uploads with GPU work and let output drain asynchronously. The current CLI uses one shared arena and waits for the full frame before updating rate control. Consecutive P-frames would still advance in reference order. Four independent GPU jobs would require separate GOPs with independent rate control or changes to the reference structure. Those changes can affect the bitstream and quality.

A live queue can submit as soon as a frame is ready. Waiting for all four arrivals at 60 fps would add 50 ms before processing the first frame in each group. The profiled raw file read averaged 2.42 ms per frame, so hiding file input alone would recover only a small portion of the total.

Raw results [paired shader profiles](paired.json), [paired full clips](full-paired.json), [original build](before.json), and [parallel build](after.json).

CAVLC reconstruction, bitstream parity, and NAL parity passed. [Test log](ctest.log).
