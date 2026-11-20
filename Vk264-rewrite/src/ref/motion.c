/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "motion.h"

void vk_mvp16(int lx, int ly, int tx, int ty, int cx, int cy,
              int *mvpx, int *mvpy)
{
    /* Component-wise median of the three predictors. */
    int a[3] = {lx, tx, cx}, b[3] = {ly, ty, cy};
    for (int i = 0; i < 3; ++i)
        for (int j = i+1; j < 3; ++j) {
            if (a[j] < a[i]) { int t=a[i]; a[i]=a[j]; a[j]=t; }
            if (b[j] < b[i]) { int t=b[i]; b[i]=b[j]; b[j]=t; }
        }
    *mvpx = a[1]; *mvpy = b[1];
}

void vk_pad_plane(uint8_t *dst, int ds, const uint8_t *src, int ss,
                  int w, int h, int pad)
{
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = src + y*ss;
        uint8_t *d = dst + (y+pad)*ds;
        for (int x = 0; x < pad; ++x) d[x] = row[0];
        for (int x = 0; x < w; ++x) d[pad+x] = row[x];
        for (int x = 0; x < pad; ++x) d[pad+w+x] = row[w-1];
    }
    for (int y = 0; y < pad; ++y) {
        for (int x = 0; x < ds; ++x) {
            dst[y*ds+x] = dst[pad*ds+x];
            dst[(h+pad+y)*ds+x] = dst[(h+pad-1)*ds+x];
        }
    }
}

static int sad16(const uint8_t *cur, int cs,
                 const uint8_t *ref, int rs,
                 int x, int y, int mx, int my)
{
    const uint8_t *c = cur + y*cs + x;
    const uint8_t *r = ref + (y+my)*rs + x + mx;  /* ref already padded */
    int sad = 0;
    for (int yy = 0; yy < 16; ++yy)
        for (int xx = 0; xx < 16; ++xx) {
            int cv = c[yy*cs+xx], rv = r[yy*rs+xx];
            sad += cv > rv ? cv-rv : rv-cv;
        }
    return sad;
}

int vk_me16(const uint8_t *cur, int cur_stride,
            const uint8_t *ref, int ref_stride, int pad,
            int x, int y, int mvpx, int mvpy, int range,
            int *best_x, int *best_y)
{
    /* ref pointer is the padded plane; the caller offsets x,y in unpadded
     * coordinates, so shift by pad here. */
    const uint8_t *rpad = ref + pad*ref_stride + pad;
    int best = sad16(cur, cur_stride, rpad, ref_stride, x, y, mvpx, mvpy);
    *best_x = mvpx; *best_y = mvpy;
    for (int my = mvpy-range; my <= mvpy+range; ++my)
        for (int mx = mvpx-range; mx <= mvpx+range; ++mx) {
            if (mx == mvpx && my == mvpy) continue;
            int sad = sad16(cur, cur_stride, rpad, ref_stride, x, y, mx, my);
            if (sad < best) { best = sad; *best_x = mx; *best_y = my; }
        }
    return best;
}
