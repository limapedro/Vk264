/* Phase-B macroblock encode/reconstruct boundary.
 *
 * Consumes one phx_mb_decision_t produced by the analysis shaders and writes
 * quantized coefficients, nnz flags and reconstructed samples.  This is the
 * exact serial-per-MB stage that feeds the phase-C entropy writers.
 *
 * Source (authoritative, vendored x264):
 *   reference/x264/encoder/macroblock.c:974   x264_macroblock_encode
 *   reference/x264/encoder/macroblock.c:618   macroblock_encode_internal
 *   reference/x264/encoder/macroblock.c:126   mb_encode_i16x16
 *   reference/x264/encoder/macroblock.c:259   mb_encode_chroma_internal
 *   reference/x264/encoder/macroblock.h:112   STORE_8x8_NNZ
 *   reference/x264/encoder/macroblock.h:132   x264_mb_encode_i4x4
 *   reference/x264/encoder/macroblock.h:168   x264_mb_encode_i8x8
 *   reference/x264/common/base.h:177/178      LUMA_DC / CHROMA_DC
 *   reference/x264/common/macroblock.h:180    x264_scan8
 *   reference/x264/common/macroblock.c:1680   macroblock_cache_save
 *
 * Coefficient/nnz layout (chosen so both arrays stay inside their 48 entries):
 *   slot = phx_scan8[block]  for luma 4x4 block 0..15  (scan8 slots 12..47)
 *   slot = phx_scan8[48] = 0 for the I16x16 luma DC block.
 *   d->dct[slot][16] holds zigzag-scanned QUANTIZED coefficients (frame scan);
 *   d->nnz[slot] and cache->non_zero_count[slot] are the 0/1 nnz flags written
 *   with x264's STORE_8x8_NNZ semantics (encoder/macroblock.h:112-117).
 *   Chroma DC slots (scan8[49]/scan8[50]) and chroma 4x4 coefficients are
 *   outside phx_mb_decision_t's 48-entry arrays and are not written yet.
 *
 * SHADER: B — one workgroup invocation per MB; emits the B->C decision record.
 */
#ifndef PHX_MBENCODE_H
#define PHX_MBENCODE_H

#include "phx_param.h"
#include "phx_frame.h"
#include "phx_mbcache.h"
#include "phx_mb_decision.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reconstruct one macroblock from d and write the result back into f->plane
 * at (mb_x*16, mb_y*16).  Mirrors x264_macroblock_encode
 * (reference/x264/encoder/macroblock.c:974) and the intra branches of
 * macroblock_encode_internal (:618, :706-768); cache nnz/cbp/transform-size
 * writes mirror macroblock_cache_save
 * (reference/x264/common/macroblock.c:1680, :1761-1779).
 * Returns 0 on success and -1 for an unsupported or invalid mode/argument.
 *
 * SHADER: B — serial-per-MB reconstruction that completes the B-stage record.
 */
int phx_macroblock_encode(const phx_param_t *p, phx_frame_t *f,
                          phx_mb_cache_t *cache, int mb_x, int mb_y,
                          phx_mb_decision_t *d);

#ifdef __cplusplus
}
#endif
#endif
