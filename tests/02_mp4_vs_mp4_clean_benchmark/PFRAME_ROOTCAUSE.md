# P-frame quality root cause — VERIFIED (2026-11-20)

## Symptom
- IDR frames byte-exact and 44.55 dB (x264 identical)
- P-frames: vk264 25.8 dB vs x264 41.43 dB at QP28 gop2
- Encoder reconstruction == decoder reconstruction EXACTLY (self-consistent)
- All GOPs decode CLEAN after PR #61; encoder is not emitting corrupt syntax

## Root cause: no intra macroblock coding in P-slices

x264's own stats on the same 1080p QP28 gop2 source:
```
x264 --preset ultrafast --qp 28 --keyint 2 --frames 2 --stats ...
x264 [info]: mb P  I16..4: 15.6%  0.0%  0.0%  P16..4: 23.9%  skip:60.6%
```
So x264 codes **15.6% of P-slice macroblocks as I16x16 intra** (where inter
prediction fails). 60.6% are skip, 23.9% P16x16.

Vk264's `cavlc_frame.comp::writeMacroblock` inter branch (lines 141-155)
ALWAYS emits `ue(0)` mb_skip_run then `ue(0)` P_L0_16x16 — there is no
intra-in-P path. For the 15.6% of MBs that need intra, vk264 is forced to
code a poor inter prediction, producing heavily-quantized residual and
collapsing quality.

Supporting evidence:
- 65% of 4x4 blocks (5305/8160 MBs) have mean abs frame-to-frame residual
  >40/255 (huge motion/change) — exactly the case where x264 falls back to intra.
- Simple synthetic motion (8px shift) gives 36.67 dB P-frames — intra not needed.
- Disabling P-frame DCT decimation: no effect (decimation is NOT the cause).
- Larger MV search range (PR #64): 24.68 -> 25.8 dB, insufficient alone.
- IDR frames are byte-exact, so transform/quant/CABAC are correct; only the
  P-slice mode decision is missing.

## Retraction
An earlier analysis claimed the P-frame nonzero mask/CBP was zero. That was
WRONG: the dump prints only macroblock 0, and for inter records base+2 is the
MV y-component, not DC-nonzero. Corrected in issue #2 / PR #63 comments.

## Fix — concrete implementation plan
1. Extend `Luma4Fixture` (Vk264/vulkan/intra16_fixture.glsl and the C++
   `intraCorpus::Fixture` in fixtures_macroblock.inc) with a per-MB
   `intraInP` flag and the selected intra16 mode for P-slices.
2. In `inter_frame.comp::prepare()` for inter frames, compute intra16 cost
   via `selectIntra16Mode()` and compare with the inter SAD cost (from the
   motion search). Set `intraInP=1` when intra is cheaper (x264: 15.6% of MBs).
3. In `encodeResidentCavlcFrame`, stage the flag into the macroblock record;
   in `cavlc_frame.comp::writeMacroblock` inter branch, when the flag is set
   emit I16 mb_type `1+fix[mode]+4*cbp_chroma+12*cbp_luma` and the chroma
   pred-mode syntax instead of P_L0_16x16.
4. Verify P-frame PSNR rises from 25.8 dB toward x264's 41.43 dB, and that
   the byte-exact gop1/IDR gates stay green.

This is a real feature (multi-step), tracked in PR #63.

Evidence scripts: `frame_psnr.py`, `investigate_frames.ps1`, `error_map.py`.
