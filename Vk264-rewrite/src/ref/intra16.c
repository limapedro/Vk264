/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "intra16.h"
#include <stdio.h>
#include <stdlib.h>

static const int lambda_tab[52] = {
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,2,2,2,3,3,3,4,4,4,5,6,6,7,8,9,10,11,
    13,14,16,18,20,23,25,29,32,36,40,45,51,57,64,71,80,90
};

int vk_lambda(int qp) { return lambda_tab[qp < 0 ? 0 : qp > 51 ? 51 : qp]; }

int vk_mode_bits(int mode)
{
    int fix = mode <= 3 ? mode : 2; /* x264_mb_pred_mode16x16_fix */
    int v = fix + 1, n = 0;
    while (v > 1) { v >>= 1; ++n; }
    return 2*n+1;
}

static int clip_pixel(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

void vk_predict16(uint8_t out[256], const uint8_t top[16],
                  const uint8_t left[16], uint8_t tl, int mode)
{
    if (mode == 0) { /* V */
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) out[y*16+x] = top[x];
    } else if (mode == 1) { /* H */
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) out[y*16+x] = left[y];
    } else if (mode == 4) { /* DC_LEFT */
        int s = 0; for (int i = 0; i < 16; ++i) s += left[i];
        uint8_t v = (uint8_t)((s+8)>>4);
        for (int i = 0; i < 256; ++i) out[i] = v;
    } else if (mode == 5) { /* DC_TOP */
        int s = 0; for (int i = 0; i < 16; ++i) s += top[i];
        uint8_t v = (uint8_t)((s+8)>>4);
        for (int i = 0; i < 256; ++i) out[i] = v;
    } else if (mode == 6) { /* DC_128 */
        for (int i = 0; i < 256; ++i) out[i] = 128;
    } else if (mode == 3) { /* P */
        int H = 0, V = 0;
        for (int i = 0; i <= 7; ++i) {
            /* x264 reads src[6-i-FDEC_STRIDE] / src[-1+(6-i)*FDEC_STRIDE];
             * at i=7 this is the top-left corner, not an in-array sample. */
            int th = (i == 7) ? tl : top[6-i];
            int tv = (i == 7) ? tl : left[6-i];
            H += (i+1) * (top[8+i] - th);
            V += (i+1) * (left[8+i] - tv);
        }
        int a = 16 * (left[15] + top[15]);
        int b = (5*H+32)>>6, c = (5*V+32)>>6;
        int i00 = a - b*7 - c*7 + 16;
        for (int y = 0; y < 16; ++y) {
            int pix = i00;
            for (int x = 0; x < 16; ++x) {
                out[y*16+x] = (uint8_t)clip_pixel(pix>>5);
                pix += b;
            }
            i00 += c;
        }
    } else { /* DC (2): x264 predict_16x16_dc_c: (sum(left+top)+16)>>5 */
        int s = 0;
        for (int i = 0; i < 16; ++i) s += left[i] + top[i];
        uint8_t v = (uint8_t)((s+16)>>5);
        for (int i = 0; i < 256; ++i) out[i] = v;
    }
}

void vk_predict_chroma8(uint8_t out[64], const uint8_t top[8],
                        const uint8_t left[8], uint8_t tl, int mode)
{
    if (mode == 1) { /* H */
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) out[y*8+x] = left[y];
    } else if (mode == 2) { /* V */
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) out[y*8+x] = top[x];
    } else if (mode == 4) { /* DC_LEFT: two row groups */
        int a = 0, b = 0;
        for (int i = 0; i < 4; ++i) { a += left[i]; b += left[i+4]; }
        int d0 = (a+2)>>2, d1 = (b+2)>>2;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) out[y*8+x] = (uint8_t)(y < 4 ? d0 : d1);
    } else if (mode == 5) { /* DC_TOP: two column groups */
        int a = 0, b = 0;
        for (int i = 0; i < 4; ++i) { a += top[i]; b += top[i+4]; }
        int d0 = (a+2)>>2, d1 = (b+2)>>2;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) out[y*8+x] = (uint8_t)(x < 4 ? d0 : d1);
    } else if (mode == 6) { /* DC_128 */
        for (int i = 0; i < 64; ++i) out[i] = 128;
    } else if (mode == 3) { /* P: x264 predict_8x8c_p_c */
        int H = 0, V = 0;
        for (int i = 0; i < 4; ++i) {
            int th = (i == 3) ? tl : top[2-i];
            int tv = (i == 3) ? tl : left[2-i];
            H += (i+1) * (top[4+i] - th);
            V += (i+1) * (left[4+i] - tv);
        }
        int a = 16 * (left[7] + top[7]);
        int b = (17*H+16)>>5, c = (17*V+16)>>5;
        int i00 = a - 3*b - 3*c + 16;
        for (int y = 0; y < 8; ++y) {
            int pix = i00;
            for (int x = 0; x < 8; ++x) {
                out[y*8+x] = (uint8_t)clip_pixel(pix>>5);
                pix += b;
            }
            i00 += c;
        }
    } else { /* DC (0): four quadrants */
        int s0=0,s1=0,s2=0,s3=0;
        for (int i = 0; i < 4; ++i) {
            s0 += top[i]; s1 += top[i+4];
            s2 += left[i]; s3 += left[i+4];
        }
        int d0=(s0+s2+4)>>3, d1=(s1+2)>>2, d2=(s3+2)>>2, d3=(s1+s3+4)>>3;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                out[y*8+x] = (uint8_t)(y < 4 ? (x < 4 ? d0 : d1) : (x < 4 ? d2 : d3));
    }
}

int vk_select_chroma(const uint8_t srcu[64], const uint8_t srcv[64],
                     const uint8_t topu[8], const uint8_t leftu[8],
                     const uint8_t topv[8], const uint8_t leftv[8],
                     uint8_t tlu, uint8_t tlv, int avail, int qp)
{
    /* 5th column is the -1 terminator: the 4-mode row must not read OOB. */
    static const int8_t modes[5][5] = {
        {6,-1,-1,-1,-1}, {4,1,-1,-1,-1}, {5,2,-1,-1,-1},
        {2,1,0,-1,-1}, {2,1,0,3,-1}
    };
    int idx = avail & (1|2|8);
    idx = (idx == (1|2|8)) ? 4 : (idx & 3);
    static const int fix[7] = {0,1,2,3,0,0,0};
    uint8_t p[64];
    int best = modes[idx][0], bestcost = 0x7fffffff;
    for (int i = 0; modes[idx][i] >= 0; ++i) {
        int m = modes[idx][i];
        vk_predict_chroma8(p, topu, leftu, tlu, m);
        int cost = 0;
        for (int k = 0; k < 64; ++k) cost += srcu[k] > p[k] ? srcu[k]-p[k] : p[k]-srcu[k];
        vk_predict_chroma8(p, topv, leftv, tlv, m);
        for (int k = 0; k < 64; ++k) cost += srcv[k] > p[k] ? srcv[k]-p[k] : p[k]-srcv[k];
        int v = fix[m]+1, n = 0;
        while (v > 1) { v >>= 1; ++n; }
        cost += vk_lambda(qp) * (2*n+1);
        if (cost < bestcost) { bestcost = cost; best = m; }
    }
    return best;
}

static int sad16(const uint8_t a[256], const uint8_t b[256])
{
    int s = 0;
    for (int i = 0; i < 256; ++i) s += a[i] > b[i] ? a[i]-b[i] : b[i]-a[i];
    return s;
}

int vk_select16(const uint8_t src[256], const uint8_t top[16],
                const uint8_t left[16], uint8_t tl, int avail, int qp)
{
    static const int8_t modes[5][5] = {
        {6,-1,-1,-1,-1}, {4,1,-1,-1,-1}, {5,0,-1,-1,-1},
        {0,1,2,-1,-1}, {0,1,2,3,-1}
    };
    /* x264: MB_LEFT=0x01, MB_TOP=0x02, MB_TOPLEFT=0x08. */
    int idx = avail & (1|2|8);
    idx = (idx == (1|2|8)) ? 4 : (idx & 3);
    uint8_t pred[256];
    int best = modes[idx][0], best_cost = 0x7fffffff;
    int lambda = vk_lambda(qp);
    for (int i = 0; modes[idx][i] >= 0; ++i) {
        int m = modes[idx][i];
        vk_predict16(pred, top, left, tl, m);
        int cost = sad16(src, pred) + lambda * vk_mode_bits(m);
        if (cost < best_cost) { best_cost = cost; best = m; }
    }
    return best;
}

