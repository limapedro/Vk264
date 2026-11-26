/* Phoenix lowres slice-type frame/path costs -- serial SHADER C / pure SHADER A.
 *
 * This file is included by src/phx_lookahead.c because CMakeLists.txt is owned
 * by another worker.  TODO(owner): add src/phx_lowres.c to the `phx` target and
 * remove that include; then build this as its own translation unit.
 *
 * Every non-trivial block names the exact x264 source line it follows.  See
 * phx_lowres.h for the full line index.  No cost is tuned to force B frames.
 */
#include "phx_lowres.h"

#include "phx_cost.h"
#include "phx_predict.h"
#include "phx_tables.h"

#include <stdlib.h>
#include <string.h>

#define PHX_LR_MIN(a,b) ((a) < (b) ? (a) : (b))
#define PHX_LR_MAX(a,b) ((a) > (b) ? (a) : (b))
#define PHX_LR_CLIP3(a,v,b) ((v) < (a) ? (a) : ((v) > (b) ? (b) : (v)))

#define PHX_LR_LOOKAHEAD_MAX 250                   /* common/base.h:140 */
#define PHX_LR_COST_MAX     0x7fffffff             /* encoder/me.h:30-31 */
#define PHX_LR_PENALTY      4                      /* slicetype.c:542 */
#define PHX_LR_COST_MASK    ((1 << 14) - 1)        /* frame.h:111 */
#define PHX_LR_COST_SHIFT   14                     /* frame.h:112 */
#define PHX_LR_FDEC         32                     /* dct.h:16 */
#define PHX_LR_FENC         16                     /* dct.h:15 */
#define PHX_LR_MV_INVAL     0x7fff                 /* frame.c:481 */
#define PHX_LR_SPEL_LIMIT(n) (2*(n))               /* me.c:550 / mc.c:203 */
#define PHX_LR_LAMBDA       phx_lambda_tab[PHX_LOOKAHEAD_QP] /* tables.c:97 */
#define PHX_LR_FPEL(mv)     (((mv)+2)>>2)          /* me.c:178 */

/* common/tables.c:183-184 -- x264_hpel_ref0[16] / x264_hpel_ref1[16]. */
static const uint8_t phx_lr_hpel_ref0[16] = {0,1,1,1,0,1,1,1,2,3,3,3,0,1,1,1};
static const uint8_t phx_lr_hpel_ref1[16] = {0,0,1,0,2,2,3,2,2,2,3,2,2,2,3,2};

/* encoder/me.c:55-56 -- radius-2 hexagon and square1 refine pattern. */
static const int8_t phx_lr_hex2[6][2] = {
    {-1,-2}, {-2,0}, {-1,2}, {1,2}, {2,0}, {1,-2}
};
static const int8_t phx_lr_square1[9][2] = {
    {0,0}, {0,-1}, {0,1}, {-1,0}, {1,0}, {-1,-1}, {-1,1}, {1,-1}, {1,1}
};
/* encoder/me.c:53 -- (x-1)%6. */
static const uint8_t phx_lr_mod6m1[8] = {5,0,1,2,3,4,5,0};

/* ---- per-frame side data ------------------------------------------------- */

/* frame.h:103-127.  One entry per borrowed phx_frame_t; x264 stores these in
 * the frame object itself, but phx_frame.h:35 exposes only one MV per list and
 * distance.  The public f->lowres_mvs[] slots are kept in sync in
 * lr_store_mv() below. */
typedef struct {
    phx_frame_t *f;
    int used;
    int16_t **mvs[2];       /* [list][dist] -> [mb_xy*2+xy], spel units */
    int **mv_costs[2];      /* [list][dist] -> [mb_xy] */
    uint16_t *lowres_costs; /* [(bframes+2)^2][mb_xy], frame.h:107 */
    int16_t *intra_cost;    /* [mb_xy], frame.h:111 (aliases lowres_costs[0][0]) */
    int b_intra_calculated; /* frame.h:126 */
} phx_lr_frame_t;

struct phx_lowres {
    int mb_width, mb_height, mb_count;
    int mv_range, bframes, b_pyramid, bias, b_mb_tree;
    phx_cost_tables_t tables;               /* analyse.c:179 */
    const uint16_t *cost_mv;                /* analyse.c:250 */
    phx_lr_frame_t *frames;
    int frames_size;
};

/* Lazily created context for the standalone phx_slicetype_frame_cost() API. */
static phx_lowres_t *phx_lr_default_ctx;

static void lr_frame_clear(phx_lr_frame_t *c, int ndist, int mb_count)
{
    for (int list = 0; list < 2; list++)
        for (int dist = 0; dist < ndist; dist++) {
            int16_t *mv = c->mvs[list][dist];
            for (int i = 0; i < mb_count; i++) {
                mv[2*i]   = 0;                   /* frame.c:285: memset 0 */
                mv[2*i+1] = 0;
                c->mv_costs[list][dist][i] = 0;
            }
            /* mc.c:481 -- only MB 0 of each list/distance is the "not searched
             * yet" sentinel read by slicetype.c:855-856. */
            mv[0] = PHX_LR_MV_INVAL;
        }
    /* frame.c:288 — i_intra_cost is lowres_costs[0][0], initialised to -1. */
    for (int i = 0; i < mb_count; i++)
        c->intra_cost[i] = -1;
    c->b_intra_calculated = 0;
}

static phx_lr_frame_t *lr_get_frame(phx_lowres_t *l, phx_frame_t *f)
{
    if (!l || !f) return NULL;
    for (int i = 0; i < l->frames_size; i++)
        if (l->frames[i].used && l->frames[i].f == f)
            return &l->frames[i];
    for (int i = 0; i < l->frames_size; i++) {
        if (l->frames[i].used) continue;
        phx_lr_frame_t *c = &l->frames[i];
        memset(c, 0, sizeof(*c));
        int ndist = l->bframes + 1;           /* frame.c:211-220 */
        int ncost = (l->bframes + 2) * (l->bframes + 2);
        size_t mv_bytes = (size_t)2 * ndist * l->mb_count * 2 * sizeof(int16_t);
        size_t cost_bytes = (size_t)2 * ndist * l->mb_count * sizeof(int);
        int16_t *mvbase = calloc(1, mv_bytes);
        int *costbase = calloc(1, cost_bytes);
        uint16_t *lc = calloc((size_t)ncost * l->mb_count, sizeof(uint16_t));
        int16_t **mv_ptrs[2];
        int **cost_ptrs[2];
        for (int list = 0; list < 2; list++) {
            mv_ptrs[list] = calloc((size_t)ndist, sizeof(int16_t *));
            cost_ptrs[list] = calloc((size_t)ndist, sizeof(int *));
        }
        if (!mvbase || !costbase || !lc || !mv_ptrs[0] || !mv_ptrs[1] ||
            !cost_ptrs[0] || !cost_ptrs[1]) {
            for (int list = 0; list < 2; list++) {
                free(mv_ptrs[list]); free(cost_ptrs[list]);
            }
            free(mvbase); free(costbase); free(lc);
            return NULL;
        }
        c->mvs[0] = mv_ptrs[0]; c->mvs[1] = mv_ptrs[1];
        c->mv_costs[0] = cost_ptrs[0]; c->mv_costs[1] = cost_ptrs[1];
        size_t mv_off = 0, cost_off = 0;
        for (int list = 0; list < 2; list++)
            for (int dist = 0; dist < ndist; dist++) {
                c->mvs[list][dist] = mvbase + mv_off;
                mv_off += (size_t)l->mb_count * 2;
                c->mv_costs[list][dist] = costbase + cost_off;
                cost_off += (size_t)l->mb_count;
            }
        c->lowres_costs = lc;
        /* frame.c:287 — i_intra_cost aliases lowres_costs[0][0]. */
        c->intra_cost = (int16_t *)lc;
        c->f = f;
        c->used = 1;
        lr_frame_clear(c, ndist, l->mb_count);
        return c;
    }
    return NULL; /* capacity exhausted; caller treats as an uncached frame */
}

void phx_lowres_set_mbtree(phx_lowres_t *l, int b_mb_tree)
{
    if (l) l->b_mb_tree = !!b_mb_tree;
}

void phx_lowres_frame_reset(phx_lowres_t *l, phx_frame_t *f)
{
    phx_lr_frame_t *c = lr_get_frame(l, f);
    if (c) lr_frame_clear(c, l->bframes + 1, l->mb_count);
}

phx_lowres_t *phx_lowres_new(int mb_width, int mb_height, int mv_range,
                             int bframes, int b_pyramid, int bframe_bias)
{
    if (!mb_width || !mb_height || mb_width > 2*PHX_LR_LOOKAHEAD_MAX ||
        mb_height > 2*PHX_LR_LOOKAHEAD_MAX || mv_range <= 0 || bframes < 0 ||
        bframes > PHX_BFRAME_MAX)
        return NULL;
    phx_lowres_t *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->mb_width = mb_width;
    l->mb_height = mb_height;
    l->mb_count = mb_width * mb_height;
    l->mv_range = mv_range;
    l->bframes = bframes;
    l->b_pyramid = !!b_pyramid;
    l->bias = bframe_bias;
    l->frames_size = PHX_LR_LOOKAHEAD_MAX + 3;
    l->frames = calloc((size_t)l->frames_size, sizeof(*l->frames));
    if (!l->frames) { free(l); return NULL; }

    /* lowres_context_init(): analyse.c:179 builds the QP-12 MV cost table;
     * analyse.c:248 wires a->p_cost_mv = cost_mv[X264_LOOKAHEAD_QP]. */
    if (phx_analyse_init_costs(&l->tables, mv_range, PHX_ME_HEX,
                               PHX_LOOKAHEAD_QP, PHX_LOOKAHEAD_QP)) {
        free(l->frames); free(l); return NULL;
    }
    l->cost_mv = l->tables.cost_mv[PHX_LOOKAHEAD_QP];
    return l;
}

void phx_lowres_delete(phx_lowres_t *l)
{
    if (!l) return;
    for (int i = 0; i < l->frames_size; i++) {
        phx_lr_frame_t *c = &l->frames[i];
        if (!c->used) continue;
        /* mvs[0][0] / mvs[1][0] and mv_costs[0][0] / [1][0] point into the
         * same two allocations; free each base exactly once. */
        if (c->mvs[0]) {
            free(c->mvs[0][0]);
            free(c->mvs[0]);
        }
        if (c->mvs[1]) free(c->mvs[1]);
        if (c->mv_costs[0]) {
            free(c->mv_costs[0][0]);
            free(c->mv_costs[0]);
        }
        if (c->mv_costs[1]) free(c->mv_costs[1]);
        free(c->lowres_costs);
    }
    phx_analyse_free_costs(&l->tables, l->mv_range); /* analyse.c:204 */
    free(l->frames);
    free(l);
}

/* ---- pure kernels (SHADER A) --------------------------------------------- */

/* common/pixel.c:49-71 -- PIXEL_SAD_C(sad_8x8, 8, 8). */
static int lr_sad8(const uint8_t *a, int sa, const uint8_t *b, int sb)
{
    int sum = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            sum += a[x + y*sa] > b[x + y*sb]
                 ? a[x + y*sa] - b[x + y*sb]
                 : b[x + y*sb] - a[x + y*sa];
    return sum;
}

/* common/pixel.c:290-330 -- x264_pixel_satd_8x4() and PIXEL_SATD_C(8,8).
 * The packed sum2_t representation (pixel.c:234-258) is written out as two
 * plain 4x4 Hadamard planes; with 8-bit samples no 16-bit half can overflow,
 * so the low/high halves can be accumulated independently.  The final
 * correction term (sum>>16) is kept for exactness (pixel.c:308). */
static int lr_satd_8x4(const uint8_t *a, int sa, const uint8_t *b, int sb)
{
    int lo[4][4], hi[4][4];
    for (int i = 0; i < 4; i++, a += sa, b += sb) {
        int d[8];
        for (int x = 0; x < 8; x++) d[x] = (int)a[x] - (int)b[x];
        /* pixel.c:297-301 + HADAMARD4 (:242-251). */
        int s0 = d[0] + d[4], s1 = d[1] + d[5],
            s2 = d[2] + d[6], s3 = d[3] + d[7];
        int t0 = s0 + s1, t1 = s0 - s1, t2 = s2 + s3, t3 = s2 - s3;
        lo[i][0] = t0 + t2; lo[i][2] = t0 - t2;
        lo[i][1] = t1 + t3; lo[i][3] = t1 - t3;
        s0 = d[0] - d[4]; s1 = d[1] - d[5];
        s2 = d[2] - d[6]; s3 = d[3] - d[7];
        t0 = s0 + s1; t1 = s0 - s1; t2 = s2 + s3; t3 = s2 - s3;
        hi[i][0] = t0 + t2; hi[i][2] = t0 - t2;
        hi[i][1] = t1 + t3; hi[i][3] = t1 - t3;
    }
    int sum = 0;
    for (int c = 0; c < 4; c++) {              /* pixel.c:303-307 */
        int a0 = lo[0][c], a1 = lo[1][c], a2 = lo[2][c], a3 = lo[3][c];
        int t0 = a0 + a1, t1 = a0 - a1, t2 = a2 + a3, t3 = a2 - a3;
        int v[4] = { t0+t2, t0-t2, t1+t3, t1-t3 };
        a0 = hi[0][c]; a1 = hi[1][c]; a2 = hi[2][c]; a3 = hi[3][c];
        t0 = a0 + a1; t1 = a0 - a1; t2 = a2 + a3; t3 = a2 - a3;
        int w[4] = { t0+t2, t0-t2, t1+t3, t1-t3 };
        for (int k = 0; k < 4; k++) {
            sum += v[k] < 0 ? -v[k] : v[k];
            sum += w[k] < 0 ? -w[k] : w[k];
        }
    }
    return (sum + (sum >> 16)) >> 1;           /* pixel.c:308 */
}

/* pixel.c:311-330 -- satd_8x8 is two satd_8x4 halves. */
static int lr_satd8(const uint8_t *a, int sa, const uint8_t *b, int sb)
{
    return lr_satd_8x4(a, sa, b, sb)
         + lr_satd_8x4(a + 4*sa, sa, b + 4*sb, sb);
}

/* common/mc.c:221-249 -- get_ref(): full-pel copy, hpel selection and qpel
 * interpolation.  dest is always 8x8 with stride 16. */
static void lr_get_ref(uint8_t *dst, int dst_stride, uint8_t *const src[4],
                       int src_stride, int mvx, int mvy)
{
    int qpel_idx = ((mvy & 3) << 2) + (mvx & 3); /* mc.c:226 */
    int offset = (mvy >> 2)*src_stride + (mvx >> 2); /* mc.c:227 */
    uint8_t *s1 = src[phx_lr_hpel_ref0[qpel_idx]] + offset
                + ((mvy & 3) == 3) * src_stride;  /* :228 */
    if (qpel_idx & 5) {                       /* :230 */
        uint8_t *s2 = src[phx_lr_hpel_ref1[qpel_idx]] + offset
                    + ((mvx & 3) == 3);        /* :232 */
        for (int y = 0; y < 8; y++, dst += dst_stride, s1 += src_stride, s2 += src_stride)
            for (int x = 0; x < 8; x++)
                dst[x] = (uint8_t)((s1[x] + s2[x] + 1) >> 1); /* mc.c:70 */
    } else {
        for (int y = 0; y < 8; y++, dst += dst_stride, s1 += src_stride)
            memcpy(dst, s1, 8);               /* mc.c:162-171 */
    }
}

/* common/mc.c:77-99 + 588 -- pixel_avg for the two TRY_BIDIR sites. */
static void lr_bidir_avg(uint8_t *dst, const uint8_t *a, const uint8_t *b,
                         int weight)
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int i = y*PHX_LR_FENC + x;
            dst[i] = weight == 32
                   ? (uint8_t)((a[i] + b[i] + 1) >> 1)       /* mc.c:70 */
                   : (uint8_t)PHX_LR_CLIP3(0, (a[i]*weight + b[i]*(64-weight)
                                          + 32) >> 6, 255); /* mc.c:86 */
        }
}

static void lr_copy_fenc(uint8_t *dst, const phx_frame_t *f, int mb_x, int mb_y)
{
    const uint8_t *src = f->lowres[0] + 8*mb_x + 8*mb_y*f->i_stride_lowres;
    for (int y = 0; y < 8; y++)
        memcpy(dst + y*PHX_LR_FENC, src + y*f->i_stride_lowres, 8);
                                                  /* slicetype.c:544-545 */
}

/* ---- motion search state (SHADER A, one invocation per block/list) -------- */

typedef struct {
    const uint8_t *fenc;                       /* 8x8, stride PHX_LR_FENC */
    uint8_t *src[4];                           /* ref lowres planes */
    int stride;
    const uint16_t *cost_mv;                   /* analyse.c:250 */
    int mvp[2];                                /* spel, me.h:50 */
    int min_spel[2], max_spel[2];              /* slicetype.c:552-561 */
    int min_fpel[2], max_fpel[2];              /* slicetype.c:554-561 */
    int cost_range;                            /* 2*4*mv_range, analyse.c:151 */
} phx_lr_me_t;

static int lr_bits_full(const phx_lr_me_t *m, int mx, int my)
{
    int ix = PHX_LR_CLIP3(-m->cost_range, m->cost_range,
                         4*mx - m->mvp[0]);   /* me.c:60-61,211-212 */
    int iy = PHX_LR_CLIP3(-m->cost_range, m->cost_range,
                         4*my - m->mvp[1]);
    return m->cost_mv[ix] + m->cost_mv[iy];
}

static int lr_bits_spel(const phx_lr_me_t *m, int mx, int my)
{
    int ix = PHX_LR_CLIP3(-m->cost_range, m->cost_range, mx - m->mvp[0]);
    int iy = PHX_LR_CLIP3(-m->cost_range, m->cost_range, my - m->mvp[1]);
    return m->cost_mv[ix] + m->cost_mv[iy];
}

/* me.c:63-70 -- COST_MV with fpelcmp == SAD_8x8 (encoder.c:1423-1425:
 * fpelcmp is SATD only for TESA; lowres_context_init caps the method at HEX). */
static int lr_eval_full(const phx_lr_me_t *m, int mx, int my)
{
    return lr_sad8(m->fenc, PHX_LR_FENC,
                   m->src[0] + my*m->stride + mx, m->stride)
         + lr_bits_full(m, mx, my);
}

/* me.c:72-79,817-824 -- COST_MV_SAD through mc.get_ref(). */
static int lr_eval_spel_sad(const phx_lr_me_t *m, int mx, int my)
{
    uint8_t buf[8*PHX_LR_FENC];
    lr_get_ref(buf, PHX_LR_FENC, m->src, m->stride, mx, my);
    return lr_sad8(m->fenc, PHX_LR_FENC, buf, PHX_LR_FENC)
         + lr_bits_spel(m, mx, my);
}

/* me.c:826-833 -- COST_MV_SATD (mbcmp_unaligned == satd, encoder.c:1412-1413). */
static int lr_eval_spel_satd(const phx_lr_me_t *m, int mx, int my)
{
    uint8_t buf[8*PHX_LR_FENC];
    lr_get_ref(buf, PHX_LR_FENC, m->src, m->stride, mx, my);
    return lr_satd8(m->fenc, PHX_LR_FENC, buf, PHX_LR_FENC)
         + lr_bits_spel(m, mx, my);
}

static int lr_in_fpel_range(const phx_lr_me_t *m, int mx, int my)
{
    return mx >= m->min_fpel[0] && mx <= m->max_fpel[0] &&
           my >= m->min_fpel[1] && my <= m->max_fpel[1];
}

static void lr_init_me(phx_lr_me_t *m, const phx_lowres_t *l,
                       const uint8_t *fenc, const phx_frame_t *fref,
                       int mb_x, int mb_y, const int16_t mvp[2])
{
    m->fenc = fenc;
    m->stride = fref->i_stride_lowres;
    for (int i = 0; i < 4; i++)
        m->src[i] = fref->lowres[i]
                  + 8*mb_x + 8*mb_y*m->stride;  /* slicetype.c:564-572 */
    m->cost_mv = l->cost_mv;
    m->mvp[0] = mvp ? mvp[0] : 0;
    m->mvp[1] = mvp ? mvp[1] : 0;
    m->cost_range = 2*4*l->mv_range;              /* analyse.c:151 */

    /* slicetype.c:550-561.  mv_range there is 2*analyse.i_mv_range. */
    int spel_range = PHX_LR_SPEL_LIMIT(l->mv_range);
    m->min_spel[0] = PHX_LR_MAX(4*(-8*mb_x - 12), -spel_range);
    m->max_spel[0] = PHX_LR_MIN(4*(8*(l->mb_width - mb_x - 1) + 12),
                               spel_range - 1);
    m->min_spel[1] = PHX_LR_MAX(4*(-8*mb_y - 12), -spel_range);
    m->max_spel[1] = PHX_LR_MIN(4*(8*(l->mb_height - mb_y - 1) + 12),
                               spel_range - 1);
    m->min_fpel[0] = m->min_spel[0] >> 2;
    m->max_fpel[0] = m->max_spel[0] >> 2;
    m->min_fpel[1] = m->min_spel[1] >> 2;
    m->max_fpel[1] = m->max_spel[1] >> 2;
}

/* encoder/me.c:218-275 -- predictor screening for subpel_refine >= 3, plus
 * x264_predictor_clip() (me.c:231).  `mvc[i]` are spel candidates from the
 * reverse-order cache walk in slicetype.c:662-680. */
static void lr_check_predictors(const phx_lr_me_t *m, int16_t (*mvc)[2],
                                int i_mvc, int me_range,
                                int *best_mx, int *best_my, int *best_cost)
{
    *best_mx = PHX_LR_CLIP3(m->mvp[0], m->min_spel[0] + 2, m->max_spel[0] - 2);
    *best_my = PHX_LR_CLIP3(m->mvp[1], m->min_spel[1] + 2, m->max_spel[1] - 2);
    *best_cost = lr_eval_spel_sad(m, *best_mx, *best_my); /* me.c:225 */
    int pmv = (m->mvp[0] | m->mvp[1]) != 0;

    for (int i = 0; i < i_mvc; i++) {            /* me.c:241-249 */
        int mx = PHX_LR_CLIP3(mvc[i][0], m->min_spel[0], m->max_spel[0]);
        int my = PHX_LR_CLIP3(mvc[i][1], m->min_spel[1], m->max_spel[1]);
        if ((mx == 0 && my == 0) ||
            (mx == *best_mx && my == *best_my)) continue;
        int cost = lr_eval_spel_sad(m, mx, my);  /* me.c:244 */
        if (cost < *best_cost) {
            *best_cost = cost; *best_mx = mx; *best_my = my;
        }
        (void)me_range;
    }
    (void)pmv;
}

/* encoder/me.c:320-419 -- full-pel DIA/HEX search.  x264's packed-cost
 * X3/X4 macros are written out as plain candidate comparisons; the candidate
 * set and the stopping rule are identical. */
static void lr_fullpel_search(const phx_lr_me_t *m, int method, int me_range,
                              int *bmx, int *bmy, int *bcost)
{
    if (method == PHX_ME_DIA) {
        for (int iter = me_range; iter > 0; iter--) {
            int omx = *bmx, omy = *bmy;
            static const int dir[4][2] = {{0,-1},{0,1},{-1,0},{1,0}};
            for (int d = 0; d < 4; d++) {
                int mx = omx + dir[d][0], my = omy + dir[d][1];
                if (!lr_in_fpel_range(m, mx, my)) continue;
                int cost = lr_eval_full(m, mx, my);   /* me.c:146-150 */
                if (cost < *bcost) { *bcost = cost; *bmx = mx; *bmy = my; }
            }
            if (*bmx == omx && *bmy == omy) break;    /* me.c:334-335 */
        }
        return;
    }
    /* HEX: six radius-2 candidates, then a 3x3 square refine. */
    int dir = -1;
    for (int iter = (me_range >> 1); iter > 0 && dir != 0; iter--) {
        int omx = *bmx, omy = *bmy;
        for (int d = 0; d < 6; d++) {
            int mx = omx + phx_lr_hex2[d][0];
            int my = omy + phx_lr_hex2[d][1];
            if (!lr_in_fpel_range(m, mx, my)) continue;
            int cost = lr_eval_full(m, mx, my);       /* me.c:367-368 */
            if (cost < *bcost) {
                *bcost = cost; *bmx = mx; *bmy = my; dir = d;
            }
        }
        if (dir < 0 || (*bmx == omx && *bmy == omy)) break;
        dir = phx_lr_mod6m1[dir + 1];                /* me.c:397 */
    }
    for (int d = 1; d < 9; d++) {                    /* me.c:405-418 */
        int mx = *bmx + phx_lr_square1[d][0];
        int my = *bmy + phx_lr_square1[d][1];
        if (!lr_in_fpel_range(m, mx, my)) continue;
        int cost = lr_eval_full(m, mx, my);
        if (cost < *bcost) { *bcost = cost; *bmx = mx; *bmy = my; }
    }
}

/* encoder/me.c:865-963 (subpel_iterations[4] = {0,2,1,1}, me.c:38-50):
 * hpel_iters == 0, so only the SATD re-evaluation at the full/hpel winner and
 * two qpel diamond iterations run.  b_refine_qpel == 0, hence the direction
 * opposite to the previous step is skipped (me.c:827). */
static void lr_refine_subpel(const phx_lr_me_t *m, int *bmx, int *bmy,
                             int *bcost)
{
    *bcost = PHX_LR_COST_MAX;
    int bdir = -1;
    *bmx *= 4;                                        /* me.c:178-179 SPEL */
    *bmy *= 4;
    int start_cost = lr_eval_spel_satd(m, *bmx, *bmy); /* me.c:928 */
    *bcost = start_cost;
    for (int iter = 0; iter < 2; iter++) {
        int odir = bdir, omx = *bmx, omy = *bmy;
        static const int dir[4][2] = {{0,-1},{0,1},{-1,0},{1,0}};
        for (int d = 0; d < 4; d++) {
            if (odir >= 0 && (d ^ 1) == odir) continue; /* me.c:827 */
            int mx = omx + dir[d][0], my = omy + dir[d][1];
            if (mx <= m->min_spel[0] || mx >= m->max_spel[0] ||
                my <= m->min_spel[1] || my >= m->max_spel[1]) continue; /* :952 */
            int cost = lr_eval_spel_satd(m, mx, my);
            if (cost < *bcost) {
                *bcost = cost; *bmx = mx; *bmy = my; bdir = d;
            }
        }
        if (*bmx == omx && *bmy == omy) break;        /* me.c:960-961 */
    }
}

/* slicetype.c:654-709 + me.c:214-796.  Returns MV in spel units and the
 * x264_me_t.cost convention (already adjusted by -p_cost_mv[0] and the
 * non-zero-MV 5*lambda penalty, me.c:695-697). */
static void lr_motion_search(const phx_lowres_t *l, const phx_lr_me_t *m,
                             const int16_t *fenc_mv, int mb_width, int mb_height,
                             int mb_x, int mb_y, int method, int me_range,
                             int *out_mv, int *out_cost)
{
    int16_t mvc[4][2];
    int i_mvc = 0;
    mvc[0][0] = mvc[0][1] = 0;
    mvc[2][0] = mvc[2][1] = 0;                        /* me.c:663-664 */
#define MVC_AT(idx) do { mvc[i_mvc][0] = fenc_mv[2*(idx)]; \
                         mvc[i_mvc][1] = fenc_mv[2*(idx)+1]; i_mvc++; } while (0)
    if (mb_x < mb_width  - 1) MVC_AT(1);              /* me.c:666-667 */
    if (mb_y < mb_height - 1) {                       /* me.c:668-675 */
        MVC_AT(mb_width);
        if (mb_x > 0)             MVC_AT(mb_width-1);
        if (mb_x < mb_width - 1)  MVC_AT(mb_width+1);
    }
#undef MVC_AT
    int16_t mvp[2];
    if (i_mvc <= 1) {                                 /* me.c:677-678 */
        mvp[0] = mvc[0][0]; mvp[1] = mvc[0][1];
    } else {
        /* me.c:680 -- x264_median_mv(), componentwise. */
        for (int c = 0; c < 2; c++) {
            int a = mvc[0][c], b = mvc[1][c], d = mvc[2][c];
            mvp[c] = (int16_t)(a < b ? (b < d ? b : (a < d ? d : a))
                                     : (a < d ? a : (b < d ? d : b)));
        }
    }
    phx_lr_me_t sm = *m;
    sm.mvp[0] = mvp[0];
    sm.mvp[1] = mvp[1];

    int best_mx, best_my, best_cost;
    lr_check_predictors(&sm, mvc, i_mvc, me_range,
                        &best_mx, &best_my, &best_cost); /* me.c:218-250 */

    int bmx = PHX_LR_FPEL(best_mx), bmy = PHX_LR_FPEL(best_my);
    int bcost;
    if (best_mx & 3 || best_my & 3)
        bcost = lr_eval_full(&sm, bmx, bmy);          /* me.c:258-259 */
    else
        bcost = best_cost;                            /* me.c:260-261 */
    /* me.c:257/264 -- pmv is the *chosen* subpel predictor, not the raw
     * median of neighbours (me.c:219-220). */
    if (best_mx | best_my) {
        if (bmx | bmy) {
            int zcost = lr_eval_full(&sm, 0, 0);      /* me.c:266 */
            if (zcost < bcost) { bcost = zcost; bmx = 0; bmy = 0; }
        }
    } else if (best_cost < bcost) {                   /* me.c:271-274 */
        bcost = best_cost; bmx = 0; bmy = 0;
    }

    /* me.c:682-692 -- fast skip for near-zero residual at zero MVP. */
    int skipped = 0;
    if (mvp[0] == 0 && mvp[1] == 0) {
        int zero_cost = lr_sad8(sm.fenc, PHX_LR_FENC,
                                sm.src[0], sm.stride); /* me.c:686 */
        if (zero_cost < 64) {
            bmx = bmy = 0; bcost = zero_cost; skipped = 1;
        }
    }
    if (!skipped) {
        lr_fullpel_search(&sm, method, me_range, &bmx, &bmy, &bcost);
        bcost -= sm.cost_mv[PHX_LR_CLIP3(-sm.cost_range, sm.cost_range,
                                         -sm.mvp[0])]; /* me.c:695 */
        if (bmx | bmy) bcost += 5 * PHX_LR_LAMBDA;     /* me.c:696-697 */
        lr_refine_subpel(&sm, &bmx, &bmy, &bcost);     /* me.c:791-797 */
    }
    out_mv[0] = (int16_t)bmx;
    out_mv[1] = (int16_t)bmy;
    *out_cost = bcost;
    (void)l;
}

/* ---- TRY_BIDIR (slicetype.c:579-603,644-651,711-712) --------------------- */

static void lr_try_bidir(const uint8_t *fenc, uint8_t *src0[4], uint8_t *src1[4],
                         int stride, const int mv0[2], const int mv1[2],
                         int penalty, int weight, int *bcost, int *list_used)
{
    uint8_t a[8*PHX_LR_FENC], b[8*PHX_LR_FENC], avg[8*PHX_LR_FENC];
    lr_get_ref(a, PHX_LR_FENC, src0, stride, mv0[0], mv0[1]);
    lr_get_ref(b, PHX_LR_FENC, src1, stride, mv1[0], mv1[1]);
    lr_bidir_avg(avg, a, b, weight);
    int cost = penalty * PHX_LR_LAMBDA
             + lr_sad8(fenc, PHX_LR_FENC, avg, PHX_LR_FENC); /* :600-601 */
    if (cost < *bcost) { *bcost = cost; *list_used = 3; }    /* :602 */
}

/* ---- one lowres 8x8 block (slicetype.c:514-791, SHADER A) ---------------- */

static void lr_mb_cost(phx_lowres_t *l, phx_frame_t **frames,
                       int p0, int p1, int b, int dist_scale_factor,
                       const int do_search[2], int bipred_weight,
                       int mb_x, int mb_y,
                       int *out_inter, int *out_intra_mbs, int *out_intra)
{
    const int i_mb_xy = mb_x + mb_y*l->mb_width;
    phx_frame_t *fenc = frames[b];
    phx_frame_t *fref0 = frames[p0];
    phx_frame_t *fref1 = frames[p1];
    phx_lr_frame_t *c = lr_get_frame(l, fenc);
    const int b_bidir = (b < p1);                     /* slicetype.c:522 */
    const int b_frame_score = (i_mb_xy >= 0 &&        /* :532-534 */
        ((mb_x > 0 && mb_x < l->mb_width  - 1 &&
          mb_y > 0 && mb_y < l->mb_height - 1) ||
         l->mb_width <= 2 || l->mb_height <= 2));

    uint8_t fenc_buf[8*PHX_LR_FENC];
    lr_copy_fenc(fenc_buf, fenc, mb_x, mb_y);        /* :544-545 */

    int i_bcost = PHX_LR_COST_MAX, list_used = 0;
    int mv[2][2] = {{0,0},{0,0}};
    int mvcost[2] = {0,0};

    if (p0 != p1) {
        int dist[2] = { b - p0 - 1, p1 - b - 1 };     /* slicetype.c:530-531 */
        uint8_t *src0[4], *src1[4];
        phx_lr_me_t me[2];
        int16_t *fmv[2];

        for (int list = 0; list <= b_bidir; list++) {
            const phx_frame_t *ref = list ? fref1 : fref0;
            int d = dist[list];
            for (int i = 0; i < 4; i++)
                (list ? src1 : src0)[i] = ref->lowres[i]
                    + 8*mb_x + 8*mb_y*ref->i_stride_lowres; /* :611/626 */
            fmv[list] = (c && d >= 0 && d <= l->bframes)
                      ? c->mvs[list][d] + 2*i_mb_xy : NULL;
            int16_t mvp[2] = {0,0};
            if (fmv[list] && fmv[list][0] != PHX_LR_MV_INVAL) {
                mvp[0] = fmv[list][0]; mvp[1] = fmv[list][1];
            }
            lr_init_me(&me[list], l, fenc_buf, ref, mb_x, mb_y, mvp);
        }

        if (b_bidir) {
            /* slicetype.c:629-651 -- scaled dmv from the p0->p1 lowres MVs. */
            int dmv[2][2] = {{0,0},{0,0}};
            phx_lr_frame_t *rc = lr_get_frame(l, fref1);
            int dref = p1 - p0 - 1;
            if (rc && dref >= 0 && dref <= l->bframes &&
                rc->mvs[0][dref][2*i_mb_xy] != PHX_LR_MV_INVAL) {
                int16_t *mvr = rc->mvs[0][dref] + 2*i_mb_xy;
                dmv[0][0] = (mvr[0]*dist_scale_factor + 128) >> 8; /* :632 */
                dmv[0][1] = (mvr[1]*dist_scale_factor + 128) >> 8;
                dmv[1][0] = dmv[0][0] - mvr[0];             /* :634 */
                dmv[1][1] = dmv[0][1] - mvr[1];             /* :635 */
                for (int k = 0; k < 2; k++)
                    for (int q = 0; q < 2; q++)
                        dmv[k][q] = PHX_LR_CLIP3(dmv[k][q],
                                     me[0].min_spel[q], me[0].max_spel[q]);
                                                          /* :636-637 */
            }
            lr_try_bidir(fenc_buf, src0, src1, me[0].stride,
                         dmv[0], dmv[1], 0, bipred_weight,
                         &i_bcost, &list_used);          /* :644 */
            if (dmv[0][0] | dmv[0][1] | dmv[1][0] | dmv[1][1])
                lr_try_bidir(fenc_buf, src0, src1, me[0].stride,
                             (const int[2]){0,0}, (const int[2]){0,0},
                             0, bipred_weight, &i_bcost, &list_used);
                                                          /* :645-651 */
        }

        for (int list = 0; list <= b_bidir; list++) {
            int d = dist[list];
            int cached = !do_search[list];
            if (cached && c) {
                mv[list][0] = c->mvs[list][d][2*i_mb_xy];
                mv[list][1] = c->mvs[list][d][2*i_mb_xy+1];
                mvcost[list] = c->mv_costs[list][d][i_mb_xy]; /* :703-707 */
            } else {
                int out[2];
                lr_motion_search(l, &me[list], fmv[list], l->mb_width,
                                 l->mb_height, mb_x, mb_y,
                                 PHX_ME_HEX, 16, out, /* lowres_context_init:
                                 x264 keeps param.analyse.i_me_range (base.c:443=16) */
                                 &mvcost[list]);
                mv[list][0] = out[0]; mv[list][1] = out[1];
                if (c) {                                  /* :700-701 */
                    c->mvs[list][d][2*i_mb_xy]   = (int16_t)out[0];
                    c->mvs[list][d][2*i_mb_xy+1] = (int16_t)out[1];
                    c->mv_costs[list][d][i_mb_xy] = mvcost[list];
                    /* Public read-only view (phx_frame.h:35). */
                    fenc->lowres_mvs[list][PHX_LR_MIN(d, PHX_BFRAME_MAX)][0]
                        = (int16_t)out[0];
                    fenc->lowres_mvs[list][PHX_LR_MIN(d, PHX_BFRAME_MAX)][1]
                        = (int16_t)out[1];
                }
            }
            if (mvcost[list] < i_bcost) {                 /* :708 */
                i_bcost = mvcost[list];
                list_used = list + 1;
            }
        }
        if (b_bidir && (mv[0][0] | mv[0][1] | mv[1][0] | mv[1][1]))
            lr_try_bidir(fenc_buf, src0, src1, me[0].stride,
                         mv[0], mv[1], 5, bipred_weight,
                         &i_bcost, &list_used);            /* :711-712 */
    }

    /* slicetype.c:714-757 -- intra cost (computed once per frame). */
    int i_icost;
    if (c && c->b_intra_calculated) {
        i_icost = c->intra_cost[i_mb_xy];                 /* :764 */
    } else {
        /* :717-727 -- 16-byte top row and 4-byte left column of neighbours. */
        uint8_t pix[9*PHX_LR_FDEC + 16];
        memset(pix, 0, sizeof(pix));
        uint8_t *p = pix + 8 + PHX_LR_FDEC;
        const uint8_t *src = fenc->lowres[0] + 8*mb_x
                           + 8*mb_y*fenc->i_stride_lowres;
        memcpy(pix + 8, src - fenc->i_stride_lowres, 16);
        for (int i = -1; i < 8; i++)
            memcpy(p + i*PHX_LR_FDEC - 4,              /* :727 */
                   src + i*fenc->i_stride_lowres - 4, 4);
        int best = PHX_LR_COST_MAX;
        /* intra_mbcmp_x3_8x8c: DC, H, V (predict.c:221/260/270). */
        phx_predict_8x8c_dc(p, PHX_LR_FDEC);
        best = PHX_LR_MIN(best, lr_satd8(p, PHX_LR_FDEC,
                                         fenc_buf, PHX_LR_FENC));
        phx_predict_8x8c_h(p, PHX_LR_FDEC);
        best = PHX_LR_MIN(best, lr_satd8(p, PHX_LR_FDEC,
                                         fenc_buf, PHX_LR_FENC));
        phx_predict_8x8c_v(p, PHX_LR_FDEC);
        best = PHX_LR_MIN(best, lr_satd8(p, PHX_LR_FDEC,
                                         fenc_buf, PHX_LR_FENC));
        /* :734-743 -- P mode and the six filtered 8x8 luma modes. */
        phx_predict_8x8c_p(p, PHX_LR_FDEC);
        best = PHX_LR_MIN(best, lr_satd8(p, PHX_LR_FDEC,
                                         fenc_buf, PHX_LR_FENC));
        uint8_t edge[36];
        phx_predict_8x8_filter(p, edge, PHX_LR_FDEC,
                               PHX_MB_ALL, PHX_MB_ALL);
        typedef void (*pred8_t)(uint8_t *, const uint8_t *, int);
        static const pred8_t pred8[6] = {
            phx_predict_8x8_ddl, phx_predict_8x8_ddr, phx_predict_8x8_vr,
            phx_predict_8x8_hd,  phx_predict_8x8_vl,  phx_predict_8x8_hu
        };
        for (int i = 0; i < 6; i++) {
            pred8[i](p, edge, PHX_LR_FDEC);
            best = PHX_LR_MIN(best, lr_satd8(p, PHX_LR_FDEC,
                                             fenc_buf, PHX_LR_FENC));
        }
        i_icost = ((best + 5*PHX_LR_LAMBDA) >> 0) + PHX_LR_PENALTY; /* :746 */
        if (c) c->intra_cost[i_mb_xy] = (int16_t)i_icost;          /* :747 */
        if (b_frame_score && out_intra) *out_intra += i_icost;     /* :754 */
    }

    i_bcost += PHX_LR_PENALTY;                            /* :758 */

    if (!b_bidir) {                                       /* :762-773 */
        if (i_icost < i_bcost) {
            i_bcost = i_icost;
            list_used = 0;
            if (out_intra_mbs) (*out_intra_mbs)++;        /* :772 */
        }
    }
    if (p0 != p1 && b_frame_score && out_inter)           /* :776-787 */
        *out_inter += i_bcost;
    if (c && c->lowres_costs) {
        /* :790.  Cost is clamped to 14 bits with list_used in the high bits. */
        int row = (b - p0)*(l->bframes + 2) + (p1 - b);
        c->lowres_costs[row*l->mb_count + i_mb_xy] =
            (uint16_t)((PHX_LR_MIN(i_bcost, PHX_LR_COST_MASK))
                       + (list_used << PHX_LR_COST_SHIFT));
    }
}

/* ---- frame cost (slicetype.c:836-995, SHADER C) -------------------------- */

int phx_lowres_frame_cost(phx_lowres_t *l, phx_frame_t **frames,
                          int p0, int p1, int b)
{
    if (!l || !frames || !frames[b]) return PHX_LR_COST_MAX;
    int d0 = b - p0, d1 = p1 - b;
    if (d0 < 0 || d0 > PHX_BFRAME_MAX+1 ||
        d1 < 0 || d1 > PHX_BFRAME_MAX+1) return PHX_LR_COST_MAX;
    if (!frames[p0] || !frames[p1]) return PHX_LR_COST_MAX;
    phx_frame_t *fenc = frames[b];
    if (fenc->i_cost_est[d0][d1] >= 0) {                 /* :848-849 */
        return fenc->i_cost_est[d0][d1];
    }

    phx_lr_frame_t *c = lr_get_frame(l, fenc);
    /* TODO: grow the side-data pool when more than PHX_LOOKAHEAD_MAX+3 frames
     * are borrowed; until then an uncached frame cannot be searched. */
    if (!c) return PHX_LR_COST_MAX;
    int first_intra = !c->b_intra_calculated;             /* :949 */
    int dist_scale_factor = 128;                          /* :852 */
    int do_search[2] = {0,0};
    if (b != p0 && d0-1 >= 0 && d0-1 <= l->bframes &&
        c->mvs[0][d0-1][0] == PHX_LR_MV_INVAL) {
        do_search[0] = 1;                                 /* :855 */
        c->mvs[0][d0-1][0] = 0;                           /* :865 */
    }
    if (b != p1 && d1-1 >= 0 && d1-1 <= l->bframes &&
        c->mvs[1][d1-1][0] == PHX_LR_MV_INVAL) {
        do_search[1] = 1;                                 /* :856 */
        c->mvs[1][d1-1][0] = 0;                           /* :867 */
    }
    if (p1 != p0)
        dist_scale_factor = (((b-p0) << 8) + ((p1-p0) >> 1)) / (p1-p0);
                                                          /* :870 */
    /* slicetype.c:823-831: edges are excluded unless mb-tree/VBV is active
     * or the frame is at most 2 MBs across. */
    int do_edges = l->b_mb_tree || /* VBV not exposed in phx_param_t: TODO */
                   l->mb_width <= 2 || l->mb_height <= 2;  /* :823 */
    int start_y = l->mb_height - 2 + do_edges;
    int end_y   = 1 - do_edges;
    int start_x = l->mb_width  - 2 + do_edges;
    int end_x   = 1 - do_edges;
    start_y = PHX_LR_MIN(start_y, l->mb_height - 1);
    start_x = PHX_LR_MIN(start_x, l->mb_width  - 1);
    end_y = PHX_LR_MAX(end_y, 0);
    end_x = PHX_LR_MAX(end_x, 0);
    int bipred_weight = 64 - (dist_scale_factor >> 2);    /* :529 (weightp=1) */

    int inter_total = 0, intra_total = 0, intra_mbs = 0;
    for (int y = start_y; y >= end_y; y--)                /* :830-833 */
        for (int x = start_x; x >= end_x; x--)
            lr_mb_cost(l, frames, p0, p1, b, dist_scale_factor,
                       do_search, bipred_weight, x, y,
                       &inter_total, &intra_mbs, &intra_total);

    /* :947-948 — x264 zeroes fenc->i_intra_mbs[b-p0] here.  phx_frame_t has
     * no such array (phx_frame.h:33-35), so the per-MB count is kept only in
     * the phx_lookahead metadata; it does not feed the slicetype decision.
     * TODO: expose i_intra_mbs[] in phx_frame_t if MB-tree is ported. */
    if (first_intra) fenc->i_cost_est[0][0] = intra_total;/* :949-953 */
    fenc->i_cost_est[d0][d1] = inter_total;               /* :954 */
    int score = fenc->i_cost_est[d0][d1];                 /* :983 */
    if (b != p1)
        score = (int)(((uint64_t)score * 100) / (120 + l->bias)); /* :985 */
    else
        c->b_intra_calculated = 1;                        /* :987 */
    fenc->i_cost_est[d0][d1] = score;                     /* :989 */
    (void)intra_mbs; /* see the TODO above; no phx_frame_t field for it */
    return score;
}

/* ---- path cost (slicetype.c:1288-1327, SHADER C) ------------------------- */

uint64_t phx_lowres_path_cost(phx_lowres_t *l, phx_frame_t **frames,
                              const char *path, uint64_t threshold)
{
    uint64_t cost = 0;
    int loc = 1, cur_nonb = 0;
    path--;                                              /* :1293 */
    while (path[loc]) {
        int next_nonb = loc;
        while (path[next_nonb] == 'B') next_nonb++;      /* :1298 */
        if (path[next_nonb] == 'P')
            cost += (uint64_t)phx_lowres_frame_cost(l, frames,
                        cur_nonb, next_nonb, next_nonb);  /* :1303 */
        else
            cost += (uint64_t)phx_lowres_frame_cost(l, frames,
                        next_nonb, next_nonb, next_nonb);  /* :1305 */
        if (cost > threshold) break;                     /* :1307 */
        if (l->b_pyramid && next_nonb - cur_nonb > 2) {   /* :1310-1318 */
            int middle = cur_nonb + (next_nonb - cur_nonb)/2;
            cost += (uint64_t)phx_lowres_frame_cost(l, frames,
                        cur_nonb, next_nonb, middle);
            for (int next_b = loc; next_b < middle && cost < threshold; next_b++)
                cost += (uint64_t)phx_lowres_frame_cost(l, frames,
                            cur_nonb, middle, next_b);
            for (int next_b = middle+1; next_b < next_nonb && cost < threshold; next_b++)
                cost += (uint64_t)phx_lowres_frame_cost(l, frames,
                            middle, next_nonb, next_b);
        } else {
            for (int next_b = loc; next_b < next_nonb && cost < threshold; next_b++)
                cost += (uint64_t)phx_lowres_frame_cost(l, frames,
                            cur_nonb, next_nonb, next_b);  /* :1320-1321 */
        }
        loc = next_nonb + 1;                             /* :1323 */
        cur_nonb = next_nonb;                            /* :1324 */
    }
    return cost;
}

int phx_slicetype_frame_cost(phx_frame_t **frames, int p0, int p1, int b,
                             int b_pyramid)
{
    if (!frames || !frames[b]) return PHX_LR_COST_MAX;
    int max_i = PHX_LR_MAX(p0, PHX_LR_MAX(p1, b));
    int mbw = frames[b]->i_width_lowres  / 8;
    int mbh = frames[b]->i_lines_lowres  / 8;
    if (mbw <= 0 || mbh <= 0 || max_i > PHX_BFRAME_MAX+1)
        return PHX_LR_COST_MAX;
    if (!phx_lr_default_ctx) {
        /* x264 encoder.c:1265-1268, tables.c:29-42 level-derived mv range. */
        int mbs = mbw * mbh;
        int mv_range = mbs <= 99 ? 64 : mbs <= 396 ? 128 :
                       mbs <= 1620 ? 256 : 512;
        phx_lr_default_ctx = phx_lowres_new(mbw, mbh, mv_range,
                                            frames[b]->bframes,
                                            b_pyramid, 0);
    }
    if (!phx_lr_default_ctx) return PHX_LR_COST_MAX;
    /* The standalone API resolves b_pyramid per first use; later calls with a
     * different value are reported through the explicit-context API instead. */
    return phx_lowres_frame_cost(phx_lr_default_ctx, frames, p0, p1, b);
}
