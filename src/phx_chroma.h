/* Phase-B 4:2:0 chroma macroblock encoder.
 *
 * Authoritative source (vendored x264), followed line by line:
 *   reference/x264/encoder/macroblock.c:259-498  mb_encode_chroma_internal
 *   reference/x264/encoder/macroblock.c:925-941  chroma prediction call site
 *   reference/x264/encoder/macroblock.c:34-96    chroma DC helpers
 *   reference/x264/encoder/macroblock.c:115-121  nnz correctness contract
 *   reference/x264/encoder/macroblock.c:945-951  cbp / CABAC DC flags
 *   reference/x264/common/frame.c:45-57          I420 -> NV12 internal CSP
 *   reference/x264/common/frame.c:89-99/239-243  NV12 plane layout
 *   reference/x264/common/quant.c:96-104         quant_2x2_dc
 *   reference/x264/common/macroblock.h:249-270   chroma QP table
 *   reference/x264/common/predict.h:34-44        intra_chroma_pred_e
 *   reference/x264/common/predict.c:167-299/936-945  8x8c predictors
 *   reference/x264/common/base.h:177-180         CHROMA_DC / x264_scan8
 *   reference/x264/common/set.h:32/34            CQM_4IY / CQM_4IC
 *
 * SHADER: B — one serial invocation per macroblock; fills the B->C chroma
 * decision fields and reconstructs into the NV12 chroma plane.
 */
#ifndef PHX_CHROMA_H
#define PHX_CHROMA_H

#include "phx_param.h"
#include "phx_frame.h"
#include "phx_mbcache.h"
#include "phx_mb_decision.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Encode and reconstruct the 8x8 chroma blocks of one 4:2:0 macroblock.
 *
 * Reads the source samples from f->planes[1] (NV12 U/V pairs) at
 * (mb_x*8, mb_y*8), predicts with d->chroma_pred_mode using the reconstructed
 * top/left neighbours from that same plane, and writes back the reconstructed
 * U/V samples into those pairs.  d->chroma_dct[ch][0] is the zigzag-scanned
 * 2x2 DC block, [ch][1..4] the four zigzag-scanned 4x4 AC blocks;
 * d->chroma_nnz[ch][b] is the 0/1 flag for those five blocks.  x264's chroma
 * AC block indices 16+ch*16+i (macroblock.c:408) are mirrored into
 * d->nnz[16+ch*16+i] and cache->non_zero_count[phx_scan8[16+ch*16+i]].
 * Chroma DC nnz goes to cache->non_zero_count[phx_scan8[49+ch]] and to
 * d->chroma_nnz[ch][0]; phx_mb_decision_t::nnz[48] cannot address CHROMA_DC
 * (base.h:178: 49/50) and is deliberately not written for DC.
 *
 * Returns 0 on success, -1 for an invalid argument, mode, QP or a frame that
 * is not 4:2:0 NV12.  No bitstream parity is claimed by this function.
 *
 * SHADER: B — completes the chroma half of the B-stage decision record.
 */
int phx_macroblock_encode_chroma(const phx_param_t *p, phx_frame_t *f,
                                 phx_mb_cache_t *cache, int mb_x, int mb_y,
                                 phx_mb_decision_t *d);

#ifdef __cplusplus
}
#endif
#endif
