/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "motion.h"
#include <math.h>

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

/* x264's precomputed MV bit cost: round(lambda * logs[|mvd_qp|]). */
static int mv_cost(int mvd_qp, int lambda)
{
    int i = mvd_qp < 0 ? -mvd_qp : mvd_qp;
    double logs = i ? 2.0*log2((double)i + 1.0) + 1.718 : 0.718;
    int c = (int)(lambda*logs + 0.5);
    return c > 65535 ? 65535 : c;
}

int vk_me16_dia(const uint8_t *cur, int cur_stride,
                const uint8_t *ref, int ref_stride, int pad,
                int x, int y, int mvpx, int mvpy, int range, int lambda,
                const int *cands, int ncands, int *best_x, int *best_y,
                int frame_h)
{
    const uint8_t *rpad = ref + pad*ref_stride + pad;
    /* x264's mv_limit_fpel (analyse.c:330-349): the searchable MV range is
     * clipped by the MB's position relative to the frame border, with a
     * 6-pel guard band (i_fpel_border). Default i_mv_range is 64 qpel*4. */
    int mbx = x/16, mby = y/16, mbw = cur_stride/16, mbh = frame_h/16;
    int ph = frame_h + 2*pad;   /* padded reference plane height */
    int rx0 = 4*(-16*mbx-24), rx1 = 4*(16*(mbw-mbx-1)+24);
    int ry0 = 4*(-16*mby-24), ry1 = 4*(16*(mbh-mby-1)+24);
    int lim_xmin = ((rx0 > -256 ? rx0 : -256) >> 2) + 6;
    int lim_xmax = ((rx1 <  255 ? rx1 :  255) >> 2) - 6;
    int lim_ymin = ((ry0 > -256 ? ry0 : -256) >> 2) + 6;
    int lim_ymax = ((ry1 <  255 ? ry1 :  255) >> 2) - 6;
#define MV_IN_RANGE(mx,my) ((mx)>=lim_xmin && (mx)<=lim_xmax && (my)>=lim_ymin && (my)<=lim_ymax)
    int bmx = mvpx, bmy = mvpy;
    /* At subme=0 x264 omits the MV cost of the rounded MVP. */
    int bcost = sad16(cur, cur_stride, rpad, ref_stride, pad, ph, x, y, bmx, bmy);
    *best_x = bmx; *best_y = bmy;

    /* x264 tests the spatial neighbour MV candidates (with MVD cost) and
     * starts the diamond search from the cheapest one. */
    if (ncands > 0) {
        /* x264 deliberately omits the MVD bit cost for the rounded MVP
         * (me.c:283-291), so compare candidates against the bare SAD. */
        int bestc = bcost;
        for (int i = 0; i < ncands; ++i) {
            int cx = cands[2*i], cy = cands[2*i+1];
            if (!MV_IN_RANGE(cx, cy)) continue;
            int c = sad16(cur, cur_stride, rpad, ref_stride, pad, ph, x, y, cx, cy)
                  + mv_cost((cx-mvpx)*4, lambda) + mv_cost((cy-mvpy)*4, lambda);
            if (c < bestc) { bestc = c; bmx = cx; bmy = cy; }
        }
        bcost = bestc;
    }

    /* Test the zero vector with full cost if the MVP is non-zero. x264's
     * COST_MV(0,0) charges BITS_MVD relative to the MVP (p_cost_mv[-mvp]),
     * like every other candidate; bmx/bmy is the current best MV, not the
     * MVP, so using it here gives the wrong MVD cost.  mvpx is fullpel. */
    if (bmx || bmy) {
        int c = sad16(cur, cur_stride, rpad, ref_stride, pad, ph, x, y, 0, 0)
              + mv_cost((0-mvpx)*4, lambda) + mv_cost((0-mvpy)*4, lambda);
        if (c < bcost) { bcost = c; bmx = 0; bmy = 0; }
    }
    *best_x = bmx; *best_y = bmy;

    bcost <<= 4;
    for (int i = range; i > 0; --i) {
        /* COST_MV_X4_DIR(0,-1, 0,1, -1,0, 1,0) */
        static const int dx[4] = {0,0,-1,1};
        static const int dy[4] = {-1,1,0,0};
        static const int dirbits[4] = {1,3,4,12};
        int best_dir = 0;
        for (int d = 0; d < 4; ++d) {
            int cx = bmx+dx[d], cy = bmy+dy[d];
            if (!MV_IN_RANGE(cx, cy)) continue;
            int c = (sad16(cur, cur_stride, rpad, ref_stride, pad, ph, x, y, cx, cy)
                     + mv_cost((cx-mvpx)*4, lambda) + mv_cost((cy-mvpy)*4, lambda)) << 4;
            c |= dirbits[d];
            if (c < bcost) { bcost = c; best_dir = dirbits[d]; }
        }
        if (!best_dir) break;
        bmx -= (int32_t)((uint32_t)bcost << 28) >> 30;
        bmy -= (int32_t)((uint32_t)bcost << 30) >> 30;
        bcost &= ~15;
    }
    *best_x = bmx; *best_y = bmy;
    /* x264's me16x16.cost = SAD + lambda-weighted MVD bits. */
    return sad16(cur, cur_stride, rpad, ref_stride, pad, ph, x, y, bmx, bmy)
         + mv_cost((bmx-mvpx)*4, lambda) + mv_cost((bmy-mvpy)*4, lambda);
#undef MV_IN_RANGE
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

/* `ref` is the padded plane offset to the unpadded frame origin.  x264's
 * reference planes carry a wide replicated-edge border, so an ME SAD window
 * that runs past the frame edge reads replicated border pixels.  The oracle's
 * padded plane has only `pad` columns/rows of border, so clamp each sample to
 * the plane bounds instead of letting it wrap into the next row. */
static int sad16(const uint8_t *cur, int cs,
                 const uint8_t *ref, int rs, int pad, int ph,
                 int x, int y, int mx, int my)
{
    const uint8_t *c = cur + y*cs + x;
    int xmin = -pad, xmax = rs-1-pad, ymin = -pad, ymax = ph-1-pad;
    int sad = 0;
    for (int yy = 0; yy < 16; ++yy) {
        int ry = y + my + yy;
        if (ry < ymin) ry = ymin; else if (ry > ymax) ry = ymax;
        for (int xx = 0; xx < 16; ++xx) {
            int rx = x + mx + xx;
            if (rx < xmin) rx = xmin; else if (rx > xmax) rx = xmax;
            int cv = c[yy*cs+xx], rv = ref[ry*rs+rx];
            sad += cv > rv ? cv-rv : rv-cv;
        }
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
    int ph = 1 << 25;   /* test-only: no row clamp */
    int best = sad16(cur, cur_stride, rpad, ref_stride, pad, ph, x, y, mvpx, mvpy);
    *best_x = mvpx; *best_y = mvpy;
    for (int my = mvpy-range; my <= mvpy+range; ++my)
        for (int mx = mvpx-range; mx <= mvpx+range; ++mx) {
            if (mx == mvpx && my == mvpy) continue;
            int sad = sad16(cur, cur_stride, rpad, ref_stride, pad, ph, x, y, mx, my);
            if (sad < best) { best = sad; *best_x = mx; *best_y = my; }
        }
    return best;
}
