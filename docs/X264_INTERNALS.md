# x264 internals — the exhaustive encoder walkthrough for a GPU-shader port

Audience: the Phoenix encoder team. Purpose: explain **how x264 actually works**,
stage by stage, with the exact functions and source locations that define each
behaviour, so that a GPU shader encoder can be designed against real semantics
rather than folklore.

Authority and convention
------------------------

* The authoritative implementation is `reference/x264/`. Every `reference/x264/...:N`
  citation in this document was checked to exist in this checkout. Citations are
  `file:line` (or `file:first-last`) and refer to the file as it exists on branch
  `vk264-phoenix`.
* The Phoenix port lives in `src/phx_*.c/.h`. `src/...:N` citations describe what
  the port actually does **today**; they are not claims of x264 parity.
* "Not ported" is stated explicitly wherever the x264 stage has no Phoenix
  counterpart. Do not infer behaviour from a function name in `src/` — several
  ported functions intentionally cover only a subset of the x264 path (the
  header comments often document the gap, e.g. `src/phx_lookahead.h:27-31`).

Reading order
-------------

1. §1 — the call graph and frame pipeline (the "what runs when").
2. §2 — the state that the pipeline mutates (bitstream-determinism view).
3. §3–§5 — the per-frame decisions and the per-MB pixel pipeline.
4. §6–§8 — entropy, rate control, reconstruction.
5. §9 — determinism, serial-only stages and the shader boundary.
6. §10 — coverage check.

---

## 0. Scope, build-time axes, and the Phoenix status at a glance

### 0.1 The axes that change the bitstream

Before reading any function, fix these five axes. x264 is a family of encoders,
not one algorithm; nearly every branch below is gated by one of them.

| axis | where it is fixed | effect |
|---|---|---|
| entropy: CAVLC vs CABAC | `h->param.b_cabac` → `pps.b_cabac` (`reference/x264/encoder/set.c:483`), chosen in `x264_encoder_open` (`reference/x264/encoder/encoder.c:1668-1671`) | two completely different syntax writers (`encoder/cavlc.c`, `encoder/cabac.c`); RD cost models differ (`encoder/rdo.c:44-69`) |
| chroma format / bit depth | `x264_sps_init` (`reference/x264/encoder/set.c:103-125`), `BIT_DEPTH`, `CHROMA_FORMAT` macros used throughout | plane count, chroma QP table offset, SA8D vs SATD, deblock chroma edges |
| slice type I/P/B | `x264_slicetype_decide` (`reference/x264/encoder/slicetype.c:1745`) | analysis dispatch at `reference/x264/encoder/analyse.c:2934/2953/3305` |
| RC method CQP / CRF / ABR / 2-pass | `x264_ratecontrol_new` (`reference/x264/encoder/ratecontrol.c:744`), `x264_ratecontrol_start` (`:1432`) | QP source at `:1506-1520`, complexity estimation at `:2400` |
| threading | `h->i_thread_frames`, `b_sliced_threads`, `i_lookahead_threads` (`reference/x264/common/common.h:277-288`, `encoder.c:3337-3346`) | frame/slice parallelism, bitstream backup/restore (`encoder.c:2689-2751`) |

Phoenix selects a much smaller point in this space: 4:2:0, 8-bit, one slice per
frame, one thread, either CAVLC or CABAC, CQP unless explicitly configured
(`src/phx_encoder.h:65-147`, `src/phx_encoder.c:1277-1572`). Interlacing, MBAFF,
4:4:4, 4:2:2, high bit depth, multiple slices, frame threads and lookahead
threads are **not ported**.

### 0.2 Phoenix coverage summary (honest status)

| x264 stage | reference | Phoenix | gap |
|---|---|---|---|
| param defaults / presets | `common/base.c:344`, `:489` | `src/phx_param.c:25`, `:65` | subset of fields |
| SPS/PPS init and write | `encoder/set.c:101`, `:305`, `:479`, `:505` | `src/phx_headers.c:13`, `:76`, `:98`, `:165` | scaling lists/VUI subset |
| SEI write | `encoder/set.c:80`, `:574`, `:593` | `src/phx_sei.c:322`, `:340` | version SEI ported; x264 emits many more SEI types at `encoder.c:3693-3808` |
| lowres planes | `common/mc.c:458`, `:484` | `src/phx_frame.c:81`, `:107` | same `FILTER` recurrence |
| lookahead / scenecut / B-adapt FAST / BREF | `encoder/slicetype.c:1384`, `:1430`, `:1473`, `:1745` | `src/phx_lookahead.c:89-504` | MB-tree, trellis B-adapt, VBV lookahead, weighted prediction not ported (`src/phx_lookahead.h:27-31`) |
| cost tables / lambda | `encoder/analyse.c:143`, `:179`, `:248`, `:255` | `src/phx_cost.c:23`, `:65`, `:102`, `:135` | CQM-specific tables absent |
| ME fullpel DIA/HEX/UMH | `encoder/me.c:182-798` | `src/phx_me.c:160`, `src/phx_lowres.c:437` | 16x16 only in `phx_me16`; subpel not ported (`src/phx_analysis.h:19-20`) |
| intra I16/I4/chroma analysis | `encoder/analyse.c:668`, `:586` | `src/phx_analysis.c:693`, `:714`, `:750`, `:1007` | I8 RD path partial |
| inter analysis P/B | `encoder/analyse.c:1255-2692` | `src/phx_analysis.c:1056` | only P L0 16x16 trial; no sub-8x8, no B modes |
| MB encode I16/I4/chroma | `encoder/macroblock.c:126`, `macroblock.h:132`, `:259` | `src/phx_mbencode.c:178`, `:274`, `src/phx_chroma.c:207` | I8 encoder absent (note at `src/phx_encoder.c:1422-1424`) |
| deadzone quant / DCT / IDCT | `common/quant.c:50-103`, `common/dct.c:47-326` | `src/phx_quant.c:137-223`, `src/phx_dct.c:30-326` | ported closely |
| trellis QNS CABAC + CAVLC | `encoder/rdo.c:642`, `:921` | `src/phx_rdo.c:836`, `:979` | ported closely; psy-trellis partial |
| CABAC engine + syntax | `common/cabac.c:32-183`, `encoder/cabac.c:35-1213` | `src/phx_cabac.c:54-209`, `src/phx_cabac_syntax.c:184-864` | no field/MBAFF syntax |
| CAVLC engine + syntax | `encoder/cavlc.c:70-704` | `src/phx_cavlc.c:951-1114`, `src/phx_cavlc_syntax.c:69-411` | no field/MBAFF syntax |
| Exp-Golomb, NAL escape | `common/bitstream.h:201-299`, `common/bitstream.c:29`, `:55` | `src/phx_bitstream.c:52-140` | ported |
| rate control CQP/CRF/ABR + AQ | `encoder/ratecontrol.c:1432`, `:1590`, `:1829`, `:304` | `src/phx_ratecontrol.c:327`, `:448`, `:529`, `:626` | no 2-pass, no VBV/row RC, no MB-tree |
| deblock | `common/deblock.c:277-687` | `src/phx_deblock.c:352-689` | ported for progressive |
| ref lists, POC, frame_num | `encoder.c:2295`, `:2413`, `:2533`, `:3512` | `src/phx_encoder.c:491-698`, `:698-785` | simplified DPB of last reference |
| slice header | `encoder.c:122-364` | `src/phx_slice.c:14`, `:107` | subset (no MBAFF/MMCO/weight table) |

---

## 1. Top-level pipeline — the exact call order

### 1.1 API entry points

The public API is a thin dispatch layer: `x264_encoder_open` (`reference/x264/encoder/api.c:76`),
`x264_encoder_close` (`:130`), `x264_nal_encode` (`:138`), `x264_encoder_reconfig`
(`:145`), `x264_encoder_parameters` (`:152`), `x264_encoder_headers` (`:159`),
`x264_encoder_encode` (`:166`), `x264_encoder_delayed_frames` (`:173`),
`x264_encoder_maximum_delayed_frames` (`:180`), `x264_encoder_intra_refresh`
(`:187`), `x264_encoder_invalidate_reference` (`:194`). The real work is in
`encoder/encoder.c`.

Phoenix mirrors these as `phx_encoder_open`, `phx_headers`, `phx_encode`,
`phx_encoder_close` (`src/phx_encoder.h:154-245`) with short aliases
`phx_open/phx_encode/phx_close` (`src/phx_encoder.c:241`, `:1592`).

### 1.2 `x264_encoder_open` (`reference/x264/encoder/encoder.c:1507-1859`)

Ordered operations, each a separate step with bitstream consequences:

1. Allocate and zero `x264_t`, copy the parameter struct (`1513-1518`); deep-copy
   string options (`1520-1531`).
2. `validate_parameters(h, 1)` (`1552`; implementation at `:423`) — if it changes
   a parameter, that changed value is what all later stages see.
3. Parse CQM file if requested (`1555-1557`), reduce fps/timebase (`1559-1560`).
4. Initialise counters: `i_frame = -1`, `i_frame_num = 0`, `i_idr_pic_id`
   (`1563-1569`).
5. `set_aspect_ratio` (`1577`; implementation `:1468`), then
   `x264_sps_init(h->sps, ...)`, `x264_sps_init_scaling_list`,
   `x264_pps_init` (`1579-1581`), `x264_validate_levels` (`1583`).
6. `h->chroma_qp_table = i_chroma_qp_table + 12 + i_chroma_qp_index_offset`
   (`1585`) — this pointer is the single source of luma→chroma QP mapping; see
   the table at `reference/x264/common/macroblock.h:249-270`.
7. `x264_cqm_init(h)` (`1587`; implementation `common/set.c:73-275`) builds the
   quant/dequant/unquant/bias tables. It can **raise `i_qp_min`** and **lower
   `i_qp_max`** (`common/set.c:253-270`), which changes later QP decisions.
8. MB geometry `i_mb_width/height/count` and chroma shifts (`1590-1595`).
9. Frame/RC delay and lowres decisions (`1602-1626`): `i_delay` is
   `4*max(bframes,3)` for B-adapt trellis (`1603-1606`), extended by MB-tree/VBV
   lookahead, thread frames, sync lookahead and VFR (`1607-1613`).
   `b_have_lowres` is enabled for ABR/CRF/B-adapt/scenecut/MB-tree/weightp
   (`1618-1625`) — this is the gate on `x264_frame_init_lowres` at encode time.
10. Frame pools and `x264_rdo_init()` (`1634-1646`; RDO table build at
    `encoder/rdo.c:384-415`).
11. DSP dispatch (`1648-1671`): predict/pixel/dct/zigzag/mc/quant/deblock/
    bitstream function pointers, then CABAC or CAVLC init.
12. `x264_analyse_init_costs(h)` (`1708`) builds MV/ref/mode cost tables.
13. Bitstream buffers (`1723-1733`), thread contexts (`1756-1795`),
    `x264_lookahead_init(h, i_slicetype_length)` (`1802`),
    `x264_macroblock_thread_allocate` (`1805-1807`), `x264_ratecontrol_new`
    (`1809`).

Phoenix's `phx_encoder_open` (`src/phx_encoder.c:160-240`) performs the subset:
validate (`src/phx_encoder.c:36-65`), SPS/PPS, MB geometry, cost tables, CABAC
contexts and the serial lookahead allocation. It deliberately keeps `rc == NULL`
unless `phx_encoder_configure_ratecontrol()` was called
(`src/phx_encoder.c:315-333`), so the default CQP stream is unchanged.

### 1.3 `x264_encoder_headers` (`reference/x264/encoder/encoder.c:2096-2134`)

```
bs_init
nal_start(NAL_SPS, HIGHEST) -> x264_sps_write -> nal_end       (:2106-2109)
nal_start(NAL_PPS, HIGHEST) -> x264_pps_write -> nal_end       (:2112-2115)
nal_start(NAL_SEI, DISPOSABLE) -> x264_sei_version_write -> nal_end (:2118-2122)
encoder_encapsulate_nals(h, 0)                                 (:2124)
*pi_nal = h->out.i_nal; *pp_nal = &h->out.nal[0]               (:2129-2131)
```

`nal_start` (`:1981-1992`) records `i_ref_idc`, `i_type`, the payload pointer at
the current bitstream position and resets padding; `nal_end` (`:2010-2023`)
computes `i_payload`, writes 64 bytes of `0xff` guard for escaping assembly,
calls the optional `nalu_process` hook and increments `i_nal`.

### 1.4 `x264_encoder_encode` (`reference/x264/encoder/encoder.c:3323-3902`)

The main per-call path. Stage numbers below are the ones to preserve in a shader
pipeline.

| # | stage | source lines |
|---:|---|---|
| 0 | frame-thread rotation + rate-control sync | `encoder.c:3337-3351` |
| 1 | `*pi_nal = 0; *pp_nal = NULL` | `3354-3356` |
| 2 | input picture: `x264_frame_pop_unused`, `x264_frame_copy_picture`, mod16 border expand, `i_frame` bookkeeping | `3359-3393`; frame copy at `common/frame.c:363`, border at `common/frame.c:640` |
| 3 | AQ or MB-tree read on the *input* frame, before lookahead | `3411-3420`; `x264_adaptive_quant_frame` at `encoder/ratecontrol.c:304`, `x264_macroblock_tree_read` at `:541` |
| 4 | `x264_frame_init_lowres` (if `b_have_lowres`) | `3422-3423`; `common/mc.c:458` |
| 5 | `x264_lookahead_put_frame` | `3426`; `encoder/lookahead.c:192` |
| 6 | delay gate — return 0 NALs until `i_input <= i_delay + 1 - i_thread_frames` fails | `3428-3433` |
| 7 | `x264_lookahead_get_frames` | `3445-3447`; `encoder/lookahead.c:223` |
| 8 | end-of-stream drain via `encoder_frame_end` | `3449-3450` |
| 9 | `h->fenc = x264_frame_shift(h->frames.current)`; wait for sliced threads; apply reconfig | `3454-3477` |
| 10 | `x264_ratecontrol_zone_init` | `3478`; impl `:1422` |
| 11 | `reference_update` (DPB insert, MMCO) | `3481`; impl `:2533` |
| 12 | forced IDR if no valid references left; keyframe counters; **POC assignment** `i_poc = 2*(i_frame - max(last_idr,0))` | `3485-3513` |
| 13 | NAL type / `nal_ref_idc` / slice type from frame type; `reference_reset` on IDR, `reference_hierarchy_reset` on I/P/BREF | `3517-3555`; helpers `:2563`, `:2571` |
| 14 | `fdec` metadata, DTS/PTS reorder delay | `3557-3579` |
| 15 | `reference_build_list` builds L0/L1 | `3583`; impl `:2295` |
| 16 | `bs_init`, `h->out.i_nal = 0` | `3587-3599` |
| 17 | optional AUD NAL | `3601-3621` |
| 18 | periodic intra refresh position update | `3626-3660` |
| 19 | on keyframe: repeated SPS/PPS, buffering-period SEI | `3662-3703` |
| 20 | extra user SEI, version SEI, recovery point, mastering display, CLL, alternative transfer, frame packing, pic timing, dec-ref-marking | `3705-3808` |
| 21 | AVC-Intra filler/UMID/VANC | `3813-3853` |
| 22 | **rate control start**, `i_global_qp = x264_ratecontrol_qp`, `pic_out->i_qpplus1 = qp+1` | `3855-3861` |
| 23 | optimal reference list (2-pass stats), `i_poc_l0ref0` | `3863-3870` |
| 24 | `slice_init(h, i_nal_type, i_global_qp)` | `3873`; impl `:2608` |
| 25 | B-direct init and weighted prediction init | `3876-3879` |
| 26 | `i_frame_num++` for reference pictures only | `3881-3882` |
| 27 | `slices_write` (frame threads / slice threads / serial) | `3884-3899`; impl `:3159`, `:3219`, `:2752` |
| 28 | `encoder_frame_end`: encapsulate NALs, fill `pic_out`, `x264_ratecontrol_end`, filler NALs, stats, duplicate cleanup | `3901`; impl `:3904-4180` |

Note the ordering constraint that matters most for a port: **rate control starts
before the slice header is built** (`3857` before `3873`), and the slice header
QP comes from `x264_ratecontrol_mb_qp` inside `slice_write` (`:2787`), not from
the frame QP directly. AQ offsets must already exist on `fenc` at step 3.

Phoenix follows the same shape in `phx_encode` (`src/phx_encoder.c:1277-1572`):
picture copy `1287-1289`, AQ `1294-1295`, put/lookahead `1297`, delay gate
`1304-1311`, drain `1314-1322`, frame context `1327-1328`, ratecontrol_start
`1338-1346`, reference selection `1355-1359`, `phx_slice_header_init`/`_write`
`1365-1369`, CABAC init `1370-1376`, MB loop `1395-1539`, NAL `1542-1557`,
ratecontrol_end `1563-1564`.

### 1.5 `slice_init` and `slice_write`

`slice_init` (`reference/x264/encoder/encoder.c:2608-2675`) calls
`slice_header_init` (`:122-211`), toggles `i_idr_pic_id` (`2615-2633`), derives
the active reference counts and `b_num_ref_idx_override` (`2639-2645`), copies
`sh.i_qp`/`POC` into `fdec` (`2654-2667`) and calls
`x264_macroblock_slice_init` (`:2674`; impl `common/macroblock.c:423`).

`slice_write` (`reference/x264/encoder/encoder.c:2752-3130`) is the hot loop:

```
bs_realign; nal_start(i_nal_type, i_nal_ref_idc)             (:2776-2780)
x264_macroblock_thread_init(h)                              (:2783)
sh.i_qp = SPEC_QP(x264_ratecontrol_mb_qp(h));               (:2787-2788)
sh.i_qp_delta = sh.i_qp - pps->i_pic_init_qp;                (:2789)
slice_header_write(bs, &sh, nal_ref_idc)                     (:2791)
if CABAC: bs_align_1; cabac_context_init; cabac_encode_init  (:2792-2800)
i_last_qp = sh.i_qp; i_last_dqp = 0; field_decoding_flag=0   (:2804-2806)
for each MB in raster order:                                (:2812-3074)
    at x==0: bitstream_check_buffer; VBV row backup;
              fdec_filter_row(h, mb_y, 0)                   (:2817-2825)
    interlaced MBAFF decisions                              (:2839-2853)
    x264_macroblock_cache_load_progressive/interlaced       (:2855-2859)
    x264_macroblock_analyse                                 (:2861)
reencode:
    x264_macroblock_encode                                  (:2865)
    CABAC: terminal (except first MB), mb_skip or write_cabac (:2867-2880)
    CAVLC: skip run, write_cavlc, overflow retry           (:2881-2904)
    mb_size = bits consumed since mb_spos                  (:2906-2907)
    slice-max-size checks / rollback / new slice           (:2909-2967)
    x264_macroblock_cache_save                             (:2972)
    x264_ratecontrol_mb(h, mb_size) -> row-QP reencode     (:2974-2983)
    stats, x264_macroblock_deblock_strength                (:2986-3057)
CABAC: cabac_encode_flush, bs.p = cabac.p                  (:3080-3084)
CAVLC: final skip run, bs_rbsp_trailing, bs_flush           (:3085-3092)
nal_end; fdec_filter_row(end); misc bits                   (:3093-3102)
```

The `reencode:` label is real: CAVLC overflow (`:2894-2902`), slice-max-size
(`:2923-2966`) and VBV row failure (`:2974-2983`) all jump back to
`x264_macroblock_encode` after restoring a bitstream snapshot. A shader design
that discards rollback cannot reproduce these paths.

`bitstream_backup`/`bitstream_restore` (`encoder.c:2689-2751`) save `bs_t` for
CAVLC, the whole `x264_cabac_t` (or everything up to `f8_bits_encoded` for a
partial backup) plus `cabac.p[-1]`, and the frame stats / `i_last_qp` /
`i_last_dqp` / `field_decoding_flag`. The comment at `:2703-2705` explains why
contexts need not be backed up for a per-MB backup: a slice end discards them.

### 1.6 NAL assembly

`encoder_encapsulate_nals` (`reference/x264/encoder/encoder.c:2051-2091`) computes
the worst-case buffer as `previous + nal_size*3/2 + 4*i_nal + 4 + 64`
(`:2072`), reallocates via `check_encapsulated_buffer` (`:2025-2049`), then calls
`x264_nal_encode` per NAL (`:2084`). `b_long_startcode` is true for the first NAL,
SPS, PPS and all AVC-Intra NALs (`:2082-2083`).

`x264_nal_encode` (`reference/x264/common/bitstream.c:55-104`) writes 4-byte or
3-byte Annex-B start code (`:61-68`), the NAL header byte
`(ref_idc << 5) | type` (`:73`), then `nal_escape`, then optionally AVCHD padding
and an MP4 size header (`:79-99`). `nal_escape_c` (`:29-40`) inserts `0x03`
before any `00 00 <=03` sequence; the loop copies the first two bytes
unconditionally, which is why `nal_end` writes the 64-byte `0xff` guard.

`encoder_frame_end` (`:3904-4180`) additionally inserts the buffering-period SEI
at the correct position (`:3924-3941`), encapsulates (`:3943`), fills `pic_out`
(`:3947-3970`), runs `x264_ratecontrol_end` (`:3978`) and emits filler NALs to
spend the returned `filler` bits (`:4011-4034`).

Phoenix equivalent: `phx_nal_encode` (`src/phx_bitstream.c:125`) with
`phx_nal_escape` (`:112`); `phx_encode` performs one NAL per call
(`src/phx_encoder.c:1542-1557`). Filler NALs, AUD, multiple slices and `nalu_process`
are not ported.

---

## 2. Data structures — a bitstream-determinism view

### 2.1 `x264_t` (`reference/x264/common/common.h:270-763`)

The encoder handle. Fields that determine output bytes:

| group | fields / lines | why it matters |
|---|---|---|
| parameters | `param`, `api` (`:273-275`) | private copy made at `encoder.c:1516` |
| threads | `thread[]`, `lookahead_thread[]`, `i_thread_phase`, slice bounds (`:277-288`) | frame/slice parallelism; synchronisation boundary at `encoder.c:307-320` |
| bitstream out | `out.i_nal`, `out.nal`, `out.p_bitstream`, `out.bs` (`:291-299`) | one shared RBSP buffer for all NALs of a frame |
| counters | `i_frame`, `i_frame_num`, `i_nal_type`, `i_nal_ref_idc` (`:309-316`) | IDR toggles `i_idr_pic_id` (`encoder.c:2615-2633`); CABAC flush bit uses `i_frame` (`common/cabac.c:174`) |
| quant matrices | `dequant4_mf/dequant8_mf`, `unquant4_mf/unquant8_mf`, `quant4_mf/quant8_mf`, `quant4_bias/quant8_bias` (`:335-348`) | built by `x264_cqm_init`; deadzone quant and trellis read them directly |
| cost tables | `cost_mv[]`, `cost_mv_fpel[][]`, `cost_table->ref[][3][33]`, `cost_table->i4x4_mode` (`:350-357`) | analyse cost = distortion + λ·bits from these |
| chroma QP | `chroma_qp_table` pointer (`:359`) | `h->chroma_qp_table[SPEC_QP(qp)]` at `analyse.c:257`, `:291` |
| SPS/PPS/slice header | `sps[1]`, `pps[1]`, `sh`, `sh_backup` (`:361-370`) | header syntax source |
| CABAC ctx | `cabac` (`:373`) | the arithmetic coder's entire live state |
| frames/queues | `frames.current/unused/blank_unused/reference`, `i_last_keyframe/i_last_idr`, `fenc`, `fdec`, `fref[2][]`, `i_ref[2]` (`:375-420`) | DPB state, reference lists, POC |
| HB DCT scratch | `dct.luma16x16_dc`, `chroma_dc`, `luma8x8[12][64]`, `luma4x4[16*3][16]` (`:427-435`) | the quantized coefficient buffers the syntax writers emit |
| MB geometry/neighbours | `mb.i_mb_*`, `i_neighbour`, `i_neighbour8/4`, `i_neighbour_intra`, `i_neighbour_frame`, top/left/topright/topleft indices (`:440-513`) | availability rules drive prediction and CABAC contexts |
| MB tables | `type`, `partition`, `qp`, `cbp`, `intra4x4_pred_mode`, `non_zero_count`, `chroma_pred_mode`, `mv[2]`, `mvd[2]`, `ref[2]`, `skipbp`, `mb_transform_size`, `slice_table`, `field` (`:519-536`) | reconstructed per-MB state consumed by neighbours and deblocking |
| MB current | `i_type`, `i_partition`, `i_sub_partition[4]`, `b_transform_8x8`, `i_cbp_luma/chroma`, pred modes (`:541-551`) | the decision being encoded |
| reconstruction scratch | `fenc_buf[48*16]`, `fdec_buf[54*32]`, intra backup buffers, DCT backups, `fenc_satd_cache`, `fenc_hadamard_cache` (`:567-589`) | in-place predict/transform/reconstruct working set |
| per-block cache | `mb.cache.*` (`:613-651`) | neighbour samples for the current MB |
| QP/lambda | `i_qp`, `i_chroma_qp`, `i_last_qp`, `i_last_dqp`, `b_variable_qp`, `i_trellis_lambda2`, `i_psy_rd_lambda`, `i_chroma_lambda2_offset` (`:654-666`) | RD and trellis costs |
| RC pointer | `rc` (`:682`) | see §2.5 |
| stats | `stat.frame`, cumulative arrays (`:684-718`) | RS non-video but affects 2-pass output |
| DSP pointers | `predict_*`, `pixf`, `mc`, `dctf`, `zigzagf`, `quantf`, `loopf`, `bsf` (`:739-756`) | CPU dispatch; a shader port must fix the C reference implementations |

Phoenix's `phx_t` (`src/phx_encoder.h:65-147`) exposes the same roles explicitly:
`param/sps/pps`, `nal[]`, `fenc`, `rc`, `mb_state`, `mb_cache`, `cost`, `analysis`,
`cabac`, `rbsp`, `recon`, counters and a small DPB mirror (`:139-146`).

### 2.2 `x264_frame_t` (`reference/x264/common/frame.h:37-183`)

Important fields for determinism:

* identity/timing: `i_poc`, `i_delta_poc[2]`, `i_type`, `i_frame`, `i_frame_num`,
  `b_keyframe`, `b_kept_as_ref` (`:41-64`) — POC is assigned in
  `encoder.c:3512-3513`.
* plane geometry and padding: `i_stride[3]`, `i_width[3]`, `i_lines[3]`,
  `plane[3]`, `lowres[4]`, `i_stride_lowres`, `integral`
  (`:71-85`). Border padding is `PADH/PADV = 32` (`:31-35`).
* lowres decision data: `lowres_mvs[2][B+1]`, `lowres_costs[B+2][B+2]`
  (`:103-110`), packed as `(lists_used << LOWRES_COST_SHIFT) + cost` with
  `LOWRES_COST_MASK = (1<<14)-1`, `SHIFT = 14` (`:111-112`) — written at
  `slicetype.c:790`.
* `i_cost_est[p0][p1]`, `i_cost_est_aq`, `i_satd`, `i_intra_mbs`, `i_row_satds`
  (`:123-130`) — the frame's SATD/SA8D costs; the cost for a B candidate is
  rescaled at `slicetype.c:983-987`.
* AQ/MB-tree: `f_qp_offset`, `f_qp_offset_aq`, `i_intra_cost`, `i_propagate_cost`,
  `i_inv_qscale_factor`, `b_scenecut` (`:132-140`).
* VBV: `i_planned_type[]`, `i_planned_satd[]`, `f_planned_cpb_duration[]`
  (`:146-151`).
* reordering: `i_reordered_pts`, `i_dts`, `i_pts`, `i_bframes`
  (`:46-65`) — set by `slicetype.c:1955-1958` and `encoder.c:3567-3577`.

`x264_frame_t` allocation lives in `common/frame.c` (`align_stride` `:30`,
`frame_internal_csp` `:45`, `x264_frame_copy_picture` `:363`,
`x264_frame_expand_border` `:556`, `frame_expand_border_lowres` `:627`).

Phoenix's `phx_frame_t` (`src/phx_frame.h:17-39`) is a lean version: it owns
planes, lowres planes and the cost/mv caches but **not** `i_type/i_frame/i_bframes`;
the lookahead keeps parallel metadata (`src/phx_lookahead.h:16-20`).

### 2.3 `x264_mb_analysis_t` (`reference/x264/encoder/analyse.c:56-121`)

This is the analysis scratch pad, allocated on the stack per MB at
`analyse.c:2920`. Groups:

| group | fields (lines) |
|---|---|
| λ/QP | `i_lambda`, `i_lambda2`, `i_qp`, `p_cost_mv`, `p_cost_ref[2]`, `i_mbrd` (`:58-64`) |
| intra shortcuts | `b_fast_intra`, `b_force_intra`, `b_avoid_topright`, `b_try_skip` (`:68-72`) |
| luma intra costs | `i_satd_i16x16`, `i_satd_i16x16_dir[7]`, `i_predict16x16`, `i_satd_i8x8`, `i_satd_i8x8_dir[4][16]`, `i_predict8x8[4]`, `i_satd_i4x4`, `i_predict4x4[16]`, `i_satd_pcm` (`:74-87`) |
| chroma | `i_satd_chroma`, `i_satd_chroma_dir[7]`, `i_predict8x8chroma` (`:89-92`) |
| inter | `x264_mb_analysis_list_t l0`, `l1` (`:94-96`), defined at `:35-54` with `me16x16`, `bi16x16`, `me8x8[4]`, `me4x4[4][4]`, `me8x4[4][2]`, `me4x8[4][2]`, `me16x8[2]`, `me8x16[2]`, per-partition cost arrays and the `mvc[32][6][2]` candidate store |
| B modes | `i_cost16x16bi`, `i_cost16x16direct`, `i_cost8x8bi`, `i_cost8x8direct[4]`, `i_satd8x8[3][4]`, `i_cost_est16x8/8x16`, `i_rd16x16bi/direct`, `i_rd8x8bi`, `i_mb_partition16x8/8x16`, `b_direct_available` (`:98-119`) |

`x264_me_t` itself is at `reference/x264/encoder/me.h:33-56`: inputs `i_pixel`,
`p_cost_mv`, `i_ref_cost`, `i_ref`, `weight`, `p_fref[12]`, `p_fref_w`, `p_fenc[3]`,
`i_stride[3]`, `mvp[2]`; outputs `cost_mv`, `cost`, `mv[2]`.

Phoenix's `phx_mb_decision_t` (`src/phx_mb_decision.h:14-40`) is the equivalent
A/B boundary: it carries `mb_type`, partition, QP, CBP, prediction modes,
`mv[2][16][2]`, `ref[2][16]`, `nnz[48]`, `dct[48][16]`, chroma DCT/nnz and a skip
flag. Its documented layout maps `nnz[k]` and `dct[scan8[k]]` to x264's scan8
cache (`src/phx_analysis.h:43-61`).

### 2.4 `x264_cabac_t` (`reference/x264/common/cabac.h:30-52`)

```
i_low, i_range          arithmetic coder state                (:33-34)
i_queue                 queued output bits, offset -8        (:37)
i_bytes_outstanding     carry propagation counter            (:38)
p_start, p, p_end       output buffer cursors                (:40-42)
f8_bits_encoded         fractional bit counter (RD only)     (:45)
state[1024]             RDO_SKIP_BS 460 or 1024 live ctx     (:48)
padding[12]             alignment                            (:51)
```

`x264_cabac_pos` (`:94-97`) = `(p - p_start + i_bytes_outstanding)*8 + i_queue`.
The size-only helpers `x264_cabac_size_decision{,2,_noup,_noup2}` (`:101-124`)
mutate a state array exactly as the real coder but only add
`x264_cabac_entropy[state^b]` bits; RD uses these via macro-redefinition of the
CABAC writer (`encoder/rdo.c:55-69`).

Phoenix mirrors the engine in `src/phx_cabac.c:54-209`, with a separate
size-only accumulator in `src/phx_rdo.c:340-401`.

### 2.5 `x264_ratecontrol_t` (`reference/x264/encoder/ratecontrol.c:72-179`)

| group | fields (lines) | role |
|---|---|---|
| constants | `b_abr`, `b_2pass`, `b_vbv`, `fps`, `bitrate`, `qcompress`, `nmb`, `qp_constant[3]` (`:75-84`) | mode selection |
| current frame | `rce`, `qpm`, `qpa_rc`, `qpa_aq`, `qp_novbv` (`:87-94`) | QP before/after AQ |
| VBV | `buffer_size`, `buffer_fill`, `buffer_rate`, `vbv_max_rate`, `pred[]`, `rate_factor_max_increment` (`:96-104`) | `vbv_pass1/2`, `update_vbv` |
| ABR | `last_satd`, `cplxr_sum`, `expected_bits_sum`, `wanted_bits_window`, `short_term_cplxsum/count`, `rate_factor_constant`, `ip_offset`, `pb_offset` (`:107-118`) | CRF/ABR QP |
| 2-pass | stats files, `entry`, `last_qscale_for[3]`, `lmin/lmax/lstep`, MB-tree qp buffers (`:121-154`) | 2-pass and MB-tree |
| MBRC | `frame_size_estimated`, `bits_so_far`, `frame_size_maximum`, `frame_size_planned`, `row_pred`, `pred_b_from_p`, `bframes`, `bframe_bits` (`:157-167`) | row-level VBV QP adaptation |
| zones/HRD | `zones`, `prev_zone`, CPB delays/times (`:169-178`) | zones, SEI timing |

Phoenix's `phx_ratecontrol_t` (`src/phx_ratecontrol.h`) implements CQP/CRF/ABR
plus AQ (`src/phx_ratecontrol.c:327-448`, `:529-642`) but no 2-pass, VBV row RC
or MB-tree.

### 2.6 `scan8` and the per-MB caches

`x264_scan8` is defined at `reference/x264/common/base.h:180-195`; the layout is
documented at `:157-175`:

```
row 0  DY  . y y y y       row 5  DU  . u u u u
row 1      . y Y Y Y       row 6      . u U U U
row 2      . y Y Y Y       ...
row 3      . y Y Y Y       row 10 DV  . v v v v
row 4      . y Y Y Y       rows 11-14 v/V blocks
```

`X264_SCAN8_LUMA_SIZE = 5*8`, `X264_SCAN8_SIZE = 3*40` (`base.h:153-154`),
`X264_SCAN8_0 = 4+1*8` (`:155`), `LUMA_DC = 48`, `CHROMA_DC = 49` (`:177-178`).
The luma 4x4 blocks occupy `scan8[0..15]`, chroma U `16..31`, V `32..47`, and
the three DC slots are the last three entries. Important consequences:

* `scan8[idx] - 1` and `scan8[idx] - 8` are the left and top neighbours
  (used for MV prediction `common/mvpred.c:131-141`, CABAC ref contexts
  `encoder/cabac.c:244-245`, CBP contexts `encoder/cabac.c:634-635`,
  deblock strength `common/deblock.c:286-288`).
* `STORE_8x8_NNZ` writes the same `nz` into four scan8 slots using two 16-bit
  stores (`encoder/macroblock.h:112-117`) — an 8x8 block's nnz is replicated
  to its four constituent 4x4 slots.
* `CLEAR_16x16_NNZ` zeroes four 32-bit groups of a plane's scan8 region
  (`:119-126`).
* `FOREACH_BIT` (`:128-130`) iterates set bits of a 4-bit CBP branchlessly;
  it is used by both CABAC and CAVLC residual writers
  (`encoder/cabac.c:1041/1048/1055`, `encoder/cavlc.c:287`).
* Zigzag tables are transposed relative to the spec (`common/macroblock.h:180`):
  `x264_zigzag_scan4` at `:181-187`, `x264_zigzag_scan8` at `:188-200`.

The cache arrays themselves are in `common/common.h:613-651`: `intra4x4_pred_mode`,
`non_zero_count` (0x80 = unavailable), `ref[2]` (-1 unused, -2 unavailable),
`mv[2]`, `mvd[2]`, `skip`, direct MV/ref, `i_neighbour_transform_size`,
`i_neighbour_skip`, `i_cbp_top/left`, and deblock strength. The per-frame MB
tables are at `common/common.h:519-536`; the `cbp` bit meanings are documented
at `:524` (luma bits 0-3, chroma nibble, 0x100 luma DC, 0x200/0x400 chroma DC,
0x1000 PCM).

Neighbour availability is computed by `macroblock_cache_load_neighbours`
(`reference/x264/common/macroblock.c:673-846`): it sets `i_neighbour`,
`i_neighbour_intra` (constrained-intra gating at `:783-784`, `:801-802`),
`i_neighbour_frame` (ignores slice boundaries) and the four MB indices. The
full `macroblock_cache_load` continues at `:855-1353`, with wrappers at
`:1354-1359`. `x264_macroblock_cache_save` (`:1680-1880`) writes type/partition/
QP/intra modes/nnz/MV/ref/mvd/skipbp back into the frame tables; the I_PCM
special case is at `:1740-1750`.

`x264_macroblock_thread_init` (`common/macroblock.c:501-550`) seeds the current
MB's `fenc`/`fdec` pointers and caches; `x264_macroblock_slice_init` (`:423-500`)
resets per-slice state.

### 2.7 Enum values that must match exactly

`mb_class_e` (`common/macroblock.h:64-89`): `I_4x4=0, I_8x8=1, I_16x16=2,
I_PCM=3, P_L0=4, P_8x8=5, P_SKIP=6, B_DIRECT=7, B_L0_L0=8 … B_SKIP=18`.
`x264_mb_type_fix` (`:90-96`) maps internal modes to their canonical form (e.g.
I_8x8 → I_4x4 for cache purposes). Partition enum `mb_partition_e` is at
`:115-141`; `x264_mb_partition_listX_table` (`:97-109`, `:143-157`) says which
list a B MB type uses per partition, and
`x264_mb_partition_count_table`/`pixel_table` are at `:158-178`.

`cbp_to_golomb` and the CAVLC tables are in `common/tables.c`; the `base_ctx`
array for CABAC CBF contexts is `encoder/cabac.c:614`.

### 2.8 Phoenix state ownership

The port deliberately makes each stage's state explicit: `phx_mb_state_t`
(`src/phx_mbcache.h`) holds frame-wide MB tables; `phx_mb_cache_t` holds the
current neighbour cache; `phx_mb_decision_t` crosses the A/B→C boundary;
`phx_cabac_t` stays inside the entropy stage. `src/phx_encoder.h:60-64` states
the serial-ownership rule explicitly. One known legacy hazard is documented at
`src/phx_encoder.c:1522-1538`: `phx_mbcache.c`'s `intra_border_backup[3][3][16]`
is undersized for wide frames, so `p_fdec` is withheld from `cache_save` when
`i_mb_width > 3`. A shader port must not reproduce that workaround; size the
array as x264 does (`reference/x264/common/common.h:734`).

---

## 3. Slice-type decision and lookahead

### 3.1 Frame intake and delay

`x264_encoder_encode` copies the picture, then calls `x264_lookahead_put_frame`
(`encoder.c:3368-3426`). With `i_sync_lookahead` the frame goes to `ifbuf`;
without it, straight to `next` (`encoder/lookahead.c:192-198`). The drain gate
(`encoder.c:3428-3433`) keeps `i_delay` frames buffered; `i_delay` from
`encoder.c:1602-1613`. `x264_lookahead_get_frames` (`lookahead.c:223-250`) is
either a wait on the lookahead thread's `ofbuf` (`:225-231`) or an inline call to
`x264_slicetype_decide` (`:233-249`). After deciding, `lookahead_encoder_shift`
(`:210-221`) moves `i_bframes+1` frames from `ofbuf` to `h->frames.current`.

Phoenix keeps the same queue explicitly in `phx_t.input_frames/coded_types/
coded_frames` (`src/phx_encoder.h:101-107`), evaluates one minigop per
`phx_lookahead_get_frames*` (`src/phx_lookahead.c:592-645`).

### 3.2 Lowres planes

`x264_frame_init_lowres` (`common/mc.c:458-482`) duplicates the last column and
row (`:466-468`), calls `frame_init_lowres_core` (`:469-470`, implementation
`:484-507`), expands borders (`:471`) and **poisons the caches**:
`i_cost_est = -1` (`:473`), `i_row_satds[*][*][0] = -1` (`:475-477`),
`lowres_mvs[*][*][0][0] = 0x7FFF` (`:479-481`). The sentinel values matter:
`slicetype_frame_cost` uses them to decide whether a reference has already been
searched (`encoder/slicetype.c:855-867`).

The core filter is (`:494-499`):

```
FILTER(a,b,c,d) = ((((a+b+1)>>1) + ((c+d+1)>>1) + 1) >> 1)
dst0[x] = FILTER(src0[2x],   src1[2x],   src0[2x+1], src1[2x+1])
dsth[x] = FILTER(src0[2x+1], src1[2x+1], src0[2x+2], src1[2x+2])
dstv[x] = FILTER(src1[2x],   src2[2x],   src1[2x+1], src2[2x+1])
dstc[x] = FILTER(src1[2x+1], src2[2x+1], src0[2x+2], src2[2x+2])
```

Note the deliberate integer rounding; the comment at `:493` says it matches
assembly. Phoenix copies this recurrence verbatim at `src/phx_frame.c:81-106`.

### 3.3 Lowres cost evaluation

`lowres_context_init` (`encoder/slicetype.c:45-61`) sets `a->i_qp = X264_LOOKAHEAD_QP`,
`a->i_lambda = x264_lambda_tab[qp]`, loads cost tables and **downgrades** ME to
DIA or HEX and subme to 2 or 4 (`:50-59`), with `b_chroma_me = 0` (`:60`).

`slicetype_mb_cost` (`encoder/slicetype.c:514-791`) is the core 8x8 decision:
* `b_bidir = b < p1`, `i_bipred_weight = 64 - (dist_scale_factor>>2)` or 32
  (`:522-529`).
* Per-list lowres MV/cost caches (`:530-531`), an edge-MB score mask
  `b_frame_score_mb` (`:532-534`), and a constant `lowres_penalty = 4`
  (`:541-542`).
* Reverse-order MV prediction from right/top/top-left/top-right candidates,
  median if more than one (`:662-680`); fast skip when the MVP residual < 64
  (`:682-692`); `x264_me_search` then `cost -= p_cost_mv[0]` and
  `cost += 5*lambda` for non-zero MV (`:694-697`).
* Bidirectional candidate from the colocated `fref1->lowres_mvs[0]` scaled by
  `dist_scale_factor` (`:629-651`), plus a second TRY_BIDIR on the searched MVs
  (`:711-712`).
* Intra cost: `intra_mbcmp_x3_8x8c` plus optional plane and 8x8 modes (`:717-744`),
  cost `((i_icost + 5*lambda) >> (BIT_DEPTH-8)) + 4` (`:746`). AQ weighting
  `i_inv_qscale_factor` only affects the AQ and row-SATD accumulators
  (`:748-756`), not the raw slicetype decision.
* Final packed cost at `:790`.

`slicetype_slice_cost` (`:814-834`) walks MBs **backwards** (`:818` explains
why: lowres MVs feed the main encode as predictors) and can skip edge MBs unless
MB-tree/VBV/tiny frame is enabled (`:823-828`).

`slicetype_frame_cost` (`:836-995`) caches results in `i_cost_est[b-p0][p1-b]`
(`:848-849`), computes `dist_scale_factor = ((b-p0)<<8)+((p1-p0)>>1))/(p1-p0)`
(`:869-870`), optionally runs OpenCL/lookahead threads (`:878-944`), sums the
outputs (`:946-981`) and rescales B costs:
`i_score = i_score*100/(120+i_bframe_bias)` (`:983-987`). The intra cache flag
`b_intra_calculated` is set only when `b == p1` (`:987`).

Phoenix's lower-resolution equivalents are `phx_lowres_frame_cost`
(`src/phx_lowres.c:785`), `phx_lowres_path_cost` (`:857`) and
`phx_slicetype_frame_cost` (`:894`), with a DIA/HEX-style fullpel search at
`src/phx_lowres.c:437-483` and subpel at `:483-511`.

### 3.4 Scenecut

`scenecut_internal` (`encoder/slicetype.c:1384-1428`):

```
icost = frames[p1]->i_cost_est[0][0]          (:1394)
pcost = frames[p1]->i_cost_est[p1-p0][0]      (:1395)
i_gop_size = frame->i_frame - lookahead->i_last_keyframe   (:1397)
f_thresh_max = scenecut_threshold / 100.0    (:1398)
f_thresh_min = f_thresh_max * 0.25            (:1400)
if keyint_min == keyint_max: f_thresh_min = f_thresh_max    (:1403-1404)
bias from keyint_min/keyint_max piecewise     (:1405-1415)
res = pcost >= (1.0 - f_bias) * icost          (:1417)
```

`scenecut` (`:1430-1468`) adds flash detection: with B-frames it evaluates
extra `curp1` candidates and clears `b_scenecut` for frames that cannot be real
scenecuts (`:1433-1451`), then clears candidates that would be the p0 of a
cut but were already the p1 of one (`:1453-1461`), and finally requires
`frames[p1]->b_scenecut` (`:1464-1466`). "Real" scenecut checks (the ones that
produce a log line at `:1418-1425`) are distinguished from the internal pruning
calls (`real_scenecut = 0`).

In `x264_slicetype_analyse` (`:1473-1743`) the first decision is:

```
if IS_AUTO_OR_I(frames[1]->i_type) &&
   scenecut_threshold && scenecut(h,&a,frames,0,1,1,orig_num_frames,i_max_search)
       frames[1]->i_type = I (if AUTO); return                 (:1522-1528)
```

The deterministic search cap `i_max_search = min(frames available, 250)` is
further limited to `i_slicetype_length + 1 - intra_minigop` when
`b_deterministic` is set (`:1478-1485`). This is the single most important
detail for repeatable lookahead across different host thread schedules.

Phoenix ports `phx_lh_scenecut_internal` and `phx_lh_scenecut`
(`src/phx_lookahead.c:121`, `:158`) with the same formulas; MB-tree and VBV
lookahead are not ported (`src/phx_lookahead.h:27-31`).

### 3.5 B-adapt and BREF

After forced keyframes are replaced (`slicetype.c:1534-1539`) and GOPs closed at
IDRs (`:1541-1546`):

* **TRELLIS** (`i_bframe_adaptive == X264_B_ADAPT_TRELLIS`, `:1553-1579`):
  `slicetype_path`/`slicetype_path_cost` (`:1288-1382`) build path strings of
  `B`/`P`, exhaustive over up to `bframe+1` paths (`:1336`), accumulate costs
  (`:1301-1321`), prune with `threshold` (`:1307`, `:1314-1321`), and store the
  best path in `best_paths[j % (X264_BFRAME_MAX+1)]` (`:1381`).
* **FAST** (`:1580-1621`): for each candidate frame, build `bframes` B's then
  either `"PP"` or `"BP"`, call `slicetype_path_cost` twice
  (`:1611-1615`) and choose the cheaper (`:1617-1620`). The cost of the
  `"BP"` path is bounded by `cost_p` so it can early-terminate.
* **NONE** (`:1623-1645`): simple counter; frames are B while `num_bframes`
  remains, except when the next frame is already B.

Then the first minigop is rechecked for scenecut (`:1653-1663`), MB-tree runs
(`:1675-1678`), the keyint limit is enforced by walking candidates backwards
and allowing `last_possible` (`:1680-1731`), VBV lookahead optionally runs
(`:1733-1734`), and undecided frames are restored to their forced types
(`:1736-1738`).

`x264_slicetype_decide` (`:1745-1974`) does the duration/POC bookkeeping
(`:1757-1784`), forces types from 2-pass stats if present (`:1786-1792`), walks
the minigop applying BREF/B-pyramid/keyint rules (`:1799-1885`), sets
`b_last_minigop_bframe` and `i_bframes` (`:1887-1889`), inserts a BREF in the
middle of a long minigop (`:1891-1896`), precomputes RC costs (`:1898-1935`),
runs weighted-P analysis (`:1937-1943`) and finally reorders to coded order:

```
idx_list[] = { brefs+1, 1 }                            (:1950)
for each B frame in display order:
    idx = idx_list[is_BREF]++                          (:1953)
    frames[idx] = that frame; i_reordered_pts = next.list[idx]->pts (:1954-1955)
frames[0] = next.list[bframes]; i_reordered_pts = next.list[0]->pts (:1957-1958)
copy back                                              (:1959)
```

Phoenix implements the FAST-equivalent `phx_lh_slicetype_analyse`
(`src/phx_lookahead.c:194`) and `phx_lh_slicetype_decide` (`:390`), including
BREF insertion, but **not** trellis B-adapt, weighted prediction, VBV lookahead
or MB-tree (`src/phx_lookahead.h:27-31`).

### 3.6 Interaction with AQ and MB-tree

AQ is applied at picture intake (`encoder.c:3411-3417`) via
`x264_adaptive_quant_frame` (`encoder/ratecontrol.c:304-415`) — it writes
`f_qp_offset`, `f_qp_offset_aq` and `i_inv_qscale_factor`, which `slicetype_mb_cost`
uses for AQ-weighted row costs (`slicetype.c:749-750`, `:778-787`). MB-tree
propagation (`slicetype.c:1029-1184`) rewrites `f_qp_offset` using
`i_propagate_cost` and the inverse-QScale factors
(`macroblock_tree_finish` `:1039-1047`). In 2-pass with MB-tree,
`slicetype_frame_cost_recalculate` (`:999-1027`) recomputes costs from row SATDs
instead of re-running ME.

---

## 4. Intra and inter analysis

### 4.1 Cost tables and λ

`x264_analyse_init_costs` (`encoder/analyse.c:179-202`) builds
`logs[i] = log2(i+1)*2 + 1.718`, `logs[0] = 0.718` (`:186-188`) for all QPs
`qp_min..qp_max` plus `X264_LOOKAHEAD_QP` (`:190-195`). `init_costs`
(`:143-177`) fills:

* `cost_mv[qp][i] = min(lambda*logs[abs(i)] + 0.5, 65535)` (`:151-157`) over
  ±`4*mv_range`, with the pointer offset by `2*4*mv_range` (`:152`); factor 4
  from qpel and 2 from sign/opposite direction (`:150`).
* `cost_table->ref[qp][i][j] = i ? min(lambda*bs_size_te(i,j), 65535) : 0`
  (`:158-160`).
* `cost_mv_fpel` for ESA/EDIA (`:161-170`).
* `i4x4_mode[i] = 3*lambda*(i != 8)` (`:171-173`) — the fixed 3λ bonus for
  matching the predicted intra mode (see `:790`, `:818`, `:909`).

`mb_analyse_load_costs` (`:248-253`) binds `p_cost_mv` and `p_cost_ref[2]` from
the active number of references. `mb_analyse_init_qp` (`:255-292`) sets
`i_lambda = x264_lambda_tab[qp]`, `i_lambda2 = x264_lambda2_tab[qp]`
(`:258-259`), the four `i_trellis_lambda2[2][2]` values (`:264-267`), the psy
chroma offset (`:270-272`), and switches to emergency noise reduction for
`qp > QP_MAX_SPEC` (`:274-288`).

`mb_analyse_init` (`:294-470`) derives `i_mbrd`:

```
subme = i_subpel_refine - (slice_type == B)                 (:296)
i_mbrd = (subme>=6) + (subme>=8) + (i_subpel_refine>=10)    (:301)
b_deblock_rdo = subme>=9 && deblocking enabled              (:302)
b_early_terminate = subme < 11                             (:303)
```

and initialises all SATD costs to `COST_MAX` (`:310-318`). MV bounds are set
from the MB position and `i_mv_range` (`:330-397`), with thread-range clipping
and deterministic override at `:355-370`.

Phoenix: `src/phx_cost.c:23-88` builds the cost tables with the same formula;
`src/phx_cost.c:102-135` performs the QP/lambda and cost binding.

### 4.2 Main analysis dispatch (`encoder/analyse.c:2918-3734`)

`x264_macroblock_analyse` starts with `x264_ratecontrol_mb_qp` (`:2923`), the
±1-QP reuse trick for AQ at `:2926-2927`, then `mb_analyse_init` (`:2931`).

**I slice** (`:2934-2952`): optional `mb_init_fenc_cache`, `mb_analyse_intra`,
`intra_rd` when `i_mbrd`, then choose the minimum of I16/I4/I8/PCM with
`COPY2_IF_LT` (`:2943-2948`), then `intra_rd_refine` for `i_mbrd>=2` (`:2950-2951`).

**P slice** (`:2953-3304`):
* fast-skip / mb_info shortcuts (`:2970-3014`); `x264_macroblock_probe_pskip`
  at `:3013` (implementation `encoder/macroblock.c:1129`).
* `mb_analyse_inter_p16x16` (`:3038`; impl `:1255-1333`), then optional 8x8
  with mixed refs (`:3049-3052`), 4x4/8x4/4x8 sub-partitions (`:3068-3092`),
  16x8/8x16 (`:3095-3113`).
* qpel refinement by partition (`:3119-3179`).
* chroma ME/intra (`:3181-3198`), then inter-vs-intra SATD comparison
  (`:3200-3224`).
* RD path when `i_mbrd`: `mb_analyse_p_rd` (`:3207`; impl `:2547`), transform
  size RD (`:3217`; impl `:2773`), `intra_rd` with a `5/4` threshold
  (`:3218`), partition selection (`:3208-3215`).
* QPRD refinement (`i_mbrd>=2`) at `:3246-3302`: `x264_me_refine_qpel_rd`
  per partition (`:3256`, `:3263-3264`, `:3271-3272`, `:3281-3298`).

**B slice** (`:3305-3733`): direct-mode auto heuristic (`:3314-3334`),
`x264_mb_predict_mv_direct16x16` (`:3336`; impl `common/mvpred.c:454`), B_SKIP
probe and `ssd_mb`/λ2 test (`:3346-3351`), then `mb_analyse_inter_b16x16`
(`:1890`), B 8x8/direct (`:2283`), 16x8/8x16 (`:2360`, `:2454`), bidir RD
(`:2622`, `:2692`) and final type selection (`:3493-3732`).

Finally `analyse_update_cache` (`:3734`) writes the chosen modes/MVs/nnz into
the MB cache (`x264_macroblock_cache_intra8x8_pred` at `:3746`).

Phoenix's `phx_macroblock_analyse_slice` (`src/phx_analysis.c:1250`, `:1332`)
covers I-slice analysis and a P L0 16x16 trial; unsupported decisions are
explicitly rejected in `phx_encode` (`src/phx_encoder.c:1425-1441`).

### 4.3 Intra mode decisions

**I16x16** (`mb_analyse_intra`, `encoder/analyse.c:688-742`): availability from
`predict_16x16_mode_available(h->mb.i_neighbour_intra)` (`:692`); fast SIMD path
`intra_mbcmp_x3_16x16` for DC/H/V plus `bs_size_ue` mode cost (`:700-706`);
plane mode only if the best so far is within a subme-dependent threshold
(`:695-696`, `:709-715`). B slices add the CAVLC mb-type prefix cost
`lambda*i_mb_b_cost_table[I_16x16]` (`:736-738`), where the table is at
`:124-127`.

**I8x8** (`:745-862`): base cost `lambda*4` (`:753`), SA8D when available
(`:749`), predicted mode bonus `3*lambda` (`:790`, `:818`), analysis shortcuts
from `intra_analysis_shortcut` (`:673-683`, `:800-803`), and encoding of each
8x8 block as it is chosen so subsequent blocks predict from reconstructed
samples (`:833-834`). The early-termination thresholds are at `:859-861`.

**I4x4** (`:864-979`): base `lambda*(24+16)` (JVT SATD0 + mode bits, `:867`),
per-block predicted-mode bonus `3*lambda` (`:909`, `:937`), topright sample
emulation when only top is available (`:886-888`), and the same encode-as-you-go
strategy (`:959-960`). `i_skip_intra` backups of fdec/DCT/nnz are captured at
`:965-974`.

**Chroma** (`mb_analyse_intra_chroma`, `:586-665`): availability from
`predict_chroma_mode_available` (`:615`), `intra_mbcmp_x3_chroma` for U and V
with `bs_size_ue` cost (`:621-636`), plane mode evaluated separately
(`:624-627`), and the FIX4 mapping `x264_mb_chroma_pred_mode_fix` from
`common/predict.h:45-49` applied to the signalled cost (`:657`).

**RD re-evaluation** (`intra_rd` `:982-1015`, `intra_rd_refine` `:1016-1135`,
`rd_cost_i8x8` `rdo.c:279`, `rd_cost_i4x4` `rdo.c:315`, `rd_cost_chroma`
`rdo.c:349`) replaces SATD with SSD+λ·bits using a **copy** of the live CABAC
state (`rdo.c:71-73`, `:302-308`, `:336-344`, `:362-370`).

### 4.4 Motion estimation

`x264_me_search_ref` (`encoder/me.c:182-798`) is fullpel + optional hpel
prediction evaluation:

* MV range from `h->mb.mv_limit_fpel` (`:199-206`) with the packed
  `CHECK_MVRANGE` test (`:204-209`).
* Subme≥3: evaluate the mvp and extra predictors at **halfpel** precision,
  choose the best, then round to fullpel for the search start (`:216-275`).
  Subme<3: round predictors to fullpel and deliberately omit the mvp bit cost
  to avoid biasing against it (`:276-318`, rationale at `:283-290`).
* **DIA** (`:322-342`): radius-1 diamond, `i_me_range` iterations, early break
  when the centre wins.
* **HEX** (`:344-420`): radius-2 hexagon with duplicate-candidate elimination
  (`:364-401`) followed by an 8-neighbour square refine (`:404-418`).
* **UMH** (`:422-616`): uneven-cross multi-hexagon, predictor refinement,
  SAD-threshold early exits (`:448-466`), adaptive search range from predictor
  disagreement (`:468-519`), cross + 4-hex grid (`:521-616`).
* ESA/TESA exhaustive successive elimination follows in the same switch
  (`:618-798`; not used by Phoenix).

`x264_me_refine_qpel` (`:801-810`) calls `refine_subpel` (`:865-992`) with
iteration counts from `subpel_iterations[subme] = {refine_hpel, refine_qpel,
me_hpel, me_qpel}` at `:38-50`. Halfpel diamond uses SAD `fpelcmp_x4`
(`:885-923`), quarterpel diamond uses SATD `mbcmp_unaligned` including chroma
when enabled (`:946-963`); subme=1 has a simplified SAD path (`:964-986`).
`m->cost_mv` is recomputed at `:991`.

Biprefinement: `x264_me_refine_bidir_satd` (`:1180`) and
`x264_me_refine_bidir_rd` (`:1185`) share `me_refine_bidir` (`:1027`), which
caches the 3x3 neighbourhood of both lists.

Phoenix provides `phx_me16` (`src/phx_me.c:160`) and lowres ME
(`src/phx_lowres.c:437`, `:483`); subpel refinement, multi-reference and B
bidirectional ME are **not ported** (`src/phx_analysis.h:19-20`).

---

## 5. Prediction, transform, quantization

### 5.1 Intra prediction

`common/predict.c` implements the C predictors; dispatch tables are initialised
by `x264_predict_16x16_init` (`:889`), `8x8c` (`:936`), `8x16c` (`:977`), `8x8`
(`:1000`) and `4x4` (`:1042`). Key formulas:

* 16x16 DC (`:67-100`), H (`:104`), V (`:116`), plane (`:132`).
* 4x4 DC/H/V (`:481-510`), DDL (`:534`), DDR (`:546`), VR (`:560`), HD
  (`:577`), VL (`:594`), HU (`:610`).
* 8x8 edge filter (`:632-698`), DC (`:700-720`), H/V (`:723-738`), DDL/DDR/VR/
  HD/VL/HU (`:741-886`).

The 8x8 filter's neighbour mask comes from `x264_pred_i4x4_neighbors`
(`common/macroblock.h:43-57`), one entry per intra 4x4 mode.

Mode-number fixups: `x264_mb_pred_mode16x16_fix` and
`x264_mb_pred_mode4x4_fix` collapse the pseudo-DC modes to real DC;
`x264_mb_chroma_pred_mode_fix` is at `common/predict.h:45-49`.

Phoenix ports the same predictors in `src/phx_predict.c:40-854`.

### 5.2 Transforms

* 4x4 Hadamard DC: `dct4x4dc` (`common/dct.c:47-76`) and `idct4x4dc`
  (`:78-107`); 2x4 variant `dct2x4dc` (`:109-143`). Used by I16 DC
  (`encoder/macroblock.c:215-224`) and chroma DC (`:82-96`).
* 4x4 DCT: `sub4x4_dct` (`common/dct.c:157-189`) with the butterflies at
  `:164-188`; `sub8x8_dct`/`sub16x16_dct` are four/sixteen calls (`:191-205`).
* 4x4 IDCT: `add4x4_idct` (`:272-310`) with the final `(x+32)>>6` rounding at
  `:297-300` and clipping at `:304-309`.
* 8x8 DCT/IDCT: `DCT8_1D` macro (`:332-357`), `sub8x8_dct8` (`:359`),
  `add8x8_idct8` (`:415`).
* DC-only adds: `add4x4_idct_dc` (`:442`), `add8x8_idct_dc` (`:454`),
  `add16x16_idct_dc` (`:462`).
* Zigzag scans: `zigzag_scan_8x8_frame/field` (`:829-837`),
  `zigzag_scan_4x4_frame/field` (`:843-853`), `zigzag_sub_4x4*` (`:878-916`),
  `zigzag_interleave_8x8_cavlc` (`:936-948`), dispatch at `:950-958`.

Phoenix: `src/phx_dct.c:30-413` and `src/phx_quant.c:259-387` mirror these.

### 5.3 Deadzone quantization

`QUANT_ONE` (`common/quant.c:50-57`):

```c
if (coef > 0) coef = ((f) + (uint32_t)coef) * mf >> 16;
else          coef = -(int32_t)(((f) + (uint32_t)(-coef)) * mf >> 16);
nz |= coef;
```

`quant_8x8` (`:59`), `quant_4x4` (`:67`), `quant_4x4x4` (`:75`),
`quant_4x4_dc` (`:88`), `quant_2x2_dc` (`:96`) are loops around it. Dequant is
`dequant_4x4` (`:112-128`), `dequant_8x8` (`:130-146`) with `i_mf = qp%6`,
`i_qbits = qp/6 - {4,6}` and the rounding constant `f = 1 << (-i_qbits-1)`.

Table construction in `x264_cqm_init` (`common/set.c:73-206`):
`deadzone[4] = {32-i_luma_deadzone[1], 32-i_luma_deadzone[0], 32-11, 32-21}`
(`:81-83`); `quant4_bias = min(DIV(deadzone<<10, j), (1<<15)/j)` (`:179`),
`quant4_bias0 = (1<<15)/j` (`:180`), with the 8x8 equivalents at `:199-200`.
The bias is the deadzone offset used by `QUANT_ONE`'s `f`. The RDO path uses
`quant*_bias0` (round-to-nearest) as its starting point.

The quant choice inside `x264_quant_4x4`/`8x8` (`encoder/macroblock.h:90-110`)
selects the CQM category from `(b_intra, plane)` (`:92`, `:103`), optionally
denoises (`:93-94`) and dispatches to trellis or deadzone (`:95-98`, `:106-109`).

### 5.4 Trellis QNS for CABAC

`x264_quant_4x4_trellis`/`8x8_trellis`/DC variants are thin wrappers
(`encoder/rdo.c:1098-1167`) around `quant_trellis_cabac` (`:642-895`).

Algorithm summary (C reference, `:642-895`):

1. Pick weight tables by block size (`:649-650`) and CABAC state pointers for
   significance/last/level from `x264_significant_coeff_flag_offset`,
   `x264_last_coeff_flag_offset`, `x264_coeff_abs_level_m1_offset` (`:652-654`,
   `:696`; tables at `common/tables.c:1758-1791`).
2. Produce a deadzone quantised baseline; if all zero, return 0 (`:656-693`).
3. `last_nnz` from `coeff_last[ctx_block_cat]` (`:695`). DC-only shortcut
   `:698-706` calls `trellis_dc_shortcut` (`:463-493`) to try `q-1..q`.
4. A Viterbi trellis over coefficients **in reverse zigzag order**
   (`:756-762`). Each node tracks `score`, a level-tree index, and four CABAC
   states for contexts {0,4,8,9} (`:417-428`, `:747-752`).
5. For each coefficient, candidate levels are `q-1` and `q` (`:815-841`);
   distortion uses DCT weights (`coef_weight2`) and psy-trellis optionally
   subtracts a complexity bias (`:821-830`). Bit cost comes from
   `x264_cabac_size_unary`/`cabac_transition_unary` precomputed by
   `x264_rdo_init` (`:384-415`, tables at `common/tables.c:1717-1751`).
6. The `TRELLIS_LOOP` macro (`:764-861`) has two versions (`ctx_hi` 0 and 1)
   because the level state machine has two disjoint node-context subsets; the
   chosen path is written back as signed levels at `:887-892`.

Psy-trellis DCT inputs are prepared by `psy_trellis_init` (`analyse.c:561-567`)
and the cache at `common/common.h:584-585`.

Phoenix: `src/phx_rdo.c:836` (`phx_rdo_quant_trellis_cabac`) with the same node
structure and precomputed tables (`src/phx_rdo.c:215`, `:389-401`).

### 5.5 Bounded CAVLC QNS

`quant_trellis_cavlc` (`encoder/rdo.c:921-1096`) is explicitly *not* a trellis:
the comment at `:897-919` explains CAVLC has five coupled state machines and
uses a greedy QNS instead. Steps:

1. Compute `nC` from the neighbour nnz via `x264_mb_predict_non_zero_code`
   (`:934-935`; helper `common/macroblock.h:432`).
2. Handle 8x8→4x4 CAVLC munging (`:940-951`).
3. Find the last non-zero coefficient (`:955-961`), initialise two rounding
   options per coefficient and their distortion deltas (`:963-1012`), starting
   the deadzone halfway between nearest and round-down (`:981-985`).
4. Score the initial state by actually calling the **real CAVLC bit counter**
   (`bs_write_*` are redefined to count bits at `rdo.c:44-53`):
   `h->out.bs.i_bits_encoded = 0; cavlc_block_residual_internal(...)` (`:1014-1020`).
5. Greedily flip the one coefficient that best improves
   `distortion_delta + lambda2 * bits` (`:1022-1077`), re-counting bits with the
   bounded counter each time (`:1047-1054`). Adjusted coefficients are frozen
   (`:1071-1073`).

Overflow behaviour differs between real encode and RD: `cavlc_block_residual_escape`
(`encoder/cavlc.c:70-118`) sets `h->mb.b_overflow = 1` in real encoding
(`:106-108`), causing the `reencode` retry in `slice_write`; under
`RDO_SKIP_BS` it instead adds 2000 bits (`:102-104`).

Phoenix: `src/phx_rdo.c:979` with the bounded bit counter at `:957`.

### 5.6 nnz semantics and PCM

* `non_zero_count` is set by each block encoder: 4x4 at
  `encoder/macroblock.h:158`, 8x8 via `STORE_8x8_NNZ` at `:195`/`:209-212`.
* I16 DC nnz lives in `h->dct.luma16x16_dc` and is coded through a separate
  CBF path (`encoder/cabac.c:989`, `encoder/cavlc.c:565`).
* Chroma DC nnz uses `CHROMA_DC+0/1` slots (`encoder/cabac.c:1069-1070`,
  `encoder/cavlc.c:581-582`).
* I_PCM sets all 48 nnz entries to 1 (CABAC) or 16 (CAVLC) and CBP to
  `0x1000 | chroma | luma` (`common/macroblock.c:1740-1750`).
* A zero-DCT MB with no CBP reuses the previous QP to save `mb_qp_delta` bits
  (`common/macroblock.c:1753-1754`; mirrored in `encoder/cabac.c:155-164` and
  `encoder/cavlc.c:211-219`).

---

## 6. Entropy and bitstream

### 6.1 Exp-Golomb and bit writing

`bs_t` and the bit writer are in `common/bitstream.h`. Relevant helpers:
`bs_write_ue_big` (`:221-238`), `bs_write_ue` (`:241-244`, table `x264_ue_size_tab`
`:201-219`), `bs_write_se` (`:246-262`), `bs_write_te` (`:264-270`),
`bs_rbsp_trailing` (`:272-276`), size helpers `bs_size_ue/ue_big/se`
(`:278-299`). Alignment variants `bs_align_0/1/10` are at `:182-197`.
CABAC slices use `bs_align_1` before starting arithmetic coding
(`encoder.c:2795`).

Phoenix: `src/phx_bitstream.c:8-110`.

### 6.2 CABAC engine

Initialisation: `x264_cabac_init` (`common/cabac.c:32-46`) precomputes, for four
models (I, PB×2, CABAC-init-idc) and every QP, the 460 (or 1024 for 4:4:4)
initial states:

```
state = clip3((init[j][0]*qp)>>4 + init[j][1], 1, 126)
state_byte = (min(state,127-state) << 1) | (state >> 6)        (:42-43)
```

`x264_cabac_context_init` (`:48-51`) copies the chosen row into `cb->state`;
called at `encoder.c:2798` with `clip3(sh.i_qp-QP_BD_OFFSET,0,51)`.

Core coder:
* `x264_cabac_encode_init_core` (`:53-59`): `i_low=0, i_range=0x01FE,
  i_queue=-9`, no outstanding bytes.
* `cabac_putbyte` (`:69-99`): emits a byte when `i_queue>=0`, handles 0xFF
  carry by incrementing `i_bytes_outstanding` (`:77-79`) and propagating
  `cb->p[-1] += carry` (`:89`).
* `cabac_encode_renorm` (`:101-108`): shift from `x264_cabac_renorm_shift`
  (`common/tables.c:1708-1714`).
* `x264_cabac_encode_decision_c` (`:113-125`): LPS range from
  `x264_cabac_range_lps[state>>1][(range>>6)-4]` (`common/tables.c:1668-1686`),
  state transition from `x264_cabac_transition` (`common/tables.c:1688-1706`).
* bypass (`:128-134`) and `ue_bypass` (`:142-157`) use `bypass_lut` (`:136-140`).
* terminal (`:159-163`) subtracts 2 and renormalises.
* **flush** (`:165-183`) adds `range-2`, forces a 1 bit, shifts 9, then
  injects `((0x35a4e4f5 >> (h->i_frame & 31)) & 1) << 10` (`:174`). This makes
  the final CABAC byte depend on the frame counter — a critical determinism
  detail for a shader port.

Phoenix mirrors the engine at `src/phx_cabac.c:54-209` and takes an explicit
`frame` argument for the flush bit (`:209`).

### 6.3 CABAC syntax and contexts

`x264_macroblock_write_cabac` (`encoder/cabac.c:1088-1105`) dispatches to
`macroblock_write_cabac_internal` (`:917-1086`) with `plane_count/chroma`.

Ordered syntax (`:917-1086`):

1. optional `field_decoding_flag` (`:925-930`, writer `:70-80`).
2. MB type header: `cabac_mb_header_p/i/b` (`:435-610`). I-slice uses contexts
   `3+ctx … 3+7` (`:385-392`); P-slice adds the `ctx=14` prefix (`:396`); B-slice
   emits five bins at `27+3/4/5` (`:404-408`).
3. If I_PCM, raw samples and `cabac_encode_init_core` (`I_PCM` block `:943-963`).
4. `cabac_cbp_luma` (`:118-127`, contexts around 76), `cabac_cbp_chroma`
   (`:129-148`, contexts 77+0..7).
5. `cabac_transform_size` (`:235-239`, `399 + i_neighbour_transform_size`).
6. `cabac_qp_delta` (`:150-186`, contexts 60+0..3; the empty-I16 shortcut at
   `:155-164`).
7. Residual: I16 DC (`ctx_cat_plane[DCT_LUMA_DC][p]`, `LUMA_DC+p`), I16 AC,
   8x8 or 4x4 luma, chroma DC, chroma AC (`:984-1080`), each guarded by a CBF
   bin via `cabac_block_residual_cbf_internal` (`:892-903`) whose context is
   computed by `cabac_cbf_ctxidxinc` (`:612-645`, `base_ctx` at `:614`).

Sub-syntax details worth copying exactly:
* `cabac_intra4x4_pred_mode` (`:83-96`): one bin at ctx 68 for "same as
  predicted", else three bins at ctx 69 with `i_mode--` when greater.
* `cabac_intra_chroma_pred_mode` (`:98-116`): ctx 64+0..3 based on left/top
  chroma modes.
* `cabac_mvd_cpn` (`:270-327`): ctxbase 40 (x) / 47 (y), unary prefixes then
  `ue_bypass` for |mvd|≥9, capped storage at 66 (`:324-326`); `cabac_mvd`
  computes MVD from `x264_mb_predict_mv` and updates the mvd cache (`:329-353`).
* `cabac_ref_internal` (`:241-259`): ctx 54+0.. based on left/top refs; decrements
  the ref index unarily.
* Residual level coding: `coeff_abs_level1_ctx`, `coeff_abs_levelgt1_ctx`,
  and `coeff_abs_level_transition` at `:650-662`; significance/last maps via
  `WRITE_SIGMAP` (`:675-717`).

Context offset tables are in `common/tables.c:1758-1792`; the
`cabac_ctx_block_cat_e` enum and `ctx_cat_plane` mapping are in
`common/macroblock.h:273-299`.

Phoenix: `src/phx_cabac_syntax.c:184-864`, including the same CBF and context
tables (`src/phx_cabac_syntax.c:732`, `:767`).

### 6.4 CAVLC syntax

`x264_macroblock_write_cavlc` (`encoder/cavlc.c:487-595`) order:
MB header (`cavlc_mb_header_i/p/b`, `:305-486`), CBP via `cbp_to_golomb`
(`:550-553`), `transform_size_8x8_flag` (`:554-556`), `mb_qp_delta` (`:211-241`),
I16 DC+AC (`:558-572`), otherwise luma residual (`:573-577`), then chroma DC
and AC (`:578-590`). Skip runs are written by `slice_write` (`:2883-2891`,
`:3087-3088`).

`cavlc_block_residual_internal` (`:121-196`):
`coeff_level_run` → `i_total`, `i_total_zero` (`:131-133`); branchless trailing
ones/signs (`:135-145`); `x264_coeff_token[nC][total-1][trailing]` (`:148`);
level tokens with suffix-length updates (`:150-178`); total-zeros VLC specialized
for chroma DC (`:180-190`); run-before table (`:192-193`). Escape handling is at
`:70-118`. `nC` comes from `ct_index[x264_mb_predict_non_zero_code(...)]`
(`:198-208`).

CAVLC token tables are in `common/tables.c` (`x264_coeff0_token` at `:1799`,
`x264_coeff_token`, `x264_level_token`, `x264_total_zeros`, `x264_run_before`,
`x264_total_zeros_2x2_dc/2x4_dc`).

Phoenix: `src/phx_cavlc_syntax.c:69-411`, `src/phx_cavlc.c:951-1114`.

### 6.5 NAL encapsulation

Covered in §1.6. Two escape-sensitive details:
* `nal_escape_c` (`common/bitstream.c:29-40`) inserts 0x03 for `00 00 00/01/02/03`
  but not for `00 00 04+`.
* `encoder_encapsulate_nals` allocates `3/2 × payload` (`encoder.c:2072`) because
  emulation prevention can expand the RBSP; a shader port must allocate the
  same worst case.

### 6.6 SPS / PPS / SEI

`x264_sps_write` (`encoder/set.c:305-477`): profile/constraint/level
(`:308-316`), SPS id (`:318`), High-profile fields including chroma format and
bit depths (`:320-348`), `log2_max_frame_num-4`, POC type, `log2_max_poc_lsb-4`,
num refs, gaps flag, dimensions and frame-mbs-only (`:350-361`), crop (`:363-372`),
VUI (`:374-473`), rbsp trailing (`:475-476`).

`x264_pps_init` (`encoder/set.c:479-503`) sets `i_pic_init_qp = 26+QP_BD_OFFSET`
for ABR/stitchable, otherwise `SPEC_QP(qp_constant)` (`:494`), weighted pred
flags (`:491-492`) and `b_transform_8x8_mode` (`:502`). `x264_pps_write` is at
`:505-572`.

`x264_sei_write` (`encoder/set.c:80-99`) writes type/size with 0xFF extension
bytes then payload then `rbsp_trailing`. Writers: recovery point (`:574-591`),
version with the fixed UUID and option string (`:593-623`), buffering period
(`:625-645`), pic timing (`:647-676`), frame packing (`:678-718`), mastering
display (`:720-744`), content light level (`:745-761`), alternative transfer
(`:762-777`), dec-ref marking (`:789-819`), AVC-Intra UMID/VANC (`:844`, `:863`).
`x264_encoder_encode` selects them at `encoder.c:3693-3808`.

Phoenix: `src/phx_headers.c:98/165`, `src/phx_sei.c:322/340`. Only version SEI is
ported; the rest of the `encoder.c` SEI block is not.

### 6.7 Slice header

`slice_header_write` (`encoder.c:213-364`) in spec order:
`first_mb_in_slice` (`:215-223`), `slice_type = sh.i_type + 5` (`:225`), `pps_id`
(`:226`), `frame_num` masked to `log2_max_frame_num` (`:227`), field flags
(`:229-234`), `idr_pic_id` (`:236-237`), POC LSB and delta bottom (`:239-244`),
`redundant_pic_cnt` (`:246-247`), `direct_spatial_mv_pred_flag` for B (`:249-250`),
num-ref-index override (`:252-261`), ref list reordering loops (`:263-289`),
pred_weight_table for P (`:291-322`), dec_ref_pic_marking for reference NALs
(`:328-348`), `cabac_init_idc` (`:350-351`), `slice_qp_delta` (`:353`), deblocking
flags/offsets (`:355-363`).

`slice_header_init` (`:122-211`) fills all fields, including the direct-mode
auto heuristic (`:152-170`), ref-list reorder argument encoding (`:180-194`),
deblock enable rule (`:203-210`).

Phoenix: `src/phx_slice.c:14` / `:107`; the subset excludes MBAFF, MMCO and
weight tables, with `phx_slice_frame_t` carrying frame_num/POC/ref counts from
`phx_encoder_frame_context` (`src/phx_encoder.c:698-785`).

---

## 7. Rate control

### 7.1 QP ⇄ qscale and frame QP

`qp2qscale` (`encoder/ratecontrol.c:203-206`):
`0.85 * 2^((qp - 12 - QP_BD_OFFSET)/6)`; inverse at `:207-210`.
`qscale2bits` (`:216-223`) models texture as `(tex_bits+.1)*(qscale_old/qscale)^1.1`,
MV bits with exponent 0.5, plus misc.

`x264_ratecontrol_start` (`:1432-1538`):
* 2-pass: bind `rce` and read `direct_mode` (`:1441-1453`).
* VBV: clear row arrays, select `row_pred`, compute `buffer_rate` and
  `frame_size_maximum` from the level's `mincr` (`:1455-1492`).
* QP source: ABR/CRF `qscale2qp(rate_estimate_qscale)` (`:1497-1500`), 2-pass
  (`:1501-1505`), CQP with B-ref average and zones (`:1506-1520`);
  `i_force_qp` overrides (`:1521-1522`).
* clip to `[i_qp_min, i_qp_max]` (`:1524`), seed `qpm`, average trackers and
  `accum_p_qp_update` (`:1526-1537`).

`get_qscale` (`:2003-2035`) is the CRF/ABR core:
`q = pow(blurred_complexity, 1-qcompress)` (or the MB-tree duration form at
`:2008-2012`), divided by `rate_factor`, with zone overrides (`:2026-2032`).

`rate_estimate_qscale` (`:2400-2663`):
* B frames have no independent RC; QP is interpolated between the two nearest
  references by POC distance plus `pb_offset` (`:2421-2451`), with `qp_novbv`
  recorded at `:2452`.
* 1-pass ABR: complexity smoothing (`:2560-2566`), CRF uses
  `rate_factor_constant` (`:2575-2578`), ABR uses `wanted_bits_window/cplxr_sum`
  with an overflow multiplier (`:2580-2599`), I-frame uses `accum_p_qp/ip_factor`
  (`:2602-2608`), then `vbv_pass1` (`:2631`).
* frame-size prediction via `predict_size` (`:2109-2112`), MinCR/buffer clamp
  (`:2645-2658`).

`x264_ratecontrol_qp` (`:1748-1752`) rounds `qpm` to the slice QP;
`x264_ratecontrol_mb_qp` (`:1754-1768`) adds the MB's AQ/MB-tree offset and
clips — this is what analysis and the slice header use.

### 7.2 AQ

`x264_adaptive_quant_frame` (`encoder/ratecontrol.c:304-415`) is called at
picture intake (`encoder.c:3417`). Energy is variance from `ac_energy_mb`
(`:260-302`) using `pixf.var`; the modes are:
* none / strength 0: zero or user-supplied offsets (`:313-345`);
* autovariance / biased: `qp_adj = pow(energy*bit_depth_correction+1, 0.125)`
  (`:363`) then centred (`:370-372`, `:382-391`);
* plain AQ: `strength*(log2(max(energy,1)) - (14.427+2*(BIT_DEPTH-8)))`
  (`:392-396`).

`f_qp_offset` is the MB-tree-adjusted offset; `f_qp_offset_aq` is the pure AQ
offset, and unreferenced B frames use the latter at `:1761`.

Phoenix: `phx_ratecontrol_aq_frame` (`src/phx_ratecontrol.c:529`),
`phx_ratecontrol_mb_qp` (`:626`).

### 7.3 MB-tree

`macroblock_tree` (`encoder/slicetype.c:1091-1184`) walks non-B frames backwards,
computes per-MB costs (`:1133-1171`) and calls `macroblock_tree_finish`
(`:1029-1049`) at the last non-B:

```
fps_factor  = round(clip(dur_avg)/clip(dur_frame)*256/MBTREE_PRECISION)
strength    = 5*(1-qcompress)
log2_ratio  = log2(intra_cost + propagate_cost) - log2(intra_cost) + weightdelta
f_qp_offset = f_qp_offset_aq - strength*log2_ratio                 (:1041-1047)
```

Propagation itself is `mbtree_propagate_cost` (`common/mc.c:511-525`) and
`mbtree_propagate_list` (`:527-600`) from lowres MVs and costs. **Not ported** in
Phoenix (`src/phx_lookahead.h:27`).

### 7.4 Per-row VBV rate control

`x264_ratecontrol_mb` (`encoder/ratecontrol.c:1590-1746`) is called after every MB
with that MB's bit count (`encoder.c:2974`). It accumulates `i_row_bits[y]` and
`qpa_aq` (`:1595-1596`), and only acts at row end (`:1598-1599`). Row size is
predicted by `predict_row_size` (`:1540-1567`) combining absolute-SATD and
temporal predictors; QP is stepped by 0.5 within
`prev_qp ± i_qp_step` (`:1628-1630`) withVBV/MinCR clamps (`:1634-1636`,
`:1680-1708`). If the row overshot badly it returns -1, and `slice_write`
restores the row checkpoint and re-encodes (`encoder.c:2974-2983`).

Phoenix has **no row RC**; `phx_ratecontrol_end` (`src/phx_ratecontrol.c:448`)
only updates frame-level windows.

### 7.5 End of frame and filler

`x264_ratecontrol_end` (`encoder/ratecontrol.c:1829-1994`) aggregates MB counts
and averages (`:1836-1845`), writes 2-pass stats (`:1846-1913`), updates ABR
complexity windows (`:1915-1928`), B-from-P predictors (`:1933-1945`), calls
`update_vbv` for the filler amount (`:1947`) and computes HRD timing (`:1950-1988`).
Filler NALs are emitted by `encoder_frame_end` (`encoder.c:4011-4034`).

### 7.6 Chroma QP

The chroma QP is never derived by a separate RC stage: `h->chroma_qp_table`
(`encoder.c:1585`) maps luma QP through the nonlinear table
(`common/macroblock.h:249-270`, values `QP(0..39)` with plateaus after 30),
plus `pps.i_chroma_qp_index_offset`. Analysis sets
`h->mb.i_chroma_qp = h->chroma_qp_table[qp]` (`analyse.c:257`, `:291`); the
effective chroma QP for trellis includes the out-of-spec extension
`+ max(qp-QP_MAX_SPEC,0)` (`:257`). Deblocking uses `chroma_qp_table[qp]`
(`common/deblock.c:414`). Phoenix: `phx_mb_chroma_qp` (`src/phx_mbencode.c:611`),
`phx_ratecontrol_chroma_qp` (`src/phx_ratecontrol.c:662`).

---

## 8. Reconstruction

### 8.1 Per-MB reconstruct

`x264_macroblock_encode` (`encoder/macroblock.c:974-987`) dispatches to
`macroblock_encode_internal` (`:618-973`). The generic per-block sequence for
intra (`encoder/macroblock.h:132-166`) is:

```
predict into p_fdec                          (:139-145)
sub4x4_dct(p_src=fenc, p_dst=fdec)          (:155)
nz = quant_4x4(...)                         (:157)
nnz[scan8[idx]] = nz; if nz: cbp_luma bit   (:158-162)
zigzag scan -> h->dct.luma4x4               (:162)
dequant -> add4x4_idct into p_fdec          (:163-164)
```

8x8 version at `:168-213`. Chroma is `mb_encode_chroma_internal`
(`encoder/macroblock.c:259-491`): 2x2 (or 2x4) Hadamard DC, `quant_2x2_dc`
(`:96` of quant.c), optional `mb_optimize_chroma_dc` (`:245`), then AC 4x4
blocks; wrapper `x264_mb_encode_chroma` at `:492-498`. I16 is
`mb_encode_i16x16` (`:126-243`).

Skip/direct reconstruction: `macroblock_encode_skip` (`:500-525`) followed by
`x264_mb_mc` (`common/macroblock.c:200-247`) / `x264_mb_mc_8x8` (`:158-199`),
which performs motion compensation with the weighted/hpel pointers set up by
`macroblock_load_pic_pointers` (`:568-672`). Inter residual blocks are encoded
by `macroblock_encode_p8x8_internal` (`:1179-1369`) and
`macroblock_encode_p4x4_internal` (`:1385-1418`).

Once written, `x264_macroblock_cache_save` publishes the reconstruction state
(§2.6), and `x264_macroblock_deblock_strength` computes boundary strengths
(`common/macroblock.c:1438-1623`; `encoder.c:3057`).

Phoenix reconstructs via `phx_macroblock_encode_ex` (`src/phx_mbencode.c:475`)
and `phx_macroblock_encode_chroma` (`src/phx_chroma.c:207`); only P L0 16x16
inter is reconstructed (`src/phx_encoder.c:1419-1424`).

### 8.2 Deblocking

`deblock_strength_c` (`common/deblock.c:277-304`) computes bS for each 4x4 edge:
2 if either side has non-zero nnz, else 1 if refs differ or |MV diff| ≥ 4
(vertical) / ≥ `mvy_limit` (horizontal, B frames also check list 1), else 0.
`deblock_edge` (`:306-324`) derives α/β from `alpha_table(index_a)`,
`beta_table(index_b)` with `a = i_alpha_c0_offset`, `b = i_beta_offset`, and tc
from `tc0_table[index_a][bS]` plus 1 for chroma.

`x264_frame_deblock_row` (`common/deblock.c:378-615`) walks the MB row,
loads deblock neighbour availability (`:340-376`), and filters vertical/horizontal
luma and chroma edges using the `FILTER` macro (`:417-614`). The intra variant
uses `deblock_edge_intra` (`:326-338`). `x264_macroblock_deblock` at `:615`
handles the RDO single-MB case.

Deblocking is interleaved with encoding by `fdec_filter_row`
(`encoder.c:2413-2531`): deblock one row behind the encode head
(`:2456-2458`), expand borders (`:2470-2471`), build hpel/filtered planes for
references (`:2472-2481`), signal `x264_frame_cond_broadcast` to frame threads
(`:2490-2491`) and measure PSNR/SSIM (`:2493-2530`).

Phoenix: `src/phx_deblock.c:352-689`, including `phx_frame_deblock_ex` (`:598`).

### 8.3 Reference frames, POC, frame_num

* POC: `fenc->i_poc = 2*(i_frame - max(last_idr,0))` (`encoder.c:3512-3513`),
  copied into `sh.i_poc` at `:2656-2667`.
* `frame_num`: increment only when `nal_ref_idc != 0` (`encoder.c:3881-3882`);
  reset to 0 on IDR (`:3504`). `fdec->i_frame_num = sh.i_frame_num` (`:2654`).
* DPB: `reference_update` (`encoder.c:2533-2562`) inserts `fdec` when
  `b_kept_as_ref`; `reference_reset` (`:2563`) empties it at IDR;
  `reference_hierarchy_reset` (`:2571-2607`) applies MMCO for
  open-GOP/B-pyramid.
* `reference_build_list` (`:2295-2411`) partitions refs by POC into L0 (`< i_poc`)
  and L1 (`> i_poc`) (`:2305-2313`), optionally reorders for MMCO (`:2315-2338`),
  sorts by `reference_distance` (`:2286-2293`, `:2340-2360`), caps counts
  (`:2364-2370`) and adds weighted duplicates for P slices (`:2372-2406`).
* `x264_macroblock_bipred_init` (`common/macroblock.c:1883-1925`) computes
  `dist_scale_factor` and `bipred_weight` for B slices.
* Direct MV prediction: temporal mode `mb_predict_mv_direct16x16_temporal`
  (`common/mvpred.c:183-288`) uses the colocated frame's `mv16x16`/`ref[2]`
  and `inv_ref_poc`; spatial mode `:289-448` uses left/top/topright
  candidates and median MV. `x264_mb_predict_mv_direct16x16` (`:454-516`)
  caches the result in `direct_mv/direct_ref/direct_partition`.
* Normal MV prediction: `x264_mb_predict_mv_16x16` (`common/mvpred.c:129-163`)
  uses left/top/topright with ref matching; `x264_mb_predict_mv_pskip` (`:166-181`)
  zeroes the predictor if neighbours are unavailable/zero.

Phoenix's DPB mirror is `phx_t.dpb[16]` (`src/phx_encoder.h:139-146`) with
`phx_encoder_dpb_refresh/lookup` (`src/phx_encoder.c:499/540`) and
`phx_encoder_build_ref_lists` (`:556`); it keeps only the data needed for the
single list-0 reference (`i_last_ref_frame`, `:128`). B-direct and L1 are not
ported.

---

## 9. Determinism, seek safety, and the shader boundary

### 9.1 Stages that are strictly serial (one ordered stream of state)

These stages cannot be reordered or parallelised without changing bytes:

1. **CABAC arithmetic coder** — `i_low/i_range/i_queue/i_bytes_outstanding/p`
   and `state[1024]` (`common/cabac.h:30-52`). Every bin depends on the previous
   bin's renormalisation and on contexts updated by earlier MBs. Even the flush
   byte depends on `i_frame` (`common/cabac.c:174`). RD's "size-only" copies
   (`encoder/rdo.c:71-73`) must *not* advance the live coder — Phoenix follows
   this with `phx_rdo_size_init` (`src/phx_rdo.c:340`).
2. **CAVLC skip runs** — `i_skip` accumulates across MBs and is only flushed
   when a non-skip MB or the slice end occurs (`encoder.c:2810`, `:2883-2891`,
   `:3087-3088`).
3. **`qp_delta` state** — `i_last_qp`, `i_last_dqp` (`encoder.c:2804-2805`,
   `common/macroblock.c:1756-1757`). CABAC `qp_delta` and CAVLC `qp_delta`
   depend on it (`encoder/cabac.c:150-186`, `encoder/cavlc.c:211-241`).
4. **Neighbour caches** — MB N is analysed after MB N-1's `cache_save`
   (`encoder.c:2861` → `:2972`). Intra prediction, MV prediction, CBF contexts
   and deblock strength all read those caches.
5. **Slice-header state** — `i_frame_num`, `i_idr_pic_id`, `i_poc`, MMCO list.
6. **Rate-control state** — `qpm`, `row_pred`, `buffer_fill`, accumulator
   windows (`encoder/ratecontrol.c:72-179`); row re-encode forces a rollback.
7. **NAL assembly and emulation prevention** — escape insertion is a streaming
   function of the previous two output bytes (`common/bitstream.c:29-40`).
8. **Lookahead order** — `last_nonb` and the lowres MV caches are sequential
   predictors (`encoder/lookahead.c:59-65`, `encoder/slicetype.c:662-680`).

### 9.2 Stages that can be parallelised (with x264's caveats)

* Lowres frame-cost evaluation across lookahead frames / rows:
  `slicetype_slice_cost` is dispatched to `lookaheadpool` with per-thread row
  bands (`encoder/slicetype.c:901-932`) and results summed deterministically
  (`:946-981`).
* Slice/frame threads: `slices_write` (`:3159`) and frame-thread rotation
  (`:3337-3346`), with `x264_thread_sync_ratecontrol` (`:2754`) for RC state.
  Determinism requires `b_deterministic` to cap the MV vertical range
  (`analyse.c:366-367`) and cap the lookahead search (`slicetype.c:1484-1485`).
* Pure per-block kernels with no cache feedback: SAD/SATD/SA8D, DCT/IDCT,
  quant/dequant, deblock filter kernels — but **not** their ordering relative to
  cache_save.

For a shader encoder the practical split is: shader classes A/B compute
candidate costs and coefficients from immutable inputs; class C writes the
entropy stream with the live CABAC/CAVLC state on one serial lane. This is the
split Phoenix already documents (`src/phx_mb_decision.h:1-8`,
`src/phx_analysis.h:22-24`, `src/phx_encoder.h:22-23`).

### 9.3 Checkpoint / rollback as a first-class operation

x264 backs up and restores the bitstream in four situations
(`encoder.c:2770-2774` defines the four slots):
`slice-max-size`, CAVLC level overflow, slice-min-MBS, VBV row. The backup must
include the `bs_t`/CABAC cursors, the previous CABAC byte, frame statistics and
QP state (`encoder.c:2689-2721`), and restore in the reverse order
(`:2723-2751`). A GPU design that cannot roll back must either fix the QP before
the MB (losing parity) or buffer MB-local bitstreams and concatenate (changing
CABAC context evolution — not equivalent).

### 9.4 Seek-safety notes

* Each slice starts from `x264_cabac_context_init` with a state derived only
  from slice type, QP and `cabac_init_idc` (`common/cabac.c:48-51`,
  `encoder.c:2798`), so a decoder can enter at a slice boundary. An IDR also
  resets DPB (`encoder.c:3523`) and frame_num (`:3504`).
* `bs_realign` before every NAL (`encoder.c:2776`, `encoder/set.c:307/:507`)
  means NALs are byte-aligned, so a shader writer must flush to a byte boundary
  between NALs.
* `i_poc` depends on `i_last_idr`, not on absolute frame number
  (`encoder.c:3513`); seeking must restore that counter consistently.
* The CABAC flush pseudo-random bit is indexed by `h->i_frame & 31`
  (`common/cabac.c:174`), so a seek that resets `i_frame` changes the last byte
  even if all MB data is identical.
* MB-tree propagates costs forward across frames (`slicetype.c:1051-1089`), so
  CQP parity tests should disable it; x264 enables it only for non-CQP modes
  (`encoder.c:1618-1624`, `ratecontrol.c:304-415`).

### 9.5 Practical invariants for the shader port

1. Freeze `b_cabac`, `i_cabac_init_idc`, chroma format, bit depth and CQM before
   the first frame; they change table construction at open time.
2. Compute AQ offsets **at picture intake**, not at MB time (`encoder.c:3417`).
3. Decide slice types on **lowres planes with the exact filter recurrence**
   (`common/mc.c:494-499`) and poisoned cache sentinels (`:473-481`).
4. Keep MV/ref/nnz/intra-mode caches in raster MB order; `scan8` is the required
   addressing scheme (`base.h:157-195`).
5. Use `λ` for mode/SATD decisions and `λ²` for SSD/RD decisions
   (`analyse.c:258-259`, `rdo.c:162-197`). Do not mix them.
6. Trellis must read the **live CABAC states** for level contexts
   (`rdo.c:652-654`, `:696`) and the precomputed unary tables (`:384-415`);
   approximate bit costs cause byte drift.
7. Apply the `i_chroma_qp_table` offset for chroma QP everywhere (analysis,
   chroma DC trellis, deblock) — `encoder.c:1585`, `analyse.c:257`,
   `deblock.c:414`.
8. Serialise entropy; if MB coefficients are computed in parallel, publish a
   `phx_mb_decision_t`-like record and let one serial lane encode it in raster
   order.
9. Preserve rollback points, or explicitly document that slice-max-size / VBV
   paths are unsupported.

---

## 10. Coverage check

### 10.1 Required-topic coverage

| # | requested topic | sections | primary citations |
|---:|---|---|---|
| 1 | top-level pipeline and exact call order | §1 | `encoder.c:1507-1859`, `:2096-2134`, `:3323-3902`, `:2752-3130`, `:2051-2091`, `:3904-4180` |
| 2 | data structures, per-MB caches | §2 | `common/common.h:270-763`, `common/frame.h:37-183`, `encoder/analyse.c:35-121`, `common/cabac.h:30-52`, `encoder/ratecontrol.c:72-179`, `common/base.h:153-195`, `common/macroblock.c:673-846`, `:1680-1880` |
| 3 | slice type, lowres, SATD/SA8D, scenecut, B-adapt FAST, BREF, keyint, reorder | §3 | `common/mc.c:458-507`, `encoder/slicetype.c:514-995`, `:1288-1743`, `:1745-1974` |
| 4 | intra/inter analysis, λ, I16/I8/I4/chroma, DIA/HEX/UMH, subpel, RD/MBRD/trellis | §4, §5.4-5.5 | `encoder/analyse.c:143-318`, `:586-979`, `:982-1135`, `:2918-3734`, `encoder/me.c:182-992`, `encoder/rdo.c:162-373`, `:642-1096` |
| 5 | prediction, transform, quantization, Hadamard DC, deadzone, QNS, scans/nnz | §5 | `common/predict.c:67-886`, `common/dct.c:47-326`, `:829-958`, `common/quant.c:50-146`, `common/set.c:73-206`, `encoder/rdo.c:642-1096`, `encoder/macroblock.h:112-213` |
| 6 | entropy/bitstream: CABAC states/contexts, CAVLC tokens, Exp-Golomb, NAL escape, SPS/PPS/SEI, slice header | §6 | `common/cabac.c:32-183`, `common/tables.c:1668-1792`, `encoder/cabac.c:35-1213`, `encoder/cavlc.c:70-704`, `common/bitstream.h:201-299`, `common/bitstream.c:29-104`, `encoder/set.c:80-818`, `encoder.c:122-364` |
| 7 | rate control CQP/CRF/ABR, AQ, MB-tree, QP + chroma offsets | §7 | `encoder/ratecontrol.c:203-223`, `:304-415`, `:541`, `:1432-1538`, `:1590-1768`, `:1829-1994`, `:2003-2663`, `encoder/slicetype.c:1029-1184` |
| 8 | reconstruction: deblock, refs, POC/frame_num | §8 | `common/deblock.c:277-615`, `common/macroblock.c:158-247`, `encoder.c:2286-2607`, `:2654-2667`, `:3512-3513`, `:3881-3882`, `common/mvpred.c:129-516` |
| 9 | determinism / serial stages | §9 | `common/cabac.h:30-52`, `common/cabac.c:165-183`, `encoder.c:2689-2751`, `:2770-2774`, `analyse.c:366-367`, `slicetype.c:1484-1485`, `common/bitstream.c:29-40` |

### 10.2 Citation verification

All `reference/x264/...:N` citations in this document were generated from the
same checkout and checked against the files' line counts with a script that
parses each `path:line` token and asserts `1 <= line <= file_length`. Sources
most heavily cited and their sizes: `encoder/encoder.c` (4603 lines),
`encoder/analyse.c` (3895), `encoder/ratecontrol.c` (3134), `encoder/slicetype.c`
(2036), `encoder/cabac.c` (1239), `encoder/rdo.c` (1184), `common/common.h`
(813), `common/tables.c` (2570). Where a range endpoint was not read in full,
the range is written as `first-last` with `last` equal to a verified function
boundary in the same file.

### 10.3 Things a shader port must consciously omit or re-implement

* Everything upstream of the MB loop that depends on threads:
  `x264_thread_sync_ratecontrol`, `thread_sync_context`, `thread_sync_stat`
  (`encoder.c:3132-3158`, `ratecontrol.c:2754`).
* Slice splitting, `slice-max-size`, `slice-min-mbs` and CAVLC overflow re-encode
  (`encoder.c:2761-2774`, `:2894-2902`, `:2909-2967`).
* OpenCL lookahead (`encoder/slicetype.c:878-897`) and multi-threaded lookahead
  (`:901-944`).
* Multi-pass, VBV row RC and MB-tree in rate control.
* All B-slice analysis, direct/spatial MV prediction, weighted prediction and
  8x8 transform size decisions in Phoenix today (§0.2).
* MBAFF/interlaced, 4:2:2/4:4:4 and high-bit-depth paths throughout.

Do not treat any of these as "small remaining cleanup": each one carries its
own bitstream-visible state machine.
