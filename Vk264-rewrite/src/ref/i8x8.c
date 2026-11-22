/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "i8x8.h"
#include <string.h>

/* ---- x264 common/dct.c DCT8_1D / IDCT8_1D ---- */
#define DCT8_1D(SRC,DST) {\
    int s07 = SRC(0) + SRC(7);\
    int s16 = SRC(1) + SRC(6);\
    int s25 = SRC(2) + SRC(5);\
    int s34 = SRC(3) + SRC(4);\
    int a0 = s07 + s34;\
    int a1 = s16 + s25;\
    int a2 = s07 - s34;\
    int a3 = s16 - s25;\
    int d07 = SRC(0) - SRC(7);\
    int d16 = SRC(1) - SRC(6);\
    int d25 = SRC(2) - SRC(5);\
    int d34 = SRC(3) - SRC(4);\
    int a4 = d16 + d25 + (d07 + (d07>>1));\
    int a5 = d07 - d34 - (d25 + (d25>>1));\
    int a6 = d07 + d34 - (d16 + (d16>>1));\
    int a7 = d16 - d25 + (d34 + (d34>>1));\
    DST(0,  a0 + a1     );\
    DST(1,  a4 + (a7>>2));\
    DST(2,  a2 + (a3>>1));\
    DST(3,  a5 + (a6>>2));\
    DST(4,  a0 - a1     );\
    DST(5,  a6 - (a5>>2));\
    DST(6, (a2>>1) - a3 );\
    DST(7, (a4>>2) - a7 );\
}

#define IDCT8_1D(SRC,DST) {\
    int a0 = SRC(0) + SRC(4);\
    int a2 = SRC(0) - SRC(4);\
    int a4 = (SRC(2)>>1) - SRC(6);\
    int a6 = (SRC(6)>>1) + SRC(2);\
    int b0 = a0 + a6;\
    int b2 = a2 + a4;\
    int b4 = a2 - a4;\
    int b6 = a0 - a6;\
    int a1 = -SRC(3) + SRC(5) - SRC(7) - (SRC(7)>>1);\
    int a3 =  SRC(1) + SRC(7) - SRC(3) - (SRC(3)>>1);\
    int a5 = -SRC(1) + SRC(7) + SRC(5) + (SRC(5)>>1);\
    int a7 =  SRC(3) + SRC(5) + SRC(1) + (SRC(1)>>1);\
    int b1 = (a7>>2) + a1;\
    int b3 =  a3 + (a5>>2);\
    int b5 = (a3>>2) - a5;\
    int b7 =  a7 - (a1>>2);\
    DST(0, b0 + b7); DST(1, b2 + b5); DST(2, b4 + b3); DST(3, b6 + b1);\
    DST(4, b6 - b1); DST(5, b4 - b3); DST(6, b2 - b5); DST(7, b0 - b7);\
}

void vk_ref_dct8x8(int dct[64], const uint8_t src[64], const uint8_t pred[64])
{
    int tmp[64];
    for (int i = 0; i < 64; ++i) tmp[i] = (int)src[i] - (int)pred[i];
#define S1(x) tmp[(x)*8+i]
#define D1(x,v) tmp[(x)*8+i] = (v)
    for (int i = 0; i < 8; ++i) DCT8_1D(S1,D1);
#undef S1
#undef D1
#define S2(x) tmp[i*8+(x)]
#define D2(x,v) dct[(x)*8+i] = (v)
    for (int i = 0; i < 8; ++i) DCT8_1D(S2,D2);
#undef S2
#undef D2
}

void vk_ref_idct8x8_add(uint8_t dst[64], int dct[64])
{
    dct[0] += 32; /* rounding for the >>6 at the end (x264 add8x8_idct8) */
#define S1(x) dct[(x)*8+i]
#define D1(x,v) dct[(x)*8+i] = (v)
    for (int i = 0; i < 8; ++i) IDCT8_1D(S1,D1);
#undef S1
#undef D1
#define S2(x) dct[i*8+(x)]
#define D2(x,v) { int p = dst[i+(x)*8] + ((v)>>6); \
                  dst[i+(x)*8] = (uint8_t)(p<0?0:(p>255?255:p)); }
    for (int i = 0; i < 8; ++i) IDCT8_1D(S2,D2);
#undef S2
#undef D2
}

/* ---- x264 common/set.c quant8 tables (flat CQM) ---- */
static const uint8_t quant8_scan[16] = {
    0,3,4,3, 3,1,5,1, 4,5,2,5, 3,1,5,1
};
static const uint16_t quant8_scale[6][6] = {
    {13107,11428,20972,12222,16777,15481},
    {11916,10826,19174,11058,14980,14290},
    {10082, 8943,15978, 9675,12710,11985},
    { 9362, 8228,14913, 8931,11984,11259},
    { 8192, 7346,13159, 7740,10486, 9777},
    { 7282, 6428,11570, 6830, 9118, 8640}
};
static int shift_r8(int x, int s)
{
    return s <= 0 ? x << -s : (x + (1 << (s-1))) >> s;
}
static int div_r8(int n, int d) { return (n + (d >> 1)) / d; }

void vk_ref_quant_tables8(int qp, int mf[64], int bias[64], int dequant[64])
{
    const int deadzone = 21; /* CQM_8IY, intra deadzone[0] = 32-11 */
    int q = qp % 6, s = qp / 6;
    for (int i = 0; i < 64; ++i) {
        int j = quant8_scan[((i>>1)&12) | (i&3)];
        int base = quant8_scale[q][j];
        int m = shift_r8(base, s);
        mf[i] = m;
        bias[i] = m ? div_r8(deadzone << 10, m) : 0;
        if (bias[i] > (1 << 15) / m) bias[i] = (1 << 15) / m;
    }
    /* dequant8_mf[q][i]*16 is not used on our path (we use the qp-scaled
     * dequant via quant8_scale pattern below); keep the table for callers. */
    (void)dequant;
}

static int quant_one8(int coef, int mf, int bias)
{
    if (coef > 0) return (int)(((uint32_t)bias + (uint32_t)coef) * (uint32_t)mf >> 16);
    return -(int)(((uint32_t)bias + (uint32_t)(-coef)) * (uint32_t)mf >> 16);
}

int vk_ref_quant8(int dct[64], const int mf[64], const int bias[64])
{
    int nz = 0;
    for (int i = 0; i < 64; ++i) {
        dct[i] = quant_one8(dct[i], mf[i], bias[i]);
        nz |= dct[i] != 0;
    }
    return nz;
}

/* dequant8 uses dequant8_scale[q][j]*16 with the same quant8_scan mapping. */
static const uint8_t dequant8_scale[6][6] = {
    {20,18,32,19,25,24},{22,19,35,21,28,26},{26,23,42,24,33,31},
    {28,25,45,26,35,33},{32,28,51,30,40,38},{36,32,58,34,46,43}
};
void vk_ref_dequant8_idct_add(uint8_t dst[64], int dct[64],
                              const int dequant[64], int qp)
{
    (void)dequant;
    int q = qp % 6, i_qbits = qp / 6 - 6;
    int f = i_qbits >= 0 ? 0 : (1 << (-i_qbits - 1));
    for (int i = 0; i < 64; ++i) {
        int j = quant8_scan[((i>>1)&12) | (i&3)];
        int dmf = dequant8_scale[q][j] * 16;
        int v = dct[i] * dmf;
        dct[i] = i_qbits >= 0 ? (v << i_qbits) : ((v + f) >> (-i_qbits));
    }
    vk_ref_idct8x8_add(dst, dct);
}

/* ---- x264 common/dct.c ZIGZAG8_FRAME: ZIG(i,y,x) = dct[x*8+y] ---- */
/* ZIG(i,y,x) = dct[x*8+y]; generated from x264 ZIGZAG8_FRAME. */
static const uint8_t zigzag8_idx[64] = {
     0, 8, 1, 2, 9,16,24,17, 10, 3, 4,11,18,25,32,40,
    33,26,19,12, 5, 6,13,20, 27,34,41,48,56,49,42,35,
    28,21,14, 7,15,22,29,36, 43,50,57,58,51,44,37,30,
    23,31,38,45,52,59,60,53, 46,39,47,54,61,62,55,63
};
void vk_ref_zigzag8(int level[64], const int dct[64])
{
    for (int i = 0; i < 64; ++i) level[i] = dct[zigzag8_idx[i]];
}

void vk_ref_interleave8x8_cavlc(int dst[4][16], const int src[64],
                                uint8_t nnz[4])
{
    /* x264 zigzag_interleave_8x8_cavlc; nnz indexed by scan8:
     * nnz[(i&1) + (i>>1)*8]. */
    for (int i = 0; i < 4; ++i) {
        int nz = 0;
        for (int j = 0; j < 16; ++j) {
            int v = src[i + j*4];
            nz |= v;
            dst[i][j] = v;
        }
        nnz[(i&1) + (i>>1)*8] = (uint8_t)!!nz;
    }
}
