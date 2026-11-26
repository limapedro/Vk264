/* Per-macroblock analysis driver — the explicit A/B boundary described by
 * docs/SHADER_MAPPING.md:67-70.  It fills one phx_mb_decision_t from the
 * already-loaded neighbour cache and the frame's source pixels; the serial
 * entropy stage consumes that structure later.
 *
 * Source followed:
 *   reference/x264/encoder/analyse.c:294   mb_analyse_init()
 *   reference/x264/encoder/analyse.c:303   b_early_terminate (subme < 11)
 *   reference/x264/encoder/analyse.c:569-584 mb_init_fenc_cache()
 *   reference/x264/encoder/analyse.c:668   mb_analyse_intra()
 *   reference/x264/encoder/analyse.c:673-683 intra_analysis_shortcut[][][][]
 *   reference/x264/encoder/analyse.c:688-979 I16/I8/I4 searches (x3 :700/:771/
 *                           :784/:903, shortcut :800/:916, bonus :817/:935,
 *                           early-out :740/:825/:860/:868/:951)
 *   reference/x264/encoder/analyse.c:982-1014 intra_rd() [TODO: needs
 *                           macroblock_size_cabac from the RDO stage]
 *   reference/x264/encoder/analyse.c:1016-1135 intra_rd_refine() (I4 :1086-1135)
 *   reference/x264/encoder/analyse.c:586   mb_analyse_intra_chroma()
 *   reference/x264/encoder/analyse.c:3734  analyse_update_cache()
 *   reference/x264/encoder/encoder.c:1409-1422 mbcmp_init() (SAD subme<2,
 *                           SATD otherwise)
 *   reference/x264/encoder/rdo.c:71-73/:162-197/:315-347 COPY_CABAC,
 *                           rd_cost_mb, rd_cost_i4x4
 *   reference/x264/common/pixel.c:55-73/:242-288/:311-332 SAD / SATD kernels
 *   reference/x264/encoder/analyse.c:2918  x264_macroblock_analyse()
 *   reference/x264/encoder/analyse.c:2735-2770 mb_analyse_transform()
 *   reference/x264/encoder/analyse.c:2773-2800 mb_analyse_transform_rd() [TODO:
 *                           needs rd_cost_mb() from the RDO stage]
 *   reference/x264/encoder/analyse.c:2904-2912 QP-change transform toggle [TODO]
 *   reference/x264/encoder/analyse.c:3718-3719 transform decision call site
 *   reference/x264/common/macroblock.h:454-461 transform-allowed predicate
 *   reference/x264/encoder/macroblock.c:713-737 I8x8 transform flag at encode
 *   reference/x264/encoder/analyse.c:1255-1332 mb_analyse_inter_p16x16()
 *   reference/x264/encoder/analyse.c:3334-3359 B-direct / B_SKIP gate
 *   reference/x264/encoder/analyse.c:2953-3024 P_SKIP fast-skip / b_try_skip
 *   reference/x264/encoder/macroblock.c:988-1139 probe_skip (4:2:0)
 *   reference/x264/encoder/macroblock.c:953-964 P_L0 -> P_SKIP promotion
 *   reference/x264/common/mvpred.c:129-163  x264_mb_predict_mv_16x16()
 *   reference/x264/encoder/me.c:276-419      full-pel x264_me_search_ref()
 *   reference/x264/encoder/macroblock.h:132/168 i4x4/i8x8 encode
 *   reference/x264/encoder/macroblock.c:126  mb_encode_i16x16()
 *   reference/x264/common/pixel.c:55-73/265/290/334 SAD / SATD / SA8D
 *
 * TODO (see phx_analysis.c): sub-pel ME, chroma ME, whole-MB intra_rd()
 * (needs the size-only macroblock_size_cabac writer of the RDO stage) and
 * the I16/chroma/I8 branches of intra_rd_refine() are not representable with
 * the interfaces exposed by phx_frame_t / phx_rdo.h.  The rd_cost_i4x4()
 * path and the I4 branch of intra_rd_refine() are ported and golden-tested.
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
#include "phx_mvdirect.h"     /* B1's byte-exact direct / B_SKIP predictor */

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

/* x264 encoder/macroblock.h:38-39 + encoder/macroblock.c:988-1139 —
 * x264_macroblock_probe_pskip().  Returns 1 when the MB can be coded as
 * P_SKIP (zero residual after MC with the cached pskip MV), 0 otherwise.
 * `fref` is list-0 reference 0; `cache` supplies pskip_mv and the neighbour
 * motion/ref state; qp/chroma_qp are the MB's luma/chroma QPs (analyse.c:993/
 * :1035).  No live CABAC or entropy state is touched.
 * SHADER: B — explicit fenc/fref/neighbour inputs, deterministic output. */
int phx_analysis_probe_pskip(const phx_param_t *p, const phx_frame_t *fenc,
                             const phx_frame_t *fref,
                             const phx_mb_cache_t *cache,
                             int mb_x, int mb_y, int qp, int chroma_qp);

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

/* Exact per-frame B-slice inputs the caller's DPB must supply (DPB1 is adding
 * the list-1 accessors).  `col` is x264's h->fref[1][0] state consumed by B1's
 * phx_mvdirect_predict(); `fref1` is the colocated picture itself for the
 * bidirectional probe.  SHADER: B — read-only DPB/frame state. */
typedef struct {
    const phx_frame_t *fref1;           /* list-1 reference 0 */
    const phx_mvdirect_col_t *col;      /* h->fref[1][0] fields, B1's input */
    int cur_poc;                        /* h->fdec->i_poc */
    int b_direct_spatial;               /* h->sh.b_direct_spatial_mv_pred */
    int bipred_weight;                  /* h->bipred_weight[r0][r1], 32 default */
} phx_analysis_b_input_t;

/* Per-MB decision capture used by the byte-exact B golden tests (and by the
 * diagnostic harness).  phx_encode() appends one record per coded MB when
 * `phx_g_mbdiag_enable` is nonzero; the arrays are caller-readable and hold no
 * entropy state.  SHADER: B/C — read-only observation of the decision boundary.
 */
typedef struct {
    int frame_index, mb_x, mb_y, mb_type, skip, partition;
    int8_t  ref[2][16];
    int16_t mv[2][16][2];
} phx_mbdiag_rec_t;
extern phx_mbdiag_rec_t phx_g_mbdiag[4096];
extern int phx_g_mbdiag_count, phx_g_mbdiag_enable;

/* x264 common/macroblock.h:444-461 — x264_mb_transform_8x8_allowed() with its
 * inputs passed explicitly (no h->mb state).  `pps_mode` is
 * pps->b_transform_8x8_mode (encoder/set.c:502).  SHADER: A — pure predicate. */
int phx_mb_transform_8x8_allowed_ex(int pps_mode, int mb_type, int i_partition);

/* x264 encoder/analyse.c:2757-2768 — the luma cost pair of
 * mb_analyse_transform() on a source window and an already-MC'd prediction:
 * pixf.sa8d[16x16] (four unscaled sa8d_8x8 tiles, pixel.c:374-381) and
 * pixf.satd[16x16] (pixel.c:311-332).  `*b8` is the source-aligned
 * `i_cost8 < i_cost4` decision, valid for the mbrd==0 call site
 * (analyse.c:3718-3719).  SHADER: A — pure kernels, no state. */
void phx_mb_transform_costs(const uint8_t *fenc, int fence_stride,
                            const uint8_t *pred, int pred_stride,
                            int *cost8, int *cost4, int *b8);

/* Per-MB transform-size decision capture, used by the byte-exact golden
 * tests and by the diagnostic harness.  `i_cost8` is x264's
 * pixf.sa8d[PIXEL_16x16] (analyse.c:2761/:376-381, four unscaled sa8d_8x8
 * tiles with one final (sum+2)>>2), `i_cost4` is pixf.satd[PIXEL_16x16]
 * (analyse.c:2763).  `allowed` is the exact x264_mb_transform_8x8_allowed()
 * predicate (common/macroblock.h:454-461).  No entropy state is touched.
 * SHADER: B/C — read-only observation of the analysis decision boundary. */
typedef struct {
    int mb_x, mb_y, mb_type, i_partition;
    int allowed, mbrd, pps_mode;
    int i_cost8, i_cost4;
    int b_transform_8x8;
} phx_transform_diag_rec_t;
extern phx_transform_diag_rec_t phx_g_transform_diag[4096];
extern int phx_g_transform_diag_count, phx_g_transform_diag_enable;

/* mbrd>=1 intra_rd() observation: the three re-costed intra modes
 * (analyse.c:987-1013) and the winning MB type.  No entropy state.
 * SHADER: B/C — read-only observation of the A/B boundary. */
typedef struct {
    int mb_x, mb_y, qp;
    int cost[19];                   /* rd_cost[type] after rd_cost_mb() */
    int satd[19];                   /* pre-RD SATD costs (a->i_satd_iXX) */
    int ssd[19], bits[19];          /* rdo.c:174/:185 components */
    int cbp[19], nnz0[19], pred16;  /* candidate diagnostics */
    int raw[19], psy[19], chr[19];  /* ssd_mb components */
    int fdec_sum[19];               /* diagnostic: sum of reconstructed luma */
    int cbp_chroma[19];             /* diagnostic: candidate chroma CBP */
    int winner;                     /* mb_class_e selected by :2943-2946 */

    /* mbrd>=1 i_skip_intra==2 candidate handoff (analyse.c:840-850/:965-974,
     * macroblock.c:717-728/:742-752).  These are the exact arrays the winning
     * candidate published to rd_cost_mb (rdo.c:169/184), captured before any
     * serial transform writes MB0.  SHADER: B/C — read-only observation. */
    int pred8[4];                   /* a->i_predict8x8[] (:3744-3746) */
    int cand_cbp_luma, cand_cbp_chroma;
    int16_t cand_dct8[4][64];       /* h->dct.luma8x8, 4 zigzag 64-coeff blocks */
    int16_t cand_chroma_dct[2][5][16];   /* h->dct.chroma_dc/luma4x4[16..47] */
    uint8_t cand_chroma_nnz[2][5];       /* chroma DC + 4 AC nnz flags */
    uint8_t cand_luma_nnz[16];           /* nnz at phx_scan8[0..15] */
} phx_intra_rd_diag_rec_t;
extern phx_intra_rd_diag_rec_t phx_g_intra_rd_diag[4096];
extern int phx_g_intra_rd_diag_count, phx_g_intra_rd_diag_enable;

/* Additive B-frame entry point: same ABI as phx_macroblock_analyse_slice()
 * plus the list-1/colocated bundle above.  For B slices it calls B1's
 * phx_mvdirect_predict() and the analyse.c:3334-3359 B_SKIP gate/probe, and
 * publishes PHX_B_SKIP with the exact direct MV/ref arrays.  A NULL `b`
 * keeps the existing intra-only fallback.  SHADER: B — one MB, no hidden state. */
int phx_macroblock_analyse_slice_b(const phx_param_t *p, const phx_frame_t *fenc,
                                   const phx_frame_t *fref0,
                                   const phx_analysis_b_input_t *b,
                                   phx_mb_cache_t *cache, phx_cabac_t *cb,
                                   int mb_x, int mb_y, int qp,
                                   phx_mb_decision_t *d);

#ifdef __cplusplus
}
#endif

#endif
