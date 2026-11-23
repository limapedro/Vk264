# Reconstruction gap is in final-encode FDEC state

With skip-intra and early-terminate disabled, x264's analysis path for
frame-0 MB1 idx1 gives:

```
predmode (predicted) = 2
chosen mode = 9 (DC_LEFT)
prediction = 67,67,67,67,...   (oracle: 67,67,67,67,... — MATCH)
quantized DCT = all zero, nz=0   (oracle: residual quantized away — MATCH)
```

So the prediction and quantization primitives agree in the analysis
pass. Yet x264's final reconstructed frame has pixel (22,0)=68 while
the oracle has 67.

Conclusion: x264's final `x264_macroblock_encode` re-encodes with a
different FDEC neighbour state (neighbouring MBs' final reconstruction)
than the analysis pass used. The oracle's single-pass reconstruction
matches the analysis pass, not x264's final encode. This explains why
isolated primitive checks (IDCT, quant tables, prediction) all pass
while the final frames differ by 1460 bytes.

Fixing this requires porting x264's two-phase analyse/final-encode FDEC
state separation, not another primitive tweak. No source change landed.

Verified state: MB0-5 decisions match, NAL2 prefix 50/130, all gates
green, audit 12/120, parity 2/10.
