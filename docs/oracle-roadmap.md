# C oracle → libx264 byte-exact roadmap (2026-11-22)

Companion to `HYPOTHESIS.md` and `tests/video_experiments.json`.
The oracle is the C reference port under `Vk264-rewrite/src/ref/`. It is the
ground truth for the GPU shaders; it is only useful if it is byte-exact
against libx264 for **multi-frame video**, not just isolated 2-frame fixtures.

## Verified status

| surface | status | evidence |
|---|---|---|
| CAVLC I-frame 64x64 sweeps | EXACT 72/72 | `cavlc_iframe_sweep.py` |
| CAVLC P 2-frame sweeps | EXACT 1360/1360 | `cavlc_parity_sweep.py` |
| CABAC P 2-frame sweeps | EXACT 1360/1360 | `cabac_parity_sweep.py` |
| 8-frame video, warzone, ultrafast CAVLC | EXACT | exp001/002 |
| 8-frame video, warzone, ultrafast CABAC | EXACT | (CABAC flush frame-index fix) |
| 8-frame video, forza, ultrafast | **FAIL** frame 2 | exp003..006 |
| any preset above ultrafast | **FAIL** SPS | exp007/008 |

## Phases

### Phase O1 — chained P-slice ME parity (BLOCKING)
- Symptom: first divergence at frame 2, MB 9 (forza): x264 MV -64,40 vs
  oracle -48,52; CBP/MVD diverge afterwards.
- Work: instrument `vk_me16_dia()` candidate list + SAD costs; compare with
  x264 `mb_analyse_inter_p16x16()`. Test H1 (border clamp), H2 (intra
  neighbour predictor cache), H3 (`mv_limit_fpel`) from `HYPOTHESIS.md`.
- Acceptance: exp003/004 and exp005/006 become BYTE-EXACT, 2-frame sweeps
  stay 1360/1360.

### Phase O2 — preset parameter mirroring
- SPS/PPS fields: `i_log2_max_frame_num`, `b_deblocking_filter_control`,
  `i_num_ref_frames`, High-profile `chroma_format_idc`/8x8 flags, VUI.
- Acceptance: NAL 0/1 match x264 for each preset before slice data is compared.

### Phase O3 — analysis features per preset
- 8x8dct, deblocking, subpel (DIA/HEX/UMH/TESA), multi-ref, trellis, AQ,
  lookahead/mb-tree, B-frames (see `docs/parity-roadmap.md` phases 1-9).
- Acceptance: video-level byte-exact sweep at each accepted preset.

### Phase O4 — full-resolution video
- Current limitation: oracle P-frame path crashes at ≥1280px width
  (access violation in the C reference). Needs a bounds audit of motion/
  residual helpers before 1080p video experiments can be produced.
- Acceptance: 1920x1072 x N-frame encode produces exact streams.

## Artefacts / commands

```
python build/make_video_experiments.py        # writes tests/expNNN_*.mp4
python build/verify_video_presets.py --clip <yuv> ... --entropy both
ffprobe -show_entries stream=codec_name,profile,width,height ...
```
