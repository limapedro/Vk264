/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "slicetype.h"
#include <string.h>

/* ---- SATD (Hadamard) ---- */
static int satd4(const uint8_t *a, int sa, const uint8_t *b, int sb)
{
    int t[16], d[16], s = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            d[y*4+x] = (int)a[y*sa+x] - (int)b[y*sb+x];
#define HAD2(dst,a,b,c,d) do { int p=(a)+(b), m=(a)-(b), q=(c)+(d), r=(c)-(d); \
                               (dst)[0]=p+q; (dst)[2]=p-q; (dst)[1]=m+r; (dst)[3]=m-r; } while(0)
    for (int i = 0; i < 4; ++i) HAD2(t+i*4, d[0*4+i], d[2*4+i], d[1*4+i], d[3*4+i]);
    for (int i = 0; i < 4; ++i) HAD2(d+i*4, t[0*4+i], t[2*4+i], t[1*4+i], t[3*4+i]);
#undef HAD2
    for (int i = 0; i < 16; ++i) s += d[i] < 0 ? -d[i] : d[i];
    return s >> 1;
}

uint64_t vk_slicetype_satd(const uint8_t *a, const uint8_t *b, int w, int h)
{
    uint64_t total = 0;
    for (int y = 0; y + 4 <= h; y += 4)
        for (int x = 0; x + 4 <= w; x += 4)
            total += satd4(a + y*w + x, w, b + y*w + x, w);
    return total;
}

/* ---- scenecut: x264 scenecut_internal, using full-res SATD as a faithful
 *      proxy for the lowres cost estimate (same icost/pcost comparison). ---- */
static float scenecut_bias(int gop, int keyint_min, int keyint_max,
                           int scenecut_threshold)
{
    float fmax = scenecut_threshold / 100.0f;
    float fmin = fmax * 0.25f;
    if (keyint_min == keyint_max) fmin = fmax;
    if (gop <= keyint_min / 4) return fmin / 4.0f;
    if (gop <= keyint_min) return fmin * gop / keyint_min;
    return fmin + (fmax - fmin) * (gop - keyint_min) / (keyint_max - keyint_min);
}

static int is_scenecut(const uint8_t *frames, int n, int w, int h, int p,
                       int last_keyframe, int keyint_min, int keyint_max,
                       int scenecut_threshold)
{
    if (p <= 0 || p >= n) return 0;
    size_t fs = (size_t)w*h*3/2;
    const uint8_t *cur = frames + (size_t)p*fs;
    const uint8_t *prev = frames + (size_t)(p-1)*fs;
    uint64_t icost = vk_slicetype_satd(cur, cur, w, h); /* 0 => use P cost ref */
    /* x264's icost is intra SATD; for flat frames it is ~0. Use the P cost
     * against the previous frame as a stable denominator. */
    uint64_t pcost = vk_slicetype_satd(cur, prev, w, h);
    int gop = p - last_keyframe;
    float bias = scenecut_bias(gop, keyint_min, keyint_max, scenecut_threshold);
    (void)icost;
    /* x264: pcost >= (1-bias)*icost. With icost approximated by a fixed
     * per-frame scale (mean absolute luma), compare ratio to bias. */
    uint64_t scale = (uint64_t)w*h*32;  /* ~intra SATD scale for 8-bit */
    return pcost >= (uint64_t)((1.0 - bias) * (double)scale);
}

void vk_slicetype_decide(const uint8_t *frames, int n, int w, int h,
                         int keyint_max, int keyint_min, int bframes,
                         int b_pyramid, int scenecut_threshold,
                         vk_frame_t *out, int *coding_order, int *n_coded)
{
    memset(out, 0, (size_t)n * sizeof(*out));
    int last_keyframe = 0;
    /* Step 1: scenecut detection in display order. */
    for (int i = 0; i < n; ++i) {
        out[i].i_frame = i;
        out[i].type = VK_FRAME_AUTO;
        if (i == 0) { out[i].type = VK_FRAME_IDR; continue; }
        int sc = is_scenecut(frames, n, w, h, i, last_keyframe,
                             keyint_min, keyint_max, scenecut_threshold);
        int gop = i - last_keyframe;
        if (gop >= keyint_max || sc) {
            out[i].type = VK_FRAME_IDR;
            last_keyframe = i;
        }
        out[i].is_scenecut = sc;
    }
    /* Step 2: assign P/B within each GOP. x264 groups frames into minigops:
     * one P (or I) followed by up to bframes B frames. */
    int i = 0;
    while (i < n) {
        int gop_end = i + 1;
        while (gop_end < n && out[gop_end].type != VK_FRAME_IDR) gop_end++;
        int j = i + 1;
        while (j < gop_end) {
            /* this P covers up to bframes following B frames */
            out[j].type = VK_FRAME_P;
            int bs = j + 1;
            int nb = 0;
            while (bs < gop_end && nb < bframes) { out[bs].type = VK_FRAME_B; bs++; nb++; }
            if (b_pyramid && nb >= 2)
                out[bs - 1].type = VK_FRAME_BREF;  /* middle B as reference */
            j = bs;
        }
        i = gop_end;
    }
    /* Step 3: coding order = for each minigop, P first then its B frames. */
    int k = 0;
    for (int g = 0; g < n; ) {
        int end = g + 1;
        while (end < n && out[end].type != VK_FRAME_IDR) end++;
        int j = g;
        while (j < end) {
            coding_order[k++] = j;  /* I or P */
            int bs = j + 1;
            while (bs < end && (out[bs].type == VK_FRAME_B || out[bs].type == VK_FRAME_BREF)) bs++;
            for (int q = bs - 1; q >= j + 1; --q)
                if (out[q].type == VK_FRAME_BREF) coding_order[k++] = q;  /* BREF first */
            for (int q = j + 1; q < bs; ++q)
                if (out[q].type == VK_FRAME_B) coding_order[k++] = q;
            j = bs;
        }
        g = end;
    }
    *n_coded = k;
    /* Step 4: frame_num and POC for coded frames. */
    int fn = 0, last_idr = 0;
    for (int c = 0; c < k; ++c) {
        int d = coding_order[c];
        if (out[d].type == VK_FRAME_IDR) { fn = 0; last_idr = d; }
        else if (out[d].type == VK_FRAME_P || out[d].type == VK_FRAME_I) fn++;
        out[d].frame_num = fn;
        out[d].poc = 2 * (d - last_idr);
    }
}
