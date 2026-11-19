# Multi-reference support (issue #5) — design and sub-PR plan

Status (2026-11-19, Ram): **groundwork landed, full ref>=2 selection not implemented.**
This document is the plan of record for issue #5. It records the authoritative
x264 behaviour, the exact Vk264 code surface, and a dependency-ordered set of
sub-PRs. Issue #5 is labelled `blocked` because the resident CABAC path has no
per-MB reference-index syntax; that must land before references can be selected.

## 1. Authoritative x264 behaviour

From `references/x264-master/encoder/set.c` (pinned snapshot):

```c
// set.c:147-158
sps->vui.i_num_reorder_frames = param->i_bframe_pyramid ? 2 : param->i_bframe ? 1 : 0;
sps->vui.i_max_dec_frame_buffering =
sps->i_num_ref_frames = X264_MIN(X264_REF_MAX, X264_MAX4(param->i_frame_reference,
                        1 + sps->vui.i_num_reorder_frames,
                        param->i_bframe_pyramid ? 4 : 1, param->i_dpb_size));
sps->i_num_ref_frames -= param->i_bframe_pyramid == X264_B_PYRAMID_STRICT;
if (param->i_keyint_max == 1) {
    sps->i_num_ref_frames = 0;
    sps->vui.i_max_dec_frame_buffering = 0;
}
// set.c:488
pps->i_num_ref_idx_l0_default_active = param->i_frame_reference;
```

Consequences for the current resident path (no B-frames, `i_num_reorder_frames=0`):

| stream | `max_num_ref_frames` | PPS `num_ref_idx_l0_default_active` |
|---|---|---|
| gop=1 (intra only) | 0 | `ref` (x264 still writes it) |
| gop>1, ref=1 | 1 | 1 |
| gop>1, ref=2 (`faster`, `fast`) | 2 | 2 |

Measured with ffmpeg `libx264 -preset faster|fast -x264-params bframes=0`
(32x32, gop=12, qp=28): SPS `num_ref_frames=2`, PPS
`num_ref_idx_l0_default_active_minus1=1`, POC type 2 — matching the table.
Before this PR Vk264 emitted `num_ref_frames=1`, `...minus1=0`.

`cabac.c:241-259` shows the reference-index bin syntax that must be added:

```c
ctx = 0;
if (refa > 0) ctx++;
if (refb > 0) ctx += 2;
for (i_ref = ref; i_ref > 0; i_ref--) {
    x264_cabac_encode_decision(cb, 54 + ctx, 1);
    ctx = (ctx >> 2) + 4;
}
x264_cabac_encode_decision(cb, 54 + ctx, 0);
```

`cabac_ref_p` is invoked once per 16x16/16x8 partition, or four times for
8x8 partitions (`cabac.c:445-489`). When every MB uses ref 0, this is a single
`emitDecision(f, 54, 0)` per partition — small, but it must be present whenever
`num_ref_idx_l0_active > 1`, or the decoder desynchronises.

## 2. Vk264 code surface (exact locations)

All paths relative to `Vk264/vulkan/` unless noted.

### 2.1 SPS/PPS/slice header — `dct_parity.cpp`
- `encodeCabacClip()` starts at L5266.
- L5294-5301: **landed** — `referenceBudget` read from
  `presets::find(options->preset)->referenceBudget` (already transcribed in
  `x264_presets.hpp:26,62,66`; values 1/1/1/2/2/3/5/8/16/16).
- L5324: level `maxDecFrameBuffering` now follows the budget for gop>1
  (kept at 1 for gop=1 so the byte-exact intra-only cases do not move).
- L5338-5350: **landed** — `sps.i_poc_type = 2`,
  `sps.i_num_ref_frames = gop == 1 ? 0 : referenceBudget`.
- L5382: `vui.i_max_dec_frame_buffering` already mirrors `i_num_ref_frames`,
  so it follows automatically.
- L5399-5400: PPS `i_num_ref_idx_l0/l1_default_active = 1` — **deliberately
  unchanged**. Raising it to 2 without emitting `ref_idx` bins produces an
  undecodable stream. Must land together with sub-PR B (below).
- L5428: slice `sh.i_num_ref_idx_l0_active = 1`; no override, no ref-pic-list
  modification, no MMCO.
- `parameters.comp:167,194-195` already serialises arbitrary ref counts; the
  GPU serializer is covered by `verifyParameters` (`dct_parity.cpp:3557`, which
  randomises `num_ref_frames` up to 16 at L3587 and the PPS defaults at
  L3672-3673). The limitation is purely the hard-coded *values* in
  `encodeCabacClip`, now partly fixed.

### 2.2 The reference ring — `dct_parity.cpp`
- L5443-5448: host DPB is two slots: `storedPictures = clip ? 2 : pictures`,
  `storageIndex = picture % 2`. This feeds only the staged/fixture path.
- L5544: `referencePicture = storageIndex(picture-1)` — the only reference.
- L5561: staged ME gets exactly one `reconstructed[referencePicture]`.
- The **resident** path (L5889-5903) does not read those host buffers as
  reference input: motion reads the previous frame's reconstruction from the
  persistent arena `referenceEnd` region written by the preceding call
  (`inter_frame.comp:34-64`, `dct_parity.cpp:4923-4935,5135-5138`). It is a
  single-frame GPU ping-pong, not an N-slot DPB.

### 2.3 Motion / CABAC shaders
- `motion_search.comp:54` declares `uint reference[4][planeSize]` — the `4` is
  the four halfpel planes of **one** reference, not four frames. All
  `motion_frame*.spv` variants share this single-reference ABI.
- `reference_frame.comp` takes one source frame, writes one set of 4 planes.
- `motion_bidir.comp` exists but is fixture-only (parity-roadmap.md:117).
- `cabac_syntax.glsl:241-287` (`encodeInter16Macroblock`) states "P_L0 16x16
  with one active reference" and hardcodes `refs = ivec4(0,0,0,0)` in
  `interMotionDelta` (L232-238). There is no `ref_idx` operation kind in
  `cabacCorpus::Kind` (`dct_parity.cpp:2058-2062`).

### 2.4 Reusable legacy infrastructure (different ABI)
The legacy CAVLC backend already has a working N-slot ring:
- `Vk264/shaders/layout.glsl:68-76` — `availableReferences()`,
  `referenceOffset(index)` ring algebra; `referenceIndex(mb)` reads a per-MB
  ref index from `MotionMatch.mode`.
- `Vk264/shaders/older_references.comp:47-87` — selects older refs, scales MVs
  by `(ref+1)`, writes the index into the match record.
- `Vk264/src/gpu.cpp:379-399` — allocates `refs+1` slots and dispatches the
  older-reference kernel.
These cannot be reused directly: the resident shaders use a different SSBO
ABI, so the ring algebra and selection heuristic must be re-expressed in
`Vk264/vulkan/`.

## 3. Sub-PR plan (dependency ordered)

### Sub-PR A — SPS/PPS groundwork (this PR, partially complete)
- [x] Consume `referenceBudget` for SPS `max_num_ref_frames` and level DPB
      (gop>1), with evidence from x264 `bframes=0` streams.
- [ ] Decide PPS default-active refs: keep 1 until sub-PR B lands; do not
      raise it while `ref_idx` bins are absent.
- [ ] Once B-frames land, replace the simplified formula with the full
      `set.c:147-158` expression (reorder frames + b-pyramid DPB slots).
- Verification: `ctest` 46/46; `parity_check --preset ultrafast/fast --gop 1`
      stays byte-exact; SPS field dump for `faster`/`fast` gop>1 equals 2.

### Sub-PR B — CABAC `ref_idx` syntax (prerequisite for real selection)
- Add a `ref_idx` operation / emit `emitDecision(54+ctx, ...)` in
  `cabac_syntax.glsl` for P16x16/16x8/8x8 partitions, with left/top ref
  tracking in the per-MB bank.
- Keep the chosen ref 0 for now; this makes streams with PPS l0=2 decodable
  and moves P-slice bins closer to x264.
- New fixture `verifyCabacRefIdx` comparing against `cabac_ref_p` from
  `upstream_cabac.inc` for a range of left/top ref combinations.
- Only after this lands, set PPS l0 default-active to the ref budget.

### Sub-PR C — N-slot reconstruction ring in the resident path
- Replace the single `referenceEnd` arena window with `refs+1` slots; add a
  per-frame slot selector in the push constants (borrow `layout.glsl` algebra).
- Extend `reference_frame.comp` / `motion_search.comp` ABI to take N slots and
  produce N candidate motion results.
- Host DPB `storedPictures = refs+1`, `storageIndex = picture % (refs+1)`;
  update L5544/L5561 to build a real reference list.

### Sub-PR D — per-MB reference selection
- Port the `older_references.comp` heuristic: for each MB, search older refs
  with MV scaled by `(ref+1)`, pick min cost, write the ref index into the MB
  bank; feed it to sub-PR B's `ref_idx` emitter and to MV prediction.
- Match x264's ref cost comparison (SATD + ref bias) before claiming parity.

### Sub-PR E — parity and preset gates
- `harness/parity_check.py` gains an explicit `--ref` override (today the ref
  count is only implicit in the preset) so targeted ref=2 P-frame cases can be
  compared with `-x264-params bframes=0:ref=2`.
- Acceptance criteria from issue #5: deepest selected ref logged, SPS/PPS
  counts match, byte-exact simple P-frame case, no single-ref regression.

## 4. Risks and honest limitations

- Raising PPS active refs before sub-PR B breaks decoding — do not do it.
- B-frame DPB slots (`i_bframe_pyramid ? 4 : 1`) mean the full x264 formula
  gives `num_ref_frames=4` for `faster` with default b-frames, **not** 2. The
  value 2 landed here is correct only for the current no-B-frame resident
  path; this is the main reason gop>1 SPS still differs from default x264.
- Changing `levelIn.maxDecFrameBuffering` may change `level_idc` at high
  resolution; gop=1 is intentionally pinned to 1 to preserve byte-exact cases.
- No claim of byte-exact ref>=2 parity is made in this PR.
