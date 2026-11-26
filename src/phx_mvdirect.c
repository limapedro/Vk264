/* Byte-exact B direct MV prediction + B_SKIP probe.
 *
 * Every block below cites the exact reference/x264 line(s) it implements.
 * The progressive path is the specification used by the 64x64/30f clip; MBAFF
 * field pairing (mvpred.c:195-229/:343-372) is NOT supported (see limitation
 * in the report) and is rejected up front rather than approximated.
 *
 * SHADER: B — one workgroup per MB; explicit read-only frame/cache inputs and
 *              one caller-owned result, no hidden encoder state.
 */
#include "phx_mvdirect.h"

#include "phx_dct.h"
#include "phx_mc.h"
#include "phx_quant.h"
#include "phx_tables.h"

#include <string.h>

#define PHX_MV_SCAN8_0 (12)          /* scan8[0] = 4 + 1*8, base.h:180 */
#define PHX_MV_REF_MAX 16            /* X264_REF_MAX for 8-bit C build */
#define PHX_MV_FDEC_STRIDE 32        /* macroblock.c:FDEC_STRIDE */
#define PHX_MV_FENC_STRIDE 16        /* macroblock.c:FENC_STRIDE */
#define PHX_MV_CQM_4PY 1             /* set.h:31 */
#define PHX_MV_CQM_4PC 3             /* set.h:34 */

/* x264 common/macroblock.h:61. */
#define PHX_MV_IS_INTRA(t) ((t) == PHX_I_4x4 || (t) == PHX_I_8x8 || \
                            (t) == PHX_I_16x16 || (t) == PHX_I_PCM)

/* x264 common/base.h:35. */
#define PHX_MV_MIN3(a, b, c) ((a) < (b) ? ((a) < (c) ? (a) : (c)) \
                                          : ((b) < (c) ? (b) : (c)))

static int phx_mvdirect_median3(int a, int b, int c)      /* base.h:~230 */
{
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { int t = b; b = c; c = t; }
    if (a > b) { int t = a; a = b; b = t; }
    return b;
}

static int phx_mvdirect_clip3(int v, int lo, int hi)      /* common.h:145-148 */
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Working copy of the parts of h->mb.cache written by mvpred.c.  Keeping it
 * local makes phx_mvdirect_predict() pure; the caller's cache is untouched. */
typedef struct {
    int8_t  ref[2][40];
    int16_t mv[2][40][2];
} phx_mvdirect_work_t;

/* rectangle.h:145-152 + :128-136.  x/y/width/height are in 4x4-block units
 * (x264_cache_mv_func_table semantics); cache position = scan8[0] + x + 8*y. */
static void work_cache_ref(phx_mvdirect_work_t *w, int x, int y,
                           int width, int height, int list, int8_t ref)
{
    const int base = PHX_MV_SCAN8_0 + x + 8*y;
    for (int j = 0; j < height; j++)
        for (int i = 0; i < width; i++)
            w->ref[list][base + i + j*8] = ref;
}
static void work_cache_mv(phx_mvdirect_work_t *w, int x, int y,
                          int width, int height, int list, int mx, int my)
{
    const int base = PHX_MV_SCAN8_0 + x + 8*y;
    for (int j = 0; j < height; j++)
        for (int i = 0; i < width; i++) {
            w->mv[list][base + i + j*8][0] = (int16_t)mx;
            w->mv[list][base + i + j*8][1] = (int16_t)my;
        }
}

/* mvpred.c:183-287, progressive branch only (SLICE_MBAFF=0, preshift=0,
 * postshift=0, offset=1, yshift=1).  Returns x264's b_available. */
static int predict_temporal(const phx_mvdirect_input_t *in,
                            phx_mvdirect_work_t *w, int *partition)
{
    const phx_mvdirect_col_t *col = in->col;
    if (!col || !col->mb_type || !col->mb_partition || !col->ref[0] || !col->mv[0])
        return 0;

    const int mb_x = in->mb_x, mb_y = in->mb_y, mb_xy = in->mb_xy;
    const int type_col = col->mb_type[mb_xy];            /* :188 */
    const int partition_col = col->mb_partition[mb_xy];  /* :189 */
    *partition = partition_col;                          /* :194 */

    const int i_mb_4x4 = 16*col->i_mb_stride*mb_y + 4*mb_x; /* :230 */
    const int i_mb_8x8 =  4*col->i_mb_stride*mb_y + 2*mb_x; /* :231 */

    work_cache_ref(w, 0, 0, 4, 4, 1, 0);                 /* :233 */

    /* :238-241 — enum order D_8x8 < D_16x8 < D_8x16 < D_16x16. */
    const int max_i8 = (PHX_MVDIR_D_16x16 - partition_col) + 1;
    const int step = (partition_col == PHX_MVDIR_D_16x8) + 1;
    const int width = 4 >> ((PHX_MVDIR_D_16x16 - partition_col) & 1);
    const int height = 4 >> ((PHX_MVDIR_D_16x16 - partition_col) >> 1);

    for (int i8 = 0; i8 < max_i8; i8 += step) {           /* :242 */
        const int x8 = i8 & 1;                            /* :244 */
        const int y8 = i8 >> 1;                           /* :245 */
        const int ypart = 3*y8;                           /* :248 (progressive) */

        if (PHX_MV_IS_INTRA(type_col)) {                  /* :250 */
            work_cache_ref(w, 2*x8, 2*y8, width, height, 0, 0); /* :252 */
            work_cache_mv (w, 2*x8, 2*y8, width, height, 0, 0, 0); /* :253 */
            work_cache_mv (w, 2*x8, 2*y8, width, height, 1, 0, 0); /* :254 */
            continue;
        }

        /* :258-260 — map_col_to_list0(i_ref1_ref>>0) * 1 + (1&i_ref1_ref&0). */
        const int i_part_8x8 = i_mb_8x8 + x8 + (ypart >> 1)*col->i_b8_stride;
        const int i_ref1_ref = col->ref[0][i_part_8x8];    /* :259 */
        int i_ref;                                        /* :260 */
        if (in->map_col_to_list0)
            i_ref = in->map_col_to_list0[i_ref1_ref];
        else
            i_ref = (i_ref1_ref >= 0 && i_ref1_ref < PHX_MV_REF_MAX)
                  ? i_ref1_ref : i_ref1_ref;               /* identity fallback */

        if (i_ref >= 0) {                                 /* :262 */
            const int dsf = in->dist_scale_factor
                          ? in->dist_scale_factor[i_ref][0] : 256; /* :264 */
            const int16_t *mv_col = col->mv[0][i_mb_4x4 + 3*x8
                                             + ypart*col->i_b4_stride]; /* :265 */
            /* :266 — (mv_col[1] * (1 << yshift)) / 2 with yshift = 1,
             * i.e. *2/2 (NOT a plain /2: it must not halve the colocated MV). */
            const int mv_y = (mv_col[1] * 2) / 2;
            const int l0x = (dsf*mv_col[0] + 128) >> 8;    /* :267 */
            const int l0y = (dsf*mv_y + 128) >> 8;         /* :268 */
            if (in->i_threads > 1 &&
                (l0y > in->mv_max_spel_y || l0y - mv_y > in->mv_max_spel_y))
                return 0;                                 /* :269-270 */
            work_cache_ref(w, 2*x8, 2*y8, width, height, 0, (int8_t)i_ref); /* :271 */
            work_cache_mv (w, 2*x8, 2*y8, width, height, 0, l0x, l0y);      /* :272 */
            work_cache_mv (w, 2*x8, 2*y8, width, height, 1,
                           l0x - mv_col[0], l0y - mv_y);                   /* :273 */
        } else {
            return 0;                                     /* :275-283 */
        }
    }
    return 1;                                             /* :286 */
}

/* mvpred.c:289-441, progressive branch (b_interlaced=0).  Returns b_available. */
static int predict_spatial(const phx_mvdirect_input_t *in,
                           phx_mvdirect_work_t *w, int *partition)
{
    const phx_mb_cache_t *c = in->cache;
    const phx_mvdirect_col_t *col = in->col;
    if (!c || !col || !col->mb_type || !col->mb_partition ||
        !col->ref[0] || !col->ref[1] || !col->mv[0] || !col->mv[1])
        return 0;

    int8_t ref[2];
    int16_t mv[2][2];

    for (int list = 0; list < 2; list++) {                /* :293 */
        const int i8 = PHX_MV_SCAN8_0;
        const int i_refa = c->ref[list][i8 - 1];          /* :295 */
        const int16_t *mv_a = c->mv[list][i8 - 1];        /* :296 */
        const int i_refb = c->ref[list][i8 - 8];          /* :297 */
        const int16_t *mv_b = c->mv[list][i8 - 8];        /* :298 */
        int i_refc = c->ref[list][i8 - 8 + 4];            /* :299 */
        const int16_t *mv_c = c->mv[list][i8 - 8 + 4];    /* :300 */
        if (i_refc == -2) {                               /* :301 */
            i_refc = c->ref[list][i8 - 8 - 1];            /* :303 */
            mv_c   = c->mv[list][i8 - 8 - 1];             /* :304 */
        }

        /* :307 — X264_MIN3 on unsigned values. */
        unsigned ua = (unsigned)i_refa, ub = (unsigned)i_refb, uc = (unsigned)i_refc;
        int i_ref = (int)PHX_MV_MIN3(ua, ub, uc);
        if (i_ref < 0) {                                  /* :308 */
            i_ref = -1;                                   /* :310 */
            mv[list][0] = mv[list][1] = 0;                /* :311 */
        } else {
            const int count = (i_refa == i_ref) + (i_refb == i_ref) +
                              (i_refc == i_ref);          /* :317 */
            if (count > 1) {                              /* :319-320 */
                for (int k = 0; k < 2; k++)
                    mv[list][k] = (int16_t)phx_mvdirect_median3(
                        mv_a[k], mv_b[k], mv_c[k]);
            } else {                                      /* :322-329 */
                const int16_t *src = (i_refa == i_ref) ? mv_a
                                   : (i_refb == i_ref) ? mv_b : mv_c;
                mv[list][0] = src[0];
                mv[list][1] = src[1];
            }
        }
        work_cache_ref(w, 0, 0, 4, 4, list, (int8_t)i_ref); /* :332 */
        work_cache_mv(w, 0, 0, 4, 4, list, mv[list][0], mv[list][1]); /* :333 */
        ref[list] = (int8_t)i_ref;                        /* :334 */
    }

    const int mb_xy = in->mb_xy;
    const int type_col = col->mb_type[mb_xy];            /* :340 */
    const int partition_col = col->mb_partition[mb_xy];  /* :341 */
    *partition = partition_col;                          /* :342 */

    const int i_mb_4x4 = in->cache->i_b4_xy;             /* :374 (progressive) */
    const int i_mb_8x8 = in->cache->i_b8_xy;             /* :375 */
    const int8_t *l1ref0 = &col->ref[0][i_mb_8x8];       /* :376 */
    const int8_t *l1ref1 = &col->ref[1][i_mb_8x8];       /* :377 */
    const int16_t (*l1mv0)[2] = (const int16_t (*)[2])&col->mv[0][i_mb_4x4]; /* :378 */
    const int16_t (*l1mv1)[2] = (const int16_t (*)[2])&col->mv[1][i_mb_4x4]; /* :379 */

    /* :381 — (M16(ref) & 0x8080) == 0x8080: both refs negative. */
    if ((((unsigned)(uint8_t)ref[0] | ((unsigned)(uint8_t)ref[1] << 8)) & 0x8080u)
        == 0x8080u) {
        work_cache_ref(w, 0, 0, 4, 4, 0, 0);             /* :383 */
        work_cache_ref(w, 0, 0, 4, 4, 1, 0);             /* :384 */
        return 1;                                        /* :385 */
    }

    if (in->i_threads > 1 &&
        (mv[0][1] > in->mv_max_spel_y || mv[1][1] > in->mv_max_spel_y))
        return 0;                                        /* :388-398 */

    /* :400 — !M64(mv) || IS_INTRA(col) || (ref[0] && ref[1]). */
    if (!(mv[0][0] | mv[0][1] | mv[1][0] | mv[1][1]) ||
        PHX_MV_IS_INTRA(type_col) || (ref[0] && ref[1]))
        return 1;

    const int max_i8 = (PHX_MVDIR_D_16x16 - partition_col) + 1; /* :406 */
    const int step = (partition_col == PHX_MVDIR_D_16x8) + 1;   /* :407 */
    const int width = 4 >> ((PHX_MVDIR_D_16x16 - partition_col) & 1); /* :408 */
    const int height = 4 >> ((PHX_MVDIR_D_16x16 - partition_col) >> 1);/* :409 */

    for (int i8 = 0; i8 < max_i8; i8 += step) {           /* :412 */
        const int x8 = i8 & 1;                            /* :414 */
        const int y8 = i8 >> 1;                           /* :415 */
        const int ypart = 3*y8;                           /* :418 (progressive) */
        const int o8 = x8 + (ypart >> 1)*col->i_b8_stride; /* :419 */
        const int o4 = 3*x8 + ypart*col->i_b4_stride;     /* :420 */

        int idx;                                         /* :425-431 */
        if (l1ref0[o8] == 0) idx = 0;
        else if (l1ref0[o8] < 0 && l1ref1[o8] == 0) idx = 1;
        else continue;

        const int16_t (*mv1)[2] = idx ? l1mv1 : l1mv0;
        if (abs(mv1[o4][0]) <= 1 && abs(mv1[o4][1]) <= 1) { /* :433 */
            if (ref[0] == 0) work_cache_mv(w, 2*x8, 2*y8, width, height, 0, 0, 0); /* :435 */
            if (ref[1] == 0) work_cache_mv(w, 2*x8, 2*y8, width, height, 1, 0, 0); /* :436 */
        }
    }
    return 1;                                            /* :440 */
}

/* mvpred.c:454-516 — the public direct entry point. */
int phx_mvdirect_predict(const phx_mvdirect_input_t *in,
                         phx_mvdirect_result_t *out)
{
    if (!in || !out || !in->cache) return -1;
    if (in->direct_mode == PHX_MVDIR_NONE) {             /* :457-458 */
        memset(out, 0, sizeof(*out));
        out->i_partition = PHX_MVDIR_D_16x16;
        out->ref[0][0] = out->ref[1][0] = -1;
        return 0;
    }
    if (in->b_interlaced) {                              /* unsupported, no guess */
        memset(out, 0, sizeof(*out));
        return -1;
    }

    /* :471-476 — x264 compares the *previous* direct_* cache with the new
     * cache.mv/ref.  phx_mb_cache_t has no direct_* members, so the caller
     * supplies them through in->prev. */
    int8_t  prev_ref[2][4] = {{0}};
    int16_t prev_mv[2][4][2] = {{{0}}};
    if (in->prev) {
        memcpy(prev_ref, in->prev->direct_ref, sizeof(prev_ref));
        memcpy(prev_mv, in->prev->direct_mv, sizeof(prev_mv));
    }

    phx_mvdirect_work_t w;
    memset(&w, 0, sizeof(w));
    for (int l = 0; l < 2; l++)
        for (int i = 0; i < 40; i++) {
            w.ref[l][i] = in->cache->ref[l][i];
            w.mv[l][i][0] = in->cache->mv[l][i][0];
            w.mv[l][i][1] = in->cache->mv[l][i][1];
        }

    /* x264's four 8x8 blocks are cache positions scan8[0], [4], [8], [12]
     * (mvpred.c:473-482/:504-511).  phx_scan8 maps raster block -> cache pos. */
    const int pos[4] = {phx_scan8[0], phx_scan8[4], phx_scan8[8], phx_scan8[12]};
    int available;
    if (in->b_direct_spatial)                            /* :459-465 */
        available = predict_spatial(in, &w, &out->i_partition);
    else
        available = predict_temporal(in, &w, &out->i_partition); /* :466-467 */

    int changed = 0;
    if (available) {                                     /* :469-498 */
        /* x264's comparison order: scan8[0], scan8[12], scan8[4], scan8[8]. */
        const int ord[4] = {0, 3, 1, 2};
        for (int j = 0; j < 4; j++) {
            const int p = pos[ord[j]];
            for (int l = 0; l < 2; l++) {
                changed |= (prev_mv[l][j][0] ^ w.mv[l][p][0]) |
                           (prev_mv[l][j][1] ^ w.mv[l][p][1]);
                changed |= (int)(prev_ref[l][j] ^ w.ref[l][p]);
            }
        }
        out->changed = changed;
        /* mvpred.c:496-497 returns early here when nothing changed.  That
         * early return only skips the (now redundant) direct_* cache copy;
         * this API still exports raster ref/mv so callers always see a
         * complete result. */
    }

    /* :500-513 — cache ref & mv into direct_*. */
    if (available)
        for (int l = 0; l < 2; l++)
            for (int j = 0; j < 4; j++) {
                const int p = pos[j];
                out->direct_mv[l][j][0] = w.mv[l][p][0];
                out->direct_mv[l][j][1] = w.mv[l][p][1];
                out->direct_ref[l][j]   = w.ref[l][p];
            }

    out->available = available;

    /* Export the 16 4x4 blocks in raster order (phx_mb_decision_t layout). */
    for (int l = 0; l < 2; l++)
        for (int k = 0; k < 16; k++) {
            const int p = phx_scan8[k];
            out->ref[l][k] = w.ref[l][p];
            out->mv[l][k][0] = w.mv[l][p][0];
            out->mv[l][k][1] = w.mv[l][p][1];
        }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* B_SKIP probe — encoder/macroblock.c:988-1139, b_bidir=1, 8-bit 4:2:0.      */
/* -------------------------------------------------------------------------- */

/* x264 common/tables.c:373-376 + quant.c:326-364.  Static copies because
 * phx_analysis.c owns its own copies and is not part of this unit. */
static const uint8_t phx_mv_decimate_table4[16] = {
    3, 2, 2, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
static int phx_mv_decimate16(const phx_dctcoef *dct)
{
    int score = 0, idx = 15;
    while (idx >= 0 && dct[idx] == 0) idx--;
    while (idx >= 0) {
        int run;
        if ((unsigned)(dct[idx--] + 1) > 2) return 9;    /* quant.c:338-339 */
        run = 0;
        while (idx >= 0 && dct[idx] == 0) { idx--; run++; }
        score += phx_mv_decimate_table4[run];             /* :347 */
    }
    return score;
}
/* quant.c:353-355 — dct[1..15]. */
static int phx_mv_decimate15(const phx_dctcoef *dct)
{
    return phx_mv_decimate16(dct + 1);
}

/* common/dct.c:843-846 progressive zigzag of 16 coefficients. */
static const uint8_t phx_mv_zigzag4[16] = {
     0, 1, 8, 4, 2, 9, 3,10,
     6, 5,12,13, 7,14,11,15
};
static void phx_mv_zigzag_scan4(phx_dctcoef level[16], const phx_dctcoef dct[16])
{
    for (int i = 0; i < 16; i++) level[i] = dct[phx_mv_zigzag4[i]];
}

/* common/pixel.c:55-73 — 8x8 SAD of squares. */
static int phx_mv_ssd8(const uint8_t *p1, int s1, const uint8_t *p2, int s2)
{
    int sum = 0;
    for (int y = 0; y < 8; y++, p1 += s1, p2 += s2)
        for (int x = 0; x < 8; x++) {
            const int d = (int)p1[x] - (int)p2[x];
            sum += d*d;
        }
    return sum;
}

/* The raster 4x4 block index of each 8x8 block's top-left 4x4 block. */
static const int phx_mv_i8_raster[4] = {0, 2, 8, 10};

/* Bidirectional MC of one MB, mirroring common/macroblock.c:112-153
 * (mb_mc_01xywh) / :76-102 / :40-75 for the four 8x8 blocks.  The caller
 * supplies separate luma and planar chroma buffers with FDEC_STRIDE=32. */
static void phx_mv_mc_bidir(const phx_frame_t *fref0, const phx_frame_t *fref1,
                            const phx_mvdirect_result_t *direct,
                            int mb_x, int mb_y, int mv_min[2], int mv_max[2],
                            int weight, uint8_t *dec_y, uint8_t *dec_u,
                            uint8_t *dec_v)
{
    uint8_t tmp0[16*16], tmp1[16*16];
    uint8_t cu0[8*8], cu1[8*8], cv0[8*8], cv1[8*8];

    for (int i8 = 0; i8 < 4; i8++) {                     /* macroblock.c:202-206 */
        const int x8 = i8 & 1, y8 = i8 >> 1;
        const int k = phx_mv_i8_raster[i8];
        const int r0 = direct->ref[0][k], r1 = direct->ref[1][k];
        int mx0 = phx_mvdirect_clip3(direct->mv[0][k][0], mv_min[0], mv_max[0]);
        int my0 = phx_mvdirect_clip3(direct->mv[0][k][1], mv_min[1], mv_max[1]);
        int mx1 = phx_mvdirect_clip3(direct->mv[1][k][0], mv_min[0], mv_max[0]);
        int my1 = phx_mvdirect_clip3(direct->mv[1][k][1], mv_min[1], mv_max[1]);
        mx0 += 32*x8; my0 += 32*y8;                       /* :118-121 */
        mx1 += 32*x8; my1 += 32*y8;

        /* The luma/chroma MC wrappers cache one reference frame at a time, so
         * finish list 0 before list 1 (no interleaving of the two caches). */
        if (r0 >= 0) {
            phx_mc_luma_mb(fref0, tmp0, 16, mb_x, mb_y, mx0, my0, 8, 8); /* :44/:105 */
            /* p_fref[0][r][4] is the chroma plane at the MB origin
             * (macroblock.c:625); the 8x8 offset is carried by mx/my. */
            phx_mc_chroma(cu0, 8, cv0, 8,
                          fref0->plane_chroma + (mb_y*8)*fref0->i_stride_planes[1]
                                              + (mb_x*8)*2,
                          fref0->i_stride_planes[1], mx0, my0, 4, 4);   /* :143-146 */
        }
        if (r1 >= 0) {
            phx_mc_luma_mb(fref1, tmp1, 16, mb_x, mb_y, mx1, my1, 8, 8); /* :44/:108 */
            phx_mc_chroma(cu1, 8, cv1, 8,
                          fref1->plane_chroma + (mb_y*8)*fref1->i_stride_planes[1]
                                              + (mb_x*8)*2,
                          fref1->i_stride_planes[1], mx1, my1, 4, 4);   /* :145-146 */
        }

        uint8_t *dst_y = dec_y + x8*8 + y8*8*PHX_MV_FDEC_STRIDE;
        uint8_t *dst_u = dec_u + x8*4 + y8*4*PHX_MV_FDEC_STRIDE;
        uint8_t *dst_v = dec_v + x8*4 + y8*4*PHX_MV_FDEC_STRIDE;
        if (r0 >= 0 && r1 >= 0) {                        /* :190-192, :128-151 */
            if (weight == 32) {
                phx_mc_pixel_avg(dst_y, PHX_MV_FDEC_STRIDE, tmp0, 16, tmp1, 16, 8, 8);
                phx_mc_pixel_avg(dst_u, PHX_MV_FDEC_STRIDE, cu0, 8, cu1, 8, 4, 4);
                phx_mc_pixel_avg(dst_v, PHX_MV_FDEC_STRIDE, cv0, 8, cv1, 8, 4, 4);
            } else {                                     /* mc.c:79-87 */
                for (int j = 0; j < 8; j++)
                    for (int i = 0; i < 8; i++)
                        dst_y[j*PHX_MV_FDEC_STRIDE + i] = (uint8_t)phx_mvdirect_clip3(
                            (tmp0[j*16 + i]*weight + tmp1[j*16 + i]*(64-weight) + 32) >> 6, 0, 255);
                for (int j = 0; j < 4; j++)
                    for (int i = 0; i < 4; i++) {
                        dst_u[j*PHX_MV_FDEC_STRIDE + i] = (uint8_t)phx_mvdirect_clip3(
                            (cu0[j*8 + i]*weight + cu1[j*8 + i]*(64-weight) + 32) >> 6, 0, 255);
                        dst_v[j*PHX_MV_FDEC_STRIDE + i] = (uint8_t)phx_mvdirect_clip3(
                            (cv0[j*8 + i]*weight + cv1[j*8 + i]*(64-weight) + 32) >> 6, 0, 255);
                    }
            }
        } else if (r0 >= 0) {                            /* :193-194 */
            memcpy(tmp1, tmp0, 8*16); memcpy(cu1, cu0, 4*8); memcpy(cv1, cv0, 4*8);
            phx_mc_pixel_avg(dst_y, PHX_MV_FDEC_STRIDE, tmp0, 16, tmp1, 16, 8, 8);
            phx_mc_pixel_avg(dst_u, PHX_MV_FDEC_STRIDE, cu0, 8, cu1, 8, 4, 4);
            phx_mc_pixel_avg(dst_v, PHX_MV_FDEC_STRIDE, cv0, 8, cv1, 8, 4, 4);
        } else if (r1 >= 0) {                            /* :196 */
            memcpy(tmp0, tmp1, 8*16); memcpy(cu0, cu1, 4*8); memcpy(cv0, cv1, 4*8);
            phx_mc_pixel_avg(dst_y, PHX_MV_FDEC_STRIDE, tmp0, 16, tmp1, 16, 8, 8);
            phx_mc_pixel_avg(dst_u, PHX_MV_FDEC_STRIDE, cu0, 8, cu1, 8, 4, 4);
            phx_mc_pixel_avg(dst_v, PHX_MV_FDEC_STRIDE, cv0, 8, cv1, 8, 4, 4);
        }
    }
}

int phx_mvdirect_probe_bskip(const phx_frame_t *fenc,
                             const phx_frame_t *fref0,
                             const phx_frame_t *fref1,
                             const phx_mvdirect_result_t *direct,
                             int mb_x, int mb_y, int qp, int chroma_qp,
                             int bipred_weight)
{
    if (!fenc || !fref0 || !fref1 || !direct || !fenc->plane ||
        !fref0->plane || !fref1->plane || !fenc->plane_chroma ||
        !fref0->plane_chroma || !fref1->plane_chroma)
        return -1;

    const int q = phx_mvdirect_clip3(qp, 0, PHX_QUANT_QP_MAX_SPEC);
    const int cq = phx_mvdirect_clip3(chroma_qp, 0, PHX_QUANT_QP_MAX_SPEC);
    const int weight = bipred_weight == 0 ? 32 : bipred_weight;

    /* analyse.c:336-337 / :391-392 — h->mb.mv_min/mv_max (full range). */
    int mv_min[2] = {4*(-16*mb_x - 24), 4*(-16*mb_y - 24)};
    int mv_max[2] = {4*(16*(fenc->i_width/16  - mb_x - 1) + 24),
                     4*(16*(fenc->i_height/16 - mb_y - 1) + 24)};

    uint8_t dec_y[16*PHX_MV_FDEC_STRIDE], dec_u[8*PHX_MV_FDEC_STRIDE];
    uint8_t dec_v[8*PHX_MV_FDEC_STRIDE];
    memset(dec_y, 0, sizeof(dec_y));
    memset(dec_u, 0, sizeof(dec_u));
    memset(dec_v, 0, sizeof(dec_v));
    phx_mv_mc_bidir(fref0, fref1, direct, mb_x, mb_y, mv_min, mv_max,
                    weight, dec_y, dec_u, dec_v);

    phx_quant_tables_t quant;
    phx_quant_tables_init(&quant);

    const uint8_t *src = fenc->plane + mb_y*16*fenc->i_stride + mb_x*16;
    phx_dctcoef dct4x4[4][16], dctscan[16];
    for (int i8 = 0; i8 < 4; i8++) {                     /* macroblock.c:1010 */
        const int fx = (i8 & 1)*8, fy = (i8 >> 1)*8;
        const int fo = fx + fy*PHX_MV_FDEC_STRIDE;
        phx_sub8x8_dct(dct4x4, src + fx + fy*fenc->i_stride, fenc->i_stride,
                       dec_y + fo, PHX_MV_FDEC_STRIDE);  /* :1015-1016 */
        const int nz = phx_quant_4x4x4(dct4x4,            /* :1022 */
                                       quant.quant4_mf[PHX_MV_CQM_4PY][q],
                                       quant.quant4_bias[PHX_MV_CQM_4PY][q]);
        int score = 0;                                   /* :1010 */
        for (int i4 = 0; i4 < 4; i4++) {                  /* :1023 */
            if (!(nz & (1 << i4))) continue;
            phx_mv_zigzag_scan4(dctscan, dct4x4[i4]);    /* :1025 */
            score += phx_mv_decimate16(dctscan);         /* :1026 */
            if (score >= 6) return 0;                    /* :1027-1028 */
        }
    }

    /* 4:2:0 chroma probe — macroblock.c:1033-1123. */
    const int thresh = (phx_lambda2_tab[cq] + 32) >> 6;  /* :1037 */
    const uint8_t *fc = fenc->plane_chroma;
    /* FENC_STRIDE=16 rows: the probe reads p_fenc[p] with that stride
     * (encoder/macroblock.c:1012/:1055), so the buffers must be 8*16 bytes. */
    uint8_t fenc_u[8*PHX_MV_FENC_STRIDE], fenc_v[8*PHX_MV_FENC_STRIDE];
    for (int y = 0; y < 8; y++)                          /* frame.c:45-57 */
        for (int x = 0; x < 8; x++) {
            const uint8_t *row = fc + (mb_y*8 + y)*fenc->i_stride_planes[1]
                                     + 2*(mb_x*8 + x);
            fenc_u[y*PHX_MV_FENC_STRIDE + x] = row[0];
            fenc_v[y*PHX_MV_FENC_STRIDE + x] = row[1];
        }

    const uint8_t *p_src[2] = {fenc_u, fenc_v};
    const uint8_t *p_dst[2] = {dec_u, dec_v};
    phx_dctcoef dct_dc[4];
    for (int ch = 0; ch < 2; ch++) {                     /* :1053 */
        const int ssd = phx_mv_ssd8(p_dst[ch], PHX_MV_FDEC_STRIDE,
                                    p_src[ch], PHX_MV_FENC_STRIDE); /* :1065 */
        if (ssd < thresh) continue;                      /* :1066-1067 */
        phx_sub8x8_dct_dc(dct_dc, p_src[ch], PHX_MV_FENC_STRIDE,
                          p_dst[ch], PHX_MV_FDEC_STRIDE); /* :1088 */
        const int mf = (int)quant.quant4_mf[PHX_MV_CQM_4PC][cq][0] >> 1; /* :1092 */
        const int bias = (int)quant.quant4_bias[PHX_MV_CQM_4PC][cq][0] << 1; /* :1093 */
        if (phx_quant_2x2_dc(dct_dc, mf, bias)) return 0; /* :1092-1094 */
        if (ssd < thresh*4) continue;                    /* :1097-1098 */
        phx_sub8x8_dct(dct4x4, p_src[ch], PHX_MV_FENC_STRIDE,
                       p_dst[ch], PHX_MV_FDEC_STRIDE);   /* :1103 */
        for (int i = 0; i < 4; i++) dct4x4[i][0] = 0;    /* :1104-1107 */
        const int nz = phx_quant_4x4x4(dct4x4,           /* :1113 */
                                       quant.quant4_mf[PHX_MV_CQM_4PC][cq],
                                       quant.quant4_bias[PHX_MV_CQM_4PC][cq]);
        int score = 0;                                   /* :1111 */
        for (int i4 = 0; i4 < 4; i4++) {                  /* :1114 */
            if (!(nz & (1 << i4))) continue;
            phx_mv_zigzag_scan4(dctscan, dct4x4[i4]);    /* :1116 */
            score += phx_mv_decimate15(dctscan);         /* :1117 */
            if (score >= 7) return 0;                    /* :1118-1119 */
        }
    }
    return 1;                                            /* :1125-1126, :969 */
}
