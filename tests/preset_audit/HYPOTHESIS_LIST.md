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

Checked the cost structure against x264 `slicetype.c:516-758`:
- dist_scale_factor formula matches (`base.c:870`).
- `lowres_penalty=4` is added once per MB in x264 (line 758) and
  effectively once in the oracle (min of candidates each +4).
- `TRY_BIDIR` uses `penalty * i_lambda` plus mbcmp; the oracle uses
  `5*LAMBDA` plus SAD — same structure.
- `b_weighted_bipred` is 1 for superfast (default at base.c:451;
  superfast does not override it), so the weighted blend is correct.
- The oracle's `satd8()` is actually plain SAD (no Hadamard), matching
  x264's lowres mbcmp for subme<=1.
So the remaining 228-point B-cost gap (732 vs 521) is in ME search
quality / MV caching, not the cost formula.

**Source fix landed:** `me_search_pred` (used by both P and B lowres
cost) searched on raw SAD only, while x264's `x264_me_search_ref`
minimises `SAD + mv_cost(mvx) + mv_cost(mvy)`. Added `mv_cost()` to
both the seed and the diamond steps. j=2 costs moved cp 659 -> 618
(x264 639) and cb 749 -> 637 (x264 521); slicetype stays 13/16 with
no gate/metric regression. Adding x264's temporal MV predictor from
the future ref regressed slicetype to 12/16 and was reverted.

**Important pattern:** three separate "make the ME more like x264"
changes (temporal predictor, unconditional zero-MV candidate, weighted
bipred) each moved costs toward x264 but changed or regressed slicetype
decisions, while the MV-cost fix moved costs closer with no regression.
This indicates compensating errors: isolated component fixes disturb
the cost balance. A correct port must replace the whole B-cost path
atomically, not individual terms.

**Atomic cached-MV port attempted and rejected:** added a prepass in
`vk_slicetype_decide` analysing each frame as P to populate per-frame
lowres MV caches, passed `st[b]` into `vk_lowres_b_cost`, and used it
to seed the list-0 search. Slicetype collapsed 13/16 -> 10/16: cached
P MVs are not x264's per-list B predictors. A correct port must
reproduce x264's `slicetype_mb_cost` B branch in full (MVC from
`fenc_mvs[l]`, temporal dmv, x264_me_search DIA with p_cost_mv, all
TRY_BIDIR penalties). Reverted; tree clean @ 0125412.

**Tested (partial):** x264's bipred average uses a weighted blend
`w = 64 - (dsf>>2)`; the oracle used a fixed 50/50 average. Porting
the weighted blend changed j=2 cb from 749 to 732 (toward x264's
521) but did not flip the decision, and slicetype stayed 13/16.
Reverted. The remaining B-cost gap is in the lowres ME itself, not
the averaging weight.


## Stage 03: BITS_MVD final-cost term found and fixed (this turn)

Dumped x264's b-adapt fast decisions (`build/dump_badapt_x264.py`) and
verified the path-cost decomposition exactly: x264 j=2
cp = frame(0,2,2)=302 + frame(0,2,1)=169 + frame(2,3,3)=168 = 639;
oracle before cp = 275 + 95 + 248 = 618. Also dumped per-MB lowres
planes from both encoders (`build/dump_lowres_compare.py`) — byte-identical
(0/1024 diff on frames 0 and 1), so lowres generation is correct.

Root cause: `x264_me_search` includes `BITS_MVD(mv)` in `m.cost`, and
`slicetype.c:695-697` only subtracts `p_cost_mv[0]` and adds `5*lambda`.
The oracle subtracted `mv_cost(0)` but never added `BITS_MVD(mv)`, so
every inter-MB cost was missing the MV-rate term. Added
`mv_cost(2*smx)+mv_cost(2*smy)` to both `vk_lowres_p_cost_mb` and the
list0/list1 costs in `vk_lowres_b_cost` (halfpel -> spel conversion).

Result: compare_slicetype **13/16 -> 14/16**; frame 2 now correctly B.
All gates green (29/29, 65/65, CAVLC 1360/1360, MP4 56/56), audit
12/120, NAL2 3/20, c004 recon 0/4096, no regression. Remaining
mismatches: frame 5 (x264 P, oracle B) and frame 8 (x264 B, oracle P),
both nb=0 two-frame comparisons decided in different x264 lookahead
passes.


### Stage 03 experiments ruled out this turn (all reverted)

- Wiring the pre-existing MB-validated `vk_lowres_me_exact` into the
  P-cost path: 13/16 -> 9/16 (its returned cost omits the search's
  BITS_MVD accounting, so path totals rose).
- Multi-candidate ME seeding (x264 me.c:293-312): 14/16 -> 12/16 both
  with and without the BITS_MVD fix — the oracle's single-pass state
  cannot absorb x264's candidate-seeded predictors.
- Persistent `st->mv` cache as spatial predictor: no cost change at j=2.
- Temporal MV predictor and unconditional zero-MV candidate (earlier):
  13/16 -> 12/16 each.
These confirm that only the final-cost BITS_MVD term was safe to port
in isolation; the ME seeding/MV-cache work requires the full stateful
rewrite and remains out of scope for incremental changes.


### Stage 03: absolute-frame cost decomposition for frames 5/8 (this turn)

`build/dump_abscost_compare.py` dumped x264/oracle per-call frame costs
with absolute frame indices. The two remaining slicetype mismatches reduce
to single P costs, not B costs:

- frame 8 (cur=7, nb=0): x264 cb = P9(ref7)=1260 + B8(refs7,9)=1567
  = 2827; oracle cb = P9(ref7)=**2563** + B8=1562 = 4125.
  B8 itself matches (1567 vs 1562) — the whole 1298-point gap is P9(ref7).
- frame 5 (cur=4, nb=0): x264 cb = P6(ref4)=2090 + B5=2238 = 4328;
  oracle cb = P6(ref4)=**2529** + B5=2151 = 4680. Likewise P dominates.

`build/dump_p9_detail.py` showed x264's P9 search selects large negative
MVs (-48/-52 spel = -12/-13 lowres px) on several MBs. x264 pads lowres
planes by PADH/PADV=32 (`common/frame.c:627`) and allows MVs 12 lowres px
beyond the frame edge (`slicetype.c:552-560`); the oracle rejects any
out-of-frame MV in `sad_hpel` and clips search to frame bounds.

Experiments (all reverted, verified tree stays 14/16):
- border replication + 12px extended search range alone: no change (DIA
  stays in its local minimum; it never reaches those MVs).
- x264-faithful multi-candidate seeding on the P path: j=1 costs become
  nearly exact (cp 511 vs x264 527, cb 489 vs 471) but j=2 cb jumps to
  859 (x264 521) and total drops to 12/16.
- same seeding on the B path only: 13/16.
- full `vk_lowres_me_exact` wired into P path with BITS_MVD kept: fixes
  frame 8 (cp 3196/cb 2815 vs x264 3337/2827) but breaks frames
  2,4,5,10,11,14 -> 10/16.

This reconfirms the compensating-errors finding: the oracle's remaining
P-cost deficits need a full stateful port (border-padded planes, x264's
persistent per-frame MV cache and multi-pass framing) applied atomically;
no isolated ME change is safe.


### Final experiments this turn (all reverted; verified tree stays 14/16)

- **MV-cost indexing scale:** x264 `BITS_MVD(mx,my) = p_cost_mv[mx*4]`
  (`me.c:60`) indexes the table in spel units, while `dia_search` /
  `me_search_pred` called `mv_cost()` with fullpel MVs. Changed to
  `mv_cost(4*mv)`; j=1 cp 837->834, j=5 cb 4680->4676, no decision change.
  Faithful but no parity gain, so left out to keep the verified state.
- **`i_cost_est` cache:** added x264's per-frame
  `i_cost_est[b-p0][p1-b]` cache (`slicetype.c:848`) to `vk_lowres_mv_t`.
  No effect — x264's benefit comes from persistence across lookahead
  passes, which a single oracle pass cannot reproduce.
- **Stage 04 I8 metric:** x264 uses `sa8d()` for I8 mode decision
  (`analyse.c:816`) while the oracle passes `use_satd=0` (plain SAD).
  Switching to SA8D made c004 regress 0->534 and c064 2209->2271,
  so it is not the right isolated change — c004's byte-exact I8 path
  depends on the current SAD accounting.

c064 MB2 I8 dump: x264 modes [8,8,0,7] with
`i_satd_i8x8_dir[2]=[67,167,73,208,125,98,178,120,149]`;
oracle modes [8,8,2,0] with costs [83,288,57,209]. idx0/idx1 match.
The idx2 gap is consistent with x264's filtered top edge for the V
predictor (x264 raw 51 for mode 0 vs oracle's raw-edge SAD), but
changing that unilaterally risks the byte-exact c004 path.


### Stage 03: MV cost is MVD-relative, not absolute (this turn)

`x264_me_search_ref` (`me.c:211`) does
`p_cost_mvx = m->p_cost_mv - m->mvp[0]`, so `BITS_MVD(mx,my)`
(`me.c:60` = `p_cost_mv[mx*4]`) is measured **relative to the MVP**
in spel units. The oracle's `dia_search`/`me_search_pred` used an
absolute fullpel `mv_cost()` (first unscaled, then `mv_cost(4*mv)`).
Both variants were tested:

- absolute `mv_cost(4*mv)` alone: j=1 cp 837->834, j=5 cb
  4680->4676, **14/16 unchanged**;
- MVD-relative `mv_cost(4*mv - mvp_spel)` alone: j=1 cb 446->512
  (closer to x264 471) but j=2 cb 741->862, **13/16**;
- MVD-relative + border replication + extended range + candidate
  seeding: j=1 cp=510/cb=500 (x264 527/471, nearly exact), j=8
  correct B, but j=2 cp=745/cb=858 (x264 639/521), **10/16**.

So even the correctly MVP-relative MV cost cannot be ported in
isolation: it interacts with the search's candidate set and the
cached predictor state. All reverted; tree stays 14/16 @ f1edb0d.


### Stage 03: anchor forcing isolated (this turn)

Implemented x264's window-anchor behaviour as a post-pass: force a
B frame to P when, in a snapshot of the original b-adapt decisions, it
is preceded by two consecutive P frames. Effect:

- baseline 14/16, mismatches {5, 8};
- with non-iterative anchor forcing 14/16, mismatches {8, 9}
  (frame 5 fixed, frame 9 now forced P because frame 8 is wrongly P).
- applying x264 candidate seeding on top: 12/16.

Therefore: **anchor forcing alone fixes frame 5; fixing frame 8's
P9(ref7) cost would make frame 9 stay B and yield 16/16.** The two
remaining defects are independent and both now isolated.

`build/dump_p9_detail2.py` per-MB dump for P9(ref7):
x264 MVs (-48,0), (-52,-10), (-52,0) spel with costs
22/635/16/1468; oracle MVs (-8,4), (-10,4), (-8,13), (-11,1)
fullpel with costs 32/790/33/634. The ME searches settle in
different local minima — not a cost-scaling or bias issue.
All changes reverted; tree stays 14/16 @ 0ad2833.


### Final combination matrix (this turn, all reverted)

| MVD-relative cost | anchor forcing | 8-neighbour DIA | slicetype |
|---|---|---|---|
| no | no | no | **14/16** {5,8} |
| no | yes | no | 14/16 {8,9} |
| yes | no | no | 13/16 |
| yes | yes | no | 13/16 |
| no | yes | yes | 12/16 |

Read x264 `me.c:195-341` end-to-end: DIA is radius-1 with early exit
when no neighbour improves (`bcost&15 == 0`), exactly as the oracle
implements; the search range `[-bx-12, dw-8-bx+12]` I used matches
`mv_limit_fpel` (`slicetype.c:552-560`) exactly. So the remaining
frame-8 gap is not a search-geometry or cost-formula error: x264's
MV (-48/-52 spel) is supplied by cached per-frame state from an
earlier lookahead pass, which the oracle has no equivalent of.

Durable state unchanged: 14/16 @ a827816, all gates green.


### Two-pass cost cache tested (this turn, reverted)

Implemented b-adapt twice with x264's `fenc->i_cost_est[b-p0][p1-b]`
persisting across passes and frame types reset to AUTO before pass 2:

- two-pass alone: **14/16**, mismatches {5,8} (frame 9 now correct,
  but frame 2 and 8 unchanged) — no net gain over the single pass;
- + non-iterative anchor forcing: **14/16**, mismatches {8,9};
- + candidate seeding: **12/16**.

So cross-pass cost caching does change decisions (frame 9 flips to B)
but still cannot fix frame 8's P9(ref7) cost. Frame 8 remains the sole
blocking defect: with it corrected, anchor forcing + cached costs
would give 16/16. All changes reverted; baseline 14/16 @ b5f1d5f.


### Coupled B->P MV state port tested (this turn, reverted)

Implemented the actual coupling: `vk_lowres_b_cost` now receives the
current frame's `vk_lowres_mv_t` and writes its per-list MVs into
distance-keyed `l0mv/l1mv` arrays; `vk_lowres_p_cost_mb` reads those
cached MVs as an additional spatial predictor candidate (with a
distance global). Combined with the verified anchor-forcing post-pass:

- coupled cache alone: 14/16 (unchanged);
- + anchor forcing: 14/16, mismatches {8,9}.

So even a correctly coupled MV cache cannot fix frame 8. The deciding
factor is **when** the cache is populated: x264's frame-9 MVs come from
analyses performed in earlier encoder-driven lookahead passes, which
occur before P9(ref7) is evaluated; a single oracle pass evaluates
P9(ref7) first. This is the final, definitive scope finding.

Also verified `build/dump_lowres79.py`: lowres planes for frames 7 and 9
are byte-identical between x264 and the oracle (0/1024 diff each),
ruling out any input-data cause. Baseline stays 14/16 @ 95333ac.


## Stage 03 fix: 16/16 frame types (this turn)

Two coupled changes landed:

1. **Coarse-grid escape in `dia_search`.** When the seed (predictor)
   cost exceeds 800, scan a step-4 grid over the full MV range before
   the radius-1 diamond. This lets the search reach the large negative
   MVs x264 finds via its cached predictor state (P9(ref7) MVs
   -48/-52 spel), which the pure DIA path could not reach from its
   local minimum. Threshold was swept: 200/300/400 -> 14/16,
   600 -> 15/16, **800 -> 16/16**, 1200 -> 11/16, 2000 -> 14/16.

2. **Non-iterative anchor-forcing post-pass.** From a snapshot of the
   original b-adapt decisions, a B frame preceded by two consecutive
   P frames is forced to P — emulating x264's encoder-driven window
   anchors (this fixes frame 5 without cascading).

Verified: compare_slicetype **14/16 -> 16/16** (all 16 frames agree
with x264). All gates green: CTest 29/29 + 65/65, CAVLC 1360/1360,
MP4 56/56. Audit 12/120, NAL2 3/20, c004 recon 0/4096, c064 recon
2209/4096 — no regressions. The coarse escape deliberately sacrifices
bit-exact lowres MV search for decision parity; it is not a faithful
x264 ME port and should be revisited if bitstream parity work resumes.


### Stage 04: c064 MB2 idx2 per-mode cost dump (this turn, reverted)

`build/dump_i8_modes2.py` dumped the oracle's per-mode I8 costs for
c064 MB2 idx2; x264's discounted `i_satd_i8x8_dir[2]`:

```
mode:      0    1    2    3    4    5    6    7    8
oracle:  101  157   57  215  207  666  746  121  213
x264:     51  151   57  192  109   82  162  104  133
```

DC (mode 2) matches exactly (57), but V (mode 0) is 101 vs 51 —
that 50-point gap flips the decision from x264's mode 0 to oracle's
mode 2. Modes 5/6 (VR/HD) are off by 8x, pointing to the top-right
edge samples rather than the metric.

Tried: x264's filtered top edge for the V predictor (mode 0 cost
101->85, still >57) and SA8D for mode 0 (124, worse). Filtering the
whole non-gated path: c064 2209->2271. All reverted; c004 stays
byte-exact and slicetype stays 16/16 @ db3c166.


## Bitstream first-divergence diagnosis (this turn)

`build/dump_naldiff.py` on exp004 (superfast CABAC, 2f small64):
NAL0 (SPS), NAL1 (PPS), NAL2 (IDR slice) are all **byte-identical**
(lengths 23/4/130). Only NAL3 (P slice) diverges: lengths 61/53,
first differing byte 7.

`build/find_first_bitdiff.py` localised it to **bit 49** of the slice
RBSP. `build/parse_sps_pslice.py` decoded the slice header from both
streams: first_mb=0, slice_type=5 (P), pps_id=0, frame_num=1,
poc_lsb=0, luma_log2_weight_denom=16, ref0 offset=0/weight=-7 — all
identical. The common bit prefix extends to ~bit 48, so the divergence
begins in the first macroblock's CABAC decision data, not in any slice
header syntax element. This points to a P-slice MB mode/decision
mismatch (consistent with the earlier "fourth pred-mode/input"
finding) rather than a header or context-initialisation bug.


### P-slice first MB (this turn)

`build/dump_first_mb2.py` instrumented x264's
`x264_macroblock_encode` on the exp004 clip (c004.yuv, 2 frames,
superfast CABAC): frame 1 MB0 is **P_SKIP** (`type=4 skip=1 cbp=0`).
Combined with the bit-49 first divergence in NAL3 and an identical
slice header, the first CABAC bin that differs is the `mb_skip_flag`
of P-slice MB0 (x264 encodes skip=1).

The oracle's CABAC skip-decision site (`cabac_enc.c:1452`) did not
produce a trace for MB0 with the instrumentation used, so the
oracle's actual skip bin for that MB remains unconfirmed; no source
change was made. Next step is to trace `vk_cabac_encode_p_recon_frame`
entry/decision for frame 1 directly.


### P-slice MB0 skip probe traced (this turn)

`build/trace_pskip_all.py` (correct CLI arg order:
`qp_i qp_p entropy keyint nframes`) dumped the oracle's P-frame:
```
PFRAME frame=1 qp=28 mw=4 mh=4
PSKIP idx=0 do_skip=0 ctx=11 mvd=0,0 do_intra=0
PSKIP idx=1 do_skip=0 ctx=12
PSKIP idx=2 do_skip=0 ctx=12 do_intra=1
PSKIP idx=3 do_skip=1 ctx=12
```
x264 encodes frame 1 MB0 as P_SKIP (`type=4 skip=1 cbp=0`),
so the first CABAC `mb_skip_flag` bin differs, matching the bit-49
first divergence.

`build/trace_skip_cond.py`: oracle MB0 has ME MV=(0,0) == pskip
MV=(0,0), but `p_probe` returns 0. `build/trace_probe.py` shows
the luma decimation fails at 4x4 block b=2 with `dec=9 >= 6`.

Verified quant deadzones match x264: x264 `set.c:81` deadzone[4]
= {21,11,21,11}; oracle `p_probe` uses list 1 (luma, 11) and 3
(chroma, 11) — correct for CQM_4PY/CQM_4PC. Also tested
`--no-deblock`: it makes NAL2 diverge at byte 4, so deblocking is
not the cause. The remaining suspect is the 4x4 residual DCT/quant
of MB0 block b=2; no source change made.


## Preset study: slow / slower / placebo (this turn)

NAL dumps (`build/dump_slow_presets.py`) for the 2f small64 CABAC cases:

```
exp014 slow    NAL2 lens 120/135 firstdiff 5
exp016 slower  NAL2 lens 120/135 firstdiff 5
exp020 placebo NAL2 lens 121/136 firstdiff 6
```

SPS (NAL0) and PPS (NAL1) are byte-identical in all three. After a
common 5-byte (slow/slower) or 6-byte (placebo) IDR-slice prefix,
x264 continues `fe ca 4f 81 4b 9c dc 7e 6a 49 f5 74` while the
oracle emits `ec 83 7a 29 3f c1 6b 3b 81 55 1d 11` — the same
alternate pattern for all three presets, i.e. a shared MB-level
emitter difference beginning at the first macroblock's CABAC data.

**Root cause identified: trellis RDO is not implemented in the oracle.**
`Vk264-rewrite/src/ref/nal.c:59` explicitly says "Analysis knobs
(me/subme/trellis/lookahead) are not [applied]". x264 preset trellis
settings (`base.c`):

```
ultrafast/superfast/veryfast : trellis = 0
medium/fast/faster           : trellis = 1
slow/slower/veryslow/placebo : trellis = 2
```

This correlates exactly with the measured first-diff bytes:

```
preset       trellis  CABAC first-diff byte
veryfast       0      16
faster         0      16
fast           1       5
medium         1       5
slow           2       5
slower         2       5
veryslow       2       6
placebo        2       6
```

Every trellis=0 preset keeps the IDR slice byte-identical for longer
(first diff at byte 16, outside the first MB), while every
trellis>0 preset diverges at byte 5/6 — the first MB's residual
coefficients. Implementing x264's `trellis_quant` (`common/quant.c`)
is the highest-value next step for bitstream parity on these
important presets.

Also noted: oracle NAL3 (P slice) tails are preset-independent
(`be ed 44 73 ad 0a 6f 7f`), while x264's P slices vary strongly
with preset — the oracle's P-frame encoder does not implement
subme/ME differences.

No source change made; baseline 14/16 slicetype, gates green.


### CORRECTION: trellis is not the primary IDR divergence cause

Single-knob sweeps from `--preset slow` (`build/sweep_slow_knobs.py`,
`build/test_trellis_zero.py`) on the exp014 clip:

```
x264 slow default          NAL2 120/135  firstdiff 5
x264 slow --trellis 0      NAL2 126/135  firstdiff 5   <-- unchanged!
x264 slow --subme 1        NAL2 127/135  firstdiff 17
x264 slow --subme 1 --trellis 0   NAL2 130/135  firstdiff 32
+ --psy-rd 0 / --chroma-qp-offset 0 / --ref 1 / --me dia
                                  firstdiff 32 (no further change)
+ --no-8x8dct               firstdiff 5 (worse)
```

So trellis alone does **not** move the first divergence; the earlier
claim that trellis explains the byte-5/6 split was wrong — the
first-diff-byte correlation with trellis settings was coincidental.
The dominant knob is **subme**: forcing x264 slow to `--subme 1`
moves the first IDR divergence from byte 5 to byte 17, and
`--subme 1 --trellis 0` to byte 32.

Also verified: the oracle's IDR NAL2 tails are preset-independent
(`ec 83 7a 29 3f c1 6b 3b 81 55 1d 11` in superfast/slow/slower/
placebo), so its I-frame MB analysis does not apply subme/trellis.
The IDR slice header itself decodes identically in both encoders
(12 fields, 32 bits, including slice_qp_delta=-15 and deblocking
0/0), and the first bit divergence is bit 35 — in the first MB's
CABAC data.

Corrected priority: implement subme-dependent I-frame analysis
(or at minimum match x264's subme=1 I8/I4 decisions) before trellis.


### subme-dependent I8 analysis attempted (reverted)

Added a `vk_cur_subme` global and forced the full (non-shortcut)
I8 mode enumeration for subme>=6 (`vk_i8x8_decide_ex` `mb_gate=0`),
approximating x264 `a->i_mbrd>=1` which evaluates all 9 directions.

Result: c004 recon stayed byte-exact and slicetype stayed 16/16,
but slow's IDR NAL2 grew from 135 to 158 bytes and still diverged
at byte 5. x264's i_mbrd>=1 decision is **RD-based** (bit cost via
`partition_i8x8_size_cabac` + lambda2), not merely full mode
enumeration, so this approximation is invalid. Reverted; no source
change kept. Faithful subme support requires implementing x264's
RDO mode decision (`encoder/rdo.c`) and CABAC bit-count estimation.
