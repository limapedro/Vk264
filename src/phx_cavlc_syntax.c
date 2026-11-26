/* Function-for-function port of x264 CAVLC macroblock-header / MVD syntax.
 * See src/phx_cavlc_syntax.h for the complete source-line index.
 *
 * SHADER: C — all writers are strictly serial; no parallel stage may reorder
 * the bit stream.  The borrowed phx_mb_cache_t is the neighbour boundary.
 */
#include "phx_cavlc_syntax.h"
#include <string.h>

#define PHX_MIN(a, b) ((a) < (b) ? (a) : (b))
#define PHX_MAX(a, b) ((a) > (b) ? (a) : (b))
#define PHX_IS_SKIP(t)  ((t) == PHX_P_SKIP || (t) == PHX_B_SKIP)

/* encoder/cavlc.c:36-46 — cbp_to_golomb[400/420][inter/intra][cbp]. */
const uint8_t phx_cavlc_cbp_to_golomb[2][2][48] =
{
    {{ 0,  1,  2,  5,  3,  6, 14, 10,  4, 15,  7, 11,  8, 12, 13,  9 },
     { 1, 10, 11,  6, 12,  7, 14,  2, 13, 15,  8,  3,  9,  4,  5,  0 }},
    {{ 0,  2,  3,  7,  4,  8, 17, 13,  5, 18,  9, 14, 10, 15, 16, 11,
       1, 32, 33, 36, 34, 37, 44, 40, 35, 45, 38, 41, 39, 42, 43, 19,
       6, 24, 25, 20, 26, 21, 46, 28, 27, 47, 22, 29, 23, 30, 31, 12 },
     { 3, 29, 30, 17, 31, 18, 37,  8, 32, 38, 19,  9, 20, 10, 11,  2,
      16, 33, 34, 21, 35, 22, 39,  4, 36, 40, 23,  5, 24,  6,  7,  1,
      41, 42, 43, 25, 44, 26, 46, 12, 45, 47, 27, 13, 28, 14, 15,  0 }}
};

/* encoder/cavlc.c:48-53 — mb_type_b_to_golomb[16x8,8x16,16x16][B type]. */
const uint8_t phx_cavlc_mb_type_b_to_golomb[3][9] =
{
    { 4,  8, 12, 10,  6, 14, 16, 18, 20 }, /* D_16x8 */
    { 5,  9, 13, 11,  7, 15, 17, 19, 21 }, /* D_8x16 */
    { 1, -1, -1, -1,  2, -1, -1, -1,  3 }  /* D_16x16 */
};

/* encoder/cavlc.c:55-58. */
const uint8_t phx_cavlc_subpartition_p_to_golomb[4] = { 3, 1, 2, 0 };

/* encoder/cavlc.c:60-63. */
const uint8_t phx_cavlc_subpartition_b_to_golomb[13] =
{
    10,  4,  5,  1, 11,  6,  7,  2, 12,  8,  9,  3,  0
};

/* common/predict.h:45-49,62-66,84-92.  FIX4(t) is the macro at :92. */
static const uint8_t phx_chroma_pred_mode_fix[7] = { 0, 1, 2, 3, 0, 0, 0 };
static const uint8_t phx_pred_mode16x16_fix[7] = { 0, 1, 2, 3, 2, 2, 2 };
static const int8_t phx_pred_mode4x4_fix[13] = {
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 2, 2, 2
};
#define FIX4(t) phx_pred_mode4x4_fix[(t)+1]

/* common/macroblock.h:97-109 — x264_mb_type_list_table. */
static const uint8_t phx_mb_type_list_table[19][2][2] = {
    {{0,0},{0,0}}, {{0,0},{0,0}}, {{0,0},{0,0}}, {{0,0},{0,0}},
    {{1,1},{0,0}}, {{0,0},{0,0}}, {{1,1},{0,0}},
    {{0,0},{0,0}},
    {{1,1},{0,0}}, {{1,0},{0,1}}, {{1,1},{0,1}},
    {{0,1},{1,0}}, {{0,0},{1,1}}, {{0,1},{1,1}},
    {{1,1},{1,0}}, {{1,0},{1,1}}, {{1,1},{1,1}},
    {{0,0},{0,0}}, {{0,0},{0,0}}
};

/* common/macroblock.h:143-157 — x264_mb_partition_listX_table. */
static const uint8_t phx_mb_partition_listX_table[2][17] = {
    { 1,1,1,1, 0,0,0,0, 1,1,1,1, 0, 0,0,0,0 },
    { 0,0,0,0, 1,1,1,1, 1,1,1,1, 0, 0,0,0,0 }
};

void phx_cavlc_mb_init(phx_cavlc_mb_t *mb, phx_bs_t *bs, phx_mb_cache_t *cache)
{
    memset(mb, 0, sizeof(*mb));
    mb->bs = bs;
    mb->cache = cache;
}

/* mvpred.c:30-127 — x264_mb_predict_mv(), progressive path.  The MBAFF
 * topright_mv/topright_ref override at mvpred.c:47-66 is deliberately not
 * ported (phx_mbcache.c is progressive only).  SHADER: C. */
void phx_cavlc_predict_mv(const phx_cavlc_mb_t *mb, int i_list, int idx,
                          int width, int16_t mvp[2])
{
    const phx_mb_cache_t *c = mb->cache;
    const int i8 = phx_scan8[idx];                          /* :32 */
    const int i_ref = c->ref[i_list][i8];                   /* :33 */
    const int i_refa = c->ref[i_list][i8 - 1];              /* :34 */
    const int16_t *mv_a = c->mv[i_list][i8 - 1];            /* :35 */
    const int i_refb = c->ref[i_list][i8 - 8];              /* :36 */
    const int16_t *mv_b = c->mv[i_list][i8 - 8];            /* :37 */
    int i_refc = c->ref[i_list][i8 - 8 + width];            /* :38 */
    const int16_t *mv_c = c->mv[i_list][i8 - 8 + width];    /* :39 */

    if ((idx & 3) >= 2 + (width & 1) || i_refc == -2)       /* :42 */
    {
        i_refc = c->ref[i_list][i8 - 8 - 1];                /* :44 */
        mv_c   = c->mv[i_list][i8 - 8 - 1];                 /* :45 */
    }
    if (mb->i_partition == PHX_D_16x8)                      /* :68 */
    {
        if (idx == 0 && i_refb == i_ref) { mvp[0]=mv_b[0]; mvp[1]=mv_b[1]; return; } /* :72-75 */
        if (idx != 0 && i_refa == i_ref) { mvp[0]=mv_a[0]; mvp[1]=mv_a[1]; return; } /* :80-83 */
    }
    else if (mb->i_partition == PHX_D_8x16)                 /* :87 */
    {
        if (idx == 0 && i_refa == i_ref) { mvp[0]=mv_a[0]; mvp[1]=mv_a[1]; return; } /* :91-94 */
        if (idx != 0 && i_refc == i_ref) { mvp[0]=mv_c[0]; mvp[1]=mv_c[1]; return; } /* :99-102 */
    }

    int i_count = (i_refa == i_ref) + (i_refb == i_ref) + (i_refc == i_ref); /* :107 */
    if (i_count > 1)                                        /* :109 */
    {
        for (int k = 0; k < 2; k++)                          /* base.h:244-245 */
            mvp[k] = (int16_t)PHX_MAX(PHX_MIN(mv_a[k], mv_b[k]),
                                      PHX_MIN(PHX_MAX(mv_a[k], mv_b[k]), mv_c[k])); /* :112 */
    }
    else if (i_count == 1)                                  /* :114 */
    {
        const int16_t *src = i_refa == i_ref ? mv_a
                           : i_refb == i_ref ? mv_b : mv_c; /* :116-121 */
        mvp[0] = src[0]; mvp[1] = src[1];
    }
    else if (i_refb == -2 && i_refc == -2 && i_refa != -2)  /* :123 */
    {
        mvp[0] = mv_a[0]; mvp[1] = mv_a[1];                 /* :124 */
    }
    else                                                    /* :126 */
    {
        for (int k = 0; k < 2; k++)                          /* base.h:244-245 */
            mvp[k] = (int16_t)PHX_MAX(PHX_MIN(mv_a[k], mv_b[k]),
                                      PHX_MIN(PHX_MAX(mv_a[k], mv_b[k]), mv_c[k])); /* :126 -> :112 */
    }
}

/* cavlc.c:241-248 — cavlc_mvd().  No VLC table exists here: both components
 * are plain signed Exp-Golomb (bs_write_se, bitstream.h:246).  SHADER: C. */
int phx_cavlc_mvd(phx_cavlc_mb_t *mb, int i_list, int idx, int width)
{
    phx_bs_t *s = mb->bs;
    int start = phx_bs_pos(s);
    int16_t mvp[2];
    phx_cavlc_predict_mv(mb, i_list, idx, width, mvp);      /* :245 */
    phx_bs_write_se(s, mb->cache->mv[i_list][phx_scan8[idx]][0] - mvp[0]); /* :246 */
    phx_bs_write_se(s, mb->cache->mv[i_list][phx_scan8[idx]][1] - mvp[1]); /* :247 */
    return phx_bs_pos(s) - start;
}

/* macroblock.h:420-431 — x264_mb_predict_intra4x4_mode().  SHADER: C. */
static int phx_predict_intra4x4_mode(const phx_cavlc_mb_t *mb, int idx)
{
    const phx_mb_cache_t *c = mb->cache;
    const int ma = c->intra4x4_pred_mode[phx_scan8[idx] - 1];
    const int mt = c->intra4x4_pred_mode[phx_scan8[idx] - 8];
    const int m = PHX_MIN(FIX4(ma), FIX4(mt));
    return m < 0 ? 2 /* I_PRED_4x4_DC */ : m;
}

/* cavlc.c:305-335 — cavlc_mb_header_i().  SHADER: C. */
static void phx_cavlc_mb_header_i(phx_cavlc_mb_t *mb, int i_mb_i_offset)
{
    phx_bs_t *s = mb->bs;
    if (mb->i_type == PHX_I_16x16)
    {
        phx_bs_write_ue(s, (unsigned)(i_mb_i_offset + 1
            + phx_pred_mode16x16_fix[mb->i_intra16x16_pred_mode]
            + mb->i_cbp_chroma * 4 + (mb->i_cbp_luma == 0 ? 0 : 12))); /* :310-311 */
    }
    else /* I_4x4 or I_8x8 */                                /* :313 */
    {
        int di = mb->i_type == PHX_I_8x8 ? 4 : 1;            /* :315 */
        phx_bs_write_ue(s, (unsigned)(i_mb_i_offset + 0));   /* :316 */
        if (mb->b_transform_8x8_mode)                        /* :317 */
            phx_bs_write1(s, (uint32_t)mb->b_transform_8x8); /* :318 */

        for (int i = 0; i < 16; i += di)                     /* :321 */
        {
            int i_pred = phx_predict_intra4x4_mode(mb, i);   /* :323 */
            int i_mode = FIX4(mb->cache->intra4x4_pred_mode[phx_scan8[i]]); /* :324 */
            if (i_pred == i_mode)
                phx_bs_write1(s, 1);                         /* :327 */
            else
                phx_bs_write(s, 4, (uint32_t)(i_mode - (i_mode > i_pred))); /* :329 */
        }
    }
    if (mb->chroma)                                          /* :333 */
        phx_bs_write_ue(s, phx_chroma_pred_mode_fix[mb->i_chroma_pred_mode]); /* :334 */
}

/* cavlc.c:250-272 — cavlc_8x8_mvd() (P_8x8 sub-partitions).  SHADER: C. */
static void phx_cavlc_8x8_mvd(phx_cavlc_mb_t *mb, int i)
{
    switch (mb->i_sub_partition[i])
    {
        case PHX_D_L0_8x8:
            phx_cavlc_mvd(mb, 0, 4*i,     2); break;          /* :255 */
        case PHX_D_L0_8x4:
            phx_cavlc_mvd(mb, 0, 4*i+0,   2);                 /* :258 */
            phx_cavlc_mvd(mb, 0, 4*i+2,   2); break;          /* :259 */
        case PHX_D_L0_4x8:
            phx_cavlc_mvd(mb, 0, 4*i+0,   1);                 /* :262 */
            phx_cavlc_mvd(mb, 0, 4*i+1,   1); break;          /* :263 */
        case PHX_D_L0_4x4:
            phx_cavlc_mvd(mb, 0, 4*i+0,   1);                 /* :266 */
            phx_cavlc_mvd(mb, 0, 4*i+1,   1);                 /* :267 */
            phx_cavlc_mvd(mb, 0, 4*i+2,   1);                 /* :268 */
            phx_cavlc_mvd(mb, 0, 4*i+3,   1); break;          /* :269 */
        default: break;
    }
}

/* cavlc.c:340-372 — P_L0 branch of cavlc_mb_header_p().  SHADER: C. */
static void phx_cavlc_mb_header_p_l0(phx_cavlc_mb_t *mb)
{
    phx_bs_t *s = mb->bs;
    const int i_ref0_max = mb->i_fref[0] - 1;
    if (mb->i_partition == PHX_D_16x16)
    {
        phx_bs_write1(s, 1);                                 /* :344 */
        if (mb->i_fref[0] > 1)                               /* :346 */
            phx_bs_write_te(s, i_ref0_max,
                (unsigned)mb->cache->ref[0][phx_scan8[0]]);  /* :347 */
        phx_cavlc_mvd(mb, 0, 0, 4);                          /* :348 */
    }
    else if (mb->i_partition == PHX_D_16x8)
    {
        phx_bs_write_ue(s, 1);                               /* :352 */
        if (mb->i_fref[0] > 1)                               /* :353 */
        {
            phx_bs_write_te(s, i_ref0_max,
                (unsigned)mb->cache->ref[0][phx_scan8[0]]);  /* :355 */
            phx_bs_write_te(s, i_ref0_max,
                (unsigned)mb->cache->ref[0][phx_scan8[8]]);  /* :356 */
        }
        phx_cavlc_mvd(mb, 0, 0, 4);                          /* :358 */
        phx_cavlc_mvd(mb, 0, 8, 4);                          /* :359 */
    }
    else if (mb->i_partition == PHX_D_8x16)
    {
        phx_bs_write_ue(s, 2);                               /* :363 */
        if (mb->i_fref[0] > 1)                               /* :364 */
        {
            phx_bs_write_te(s, i_ref0_max,
                (unsigned)mb->cache->ref[0][phx_scan8[0]]);  /* :366 */
            phx_bs_write_te(s, i_ref0_max,
                (unsigned)mb->cache->ref[0][phx_scan8[4]]);  /* :367 */
        }
        phx_cavlc_mvd(mb, 0, 0, 2);                          /* :369 */
        phx_cavlc_mvd(mb, 0, 4, 2);                          /* :370 */
    }
}

/* cavlc.c:373-406 — P_8x8 branch of cavlc_mb_header_p().  SHADER: C. */
static void phx_cavlc_mb_header_p_8x8(phx_cavlc_mb_t *mb)
{
    phx_bs_t *s = mb->bs;
    const phx_mb_cache_t *c = mb->cache;
    const int i_ref0_max = mb->i_fref[0] - 1;
    int b_sub_ref;

    if ((c->ref[0][phx_scan8[0]] | c->ref[0][phx_scan8[4]] |
         c->ref[0][phx_scan8[8]] | c->ref[0][phx_scan8[12]]) == 0) /* :376-377 */
    {
        phx_bs_write_ue(s, 4);                               /* :379 */
        b_sub_ref = 0;                                       /* :380 */
    }
    else
    {
        phx_bs_write_ue(s, 3);                               /* :384 */
        b_sub_ref = 1;                                       /* :385 */
    }

    if (mb->b_psub8x8)                                       /* :389 */
        for (int i = 0; i < 4; i++)
            phx_bs_write_ue(s, phx_cavlc_subpartition_p_to_golomb[
                mb->i_sub_partition[i]]);                    /* :391 */
    else
        phx_bs_write(s, 4, 0xf);                             /* :393 */

    if (b_sub_ref)                                           /* :396 */
    {
        phx_bs_write_te(s, i_ref0_max, (unsigned)c->ref[0][phx_scan8[0]]);  /* :398 */
        phx_bs_write_te(s, i_ref0_max, (unsigned)c->ref[0][phx_scan8[4]]);  /* :399 */
        phx_bs_write_te(s, i_ref0_max, (unsigned)c->ref[0][phx_scan8[8]]);  /* :400 */
        phx_bs_write_te(s, i_ref0_max, (unsigned)c->ref[0][phx_scan8[12]]); /* :401 */
    }

    for (int i = 0; i < 4; i++)
        phx_cavlc_8x8_mvd(mb, i);                            /* :405 */
}

/* cavlc.c:414-439 — B_8x8 branch of cavlc_mb_header_b().  SHADER: C. */
static void phx_cavlc_mb_header_b_8x8(phx_cavlc_mb_t *mb)
{
    phx_bs_t *s = mb->bs;
    const phx_mb_cache_t *c = mb->cache;

    phx_bs_write_ue(s, 22);                                  /* :416 */
    for (int i = 0; i < 4; i++)                              /* :419 */
        phx_bs_write_ue(s, phx_cavlc_subpartition_b_to_golomb[
            mb->i_sub_partition[i]]);                        /* :420 */

    if (mb->i_fref[0] > 1)                                   /* :423 */
        for (int i = 0; i < 4; i++)
            if (phx_mb_partition_listX_table[0][mb->i_sub_partition[i]])
                phx_bs_write_te(s, mb->i_fref[0] - 1,
                    (unsigned)c->ref[0][phx_scan8[i*4]]);    /* :426 */
    if (mb->i_fref[1] > 1)                                   /* :427 */
        for (int i = 0; i < 4; i++)
            if (phx_mb_partition_listX_table[1][mb->i_sub_partition[i]])
                phx_bs_write_te(s, mb->i_fref[1] - 1,
                    (unsigned)c->ref[1][phx_scan8[i*4]]);    /* :430 */

    for (int i = 0; i < 4; i++)                              /* :433 */
        if (phx_mb_partition_listX_table[0][mb->i_sub_partition[i]])
            phx_cavlc_mvd(mb, 0, 4*i, 2);                    /* :435 */
    for (int i = 0; i < 4; i++)                              /* :436 */
        if (phx_mb_partition_listX_table[1][mb->i_sub_partition[i]])
            phx_cavlc_mvd(mb, 1, 4*i, 2);                    /* :438 */
}

/* cavlc.c:440-477 — B_L0_L0..B_BI_BI branch.  SHADER: C. */
static void phx_cavlc_mb_header_b_inter(phx_cavlc_mb_t *mb)
{
    phx_bs_t *s = mb->bs;
    const phx_mb_cache_t *c = mb->cache;
    const uint8_t (*b_list)[2] = phx_mb_type_list_table[mb->i_type]; /* :444 */
    const int i_ref0_max = mb->i_fref[0] - 1;                /* :445 */
    const int i_ref1_max = mb->i_fref[1] - 1;                /* :446 */

    phx_bs_write_ue(s, phx_cavlc_mb_type_b_to_golomb[
        mb->i_partition - PHX_D_16x8][mb->i_type - PHX_B_L0_L0]); /* :448 */

    if (mb->i_partition == PHX_D_16x16)                      /* :449 */
    {
        if (i_ref0_max && b_list[0][0])
            phx_bs_write_te(s, i_ref0_max, (unsigned)c->ref[0][phx_scan8[0]]); /* :451 */
        if (i_ref1_max && b_list[1][0])
            phx_bs_write_te(s, i_ref1_max, (unsigned)c->ref[1][phx_scan8[0]]); /* :452 */
        if (b_list[0][0]) phx_cavlc_mvd(mb, 0, 0, 4);        /* :453 */
        if (b_list[1][0]) phx_cavlc_mvd(mb, 1, 0, 4);        /* :454 */
    }
    else                                                     /* :456 */
    {
        if (i_ref0_max && b_list[0][0])
            phx_bs_write_te(s, i_ref0_max, (unsigned)c->ref[0][phx_scan8[0]]);  /* :458 */
        if (i_ref0_max && b_list[0][1])
            phx_bs_write_te(s, i_ref0_max, (unsigned)c->ref[0][phx_scan8[12]]); /* :459 */
        if (i_ref1_max && b_list[1][0])
            phx_bs_write_te(s, i_ref1_max, (unsigned)c->ref[1][phx_scan8[0]]);  /* :460 */
        if (i_ref1_max && b_list[1][1])
            phx_bs_write_te(s, i_ref1_max, (unsigned)c->ref[1][phx_scan8[12]]); /* :461 */
        if (mb->i_partition == PHX_D_16x8)                   /* :462 */
        {
            if (b_list[0][0]) phx_cavlc_mvd(mb, 0, 0, 4);    /* :464 */
            if (b_list[0][1]) phx_cavlc_mvd(mb, 0, 8, 4);    /* :465 */
            if (b_list[1][0]) phx_cavlc_mvd(mb, 1, 0, 4);    /* :466 */
            if (b_list[1][1]) phx_cavlc_mvd(mb, 1, 8, 4);    /* :467 */
        }
        else /* D_8x16 */                                    /* :469 */
        {
            if (b_list[0][0]) phx_cavlc_mvd(mb, 0, 0, 2);    /* :471 */
            if (b_list[0][1]) phx_cavlc_mvd(mb, 0, 4, 2);    /* :472 */
            if (b_list[1][0]) phx_cavlc_mvd(mb, 1, 0, 2);    /* :473 */
            if (b_list[1][1]) phx_cavlc_mvd(mb, 1, 4, 2);    /* :474 */
        }
    }
}

/* cavlc.c:337-409 / :411-482 / :538-543.  SHADER: C. */
int phx_cavlc_mb_header(phx_cavlc_mb_t *mb)
{
    phx_bs_t *s = mb->bs;
    int start = phx_bs_pos(s);

    /* I_PCM is written by x264_macroblock_write_cavlc (cavlc.c:511-535), not
     * by a mb_header_* helper. It needs raw fenc samples and a bs re-init, so
     * it is deliberately out of scope for this API. */
    if (mb->i_type == PHX_I_PCM)                             /* cavlc.c:511 */
        return -1;

    /* Skipped MBs use mb_skip_run (encoder.c:2883-2890), never a header. */
    if (PHX_IS_SKIP(mb->i_type))
        return 0;

    if (mb->slice_type == PHX_SLICE_TYPE_P)                  /* cavlc.c:538 */
    {
        if (mb->i_type == PHX_P_L0)
            phx_cavlc_mb_header_p_l0(mb);                    /* :340-372 */
        else if (mb->i_type == PHX_P_8x8)
            phx_cavlc_mb_header_p_8x8(mb);                   /* :373-406 */
        else
            phx_cavlc_mb_header_i(mb, 5);                    /* :407-408 */
    }
    else if (mb->slice_type == PHX_SLICE_TYPE_B)             /* cavlc.c:540 */
    {
        if (mb->i_type == PHX_B_8x8)
            phx_cavlc_mb_header_b_8x8(mb);                   /* :414-439 */
        else if (mb->i_type >= PHX_B_L0_L0 && mb->i_type <= PHX_B_BI_BI)
            phx_cavlc_mb_header_b_inter(mb);                 /* :440-477 */
        else if (mb->i_type == PHX_B_DIRECT)
            phx_bs_write1(s, 1);                             /* :479 */
        else
            phx_cavlc_mb_header_i(mb, 23);                   /* :480-481 */
    }
    else /* SLICE_TYPE_I */                                  /* cavlc.c:542 */
    {
        phx_cavlc_mb_header_i(mb, 0);                        /* :543 */
    }
    return phx_bs_pos(s) - start;
}

/* encoder.c:2883-2890 and :3087-3088 — mb_skip_run.  SHADER: C. */
int phx_cavlc_mb_skip(phx_bs_t *s, int *skip_run, int b_skip, int b_last)
{
    int start = phx_bs_pos(s);
    if (b_skip) { (*skip_run)++; return 0; }                 /* encoder.c:2884 */
    if (b_last)
    {
        /* encoder.c:3087-3088 — trailing run is written only when nonzero. */
        if (*skip_run > 0)
            phx_bs_write_ue(s, (unsigned)*skip_run);         /* encoder.c:3088 */
    }
    else
    {
        /* encoder.c:2876-2890 — a coded MB in a P/B slice writes ue(i_skip)
         * UNCONDITIONALLY, including ue(0) (a single '1' bit) when the
         * previous MB was coded.  Only the slice-end flush is conditional;
         * treating both call sites alike dropped one bit per coded MB and
         * shifted every following CAVLC header. */
        phx_bs_write_ue(s, (unsigned)*skip_run);             /* encoder.c:2889 */
    }
    *skip_run = 0;                                           /* encoder.c:2890 */
    return phx_bs_pos(s) - start;
}
