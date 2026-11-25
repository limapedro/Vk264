/* Function-for-function port of x264's scalar common/dct.c (8-bit build).
 * Each citation is the exact source line of reference/x264/common/dct.c.
 * SHADER: A for every transform: one invocation per block, no hidden state.
 */
#include "phx_dct.h"

/* common.h:145-148 — x264_clip_pixel, 8-bit.
 * SHADER: A — per-sample clamp, one lane per pixel. */
static uint8_t phx_clip_pixel(int x)
{
    return (uint8_t)((x & ~255) ? ((-x) >> 31) & 255 : x);
}

/* dct.c:145-155 — pixel_sub_wxh().
 * SHADER: A — per-pixel difference inside one block invocation. */
static void phx_pixel_sub_wxh(phx_dctcoef *diff, int size,
                              const uint8_t *pix1, int i_pix1,
                              const uint8_t *pix2, int i_pix2)
{
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++)
            diff[x + y * size] = (phx_dctcoef)(pix1[x] - pix2[x]);
        pix1 += i_pix1;
        pix2 += i_pix2;
    }
}

/* dct.c:47-76 — dct4x4dc(): 4x4 Hadamard of DC coefficients, /2 scaling.
 * SHADER: A — one invocation per 16-coefficient DC block. */
void phx_dct4x4dc(phx_dctcoef d[16])
{
    phx_dctcoef tmp[16];
    for (int i = 0; i < 4; i++) {
        int s01 = d[i * 4 + 0] + d[i * 4 + 1];
        int d01 = d[i * 4 + 0] - d[i * 4 + 1];
        int s23 = d[i * 4 + 2] + d[i * 4 + 3];
        int d23 = d[i * 4 + 2] - d[i * 4 + 3];
        tmp[0 * 4 + i] = (phx_dctcoef)(s01 + s23);
        tmp[1 * 4 + i] = (phx_dctcoef)(s01 - s23);
        tmp[2 * 4 + i] = (phx_dctcoef)(d01 - d23);
        tmp[3 * 4 + i] = (phx_dctcoef)(d01 + d23);
    }
    for (int i = 0; i < 4; i++) {
        int s01 = tmp[i * 4 + 0] + tmp[i * 4 + 1];
        int d01 = tmp[i * 4 + 0] - tmp[i * 4 + 1];
        int s23 = tmp[i * 4 + 2] + tmp[i * 4 + 3];
        int d23 = tmp[i * 4 + 2] - tmp[i * 4 + 3];
        d[i * 4 + 0] = (phx_dctcoef)((s01 + s23 + 1) >> 1);
        d[i * 4 + 1] = (phx_dctcoef)((s01 - s23 + 1) >> 1);
        d[i * 4 + 2] = (phx_dctcoef)((d01 - d23 + 1) >> 1);
        d[i * 4 + 3] = (phx_dctcoef)((d01 + d23 + 1) >> 1);
    }
}

/* dct.c:78-107 — idct4x4dc(): inverse Hadamard, no /2 scaling.
 * SHADER: A — one invocation per 16-coefficient DC block. */
void phx_idct4x4dc(phx_dctcoef d[16])
{
    phx_dctcoef tmp[16];
    for (int i = 0; i < 4; i++) {
        int s01 = d[i * 4 + 0] + d[i * 4 + 1];
        int d01 = d[i * 4 + 0] - d[i * 4 + 1];
        int s23 = d[i * 4 + 2] + d[i * 4 + 3];
        int d23 = d[i * 4 + 2] - d[i * 4 + 3];
        tmp[0 * 4 + i] = (phx_dctcoef)(s01 + s23);
        tmp[1 * 4 + i] = (phx_dctcoef)(s01 - s23);
        tmp[2 * 4 + i] = (phx_dctcoef)(d01 - d23);
        tmp[3 * 4 + i] = (phx_dctcoef)(d01 + d23);
    }
    for (int i = 0; i < 4; i++) {
        int s01 = tmp[i * 4 + 0] + tmp[i * 4 + 1];
        int d01 = tmp[i * 4 + 0] - tmp[i * 4 + 1];
        int s23 = tmp[i * 4 + 2] + tmp[i * 4 + 3];
        int d23 = tmp[i * 4 + 2] - tmp[i * 4 + 3];
        d[i * 4 + 0] = (phx_dctcoef)(s01 + s23);
        d[i * 4 + 1] = (phx_dctcoef)(s01 - s23);
        d[i * 4 + 2] = (phx_dctcoef)(d01 - d23);
        d[i * 4 + 3] = (phx_dctcoef)(d01 + d23);
    }
}

/* dct.c:109-143 — dct2x4dc(): 2x4 Hadamard of eight 4x4 DC values.
 * SHADER: A — one invocation per chroma DC block. */
void phx_dct2x4dc(phx_dctcoef dct[8], phx_dctcoef dct4x4[8][16])
{
    int a0 = dct4x4[0][0] + dct4x4[1][0];
    int a1 = dct4x4[2][0] + dct4x4[3][0];
    int a2 = dct4x4[4][0] + dct4x4[5][0];
    int a3 = dct4x4[6][0] + dct4x4[7][0];
    int a4 = dct4x4[0][0] - dct4x4[1][0];
    int a5 = dct4x4[2][0] - dct4x4[3][0];
    int a6 = dct4x4[4][0] - dct4x4[5][0];
    int a7 = dct4x4[6][0] - dct4x4[7][0];
    int b0 = a0 + a1, b1 = a2 + a3, b2 = a4 + a5, b3 = a6 + a7;
    int b4 = a0 - a1, b5 = a2 - a3, b6 = a4 - a5, b7 = a6 - a7;
    dct[0] = (phx_dctcoef)(b0 + b1);
    dct[1] = (phx_dctcoef)(b2 + b3);
    dct[2] = (phx_dctcoef)(b0 - b1);
    dct[3] = (phx_dctcoef)(b2 - b3);
    dct[4] = (phx_dctcoef)(b4 - b5);
    dct[5] = (phx_dctcoef)(b6 - b7);
    dct[6] = (phx_dctcoef)(b4 + b5);
    dct[7] = (phx_dctcoef)(b6 + b7);
    for (int i = 0; i < 8; i++) dct4x4[i][0] = 0;
}

/* dct.c:157-189 — sub4x4_dct().
 * SHADER: A — one invocation per 4x4 block. */
void phx_sub4x4_dct(phx_dctcoef dct[16], const uint8_t *pix1, int i_pix1,
                    const uint8_t *pix2, int i_pix2)
{
    phx_dctcoef d[16], tmp[16];
    phx_pixel_sub_wxh(d, 4, pix1, i_pix1, pix2, i_pix2);

    for (int i = 0; i < 4; i++) {
        int s03 = d[i * 4 + 0] + d[i * 4 + 3];
        int s12 = d[i * 4 + 1] + d[i * 4 + 2];
        int d03 = d[i * 4 + 0] - d[i * 4 + 3];
        int d12 = d[i * 4 + 1] - d[i * 4 + 2];
        tmp[0 * 4 + i] = (phx_dctcoef)(s03 + s12);
        tmp[1 * 4 + i] = (phx_dctcoef)(2 * d03 + d12);
        tmp[2 * 4 + i] = (phx_dctcoef)(s03 - s12);
        tmp[3 * 4 + i] = (phx_dctcoef)(d03 - 2 * d12);
    }
    for (int i = 0; i < 4; i++) {
        int s03 = tmp[i * 4 + 0] + tmp[i * 4 + 3];
        int s12 = tmp[i * 4 + 1] + tmp[i * 4 + 2];
        int d03 = tmp[i * 4 + 0] - tmp[i * 4 + 3];
        int d12 = tmp[i * 4 + 1] - tmp[i * 4 + 2];
        dct[i * 4 + 0] = (phx_dctcoef)(s03 + s12);
        dct[i * 4 + 1] = (phx_dctcoef)(2 * d03 + d12);
        dct[i * 4 + 2] = (phx_dctcoef)(s03 - s12);
        dct[i * 4 + 3] = (phx_dctcoef)(d03 - 2 * d12);
    }
}

/* dct.c:191-197 — sub8x8_dct().
 * SHADER: A — one invocation per 8x8 block (four 4x4 DCTs). */
void phx_sub8x8_dct(phx_dctcoef dct[4][16], const uint8_t *pix1, int i_pix1,
                    const uint8_t *pix2, int i_pix2)
{
    phx_sub4x4_dct(dct[0], pix1, i_pix1, pix2, i_pix2);
    phx_sub4x4_dct(dct[1], pix1 + 4, i_pix1, pix2 + 4, i_pix2);
    phx_sub4x4_dct(dct[2], pix1 + 4 * i_pix1, i_pix1, pix2 + 4 * i_pix2, i_pix2);
    phx_sub4x4_dct(dct[3], pix1 + 4 * i_pix1 + 4, i_pix1, pix2 + 4 * i_pix2 + 4, i_pix2);
}

/* dct.c:199-205 — sub16x16_dct().
 * SHADER: A — one invocation per 16x16 block (16 4x4 DCTs). */
void phx_sub16x16_dct(phx_dctcoef dct[16][16], const uint8_t *pix1, int i_pix1,
                      const uint8_t *pix2, int i_pix2)
{
    phx_sub8x8_dct(&dct[0], pix1, i_pix1, pix2, i_pix2);
    phx_sub8x8_dct(&dct[4], pix1 + 8, i_pix1, pix2 + 8, i_pix2);
    phx_sub8x8_dct(&dct[8], pix1 + 8 * i_pix1, i_pix1, pix2 + 8 * i_pix2, i_pix2);
    phx_sub8x8_dct(&dct[12], pix1 + 8 * i_pix1 + 8, i_pix1, pix2 + 8 * i_pix2 + 8, i_pix2);
}

/* dct.c:207-214 — sub4x4_dct_dc().
 * SHADER: A — one invocation per 4x4 DC block. */
int phx_sub4x4_dct_dc(const uint8_t *pix1, int i_pix1,
                      const uint8_t *pix2, int i_pix2)
{
    int sum = 0;
    for (int i = 0; i < 4; i++, pix1 += i_pix1, pix2 += i_pix2)
        sum += pix1[0] + pix1[1] + pix1[2] + pix1[3]
             - pix2[0] - pix2[1] - pix2[2] - pix2[3];
    return sum;
}

/* dct.c:216-232 — sub8x8_dct_dc(), including the 2x2 DC transform.
 * SHADER: A — one invocation per 8x8 DC block. */
void phx_sub8x8_dct_dc(phx_dctcoef dct[4], const uint8_t *pix1, int i_pix1,
                       const uint8_t *pix2, int i_pix2)
{
    dct[0] = (phx_dctcoef)phx_sub4x4_dct_dc(pix1, i_pix1, pix2, i_pix2);
    dct[1] = (phx_dctcoef)phx_sub4x4_dct_dc(pix1 + 4, i_pix1, pix2 + 4, i_pix2);
    dct[2] = (phx_dctcoef)phx_sub4x4_dct_dc(pix1 + 4 * i_pix1, i_pix1, pix2 + 4 * i_pix2, i_pix2);
    dct[3] = (phx_dctcoef)phx_sub4x4_dct_dc(pix1 + 4 * i_pix1 + 4, i_pix1,
                                            pix2 + 4 * i_pix2 + 4, i_pix2);
    int d0 = dct[0] + dct[1];
    int d1 = dct[2] + dct[3];
    int d2 = dct[0] - dct[1];
    int d3 = dct[2] - dct[3];
    dct[0] = (phx_dctcoef)(d0 + d1);
    dct[1] = (phx_dctcoef)(d0 - d1);
    dct[2] = (phx_dctcoef)(d2 + d3);
    dct[3] = (phx_dctcoef)(d2 - d3);
}

/* dct.c:234-270 — sub8x16_dct_dc(), including the 2x4 DC transform.
 * SHADER: A — one invocation per 8x16 DC block. */
void phx_sub8x16_dct_dc(phx_dctcoef dct[8], const uint8_t *pix1, int i_pix1,
                        const uint8_t *pix2, int i_pix2)
{
    int a0 = phx_sub4x4_dct_dc(pix1, i_pix1, pix2, i_pix2);
    int a1 = phx_sub4x4_dct_dc(pix1 + 4, i_pix1, pix2 + 4, i_pix2);
    int a2 = phx_sub4x4_dct_dc(pix1 + 4 * i_pix1, i_pix1, pix2 + 4 * i_pix2, i_pix2);
    int a3 = phx_sub4x4_dct_dc(pix1 + 4 * i_pix1 + 4, i_pix1, pix2 + 4 * i_pix2 + 4, i_pix2);
    int a4 = phx_sub4x4_dct_dc(pix1 + 8 * i_pix1, i_pix1, pix2 + 8 * i_pix2, i_pix2);
    int a5 = phx_sub4x4_dct_dc(pix1 + 8 * i_pix1 + 4, i_pix1, pix2 + 8 * i_pix2 + 4, i_pix2);
    int a6 = phx_sub4x4_dct_dc(pix1 + 12 * i_pix1, i_pix1, pix2 + 12 * i_pix2, i_pix2);
    int a7 = phx_sub4x4_dct_dc(pix1 + 12 * i_pix1 + 4, i_pix1, pix2 + 12 * i_pix2 + 4, i_pix2);

    int b0 = a0 + a1, b1 = a2 + a3, b2 = a4 + a5, b3 = a6 + a7;
    int b4 = a0 - a1, b5 = a2 - a3, b6 = a4 - a5, b7 = a6 - a7;
    a0 = b0 + b1; a1 = b2 + b3; a2 = b4 + b5; a3 = b6 + b7;
    a4 = b0 - b1; a5 = b2 - b3; a6 = b4 - b5; a7 = b6 - b7;
    dct[0] = (phx_dctcoef)(a0 + a1);
    dct[1] = (phx_dctcoef)(a2 + a3);
    dct[2] = (phx_dctcoef)(a0 - a1);
    dct[3] = (phx_dctcoef)(a2 - a3);
    dct[4] = (phx_dctcoef)(a4 - a5);
    dct[5] = (phx_dctcoef)(a6 - a7);
    dct[6] = (phx_dctcoef)(a4 + a5);
    dct[7] = (phx_dctcoef)(a6 + a7);
}

/* dct.c:272-310 — add4x4_idct().
 * SHADER: A — one invocation per 4x4 block. */
void phx_add4x4_idct(uint8_t *dst, int stride, const phx_dctcoef dct[16])
{
    phx_dctcoef d[16], tmp[16];
    for (int i = 0; i < 4; i++) {
        int s02 = dct[0 * 4 + i] + dct[2 * 4 + i];
        int d02 = dct[0 * 4 + i] - dct[2 * 4 + i];
        int s13 = dct[1 * 4 + i] + (dct[3 * 4 + i] >> 1);
        int d13 = (dct[1 * 4 + i] >> 1) - dct[3 * 4 + i];
        tmp[i * 4 + 0] = (phx_dctcoef)(s02 + s13);
        tmp[i * 4 + 1] = (phx_dctcoef)(d02 + d13);
        tmp[i * 4 + 2] = (phx_dctcoef)(d02 - d13);
        tmp[i * 4 + 3] = (phx_dctcoef)(s02 - s13);
    }
    for (int i = 0; i < 4; i++) {
        int s02 = tmp[0 * 4 + i] + tmp[2 * 4 + i];
        int d02 = tmp[0 * 4 + i] - tmp[2 * 4 + i];
        int s13 = tmp[1 * 4 + i] + (tmp[3 * 4 + i] >> 1);
        int d13 = (tmp[1 * 4 + i] >> 1) - tmp[3 * 4 + i];
        d[0 * 4 + i] = (phx_dctcoef)((s02 + s13 + 32) >> 6);
        d[1 * 4 + i] = (phx_dctcoef)((d02 + d13 + 32) >> 6);
        d[2 * 4 + i] = (phx_dctcoef)((d02 - d13 + 32) >> 6);
        d[3 * 4 + i] = (phx_dctcoef)((s02 - s13 + 32) >> 6);
    }
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++)
            dst[x] = phx_clip_pixel(dst[x] + d[y * 4 + x]);
        dst += stride;
    }
}

/* dct.c:312-318 — add8x8_idct().
 * SHADER: A — one invocation per 8x8 block. */
void phx_add8x8_idct(uint8_t *dst, int stride, const phx_dctcoef dct[4][16])
{
    phx_add4x4_idct(dst, stride, dct[0]);
    phx_add4x4_idct(dst + 4, stride, dct[1]);
    phx_add4x4_idct(dst + 4 * stride, stride, dct[2]);
    phx_add4x4_idct(dst + 4 * stride + 4, stride, dct[3]);
}

/* dct.c:320-326 — add16x16_idct().
 * SHADER: A — one invocation per 16x16 block. */
void phx_add16x16_idct(uint8_t *dst, int stride, const phx_dctcoef dct[16][16])
{
    phx_add8x8_idct(dst, stride, &dct[0]);
    phx_add8x8_idct(dst + 8, stride, &dct[4]);
    phx_add8x8_idct(dst + 8 * stride, stride, &dct[8]);
    phx_add8x8_idct(dst + 8 * stride + 8, stride, &dct[12]);
}

/* dct.c:332-357 — DCT8_1D macro. SRC(x)/DST(x) are supplied by callers
 * so the same butterfly runs along columns then rows. */
#define PHX_DCT8_1D {\
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
    DST(0) =  a0 + a1     ;\
    DST(1) =  a4 + (a7>>2);\
    DST(2) =  a2 + (a3>>1);\
    DST(3) =  a5 + (a6>>2);\
    DST(4) =  a0 - a1     ;\
    DST(5) =  a6 - (a5>>2);\
    DST(6) = (a2>>1) - a3 ;\
    DST(7) = (a4>>2) - a7 ;\
}

/* dct.c:359-378 — sub8x8_dct8().
 * SHADER: A — one invocation per 8x8 block. */
void phx_sub8x8_dct8(phx_dctcoef dct[64], const uint8_t *pix1, int i_pix1,
                     const uint8_t *pix2, int i_pix2)
{
    phx_dctcoef tmp[64];
    phx_pixel_sub_wxh(tmp, 8, pix1, i_pix1, pix2, i_pix2);

#define SRC(x) tmp[x * 8 + i]
#define DST(x) tmp[x * 8 + i]
    for (int i = 0; i < 8; i++) PHX_DCT8_1D
#undef SRC
#undef DST
#define SRC(x) tmp[i * 8 + x]
#define DST(x) dct[x * 8 + i]
    for (int i = 0; i < 8; i++) PHX_DCT8_1D
#undef SRC
#undef DST
}

/* dct.c:380-386 — sub16x16_dct8().
 * SHADER: A — one invocation per 16x16 block (four 8x8 DCTs). */
void phx_sub16x16_dct8(phx_dctcoef dct[4][64], const uint8_t *pix1, int i_pix1,
                       const uint8_t *pix2, int i_pix2)
{
    phx_sub8x8_dct8(dct[0], pix1, i_pix1, pix2, i_pix2);
    phx_sub8x8_dct8(dct[1], pix1 + 8, i_pix1, pix2 + 8, i_pix2);
    phx_sub8x8_dct8(dct[2], pix1 + 8 * i_pix1, i_pix1, pix2 + 8 * i_pix2, i_pix2);
    phx_sub8x8_dct8(dct[3], pix1 + 8 * i_pix1 + 8, i_pix1, pix2 + 8 * i_pix2 + 8, i_pix2);
}

/* dct.c:388-413 — IDCT8_1D macro (inverse butterfly). */
#define PHX_IDCT8_1D {\
    int a0 =  SRC(0) + SRC(4);\
    int a2 =  SRC(0) - SRC(4);\
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
    DST(0, b0 + b7);\
    DST(1, b2 + b5);\
    DST(2, b4 + b3);\
    DST(3, b6 + b1);\
    DST(4, b6 - b1);\
    DST(5, b4 - b3);\
    DST(6, b2 - b5);\
    DST(7, b0 - b7);\
}

/* dct.c:415-432 — add8x8_idct8(). The input coefficients are modified in
 * place exactly as in x264 (dct[0] += 32 and both butterfly passes).
 * SHADER: A — one invocation per 8x8 block. */
void phx_add8x8_idct8(uint8_t *dst, int stride, phx_dctcoef dct[64])
{
    dct[0] = (phx_dctcoef)(dct[0] + 32); /* rounding for the >>6 at the end */

#define SRC(x)     dct[x * 8 + i]
#define DST(x, rhs) dct[x * 8 + i] = (phx_dctcoef)(rhs)
    for (int i = 0; i < 8; i++) PHX_IDCT8_1D
#undef SRC
#undef DST
#define SRC(x)     dct[i * 8 + x]
#define DST(x, rhs) dst[i + x * stride] = phx_clip_pixel(dst[i + x * stride] + ((rhs) >> 6))
    for (int i = 0; i < 8; i++) PHX_IDCT8_1D
#undef SRC
#undef DST
}

/* dct.c:434-440 — add16x16_idct8().
 * SHADER: A — one invocation per 16x16 block. */
void phx_add16x16_idct8(uint8_t *dst, int stride, phx_dctcoef dct[4][64])
{
    phx_add8x8_idct8(dst, stride, dct[0]);
    phx_add8x8_idct8(dst + 8, stride, dct[1]);
    phx_add8x8_idct8(dst + 8 * stride, stride, dct[2]);
    phx_add8x8_idct8(dst + 8 * stride + 8, stride, dct[3]);
}

/* dct.c:442-452 — add4x4_idct_dc().
 * SHADER: A — one invocation per 4x4 DC-only block. */
void phx_add4x4_idct_dc(uint8_t *dst, int stride, phx_dctcoef dc)
{
    dc = (phx_dctcoef)((dc + 32) >> 6);
    for (int i = 0; i < 4; i++, dst += stride) {
        dst[0] = phx_clip_pixel(dst[0] + dc);
        dst[1] = phx_clip_pixel(dst[1] + dc);
        dst[2] = phx_clip_pixel(dst[2] + dc);
        dst[3] = phx_clip_pixel(dst[3] + dc);
    }
}

/* dct.c:454-460 — add8x8_idct_dc().
 * SHADER: A — one invocation per 8x8 DC-only block. */
void phx_add8x8_idct_dc(uint8_t *dst, int stride, const phx_dctcoef dct[4])
{
    phx_add4x4_idct_dc(dst, stride, dct[0]);
    phx_add4x4_idct_dc(dst + 4, stride, dct[1]);
    phx_add4x4_idct_dc(dst + 4 * stride, stride, dct[2]);
    phx_add4x4_idct_dc(dst + 4 * stride + 4, stride, dct[3]);
}

/* dct.c:462-471 — add16x16_idct_dc().
 * SHADER: A — one invocation per 16x16 DC-only block. */
void phx_add16x16_idct_dc(uint8_t *dst, int stride, const phx_dctcoef dct[16])
{
    for (int i = 0; i < 4; i++, dct += 4, dst += 4 * stride) {
        phx_add4x4_idct_dc(dst + 0, stride, dct[0]);
        phx_add4x4_idct_dc(dst + 4, stride, dct[1]);
        phx_add4x4_idct_dc(dst + 8, stride, dct[2]);
        phx_add4x4_idct_dc(dst + 12, stride, dct[3]);
    }
}
