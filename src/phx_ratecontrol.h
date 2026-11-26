/* Phoenix rate control — function-for-function CPU port of x264's one-pass
 * CQP / CRF / ABR QP derivation and adaptive quantization.
 *
 * Source lines (all paths relative to reference/x264/):
 *   encoder/ratecontrol.c:199-210  qp2qscale / qscale2qp
 *   encoder/ratecontrol.c:225-302  ac_energy_var / ac_energy_plane / ac_energy_mb
 *   encoder/ratecontrol.c:304-415  x264_adaptive_quant_frame
 *   encoder/ratecontrol.c:632-646  x264_ratecontrol_init_reconfigurable (CRF RF constant)
 *   encoder/ratecontrol.c:750-825  x264_ratecontrol_new (ABR/QP constants, ip/pb offsets)
 *   encoder/ratecontrol.c:1410-1420 accum_p_qp_update
 *   encoder/ratecontrol.c:1431-1538 x264_ratecontrol_start
 *   encoder/ratecontrol.c:1748-1768 x264_ratecontrol_qp / x264_ratecontrol_mb_qp
 *   encoder/ratecontrol.c:1828-1844 x264_ratecontrol_end ( averages )
 *   encoder/ratecontrol.c:1915-1928 x264_ratecontrol_end (ABR complexity update)
 *   encoder/ratecontrol.c:2003-2035 get_qscale
 *   encoder/ratecontrol.c:2400-2455 / 2545-2638 rate_estimate_qscale (1pass ABR/CRF)
 *   common/common.h:58-64          QP_BD_OFFSET / QP_MAX_SPEC / SPEC_QP (8-bit build)
 *   common/base.h:211-230          x264_clip3f / x264_exp2fix8 / x264_log2
 *   common/tables.c:58-90          x264_exp2_lut / x264_log2_lut / x264_log2_lz_lut
 *   common/macroblock.h:249, encoder/set.c:1585  chroma QP lookup table
 *
 * SHADER: C — serial, per-frame scalar derivation.  The AQ pass is a
 * deterministic read-only reduction over the fenc planes; every MB offset is
 * an independent output element, so it maps to one workgroup element each.
 *
 * Golden scope and limitations (worker M15):
 *   Golden: instrumented x264 (hooks at ratecontrol.c:1431/:1829/:304/:1754,
 *   slicetype.c:1029) on a fixed 64x64/8-frame fixture drives
 *   CQP / CRF / ABR (all --no-mbtree) and CRF with dumped MB-tree offsets.
 *   Tests assert the frame QP sequence, per-MB QP, AQ offsets (float bits +
 *   inv qscale) and the chroma QP table bit-for-bit.
 *   - CRF/ABR B frames use the last P QP (ratecontrol.c:2445) rather than the
 *     two adjacent references' f_qp_avg_rc (:2426-2450): no POC/ref-QP history
 *     is exposed by this API.  CQP BREF midpoint (:1508-1511) stays in
 *     phx_encoder.c, which owns that path.
 *   - Frame duration is cfg.duration (default 1/fps); VFR f_duration is not
 *     available from phx_param_t.
 *   - VBV / row RC / 2-pass are not ported: cbr_decay is 1 (:1925/:1927) and
 *     vbv_pass1 (:2631) is a no-op.
 *   - MB-tree quant offsets are an explicit input (set_quant_offsets); the
 *     lookahead propagation in slicetype.c:1029-1183 is owned by another
 *     worker.  qpa_aq (:1596) sums f_qp_offset for all MBs; x264 uses
 *     f_qp_offset_aq for non-reference frames.  Neither value feeds the QP
 *     sequence in 1-pass.
 *   - CQP AQ: x264's encoder.c:951-966 disables i_aq_mode and b_mb_tree for
 *     every CQP encode (not only qp=0).  The per-MB QP increment seen with
 *     "--qp 28" on B frames is the CQP pb_factor offset (ratecontrol.c:825,
 *     :1508-1511), not AQ.  phx_cli.c must keep i_aq_mode=0 for CQP.
 *   - With MB-tree, CQP intentionally keeps phx_encoder.c:473-475's double
 *     log2 mapping and the caller's f_pb_factor, as required by the contract;
 *     CRF/ABR apply x264's :762-766 f_pb_factor=1 override.
 */
#ifndef PHX_RATECONTROL_H
#define PHX_RATECONTROL_H

#include <stdint.h>
#include "phx_param.h"
#include "phx_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* common/base.h:111-116 — x264 only emits P/B/I.  Identical to
 * phx_slice.h:30-32 so callers may pass PHX_SLICE_* directly. */
#define PHX_RC_SLICE_P 0
#define PHX_RC_SLICE_B 1
#define PHX_RC_SLICE_I 2

/* x264.h RC method values as mirrored in phx_param.h:20-22. */
#define PHX_RC_AQ_NONE             0
#define PHX_RC_AQ_VARIANCE         1
#define PHX_RC_AQ_AUTOVARIANCE     2
#define PHX_RC_AQ_AUTOVARIANCE_BIASED 3

/* common/common.h:58-60, 8-bit build: QP_BD_OFFSET=0, QP_MAX_SPEC=51. */
#define PHX_RC_QP_MIN  0
#define PHX_RC_QP_MAX  51
#define PHX_RC_QP_INTERNAL_MAX 69  /* QP_MAX = QP_MAX_SPEC+18, common.h:60 */

typedef struct phx_ratecontrol_t phx_ratecontrol_t;

/* x264's one-pass knobs that are not present in phx_param_t.  Defaults are
 * x264's: base.c:403 (bitrate=0), base.c:404 (rate_tolerance=1.0),
 * base.c:410-412 (qp_min=0, qp_max=INT_MAX -> clipped to QP_MAX_SPEC,
 * qp_step=4), base.c:423 (qcompress=0.6), ratecontrol.c:757-760 (fps=25).
 * ratecontrol.c:762-768 overrides qcompress to 1.0 when MB-tree is enabled.
 *
 * SHADER: C — host-side configuration, uploaded to the shader as uniforms. */
typedef struct {
    double bitrate;         /* bits/s = x264 i_bitrate * 1000 (base.c:403, rc.c:770) */
    double fps;             /* frames/s (ratecontrol.c:757-760) */
    double duration;        /* seconds per frame; <=0 uses 1/fps (rc.h:34-40) */
    double qcompress;       /* base.c:423; ignored (forced to 1) with MB-tree */
    double rate_tolerance;  /* base.c:404; floored at .01 (ratecontrol.c:801-805) */
    int qp_min;             /* base.c:410 */
    int qp_max;             /* base.c:411, clipped to PHX_RC_QP_MAX */
    int qp_step;            /* base.c:412; lstep = 2^(qp_step/6), rc.c:828 */
} phx_ratecontrol_config_t;

/* ratecontrol.c:750-825 + :632-646.  mb_count must match p->i_width/16 *
 * p->i_height/16 when those fields are set.  Returns 0 on success, -1 on
 * invalid arguments or allocation failure.  *rc is NULL on failure.
 * SHADER: C — one-time per-encode uniform setup. */
int phx_ratecontrol_init(phx_ratecontrol_t **rc, const phx_param_t *p, int mb_count);

/* ratecontrol.c:1358-1408 (x264_ratecontrol_delete). */
void phx_ratecontrol_delete(phx_ratecontrol_t *rc);

/* Optional: override the x264 defaults above before the first frame.
 * Returns 0 on success, -1 for a NULL handle or an out-of-range value.
 * SHADER: C — uniform setup. */
int phx_ratecontrol_configure(phx_ratecontrol_t *rc, const phx_ratecontrol_config_t *cfg);

/* Optional real per-frame SATD (x264_rc_analyse_slice at ratecontrol.c:2559).
 * When unset, phx_ratecontrol_start() uses its frame_bits argument as the
 * complexity sample; this is the only input substitution in the port and is
 * reported as a limitation, not a tuned approximation. */
int phx_ratecontrol_set_complexity(phx_ratecontrol_t *rc, int satd);

/* Optional MB-tree quant offsets (x264_adaptive_quant_frame's quant_offsets,
 * ratecontrol.c:319-325 / :397-398).  The array must have mb_count floats and
 * is copied.  Pass NULL to clear. */
int phx_ratecontrol_set_quant_offsets(phx_ratecontrol_t *rc, const float *quant_offsets);

/* ratecontrol.c:1431-1538.  slice_type is PHX_RC_SLICE_{P,B,I}.
 * frame_bits is the planned/reported size of this frame; it feeds the ABR
 * complexity/overflow state (ratecontrol.c:2479-2480, :2560-2563).
 * qp_out receives x264_ratecontrol_qp() (ratecontrol.c:1748-1752): the frame
 * QP before per-MB AQ offsets.  Returns 0 on success, -1 on bad arguments.
 * SHADER: C — serial scalar derivation, one call per decided frame. */
int phx_ratecontrol_start(phx_ratecontrol_t *rc, int slice_type, int frame_bits, int *qp_out);

/* ratecontrol.c:1828-1928.  Consumes the actual encoded frame size and updates
 * cplxr_sum / wanted_bits_window.  Returns 0 on success, -1 on bad arguments.
 * SHADER: C — serial scalar derivation. */
int phx_ratecontrol_end(phx_ratecontrol_t *rc, int slice_type, int bits);

/* ratecontrol.c:304-415 (x264_adaptive_quant_frame).  Computes
 * f_qp_offset[], f_qp_offset_aq[] and i_inv_qscale_factor[] for one fenc
 * frame using the scalar C ac_energy_mb() path (ratecontrol.c:225-302,
 * common/pixel.c:183-201, common/mc.c:402-405).  Returns 0 on success,
 * -1 on bad arguments / frame-size mismatch.
 * SHADER: C — one reduction + one independent offset write per MB. */
int phx_ratecontrol_aq_frame(phx_ratecontrol_t *rc, phx_frame_t *f, const phx_param_t *p);

/* ratecontrol.c:1506-1511 — CQP BREF midpoint.  x264 averaged qp_constant[B]
 * and qp_constant[P] when the B frame is kept as a reference.  Set this before
 * phx_ratecontrol_start() for a CQP B slice; 0 for a normal B/P/I slice.
 * SHADER: C — one scalar uniform. */
void phx_ratecontrol_set_kept_as_ref(phx_ratecontrol_t *rc, int b_kept_as_ref);

/* ratecontrol.c:1754-1768 — frame QP plus the MB's AQ offset (emergency-mode
 * scaling included).  frame_qp is h->rc->qpm (the float frame QP), so pass
 * the value reported by x264_ratecontrol_start's qp_out only when the frame
 * QP is integral; the golden harness passes the exact float.
 * b_kept_as_ref selects f_qp_offset vs f_qp_offset_aq; with AQ disabled by
 * CQP (encoder.c:964) the returned QP is exactly the clipped frame QP.
 * Returns a clipped QP, or -1 on bad arguments. */
int phx_ratecontrol_mb_qp(const phx_ratecontrol_t *rc, int mb_xy, float frame_qp, int b_kept_as_ref);

/* analyse.c:255-259 — the lambda actually used by mb_analyse_* is a direct
 * x264_lambda_tab[qp] lookup (tables.c:97-109).  Exposed so a caller holding
 * phx_ratecontrol_mb_qp()'s result can derive the exact ME/RD lambda without
 * reimplementing the table.  Returns -1 for an out-of-range QP.
 * SHADER: C — pure table lookup. */
int phx_ratecontrol_lambda(int qp);

/* Read-only views of the AQ outputs (ratecontrol.c:399-402). */
const float *phx_ratecontrol_qp_offsets(const phx_ratecontrol_t *rc);
const float *phx_ratecontrol_qp_offsets_aq(const phx_ratecontrol_t *rc);
const uint16_t *phx_ratecontrol_inv_qscale(const phx_ratecontrol_t *rc);

/* ratecontrol.c:823-825 — the three CQP constants, for verification. */
int phx_ratecontrol_qp_constant(const phx_ratecontrol_t *rc, int slice_type);

/* common/common.h:58-64 + common/macroblock.h:249 + encoder/set.c:1585 —
 * luma QP -> chroma QP for phx_param_t::i_chroma_qp_index_offset.  Uses the
 * same phx_chroma_qp_base[] table as phx_cost.c:106-110 and phx_chroma.c:224.
 * SHADER: C — pure lookup. */
int phx_ratecontrol_chroma_qp(const phx_param_t *p, int qp);

#ifdef __cplusplus
}
#endif

#endif /* PHX_RATECONTROL_H */
