/* Function-for-function port of x264's P-slice motion estimation:
 *   fullpel DIA / HEX / UMH   encoder/me.c:276-616
 *   qpel output + dispatch    encoder/me.c:774-798
 *   hpel/qpel refinement      encoder/me.c:801-992
 *   interpolation             common/mc.c:173-249, tables.c:183-184
 * Every ported operation carries its exact source line; see phx_me.h for the
 * complete source map.
 *
 * SHADER: A — pure, state-free per-MB search/interpolation (see phx_me.h).
 */
#include "phx_me.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* x264 encoder/me.h:30. */
#define PHX_COST_MAX (1 << 28)

/* x264 common/common.h:93-108. */
#define PHX_MIN(a,b) ((a) < (b) ? (a) : (b))
#define PHX_MAX(a,b) ((a) > (b) ? (a) : (b))
#define PHX_MIN3(a,b,c) PHX_MIN((a), PHX_MIN((b),(c)))   /* base.h:37 */
#define PHX_MIN4(a,b,c,d) PHX_MIN((a), PHX_MIN3((b),(c),(d))) /* base.h:37 */
#define PHX_CLIP3(a,v,b) ((v) < (a) ? (a) : ((v) > (b) ? (b) : (v)))

/* x264 common/common.h:145-148 (8-bit x264_clip_pixel). */
#define PHX_CLIP_PIXEL(x) (((x) & ~255) ? ((-(x)) >> 31) & 255 : (x))

/* x264 encoder/me.c:60-61 — BITS_MVD is implemented by phx_bits_mvd(). */
/* x264 encoder/me.c:178-180. */
#define PHX_FPEL(mv) (((mv)+2)>>2)

/* x264 encoder/me.c:204 — pack16to32_mask2(), used only by CHECK_MVRANGE. */
#define PHX_PACK16TO32_MASK2(mx,my) \
    (((uint32_t)(mx) << 16) | ((uint32_t)(my) & 0x7FFF))

/* x264 encoder/me.c:52-56 — search patterns, copied verbatim. */
static const uint8_t phx_mod6m1[8] = { 5,0,1,2,3,4,5,0 };
static const int8_t  phx_hex2[8][2] = {
    {-1,-2}, {-2,0}, {-1,2}, {1,2}, {2,0}, {1,-2}, {-1,-2}, {-2,0}
};
static const int8_t  phx_square1[9][2] = {
    {0,0}, {0,-1}, {0,1}, {-1,0}, {1,0}, {-1,-1}, {-1,1}, {1,-1}, {1,1}
};

/* x264 common/pixel.c:55-70 PIXEL_SAD_C( ..., 16, 16 ) at pixel.c:73. */
static int phx_pixel_sad_16x16(const uint8_t *pix1, intptr_t i_stride_pix1,
                               const uint8_t *pix2, intptr_t i_stride_pix2)
{
    int i_sum = 0;                                  /* pixel.c:59 */
    for (int y = 0; y < 16; y++)                    /* pixel.c:60 */
    {
        for (int x = 0; x < 16; x++)                /* pixel.c:61 */
            i_sum += abs((int)pix1[x] - (int)pix2[x]); /* pixel.c:64 */
        pix1 += i_stride_pix1;                      /* pixel.c:67 */
        pix2 += i_stride_pix2;                      /* pixel.c:68 */
    }
    return i_sum;                                   /* pixel.c:69 */
}

/* x264 common/pixel.c:235-259 — the pseudo-SIMD Hadamard accumulator used by
 * SATD.  Needed only by refine_subpel()'s qpel stage (me.c:826/831/956-959). */
typedef uint16_t phx_sum_t;                         /* pixel.c:237 */
typedef uint32_t phx_sum2_t;                        /* pixel.c:238 */
#define PHX_BITS_PER_SUM 16                         /* pixel.c:240 (8-bit) */
#define PHX_HADAMARD4(d0,d1,d2,d3,s0,s1,s2,s3) {\
    phx_sum2_t t0 = (s0) + (s1);\
    phx_sum2_t t1 = (s0) - (s1);\
    phx_sum2_t t2 = (s2) + (s3);\
    phx_sum2_t t3 = (s2) - (s3);\
    (d0) = t0 + t2;\
    (d2) = t0 - t2;\
    (d1) = t1 + t3;\
    (d3) = t1 - t3;\
}
static phx_sum2_t phx_abs2(phx_sum2_t a)             /* pixel.c:255-259 */
{
    phx_sum2_t s = ((a >> (PHX_BITS_PER_SUM-1)) &
                (((phx_sum2_t)1 << PHX_BITS_PER_SUM) + 1)) * (phx_sum_t)-1;
    return (a + s) ^ s;
}
static int phx_pixel_satd_8x4(const uint8_t *p1, intptr_t i1,
                               const uint8_t *p2, intptr_t i2)
{
    phx_sum2_t tmp[4][4];                            /* pixel.c:292 */
    phx_sum2_t a0,a1,a2,a3;
    phx_sum2_t sum = 0;                              /* pixel.c:294 */
    for (int i = 0; i < 4; i++, p1 += i1, p2 += i2)   /* pixel.c:295 */
    {
        a0 = (phx_sum2_t)(p1[0] - p2[0]) + ((phx_sum2_t)(p1[4] - p2[4]) << PHX_BITS_PER_SUM); /* :297 */
        a1 = (phx_sum2_t)(p1[1] - p2[1]) + ((phx_sum2_t)(p1[5] - p2[5]) << PHX_BITS_PER_SUM); /* :298 */
        a2 = (phx_sum2_t)(p1[2] - p2[2]) + ((phx_sum2_t)(p1[6] - p2[6]) << PHX_BITS_PER_SUM); /* :299 */
        a3 = (phx_sum2_t)(p1[3] - p2[3]) + ((phx_sum2_t)(p1[7] - p2[7]) << PHX_BITS_PER_SUM); /* :300 */
        PHX_HADAMARD4(tmp[i][0], tmp[i][1], tmp[i][2], tmp[i][3], a0,a1,a2,a3); /* :301 */
    }
    for (int i = 0; i < 4; i++)                      /* pixel.c:303 */
    {
        PHX_HADAMARD4(a0, a1, a2, a3, tmp[0][i], tmp[1][i], tmp[2][i], tmp[3][i]); /* :305 */
        sum += phx_abs2(a0) + phx_abs2(a1) + phx_abs2(a2) + phx_abs2(a3); /* :306 */
    }
    return (((phx_sum_t)sum) + (sum >> PHX_BITS_PER_SUM)) >> 1; /* pixel.c:308 */
}
static int phx_pixel_satd_16x16(const uint8_t *p1, intptr_t i1,
                                 const uint8_t *p2, intptr_t i2)
{
    /* PIXEL_SATD_C(16,16,...), pixel.c:311-327. */
    int sum = phx_pixel_satd_8x4(p1, i1, p2, i2)                       /* :314 */
            + phx_pixel_satd_8x4(p1+4*i1, i1, p2+4*i2, i2);            /* :315 */
    sum += phx_pixel_satd_8x4(p1+8, i1, p2+8, i2)                      /* :317 */
         + phx_pixel_satd_8x4(p1+8+4*i1, i1, p2+8+4*i2, i2);           /* :318 */
    sum += phx_pixel_satd_8x4(p1+8*i1, i1, p2+8*i2, i2)                /* :320 */
         + phx_pixel_satd_8x4(p1+12*i1, i1, p2+12*i2, i2);             /* :321 */
    sum += phx_pixel_satd_8x4(p1+8+8*i1, i1, p2+8+8*i2, i2)            /* :323 */
         + phx_pixel_satd_8x4(p1+8+12*i1, i1, p2+8+12*i2, i2);         /* :324 */
    return sum;                                                        /* :325 */
}

/* x264 encoder/analyse.c:186-188 (logs) and :156 (lambda * logs + .5).
 * Used when no cost_mv table matches `lambda`, so phx_me16 is self-contained. */
static int phx_mv_cost_formula(int lambda, int delta)
{
    int i = delta < 0 ? -delta : delta;
    float logs = i ? log2f((float)(i+1)) * 2.0f + 1.718f : 0.718f;
    int c = (int)(lambda * logs + 0.5f);            /* analyse.c:156 */
    return PHX_MIN(c, 65535);                       /* analyse.c:156 UINT16_MAX */
}

/* phx_me16 is parameterised by lambda, not by a QP.  phx_cost_tables_t is
 * indexed by QP (phx_cost.h:24), so pick the table built with the same
 * phx_lambda_tab entry (tables.c:102).  NULL means "use the log formula". */
static const uint16_t *phx_resolve_cost_mv(const phx_cost_tables_t *cost,
                                            int lambda)
{
    if (!cost) return NULL;
    for (int qp = 0; qp <= PHX_QP_MAX; qp++)
        if (cost->cost_mv[qp] && phx_lambda_tab[qp] == (uint16_t)lambda)
            return cost->cost_mv[qp];
    return NULL;
}

/* x264 encoder/me.c:209 — CHECK_MVRANGE() as a function (bit-identical). */
static int phx_check_mv_range(int mx, int my, uint32_t mv_min, uint32_t mv_max)
{
    uint32_t p = PHX_PACK16TO32_MASK2(mx, my);      /* me.c:204 */
    /* me.c:209 */
    return !(((p + mv_min) | (mv_max - p)) & 0x80004000u);
}

typedef struct {
    const uint8_t *fenc;      /* me.c:192 p_fenc, already at (x,y) */
    const uint8_t *fref;      /* me.c:193 p_fref_w, already at (x,y) */
    intptr_t stride;          /* me.c:187 */
    const uint16_t *p_cost_mv;/* me.c:211 p_cost_mv, centred at 0 */
    int lambda;               /* analyse.c:149 */
    int mv_range;             /* table half-width guard, analyse.c:151 */
    int mvp_x, mvp_y;         /* me.c:211-212 subtraction offsets (qpel) */
    int bmx, bmy, bcost;      /* me.c:189 — the ONLY best-MV state */
} phx_me_ctx_t;

/* x264 encoder/me.c:60-61/211-212:
 *   p_cost_mvx = p_cost_mv - m->mvp[0];  BITS_MVD(mx,my) = p_cost_mvx[mx*4]
 *                                                       + p_cost_mvy[my*4].
 * => p_cost_mv[mx*4 - mvp_x] + p_cost_mv[my*4 - mvp_y].
 * Indexes outside the table's qpel half-width (8*mv_range, analyse.c:151)
 * use the identical log formula so a plausible-but-unclipped predictor can
 * never read out of bounds. */
static int phx_bits_mvd(const phx_me_ctx_t *c, int mx, int my)
{
    int dx = mx*4 - c->mvp_x;
    int dy = my*4 - c->mvp_y;
    int lim = 8 * c->mv_range;
    int cx = (c->p_cost_mv && dx >= -lim && dx <= lim)
           ? c->p_cost_mv[dx] : phx_mv_cost_formula(c->lambda, dx);
    int cy = (c->p_cost_mv && dy >= -lim && dy <= lim)
           ? c->p_cost_mv[dy] : phx_mv_cost_formula(c->lambda, dy);
    return cx + cy;
}

/* x264 encoder/me.c:63-70 — COST_MV(). */
static void phx_cost_mv(phx_me_ctx_t *c, int mx, int my)
{
    int cost = phx_pixel_sad_16x16(c->fenc, c->stride,
                                   c->fref + my*c->stride + mx, c->stride)
              + phx_bits_mvd(c, mx, my);            /* me.c:66-68 */
    if (cost < c->bcost) {                         /* COPY3_IF_LT, me.c:69 */
        c->bcost = cost;
        c->bmx = mx;
        c->bmy = my;
    }
}

/* x264 encoder/me.c:94-107 — COST_MV_X4_DIR(): relative to the CURRENT best. */
static void phx_cost_mv_x4_dir(phx_me_ctx_t *c, const int off[4][2],
                               int costs[4])
{
    const uint8_t *base = c->fref + c->bmy*c->stride + c->bmx; /* me.c:96 */
    for (int i = 0; i < 4; i++)                    /* me.c:97-102 */
        costs[i] = phx_pixel_sad_16x16(c->fenc, c->stride,
                        base + off[i][1]*c->stride + off[i][0], c->stride)
                 + phx_bits_mvd(c, c->bmx+off[i][0], c->bmy+off[i][1]);
                                                   /* me.c:103-106 */
}

/* x264 encoder/me.c:81-92 — COST_MV_X3_DIR(): relative to the CURRENT best. */
static void phx_cost_mv_x3_dir(phx_me_ctx_t *c, const int off[3][2],
                               int costs[3])
{
    const uint8_t *base = c->fref + c->bmy*c->stride + c->bmx; /* me.c:83 */
    for (int i = 0; i < 3; i++)                    /* me.c:84-88 */
        costs[i] = phx_pixel_sad_16x16(c->fenc, c->stride,
                        base + off[i][1]*c->stride + off[i][0], c->stride)
                 + phx_bits_mvd(c, c->bmx+off[i][0], c->bmy+off[i][1]);
                                                   /* me.c:89-91 */
}

/* x264 encoder/me.c:109-126 COST_MV_X4() with COPY3_IF_LT (:122-125).
 * Used by UMH's DIA1_ITER/CROSS/octagon probes; the centre is the caller's
 * omx/omy and is NOT the current best bmx/bmy (me.c:111,521). */
static void phx_cost_mv_x4_abs(phx_me_ctx_t *c, int omx, int omy,
                               const int off[4][2], int costs[4])
{
    const uint8_t *base = c->fref + omy*c->stride + omx;       /* me.c:111 */
    for (int i = 0; i < 4; i++)                    /* me.c:112-117 */
        costs[i] = phx_pixel_sad_16x16(c->fenc, c->stride,
                        base + off[i][1]*c->stride + off[i][0], c->stride)
                 + phx_bits_mvd(c, omx+off[i][0], omy+off[i][1]); /* :118-121 */
    for (int i = 0; i < 4; i++)                    /* COPY3_IF_LT, :122-125 */
        if (costs[i] < c->bcost) {
            c->bcost = costs[i];
            c->bmx = omx + off[i][0];
            c->bmy = omy + off[i][1];
        }
}

/* x264's COPY1_IF_LT (common/common.h): a plain strict "less-than" assign.
 * The callers pack the direction tag into `val` themselves (e.g. me.c:330-333
 * passes (costs[0]<<4)+1), so this macro must NOT add `tag` a second time.
 * `tag` is kept only to document which search candidate each line handles. */
#define PHX_COPY1_IF_LT_TAG(bcost, val, tag) do { \
    (void)(tag); \
    if ((val) < (bcost)) (bcost) = (val); \
} while (0)

/* x264 encoder/me.c:344-418 — the HEX search body, factored out so both
 * X264_ME_HEX (:344) and the UMH fall-through (:614 "goto me_hex2") run the
 * identical code.  SHADER: A — pure, no hidden state. */
static void phx_hex2_search(phx_me_ctx_t *c, int mv_range,
                            uint32_t mv_min, uint32_t mv_max, int costs[8])
{
    /* me.c:344-375 — initial hexagon, radius 2. */
    const int ha[3][2] = { {-2,0}, {-1,2}, {1,2} };  /* me.c:367 */
    const int hb[3][2] = { { 2,0}, { 1,-2}, {-1,-2} };/* me.c:368 */
    phx_cost_mv_x3_dir(c, ha, costs);               /* me.c:367 */
    phx_cost_mv_x3_dir(c, hb, costs+4);             /* me.c:368 (+4 align) */
    c->bcost <<= 3;                                 /* me.c:369 */
    PHX_COPY1_IF_LT_TAG(c->bcost, (costs[0]<<3)+2, 2); /* me.c:370 */
    PHX_COPY1_IF_LT_TAG(c->bcost, (costs[1]<<3)+3, 3); /* me.c:371 */
    PHX_COPY1_IF_LT_TAG(c->bcost, (costs[2]<<3)+4, 4); /* me.c:372 */
    PHX_COPY1_IF_LT_TAG(c->bcost, (costs[4]<<3)+5, 5); /* me.c:373 */
    PHX_COPY1_IF_LT_TAG(c->bcost, (costs[5]<<3)+6, 6); /* me.c:374 */
    PHX_COPY1_IF_LT_TAG(c->bcost, (costs[6]<<3)+7, 7); /* me.c:375 */

    if (c->bcost & 7)                               /* me.c:377 */
    {
        int dir = (c->bcost & 7) - 2;               /* me.c:379 */
        c->bmx += phx_hex2[dir+1][0];               /* me.c:380 */
        c->bmy += phx_hex2[dir+1][1];               /* me.c:381 */
        /* me.c:383-384 — half hexagon, no overlap with previous iteration. */
        for (int i = (mv_range>>1) - 1;
             i > 0 && phx_check_mv_range(c->bmx, c->bmy, mv_min, mv_max); i--)
        {
            int off[3][2] = {
                { phx_hex2[dir+0][0], phx_hex2[dir+0][1] },
                { phx_hex2[dir+1][0], phx_hex2[dir+1][1] },
                { phx_hex2[dir+2][0], phx_hex2[dir+2][1] }
            };
            phx_cost_mv_x3_dir(c, off, costs);      /* me.c:386-389 */
            c->bcost &= ~7;                         /* me.c:390 */
            PHX_COPY1_IF_LT_TAG(c->bcost, (costs[0]<<3)+1, 1); /* :391 */
            PHX_COPY1_IF_LT_TAG(c->bcost, (costs[1]<<3)+2, 2); /* :392 */
            PHX_COPY1_IF_LT_TAG(c->bcost, (costs[2]<<3)+3, 3); /* :393 */
            if (!(c->bcost & 7)) break;             /* me.c:394-395 */
            dir += (c->bcost & 7) - 2;              /* me.c:396 */
            dir = phx_mod6m1[dir+1];                /* me.c:397 */
            c->bmx += phx_hex2[dir+1][0];           /* me.c:398 */
            c->bmy += phx_hex2[dir+1][1];           /* me.c:399 */
        }
    }
    c->bcost >>= 3;                                 /* me.c:402 */

    /* me.c:404-418 — square refine. */
    const int s1[4][2] = { {0,-1}, {0,1}, {-1,0}, {1,0} };   /* me.c:406 */
    const int s2[4][2] = { {-1,-1}, {-1,1}, {1,-1}, {1,1} }; /* me.c:411 */
    c->bcost <<= 4;                                 /* me.c:405 */
    phx_cost_mv_x4_dir(c, s1, costs);               /* me.c:406 */
    PHX_COPY1_IF_LT_TAG(c->bcost, (costs[0]<<4)+1, 1); /* me.c:407 */
    PHX_COPY1_IF_LT_TAG(c->bcost, (costs[1]<<4)+2, 2); /* me.c:408 */
    PHX_COPY1_IF_LT_TAG(c->bcost, (costs[2]<<4)+3, 3); /* me.c:409 */
    PHX_COPY1_IF_LT_TAG(c->bcost, (costs[3]<<4)+4, 4); /* me.c:410 */
    phx_cost_mv_x4_dir(c, s2, costs);               /* me.c:411 */
    PHX_COPY1_IF_LT_TAG(c->bcost, (costs[0]<<4)+5, 5); /* me.c:412 */
    PHX_COPY1_IF_LT_TAG(c->bcost, (costs[1]<<4)+6, 6); /* me.c:413 */
    PHX_COPY1_IF_LT_TAG(c->bcost, (costs[2]<<4)+7, 7); /* me.c:414 */
    PHX_COPY1_IF_LT_TAG(c->bcost, (costs[3]<<4)+8, 8); /* me.c:415 */
    c->bmx += phx_square1[c->bcost & 15][0];        /* me.c:416 */
    c->bmy += phx_square1[c->bcost & 15][1];        /* me.c:417 */
    c->bcost >>= 4;                                 /* me.c:418 */
}

int phx_me16(int x, int y, const phx_frame_t *fenc, const phx_frame_t *fref,
             const phx_cost_tables_t *cost, int mv_range, int lambda,
             int method, const int16_t pred[2], int *out_mv)
{
    if (!fenc || !fref || !fenc->plane || !fref->plane || !out_mv)
        return -1;
    if (mv_range < 0 || x < 0 || y < 0 ||
        x % 16 || y % 16 || x + 16 > fenc->i_width || y + 16 > fenc->i_height)
        return -1;
    /* DIA / HEX / UMH are ported; ESA / TESA (>= 3) are rejected. */
    if (method != PHX_ME_DIA && method != PHX_ME_HEX && method != PHX_ME_UMH)
        return -1;

    /* x264 analyse.c:330-339 (:336-339) and :391-396 (:391-394), with
     * i_fmv_range = 4*i_me_range (:330) and i_fpel_border = 6 (:333).
     * Progressive, single-thread build: thread_mvy_range = i_fmv_range (:353). */
    int mb_x = x / 16, mb_y = y / 16;
    int mb_w = fref->i_width / 16, mb_h = fref->i_height / 16;
    int i_fmv_range = 4 * mv_range;                   /* analyse.c:330 */
    int min_x_s = PHX_MAX(4*(-16*mb_x - 24), -i_fmv_range); /* :336/:338 */
    int max_x_s = PHX_MIN(4*(16*(mb_w - mb_x - 1) + 24), i_fmv_range-1); /* :337/:339 */
    int min_y_s = PHX_MAX(4*(-16*mb_y - 24), -i_fmv_range); /* :391/:393 */
    int max_y_s = PHX_MIN3(4*(16*(mb_h - mb_y - 1) + 24), i_fmv_range-1,
                           4*i_fmv_range);            /* :392/:394 */
    int mv_x_min = (min_x_s >> 2) + 6;                /* analyse.c:348 */
    int mv_x_max = (max_x_s >> 2) - 6;                /* analyse.c:349 */
    int mv_y_min = (min_y_s >> 2) + 6;                /* analyse.c:395 */
    int mv_y_max = (max_y_s >> 2) - 6;                /* analyse.c:396 */
    /* x264's p_fref_w window already guarantees in-bounds reads; the extra
     * clamp here only protects the standalone padded-plane caller. */
    if (x + mv_x_min < -PHX_PADH) mv_x_min = -PHX_PADH - x;
    if (x + mv_x_max + 16 > fref->i_width  + PHX_PADH)
        mv_x_max = fref->i_width  + PHX_PADH - x - 16;
    if (y + mv_y_min < -PHX_PADV) mv_y_min = -PHX_PADV - y;
    if (y + mv_y_max + 16 > fref->i_height + PHX_PADV)
        mv_y_max = fref->i_height + PHX_PADV - y - 16;
    if (mv_x_min > mv_x_max) mv_x_min = mv_x_max = 0;
    if (mv_y_min > mv_y_max) mv_y_min = mv_y_max = 0;
    uint32_t mv_min = PHX_PACK16TO32_MASK2(-mv_x_min, -mv_y_min); /* me.c:205 */
    uint32_t mv_max = PHX_PACK16TO32_MASK2( mv_x_max,  mv_y_max) | 0x8000u;
                                                                   /* me.c:206 */

    phx_me_ctx_t c;
    c.fenc = fenc->plane + y*fenc->i_stride + x;      /* me.c:192 */
    /* p_fref_w is a window at the current block, so candidate (mx,my) reads
     * fref[(y+my)*stride + (x+mx)] (me.c:193, 66-67). */
    c.fref = fref->plane + y*fref->i_stride + x;      /* me.c:193 p_fref_w */
    c.stride = fref->i_stride;                       /* me.c:187 */
    c.p_cost_mv = phx_resolve_cost_mv(cost, lambda); /* me.c:211 */
    c.lambda = lambda;                               /* analyse.c:149 */
    c.mv_range = mv_range;
    c.mvp_x = pred ? pred[0] : 0;                    /* me.h:50, me.c:211 */
    c.mvp_y = pred ? pred[1] : 0;
    c.bcost = PHX_COST_MAX;                          /* me.c:189 */

    /* x264 me.c:276-281 — full-pel MVP branch (subme < 3).  All best-MV
     * state lives in `c`; x264's bmx/bmy are c.bmx/c.bmy here. */
    c.bmx = PHX_CLIP3(PHX_FPEL(c.mvp_x), mv_x_min, mv_x_max); /* me.c:279 */
    c.bmy = PHX_CLIP3(PHX_FPEL(c.mvp_y), mv_y_min, mv_y_max); /* me.c:280 */
    int pmvx = c.bmx, pmvy = c.bmy;                  /* me.c:281 pmv */
    /* me.c:291 — SAD only; MV bits are deliberately omitted for the MVP. */
    c.bcost = phx_pixel_sad_16x16(c.fenc, c.stride,
                                   c.fref + c.bmy*c.stride + c.bmx, c.stride);
    /* me.c:316-317 — test the zero vector when it is not the MVP.
     * (No mvc[] candidates exist in this API: me.c:293-313 is skipped.) */
    if (c.bmx | c.bmy) phx_cost_mv(&c, 0, 0);

    int costs[16];

    if (method == PHX_ME_DIA)
    {
        /* x264 me.c:322-341 — diamond search, radius 1. */
        const int dia[4][2] = { {0,-1}, {0,1}, {-1,0}, {1,0} }; /* me.c:329 */
        c.bcost <<= 4;                               /* me.c:325 */
        int i = mv_range;                            /* me.c:326 */
        do
        {
            phx_cost_mv_x4_dir(&c, dia, costs);      /* me.c:329 */
            PHX_COPY1_IF_LT_TAG(c.bcost, (costs[0]<<4)+1,  1); /* :330 */
            PHX_COPY1_IF_LT_TAG(c.bcost, (costs[1]<<4)+3,  3); /* :331 */
            PHX_COPY1_IF_LT_TAG(c.bcost, (costs[2]<<4)+4,  4); /* :332 */
            PHX_COPY1_IF_LT_TAG(c.bcost, (costs[3]<<4)+12, 12);/* :333 */
            if (!(c.bcost & 15)) break;              /* me.c:334-335 */
            c.bmx -= (int32_t)((uint32_t)c.bcost << 28) >> 30; /* me.c:336 */
            c.bmy -= (int32_t)((uint32_t)c.bcost << 30) >> 30; /* me.c:337 */
            c.bcost &= ~15;                          /* me.c:338 */
        } while (--i && phx_check_mv_range(c.bmx, c.bmy, mv_min, mv_max));
                                                     /* me.c:339 */
        c.bcost >>= 4;                               /* me.c:340 */
    }
    else if (method == PHX_ME_HEX)
    {
        phx_hex2_search(&c, mv_range, mv_min, mv_max, costs);
    }
    else /* PHX_ME_UMH — me.c:422-616, PIXEL_16x16 (pixel_size_shift[0]=0). */
    {
        /* me.c:427-430: pixel_size_shift[0] = 0, SAD_THRESH(v) = bcost < v. */
        int ucost1, ucost2, cross_start = 1;

        /* me.c:432-434 — refine predictors: DIA1_ITER(pmx,pmy). */
        ucost1 = c.bcost;                            /* me.c:433 */
        {
            const int off[4][2] = { {0,-1}, {0,1}, {-1,0}, {1,0} }; /* :149 */
            phx_cost_mv_x4_abs(&c, pmvx, pmvy, off, costs); /* me.c:434 */
        }
        if (pmvx | pmvy)                             /* me.c:435 */
        {
            const int off[4][2] = { {0,-1}, {0,1}, {-1,0}, {1,0} }; /* :149 */
            phx_cost_mv_x4_abs(&c, 0, 0, off, costs); /* me.c:436 */
        }

        /* me.c:438-440 skipped: PIXEL_16x16 (0) != PIXEL_4x4, so no
         * "goto me_hex2" here; the search continues with UMH. */

        ucost2 = c.bcost;                            /* me.c:441 */
        if ((c.bmx | c.bmy) && ((c.bmx-pmvx) | (c.bmy-pmvy))) /* me.c:442 */
        {
            const int off[4][2] = { {0,-1}, {0,1}, {-1,0}, {1,0} }; /* :149 */
            phx_cost_mv_x4_abs(&c, c.bmx, c.bmy, off, costs); /* me.c:443 */
        }
        if (c.bcost == ucost2) cross_start = 3;      /* me.c:444-445 */
        int omx = c.bmx, omy = c.bmy;                /* me.c:446 */

        /* me.c:448-466 — early termination.  CROSS uses the unchanged
         * omx/omy centre (:446, :521). */
        if (c.bcost == ucost2 && c.bcost < 2000)     /* me.c:449-450 */
        {
            const int o1[4][2] = { {0,-2}, {-1,-1}, {1,-1}, {-2,0} }; /* :452 */
            const int o2[4][2] = { {2,0}, {-1,1}, {1,1}, {0,2} };     /* :453 */
            phx_cost_mv_x4_abs(&c, omx, omy, o1, costs); /* me.c:452 */
            phx_cost_mv_x4_abs(&c, omx, omy, o2, costs); /* me.c:453 */
            if (c.bcost == ucost1 && c.bcost < 500) goto phx_umh_end; /* :454-455 */
            if (c.bcost == ucost2)                   /* me.c:456 */
            {
                int range = (mv_range >> 1) | 1;     /* me.c:458 */
                /* CROSS(3, range, range), me.c:459 (macro :152-176). */
                int i = 3;
                if (range <= PHX_MIN(mv_x_max-omx, omx-mv_x_min)) /* :155 */
                    for (; i < range-2; i += 4)      /* :156 */
                    {
                        const int cr[4][2] = {
                            {i,0}, {-i,0}, {i+2,0}, {-i-2,0} }; /* :157 */
                        phx_cost_mv_x4_abs(&c, omx, omy, cr, costs);
                    }
                for (; i < range; i += 2)            /* :158 */
                {
                    if (omx+i <= mv_x_max) phx_cost_mv(&c, omx+i, omy); /* :160-161 */
                    if (omx-i >= mv_x_min) phx_cost_mv(&c, omx-i, omy); /* :162-163 */
                }
                i = 3;                               /* :165 */
                if (range <= PHX_MIN(mv_y_max-omy, omy-mv_y_min)) /* :166 */
                    for (; i < range-2; i += 4)      /* :167 */
                    {
                        const int cr[4][2] = {
                            {0,i}, {0,-i}, {0,i+2}, {0,-i-2} }; /* :168 */
                        phx_cost_mv_x4_abs(&c, omx, omy, cr, costs);
                    }
                for (; i < range; i += 2)            /* :169 */
                {
                    if (omy+i <= mv_y_max) phx_cost_mv(&c, omx, omy+i); /* :171-172 */
                    if (omy-i >= mv_y_min) phx_cost_mv(&c, omx, omy-i); /* :173-174 */
                }
                const int o3[4][2] = { {-1,-2}, {1,-2}, {-2,-1}, {2,-1} }; /* :460 */
                const int o4[4][2] = { {-2,1}, {2,1}, {-1,2}, {1,2} };     /* :461 */
                phx_cost_mv_x4_abs(&c, omx, omy, o3, costs); /* me.c:460 */
                phx_cost_mv_x4_abs(&c, omx, omy, o4, costs); /* me.c:461 */
                if (c.bcost == ucost2) goto phx_umh_end; /* me.c:462-463 */
                cross_start = range + 2;             /* me.c:464 */
            }
        }

        /* me.c:468-519 — adaptive search range.  No mvc[] exists in this
         * API (i_mvc == 0), so the multiplier block is skipped verbatim. */

        /* me.c:521-523 — CROSS(cross_start, i_me_range, i_me_range>>1). */
        {
            int i = cross_start;
            int x_max = mv_range, y_max = mv_range >> 1;
            if (x_max <= PHX_MIN(mv_x_max-omx, omx-mv_x_min)) /* :155 */
                for (; i < x_max-2; i += 4)          /* :156 */
                {
                    const int cr[4][2] = {
                        {i,0}, {-i,0}, {i+2,0}, {-i-2,0} }; /* :157 */
                    phx_cost_mv_x4_abs(&c, omx, omy, cr, costs);
                }
            for (; i < x_max; i += 2)                /* :158 */
            {
                if (omx+i <= mv_x_max) phx_cost_mv(&c, omx+i, omy); /* :160-161 */
                if (omx-i >= mv_x_min) phx_cost_mv(&c, omx-i, omy); /* :162-163 */
            }
            i = cross_start;                         /* :165 */
            if (y_max <= PHX_MIN(mv_y_max-omy, omy-mv_y_min)) /* :166 */
                for (; i < y_max-2; i += 4)          /* :167 */
                {
                    const int cr[4][2] = {
                        {0,i}, {0,-i}, {0,i+2}, {0,-i-2} }; /* :168 */
                    phx_cost_mv_x4_abs(&c, omx, omy, cr, costs);
                }
            for (; i < y_max; i += 2)                /* :169 */
            {
                if (omy+i <= mv_y_max) phx_cost_mv(&c, omx, omy+i); /* :171-172 */
                if (omy-i >= mv_y_min) phx_cost_mv(&c, omx, omy-i); /* :173-174 */
            }
        }

        /* me.c:525 — four diagonal probes around the updated best. */
        {
            const int off[4][2] = { {-2,-2}, {-2,2}, {2,-2}, {2,2} }; /* :525 */
            phx_cost_mv_x4_abs(&c, c.bmx, c.bmy, off, costs);
        }

        /* me.c:527-612 — hexagon grid, rings i = 1 .. i_me_range>>2. */
        omx = c.bmx; omy = c.bmy;                    /* me.c:528 */
        static const int8_t hex4[16][2] = {         /* me.c:534-539 */
            { 0,-4}, { 0, 4}, {-2,-3}, { 2,-3},
            {-4,-2}, { 4,-2}, {-4,-1}, { 4,-1},
            {-4, 0}, { 4, 0}, {-4, 1}, { 4, 1},
            {-4, 2}, { 4, 2}, {-2, 3}, { 2, 3},
        };
        for (int i = 1; i <= (mv_range >> 2); i++)   /* me.c:531/612 */
        {
            if (4*i > PHX_MIN4(mv_x_max-omx, omx-mv_x_min,
                               mv_y_max-omy, omy-mv_y_min)) /* me.c:541-542 */
            {
                for (int j = 0; j < 16; j++)         /* me.c:544 */
                {
                    int mx = omx + hex4[j][0]*i, my = omy + hex4[j][1]*i; /* :546-547 */
                    if (phx_check_mv_range(mx, my, mv_min, mv_max)) /* :548 */
                        phx_cost_mv(&c, mx, my);    /* me.c:549 */
                }
            }
            else
            {
                /* me.c:554-610, SADS/ADD_MVCOST/MIN_MV: the 16 candidates
                 * are evaluated in hex4[] order with COPY2_IF_LT (:566),
                 * so a strict "<" scan is bit-identical. */
                int dir = 0;                        /* me.c:554 */
                for (int j = 0; j < 16; j++)        /* MIN_MV :587-602 */
                {
                    int mx = omx + hex4[j][0]*i, my = omy + hex4[j][1]*i;
                    int cost = phx_pixel_sad_16x16(c.fenc, c.stride,
                                    c.fref + my*c.stride + mx, c.stride)
                             + phx_bits_mvd(&c, mx, my); /* ADD_MVCOST :565 */
                    if (cost < c.bcost) {           /* COPY2_IF_LT :566 */
                        c.bcost = cost;
                        dir = hex4[j][0]*16 + (hex4[j][1] & 15);
                    }
                }
                if (dir)                            /* me.c:606 */
                {
                    c.bmx = omx + i*(dir >> 4);     /* me.c:608 */
                    c.bmy = omy + i*((int32_t)((uint32_t)dir<<28)>>28); /* :609 */
                }
            }
        }
        if (c.bmy <= mv_y_max && c.bmy >= mv_y_min &&
            c.bmx <= mv_x_max && c.bmx >= mv_x_min) /* me.c:613 */
            phx_hex2_search(&c, mv_range, mv_min, mv_max, costs); /* :614 me_hex2 */

    phx_umh_end: ;                                   /* me.c:615 break */
    }

    /* x264 me.c:774-784 — qpel MV / cost output (subme < 3 branch). */
    int cost_mv = phx_bits_mvd(&c, c.bmx, c.bmy);    /* me.c:779 */
    int final_cost = c.bcost;                        /* me.c:780 */
    if (c.bmx == pmvx && c.bmy == pmvy)              /* me.c:782 bmv == pmv */
        final_cost += cost_mv;                       /* me.c:782 */
    out_mv[0] = c.bmx * 4;                           /* SPEL(bmx), me.c:179 */
    out_mv[1] = c.bmy * 4;                           /* SPEL(bmy), me.c:179 */
    return final_cost;                               /* x264_me_t.cost, me.h:54 */
}

/* ------------------------------------------------------------------------ */
/* Sub-pel interpolation — common/mc.c:173-249, tables.c:183-184.           */
/* ------------------------------------------------------------------------ */

/* common/mc.c:173 — TAPFILTER() verbatim (8-bit scalar build). */
#define PHX_TAPFILTER(pix,d) ((pix)[x-2*(d)] + (pix)[x+3*(d)] \
                            - 5*((pix)[x-(d)] + (pix)[x+2*(d)]) \
                            + 20*((pix)[x] + (pix)[x+(d)]))

/* common/mc.c:174-196 — hpel_filter(), BIT_DEPTH == 8 so pad == 0 (:177). */
static void phx_hpel_filter(uint8_t *dsth, uint8_t *dstv, uint8_t *dstc,
                            const uint8_t *src, intptr_t stride,
                            int width, int height, int16_t *buf)
{
    for (int y = 0; y < height; y++)                 /* mc.c:178 */
    {
        for (int x = -2; x < width+3; x++)           /* mc.c:180 */
        {
            int v = PHX_TAPFILTER(src, stride);      /* mc.c:182 */
            dstv[x] = PHX_CLIP_PIXEL((v + 16) >> 5); /* mc.c:183 */
            buf[x+2] = (int16_t)v;                   /* mc.c:185 (pad = 0) */
        }
        for (int x = 0; x < width; x++)              /* mc.c:187 */
            dstc[x] = PHX_CLIP_PIXEL((PHX_TAPFILTER(buf+2, 1) + 512) >> 10); /* :188 */
        for (int x = 0; x < width; x++)              /* mc.c:189 */
            dsth[x] = PHX_CLIP_PIXEL((PHX_TAPFILTER(src, 1) + 16) >> 5); /* :190 */
        dsth += stride; dstv += stride; dstc += stride; src += stride; /* :191-194 */
    }
}

/* common/tables.c:183-184. */
static const uint8_t phx_hpel_ref0[16] = {0,1,1,1,0,1,1,1,2,3,3,3,0,1,1,1};
static const uint8_t phx_hpel_ref1[16] = {0,0,1,0,2,2,3,2,2,2,3,2,2,2,3,2};

/* common/mc.c:49-61 pixel_avg() (weight 32 -> (a+b+1)>>1). */
static void phx_pixel_avg(uint8_t *dst, intptr_t dstride,
                          const uint8_t *s1, intptr_t s1stride,
                          const uint8_t *s2, intptr_t s2stride,
                          int width, int height)
{
    for (int y = 0; y < height; y++)                 /* mc.c:53 */
    {
        for (int x = 0; x < width; x++)              /* mc.c:55 */
            dst[x] = (uint8_t)((s1[x] + s2[x] + 1) >> 1); /* mc.c:56 */
        dst += dstride; s1 += s1stride; s2 += s2stride; /* mc.c:57-59 */
    }
}

/* common/mc.c:162-171 mc_copy(). */
static void phx_mc_copy(const uint8_t *src, intptr_t sstride,
                        uint8_t *dst, intptr_t dstride,
                        int width, int height)
{
    for (int y = 0; y < height; y++)                 /* mc.c:164 */
    {
        memcpy(dst, src, (size_t)width);             /* mc.c:166 (SIZEOF_PIXEL=1) */
        src += sstride; dst += dstride;              /* mc.c:168-169 */
    }
}

int phx_me_refplanes_init(const phx_frame_t *f, phx_me_refplanes_t *out)
{
    if (!f || !f->plane || !out || f->i_width <= 0 || f->i_height <= 0)
        return -1;
    memset(out, 0, sizeof(*out));
    int stride = f->i_stride;
    size_t size = (size_t)stride * (f->i_height + 2*PHX_PADV);
    int16_t *buf = (int16_t*)malloc((size_t)(f->i_width + 64) * sizeof(int16_t));
    if (!buf) return -1;
    for (int i = 0; i < 3; i++)
    {
        out->owned[i] = (uint8_t*)malloc(size);
        if (!out->owned[i]) {                        /* mc.c:704-726 cleanup */
            for (int j = 0; j < i; j++) free(out->owned[j]);
            free(buf);
            memset(out, 0, sizeof(*out));
            return -1;
        }
        out->p[i+1] = out->owned[i] + PHX_PADV*stride + PHX_PADH;
    }
    out->p[0] = f->plane;                            /* mc.c:719 filtered[0] */
    out->i_stride = stride;

    /* common/mc.c:704-726 — progressive full-frame filter (b_end=1):
     * start = mb_y*16-8 = -8, height = i_lines[0]+16, offs = start*stride-8. */
    int start = -8;                                  /* mc.c:707 */
    int height = f->i_height + 16;                   /* mc.c:708 */
    int offs = start*stride - 8;                     /* mc.c:717 */
    phx_hpel_filter(out->p[1]+offs, out->p[2]+offs, out->p[3]+offs,
                    f->plane+offs, stride, f->i_width + 16, height - start,
                    buf);                            /* mc.c:720-726 */
    free(buf);
    return 0;
}

void phx_me_refplanes_free(phx_me_refplanes_t *out)
{
    if (!out) return;
    for (int i = 0; i < 3; i++)
    {
        free(out->owned[i]);
        out->owned[i] = NULL;
        out->p[i+1] = NULL;
    }
    out->p[0] = NULL;
}

/* common/mc.c:221-249 get_ref(): return the zero-copy source pointer used by
 * the pure-hpel diamond; NULL means qpel interpolation is required. */
static const uint8_t *phx_get_ref(const phx_me_refplanes_t *ref,
                                  int mvx, int mvy, intptr_t *out_stride)
{
    int qpel_idx = ((mvy & 3) << 2) + (mvx & 3);    /* mc.c:226 */
    int offset = (mvy >> 2)*ref->i_stride + (mvx >> 2); /* mc.c:227 */
    const uint8_t *src1 = ref->p[phx_hpel_ref0[qpel_idx]] + offset
                        + ((mvy & 3) == 3) * ref->i_stride; /* mc.c:228 */
    if (qpel_idx & 5) return NULL;                   /* mc.c:230 */
    *out_stride = ref->i_stride;                     /* mc.c:246 */
    return src1;                                     /* mc.c:247 */
}

void phx_mc_luma16(const phx_me_refplanes_t *ref, int mvx, int mvy,
                   uint8_t *dst, intptr_t dstride, int width, int height)
{
    if (!ref || !ref->p[0] || !dst || !ref->p[1]) return;
    int qpel_idx = ((mvy & 3) << 2) + (mvx & 3);    /* mc.c:203/226 */
    int offset = (mvy >> 2)*ref->i_stride + (mvx >> 2); /* mc.c:204/227 */
    const uint8_t *src1 = ref->p[phx_hpel_ref0[qpel_idx]] + offset
                        + ((mvy & 3) == 3) * ref->i_stride; /* mc.c:205/228 */
    if (qpel_idx & 5)                                /* mc.c:207/230 */
    {
        const uint8_t *src2 = ref->p[phx_hpel_ref1[qpel_idx]] + offset
                            + ((mvx & 3) == 3);      /* mc.c:209/232 */
        phx_pixel_avg(dst, dstride, src1, ref->i_stride,
                      src2, ref->i_stride, width, height); /* mc.c:210-211/233-234 */
    }
    else
        phx_mc_copy(src1, ref->i_stride, dst, dstride,
                    width, height);                  /* mc.c:218/247 */
}

/* ------------------------------------------------------------------------ */
/* Sub-pel refinement — encoder/me.c:801-992.                               */
/* ------------------------------------------------------------------------ */

/* subpel_iterations[][4], me.c:38-50: {refine_hpel, refine_qpel,
 * me_hpel, me_qpel}.  refine_subpel() uses columns [0] and [1] via
 * x264_me_refine_qpel() (me.c:803-804); columns [2]/[3] are used by the
 * call from x264_me_search_ref() (me.c:794-795). */
static const uint8_t phx_subpel_iterations[12][4] = {
    {0,0,0,0}, {1,1,0,0}, {0,1,1,0}, {0,2,1,0},
    {0,2,1,1}, {0,2,1,2}, {0,0,2,2}, {0,0,2,2},
    {0,0,4,10}, {0,0,4,10}, {0,0,4,10}, {0,0,4,10}
};

typedef struct {
    const uint8_t *fenc;                 /* me.c:821 p_fenc[0] */
    intptr_t fenc_stride;                /* phx_frame_t stride (x264 FENC_STRIDE=16) */
    const phx_me_refplanes_t *ref;       /* me.c:830 m->p_fref */
    const uint16_t *p_cost_mv;           /* me.c:869-870 */
    int lambda, lim, mvp_x, mvp_y;
    /* refplanes[] share the frame-plane origin, while refine_subpel()'s MV
     * coordinates are relative to the current MB (me.c:192/830/869). */
    int ox, oy;
    int bmx, bmy, bcost, bdir, odir;
    int b_refine_qpel;                   /* me.c:827/925 */
    int subme;
    int min_s[2], max_s[2];              /* me.c:891-892/952 spel limits */
} phx_sub_ctx_t;

/* me.c:869-870 + analyse.c:155-156: p_cost_mvx[mx] = table[mx - mvp_x]. */
static int phx_bits_qpel(const phx_sub_ctx_t *c, int mx, int my)
{
    int dx = mx - c->mvp_x, dy = my - c->mvp_y;
    int cx = (c->p_cost_mv && dx >= -c->lim && dx <= c->lim)
           ? c->p_cost_mv[dx] : phx_mv_cost_formula(c->lambda, dx);
    int cy = (c->p_cost_mv && dy >= -c->lim && dy <= c->lim)
           ? c->p_cost_mv[dy] : phx_mv_cost_formula(c->lambda, dy);
    return cx + cy;
}

/* me.c:817-824 COST_MV_SAD(): get_ref(), SAD + MV bits, COPY3_IF_LT. */
static void phx_cost_mv_sad(phx_sub_ctx_t *c, int mx, int my,
                            uint8_t *pix, intptr_t pstride)
{
    phx_mc_luma16(c->ref, mx+c->ox, my+c->oy, pix, pstride, 16, 17); /* :820 */
    int cost = phx_pixel_sad_16x16(c->fenc, c->fenc_stride, pix, pstride) /* :821 */
             + phx_bits_qpel(c, mx, my);                   /* :822 */
    if (cost < c->bcost) { c->bcost = cost; c->bmx = mx; c->bmy = my; } /* :823 */
}

/* me.c:826-863 COST_MV_SATD().  b_chroma_me == 0 (chroma ME out of scope). */
static void phx_cost_mv_satd(phx_sub_ctx_t *c, int mx, int my, int dir,
                             uint8_t *pix, intptr_t pstride)
{
    if (c->b_refine_qpel || (dir ^ 1) != c->odir)     /* me.c:827 */
    {
        phx_mc_luma16(c->ref, mx+c->ox, my+c->oy, pix, pstride, 16, 16); /* :830 */
        int cost = phx_pixel_satd_16x16(c->fenc, c->fenc_stride, pix, pstride) /* :831 */
                 + phx_bits_qpel(c, mx, my);                   /* :832 */
        if (cost < c->bcost) {                          /* COPY4_IF_LT :862 */
            c->bcost = cost; c->bmx = mx; c->bmy = my; c->bdir = dir;
        }
    }
}

int phx_me_refine_subpel(int mb_x, int mb_y, int mb_width, int mb_height,
                         const phx_frame_t *fenc,
                         const phx_me_refplanes_t *fref,
                         const phx_cost_tables_t *cost, int i_mv_range,
                         int lambda, int subme, int b_refine_qpel,
                         const int16_t mvp[2],
                         int mv[2], int *cost_out, int *cost_mv_out)
{
    if (!fenc || !fenc->plane || !fref || !fref->p[0] || !fref->p[1] ||
        !mv || !cost_out || subme < 0 || subme > 11 || i_mv_range < 0)
        return -1;
    if (mb_x < 0 || mb_y < 0 || mb_x >= mb_width || mb_y >= mb_height)
        return -1;

    phx_sub_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.fenc = fenc->plane + mb_y*16*fenc->i_stride + mb_x*16; /* me.c:821 */
    c.fenc_stride = fenc->i_stride;
    c.ref = fref;
    c.p_cost_mv = phx_resolve_cost_mv(cost, lambda);        /* me.c:869 */
    c.lambda = lambda;
    c.lim = 8 * i_mv_range;                                  /* analyse.c:151 */
    c.mvp_x = mvp ? mvp[0] : 0;                              /* me.c:870 */
    c.mvp_y = mvp ? mvp[1] : 0;
    c.ox = mb_x * 16 * 4; c.oy = mb_y * 16 * 4;              /* block origin */
    c.bmx = mv[0]; c.bmy = mv[1]; c.bcost = *cost_out;       /* me.c:880-882 */
    c.odir = -1; c.bdir = -1;                                /* me.c:883 */
    c.b_refine_qpel = b_refine_qpel ? 1 : 0;
    c.subme = subme;

    /* me.c:330-339/391-396 — quarter-pel (spel) MV limits. */
    int i_fmv_range = 4 * i_mv_range;                        /* analyse.c:330 */
    c.min_s[0] = PHX_MAX(4*(-16*mb_x - 24), -i_fmv_range);   /* :336/:338 */
    c.max_s[0] = PHX_MIN(4*(16*(mb_width - mb_x - 1) + 24), i_fmv_range-1); /* :337/:339 */
    c.min_s[1] = PHX_MAX(4*(-16*mb_y - 24), -i_fmv_range);   /* :391/:393 */
    c.max_s[1] = PHX_MIN3(4*(16*(mb_height - mb_y - 1) + 24), i_fmv_range-1,
                          4*i_fmv_range);                    /* :392/:394 */

    uint8_t pix[64*18];                                 /* me.c:877 (16*17x2) */
    uint8_t satd_pix[16*17];                            /* me.c:830 stride=16 */
    (void)satd_pix;

    int hpel = phx_subpel_iterations[subme][b_refine_qpel ? 0 : 2]; /* me.c:803 / 794 */
    int qpel = phx_subpel_iterations[subme][b_refine_qpel ? 1 : 3]; /* me.c:804 / 795 */

    /* me.c:885-923 — halfpel diamond. */
    if (hpel)
    {
        /* me.c:888-895 — try the subpel component of the predicted mv. */
        if (subme < 3)
        {
            int mx = PHX_CLIP3(c.mvp_x, c.min_s[0]+2, c.max_s[0]-2); /* :891 */
            int my = PHX_CLIP3(c.mvp_y, c.min_s[1]+2, c.max_s[1]-2); /* :892 */
            if ((mx-c.bmx) | (my-c.bmy))
                phx_cost_mv_sad(&c, mx, my, satd_pix, 16); /* :893-894 */
        }

        c.bcost <<= 6;                                   /* me.c:897 */
        for (int i = hpel; i > 0; i--)                   /* me.c:898 */
        {
            int omx = c.bmx, omy = c.bmy;                /* me.c:900 */
            intptr_t stride = 16;                        /* me.c:901 */
            /* me.c:903-906 — pure hpel candidates: get_ref() is zero-copy,
             * and src1/src3 are the next row/column of the same plane. */
            const uint8_t *src0 = phx_get_ref(fref, omx+c.ox, omy-2+c.oy, &stride);
            const uint8_t *src2 = phx_get_ref(fref, omx-2+c.ox, omy+c.oy, &stride);
            if (!src0 || !src2) return -1;               /* cannot happen for hpel */
            const uint8_t *src1 = src0 + stride;         /* me.c:905 */
            const uint8_t *src3 = src2 + 1;              /* me.c:906 */
            int costs[4];
            costs[0] = phx_pixel_sad_16x16(c.fenc, c.fenc_stride, src0, stride) /* :907 */
                     + phx_bits_qpel(&c, omx, omy-2);    /* :908 */
            costs[1] = phx_pixel_sad_16x16(c.fenc, c.fenc_stride, src1, stride)
                     + phx_bits_qpel(&c, omx, omy+2);    /* :909 */
            costs[2] = phx_pixel_sad_16x16(c.fenc, c.fenc_stride, src2, stride)
                     + phx_bits_qpel(&c, omx-2, omy);    /* :910 */
            costs[3] = phx_pixel_sad_16x16(c.fenc, c.fenc_stride, src3, stride)
                     + phx_bits_qpel(&c, omx+2, omy);    /* :911 */
            PHX_COPY1_IF_LT_TAG(c.bcost, (costs[0]<<6)+2,  2); /* :912 */
            PHX_COPY1_IF_LT_TAG(c.bcost, (costs[1]<<6)+6,  6); /* :913 */
            PHX_COPY1_IF_LT_TAG(c.bcost, (costs[2]<<6)+16, 16);/* :914 */
            PHX_COPY1_IF_LT_TAG(c.bcost, (costs[3]<<6)+48, 48);/* :915 */
            if (!(c.bcost & 63)) break;                  /* :916-917 */
            c.bmx -= (int32_t)((uint32_t)c.bcost << 26) >> 29; /* :918 */
            c.bmy -= (int32_t)((uint32_t)c.bcost << 29) >> 29; /* :919 */
            c.bcost &= ~63;                              /* :920 */
        }
        c.bcost >>= 6;                                   /* me.c:922 */
    }

    /* me.c:925-929 — C build always has mbcmp_unaligned != fpelcmp, so
     * b_refine_qpel == 0 re-seeds bcost with SATD (b_chroma_me == 0). */
    if (!b_refine_qpel)
    {
        c.bcost = PHX_COST_MAX;                          /* me.c:927 */
        phx_cost_mv_satd(&c, c.bmx, c.bmy, -1, satd_pix, 16); /* :928 */
    }

    /* me.c:931-944 — p_halfpel_thresh is NULL in this API; skipped. */

    /* me.c:946-963 — quarterpel diamond. */
    if (subme != 1)                                      /* me.c:947 */
    {
        c.bdir = -1;                                     /* me.c:949 */
        for (int i = qpel; i > 0; i--)                   /* me.c:950 */
        {
            if (c.bmy <= c.min_s[1] || c.bmy >= c.max_s[1] ||
                c.bmx <= c.min_s[0] || c.bmx >= c.max_s[0]) break; /* :952 */
            c.odir = c.bdir;                             /* me.c:954 */
            int omx = c.bmx, omy = c.bmy;                /* me.c:955 */
            phx_cost_mv_satd(&c, omx, omy-1, 0, satd_pix, 16); /* :956 */
            phx_cost_mv_satd(&c, omx, omy+1, 1, satd_pix, 16); /* :957 */
            phx_cost_mv_satd(&c, omx-1, omy, 2, satd_pix, 16); /* :958 */
            phx_cost_mv_satd(&c, omx+1, omy, 3, satd_pix, 16); /* :959 */
            if ((c.bmx == omx) & (c.bmy == omy)) break;  /* me.c:960 */
        }
    }
    else if (c.bmy > c.min_s[1] && c.bmy < c.max_s[1] &&
             c.bmx > c.min_s[0] && c.bmx < c.max_s[0])   /* me.c:965 */
    {
        /* me.c:967-973 — subme=1 SAD-only shortcut, four mc_luma buffers at
         * stride 64 (pix+0/16/32/48). */
        int omx = c.bmx, omy = c.bmy;                    /* me.c:967 */
        phx_mc_luma16(fref, omx+c.ox,   omy-1+c.oy, pix,      64, 16, 16); /* :969 */
        phx_mc_luma16(fref, omx+c.ox,   omy+1+c.oy, pix+16,   64, 16, 16); /* :970 */
        phx_mc_luma16(fref, omx-1+c.ox, omy+c.oy,   pix+32,   64, 16, 16); /* :971 */
        phx_mc_luma16(fref, omx+1+c.ox, omy+c.oy,   pix+48,   64, 16, 16); /* :972 */
        int costs[4];
        costs[0] = phx_pixel_sad_16x16(c.fenc, c.fenc_stride, pix,      64) /* :973 */
                 + phx_bits_qpel(&c, omx, omy-1);            /* :974 */
        costs[1] = phx_pixel_sad_16x16(c.fenc, c.fenc_stride, pix+16,   64)
                 + phx_bits_qpel(&c, omx, omy+1);            /* :975 */
        costs[2] = phx_pixel_sad_16x16(c.fenc, c.fenc_stride, pix+32,   64)
                 + phx_bits_qpel(&c, omx-1, omy);            /* :976 */
        costs[3] = phx_pixel_sad_16x16(c.fenc, c.fenc_stride, pix+48,   64)
                 + phx_bits_qpel(&c, omx+1, omy);            /* :977 */
        c.bcost <<= 4;                                   /* me.c:978 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[0]<<4)+1,  1); /* :979 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[1]<<4)+3,  3); /* :980 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[2]<<4)+4,  4); /* :981 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[3]<<4)+12, 12);/* :982 */
        c.bmx -= (int32_t)((uint32_t)c.bcost << 28) >> 30; /* :983 */
        c.bmy -= (int32_t)((uint32_t)c.bcost << 30) >> 30; /* :984 */
        c.bcost >>= 4;                                   /* me.c:985 */
    }

    *cost_out = c.bcost;                                 /* me.c:988 */
    mv[0] = c.bmx;                                       /* me.c:989 */
    mv[1] = c.bmy;                                       /* me.c:990 */
    *cost_mv_out = phx_bits_qpel(&c, c.bmx, c.bmy);      /* me.c:991 */
    return 0;
}
