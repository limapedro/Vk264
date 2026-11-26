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
 *   reference/x264/encoder/macroblock.c:654   P_SKIP MC (p_fref[0][0])
 *   reference/x264/encoder/macroblock.c:669   P_SKIP chroma MC + mv0 shortcut
 *   reference/x264/encoder/macroblock.c:769   inter MC (x264_mb_mc)
 *   reference/x264/common/macroblock.c:61     inter 16x16 chroma MC call site
 *   reference/x264/common/mc.c:63-75          pixel_avg_wxh (qpel average)
 *   reference/x264/common/mc.c:162            mc_copy (16x16 row copy)
 *   reference/x264/common/mc.c:174-219        hpel_filter / mc_luma
 *   reference/x264/common/mc.c:252-283        mc_chroma (1/8-pel bilinear)
 *   reference/x264/common/tables.c:183-184    hpel_ref0 / hpel_ref1
 *   reference/x264/encoder/analyse.c:336      mv_min/mv_max frame-edge clip
 *   reference/x264/encoder/analyse.c:261      b_trellis = trellis>1 && mbrd
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
#include "phx_cabac.h"
#include "phx_dct.h"      /* phx_dctcoef for the 64-coefficient I8x8 slot */

#ifdef __cplusplus
extern "C" {
#endif

/* Row 19: x264's I_8x8 / inter-8x8 residual lives in h->dct.luma8x8[4]
 * (encoder/macroblock.h:168-211, macroblock.c:779-838), i.e. 64 coefficients
 * per 8x8 block.  phx_mb_decision_t::dct[48][16] has no such slot, so the
 * B stage publishes the four zigzag-scanned blocks through this explicit
 * array (order: 8x8 block 0..3, common/macroblock.h:188-194).  `nnz` is
 * x264's STORE_8x8_NNZ value (encoder/macroblock.h:112-117), also mirrored
 * into d->nnz[]/cache->non_zero_count[] for neighbour contexts.
 * SHADER: B — 64 coefficients per block, no cross-block coefficient state. */
typedef struct {
    phx_dctcoef coef[64];
    uint8_t nnz;
} phx_mb_i8x8_block_t;

/* Reconstruct one macroblock from d and write the result back into f->plane
 * at (mb_x*16, mb_y*16).  Mirrors x264_macroblock_encode
 * (reference/x264/encoder/macroblock.c:974) and the intra branches of
 * macroblock_encode_internal (:618, :706-768); cache nnz/cbp/transform-size
 * writes mirror macroblock_cache_save
 * (reference/x264/common/macroblock.c:1680, :1761-1779).  The inter branch is
 * macroblock_encode_internal's no-residual P_L0/D_16x16 path (:654-667,
 * :769-775), reconstructed from the caller-supplied list-0 reference plane:
 * luma MC follows common/mc.c:198-219 (integer MV = mc_copy; fractional MV =
 * 6-tap HPEL + (a+b+1)>>1 qpel average) and inter chroma follows
 * encoder/macroblock.c:669-681 / common/macroblock.c:61-64 (mc.c:252-283).
 * Quantization uses the final-encode b_trellis flag of analyse.c:3724
 * (b_trellis = i_trellis); analyse.c:261 is the analysis-only gate.
 * Returns 0 on success and -1 for an unsupported or invalid mode/argument.
 *
 * SHADER: B — serial-per-MB reconstruction that completes the B-stage record.
 */
int phx_macroblock_encode(const phx_param_t *p, phx_frame_t *f,
                          phx_mb_cache_t *cache, int mb_x, int mb_y,
                          phx_mb_decision_t *d);

/* Same entry point with the live CABAC coder passed in so trellis QNS can use
 * x264's context state (rdo.c:1142-1157).  NULL keeps the plain deadzone path.
 * fref is the caller's list-0 reference 0 (h->fref[0][0], macroblock.c:666):
 * it is required for any PHX_P_L0 decision (D_16x16/16x8/8x16/8x8), whose
 * motion-compensated luma/chroma reconstruction follows macroblock.c:654-667/
 * :669-681/:769-775, common/macroblock.c:34-72/:200-244 and
 * common/mc.c:162-219/:252-283.  NULL rejects every inter mode.
 * SHADER: B — serial per-MB reconstruction; fref is read-only. */
int phx_macroblock_encode_ex(const phx_param_t *p, phx_frame_t *f,
                             const phx_frame_t *fref,
                             phx_mb_cache_t *cache, int mb_x, int mb_y,
                             phx_mb_decision_t *d, phx_cabac_t *cb);

/* Full entry point, including the two rows that need storage beyond the
 * 48x16 decision arrays:
 *  - inter P_L0 residual (encoder/macroblock.c:768-914): MC for every L0
 *    partition, 4x4 or 8x8 residual quant, dequant and FDEC reconstruction;
 *  - I_8x8 / inter transform-8x8 residual (macroblock.c:713-737/:801-838,
 *    encoder/macroblock.h:168-211): 64 coefficients per block in i8x8_out.
 * `i8x8_out` may be NULL when the caller does not need the 64-coefficient
 * arrays (the 4x4 paths still publish their coefficients in d->dct[]).
 * A P_L0 decision with cbp_luma == cbp_chroma == 0 keeps the established
 * no-residual handoff; any residual-carrying decision takes the full path.
 * Plain phx_macroblock_encode_ex() forwards here with i8x8_out == NULL.
 * SHADER: B — one serial invocation per MB; fref/i8x8_out are explicit. */
int phx_macroblock_encode_ex2(const phx_param_t *p, phx_frame_t *f,
                              const phx_frame_t *fref,
                              phx_mb_cache_t *cache, int mb_x, int mb_y,
                              phx_mb_decision_t *d, phx_cabac_t *cb,
                              phx_mb_i8x8_block_t i8x8_out[4]);

/* x264's chroma DC path uses x264_quant_chroma_dc_trellis() (macroblock.c:420,
 * rdo.c:1114-1140) whenever trellis is enabled, and zigzag_scan_2x2_dc()
 * (macroblock.c:35-41) before handing the 2x2 block to CABAC.  Route the
 * phase-B chroma entry point through the trellis-aware wrapper so the entropy
 * stage sees the same coefficients as x264.  Plain/non-trellis callers fall
 * through to phx_chroma.c unchanged.  SHADER: B — serial per MB. */
#define phx_macroblock_encode_chroma phx_mb_encode_chroma_trellis
int phx_mb_encode_chroma_trellis(const phx_param_t *p, phx_frame_t *f,
                                 phx_mb_cache_t *cache, int mb_x, int mb_y,
                                 phx_mb_decision_t *d);

#ifdef __cplusplus
}
#endif
#endif
