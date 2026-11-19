# Vulkan motion search batching

Measured on the GTX 1660 SUPER with the GPU-resident CAVLC path. The source was 1920x1080 at 60 fps. The target video rate was 6,000 kb/s with average-rate feedback and a 120-frame GOP.

## 600-frame process throughput

| Clip | Previous build | Batched candidates | Active-rows trial | x264 Fast reference |
|---|---:|---:|---:|---:|
| Warzone | 25.13 fps | 26.15 fps | 24.88 fps | 106.14 fps |
| Fortnite | 22.77 fps | 26.65 fps | 23.66 fps | 98.28 fps |

The shipped build and the active-rows trial both matched the previous version's SHA-256. The x264 Fast values are references from the prior benchmark. Process timing includes startup and raw file I/O. Disk cache and pipeline compilation can affect these figures.

## Warzone GPU timings

Vulkan queue timestamps averaged across the 119 P-frames in a separate 120-frame run.

| Stage | Previous build | Batched candidates | Active-rows trial |
|---|---:|---:|---:|
| Motion estimation and compensation | 13.93 ms | 10.60 ms | 11.25 ms |
| GPU-resident P-frame graph | 27.76 ms | 23.40 ms | 24.76 ms |

The batched shader tests up to four candidates together for full-pixel diamond and square searches, the UMH hexagonal grid, and half- and quarter-pixel refinement. Each batch retains the original order for tied costs. A trial dispatched only active rows of each dependency wave, reducing workgroups per P-frame from 8,636 to 4,140 at 1080p. It ran slower on the profiled Warzone clip, so the shipped build retains the original dispatch geometry.

A broader experimental variant triggered a Vulkan device loss on the first P-frame. The build uses the narrower batching variant validated here. GPUs without 32-lane subgroups and shuffle support continue to use the previous motion cost kernels.

Raw profiles [before](baseline.log), [batched](final-profile.log), and [active-rows trial](trimmed-profile.log). Process timings and hashes are in [throughput.json](throughput.json) and [trimmed-throughput.json](trimmed-throughput.json).
