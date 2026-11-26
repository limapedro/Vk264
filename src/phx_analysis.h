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
 *   reference/x264/encoder/analyse.c:1255-1332 mb_analyse_inter_p16x16()
 *   reference/x264/common/mvpred.c:129-163  x264_mb_predict_mv_16x16()
 *   reference/x264/encoder/me.c:276-419      full-pel x264_me_search_ref()
 *   reference/x264/encoder/macroblock.h:132/168 i4x4/i8x8 encode
 *   reference/x264/encoder/macroblock.c:126  mb_encode_i16x16()
 *   reference/x264/common/pixel.c:55-73/265/290/334 SAD / SATD / SA8D
 *
 * TODO (see phx_analysis.c): sub-pel ME, chroma ME, RD refinement and
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
#include "phx_cabac.h"        /* phx_cabac_t, trellis context read-only */
#include "phx_slice.h"        /* PHX_SLICE_I/P/B, encoder.c:3522/3530/3539 */

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
 *   chroma_pred_mode is derived from the NV12 U/V SATD of analyse.c:586-665
 *             (availability table :485-492/:537-542, predictor :650-651,
 *             cost :632/:655-657).  Chroma coefficients are not emitted by
 *             x264's analysis; x264_macroblock_encode_chroma
 *             (encoder/macroblock.c:259-498/:940) fills chroma_dct/nnz and
 *             the scan8[16..47]/[49,50] nnz state afterwards.
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

/* Slice-aware driver used by phx_encode().  `slice_type` is the decoded
 * PHX_SLICE_* value (encoder.c:3522/3530/3539); `fref` is list-0 reference 0
 * for P/B slices and may be NULL for an I slice.  For P slices this runs
 * phx_me16() (encoder/me.c:276-419) from the neighbour MV predictor
 * (common/mvpred.c:129-163), compares its SAD+MV-bit cost against the intra
 * SATD costs exactly as analyse.c:3200-3223 does, and publishes either a
 * PHX_P_L0/D_16x16 decision (mv/ref, cbp=0) or the winning intra decision.
 * P_SKIP is produced only when the pskip MV probe of analyse.c:1296-1301 and
 * mvpred.c:166-181 succeeds and the prediction residual is exactly zero.
 * `cb` is the live CABAC coder (analyse.c:261 switches trellis on only when
 * i_trellis>1 && mbrd>=1) and supplies the COPY_CABAC context states
 * (rdo.c:71-73) for the mbrd>=1 I_4x4 RD refinement of analyse.c:1086-1135.
 * The refinement uses the exact size-only bit accumulator exposed by
 * phx_rdo.h (common/cabac.h:99-124, rdo.c:31-69/315-347); it never copies or
 * advances the arithmetic coder.  Pass NULL for standalone analysis (plain
 * deadzone quant, SATD-only mode choice).
 *
 * This is an additive API: the seven-argument phx_macroblock_analyse() above
 * remains the I-slice wrapper so standalone callers keep their ABI.
 * SHADER: B — explicit slice type/reference, no hidden encoder state. */
int phx_macroblock_analyse_slice(const phx_param_t *p, const phx_frame_t *fenc,
                                 const phx_frame_t *fref,
                                 phx_mb_cache_t *cache, phx_cabac_t *cb,
                                 int mb_x, int mb_y, int qp, int slice_type,
                                 phx_mb_decision_t *d);

#ifdef __cplusplus
}
#endif

#endif
