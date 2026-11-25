/* Integer (full-pel) motion estimation for P slices — a function-for-function
 * port of the full-pel half of x264's x264_me_search_ref().
 *
 * Source followed (all under reference/x264/; line numbers are exact):
 *   encoder/me.c:52-56          mod6m1 / hex2 / square1 search patterns
 *   encoder/me.c:60-70          BITS_MVD / COST_MV (SAD + MV bits)
 *   encoder/me.c:81-92          COST_MV_X3_DIR
 *   encoder/me.c:94-107         COST_MV_X4_DIR
 *   encoder/me.c:178-180        FPEL / SPEL / SPELx2 (quarter-pel conversion)
 *   encoder/me.c:182-212        search state, mv window, p_cost_mvx/mvy
 *   encoder/me.c:276-317        full-pel MVP branch (subme < 3)
 *   encoder/me.c:322-341        X264_ME_DIA diamond search
 *   encoder/me.c:344-419        X264_ME_HEX hexagon + square refine
 *   encoder/me.c:774-784        qpel MV / cost output (subme < 3 branch)
 *   encoder/analyse.c:143-177   init_costs() — MV cost table construction
 *   encoder/analyse.c:186-188   log2 cost model used by that table
 *   common/pixel.c:55-73        PIXEL_SAD_C( x264_pixel_sad_16x16, 16, 16 )
 *   common/tables.c:97,102      x264_lambda_tab (via phx_lambda_tab)
 *
 * MV bit costs come from phx_cost_tables_t::cost_mv (built by
 * phx_analyse_init_costs(), analyse.c:179).  phx_me16 is parameterised by
 * `lambda` rather than by a QP, so it resolves the table whose
 * phx_lambda_tab[qp] == lambda; if no table matches it falls back to the exact
 * log2 formula of analyse.c:186-188/156 so the function still works standalone.
 *
 * Out of scope (documented limitations, NOT guessed):
 *   - sub-pel / HPEL / QPEL refinement (me.c:58, 786+)
 *   - chroma ME, weighted prediction, multi-ref, bidirectional ME
 *   - UMH / ESA / TESA (method values >= 2), SATD / RD cost functions
 *   - MV predictors derived from neighbours (the caller supplies `pred`)
 * No stream parity is claimed; inter-MB syntax integration is the caller's job.
 *
 * SHADER: A — host-side per-MB search.  It reads two luma planes plus a
 *              constant cost table and writes exactly one MV and one cost;
 *              there is no hidden state and the result is deterministic /
 *              seek-safe for identical inputs.  The SAD and MV-cost lookups
 *              are the pure primitives the B-stage mode decision consumes.
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
 *   PHX_ME_DIA = 0, PHX_ME_HEX = 1.  Higher values are rejected. */

/* Search one 16x16 luma block of `fenc` at coded-plane position (x, y)
 * against `fref` for an integer MV.
 *
 *   x, y      : top-left of the 16x16 block in luma-plane pixels (x264 mb_x*16).
 *   pred[2]   : motion-vector predictor in QUARTER-PEL units (x264 m->mvp,
 *               me.h:50).  FPEL(pred) is the full-pel starting point
 *               (me.c:279-280); pred is also the centring offset of the
 *               cost_mv table (me.c:211-212).
 *   mv_range  : full-pel search radius (x264 i_me_range; analyse.i_mv_range).
 *   lambda    : x264 i_lambda for the active QP (tables.c / phx_lambda_tab).
 *   method    : PHX_ME_DIA or PHX_ME_HEX; other values return -1.
 *   out_mv[2] : best MV in QUARTER-PEL units, i.e. 4 * full-pel MV
 *               (x264_me_t.mv, me.h:55).  Divide by 4 to get full-pel.
 *
 * Returns the best cost (SAD + lambda*MVbits, x264_me_t.cost semantics) on
 * success, or -1 for invalid arguments / unsupported method.
 *
 * Caller must have prepared fref's padded luma border (x264 uses a widened
 * p_fref_w window); phx_me16 never writes to either frame.
 */
int phx_me16(int x, int y, const phx_frame_t *fenc, const phx_frame_t *fref,
             const phx_cost_tables_t *cost, int mv_range, int lambda,
             int method, const int16_t pred[2], int *out_mv);

#ifdef __cplusplus
}
#endif

#endif
