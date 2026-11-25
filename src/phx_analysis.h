/* Per-macroblock analysis driver — the explicit A/B boundary described by
 * docs/SHADER_MAPPING.md:67-70.  It fills one phx_mb_decision_t from the
 * already-loaded neighbour cache and the frame's source pixels; the serial
 * entropy stage consumes that structure later.
 *
 * Source followed:
 *   reference/x264/encoder/analyse.c:294   mb_analyse_init()
 *   reference/x264/encoder/analyse.c:668   mb_analyse_intra()
 *   reference/x264/encoder/analyse.c:586   mb_analyse_intra_chroma()
 *   reference/x264/encoder/analyse.c:3734  analyse_update_cache()
 *   reference/x264/encoder/analyse.c:2918  x264_macroblock_analyse()
 *   reference/x264/encoder/analyse.c:1255  mb_analyse_inter_p16x16()
 *   reference/x264/encoder/macroblock.h:132/168 i4x4/i8x8 encode
 *   reference/x264/encoder/macroblock.c:126  mb_encode_i16x16()
 *   reference/x264/common/pixel.c:265/290/334 SATD / SA8D
 *
 * TODO (see phx_analysis.c): real inter ME, chroma SATD, RD refinement and
 * trellis QNS are not representable with the inputs exposed by phx_frame_t.
 *
 * SHADER: B — per-MB workgroup stage; reads neighbour caches, writes one
 *              phx_mb_decision_t, no hidden state beyond the caller's cache.
 */
#ifndef PHX_ANALYSIS_H
#define PHX_ANALYSIS_H

#include "phx_param.h"
#include "phx_frame.h"
#include "phx_mbcache.h"
#include "phx_mb_decision.h"

#ifdef __cplusplus
extern "C" {
#endif

/* x264 encoder/analyse.c:2918 — x264_macroblock_analyse().  Analyse one MB at
 * (mb_x, mb_y) with QP `qp` and write the complete decision into `d`.
 * Returns 0 on success, -1 on invalid arguments / allocation failure.
 *
 * Decision layout (x264 common/base.h:180 scan8 cache layout, shared with
 * phx_mbencode.c and the entropy stage):
 *   nnz[k]  = non_zero_count[phx_scan8[k]] for k in [0,48) (x264 macroblock.c
 *             :1761); the three DC nnz flags are not representable (TODO).
 *   dct[phx_scan8[k]] = zigzag-scanned 4x4 coefficients of luma block k.
 *   dct[phx_scan8[48]] = dct[0] = the 4x4 Hadamard of the 16 I16x16 luma DC
 *             values (macroblock.c:215/:224).
 *   dct[phx_scan8[4i+j]] (j=0..3) = the 64 zigzag-scanned coefficients of
 *             8x8 block i, four 16-entry slots per 8x8 block.
 *   Intra predictor fields carry x264's raw mode indices: intra16_pred_mode
 *             in 0..6, intra4x4_pred_mode/intra8x8_pred_mode in 0..11.
 *             The signalled FIX4/FIX16 mapping is applied by the entropy
 *             stage, not by analysis (analyse.c:3739-3751).
 *   Chroma slots are zero until chroma planes are exposed (TODO).
 *
 * Intra prediction neighbours: if cache->p_fdec is non-NULL it is used as
 * x264's FDEC_STRIDE reconstruction buffer; otherwise a private buffer seeded
 * with 128 (x264 common/macroblock.c:501-548) is used and the outer neighbour
 * samples are proxied from f->plane (TODO: pass real reconstructed samples).
 *
 * SHADER: B — one invocation per MB; deterministic and seek-safe given the
 *              same frame, cache and parameters.
 */
int phx_macroblock_analyse(const phx_param_t *p, const phx_frame_t *f,
                           phx_mb_cache_t *cache, int mb_x, int mb_y,
                           int qp, phx_mb_decision_t *d);

#ifdef __cplusplus
}
#endif

#endif
