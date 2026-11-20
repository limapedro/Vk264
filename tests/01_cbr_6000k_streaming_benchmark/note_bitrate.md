# CBR bitrate correction + GOP60 finding

## Bitrate was correct — my reporting formula was inverted

The first `summary.json` reported `1.7 kbps` actual, which was wrong. The
formula used `bytes*8/frames/fps/1000` (dividing by frames*fps). Correct:

    actual_kbps = bytes * 8 / (frames / fps) / 1000 = bytes*8*fps/frames/1000

For the GOP60 run: 8,245,978 bytes × 8 / 11.0 s / 1000 = **5,997 kbps** —
essentially exactly the 6000 kbps target. Both encoders hit the target:
- vk264: 8,245,978 B → 5,997 kbps
- x264:  7,966,952 B → 5,796 kbps

So vk264's ABR rate control does converge to the target bitrate on this
content; the "1.7 kbps" figure was a reporting bug in the benchmark script,
now fixed in `run_gop1.ps1`.

## GOP60 CBR run: P-slice corruption (same as experiment 00)

vk264 GOP60 CBR stream: `top block unavailable for requested intra mode` —
decode ERRORS. x264 GOP60 CBR: CLEAN. Quality metrics from that run are
unreliable (PSNR 14.7 dB reflects corrupted frames, not encoder quality).

The clean `run_gop1.ps1` (GOP=1, all-IDR) avoids the P-slice path and gives
valid quality metrics. Results in `summary_gop1.json`.
