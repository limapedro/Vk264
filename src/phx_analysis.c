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
    int slice_type;                   /* PHX_SLICE_* (encoder.c:3522/3530/3539) */
    phx_mb_cache_t *cache;
    phx_cabac_t *cb;                  /* live CABAC state, read-only for trellis */
    int mbrd;                         /* analyse.c:301 (subme>=6)+(subme>=8)+... */
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
} phx_analysis_ctx_t;

/* Full-pel P16 search result, the reduced x264_me_t of analyse.c:1257-1313.
 * `cost` is SAD + lambda*MVbits (encoder/me.h:54), `sad` is the raw residual
 * and `skip` is the conservative P_SKIP probe of analyse.c:1296-1301.
 * SHADER: B — one record per MB, no hidden state. */
typedef struct {
    int found, cost, sad, skip;
    int mv[2];                        /* qpel, x264_me_t.mv (encoder/me.h:55) */
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
static void phx_predict_mv16(const phx_mb_cache_t *cache, int i_ref,
                             int16_t mvp[2])
{
    const int i8 = phx_scan8[0];
    const int i_refa = cache->ref[0][i8 - 1];            /* mvpred.c:131 */
    const int16_t *mv_a = cache->mv[0][i8 - 1];          /* :132 */
    const int i_refb = cache->ref[0][i8 - 8];            /* :133 */
    const int16_t *mv_b = cache->mv[0][i8 - 8];          /* :134 */
    int i_refc = cache->ref[0][i8 - 8 + 4];              /* :135 */
    const int16_t *mv_c = cache->mv[0][i8 - 8 + 4];      /* :136 */
    if (i_refc == -2) {                                  /* :137 */
        i_refc = cache->ref[0][i8 - 8 - 1];              /* :139 */
        mv_c   = cache->mv[0][i8 - 8 - 1];               /* :140 */
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

/* x264 encoder/analyse.c:688-742 — I16x16 SATD selection.  The x3 fast path
 * is replaced by an explicit loop over the same modes (identical costs).
 * SHADER: B — uses the neighbour-mode table and the per-MB lambda. */
static void phx_search_i16(phx_analysis_ctx_t *c)
{
    const int8_t *pm = phx_i16_modes(c->cache->i_neighbour_intra);
    int lambda = c->a.i_lambda;
    int best = PHX_COST_MAX, best_mode = 0;
    for (; *pm >= 0; ++pm) {
        int mode = *pm;
        int satd;
        phx_predict16(mode, c->fdec, c->fdec_stride);
        satd = phx_satd_16x16(c->fenc, c->fenc_stride, c->fdec, c->fdec_stride)
             + lambda * phx_bs_size_te(0, phx_mode16_fix[mode]);
        if (satd < best) { best = satd; best_mode = mode; }
    }
    c->satd_i16x16 = best;
    c->pred16 = best_mode;
}

/* x264 encoder/analyse.c:745-862 — 8x8 prediction selection.  The SSSE3 x9
 * fast path and the directional analysis shortcut are not available; all
 * modes from the availability table are evaluated (same cost formula).
 * SHADER: B — four blocks in scan order, each reconstructing before the next. */
static void phx_search_i8(phx_analysis_ctx_t *c)
{
    int lambda = c->a.i_lambda;
    int i_cost = lambda * 4;                        /* analyse.c:753 */
    for (int idx = 0; idx < 4; idx++) {
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
        for (; *pm >= 0; ++pm) {
            int mode = *pm, satd;
            phx_predict8(mode, dst, edge, c->fdec_stride);
            satd = phx_sa8d_8x8(dst, c->fdec_stride, src, c->fenc_stride)
                 + 4*lambda;                        /* analyse.c:794/821 */
            if (pred_mode == PHX_MODE4_FIX(mode))
                satd -= 3*lambda;                   /* analyse.c:817-818 */
            if (satd < best) { best = satd; best_mode = mode; }
        }
        c->pred8[idx] = best_mode;
        i_cost += best + 3*lambda;                  /* analyse.c:823 */
        phx_predict8(best_mode, dst, edge, c->fdec_stride);
        { phx_dctcoef coef[64]; phx_encode_i8(c, idx, coef); (void)coef; }
        c->cache->intra4x4_pred_mode[phx_scan8[4*idx]] = (int8_t)best_mode;
    }
    c->satd_i8x8 = i_cost;
}

/* x264 encoder/analyse.c:864-979 — 4x4 prediction selection.  The x3/x9
 * shortcuts are replaced by the equivalent full mode loop (analyse.c:924-947).
 * SHADER: B — 16 blocks in scan order, each reconstructing before the next. */
static void phx_search_i4(phx_analysis_ctx_t *c)
{
    int lambda = c->a.i_lambda;
    int i_cost = lambda * (24+16);                  /* analyse.c:867 */
    for (int idx = 0; idx < 16; idx++) {
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
        for (; *pm >= 0; ++pm) {
            int mode = *pm, satd;
            phx_predict4(mode, dst, c->fdec_stride);
            satd = phx_satd_4x4(src, c->fenc_stride, dst, c->fdec_stride);
            if (pred_mode == PHX_MODE4_FIX(mode)) {
                satd -= 3*lambda;                   /* analyse.c:935-937 */
                if (satd <= 0) { best = satd; best_mode = mode; break; }
            }
            if (satd < best) { best = satd; best_mode = mode; }
        }
        c->pred4[idx] = best_mode;
        i_cost += best + 3*lambda;                  /* analyse.c:950 */
        phx_predict4(best_mode, dst, c->fdec_stride);
        { phx_dctcoef coef[16]; phx_encode_i4(c, idx, coef); (void)coef; }
        c->cache->intra4x4_pred_mode[phx_scan8[idx]] = (int8_t)best_mode;
    }
    c->satd_i4x4 = i_cost;
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
    for (; *pm >= 0; ++pm) {                         /* analyse.c:629/640 */
        const int mode = *pm;
        int satd_u, satd_v, satd;
        pred8[mode](p_dst[0], PHX_FDEC_STRIDE);      /* analyse.c:650/:624 */
        pred8[mode](p_dst[1], PHX_FDEC_STRIDE);      /* analyse.c:651/:625 */
        satd_u = phx_satd_8x8(fenc[0], PHX_FENC_STRIDE,
                              p_dst[0], PHX_FDEC_STRIDE); /* :655 */
        satd_v = phx_satd_8x8(fenc[1], PHX_FENC_STRIDE,
                              p_dst[1], PHX_FDEC_STRIDE); /* :656 */
        satd = satd_u + satd_v + c->a.i_lambda *
               phx_bs_size_ue((unsigned)(raw_mode_cost ? mode
                                                       : phx_chroma_fix[mode]));
        if (satd < best) { best = satd; best_mode = mode; } /* :635/:660 */
    }
    d->chroma_pred_mode = (int8_t)best_mode;         /* analyse.c:664 */
    c->cbp_chroma = 0;                               /* macroblock.c:265 */
}

/* x264 encoder/analyse.c:1255-1332 — mb_analyse_inter_p16x16(), reduced to
 * the full-pel, single-reference path that phx_me16() implements.  The
 * ref iteration (:1268) has one element; LOAD_FENC/LOAD_HPELS (:1265/:1274)
 * are the caller's fenc/fref planes; x264_mb_predict_mv_16x16 (:1277) is
 * phx_predict_mv16(); x264_me_search_ref (:1287) is phx_me16()
 * (encoder/me.c:276-419).  QPEL refinement (:3125) and the multi-ref
 * P_SKIP probe (:1279-1301) are TODO; the skip test below is the strict
 * integer-MV subset of :1296-1301 (zero residual + exact pskip MV).
 * SHADER: B — one ME workgroup invocation, explicit inputs/outputs. */
static int phx_analysis_inter_p16x16(phx_analysis_ctx_t *c,
                                     phx_inter_result_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!c->fref || !c->fref->plane || !(c->p->analyse_inter)) return -1;

    int16_t mvp[2];
    phx_predict_mv16(c->cache, 0, mvp);                   /* analyse.c:1277 */
    int mv_range = c->p->i_me_range > 0 ? c->p->i_me_range : 16;
    int mv[2] = { 0, 0 };
    /* encoder/me.c:276-419; cost tables built with analyse.c:143-188. */
    int cost = phx_me16(c->mb_x*16, c->mb_y*16, c->f, c->fref, c->costs,
                        mv_range, c->a.i_lambda, (int)c->p->i_me_method,
                        mvp, mv);
    if (cost < 0 || cost >= PHX_COST_MAX) return -1;

    int sad = phx_inter_sad(c, mv);                       /* :1298 residual */
    out->found = 1;
    out->cost = cost;                                     /* :1312 comparison */
    out->sad = sad;
    out->mv[0] = mv[0]; out->mv[1] = mv[1];

    /* analyse.c:1296-1301 (i_ref == 0, b_try_skip, near pskip MV, probe).
     * We require an exact pskip MV and an exactly zero integer residual;
     * no coefficient or MV is fabricated to force a skip. */
    int16_t pskip[2];
    phx_predict_mv_pskip(c->cache, pskip);                /* mvpred.c:166-181 */
    if (sad == 0 && mv[0] == pskip[0] && mv[1] == pskip[1] &&
        cost < 300 * c->a.i_lambda)
        out->skip = 1;
    return 0;
}

/* ---- driver (encoder/analyse.c:2918-2952, :3734-3753) ------------------- */

/* x264 encoder/analyse.c:294-305 — mb_analyse_init() (I-slice/progressive
 * subset).  TODO: RD refinement (i_mbrd>=1) and fast-intra heuristics need
 * frame-level statistics and are not evaluated.
 * SHADER: B — sets up the per-MB analysis state from the ported tables. */
static void phx_analysis_init(phx_analysis_ctx_t *c, int qp)
{
    int subme = c->p->i_subpel_refine;
    /* x264 analyse.c:301 — a->i_mbrd drives both the trellis switch (:261)
     * and the RD decision (:2940-2951); keep it in the context as well as
     * passing it into init_qp. */
    c->mbrd = (subme >= 6) + (subme >= 8) + (subme >= 10);
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
        type = PHX_P_L0;                                /* analyse.c:3056 */
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

    if (type == PHX_P_L0) {
        /* analyse.c:1316/1326 — ref/mv cached for all 16 4x4 blocks of the
         * 16x16 partition; single list-0 reference, no residual (cbp=0). */
        for (int i = 0; i < 16; i++) {
            d->ref[0][i] = 0;
            d->ref[1][i] = -1;
            d->mv[0][i][0] = inter->mv[0];              /* qpel, me.h:55 */
            d->mv[0][i][1] = inter->mv[1];
            d->mv[1][i][0] = d->mv[1][i][1] = 0;
            c->cache->ref[0][phx_scan8[i]] = 0;         /* analyse.c:1316 */
            c->cache->mv[0][phx_scan8[i]][0] = inter->mv[0];
            c->cache->mv[0][phx_scan8[i]][1] = inter->mv[1];
            /* macroblock.c:1735 — inter MBs carry no intra4x4 predictor. */
            c->cache->intra4x4_pred_mode[phx_scan8[i]] = PHX_I_PRED_4x4_DC;
        }
        d->skip = (uint8_t)inter->skip;
        c->cbp_luma = c->cbp_chroma = 0;                /* macroblock.c:624/943 */
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

    if (type == PHX_P_L0) {
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
             | ((type == PHX_P_L0) ? 0 : 0x1700);     /* macroblock.c:946-951 */
                                                    /* inter cbp has no DC flags; intra cbp keeps 0x1700 (macroblock.c:946-951) */
}

/* x264 encoder/analyse.c:2918-2952 — x264_macroblock_analyse(), with the P
 * branch of :2953/:3038 and the mode comparison of :3200-3223.  B-slice
 * analysis is still a TODO (no list-1 reference/motion data).
 * SHADER: B — one invocation per macroblock, no hidden state. */
int phx_macroblock_analyse_slice(const phx_param_t *p, const phx_frame_t *fenc,
                                 const phx_frame_t *fref,
                                 phx_mb_cache_t *cache, phx_cabac_t *cb,
                                 int mb_x, int mb_y, int qp, int slice_type,
                                 phx_mb_decision_t *d)
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
    phx_init_neighbours(&c);

    /* P-slice ME (analyse.c:3036-3038) runs before the intra analysis of
     * :3198 so both costs are available for the :3221-3223 comparison.  B
     * slices keep the intra-only path until list-1 data is exposed. */
    if (slice_type == PHX_SLICE_P)
        phx_analysis_inter_p16x16(&c, &inter);       /* TODO on no fref */

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
