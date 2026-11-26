/* Phoenix CAVLC macroblock-header and motion-vector-difference syntax.
 *
 * Function-for-function port of reference/x264/encoder/cavlc.c (8-bit,
 * 4:2:0/4:2:2, progressive, RDO_SKIP_BS=0).  The residual writer is NOT
 * duplicated here: use src/phx_cavlc.h (phx_cavlc_block_residual) for the
 * coefficient syntax that follows the header in x264_macroblock_write_cavlc.
 *
 * Exact source lines followed (all paths relative to reference/x264/):
 *   common/dct.c:936-948      zigzag_interleave_8x8_cavlc
 *   encoder/cavlc.c:276-284   cavlc_macroblock_luma_residual interleave call
 *   encoder/cavlc.c:290-293   4x4 residual write loop
 *   encoder/cavlc.c:36-46     cbp_to_golomb
 *   encoder/cavlc.c:48-53     mb_type_b_to_golomb
 *   encoder/cavlc.c:55-58     subpartition_p_to_golomb
 *   encoder/cavlc.c:60-63     subpartition_b_to_golomb
 *   encoder/cavlc.c:241-248   cavlc_mvd
 *   encoder/cavlc.c:250-272   cavlc_8x8_mvd
 *   encoder/cavlc.c:305-335   cavlc_mb_header_i
 *   encoder/cavlc.c:337-409   cavlc_mb_header_p
 *   encoder/cavlc.c:411-482   cavlc_mb_header_b
 *   encoder/cavlc.c:487-543   x264_macroblock_write_cavlc dispatch
 *   encoder/cavlc.c:538-543   slice-type -> header dispatch
 *   encoder/encoder.c:2883-2890, :3087-3088  CAVLC mb_skip_run
 *   common/mvpred.c:30-127    x264_mb_predict_mv (progressive path)
 *   common/base.h:155,180     X264_SCAN8_0, x264_scan8
 *   common/base.h:234-246     x264_median, x264_median_mv
 *   common/macroblock.h:64-89,115-141,97-109,143-157,420-431
 *   common/predict.h:45-49,62-66,84-92
 *   common/bitstream.h:169,241,246,264  bs_write1/ue/se/te
 *   x264.h:194                X264_ANALYSE_PSUB8x8
 *
 * SHADER: C — strictly serial bit order.  Keep these writers in the serial
 * entropy stage; the borrowed neighbour cache is the explicit input boundary.
 */
#ifndef PHX_CAVLC_SYNTAX_H
#define PHX_CAVLC_SYNTAX_H

#include <stdint.h>
#include "phx_bitstream.h"
#include "phx_mbcache.h"

/* macroblock.h:64-89 (mb_class_e).  I_ and P_ constants already come from
 * phx_mbcache.h. */
#define PHX_B_DIRECT  7
#define PHX_B_L0_L0   8
#define PHX_B_L0_L1   9
#define PHX_B_L0_BI  10
#define PHX_B_L1_L0  11
#define PHX_B_L1_L1  12
#define PHX_B_L1_BI  13
#define PHX_B_BI_L0  14
#define PHX_B_BI_L1  15
#define PHX_B_BI_BI  16
#define PHX_B_8x8    17
#define PHX_B_SKIP   18

/* macroblock.h:115-141 (mb_partition_e). */
#define PHX_D_L0_4x4      0
#define PHX_D_L0_8x4      1
#define PHX_D_L0_4x8      2
#define PHX_D_L0_8x8      3
#define PHX_D_L1_4x4      4
#define PHX_D_L1_8x4      5
#define PHX_D_L1_4x8      6
#define PHX_D_L1_8x8      7
#define PHX_D_BI_4x4      8
#define PHX_D_BI_8x4      9
#define PHX_D_BI_4x8     10
#define PHX_D_BI_8x8     11
#define PHX_D_DIRECT_8x8 12
#define PHX_D_8x8        13
#define PHX_D_16x8       14
#define PHX_D_8x16       15
#define PHX_D_16x16      16

/* common/common.h SLICE_TYPE_* (see src/phx_slice.h / phx_mbcache.h:57). */
#define PHX_SLICE_TYPE_P 0
#define PHX_SLICE_TYPE_B 1
#define PHX_SLICE_TYPE_I 2

/* x264.h:194. */
#define PHX_ANALYSE_PSUB8x8 0x0020u

/* The subset of x264_t state the CAVLC header/MVD writers read.
 * `cache` is borrowed (never owned) and must already be loaded by
 * phx_macroblock_cache_load; its mv/ref/intra4x4_pred_mode arrays are the
 * explicit neighbour boundary. */
typedef struct {
    phx_bs_t       *bs;                      /* &h->out.bs */
    phx_mb_cache_t *cache;                   /* h->mb.cache */
    int i_type;                              /* h->mb.i_type (mb_class_e) */
    int i_partition;                         /* h->mb.i_partition */
    int i_sub_partition[4];                  /* h->mb.i_sub_partition */
    int i_intra16x16_pred_mode;              /* h->mb.i_intra16x16_pred_mode */
    int i_chroma_pred_mode;                  /* h->mb.i_chroma_pred_mode */
    int i_cbp_luma, i_cbp_chroma;            /* h->mb.i_cbp_luma/chroma */
    int b_transform_8x8;                     /* h->mb.b_transform_8x8 */
    int b_transform_8x8_mode;                /* h->pps->b_transform_8x8_mode */
    int i_fref[2];                           /* h->mb.pic.i_fref[] */
    int b_psub8x8;                           /* analyse.inter & PSUB8x8 */
    int slice_type;                          /* h->sh.i_type (PHX_SLICE_TYPE_*) */
    int chroma;                              /* CHROMA_420 || CHROMA_422 */
} phx_cavlc_mb_t;

/* Zero-init.  SHADER: C. */
void phx_cavlc_mb_init(phx_cavlc_mb_t *mb, phx_bs_t *bs, phx_mb_cache_t *cache);

/* common/dct.c:936-948 (zigzag_interleave_8x8_cavlc, installed at dct.c:1065
 * for the progressive C path): split ONE 64-coefficient zigzag-scanned 8x8
 * array into the four 4x4 coefficient lists x264's CAVLC residual writer
 * consumes.  `src` is h->dct.luma8x8[i8]; `dst` is the flat 4*16 array at
 * h->dct.luma4x4[i8*4]; `nnz` is &h->mb.cache.non_zero_count[scan8[i8*4]],
 * so the four flags land at nnz offsets 0,1,8,9 (scan8's 2x2 luma layout).
 * The stride-4 read and the !!nz reduction are verbatim; do not replace
 * them with a contiguous 16-coefficient memcpy (that is the I8 divergence).
 * SHADER: C -- pure coefficient permutation plus four flag writes; no
 * bitstream or entropy state is touched. */
void phx_cavlc_interleave_8x8(int16_t *dst, const int16_t *src, uint8_t *nnz);

/* Verbatim syntax-mapping tables (encoder/cavlc.c:36-63), exposed so parity
 * tests can check the literal x264 values. */
extern const uint8_t phx_cavlc_cbp_to_golomb[2][2][48];       /* cavlc.c:36 */
extern const uint8_t phx_cavlc_mb_type_b_to_golomb[3][9];     /* cavlc.c:48 */
extern const uint8_t phx_cavlc_subpartition_p_to_golomb[4];   /* cavlc.c:55 */
extern const uint8_t phx_cavlc_subpartition_b_to_golomb[13];  /* cavlc.c:60 */

/* mvpred.c:30-127 (progressive path only): compute the MV predictor.
 * SHADER: C. */
void phx_cavlc_predict_mv(const phx_cavlc_mb_t *mb, int i_list, int idx,
                          int width, int16_t mvp[2]);

/* cavlc.c:241-248 — mvd_lX = mv - mvp, written as two bs_write_se().
 * NOTE: x264 has no "cavlc_mvd VLC table": the mvd is plain signed Exp-Golomb
 * (H.264 table 9-1).  `cavlc_mvd` is the function at cavlc.c:241.
 * Returns bits written.  SHADER: C. */
int phx_cavlc_mvd(phx_cavlc_mb_t *mb, int i_list, int idx, int width);

/* cavlc.c:305-335, :337-409, :411-482, dispatched as at :538-543.
 * Writes mb_type, ref indices and partition flags (including the mvd for each
 * listed partition, exactly as the x264 header functions do).  Does NOT write
 * CBP, qp_delta or residuals — those stay in the caller / phx_cavlc.h.
 * I_PCM (:511-535) is not ported: returns -1 and writes nothing.
 * P_SKIP/B_SKIP are handled as a skip run, not a header: returns 0.
 * Returns bits written (or -1 for unsupported I_PCM).  SHADER: C. */
int phx_cavlc_mb_header(phx_cavlc_mb_t *mb);

/* encoder.c:2876-2890, :3087-3088 — CAVLC has no mb_skip *flag*: skipped MBs
 * are accumulated into a `mb_skip_run` written with bs_write_ue.  Pass
 * b_skip=1 to accumulate (no bits written), b_skip=0 with b_last=0 when a
 * coded P/B MB is reached (encoder.c:2889 writes ue(0) even for an empty
 * run), or b_last=1 at slice end to flush the trailing run only if nonzero
 * (encoder.c:3087-3088).  Do not call with b_last=0 in an I slice:
 * encoder.c:2876 suppresses the run there.  IS_SKIP() is false for I MBs,
 * so *skip_run stays 0.  Returns bits written and resets *skip_run to 0
 * after a flush.  SHADER: C. */
int phx_cavlc_mb_skip(phx_bs_t *s, int *skip_run, int b_skip, int b_last);

#endif
