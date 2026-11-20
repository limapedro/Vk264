/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "intra16.h"

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
            H += (i+1) * (top[8+i] - top[6-i]);
            V += (i+1) * (left[8+i] - left[6-i]);
        }
        (void)tl;
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
    } else { /* DC (2) */
        int s = 16;
        for (int i = 0; i < 16; ++i) s += left[i] + top[i];
        uint8_t v = (uint8_t)((s+16)>>5);
        for (int i = 0; i < 256; ++i) out[i] = v;
    }
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
    int idx = avail & 7;
    idx = (idx == 7) ? 4 : (idx & 3);
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
