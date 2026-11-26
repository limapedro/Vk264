/* Phoenix public encoder API — an explicit struct-based mirror of x264's
 * encoder entry points.
 *
 * Source: reference/x264/encoder/api.c:76   (x264_encoder_open dispatch),
 *         reference/x264/encoder/api.c:159  (x264_encoder_headers dispatch),
 *         reference/x264/encoder/api.c:166  (x264_encoder_encode dispatch),
 *         reference/x264/encoder/api.c:130  (x264_encoder_close dispatch);
 *         reference/x264/encoder/encoder.c:1507 (x264_encoder_open core),
 *         :1579/:1581 (sps/pps init), :1590-1592 (mb dimensions),
 *         :1669 (cabac init), :1708 (cost init), :2096 (headers),
 *         :3323 (encode), :3358-3454 (put_frame / lookahead),
 *         :2855-2892 (slice/MB loop), :3857/:3978 (rate control),
 *         :4196/:4520 (close / cost free).
 *
 * Naming note: x264_encoder_headers' SPS/PPS byte assembly already exists as
 * phx_encoder_headers() in phx_headers.h:53.  The handle-based public mirror
 * is therefore phx_headers() here; it delegates to that function so there is
 * one header assembler.  phx_encoder_open/encode/close are the direct mirrors
 * and phx_open/phx_encode/phx_close are the short aliases from
 * docs/ORACLE_DESIGN.md:40-45.
 *
 * SHADER: C — every entry point drives the serial, single-threaded host
 * pipeline; no hidden thread or GPU state is introduced here.
 */
#ifndef PHX_ENCODER_H
#define PHX_ENCODER_H

#include <stdint.h>
#include "phx_param.h"
#include "phx_bitstream.h"
#include "phx_headers.h"
#include "phx_frame.h"
#include "phx_mbcache.h"
#include "phx_cost.h"
#include "phx_cabac.h"
#include "phx_lookahead.h"
#include "phx_slice.h"
#include "phx_ratecontrol.h"
#include "phx_dpb.h"

/* Legacy sentinel kept only so old callers still compile: phx_encode() no
 * longer returns it once the per-MB pipeline is wired.  Source: the old
 * "no data out" envelope at encoder.c:3354-3356. */
#define PHX_ERR_UNIMPLEMENTED (-2)

/* FDEC_STRIDE (common/common.h:571).  The reconstructed plane uses the same
 * stride so phx_mb_cache_t.p_fdec can be indexed exactly like x264's. */
#define PHX_FDEC_STRIDE 32

/* One raw input picture.  x264_picture_t's I420 planes, reduced to the fields
 * the ported path uses.  Source: reference/x264/x264.h:862-904 (x264_picture_t)
 * and reference/x264/input/raw.c:113-124 (plane layout).
 * SHADER: C — host-side view over caller-owned memory. */
typedef struct {
    uint8_t *plane[3];              /* Y, U, V (I420) */
    int i_stride[3];                /* plane strides in bytes */
    int i_width, i_height;          /* luma dimensions */
    int64_t i_pts;                  /* presentation timestamp */
} phx_picture_t;

/* The encoder handle.  Unlike x264_t this is an open, explicit struct: every
 * piece of state has one owner and one stage that mutates it (ORACLE_DESIGN
 * "State ownership").  Field origins are x264 encoder.c lines above.
 * SHADER: C — serial host state; CABAC (phx_cabac_t) stays in the entropy
 * stage and is never parallelised. */
typedef struct phx_t {
    phx_param_t param;              /* encoder.c:1516 — private copy of param */
    int i_width, i_height;          /* coded picture size (16-pixel aligned) */
    int i_width_display, i_height_display; /* x264 param.i_width/i_height: the
                                            * un-padded input size.  x264 codes
                                            * mod16 dimensions and signals the
                                            * remainder as SPS cropping (set.c:
                                            * :116/:161-172); only the display
                                            * size is read from the caller's
                                            * picture.  SHADER: C — scalar. */
    int i_mb_width, i_mb_height;    /* encoder.c:1590-1592 */
    phx_sps_t sps;                  /* encoder.c:1579 */
    phx_pps_t pps;                  /* encoder.c:1581 */
    uint8_t header_bytes[1024];     /* Annex-B SPS+PPS+SEI (encoder.c:2106-2122) */
    phx_nal_t header_nal[3];        /* SPS, PPS, SEI NALs handed out by phx_headers */
    uint8_t *bitstream;             /* encoder.c:1724 — out.p_bitstream */
    int i_bitstream;                /* encoder.c:1724 — out.i_bitstream */
    phx_nal_t nal[2];               /* encoder.c:2130 / per-frame NAL array */
    phx_frame_t *fenc;              /* encoder.c:3454 — frame being encoded */

    /* One-pass rate control (encoder.c:3857 start / :3978 end).  NULL on the
     * CQP path: x264's --qp form never allocates/starts ratecontrol, and the
     * parity streams must stay byte-identical.  Only an explicit
     * phx_encoder_configure_ratecontrol() call creates this handle, with
     * phx_param_t::rc_method != PHX_RC_CQP.  SHADER: C — one serial scalar
     * state machine, started before each slice and ended after its NAL. */
    phx_ratecontrol_t *rc;          /* ratecontrol.c:1431/1828 */
    int i_target_frame_bits;        /* cfg.bitrate / cfg.fps / 8, for start() */
    int i_last_frame_qp;            /* read-only view of the QP actually used */

    /* Serial lookahead pipeline.  x264 keeps every input picture alive in its
     * frame pool until the slice-type decision has consumed it
     * (encoder.c:3368/3426/3454; lookahead.c:192/223).  Phoenix owns the same
     * list explicitly: input_frames[] is display order, coded_types[]/frames[]
     * is the decided queue in coding order (slicetype.c:1950-1964 reorder).
     * SHADER: C — one queue, no threads. */
    phx_lookahead_t *lookahead;     /* encoder.c:3426 / lookahead.c:192 */
    phx_frame_t **input_frames;     /* encoder.c:3368 x264_frame_pop_unused */
    int n_input_frames, cap_input_frames;
    int i_next_frame;               /* encoder.c:3454 frames.current head */
    int *coded_types;               /* lookahead.c:223, coding order */
    int *coded_frames;              /* parallel indices into input_frames */
    int n_coded, cap_coded, i_coded_head;

    phx_mb_state_t *mb_state;       /* encoder.c:1590-1592 MB arrays */
    phx_mb_cache_t mb_cache;        /* encoder.c:2855 — h->mb.cache */
    phx_cost_tables_t cost;         /* encoder.c:1708 / analyse.c:179 */
    phx_mb_analysis_t analysis;     /* analyse.c:248-292 */
    phx_cabac_t cabac;              /* encoder.c:1669 / cabac.c:32 */
    uint8_t *rbsp;                  /* slice-header + MB scratch RBSP (encoder.c:1724) */
    uint8_t *recon;                 /* reconstructed plane, FDEC_STRIDE layout */
    phx_dpb_t *recon_dpb;           /* reconstructed/deblocked reference pictures */
    phx_frame_t *recon_frame;       /* full-picture reconstruction assembled per MB */
    int i_frame;                    /* encoder.c:1563 */
    int b_first_slice_nal;          /* first slice shares the header NAL array */
    int i_frame_num;                /* encoder.c:1564 */
    int i_poc;                      /* encoder.c:3513 (fenc->i_poc) */
    int i_idr_pic_id;               /* encoder.c:142 */
    int i_input;                    /* encoder.c:1630 */
    int i_last_idr;                 /* encoder.c:1628/3505 (frames.i_last_idr),
                                     * display-order frame number of last IDR */
    /* Input-frame index of the last coded reference picture.  x264 keeps a
     * DPB of reconstructed frames (encoder.c:3454/3882, common/frame.c);
     * Phoenix exposes the last reference's source samples through this index
     * so phx_me16() has a real list-0 picture.  SHADER: C — serial host state,
     * advanced once per reference NAL. */
    int i_last_ref_frame;

    /* Serial DPB mirror (encoder.c:2533-2606, 2295-2411).  One entry per
     * decoded picture kept as a short-term reference; frame_num/POC/display
     * number are the x264_frame_t fields read by reference_build_list().
     * `i_poc_l0ref0` is fdec->i_poc_l0ref0 (encoder.c:3869-3870).  The prev_*
     * record holds the previous frame's fdec until reference_update() at the
     * start of the next frame; prev_fc carries its MMCO commands so they can
     * be applied to the DPB exactly where x264 applies them.  SHADER: C —
     * one ordered push/remove per coded frame. */
#define PHX_ENC_DPB_MAX 16              /* common/base.h:137 (X264_REF_MAX) */
    struct {
        int i_frame_num, i_poc, i_frame, i_type;
        int b_kept_as_ref, b_duplicate, b_corrupt;
        int i_poc_l0ref0;
    } dpb[PHX_ENC_DPB_MAX];
    int n_dpb;
    phx_slice_frame_t prev_fc;
    int prev_type, prev_poc_l0ref0, prev_display_frame, b_prev_valid;
} phx_t;

/* x264_encoder_open (api.c:76 -> encoder.c:1507): validate params, copy them
 * into the handle, derive SPS/PPS + MB geometry, allocate frames, cost tables
 * and (when selected) CABAC contexts.  Returns NULL on invalid params/OOM.
 * SHADER: C — host-side serial initialisation (encoder.c:1552,1579,1581,
 * 1590,1669,1708). */
phx_t *phx_encoder_open(const phx_param_t *p);

/* Short alias required by docs/ORACLE_DESIGN.md:40.
 * SHADER: C — forwards to phx_encoder_open (api.c:76 / encoder.c:1507). */
phx_t *phx_open(const phx_param_t *p);

/* x264_encoder_headers (api.c:159 -> encoder.c:2096): emit SPS and PPS as
 * Annex-B NALs.  SEI (encoder.c:2118-2122) is not emitted yet — phx_headers
 * has no SEI writer.  *out points into handle-owned storage and stays valid
 * until the next phx_headers()/phx_close().  Returns the total byte count.
 * SHADER: C — serial header stage (encoder.c:2106-2133). */
int phx_headers(phx_t *h, phx_nal_t **out, int *nnal);

/* Direct byte-form of the above: calls phx_encoder_headers() (phx_headers.h:53,
 * the port of set.c:305/:505 + bitstream.c:55) with the handle's parameters.
 * SHADER: C — serial header stage. */
int phx_encoder_header_bytes(phx_t *h, uint8_t *dst, int capacity);

/* Enable one-pass rate control for a non-CQP encode.  This is the explicit
 * bridge from phx_ratecontrol.h into the encoder: phx_param_t has no bitrate
 * field, so x264's param.rc.i_bitrate/fps knobs (base.c:403, ratecontrol.c:757)
 * arrive through phx_ratecontrol_config_t instead.  Call it once, before the
 * first phx_encode().  Returns -1 for a CQP handle, a NULL/!bitrate-valid cfg,
 * or after a frame has been encoded; 0 on success.  When it is not called the
 * handle keeps the unchanged CQP constant-QP path.
 * AQ/MB-tree are intentionally left at the preset defaults (base.c:415/427):
 * x264 enables variance AQ and MB-tree for ABR/CRF and disables both only for
 * --qp (or for the presets that disable them, base.c:496/517).
 * SHADER: C — one-time host-side uniform setup (ratecontrol.c:750-825). */
int phx_encoder_configure_ratecontrol(phx_t *h, const phx_ratecontrol_config_t *cfg);

/* QP actually used by the most recent decided frame (ratecontrol.c:1748).
 * Returns -1 for a NULL handle or before the first frame.  Read-only: no
 * encoder state is advanced.  SHADER: C — scalar view. */
int phx_encoder_last_qp(const phx_t *h);

/* encoder.c:3499-3513 / 3881-3882 / 2613-2637 — derive one frame's slice
 * context from the decided frame type and the picture's *display-order*
 * number.  `h` owns the two counters (i_frame_num, i_last_idr) and the two
 * are advanced here exactly where x264 advances them: an IDR resets
 * frame_num before its header (encoder.c:3504) and stores i_last_idr (:3505);
 * POC is 2*(display_frame - max(i_last_idr,0)) (:3513); frame_num is
 * incremented only for reference pictures (:3882).  nal_ref_idc and the
 * slice type come from the frame-type branches at :3517-3554 (BREF keeps
 * NAL_PRIORITY_HIGH for normal B-pyramid, :3546).  Exposed so the golden
 * slice-header assertions in tests/test_phx_encoder.c can drive the same
 * derivation the encoder uses, with types/POC decoded from the x264 streams.
 * SHADER: C — serial scalar state advance; no hidden threading state. */
void phx_encoder_frame_context(phx_t *h, int decided_type, int display_frame,
                               phx_slice_frame_t *fc);

/* x264_encoder_encode (api.c:166 -> encoder.c:3323): stage order is
 *   put_frame/copy picture (encoder.c:3368-3426) ->
 *   lowres (mc.c:458) -> lookahead (lookahead.c:192/223, encoder.c:3445-3454)
 *   -> ratecontrol_start (encoder.c:3857) ->
 *   slice/MB loop (encoder.c:2855-2892: cache_load, analyse, encode,
 *   write_{cabac,cavlc}, cache_save) ->
 *   ratecontrol_end (encoder.c:3978) -> NAL assembly (encoder.c:2124).
 *
 * The wired path feeds each picture into phx_lookahead_put_frame()
 * (encoder.c:3426) and consumes decided frame types in coding order via
 * phx_lookahead_get_frames_full() (encoder.c:3447, lookahead.c:223,
 * slicetype.c:1950-1964), keeping the BREF tag and each frame's display
 * number.  One slice NAL is emitted per call once a decided
 * frame is available; early calls may return zero NALs while the lookahead
 * window fills, exactly like x264's i_delay gate (encoder.c:3428-3433).
 * SPS/PPS are NOT repeated here (x264.c:1977 emits them once).
 *
 * Slice headers carry the decided I/P/B type, frame_num, POC, ref counts and
 * marking flags (encoder.c:213-364).  The MB stage still falls back to
 * I_16x16 for P/B frames because inter ME is not ported in phx_mbencode.c:
 * the fallback is explicit and is NOT a claim of x264 bitstream parity.
 *
 * Passing pic == NULL is the x264 flush call (x264.c:2049): it drains the
 * lookahead one frame per call and finally returns 0 with *out=NULL, *nnal=0.
 * SHADER: C — the whole staged pipeline is serial and seek-safe. */
int phx_encode(phx_t *h, const phx_picture_t *pic,
               phx_nal_t **out, int *nnal, uint8_t **recon);

/* Mirror name for phx_encode (api.c:166 / encoder.c:3323).
 * SHADER: C — forwards to the serial phx_encode pipeline. */
int phx_encoder_encode(phx_t *h, const phx_picture_t *pic,
                       phx_nal_t **out, int *nnal, uint8_t **recon);

/* x264_encoder_close (api.c:130 -> encoder.c:4196): release cost tables
 * (encoder.c:4520), frames, MB arrays, bitstream and the handle.
 * SHADER: C — host-side serial teardown. */
void phx_encoder_close(phx_t *h);

/* Short alias required by docs/ORACLE_DESIGN.md:45.
 * SHADER: C — forwards to phx_encoder_close (api.c:130 / encoder.c:4196). */
void phx_close(phx_t *h);

#endif /* PHX_ENCODER_H */
