/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "slicetype.h"
#include "lowres.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X264_LOOKAHEAD_MAX 60

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
    /* Step 2: assign P/B with x264's b-adapt=fast decision.
     * Build half-res luma planes for all frames, then for each candidate
     * frame compare the cost of path "...BP" (B) vs "...PP" (P). */
    int dw=w/2, dh=h/2;
    uint8_t **lo = malloc((size_t)n*sizeof(*lo));
    for (int f=0; f<n; ++f) {
        lo[f]=malloc((size_t)dw*dh*VK_LOWRES_PLANES);
        vk_lowres_luma(frames+(size_t)f*(size_t)w*h*3/2,w,h,lo[f],dw,dh);
    }
    int i = 0;
    while (i < n) {
        int gop_end = i + 1;
        while (gop_end < n && out[gop_end].type != VK_FRAME_IDR) gop_end++;
        /* x264 b-adapt=fast: last_nonb starts at the GOP keyframe. */
        int last_nonb = i;
        int remaining_b = bframes;
        /* x264 caches lowres_mvs on frame objects across all slicetype passes;
         * keep one MV state array for the whole GOP, indexed by absolute frame. */
        vk_lowres_mv_t *st = calloc(n, sizeof(*st));
        for(int qf=0;qf<n;++qf) vk_lowres_mv_init(&st[qf],dw,dh);
        for (int j = i+1; j < gop_end; ++j) {
            if (out[j].type != VK_FRAME_AUTO && out[j].type != VK_FRAME_B) {
                last_nonb = j; remaining_b = bframes; continue;
            }
            if (j-1 > i && out[j-1].type == VK_FRAME_B) remaining_b--;
            else { last_nonb = j-1; remaining_b = bframes; }
            if (!remaining_b) { out[j].type = VK_FRAME_P; continue; }
            if (j+1<gop_end && out[j+1].type == VK_FRAME_B) {
                out[j].type = VK_FRAME_P; continue;
            }
            /* x264 b-adapt=fast: path of nb Bs then "PP" vs "BP". */
            int nb = j-last_nonb-1;
            if (nb<0) nb=0;
            char path[X264_LOOKAHEAD_MAX+4];
            path[0]='X'; /* x264's path-- makes path[0] unused */
            for(int q=0;q<nb;++q) path[1+q]='B';
            path[1+nb]='P'; path[2+nb]='P'; path[3+nb]='\0';
            int npath = nb+4;
            if (npath > gop_end-last_nonb) npath = gop_end-last_nonb;
            uint64_t cp=vk_slicetype_path_cost(lo+last_nonb,dw,dh,path,npath,UINT64_MAX,st+last_nonb);
            path[1+nb]='B'; /* path[2+nb] stays 'P' */
            uint64_t cb=vk_slicetype_path_cost(lo+last_nonb,dw,dh,path,npath,cp,st+last_nonb);
            out[j].type = (cb < cp) ? VK_FRAME_B : VK_FRAME_P;
        }
        free(st);
        if (b_pyramid) {
            /* find B runs of length>=2 and mark the middle as BREF */
            for (int j=i+1;j<gop_end;++j)
                if (out[j].type==VK_FRAME_B && j+1<gop_end && out[j+1].type==VK_FRAME_B)
                    out[j].type=VK_FRAME_BREF;
        }
        i = gop_end;
    }
    for(int f=0;f<n;++f) free(lo[f]);
    free(lo);
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
