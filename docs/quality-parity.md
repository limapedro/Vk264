# Quality parity study

## Where the blocks come from

I aligned the Warzone decode and raw source by frame index and measured the absolute jump in luma reconstruction error across each horizontal boundary. A jump means the error changed between adjacent rows even after accounting for the source edge. On frame 30, the original one-row P slices had a mean jump of 5.30 luma levels at 16-pixel boundaries and 2.92 levels eight pixels away. Every horizontal macroblock boundary was a slice boundary. The slice header selected within-slice deblocking, so those boundaries received no filtering. About 34% of the worst-error 10% of macroblocks were zero-motion P-skip. [ITU-T H.264, section 8.7](https://www.itu.int/rec/T-REC-H.264-202408-S/en)

| Warzone frame 30 | At rows 16, 48, … | At rows 32, 64, … | Eight pixels off boundary | PSNR-Y |
|---|---:|---:|---:|---:|
| Wx264 one-row baseline | 5.39 | 5.20 | 2.92 | 30.52 dB |
| Wx264 two-row slices | 2.46 | 4.50 | 2.39 | 31.79 dB |
| Wx264 two-row slices and SATD | 2.36 | 4.25 | 2.26 | 32.08 dB |
| x264 slow | 1.51 | 1.47 | 1.32 | 37.15 dB |

With within-slice filtering, the two-row layout filtered every other horizontal macroblock boundary in P pictures. It also made the upper macroblock available for coefficient context and motion prediction. The encoder only emits P-skip when the decoder's inferred vector matches the chosen reconstruction and there is no coded residual. The experiment that penalized the largest local zero-motion skip error raised aggregate PSNR-Y by 0.02 dB on Warzone and stayed out of the default.

## Full-frame filtering and flat-area intra

The full-frame filter follows H.264 macroblock dependencies in a GPU wavefront. On Warzone frame 30, the mean error jump at the old slice boundaries fell from 4.18 luma levels to 2.17 with full-frame filtering. The neighboring macroblock boundaries measured 2.28. On Fortnite frame 8, the slice-boundary jump fell from 6.59 to 3.54.

Filtering alone left false texture in dark, flat areas. Inter prediction could carry reconstructed texture forward while quantization discarded the residual that would have removed it. The two-row P path now evaluates a DC-only Intra16×16 candidate for these macroblocks, using the source and reconstructed Y, Cb and Cr samples to compare distortion and an estimated syntax cost. On Warzone frame 60, 5,012 complete macroblocks had source luma mean below 64 and variance at most 9. Their mean variance was 0.33 in the source, 6.63 with full-frame filtering alone and 0.41 with the intra candidate enabled. Luma RMS error in those blocks fell from 3.90 to 1.34 levels between the two filtered runs.

Both fixes are enabled by default on the two-row P path. These 600-frame runs used 1080p60, the `slow` preset and a 6,000 kbit/s target. Loop throughput excludes Vulkan initialization. FFmpeg matched all 1,200 reconstructed pictures sample for sample.

| Clip | Achieved kbit/s | Previous PSNR-Y | Updated PSNR-Y | Previous SSIM | Updated SSIM | Updated loop fps |
|---|---:|---:|---:|---:|---:|---:|
| Warzone | 6034.4 | 32.65 dB | 33.25 dB | 0.9274 | 0.9403 | 104.6 |
| Fortnite | 6046.8 | 32.08 dB | 32.45 dB | 0.8998 | 0.9095 | 91.7 |

## Block boundaries in the reported Warzone frame

The reported screenshot matches source frame 31, at 0.517 seconds. In the region below the window (x 0 to 527, y 544 to 799), the absolute luma error jump averaged 0.94 levels across horizontal 16-pixel boundaries and 0.80 across vertical ones. This measurement includes boundaries where the source changes by at most four levels. At boundaries between inter blocks, the mean was 1.67 levels when their motion vectors differed and 0.66 when they agreed. In 25 sampled Fortnite frames the corresponding means were 1.62 and 1.26. These measurements associate the visible pattern with block prediction and correction.

Inter DC previously used the same wide quantizer dead zone as AC. Rounding DC to the nearest level preserves smaller corrections to the predicted average, with the reconstruction and deblocking path using the same signaled quantizer. At the 6 Mbps target this raised `slow` VMAF from 76.67 to 78.73 on Warzone and from 63.00 to 65.18 on Fortnite. The Warzone crop's horizontal and vertical boundary error jumps fell to 0.90 and 0.77 levels. The sampled Fortnite jumps rose slightly. Color patches in the Warzone crop and the gap to x264 remain visible.

## x264 slow

x264 `slow` selects subme 8, five references, automatic direct motion prediction, trellis for mode decisions and final coding, and 50 frames of lookahead. It inherits CABAC, deblocking, B pictures, and variance AQ. Its motion search uses SAD in the full-pixel stage and can use SATD plus motion-vector cost when refining at subpixel positions. Its macroblock RD cost evaluates the actual coded reconstruction and an entropy bit-count model. [Preset and defaults](https://github.com/mirror/x264/blob/master/common/base.c) · [Motion search](https://github.com/mirror/x264/blob/master/encoder/me.c) · [Mode analysis](https://github.com/mirror/x264/blob/master/encoder/analyse.c) · [RD cost](https://github.com/mirror/x264/blob/master/encoder/rdo.c)

I ran x264 `slow` on the first 240 Warzone frames using a 6 Mbps target, a 12 Mbit VBV buffer and a fixed 120-frame GOP. The feature changes share a common target with different achieved rates.

| x264 variant | Average Mbps | PSNR-Y | SSIM |
|---|---:|---:|---:|
| Slow | 5.91 | 38.89 dB | 0.9788 |
| CAVLC, trellis off | 5.99 | 37.76 dB | 0.9743 |
| I/P pictures only | 6.18 | 38.85 dB | 0.9785 |
| One reference | 6.00 | 38.81 dB | 0.9785 |
| Smaller motion search and lighter subpixel refinement | 5.86 | 38.09 dB | 0.9744 |
| 4×4 transform only | 5.93 | 38.51 dB | 0.9770 |
| Restricted tools together | 6.15 | 37.31 dB | 0.9668 |

The restricted run used CAVLC, I/P pictures, one reference, 4×4 transforms and `partitions=none`. It also disabled trellis, AQ, MB-tree and weighted P prediction. Its remaining intra choices, motion refinement and RD decisions give a more relevant target for the current GPU pipeline. The small standalone changes from removing B pictures or extra references on this prefix are limited to this clip and bitrate.

### CABAC and trellis measured separately

I repeated the first 240 frames with the same x264 `slow` settings and switched CABAC while holding trellis off. CABAC learns probabilities for binary symbols from context and uses arithmetic coding to spend fewer bits on likely outcomes. It can encode the same chosen syntax losslessly with fewer bits. At a constrained bitrate, the encoder can spend the savings on prediction and residual detail. [CABAC design paper](https://iphome.hhi.de/marpe/download/cabac_ieee03.pdf) · [x264 defaults and presets](https://github.com/mirror/x264/blob/master/common/base.c)

| x264 slow variant | Average Mbps | PSNR-Y | SSIM |
|---|---:|---:|---:|
| CAVLC, trellis off | 5.983 | 37.76 dB | 0.97440 |
| CABAC, trellis off | 5.939 | 38.74 dB | 0.97854 |
| CABAC, trellis 2 | 5.914 | 38.89 dB | 0.97883 |

These runs measure end-to-end rate-distortion behavior at a common bitrate target. With CABAC enabled, x264 can also change mode decisions based on the different estimated entropy cost. CABAC and deblocking are shared by x264 `medium` and `slow`. The `slow` changes include deeper I/P mode refinement, five references, automatic B-direct prediction, trellis at every mode decision, and 50 frames of lookahead. [x264 rate-distortion cost](https://github.com/mirror/x264/blob/master/encoder/rdo.c) · [x264 preset definitions](https://github.com/mirror/x264/blob/master/common/base.c#L521-L532)

## GPU timing and remaining work

Serial CAVLC for the longer P slices reduced the initialized Warzone loop to 92.6 fps. Encoding macroblock bodies in parallel and joining the bits before emulation prevention reduced the measured entropy stages from 3.33 to 0.55 ms per frame. The output was byte-identical and throughput rose to 120.6 fps. Full-resolution SATD on four shortlisted motion vectors brought the completed runs to 109.5 fps on Warzone and 105.7 fps on Fortnite, with 32.43 and 32.09 dB PSNR-Y respectively. Both runs used all 600 frames.

The reference-picture presets now configure the same budgets as x264. In a full 600-frame check at a 6 Mbps target, enabling five references improved Warzone from 32.43 to 32.65 dB and left Fortnite near 32.09 dB. Older references were selected for about 1.9% and 2.8% of P-picture macroblocks, respectively. The initialized encode loops ran at 111.3 and 105.8 fps. The one-reference controls were byte-identical to the previous streams. A synthetic clip with a 16-frame repeat selected the sixteenth reference and matched FFmpeg reconstruction through multiple IDRs.

The remaining block artifacts are visible on textured regions and along fine edges in motion. The current intra candidate only covers dark, flat macroblocks. Broader prediction choices, more accurate residual costing, strict VBV and x264 `slow` parity remain open.
