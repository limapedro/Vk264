/* Function-for-function port of x264's scalar intra predictors.
 * Every function below cites the exact line of reference/x264/common/predict.c
 * it follows; formulas, shifts, rounding and clipping are copied verbatim
 * (SIMD dispatch is intentionally omitted — the C path is the specification).
 * SHADER: A for all predictors: one invocation per block, no hidden state.
 */
#include "phx_predict.h"

/* common.h:145-148, 8-bit variant.
 * SHADER: A — per-sample clamp, one lane per pixel. */
static uint8_t phx_clip_pixel(int x)
{
    return (uint8_t)((x & ~PHX_PRED_PIXEL_MAX) ? ((-x) >> 31) & PHX_PRED_PIXEL_MAX : x);
}

/* predict.c:531-532. */
#define F1(a, b)    (((a) + (b) + 1) >> 1)
#define F2(a, b, c) (((a) + 2 * (b) + (c) + 2) >> 2)

#define SRC(x, y) dst[(x) + (y) * stride]

/* Store the four bytes produced by pack_pixel_2to4(pairA, pairB), where each
 * pair is (low byte, high byte); equivalently p = lo | (hi << 8).
 * Source: predict.c:416-417,827-836 (macroblock.h pack_pixel_*).
 * SHADER: A — four adjacent lanes written by one block invocation. */
static void phx_store4(uint8_t *dst, const int pa[2], const int pb[2])
{
    dst[0] = (uint8_t)pa[0];
    dst[1] = (uint8_t)pa[1];
    dst[2] = (uint8_t)pb[0];
    dst[3] = (uint8_t)pb[1];
}

/****************************************************************************
 * 16x16 luma prediction
 ****************************************************************************/

/* predict.c:67-79 — x264_predict_16x16_dc_c.
 * SHADER: A — one invocation per 16x16 block; average of 32 neighbours. */
void phx_predict_16x16_dc(uint8_t *dst, int stride)
{
    int dc = 0;
    for (int i = 0; i < 16; i++) {
        dc += dst[-1 + i * stride];
        dc += dst[i - stride];
    }
    const uint8_t v = (uint8_t)((dc + 16) >> 5);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            SRC(x, y) = v;
}

/* predict.c:80-89 — predict_16x16_dc_left_c.
 * SHADER: A — one invocation per 16x16 block; average of 16 left neighbours. */
void phx_predict_16x16_dc_left(uint8_t *dst, int stride)
{
    int dc = 0;
    for (int i = 0; i < 16; i++)
        dc += dst[-1 + i * stride];
    const uint8_t v = (uint8_t)((dc + 8) >> 4);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            SRC(x, y) = v;
}

/* predict.c:90-99 — predict_16x16_dc_top_c.
 * SHADER: A — one invocation per 16x16 block; average of 16 top neighbours. */
void phx_predict_16x16_dc_top(uint8_t *dst, int stride)
{
    int dc = 0;
    for (int i = 0; i < 16; i++)
        dc += dst[i - stride];
    const uint8_t v = (uint8_t)((dc + 8) >> 4);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            SRC(x, y) = v;
}

/* predict.c:100-103 — predict_16x16_dc_128_c (1 << (BIT_DEPTH-1) = 128).
 * SHADER: A — constant fill, one invocation per block. */
void phx_predict_16x16_dc_128(uint8_t *dst, int stride)
{
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            SRC(x, y) = 128;
}

/* predict.c:104-115 — x264_predict_16x16_h_c.
 * SHADER: A — one invocation per block; left column broadcast. */
void phx_predict_16x16_h(uint8_t *dst, int stride)
{
    for (int y = 0; y < 16; y++) {
        const uint8_t v = dst[-1 + y * stride];
        for (int x = 0; x < 16; x++)
            SRC(x, y) = v;
    }
}

/* predict.c:116-131 — x264_predict_16x16_v_c.
 * SHADER: A — one invocation per block; top row copied to each row. */
void phx_predict_16x16_v(uint8_t *dst, int stride)
{
    uint8_t top[16];
    for (int x = 0; x < 16; x++)
        top[x] = dst[x - stride];
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            SRC(x, y) = top[x];
}

/* predict.c:132-160 — x264_predict_16x16_p_c.
 * SHADER: A — one invocation per block; planar interpolation from edges. */
void phx_predict_16x16_p(uint8_t *dst, int stride)
{
    int H = 0, V = 0;
    for (int i = 0; i <= 7; i++) {
        H += (i + 1) * (dst[8 + i - stride] - dst[6 - i - stride]);
        V += (i + 1) * (dst[-1 + (8 + i) * stride] - dst[-1 + (6 - i) * stride]);
    }
    const int a = 16 * (dst[-1 + 15 * stride] + dst[15 - stride]);
    const int b = (5 * H + 32) >> 6;
    const int c = (5 * V + 32) >> 6;
    int i00 = a - b * 7 - c * 7 + 16;

    for (int y = 0; y < 16; y++) {
        int pix = i00;
        for (int x = 0; x < 16; x++) {
            SRC(x, y) = phx_clip_pixel(pix >> 5);
            pix += b;
        }
        i00 += c;
    }
}

/****************************************************************************
 * 8x8 chroma prediction (4:2:0)
 ****************************************************************************/

/* predict.c:167-175 — predict_8x8c_dc_128_c.
 * SHADER: A — constant fill, one invocation per 8x8 chroma block. */
void phx_predict_8x8c_dc_128(uint8_t *dst, int stride)
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            SRC(x, y) = 128;
}

/* predict.c:176-201 — predict_8x8c_dc_left_c.
 * SHADER: A — one invocation per 8x8 chroma block. */
void phx_predict_8x8c_dc_left(uint8_t *dst, int stride)
{
    int dc0 = 0, dc1 = 0;
    for (int y = 0; y < 4; y++) {
        dc0 += dst[y * stride - 1];
        dc1 += dst[(y + 4) * stride - 1];
    }
    const uint8_t v0 = (uint8_t)((dc0 + 2) >> 2);
    const uint8_t v1 = (uint8_t)((dc1 + 2) >> 2);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++)
            SRC(x, y) = v0;
    for (int y = 4; y < 8; y++)
        for (int x = 0; x < 8; x++)
            SRC(x, y) = v1;
}

/* predict.c:202-220 — predict_8x8c_dc_top_c.
 * SHADER: A — one invocation per 8x8 chroma block. */
void phx_predict_8x8c_dc_top(uint8_t *dst, int stride)
{
    int dc0 = 0, dc1 = 0;
    for (int x = 0; x < 4; x++) {
        dc0 += dst[x - stride];
        dc1 += dst[x + 4 - stride];
    }
    const uint8_t v0 = (uint8_t)((dc0 + 2) >> 2);
    const uint8_t v1 = (uint8_t)((dc1 + 2) >> 2);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 4; x++) SRC(x, y) = v0;
        for (int x = 4; x < 8; x++) SRC(x, y) = v1;
    }
}

/* predict.c:221-259 — x264_predict_8x8c_dc_c.
 * SHADER: A — one invocation per 8x8 chroma block. */
void phx_predict_8x8c_dc(uint8_t *dst, int stride)
{
    int s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (int i = 0; i < 4; i++) {
        s0 += dst[i - stride];
        s1 += dst[i + 4 - stride];
        s2 += dst[-1 + i * stride];
        s3 += dst[-1 + (i + 4) * stride];
    }
    const uint8_t dc0 = (uint8_t)((s0 + s2 + 4) >> 3);
    const uint8_t dc1 = (uint8_t)((s1 + 2) >> 2);
    const uint8_t dc2 = (uint8_t)((s3 + 2) >> 2);
    const uint8_t dc3 = (uint8_t)((s1 + s3 + 4) >> 3);

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) SRC(x, y) = dc0;
        for (int x = 4; x < 8; x++) SRC(x, y) = dc1;
    }
    for (int y = 4; y < 8; y++) {
        for (int x = 0; x < 4; x++) SRC(x, y) = dc2;
        for (int x = 4; x < 8; x++) SRC(x, y) = dc3;
    }
}

/* predict.c:260-269 — x264_predict_8x8c_h_c.
 * SHADER: A — one invocation per 8x8 chroma block. */
void phx_predict_8x8c_h(uint8_t *dst, int stride)
{
    for (int y = 0; y < 8; y++) {
        const uint8_t v = dst[-1 + y * stride];
        for (int x = 0; x < 8; x++) SRC(x, y) = v;
    }
}

/* predict.c:270-281 — x264_predict_8x8c_v_c.
 * SHADER: A — one invocation per 8x8 chroma block. */
void phx_predict_8x8c_v(uint8_t *dst, int stride)
{
    uint8_t top[8];
    for (int x = 0; x < 8; x++) top[x] = dst[x - stride];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) SRC(x, y) = top[x];
}

/* predict.c:282-308 — x264_predict_8x8c_p_c.
 * SHADER: A — planar interpolation, one invocation per chroma block. */
void phx_predict_8x8c_p(uint8_t *dst, int stride)
{
    int H = 0, V = 0;
    for (int i = 0; i < 4; i++) {
        H += (i + 1) * (dst[4 + i - stride] - dst[2 - i - stride]);
        V += (i + 1) * (dst[-1 + (i + 4) * stride] - dst[-1 + (2 - i) * stride]);
    }
    const int a = 16 * (dst[-1 + 7 * stride] + dst[7 - stride]);
    const int b = (17 * H + 16) >> 5;
    const int c = (17 * V + 16) >> 5;
    int i00 = a - 3 * b - 3 * c + 16;

    for (int y = 0; y < 8; y++) {
        int pix = i00;
        for (int x = 0; x < 8; x++) {
            SRC(x, y) = phx_clip_pixel(pix >> 5);
            pix += b;
        }
        i00 += c;
    }
}

/****************************************************************************
 * 8x16 chroma prediction (4:2:2)
 ****************************************************************************/

/* predict.c:314-322 — predict_8x16c_dc_128_c.
 * SHADER: A — constant fill, one invocation per 8x16 chroma block. */
void phx_predict_8x16c_dc_128(uint8_t *dst, int stride)
{
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++) SRC(x, y) = 128;
}

/* predict.c:323-341 — predict_8x16c_dc_left_c (four 4-row strips).
 * SHADER: A — one invocation per 8x16 chroma block. */
void phx_predict_8x16c_dc_left(uint8_t *dst, int stride)
{
    for (int i = 0; i < 4; i++) {
        int dc = 0;
        for (int y = 0; y < 4; y++)
            dc += dst[y * stride - 1];
        const uint8_t v = (uint8_t)((dc + 2) >> 2);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 8; x++) SRC(x, y) = v;
        dst += 4 * stride;
    }
}

/* predict.c:342-360 — predict_8x16c_dc_top_c.
 * SHADER: A — one invocation per 8x16 chroma block. */
void phx_predict_8x16c_dc_top(uint8_t *dst, int stride)
{
    int dc0 = 0, dc1 = 0;
    for (int x = 0; x < 4; x++) {
        dc0 += dst[x - stride];
        dc1 += dst[x + 4 - stride];
    }
    const uint8_t v0 = (uint8_t)((dc0 + 2) >> 2);
    const uint8_t v1 = (uint8_t)((dc1 + 2) >> 2);
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 4; x++) SRC(x, y) = v0;
        for (int x = 4; x < 8; x++) SRC(x, y) = v1;
    }
}

/* predict.c:361-420 — x264_predict_8x16c_dc_c (four 4-row strips).
 * SHADER: A — one invocation per 8x16 chroma block. */
void phx_predict_8x16c_dc(uint8_t *dst, int stride)
{
    int s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0;
    for (int i = 0; i < 4; i++) {
        s0 += dst[i - stride];
        s1 += dst[i + 4 - stride];
        s2 += dst[-1 + (i + 0) * stride];
        s3 += dst[-1 + (i + 4) * stride];
        s4 += dst[-1 + (i + 8) * stride];
        s5 += dst[-1 + (i + 12) * stride];
    }
    const uint8_t dc0 = (uint8_t)((s0 + s2 + 4) >> 3);
    const uint8_t dc1 = (uint8_t)((s1 + 2) >> 2);
    const uint8_t dc2 = (uint8_t)((s3 + 2) >> 2);
    const uint8_t dc3 = (uint8_t)((s1 + s3 + 4) >> 3);
    const uint8_t dc4 = (uint8_t)((s4 + 2) >> 2);
    const uint8_t dc5 = (uint8_t)((s1 + s4 + 4) >> 3);
    const uint8_t dc6 = (uint8_t)((s5 + 2) >> 2);
    const uint8_t dc7 = (uint8_t)((s1 + s5 + 4) >> 3);

    const uint8_t dc[8] = {dc0, dc1, dc2, dc3, dc4, dc5, dc6, dc7};
    for (int strip = 0; strip < 4; strip++) {
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 8; x++)
                SRC(x, y) = dc[2 * strip + (x >> 2)];
        dst += 4 * stride;
    }
}

/* predict.c:421-430 — x264_predict_8x16c_h_c.
 * SHADER: A — one invocation per 8x16 chroma block. */
void phx_predict_8x16c_h(uint8_t *dst, int stride)
{
    for (int y = 0; y < 16; y++) {
        const uint8_t v = dst[-1 + y * stride];
        for (int x = 0; x < 8; x++) SRC(x, y) = v;
    }
}

/* predict.c:431-442 — x264_predict_8x16c_v_c.
 * SHADER: A — one invocation per 8x16 chroma block. */
void phx_predict_8x16c_v(uint8_t *dst, int stride)
{
    uint8_t top[8];
    for (int x = 0; x < 8; x++) top[x] = dst[x - stride];
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++) SRC(x, y) = top[x];
}

/* predict.c:443-469 — x264_predict_8x16c_p_c.
 * SHADER: A — planar interpolation, one invocation per chroma block. */
void phx_predict_8x16c_p(uint8_t *dst, int stride)
{
    int H = 0, V = 0;
    for (int i = 0; i < 4; i++)
        H += (i + 1) * (dst[4 + i - stride] - dst[2 - i - stride]);
    for (int i = 0; i < 8; i++)
        V += (i + 1) * (dst[-1 + (i + 8) * stride] - dst[-1 + (6 - i) * stride]);

    const int a = 16 * (dst[-1 + 15 * stride] + dst[7 - stride]);
    const int b = (17 * H + 16) >> 5;
    const int c = (5 * V + 32) >> 6;
    int i00 = a - 3 * b - 7 * c + 16;

    for (int y = 0; y < 16; y++) {
        int pix = i00;
        for (int x = 0; x < 8; x++) {
            SRC(x, y) = phx_clip_pixel(pix >> 5);
            pix += b;
        }
        i00 += c;
    }
}

/****************************************************************************
 * 4x4 luma prediction
 ****************************************************************************/

/* predict.c:481-484 — predict_4x4_dc_128_c.
 * SHADER: A — constant fill, one invocation per 4x4 block. */
void phx_predict_4x4_dc_128(uint8_t *dst, int stride)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) SRC(x, y) = 128;
}

/* predict.c:485-489 — predict_4x4_dc_left_c.
 * SHADER: A — one invocation per 4x4 block. */
void phx_predict_4x4_dc_left(uint8_t *dst, int stride)
{
    const uint8_t v = (uint8_t)((SRC(-1, 0) + SRC(-1, 1) + SRC(-1, 2) + SRC(-1, 3) + 2) >> 2);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) SRC(x, y) = v;
}

/* predict.c:490-494 — predict_4x4_dc_top_c.
 * SHADER: A — one invocation per 4x4 block. */
void phx_predict_4x4_dc_top(uint8_t *dst, int stride)
{
    const uint8_t v = (uint8_t)((SRC(0, -1) + SRC(1, -1) + SRC(2, -1) + SRC(3, -1) + 2) >> 2);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) SRC(x, y) = v;
}

/* predict.c:495-500 — x264_predict_4x4_dc_c.
 * SHADER: A — one invocation per 4x4 block. */
void phx_predict_4x4_dc(uint8_t *dst, int stride)
{
    const uint8_t v = (uint8_t)((SRC(-1, 0) + SRC(-1, 1) + SRC(-1, 2) + SRC(-1, 3) +
                                 SRC(0, -1) + SRC(1, -1) + SRC(2, -1) + SRC(3, -1) + 4) >> 3);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) SRC(x, y) = v;
}

/* predict.c:501-507 — x264_predict_4x4_h_c.
 * SHADER: A — one invocation per 4x4 block. */
void phx_predict_4x4_h(uint8_t *dst, int stride)
{
    for (int y = 0; y < 4; y++) {
        const uint8_t v = SRC(-1, y);
        for (int x = 0; x < 4; x++) SRC(x, y) = v;
    }
}

/* predict.c:508-511 — x264_predict_4x4_v_c.
 * SHADER: A — one invocation per 4x4 block. */
void phx_predict_4x4_v(uint8_t *dst, int stride)
{
    /* x264 uses PREDICT_4x4_DC(SRC_X4(0,-1)) (predict.c:508-511): the four
     * top-row samples are copied to every row, not the single top-left
     * sample broadcast. */
    const uint8_t *top = dst - stride;                 /* SRC_X4(0,-1) */
    for (int y = 0; y < 4; y++)
        memcpy(dst + y*stride, top, 4);                 /* PREDICT_4x4_DC, :478-479 */
}

/* predict.c:534-545 — predict_4x4_ddl_c (top-left down-left diagonal).
 * SHADER: A — one invocation per 4x4 block. */
void phx_predict_4x4_ddl(uint8_t *dst, int stride)
{
    const int t0 = SRC(0, -1), t1 = SRC(1, -1), t2 = SRC(2, -1), t3 = SRC(3, -1);
    const int t4 = SRC(4, -1), t5 = SRC(5, -1), t6 = SRC(6, -1), t7 = SRC(7, -1);
    SRC(0, 0) = F2(t0, t1, t2);
    SRC(1, 0) = SRC(0, 1) = F2(t1, t2, t3);
    SRC(2, 0) = SRC(1, 1) = SRC(0, 2) = F2(t2, t3, t4);
    SRC(3, 0) = SRC(2, 1) = SRC(1, 2) = SRC(0, 3) = F2(t3, t4, t5);
    SRC(3, 1) = SRC(2, 2) = SRC(1, 3) = F2(t4, t5, t6);
    SRC(3, 2) = SRC(2, 3) = F2(t5, t6, t7);
    SRC(3, 3) = F2(t6, t7, t7);
}

/* predict.c:546-558 — predict_4x4_ddr_c.
 * SHADER: A — one invocation per 4x4 block. */
void phx_predict_4x4_ddr(uint8_t *dst, int stride)
{
    const int lt = SRC(-1, -1);
    const int l0 = SRC(-1, 0), l1 = SRC(-1, 1), l2 = SRC(-1, 2), l3 = SRC(-1, 3);
    const int t0 = SRC(0, -1), t1 = SRC(1, -1), t2 = SRC(2, -1), t3 = SRC(3, -1);
    SRC(3, 0) = F2(t3, t2, t1);
    SRC(2, 0) = SRC(3, 1) = F2(t2, t1, t0);
    SRC(1, 0) = SRC(2, 1) = SRC(3, 2) = F2(t1, t0, lt);
    SRC(0, 0) = SRC(1, 1) = SRC(2, 2) = SRC(3, 3) = F2(t0, lt, l0);
    SRC(0, 1) = SRC(1, 2) = SRC(2, 3) = F2(lt, l0, l1);
    SRC(0, 2) = SRC(1, 3) = F2(l0, l1, l2);
    SRC(0, 3) = F2(l1, l2, l3);
}

/* predict.c:560-575 — predict_4x4_vr_c.
 * SHADER: A — one invocation per 4x4 block. */
void phx_predict_4x4_vr(uint8_t *dst, int stride)
{
    const int lt = SRC(-1, -1);
    const int l0 = SRC(-1, 0), l1 = SRC(-1, 1), l2 = SRC(-1, 2);
    const int t0 = SRC(0, -1), t1 = SRC(1, -1), t2 = SRC(2, -1), t3 = SRC(3, -1);
    SRC(0, 3) = F2(l2, l1, l0);
    SRC(0, 2) = F2(l1, l0, lt);
    SRC(0, 1) = SRC(1, 3) = F2(l0, lt, t0);
    SRC(0, 0) = SRC(1, 2) = F1(lt, t0);
    SRC(1, 1) = SRC(2, 3) = F2(lt, t0, t1);
    SRC(1, 0) = SRC(2, 2) = F1(t0, t1);
    SRC(2, 1) = SRC(3, 3) = F2(t0, t1, t2);
    SRC(2, 0) = SRC(3, 2) = F1(t1, t2);
    SRC(3, 1) = F2(t1, t2, t3);
    SRC(3, 0) = F1(t2, t3);
}

/* predict.c:577-592 — predict_4x4_hd_c.
 * SHADER: A — one invocation per 4x4 block. */
void phx_predict_4x4_hd(uint8_t *dst, int stride)
{
    const int lt = SRC(-1, -1);
    const int l0 = SRC(-1, 0), l1 = SRC(-1, 1), l2 = SRC(-1, 2), l3 = SRC(-1, 3);
    const int t0 = SRC(0, -1), t1 = SRC(1, -1), t2 = SRC(2, -1);
    SRC(0, 3) = F1(l2, l3);
    SRC(1, 3) = F2(l1, l2, l3);
    SRC(0, 2) = SRC(2, 3) = F1(l1, l2);
    SRC(1, 2) = SRC(3, 3) = F2(l0, l1, l2);
    SRC(0, 1) = SRC(2, 2) = F1(l0, l1);
    SRC(1, 1) = SRC(3, 2) = F2(lt, l0, l1);
    SRC(0, 0) = SRC(2, 1) = F1(lt, l0);
    SRC(1, 0) = SRC(3, 1) = F2(t0, lt, l0);
    SRC(2, 0) = F2(t1, t0, lt);
    SRC(3, 0) = F2(t2, t1, t0);
}

/* predict.c:594-608 — predict_4x4_vl_c.
 * SHADER: A — one invocation per 4x4 block. */
void phx_predict_4x4_vl(uint8_t *dst, int stride)
{
    const int t0 = SRC(0, -1), t1 = SRC(1, -1), t2 = SRC(2, -1), t3 = SRC(3, -1);
    const int t4 = SRC(4, -1), t5 = SRC(5, -1), t6 = SRC(6, -1);
    SRC(0, 0) = F1(t0, t1);
    SRC(0, 1) = F2(t0, t1, t2);
    SRC(1, 0) = SRC(0, 2) = F1(t1, t2);
    SRC(1, 1) = SRC(0, 3) = F2(t1, t2, t3);
    SRC(2, 0) = SRC(1, 2) = F1(t2, t3);
    SRC(2, 1) = SRC(1, 3) = F2(t2, t3, t4);
    SRC(3, 0) = SRC(2, 2) = F1(t3, t4);
    SRC(3, 1) = SRC(2, 3) = F2(t3, t4, t5);
    SRC(3, 2) = F1(t4, t5);
    SRC(3, 3) = F2(t4, t5, t6);
}

/* predict.c:610-621 — predict_4x4_hu_c.
 * SHADER: A — one invocation per 4x4 block. */
void phx_predict_4x4_hu(uint8_t *dst, int stride)
{
    const int l0 = SRC(-1, 0), l1 = SRC(-1, 1), l2 = SRC(-1, 2), l3 = SRC(-1, 3);
    SRC(0, 0) = F1(l0, l1);
    SRC(1, 0) = F2(l0, l1, l2);
    SRC(2, 0) = SRC(0, 1) = F1(l1, l2);
    SRC(3, 0) = SRC(1, 1) = F2(l1, l2, l3);
    SRC(2, 1) = SRC(0, 2) = F1(l2, l3);
    SRC(3, 1) = SRC(1, 2) = F2(l2, l3, l3);
    SRC(3, 2) = SRC(1, 3) = SRC(0, 3) = l3;
    SRC(2, 2) = SRC(2, 3) = SRC(3, 3) = l3;
}

/****************************************************************************
 * 8x8 luma prediction with optional filtered edges
 ****************************************************************************/

/* predict.c:627-675 — predict_8x8_filter_c. Entries not selected by
 * i_filters are left untouched (the caller supplies the raw neighbour edge).
 * PL(y): edge[14-y] = F2(SRC(-1,y-1), SRC(-1,y), SRC(-1,y+1))  (:628)
 * PT(x): edge[16+x] = F2(SRC(x-1,-1), SRC(x,-1), SRC(x+1,-1)) (:630)
 * SHADER: A — one invocation per 8x8 block, edge array local to invocation. */
void phx_predict_8x8_filter(uint8_t *dst, uint8_t edge[36], int stride,
                            int i_neighbor, int i_filters)
{
    const int have_lt = (i_neighbor & PHX_MB_TOPLEFT) != 0;

    if (i_filters & PHX_MB_LEFT) {
        edge[15] = (uint8_t)((SRC(0, -1) + 2 * SRC(-1, -1) + SRC(-1, 0) + 2) >> 2);
        edge[14] = (uint8_t)(((have_lt ? SRC(-1, -1) : SRC(-1, 0))
                            + 2 * SRC(-1, 0) + SRC(-1, 1) + 2) >> 2);
        for (int y = 1; y <= 6; y++)
            edge[14 - y] = (uint8_t)F2(SRC(-1, y - 1), SRC(-1, y), SRC(-1, y + 1));
        edge[6] = edge[7] = (uint8_t)((SRC(-1, 6) + 3 * SRC(-1, 7) + 2) >> 2);
    }

    if (i_filters & PHX_MB_TOP) {
        const int have_tr = (i_neighbor & PHX_MB_TOPRIGHT) != 0;
        edge[16] = (uint8_t)(((have_lt ? SRC(-1, -1) : SRC(0, -1))
                            + 2 * SRC(0, -1) + SRC(1, -1) + 2) >> 2);
        for (int x = 1; x <= 6; x++)
            edge[16 + x] = (uint8_t)F2(SRC(x - 1, -1), SRC(x, -1), SRC(x + 1, -1));
        edge[23] = (uint8_t)((SRC(6, -1) + 2 * SRC(7, -1)
                            + (have_tr ? SRC(8, -1) : SRC(7, -1)) + 2) >> 2);

        if (i_filters & PHX_MB_TOPRIGHT) {
            if (have_tr) {
                for (int x = 8; x <= 14; x++)
                    edge[16 + x] = (uint8_t)F2(SRC(x - 1, -1), SRC(x, -1), SRC(x + 1, -1));
                edge[31] = edge[32] = (uint8_t)((SRC(14, -1) + 3 * SRC(15, -1) + 2) >> 2);
            } else {
                for (int i = 24; i < 32; i++) edge[i] = SRC(7, -1);
                edge[32] = SRC(7, -1);
            }
        }
    }
}

/* predict.c:693-698 — PREDICT_8x8_DC helper.
 * SHADER: A — constant fill inside one block invocation. */
static void phx_predict_8x8_fill(uint8_t *dst, int stride, uint8_t v)
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) SRC(x, y) = v;
}

/* predict.c:700-703 — predict_8x8_dc_128_c.
 * SHADER: A — constant fill, one invocation per 8x8 block. */
void phx_predict_8x8_dc_128(uint8_t *dst, const uint8_t edge[36], int stride)
{
    (void)edge;
    phx_predict_8x8_fill(dst, stride, 128);
}

/* predict.c:704-709 — predict_8x8_dc_left_c (edge[7..14] = l7..l0).
 * SHADER: A — one invocation per 8x8 block. */
void phx_predict_8x8_dc_left(uint8_t *dst, const uint8_t edge[36], int stride)
{
    int dc = 0;                     /* PL(0..7): edge[14-y] = l7..l0 */
    for (int i = 7; i <= 14; i++) dc += edge[i];
    phx_predict_8x8_fill(dst, stride, (uint8_t)((dc + 4) >> 3));
}

/* predict.c:710-715 — predict_8x8_dc_top_c (edge[16..23] = t0..t7).
 * SHADER: A — one invocation per 8x8 block. */
void phx_predict_8x8_dc_top(uint8_t *dst, const uint8_t edge[36], int stride)
{
    int dc = 0;
    for (int i = 16; i < 24; i++) dc += edge[i];
    phx_predict_8x8_fill(dst, stride, (uint8_t)((dc + 4) >> 3));
}

/* predict.c:716-722 — x264_predict_8x8_dc_c.
 * SHADER: A — one invocation per 8x8 block. */
void phx_predict_8x8_dc(uint8_t *dst, const uint8_t edge[36], int stride)
{
    int dc = 0;
    for (int i = 7; i <= 14; i++) dc += edge[i];
    for (int i = 16; i < 24; i++) dc += edge[i];
    phx_predict_8x8_fill(dst, stride, (uint8_t)((dc + 8) >> 4));
}

/* predict.c:723-730 — x264_predict_8x8_h_c.
 * SHADER: A — one invocation per 8x8 block. */
void phx_predict_8x8_h(uint8_t *dst, const uint8_t edge[36], int stride)
{
    for (int y = 0; y < 8; y++) {
        const uint8_t v = edge[14 - y];
        for (int x = 0; x < 8; x++) SRC(x, y) = v;
    }
}

/* predict.c:731-740 — x264_predict_8x8_v_c.
 * SHADER: A — one invocation per 8x8 block. */
void phx_predict_8x8_v(uint8_t *dst, const uint8_t edge[36], int stride)
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) SRC(x, y) = edge[16 + x];
}

/* predict.c:741-760 — predict_8x8_ddl_c. Index k = x+y, t[k] clamped to t15
 * at the far corner (predict.c:759 uses F2(t14,t15,t15)).
 * SHADER: A — one invocation per 8x8 block. */
void phx_predict_8x8_ddl(uint8_t *dst, const uint8_t edge[36], int stride)
{
    const uint8_t *t = edge + 16;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int k = x + y;
            if (k > 15) k = 15;
            SRC(x, y) = F2(t[k], t[k + 1 < 16 ? k + 1 : 15], t[k + 2 < 16 ? k + 2 : 15]);
        }
    }
}

/* predict.c:761-782 — predict_8x8_ddr_c.
 * SHADER: A — one invocation per 8x8 block. */
void phx_predict_8x8_ddr(uint8_t *dst, const uint8_t edge[36], int stride)
{
    const int lt = edge[15];
    const int l[8] = {edge[14], edge[13], edge[12], edge[11],
                      edge[10], edge[9],  edge[8],  edge[7]};
    const int t[8] = {edge[16], edge[17], edge[18], edge[19],
                      edge[20], edge[21], edge[22], edge[23]};
    /* Diagonal sequence q = [l7,l6,...,l0,lt,t0,t1,...,t7]; F2 is symmetric
     * in its outer arguments, so q[k],q[k-1],q[k-2] reproduces every explicit
     * assignment at predict.c:766-780. (0,7) -> q[2]=l7, (7,0) -> q[16]=t7. */
    int q[17];
    for (int i = 0; i < 8; i++) q[i] = l[7 - i];
    q[8] = lt;
    for (int i = 0; i < 8; i++) q[9 + i] = t[i];
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int k = 9 + x - y;
            SRC(x, y) = F2(q[k], q[k - 1], q[k - 2]);
        }
    }
}

/* predict.c:783-810 — predict_8x8_vr_c.
 * SHADER: A — one invocation per 8x8 block. */
void phx_predict_8x8_vr(uint8_t *dst, const uint8_t edge[36], int stride)
{
    const int lt = edge[15];
    const int l[7] = {edge[14], edge[13], edge[12], edge[11], edge[10], edge[9], edge[8]};
    const int t[8] = {edge[16], edge[17], edge[18], edge[19],
                      edge[20], edge[21], edge[22], edge[23]};
    SRC(0, 6) = F2(l[5], l[4], l[3]);
    SRC(0, 7) = F2(l[6], l[5], l[4]);
    SRC(0, 4) = SRC(1, 6) = F2(l[3], l[2], l[1]);
    SRC(0, 5) = SRC(1, 7) = F2(l[4], l[3], l[2]);
    SRC(0, 2) = SRC(1, 4) = SRC(2, 6) = F2(l[1], l[0], lt);
    SRC(0, 3) = SRC(1, 5) = SRC(2, 7) = F2(l[2], l[1], l[0]);
    SRC(0, 1) = SRC(1, 3) = SRC(2, 5) = SRC(3, 7) = F2(l[0], lt, t[0]);
    SRC(0, 0) = SRC(1, 2) = SRC(2, 4) = SRC(3, 6) = F1(lt, t[0]);
    SRC(1, 1) = SRC(2, 3) = SRC(3, 5) = SRC(4, 7) = F2(lt, t[0], t[1]);
    SRC(1, 0) = SRC(2, 2) = SRC(3, 4) = SRC(4, 6) = F1(t[0], t[1]);
    SRC(2, 1) = SRC(3, 3) = SRC(4, 5) = SRC(5, 7) = F2(t[0], t[1], t[2]);
    SRC(2, 0) = SRC(3, 2) = SRC(4, 4) = SRC(5, 6) = F1(t[1], t[2]);
    SRC(3, 1) = SRC(4, 3) = SRC(5, 5) = SRC(6, 7) = F2(t[1], t[2], t[3]);
    SRC(3, 0) = SRC(4, 2) = SRC(5, 4) = SRC(6, 6) = F1(t[2], t[3]);
    SRC(4, 1) = SRC(5, 3) = SRC(6, 5) = SRC(7, 7) = F2(t[2], t[3], t[4]);
    SRC(4, 0) = SRC(5, 2) = SRC(6, 4) = SRC(7, 6) = F1(t[3], t[4]);
    SRC(5, 1) = SRC(6, 3) = SRC(7, 5) = F2(t[3], t[4], t[5]);
    SRC(5, 0) = SRC(6, 2) = SRC(7, 4) = F1(t[4], t[5]);
    SRC(6, 1) = SRC(7, 3) = F2(t[4], t[5], t[6]);
    SRC(6, 0) = SRC(7, 2) = F1(t[5], t[6]);
    SRC(7, 1) = F2(t[5], t[6], t[7]);
    SRC(7, 0) = F1(t[6], t[7]);
}

/* predict.c:811-837 — predict_8x8_hd_c. Each pack_pixel_1to2(a,b) yields a
 * 16-bit value a|(b<<8); pack_pixel_2to4 of two such values yields four bytes.
 * SHADER: A — one invocation per 8x8 block. */
void phx_predict_8x8_hd(uint8_t *dst, const uint8_t edge[36], int stride)
{
    const int lt = edge[15];
    const int l[8] = {edge[14], edge[13], edge[12], edge[11], edge[10], edge[9], edge[8], edge[7]};
    const int t[7] = {edge[16], edge[17], edge[18], edge[19], edge[20], edge[21], edge[22]};
    int p1[2] = {F1(l[6], l[7]), F2(l[5], l[6], l[7])};
    int p2[2] = {F1(l[5], l[6]), F2(l[4], l[5], l[6])};
    int p3[2] = {F1(l[4], l[5]), F2(l[3], l[4], l[5])};
    int p4[2] = {F1(l[3], l[4]), F2(l[2], l[3], l[4])};
    int p5[2] = {F1(l[2], l[3]), F2(l[1], l[2], l[3])};
    int p6[2] = {F1(l[1], l[2]), F2(l[0], l[1], l[2])};
    int p7[2] = {F1(l[0], l[1]), F2(lt, l[0], l[1])};
    int p8[2] = {F1(lt, l[0]), F2(l[0], lt, t[0])};
    int p9[2] = {F2(t[1], t[0], lt), F2(t[2], t[1], t[0])};
    int p10[2] = {F2(t[3], t[2], t[1]), F2(t[4], t[3], t[2])};
    int p11[2] = {F2(t[5], t[4], t[3]), F2(t[6], t[5], t[4])};
    const int *pp[11] = {p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11};

    /* predict.c:827-836: row y stores packs (p[9-y],p[10-y]) at x=0 and
     * (p[11-y],p[12-y]) at x=4, clamped to p11 at the extreme rows. */
    for (int y = 0; y < 8; y++) {
        int a = 8 - y, b = 9 - y, c = 10 - y, d = 11 - y;
        if (a > 11) a = 11; if (b > 11) b = 11;
        if (c > 11) c = 11; if (d > 11) d = 11;
        phx_store4(&SRC(0, y), pp[a - 1], pp[b - 1]);
        phx_store4(&SRC(4, y), pp[c - 1], pp[d - 1]);
    }
}

/* predict.c:838-864 — predict_8x8_vl_c: k = x + (y>>1), F1 for even rows,
 * F2 for odd rows (predict.c:863: SRC(7,7)=F2(t10,t11,t12)).
 * SHADER: A — one invocation per 8x8 block. */
void phx_predict_8x8_vl(uint8_t *dst, const uint8_t edge[36], int stride)
{
    const uint8_t *t = edge + 16;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int k = x + (y >> 1);
            if (y & 1)
                SRC(x, y) = F2(t[k], t[k + 1], t[k + 2]);
            else
                SRC(x, y) = F1(t[k], t[k + 1]);
        }
    }
}

/* predict.c:865-884 — predict_8x8_hu_c.
 * SHADER: A — one invocation per 8x8 block. */
void phx_predict_8x8_hu(uint8_t *dst, const uint8_t edge[36], int stride)
{
    const int l[8] = {edge[14], edge[13], edge[12], edge[11], edge[10], edge[9], edge[8], edge[7]};
    int p1[2] = {F1(l[0], l[1]), F2(l[0], l[1], l[2])};
    int p2[2] = {F1(l[1], l[2]), F2(l[1], l[2], l[3])};
    int p3[2] = {F1(l[2], l[3]), F2(l[2], l[3], l[4])};
    int p4[2] = {F1(l[3], l[4]), F2(l[3], l[4], l[5])};
    int p5[2] = {F1(l[4], l[5]), F2(l[4], l[5], l[6])};
    int p6[2] = {F1(l[5], l[6]), F2(l[5], l[6], l[7])};
    int p7[2] = {F1(l[6], l[7]), F2(l[6], l[7], l[7])};
    int p8[2] = {l[7], l[7]};
    const int *pp[8] = {p1, p2, p3, p4, p5, p6, p7, p8};

    /* predict.c:876-883: row y uses packs (p[y],p[y+1]) at x=0 and
     * (p[y+2],p[y+3]) at x=4, clamped to p8. */
    for (int y = 0; y < 8; y++) {
        int a = y, b = y + 1, c = y + 2, d = y + 3;
        if (a > 7) a = 7; if (b > 7) b = 7;
        if (c > 7) c = 7; if (d > 7) d = 7;
        phx_store4(&SRC(0, y), pp[a], pp[b]);
        phx_store4(&SRC(4, y), pp[c], pp[d]);
    }
}

/****************************************************************************
 * Function-pointer tables (no CPU dispatch)
 ****************************************************************************/

/* predict.c:889-898 — x264_predict_16x16_init.
 * SHADER: B — table of per-block A functions; filled once before encoding. */
void phx_predict_16x16_init(phx_predict_t pf[7])
{
    pf[0] = phx_predict_16x16_v;
    pf[1] = phx_predict_16x16_h;
    pf[2] = phx_predict_16x16_dc;
    pf[3] = phx_predict_16x16_p;
    pf[4] = phx_predict_16x16_dc_left;
    pf[5] = phx_predict_16x16_dc_top;
    pf[6] = phx_predict_16x16_dc_128;
}

/* predict.c:936-945 — x264_predict_8x8c_init.
 * SHADER: B — table of per-block A functions; filled once before encoding. */
void phx_predict_8x8c_init(phx_predict_t pf[7])
{
    pf[0] = phx_predict_8x8c_dc;
    pf[1] = phx_predict_8x8c_h;
    pf[2] = phx_predict_8x8c_v;
    pf[3] = phx_predict_8x8c_p;
    pf[4] = phx_predict_8x8c_dc_left;
    pf[5] = phx_predict_8x8c_dc_top;
    pf[6] = phx_predict_8x8c_dc_128;
}

/* predict.c:977-986 — x264_predict_8x16c_init.
 * SHADER: B — table of per-block A functions; filled once before encoding. */
void phx_predict_8x16c_init(phx_predict_t pf[7])
{
    pf[0] = phx_predict_8x16c_dc;
    pf[1] = phx_predict_8x16c_h;
    pf[2] = phx_predict_8x16c_v;
    pf[3] = phx_predict_8x16c_p;
    pf[4] = phx_predict_8x16c_dc_left;
    pf[5] = phx_predict_8x16c_dc_top;
    pf[6] = phx_predict_8x16c_dc_128;
}

/* predict.c:1042-1055 — x264_predict_4x4_init.
 * SHADER: B — table of per-block A functions; filled once before encoding. */
void phx_predict_4x4_init(phx_predict_t pf[12])
{
    pf[0] = phx_predict_4x4_v;
    pf[1] = phx_predict_4x4_h;
    pf[2] = phx_predict_4x4_dc;
    pf[3] = phx_predict_4x4_ddl;
    pf[4] = phx_predict_4x4_ddr;
    pf[5] = phx_predict_4x4_vr;
    pf[6] = phx_predict_4x4_hd;
    pf[7] = phx_predict_4x4_vl;
    pf[8] = phx_predict_4x4_hu;
    pf[9] = phx_predict_4x4_dc_left;
    pf[10] = phx_predict_4x4_dc_top;
    pf[11] = phx_predict_4x4_dc_128;
}

/* predict.c:1000-1013 — x264_predict_8x8_init.
 * SHADER: B — tables of per-block A functions; filled once before encoding. */
void phx_predict_8x8_init(phx_predict8x8_t pf[12],
                          void (**filter)(uint8_t *, uint8_t *, int, int, int))
{
    pf[0] = phx_predict_8x8_dc;
    pf[1] = phx_predict_8x8_h;
    pf[2] = phx_predict_8x8_v;
    pf[3] = phx_predict_8x8_ddl;
    pf[4] = phx_predict_8x8_ddr;
    pf[5] = phx_predict_8x8_vr;
    pf[6] = phx_predict_8x8_hd;
    pf[7] = phx_predict_8x8_vl;
    pf[8] = phx_predict_8x8_hu;
    pf[9] = phx_predict_8x8_dc_left;
    pf[10] = phx_predict_8x8_dc_top;
    pf[11] = phx_predict_8x8_dc_128;
    if (filter) *filter = phx_predict_8x8_filter;
}
