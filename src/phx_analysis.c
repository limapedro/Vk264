/* Per-MB analysis driver.  Line-for-line where the ported interfaces allow it;
 * every place where Phoenix cannot follow x264 is marked TODO, not guessed.
 *
 * Source files followed (all under reference/x264/):
 *   encoder/analyse.c:143/179/248/255/294/476-558/586-665/668-980/1255-1332/
 *                     2918-2952/3734-3753
 *   encoder/macroblock.h:43/120-213/225-246, encoder/macroblock.c:126-239
 *   common/predict.h:34-104, common/macroblock.c:489-498/1340-1351
 *   common/pixel.c:242-381, common/dct.c:780-846
 *   encoder/me.h:30, common/bitstream.h:301
 *
 * SHADER: B — per-MB workgroup stage, explicit inputs/outputs.
 */
#include "phx_analysis.h"

#include "phx_bitstream.h"
#include "phx_cost.h"
#include "phx_dct.h"
#include "phx_predict.h"
#include "phx_quant.h"
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
    phx_mb_cache_t *cache;
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
 * TODO: phx_frame_t has no reconstructed plane; until the encoder supplies one
 * through cache->p_fdec, outer neighbour samples below are proxied from the
 * source plane. SHADER: B — seeds the per-MB reconstruction scratch. */
static void phx_setup_fdec(phx_analysis_ctx_t *c)
{
    int fs = c->fdec_stride;
    int in = c->cache->i_neighbour_intra;
    uint8_t *d = c->fdec;
    if (c->fdec_buf) {
        memset(c->fdec_buf, 128, (size_t)fs * 48);
    } else {
        /* Caller-owned FDEC: seed only the MB and its one-pixel neighbour
         * border, exactly the region x264 exposes to the predictors. */
        for (int y = -1; y < 16; y++)
            memset(d - 1 + y*fs, 128, 18);
    }
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
    int nz = phx_quant_4x4(d, c->quant->quant4_mf[0][qp],
                              c->quant->quant4_bias[0][qp]);
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
        int nz = phx_quant_4x4x4(&dct4[i8*4], c->quant->quant4_mf[0][qp],
                                    c->quant->quant4_bias[0][qp]);
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
    dc_nz = phx_quant_4x4_dc(dcdc,
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

/* x264 encoder/analyse.c:1255-1332 / encoder/me.c — mb_analyse_inter_p16x16.
 * TODO: phx_frame_t has no fref[]/p_fref[] planes — its lowres[] planes
 * (frame.h:30) are the lookahead pyramid and lowres_mvs (frame.h:35) are MV
 * predictors, not reference samples.  x264_mb_predict_mv_16x16,
 * x264_me_search_ref and x264_me_refine_qpel therefore cannot be called from
 * this API.  The cost tables are wired up (analyse.c:248) but no inter
 * candidate is produced; callers must supply a reference list first.
 * SHADER: B — placeholder for the per-MB ME workgroup stage. */
static void phx_analysis_inter_p16x16_todo(phx_analysis_ctx_t *c)
{
    phx_mb_analyse_load_costs(c->costs, &c->a, 1, 0);
}

/* ---- driver (encoder/analyse.c:2918-2952, :3734-3753) ------------------- */

/* x264 encoder/analyse.c:294-305 — mb_analyse_init() (I-slice/progressive
 * subset).  TODO: RD refinement (i_mbrd>=1) and fast-intra heuristics need
 * frame-level statistics and are not evaluated.
 * SHADER: B — sets up the per-MB analysis state from the ported tables. */
static void phx_analysis_init(phx_analysis_ctx_t *c, int qp)
{
    int subme = c->p->i_subpel_refine;
    /* x264 analyse.c:301 — mbrd is local to a->i_mbrd, which Phoenix's
     * reduced cost state does not carry; pass it into init_qp directly. */
    int mbrd = (subme >= 6) + (subme >= 8) + (subme >= 10);
    phx_mb_analyse_init_qp(c->p, qp, c->p->i_chroma_qp_index_offset,
                           mbrd, &c->a);
    c->cache->b_transform_8x8 = 0;                   /* analyse.c:307 */
    c->satd_i16x16 = c->satd_i8x8 = c->satd_i4x4 = PHX_COST_MAX;
                                                    /* analyse.c:310-312 */
    c->pred16 = 0;
    for (int i = 0; i < 4; i++) c->pred8[i] = 0;
    for (int i = 0; i < 16; i++) c->pred4[i] = 2;
    c->cbp_luma = 0;
    c->cbp_chroma = 0;
}

/* x264 encoder/analyse.c:2943-2948 + :3734-3753 — I-slice type selection and
 * analyse_update_cache(), reduced to the fields of phx_mb_decision_t.
 * TODO: PCM candidate (analyse.c:2947), RD refinement and chroma encode.
 * SHADER: B — writes one explicit decision record. */
static void phx_analysis_finalise(phx_analysis_ctx_t *c, phx_mb_decision_t *d)
{
    int cost = c->satd_i16x16;
    int type = PHX_I_16x16;
    PHX_COPY2_IF_LT(cost, c->satd_i4x4,  type, PHX_I_4x4);
    PHX_COPY2_IF_LT(cost, c->satd_i8x8,  type, PHX_I_8x8);
    /* i_satd_pcm == COST_MAX: PCM is not implemented (TODO). */

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

    if (type == PHX_I_16x16) {
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

    phx_analysis_intra_chroma(c, d);
    d->mb_type = type;
    d->i_partition = 16;                            /* D_16x16, macroblock.h:139 */
    d->i_qp = c->qp;
    d->b_transform_8x8 = (type == PHX_I_8x8);
    c->cache->b_transform_8x8 = d->b_transform_8x8;
    d->cbp_luma = c->cbp_luma;
    d->cbp_chroma = c->cbp_chroma;
    d->i_cbp = (c->cbp_chroma << 4) | c->cbp_luma | 0x1700;
                                                    /* macroblock.c:490 */
}

/* x264 encoder/analyse.c:2918-2952 — x264_macroblock_analyse() (the I-slice
 * branch).  Inter/B-slice analysis is a TODO (phx_frame_t has no references).
 * SHADER: B — one invocation per macroblock, no global state. */
int phx_macroblock_analyse(const phx_param_t *p, const phx_frame_t *f,
                           phx_mb_cache_t *cache, int mb_x, int mb_y,
                           int qp, phx_mb_decision_t *d)
{
    phx_analysis_ctx_t c;
    int q, rc = 0;

    if (!p || !f || !cache || !d || !f->plane) return -1;
    if (mb_x < 0 || mb_y < 0 || mb_x*16 >= f->i_width || mb_y*16 >= f->i_height)
        return -1;
    if (qp < 0 || qp > PHX_QP_MAX_MAX) return -1;
    q = qp < PHX_QP_MAX_SPEC ? qp : PHX_QP_MAX_SPEC;

    memset(&c, 0, sizeof(c));
    c.p = p; c.f = f; c.cache = cache;
    c.mb_x = mb_x; c.mb_y = mb_y; c.qp = qp;
    c.costs = (phx_cost_tables_t *)malloc(sizeof(*c.costs));
    c.quant = (phx_quant_tables_t *)malloc(sizeof(*c.quant));
    if (!c.costs || !c.quant) { rc = -1; goto done; }
    /* x264 encoder.c:1570 — cost tables are normally built once per encoder;
     * this standalone driver builds them per call (TODO: hoist to init). */
    if (phx_analyse_init_costs(c.costs, p->i_me_range > 0 ? p->i_me_range : 16,
                               (int)p->i_me_method, q, q)) { rc = -1; goto done; }
    phx_quant_tables_init(c.quant);

    c.fenc_stride = f->i_stride;
    c.fenc = f->plane + mb_y*16*f->i_stride + mb_x*16;
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

    if (p->analyse_inter)
        phx_analysis_inter_p16x16_todo(&c);          /* TODO: no fref[] */

    /* I-slice path (analyse.c:2934-2952): I16 is always evaluated; I8/I4 are
     * gated by h->param.analyse.intra (base.c:435). */
    phx_search_i16(&c);
    if (p->analyse_intra & PHX_ANALYSE_I8x8) phx_search_i8(&c);
    if (p->analyse_intra & PHX_ANALYSE_I4x4) phx_search_i4(&c);
    phx_analysis_finalise(&c, d);

done:
    free(c.fdec_buf);
    free(c.costs);
    free(c.quant);
    return rc;
}
