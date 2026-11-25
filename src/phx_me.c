/* Function-for-function port of x264's full-pel P-slice motion estimation.
 * Every ported operation carries its source line; see phx_me.h for the full
 * source map (exact reference/x264 line numbers) and the out-of-scope list.
 *
 * SHADER: A — pure, state-free per-MB search (see phx_me.h).
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
#define PHX_CLIP3(a,v,b) ((v) < (a) ? (a) : ((v) > (b) ? (b) : (v)))

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

/* x264's COPY1_IF_LT (common/common.h): a plain strict "less-than" assign.
 * The callers pack the direction tag into `val` themselves (e.g. me.c:330-333
 * passes (costs[0]<<4)+1), so this macro must NOT add `tag` a second time.
 * `tag` is kept only to document which search candidate each line handles. */
#define PHX_COPY1_IF_LT_TAG(bcost, val, tag) do { \
    (void)(tag); \
    if ((val) < (bcost)) (bcost) = (val); \
} while (0)

int phx_me16(int x, int y, const phx_frame_t *fenc, const phx_frame_t *fref,
             const phx_cost_tables_t *cost, int mv_range, int lambda,
             int method, const int16_t pred[2], int *out_mv)
{
    if (!fenc || !fref || !fenc->plane || !fref->plane || !out_mv)
        return -1;
    if (mv_range < 0 || x < 0 || y < 0 ||
        x % 16 || y % 16 || x + 16 > fenc->i_width || y + 16 > fenc->i_height)
        return -1;
    /* Only the two smallest x264 methods are ported (phx_param.h:13). */
    if (method != PHX_ME_DIA && method != PHX_ME_HEX)
        return -1;

    /* x264 me.c:199-202 — mv_limit_fpel[], clipped here to the padded luma
     * plane so every 16x16 candidate read stays in bounds. */
    int mv_x_min = -mv_range, mv_x_max = mv_range;
    int mv_y_min = -mv_range, mv_y_max = mv_range;
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
    c.fenc = fenc->plane + y*fenc->i_stride + x;   /* me.c:192 */
    /* p_fref_w is a window at the current block, so candidate (mx,my) reads
     * fref[(y+my)*stride + (x+mx)] (me.c:193, 66-67). */
    c.fref = fref->plane + y*fref->i_stride + x;    /* me.c:193 p_fref_w */
    c.stride = fref->i_stride;                     /* me.c:187 */
    c.p_cost_mv = phx_resolve_cost_mv(cost, lambda); /* me.c:211 */
    c.lambda = lambda;                             /* analyse.c:149 */
    c.mv_range = mv_range;
    c.mvp_x = pred ? pred[0] : 0;                  /* me.h:50, me.c:211 */
    c.mvp_y = pred ? pred[1] : 0;
    c.bcost = PHX_COST_MAX;                        /* me.c:189 */

    /* x264 me.c:276-281 — full-pel MVP branch (subme < 3).  All best-MV
     * state lives in `c`; x264's bmx/bmy are c.bmx/c.bmy here. */
    c.bmx = PHX_CLIP3(PHX_FPEL(c.mvp_x), mv_x_min, mv_x_max); /* me.c:279 */
    c.bmy = PHX_CLIP3(PHX_FPEL(c.mvp_y), mv_y_min, mv_y_max); /* me.c:280 */
    int pmvx = c.bmx, pmvy = c.bmy;                /* me.c:281 pmv */
    /* me.c:291 — SAD only; MV bits are deliberately omitted for the MVP. */
    c.bcost = phx_pixel_sad_16x16(c.fenc, c.stride,
                                  c.fref + c.bmy*c.stride + c.bmx, c.stride);
    /* me.c:316-317 — test the zero vector when it is not the MVP.
     * (No mvc[] candidates exist in this API: me.c:293-313 is skipped.) */
    if (c.bmx | c.bmy) phx_cost_mv(&c, 0, 0);

    int costs[8];

    if (method == PHX_ME_DIA)
    {
        /* x264 me.c:322-341 — diamond search, radius 1. */
        const int dia[4][2] = { {0,-1}, {0,1}, {-1,0}, {1,0} }; /* me.c:329 */
        c.bcost <<= 4;                             /* me.c:325 */
        int i = mv_range;                          /* me.c:326 */
        do
        {
            phx_cost_mv_x4_dir(&c, dia, costs);    /* me.c:329 */
            PHX_COPY1_IF_LT_TAG(c.bcost, (costs[0]<<4)+1,  1); /* me.c:330 */
            PHX_COPY1_IF_LT_TAG(c.bcost, (costs[1]<<4)+3,  3); /* me.c:331 */
            PHX_COPY1_IF_LT_TAG(c.bcost, (costs[2]<<4)+4,  4); /* me.c:332 */
            PHX_COPY1_IF_LT_TAG(c.bcost, (costs[3]<<4)+12, 12);/* me.c:333 */
            if (!(c.bcost & 15)) break;            /* me.c:334-335 */
            c.bmx -= (int32_t)((uint32_t)c.bcost << 28) >> 30; /* me.c:336 */
            c.bmy -= (int32_t)((uint32_t)c.bcost << 30) >> 30; /* me.c:337 */
            c.bcost &= ~15;                        /* me.c:338 */
        } while (--i && phx_check_mv_range(c.bmx, c.bmy, mv_min, mv_max));
                                                   /* me.c:339 */
        c.bcost >>= 4;                             /* me.c:340 */
    }
    else /* PHX_ME_HEX */
    {
        /* x264 me.c:344-375 — initial hexagon, radius 2. */
        const int ha[3][2] = { {-2,0}, {-1,2}, {1,2} };  /* me.c:367 */
        const int hb[3][2] = { { 2,0}, { 1,-2}, {-1,-2} };/* me.c:368 */
        phx_cost_mv_x3_dir(&c, ha, costs);         /* me.c:367 */
        phx_cost_mv_x3_dir(&c, hb, costs+4);       /* me.c:368 (+4 alignment) */
        c.bcost <<= 3;                             /* me.c:369 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[0]<<3)+2, 2); /* me.c:370 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[1]<<3)+3, 3); /* me.c:371 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[2]<<3)+4, 4); /* me.c:372 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[4]<<3)+5, 5); /* me.c:373 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[5]<<3)+6, 6); /* me.c:374 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[6]<<3)+7, 7); /* me.c:375 */

        if (c.bcost & 7)                           /* me.c:377 */
        {
            int dir = (c.bcost & 7) - 2;           /* me.c:379 */
            c.bmx += phx_hex2[dir+1][0];           /* me.c:380 */
            c.bmy += phx_hex2[dir+1][1];           /* me.c:381 */
            /* me.c:383-384 — half hexagon, no overlap with previous iteration. */
            for (int i = (mv_range>>1) - 1;
                 i > 0 && phx_check_mv_range(c.bmx, c.bmy, mv_min, mv_max); i--)
            {
                int off[3][2] = {
                    { phx_hex2[dir+0][0], phx_hex2[dir+0][1] },
                    { phx_hex2[dir+1][0], phx_hex2[dir+1][1] },
                    { phx_hex2[dir+2][0], phx_hex2[dir+2][1] }
                };
                phx_cost_mv_x3_dir(&c, off, costs);/* me.c:386-389 */
                c.bcost &= ~7;                     /* me.c:390 */
                PHX_COPY1_IF_LT_TAG(c.bcost, (costs[0]<<3)+1, 1); /* me.c:391 */
                PHX_COPY1_IF_LT_TAG(c.bcost, (costs[1]<<3)+2, 2); /* me.c:392 */
                PHX_COPY1_IF_LT_TAG(c.bcost, (costs[2]<<3)+3, 3); /* me.c:393 */
                if (!(c.bcost & 7)) break;         /* me.c:394-395 */
                dir += (c.bcost & 7) - 2;          /* me.c:396 */
                dir = phx_mod6m1[dir+1];           /* me.c:397 */
                c.bmx += phx_hex2[dir+1][0];       /* me.c:398 */
                c.bmy += phx_hex2[dir+1][1];       /* me.c:399 */
            }
        }
        c.bcost >>= 3;                             /* me.c:402 */

        /* x264 me.c:404-418 — square refine. */
        const int s1[4][2] = { {0,-1}, {0,1}, {-1,0}, {1,0} };   /* me.c:406 */
        const int s2[4][2] = { {-1,-1}, {-1,1}, {1,-1}, {1,1} }; /* me.c:411 */
        c.bcost <<= 4;                             /* me.c:405 */
        phx_cost_mv_x4_dir(&c, s1, costs);         /* me.c:406 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[0]<<4)+1, 1); /* me.c:407 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[1]<<4)+2, 2); /* me.c:408 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[2]<<4)+3, 3); /* me.c:409 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[3]<<4)+4, 4); /* me.c:410 */
        phx_cost_mv_x4_dir(&c, s2, costs);         /* me.c:411 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[0]<<4)+5, 5); /* me.c:412 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[1]<<4)+6, 6); /* me.c:413 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[2]<<4)+7, 7); /* me.c:414 */
        PHX_COPY1_IF_LT_TAG(c.bcost, (costs[3]<<4)+8, 8); /* me.c:415 */
        c.bmx += phx_square1[c.bcost & 15][0];     /* me.c:416 */
        c.bmy += phx_square1[c.bcost & 15][1];     /* me.c:417 */
        c.bcost >>= 4;                             /* me.c:418 */
    }

    /* x264 me.c:774-784 — qpel MV / cost output (subme < 3 branch). */
    int cost_mv = phx_bits_mvd(&c, c.bmx, c.bmy);  /* me.c:779 */
    int final_cost = c.bcost;                      /* me.c:780 */
    if (c.bmx == pmvx && c.bmy == pmvy)            /* me.c:782 bmv == pmv */
        final_cost += cost_mv;                     /* me.c:782 */
    out_mv[0] = c.bmx * 4;                         /* SPEL(bmx), me.c:179 */
    out_mv[1] = c.bmy * 4;                         /* SPEL(bmy), me.c:179 */
    return final_cost;                             /* x264_me_t.cost, me.h:54 */
}
