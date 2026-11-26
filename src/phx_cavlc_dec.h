/* Phoenix CAVLC decoder — inverse of src/phx_cavlc.c and src/phx_cavlc_syntax.c.
 *
 * Exact source lines followed (all relative to reference/x264/):
 *   encoder/cavlc.c:36-63       cbp/mb-type/sub-partition syntax maps (read
 *                                only through phx_cavlc_syntax.h tables)
 *   encoder/cavlc.c:65          bs_write_vlc == bs_write(size, bits)
 *   encoder/cavlc.c:70-119      cavlc_block_residual_escape (level_prefix)
 *   encoder/cavlc.c:121-196     cavlc_block_residual_internal (residual order)
 *   encoder/cavlc.c:200-209     x264_cavlc_block_residual (empty-token path)
 *   encoder/cavlc.c:241-248     cavlc_mvd (two bs_write_se, no VLC)
 *   encoder/cavlc.c:250-272     cavlc_8x8_mvd
 *   encoder/cavlc.c:305-335     cavlc_mb_header_i
 *   encoder/cavlc.c:337-409     cavlc_mb_header_p
 *   encoder/cavlc.c:411-482     cavlc_mb_header_b
 *   encoder/cavlc.c:487-543     x264_macroblock_write_cavlc dispatch
 *   common/tables.c:1799,1810,2265,2435,2455,2509  verbatim VLC leaves
 *   common/vlc.c:30-31,33-99    generated level_token / run_before tables
 *   common/quant.c:381-395      coeff_level_runN (zigzag scan order)
 *   common/bitstream.h:125,169,221,241,246,264     bs_write/ue/se/te
 *   common/base.h:155,180       X264_SCAN8_0 / x264_scan8 (phx_scan8)
 *   common/macroblock.h:97-109,115-141,275-289,420-431
 *   H.264 (ITU-T Rec. H.264) 9.2.2  CAVLC residual decoding:
 *     9.2.2.1 coeff_token, 9.2.2.2 trailing ones, 9.2.2.3 levels
 *     (level_prefix/level_suffix, 9-6), 9.2.2.4 total_zeros,
 *     9.2.2.5 run_before; 9.3.3.1 mb_type/MVD semantics.
 *   Cross-check: FFmpeg libavcodec/h264_cavlc.c decode_residual() and
 *     ff_h264_decode_mb_cavlc() (same bit order, same prefix arithmetic).
 *
 * SHADER: C — entropy decoding is strictly serial.  Keep every function in
 * the serial entropy stage; the borrowed phx_mb_cache_t is the explicit
 * neighbour boundary, exactly as on the writer side.
 *
 * LIMITATIONS (deliberate, not approximations):
 *  - phx_bs_t carries no end pointer (src/phx_bitstream.h:20-24).  Callers
 *    must supply a readable buffer padded with RBSP trailing bits; the decoder
 *    cannot detect end-of-buffer and reports -1 only for invalid VLC/prefix
 *    codes or impossible symbol counts.
 *  - I_PCM (cavlc.c:511-535), MBAFF and slice-level mb_skip_run flushing are
 *    out of scope, matching the writer's own limitations in phx_cavlc_syntax.h.
 *  - Callers pass the coeff_token table index in `nC` exactly as x264 computes
 *    it (cavlc.c:202-204): 0..3 for luma via ct_index[], 4 for 4:2:0 chroma DC,
 *    5 for 4:2:2 chroma DC.  For safety, a chroma block with nC outside 4..5
 *    is mapped to the chroma-DC token table implied by `chroma_2x4`.
 */
#ifndef PHX_CAVLC_DEC_H
#define PHX_CAVLC_DEC_H

#include <stdint.h>
#include "phx_bitstream.h"
#include "phx_cavlc.h"
#include "phx_cavlc_syntax.h"

/* Decode one residual block written by phx_cavlc_block_residual().
 *
 * i_quant_cat : DCT category (PHX_DCT_* in phx_cavlc.h); selects the number
 *               of coefficients and the total_zeros table (cavlc.c:189).
 * nC          : coeff_token table index 0..5 (cavlc.c:148, :202-204).
 * chroma      : nonzero for a chroma DC block (selects the DC total_zeros
 *               tables at cavlc.c:180-187).
 * chroma_2x4  : nonzero selects x264_total_zeros_2x4_dc (4:2:2/4:4:4),
 *               zero selects x264_total_zeros_2x2_dc (4:2:0).
 * coef        : output, zigzag order, zeroed on entry.  Capacity must be
 *               16 (luma DC/4x4/8x8), 15 (luma/chroma AC) or 4/8 (chroma DC).
 *
 * Returns bits consumed (>0 even for an empty block), or -1 on invalid data.
 * SHADER: C.
 */
int phx_cavlc_dec_residual(phx_bs_t *bs, int16_t *coef, int i_quant_cat,
                           int nC, int chroma, int chroma_2x4);

/* Inverse of cavlc.c:241-248: read mvd_lX[x] and mvd_lX[y] as two se(v)
 * (bitstream.h:246 / H.264 table 9-1) into mv[0], mv[1].
 * Returns bits consumed, or -1 if the Exp-Golomb prefix overflows.
 * SHADER: C. */
int phx_cavlc_dec_mvd(phx_bs_t *bs, int16_t mv[2]);

/* Inverse of phx_cavlc_mb_header(): reads mb_type (ue), ref indices (te),
 * intra prediction modes and MVDs, and writes them back into `mb` and its
 * borrowed cache (`mb->cache->ref`, `mb->cache->mv`,
 * `mb->cache->intra4x4_pred_mode`).  `bs` is used even when mb->bs differs.
 * Fields not present in the CAVLC header (P/B CBP, qp_delta) are untouched;
 * for I16x16 the CBP fields are reconstructed from mb_type (cavlc.c:310-311).
 * Returns bits consumed, or -1 for I_PCM / invalid mb_type.
 * SHADER: C. */
int phx_cavlc_dec_mb_header(phx_bs_t *bs, phx_cavlc_mb_t *mb);

#endif
