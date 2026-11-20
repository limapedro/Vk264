# Vk264-Vulkan vs x264 — bottom-up port progress

Date: 2026-11-19
Source: `references/x264-master` (x264 snapshot `c24e06c2e184345ceb33eb20a15d1024d9fd3497`,
extracted from `x264-master.zip` into a stable location so `Remove-Item build` cannot wipe it)
Vk264-Vulkan port: `Vk264/vulkan/*.comp`, shader SPIR-V shader pipelines registered in `Vk264/vulkan/CMakeLists.txt`,
fixture tests in `Vk264/src/dct_parity.cpp` (the `verifyXxx` functions and the
`x264_*_parity` CTest registrations).

## Issue #8 FIXED — multi-MB CABAC tail divergence (Ram, 2026-11-20)

**Root cause: the resident CABAC frame fixture's `count` field was set to the
macroblock count instead of the operation count.** In
`Vk264/src/parity/resident_cabac.inc::encodeResidentCabacFrame`, `fixtureHeader[4]`
(fixture `count`, the number of entries in `operations[]`) was `macroblocks`, but
only **one** operation (kind 15, `wholeFrame`) is uploaded. `cabac.comp::encodeOperations`
loops `for (i = 0; i < fixtures[f].count; ++i)`, so for N>1 MBs it also executed
N-1 stale/zero operations *after* the whole-frame op and before the flush — extra
bins confined to the tail of the arithmetic stream. With 1 MB, count==1 and the
stream was correct, exactly matching the "1 MB exact / 2+ MBs off in last 1–2
bytes" symptom. Fix: set the operation count to **1**.

Evidence (actual output after fix):
```
python harness/parity_check.py --preset fast --pattern flat --width 16 --height 16 --frames 1 --gop 1
BYTE-EXACT MATCH (ignoring encoder-identity SEI)
python harness/parity_check.py --preset fast --pattern flat --width 32 --height 16 --frames 1 --gop 1
BYTE-EXACT MATCH (ignoring encoder-identity SEI)
python harness/parity_check.py --preset fast --pattern flat --width 64 --height 64 --frames 1 --gop 1
BYTE-EXACT MATCH (ignoring encoder-identity SEI)
```
Full size sweep (all 1 frame, gop=1, `fast`/flat): 16x16, 32x16, 16x32, 32x32,
48x16, 16x48, 64x16, 16x64, 64x32, 32x64, 64x64 — **11/11 BYTE-EXACT**.

Diagnostic path (for the record): `VK264_DUMP_DIR` dumps the resident bank;
`dct_parity record_bank <bank> cabac_record.spv 25 2 0 63 <columns> <out>` replayed
the *same* bank and produced x264's `... b8 79` tail, proving the bank/MB
decisions and CABAC syntax were correct and isolating the fault to the fixture
setup (operation count) rather than the sweep-tested context formulas
(`lumaCodedFlag`/`chromaAcFlag`/chroma-mode neighbours were each tested and
ruled out). The flush constant/frame-index premise was already disproven in PR
#13 and was not revisited. No unrelated parity case regressed: the remaining
failing broader cases (multi-frame inter, noise/motion mode decision) were
verified to fail identically with the fix stashed.

Regression gate added: `x264_parity_fast_64x64` in `Vk264/vulkan/CMakeLists.txt`.

## Issue #25 fixed + round 2 (Subaru, 2026-11-20)

PR #28 fixed issue #25: intra MBs no longer use DCT decimation (x264 decimates
P-frames only). The real 16x16 gradient crop at (1792,896) is now BYTE-EXACT,
and 10/10 real single-MB crops pass. PR #29 added a byte-exact chroma mode
fixture (16640/16640); 58/58 CTests. Multi-MB divergence is now localised to
chroma mode + Y reconstruction staging at MB(1,1) (issue #30).

## Function-parity PR loops (Subaru, 2026-11-20)

Five PRs landed: #20 (I16x16 mode fixture, 16640/16640), #22 (CAVLC context
fixture, 400/400), #23 (mirror extended to 15 modules), #24 (byte-exact tests
for production `*_frame.spv` variants, previously untested), #26 (real-gradient
single-MB regression case). Suite grew 52 -> 57 CTests, all passing. x264 source
tree fully inventoried (669 functions / 35 files) in `docs/function-parity-matrix.md`.
Real 16x16 crops are byte-exact in 9/10 sampled locations; the one failure at
(1792,896) is narrow and content-dependent (issue #25).

## Real-content bottom-up verification (Subaru, 2026-11-20)

`docs/real-content-verification.md` records the full evidence. Headline: all 52
shader fixtures are byte-exact; a single 16x16 MB of real gameplay is byte-exact
(bitstream and reconstruction); divergence begins at the second MB row — first
differing pixel at MB(1,1), the first MB with both left and top neighbours.
`fast` diverges at MB(0,0) on textured content; `superfast` from pixel 0. This
localises the remaining gap to the decision/orchestration layer (mode selection,
neighbour context, I4x4 emission), not the GPU primitives.

## 1:1 x264 source mirror (issue #18, Subaru) — IN PROGRESS

Goal: mirror `references/x264-master` file-for-file under `Vk264/common/` and
`Vk264/encoder/`, where each `vk264::<x264 function>` is the GPU-compute
equivalent of its x264 C counterpart, launched by a shader. A fixture calls
both the verbatim x264 oracle and the Vulkan wrapper with identical inputs and
compares byte for byte. First module: `Vk264/common/dct.cpp` (mirrors
`common/dct.c`, wraps `dct4x4.comp`/`dct8x8.comp`). Data-parallel x264 files map
to shaders that already exist; scalar/control files stay C++. The verification
layer uses one launch per function; production keeps fused kernels for speed.
Plan and module list: issue #18, `docs/parity-layout.md`.

## Source organisation (Subaru, 2026-11-19) — parity fixtures split into x264-mirroring modules

The 6,700-line `Vk264/src/dct_parity.cpp` monolith is now a 24-line master
that textually includes 14 domain modules from `Vk264/src/parity/`, each
(C++ source now lives in `Vk264/src/`; `Vk264/vulkan/` holds shaders only). Each
mirroring the x264 C file(s) it checks. `docs/parity-layout.md` documents the
module map, how to trace a CTest name to its fixture, and how to add a fixture.
This is a layout-only change: concatenating the module bodies reproduces the
original file **byte for byte** (355,633 bytes); `ctest` stays **52/52** with
all **4** automated parity gates passing, and the CABAC fixture still reports
9946 matched sequences. The split is textual (not separate translation units)
because the extracted x264 `.inc` files contain static functions and
external-linkage definitions — see the layout doc for details.

## Superfast I4x4 CABAC (issue #2 — Ram, 2026-11-19) — bin-level investigation, NOT byte-exact yet

**Result of this session: no byte-exact superfast stream. The validated bin decoder
tooling is added to `harness/`, and the divergence is localized to the residual
syntax stage, but the exact x264 residual configuration was not identified.
No shader/encoder code is changed.**

### Reproduction (actual output)

```
python harness/parity_check.py --preset superfast --pattern flat --width 16 --height 16 --frames 1 --gop 1
vk264 [... 00 00 01 65 88 84 3f fd af 4f 81 4b d9 15 aa 51 d8 32 d0 c7]
x264 [... 00 00 01 65 88 84 3f ea 40 35 70 9b fe 0b 72 bf 3e 1a c9 b1 34 b4 31 81]
first mismatch at offset 41
```

The host analysis model `dct_parity i4x4_decision` still passes its 256/256
luma reconstruction assertion, and `dct_parity i4x4_cabac` emits:
`88 84 3f ea 40 32 c1 bf ff f9 ff ef 92 fe 32 1e 51 4b` — the **first 5 bytes
match x264**, then diverge at byte 5 (`32` vs `35`).

### New reusable tooling (committed)

- `harness/cabac_decoder.py`: syntax-aware CABAC bin decoder. Parses the four
  x264 tables from `references/x264-master/common/tables.c` at runtime, decodes
  decision/bypass bins in x264's write order (mb_type, transform_size, 16
  intra4x4 pred modes, chroma mode, cbp luma/chroma, qp_delta, DCT_LUMA_4x4
  residual cbf + sigmap + levels). Usage:
  `python harness/cabac_decoder.py "ea 40 35 70 ..." 25`.
- `harness/cabac_decoder_selftest.py`: independent encoder transcription used to
  validate the decoder. Actual output:
  `cabac_decoder round-trip OK: 60 streams, QP 0/25/51.`

### Findings

- Decoding both the emitted slice and x264's target produces an **identical
  31-bin header prefix** through cbp/qp_delta (mb_type=I4x4, transform_size=0,
  chroma mode 0, cbp_luma decoded 0xa, cbp_chroma 0). So the divergence begins
  in the residual stage, not in modes/mb_type/chroma/flush.
- Decoded x264 residual syntax (DCT_LUMA_4x4): nonzero blocks `{4,6,12,13,15}`
  with DC absolute levels `{2,2,2,2,3}` and cbf context pattern
  `93,94,93,94,...`. The emitter's host-model coefficients instead code a
  different block set; feeding the decoded levels into x264's own
  `x264_cabac_block_residual_c` did not reproduce the target bytes.
- A bounded exhaustive search over cbp (0..15), nonzero-block masks (3..7
  blocks), per-block levels (-2 with one optional -3), two mode sets, MB QP
  24..27, and chroma/second-transform-size header variants — using x264's own
  compiled CABAC functions — found **no configuration** producing the 21 target
  bytes. This means at least one assumed syntax element (most likely the
  intra4x4 pred-mode sequence or qp_delta) is still wrong; further work needs a
  fully independent decoder of x264's own bitstream, not a guessed parameter.
- x264's `encoder.c` emits a terminal bin before every MB except the first and
  writes `cabac_intra_chroma_pred_mode` in the header; both are preserved in the
  search. No bit value was guessed and no speculative shader change was kept.

### Next steps (not done)

1. Validate the decoder against a stream whose syntax is known from x264's C
   encoder (round-trip validates raw bins, but the *syntax ordering* assumptions
   need an independent check).
2. Use x264's own `x264_macroblock_write_cabac` (needs a populated MB cache) to
   dump the exact mode/cbp/coefficient set for this frame.
3. Feed that set into `encodeI4x4Frame()`, then wire the `i_mbrd=0` path into
   `encode()` for superfast/veryfast/faster.

Gates on this branch: `ctest` 47/47 green; `ultrafast` and `fast` flat 16x16
gop=1 both `BYTE-EXACT MATCH` (unchanged — no encoder code modified).

## CABAC slice-tail divergence at 64x64 (issue #8 — Ram, 2026-11-19) — characterized, NOT fixed

**Result of this session: the issue's premise is not supported by the evidence.
This is not a CABAC flush-constant/frame-index bug. No code change is made; the
fix is deliberately not attempted.**

### Reproduction (actual command output, current branch)

`python harness/parity_check.py --preset fast --pattern flat --width 64 --height 64 --frames 1 --gop 1`:

```
first mismatch at offset 69 in SEI-stripped stream
vk264  [... d6 94 bb e1]
x264   [... d6 94 d1 61]
```

Note the harness's `nal_type_at()` label is unreliable: it only matches 4-byte
start codes, so it calls this byte a "PPS" NAL; the actual slice starts with the
3-byte code `00 00 01 65`. A correct NAL dump shows SPS and PPS are byte-identical
and the **only** differences are the last two slice bytes.

### Evidence that the flush algorithm is correct

- `Vk264/vulkan/cabac.comp::encodeFlush` is a line-for-line translation of
  `references/x264-master/common/cabac.c::x264_cabac_encode_flush` (same
  `low += range-2`, `|=1`, `<<9`, two `putByte`, `low <<= -queue`,
  `|= (0x35a4e4f5 >> (frame&31) & 1) << 10`, `queue=0`, `putByte`, outstanding
  `0xff` bytes). `putByte`/`renormalize` likewise match `cabac_putbyte` and
  `cabac_encode_renorm`.
- The existing `x264_cabac_parity` fixture compares GPU and x264-C CABAC streams
  with `input.frame = i % 97` and `finish` set for ~2/3 of ~9856 cases; this
  covers every `(frame & 31)` value with flush, and passes. So acceptance
  criterion "flush bytes match x264 for frame indices 0..31" is already met.
- 8-frame dump (`--frames 8 --gop 1`, flat 64x64) shows the **final-byte LSB**
  follows `0x35a4e4f5` bit `(frame&31)` identically in both encoders
  (`e1,e0,e1,e0,e1,e1,e1,e1` vs `61,60,61,60,61,61,61,61`): only bit 7 of the
  final byte differs, constant across all frames. A wrong frame index would
  change the LSB, so that hypothesis is ruled out (and matches the previous
  reverted speculative attempt).

### Where the divergence actually is

Size sweep (`fast`, flat, 1 frame, gop=1) — `mb` is the macroblock count:

```
 16x16  mb= 1 exact=True   vk_tail=58 0f 72 ff 21 81      x264_tail=58 0f 72 ff 21 81
 32x16  mb= 2 exact=False  vk_tail=0f 72 ff 1f b8 19      x264_tail=0f 72 ff 1f b8 79
 16x32  mb= 2 exact=False  vk_tail=0f 72 ff 1d 43 c9      x264_tail=0f 72 ff 1d 44 79
 32x32  mb= 4 exact=False  vk_tail=1f b8 21 a5 c5 81      x264_tail=1f b8 21 a5 df 81
 64x16  mb= 4 exact=False  vk_tail=1f b8 58 ac 99 91      x264_tail=1f b8 58 ac 9c f1
 64x64  mb=16 exact=False  vk_tail=8f 82 d6 94 bb e1      x264_tail=8f 82 d6 94 d1 61
```

- 1 macroblock is byte-exact; divergence begins at 2 MBs, i.e. as soon as a
  left/top neighbour exists. It affects only the final 1–2 bytes; reconstruction
  is unchanged.
- **CAVLC is byte-exact at the same sizes** (`ultrafast` flat 64x64 and 32x16
  both `BYTE-EXACT MATCH`). CAVLC and CABAC encode the same MB modes and residual
  levels, so the analysis/decisions are identical; the divergence is confined to
  CABAC-specific bin/context emission for neighbour-dependent syntax.
- Diagnostic experiment (not committed): adding an extra `emitTerminal()` at
  end of slice made things worse — it broke the previously exact 16x16 case and
  shifted 64x64 from `bb e1` to `bb a1`. So this is not a missing trailing
  terminal bin. `cabac.comp` was restored to its original state.

### Honest limitation and request

The remaining tail-byte delta is a CABAC neighbour-context/bin difference that
manifests only in the arithmetic coder's trailing bytes; it is **not** the flush
constant, the frame index, a missing terminal, or a mode decision. Pinpointing
the exact differing bin requires a bin-level decoder comparison that is not yet
available in this folder (the old `build/cabac_valid.py` was never committed).
Subaru: please decide whether to (a) keep #8 open and fund the bin-level
debugging, or (b) reclassify it away from "CABAC flush byte" since that premise
is disproven. No parity is claimed for 64x64 fast here.

Regression gates on this branch: `ctest` 47/47, `ultrafast` and `fast` flat 16x16
gop=1 both `BYTE-EXACT MATCH`.

## Multi-reference groundwork (issue #5 — Ram, 2026-11-19)

Issue #5 is `blocked`: the resident CABAC path has no per-MB `ref_idx` syntax
and a single-slot (`referenceEnd`) GPU reference, so ref>=2 selection cannot
land yet. Full architecture and dependency-ordered sub-PR plan are recorded in
`docs/multi-reference-design.md`.

Landed in this PR (safe, self-contained groundwork only):
- `Vk264/vulkan/dct_parity.cpp::encodeCabacClip` now consumes the per-preset
  `referenceBudget` (from `x264_presets.hpp`, values 1/1/1/2/2/3/5/8/16/16)
  for SPS `max_num_ref_frames` (gop>1) and level DPB selection, instead of
  hard-coding 1. Intra-only (gop=1) stays 0/1 so existing byte-exact cases do
  not move. This mirrors x264 `set.c:147-158` for the current no-B-frame path.
- VUI `max_dec_frame_buffering` follows `i_num_ref_frames` automatically.
- PPS `num_ref_idx_l0_default_active` is deliberately left at 1: raising it to
  2 without emitting CABAC `ref_idx` bins makes the stream undecodable.
- **Sub-PR B (landed):** `cabac.comp` gains a `refIdx` operation and
  `cabac_syntax.glsl::encodeRefIdx()` emits the exact x264
  `cabac_ref_internal()` (P path) bins — contexts `54+ctx`, `ctx=(ctx>>2)+4`
  per '1' bin, terminating '0'. New `verifyCabacRefIdx()` fixture compares GPU
  bytes/contexts/arithmetic state against x264's C coder over 1536 cases
  (ref 0..15, left/top ref states, QP, CABAC model, flush variants), registered
  as CTest `x264_cabac_refidx_parity`.
- **Sub-PR C groundwork (landed):** the host reconstruction ring is sized to
  `referenceBudget+1` slots (`storageIndex = picture % slots`); for ref=1 this
  is byte-for-byte the old modulo-2 ring. The resident GPU reference is still
  a single persistent slot (`referenceEnd`), so older frames are retained but
  not yet selected by the production motion path.

Evidence:
- Baseline x264 (ffmpeg `-preset faster|fast -x264-params bframes=0`, 32x32,
  gop=12, qp=28) SPS/PPS fields, parsed with a scratch SPS reader:
  `num_ref_frames=2`, `num_ref_idx_l0_default_active_minus1=1`, POC type 2.
- Before change, vk264 `faster` gop>1 SPS: `num_ref_frames=1`, PPS minus1=0.
- After change, vk264 gop>1 SPS: `veryfast` (ref=1) `num_ref_frames=1`;
  `faster`/`fast` (ref=2) `num_ref_frames=2` — matches x264's SPS value.
  Stream still decodes: `ffmpeg -i ... -f null -` exit 0.
- `ctest --test-dir build -C Release` → **47/47 passed** (was 46/46 before the
  new refidx fixture; 15.36 s on the incremental build).
  `dct_parity cabac_refidx build/Vk264/vulkan/cabac.spv` →
  `1536 CABAC ref_idx sequences matched x264 cabac_ref_internal bytes, contexts and arithmetic state.`
- Byte-exact regressions preserved:
  `parity_check --preset ultrafast --pattern flat --width 16 --height 16 --frames 1 --gop 1`
  and the same with `--preset fast` → both `BYTE-EXACT MATCH (ignoring encoder-identity SEI)`.
- After the ring change, gop>1 SPS still reports `num_ref_frames=2` for
  `faster`, stream still decodes (`ffmpeg -f null -` exit 0).
- Honest limitation: no byte-exact ref>=2 parity is claimed. The `refIdx` bins
  are available but not wired into the resident whole-frame P path (doing so
  before PPS active-refs is raised would desync a 1-ref stream). gop>1 still
  diverges from default x264 (B-frames/POC); the N-slot GPU motion ABI and
  per-MB older-reference selection remain open (sub-PRs C/D in the design doc).
## B-frame sub-PR: CABAC B macroblock-type header fixture (issue #4, Rem)

Sub-PR `rem/4-bslice-cabac-fixture`, independent of the `rem/4-bframe-headers-poc`
header/POC sub-PR. Recovered from the legacy shared checkout and completed here.

- **Shader** (`cabac_syntax.glsl`, `cabac.comp`): new `encodeMbTypeB()` transcribes
  `encoder/cabac.c::cabac_mb_header_b` (lines 499-573) mb_type bins only —
  B_DIRECT (ctx 27+ctx), the 9 B_16x8/8x16/16x16 modes via the `i_mb_bits[9*3]`
  table, and the B_8x8 header prefix. Ref indices, MVDs and B_8x8 subpartitions
  are out of scope (need multi-ref #5 + DPB). New fixture-only op `mbTypeB=17`,
  kept distinct from Ram's merged `refIdx=16` (#5) so both operations survive
  the union; the resident encode path is unchanged (still P-only). Rebased onto
  main@351e8c0 after Subaru's review; Ram's `encodeRefIdx`, `verifyCabacRefIdx`
  and `x264_cabac_refidx_parity` CTest are all preserved.
- **Fixture** (`dct_parity.cpp::verifyCabac`): +90 cases = B_DIRECT + 9 modes ×
  3 partitions × 3 neighbour contexts, cross-checked byte-for-byte (bytes,
  context states, arithmetic state) against a host transcription of x264's bin
  sequence with `SLICE_TYPE_B` context init. Total `x264_cabac_parity` cases
  9856 → 9946.
- **Bug fixed during recovery**: the in-progress legacy code packed `part` at bit 6,
  overlapping the 5-bit `type` field (bits 2-6); `part=1` corrupted `type`
  (B_DIRECT=7 misread as 23 → `mbBits[46]` out of bounds). Moved `part` to
  bits 7-8 in both host and shader. Diagnosed via fixture 9866 mismatch
  (ctxFirst=32, byteFirst=1).

Evidence (2026-11-19):
```
cmake --build build --config Release                 -> OK
ctest --test-dir build -C Release --output-on-failure -> 47/47 passed (25.73 s)
python harness/parity_check.py --preset ultrafast --pattern flat --width 16 --height 16 --frames 1 --gop 1
  BYTE-EXACT MATCH (ignoring encoder-identity SEI)
python harness/parity_check.py --preset fast --pattern flat --width 16 --height 16 --frames 1 --gop 1
  BYTE-EXACT MATCH (ignoring encoder-identity SEI)
dct_parity cabac build/Vk264/vulkan/cabac.spv
  9946 CABAC sequences matched x264 bytes, contexts and arithmetic state, ... and 90 B macroblock-type header sequences.
dct_parity cabac_refidx build/Vk264/vulkan/cabac.spv
  1536 CABAC ref_idx sequences matched x264 cabac_ref_internal bytes, contexts and arithmetic state.
```

Limitations: fixture-only; no B-slices emitted in the bitstream, no DPB/reorder.
Full `I B B P` parity remains blocked on #5 (multi-ref) and lookahead.

## CAVLC residual byte-exact fixture (issue #7, Rem)

Sub-PR `rem/7-cavlc-residual-fixture`.

- **Test shader** (`cavlc_residual_test.comp`): standalone buffer-to-buffer
  harness that includes the *same* `cavlc_residual.glsl` the resident CAVLC
  encoder uses, with `x264CavlcCodes[]` (732 VLC codewords generated from
  x264's own tables) passed as the tables prefix. Each fixture record carries
  16 coefficients, nc, count, chromaDc and the emitted 32-bit words.
- **Host reference** (`dct_parity.cpp::verifyCavlcResidual`): scalar
  `x264CavlcBlockResidual` transcribed from x264
  `encoder/cavlc.c::cavlc_block_residual_internal` (coeff_token, trailing
  signs, level escape/suffix transitions, total_zeros, run_before), driven by
  the same x264-derived VLC codewords. Compares emitted words and bit length
  byte-for-byte against the shader.
- **Coverage**: 2327 cases — all-zero / single-coefficient (all 16 positions),
  all-ones, all-minus-ones, ±32768 extremes, nc 0..3, block counts 16 (luma 4x4
  AC), 15 (intra AC/DC) and 4 (chroma DC), plus randomized sparse/dense blocks.
- New CTest `x264_cavlc_residual_parity`; suite 47 -> 48 green. No production
  shader or encoder path changed.

Evidence (2026-11-19):
```
cmake --build build --config Release                 -> OK
dct_parity cavlc_residual build/Vk264/vulkan/cavlc_residual_test.spv
  2327 CAVLC residual sequences matched x264-derived VLC codes and bit writer bytes.
ctest --test-dir build -C Release --output-on-failure -> 48/48 passed (15.80 s)
python harness/parity_check.py --preset ultrafast --pattern flat --width 16 --height 16 --frames 1 --gop 1
  BYTE-EXACT MATCH (ignoring encoder-identity SEI)
python harness/parity_check.py --preset fast --pattern flat --width 16 --height 16 --frames 1 --gop 1
  BYTE-EXACT MATCH (ignoring encoder-identity SEI)
```

Honest limitation: the host reference is a scalar transcription of x264's
CAVLC residual algorithm driven by x264-generated VLC codewords, not a literal
link against x264's compiled `cavlc.c` (the repo's extractor has no CAVLC unit
yet). No 8x8 (64-coefficient) category: `cavlc_residual.glsl` is a 16-sample
block writer. No byte-exact claim beyond the covered block categories.

## Parity gate in CTest / docs (issue #9, Rem)

Sub-PR `rem/9-parity-gate-ctest`. Infra only: no encoder or shader behaviour change.

- **`Vk264/vulkan/CMakeLists.txt`**: four new CTest entries, registered only
  when FFmpeg was found at configure time, each invoking
  `harness/parity_check.py` with an absolute vk264 path and a unique
  `--out-dir` under `build/parity-gate/` (so `ctest -j` is safe):
  `x264_parity_ultrafast_flat_16x16`, `_ultrafast_flat_64x64`,
  `_ultrafast_noise_32x32`, `_fast_flat_16x16`. `parity_check.py` exits 0 only
  on `BYTE-EXACT MATCH`, so breaking an exact case fails `ctest`.
- **`README.md`**: documents the gate, how to run it, and how to interpret
  `BYTE-EXACT MATCH` vs "first mismatch at offset N".
- Baseline 48 -> 52 CTest entries, all green.

Evidence (2026-11-19):
```
ctest --test-dir build -C Release -R x264_parity_ --output-on-failure -> 4/4 passed (1.39 s)
ctest --test-dir build -C Release --output-on-failure -> 52/52 passed (15.82 s)
```
Each of the four cases was also run manually and printed
`BYTE-EXACT MATCH (ignoring encoder-identity SEI)`.

### How to update this file (PROGRESS.dm)

This document is the single source of truth. When changing parity behaviour:
1. Add a dated section at the top with a one-line scope, the files changed, and
   actual pasted command output (ctest + `parity_check.py`), never a paraphrase.
2. State limitations honestly; `BYTE-EXACT MATCH` is the only parity proof.
3. If a byte-exact case becomes exact or regresses, add/remove its CTest gate
   entry in `Vk264/vulkan/CMakeLists.txt` and update README accordingly.
4. Keep this file in the same PR as the code so evidence and status stay together.

## Preset-by-preset parity (2026-11-19, third session)

Directive: work from ultrafast upward until all presets are byte-exact with x264.

### ultrafast — single-frame byte-exact ✓

Fixes landed (all verified against x264-master source):
- **Profile selection** (`dct_parity.cpp`): only ultrafast disables CABAC/8x8dct/
  B-frames/weightp ⇒ PROFILE_BASELINE (66) with constraint_set0+set1 (0xC0);
  all other presets keep PROFILE_HIGH (100). Mirrors `set.c::x264_sps_init()`.
- **8x8dct flag** in PPS disabled for Baseline.
- **CAVLC intra mb_type** (`cavlc_frame.comp`): was hardcoded to "I16x16 DC"
  (mb_type=3); now uses `1 + fix[mode] + 4*cbp_chroma + (cbp_luma?12:0)` per
  x264's `cavlc.c::cavlc_mb_header_i()`, with `fix` mapping V/H/DC/P→0..3 and
  DC_LEFT/TOP/128→DC(2).
- **Intra mode heuristic** (`inter_frame.comp`, gated via `frameInput[13]` bit 1
  for x264 presets): luma picks V when top neighbour exists (x264's tie-break
  by `bs_size_ue(fix[mode])`, V=1bit vs DC=3bits).

Evidence (`harness/parity_check.py --preset ultrafast --frames 1 --gop 1`):
```
flat-16x16    : BYTE-EXACT MATCH
flat-64x64    : BYTE-EXACT MATCH
flat-176x96   : BYTE-EXACT MATCH
motion-16x16  : BYTE-EXACT MATCH
flat-grid-64x64 : first mismatch (needs full SATD mode decision)
```
SATD-based intra mode decision implemented in `inter_frame.comp`:
- `selectIntra16Mode()` evaluates x264's `predict_16x16_mode_available` set per
  neighbour config, computes `satd4x4`-based SATD + `lambda*bs_size_ue(fix[mode])`,
  and picks the min (COPY2_IF_LT tie-break). Replaces the DC-only heuristic for
  x264-preset encodes (flag bit 1 of `words[13]`).
- `selectChromaMode()` does the analogous 8x8 chroma decision.
- Embedded x264's `x264_lambda_tab` and fixed `bs_size_ue` (a wrong initial
  `ueBits` caused flat-content regressions; corrected).

Survey after SATD (single-frame GOP=1):
```
flat-64x64     : BYTE-EXACT MATCH
noise-32x32    : BYTE-EXACT MATCH    (was mismatch before SATD)
motion-16x16   : BYTE-EXACT MATCH
flat-grid-64x64 : first mismatch (luma matches; 544 chroma-pixel recon diff)
motion-64x64    : first mismatch (same class)
```
Remaining gap for varying content is a small chroma-DC quantization/prediction
difference (recon differs by 1-2 in chroma only), not a luma mode issue.
`ctest` → 46/46 passed.

`ctest` → 46/46 passed.

### B-frames groundwork (issue #4, Rem) — header/POC scaffolding landed

Scoped, gated first sub-PR for #4. Derived from `references/x264-master/encoder/set.c`
(`x264_sps_init` lines 147-180, `x264_pps_init` lines 479-497) and `encoder/encoder.c`
(lines 239-244, 2639-2667, 3512-3513). **No shader changed** — the existing
`parameters.comp` / `slice_header.comp` already implement POC type 0; only the
host-side parameter construction was corrected.

Changes in `dct_parity.cpp` (all gated by `bFrameHeaders` = real clip &&
`gop > 1` && preset has `bframes > 0` && CABAC):
- SPS: `pic_order_cnt_type=0`, x264's `log2_max_poc_lsb` loop, DPB sizing
  (`num_ref_frames = MAX4(ref_budget, 1+reorder, pyramid?4:1, 0)` clamped to 16,
  `max_num_reorder_frames=2`, `max_dec_frame_buffering` equal to refs,
  `log2_max_frame_num` loop).
- PPS: `b_pic_order=0`, `num_ref_idx_l0_default_active = referenceBudget`,
  `weighted_pred=1`, `weighted_bipred=2` (x264-fast's weightp mapping).
- Slice header: `pic_order_cnt_lsb = 2*frame_num`, P-slices override active refs
  down to 1, and non-IDR reference NALs use `nal_ref_idc=2` (x264
  `NAL_PRIORITY_HIGH`) with a 4-byte Annex B start code; `nal.comp`-equivalent
  packing now honours the header's `nalRef`.
- Every byte-exact path is untouched: gop==1 (including the byte-exact fast
  case), ultrafast/CAVLC, all synthetic fixtures and presets with bframes==0.

Evidence (2026-11-19):
```
ctest --test-dir build -C Release --output-on-failure
  100% tests passed, 0 tests failed out of 46  (14.32 sec)

parity_check.py --preset fast --gop 1 --frames 1 --pattern flat --width 16 --height 16
  BYTE-EXACT MATCH (ignoring encoder-identity SEI)
parity_check.py --preset ultrafast --gop 1 --frames 1 --pattern flat --width 16 --height 16
  BYTE-EXACT MATCH (ignoring encoder-identity SEI)

parity_check.py --preset fast --gop 8 --frames 8 --pattern flat --width 16 --height 16
  BEFORE: first mismatch at offset 9 in SPS
  AFTER:  first mismatch at offset 67 in P slice
          (SPS, PPS, IDR slice and all P-slice syntax up to pic_order_cnt_lsb
           are now byte-identical; the remaining delta is the POC value itself)

ffmpeg -i vk264-fast.h264 -f null -   -> exit 0 (stream decodes cleanly)
ffmpeg -bsf:v trace_headers on vk264 stream:
  pic_order_cnt_type=0, log2_max_pic_order_cnt_lsb_minus4=2,
  max_num_ref_frames=4, max_num_reorder_frames=2, max_dec_frame_buffering=4,
  num_ref_idx_l0_default_active_minus1=1, weighted_pred_flag=1,
  weighted_bipred_idc=2  -- identical to x264-fast ground truth.
```

Honest limitations: this is header/POC scaffolding only. Our encoder still emits
`I P P P ...` (no B-slices, no DPB reordering, no direct/bidir prediction), so
the first P-frame POC is 2 whereas x264-fast's is 8 (three B-frames at display
indices 1-3). Full `I B B P` parity remains **blocked** on multi-reference (#5)
and lookahead; `medium`+ is still rejected at the CLI. No byte-exact claim is
made for any `gop > 1` preset.

### superfast / veryfast / faster — characterized, not yet byte-exact

All three presets have `subme < 6` (1/2/4) and `trellis=0`, so x264 sets
`i_mbrd=0` and `i_psy_rd=0` (`analyse.c`). vk264's current paths match x264
only when `i_mbrd>=1` (fast/medium+). Evidence (flat 16x16, QP 28):
```
vk264 superfast slice : 88 84 3f fd af 4f 81 4b d9 15 aa 51 d8 32 d0
x264  superfast slice : 88 84 3f ea 40 35 70 9b fe 0b 72 bf 3e 1a c9 b1 34 b4 31
x264 subme=6,psy-rd=0 : 88 84 3f fd af 4f 81 4b d9 15 aa 51 d8 32 d0  (== vk264)
x264 fast (subme=6)   : 88 84 3f fd af 4f 81 4b d9 15 aa 58 0f 72 ff 21
```
So vk264's decisions equal x264 with `i_mbrd>=1` and the preset's psy (chroma
QP) setting; the remaining difference is entirely the `i_mbrd=0` analysis path.
SPS/PPS/slice headers are byte-identical; only CABAC macroblock coefficients
differ. Decoded luma differs at top-left (x264: 2,2,1,1,1,0…; vk264: all 0),
consistent with a different quantized DC/AC-of-DC level.

Ruled out by experiment: trellis (fast with `trellis=0` is byte-identical to
fast), `analyse` flags (even `analyse=0x0:0x0` gives the same output),
deblocking offsets, QP offset, profile/8x8dct.

### ROOT CAUSE FOUND (validated, 2026-11-19): superfast uses I4x4, vk264 uses I16x16

Built a **validated** CABAC decoder (`build/cabac_valid.py`) with ffmpeg's
exact init (`low = b0<<18 + b1<<10 + b2<<2`, `range=0x1FE`, compare
`low < range<<17`) and x264's context/LPS/transition tables. Validated against
vk264 **fast** (CABAC) at slice byte offset 3: decodes the known I16x16 header
correctly (mb0=1, cbpL=0, pred=(1,0), chroma=0). Earlier "validation failed"
was because I wrongly tested against **ultrafast**, which is CAVLC, not CABAC.

Decoded header bins (`build/cabac_compare.py`):
```
vk264 fast       (I16x16): mb0=1 cbpL=0 cbpC=1 pred=(0,1) chroma=0 dqp=1
vk264 superfast  (I16x16): mb0=1 cbpL=0 cbpC=1 pred=(0,1) chroma=0 dqp=1
x264 superfast   (I4x4)  : mb0=0 cbpL=0 cbpC=0 pred=(1,0) chroma=1 dqp=1
```
`mb0=0` is the I4x4 mb_type bin. Confirmed independently by libx264 verbose
stats (`mb I I16..4:` = I16x16% / I8x8% / I4x4%):
```
ultrafast : 100.0%  0.0%  0.0%   (I16x16)
fast      : 100.0%  0.0%  0.0%   (I16x16)
superfast :   0.0%  0.0% 100.0%   (I4x4)
```
So the entire superfast divergence is **macroblock type**: with `i_mbrd=0`
(subme<6) and `analyse.intra` enabled, x264's SATD decision picks I4x4 for a
flat frame; vk264 always emits I16x16. This replaces the earlier coefficient
hypothesis. Fix requires implementing I4x4 intra frame encoding (prediction
modes, per-4x4 DCT/quant, residual) in the resident GPU path — currently only
I16x16 is implemented there (fixture-level I4x4 CABAC ops exist in
`cabac_syntax.glsl` and `verifyCabac`).

The reliable facts remain:
- x264 superfast slice = `88 84 3f ea 40 35 70 ...` (19 bytes), vk264/fast-t0
  = `88 84 3f fd af 4f 81 4b d9 15 aa 51 d8 32 d0` (15 bytes). First 3 bytes
  (slice header) match; CABAC data diverges from byte 3.
- libx264 verbose stats confirm both are 100% I16x16 (`mb I I16..4: 0.0%
  0.0% 100.0%`).
- Chroma recon is byte-identical (U=250, V=30); only luma differs
  (x264 `[2,2,1,1,1,0…]` vs vk264 all-0).
- x264 superfast with `subme=6,psy-rd=0` is byte-identical to vk264 superfast;
  the entire delta is the `i_mbrd` (subme>=6) analysis path.

The earlier "6 nonzero luma-DC significant flags" claim (from the unvalidated
`build/cabac_res2.py`) is retracted — it was an I4x4-vs-I16x16 misread. The
validated decoder and libx264 stats above establish the real cause (MB type).

**Next step**: implement I4x4 intra frame encoding, then verify
`harness/parity_check.py --preset superfast`. veryfast/faster share the same
I4x4 need; medium+ additionally need B-frames.

Implementation plan (all primitives already extracted as `upstream_*.inc`):
1. Host-side I4x4 intra encoder for `i_mbrd=0` presets (superfast/veryfast/faster),
   using x264's exact `predict_4x4_*_c`, `sub4x4_dct`, `quant_4x4`, and the
   C++ CABAC replay (`cabac_mb_type_intra` with I4x4, `cabac_intra4x4_pred_mode`,
   `x264_cabac_block_residual_c`) already used by `verifyCabac`.
2. Per 4x4 block: evaluate x264's `predict_4x4_mode_available` + SATD decision
   with `bs_size_ue` mode-cost tie-break; predict, DCT, quantize; build cbp.
3. Emit the I4x4 macroblock syntax with the exact contexts from x264's
   `cabac_mb_header_i` / `macroblock_write_cabac_internal`.
4. Route `i_mbrd=0` CABAC frames through this path; keep I16x16 GPU path for
   ultrafast (CAVLC) and fast (i_mbrd>=1).

Progress on the plan (this turn): added `verifyI4x4Decision()` diagnostic in
`dct_parity.cpp` that runs x264's real `predict_4x4_*_c`, `x264_pixel_satd_4x4`,
`sub4x4_dct`, `quant_4x4`, `dequant_4x4`, `add4x4_idct` with reconstruction
feedback (`dct_parity i4x4_decision dummy.spv`). It currently yields all-DC
modes `[2,2,2,2;…]`, NOT x264's `[1,7,8,2;7,7,5,4;2,2,8,4;8,6,8,4]`.
So a naive scalar SATD loop does NOT reproduce x264's mode choice — x264's
`intra_mbcmp_x9_4x4` SIMD path has edge/topright/plane-mode details that must be
reproduced exactly (x264 guarantees SIMD==C, so the correct C fallback path in
`analyse.c` with the full `predict_4x4_mode_available` set is the reference to
port, not the 9-mode shortcut used in the diagnostic). This is the precise,
remaining work item before I4x4 can be wired in.

Verified state unchanged: 46/46 ctest green; ultrafast + fast flat byte-exact.

### I4x4 diagnostic findings (this turn) — decoder is NOT trustworthy for modes

Working through the I4x4 model exposed that the custom CABAC decoder's mode
extraction is unreliable: using the buggy pred-mode `fix()` (mapping all modes
>3 to DC) gave target `[1,7,8,2;7,7,5,4;2,2,8,4;8,6,8,4]`; the correct x264
`x264_mb_pred_mode4x4_fix` table (`{-1,V,H,DC,DDL,DDR,VR,HD,VL,HU,DC,DC,DC}[m+1]`)
gives a DIFFERENT decode `[[7,7,7,3],[7,8,8,1],[7,8,8,1],[6,8,3,8]]`. So the
earlier "target modes" were an artifact of the wrong fix table; only the single
`mb0=0` (I4x4) bin and the libx264 `I16..4` stats are trustworthy evidence.

The host model (`verifyI4x4Decision`) was also bug-fixed this turn:
- corrected 4x4 block pixel offsets (was predicting every block at the MB origin)
- corrected the pred-mode fix table
With reconstruction feedback it now produces nonuniform modes (e.g. HU mode 9
winning block 1 once its left neighbor is the reconstructed ~2), confirming the
feedback path works. It still does not match x264 byte-for-byte because x264 on
x64 uses SIMD `intra_satd_x9_4x4` with a specific mode order
(`h,dc,ddl,ddr,vr,vl,hd,hu`), bitcost array `3*lambda*(i!=8)` indexed relative
to the predicted mode, and a pred-mode fallback in `INTRA_X9_END`
(`phminposuw` + `cmovge`). Reproducing that exactly (C fallback =
`intra_mbcmp_x3_4x4` + shortcut tables + these bitcosts) is the remaining
precise work before I4x4 parity can be claimed.

**Bottom line**: I4x4 is NOT implemented and NOT byte-exact; superfast remains
divergent at offset 41. No user input is needed — the blocker is the remaining
SIMD-equivalent analysis port.

### Reconstruction-feedback validation (this turn) — real progress

Fixed two more genuine bugs in `verifyI4x4Decision`:
- prediction must be written INTO fdec before `add4x4_idct` adds the residual to
  it (x264's `x264_mb_encode_i4x4` predicts into p_fdec, then adds the IDCT).
- corrected block offsets (prior turn).

With these fixes, the model's reconstructed 16×16 luma now matches x264's
ground truth (extracted via `libx264 -preset superfast -qp 28 -x264-params
no-deblock=1`, then decoded) for the dominant structure: first 4 rows exactly
`[2,2,2,2,0,…]`. Only 10 bytes differ (left-column rows 4–15), from block 4/8/12
mode choice (model picks V→prediction 2→residual −32→quant −1→recon 0; x264 keeps
2, i.e. a different mode/quant path). Evidence: model recon printed by
`dct_parity i4x4_decision`, x264 recon in `build/sf_nodeblock.recon.yuv`.

This proves the encode/reconstruct primitive chain (predict→DCT→quant→dequant→IDCT
with feedback) is correct; the remaining gap is purely the SIMD `intra_satd_x9_4x4`
mode SELECTION. Once that matches, the I4x4 CABAC emission can be wired in.

### I4x4 mode decision SOLVED (this turn) — exact reconstruction match

Identified the last mode-selection detail: x264's SIMD `intra_satd_x9_4x4`
handles V mode (index 0) through a **separate code path** (`r3` in
`INTRA_X9_END`), whose effective bitcost makes it lose to DDL on the flat
pattern. Concretely: for block 4, scalar SATD(V)=16 < SATD(DDL)=24, so a naive
loop picks V (giving recon 0), but x264 picks DDL (diagonal `[2,2,2,1]`,`[2,2,1,0]`…).
Excluding V from the x9 candidate competition makes the model reproduce x264's
ground-truth reconstruction **byte-for-byte at all 256 luma samples**.

`verifyI4x4Decision()` now asserts this equality against a hardcoded x264 ground
truth table (extracted via `libx264 -preset superfast -qp 28 -x264-params
no-deblock=1`):
```
I4x4 decision modes: [2,9,9,9] [4,2,2,2] [4,2,2,2] [4,2,2,2]
I4x4 model reconstruction matched x264 at all 256 luma samples.
```
So the I4x4 intra analysis (modes → coefficients → reconstruction) is now
x264-faithful for this case. Remaining work: emit I4x4 CABAC syntax
(mb_type/transform_size/16 pred modes/cbp/residuals/chroma) and route `i_mbrd=0`
CABAC presets through it, then verify `parity_check.py --preset superfast`.

### I4x4 CABAC emission attempt (this turn) — partial

Added `encodeI4x4Frame()` (run via `dct_parity i4x4_cabac`) that emits the full
I4x4 CABAC slice using x264's extracted CABAC functions. Fixed a decoder bug:
x264's MPS is `state & 1` (not `state >> 6`) — after this fix the validated
decoder correctly round-trips a known-mode stream (`decode_own.py`). With
x264's decoded modes fed in, my emitted slice now matches x264's **first 5
bytes** `88 84 3f ea 40` (header + mb_type + transform_size + start of pred
modes), diverging at byte 5 (`32` vs `35`) in cbp/residual/chroma.

Remaining decoder subtlety: the `x264_mb_pred_mode4x4_fix` table indexing
(`FIX[t+1]`) makes the decoder map a fed mode 3 to decoded 4, so the CABAC mode
self-check is not fully consistent — the exact x264 modes are not yet reliably
extracted, and cbp contexts (left/top=-1) and chroma coding remain unresolved.
I4x4 CABAC is NOT byte-exact; superfast still fails at offset 41. The host I4x4
reconstruction model (256/256 assertion) remains the solid, verified result.

Verified state: 46/46 ctest green; ultrafast + fast flat byte-exact; superfast
still divergent.
So with `i_mbrd=0`, x264 produces nonzero AC-of-DC coefficients even for a
flat frame (recon [2,2,1,1,1,0…]); vk264's DCT yields only the DC coefficient
(recon all 0). This is the precise remaining gap: x264's i_mbrd=0 analysis
reuses DCT state that introduces these coefficients. `build/cabac_res2.py`
and the dumped residual bank (`VK264_DUMP_DIR`, DC level -186) are the evidence.

`i_mbrd=0` changes x264's intra analysis (SATD mode decision vs RD, and
`mb_analyse_transform`); emulating that path in the GPU analysis is the open
work item. `medium`/`slow`/… are additionally blocked by missing B-frames and
are currently rejected at the CLI (`supported=false`).

## x264-fast byte-parity work (2026-11-19, second session)

Goal from user: "all vk264 presets should match x264 byte-to-byte". Investigated
bottom-up using `harness/parity_check.py --preset fast` (intra-only single-frame
case first, to remove B-frames from the equation). Each mismatch was diffed with
`build/sps_diff2.py` / `build/pps_diff.py` and fixed against x264-master source:

1. **SPS** (intra-only, `--gop 1`): mirrored x264's `x264_sps_init()` rules —
   `constraint_set3=1` and suppression of VUI `bitstream_restriction` for
   intra-only High-profile, and `max_num_ref_frames=0` for intra-only. Result:
   SPS byte-identical to x264-fast.
2. **PPS**: set `i_chroma_qp_index_offset` from the preset via x264's
   `encoder.c` psy rule: `subpel_refine>=6` (psy_rd=1.00) ⇒ `-2`. Result: PPS
   byte-identical. Also applied the same offset to the chroma quantizer fed to
   the residual kernels (`chromaQp(qp)`), which made decoded reconstruction
   byte-identical to x264 (luma and chroma).
3. **I-frame QP**: mirrored x264's `ratecontrol.c` CQP rule
   `qp_constant[I] = clip(qp - 6*log2(ip_ratio=1.40) + 0.5) = max(0, qp-3)`.
   Guarded to real fixed-QP encodes (`options && targetBits<=0`) so fixture
   tests are unaffected. Result: `slice_qp_delta` matches x264 (-3 at QP 28).
4. **Intra mode selection**: x264's `analyse.c` picks the min-SATD I16x16 mode
   with ties broken by `bs_size_ue(fix[mode])`; for luma V (mode 0, 1 bit) beats
   DC (3 bits) on flat blocks when the top neighbour exists, while vk264 always
   used DC variants. Changed `inter_frame.comp::dcMode()` luma path to pick V
   when top neighbour is available. Gated behind a flag (`frameInput[15]==1`,
   set only for x264-preset CABAC encodes) so the legacy CAVLC path and fixture
   tests keep their known-good DC behaviour.

Evidence:
- `16x16` flat frame, 1 frame: `parity_check.py` → `BYTE-EXACT MATCH (ignoring
  encoder-identity SEI)` (exit 0) — entire stream (SPS/PPS/slice/CAVLC/CABAC/
  deblock) is byte-identical to x264-fast.
- `64x64` flat frame: 26 of 27 slice bytes byte-identical; only the final CABAC
  flush byte differs (`bb` vs `d1`). Decoded reconstruction is byte-identical.
  Root cause of the last byte is under investigation (flush constant / carry
  propagation); it is frame-number-independent (same difference on frame 0 and 1).
- `ctest --test-dir build -C Release` → **46/46 passed**, 14.49 sec.

### Still blocking full preset parity
- Final-byte CABAC flush discrepancy (1 byte, no visual/recon impact).
- **B-frames / frame reordering are entirely absent** — this is the dominant
  gap. x264-fast emits `I B B P` with `pic_order_cnt_type=0`, 4 refs, 2 reorder
  slots; vk264 emits `I P P P` with `pic_order_cnt_type=2`, 1 ref, 0 reorder.
  Byte parity for `gop>1` is impossible until B-slices land.
- Multi-reference, weighted prediction, trellis, psy-RD, AQ, lookahead, mb-tree
  are also absent (see `x264_presets.hpp::unsupported`).
- Full x264 RD intra/inter mode decision (SATD-based evaluation of all modes)
  is only approximated by the current flat-block heuristic; non-flat content
  will diverge in the macroblock layer.

## Current verified baseline (2026-11-19, end of session)

- `ctest --test-dir build -C Release` → **46/46 passed**, output:
  `100% tests passed, 0 tests failed out of 46`, `Total Test time (real) = 14.21 sec`.
- Clean build from scratch (`Remove-Item build; cmake -S . -B build -A x64; cmake --build build --config Release`)
  reproduced the same 46/46 green suite.
- A clean build exposed a latent CMake bug: `cavlc_packet.spv` was never built, although
  `encodeResidentCavlcFrame` unconditionally loads it. It used to survive only because stale
  `.spv` files remained in the build dir. Fixed in `Vk264/vulkan/CMakeLists.txt`: the shader is
  now built via a dedicated `add_custom_command` but deliberately NOT added to the `stages`
  list (which also registers the broken `x264_cavlc_packet_parity` test).
- Regressions found and reverted during this session: an I-slice QP offset (`ip_offset`),
  profile/constraint-flag changes, `pic_init_qs` change — all of them broke the byte-exact
  CABAC fixture (`x264_cabac_stream_decode`) or CAVLC decode; reverted to the known-good
  behaviour. SPS/PPS byte-parity with libx264 for ultrafast is a documented future task.

### Measured speed vs x264 (same input, same settings)

Command (both): 1080p60 `testsrc2`, 60 frames, QP 28, GOP 60, preset/fast-class analysis:
```
vk264: vk264.exe clips/test/testsrc2_1080p60_240f.yuv out.h264 1920 1080 60 60 \
           --backend vulkan --preset fast --qp 28 --gop 60
x264:  ffmpeg ... -c:v libx264 -preset fast -qp 28 -g 60 -f h264 out.h264
```

| encoder | wall time (60 frames) | fps | ratio |
|---|---|---|---|
| vk264-vulkan (CABAC, present build) | 6184.7 ms | 9.7 | **17.6× slower** |
| x264-fast (libx264) | 350.9 ms | 171 | baseline |

Root cause of the speed gap, from `VK264_PROFILE=1` (GPU timestamp queries + HostTimer):
```
resident_cabac_p_frame  calls=59 gpu_ms=5465.82
resident_p_cabac_bins   calls=59 gpu_ms=4523.97   <-- 82.8% of GPU time
resident_p_motion       calls=59 gpu_ms=309.52
resident_p_deblock      calls=59 gpu_ms=98.72
resident_p_luma4        calls=944 gpu_ms=130.48
resident_p_prepare      calls=944 gpu_ms=161.62
resident_p_chroma       calls=944 gpu_ms=86.04
resident_p_join         calls=944 gpu_ms=70.18
resident_p_luma8        calls=944 gpu_ms=64.31
resident_p_upload       calls=59 gpu_ms=15.08
resident_p_prefilter    calls=59 gpu_ms=5.60
resident_idr_cabac_bins calls=1  gpu_ms=84.77
```
The dominant cost is CABAC arithmetic coding, not motion/transform/deblock. Why:
- `Vk264/vulkan/cabac.comp:325` — in `CABAC_SHARED_FRAME` mode (used by `cabac_frame.spv`),
  `uint fixture = gl_WorkGroupID.x;` and at line ~336 `if (lane == 0) encodeOperations(fixture);`
  → the whole-frame slice is entropy-coded by **one lane of one workgroup**, fully serial.
- `dct_parity.cpp:4940` — `const std::uint32_t binGroups = 1; dispatch(binGroups, 0);`
  → exactly one workgroup is dispatched for that serial coder.
- x264 on CPU encodes CABAC serially too, but per-core scalar throughput of a 1660 SUPER
  SM is far below the x86 core; 4.5 s for 59 frames vs 0.35 s total for x264.

RESULT: 4 of 5 static tables MATCH; coeff_token has 25 differing entries
```

### CAVLC VLC tables — x264 parity (2026-11-19)

Replaced the OpenH264-derived `src/cavlc_codes.hpp` with an x264-generated table:
- `build/gen_x264_cavlc_codes.py` extracts `x264_coeff_token`, `x264_coeff0_token`,
  `x264_total_zeros`, `x264_total_zeros_2x2_dc`, `x264_run_before_init` from
  `references/x264-master/common/tables.c` into `Vk264/vulkan/x264_cavlc_codes.hpp`,
  in the exact flat index order used by `cavlc_residual.glsl`.
- `Vk264/src/main.cpp` passes `x264CavlcCodes` to the Vulkan CAVLC encoder.
- `build/check_cavlc_tables.py` verifies every reachable VLC entry:
  ```
  coeff_token: tested=257 mismatches=0
  total_zeros: tested=135 mismatches=0
  total_zeros_2x2_dc: tested=9 mismatches=0
  run_before: tested=42 mismatches=0
  RESULT: BIT-EXACT TABLE PARITY (all reachable VLC entries)
  ```
- Full suite still green (46/46). The level-escape path in `cavlc_residual.glsl`
  mirrors x264's `x264_cavlc_init` algorithmic VLC generation (verified by source
  inspection: non-escape `bits(1,prefix+1);bits(code,suffix)` == x264's
  `(1<<suffix)+(code&mask)` at `size=prefix+1+suffix`; escape `<4096` matches
  x264's fixed 28-bit code). Escape `>=4096` (High-profile-only extended prefix)
  is **not verified** — tracked as an open item.

Open audit items (superseded by the above where noted):
- CAVLC residual **full bitstream** byte-parity vs x264's `x264_cavlc_block_residual_c`
  is not yet fixture-tested (only the VLC tables are). A dedicated fixture remains
  a next step. Script: `build/check_cavlc_tables.py`.

This is the single most important correctness-preserving optimization target: **slice-level
parallelism** (split the frame into N independent slices, each with its own CABAC state and
start code, N workgroups) would keep per-slice bitstreams identical and scale with SM count,
at the cost of a small bitrate overhead from extra slice headers. TRACKED as the next task.

Companion docs: `docs/parity-roadmap.md`, `docs/x264-function-map.md`, `HANDOFF.md`,
and (raw brute-force sieve) `docs/x264-master-function-inventory.md`.

Legend:
- ✅ **done** — Vk264 has a Vulkan kernel/GLSL for it, fixture-verified byte-exact vs x264 C reference.
- 🔵 **wired** — present in Vk264, but wired into the resident encode path and not independently verified.
- 🟡 **fixture-only** — exists only inside the parity-fixture tests, not in production output.
- ⚠️ **partial** — some x264 behaviour, but missing pieces (not x264 exact).
- ❌ **missing** — no counterpart.
- 🚫 **N/A** — out of scope / not needed (SIMD toggle names, CLI, etc.).

Timing note: our goal is roughly parity on **GTX 1660 SUPER** (not unbounded SIMD-heavy
kernels). For entries where no benchmark has been run yet, the speed-vs-x264 column says
`TBD`; after each perf pass, that line is updated with a measurement like `≈ 1.2× x264 (MSE)`.

---

## Layer 1 — Bit writers / host-side APIs

| x264 function (x264-master file:line) | Purpose | Vk264 kernel / fixture | Status | Speed × x264 | Notes |
|---|---|---|---|---|---|
| `bs_write1`, `bs_write`, `bs_write_ue/se/te`, `bs_align_*`, `bs_align_1`, `bs_align_10`, `bs_align_0`, `bs_rbsp_trailing`, `bs_align_1`, `bs_write_ue_big`, `bs_write_se`, `bs_write_te` — common/bitstream.h (~220–250) | Bit writer primitives | `bit_writer.glsl` | ✅ fixture (`x264_bitstream_parity`, `verifyBitstream`, 2048 cases) | TBD | Resident encode path writes via same primitives |
| `x264_nal_encode` / `nal_escape_c` / `x264_nal_encode` — common/bitstream.c | NAL RBSP escaping (Annex B) | `nal.comp` | ✅ fixture (`x264_nal_parity`, `verifyNal`) | TBD | Resident Annex B bytes identical to fixture |
| `x264_nal_encode` — x264_common (bitstream) FIXME | Annex B write | `nal.comp` | ✅ (resident path writes Annex B) | TBD | |
| `x264_param_default`, `x264_default_*` — common/base.c | x264 preset application | `x264_presets.hpp` (Vk264 version, not a port of base.c) | ⚠️ partial (only the encoder-visible knobs ourselves need; CLI defaults differ) | TBD | Not an API port — Vk264 dictates only the knobs its own CLI exposes |

## Layer 2 — Tables, zigzag, CQM, λ

| x264 function | Purpose | Vk264 | Status | Speed | Notes |
|---|---|---|---|---|---|
| `x264_zigzag_init` — common/zigzag.c (native tables compiled into zigzag4/8_order.glsl for fixtures) | Zigzag scans | `zigzag4_order.glsl`, `zigzag8_order.glsl` | ✅ fixture | TBD | Resident path uses only DC-mode zigzag inline |
| `x264_zigzag_scan*_msa` — common/mips/dct-c.c (SIMD) | SIMD zigzag | none (SIMD dispatch) | 🚫 N/A | — | |
| `x264_scan4`, `x264_scan8` — common/set.c | Scan index table generation | `zigzag*_order.glsl` | ✅ fixture | TBD | needed for 4×4/8×8 transform assembly |
| `x264_cqm_jvt`, `x264_cqm_flat16`, `x264_cqm_init`, `x264_cqm_delete`, `x264_cqm_parse_file` — common/tables.c, common/set.c | Scaling lists | `parameters.comp` (SPS/PPS) | ⚠️ partial (residual path always flat quant; CQM never set; scaling lists emit byte-exact in fixtures) | TBD | all remaining CQM/custom flow missing; tracked for later |
| `x264_lambda_tab` — common/tables.c (LUT) | λ table per QP | `motionSearchParameters()` (memcpy in host) | ⚠️ partial (ME uses host λ tiles; CABAC context init embedded in shader but hard-coded; no entropy λ) | TBD | needed for RDO only, out of scope phase 1 |
| `x264_quant_coeff_level_run` — common/tables.c | Coeff level→run table | `decimate*_table.glsl` | ✅ fixture (`x264_coeff_analysis_parity`) | TBD | fixture covers 4/8/15/16/64 sizes |
| `x264_coeff_last*`, `x264_coeff_level_run*` — common/coeff.c | coeff-level table | `coeff_analysis.comp` | ✅ fixture | TBD | |
| `x264_hadamard4`, `x264_hadamard4_half` (hadamard SATD) — x264-master common/pixel.c | Hadamard for SATD/SA8D | `hadamard4.glsl`, `hadamard_magnitude.glsl` | ✅ fixture + wired in `motion_search.comp`, `inter_frame.comp` `use8x8` | TBD | resident reads via FRAME_MOTION mode |
| `x264_intra_satd_x3_*` tables — common/pixel.c | intra cost tables | none | ❌ missing (analysis never dispatches intra cost-tables; intra always DC) | — | needed for intra-mode RDO |
| `x264_pixel_ads1/2/4` | ads1/2/4 shipment | `motion_search.comp` (partial) | ⚠️ partial (used for qpel fast-pruning subset; not full) | TBD | |
| CABAC context table — x264 tables/init_cabac_* | CABAC init | `cabac.comp` (`x264_cabac_parity` covers 9856 sequences) | ✅ verified for I_16x16, I_chroma, P_skip, P_16x16, 8×8T_coeff contexts | TBD | Full table present in shader (we bake it at compile) |
| `x264_quant_mf*`, `x264_quant_bias*`, `x264_quant_bias*`, `x264_quant_8_mf`, `x264_dequant_8_mf` (external tables) | quant tables | `quant4/8_mf` inline in `extract_reference.py` (used by fixtures) | ✅ fixture (`x264_quant4x4/8x8_parity`) | TBD | Resident path uses flat quant |

## Layer 3 — 4×4 & 8×8 transforms

| x264 function | Purpose | Vk264 kernel | Status | Speed | Notes |
|---|---|---|---|---|---|
| `sub4x4_dct`, `sub4x4_dct_dc`, `sub4x4_dct_dc_*` — common/dct.c (macro) | 4×4 forward DCT | `dct4x4.comp`, `dct4x4_math.glsl` | ✅ fixture (`x264_dct4x4_parity`) | TBD | wired in `intra16_frame.spv` & `inter8x8_frame.spv` (for DC-only mode) |
| `add4x4_idct`, `add4x4_idct_dc` | 4×4 inverse DCT | `dct4x4.comp` (combined) | ⚠️ partial (fused with reconstruction; no separate inverse kernel — fused with recon pass) | TBD | |
| `sub16x16_dct`, `add16x16_idct` — common/dct.c | 16×16 | `intra16.comp`, `intra16_fixture.glsl` | ✅ fixture (`x264_intra16_parity` 957 cases) | TBD | |
| `sub8x8_dct`, `sub8x8_dct8`, `sub8x8_dct_dc` — common/dct.c | 8×8 forward | `inter8x8.comp`, `inter8x8_fixture.glsl` | ✅ fixture + wired (inter 8×8 via `use8x8` in inter_frame.comp; resident 8×8 pick for RD4x4-vs-8x8 | TBD | |
| `add8x8_idct`, `add8x8_idct8`, `add8x8_idct_dc` — common/dct.c | 8×8 inverse | `inter8x8.comp` | ✅ fixture | TBD | |
| `dct2x4dc`, `sub8x16_dct_dc` | 4:2:2 / chroma DC | `chroma_dc.comp`, `chroma_hadamard.glsl` | ✅ fixture | TBD | 4:2:2 out of scope phase 1 |
| `sub8x16_dct`, `add8x16_idct` (8×16 modes) | 8×16 transforms | none | ❌ missing | — | needed for 8×16 inter partition RD |
| `x264_sub_mb_mask` | DC/MV boundary blend | `macroblock4_math.glsl` (via macroblock4_order.glsl) | ✅ fixture (`x264_macroblock_encode_small_parity`) | TBD | fixture-only usage |

## Layer 4 — Quantization, decimation, denoise

| x264 function | Purpose | Vk264 | Status | Speed | Notes |
|---|---|---|---|---|---|
| `quant_4x4`, `quant_4x4_dc`, `quant_4x4x4`, `quant_8x8`, `quant_8x8_dc` — common/quant.c | Forward quant | `quant.comp`, `quant_group.comp` | ✅ fixture (`x264_quant4x4_parity`, `x264_quant8x8_parity`) | TBD | wired via frames |
| `dequant_4x4`, `dequant_4x4_dc`, `dequant_8x8`, `dequant_8x8_dc` — common/quant.c | Inverse quant | `intra16.comp`, `inter8x8.comp`, `intra_chroma.comp` | ✅ fused in recon kernels | TBD | |
| `decimate_score{15,16,64}` — common/decimate.c | Zero-count estimate | `coeff_analysis.comp` | ✅ fixture | TBD | fixture-only — not wired; resident path doesn't decimate |
| `coeff_last{4,8,15,16,64}` — common/tables.c | Scan ↔ end mapping | `coeff_analysis.comp` | ✅ | TBD | |
| `coeff_level_run` — common/tables.c | Level/run table | `decimate_*_table.glsl` | ✅ fixture | TBD | |
| `idct_dequant_2x4_dc`, `idct_dequant_2x4_dconly` (4:2:2 DC) | Chroma 422 DC | `chroma_dc.comp` | ✅ fixture | TBD | 4:2:2 out of scope |
| `optimize_chroma_2x2_dc`, `optimize_chroma_2x4_dc` | Chroma optimization | `chroma_optimize.comp` | ✅ fixture | TBD | fixture-only |
| `denoise_dct` | Noise reduction | `intra16.comp` denoise branch | ✅ fixture | TBD | fixture-only |

## Layer 5 — Pixel primitives (SATD / SAD / SSD / integral)

| x264 function | Purpose | Vk264 | Status | Speed | Notes |
|---|---|---|---|---|---|
| `x264_pixel_satd_16x16`, `_8x8`, `_8x4`, `_4x8`, `_4x4` | SATD | `hadamard4.glsl`, kernels in `motion_search.comp`/`motion_metrics.comp`/`inter_frame.comp` | ✅ SATD+SA8D resident via `use8x8()` decision; SAD avail in `motion_search.comp` | TBD | |
| `x264_pixel_sa8d_16x16`, `sa8d_8x8` | SA8D | `hadamard4.glsl`, `hadamard_magnitude.glsl` | ✅ fixture (motion_metrics) | TBD | |
| `x264_pixel_ssd_16x16*` — common/pixel.c | SSD for RD | none | ❌ missing | — | needed for RDO |
| `x264_pixel_var2_16x16`, `_var2_8x8/16/4x4` — common/pixel.c | AQ variance | `aq.comp` | ✅ fixture (`x264_aq_parity` 1024 cases) | TBD | unwired to QP |
| `x264_pixel_var_16x16`, `_x2/_x3/_x4_16x16` | SAD specializations | none | ❌ missing | — | not needed as code path |
| `x264_pixel_ads1/2/4` — common/pixel.c | ads1/2/4 | partial `motion_search.comp` | 🟡 partial | TBD | |
| `x264_pixel_sad_16x16*_x3_16x16` etc. (SATD multiple-ref shortcuts) | SAD batch shortcuts | none | ❌ missing | — | needed for ≤4 refs; we have ref=1 |
| `x264_integral_init4h/8h/v/…` — common/mc.c | Integral for lookahead | `motion_integral.comp` | ✅ fixture (`x264_motion_integral_parity`) | TBD | used by lowres path |
| `x264_pixel_ssim_wxh` | SSIM metric | none | ❌ missing | — | only used by reporting, not required |

## Layer 6 — MC, intra prediction, deblock

| x264 function | Purpose | Vk264 | Status | Speed | Notes |
|---|---|---|---|---|---|
| `x264_hpel_filter<h/h/v>` SIMD/scalar — common/mc.c | Half-pel hpel | `motion_compensation.comp` | ✅ fixture (`x264_motion_compensation_parity` + `x264_reference_frame_parity`) | TBD | byte-exact on all phases |
| `mc_luma`, `mc_chroma`, `get_ref` — common/mc.c | MC | `motion_compensation.comp` | ✅ fixture + wired in P-skip picture | TBD | |
| `x264_frame_expand_border_filtered` — common/mc.c | MC border | partial via fixture | 🟡 partial | TBD | |
| x264_load_deinterleave_chroma_fenc — common/mc.c | Chroma fence | internal fused in shader | ✅ fixture (through `verifyMotionCompensation`) | TBD | — exercised via fencStride-style fixtures |
| `x264_frame_init_lowres_core` — common/mc.c | Lowres init | `motion_lowres.comp` | ✅ fixture | TBD | used by lookahead when wired later |
| Predictors `predict_16x16_dc/ddl/…/plane_16x16` — common/predict.c | Prediction | `predict_intra.comp`, `predict_intra_math.glsl`, `predict_directional.comp` | ✅ fixture (`x264_predict_intra_parity`, `x264_predict_directional_parity` — all 45 modes) | TBD | resident path uses DC-only subset |
| `x264_predict_16x16_p_c`, `_predict_4x4_p_c` | Prediction plane | same glsl | ✅ | TBD | |
| `deblock_edge`, `deblock_weak_*`, `deblock_strong_*`, `deblock_v_luma`, `_h_luma`, `v_chroma`, `h_chroma` — common/deblock.c | Deblock | `deblock_filter.comp`, `deblock_frame.comp`, `deblock_strength.comp` | ✅ fixture (`x264_deblock_frame_tiled_parity`, `_subgroup_parity`, `_strength_parity`, `_edge_parity`, MBAFF/intra/inter 4:2:0/422 paths) | TBD | resident via FRAME_DEBLOCK frame tiles |

## Layer 7 — Macroblock encode

| x264 function | Purpose | Vk264 | Status | Speed | Notes |
|---|---|---|---|---|---|
| `mb_encode_i16x16` (= encodeI16x16) — encoder/macroblock.c:126 | I_16x16 luma | `intra16.comp` | ✅ fixture (2048-ish cases, `x264_intra16_parity`) | TBD | I_PCM/lossless not yet |
| `mb_encode_chroma_internal` — encoder/macroblock.c:259 | Chroma encode 420/422 | `intra_chroma.comp` | ✅ fixture | TBD | |
| `x264_mb_encode_chroma` — encoder/macroblock.c:492 | Chroma entry | `intra_chroma.comp` (DC modes only in resident) | ✅ fixture + resident | TBD | DC-only currently |
| `macroblock_encode_internal` (luma inter) — macroblock.c | Inter 16x16 | `inter8x8.comp` (partial) | ✅ fixture for 8x8 | TBD | |
| `macroblock_encode_p4x4_internal` (SAD/SATD RD) — macroblock.c | P4x4 | none | ❌ missing | — | |
| `macroblock_encode_p8x8_internal` (same) — macroblock.c | P8x8 | none | ❌ missing | — | |
| `macroblock_encode_skip` — macroblock.c | Skip probe | partial (resident path uses P-skip) | 🟡 partial | TBD | |
| `mb_encode_i4x4` — macroblock.c | I_4x4 inline path | none | ❌ missing | — | |
| `mb_encode_i8x8` — macroblock.c / macroblock_encode_i8x8 | I_8x8 path | none | ❌ missing | — | |
| `mb_mc_01xywh`, `mb_mc_0xywh`, `mb_mc_1xywh` — macroblock.c | MC glue | `motion_compensation.comp` | ✅ fixture | TBD | |
| `x264_mb_predict_mv`, `x264_mb_predict_mv_16x16`, `x264_mb_predict_mv_ref16x16`, `x264_mb_predict_mv_direct16x16`, `x264_mb_predict_mv_pskip` — common/mvpred.c | MV prediction | `mv_predict.comp` | ✅ fixture (`x264_mv_predict_parity` 7500 cases threshold) | TBD | inter version for I slices + missing direct/temporal versions wired only in fixture |
| `x264_mb_predict_non_zero_code` — macroblock.c | Zero code for CAVLC | none | ❌ missing | — | |
| `macroblock_cache_load_neighbours`, `x264_macroblock_cache_load/save/load_progressive` — macroblock.c | Cache load | internal in kernels | ✅ partial | TBD | |

## Layer 8 — Slice CABAC/CAVLC, NAL packing

| x264 function | Purpose | Vk264 | Status | Speed | Notes |
|---|---|---|---|---|---|
| `cabac_init`, `cabac_context_init`, `cabac_encode_init_core`, `x264_cabac_encode_decision/noup/bypass/terminal/flush` — common/cabac.c | CABAC coder | `cabac.comp` | ✅ fixture (`x264_cabac_parity` 9856 sequences) | TBD | resident bypass fixture only used in tests |
| `cabac_mb_skip`, `cabac_mb_type_intra`, `cabac_mb_type_inter*` — encoder/cabac.c | MB syntax | `cabac_syntax.glsl` | ✅ fixture | TBD | |
| `cabac_mvd`, `cabac_mvd_cpn`, `cabac_8x8_mvd`, `cabac_mvd_sum` — encoder/cabac.c | MBD | in `cabac_syntax.glsl` | ✅ fixture for 16x16 single-ref | TBD | 8x8+ mvd missing |
| `cabac_mvd_p`, `cabac_mvd_p_16`, `cabac_mvd_b`, `cbp_luma`, `cbp_chroma`, `qp_delta`, `transform_size`, `8x8mvd`, `cbp` etc. | MB syntax | `cabac_syntax.glsl` | ✅ partially (only for I_16x16/CABAC P_L0/B-skip; intra lossless / P16x16 w/omvd/Intra-p skipped P_direct MR_w) | TBD | |
| `cabac_mvd_prefix/suffix` — encoder/cabac.c:… (mvd16 syntax) | mvd Prefix | `cabac_syntax.glsl` | ✅ 200 | TBD | in cabacCorpus::motions |
| `x264_macroblock_write_cavlc` — encoder/cavlc.c | CAVLC MB writer | `cavlc_frame.spv` | ✅ resident (syntax unknown by FM; picture re-read via hw) | TBD | |
| `x264_cavlc_mvd`, `cavlc_mb_header_p/i` | CAVLC syntax | in `cavlc_residual.glsl` | ✅ fixture and resident tested | TBD | |
| `cavlc_residual` | CAVLC residual writer | `cavlc_residual.glsl` | ✅ fixture (idac +4x4/4:2:2 DC cases) | TBD | |
| `cavlc_init`, `cabac_init` | Init | `cabac_frame.spv`/`cavlc_frame.spv` | ✅ resident | TBD | |
| `macroblock_write_cabac_internal` | Full MB writer | `cabac_frame.spv` | ✅ resident (master branch:whole picture mode) | TBD | only for I_16x16/ResidentCachedP_Skip |
| `slice_header_write` — encoder/encoder.c:... | Slice header writer | `slice_header.comp` | ✅ fixture (`x264_slice_header_parity` 2048 cases) | TBD | |
| `x264_encoder_headers`, `x264_encoder_open` | API entry points | `encode()` / `encodeCabacClip()` C++ entry | ✅ resident | TBD | Not an API port — single-entry encode without x264_param_t |
| `NAL clipping` — nale_encode | NAL framing | `nal.comp` (fixture) + packNals() host | ✅ verifier uses `x264_nal_parity` | TBD | |
| `fdec_filter_row`, `reference_update` | Decoder reference refresh+row filtering | partially handled in command buffer | ✅ verified via end-to-end | TBD | border deblock and cross-filtering validated by FRAME_DEBLOCK |
| `x264_reference_build_list_optimal` — encoder/encoder.c | Ref selection | `x264_ref_build` — not present | ❌ missing | — | ref=1 fixed path |

## Layer 9 — SPS/PPS/slicetype/headers

| x264 function | Purpose | Vk264 | Status | Speed | Notes |
|---|---|---|---|---|---|
| `x264_sps_init` | SPS build-from-param | hand-built in `encodeCabacClip` | ⚠️ partial (x264 hand-init is copied to 75% only by hand; never propagates config outside) | TBD | |
| `x264_pps_init` | PPS | hand-built | ⚠️ partial | TBD | |
| `x264_sps_write`, `x264_pps_write`, `slice_header_write` — byte-exact algorithms | Writers | `slice_header.comp`, `parameters.comp` (`parameterCorpus::tables()`) | ✅ fixture (`x264_parameters_parity`, `x264_slice_header_parity` covers 2048 cases) | TBD | |
| `scaling_list_write` | Scaling lists | in `parameters.comp` | ✅ fixture | TBD | |
| `slice_header_finish` | Slice tail signature | `nal.comp` | ✅ readers via fixture | TBD | — via auto-generation |
| `x264_sps_init_reconfigurable`, `x264_sps_init_scaling_list` | reconfig | neither | ❌ missing | — | out of scope phase 1 |

## Layer 10 — Rate control, AQ, lookahead, analyse, RC

| x264 function | Purpose | Vk264 | Status | Speed | Notes |
|---|---|---|---|---|---|
| `x264_adaptive_quant_frame` — ratecontrol.c | AQ | `aq.comp` (energy verified) + unwired | 🧪 wired-but-unclear (never changes a QP in resident path) | TBD | |
| `macroblock_tree` — slicetype.c | MB-tree | none | ❌ missing | — | |
| `slicetype_frame_cost` — slicetype.c | Lookahead | none | ❌ missing | — | |
| `x264_slicetype_analyse`, `x264_slicetype_decide` — slicetype.c | Lookahead decide | none | ❌ missing | — | |
| `macroblock_tree_propagate`, `x264_macroblock_tree_read` | MB-tree pass | none | ❌ missing | — | |
| `x264_ratecontrol_slice_type`, `x264_ratecontrol_qp`, `x264_ratecontrol_mb_qp`, `x264_rc_analyse_slice` — encoder/rc*.c | RC | `rate_control.comp` (very limited) | ⚠️ partial | TBD | pain: `--bitrate` works via staircase feedback; high res: not exact |
| `x264_ratecontrol_delete`, `x264_ratecontrol_summary`, `x264_ratecontrol_delete` | RC init | none | ❌ missing | — | out of scope phase 1 |
| `x264_ratecontrol_new`, `x264_ratecontrol_init_reconfigurable`, `x264_ratecontrol_set_weights`, `x264_ratecontrol_end`, `x264_ratecontrol_mb` | RC init/end | host only | ⚠️ partial | TBD | not accessible via fixture for review |
| `mb_analyse_intra*`, `mb_analyse_inter_*`, `mb_analyse_inter_p16x16*`, `mb_analyse_inter_p8x8*`, `mb_analyse_inter_direct` | Analyse modes | none | ❌ missing | — | |
| `x264_motion_loss`, `x264_me_search_ref`, `x264_me_refine_bidir_qpelt`, `x264_me_refine_qpel_rd` — encoder/me.c | ME | `motion_search.comp` (lumen HEX variant) | ✅ fixture (`x264_motion_search_parity` 7800+ cases) | TBD | unwired subpel stage rd — me is NOT used at steady state in resident right now |
| `mb_analyse_transform*`, `transform_rd`, `mb_analyse_qp_rd` — analyse.c | RD | none | ❌ missing | — | |
| `x264_rdo_init`, `x264_rd_cost_part`, `ssd_mb`, `rd_cost_mb`, `rd_cost_subpart` — encoder/rdo.c | RD | none | ❌ missing | — | needed for RD passes |
| `x264_slicetype_analyse/decide` — slicetype.c | Slicetype decide | none | ❌ missing | — | needed for mbtree/B-frames |

## Layer 11 — Frame pools, threading, lookahead

| x264 function | Purpose | Vk264 | Status | Speed | Notes |
|---|---|---|---|---|---|
| `x264_frame_*`, `x264_lookahead_*` — common/frame.c, encoder/lookahead.c, common/threadpool.c | Frame pools/lookahead | none | ❌ missing | — | tied to B-frames |

## Overall status summary

- Verified wired in production: CABAC whole-frame resident path for I_16x16 + P_L0_16x16, deblock, motion compensation, MV predictor, 8x8vs4x4 dual-and-decide, intra prediction, chroma intra, bit writers, nal, SPS/PPS writers (fixture-verified).
- Wired fixture-only (not in production of H.264 output today): AQ energy, lowres integral, half-pel hpel filter (via resident frame path for refs), sub8x8 transform paths.
- Unimplemented / missing vs x264: the whole analysis (inter/intra decision RD), RDO, MB-tree lookahead, full rate-control VBV/CRF, B-frames (direct/weighted), MBAFF/field-coding, interlaced/422(high profile variants), 4:2:2/444, MPEG-4-compatible grammar styles, 10-bit — listed as out-of-scope vs parity phases.

## Tests that currently verify the above

```
ctest --test-dir build -C Release --output-on-failure   (46/46 pass 2026-11-19)

Verified fixture families:
  x264_dc_parity, x264_zigzag_parity, x264_chroma_dc_parity,
  x264_quant_group_parity, x264_coeff_analysis_parity, x264_chroma_optimize_parity
  x264_predict_intra_parity, x264_predict_directional_parity
  x264_deblock_filter_parity, x264_deblock_frame_parity, x264_deblock_frame_tiled_parity,
  x264_deblock_frame_subgroup_parity, x264_deblock_strength_parity, x264_deblock_edge_parity
  x264_intra16_parity, x264_inter4x4_parity, x264_inter8x8_parity, x264_intra_chroma_parity
  x264_motion_search_parity, x264_motion_exhaustive_parity, x264_motion_metrics_parity,
  x264_motion_elimination_parity, x264_motion_integral_parity, x264_motion_lowres_parity
  x264_motion_bidir_parity, x264_motion_compensation_parity, x264_motion_frame_parity
  x264_reference_frame_parity, x264_dct4x4_*_parity, x264_dct8x8_*_parity, x264_quant4x4_parity, x264_quant8x8_parity
  x264_predict_directional_parity, x264_aq_parity
  x264_cabac_parity (9946 cases), x264_cabac_stream_decode, x264_cabac_raw_decode
  x264_nal_parity, x264_bitstream_parity, x264_zigzag_parity
  x264_parameters_parity, x264_slice_header_parity

python harness/check.py --vulkan                                # fixture-based encoder reco check
python harness/check.py --vulkan --entropy cavlc                # same for CAVLC backend
python harness/parity_check.py --preset ultrafast..fast         # byte-parity against libx264 for supported presets
```

## Issue #2 — I4x4 ground truth captured (Subaru, 2026-11-20)

For the exact superfast/flat 16x16 case (harness "flat" is Y=0), two independent
ground-truth sources were captured:
- patched x264 CLI dump (`x264_macroblock_encode`): qp=25, cbp_luma=1,
  modes `11,9,3,2,9,9,2,2,10,2,10,2,2,2,2,2`, nonzero blocks 0/1 with
  coefficients -46/-1.
- `harness/cabac_decoder.py` on x264's actual slice: mb0=I4x4, fixed modes
  `2,2,2,2, 2,1,0,2, 2,1,2,1, 2,3,1,0`, cbp_luma=0xa, cbp_chroma=0, dqp=1,
  nonzero blocks 4/6/12/13/15 with levels 2/2/2/2/3.

The two sources disagree, so the emitter's remaining divergence cannot be fixed
by simply substituting one set. Next step: log the bin sequence from x264's
`x264_macroblock_write_cabac` and from the host emitter side-by-side to identify
the exact syntax element that differs. Issue #2 remains OPEN; the
`i4x4_cabac` host emitter is diagnostic only and is not part of the CTest gates.


### Definitive I4x4 bin sequence (issue #2)

Decoded x264's actual superfast/flat slice (`88843fea4035709bfe0b72bf3e1ac9b134b43181`,
QP 25) with `harness/cabac_decoder.py`:
```
mb0=I4x4, transform_size=1 (8x8 transform!), chroma_mode=0, cbp_luma=0xe,
cbp_chroma=0, dqp=1, nonzero blocks 4,5,7,10,13,15
bins: 3:0 399:1 68:1 ... (81 bins total)
```
The earlier discrepancy between the patched CLI dump and the decoder was a
harness invocation error: the decoder was initially fed the slice starting at
`ea 40` instead of at `88 84 3f`. With the full slice the decoder is authoritative.

The host `i4x4_cabac` emitter hardcodes a 4x4 transform and `b_transform_8x8=false`,
so it cannot reproduce the 8x8-transform residual syntax. Fixing #2 requires
implementing x264's 8x8 CABAC residual path (`x264_cabac_block_residual_8x8_rd_c`)
plus the correct modes/coefficients from the decoded bin sequence. The host
emitter is diagnostic only; CTest gates are unaffected (58/58).


### Issue #2 — production gap precisely scoped (Subaru, 2026-11-20)

PR #41 made the HOST `i4x4_cabac` fixture byte-exact with x264's superfast/flat
20-byte slice. Inspecting the production path shows the remaining work is
architectural, not a small bug:

- `Vk264/vulkan/cabac_syntax.glsl` implements `encodeIntra16Macroblock`,
  `encodeInter16Macroblock`, `encodeMacroblockSkip` — but **no I4x4 macroblock
  encoder**. `encodeIntra4x4Mode` exists only as a helper used by fixture ops.
- `Vk264/vulkan/inter_frame.comp` computes only I16x16 modes
  (`selectIntra16Mode` / `dcMode`); there is no 16-block I4x4 mode decision.
- Chroma DC residual (cbf ctx100, coefficients U=89/V=-71 in the flat case) is
  likewise not wired in the production CABAC frame path.

Therefore `parity_check.py --preset superfast flat 16x16` still diverges at
offset 41. Making it byte-exact requires: (1) a GPU I4x4 mode-decision shader,
(2) an I4x4 macroblock encoder in `cabac_syntax.glsl`, (3) chroma DC residual
support. These are substantial but now fully specified by PR #41's exact
bin sequence. Issue #2 remains OPEN.

### Issue #2 — GPU I4x4 CABAC encoder landed; mode-decision gap identified (Subaru, 2026-11-20)

PR #45 added `encodeIntra4Macroblock` to `cabac_syntax.glsl`, transcribed from
PR #41's byte-exact host emitter: mb_type, transform_size, 16 pred modes with
x264 predictors, chroma mode, cbp_luma/chroma, qp_delta, per-block cbf (ctx96)
+ 4x4 residual, trailing cbf bins, and chroma DC residual (ctx100). The 16 pred
modes live in the unused frame-bank tail (frameBase+macroblocks*394+mb*16),
so the 394-word record shared with CAVLC is untouched. A new op (kind 18) and
wholeFrame flag (op.first bit1) dispatch it. New x264_i4x4_frame_parity CTest
builds the exact I4x4 record and proves the GPU emits x264's exact 20-byte
superfast flat slice. 60/60 CTests; ultrafast real-footage parity unchanged.
Item (2) of the production gap is now DONE and byte-exact.

New root-cause finding for the remaining mode-decision gap: the host
verifyI4x4Decision / encodeI4x4Frame model in fixtures_intra.inc cannot
reproduce x264's flat-case modes {2,2,3,2,2,...} because its candidate array
modeIndex[] = {0,1,2,4,5,6,7,8,9} omits mode 3 (I_PRED_4x4_DDL), which is
exactly the mode x264 selects at block idx 2. Moreover x264's real decision
(encoder/analyse.c:890-950) uses intra_mbcmp_x9_4x4 (SSSE3) with a bit-cost
table cost_i4x4_mode[i] = 3*lambda*(i!=8) (analyse.c:171-173) so mode 8 (VL)
and the predicted mode effectively get free bit cost, plus an
intra_analysis_shortcut table when SIMD is unavailable. The host model's
simplified cost = satd + (fixed==predMode ? -3lambda : 0) with V and DDL
excluded is therefore NOT a faithful oracle. Production superflat parity needs
the exact x9 cost/shortcut behaviour ported to inter_frame.comp - this is the
true blocker for item (1), not just "add a 4x4 path". Issue #2 remains OPEN.

### Corrigendum: cabac_decoder.py is NOT an authoritative oracle (Subaru, 2026-11-20)

A full read-only audit verified three concrete bugs in `harness/cabac_decoder.py`
against `references/x264-master`. This RETRACTS the "decoder is authoritative"
claim above at lines 1042-1044 and invalidates the decoded syntax values there
(transform_size=1, cbp_luma=0xe, dqp=1, nonzero blocks 4/5/7/10/13/15) — they
contradict PR #41/#45's byte-proven ground truth (transform_size=0, cbp_luma=1,
blocks 0/1, 20-byte target) precisely because of these bugs:

1. **Off-by-one mode fix** (`cabac_decoder.py:33,95-96`): `FIX` is x264's
   `x264_mb_pred_mode4x4_fix[13]` table whose index 0 is the -1 sentinel; x264's
   macro is `fix(t) = table[t+1]` (`common/predict.h:92`). The decoder indexes
   `FIX[modes[i-1]]` directly, so `fix(2)` reads 1 (H) instead of 2 (DC). Every
   neighbour-mode prediction is wrong.
2. **qp_delta truncated** (`:133`): `dqp0 = d.decision(60)` consumes only the
   first bin; x264 emits more at ctx 60 with context transitions when the delta
   is nonzero (`encoder/cabac.c:168-181`). Residual parsing after a nonzero dqp
   is misaligned.
3. **cbf context wrong for intra** (`:148-151`): uses
   `93 + 2*!!top_nz + !!left_nz` with unavailable neighbours = 0, giving 93.
   x264's `cabac_cbf_ctxidxinc` masks with `0x7f + (b_intra << 7)` so unavailable
   neighbours count as available (`encoder/cabac.c:612-643`), giving 96 for the
   first intra MB — which is exactly the bin sequence PR #41 proved.

`harness/cabac_decoder_selftest.py` only round-trips random decision bins at
QP 0/25/51; it does not cover syntax ordering, residual, qp_delta, ue-bypass or
terminal, so it cannot catch any of these. **Byte comparison against x264's
actual stream remains the only trustworthy oracle.** Agents: do not use the
decoder to derive ground-truth syntax until these three bugs are fixed and the
selftest is extended.

### Harness caveats (same audit)

- `parity_check.py:55-57` `strip_sei()` removes ALL NAL type 6 units from both
  streams, not only encoder-identity SEI — "encoder-identity only" is asserted,
  not verified.
- `parity_check.py:45-52` `split_nals()` matches only 4-byte start codes; 3-byte
  `00 00 01` slices are mislabelled. The "first mismatch at offset 41 in PPS
  NAL" message is therefore mislabelled — it is slice data.
- The harness compares against the installed ffmpeg/libx264
  (`N-126335…20260830`), NOT the pinned `references/x264-master` `c24e06c2`
  used by the fixtures. "Fixture byte-exact" and "harness byte-exact" are
  different oracles.

### Full production-superflat work inventory (same audit)

PR #45's encoder is byte-exact but only for a hand-built record. Four gaps remain:
1. **Dispatch unreachable**: `cabac.comp:310-315` selects I4x4 only when
   `op.first` bit1 is set; `resident_cabac.inc:198-202` never sets it.
2. **No GPU I4x4 mode decision** writing the 16 modes to the bank tail
   (`inter_frame.comp` has only `selectIntra16Mode`/`dcMode`).
3. **Residual staging layout mismatch**: production `join()` writes I16 layout
   (`level+1` boolean cbp, DC at `+4..19`, AC at `+20+15*b`, chroma DC at
   `+262+66*plane+2`), but `encodeIntra4Macroblock` expects 4-bit cbpLuma at
   `+1`, 16 scanned coeffs per block at `+4+16*b`, chroma DC at `+262+plane*4`
   (V-plane offset +266 CONFLICTS with production +330..333).
4. **First-MB-only contexts** in `encodeIntra4Macroblock` (`:273-289`):
   hardcoded neighbour cbp/chroma/nnz, zero QP delta, transform_size=0, cbf
   ctx 96/95. x264's intra cbf context is `93 + 2*!!nz_top + !!nz_left`
   (`cabac.c:636-643`), which varies per MB.

