# Hypothesis list for the remaining 108/120 non-byte-exact cases

Generated 2026-11-24 from a fresh 120-case audit (`report.md`) and
all prior findings. Ranked by case count. Each hypothesis has a
concrete verification step; results are recorded below as they are
tested.

## Verified baseline (this turn)
```
audit 12/120 byte-exact (all ultrafast)
NAL2 byte-exact pairs 3/20
NAL3 superfast warzone 2f: first diff byte 7; 8f: byte 2
recon c004 (warzone 2f): 0/4096 byte-exact
IDR wholeframe decisions (c004): all 16 MBs match
slice types: 13/16
gates: CTest 29/29 + 65/65, CAVLC 1360/1360, MP4 56/56
```

## Failure classes from report.md (counts)

| class | verdict pattern | cases | examples |
|---|---|---|---|
| A | NAL2 byte4 (CAVLC) | ~20 | exp003,005,007,063,065,069,085 |
| B | NAL2 byte5 (CABAC fast..slower; CAVLC fast..slower) | ~28 | exp010,030,070,090,116 |
| C | NAL2 byte6 (CABAC superfast..faster, veryslow/placebo) | ~16 | exp064,066,086,018,038 |
| D | NAL2 byte10 (CAVLC fast..placebo on warzone) | ~10 | exp009,011,055 |
| E | NAL2 byte16 (CABAC veryfast/faster) | 4 | exp006,008,026,028 |
| F | NAL COUNT (extra I-slices / scene cuts) | ~25 | exp043,083,103,105 |
| G | NAL3 (P-slice payload) | 2 | exp004 (byte7), exp024 (byte2) |
| H | oracle PSNR `-` / green blocks (placebo CAVLC, veryslow) | ~12 | exp099,119 |

## Hypotheses

### H1 — IDR divergence is content-dependent, not preset-dependent
**Evidence:** c004 (warzone 2f) superfast CABAC NAL2 is byte-exact;
c064 (forza 2f) superfast CABAC diverges at byte 6; c004 fast CABAC
diverges at byte 5. Different first-diff bytes for different content
suggest the *first divergent MB type* differs (I4 vs I8 vs I16).
**Verify:** run the IDR bin differential on c064 (forza 2f) superfast
CABAC; find the first divergent bin and which MB/type it belongs to.
If the split is at a different MB type than c004, this is confirmed.

### H2 — P-slice I8 intra is the only remaining superfast-warzone gap
**Evidence:** exp004/024 have byte-exact NAL2; only NAL3 diverges.
Bin dump showed the header and three pred modes match, diverging at
the 4th (x264 mode 0 V vs oracle mode 2 DC) with equal per-mode SAD.
**Verify:** dump x264's x9 edge/cost for that block against the
oracle's; identify the input that flips the cost ordering.

### H3 — CAVLC byte4/byte10 are the CAVLC analogue of the I4 path
**Evidence:** ultrafast CAVLC is byte-exact (I16 only); all other
presets diverge at byte 4 or 10 — consistent with I4/I8 residuals not
being coded by the CAVLC path.
**Verify:** bin/bit differential on exp003 (superfast CAVLC warzone
2f) at NAL2 byte4; identify the MB syntax element.

### H4 — NAL COUNT mismatches come from slicetype 13/16
**Evidence:** x264 emits 34/36/40 NALs where the oracle emits 32;
`compare_slicetype.py` reports 13/16 frame types agree.
**Verify:** decode the frame-type sequence for exp043 (30f warzone
superfast CABAC) and identify the extra I-slice(s); check which
slicetype case is wrong.

### H5 — veryslow/placebo CAVLC decode failure is a hang/crash
**Evidence:** oracle PSNR `-` for exp017/019/037/039/057/059/077/079/097/117.
**Verify:** run the oracle CLI directly on that clip, capture exit
code and stderr.

### H6 — placebo green blocks are a malformed-stream bug
**Evidence:** exp099/119 oracle green_blocks 320/1600, PSNR ~4 dB.
**Verify:** decode with ffmpeg, inspect first MB's chroma/heading.

## Function/pipeline map to read (in order)
1. `Vk264-rewrite/src/ref/nal.c` — SPS/PPS writing (verified exact).
2. `Vk264-rewrite/src/ref/slice.c` — slice header (verified exact).
3. `Vk264-rewrite/src/ref/cabac_enc.c` — IDR (I4/I8/I16) + P-slice CABAC.
4. `Vk264-rewrite/src/ref/cavlc.c` — CAVLC residuals.
5. `Vk264-rewrite/src/ref/slicetype.c` + `tests/oracle_video_cli.c` — frame types.
6. `Vk264-rewrite/src/ref/pframe.c` — P-slice decisions/ME.

## Verification log

### H1 — CONFIRMED (content-dependent IDR divergence)
Ran `build/wf_pair.py` on c064 (forza 2f, superfast CABAC):
```
x264 MB2 type=1 cbpL=11 modes(I4 cache)=8,8,8,8,8,8,8,8,0,0,0,0,7,7,7,7
orcl MB2 type=1 cbpL=15 modes(I4 cache)=8,8,2,0,2,2,2,2,2,2,2,2,2,2,2,2
```
c004's 16 IDR MB decisions all match; c064 first diverges at MB2.
x264 I8 modes = `[8,8,0,7]`, oracle = `[8,8,2,0]`. Recon:
c004 = 0/4096 exact, c064 = 2325/4096 differing from MB2 onward.
So NAL2 byte4/5/6/10/16 are all the same class: the first divergent
MB type depends on content, not preset.

### H1a — REAL BUG FOUND AND FIXED: I8 HU predictor
x264 `predict_8x8_hu_c` (common/predict.c:865-884) uses packed F1/F2
pairs of the LEFT edge (`q[0..15]`, `out[y*8+x]=q[14-2*y+x]`).
The oracle's `i8_pred_edge` case 8 used a crude `EL(y+x/2)` — wrong.
Ported x264's exact pattern into `Vk264-rewrite/src/ref/i8x8decide.c`.
Effects: c064 recon 2325 -> 2209; c004 unchanged (0); gates unchanged
(29/29, 65/65, 1360/1360, 56/56), audit 12/120, NAL2 3/20.

### H1b — filtered-edge gating is content-dependent (tested, reverted)
Forcing `vk_predict8_filtered_ex` on top/left MB rows improved c064
MB2 (210->107) but regressed c004 MB3 (0->64). x264's edge filter
behaviour differs for unavailable neighbours; the existing gating
`mx>0 && my>0` is required for c004. Reverted.

### H2 — partially verified
c004 MB1 (P slice) x264 x9 costs `45,63,63,592,63,204,63,591,63`
(λ=6); oracle per-mode SAD uniform 39 — the mode split is NOT
explained by the cost array alone; instrumented `vk_i8x8_decide_ex`
returned mode 2 with `bc=21` despite `pred8=0`. Need a deeper look at
context state vs SAD values. No fix landed.

### H5 — CONFIRMED, root cause identified, fix insufficient alone
Reproduced the veryslow CAVLC decode failure on `c017.yuv`:
```
oracle CLI exit=0, 414-byte stream
ffmpeg: illegal reordering_of_pic_nums_idc 32 / no frame
x264 NAL sizes: [7:25, 8:6, 5:120, 1:20]
oracle NAL sizes: [7:25, 8:6, 5:216, 1:151]
```
Instrumented x264 `slice_header_write`:
```
VH fn=1 poc=2 ovr=1 nl0=1 defl0=16 wpred=1
```
x264 (`encoder.c:2639`) sets `i_num_ref_idx_l0_active` to the number
of available references (1 for the first P frame) and signals
`num_ref_idx_active_override_flag=1`; the oracle always wrote 0 and
its pred_weight_table looped over the PPS default (16) instead of the
active count.

Implemented both corrections in `pframe.c`: override signalling and
active-count weight table. The error changed from
`illegal reordering_of_pic_nums_idc 32` to `no frame`, but the stream
still does not decode and the audit stays 12/120 — the IDR NAL2 is
216 bytes vs x264's 120, i.e. the CAVLC IDR residual path itself is
substantially wrong for multi-ref presets. Changes reverted; the
header bug is documented for the CAVLC rewrite.

### H3/H4/H6 — not yet tested
