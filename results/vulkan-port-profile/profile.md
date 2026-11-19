# x264-port (Wx264/vulkan) profile @ mandelbrot 1080p60, --qp 34 --bitrate 6000

| stage | calls/238fr | total ms | per frame | % of GPU |
|---|---|---|---|---|
| motion seek (resident_p_motion) | 238 | 2494.3 | 10.48 | 43% |
| Residual chain (prepare+luma4+luma8+chroma+join) | 3808×5 | 2403.9 | 10.10 | 41% |
|  deblock | 238 | 433.3 | 1.82 |  7% |
|  entropy (CAVLC) | 238 | 402.0 | 1.69 |  7% |
|  upload + prefilter + download + packet | 238×4 | 96.3 | 0.40 |  2% |
| **GPU total per P frame** | | | **~24.5 ms** | |
| IDR frames (2 total) | | 200.3 ms | ~100 ms each (amortised ~0.8 ms/frame) | |

240 frames of `clips/test/mandelbrot_1080p60_240f.yuv`, `--backend vulkan --entropy cavlc`, GTX 1660 SUPER.
