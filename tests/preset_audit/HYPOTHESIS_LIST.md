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

### H1 update — c064 MB2 residual quantified
Dumped writer-pass I8 coefficients for c064 MB2 (x264) and the
oracle's `i8q[4][64]`:
```
x264 q0=[0,0,0,0,-1,0...] q1=[-1,0,0,0,-1,0...]
      q2=[1,0,-1,0...]    q3=[-2,1,0,0...]
orcl q0=[0,-1,0,0...]     q1=[-3,2,0,0...]
      q2=[0,0,0,0,0,0,0,0,1,0...] q3=[-1,1,0,0,0,0,0,0,2,0...]
```
Differences are small (one or two levels) but change which
coefficient is nonzero, so reconstructed pixels differ by a few LSB
and cascade. This is quant/rounding sensitivity to slightly different
predictions, not a syntax bug.

### H2 update — full 9-mode search tested and reverted
Replaced the oracle's I8 x3+shortcut with an all-9-mode search
(matching x264 x9 breadth). Result: c064 recon 2209 -> 2265 (worse),
c004 stayed 0, audit 12/120, NAL2 3/20. Reverted. The mode choice
difference is therefore in the *edge/cost inputs*, not the candidate
set. A per-call decision dump showed
`satd=[52,56,26], pred8=0, lambda=6 -> bm=2, cost=38`: DC genuinely
wins in the oracle's cost model for that block, so matching x264
requires matching the edge samples feeding SAD, not the search.

### H1/H2 edge dump (c064 MB2) — precise input difference
Dumped `edge[36]` for c064 MB2 idx0 (I8, top MB row):
```
x264: rawl=203,203,203,203,200,199,196,195
      rawt=135,69,89,65,180,5,90,65
      filtered edge[16..23]=117,119,91,78,100,108,70,63
orcl: rawl=203,203,203,203,200,199,196,195   (identical, verified
                                               by a direct l8 dump)
      rawt=128,128,128,128,128,128,128,128   (top unavailable)
```
The left neighbours match exactly; the divergence is the **top
samples for a top-MB-row I8 block**. x264's `i_neighbour8[0]` for
MB2 is `1` (LEFT only), yet `predict_8x8_filter(..., ALL_NEIGHBORS)`
still filters the top edge from FDEC padding, which on c064 contains
highly variable data. The oracle substitutes 128 for unavailable
top neighbours. This is why the I8 cost/SAD inputs diverge on
textured content but happen to agree on flat c004.
Any fix must reproduce x264's FDEC top-padding content for top-row
MBs rather than substituting 128.

**Tested (negative result):** the padding values are identical for
c004 and c064 (`135,69,89,65,180,5,90,65`), i.e. a stable initial
`intra_border_backup` allocation state, not content. Initialising the
oracle's unavailable top samples with that pattern changed neither
recon (c064 2209, c004 0) nor NAL2 pairs (3/20). So the padding is
NOT the driver; the divergence is elsewhere. Reverted.

### H3 — CONFIRMED (CAVLC I4/I8 not implemented in normal path)
Read `Vk264-rewrite/src/ref/pframe.c:1168`: the CAVLC P-slice
I4/I8 emitters (`vk_encode_i4x4_mb`, `vk_encode_i8x8_mb`, mb_type
offset 5) are gated behind `use_intra && (p7==0 || p7==1)`, and `p7`
is only nonzero when `VK_P7_TEST` is set with `frame_num==4 &&
poc==14`. Normal CAVLC intra MBs fall through to I16-only
(`use_intra` branch at line 1235). Explains the CAVLC byte4/byte5/
byte10 classes: ultrafast (I16-only) is byte-exact, every other
preset needs I4/I8 and has no normal-path emitter. Fix requires
wiring the existing emitters to real `vk_i4x4_decide`/I8 decisions.

**IDR CAVLC attempt:** wired real I4/I8 decisions into
`encode_idr_cavlc` for all MBs with an I16 cost comparison. Type
decisions and costs matched x264 exactly for MB0-3:
```
MB0 ctype=I4 ic16=28682 c4=2194
MB1 ctype=I4 ic16=9576  c4=975
MB2 ctype=I16 ic16=155  (x264 I16)
MB3 ctype=I8 ic16=357  c8=186 (x264 I8)
```
NAL2 first-diff improved byte4 -> byte17, but NAL2 grew 210 -> 242
bytes and decoded PSNR dropped 24.49 -> 11.5 dB: the CAVLC
`vk_encode_i4x4_mb` / `vk_encode_i8x8_mb` emitters themselves emit
wrong residual/nnz state. Reverted. Next prerequisite: fix those
emitters, not the decision logic.

**Attempted (negative result):** removed the P7 gate and wired
`vk_i4x4_decide`/`vk_i8x8_decide_ex` into the normal CAVLC path,
choosing min(I16,I4,I8) cost. Decisions fired (MB1: ca_type=1,
icost=2856, c4=1828, c8=1256) but x264 chooses **I4** for that MB
with i4=975 — the oracle's I4 cost model is ~2x too high, so it
picked I8. **Correction:** the I4 primitive itself is correct — on
c004 MB1 the oracle's `vk_i4x4_decide` returns exactly 975, matching
x264's i4=975. The 1828 reading was a bug in my new helper: I passed
`cur` (frame stride 64) where `vk_i4x4_decide` expects a tight
stride-16 source patch. After fixing that (and pointing `recon_y` at
the current MB), MB1 correctly selects **I4** (c4=602, c8=7198),
yet the audit still stayed 12/120, NAL2 3/20, CAVLC byte4 unchanged.
So the remaining CAVLC gap is in the emitter/header details, not the
cost primitive. Reverted.

### H6 — CONFIRMED (same root cause as H5)
Reproduced on `c099.yuv` (forza 8f placebo CAVLC): CLI exits 0 but
ffmpeg reports `luma_log2_weight_denom 32 is out of range`, the
same PPS-weight-table misalignment as H5. H5/H6 are one class.

### H4 — partially verified
`compare_slicetype.py` shows 3/16 mismatches in display order:
frames 2 (B->P), 5 (P->B), 8 (B->P) — b-adapt decisions in
`vk_slicetype_decide`, not scene cuts.

Dumped path costs on both sides:
```
x264  j=2 (first pass): cp=639  cb=521  -> B
orcl  j=2:             cp=659  cb=749  -> P
```
The P-path cost is close; the oracle's **B-path cost is ~44% too
high**. The lowres B-cost primitive (`vk_lowres_b_cost`) or the
lookahead framing differs. x264's dump also shows **four separate
decision passes** over j=1..3 with different costs, i.e. incremental
lookahead frames; the oracle decides once over all 16 frames.
**Framing ruled out as the cause:** running x264 with `--frames`
4/8/16 and dumping slice-header types gives I,B,B,P for frames
0-3 in every case (`build/types_by_depth2.py`). Early decisions are
stable across lookahead depth, so the 3/16 mismatch is purely the
lowres B-path cost, not the multi-pass framing.

Also corrected: superfast's `b_weighted_bipred` stays at its default
1 (`base.c:451`) — the weighted-blend port was the right direction;
it reduced cb 749->732 but x264 is 521, so the residual gap is in
the lowres B motion estimation itself.

**Tested (partial):** x264's bipred average uses a weighted blend
`w = 64 - (dsf>>2)`; the oracle used a fixed 50/50 average. Porting
the weighted blend changed j=2 cb from 749 to 732 (toward x264's
521) but did not flip the decision, and slicetype stayed 13/16.
Reverted. The remaining B-cost gap is in the lowres ME itself, not
the averaging weight.
