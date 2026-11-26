/* Integer (full-pel) + sub-pel motion estimation for P slices — a
 * function-for-function port of x264's x264_me_search_ref(), refine_subpel()
 * and the hpel/qpel interpolation they call.
 *
 * Source followed (all under reference/x264/; line numbers are exact):
 *   encoder/me.c:52-56          mod6m1 / hex2 / square1 search patterns
 *   encoder/me.c:60-70          BITS_MVD / COST_MV (SAD + MV bits)
 *   encoder/me.c:81-92          COST_MV_X3_DIR
 *   encoder/me.c:94-107         COST_MV_X4_DIR
 *   encoder/me.c:109-126        COST_MV_X4 (absolute centre, UMH)
 *   encoder/me.c:128-141        COST_MV_X3_ABS
 *   encoder/me.c:143-150        DIA1_ITER
 *   encoder/me.c:152-176        CROSS
 *   encoder/me.c:178-180        FPEL / SPEL / SPELx2 (quarter-pel conversion)
 *   encoder/me.c:182-212        search state, mv window, p_cost_mvx/mvy,
 *                               pack16to32_mask2 / CHECK_MVRANGE
 *   encoder/analyse.c:330-339   i_fmv_range / mv_limit_fpel[] (x limits)
 *   encoder/analyse.c:391-396   mv_limit_fpel[] (y limits), i_fpel_border=6
 *   encoder/me.c:276-317        full-pel MVP branch (subme < 3)
 *   encoder/me.c:322-341        X264_ME_DIA diamond search
 *   encoder/me.c:344-419        X264_ME_HEX hexagon + square refine
 *   encoder/me.c:422-616        X264_ME_UMH uneven-cross multi-hexagon grid
 *   encoder/me.c:774-798        qpel MV / cost output + subpel dispatch
 *   encoder/me.c:801-810        x264_me_refine_qpel() (SAD-only refinement)
 *   encoder/me.c:817-863        COST_MV_SAD / COST_MV_SATD
 *   encoder/me.c:865-992        refine_subpel() hpel + qpel diamond
 *   common/mc.c:173-196         hpel_filter() (6-tap filter + downsample)
 *   common/mc.c:198-219         mc_luma() (qpel interpolation/copy)
 *   common/mc.c:221-249         get_ref() (zero-copy or averaged reference)
 *   common/mc.c:704-726         x264_frame_filter() (filtered plane layout)
 *   common/tables.c:183-184     x264_hpel_ref0 / x264_hpel_ref1
 *   common/pixel.c:55-73        PIXEL_SAD_C( x264_pixel_sad_16x16, 16, 16 )
 *   common/pixel.c:242-327      Hadamard + SATD (16x16) for the satd path
 *   encoder/analyse.c:143-177   init_costs() — MV cost table construction
 *   encoder/analyse.c:186-188   log2 cost model used by that table
 *   common/tables.c:97,102      x264_lambda_tab (via phx_lambda_tab)
 *
 * MV bit costs come from phx_cost_tables_t::cost_mv (built by
 * phx_analyse_init_costs(), analyse.c:179).  phx_me16 is parameterised by
 * `lambda` rather than by a QP, so it resolves the table whose
 * phx_lambda_tab[qp] == lambda; if no table matches it falls back to the exact
 * log2 formula of analyse.c:186-188/156 so the function still works standalone.
 *
 * Out of scope (documented limitations, NOT guessed):
 *   - chroma ME, weighted prediction, multi-ref, bidirectional ME
 *   - ESA / TESA (method values >= 3), SATD/RD fullpel cost functions
 *   - MV predictors derived from neighbours (the caller supplies `pred`)
 * No stream parity is claimed; inter-MB syntax integration is the caller's job.
 *
 * SHADER: A — host-side per-MB search and interpolation.  It reads two luma
 *              planes (plus optional filtered planes) and a constant cost
 *              table, and writes one MV and one cost; there is no hidden state
 *              and the result is deterministic / seek-safe for identical
 *              inputs.  The SAD, MV-cost lookup and 6-tap filter are the pure
 *              primitives the B-stage mode decision consumes.
 */
#ifndef PHX_ME_H
#define PHX_ME_H

#include <stdint.h>
#include "phx_frame.h"
#include "phx_cost.h"

#ifdef __cplusplus
extern "C" {
#endif

/* x264 encoder/me.h:30 — COST_MAX, also the error/sentinel return value. */
#define PHX_ME_COST_MAX (1 << 28)

/* Method values mirror phx_me_method_t (phx_param.h:13, base.h:435):
 *   PHX_ME_DIA = 0, PHX_ME_HEX = 1, PHX_ME_UMH = 2.  ESA/TESA are rejected. */

/* Search one 16x16 luma block of `fenc` at coded-plane position (x, y)
 * against `fref` for an integer MV.  DIA, HEX and UMH are all exact ports.
 *
 *   x, y      : top-left of the 16x16 block in luma-plane pixels (x264 mb_x*16).
 *   pred[2]   : motion-vector predictor in QUARTER-PEL units (x264 m->mvp,
 *               me.h:50).  FPEL(pred) is the full-pel starting point
 *               (me.c:279-280); pred is also the centring offset of the
 *               cost_mv table (me.c:211-212).
 *   mv_range  : full-pel search radius (x264 i_me_range; analyse.i_mv_range).
 *   lambda    : x264 i_lambda for the active QP (tables.c / phx_lambda_tab).
 *   method    : PHX_ME_DIA, PHX_ME_HEX or PHX_ME_UMH; other values return -1.
 *   out_mv[2] : best MV in QUARTER-PEL units, i.e. 4 * full-pel MV
 *               (x264_me_t.mv, me.h:55).  Divide by 4 to get full-pel.
 *
 * Returns the best cost (SAD + lambda*MVbits, x264_me_t.cost semantics) on
 * success, or -1 for invalid arguments / unsupported method.
 *
 * Caller must have prepared fref's padded luma border (x264 uses a widened
 * p_fref_w window); phx_me16 never writes to either frame.
 *
 * SHADER: A — see header. */
int phx_me16(int x, int y, const phx_frame_t *fenc, const phx_frame_t *fref,
             const phx_cost_tables_t *cost, int mv_range, int lambda,
             int method, const int16_t pred[2], int *out_mv);

/* x264_frame_t.filtered[p] (frame.h:67 / mc.c:704-726): four luma planes with
 * the SAME stride and origin, [0] = source plane (borrowed, never freed),
 * [1] = horizontal (H), [2] = vertical (V), [3] = centre (C) half-pel planes.
 * p[1..3] are allocated by phx_me_refplanes_init() and released by
 * phx_me_refplanes_free().  SHADER: A — pure preparation, no hidden state. */
typedef struct {
    uint8_t *p[4];
    uint8_t *owned[3];          /* backing allocations for p[1], p[2], p[3] */
    int i_stride;
} phx_me_refplanes_t;

/* x264_frame_filter() (common/mc.c:704-726) for a complete progressive frame:
 * start = -8, height = i_height + 16, offs = start*stride - 8, then
 * hpel_filter() (mc.c:174-196) on width+16 columns.  Returns 0 / -1 (OOM). */
int phx_me_refplanes_init(const phx_frame_t *fref, phx_me_refplanes_t *out);
void phx_me_refplanes_free(phx_me_refplanes_t *out);

/* common/mc.c:198-219 mc_luma() + :221-249 get_ref(), 8-bit C path.
 * Interpolate a `width`x`height` luma block for quarter-pel MV (mvx,mvy) into
 * `dst`.  src1/src2 come from x264_hpel_ref0/x264_hpel_ref1 (tables.c:183-184);
 * qpel samples are averaged with (a+b+1)>>1 (mc.c:56).  `mvx`/`mvy` are
 * quarter-pel units relative to the caller's reference-plane origin. */
void phx_mc_luma16(const phx_me_refplanes_t *ref, int mvx, int mvy,
                   uint8_t *dst, intptr_t dst_stride, int width, int height);

/* me.c:865-992 refine_subpel().  `subme` is h->mb.i_subpel_refine; the
 * hpel/qpel iteration counts come from subpel_iterations[][] (me.c:38-50).
 * `b_refine_qpel` is the macro's last argument: 1 reproduces
 * x264_me_refine_qpel() (me.c:801-810, SAD hpel + SATD qpel for subme>=2);
 * 0 reproduces the call made by x264_me_search_ref() (me.c:792-797), which
 * additionally re-seeds bcost with SATD at me.c:925-929.  `mv`/`cost` are the
 * fullpel search result in/out (quarter-pel MV and SAD/SATD+lambda*bits);
 * `cost_mv` receives the final p_cost_mvx[bmx]+p_cost_mvy[bmy] (me.c:991).
 * MV limits are the exact analyse.c:330-339/391-396 spel limits derived from
 * (mb_x,mb_y) and the frame's MB grid.  Returns 0 on success, -1 on invalid
 * arguments.  Chroma ME is out of scope (b_chroma_me == 0). */
int phx_me_refine_subpel(int mb_x, int mb_y, int mb_width, int mb_height,
                         const phx_frame_t *fenc,
                         const phx_me_refplanes_t *fref,
                         const phx_cost_tables_t *cost, int i_mv_range,
                         int lambda, int subme, int b_refine_qpel,
                         const int16_t mvp[2],
                         int mv[2], int *cost_out, int *cost_mv_out);

#ifdef __cplusplus
}
#endif

#endif
