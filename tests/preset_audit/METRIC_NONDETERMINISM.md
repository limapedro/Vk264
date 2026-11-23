# progress_metrics.py was non-deterministic — fixed

`progress_metrics.py` selected `fs[0]` from `W.glob('exp*_*_64x64_8f_...')`.
Two experiments share that pattern (exp024 and exp084), with different
NAL2 first-diff bytes (49 vs 6). Depending on filesystem glob ordering the
reported superfast prefix changed between runs (observed 43 and 49), which
made progress appear to regress without any source change.

Fixed by sorting the glob results so the lowest experiment number is used
deterministically. The stable baseline is superfast first diff 49/130.

Quant8 investigation this turn: x264's quant8 tables for qp25 CQM_8IY are
`mf=745, bias=29, dequant8_mf=352`; our `vk_ref_quant_tables8` produces
identical values. Both encoders have pred=15, src=16, raw DCT DC=64,
quantized DC=1. The remaining 1-LSB reconstruction difference (oracle 16
vs x264 15) is therefore in IDCT rounding/pass alignment, not the quant
tables. No source changes landed; all gates green; audit 12/120.
