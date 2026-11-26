/* Scalar in-loop deblocking filter, line-by-line port of:
 *   reference/x264/common/deblock.c:32-606
 *   reference/x264/common/macroblock.c:1438-1622 (bs producer, encoder.c:3057)
 *   reference/x264/common/macroblock.h:249-270  (chroma QP table)
 *   reference/x264/common/common.h:93-148       (x264_clip3 / x264_clip_pixel)
 *
 * SHADER: B — one invocation per MB; all eight edges of that MB are computed
 * from explicit cache/plane inputs and written back in x264's fixed order.
 * Edge filters themselves are seek-safe Class A-style pure functions.
 */
#include "phx_deblock.h"
#include "phx_tables.h"
#include <stdlib.h>   /* abs(), deblock.c:88/92/98/144/146/192/248 */
#include <string.h>

/* 8-bit pixel format.  deblock.c uses BIT_DEPTH-8 == 0 shifts throughout. */
#define PHX_DB_CHROMA_V_SHIFT 1          /* common/common.h:82 (CHROMA_420) */

/* common.h:93-108 / :145-148 — x264_clip3 and x264_clip_pixel. */
static int phx_db_clip3(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static uint8_t phx_db_clip_pixel(int v)
{
    return (uint8_t)phx_db_clip3(v, 0, 255);
}
#define PHX_DB_MIN(a,b) ((a) < (b) ? (a) : (b))
#define PHX_DB_MAX(a,b) ((a) > (b) ? (a) : (b))

/* deblock.c:32-73 — the three tables, copied verbatim. */
static const uint8_t phx_db_i_alpha_table[52+12*3] =
{
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  4,  4,  5,  6,
     7,  8,  9, 10, 12, 13, 15, 17, 20, 22,
    25, 28, 32, 36, 40, 45, 50, 56, 63, 71,
    80, 90,101,113,127,144,162,182,203,226,
   255,255,
   255,255,255,255,255,255,255,255,255,255,255,255,
};
static const uint8_t phx_db_i_beta_table[52+12*3] =
{
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  2,  2,  2,  3,
     3,  3,  3,  4,  4,  4,  6,  6,  7,  7,
     8,  8,  9,  9, 10, 10, 11, 11, 12, 12,
    13, 13, 14, 14, 15, 15, 16, 16, 17, 17,
    18, 18,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,
};
static const int8_t phx_db_i_tc0_table[52+12*3][4] =
{
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 1 },
    {-1, 0, 0, 1 }, {-1, 0, 0, 1 }, {-1, 0, 0, 1 }, {-1, 0, 1, 1 }, {-1, 0, 1, 1 }, {-1, 1, 1, 1 },
    {-1, 1, 1, 1 }, {-1, 1, 1, 1 }, {-1, 1, 1, 1 }, {-1, 1, 1, 2 }, {-1, 1, 1, 2 }, {-1, 1, 1, 2 },
    {-1, 1, 1, 2 }, {-1, 1, 2, 3 }, {-1, 1, 2, 3 }, {-1, 2, 2, 3 }, {-1, 2, 2, 4 }, {-1, 2, 3, 4 },
    {-1, 2, 3, 4 }, {-1, 3, 3, 5 }, {-1, 3, 4, 6 }, {-1, 3, 4, 6 }, {-1, 4, 5, 7 }, {-1, 4, 5, 8 },
    {-1, 4, 6, 9 }, {-1, 5, 7,10 }, {-1, 6, 8,11 }, {-1, 6, 8,13 }, {-1, 7,10,14 }, {-1, 8,11,16 },
    {-1, 9,12,18 }, {-1,10,13,20 }, {-1,11,15,23 }, {-1,13,17,25 },
    {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 },
    {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 },
};

/* deblock.c:74-76 — table(x) is table[x+24].  Clamp only at the accessor
 * boundary; the tables saturate, so clamping never changes a valid sample. */
static int phx_db_table_index(int x)
{
    return phx_db_clip3(x + 24, 0, (int)sizeof(phx_db_i_alpha_table) - 1);
}

/* SHADER: A — immutable table lookup (deblock.c:74-76). */
int phx_deblock_alpha(int qp_plus_offset)
{
    return phx_db_i_alpha_table[phx_db_table_index(qp_plus_offset)];
}
/* SHADER: A — immutable table lookup (deblock.c:74-76). */
int phx_deblock_beta(int qp_plus_offset)
{
    return phx_db_i_beta_table[phx_db_table_index(qp_plus_offset)];
}
/* SHADER: A — immutable table lookup (deblock.c:74-76). */
int phx_deblock_tc0(int qp_plus_offset, int bs)
{
    return phx_db_i_tc0_table[phx_db_table_index(qp_plus_offset)]
                             [phx_db_clip3(bs, 0, 3)];
}

/* ------------------------------------------------------------------ *
 * Inter edge filters, deblock.c:79-181
 * ------------------------------------------------------------------ */

/* deblock.c:79-109 — deblock_edge_luma_c. */
static void phx_db_edge_luma_c(uint8_t *pix, intptr_t xstride,
                               int alpha, int beta, int8_t tc0)
{
    int p2 = pix[-3*xstride];
    int p1 = pix[-2*xstride];
    int p0 = pix[-1*xstride];
    int q0 = pix[ 0*xstride];
    int q1 = pix[ 1*xstride];
    int q2 = pix[ 2*xstride];

    if( abs( p0 - q0 ) < alpha && abs( p1 - p0 ) < beta && abs( q1 - q0 ) < beta )
    {
        int tc = tc0;
        int delta;
        if( abs( p2 - p0 ) < beta )
        {
            if( tc0 )
                pix[-2*xstride] = (uint8_t)(p1 + phx_db_clip3( (( p2 + ((p0 + q0 + 1) >> 1)) >> 1) - p1, -tc0, tc0 ));
            tc++;
        }
        if( abs( q2 - q0 ) < beta )
        {
            if( tc0 )
                pix[ 1*xstride] = (uint8_t)(q1 + phx_db_clip3( (( q2 + ((p0 + q0 + 1) >> 1)) >> 1) - q1, -tc0, tc0 ));
            tc++;
        }

        delta = phx_db_clip3( (((q0 - p0 ) * 4) + (p1 - q1) + 4) >> 3, -tc, tc );
        pix[-1*xstride] = phx_db_clip_pixel( p0 + delta );    /* p0' */
        pix[ 0*xstride] = phx_db_clip_pixel( q0 - delta );    /* q0' */
    }
}

/* deblock.c:110-122 — deblock_luma_c. */
static void phx_db_luma_c(uint8_t *pix, intptr_t xstride, intptr_t ystride,
                          int alpha, int beta, const int8_t *tc0)
{
    for( int i = 0; i < 4; i++ )
    {
        if( tc0[i] < 0 )
        {
            pix += 4*ystride;
            continue;
        }
        for( int d = 0; d < 4; d++, pix += ystride )
            phx_db_edge_luma_c( pix, xstride, alpha, beta, tc0[i] );
    }
}

/* deblock.c:123-127 — deblock_h_luma_mbaff_c. */
/* SHADER: B — edge-parallel within the MB. */
void phx_deblock_h_luma_mbaff_c(uint8_t *pix, intptr_t stride,
                                int alpha, int beta, const int8_t tc0[4])
{
    for( int d = 0; d < 8; d++, pix += stride )
        phx_db_edge_luma_c( pix, 1, alpha, beta, tc0[d>>1] );
}

/* deblock.c:128-135 — deblock_v_luma_c / deblock_h_luma_c. */
/* SHADER: B — edge-parallel within the MB. */
void phx_deblock_v_luma_c(uint8_t *pix, intptr_t stride,
                          int alpha, int beta, const int8_t tc0[4])
{
    phx_db_luma_c( pix, stride, 1, alpha, beta, tc0 );
}
/* SHADER: B — edge-parallel within the MB. */
void phx_deblock_h_luma_c(uint8_t *pix, intptr_t stride,
                          int alpha, int beta, const int8_t tc0[4])
{
    phx_db_luma_c( pix, 1, stride, alpha, beta, tc0 );
}

/* deblock.c:137-150 — deblock_edge_chroma_c. */
static void phx_db_edge_chroma_c(uint8_t *pix, intptr_t xstride,
                                 int alpha, int beta, int8_t tc)
{
    int p1 = pix[-2*xstride];
    int p0 = pix[-1*xstride];
    int q0 = pix[ 0*xstride];
    int q1 = pix[ 1*xstride];

    if( abs( p0 - q0 ) < alpha && abs( p1 - p0 ) < beta && abs( q1 - q0 ) < beta )
    {
        int delta = phx_db_clip3( (((q0 - p0 ) * 4) + (p1 - q1) + 4) >> 3, -tc, tc );
        pix[-1*xstride] = phx_db_clip_pixel( p0 + delta );    /* p0' */
        pix[ 0*xstride] = phx_db_clip_pixel( q0 - delta );    /* q0' */
    }
}

/* deblock.c:151-165 — deblock_chroma_c. */
static void phx_db_chroma_c(uint8_t *pix, int height, intptr_t xstride,
                            intptr_t ystride, int alpha, int beta,
                            const int8_t *tc0)
{
    for( int i = 0; i < 4; i++ )
    {
        int tc = tc0[i];
        if( tc <= 0 )
        {
            pix += height*ystride;
            continue;
        }
        for( int d = 0; d < height; d++, pix += ystride-2 )
            for( int e = 0; e < 2; e++, pix++ )
                phx_db_edge_chroma_c( pix, xstride, alpha, beta, tc0[i] );
    }
}

/* deblock.c:166-181 — chroma wrappers (4:2:0 NV12 byte strides). */
/* SHADER: B — edge-parallel within the MB. */
void phx_deblock_h_chroma_mbaff_c(uint8_t *pix, intptr_t stride,
                                  int alpha, int beta, const int8_t tc0[4])
{
    phx_db_chroma_c( pix, 1, 2, stride, alpha, beta, tc0 );
}
/* SHADER: B — edge-parallel within the MB. */
void phx_deblock_v_chroma_c(uint8_t *pix, intptr_t stride,
                            int alpha, int beta, const int8_t tc0[4])
{
    phx_db_chroma_c( pix, 2, stride, 2, alpha, beta, tc0 );
}
/* SHADER: B — edge-parallel within the MB. */
void phx_deblock_h_chroma_c(uint8_t *pix, intptr_t stride,
                            int alpha, int beta, const int8_t tc0[4])
{
    phx_db_chroma_c( pix, 2, 2, stride, alpha, beta, tc0 );
}

/* ------------------------------------------------------------------ *
 * Intra edge filters, deblock.c:183-275
 * ------------------------------------------------------------------ */

/* deblock.c:183-221 — deblock_edge_luma_intra_c. */
static void phx_db_edge_luma_intra_c(uint8_t *pix, intptr_t xstride,
                                     int alpha, int beta)
{
    int p2 = pix[-3*xstride];
    int p1 = pix[-2*xstride];
    int p0 = pix[-1*xstride];
    int q0 = pix[ 0*xstride];
    int q1 = pix[ 1*xstride];
    int q2 = pix[ 2*xstride];

    if( abs( p0 - q0 ) < alpha && abs( p1 - p0 ) < beta && abs( q1 - q0 ) < beta )
    {
        if( abs( p0 - q0 ) < ((alpha >> 2) + 2) )
        {
            if( abs( p2 - p0 ) < beta ) /* p0', p1', p2' */
            {
                const int p3 = pix[-4*xstride];
                pix[-1*xstride] = (uint8_t)(( p2 + 2*p1 + 2*p0 + 2*q0 + q1 + 4 ) >> 3);
                pix[-2*xstride] = (uint8_t)(( p2 + p1 + p0 + q0 + 2 ) >> 2);
                pix[-3*xstride] = (uint8_t)(( 2*p3 + 3*p2 + p1 + p0 + q0 + 4 ) >> 3);
            }
            else /* p0' */
                pix[-1*xstride] = (uint8_t)(( 2*p1 + p0 + q1 + 2 ) >> 2);
            if( abs( q2 - q0 ) < beta ) /* q0', q1', q2' */
            {
                const int q3 = pix[3*xstride];
                pix[0*xstride] = (uint8_t)(( p1 + 2*p0 + 2*q0 + 2*q1 + q2 + 4 ) >> 3);
                pix[1*xstride] = (uint8_t)(( p0 + q0 + q1 + q2 + 2 ) >> 2);
                pix[2*xstride] = (uint8_t)(( 2*q3 + 3*q2 + q1 + q0 + p0 + 4 ) >> 3);
            }
            else /* q0' */
                pix[0*xstride] = (uint8_t)(( 2*q1 + q0 + p1 + 2 ) >> 2);
        }
        else /* p0', q0' */
        {
            pix[-1*xstride] = (uint8_t)(( 2*p1 + p0 + q1 + 2 ) >> 2);
            pix[ 0*xstride] = (uint8_t)(( 2*q1 + q0 + p1 + 2 ) >> 2);
        }
    }
}

/* deblock.c:222-239 — luma intra wrappers. */
static void phx_db_luma_intra_c(uint8_t *pix, intptr_t xstride,
                                intptr_t ystride, int alpha, int beta)
{
    for( int d = 0; d < 16; d++, pix += ystride )
        phx_db_edge_luma_intra_c( pix, xstride, alpha, beta );
}
/* SHADER: B — edge-parallel within the MB. */
void phx_deblock_h_luma_intra_mbaff_c(uint8_t *pix, intptr_t ystride,
                                      int alpha, int beta)
{
    for( int d = 0; d < 8; d++, pix += ystride )
        phx_db_edge_luma_intra_c( pix, 1, alpha, beta );
}
/* SHADER: B — edge-parallel within the MB. */
void phx_deblock_v_luma_intra_c(uint8_t *pix, intptr_t stride,
                                int alpha, int beta)
{
    phx_db_luma_intra_c( pix, stride, 1, alpha, beta );
}
/* SHADER: B — edge-parallel within the MB. */
void phx_deblock_h_luma_intra_c(uint8_t *pix, intptr_t stride,
                                int alpha, int beta)
{
    phx_db_luma_intra_c( pix, 1, stride, alpha, beta );
}

/* deblock.c:241-275 — chroma intra. */
static void phx_db_edge_chroma_intra_c(uint8_t *pix, intptr_t xstride,
                                       int alpha, int beta)
{
    int p1 = pix[-2*xstride];
    int p0 = pix[-1*xstride];
    int q0 = pix[ 0*xstride];
    int q1 = pix[ 1*xstride];

    if( abs( p0 - q0 ) < alpha && abs( p1 - p0 ) < beta && abs( q1 - q0 ) < beta )
    {
        pix[-1*xstride] = (uint8_t)((2*p1 + p0 + q1 + 2) >> 2);   /* p0' */
        pix[ 0*xstride] = (uint8_t)((2*q1 + q0 + p1 + 2) >> 2);   /* q0' */
    }
}
static void phx_db_chroma_intra_c(uint8_t *pix, int width, int height,
                                  intptr_t xstride, intptr_t ystride,
                                  int alpha, int beta)
{
    for( int d = 0; d < height; d++, pix += ystride-2 )
        for( int e = 0; e < width; e++, pix++ )
            phx_db_edge_chroma_intra_c( pix, xstride, alpha, beta );
}
/* SHADER: B — edge-parallel within the MB. */
void phx_deblock_h_chroma_intra_mbaff_c(uint8_t *pix, intptr_t stride,
                                        int alpha, int beta)
{
    phx_db_chroma_intra_c( pix, 2, 4, 2, stride, alpha, beta );
}
/* SHADER: B — edge-parallel within the MB. */
void phx_deblock_v_chroma_intra_c(uint8_t *pix, intptr_t stride,
                                  int alpha, int beta)
{
    phx_db_chroma_intra_c( pix, 1, 16, stride, 2, alpha, beta );
}
/* SHADER: B — edge-parallel within the MB. */
void phx_deblock_h_chroma_intra_c(uint8_t *pix, intptr_t stride,
                                  int alpha, int beta)
{
    phx_db_chroma_intra_c( pix, 2, 8, 2, stride, alpha, beta );
}

/* ------------------------------------------------------------------ *
 * Boundary strength, deblock.c:277-304 / macroblock.c:1438-1622
 * ------------------------------------------------------------------ */

/* deblock.c:277-304 — deblock_strength_c over explicit arrays. */
static void phx_db_strength(const uint8_t nnz[120],
                            const int8_t ref[2][40],
                            const int16_t mv[2][40][2],
                            uint8_t bs[2][8][4], int mvy_limit, int bframe)
{
    for( int dir = 0; dir < 2; dir++ )
    {
        int s1 = dir ? 1 : 8;
        int s2 = dir ? 8 : 1;
        for( int edge = 0; edge < 4; edge++ )
            for( int i = 0, loc = phx_scan8[0]+edge*s2; i < 4; i++, loc += s1 )
            {
                int locn = loc - s2;
                if( nnz[loc] || nnz[locn] )
                    bs[dir][edge][i] = 2;
                else if( ref[0][loc] != ref[0][locn] ||
                         abs( mv[0][loc][0] - mv[0][locn][0] ) >= 4 ||
                         abs( mv[0][loc][1] - mv[0][locn][1] ) >= mvy_limit ||
                         (bframe && (ref[1][loc] != ref[1][locn] ||
                          abs( mv[1][loc][0] - mv[1][locn][0] ) >= 4 ||
                          abs( mv[1][loc][1] - mv[1][locn][1] ) >= mvy_limit )))
                {
                    bs[dir][edge][i] = 1;
                }
                else
                    bs[dir][edge][i] = 0;
            }
    }
}

/* SHADER: B — edge-parallel bs gather from the shared per-MB cache. */
void phx_deblock_strength_c(const phx_mb_cache_t *c, uint8_t bs[2][8][4],
                            int mvy_limit, int bframe)
{
    phx_db_strength( c->non_zero_count, c->ref, c->mv, bs, mvy_limit, bframe );
}

static int phx_db_is_intra(int type)
{
    return type == PHX_I_4x4 || type == PHX_I_8x8 ||
           type == PHX_I_16x16 || type == PHX_I_PCM;
}
/* x264's D_16x16 partition for the ported inter types (macroblock.h:139). */
static int phx_db_is_16x16(int type)
{
    return type == PHX_P_L0 || type == PHX_P_SKIP;
}

/* macroblock.c:1438-1622, progressive subset.  `disable` == 2 keeps the
 * encode-time neighbour flags; MBAFF (:1364-1436) is not used. */
/* SHADER: B — one workgroup derives its eight bs edge arrays. */
void phx_macroblock_deblock_strength(const phx_mb_state_t *s,
                                     const phx_mb_cache_t *c,
                                     uint8_t bs[2][8][4], int b_cabac,
                                     int disable_deblocking_filter_idc,
                                     int b_transform_8x8_mode)
{
    int xy = c->i_mb_xy;
    memset( bs, 0, sizeof(uint8_t[2][8][4]) );
    (void)disable_deblocking_filter_idc; /* no slice-edge reload in this port */

    if( phx_db_is_intra( s->type[xy] ) )             /* macroblock.c:1443-1446 */
    {
        for( int k = 0; k < 4; k++ )
            bs[0][1][k] = bs[1][1][k] = bs[0][2][k] = bs[1][2][k] = 3;
        return;
    }

    if( c->b_transform_8x8 )                        /* macroblock.c:1451-1464 */
    {
        int cbp_mask = 0xf >> PHX_DB_CHROMA_V_SHIFT;
        if( (c->i_cbp_luma & cbp_mask) == cbp_mask )
        {
            /* macroblock.c:1456-1461: bs[0][0/2/4], all of bs[1][0..4]. */
            const int e0[] = { 0, 2, 4 };
            for( int k = 0; k < 3; k++ )
                bs[0][e0[k]][0] = bs[0][e0[k]][1] =
                bs[0][e0[k]][2] = bs[0][e0[k]][3] = 2;
            for( int e = 0; e < 5; e++ )
                bs[1][e][0] = bs[1][e][1] = bs[1][e][2] = bs[1][e][3] = 2;
            return;
        }
    }

    /* macroblock.c:1568-1615 — CAVLC + 8x8dct nnz munging.  M16/M32 are
     * replaced by explicit byte ORs over the same nnz[48] layout. */
    uint8_t nnz[120];
    memcpy( nnz, c->non_zero_count, sizeof(nnz) );
    if( !b_cabac && b_transform_8x8_mode )
    {
        int neighbours = c->i_neighbour;
        int top = c->top;
        if( (neighbours & PHX_MB_TOP) && top >= 0 &&
            s->mb_transform_size[top] )
        {
            const uint8_t *nt = s->non_zero_count[top];
            int n0 = (nt[8] | nt[9]) | (nt[12] | nt[13]);  /* :1578 */
            int n1 = (nt[10] | nt[11]) | (nt[14] | nt[15]);/* :1579 */
            int i8 = phx_scan8[0] - 8;                     /* :1577 */
            nnz[i8+0] = nnz[i8+1] = (uint8_t)!!n0;
            nnz[i8+2] = nnz[i8+3] = (uint8_t)!!n1;
        }
        if( neighbours & PHX_MB_LEFT )                     /* :1584-1599 */
        {
            int l0 = c->left[0], l1 = c->left[1];
            if( l0 >= 0 && s->mb_transform_size[l0] )
            {
                const uint8_t *nl = s->non_zero_count[l0];
                int n0 = (nl[2] | nl[3]) | (nl[6] | nl[7]);
                int i8 = phx_scan8[0] - 1;
                nnz[i8+0*8] = nnz[i8+1*8] = (uint8_t)!!n0;
            }
            if( l1 >= 0 && s->mb_transform_size[l1] )
            {
                const uint8_t *nl = s->non_zero_count[l1];
                int n1 = (nl[10] | nl[11]) | (nl[14] | nl[15]);
                int i8 = phx_scan8[0] - 1;
                nnz[i8+2*8] = nnz[i8+3*8] = (uint8_t)!!n1;
            }
        }
        if( c->b_transform_8x8 )                           /* :1601-1614 */
        {
            const int b[] = { 0, 2, 4, 6, 8, 10, 12, 14 };
            int n[4];
            for( int k = 0; k < 4; k++ )
                n[k] = (nnz[phx_scan8[b[2*k]]] | nnz[phx_scan8[b[2*k]]+1]) |
                       (nnz[phx_scan8[b[2*k+1]]] | nnz[phx_scan8[b[2*k+1]]+1]);
            for( int j = 0; j < 4; j++ )
            {
                int val = !!n[j >> 1];
                int base = phx_scan8[0] + 8*j;
                nnz[base+0] = nnz[base+1] = nnz[base+2] = nnz[base+3] = (uint8_t)val;
            }
        }
    }

    phx_db_strength( nnz, c->ref, c->mv, bs, 4, s->slice_type == 1 );
}

/* ------------------------------------------------------------------ *
 * Edge dispatch, deblock.c:306-338
 * ------------------------------------------------------------------ */

/* SHADER: B — one edge of the MB workgroup, seek-safe from its arguments. */
void phx_deblock_edge(uint8_t *pix, intptr_t stride, const uint8_t bs[4],
                      int qp, int a, int b, int b_chroma, int intra, int dir)
{
    int index_a = qp + a;
    int index_b = qp + b;
    int alpha = phx_deblock_alpha(index_a);
    int beta  = phx_deblock_beta(index_b);
    int8_t tc[4];

    if( intra )
    {
        if( !alpha || !beta )
            return;
        if( b_chroma )
        {
            if( dir ) phx_deblock_v_chroma_intra_c( pix, stride, alpha, beta );
            else      phx_deblock_h_chroma_intra_c( pix, stride, alpha, beta );
        }
        else
        {
            if( dir ) phx_deblock_v_luma_intra_c( pix, stride, alpha, beta );
            else      phx_deblock_h_luma_intra_c( pix, stride, alpha, beta );
        }
        return;
    }

    if( !bs[0] && !bs[1] && !bs[2] && !bs[3] ) /* x264 M32(bS) == 0 */
        return;
    if( !alpha || !beta )
        return;

    for( int k = 0; k < 4; k++ )
        tc[k] = (int8_t)(phx_deblock_tc0(index_a, bs[k]) + b_chroma);

    if( b_chroma )
    {
        if( dir ) phx_deblock_v_chroma_c( pix, stride, alpha, beta, tc );
        else      phx_deblock_h_chroma_c( pix, stride, alpha, beta, tc );
    }
    else
    {
        if( dir ) phx_deblock_v_luma_c( pix, stride, alpha, beta, tc );
        else      phx_deblock_h_luma_c( pix, stride, alpha, beta, tc );
    }
}

/* ------------------------------------------------------------------ *
 * Per-MB edge loop, deblock.c:340-606
 * ------------------------------------------------------------------ */

/* deblock.c:340-376 — macroblock_cache_load_neighbours_deblock, progressive
 * branch (MB_INTERLACED == 0). */
static int phx_db_neighbours(const phx_mb_state_t *s, int mb_x, int mb_y,
                             int disable)
{
    int deblock_on_slice_edges = disable != 2;        /* :342 */
    int xy = mb_y*s->mb_stride + mb_x;                /* :345 */
    int top = (mb_y-1)*s->mb_stride + mb_x;           /* :347-348 */
    int n = 0;
    if( mb_x > 0 && (deblock_on_slice_edges ||       /* :370-372 */
        s->slice_table[xy-1] == s->slice_table[xy]) )
        n |= PHX_MB_LEFT;
    if( mb_y > 0 && (deblock_on_slice_edges ||       /* :373-375 */
        s->slice_table[top] == s->slice_table[xy]) )
        n |= PHX_MB_TOP;
    return n;
}

/* macroblock.h:249-270 / encoder.c:1585 / cost.c:106-110, as mirrored in
 * phx_chroma.c:224-231. */
static int phx_db_chroma_qp(int qp, int chroma_offset)
{
    int idx = phx_db_clip3(PHX_CHROMA_QP_BASE_OFFSET + chroma_offset + qp,
                           0, (int)sizeof(phx_chroma_qp_base) - 1);
    return phx_chroma_qp_base[idx] + PHX_DB_MAX(qp - PHX_QP_MAX_SPEC, 0);
}

static int phx_db_qp_at(const phx_mb_state_t *s, const phx_deblock_cfg_t *cfg,
                        int xy, int qp)
{
    (void)s;
    return cfg->qp_array ? cfg->qp_array[xy] : qp;
}

/* The FILTER macro, deblock.c:417-447, for 4:2:0 NV12. */
static void phx_db_filter_edge(uint8_t *pixy, uint8_t *pixuv,
                               intptr_t stridey, intptr_t strideuv,
                               int dir, int edge, const uint8_t bs[4],
                               int qp, int qpc, int a, int b, int intra,
                               int transform_8x8)
{
    if( !(edge & 1) || !transform_8x8 )              /* :420 */
    {
        phx_deblock_edge( pixy + 4*edge*(dir?stridey:1), stridey,
                          bs, qp, a, b, 0, intra, dir );        /* :422-424 */
        if( !(edge & 1) )                            /* CHROMA_420, :434 */
            phx_deblock_edge( pixuv + edge*(dir?2*strideuv:4), strideuv,
                              bs, qpc, a, b, 1, intra, dir );   /* :436-438 */
    }
}

/* SHADER: B — one MB workgroup, edges inside it are independent. */
int phx_frame_deblock_ex(phx_frame_t *f, phx_mb_state_t *s, phx_mb_cache_t *c,
                         int mb_x, int mb_y, int qp, int b_cabac,
                         const phx_deblock_cfg_t *cfg)
{
    if( !f || !s || !c || !cfg ) return -1;
    if( f->i_width <= 0 || f->i_height <= 0 ||
        (f->i_width % 16) || (f->i_height % 16) ) return -1;
    int mb_w = f->i_width / 16, mb_h = f->i_height / 16;
    if( mb_x < 0 || mb_x >= mb_w || mb_y < 0 || mb_y >= mb_h ) return -1;
    if( !f->plane || !f->plane_chroma || f->i_stride < 16 ) return -1;
    if( !s->type || !s->non_zero_count || !s->cbp || !s->mb_transform_size ||
        !s->slice_table || !s->ref[0] || !s->mv[0] ) return -1;
    if( qp < 0 || qp > PHX_QP_MAX ) return -1;       /* common.h:60 QP_MAX */

    int a = cfg->alpha_c0_offset;                    /* deblock.c:381 */
    int b = cfg->beta_offset;                        /* deblock.c:382 */
    int disable = cfg->disable_deblocking_filter_idc;/* deblock.c:342 */
    if( disable == 1 ) return 0;                     /* encoder.c:2417 */

    int xy = mb_y*s->mb_stride + mb_x;
    if( xy < 0 || xy >= s->mb_count || s->field[xy] )
        return -1;                                   /* progressive only */

    /* deblock.c:378-417 — row constants, progressive (MB_INTERLACED == 0). */
    int stridey = f->i_stride;
    int strideuv = f->i_stride_planes[1] > 0 ? f->i_stride_planes[1] : f->i_stride;
    int chroma_height = 16 >> PHX_DB_CHROMA_V_SHIFT; /* 8, deblock.c:388 */
    uint8_t *pixy = f->plane + 16*mb_y*stridey + 16*mb_x;              /* :401 */
    uint8_t *pixuv = f->plane_chroma + chroma_height*mb_y*strideuv
                     + 16*mb_x;                                        /* :402 */
    int transform_8x8 = s->mb_transform_size[xy] != 0;                 /* :397 */
    int intra_cur = phx_db_is_intra( s->type[xy] );                    /* :398 */
    int qpc = phx_db_chroma_qp( qp, cfg->chroma_qp_index_offset );      /* :414-415 */
    int qp_thresh = 15 - PHX_DB_MIN(a, b)                              /* :383 */
                      - PHX_DB_MAX(0, cfg->chroma_qp_index_offset);
    int b8x8_mode = cfg->b_transform_8x8_mode;
    if( !b8x8_mode )                                 /* macroblock.c:1569 */
        for( int i = 0; i < s->mb_count; i++ )
            if( s->mb_transform_size[i] ) { b8x8_mode = 1; break; }
    uint8_t bs[2][8][4];
    phx_macroblock_deblock_strength( s, c, bs, b_cabac, disable,
                                     b8x8_mode );                     /* encoder.c:3057 */
    int first_edge_only = (phx_db_is_16x16(s->type[xy]) &&
                           !s->cbp[xy] && !intra_cur) || qp <= qp_thresh; /* :415 */

    int neighbours = phx_db_neighbours( s, mb_x, mb_y, disable );      /* :394 */

    /* :449-535 — left edge (dir 0, edge 0) with averaged QP. */
    if( neighbours & PHX_MB_LEFT )
    {
        int left = xy - 1;
        int qpl = phx_db_qp_at( s, cfg, left, qp );                    /* :514 */
        int qp_left = (qp + qpl + 1) >> 1;                             /* :515 */
        int qpc_left = (qpc + phx_db_chroma_qp(qpl, cfg->chroma_qp_index_offset) + 1) >> 1; /* :516 */
        int intra_left = phx_db_is_intra( s->type[left] );             /* :517 */
        int intra_deblock = intra_cur || intra_left;                   /* :518 */
        phx_db_filter_edge( pixy, pixuv, stridey, strideuv, 0, 0,
                            bs[0][0], qp_left, qpc_left, a, b,
                            intra_deblock, transform_8x8 );            /* :530-533 */
    }

    /* :536-541 — internal vertical edges (never intra-filtered). */
    if( !first_edge_only )
        for( int edge = 1; edge < 4; edge++ )
            phx_db_filter_edge( pixy, pixuv, stridey, strideuv, 0, edge,
                                bs[0][edge], qp, qpc, a, b, 0, transform_8x8 );

    /* :543-595 — top edge (dir 1, edge 0) with averaged QP. */
    if( neighbours & PHX_MB_TOP )
    {
        int top = (mb_y-1)*s->mb_stride + mb_x;                        /* :571 */
        int qpt = phx_db_qp_at( s, cfg, top, qp );
        int qp_top = (qp + qpt + 1) >> 1;                              /* :572 */
        int qpc_top = (qpc + phx_db_chroma_qp(qpt, cfg->chroma_qp_index_offset) + 1) >> 1; /* :573 */
        int intra_top = phx_db_is_intra( s->type[top] );               /* :574 */
        int intra_deblock = intra_cur || intra_top;                    /* :575 */
        phx_db_filter_edge( pixy, pixuv, stridey, strideuv, 1, 0,
                            bs[1][0], qp_top, qpc_top, a, b,
                            intra_deblock, transform_8x8 );            /* :584-592 */
    }

    /* :597-602 — internal horizontal edges (never intra-filtered). */
    if( !first_edge_only )
        for( int edge = 1; edge < 4; edge++ )
            phx_db_filter_edge( pixy, pixuv, stridey, strideuv, 1, edge,
                                bs[1][edge], qp, qpc, a, b, 0, transform_8x8 );

    return 0;
}

/* SHADER: B — stable entry point; one MB workgroup, edge-parallel within it. */
int phx_frame_deblock(phx_frame_t *f, phx_mb_state_t *s, phx_mb_cache_t *c,
                      int mb_x, int mb_y, int qp, int b_cabac)
{
    phx_deblock_cfg_t cfg;
    memset( &cfg, 0, sizeof(cfg) );     /* x264 default 8-bit slice offsets */
    return phx_frame_deblock_ex( f, s, c, mb_x, mb_y, qp, b_cabac, &cfg );
}
