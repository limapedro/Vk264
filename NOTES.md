# CAVLC parity investigation notes

## Current state (verified)
- Branch: `rewrite/x264-c-vulkan` @ `cfb2844`, clean tree
- CAVLC I-frames 72/72, CAVLC P 1350/1360 (10 mismatches), CABAC P 1360/1360
- Gates 65/65, 18/18 green

## The 10 remaining CAVLC cases
Complement of `build/ex_dec15.txt`:
`0,420,16`, `0,900,16`, `120,360,12`, `120,960,12/16/4/8`, `1800,960,16`, `480,960,16`, `960,0,16`

## Detailed evidence for (0,420,16)
- P-slice NAL: ref 119 bytes, oracle 118; first divergent byte 68
- No `00 00` byte pairs in either NAL -> emulation prevention not involved
- I-frame NAL byte-exact
- All 135 residual VLC token sequences (total/trailing/total_zeros/mask/levels) identical between x264 and oracle
- Level-VLC table generation identical (x264 `common/vlc.c` vs oracle `cavlc.c`)
- CBP golomb tables identical (x264 `cavlc.c:36-46` vs oracle `pframe.c:286-290`)
- MB(2,0) header values match: mvp(0,0), mv(16,8), mvd(16,8), cbp luma=3 chroma=2
- MB types match for all MBs
- Bit-position traces are hard to compare because the oracle logs writes that are later
  discarded by the encode-time P_SKIP rollback (`*bs = mb_start`)

## Hypotheses (ranked)
1. **Header-bit encoding difference** with matching symbol values — e.g. `mb_qp_delta`
   written in a slightly different condition, or chroma-mode fix bit.
2. **Rollback semantics** — the oracle's CAVLC P_SKIP promotion restores `*bs` but may
   mis-handle `skip_run` accumulation vs x264's pre-write skip decision.
3. **I16 chroma prediction mode** — CAVLC's `intra_decide()` vs CABAC's inline selection
   may pick a different `icmode` for an I16-in-P MB, changing chroma residual and bits.
4. **`mb_type_b_to_golomb` / I16 mb_type** — x264 uses a distinct golomb table for
   intra MBs in P slices; oracle uses a direct formula. Verify equivalence.
5. **Partial-byte accumulator rollback** — `vk_bs_t` has `acc`/`nbits` in addition to
   `p`; restoring `*bs = mb_start` should restore all, but verify.

## Investigations already ruled out
- Chroma AC decimation DC-slot (fixed in PR #179, gained 6 cases)
- Inter luma decimation DC-slot (changing it regressed to 735/1360)
- Level table generation, CBP golomb table, emulation prevention

## BREAKTHROUGH — fixed 5 cases (2026-11-22)

### Root cause
`code_residual()` in `pframe.c` captured `oc[pl][b]` (chroma AC nonzero counts for
the left/top neighbours of subsequent MBs) at line 386, **before** chroma AC
decimation (lines 407-424) zeroed blocks with score < 7. x264's nnz cache is
updated with the post-decimation total (`*nnz = i_total` after writing each block).
So the oracle computed wrong `nC` for chroma AC coeff_token VLCs.

### Evidence (case 0,420,16)
Built a fully accurate per-write bit trace (filtered x264's `bs_write1` and RDO
scratch writes) and a per-block residual dump. First divergent bit: MB2 offset 282.
Block dump showed MB2 chroma AC block 0 data identical (total=4, trail=3,
mask=0x56, levels -1,-1,-1,-2...) but `nC=0` in x264 vs `nC=1` in oracle.

### Fix
After decimation, set `oc[pl][b] = cnnz[pl][b]`.

### Result
CAVLC P-slice parity 1350 -> **1355/1360**. Gained: 0,420,16 / 120,960,12 /
120,960,16 / 120,960,4 / 960,0,16. Lost: none.

### Remaining 5 cases (now fixed)
`0,900,16`, `120,360,12`, `120,960,8`, `1800,960,16`, `480,960,16`

## BREAKTHROUGH #2 — full parity (2026-11-22)

### Root cause
`code_i16_dc()` wrote I16 luma AC residual blocks with `VK_DCT_LUMA_4x4`
(count_cat[2]=16) but x264 uses `DCT_LUMA_AC` (count_cat[1]=15). For a
15-coefficient AC block, x264 skips total_zeros (`15 < 15` false) while the
oracle wrote an invalid `sz=1 bits=0` VLC — a 1-bit stream shift.

### Evidence (case 0,900,16)
Accurate trace: first divergent bit at MB7 offset 509; oracle wrote an extra
`W 1 0` before the next VLC. Tagged VLC logging showed:
- x264: no total_zeros after the total=15 block
- oracle: `TZ sz=1 bits=0` after the total=15 block

### Fix
Pass `VK_DCT_LUMA_AC` instead of `VK_DCT_LUMA_4x4` in `code_i16_dc()`.

### Result — FULL ORACLE PARITY
CAVLC P-slice parity 1355 -> **1360/1360**. Gained all 5 remaining cases,
lost 0. CABAC P 1360/1360, CAVLC I 72/72.
CTest 65/65 + 18/18. GPU shaders can now be un-deferred.
