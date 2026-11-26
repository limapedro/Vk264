/* Per-MB analysis driver.  Line-for-line where the ported interfaces allow it;
 * every place where Phoenix cannot follow x264 is marked TODO, not guessed.
 *
 * Source files followed (all under reference/x264/):
 *   encoder/analyse.c:143/179/248/255/294/476-558/586-665/668-980/1255-1332/
 *                     2918-2952/3061-3065/3200-3223/3734-3753
 *   encoder/macroblock.h:43/120-213/225-246, encoder/macroblock.c:126-239
 *   common/predict.h:34-104, common/macroblock.c:489-498/1340-1351
 *   common/mvpred.c:129-181, common/base.h:180
 *   common/pixel.c:55-73/242-381, common/dct.c:780-846
 *   encoder/me.c:276-419, encoder/me.h:30, common/bitstream.h:301
 *
 * SHADER: B — per-MB workgroup stage, explicit inputs/outputs.
 */
#include "phx_analysis.h"

#include "phx_bitstream.h"
#include "phx_cabac_syntax.h"   /* PHX_DCT_* context categories (cabac.c:664) */
#include "phx_cost.h"
#include "phx_dct.h"
#include "phx_me.h"
#include "phx_mc.h"
#include "phx_predict.h"
#include "phx_quant.h"
#include "phx_rdo.h"            /* trellis QNS, analyse.c:261 */
#include "phx_tables.h"

#include <stdlib.h>
#include <string.h>

#define PHX_COST_MAX       (1 << 28)       /* encoder/me.h:30 */
#define PHX_ANALYSE_I4x4   1               /* common/base.c:435 (phx_param.c:14) */
#define PHX_ANALYSE_I8x8   2               /* common/base.c:435 (phx_param.c:15) */

/* x264 common/pixel.c:237-240 — 8-bit scalar build. */
typedef uint16_t phx_sum_t;
typedef uint32_t phx_sum2_t;
#define PHX_BITS_PER_SUM (8 * sizeof(phx_sum_t)) /* pixel.c:240 = 16 */

#define PHX_HADAMARD4(d0,d1,d2,d3,s0,s1,s2,s3) { \
    phx_sum2_t t0 = (s0) + (s1); \
    phx_sum2_t t1 = (s0) - (s1); \
    phx_sum2_t t2 = (s2) + (s3); \
    phx_sum2_t t3 = (s2) - (s3); \
    (d0) = t0 + t2; (d2) = t0 - t2; \
    (d1) = t1 + t3; (d3) = t1 - t3; \
}

#define PHX_COPY2_IF_LT(x,y,a,b) do { \
    if ((y) < (x)) { (x) = (y); (a) = (b); } \
} while (0)
/* x264 encoder/me.h:80-85 — keep the reference macro name for the ported
 * analysis searches; identical semantics to PHX_COPY2_IF_LT above. */
#define COPY2_IF_LT(x,y,a,b) PHX_COPY2_IF_LT(x,y,a,b)

/* x264 common/common.h:93-108 — x264_clip3(), used to replicate the frame
 * border samples that frame.c:535-553 places outside the coded area.
 * SHADER: A — pure clamp, no state. */
static int phx_clip3(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* x264 common/predict.h:45 — I_PRED_CHROMA_*. */
static const uint8_t phx_chroma_fix[7] = { 0, 1, 2, 3, 0, 0, 0 };
/* x264 common/predict.h:62 — I_PRED_16x16_*. */
static const uint8_t phx_mode16_fix[7] = { 0, 1, 2, 3, 2, 2, 2 };
/* x264 common/predict.h:84-92 — index is (mode+1). */
static const int8_t phx_mode4_fix[13] = {
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 2, 2, 2
};
#define PHX_MODE4_FIX(t) phx_mode4_fix[(t)+1]

/* x264 common/macroblock.h:43-56 — neighbour filter flags per 8x8 mode. */
static const uint8_t phx_pred_neighbours[12] = {
    PHX_MB_TOP, PHX_MB_LEFT, PHX_MB_TOP|PHX_MB_LEFT,
    PHX_MB_TOP|PHX_MB_TOPRIGHT,
    PHX_MB_LEFT|PHX_MB_TOPLEFT|PHX_MB_TOP,
    PHX_MB_LEFT|PHX_MB_TOPLEFT|PHX_MB_TOP,
    PHX_MB_LEFT|PHX_MB_TOPLEFT|PHX_MB_TOP,
    PHX_MB_TOP|PHX_MB_TOPRIGHT,
    PHX_MB_LEFT, PHX_MB_LEFT, PHX_MB_TOP, 0
};

/* x264 encoder/analyse.c:476-483. */
static const int8_t phx_i16x16_mode_available[5][5] = {
    { 6, -1, -1, -1, -1 },
    { 4, 1, -1, -1, -1 },
    { 5, 0, -1, -1, -1 },
    { 0, 1, 2, -1, -1 },
    { 0, 1, 2, 3, -1 },
};
/* x264 encoder/analyse.c:485-492 — chroma_mode_available (mode order matters
 * for COPY2_IF_LT tie-breaks).  I_PRED_CHROMA_*: DC=0, H=1, V=2, P=3,
 * DC_LEFT=4, DC_TOP=5, DC_128=6 (common/predict.h:34-44). */
static const int8_t phx_chroma_mode_available[5][5] = {
    { 6, -1, -1, -1, -1 },                             /* analyse.c:487 */
    { 4,  1, -1, -1, -1 },                             /* analyse.c:488 */
    { 5,  2, -1, -1, -1 },                             /* analyse.c:489 */
    { 2,  1,  0, -1, -1 },                             /* analyse.c:490 */
    { 2,  1,  0,  3, -1 },                             /* analyse.c:491 */
};
/* x264 encoder/analyse.c:494-510 (force_intra=0 row; PIR is not ported). */
static const int8_t phx_i8x8_mode_available[5][10] = {
    { 11, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  9,  1,  8, -1, -1, -1, -1, -1, -1, -1 },
    { 10,  0,  3,  7, -1, -1, -1, -1, -1, -1 },
    {  2,  1,  0,  3,  7,  8, -1, -1, -1, -1 },
    {  2,  1,  0,  3,  4,  5,  6,  7,  8, -1 },
};
/* x264 encoder/analyse.c:512-528 (force_intra=0 row). */
static const int8_t phx_i4x4_mode_available[5][10] = {
    { 11, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    {  9,  1,  8, -1, -1, -1, -1, -1, -1, -1 },
    { 10,  0,  3,  7, -1, -1, -1, -1, -1, -1 },
    {  2,  1,  0,  3,  7,  8, -1, -1, -1, -1 },
    {  2,  1,  0,  3,  4,  5,  6,  7,  8, -1 },
};

/* x264 encoder/analyse.c:695 — i16x16_thresh_lut[11]. */
static const uint8_t phx_i16x16_thresh_lut[11] = {
    2, 2, 2, 3, 3, 4, 4, 4, 4, 4, 4
};
/* x264 encoder/analyse.c:859 — i8x8_thresh[11]. */
static const uint8_t phx_i8x8_thresh_lut[11] = {
    4, 4, 4, 5, 5, 5, 6, 6, 6, 6, 6
};

/* x264 encoder/analyse.c:124-135 — B-slice MB-type bit-cost tables.  These are
 * the literal C tables used by mb_analyse_inter_b16x16 (:1852/:2071-2073), the
 * mode selection of :3397-3402 and the early-terminate estimate of :3463/:3466.
 * SHADER: A — immutable table lookup, no state. */
static const uint8_t phx_mb_b_cost_table[19] = {
     9, 9, 9, 9, 0, 0, 0, 1, 3, 7, 7, 7, 3, 7, 7, 7, 5, 9, 0
};
static const uint8_t phx_mb_b16x8_cost_table[17] = {
     0, 0, 0, 0, 0, 0, 0, 0, 5, 7, 7, 7, 5, 7, 9, 9, 9
};
static const uint8_t phx_sub_mb_b_cost_table[13] = {
     7, 5, 5, 3, 7, 5, 7, 3, 7, 7, 7, 5, 1
};
/* x264 common/macroblock.h:97-109 — which list(s) a B mb_type uses per 8x8
 * partition half ([list][0]=first 8x8, [list][1]=second).  Index is the raw
 * mb_type 7..18 (B_DIRECT..B_SKIP); only 8..16 are read here.  SHADER: A. */
static const uint8_t phx_mb_type_list_table[19][2][2] = {
    {{0,0},{0,0}}, {{0,0},{0,0}}, {{0,0},{0,0}}, {{0,0},{0,0}}, /* I0..I3 */
    {{1,1},{0,0}}, {{0,0},{0,0}}, {{1,1},{0,0}},               /* P_L0..P_SKIP */
    {{0,0},{0,0}},                                              /* B_DIRECT */
    {{1,1},{0,0}}, {{1,0},{0,1}}, {{1,1},{0,1}},                /* B_L0_* */
    {{0,1},{1,0}}, {{0,0},{1,1}}, {{0,1},{1,1}},                /* B_L1_* */
    {{1,1},{1,0}}, {{1,0},{1,1}}, {{1,1},{1,1}},                /* B_BI_* */
    {{0,0},{0,0}}, {{0,0},{0,0}}                               /* B_8x8, B_SKIP */
};

/* x264 encoder/analyse.c:673-683 — intra_analysis_shortcut[avoid_topright]
 * [predict_mode[8]>=0][favor_vertical].  At mbrd==0 (and no fast-intra) x264
 * prunes the post-x3 candidate list to this short list, so modes such as DDL
 * are never evaluated; a full-mode loop can pick a different winner.
 * SHADER: A — immutable table lookup. */
static const int8_t phx_intra_analysis_shortcut[2][2][2][5] = {
    {{{ 8, -1, -1, -1, -1 }, { 3, 7, -1, -1, -1 }},
     {{ 4, 6,  8, -1, -1 }, { 3, 4,  5,  7, -1 }}},
    {{{ 8, -1, -1, -1, -1 }, {-1, -1, -1, -1, -1 }},
     {{ 4, 6,  8, -1, -1 }, { 4, 5, -1, -1, -1 }}},
};

/* x264 common/macroblock.h:225-246 expressed as (x,y) so either stride works. */
static const uint8_t phx_block_xy[16][2] = {
    { 0,0 }, { 4,0 }, { 0,4 }, { 4,4 },
    { 8,0 }, {12,0 }, { 8,4 }, {12,4 },
    { 0,8 }, { 4,8 }, { 0,12}, { 4,12},
    { 8,8 }, {12,8 }, { 8,12}, {12,12}
};
/* x264 common/macroblock.h:221-223 (block_idx_yx_1d). */
static const uint8_t phx_block_yx_1d[16] = {
    0, 4, 1, 5, 8, 12, 9, 13, 2, 6, 3, 7, 10, 14, 11, 15
};
/* x264 common/dct.c:780-821 (progressive zigzag scans). */
static const uint8_t phx_zigzag4[16] = {
     0, 4, 1, 2, 5, 8,12, 9, 6, 3, 7,10,13,14,11,15
};
static const uint8_t phx_zigzag8[64] = {
     0, 8, 1, 2, 9,16,24,17,10, 3, 4,11,18,25,32,40,
    33,26,19,12, 5, 6,13,20,27,34,41,48,56,49,42,35,
    28,21,14, 7,15,22,29,36,43,50,57,58,51,44,37,30,
    23,31,38,45,52,59,60,53,46,39,47,54,61,62,55,63
};

/* x264 common/dct.c:843-846 / :829-832 — progressive zigzag scans of the
 * quantized coefficients. SHADER: A — pure permutation, no state. */
static void phx_zigzag_scan4(phx_dctcoef level[16], const phx_dctcoef dct[16])
{
    for (int i = 0; i < 16; i++) level[i] = dct[phx_zigzag4[i]];
}
static void phx_zigzag_scan8(phx_dctcoef level[64], const phx_dctcoef dct[64])
{
    for (int i = 0; i < 64; i++) level[i] = dct[phx_zigzag8[i]];
}

/* Reduced x264_mb_analysis_t (encoder/analyse.c:56-121). */
typedef struct {
    const phx_param_t *p;
    const phx_frame_t *f;
    const phx_frame_t *fref;          /* list-0 reference 0 (analyse.c:1274) */
    const phx_frame_t *fref1;         /* list-1 reference 0 (B slices only) */
    const phx_analysis_b_input_t *b;  /* list-1/colocated bundle (B slices) */
    int slice_type;                   /* PHX_SLICE_* (encoder.c:3522/3530/3539) */
    phx_mb_cache_t *cache;
    phx_cabac_t *cb;                  /* live CABAC state, read-only for trellis */
    int mbrd;                         /* analyse.c:301 (subme>=6)+(subme>=8)+... */
    int b_satd;                       /* encoder.c:1411 mbcmp selector (SATD/SAD) */
    int b_early_terminate;            /* analyse.c:303 — subme < 11 */
    int b_fast_intra;                 /* analyse.c:320/:457 — P-slice fast intra */
    int b_avoid_topright;             /* analyse.c:321/:466 — P-slice intra refresh */
    int i_satd_inter;                 /* analyse.c:2939 COST_MAX for I slices */
    int b_try_skip;                   /* analyse.c:2959/:3008 P_SKIP trial gate */
    int fast_skip;                    /* analyse.c:3009-3013 fast P_SKIP result */
    phx_mvdirect_result_t b_direct;   /* B1's direct prediction for this MB */
    phx_cost_tables_t *costs;
    phx_quant_tables_t *quant;
    phx_mb_analysis_t a;
    uint8_t *fdec;               /* p_fdec: fdec_buf + 2*FDEC_STRIDE + 1 */
    uint8_t *fdec_buf;           /* owned scratch, NULL when cache->p_fdec used */
    int fdec_stride;
    const uint8_t *fenc;
    int fenc_stride;
    int mb_x, mb_y, qp;
    int n4[16], n8[4];           /* h->mb.i_neighbour4 / i_neighbour8 */
    int satd_i16x16, satd_i8x8, satd_i4x4;
    int pred16, pred8[4], pred4[16];
    int cbp_luma, cbp_chroma;
    phx_dctcoef fenc_dct4[16][16];   /* mb_init_fenc_cache, analyse.c:566 */
    phx_dctcoef fenc_dct8[4][64];    /* :564 (b_transform_8x8) */
} phx_analysis_ctx_t;

/* Full-pel P16 search result, the reduced x264_me_t of analyse.c:1257-1313.
 * `cost` is SAD + lambda*MVbits (encoder/me.h:54), `sad` is the raw residual
 * and `skip` is the conservative P_SKIP probe of analyse.c:1296-1301.
 * SHADER: B — one record per MB, no hidden state. */
typedef struct {
    int found, cost, sad, skip;
    int b_skip;                       /* x264 B_SKIP from analyse.c:3338-3358 */
    int mv[2];                        /* qpel, x264_me_t.mv (encoder/me.h:55) */
    /* B-slice 16x16 decision (analyse.c:3397-3415).  `b_type` is the winning
     * x264 mb_class_e (B_DIRECT=7 .. B_BI_BI=16), `b_mv`/`b_ref` are the MV/ref
     * of that mode, and `cost_*` are the four SATD+bit costs compared at
     * :3399-3402.  `i_bskip_cost` is the mbrd SSD of the direct prediction
     * (:3348) used by the B_SKIP gate.  SHADER: B — one record per MB. */
    int b_type;
    int16_t b_mv[2][2];               /* [list][x,y], qpel */
    int b_ref[2];                     /* [list] reference index */
    int cost_l0, cost_l1, cost_bi, cost_direct, i_bskip_cost;
} phx_inter_result_t;

/* ---- SATD / SA8D (pure A-class kernels from common/pixel.c) -------------- */

/* x264 common/pixel.c:255-259 — pseudo-SIMD absolute value of two 16-bit
 * lanes. SHADER: A — per-coefficient bit trick, no state. */
static phx_sum2_t phx_abs2(phx_sum2_t a)
{
    phx_sum2_t s = ((a >> (PHX_BITS_PER_SUM-1)) &
                    (((phx_sum2_t)1 << PHX_BITS_PER_SUM) + 1)) *
                   (phx_sum2_t)(phx_sum_t)-1;
    return (a + s) ^ s;
}

/* x264 common/pixel.c:265-288 — x264_pixel_satd_4x4().
 * SHADER: A — one invocation per 4x4 block. */
static int phx_satd4(const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    phx_sum2_t tmp[4][2], a0, a1, a2, a3, b0, b1;
    phx_sum2_t sum = 0;
    for (int i = 0; i < 4; i++, p1 += s1, p2 += s2) {
        a0 = (phx_sum2_t)(int)(p1[0] - p2[0]);
        a1 = (phx_sum2_t)(int)(p1[1] - p2[1]);
        b0 = (a0+a1) + ((a0-a1) << PHX_BITS_PER_SUM);
        a2 = (phx_sum2_t)(int)(p1[2] - p2[2]);
        a3 = (phx_sum2_t)(int)(p1[3] - p2[3]);
        b1 = (a2+a3) + ((a2-a3) << PHX_BITS_PER_SUM);
        tmp[i][0] = b0 + b1;
        tmp[i][1] = b0 - b1;
    }
    for (int i = 0; i < 2; i++) {
        PHX_HADAMARD4(a0,a1,a2,a3, tmp[0][i],tmp[1][i],tmp[2][i],tmp[3][i]);
        a0 = phx_abs2(a0) + phx_abs2(a1) + phx_abs2(a2) + phx_abs2(a3);
        sum += (phx_sum_t)a0 + (a0 >> PHX_BITS_PER_SUM);
    }
    return (int)(sum >> 1);
}

/* x264 common/pixel.c:290-309 — x264_pixel_satd_8x4().
 * SHADER: A — one invocation per 8x4 block. */
static int phx_satd8x4(const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    phx_sum2_t tmp[4][4], a0, a1, a2, a3;
    phx_sum2_t sum = 0;
    for (int i = 0; i < 4; i++, p1 += s1, p2 += s2) {
        a0 = (phx_sum2_t)(int)(p1[0]-p2[0]) + ((phx_sum2_t)(int)(p1[4]-p2[4]) << PHX_BITS_PER_SUM);
        a1 = (phx_sum2_t)(int)(p1[1]-p2[1]) + ((phx_sum2_t)(int)(p1[5]-p2[5]) << PHX_BITS_PER_SUM);
        a2 = (phx_sum2_t)(int)(p1[2]-p2[2]) + ((phx_sum2_t)(int)(p1[6]-p2[6]) << PHX_BITS_PER_SUM);
        a3 = (phx_sum2_t)(int)(p1[3]-p2[3]) + ((phx_sum2_t)(int)(p1[7]-p2[7]) << PHX_BITS_PER_SUM);
        PHX_HADAMARD4(tmp[i][0],tmp[i][1],tmp[i][2],tmp[i][3], a0,a1,a2,a3);
    }
    for (int i = 0; i < 4; i++) {
        PHX_HADAMARD4(a0,a1,a2,a3, tmp[0][i],tmp[1][i],tmp[2][i],tmp[3][i]);
        sum += phx_abs2(a0) + phx_abs2(a1) + phx_abs2(a2) + phx_abs2(a3);
    }
    return (int)(((phx_sum_t)sum) + (sum >> PHX_BITS_PER_SUM)) >> 1;
}

/* x264 common/pixel.c:311-332 — PIXEL_SATD_C(4,4), C(8,8), C(16,16).
 * SHADER: A — reductions over independent 4x4/8x4 sub-blocks. */
static int phx_satd_4x4(const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    return phx_satd4(p1, s1, p2, s2);
}
static int phx_satd_8x8(const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    return phx_satd8x4(p1, s1, p2, s2)
         + phx_satd8x4(p1 + 4*s1, s1, p2 + 4*s2, s2);
}
static int phx_satd_16x16(const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    return phx_satd_8x8(p1, s1, p2, s2)
         + phx_satd_8x8(p1 + 8, s1, p2 + 8, s2)
         + phx_satd_8x8(p1 + 8*s1, s1, p2 + 8*s2, s2)
         + phx_satd_8x8(p1 + 8 + 8*s1, s1, p2 + 8 + 8*s2, s2);
}

/* x264 common/pixel.c:55-73 — PIXEL_SAD_C(w,h).  superfast has
 * i_subpel_refine=1 (base.c:517-521), so encoder.c:1411 selects SAD, not
 * SATD, for every mbcmp[PIXEL_*] used by mb_analyse_intra[_chroma].
 * SHADER: A — one reduction per tile, no state. */
static int phx_sad_generic(const uint8_t *p1, int s1,
                           const uint8_t *p2, int s2, int w, int h)
{
    int sum = 0;                                        /* pixel.c:59 */
    for (int y = 0; y < h; y++, p1 += s1, p2 += s2)     /* :60/67-68 */
        for (int x = 0; x < w; x++)                     /* :61 */
            sum += abs((int)p1[x] - (int)p2[x]);        /* :64 */
    return sum;                                         /* :69 */
}
static int phx_sad_4x4(const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    return phx_sad_generic(p1, s1, p2, s2, 4, 4);
}
static int phx_sad_8x4(const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    return phx_sad_generic(p1, s1, p2, s2, 8, 4);
}
static int phx_sad_8x8(const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    return phx_sad_generic(p1, s1, p2, s2, 8, 8);
}
static int phx_sad_16x16(const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    return phx_sad_generic(p1, s1, p2, s2, 16, 16);
}

/* x264 encoder/encoder.c:1409-1422 — mbcmp_init(): for 8-bit non-lossless
 * subme>1 the comparator is SATD, otherwise SAD.  All intra mode searches
 * below use this exact dispatch (analyse.c:700/712/729/749/816/934/962,
 * :622/:626/:655-656).  SHADER: A — pure metric selection, no state. */
static int phx_mbcmp16(int b_satd,
                       const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    return b_satd ? phx_satd_16x16(p1, s1, p2, s2)
                  : phx_sad_16x16(p1, s1, p2, s2);
}
static int phx_mbcmp8(int b_satd,
                      const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    return b_satd ? phx_satd_8x8(p1, s1, p2, s2)
                  : phx_sad_8x8(p1, s1, p2, s2);
}
static int phx_mbcmp4(int b_satd,
                      const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    return b_satd ? phx_satd_4x4(p1, s1, p2, s2)
                  : phx_sad_4x4(p1, s1, p2, s2);
}

/* x264 common/pixel.c:334-372 — sa8d_8x8() + x264_pixel_sa8d_8x8().
 * SHADER: A — one invocation per 8x8 block. */
static int phx_sa8d_8x8(const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    phx_sum2_t tmp[8][4], a0,a1,a2,a3,a4,a5,a6,a7,b0,b1,b2,b3;
    phx_sum2_t sum = 0;
    for (int i = 0; i < 8; i++, p1 += s1, p2 += s2) {
        a0 = (phx_sum2_t)(int)(p1[0]-p2[0]);
        a1 = (phx_sum2_t)(int)(p1[1]-p2[1]);
        b0 = (a0+a1) + ((a0-a1) << PHX_BITS_PER_SUM);
        a2 = (phx_sum2_t)(int)(p1[2]-p2[2]);
        a3 = (phx_sum2_t)(int)(p1[3]-p2[3]);
        b1 = (a2+a3) + ((a2-a3) << PHX_BITS_PER_SUM);
        a4 = (phx_sum2_t)(int)(p1[4]-p2[4]);
        a5 = (phx_sum2_t)(int)(p1[5]-p2[5]);
        b2 = (a4+a5) + ((a4-a5) << PHX_BITS_PER_SUM);
        a6 = (phx_sum2_t)(int)(p1[6]-p2[6]);
        a7 = (phx_sum2_t)(int)(p1[7]-p2[7]);
        b3 = (a6+a7) + ((a6-a7) << PHX_BITS_PER_SUM);
        PHX_HADAMARD4(tmp[i][0],tmp[i][1],tmp[i][2],tmp[i][3], b0,b1,b2,b3);
    }
    for (int i = 0; i < 4; i++) {
        PHX_HADAMARD4(a0,a1,a2,a3, tmp[0][i],tmp[1][i],tmp[2][i],tmp[3][i]);
        PHX_HADAMARD4(a4,a5,a6,a7, tmp[4][i],tmp[5][i],tmp[6][i],tmp[7][i]);
        b0  = phx_abs2(a0+a4) + phx_abs2(a0-a4);
        b0 += phx_abs2(a1+a5) + phx_abs2(a1-a5);
        b0 += phx_abs2(a2+a6) + phx_abs2(a2-a6);
        b0 += phx_abs2(a3+a7) + phx_abs2(a3-a7);
        sum += (phx_sum_t)b0 + (b0 >> PHX_BITS_PER_SUM);
    }
    return (int)((sum + 2) >> 2);
}

/* x264 common/pixel.c:55-73 — PIXEL_SAD_C( ..., 16, 16 ) at pixel.c:73.
 * PHX_ME_DIA/HEX use exactly this metric (encoder/me.c:66).  SHADER: A —
 * one reduction over two equally strided luma windows. */
static int phx_pixel_sad_16x16(const uint8_t *p1, int s1,
                               const uint8_t *p2, int s2)
{
    int sum = 0;                                        /* pixel.c:59 */
    for (int y = 0; y < 16; y++, p1 += s1, p2 += s2) {   /* pixel.c:60/67-68 */
        for (int x = 0; x < 16; x++)                     /* pixel.c:61 */
            sum += abs((int)p1[x] - (int)p2[x]);         /* pixel.c:64 */
    }
    return sum;                                         /* pixel.c:69 */
}

/* Residual SAD of the 16x16 integer-MV prediction.  `mv` is in qpel units
 * (x264_me_t.mv, encoder/me.h:55), so FPEL(mv) is the sample offset; the
 * caller's phx_me16() result is passed here unchanged.  SHADER: A — pure
 * read of the two frames. */
static int phx_inter_sad(const phx_analysis_ctx_t *c, const int mv[2])
{
    int mx = (mv[0] + 2) >> 2;                          /* encoder/me.c:178 */
    int my = (mv[1] + 2) >> 2;
    const uint8_t *p = c->fref->plane
        + (size_t)(c->mb_y*16 + my) * c->fref->i_stride + c->mb_x*16 + mx;
    return phx_pixel_sad_16x16(c->fenc, c->fenc_stride, p, c->fref->i_stride);
}

/* x264 common/common.h — x264_median_mv(), component-wise median. */
static int phx_median3(int a, int b, int c)
{
    if (a > b) {
        if (b > c) return b;
        return a > c ? c : a;
    }
    if (a > c) return a;
    return b > c ? c : b;
}

/* x264 common/mvpred.c:129-163 — x264_mb_predict_mv_16x16() for list 0 and
 * ref 0 (Phoenix carries a single reference).  Cache indices are x264_scan8
 * (common/base.h:180): left = scan8[0]-1, top = scan8[0]-8, top-right =
 * scan8[0]-8+4, top-left = scan8[0]-8-1.  SHADER: B — reads the neighbour
 * ref/mv cache only. */
static void phx_predict_mv16_l(const phx_mb_cache_t *cache, int l, int i_ref,
                               int16_t mvp[2])
{
    const int i8 = phx_scan8[0];
    const int i_refa = cache->ref[l][i8 - 1];            /* mvpred.c:131 */
    const int16_t *mv_a = cache->mv[l][i8 - 1];          /* :132 */
    const int i_refb = cache->ref[l][i8 - 8];            /* :133 */
    const int16_t *mv_b = cache->mv[l][i8 - 8];          /* :134 */
    int i_refc = cache->ref[l][i8 - 8 + 4];              /* :135 */
    const int16_t *mv_c = cache->mv[l][i8 - 8 + 4];      /* :136 */
    if (i_refc == -2) {                                  /* :137 */
        i_refc = cache->ref[l][i8 - 8 - 1];              /* :139 */
        mv_c   = cache->mv[l][i8 - 8 - 1];               /* :140 */
    }
    int count = (i_refa == i_ref) + (i_refb == i_ref) + (i_refc == i_ref);
                                                         /* :143 */
    if (count > 1) {                                     /* :145/:148 */
        for (int k = 0; k < 2; k++)
            mvp[k] = (int16_t)phx_median3(mv_a[k], mv_b[k], mv_c[k]);
    } else if (count == 1) {                             /* :150-157 */
        const int16_t *src = i_refa == i_ref ? mv_a
                           : i_refb == i_ref ? mv_b : mv_c;
        mvp[0] = src[0]; mvp[1] = src[1];
    } else if (i_refb == -2 && i_refc == -2 && i_refa != -2) {
        mvp[0] = mv_a[0]; mvp[1] = mv_a[1];              /* :159-160 */
    } else {                                             /* :161-162 */
        for (int k = 0; k < 2; k++)
            mvp[k] = (int16_t)phx_median3(mv_a[k], mv_b[k], mv_c[k]);
    }
}

/* x264 common/mvpred.c:129-163 — list-0 wrapper (unchanged ABI for callers). */
static void phx_predict_mv16(const phx_mb_cache_t *cache, int i_ref,
                             int16_t mvp[2])
{
    phx_predict_mv16_l(cache, 0, i_ref, mvp);
}

/* x264 common/mvpred.c:166-181 — x264_mb_predict_mv_pskip().  The P_SKIP MV
 * is zero unless both neighbours have a usable nonzero MV.  SHADER: B —
 * neighbour-cache lookup. */
static void phx_predict_mv_pskip(const phx_mb_cache_t *cache, int16_t mv[2])
{
    const int i8 = phx_scan8[0];
    const int ra = cache->ref[0][i8 - 1];                 /* :168 */
    const int rb = cache->ref[0][i8 - 8];                 /* :170 */
    const int16_t *ma = cache->mv[0][i8 - 1];            /* :169 */
    const int16_t *mb = cache->mv[0][i8 - 8];            /* :171 */
    uint32_t ma32 = (uint16_t)ma[0] | ((uint32_t)(uint16_t)ma[1] << 16);
    uint32_t mb32 = (uint16_t)mb[0] | ((uint32_t)(uint16_t)mb[1] << 16);
    if (ra == -2 || rb == -2 || (!(uint32_t)ra | ma32) ||
        (!(uint32_t)rb | mb32)) {                        /* :173-175 */
        mv[0] = mv[1] = 0;                               /* :177 */
        return;
    }
    phx_predict_mv16(cache, 0, mv);                      /* :180 */
}

/* ---- mode availability / predictors ------------------------------------- */

/* x264 encoder/analyse.c:530-535 — I16 mode list indexed by neighbour mask.
 * SHADER: A — pure table lookup. */
static const int8_t *phx_i16_modes(int neighbour_intra)
{
    int idx = neighbour_intra & (PHX_MB_TOP|PHX_MB_LEFT|PHX_MB_TOPLEFT);
    idx = (idx == (PHX_MB_TOP|PHX_MB_LEFT|PHX_MB_TOPLEFT))
        ? 4 : idx & (PHX_MB_TOP|PHX_MB_LEFT);
    return phx_i16x16_mode_available[idx];
}
/* x264 encoder/analyse.c:537-542 — chroma mode list.
 * SHADER: A — pure table lookup. */
static const int8_t *phx_chroma_modes(int neighbour_intra)
{
    int idx = neighbour_intra & (PHX_MB_TOP|PHX_MB_LEFT|PHX_MB_TOPLEFT);
    idx = (idx == (PHX_MB_TOP|PHX_MB_LEFT|PHX_MB_TOPLEFT))
        ? 4 : idx & (PHX_MB_TOP|PHX_MB_LEFT);
    return phx_chroma_mode_available[idx];
}
/* x264 encoder/analyse.c:544-550 — 8x8 mode list (force_intra==0 here).
 * SHADER: A — pure table lookup. */
static const int8_t *phx_i8_modes(int neighbour8)
{
    int idx = neighbour8 & (PHX_MB_TOP|PHX_MB_LEFT|PHX_MB_TOPLEFT);
    idx = (idx == (PHX_MB_TOP|PHX_MB_LEFT|PHX_MB_TOPLEFT))
        ? 4 : idx & (PHX_MB_TOP|PHX_MB_LEFT);
    return phx_i8x8_mode_available[idx];
}
/* x264 encoder/analyse.c:552-558 — 4x4 mode list (force_intra==0 here).
 * SHADER: A — pure table lookup. */
static const int8_t *phx_i4_modes(int neighbour4)
{
    int idx = neighbour4 & (PHX_MB_TOP|PHX_MB_LEFT|PHX_MB_TOPLEFT);
    idx = (idx == (PHX_MB_TOP|PHX_MB_LEFT|PHX_MB_TOPLEFT))
        ? 4 : idx & (PHX_MB_TOP|PHX_MB_LEFT);
    return phx_i4x4_mode_available[idx];
}

/* x264 common/macroblock.h:420-431 — x264_mb_predict_intra4x4_mode().
 * SHADER: B — reads the two cached neighbour modes of the current MB. */
static int phx_predict4_mode(const phx_analysis_ctx_t *c, int idx)
{
    int ma = c->cache->intra4x4_pred_mode[phx_scan8[idx] - 1];
    int mb = c->cache->intra4x4_pred_mode[phx_scan8[idx] - 8];
    int m = PHX_MODE4_FIX(ma);
    int n = PHX_MODE4_FIX(mb);
    if (n < m) m = n;
    return m < 0 ? 2 /* I_PRED_4x4_DC */ : m;
}

/* x264 common/predict.c:67/80/90/100/104/116/132 — predict_16x16[mode].
 * SHADER: A — one invocation per MB prediction. */
static void phx_predict16(int mode, uint8_t *dst, int stride)
{
    switch (mode) {
    case 0: phx_predict_16x16_v(dst, stride); break;
    case 1: phx_predict_16x16_h(dst, stride); break;
    case 2: phx_predict_16x16_dc(dst, stride); break;
    case 3: phx_predict_16x16_p(dst, stride); break;
    case 4: phx_predict_16x16_dc_left(dst, stride); break;
    case 5: phx_predict_16x16_dc_top(dst, stride); break;
    default: phx_predict_16x16_dc_128(dst, stride); break;
    }
}
/* x264 common/predict.c:508-511 — predict_4x4_v_c() uses
 * PREDICT_4x4_DC(SRC_X4(0,-1)): each output column replicates *its own* top
 * sample.  phx_predict.c:426 currently broadcasts SRC(0,-1) to all columns;
 * until that owned file is corrected, use the source-exact form locally.
 * SHADER: A — one invocation per 4x4 block. */
static void phx_predict4_v_fixed(uint8_t *dst, int stride)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            dst[x + y*stride] = dst[x - stride];
}

/* x264 common/predict.c:481-610 — predict_4x4[mode].
 * SHADER: A — one invocation per 4x4 block. */
static void phx_predict4(int mode, uint8_t *dst, int stride)
{
    switch (mode) {
    case 0: phx_predict4_v_fixed(dst, stride); break;
    case 1: phx_predict_4x4_h(dst, stride); break;
    case 2: phx_predict_4x4_dc(dst, stride); break;
    case 3: phx_predict_4x4_ddl(dst, stride); break;
    case 4: phx_predict_4x4_ddr(dst, stride); break;
    case 5: phx_predict_4x4_vr(dst, stride); break;
    case 6: phx_predict_4x4_hd(dst, stride); break;
    case 7: phx_predict_4x4_vl(dst, stride); break;
    case 8: phx_predict_4x4_hu(dst, stride); break;
    case 9: phx_predict_4x4_dc_left(dst, stride); break;
    case 10: phx_predict_4x4_dc_top(dst, stride); break;
    default: phx_predict_4x4_dc_128(dst, stride); break;
    }
}
/* x264 common/predict.c:632-884 — predict_8x8[mode] with filtered edge.
 * SHADER: A — one invocation per 8x8 block. */
static void phx_predict8(int mode, uint8_t *dst, const uint8_t edge[36], int stride)
{
    switch (mode) {
    case 0: phx_predict_8x8_v(dst, edge, stride); break;
    case 1: phx_predict_8x8_h(dst, edge, stride); break;
    case 2: phx_predict_8x8_dc(dst, edge, stride); break;
    case 3: phx_predict_8x8_ddl(dst, edge, stride); break;
    case 4: phx_predict_8x8_ddr(dst, edge, stride); break;
    case 5: phx_predict_8x8_vr(dst, edge, stride); break;
    case 6: phx_predict_8x8_hd(dst, edge, stride); break;
    case 7: phx_predict_8x8_vl(dst, edge, stride); break;
    case 8: phx_predict_8x8_hu(dst, edge, stride); break;
    case 9: phx_predict_8x8_dc_left(dst, edge, stride); break;
    case 10: phx_predict_8x8_dc_top(dst, edge, stride); break;
    default: phx_predict_8x8_dc_128(dst, edge, stride); break;
    }
}

/* x264 common/pixel.c:542-554 — INTRA_MBCMP(sad/satd,4x4,v,h,dc,,,c):
 * predict V, H and DC into fdec in turn, measuring each with the encoder's
 * selected comparator; fdec is left holding the DC prediction (the x3 path
 * does not write the winner — analyse.c:953-956 does that afterwards).
 * SHADER: A — three 4x4 predictions + comparisons, no state. */
static void phx_i4_x3(const phx_analysis_ctx_t *c, const uint8_t *src, int ss,
                      uint8_t *dst, int ds, int res[4])
{
    phx_predict4_v_fixed(dst, ds);       /* pixel.c:553 pred1=v */
    res[0] = phx_mbcmp4(c->b_satd, dst, ds, src, ss);
    phx_predict_4x4_h(dst, ds);          /* :553 pred2=h */
    res[1] = phx_mbcmp4(c->b_satd, dst, ds, src, ss);
    phx_predict_4x4_dc(dst, ds);         /* :553 pred3=dc */
    res[2] = phx_mbcmp4(c->b_satd, dst, ds, src, ss);
}

/* x264 common/pixel.c:518-531 — INTRA_MBCMP_8x8(sa8d,,_c): V, H, DC with
 * SA8D (the 8x8 x9 kernel is absent in the C-only reference build, so
 * analyse.c:784-804 takes this branch).  SHADER: A — no state. */
static void phx_i8_x3(const uint8_t *src, int ss, uint8_t *dst,
                      const uint8_t edge[36], int ds, int res[4])
{
    phx_predict8(0, dst, edge, ds);      /* pixel.c:522 v */
    res[0] = phx_sa8d_8x8(dst, ds, src, ss);
    phx_predict8(1, dst, edge, ds);      /* :524 h */
    res[1] = phx_sa8d_8x8(dst, ds, src, ss);
    phx_predict8(2, dst, edge, ds);      /* :526 dc */
    res[2] = phx_sa8d_8x8(dst, ds, src, ss);
}

/* x264 common/pixel.c:542-556 — INTRA_MBCMP(sad/satd,8x8,dc,h,v,c,,,c),
 * used by analyse.c:622-623.  SHADER: A — no state. */
static void phx_chroma_x3(const phx_analysis_ctx_t *c,
                          const uint8_t *src, int ss, uint8_t *dst, int ds,
                          int res[4])
{
    phx_predict_8x8c_dc(dst, ds);         /* pixel.c:555 pred1=dc */
    res[0] = phx_mbcmp8(c->b_satd, dst, ds, src, ss);
    phx_predict_8x8c_h(dst, ds);          /* :555 pred2=h */
    res[1] = phx_mbcmp8(c->b_satd, dst, ds, src, ss);
    phx_predict_8x8c_v(dst, ds);          /* :555 pred3=v */
    res[2] = phx_mbcmp8(c->b_satd, dst, ds, src, ss);
}

/* ---- cache / reconstruction setup --------------------------------------- */

/* x264 common/macroblock.c:489-498 and :1340-1351 — i_neighbour4/8 tables.
 * SHADER: B — derives block-level neighbour masks from the MB cache. */
static void phx_init_neighbours(phx_analysis_ctx_t *c)
{
    const int in = c->cache->i_neighbour_intra;
    const int m = PHX_MB_TOP | PHX_MB_LEFT | PHX_MB_TOPLEFT;
    for (int i = 0; i < 16; i++) c->n4[i] = 0;
    for (int i = 0; i < 4; i++) c->n8[i] = 0;
    /* macroblock.c:489-498 — blocks that always have all four neighbours. */
    c->n4[6] = c->n4[9] = c->n4[12] = c->n4[14] =
        PHX_MB_LEFT|PHX_MB_TOP|PHX_MB_TOPLEFT|PHX_MB_TOPRIGHT;
    c->n4[3] = c->n4[7] = c->n4[11] = c->n4[13] = c->n4[15] = c->n8[3] =
        PHX_MB_LEFT|PHX_MB_TOP|PHX_MB_TOPLEFT;   /* macroblock.c:493-498 */
    /* macroblock.c:1340-1351 — progressive branch. */
    c->n4[0] = c->n8[0] = (in & m) | ((in & PHX_MB_TOP) ? PHX_MB_TOPRIGHT : 0);
    c->n4[4] = c->n4[1] = PHX_MB_LEFT |
        ((in & PHX_MB_TOP) ? (PHX_MB_TOP|PHX_MB_TOPLEFT|PHX_MB_TOPRIGHT) : 0);
    c->n4[2] = c->n4[8] = c->n4[10] = c->n8[2] = PHX_MB_TOP|PHX_MB_TOPRIGHT |
        ((in & PHX_MB_LEFT) ? (PHX_MB_LEFT|PHX_MB_TOPLEFT) : 0);
    c->n4[5] = c->n8[1] = PHX_MB_LEFT | (in & PHX_MB_TOPRIGHT) |
        ((in & PHX_MB_TOP) ? (PHX_MB_TOP|PHX_MB_TOPLEFT) : 0);
}

/* x264 encoder/analyse.c:569-584 — mb_init_fenc_cache().  The frame-level
 * SATD/hadamard caches (:577-583) are a pure speed optimisation in the
 * C-only reference: cached_satd (rdo.c:94-112) recomputes the same value on a
 * miss, and ssd_plane (rdo.c:123-148) reads the same fenc/fdec windows.
 * The observable part is psy_trellis_init (:561-567), which stores the
 * unquantized source DCT used by trellis QNS (rdo.c:822-829).  Compute it
 * here and hand it to phx_rdo_set_config() in phx_analysis_quant4().
 * SHADER: B — source-pixel staging for the per-MB trellis workgroup. */
static void phx_analysis_mb_init_fenc_cache(phx_analysis_ctx_t *c)
{
    static const uint8_t zero[16] = { 0 };
    if (c->p->i_trellis == 2 && c->p->f_psy_trellis != 0.0) {
        if (c->p->b_transform_8x8)
            phx_sub16x16_dct8(c->fenc_dct8, c->fenc, c->fenc_stride, zero, 0);
        phx_sub16x16_dct(c->fenc_dct4, c->fenc, c->fenc_stride, zero, 0);
    }
}

/* x264 common/macroblock.c:501-548 (p_fdec layout, FDEC_STRIDE=32) and
 * macroblock.c:883/907/960-977 (unavailable neighbours = -1/128).
 * The encoder hands us cache->p_fdec pointing into its live reconstruction
 * frame (phx_encoder.c:1396, x264 encoder.c:2861 -> macroblock.c:533).  Its
 * one-pixel left/top border is the *reconstructed* neighbour from the previous
 * MB/row (x264 common/macroblock.c:1645-1655 + frame.c:535-553); overwriting it
 * with source samples makes every intra predictor see the wrong edge and
 * changes the DCT/SATD of the following block.  Keep that border untouched.
 * Only a private scratch buffer (standalone analysis without a caller FDEC)
 * needs seeding and source-plane proxying.  SHADER: B — seeds scratch only. */
static void phx_setup_fdec(phx_analysis_ctx_t *c)
{
    if (!c->fdec_buf) return;              /* caller FDEC is authoritative */

    int fs = c->fdec_stride;
    int in = c->cache->i_neighbour_intra;
    uint8_t *d = c->fdec;
    memset(c->fdec_buf, 128, (size_t)fs * 48);
    if (in & PHX_MB_LEFT) {
        for (int y = 0; y < 16; y++)
            d[-1 + y*fs] = c->fenc[-1 + y*c->fenc_stride];
    }
    if (in & PHX_MB_TOP) {
        for (int x = 0; x < 16; x++)
            d[x - fs] = c->fenc[x - c->fenc_stride];
    }
    /* macroblock.c:816-839 — corner flags live in i_neighbour, not
     * i_neighbour_intra. */
    if (c->cache->i_neighbour & PHX_MB_TOPLEFT)
        d[-1 - fs] = c->fenc[-1 - c->fenc_stride];
    if (c->cache->i_neighbour & PHX_MB_TOPRIGHT) {
        int w = c->f->i_width;
        if (c->mb_x*16 + 16 < w)
            d[16 - fs] = c->fenc[16 - c->fenc_stride];
    }
}

/* ---- encode helpers (encoder/macroblock.h:132-213, macroblock.c:126-239) -- */

/* x264 analyse.c:261 selects h->mb.b_trellis = (trellis > 1 && a->i_mbrd),
 * medium defaults i_trellis=1 (base.c:455), so the trellis branch is off for
 * the 715-bin probe; slow/veryslow (base.c:562+) turn it on.
 * phx_mbencode.c applies the same choice at encode time;
 * analysis must reconstruct with the identical quantiser or the second block's
 * neighbours diverge from x264 (the MB1 intra4x4 divergence).  Trellis reads
 * the live CABAC contexts but does not advance them (rdo.c:641-895).
 * SHADER: B — one 4x4 quantisation choice, no hidden state. */
static int phx_analysis_quant4(phx_analysis_ctx_t *c, phx_dctcoef d[16],
                               int idx, int b_intra)
{
    int qp = c->a.i_qp;
    if (c->mbrd > 0 && c->p->i_trellis > 1 && c->cb) {
        phx_rdo_config_t cfg;
        phx_rdo_get_config(&cfg);
        cfg.idx = idx;                              /* psy-trellis block index */
        /* analyse.c:570-573 / rdo.c:822-829 — psy-trellis source DCT from
         * mb_init_fenc_cache(); no-op when i_psy_trellis == 0. */
        cfg.i_psy_trellis = (int)(c->p->f_psy_trellis*(1 << 8) + .5);
        cfg.fenc_dct4 = c->fenc_dct4[idx];
        phx_rdo_set_config(&cfg);
        /* CQM_4IY = 0 (common/set.h:32), DCT_LUMA_4x4 = 2 (macroblock.h:275). */
        return phx_quant_4x4_trellis(c->cb, d, 0, qp, PHX_DCT_LUMA_4x4, b_intra);
    }
    return phx_quant_4x4(d, c->quant->quant4_mf[0][qp],
                         c->quant->quant4_bias[0][qp]);
}

/* x264 encoder/macroblock.h:132-166 — x264_mb_encode_i4x4(), non-lossless.
 * SHADER: A — predict/DCT/quant/dequant/IDCT of one 4x4 block. */
static int phx_encode_i4(phx_analysis_ctx_t *c, int idx, phx_dctcoef out[16])
{
    const uint8_t *src = c->fenc + phx_block_xy[idx][0]
                       + phx_block_xy[idx][1]*c->fenc_stride;
    uint8_t *dst = c->fdec + phx_block_xy[idx][0]
                 + phx_block_xy[idx][1]*c->fdec_stride;
    phx_dctcoef d[16];
    int qp = c->a.i_qp;
    phx_sub4x4_dct(d, src, c->fenc_stride, dst, c->fdec_stride);
    /* macroblock.h:157 — x264_mb_encode_i4x4(..., b_trellis=a->i_mbrd). */
    int nz = phx_analysis_quant4(c, d, idx, 1);      /* b_intra = 1 */
    phx_zigzag_scan4(out, d);
    if (nz) {
        phx_dequant_4x4(d, c->quant->dequant4_mf[0], qp);
        phx_add4x4_idct(dst, c->fdec_stride, d);
    }
    return nz;
}

/* x264 encoder/macroblock.h:168-213 — x264_mb_encode_i8x8(), non-lossless.
 * SHADER: A — predict/DCT/quant/dequant/IDCT of one 8x8 block. */
static int phx_encode_i8(phx_analysis_ctx_t *c, int idx, phx_dctcoef out[64])
{
    int x = 8*(idx&1), y = 8*(idx>>1);
    const uint8_t *src = c->fenc + x + y*c->fenc_stride;
    uint8_t *dst = c->fdec + x + y*c->fdec_stride;
    phx_dctcoef d[64];
    int qp = c->a.i_qp;
    phx_sub8x8_dct8(d, src, c->fenc_stride, dst, c->fdec_stride);
    int nz = phx_quant_8x8(d, c->quant->quant8_mf[0][qp],
                              c->quant->quant8_bias[0][qp]);
    for (int i = 0; i < 64; i++) out[i] = d[phx_zigzag8[i]];
    if (nz) {
        phx_dequant_8x8(d, c->quant->dequant8_mf[0], qp);
        phx_add8x8_idct8(dst, c->fdec_stride, d);
    }
    return nz;
}

/* x264 encoder/macroblock.c:126-239 — mb_encode_i16x16(), non-trellis path.
 * TODO: CQM_4IY is hard-coded (Phoenix has no custom lists); DCT decimation
 * (macroblock.c:205-213) and trellis QNS (rdo.c) are not ported.  The DC nnz
 * flag cannot be stored in phx_mb_decision_t.nnz[48] (needs scan8[48]).
 * SHADER: A — 16 4x4 DCTs plus one 4x4 Hadamard of the DC coefficients. */
static void phx_encode_i16(phx_analysis_ctx_t *c, phx_mb_decision_t *d)
{
    phx_dctcoef dct4[16][16], dcdc[16];
    int qp = c->a.i_qp;
    int block_cbp = 0, dc_nz;
    phx_sub16x16_dct(dct4, c->fenc, c->fenc_stride, c->fdec, c->fdec_stride);
    for (int i = 0; i < 16; i++) {
        dcdc[phx_block_yx_1d[i]] = dct4[i][0];
        dct4[i][0] = 0;                              /* macroblock.c:171 */
    }
    for (int i8 = 0; i8 < 4; i8++) {
        int nz;
        if (c->mbrd > 0 && c->p->i_trellis > 1 && c->cb) {
            /* macroblock.c:190 + analyse.c:261 — quant_4x4x4 is trellis
             * when b_trellis; apply the same per-4x4 decision as x264. */
            nz = 0;
            for (int j = 0; j < 4; j++)
                if (phx_analysis_quant4(c, dct4[i8*4+j], i8*4+j, 1))
                    nz |= 1 << j;
        } else {
            nz = phx_quant_4x4x4(&dct4[i8*4], c->quant->quant4_mf[0][qp],
                                    c->quant->quant4_bias[0][qp]);
        }
        for (int j = 0; j < 4; j++) {
            int idx = i8*4 + j;
            if (!(nz & (1 << j))) continue;
            /* x264 encoder/macroblock.h:199 stores the zigzag-scanned block at
             * scan8[idx] in the shared per-MB cache layout. */
            phx_zigzag_scan4(d->dct[phx_scan8[idx]], dct4[idx]);
            d->nnz[phx_scan8[idx]] = 1;
            block_cbp = 0xf;                        /* macroblock.c:193 */
        }
    }
    c->cbp_luma |= block_cbp;
    phx_dct4x4dc(dcdc);                             /* macroblock.c:215 */
    dc_nz = (c->mbrd > 0 && c->p->i_trellis > 1 && c->cb)
          ? phx_quant_luma_dc_trellis(c->cb, dcdc, 0, qp,
                                      PHX_DCT_LUMA_DC, 1) /* macroblock.c:219 */
          : phx_quant_4x4_dc(dcdc,
                             c->quant->quant4_mf[0][qp][0] >> 1,
                             c->quant->quant4_bias[0][qp][0] << 1);
    /* LUMA_DC == 48 -> scan8[48] == 0 (common/base.h:180). */
    phx_zigzag_scan4(d->dct[phx_scan8[48]], dcdc);  /* luma16x16_dc[0] */
    if (dc_nz) {
        phx_idct4x4dc(dcdc);                        /* macroblock.c:227 */
        phx_dequant_4x4_dc(dcdc, c->quant->dequant4_mf[0], qp);
        if (block_cbp)
            for (int i = 0; i < 16; i++)
                dct4[i][0] = dcdc[phx_block_yx_1d[i]];
    }
    if (block_cbp) phx_add16x16_idct(c->fdec, c->fdec_stride, dct4);
    else if (dc_nz) phx_add16x16_idct_dc(c->fdec, c->fdec_stride, dcdc);
}

/* ---- intra mode searches (encoder/analyse.c:668-980) -------------------- */

/* x264 encoder/analyse.c:688-742 — I16x16 selection.  The x3 candidate
 * pre-selection (analyse.c:700-715) is reproduced exactly, including the
 * `i16x16_thresh` plane pruning (:696/:709) and the per-mode bit cost
 * (:701-703/:713/:730).  The comparator is x264's mbcmp (SAD at subme<2,
 * SATD otherwise — encoder.c:1411).  SHADER: B — neighbour modes + lambda. */
static void phx_search_i16(phx_analysis_ctx_t *c)
{
    const int8_t *pm = phx_i16_modes(c->cache->i_neighbour_intra);
    int lambda = c->a.i_lambda;
    int best = PHX_COST_MAX, best_mode = 0;
    int satd[4] = { 0, 0, 0, 0 };      /* analyse.c:700/:729 direction costs */
    /* analyse.c:695-696 — COST_MAX for I slices (b_fast_intra == 0). */
    int thresh = c->b_fast_intra
               ? (phx_i16x16_thresh_lut[c->p->i_subpel_refine > 10 ? 10
                  : c->p->i_subpel_refine < 0 ? 0 : c->p->i_subpel_refine]
                  * c->i_satd_inter) >> 1
               : PHX_COST_MAX;

    if (pm[3] >= 0) {                    /* analyse.c:698 — x3 path */
        phx_predict16(0, c->fdec, c->fdec_stride);        /* V */
        satd[0] = phx_mbcmp16(c->b_satd, c->fdec, c->fdec_stride,
                              c->fenc, c->fenc_stride)
                + lambda * phx_bs_size_ue(0);             /* :701 */
        phx_predict16(1, c->fdec, c->fdec_stride);        /* H */
        satd[1] = phx_mbcmp16(c->b_satd, c->fdec, c->fdec_stride,
                              c->fenc, c->fenc_stride)
                + lambda * phx_bs_size_ue(1);             /* :702 */
        phx_predict16(2, c->fdec, c->fdec_stride);        /* DC */
        satd[2] = phx_mbcmp16(c->b_satd, c->fdec, c->fdec_stride,
                              c->fenc, c->fenc_stride)
                + lambda * phx_bs_size_ue(2);             /* :703 */
        COPY2_IF_LT(best, satd[0], best_mode, 0);         /* :704 */
        COPY2_IF_LT(best, satd[1], best_mode, 1);         /* :705 */
        COPY2_IF_LT(best, satd[2], best_mode, 2);         /* :706 */
        if (best <= thresh) {                            /* :709 */
            phx_predict16(3, c->fdec, c->fdec_stride);    /* P */
            satd[3] = phx_mbcmp16(c->b_satd, c->fdec, c->fdec_stride,
                                  c->fenc, c->fenc_stride)
                    + lambda * phx_bs_size_ue(3);         /* :713 */
            COPY2_IF_LT(best, satd[3], best_mode, 3);     /* :714 */
        }
    } else {
        for (; *pm >= 0; ++pm) {                          /* :719-733 */
            int mode = *pm, cost;
            phx_predict16(mode, c->fdec, c->fdec_stride);
            cost = phx_mbcmp16(c->b_satd, c->fdec, c->fdec_stride,
                               c->fenc, c->fenc_stride)
                 + lambda * phx_bs_size_ue(phx_mode16_fix[mode]);
            COPY2_IF_LT(best, cost, best_mode, mode);
        }
    }
    c->satd_i16x16 = best;
    c->pred16 = best_mode;
}

/* x264 encoder/analyse.c:745-862 — 8x8 prediction selection with the exact
 * x3 pre-selection (:784-804), the mbrd==0 analysis shortcut (:800-803), the
 * `3*lambda` predicted-mode bonus (:817-818) and the early-out (:825-826,
 * :859-861).  The C-only reference has no intra_mbcmp_x9_8x8, so the x3
 * branch (not the x9 branch) is authoritative here.  SHADER: B — four
 * scan-order blocks, each reconstructed before the next. */
static void phx_search_i8(phx_analysis_ctx_t *c)
{
    int lambda = c->a.i_lambda;
    int i_cost = lambda * 4;                        /* analyse.c:753 */
    /* :750 — mbrd gates the SATD threshold; I slices pass COST_MAX as inter. */
    int thresh = c->mbrd ? PHX_COST_MAX
                         : (c->i_satd_inter < c->satd_i16x16
                            ? c->i_satd_inter : c->satd_i16x16);
    int idx = 0;
    for (; idx < 4; idx++) {
        int x = 8*(idx&1), y = 8*(idx>>1);
        uint8_t *dst = c->fdec + x + y*c->fdec_stride;
        const uint8_t *src = c->fenc + x + y*c->fenc_stride;
        uint8_t edge[36];
        const int8_t *pm;
        int pred_mode = phx_predict4_mode(c, 4*idx);
        int best = PHX_COST_MAX, best_mode = 0;
        phx_predict_8x8_filter(dst, edge, c->fdec_stride,
                               c->n8[idx], PHX_MB_ALL);
        pm = phx_i8_modes(c->n8[idx]);
        if (pm[5] >= 0) {                           /* :784 — x3 path */
            int satd[4] = { 0, 0, 0, 0 };
            int favor_vertical, i;
            phx_i8_x3(src, c->fenc_stride, dst, edge, c->fdec_stride, satd);
            favor_vertical = satd[1] > satd[0];     /* :788 */
            if (pred_mode < 3) satd[pred_mode] -= 3*lambda; /* :789-790 */
            for (i = 2; i >= 0; i--) {              /* :791-796 */
                int cost = satd[i] + 4*lambda;
                COPY2_IF_LT(best, cost, best_mode, i);
            }
            if (c->mbrd < 1 + c->b_fast_intra)      /* :800-803 */
                pm = phx_intra_analysis_shortcut[c->b_avoid_topright]
                                                [pm[8] >= 0][favor_vertical];
            else
                pm += 3;
        }
        for (; *pm >= 0 && (best >= 0 || c->mbrd >= 2); ++pm) {
            int mode = *pm, satd;
            phx_predict8(mode, dst, edge, c->fdec_stride);
            satd = phx_sa8d_8x8(dst, c->fdec_stride, src, c->fenc_stride);
            if (pred_mode == PHX_MODE4_FIX(mode))
                satd -= 3*lambda;                   /* :817-818 */
            COPY2_IF_LT(best, satd, best_mode, mode);
        }
        c->pred8[idx] = best_mode;
        i_cost += best + 3*lambda;                  /* :823 */
        if (idx == 3 || i_cost > thresh)            /* :825-826 */
            break;
        phx_predict8(best_mode, dst, edge, c->fdec_stride);
        { phx_dctcoef coef[64]; phx_encode_i8(c, idx, coef); (void)coef; }
        c->cache->intra4x4_pred_mode[phx_scan8[4*idx]] = (int8_t)best_mode;
    }
    if (idx == 3) {
        c->satd_i8x8 = i_cost;
        /* i_skip_intra buffers (:840-850) are not representable (TODO). */
    } else {
        static const uint16_t div_fix8[3] = { 1024, 512, 341 }; /* :854 */
        if (idx < 3) i_cost = (i_cost * div_fix8[idx]) >> 8;    /* :856 */
        c->satd_i8x8 = PHX_COST_MAX;                /* :855 */
    }
    if (c->b_early_terminate) {                     /* :859-861 */
        int sm = c->p->i_subpel_refine > 10 ? 10
               : c->p->i_subpel_refine < 0 ? 0 : c->p->i_subpel_refine;
        int v = i_cost < c->satd_i16x16 ? i_cost : c->satd_i16x16;
        if (v > (c->i_satd_inter * phx_i8x8_thresh_lut[sm]) >> 2)
            return;                                 /* leaves satd_i8x8=COST_MAX */
    }
}

/* x264 encoder/analyse.c:864-979 — 4x4 prediction selection (see below).
 * SHADER: B — 16 blocks in scan order, each reconstructing before the next. */
static void phx_search_i4(phx_analysis_ctx_t *c)
{
    int lambda = c->a.i_lambda;
    int i_cost = lambda * (24+16);                  /* analyse.c:867 */
    int thresh;                                     /* :868 */
    int idx = 0;

    if (c->b_early_terminate) {
        int m = c->satd_i16x16 < c->satd_i8x8 ? c->satd_i16x16 : c->satd_i8x8;
        thresh = c->i_satd_inter < m ? c->i_satd_inter : m; /* MIN3 */
        /* :871-872 — absent at mbrd==0 (veryfast/faster/superfast). */
        if (c->mbrd) thresh = thresh * (10 - c->b_fast_intra) / 8;
    } else {
        thresh = PHX_COST_MAX;
    }

    for (; idx < 16; idx++) {
        const uint8_t *src = c->fenc + phx_block_xy[idx][0]
                           + phx_block_xy[idx][1]*c->fenc_stride;
        uint8_t *dst = c->fdec + phx_block_xy[idx][0]
                     + phx_block_xy[idx][1]*c->fdec_stride;
        const int8_t *pm;
        int pred_mode = phx_predict4_mode(c, idx);
        int best = PHX_COST_MAX, best_mode = 0;
        if ((c->n4[idx] & (PHX_MB_TOPRIGHT|PHX_MB_TOP)) == PHX_MB_TOP)
            memcpy(&dst[4 - c->fdec_stride], &dst[3 - c->fdec_stride], 4);
                                                    /* analyse.c:886-888 */

        pm = phx_i4_modes(c->n4[idx]);
        if (pm[5] >= 0) {                           /* :903 — x3 path */
            int satd[4] = { 0, 0, 0, 0 };
            int favor_vertical;
            phx_i4_x3(c, src, c->fenc_stride, dst, c->fdec_stride, satd);
            favor_vertical = satd[1] > satd[0];      /* :907 */
            if (pred_mode < 3) satd[pred_mode] -= 3*lambda; /* :908-909 */
            best = satd[2]; best_mode = 2;           /* :910 */
            COPY2_IF_LT(best, satd[1], best_mode, 1); /* :911 */
            COPY2_IF_LT(best, satd[0], best_mode, 0); /* :912 */
            if (c->mbrd < 1 + c->b_fast_intra)       /* :916-919 */
                pm = phx_intra_analysis_shortcut[c->b_avoid_topright]
                                                [pm[8] >= 0][favor_vertical];
            else
                pm += 3;
        }
        if (best > 0) {                             /* :922 */
            for (; *pm >= 0; ++pm) {
                int mode = *pm, satd;
                phx_predict4(mode, dst, c->fdec_stride);
                satd = phx_mbcmp4(c->b_satd, src, c->fenc_stride,
                                  dst, c->fdec_stride);  /* :934 */
                if (pred_mode == PHX_MODE4_FIX(mode)) { /* :935-937 */
                    satd -= 3*lambda;
                    if (satd <= 0) {
                        best = satd; best_mode = mode; break; /* :939-943 */
                    }
                }
                COPY2_IF_LT(best, satd, best_mode, mode); /* :946 */
            }
        }
        c->pred4[idx] = best_mode;
        i_cost += best + 3*lambda;                  /* :950 */
        if (i_cost > thresh || idx == 15)           /* :951 */
            break;
        phx_predict4(best_mode, dst, c->fdec_stride); /* :953-956 */
        { phx_dctcoef coef[16]; phx_encode_i4(c, idx, coef); (void)coef; }
        c->cache->intra4x4_pred_mode[phx_scan8[idx]] = (int8_t)best_mode; /* :957 */
    }
    if (idx == 15) {
        c->satd_i4x4 = i_cost;                      /* :964 */
        /* i_skip_intra buffers (:965-974) are not representable (TODO). */
    } else {
        c->satd_i4x4 = PHX_COST_MAX;                /* :978 */
    }
}

/* x264 common/pixel.c:55-73 — PIXEL_SAD_C(4,4) and the sibling SSD kernel.
 * SHADER: A — one 4x4 reduction over fenc/fdec (or fenc/zero). */
static int phx_pixel_sad_4x4(const uint8_t *p1, int s1,
                             const uint8_t *p2, int s2)
{
    int sum = 0;                                    /* pixel.c:59 */
    for (int y = 0; y < 4; y++, p1 += s1, p2 += s2) /* :60 */
        for (int x = 0; x < 4; x++)                 /* :61 */
            sum += abs((int)p1[x] - (int)p2[x]);    /* :64 */
    return sum;                                     /* :69 */
}
static int phx_pixel_ssd_4x4(const uint8_t *p1, int s1,
                             const uint8_t *p2, int s2)
{
    int sum = 0;
    for (int y = 0; y < 4; y++, p1 += s1, p2 += s2)
        for (int x = 0; x < 4; x++) {
            int d = (int)p1[x] - (int)p2[x];
            sum += d*d;
        }
    return sum;
}

/* x264 encoder/rdo.c:94-112 — cached_satd() for PIXEL_4x4 (index 2 in the
 * :96-98 tables): SATD(fenc,zero) - SAD(fenc,zero)>>1.  Phoenix has no
 * frame-level cache, so recompute the same value from the source block; the
 * cache in :101-110 is only a speed optimisation.  SHADER: A — pure fenc. */
static int phx_rdo_satd_ac_4x4(const uint8_t *p, int stride)
{
    static const uint8_t zero[16] = { 0 };
    int dc = phx_pixel_sad_4x4(p, stride, zero, 0) >> 1;    /* rdo.c:107 */
    return phx_satd_4x4(p, stride, zero, 0) - dc;           /* :108 */
}

/* x264 encoder/rdo.c:123-148 — ssd_plane(PIXEL_4x4, p=0): SSD plus the psy-RD
 * "absolute difference of complexities" term.  i_psy_rd is FIX8(f_psy_rd)
 * for subme>=6 and 0 otherwise (encoder.c:1218; the fenc SATD cache that
 * analyse.c:570-584 resets is only an optimisation of rdo.c:94-112).
 * SHADER: A — one 4x4 comparison, no hidden state. */
static int phx_rdo_ssd4(const phx_analysis_ctx_t *c,
                        const uint8_t *src, int ss,
                        const uint8_t *dst, int ds)
{
    int ssd = phx_pixel_ssd_4x4(src, ss, dst, ds);         /* rdo.c:147 */
    int psy_rd = c->p->i_subpel_refine >= 6
               ? (int)(c->p->f_psy_rd*(1 << 8) + .5) : 0;  /* encoder.c:1218 */
    if (psy_rd) {                                          /* :128 */
        int fenc_ac = phx_rdo_satd_ac_4x4(src, ss);        /* :134/:108 */
        int fdec_ac = phx_rdo_satd_ac_4x4(dst, ds);        /* :133 (uncached) */
        int64_t tmp = ((int64_t)abs(fdec_ac - fenc_ac) * psy_rd
                        * c->a.i_lambda + 128) >> 8;       /* :144 */
        ssd += (int)(tmp < PHX_COST_MAX ? tmp : PHX_COST_MAX); /* :145/:147 */
    }
    return ssd;
}

/* x264 encoder/rdo.c:315-347 — rd_cost_i4x4(), reduced to the 4:2:0 luma
 * plane.  The SSD is measured after the candidate mode has been encoded into
 * p_fdec; the bit cost is the exact FIX8 `f8_bits_encoded` accumulated by the
 * size-only intra4x4_pred_mode + residual writers (cabac.h:99-124,
 * rdo.c:31-69/71-73/315-347, cabac.c:775-858/1203-1211).  No copied
 * arithmetic coder and no byte-position estimate is used.  SHADER: B —
 * one candidate cost, no hidden state. */
static uint64_t phx_analysis_rd_i4(phx_analysis_ctx_t *c, int idx,
                                   int mode, const phx_dctcoef coef[16])
{
    phx_mb_cache_t cc = *c->cache;                  /* local cache copy */
    phx_cabac_mb_t mb;
    phx_cabac_mb_init(&mb, &cc);                    /* cabac.c:32 */
    mb.i_type = PHX_I_4x4;
    mb.i_partition = PHX_D_16x16;
    mb.i_intra16x16_pred_mode = c->pred16;
    mb.i_chroma_pred_mode = c->cache->intra4x4_pred_mode[phx_scan8[0]];
    mb.i_qp = c->qp;
    mb.i_last_qp = c->qp; mb.i_last_dqp = 0;
    mb.slice_type = c->slice_type;
    mb.i_fref[0] = 0; mb.i_fref[1] = 0;

    int pred = phx_predict4_mode(c, idx);            /* cabac.c:1205 */
    /* rdo.c:341 — lambda2 here is a->i_lambda2 (analyse.c:259), so the FIX8
     * bit total is scaled by >>8 (not the trellis >>4 of rdo.c:185). */
    uint64_t i_bits = ((uint64_t)phx_rdo_size_partition_i4x4(
                           c->cb, &mb, idx, pred, mode, coef,
                           c->slice_type, c->qp)
                       * (unsigned)c->a.i_lambda2 + 128) >> 8;

    const uint8_t *src = c->fenc + phx_block_xy[idx][0]
                       + phx_block_xy[idx][1]*c->fenc_stride;
    const uint8_t *dst = c->fdec + phx_block_xy[idx][0]
                       + phx_block_xy[idx][1]*c->fdec_stride;
    uint64_t ssd = (uint64_t)phx_rdo_ssd4(c, src, c->fenc_stride,
                                           dst, c->fdec_stride); /* rdo.c:327 */
    return (ssd << 8) + i_bits;                      /* rdo.c:346 */
}

/* x264 encoder/analyse.c:1086-1135 — the I_4x4 half of intra_rd_refine().
 * With a->i_mbrd >= 1 the SATD winner of each block is re-tested over the
 * full mode list using rd_cost_i4x4(); the best reconstructed pixels and
 * coefficients are restored before the next block.  SHADER: B — one block
 * at a time, no hidden state. */
static void phx_analysis_intra_rd_refine_i4(phx_analysis_ctx_t *c,
                                             phx_mb_decision_t *d)
{
    for (int idx = 0; idx < 16; idx++) {
        uint8_t *dst = c->fdec + phx_block_xy[idx][0]
                     + phx_block_xy[idx][1]*c->fdec_stride;
        uint8_t backup[4][4];
        for (int y = 0; y < 4; y++)
            memcpy(backup[y], dst + y*c->fdec_stride, 4);

        uint64_t best_cost = (uint64_t)PHX_COST_MAX << 8;
        int best_mode = c->pred4[idx], best_nz = 0;
        phx_dctcoef best_coef[16];
        memset(best_coef, 0, sizeof(best_coef));
        const int8_t *pm = phx_i4_modes(c->n4[idx]);   /* :1097 */
        for (; *pm >= 0; ++pm) {
            int mode = *pm;
            for (int y = 0; y < 4; y++)
                memcpy(dst + y*c->fdec_stride, backup[y], 4); /* :1124-1130 */
            if ((c->n4[idx] & (PHX_MB_TOPRIGHT|PHX_MB_TOP)) == PHX_MB_TOP)
                memcpy(&dst[4 - c->fdec_stride], &dst[3 - c->fdec_stride], 4);
                                                        /* :1099-1102 */
            phx_predict4(mode, dst, c->fdec_stride);    /* :932 */
            phx_dctcoef coef[16];
            int nz = phx_encode_i4(c, idx, coef);       /* :323 (trellis gate) */
            /* macroblock.h:158 — x264_mb_encode_i4x4 publishes the candidate
             * nnz before partition_i4x4_size_cabac() reads the CBF context. */
            c->cache->non_zero_count[phx_scan8[idx]] = (uint8_t)nz;
            uint64_t cost = phx_analysis_rd_i4(c, idx, mode, coef); /* :1107 */
            if (cost < best_cost) {
                best_cost = cost;
                best_mode = mode;
                best_nz = nz;
                memcpy(best_coef, coef, sizeof(best_coef));
                for (int y = 0; y < 4; y++)
                    memcpy(backup[y], dst + y*c->fdec_stride, 4);
            }
        }
        for (int y = 0; y < 4; y++)                    /* :1124-1131 */
            memcpy(dst + y*c->fdec_stride, backup[y], 4);
        c->cache->non_zero_count[phx_scan8[idx]] = (uint8_t)best_nz; /* :1130 */
        c->pred4[idx] = best_mode;                     /* :1111 */
        c->cache->intra4x4_pred_mode[phx_scan8[idx]] = (int8_t)best_mode;
                                                        /* :1133 */
        /* analyse.c:1111/1133 updates the MB's chosen predictor; Phoenix's
         * decision record is the input to the entropy/encode stages, so the
         * refined mode must be published there too.  Without this CABAC
         * signals the SATD winner while coefficients come from the RD winner
         * (slow/placebo first divergent bin 5). */
        d->intra4x4_pred_mode[idx] = (int8_t)best_mode;
        memcpy(d->dct[phx_scan8[idx]], best_coef, sizeof(best_coef));
        d->nnz[phx_scan8[idx]] = (uint8_t)best_nz;
        if (best_nz) c->cbp_luma |= 1 << (idx >> 2);  /* macroblock.h:161 */
    }
}

/* Stage the 4:2:0 NV12 source window and the reconstructed neighbour samples
 * into per-channel FENC/FDEC scratch windows.  U is the even byte of each
 * NV12 pair, V the odd byte (common/frame.c:89-99/:239-243).  This is the
 * chroma counterpart of p_fenc[1]/p_fenc[2] and p_fdec[1]/p_fdec[2]
 * (common/common.h:570-571, common/macroblock.c:532-548).  Sides without a
 * coded neighbour are seeded with 128, exactly what x264's predictors see at
 * a frame edge (common/macroblock.c:907-912/:960-977, frame.c:535-553).
 * SHADER: A — one per-MB neighbour staging invocation, no hidden state. */
static void phx_analysis_chroma_load(const phx_analysis_ctx_t *c,
                                     uint8_t fenc[2][8*PHX_FENC_STRIDE],
                                     uint8_t fdec[2][PHX_FDEC_STRIDE*10 + PHX_FDEC_STRIDE])
{
    const int x0 = c->mb_x * 8, y0 = c->mb_y * 8;   /* CHROMA_H/V_SHIFT = 1 */
    const phx_frame_t *f = c->f;
    const int cw = f->i_width_planes[1], ch = f->i_lines_planes[1];
    const int s = f->i_stride_planes[1];
    const uint8_t *plane = f->plane_chroma;
    const int in = c->cache->i_neighbour_intra;
    const int fs = PHX_FDEC_STRIDE;

    for (int ch_idx = 0; ch_idx < 2; ch_idx++) {
        uint8_t *p_dst = fdec[ch_idx] + PHX_FDEC_STRIDE + 1; /* common.h:571 */
        for (int y = 0; y < 8; y++)                  /* macroblock.c:369-370 */
            for (int x = 0; x < 8; x++)
                fenc[ch_idx][y*PHX_FENC_STRIDE + x] =
                    plane[(y0+y)*s + 2*(x0+x) + ch_idx]; /* frame.c:240 */

        /* Left column p[-1 + y*FDEC_STRIDE], y = -1..7 (predict.c:289). */
        for (int y = -1; y < 8; y++) {
            uint8_t v = 128;
            if (in & PHX_MB_LEFT) {
                const int sy = phx_clip3(y0 + y, 0, ch - 1);
                const int sx = phx_clip3(x0 - 1, 0, cw - 1);
                v = plane[sy*s + 2*sx + ch_idx];     /* frame.c:535-553 */
            }
            p_dst[-1 + y*fs] = v;
        }
        /* Top row p[x - FDEC_STRIDE], x = 0..7 (predict.c:272/288). */
        for (int x = 0; x < 8; x++) {
            uint8_t v = 128;
            if (in & PHX_MB_TOP) {
                const int sy = phx_clip3(y0 - 1, 0, ch - 1);
                const int sx = phx_clip3(x0 + x, 0, cw - 1);
                v = plane[sy*s + 2*sx + ch_idx];
            }
            p_dst[x - fs] = v;
        }
        /* Top-left is available only when MB_TOPLEFT is in i_neighbour_intra
         * (common/macroblock.c:816-824); the planar predictor uses it. */
        uint8_t v = 128;
        if (in & PHX_MB_TOPLEFT) {
            const int sy = phx_clip3(y0 - 1, 0, ch - 1);
            const int sx = phx_clip3(x0 - 1, 0, cw - 1);
            v = plane[sy*s + 2*sx + ch_idx];
        }
        p_dst[-1 - fs] = v;
    }
}

/* x264 encoder/analyse.c:586-665 — mb_analyse_intra_chroma(), 4:2:0 branch.
 * x264 derives chroma_pred_mode from the SATD of the U/V 8x8 predictions,
 * not from a placeholder: the availability list is analyse.c:615/:537-542,
 * the predictors are analyse.c:650-651 (common/predict.c:936-945) and the
 * cost is analyse.c:632/:655-657 with bs_size_ue (common/bitstream.h:278).
 * The x3 shortcut of analyse.c:619-636 is expressed as the equivalent full
 * mode loop of analyse.c:640-661 (same SATD on the same predictions).  The
 * mode bit-cost follows x264's two branches: raw mode index when pm[3] >= 0
 * (:632), chroma_pred_mode_fix[] otherwise (:657, common/predict.h:45).
 * Chroma coefficients are not produced here: x264 emits them later in
 * x264_macroblock_encode_chroma (encoder/macroblock.c:259-498/:940), which
 * Phoenix calls after the luma encode.  cbp_chroma stays 0 until then.
 * SHADER: B — one invocation per MB, writes only d->chroma_pred_mode. */
static void phx_analysis_intra_chroma(phx_analysis_ctx_t *c, phx_mb_decision_t *d)
{
    if (!c->f->plane_chroma || !c->f->i_width_planes[1] ||
        !c->f->i_lines_planes[1]) {
        d->chroma_pred_mode = 0;                     /* no NV12 plane: TODO */
        c->cbp_chroma = 0;
        return;
    }

    uint8_t fenc[2][8*PHX_FENC_STRIDE];
    uint8_t fdec[2][PHX_FDEC_STRIDE*10 + PHX_FDEC_STRIDE];
    uint8_t *p_dst[2] = { fdec[0] + PHX_FDEC_STRIDE + 1,
                          fdec[1] + PHX_FDEC_STRIDE + 1 }; /* common.h:571 */
    memset(fdec, 128, sizeof(fdec));
    phx_analysis_chroma_load(c, fenc, fdec);

    const int8_t *pm = phx_chroma_modes(c->cache->i_neighbour_intra);
    /* analyse.c:619 — the x3 path is selected when the 4th mode exists. */
    const int raw_mode_cost = pm[3] >= 0;
    phx_predict_t pred8[7];
    phx_predict_8x8c_init(pred8);                    /* predict.c:936-945 */
    int best = PHX_COST_MAX, best_mode = 6;          /* DC128 fallback (:487) */
    int pre[4] = { 0, 0, 0, 0 };                    /* :621-627 x3 cache */
    if (raw_mode_cost) {                            /* :619-627 */
        int su[4], sv[4];
        phx_chroma_x3(c, fenc[0], PHX_FENC_STRIDE,
                      p_dst[0], PHX_FDEC_STRIDE, su);
        phx_chroma_x3(c, fenc[1], PHX_FENC_STRIDE,
                      p_dst[1], PHX_FDEC_STRIDE, sv);
        for (int m = 0; m < 3; m++) pre[m] = su[m] + sv[m];
        pred8[3](p_dst[0], PHX_FDEC_STRIDE);        /* :624 P */
        pred8[3](p_dst[1], PHX_FDEC_STRIDE);        /* :625 */
        pre[3] = phx_mbcmp8(c->b_satd, p_dst[0], PHX_FDEC_STRIDE,
                            fenc[0], PHX_FENC_STRIDE) /* :626 */
               + phx_mbcmp8(c->b_satd, p_dst[1], PHX_FDEC_STRIDE,
                            fenc[1], PHX_FENC_STRIDE); /* :627 */
    }
    for (; *pm >= 0; ++pm) {                         /* analyse.c:629/640 */
        const int mode = *pm;
        int satd;
        if (raw_mode_cost) {
            satd = pre[mode];                        /* :632 */
        } else {
            pred8[mode](p_dst[0], PHX_FDEC_STRIDE);  /* analyse.c:650 */
            pred8[mode](p_dst[1], PHX_FDEC_STRIDE);  /* :651 */
            satd = phx_mbcmp8(c->b_satd, p_dst[0], PHX_FDEC_STRIDE,
                              fenc[0], PHX_FENC_STRIDE) /* :655 */
                 + phx_mbcmp8(c->b_satd, p_dst[1], PHX_FDEC_STRIDE,
                              fenc[1], PHX_FENC_STRIDE); /* :656 */
        }
        satd += c->a.i_lambda *
                phx_bs_size_ue((unsigned)(raw_mode_cost ? mode
                                                        : phx_chroma_fix[mode]));
        COPY2_IF_LT(best, satd, best_mode, mode);    /* :635/:660 */
    }
    d->chroma_pred_mode = (int8_t)best_mode;         /* analyse.c:664 */
    c->cbp_chroma = 0;                               /* macroblock.c:265 */
}

/* x264 common/quant.c:326-364 — decimate_score16()/decimate_score15() and
 * the x264_decimate_table4 values from common/tables.c:373-376.  The C-only
 * reference build has no SIMD decimate kernel, so these are authoritative.
 * SHADER: A — pure scanned-coefficient reduction, no state. */
static const uint8_t phx_decimate_table4[16] = {
    3, 2, 2, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
static int phx_decimate_score16(const phx_dctcoef *dct)
{
    int i_score = 0, idx = 15;
    while (idx >= 0 && dct[idx] == 0) idx--;
    while (idx >= 0) {
        int i_run;
        if ((unsigned)(dct[idx--] + 1) > 2) return 9;    /* quant.c:338-339 */
        i_run = 0;
        while (idx >= 0 && dct[idx] == 0) { idx--; i_run++; }
        i_score += phx_decimate_table4[i_run];           /* :347 */
    }
    return i_score;
}
/* quant.c:353-355 — decimate_score15() scores dct[1..15]. */
static int phx_decimate_score15(const phx_dctcoef *dct)
{
    return phx_decimate_score16(dct + 1);
}

/* x264 common/pixel.c:55-73 PIXEL_SAD_C / ssd_8x8 via ssd_4x4 tiles; 4:2:0
 * chroma SSD for the probe's early-out (macroblock.c:1065).
 * SHADER: A — one 8x8 reduction, no state. */
static int phx_ssd8(const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    int sum = 0;
    for (int y = 0; y < 8; y++, p1 += s1, p2 += s2)
        for (int x = 0; x < 8; x++) {
            int d = (int)p1[x] - (int)p2[x];
            sum += d*d;
        }
    return sum;
}

/* x264 encoder/macroblock.c:988-1139 — macroblock_probe_skip_internal() for
 * 8-bit 4:2:0 P_SKIP (encoder/macroblock.h:38-39).  Every threshold, DCT,
 * quant table and decimate score follows the reference exactly; the MV is
 * the cached pskip MV clipped to analyse.c:336-337's mv_min/mv_max and the
 * chroma MV is the qpel y shifted by CHROMA_V_SHIFT (macroblock.c:64).
 * SHADER: B — read-only fenc/fref workgroup, no entropy state. */
int phx_analysis_probe_pskip(const phx_param_t *p, const phx_frame_t *fenc,
                             const phx_frame_t *fref,
                             const phx_mb_cache_t *cache,
                             int mb_x, int mb_y, int qp, int chroma_qp)
{
    phx_quant_tables_t quant;
    uint8_t mc_luma[32*32], mc_u[8*8], mc_v[8*8];
    phx_dctcoef dct4x4[4][16], dctscan[16], dct_dc[4];
    int16_t mv[2];
    int cqp = chroma_qp >= 0 ? chroma_qp : qp;

    if (!p || !fenc || !fref || !cache || !fenc->plane || !fref->plane ||
        !fref->plane_chroma || !fenc->plane_chroma) return 0;
    phx_quant_tables_init(&quant);

    /* macroblock.c:1001-1002 — pskip MV clipped to the full MB MV limits. */
    phx_predict_mv_pskip(cache, mv);                     /* mvpred.c:166-181 */
    int mv_min_x = 4*(-16*mb_x - 24);                    /* analyse.c:336 */
    int mv_max_x = 4*(16*(fenc->i_width/16 - mb_x - 1) + 24); /* :337 */
    int mv_min_y = 4*(-16*mb_y - 24);                    /* :391 */
    int mv_max_y = 4*(16*(fenc->i_height/16 - mb_y - 1) + 24);
    mv[0] = phx_clip3(mv[0], mv_min_x, mv_max_x);
    mv[1] = phx_clip3(mv[1], mv_min_y, mv_max_y);

    /* macroblock.c:1005-1007 — luma MC (weighting is out of scope, no
     * weighted P in the measured presets). */
    if (phx_mc_luma_mb(fref, mc_luma, 32, mb_x, mb_y,
                       mv[0], mv[1], 16, 16)) return 0;

    const uint8_t *src = fenc->plane + mb_y*16*fenc->i_stride + mb_x*16;
    int q = qp < 0 ? 0 : (qp > 51 ? 51 : qp);
    for (int i8x8 = 0; i8x8 < 4; i8x8++) {
        int fx = (i8x8&1)*8, fy = (i8x8>>1)*8;
        int fo = fx + fy*32;
        phx_sub8x8_dct(dct4x4, src + fx + fy*fenc->i_stride, fenc->i_stride,
                       mc_luma + fo, 32);
        /* macroblock.c:1022 — CQM_4PY = 1 (set.h:31). */
        int nz = phx_quant_4x4x4(dct4x4, quant.quant4_mf[1][q],
                                 quant.quant4_bias[1][q]);
        int score = 0;
        for (int i4 = 0; i4 < 4; i4++) {
            if (!(nz & (1 << i4))) continue;
            phx_zigzag_scan4(dctscan, dct4x4[i4]);         /* :1025 */
            score += phx_decimate_score16(dctscan);        /* :1026 */
            if (score >= 6) return 0;                      /* :1027-1028 */
        }
    }

    /* macroblock.c:1033-1123 — 4:2:0 chroma probe. */
    if (mv[0] || mv[1])                                    /* :1044 */
        phx_mc_chroma(mc_u, 8, mc_v, 8, fref->plane_chroma,
                      fref->i_stride, mv[0], mv[1] >> 1, 8, 8);
    else
        phx_mc_deinterleave_chroma(mc_u, 8, mc_v, 8,
                                   fref->plane_chroma, fref->i_stride, 8, 8);

    int thresh = (phx_lambda2_tab[cqp < 0 ? 0 : (cqp > 51 ? 51 : cqp)] + 32) >> 6;
                                                            /* :1037 */
    const uint8_t *fenc_c = fenc->plane_chroma;
    /* macroblock.c:369-370 — U even / V odd bytes of the NV12 pair. */
    uint8_t fenc_u[8*8], fenc_v[8*8];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            const uint8_t *row = fenc_c + (mb_y*8 + y)*fenc->i_stride_planes[1]
                                        + 2*(mb_x*8 + x);
            fenc_u[y*8 + x] = row[0];
            fenc_v[y*8 + x] = row[1];
        }

    const uint8_t *planar[2] = { fenc_u, fenc_v };
    const uint8_t *mc[2] = { mc_u, mc_v };
    int cq = cqp < 0 ? 0 : (cqp > 51 ? 51 : cqp);
    for (int ch = 0; ch < 2; ch++) {
        int ssd = phx_ssd8(mc[ch], 8, planar[ch], 8);      /* :1065 */
        if (ssd < thresh) continue;                        /* :1066-1067 */
        /* :1088 — sub8x8_dct_dc on the 8x8 chroma plane. */
        phx_sub8x8_dct_dc(dct_dc, planar[ch], 8, mc[ch], 8);
        /* :1092-1093 — CQM_4PC = 3 (set.h:34), mf>>1 / bias<<1. */
        int mf = (int)quant.quant4_mf[3][cq][0] >> 1;
        int bias = (int)quant.quant4_bias[3][cq][0] << 1;
        if (phx_quant_2x2_dc(dct_dc, mf, bias)) return 0;
        if (ssd < thresh*4) continue;                      /* :1097-1098 */
        phx_sub8x8_dct(dct4x4, planar[ch], 8, mc[ch], 8);  /* :1103 */
        for (int i = 0; i < 4; i++) dct4x4[i][0] = 0;      /* :1104-1107 */
        int nz = phx_quant_4x4x4(dct4x4, quant.quant4_mf[3][cq],
                                 quant.quant4_bias[3][cq]);/* :1113 */
        int score = 0;
        for (int i4 = 0; i4 < 4; i4++) {
            if (!(nz & (1 << i4))) continue;
            phx_zigzag_scan4(dctscan, dct4x4[i4]);         /* :1116 */
            score += phx_decimate_score15(dctscan);        /* :1117 */
            if (score >= 7) return 0;                      /* :1118-1119 */
        }
    }
    return 1;                                              /* :1125-1126 */
}

/* x264 encoder.c:1585 / macroblock.c:610-618 — effective chroma QP for the
 * probe (macroblock.c:993/:1035).  SHADER: A — table lookup, no state. */
static int phx_chroma_qp_of(const phx_param_t *p, int qp)
{
    /* encoder.c:1585 — chroma_qp_table = i_chroma_qp_table + 12 + offset.
     * The +12 is mandatory; omitting it produced chroma QP 16 instead of 29
     * at luma QP 30 and made both P_SKIP and B_SKIP probes fail. */
    int idx = phx_clip3(PHX_CHROMA_QP_BASE_OFFSET +
                        p->i_chroma_qp_index_offset + qp, 0, 75);
    return phx_chroma_qp_base[idx] + (qp > PHX_QP_MAX_SPEC
                                     ? qp - PHX_QP_MAX_SPEC : 0);
}

/* x264 encoder/me.h:54 / analyse.c:143-156 — m.cost_mv = the lambda-scaled
 * mv bit cost from the QP's p_cost_mv table (centred at zero).  SHADER: A —
 * read-only cost-table lookup. */
static int phx_me_mv_bits(const phx_cost_tables_t *costs, int qp,
                          const int mv[2], const int16_t mvp[2])
{
    const uint16_t *tab = costs->cost_mv[qp];
    int dx = mv[0] - mvp[0], dy = mv[1] - mvp[1];
    if (!tab) return 0;
    /* phx_cost.c:29-37 allocates 2*4*mv_range on either side. */
    if (dx < -64 || dx > 64 || dy < -64 || dy > 64) return 0;
    return (int)tab[dx] + (int)tab[dy];
}

/* x264 encoder/analyse.c:1255-1332 — mb_analyse_inter_p16x16(), reduced to
 * the full-pel, single-reference path that phx_me16() implements.  The
 * ref iteration (:1268) has one element; LOAD_FENC/LOAD_HPELS (:1265/:1274)
 * are the caller's fenc/fref planes; x264_mb_predict_mv_16x16 (:1277) is
 * phx_predict_mv16(); x264_me_search_ref (:1287) is phx_me16()
 * (encoder/me.c:276-419) followed by M4's byte-exact phx_me_refine_subpel_mv():
 * b_refine_qpel == 1 for analyse.c:3119-3126 (mbrd == 0 && subme > 0),
 * otherwise 0 for the in-search refine at encoder/me.c:792-797.  The refined
 * MV is what analyse.c:1299-1301 compares against pskip_mv: a non-zero
 * refined MV keeps P_L0 instead of promoting to P_SKIP.
 * SHADER: B — one ME workgroup invocation, explicit inputs/outputs. */
static int phx_analysis_inter_p16x16(phx_analysis_ctx_t *c,
                                     phx_inter_result_t *out)
{
    memset(out, 0, sizeof(*out));
    /* analyse.c:3038 always runs the 16x16 inter search; the preset's
     * analyse.inter flags only gate the sub-partitions at :3047/:3068.
     * ultrafast clears those flags (base.c:503-505) but still codes P_L0 /
     * P_SKIP, so gating this function on p->analyse_inter was wrong. */
    if (!c->fref || !c->fref->plane) return -1;

    int16_t mvp[2];
    phx_predict_mv16(c->cache, 0, mvp);                   /* analyse.c:1277 */
    int mv_range = c->p->i_me_range > 0 ? c->p->i_me_range : 16;
    int mv[2] = { 0, 0 };
    /* encoder/me.c:276-419; cost tables built with analyse.c:143-188. */
    int cost = phx_me16(c->mb_x*16, c->mb_y*16, c->f, c->fref, c->costs,
                        mv_range, c->a.i_lambda, (int)c->p->i_me_method,
                        mvp, mv);
    if (cost < 0 || cost >= PHX_COST_MAX) return -1;

    /* M4's byte-exact refine: analyse.c:3119-3126 uses x264_me_refine_qpel()
     * (b_refine_qpel == 1) only when mbrd == 0 && subme > 0; the in-search
     * call of me.c:792-797 (b_refine_qpel == 0) is what mbrd > 0 presets use.
     * phx_me16() supplies the fullpel MV/cost; subme comes from the preset. */
    int subme = c->p->i_subpel_refine;
    int b_refine_qpel = (c->mbrd == 0 && subme > 0) ? 1 : 0;
    int16_t refined_mv[2] = { (int16_t)mv[0], (int16_t)mv[1] };
    int refined_cost = cost, refined_cost_mv = 0;
    if (phx_me_refine_subpel_mv(c->f, c->fref, c->mb_x, c->mb_y, c->costs,
                                c->a.i_lambda, mv_range, subme,
                                b_refine_qpel, mvp, refined_mv,
                                &refined_cost, &refined_cost_mv, NULL) == 0) {
        mv[0] = refined_mv[0];
        mv[1] = refined_mv[1];
        cost = refined_cost;                              /* me.h:54 m->cost */
    }

    int sad = phx_inter_sad(c, mv);                       /* :1298 residual */
    out->found = 1;
    out->cost = cost;                                     /* :1312 comparison */
    out->sad = sad;
    out->mv[0] = mv[0]; out->mv[1] = mv[1];

    /* analyse.c:1294-1307 — the b_try_skip path of mb_analyse_inter_p16x16:
     * residual-plus-bit cost below 300*lambda, MV within one qpel of the
     * cached pskip MV, and x264_macroblock_probe_pskip() succeeding.  The
     * old "exact zero SAD" shortcut is not x264's decision rule. */
    if (c->b_try_skip) {
        int16_t pskip[2];
        phx_predict_mv_pskip(c->cache, pskip);            /* mvpred.c:166-181 */
        /* me.h:53 / me.c:991 — m->cost_mv after the refine; the table-based
         * helper is only a fallback if M4's refine rejected the inputs. */
        int q = c->qp < PHX_QP_MAX_SPEC ? c->qp : PHX_QP_MAX_SPEC;
        int mv_bits = refined_cost_mv ? refined_cost_mv
                    : phx_me_mv_bits(c->costs, q, mv, mvp);
        if (cost - mv_bits < 300*c->a.i_lambda &&         /* :1298 */
            abs(mv[0] - pskip[0]) + abs(mv[1] - pskip[1]) <= 1 && /* :1299-1300 */
            phx_analysis_probe_pskip(c->p, c->f, c->fref, c->cache,
                                     c->mb_x, c->mb_y, c->qp,
                                     phx_chroma_qp_of(c->p, c->qp)))
            out->skip = 1;                                /* :1303-1306 */
    }
    return 0;
}

/* ---- B-slice inter analysis (encoder/analyse.c:1844-2074, :3305-3415) ----
 *
 * The B path used to stop after the direct B_SKIP probe: every non-skip MB
 * fell back to intra in phx_encoder.c.  This block ports the two x264 routines
 * that select a real B MB type.  All costs are the literal x264 SATD+bit-cost
 * values; no mode is invented and no B frame is forced (SHADER: B).
 */

/* x264 mb_analyse_inter_direct (:1852/:1880) and the BI cost of
 * mb_analyse_inter_b16x16 (:1979-1988) both need one 16x16 prediction from
 * list 0 ref 0 + list 1 ref 0 with the implicit equal-weight bipred average
 * (common/mc.c:49-75).  `bipred_weight` is h->mb.bipred_weight[r0][r1] (32
 * without weighted bipred).  SHADER: B — two read-only MC windows. */
static void phx_b_pred16(const phx_analysis_ctx_t *c,
                         const phx_analysis_b_input_t *b,
                         const int ref[2],
                         const int16_t mv0[2], const int16_t mv1[2],
                         uint8_t out[16*16])
{
    uint8_t p0[16*16], p1[16*16];
    /* common/macroblock.c:37-153 — a B prediction may be unidirectional when
     * the direct predictor (or the chosen mode) leaves one list unused. */
    if (ref[0] < 0 && ref[1] < 0) { memset(out, 0, 256); return; }
    if (ref[0] < 0) {
        phx_mc_luma_mb(b->fref1, out, 16, c->mb_x, c->mb_y,
                       mv1[0], mv1[1], 16, 16);
        return;
    }
    if (ref[1] < 0) {
        phx_mc_luma_mb(c->fref, out, 16, c->mb_x, c->mb_y,
                       mv0[0], mv0[1], 16, 16);
        return;
    }
    phx_mc_luma_mb(c->fref, p0, 16, c->mb_x, c->mb_y, mv0[0], mv0[1], 16, 16);
    phx_mc_luma_mb(b->fref1, p1, 16, c->mb_x, c->mb_y, mv1[0], mv1[1], 16, 16);
    int w = b->bipred_weight ? b->bipred_weight : 32;   /* mc.c:79-91 */
    for (int i = 0; i < 256; i++)
        out[i] = (uint8_t)((p0[i]*w + p1[i]*(64 - w) + 32) >> 6);
}

/* x264 encoder/rdo.c:123-160 — ssd_mb() of an already-MC'd prediction.  The
 * luma psy-RD complexity term (:128-146) is reproduced from encoder.c:1218
 * (i_psy_rd = FIX8(f_psy_rd) at subme>=6) and analyse.c:269 (i_psy_rd_lambda).
 * SHADER: B — pure reduction, no state. */
static int phx_b_ssd_pred(const phx_analysis_ctx_t *c,
                          const uint8_t pred[16*16],
                          const uint8_t u[8*16], const uint8_t v[8*16])
{
    static const uint8_t zero[16] = { 0 };
    int ssd = 0;
    for (int i = 0; i < 256; i++) {                       /* PIXEL_16x16 SSD */
        int d = (int)c->fenc[i] - (int)pred[i];
        ssd += d * d;
    }
    int psy_rd = c->p->b_psy && c->p->i_subpel_refine >= 6 ? 256 : 0;
    if (psy_rd && c->a.i_psy_rd_lambda) {
        int dc_pred = phx_sad_16x16(pred, 16, zero, 0) >> 1;       /* :141 */
        int dc_fenc = phx_sad_16x16(c->fenc, c->fenc_stride, zero, 0) >> 1;
        int satd = abs(phx_satd_16x16(pred, 16, zero, 0) - dc_pred
                       - (phx_satd_16x16(c->fenc, c->fenc_stride, zero, 0)
                          - dc_fenc));                            /* :142-143 */
        int64_t tmp = ((int64_t)satd * psy_rd * c->a.i_psy_rd_lambda + 128) >> 8;
        ssd += (int)(tmp > PHX_COST_MAX ? PHX_COST_MAX : tmp);     /* :144-145 */
    }
    /* 4:2:0 chroma SSD with the chroma lambda2 offset (rdo.c:153-158). */
    int cs = c->f->i_stride_planes[1];
    int x0 = c->mb_x*8, y0 = c->mb_y*8;
    int chroma_ssd = 0;
    for (int chpl = 0; chpl < 2; chpl++) {
        const uint8_t *cf = c->f->plane_chroma + (size_t)y0*cs + 2*x0 + chpl;
        const uint8_t *cp = chpl ? v : u;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int d = cf[(size_t)y*cs + 2*x] - cp[y*16 + x];
                chroma_ssd += d * d;
            }
    }
    ssd += (int)(((uint64_t)chroma_ssd * c->a.i_chroma_lambda2_offset + 128) >> 8);
    return ssd;
}

/* Build the 4:2:0 chroma prediction for one B MC candidate from the explicit
 * MV/ref pair (macroblock.c:96-100 / mc.c:252-283).  A list with ref < 0 is
 * not read.  SHADER: B — pure MC from read-only reference planes. */
static void phx_b_chroma_pred(const phx_analysis_ctx_t *c,
                              const phx_analysis_b_input_t *b,
                              const int ref[2], const int16_t mv[2][2],
                              uint8_t u[8*16], uint8_t v[8*16])
{
    int cs = c->f->i_stride_planes[1];
    int x0 = c->mb_x*8, y0 = c->mb_y*8;
    uint8_t u0[8*16], v0[8*16], u1[8*16], v1[8*16];
    if (ref[0] >= 0 && ref[1] >= 0) {
        phx_mc_chroma(u0,16,v0,16, c->fref->plane_chroma+(size_t)y0*cs+2*x0,
                      cs, mv[0][0], mv[0][1], 8, 8);
        phx_mc_chroma(u1,16,v1,16, b->fref1->plane_chroma+(size_t)y0*cs+2*x0,
                      cs, mv[1][0], mv[1][1], 8, 8);
        phx_mc_pixel_avg(u,16,u0,16,u1,16,8,8);
        phx_mc_pixel_avg(v,16,v0,16,v1,16,8,8);
    } else if (ref[0] >= 0) {
        phx_mc_chroma(u,16,v,16, c->fref->plane_chroma+(size_t)y0*cs+2*x0,
                      cs, mv[0][0], mv[0][1], 8, 8);
    } else if (ref[1] >= 0) {
        phx_mc_chroma(u,16,v,16, b->fref1->plane_chroma+(size_t)y0*cs+2*x0,
                      cs, mv[1][0], mv[1][1], 8, 8);
    } else { memset(u, 0, 8*16); memset(v, 0, 8*16); }
}

/* x264 analyse.c:3348 ssd_mb() of the direct prediction used by the mbrd
 * B_SKIP gate.  SHADER: B — deterministic workgroup reduction. */
static int phx_b_ssd_mb(const phx_analysis_ctx_t *c,
                        const phx_analysis_b_input_t *b,
                        const phx_mvdirect_result_t *dir)
{
    uint8_t pred[16*16], u[8*16], v[8*16];
    const int dr[2] = { dir->ref[0][0], dir->ref[1][0] };
    phx_b_pred16(c, b, dr, dir->mv[0][0], dir->mv[1][0], pred);
    phx_b_chroma_pred(c, b, dr, dir->mv, u, v);
    return phx_b_ssd_pred(c, pred, u, v);
}

/* x264 encoder/analyse.c:1890-2074 — mb_analyse_inter_b16x16(), reduced to
 * one reference in each list (the only refs Phoenix's DPB supplies).  The
 * list-1-then-list-0 order of :1912-1973 is preserved for MV-predictor
 * consistency; with a single ref in each list the fast-skip branch (:1926/
 * :1951) never triggers unless b_try_skip is armed by a non-mbrd probe.
 * Direct is searched first by the caller (:3383-3384).  SHADER: B — one ME
 * workgroup per list plus one bidirectional cost reduction. */
static void phx_analysis_inter_b16x16(phx_analysis_ctx_t *c,
                                      const phx_analysis_b_input_t *b,
                                      phx_inter_result_t *out)
{
    int mv_range = c->p->i_me_range > 0 ? c->p->i_me_range : 16;
    int subme = c->p->i_subpel_refine;
    /* analyse.c:3493-3513: with mbrd the in-search refine (me.c:792-797,
     * b_refine_qpel == 0) is the only one; mbrd == 0 adds the post-search
     * x264_me_refine_qpel() call (b_refine_qpel == 1) of :3119-3126.  Same
     * rule as the already-ported P16 path (phx_analysis_inter_p16x16). */
    int b_refine_qpel = (c->mbrd == 0 && subme > 0) ? 1 : 0;
    int16_t mv[2][2] = { { 0, 0 }, { 0, 0 } };
    int cost[2] = { PHX_COST_MAX, PHX_COST_MAX };
    int cost_mv[2] = { 0, 0 };

    phx_mb_analyse_load_costs(c->costs, &c->a, 1, 1);    /* :3379 */
    for (int l = 1; l >= 0; l--) {
        const phx_frame_t *fr = l ? b->fref1 : c->fref;
        int16_t mvp[2];
        phx_predict_mv16_l(c->cache, l, 0, mvp);         /* :1936 */
        int fullpel[2] = { 0, 0 };
        int cst = phx_me16(c->mb_x*16, c->mb_y*16, c->f, fr, c->costs,
                           mv_range, c->a.i_lambda, (int)c->p->i_me_method,
                           mvp, fullpel);                /* :1938 */
        if (cst >= 0 && cst < PHX_COST_MAX) {
            int16_t rmv[2] = { (int16_t)fullpel[0], (int16_t)fullpel[1] };
            int rc = cst, rc_mv = 0;
            if (phx_me_refine_subpel_mv(c->f, fr, c->mb_x, c->mb_y, c->costs,
                                        c->a.i_lambda, mv_range, subme,
                                        b_refine_qpel, mvp, rmv,
                                        &rc, &rc_mv, NULL) == 0) {
                mv[l][0] = rmv[0]; mv[l][1] = rmv[1];    /* me.h:55 */
                cost[l] = rc;                             /* me.h:54 */
                cost_mv[l] = rc_mv;                       /* me.h:53 */
            } else {
                mv[l][0] = rmv[0]; mv[l][1] = rmv[1];
                cost[l] = cst + phx_me_mv_bits(c->costs,
                              c->qp < PHX_QP_MAX_SPEC ? c->qp : PHX_QP_MAX_SPEC,
                              fullpel, mvp);
                cost_mv[l] = cost_mv[l] ? cost_mv[l]
                           : phx_me_mv_bits(c->costs,
                              c->qp < PHX_QP_MAX_SPEC ? c->qp : PHX_QP_MAX_SPEC,
                              fullpel, mvp);
            }
        }
        /* REF_COST(l, 0) is zero with one active reference (:1932/:1941). */
    }

    /* :1976-1991 — BI uses the two best 16x16 MVs. */
    uint8_t bi_pred[16*16];
    { const int br[2] = { 0, 0 };
      phx_b_pred16(c, b, br, mv[0], mv[1], bi_pred); }   /* :1979-1986 */
    int cost_bi = phx_mbcmp16(c->b_satd, c->fenc, c->fenc_stride,
                              bi_pred, 16)               /* :1988 */
                + cost_mv[0] + cost_mv[1];               /* :1990-1991 */
    /* :1996-2068 — always try the (0,0,0,0) vector; it avoids errant MVs in
     * fades.  p_cost_mv[-mvp] is the QP table centred on each list's MVP. */
    if (mv[0][0] | mv[0][1] | mv[1][0] | mv[1][1]) {
        int q = c->qp < PHX_QP_MAX_SPEC ? c->qp : PHX_QP_MAX_SPEC;
        const uint16_t *tab0 = c->costs->cost_mv[q];
        int16_t mvp0[2], mvp1[2];
        phx_predict_mv16_l(c->cache, 0, 0, mvp0);
        phx_predict_mv16_l(c->cache, 1, 0, mvp1);
        int l0_0 = (tab0 && -mvp0[0] >= -64 && -mvp0[0] <= 64)
                 ? tab0[-mvp0[0]] : 0;
        int l0_1 = (tab0 && -mvp0[1] >= -64 && -mvp0[1] <= 64)
                 ? tab0[-mvp0[1]] : 0;
        int l1_0 = (tab0 && -mvp1[0] >= -64 && -mvp1[0] <= 64)
                 ? tab0[-mvp1[0]] : 0;
        int l1_1 = (tab0 && -mvp1[1] >= -64 && -mvp1[1] <= 64)
                 ? tab0[-mvp1[1]] : 0;
        static const int16_t zero[2] = { 0, 0 };
        { const int br[2] = { 0, 0 };
          phx_b_pred16(c, b, br, zero, zero, bi_pred); }
        int cost00 = phx_mbcmp16(c->b_satd, c->fenc, c->fenc_stride,
                                 bi_pred, 16) + l0_0 + l0_1 + l1_0 + l1_1;
        if (cost00 < cost_bi) {
            cost_bi = cost00;
            cost_mv[0] = l0_0 + l0_1;                    /* :2064 */
            cost_mv[1] = l1_0 + l1_1;                    /* :2065 */
            mv[0][0] = mv[0][1] = mv[1][0] = mv[1][1] = 0;
        }
    }

    /* Direct cost: analyse.c:1852/1880 (no chroma ME at subme < 9). */
    uint8_t d_pred[16*16];
    out->cost_direct = (int)c->a.i_lambda * phx_mb_b_cost_table[PHX_B_DIRECT];
    { const int dr[2] = { c->b_direct.ref[0][0], c->b_direct.ref[1][0] };
      phx_b_pred16(c, b, dr, out->b_mv[0], out->b_mv[1], d_pred); }
    out->cost_direct += phx_mbcmp16(c->b_satd, c->fenc, c->fenc_stride,
                                    d_pred, 16);

    /* :3397-3402 — the four-way mode selection.  COPY2_IF_LT tie order makes
     * L0 win ties, then L1, then BI, then DIRECT (exactly x264's order). */
    int type = PHX_B_L0_L0, best = cost[0];              /* :3397-3399 */
    PHX_COPY2_IF_LT(best, cost[1],    type, PHX_B_L1_L1); /* :3400 */
    PHX_COPY2_IF_LT(best, cost_bi,    type, PHX_B_BI_BI); /* :3401 */
    PHX_COPY2_IF_LT(best, out->cost_direct, type, PHX_B_DIRECT); /* :3402 */

    /* :2071-2073 — add the mb_type bit cost to every candidate. */
    cost[0] += (int)c->a.i_lambda * phx_mb_b_cost_table[PHX_B_L0_L0];
    cost[1] += (int)c->a.i_lambda * phx_mb_b_cost_table[PHX_B_L1_L1];
    cost_bi += (int)c->a.i_lambda * phx_mb_b_cost_table[PHX_B_BI_BI];

    /* NOTE (explicit limitation): x264's mbrd path runs mb_analyse_b_rd()
     * (:3573-3589) and chooses the final mode by RD, not by these SATD costs.
     * phx_rdo.h does not expose B-slice macroblock_size_cabac (:261), so we
     * keep the exact SATD mode selection of :3397-3402 here; the mbrd-gated
     * B_SKIP decision uses the exact ssd_mb threshold of :3346-3350 instead.
     * No MB type is ever forced by this stage. */

    out->found = 1;
    out->b_type = type;
    out->cost = best;
    out->cost_l0 = cost[0];
    out->cost_l1 = cost[1];
    out->cost_bi = cost_bi;
    if (type == PHX_B_L0_L0) {
        out->b_mv[0][0] = mv[0][0]; out->b_mv[0][1] = mv[0][1];
        out->b_mv[1][0] = out->b_mv[1][1] = 0;
        out->b_ref[0] = 0; out->b_ref[1] = -1;
    } else if (type == PHX_B_L1_L1) {
        out->b_mv[0][0] = out->b_mv[0][1] = 0;
        out->b_mv[1][0] = mv[1][0]; out->b_mv[1][1] = mv[1][1];
        out->b_ref[0] = -1; out->b_ref[1] = 0;
    } else if (type == PHX_B_BI_BI) {
        out->b_mv[0][0] = mv[0][0]; out->b_mv[0][1] = mv[0][1];
        out->b_mv[1][0] = mv[1][0]; out->b_mv[1][1] = mv[1][1];
        out->b_ref[0] = 0; out->b_ref[1] = 0;
    } else { /* B_DIRECT — the caller already published the direct arrays. */
        out->b_ref[0] = c->b_direct.ref[0][0];
        out->b_ref[1] = c->b_direct.ref[1][0];
    }
}

/* ---- driver (encoder/analyse.c:2918-2952, :3734-3753) ------------------- */

/* x264 encoder/analyse.c:294-305 — mb_analyse_init() (I-slice/progressive
 * subset).  b_early_terminate (:303) and the SAD/SATD mbcmp selection
 * (encoder.c:1409-1411) are now explicit; b_fast_intra/b_avoid_topright are
 * the P-slice fast-intra heuristics that need frame-level statistics (TODO,
 * always 0 here).  mb_init_fenc_cache (:570-584) is a cache reset plus
 * psy-trellis source-DCT init: Phoenix recomputes the SATD/SSD metrics from
 * source on demand, so the reset has no observable cost, and psy-trellis is
 * disabled in all measured presets (base.c:440/:621/:629/:652/:690);
 * phx_rdo_set_config() carries fenc_dct4 into the trellis calls below.
 * SHADER: B — sets up the per-MB analysis state from the ported tables. */
static void phx_analysis_init(phx_analysis_ctx_t *c, int qp)
{
    int subme = c->p->i_subpel_refine;
    /* x264 analyse.c:301 — a->i_mbrd drives both the trellis switch (:261)
     * and the RD decision (:2940-2951); keep it in the context as well as
     * passing it into init_qp. */
    c->mbrd = (subme >= 6) + (subme >= 8) + (subme >= 10);
    c->b_early_terminate = subme < 11;              /* analyse.c:303 */
    c->b_satd = subme > 1;                          /* encoder.c:1411 */
    c->b_fast_intra = 0;                            /* analyse.c:320/:457 TODO */
    c->b_avoid_topright = 0;                        /* analyse.c:321/:466 TODO */
    c->i_satd_inter = PHX_COST_MAX;                 /* analyse.c:2939 (I) */
    phx_mb_analyse_init_qp(c->p, qp, c->p->i_chroma_qp_index_offset,
                           c->mbrd, &c->a);
    c->cache->b_transform_8x8 = 0;                   /* analyse.c:307 */
    c->satd_i16x16 = c->satd_i8x8 = c->satd_i4x4 = PHX_COST_MAX;
                                                    /* analyse.c:310-312 */
    c->pred16 = 0;
    for (int i = 0; i < 4; i++) c->pred8[i] = 0;
    for (int i = 0; i < 16; i++) c->pred4[i] = 2;
    c->cbp_luma = 0;
    c->cbp_chroma = 0;
}

/* x264 encoder/analyse.c:2943-2948 (I) / :3056-3058 + :3200-3223 (P) and
 * :3734-3753 — analyse_update_cache(), reduced to phx_mb_decision_t.
 * For P slices the inter candidate is compared against every intra cost
 * exactly as analyse.c:3221-3223; PHX_P_L0 carries ref[0]=0, the ME MV in
 * qpel units and cbp=0 (no residual is fabricated).  TODO: PCM candidate
 * (:2947), RD refinement and chroma encode.
 * SHADER: B — writes one explicit decision record. */
static void phx_analysis_finalise(phx_analysis_ctx_t *c, phx_mb_decision_t *d,
                                  const phx_inter_result_t *inter)
{
    int cost = c->satd_i16x16;
    int type = PHX_I_16x16;
    PHX_COPY2_IF_LT(cost, c->satd_i4x4,  type, PHX_I_4x4);
    PHX_COPY2_IF_LT(cost, c->satd_i8x8,  type, PHX_I_8x8);
    /* i_satd_pcm == COST_MAX: PCM is not implemented (TODO). */
    if (inter && inter->found) {
        cost = inter->cost;                             /* analyse.c:3058 */
        /* analyse.c:3021/:3040 — P_SKIP is a distinct MB type, not a
         * zero-residual P_L0; the entropy writer keys off d.skip.
         * B_SKIP (analyse.c:3338-3358) is the B-slice equivalent. */
        /* B_SKIP and the four B16x16 modes (:3397-3402) are distinct MB
         * types; the intra comparison of :3616-3619 can still override them. */
        type = inter->b_skip ? PHX_B_SKIP
             : (inter->b_type >= PHX_B_DIRECT &&
                inter->b_type <= PHX_B_BI_BI) ? inter->b_type
             : inter->skip ? PHX_P_SKIP : PHX_P_L0;    /* :3021/:3056 */
        PHX_COPY2_IF_LT(cost, c->satd_i16x16, type, PHX_I_16x16); /* :3221 */
        PHX_COPY2_IF_LT(cost, c->satd_i8x8,   type, PHX_I_8x8);   /* :3222 */
        PHX_COPY2_IF_LT(cost, c->satd_i4x4,   type, PHX_I_4x4);   /* :3223 */
    }

    for (int l = 0; l < 2; l++)
        for (int i = 0; i < 16; i++) {
            d->ref[l][i] = -1;
            d->mv[l][i][0] = d->mv[l][i][1] = 0;
        }
    d->intra8x8_pred_mode[0] = d->intra8x8_pred_mode[1] =
    d->intra8x8_pred_mode[2] = d->intra8x8_pred_mode[3] = 0;
    for (int i = 0; i < 16; i++) d->intra4x4_pred_mode[i] = 2;
    memset(d->nnz, 0, sizeof(d->nnz));
    memset(d->dct, 0, sizeof(d->dct));
    d->skip = 0;

    if (type == PHX_B_SKIP) {
        /* analyse.c:3338-3358 / macroblock.c:966-970 — B_SKIP carries the
         * direct MVs/refs on all 16 4x4 blocks, zero residual and no cbp.
         * B1's phx_mvdirect_result_t uses the same raster-4x4 layout. */
        d->skip = 1;
        d->i_partition = PHX_MVDIR_D_16x16;
        for (int l = 0; l < 2; l++)
            for (int i = 0; i < 16; i++) {
                d->ref[l][i] = c->b_direct.ref[l][i];
                d->mv[l][i][0] = c->b_direct.mv[l][i][0];
                d->mv[l][i][1] = c->b_direct.mv[l][i][1];
                c->cache->ref[l][phx_scan8[i]] = d->ref[l][i];
                c->cache->mv[l][phx_scan8[i]][0] = d->mv[l][i][0];
                c->cache->mv[l][phx_scan8[i]][1] = d->mv[l][i][1];
            }
        c->cbp_luma = c->cbp_chroma = 0;
    } else if (type >= PHX_B_DIRECT && type <= PHX_B_BI_BI) {
        /* analyse.c:3718/3735-3753 (analyse_update_cache) — a coded B 16x16
         * mode carries one MV/ref pair per list on all 16 4x4 blocks, plus the
         * MB-type bit cost already added in phx_analysis_inter_b16x16.  B_SKIP
         * is the only B type with d.skip set; B_DIRECT is a coded MB with the
         * direct predictor's MVs/refs.  No residual is fabricated here: the
         * row-18 encoder in phx_mbencode.c recomputes it from fenc/FDEC. */
        d->skip = 0;
        d->i_partition = PHX_D_16x16;                    /* :3398 */
        for (int i = 0; i < 16; i++) {
            int k = phx_scan8[i];
            for (int l = 0; l < 2; l++) {
                if (type == PHX_B_DIRECT) {
                    d->ref[l][i] = c->b_direct.ref[l][i];
                    d->mv[l][i][0] = c->b_direct.mv[l][i][0];
                    d->mv[l][i][1] = c->b_direct.mv[l][i][1];
                } else {
                    d->ref[l][i] = inter->b_ref[l];
                    d->mv[l][i][0] = inter->b_mv[l][0];
                    d->mv[l][i][1] = inter->b_mv[l][1];
                }
                c->cache->ref[l][k] = d->ref[l][i];
                c->cache->mv[l][k][0] = d->mv[l][i][0];
                c->cache->mv[l][k][1] = d->mv[l][i][1];
            }
            c->cache->intra4x4_pred_mode[k] = PHX_I_PRED_4x4_DC;
        }
        c->cbp_luma = c->cbp_chroma = 0;                 /* macroblock.c:624 */
    } else if (type == PHX_P_L0 || type == PHX_P_SKIP) {
        /* analyse.c:1316/1326 — ref/mv cached for all 16 4x4 blocks of the
         * 16x16 partition; single list-0 reference, no residual (cbp=0).
         * P_SKIP additionally zeroes mvr[] for future predictors (:3025-3027). */
        int is_skip = type == PHX_P_SKIP;
        for (int i = 0; i < 16; i++) {
            d->ref[0][i] = 0;
            d->ref[1][i] = -1;
            d->mv[0][i][0] = inter->mv[0];              /* qpel, me.h:55 */
            d->mv[0][i][1] = inter->mv[1];
            d->mv[1][i][0] = d->mv[1][i][1] = 0;
            c->cache->ref[0][phx_scan8[i]] = is_skip ? -1 : 0;
            c->cache->mv[0][phx_scan8[i]][0] = is_skip ? 0 : inter->mv[0];
            c->cache->mv[0][phx_scan8[i]][1] = is_skip ? 0 : inter->mv[1];
            /* macroblock.c:1735 — inter MBs carry no intra4x4 predictor. */
            c->cache->intra4x4_pred_mode[phx_scan8[i]] = PHX_I_PRED_4x4_DC;
        }
        d->skip = (uint8_t)inter->skip;
        c->cbp_luma = c->cbp_chroma = 0;                /* macroblock.c:624/943 */
        /* encoder/macroblock.c:953-964 — x264 promotes a zero-residual
         * D_16x16 P_L0 whose MV equals the cached pskip MV to P_SKIP at the
         * end of x264_macroblock_encode.  Publish the same decision here so
         * the entropy stage sees x264's MB type (the encoder still performs
         * the MC at macroblock.c:654-681). */
        if (!d->skip) {
            int16_t pskip[2];
            phx_predict_mv_pskip(c->cache, pskip);      /* mvpred.c:166-181 */
            if (inter->mv[0] == pskip[0] && inter->mv[1] == pskip[1]) {
                d->skip = 1;
                type = PHX_P_SKIP;                      /* macroblock.c:963 */
                for (int i = 0; i < 16; i++) {
                    c->cache->ref[0][phx_scan8[i]] = -1;
                    c->cache->mv[0][phx_scan8[i]][0] = 0;
                    c->cache->mv[0][phx_scan8[i]][1] = 0;
                }
            }
        }
    } else if (type == PHX_I_16x16) {
        phx_predict16(c->pred16, c->fdec, c->fdec_stride);
        phx_encode_i16(c, d);
        d->intra16_pred_mode = (int8_t)c->pred16;
    } else if (type == PHX_I_8x8) {
        d->b_transform_8x8 = 1;                      /* macroblock.c:715 */
        for (int idx = 0; idx < 4; idx++) {
            int x = 8*(idx&1), y = 8*(idx>>1);
            uint8_t *dst = c->fdec + x + y*c->fdec_stride;
            uint8_t edge[36];
            phx_dctcoef coef[64];
            phx_predict_8x8_filter(dst, edge, c->fdec_stride,
                                   c->n8[idx],
                                   phx_pred_neighbours[c->pred8[idx]]);
            phx_predict8(c->pred8[idx], dst, edge, c->fdec_stride);
            if (phx_encode_i8(c, idx, coef)) {
                c->cbp_luma |= 1 << idx;            /* macroblock.h:205 */
                for (int i = 0; i < 4; i++) {
                    int slot = phx_scan8[4*idx+i];  /* shared scan8 cache layout */
                    d->dct[slot][0] = coef[16*i];
                    memcpy(&d->dct[slot][1], &coef[16*i+1],
                           15*sizeof(coef[0]));
                }
                /* x264 encoder/macroblock.h:112-117 (STORE_8x8_NNZ): four
                 * 4x4 nnz flags of the 8x8 block share the 8x8 quant nz. */
                d->nnz[phx_scan8[4*idx]+0] = d->nnz[phx_scan8[4*idx]+1] =
                d->nnz[phx_scan8[4*idx]+8] = d->nnz[phx_scan8[4*idx]+9] = 1;
            } else {
                d->nnz[phx_scan8[4*idx]+0] = d->nnz[phx_scan8[4*idx]+1] =
                d->nnz[phx_scan8[4*idx]+8] = d->nnz[phx_scan8[4*idx]+9] = 0;
            }
            /* analyse.c:3744-3746 stores the raw 8x8 predictor in both the
             * 8x8 decision and the four 4x4 cache slots (rectangle.h:157).
             * The entropy stage applies x264_mb_pred_mode4x4_fix itself. */
            d->intra8x8_pred_mode[idx] = (int8_t)c->pred8[idx];
            for (int j = 0; j < 4; j++)
                d->intra4x4_pred_mode[4*idx+j] = (int8_t)c->pred8[idx];
        }
        d->intra16_pred_mode = (int8_t)c->pred16;
    } else { /* PHX_I_4x4 */
        for (int idx = 0; idx < 16; idx++) {
            uint8_t *dst = c->fdec + phx_block_xy[idx][0]
                         + phx_block_xy[idx][1]*c->fdec_stride;
            phx_dctcoef coef[16];
            if ((c->n4[idx] & (PHX_MB_TOPRIGHT|PHX_MB_TOP)) == PHX_MB_TOP)
                memcpy(&dst[4 - c->fdec_stride], &dst[3 - c->fdec_stride], 4);
            phx_predict4(c->pred4[idx], dst, c->fdec_stride);
            if (phx_encode_i4(c, idx, coef)) {
                c->cbp_luma |= 1 << (idx >> 2);     /* macroblock.h:161 */
                memcpy(d->dct[phx_scan8[idx]], coef, sizeof(coef));
                d->nnz[phx_scan8[idx]] = 1;         /* macroblock.h:158 */
            }
            /* analyse.c:3739-3740 publishes the raw predictor (0..11); the
             * signalled FIX4 mapping is applied by the entropy stage. */
            d->intra4x4_pred_mode[idx] = (int8_t)c->pred4[idx];
        }
        d->intra16_pred_mode = (int8_t)c->pred16;
    }

    int is_inter_mb = (type == PHX_P_L0 || type == PHX_P_SKIP ||
                       type == PHX_B_SKIP ||
                       (type >= PHX_B_DIRECT && type <= PHX_B_BI_BI));
    if (is_inter_mb) {
        /* macroblock.c:940-943 — inter chroma has its own MC path; analysis
         * does not fabricate chroma coefficients.  phx_encoder.c performs the
         * integer-MV chroma copy for this no-residual decision. */
        d->chroma_pred_mode = 0;
        c->cbp_chroma = 0;
    } else {
        phx_analysis_intra_chroma(c, d);
    }
    d->mb_type = type;
    d->i_partition = 16;                            /* D_16x16, macroblock.h:139 */
    d->i_qp = c->qp;
    d->b_transform_8x8 = (type == PHX_I_8x8);
    c->cache->b_transform_8x8 = d->b_transform_8x8;
    d->cbp_luma = c->cbp_luma;
    d->cbp_chroma = c->cbp_chroma;
    d->i_cbp = (c->cbp_chroma << 4) | c->cbp_luma
             | (is_inter_mb ? 0 : 0x1700);
                                                     /* macroblock.c:946-951 */
                                                    /* inter cbp has no DC flags; intra cbp keeps 0x1700 (macroblock.c:946-951) */
}

/* x264 encoder/analyse.c:2918-2952 — x264_macroblock_analyse(), with the P
 * branch of :2953/:3038 and the mode comparison of :3200-3223.  B-slice
 * analysis is still a TODO (no list-1 reference/motion data).
 * SHADER: B — one invocation per macroblock, no hidden state. */
static int phx_macroblock_analyse_impl(const phx_param_t *p,
                                       const phx_frame_t *fenc,
                                       const phx_frame_t *fref,
                                       const phx_analysis_b_input_t *b,
                                       phx_mb_cache_t *cache, phx_cabac_t *cb,
                                       int mb_x, int mb_y, int qp,
                                       int slice_type, phx_mb_decision_t *d)
{
    phx_analysis_ctx_t c;
    phx_inter_result_t inter;
    int q, rc = 0;

    if (!p || !fenc || !cache || !d || !fenc->plane) return -1;
    if (slice_type < PHX_SLICE_P || slice_type > PHX_SLICE_SI) return -1;
    if (mb_x < 0 || mb_y < 0 || mb_x*16 >= fenc->i_width ||
        mb_y*16 >= fenc->i_height) return -1;
    if (qp < 0 || qp > PHX_QP_MAX_MAX) return -1;
    if (fref && (!fref->plane || fref->i_width != fenc->i_width ||
                 fref->i_height != fenc->i_height)) return -1;
    q = qp < PHX_QP_MAX_SPEC ? qp : PHX_QP_MAX_SPEC;

    memset(&c, 0, sizeof(c));
    memset(&inter, 0, sizeof(inter));
    c.p = p; c.f = fenc; c.fref = fref; c.slice_type = slice_type;
    c.cache = cache; c.cb = cb;
    c.mb_x = mb_x; c.mb_y = mb_y; c.qp = qp;
    c.costs = (phx_cost_tables_t *)malloc(sizeof(*c.costs));
    c.quant = (phx_quant_tables_t *)malloc(sizeof(*c.quant));
    if (!c.costs || !c.quant) { rc = -1; goto done; }
    /* x264 encoder.c:1570 — cost tables are normally built once per encoder;
     * this standalone driver builds them per call (TODO: hoist to init). */
    if (phx_analyse_init_costs(c.costs, p->i_me_range > 0 ? p->i_me_range : 16,
                               (int)p->i_me_method, q, q)) { rc = -1; goto done; }
    phx_quant_tables_init(c.quant);

    c.fenc_stride = fenc->i_stride;
    c.fenc = fenc->plane + mb_y*16*fenc->i_stride + mb_x*16;
    c.fdec_stride = cache->fdec_stride > 0 ? cache->fdec_stride : PHX_FDEC_STRIDE;
    if (cache->p_fdec) {
        c.fdec = cache->p_fdec;
    } else {
        c.fdec_buf = (uint8_t *)malloc((size_t)c.fdec_stride * 48);
        if (!c.fdec_buf) { rc = -1; goto done; }
        c.fdec = c.fdec_buf + 2*c.fdec_stride + 1;  /* macroblock.c:533 */
    }

    phx_setup_fdec(&c);
    phx_analysis_init(&c, qp);
    /* analyse.c:2937-2938 — mb_init_fenc_cache() only when RD is active. */
    if (c.mbrd) phx_analysis_mb_init_fenc_cache(&c);
    phx_init_neighbours(&c);

    /* B-slice direct/B_SKIP + B16x16 path (analyse.c:3305-3415).  B1's
     * predictor and probe are byte-exact; all list-1/colocated state arrives
     * in `b` from the caller's DPB.  No B frame is forced when `b` is absent
     * — the existing intra-only fallback stays in place.  SHADER: B. */
    c.fref1 = b ? b->fref1 : NULL;
    c.b = b;
    if (slice_type == PHX_SLICE_B && b && b->fref1 && b->col && fref) {
        phx_mvdirect_input_t din;
        memset(&din, 0, sizeof(din));
        din.cache = cache;
        din.mb_x = mb_x; din.mb_y = mb_y;
        din.mb_xy = mb_y * (fenc->i_width/16) + mb_x;
        /* macroblock.c:682-683 — b8/b4 strides are 2x/4x the MB width. */
        int mbw = fenc->i_width/16;
        din.i_b8_xy = 2*(mb_y * (2*mbw) + mb_x);       /* :682 */
        din.i_b4_xy = 4*(mb_y * (4*mbw) + mb_x);       /* :683 */
        din.direct_mode = p->i_direct_mv_pred;
        din.b_direct_spatial = b->b_direct_spatial;
        din.i_threads = 1;                         /* single-threaded encoder */
        din.col = b->col;
        din.cur_poc = b->cur_poc;
        if (phx_mvdirect_predict(&din, &c.b_direct) == 0 &&
            c.b_direct.available) {
            int bskip = 0;
            if (c.mbrd) {
                /* analyse.c:3346-3350 — with RD active x264 does NOT use the
                 * probe: it MCs the direct prediction and compares its SSD
                 * (including the chroma-lambda and psy-RD terms, rdo.c:150)
                 * against the 6-bit CAVLC header cost. */
                inter.i_bskip_cost = phx_b_ssd_mb(&c, b, &c.b_direct);
                bskip = inter.i_bskip_cost
                      <= ((6 * c.a.i_lambda2 + 128) >> 8);   /* :3350 */
            } else {
                /* analyse.c:3352-3358 — the probe arms b_try_skip; at subme<3
                 * the probe result also decides B_SKIP immediately. */
                c.b_try_skip = phx_mvdirect_probe_bskip(
                    fenc, fref, b->fref1, &c.b_direct, mb_x, mb_y, qp,
                    phx_chroma_qp_of(p, qp),
                    b->bipred_weight ? b->bipred_weight : 32);
                if (p->i_subpel_refine < 3) bskip = c.b_try_skip;
            }
            if (bskip) {
                inter.found = 1;
                inter.b_skip = 1;
                inter.b_type = PHX_B_SKIP;
                inter.cost = 0;
                c.i_satd_inter = 0;
            } else {
                /* analyse.c:3383-3386 — direct is evaluated first, then the
                 * 16x16 L0/L1/BI search, then the four-way mode choice. */
                inter.b_mv[0][0] = c.b_direct.mv[0][0][0];
                inter.b_mv[0][1] = c.b_direct.mv[0][0][1];
                inter.b_mv[1][0] = c.b_direct.mv[1][0][0];
                inter.b_mv[1][1] = c.b_direct.mv[1][0][1];
                phx_analysis_inter_b16x16(&c, b, &inter);
                /* analyse.c:3404-3415 — mbrd early-terminate: when direct is
                 * already within 33/32 of the best mode, an RD pass may still
                 * promote B_SKIP.  The full mb_analyse_b_rd() estimator is not
                 * exposed for B slices (phx_rdo.h:261), so the comparison uses
                 * the same SATD costs as :3397-3402 here; the final mode choice
                 * is still published from the four candidates, never forced. */
                /* analyse.c:3404-3415 — mbrd early-terminate B_SKIP compare.
                 * The full mb_analyse_b_rd() estimator is not exposed for B
                 * slices (phx_rdo.h:261); compare the exact ssd_mb cost of
                 * the direct prediction against the SATD costs of :3397-3402.
                 * This is a gate only — it never forces a B type. */
                if (c.mbrd && c.b_early_terminate &&
                    inter.cost_direct <= inter.cost * 33 / 32 &&
                    inter.i_bskip_cost < inter.cost_l0 &&
                    inter.i_bskip_cost < inter.cost_l1 &&
                    inter.i_bskip_cost < inter.cost_bi &&
                    inter.i_bskip_cost < inter.cost_direct) {
                    inter.b_type = PHX_B_SKIP;
                    inter.b_skip = 1;
                    inter.cost = 0;
                }
                if (inter.found) c.i_satd_inter = inter.cost;
            }
        }
    }

    /* P-slice ME (analyse.c:3036-3038) runs before the intra analysis of
     * :3198 so both costs are available for the :3221-3223 comparison. */
    if (slice_type == PHX_SLICE_P) {
        /* analyse.c:2959/:3007-3013 — fast P_SKIP detection runs *before* the
         * 16x16 ME.  At subme >= 3 it only arms b_try_skip; at lower subme a
         * neighbouring P_SKIP arms the direct probe.  b_fast_pskip == 0
         * (placebo, base.c:598) disables both. */
        if (p->b_fast_pskip) {
            if (p->i_subpel_refine >= 3)
                c.b_try_skip = 1;                       /* analyse.c:3007-3008 */
            else if (cache->type_left[0] == PHX_P_SKIP ||
                     cache->type_top == PHX_P_SKIP ||
                     cache->type_topleft == PHX_P_SKIP ||
                     cache->type_topright == PHX_P_SKIP) /* :3009-3012 */
                c.fast_skip = phx_analysis_probe_pskip(
                    p, fenc, fref, cache, mb_x, mb_y, qp,
                    phx_chroma_qp_of(p, qp));            /* :3013 */
        }
        if (c.fast_skip) {
            /* analyse.c:3019-3027 — P_SKIP with the cached pskip MV and no
             * residual; ME is skipped entirely (the caller returns early at
             * :3040-3045). */
            int16_t skip_mv[2];
            phx_predict_mv_pskip(cache, skip_mv);        /* mvpred.c:166-181 */
            inter.found = 1;
            inter.skip = 1;
            inter.cost = 0;
            inter.mv[0] = skip_mv[0];
            inter.mv[1] = skip_mv[1];
        } else {
            phx_analysis_inter_p16x16(&c, &inter);      /* :3038 */
        }
        /* analyse.c:2939 — i_satd_inter is the P 16x16 ME cost. */
        if (inter.found) c.i_satd_inter = inter.cost;
    }

    /* I-slice path (analyse.c:2934-2952) / P-slice intra trial (:3198): I16
     * is always evaluated; I8/I4 are gated by analyse.intra (base.c:435). */
    phx_search_i16(&c);
    if (p->analyse_intra & PHX_ANALYSE_I8x8) phx_search_i8(&c);
    if (p->analyse_intra & PHX_ANALYSE_I4x4) phx_search_i4(&c);
    phx_analysis_finalise(&c, d, &inter);
    /* analyse.c:2937-2951 — mbrd>=1 runs intra_rd() (whole-MB RD over the
     * SATD-chosen modes), while the *per-block* rd_cost_i4x4() refinement of
     * :1086-1135 is gated by mbrd>=2 (:2950).  The 715-bin probe uses
     * subme=7 => mbrd=1, so no individual I4 mode may be replaced by a
     * per-block RD cost there.  P_L0 carries no intra4x4 predictors. */
    if (c.mbrd > 1 && d->mb_type == PHX_I_4x4 &&
        (p->analyse_intra & PHX_ANALYSE_I4x4)) {
        phx_analysis_intra_rd_refine_i4(&c, d);
        d->cbp_luma = c.cbp_luma;
        d->i_cbp = (d->cbp_chroma << 4) | d->cbp_luma | 0x1700;
    }

done:
    free(c.fdec_buf);
    free(c.costs);
    free(c.quant);
    return rc;
}

/* x264 encoder/analyse.c:2918-2952 — the original seven-argument entry point
 * is the I-slice wrapper, kept so standalone callers (test_phx_mbencode.c)
 * keep their ABI.  SHADER: B — forwards to the explicit slice driver. */
int phx_macroblock_analyse(const phx_param_t *p, const phx_frame_t *f,
                           phx_mb_cache_t *cache, int mb_x, int mb_y,
                           int qp, phx_mb_decision_t *d)
{
    return phx_macroblock_analyse_slice(p, f, NULL, cache, NULL,
                                        mb_x, mb_y, qp, PHX_SLICE_I, d);
}

/* B-direct/B_SKIP entry point — forwards to the shared implementation with
 * the caller's list-1/colocated bundle.  SHADER: B — explicit DPB inputs. */
int phx_macroblock_analyse_slice_b(const phx_param_t *p, const phx_frame_t *fenc,
                                   const phx_frame_t *fref0,
                                   const phx_analysis_b_input_t *b,
                                   phx_mb_cache_t *cache, phx_cabac_t *cb,
                                   int mb_x, int mb_y, int qp,
                                   phx_mb_decision_t *d)
{
    return phx_macroblock_analyse_impl(p, fenc, fref0, b, cache, cb,
                                       mb_x, mb_y, qp, PHX_SLICE_B, d);
}

int phx_macroblock_analyse_slice(const phx_param_t *p, const phx_frame_t *fenc,
                                 const phx_frame_t *fref,
                                 phx_mb_cache_t *cache, phx_cabac_t *cb,
                                 int mb_x, int mb_y, int qp, int slice_type,
                                 phx_mb_decision_t *d)
{
    return phx_macroblock_analyse_impl(p, fenc, fref, NULL, cache, cb,
                                       mb_x, mb_y, qp, slice_type, d);
}
