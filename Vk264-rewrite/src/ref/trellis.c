/* SPDX-License-Identifier: GPL-2.0-or-later
 * CABAC trellis quantization, ported from x264 encoder/rdo.c (C fallback).
 * Keeps the same algorithm/structure; adapted to oracle types (int dct). */
#include "trellis.h"
#include "cabac_tables.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define TRELLIS_SCORE_MAX  (~0ULL)
#define TRELLIS_SCORE_BIAS (1ULL<<60)
#define CABAC_SIZE_BITS 8
#define LAMBDA_BITS 4

/* x264_cabac_entropy[128] from common/tables.c */
static const uint16_t vk_cabac_entropy[128] = {
    7,1469,7,1449,8,1430,8,1411,9,1392,9,1372,10,1353,10,1334,
    11,1315,11,1295,12,1276,12,1257,13,1238,14,1218,15,1199,15,1180,
    16,1161,17,1141,18,1122,19,1103,20,1084,21,1064,22,1045,24,1026,
    25,1007,26,987,28,968,29,949,31,930,33,910,35,891,37,872,
    39,853,41,833,43,814,46,795,48,776,51,756,54,737,57,718,
    60,699,64,679,68,660,72,641,76,622,80,602,85,583,90,564,
    96,545,102,525,108,506,115,487,122,468,130,448,139,429,148,410,
    157,391,168,371,180,352,192,333,206,314,221,295,238,275,256,256,
};

/* Transition and size tables for abs<9 MVD/residual coding */
static uint8_t  vk_cabac_transition_unary[15][128];
static uint16_t vk_cabac_size_unary[15][128];
static uint8_t  cabac_transition_5ones[128];
static uint16_t cabac_size_5ones[128];
static int trellis_inited = 0;

/* x264 common/cabac.h:108-124: the state byte is used directly as the
 * transition/entropy table index (0..127), NOT state>>1. */
static inline int vk_cabac_size_decision2(uint8_t *state, int b)
{
    int i_state = *state;
    *state = vk_cabac_transition[i_state][b];
    return vk_cabac_entropy[i_state ^ b];
}
static inline int vk_cabac_size_decision_noup2(const uint8_t *state, int b)
{
    return vk_cabac_entropy[*state ^ b];
}

void vk_trellis_init(void)
{
    if (trellis_inited) return;
    for (int i_prefix = 0; i_prefix < 15; i_prefix++) {
        for (int i_ctx = 0; i_ctx < 128; i_ctx++) {
            int f8_bits = 0;
            uint8_t ctx = (uint8_t)i_ctx;  /* x264_rdo_init: state byte used directly */
            for (int i = 1; i < i_prefix; i++)
                f8_bits += vk_cabac_size_decision2(&ctx, 1);
            if (i_prefix > 0 && i_prefix < 14)
                f8_bits += vk_cabac_size_decision2(&ctx, 0);
            f8_bits += 1 << CABAC_SIZE_BITS;
            vk_cabac_size_unary[i_prefix][i_ctx] = (uint16_t)f8_bits;
            vk_cabac_transition_unary[i_prefix][i_ctx] = ctx;
        }
    }
    for (int i_ctx = 0; i_ctx < 128; i_ctx++) {
        int f8_bits = 0;
        uint8_t ctx = (uint8_t)i_ctx;
        for (int i = 0; i < 5; i++)
            f8_bits += vk_cabac_size_decision2(&ctx, 1);
        f8_bits += 1 << CABAC_SIZE_BITS;
        cabac_size_5ones[i_ctx] = (uint16_t)f8_bits;
        cabac_transition_5ones[i_ctx] = ctx;
    }
    trellis_inited = 1;
}

typedef struct {
    uint64_t score;
    int level_idx;
    uint8_t cabac_state[4];
} trellis_node_t;

typedef struct {
    uint16_t next;
    uint16_t abs_level;
} trellis_level_t;

#define SIGN(x,y) ((x^((y)>>31))-((y)>>31))
#define X264_MIN(a,b) ((a)<(b)?(a):(b))

static inline uint32_t pack16to32(uint32_t a, uint32_t b){ return b<<16|a; }
static inline uint32_t M32_read(const void *p){ uint32_t v; memcpy(&v,p,4); return v; }
static inline void M32_write(void *p, uint32_t v){ memcpy(p,&v,4); }

#define SET_LEVEL(ndst, nsrc, l) do { \
    if (sizeof(trellis_level_t) == sizeof(uint32_t)) \
        M32_write(&level_tree[levels_used], pack16to32((uint32_t)(nsrc).level_idx, (uint32_t)(l))); \
    else \
        level_tree[levels_used] = (trellis_level_t){ (uint16_t)(nsrc).level_idx, (uint16_t)(l) }; \
    (ndst).level_idx = levels_used; \
    levels_used++; \
} while(0)

#define COPY2_IF_LT(x,y,a,b) do { if((y)<(x)){(x)=(y);(a)=(b);} } while(0)

static int bs_size_ue_big(unsigned val)
{
    int n = 0;
    val++;
    while (val > 0xFFFF) { n += 32; val >>= 16; }
    if (val > 0xFF) { n += 16; val >>= 8; }
    /* simple log2 */
    int lz = 0; unsigned v = val;
    while (v > 1) { v >>= 1; lz++; }
    return n + 2*lz + 1;
}

/* trellis_dc_shortcut */
static int trellis_dc_shortcut(int sign_coef, int quant_coef, int deq0, int qp, int coef_weight, int lambda2,
                               uint8_t *cabac_state, int cost_sig)
{
    uint64_t bscore = TRELLIS_SCORE_MAX;
    int ret = 0;
    int q = abs(quant_coef);
    (void)qp;
    int unquant_mf = deq0 << 6;  /* x264 rdo.c: unquant_mf = dequant4_mf<<6 */
    for (int abs_level = q-1; abs_level <= q; abs_level++) {
        int unquant_abs_level = (unquant_mf * abs_level + 128) >> 8;
        int d = sign_coef - ((SIGN(unquant_abs_level, sign_coef) + 8) & ~15);
        uint64_t score = (uint64_t)((int64_t)d*d * coef_weight);
        unsigned f8_bits = cost_sig;  /* x264 rdo.c:480 includes sig/last cost */
        if (abs_level) {
            int prefix = X264_MIN(abs_level - 1, 14);
            f8_bits += vk_cabac_size_decision_noup2(cabac_state+1, prefix > 0);
            f8_bits += vk_cabac_size_unary[prefix][cabac_state[5]];
            if (abs_level >= 15)
                f8_bits += bs_size_ue_big((unsigned)(abs_level - 15)) << CABAC_SIZE_BITS;
            score += (uint64_t)f8_bits * lambda2 >> (CABAC_SIZE_BITS - LAMBDA_BITS);
        }
        if (score < bscore) { bscore = score; ret = abs_level; }
    }
    return SIGN(ret, sign_coef);
}


/* encode one value of one coef in one context */
static inline int trellis_coef(int j, int const_level, int abs_level, int prefix, int suffix_cost,
                               int node_ctx, int level1_ctx, int levelgt1_ctx, uint64_t ssd, const int cost_siglast[3],
                               trellis_node_t *nodes_cur, trellis_node_t *nodes_prev,
                               trellis_level_t *level_tree, int levels_used, int lambda2, uint8_t *level_state)
{
    uint64_t score = nodes_prev[j].score + ssd;
    unsigned f8_bits = cost_siglast[j ? 1 : 2];
    uint8_t level1_state = (j >= 3) ? nodes_prev[j].cabac_state[level1_ctx>>2] : level_state[level1_ctx];
    f8_bits += vk_cabac_entropy[level1_state ^ (const_level > 1 ? 1 : 0)];
    uint8_t levelgt1_state = 0;
    if (const_level > 1) {
        levelgt1_state = j >= 6 ? nodes_prev[j].cabac_state[levelgt1_ctx-6] : level_state[levelgt1_ctx];
        f8_bits += vk_cabac_size_unary[prefix][levelgt1_state] + suffix_cost;
    } else
        f8_bits += 1 << CABAC_SIZE_BITS;
    score += (uint64_t)f8_bits * lambda2 >> (CABAC_SIZE_BITS - LAMBDA_BITS);
    if (score < nodes_cur[node_ctx].score) {
        nodes_cur[node_ctx].score = score;
        if (j == 2 || (j <= 3 && node_ctx == 4))
            M32_write(nodes_cur[node_ctx].cabac_state, M32_read(level_state+12));
        else if (j >= 3)
            M32_write(nodes_cur[node_ctx].cabac_state, M32_read(nodes_prev[j].cabac_state));
        if (j >= 3)
            nodes_cur[node_ctx].cabac_state[level1_ctx>>2] = vk_cabac_transition[level1_state][(const_level > 1) ? 1 : 0];
        if (const_level > 1 && node_ctx == 7)
            nodes_cur[node_ctx].cabac_state[levelgt1_ctx-6] = vk_cabac_transition_unary[prefix][levelgt1_state];
        nodes_cur[node_ctx].level_idx = nodes_prev[j].level_idx;
        SET_LEVEL(nodes_cur[node_ctx], nodes_prev[j], abs_level);
    }
    return levels_used;
}

#define COEF(const_level, ctx_hi, j, ...) \
    if( !(j) || (int64_t)nodes_prev[j].score >= 0 ) \
        levels_used = trellis_coef( j, const_level, abs_level, prefix, suffix_cost, __VA_ARGS__, \
                                    (j)?ssd1:ssd0, cost_siglast, nodes_cur, nodes_prev, \
                                    level_tree, levels_used, lambda2, level_state ); \
    else if( !(ctx_hi) ) \
        return levels_used;

static int trellis_coef0_0(uint64_t ssd0, trellis_node_t *nodes_cur, trellis_node_t *nodes_prev,
                           trellis_level_t *level_tree, int levels_used)
{
    nodes_cur[0].score = nodes_prev[0].score + ssd0;
    nodes_cur[0].level_idx = nodes_prev[0].level_idx;
    for (int j = 1; j < 4 && (int64_t)nodes_prev[j].score >= 0; j++) {
        nodes_cur[j].score = nodes_prev[j].score;
        if (j >= 3) M32_write(nodes_cur[j].cabac_state, M32_read(nodes_prev[j].cabac_state));
        SET_LEVEL(nodes_cur[j], nodes_prev[j], 0);
    }
    return levels_used;
}

static int trellis_coef0_1(uint64_t ssd0, trellis_node_t *nodes_cur, trellis_node_t *nodes_prev,
                           trellis_level_t *level_tree, int levels_used)
{
    for (int j = 1; j < 8; j++) {
        if ((int64_t)nodes_prev[j].score >= 0) {
            nodes_cur[j].score = nodes_prev[j].score;
            if (j >= 3) M32_write(nodes_cur[j].cabac_state, M32_read(nodes_prev[j].cabac_state));
            SET_LEVEL(nodes_cur[j], nodes_prev[j], 0);
        }
    }
    return levels_used;
}

static int trellis_coef1_0(uint64_t ssd0, uint64_t ssd1, const int cost_siglast[3],
                           trellis_node_t *nodes_cur, trellis_node_t *nodes_prev,
                           trellis_level_t *level_tree, int levels_used, int lambda2,
                           uint8_t *level_state)
{
    int abs_level = 1, prefix = 1, suffix_cost = 0;
    (void)ssd0; (void)ssd1;
    COEF(1,0,0, 1,1,0)
    COEF(1,0,1, 2,2,0)
    COEF(1,0,2, 3,3,0)
    COEF(1,0,3, 3,4,0)
    return levels_used;
}

static int trellis_coef1_1(uint64_t ssd0, uint64_t ssd1, const int cost_siglast[3],
                           trellis_node_t *nodes_cur, trellis_node_t *nodes_prev,
                           trellis_level_t *level_tree, int levels_used, int lambda2,
                           uint8_t *level_state)
{
    int abs_level = 1, prefix = 1, suffix_cost = 0;
    (void)ssd0; (void)ssd1;
    COEF(1,1,1, 2,2,0)
    COEF(1,1,2, 3,3,0)
    COEF(1,1,3, 3,4,0)
    COEF(1,1,4, 4,0,0)
    COEF(1,1,5, 5,0,0)
    COEF(1,1,6, 6,0,0)
    COEF(1,1,7, 7,0,0)
    return levels_used;
}

static int trellis_coefn_0(int abs_level, uint64_t ssd0, uint64_t ssd1, const int cost_siglast[3],
                           trellis_node_t *nodes_cur, trellis_node_t *nodes_prev,
                           trellis_level_t *level_tree, int levels_used, int lambda2,
                           uint8_t *level_state, int levelgt1_ctx)
{
    int prefix = X264_MIN(abs_level-1, 14);
    int suffix_cost = abs_level >= 15 ? bs_size_ue_big((unsigned)(abs_level - 15)) << CABAC_SIZE_BITS : 0;
    (void)ssd0; (void)ssd1;
    COEF(2,0,0, 4,1,5)
    COEF(2,0,1, 4,2,5)
    COEF(2,0,2, 4,3,5)
    COEF(2,0,3, 4,4,5)
    return levels_used;
}

static int trellis_coefn_1(int abs_level, uint64_t ssd0, uint64_t ssd1, const int cost_siglast[3],
                           trellis_node_t *nodes_cur, trellis_node_t *nodes_prev,
                           trellis_level_t *level_tree, int levels_used, int lambda2,
                           uint8_t *level_state, int levelgt1_ctx)
{
    int prefix = X264_MIN(abs_level-1, 14);
    int suffix_cost = abs_level >= 15 ? bs_size_ue_big((unsigned)(abs_level - 15)) << CABAC_SIZE_BITS : 0;
    (void)ssd0; (void)ssd1;
    COEF(2,1,1, 4,2,5)
    COEF(2,1,2, 4,3,5)
    COEF(2,1,3, 4,4,5)
    COEF(2,1,4, 5,0,6)
    COEF(2,1,5, 6,0,7)
    COEF(2,1,6, 7,0,8)
    COEF(2,1,7, 7,0,levelgt1_ctx)
    return levels_used;
}


/* x264 common/macroblock.h: x264_zigzag_scan4[0] (frame) — required here
 * because quant_coefs are stored in zigzag order. The previous permutation
 * was a different zigzag variant and scrambled level-tree positions. */
static const uint8_t zigzag4[16] = {0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15};


static const int trellis_lambda2_inter[64] = {
    46,58,73,92,117,147,185,233,294,370,466,587,740,932,1174,1480,1864,2349,2959,3728,4697,5918,7457,9395,11837,14914,18790,23674,29828,37581,47349,59656,75163,94699,119313,150326,189399,238627,300652,378798,477255,601304,757596,954511,1202608,1515192,1909022,2405217,3030384,3818045,4810435,6060769,7636091,9620872,12121539,15272182,19241743,24243077,30544363,38483486,48486154,61088726,76966972,96972308
};
static const int trellis_lambda2_intra[64] = {
    27,34,43,54,68,86,108,136,172,216,273,343,433,545,687,865,1090,1374,1731,2180,2747,3461,4361,5494,6922,8721,10988,13844,17442,21976,27688,34885,43953,55377,69771,87906,110755,139543,175813,221511,279087,351627,443023,558174,703255,886046,1116348,1406511,1772093,2232697,2813022,3544186,4465396,5626046,7088374,8930791,11252092,14176748,17861583,22504184,28353495,35723165,45008368,56706990
};
int vk_trellis_lambda2(int qp, int b_intra) { if(qp<0)qp=0; if(qp>63)qp=63; return b_intra ? trellis_lambda2_intra[qp] : trellis_lambda2_inter[qp]; }
static const uint32_t dct4_weight2_tab[16] = {
    800,320,800,320, 320,128,320,128, 800,320,800,320, 320,128,320,128
};

/* x264 zigzag scan: scan_4x4 gives quant_coefs[i]=dct[zigzag4[i]] */
static void scan4(const int dct[16], int out[16]) { for(int i=0;i<16;i++) out[i]=dct[zigzag4[i]]; }

static inline int coeff_last4(const int q[16], int b_ac)
{
    int last = b_ac - 1;
    for (int i = b_ac; i < 16; i++) if (q[i]) last = i;
    return last;
}

int vk_trellis_quant_4x4(int dct[16], const int orig[16],
                         const int mf[16], const int bias[16],
                         const int deq[16], int qp,
                         int lambda2, int b_ac,
                         uint8_t *cabac_state, int ctx_block_cat,
                         int b_interlaced)
{
    vk_trellis_init();
    int orig_coefs[16], quant_coefs[16];
    memcpy(orig_coefs, orig, sizeof(orig_coefs));

    /* deadzone quantize -> scan */
    int nz = 0;
    int is_dc_block = (ctx_block_cat == 1);
    for (int i = b_ac; i < 16; i++) {
        int sign = orig[i] < 0 ? -1 : 1;
        int a = abs(orig[i]);
        int mfi = is_dc_block ? (mf[0]>>1) : mf[i];
        /* x264 trellis uses quant4_bias0 = (1<<15)/mf (rdo.c:1149) */
        int bii = mfi ? ((1<<15)/mfi) : 0;
        int qv = ((a + bii) * mfi) >> 16;  /* x264 quant_one: (bias+abs)*mf>>16 */
        dct[i] = qv ? qv * sign : 0;
        nz |= dct[i] != 0;
    }
    if (b_ac) dct[0] = 0;
    scan4(dct, quant_coefs);

    int last_nnz = coeff_last4(quant_coefs, b_ac);
    if (last_nnz < b_ac) {
        if (!b_ac) { /* keep DC? caller handles */ }
        /* zero block */
        if (!b_ac) memset(dct, 0, 16*sizeof(int));
        else for (int i=1;i<16;i++) dct[i]=0;
        return 0;
    }

    uint8_t *cabac_state_sig  = &cabac_state[vk_significant_coeff_flag_offset[b_interlaced?1:0][ctx_block_cat]];
    uint8_t *cabac_state_last = &cabac_state[vk_last_coeff_flag_offset[b_interlaced?1:0][ctx_block_cat]];
    uint8_t *cabac_state_coef = &cabac_state[vk_coeff_abs_level_m1_offset[ctx_block_cat]];
    int levelgt1_ctx = 9;
    (void)bias;

    /* DC-only shortcut (only when b_ac==0) */
    if (last_nnz == 0 && !b_ac) {
        int cost_sig = vk_cabac_size_decision_noup2(&cabac_state_sig[0], 1)
                     + vk_cabac_size_decision_noup2(&cabac_state_last[0], 1);
        int dq = SIGN((abs(orig_coefs[0])*mf[0]+bias[0])>>16, orig_coefs[0]); /* not quite; use quant_coefs */
        (void)dq;
        dct[0] = trellis_dc_shortcut(orig_coefs[0], quant_coefs[0], deq[0], qp, (int)dct4_weight2_tab[0], lambda2, cabac_state_coef, cost_sig);
        for(int i=1;i<16;i++) dct[i]=0;
        return !!dct[0];
    }

    trellis_level_t level_tree[64*8*2];
    int levels_used = 1;
    trellis_node_t nodes_buf[2][8];
    memset(nodes_buf, 0, sizeof(nodes_buf));
    trellis_node_t *nodes_cur = nodes_buf[0];
    trellis_node_t *nodes_prev = nodes_buf[1];
    trellis_node_t *bnode;
    for (int j = 1; j < 8; j++) nodes_cur[j].score = TRELLIS_SCORE_MAX;
    nodes_cur[0].score = TRELLIS_SCORE_BIAS;
    nodes_cur[0].level_idx = 0;
    level_tree[0].abs_level = 0;
    level_tree[0].next = 0;
    uint8_t level_state[16];
    memset(level_state,0,sizeof(level_state));
    memcpy(level_state, cabac_state_coef, 10);
    level_state[12] = cabac_state_coef[0];
    level_state[13] = cabac_state_coef[4];
    level_state[14] = cabac_state_coef[8];
    level_state[15] = cabac_state_coef[9];

    int i = last_nnz;

#define TRELLIS_LOOP(ctx_hi) \
    for( ; i >= b_ac; i-- ) { \
        if( !quant_coefs[i] ) { \
            if( !(ctx_hi) ) { \
                int sigindex = i; \
                uint64_t cost_sig0 = (uint64_t)vk_cabac_size_decision_noup2(&cabac_state_sig[sigindex], 0) * (uint64_t)lambda2 >> (CABAC_SIZE_BITS - LAMBDA_BITS); \
                nodes_cur[0].score -= cost_sig0; \
            } \
            for( int j = 1; j < ((ctx_hi)?8:4); j++ ) SET_LEVEL(nodes_cur[j], nodes_cur[j], 0); \
            continue; \
        } \
        int sign_coef = orig_coefs[zigzag4[i]]; \
        int abs_coef = abs(sign_coef); \
        int q = abs(quant_coefs[i]); \
        int cost_siglast[3]; \
        { trellis_node_t *tmp = nodes_cur; nodes_cur = nodes_prev; nodes_prev = tmp; } \
        for( int j = (ctx_hi); j < 8; j++ ) nodes_cur[j].score = TRELLIS_SCORE_MAX; \
        if( i < 15 || (ctx_hi) ) { \
            int sigindex = i; int lastindex = i; \
            cost_siglast[0] = vk_cabac_size_decision_noup2(&cabac_state_sig[sigindex], 0); \
            int cost_sig1   = vk_cabac_size_decision_noup2(&cabac_state_sig[sigindex], 1); \
            cost_siglast[1] = vk_cabac_size_decision_noup2(&cabac_state_last[lastindex], 0) + cost_sig1; \
            if( !(ctx_hi) ) cost_siglast[2] = vk_cabac_size_decision_noup2(&cabac_state_last[lastindex], 1) + cost_sig1; \
        } else { cost_siglast[0]=cost_siglast[1]=cost_siglast[2]=0; } \
        uint64_t ssd0[2], ssd1[2]; \
        for( int k = 0; k < 2; k++ ) { \
            int abs_level = q-1+k; \
            /* x264 rdo.c:819 + set.c:170: unquant_mf is derived from the
             * UNSHIFTED quant4_mf: (1<<(qp/6+15+8)) / quant4_mf_unshifted.
             * Oracle mf is SHIFT(quant4_mf_unshifted, qp/6-1), so recover it. */ \
            int is_dc = (ctx_block_cat==1); \
            int mfi = is_dc ? (mf[0]>>1) : mf[zigzag4[i]]; \
            int sh = qp/6 - 1; if (sh < 0) sh = 0; \
            int qmu = mfi << sh; \
            int unquant_mf = qmu ? (int)((1ULL << (qp/6 + 15 + 8)) / qmu) : 0; \
            if (is_dc) unquant_mf *= 2; \
            int unquant_abs_level = (unquant_mf * abs_level + 128) >> 8; \
            int d = abs_coef - unquant_abs_level; \
            int wgt = is_dc ? 256 : (int)dct4_weight2_tab[zigzag4[i]]; \
            ssd1[k] = (uint64_t)((int64_t)d*d * wgt); \
            ssd0[k] = ssd1[k]; \
            if( !i && !b_ac && !(ctx_hi) ) { \
                d = sign_coef - ((SIGN(unquant_abs_level, sign_coef) + 8)&~15); \
                ssd0[k] = (uint64_t)((int64_t)d*d * wgt); \
            } \
        } \
        switch( q ) { \
        case 1: \
            ssd1[0] += (uint64_t)cost_siglast[0] * lambda2 >> (CABAC_SIZE_BITS - LAMBDA_BITS); \
            levels_used = trellis_coef0_##ctx_hi( ssd0[0]-ssd1[0], nodes_cur, nodes_prev, level_tree, levels_used ); \
            levels_used = trellis_coef1_##ctx_hi( ssd0[1]-ssd1[0], ssd1[1]-ssd1[0], cost_siglast, nodes_cur, nodes_prev, level_tree, levels_used, lambda2, level_state ); \
            goto next##ctx_hi; \
        case 2: \
            levels_used = trellis_coef1_##ctx_hi( ssd0[0], ssd1[0], cost_siglast, nodes_cur, nodes_prev, level_tree, levels_used, lambda2, level_state ); \
            levels_used = trellis_coefn_##ctx_hi( q, ssd0[1], ssd1[1], cost_siglast, nodes_cur, nodes_prev, level_tree, levels_used, lambda2, level_state, levelgt1_ctx ); \
            goto next1; \
        default: \
            levels_used = trellis_coefn_##ctx_hi( q-1, ssd0[0], ssd1[0], cost_siglast, nodes_cur, nodes_prev, level_tree, levels_used, lambda2, level_state, levelgt1_ctx ); \
            levels_used = trellis_coefn_##ctx_hi( q, ssd0[1], ssd1[1], cost_siglast, nodes_cur, nodes_prev, level_tree, levels_used, lambda2, level_state, levelgt1_ctx ); \
            goto next1; \
        } \
        next##ctx_hi:; \
    } \
    bnode = &nodes_cur[ctx_hi]; \
    for( int j = (ctx_hi)+1; j < ((ctx_hi)?8:4); j++ ) \
        if( nodes_cur[j].score < bnode->score ) bnode = &nodes_cur[j];

    TRELLIS_LOOP(0);


    if (bnode == &nodes_cur[0]) {
        if (!b_ac) memset(dct, 0, 16*sizeof(int));
        else for(int k=1;k<16;k++) dct[k]=0;
        return 0;
    }

    if (0) { TRELLIS_LOOP(1); }

    {
        int level = bnode->level_idx;
        for (i = b_ac; i <= last_nnz; i++) {
            /* x264 rdo.c:890 uses the sign of the deadzone-quantized dct,
             * not the original coefficient, when restoring trellis levels. */
            dct[zigzag4[i]] = SIGN((int)level_tree[level].abs_level, dct[zigzag4[i]]);
            level = level_tree[level].next;
        }
        /* zero others in AC range / all if not b_ac handled above */
        if (b_ac) dct[0] = 0;
    }

    /* x264 quant_trellis_cabac returns 0/1 (block empty or not), not the
     * number of nonzero coefficients (rdo.c:878/894). */
    for (int k = b_ac; k < 16; k++) if (dct[k]) return 1;
    return 0;
}


int vk_trellis_quant_4x4_dc(int dct[16], const int orig[16],
                            int dcmf, int dcbias, int dcunquant_mf,
                            int lambda2, uint8_t *cabac_state,
                            int b_interlaced)
{
    /* TODO: proper DC trellis; fallback to deadzone for now */
    (void)orig; (void)dcmf; (void)dcbias; (void)dcunquant_mf; (void)lambda2; (void)cabac_state; (void)b_interlaced;
    int nz=0; for(int i=0;i<16;i++) if(dct[i]) nz++; return nz;
}

int vk_trellis_quant_chroma_dc(int dct[4], const int orig[4],
                               int dcmf, int dcbias, int dcunquant_mf,
                               int lambda2, uint8_t *cabac_state,
                               int b_interlaced)
{
    (void)orig; (void)dcmf; (void)dcbias; (void)dcunquant_mf; (void)lambda2; (void)cabac_state; (void)b_interlaced;
    int nz=0; for(int i=0;i<4;i++) if(dct[i]) nz++; return nz;
}
