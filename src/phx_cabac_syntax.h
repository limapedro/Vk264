/* Phoenix CABAC syntax-element writers — function-for-function port of
 * reference/x264/encoder/cabac.c (8-bit, 4:2:0, RDO_SKIP_BS=0 path).
 *
 * Every function below carries a SHADER: C tag and cites the exact x264 lines
 * it follows.  The arithmetic coder itself is NOT duplicated: all bins go
 * through phx_cabac_encode_decision / _bypass / _ue_bypass / _terminal
 * (src/phx_cabac.{h,c}).  Context indices are the literal x264 numbers/macros.
 *
 * SHADER: C — strictly serial bin order, keep in the entropy stage.
 */
#ifndef PHX_CABAC_SYNTAX_H
#define PHX_CABAC_SYNTAX_H

#include "phx_cabac.h"
#include "phx_mbcache.h"
#include <stdint.h>

/* common/macroblock.h:64-89 (mb_class_e); phx_mbcache.h already defines I/P. */
#define PHX_B_DIRECT 7
#define PHX_B_L0_L0  8
#define PHX_B_L0_L1  9
#define PHX_B_L0_BI 10
#define PHX_B_L1_L0 11
#define PHX_B_L1_L1 12
#define PHX_B_L1_BI 13
#define PHX_B_BI_L0 14
#define PHX_B_BI_L1 15
#define PHX_B_BI_BI 16
#define PHX_B_8x8   17
#define PHX_B_SKIP  18

/* common/macroblock.h:115-141 (mb_partition_e). */
#define PHX_D_L0_4x4     0
#define PHX_D_L0_8x4     1
#define PHX_D_L0_4x8     2
#define PHX_D_L0_8x8     3
#define PHX_D_L1_4x4     4
#define PHX_D_L1_8x4     5
#define PHX_D_L1_4x8     6
#define PHX_D_L1_8x8     7
#define PHX_D_BI_4x4     8
#define PHX_D_BI_8x4     9
#define PHX_D_BI_4x8    10
#define PHX_D_BI_8x8    11
#define PHX_D_DIRECT_8x8 12
#define PHX_D_8x8       13
#define PHX_D_16x8      14
#define PHX_D_8x16      15
#define PHX_D_16x16     16

/* common/macroblock.h:273-289 (cabac_ctx_block_cat_e). */
#define PHX_DCT_LUMA_DC      0
#define PHX_DCT_LUMA_AC      1
#define PHX_DCT_LUMA_4x4     2
#define PHX_DCT_CHROMA_DC    3
#define PHX_DCT_CHROMA_AC    4
#define PHX_DCT_LUMA_8x8     5
#define PHX_DCT_CHROMAU_DC   6
#define PHX_DCT_CHROMAU_AC   7
#define PHX_DCT_CHROMAU_4x4  8
#define PHX_DCT_CHROMAU_8x8  9
#define PHX_DCT_CHROMAV_DC  10
#define PHX_DCT_CHROMAV_AC  11
#define PHX_DCT_CHROMAV_4x4 12
#define PHX_DCT_CHROMAV_8x8 13

/* common/base.h:177-178. */
#define PHX_LUMA_DC  48
#define PHX_CHROMA_DC 49

typedef int16_t phx_dctcoef_t;

/* The subset of x264_t.h->mb / ->sh / ->pps that CABAC syntax writing reads.
 * cache is borrowed, never owned; its scan8 arrays must already be loaded
 * exactly as phx_mbcache.c loads them. */
typedef struct {
    phx_mb_cache_t *cache;          /* h->mb.cache */
    int i_type;                     /* h->mb.i_type (mb_class_e) */
    int i_partition;                /* h->mb.i_partition */
    int i_sub_partition[4];         /* h->mb.i_sub_partition */
    int i_intra16x16_pred_mode;     /* h->mb.i_intra16x16_pred_mode */
    int i_chroma_pred_mode;         /* h->mb.i_chroma_pred_mode */
    int chroma_pred_mode_left;      /* h->mb.chroma_pred_mode[left] */
    int chroma_pred_mode_top;       /* h->mb.chroma_pred_mode[top] */
    int i_qp, i_last_qp, i_last_dqp;/* h->mb.i_qp/i_last_qp/i_last_dqp */
    int prev_type, prev_cbp;        /* h->mb.type[prev], h->mb.cbp[prev] */
    int curr_cbp;                   /* h->mb.cbp[h->mb.i_mb_xy] */
    int i_fref[2];                  /* h->mb.pic.i_fref[] */
    int b_transform_8x8_mode;       /* h->pps->b_transform_8x8_mode */
    int slice_type;                 /* h->sh.i_type (PHX_SLICE_*) */
    int i_frame;                    /* h->i_frame (PCM flush only) */
    int plane_count;                /* 1 (4:2:0) or 3 (4:4:4) */
    int chroma;                     /* chroma present flag */
    uint8_t skip[40];               /* h->mb.cache.skip[] */
    int (*coeff_last[14])(const phx_dctcoef_t *dct); /* h->quantf.coeff_last */
} phx_cabac_mb_t;

/* Zero-init + install C coeff_last defaults (common/quant.c:375-379,437-441,
 * :878-882 and encoder/encoder.c:1446).  SHADER: C. */
void phx_cabac_mb_init(phx_cabac_mb_t *mb, phx_mb_cache_t *cache);

/* cabac.c:35-67 — intra mb_type tree. ctx0..ctx5 are the literal x264
 * contexts from cabac_mb_header_i(). SHADER: C. */
void phx_cabac_mb_type_intra(phx_cabac_t *cb, phx_cabac_mb_t *mb, int i_mb_type,
                             int ctx0, int ctx1, int ctx2, int ctx3,
                             int ctx4, int ctx5);

/* cabac.c:83-96 / :98-116 — intra4x4 and chroma prediction bins. SHADER: C. */
void phx_cabac_intra4x4_pred_mode(phx_cabac_t *cb, int i_pred, int i_mode);
void phx_cabac_intra_chroma_pred_mode(phx_cabac_t *cb, phx_cabac_mb_t *mb);

/* cabac.c:381-433 / :435-497 / :499-610 — mb_type headers. SHADER: C. */
void phx_cabac_mb_header_i(phx_cabac_t *cb, phx_cabac_mb_t *mb,
                           int i_mb_type, int slice_type, int chroma);
void phx_cabac_mb_header_p(phx_cabac_t *cb, phx_cabac_mb_t *mb,
                           int i_mb_type, int chroma);
void phx_cabac_mb_header_b(phx_cabac_t *cb, phx_cabac_mb_t *mb,
                           int i_mb_type, int chroma);

/* cabac.c:198-233 — sub-partition trees. SHADER: C. */
void phx_cabac_subpartition_p(phx_cabac_t *cb, int i_sub);
void phx_cabac_subpartition_b(phx_cabac_t *cb, int i_sub);

/* cabac.c:235-239 — transform_size_8x8_flag (ctx 399 + neighbour). SHADER: C. */
void phx_cabac_transform_size(phx_cabac_t *cb, phx_cabac_mb_t *mb);

/* cabac.c:118-127 / :129-148 — coded block pattern. SHADER: C. */
void phx_cabac_cbp_luma(phx_cabac_t *cb, phx_cabac_mb_t *mb);
void phx_cabac_cbp_chroma(phx_cabac_t *cb, phx_cabac_mb_t *mb);

/* cabac.c:150-186 — mb_qp_delta. SHADER: C. */
void phx_cabac_qp_delta(phx_cabac_t *cb, phx_cabac_mb_t *mb);

/* cabac.c:189-195 — mb_skip_flag. SHADER: C. */
void phx_cabac_mb_skip(phx_cabac_t *cb, phx_cabac_mb_t *mb, int b_skip);

/* cabac.c:241-268 / :270-346 / :355-379 — ref, mvd, sub-8x8 mvd. SHADER: C. */
void phx_cabac_ref(phx_cabac_t *cb, phx_cabac_mb_t *mb, int i_list, int idx);
int  phx_cabac_mvd_cpn(phx_cabac_t *cb, phx_cabac_mb_t *mb, int i_list,
                       int idx, int l, int mvd, int ctx);
void phx_cabac_mvd(phx_cabac_t *cb, phx_cabac_mb_t *mb, int i_list,
                   int idx, int width, int height);
void phx_cabac_8x8_mvd(phx_cabac_t *cb, phx_cabac_mb_t *mb, int i);

/* cabac.c:612-645 — coded-block-flag context index increment. SHADER: C. */
int phx_cabac_cbf_ctxidxinc(phx_cabac_mb_t *mb, int i_cat, int i_idx,
                            int b_intra, int b_dc);

/* cabac.c:664-748 — non-RD residual sigmap + level bins, ctx_block_cat
 * indexed via common/tables.c:1758-1792. SHADER: C. */
void phx_cabac_block_residual(phx_cabac_t *cb, phx_cabac_mb_t *mb,
                              int ctx_block_cat, const phx_dctcoef_t *l,
                              int chroma422dc);

/* cabac.c:892-916 — CBF bin + conditional residual. SHADER: C. */
void phx_cabac_block_residual_cbf(phx_cabac_t *cb, phx_cabac_mb_t *mb,
                                  int ctx_block_cat, int i_idx,
                                  const phx_dctcoef_t *l,
                                  int b_intra, int b_dc, int chroma422dc);

#endif
