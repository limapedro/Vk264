/* SPDX-License-Identifier: GPL-2.0-or-later
 * Sub-pel ME: x264 encoder/me.c refine_subpel() special case for subme=1.
 *
 * x264 tables (common/tables.c):
 *   hpel_ref0[16] = {0,1,1,1, 0,1,1,1, 2,3,3,3, 0,1,1,1}
 *   hpel_ref1[16] = {0,0,1,0, 2,2,3,2, 2,2,3,2, 2,2,3,2}
 * qpel_idx = ((mvy&3)<<2) + (mvx&3).
 * mc_luma picks src[ref0] at fullpel offset and, for qpel positions,
 * averages with src[ref1] (with the +1 rounding of pixel_avg).
 */
#include "subpel.h"
#include "hpel.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t hpel_ref0[16] = {0,1,1,1, 0,1,1,1, 2,3,3,3, 0,1,1,1};
static const uint8_t hpel_ref1[16] = {0,0,1,0, 2,2,3,2, 2,2,3,2, 2,2,3,2};

void vk_hpel_planes_init(vk_hpel_planes_t *p, const uint8_t *ref,
                         int rs, int w, int h, int pad)
{
    int sh = rs * (h + 2*pad);
    p->h = calloc(3, (size_t)sh);
    p->v = p->h + sh;
    p->c = p->v + sh;
    p->rs = rs;

    /* x264 hpel_filter writes dstv[x] for x in -2..w+2, so the destination
     * rows need at least 2 pixels of left padding (we have `pad`). */
    int16_t *buf = malloc((size_t)(w + 8) * sizeof(int16_t));
    for (int y = 0; y < h; ++y) {
        const uint8_t *src = ref + (y+pad)*rs + pad;
        uint8_t *dh = p->h + (y+pad)*rs + pad;
        uint8_t *dv = p->v + (y+pad)*rs + pad;
        uint8_t *dc = p->c + (y+pad)*rs + pad;
        vk_hpel_filter(dh, dv, dc, src, rs, w, 1, buf);
    }
    free(buf);
}

void vk_hpel_planes_free(vk_hpel_planes_t *p)
{
    free(p->h); p->h = p->v = p->c = NULL;
}

/* SAD between a 16x16 fenc block and an interpolated reference block.
 * The 4 planes are plane[0]=fullpel ref, [1]=h, [2]=v, [3]=c. */
static int sad_subpel(const uint8_t *fenc, const uint8_t *planes[4],
                      int rs, int x, int y, int mx, int my)
{
    /* planes[] share the padded-reference layout; offset to the current MB
     * before applying the (fullpel part of the) MV, as x264's p_fref_w does. */
    const uint8_t *base[4] = {
        planes[0] + y*rs + x, planes[1] + y*rs + x,
        planes[2] + y*rs + x, planes[3] + y*rs + x
    };
    int qi = ((my & 3) << 2) + (mx & 3);
    int ox = (mx >> 2), oy = (my >> 2);
    const uint8_t *s1 = base[hpel_ref0[qi]] + oy*rs + ox + ((my & 3) == 3) * rs;
    int need_avg = (qi & 5) != 0;
    const uint8_t *s2 = need_avg
        ? base[hpel_ref1[qi]] + oy*rs + ox + ((mx & 3) == 3)
        : NULL;
    int sad = 0;
    for (int y = 0; y < 16; ++y) {
        const uint8_t *row1 = s1 + y*rs;
        const uint8_t *row2 = s2 ? s2 + y*rs : NULL;
        const uint8_t *fc = fenc + y*16;
        for (int x = 0; x < 16; ++x) {
            int rv = row1[x];
            if (need_avg) rv = (rv + row2[x] + 1) >> 1;
            int d = fc[x] - rv;
            sad += d < 0 ? -d : d;
        }
    }
    return sad;
}

void vk_mc_luma_qpel(uint8_t *dst, int ds,
                     const uint8_t *ref, int rs,
                     const vk_hpel_planes_t *hpel,
                     int x, int y, int mx, int my)
{
    const uint8_t *planes[4] = { ref, hpel->h, hpel->v, hpel->c };
    const uint8_t *base[4] = {
        planes[0] + y*rs + x, planes[1] + y*rs + x,
        planes[2] + y*rs + x, planes[3] + y*rs + x
    };
    int qi = ((my & 3) << 2) + (mx & 3);
    int ox = (mx >> 2), oy = (my >> 2);
    const uint8_t *s1 = base[hpel_ref0[qi]] + oy*rs + ox + ((my & 3) == 3) * rs;
    int need_avg = (qi & 5) != 0;
    const uint8_t *s2 = need_avg
        ? base[hpel_ref1[qi]] + oy*rs + ox + ((mx & 3) == 3)
        : NULL;
    for (int yy = 0; yy < 16; ++yy)
        for (int xx = 0; xx < 16; ++xx) {
            int v = s1[yy*rs+xx];
            if (need_avg) v = (v + s2[yy*rs+xx] + 1) >> 1;
            dst[yy*ds+xx] = (uint8_t)v;
        }
}

static int mvcost(int mvd, int lambda)
{
    int i = mvd < 0 ? -mvd : mvd;
    double logs = i ? 2.0*log2((double)i + 1.0) + 1.718 : 0.718;
    int c = (int)(lambda*logs + 0.5);
    return c > 65535 ? 65535 : c;
}

void vk_me_refine_qpel(const uint8_t *fenc, int cs,
                       const uint8_t *ref, int rs, int pad,
                       const vk_hpel_planes_t *hpel,
                       int x, int y,
                       int *mvx, int *mvy, int *cost,
                       int mvpx, int mvpy, int lambda)
{
    (void)pad;
    const uint8_t *planes[4] = { ref, hpel->h, hpel->v, hpel->c };
    uint8_t fblock[256];
    for (int j = 0; j < 16; ++j)
        memcpy(fblock + j*16, fenc + (y+j)*cs + x, 16);
    const uint8_t *fc = fblock;
    int bmx = *mvx, bmy = *mvy, bcost = *cost;

    /* x264 tries the subpel component of the MVP when subme<3. */
    if (1) {
        int mx = mvpx, my = mvpy;
        if ((mx != bmx) || (my != bmy)) {
            int c = sad_subpel(fc, planes, rs, x, y, mx, my) + mvcost(mx-mvpx, lambda) + mvcost(my-mvpy, lambda);
            if (c < bcost) { bcost = c; bmx = mx; bmy = my; }
        }
    }

    /* ---- halfpel diamond search (1 iteration at subme=1) ---- */
    bcost <<= 6;
    for (int i = 1; i > 0; --i) {
        int omx = bmx, omy = bmy;
        int c[4];
        c[0] = sad_subpel(fc, planes, rs, x, y, omx, omy-2)
             + mvcost(omx-mvpx, lambda) + mvcost(omy-2-mvpy, lambda);
        c[1] = sad_subpel(fc, planes, rs, x, y, omx, omy+2)
             + mvcost(omx-mvpx, lambda) + mvcost(omy+2-mvpy, lambda);
        c[2] = sad_subpel(fc, planes, rs, x, y, omx-2, omy)
             + mvcost(omx-2-mvpx, lambda) + mvcost(omy-mvpy, lambda);
        c[3] = sad_subpel(fc, planes, rs, x, y, omx+2, omy)
             + mvcost(omx+2-mvpx, lambda) + mvcost(omy-mvpy, lambda);
        if ((c[0]<<6)+2  < bcost) bcost = (c[0]<<6)+2;
        if ((c[1]<<6)+6  < bcost) bcost = (c[1]<<6)+6;
        if ((c[2]<<6)+16 < bcost) bcost = (c[2]<<6)+16;
        if ((c[3]<<6)+48 < bcost) bcost = (c[3]<<6)+48;
        if (!(bcost & 63)) break;
        bmx -= (int32_t)((uint32_t)bcost << 26) >> 29;
        bmy -= (int32_t)((uint32_t)bcost << 29) >> 29;
        bcost &= ~63;
    }
    bcost >>= 6;

    /* ---- quarterpel refinement (subme=1 special case: 1 iteration) ---- */
    {
        int omx = bmx, omy = bmy;
        int c[4];
        c[0] = sad_subpel(fc, planes, rs, x, y, omx, omy-1)
             + mvcost(omx-mvpx, lambda) + mvcost(omy-1-mvpy, lambda);
        c[1] = sad_subpel(fc, planes, rs, x, y, omx, omy+1)
             + mvcost(omx-mvpx, lambda) + mvcost(omy+1-mvpy, lambda);
        c[2] = sad_subpel(fc, planes, rs, x, y, omx-1, omy)
             + mvcost(omx-1-mvpx, lambda) + mvcost(omy-mvpy, lambda);
        c[3] = sad_subpel(fc, planes, rs, x, y, omx+1, omy)
             + mvcost(omx+1-mvpx, lambda) + mvcost(omy-mvpy, lambda);
        bcost <<= 4;
        if ((c[0]<<4)+1  < bcost) bcost = (c[0]<<4)+1;
        if ((c[1]<<4)+3  < bcost) bcost = (c[1]<<4)+3;
        if ((c[2]<<4)+4  < bcost) bcost = (c[2]<<4)+4;
        if ((c[3]<<4)+12 < bcost) bcost = (c[3]<<4)+12;
        /* direction bits 1,3,4,12 encode moves: (0,-1),(0,+1),(-1,0),(+1,0)
         * extracted with the same sign-extension trick as x264. */
        bmx -= (int32_t)((uint32_t)bcost << 28) >> 30;
        bmy -= (int32_t)((uint32_t)bcost << 30) >> 30;
        bcost &= ~15;
    }

    /* final SAD + mv cost at the chosen qpel MV */
    *mvx = bmx; *mvy = bmy;
    *cost = sad_subpel(fc, planes, rs, x, y, bmx, bmy)
          + mvcost(bmx-mvpx, lambda) + mvcost(bmy-mvpy, lambda);
}
