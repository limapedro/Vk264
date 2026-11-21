/* SPDX-License-Identifier: GPL-2.0-or-later
 * CABAC I-slice writer (I16 macroblocks, zero residual DC), ported from
 * x264 encoder/cabac.c. */
#include "cabac_enc.h"
#include "cabac_tables.h"
#include "intra16.h"
#include "motion.h"
#include "slice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t clip8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

#define LPAD 16
#define CPAD 8

/* x264's CABAC residual coefficient coder (encoder/cabac.c:665). */
static void cabac_coeffs(vk_cabac_t *cb, const int *l, int cat)
{
    int ctx_sig = vk_significant_coeff_flag_offset[0][cat];
    int ctx_last = vk_last_coeff_flag_offset[0][cat];
    int ctx_level = vk_coeff_abs_level_m1_offset[cat];
    static const uint8_t lev1[8] = {1,2,3,4,0,0,0,0};
    static const uint8_t levgt1[8] = {5,5,5,5,6,7,8,9};
    static const uint8_t trans[2][8] = {{1,2,3,3,4,5,6,7},{4,4,4,4,5,6,7,7}};
    int count_m1 = vk_count_cat_m1[cat];
    int coeffs[64], nc = 0, last = -1;
    for (int i = count_m1; i >= 0; --i)
        if (l[i]) { last = i; break; }
    /* x264 WRITE_SIGMAP: index count_m1 is stored implicitly without
     * significance/last flags (reached via ++i == count_m1). */
    int i = 0;
    while (1) {
        if (l[i]) {
            coeffs[nc++] = l[i];
            vk_cabac_decision(cb, ctx_sig+i, 1);
            if (i == last) { vk_cabac_decision(cb, ctx_last+i, 1); break; }
            vk_cabac_decision(cb, ctx_last+i, 0);
        } else
            vk_cabac_decision(cb, ctx_sig+i, 0);
        if (++i == count_m1) { coeffs[nc++] = l[i]; break; }
    }
    int node = 0;
    for (int k = nc-1; k >= 0; --k) {
        int c = coeffs[k], a = c < 0 ? -c : c, sign = c >> 31;
        int ctx = lev1[node] + ctx_level;
        if (a > 1) {
            vk_cabac_decision(cb, ctx, 1);
            ctx = levgt1[node] + ctx_level;
            for (int i = (a < 15 ? a : 15) - 2; i > 0; --i)
                vk_cabac_decision(cb, ctx, 1);
            if (a < 15) vk_cabac_decision(cb, ctx, 0);
            else vk_cabac_ue_bypass(cb, 0, a-15);
            node = trans[1][node];
        } else {
            vk_cabac_decision(cb, ctx, 0);
            node = trans[0][node];
        }
        vk_cabac_bypass(cb, sign);
    }
}

/* x264_cabac_mb_type_intra for I_16x16 in an I-slice. */
static void cabac_mb_type_i16(vk_cabac_t *cb, int ctx_left, int ctx_top,
                              int mode, int cbp_luma, int cbp_chroma)
{
    int ctx = 0;
    if (ctx_left) ctx++;   /* left MB type != I_4x4 */
    if (ctx_top) ctx++;
    int ctx0 = 3 + ctx;
    vk_cabac_decision_noup(cb, ctx0, 1);       /* I16 vs I4x4/I8x8/PCM */
    vk_cabac_terminal(cb);
    vk_cabac_decision_noup(cb, 3+3, !!cbp_luma);
    if (!cbp_chroma)
        vk_cabac_decision_noup(cb, 3+4, 0);
    else {
        vk_cabac_decision(cb, 3+4, 1);
        vk_cabac_decision_noup(cb, 3+5, cbp_chroma >> 1);
    }
    int pred = mode <= 3 ? mode : 2;
    vk_cabac_decision(cb, 3+6, pred >> 1);
    vk_cabac_decision_noup(cb, 3+7, pred & 1);
}

/* x264 cabac_intra_chroma_pred_mode */
static void cabac_chroma_mode(vk_cabac_t *cb, int mode, int ctx)
{
    int m = mode <= 3 ? mode : 0;
    vk_cabac_decision_noup(cb, 64 + ctx, m > 0);
    if (m > 0) {
        vk_cabac_decision(cb, 64+3, m > 1);
        if (m > 1) vk_cabac_decision_noup(cb, 64+3, m > 2);
    }
}

/* x264 cabac_qp_delta with dqp == 0, last_dqp == 0 */
static void cabac_qp_delta0(vk_cabac_t *cb)
{
    vk_cabac_decision_noup(cb, 60, 0);
}

int vk_cabac_encode_pskip(vk_bs_t *bs, vk_cabac_t *cb, int w, int h)
{
    int mw = w/16, mh = h/16;
    uint8_t *data = bs->p;
    vk_cabac_start(cb, data, bs->end);
    /* x264_cabac_mb_skip: ctx = 11 + count(non-skip left/top neighbours).
     * With every MB skipped, that count is 0 -> ctx 11, b=1. */
    for (int i = 0; i < mw*mh; ++i) {
        if (i > 0) vk_cabac_terminal(cb);
        vk_cabac_decision(cb, 11, 1);
    }
    int bytes = vk_cabac_flush(cb, 1);  /* frame 1 */
    bs->p = data + bytes;
    return bytes;
}

/* Compute the I16 luma DC coefficients (zigzag-scanned) for one MB, and
 * reconstruct the MB (prediction + dequantised DC) into recon[256]. */
static void compute_i16_dc(const uint8_t *cur, int w, int x, int y,
                           const uint8_t pred[256], int qp, int q[16],
                           uint8_t recon[256])
{
    int mf[16], bias[16], deq[16];
    vk_ref_quant_tables(qp, 0, mf, bias, deq);
    static const uint8_t zz[16] = {0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15};
    int dc[16];
    for (int b = 0; b < 16; ++b) {
        int by = (b/4)*4, bx = (b%4)*4;
        uint8_t s[16], p[16];
        for (int yy = 0; yy < 4; ++yy)
            for (int xx = 0; xx < 4; ++xx) {
                s[yy*4+xx] = cur[(y+by+yy)*w + x+bx+xx];
                p[yy*4+xx] = pred[(by+yy)*16 + bx+xx];
            }
        int d[16];
        vk_ref_dct4x4(d, s, p);
        dc[b] = d[0];
    }
    vk_ref_dct4x4dc(dc);
    int dcmf = mf[0] >> 1, dcbias = bias[0] << 1;
    int nat[16];
    for (int i = 0; i < 16; ++i) nat[i] = vk_ref_quant_one(dc[i], dcmf, dcbias);
    for (int i = 0; i < 16; ++i) q[i] = nat[zz[i]];
    /* reconstruction: DC-only, dequant + IDCT + add to prediction */
    memcpy(recon, pred, 256);
    if (nat[0] || 1) {
        int rec[16];
        for (int i = 0; i < 16; ++i) rec[i] = nat[i];
        vk_ref_idct4x4dc(rec);
        vk_ref_dequant4dc(rec, deq, qp);
        for (int i = 0; i < 16; ++i)
            vk_ref_add_block_dc(recon + (i/4)*4*16 + (i%4)*4, 16, rec[i]);
    }
}

/* Full I16 luma residual: DC (Hadamard), AC (15 zigzag coeffs per 4x4),
 * cbp_luma with x264's decimation, and reconstruction. */
static void compute_i16_full(const uint8_t *cur, int w, int x, int y,
                             const uint8_t pred[256], int qp,
                             int q[16], int acq[16][15], int nnz[16],
                             int *cbp_luma, uint8_t recon[256])
{
    int mf[16], bias[16], deq[16];
    vk_ref_quant_tables(qp, 0, mf, bias, deq);
    static const uint8_t zz[16] = {0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15};
    int dct[16][16], dc[16];
    for (int b = 0; b < 16; ++b) {
        int by = (b/4)*4, bx = (b%4)*4;
        uint8_t s[16], p[16];
        for (int yy = 0; yy < 4; ++yy)
            for (int xx = 0; xx < 4; ++xx) {
                s[yy*4+xx] = cur[(y+by+yy)*w + x+bx+xx];
                p[yy*4+xx] = pred[(by+yy)*16 + bx+xx];
            }
        vk_ref_dct4x4(dct[b], s, p);
        dc[b] = dct[b][0];
        dct[b][0] = 0;
        for (int i = 0; i < 16; ++i) dct[b][i] = vk_ref_quant_one(dct[b][i], mf[i], bias[i]);
        for (int i = 0; i < 15; ++i) acq[b][i] = dct[b][zz[i+1]];
        nnz[b] = 0;
        for (int i = 0; i < 16; ++i) nnz[b] += dct[b][i] != 0;
    }
    vk_ref_dct4x4dc(dc);
    int dcmf = mf[0]>>1, dcbias = bias[0]<<1;
    int nat[16];
    for (int i = 0; i < 16; ++i) nat[i] = vk_ref_quant_one(dc[i], dcmf, dcbias);
    for (int i = 0; i < 16; ++i) q[i] = nat[zz[i]];
    /* x264 ultrafast has b_dct_decimate=0: decimate_score starts at 9, so
     * no I16 blocks are ever discarded; cbp_luma is 0 or 15. */
    int any_nz = 0;
    for (int b = 0; b < 16; ++b) any_nz |= nnz[b] != 0;
    *cbp_luma = any_nz ? 15 : 0;
    /* reconstruct: DC + AC */
    memcpy(recon, pred, 256);
    int dcq[16];
    for (int i = 0; i < 16; ++i) dcq[i] = nat[i];
    vk_ref_idct4x4dc(dcq);
    vk_ref_dequant4dc(dcq, deq, qp);
    for (int b = 0; b < 16; ++b) {
        int by = (b/4)*4, bx = (b%4)*4;
        uint8_t *dst = recon + by*16+bx;
        if (nnz[b]) {
            int blk[16];
            for (int i = 0; i < 16; ++i) blk[zz[i]] = dct[b][zz[i]];
            vk_ref_dequant4(blk, deq, qp);
            blk[0] = dcq[b];
            vk_ref_idct4(blk);
            vk_ref_add_block(dst, 16, blk);
        } else {
            vk_ref_add_block_dc(dst, 16, dcq[b]);
        }
    }
}

/* Compute the 2x2 chroma DC Hadamard coefficients (zigzag_2x2_dc order)
 * for one 8x8 chroma plane; returns its intra prediction in pred[64]. */
/* x264 optimize_chroma_dc_internal (common/quant.c:254): try reducing each
 * quantized DC coefficient until the dequantised residual no longer rounds
 * to the same pixels. Returns 0 if all DC can be zeroed. */
static int chroma_dc_opt(int dct[4], int dmf)
{
    int orig[4];
    #define CHROMA_DC_OUT(d, o) do { \
        int a=d[0]+d[1], b=d[2]+d[3], c=d[0]-d[1], e=d[2]-d[3]; \
        o[0]=((a+b)*dmf>>5)+32; o[1]=((a-b)*dmf>>5)+32; \
        o[2]=((c+e)*dmf>>5)+32; o[3]=((c-e)*dmf>>5)+32; } while (0)
    CHROMA_DC_OUT(dct, orig);
    int sum = 0;
    for (int i = 0; i < 4; ++i) sum |= orig[i];
    if (!(sum >> 6)) return 0;
    int nz = 0;
    for (int coeff = 3; coeff >= 0; --coeff) {
        int level = dct[coeff];
        int sign = (level >> 31) | 1;
        while (level) {
            dct[coeff] = level - sign;
            int out[4];
            CHROMA_DC_OUT(dct, out);
            int s = 0;
            for (int i = 0; i < 4; ++i) s |= orig[i] ^ out[i];
            if (s >> 6) { nz = 1; dct[coeff] = level; break; }
            level -= sign;
        }
    }
    #undef CHROMA_DC_OUT
    return nz;
}

static void compute_chroma_dc(const uint8_t *src, int cs, int x, int y,
                              const uint8_t top[8], const uint8_t left[8],
                              uint8_t tl, int mode, int qp, int q[4],
                              int acq[4][15], int nnz[4], uint8_t pred[64])
{
    vk_predict_chroma8(pred, top, left, tl, mode);
    int cmf[16], cbias[16], cdeq[16];
    vk_ref_quant_tables(qp, 2, cmf, cbias, cdeq);
    static const uint8_t zz[16] = {0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15};
    int dct[4][16], dc[4];
    for (int b = 0; b < 4; ++b) {
        int by = (b/2)*4, bx = (b%2)*4;
        uint8_t s[16], p[16];
        for (int yy = 0; yy < 4; ++yy)
            for (int xx = 0; xx < 4; ++xx) {
                s[yy*4+xx] = src[(y+by+yy)*cs + x+bx+xx];
                p[yy*4+xx] = pred[(by+yy)*8 + bx+xx];
            }
        vk_ref_dct4x4(dct[b], s, p);
        dc[b] = dct[b][0];
        /* quantize AC (DC zeroed), zigzag-scan into acq[b] */
        dct[b][0] = 0;
        for (int i = 0; i < 16; ++i)
            dct[b][i] = vk_ref_quant_one(dct[b][i], cmf[i], cbias[i]);
        nnz[b] = 0;
        for (int i = 0; i < 16; ++i) nnz[b] += dct[b][i] != 0;
        for (int i = 0; i < 15; ++i) acq[b][i] = dct[b][zz[i+1]];
    }
    int a = dc[0]+dc[1], b = dc[2]+dc[3];
    int c = dc[0]-dc[1], d = dc[2]-dc[3];
    int had[4] = {a+b, a-b, c+d, c-d};
    int dmf = cmf[0] >> 1, dbias = cbias[0] << 1;
    int qt[4];
    for (int i = 0; i < 4; ++i) qt[i] = vk_ref_quant_one(had[i], dmf, dbias);
    /* x264 mb_optimize_chroma_dc: zero out DC that rounds to the same pixels */
    int optmf = cdeq[0] << (qp/6);  /* x264 dequant4_mf already includes *16 */
    if (optmf <= 32*64 && !chroma_dc_opt(qt, optmf))
        for (int i = 0; i < 4; ++i) qt[i] = 0;
    /* x264 zigzag_scan_2x2_dc: {0,2,1,3} */
    q[0]=qt[0]; q[1]=qt[2]; q[2]=qt[1]; q[3]=qt[3];
    /* DC dequant (idct_dequant_2x2_dc / _dconly) */
    /* x264 idct_dequant_2x2_dconly operates on natural-order dct_dc */
    int d0=qt[0]+qt[1], d1=qt[2]+qt[3];
    int d2=qt[0]-qt[1], d3=qt[2]-qt[3];
    int cdmf = cdeq[0] << (qp/6);
    int rr[4] = {((d0+d1)*cdmf)>>5, ((d0-d1)*cdmf)>>5,
                 ((d2+d3)*cdmf)>>5, ((d2-d3)*cdmf)>>5};
    for (int b = 0; b < 4; ++b) {
        int by=(b/2)*4, bx=(b%2)*4;
        uint8_t *dst = pred + by*8+bx;
        if (nnz[b]) {
            /* DC+AC reconstruction: add8x8_idct with quantized AC blocks */
            int blk[16];
            for (int i = 0; i < 16; ++i) blk[zz[i]] = dct[b][zz[i]];
            vk_ref_dequant4(blk, cdeq, qp);
            blk[0] = rr[b];
            vk_ref_idct4(blk);
            for (int yy=0; yy<4; ++yy)
                for (int xx=0; xx<4; ++xx)
                    dst[yy*8+xx] = clip8(dst[yy*8+xx] + blk[yy*4+xx]);
        } else {
            for (int yy=0; yy<4; ++yy)
                for (int xx=0; xx<4; ++xx)
                    dst[yy*8+xx] = clip8(dst[yy*8+xx] + ((rr[b]+32)>>6));
        }
    }
}

int vk_cabac_encode_idr(vk_bs_t *bs, vk_cabac_t *cb,
                        const uint8_t *frame, int w, int h, int qp,
                        int frame_num)
{
    (void)frame_num;
    return vk_cabac_encode_idr_recon(bs, cb, frame, w, h, qp, NULL);
}

int vk_cabac_encode_idr_recon(vk_bs_t *bs, vk_cabac_t *cb,
                              const uint8_t *frame, int w, int h, int qp,
                              uint8_t *recon_out)
{
    int mw = w/16, mh = h/16;
    uint8_t *data = bs->p;
    vk_cabac_start(cb, data, bs->end);

    /* I16 MBs with cbp=0. Reconstruction buffer with a 16-pixel border. */
    int rs = w + 32, ro = 16*rs + 16;
    int cs = w/2 + 16, co = 8*cs + 8;
    size_t ysz = (size_t)rs*(h+32), csz = (size_t)cs*(h/2+16);
    uint8_t *recon = malloc(ysz + 2*csz);
    memset(recon, 128, ysz + 2*csz);
    uint8_t *rcu = recon + ysz, *rcv = rcu + csz;
    int *mbcbp = calloc((size_t)mw*mh, sizeof(int));
    uint8_t (*nnzc)[16] = calloc((size_t)mw*mh, 16);
    uint8_t (*cnnzc)[2][4] = calloc((size_t)mw*mh, 8);
    uint8_t *cmode_fix = calloc((size_t)mw*mh, 1);
    int mbnum = 0;
    for (int my = 0; my < mh; ++my)
        for (int mx = 0; mx < mw; ++mx) {
            if (mbnum++ > 0) vk_cabac_terminal(cb);
            int left = mx > 0, top = my > 0;
            uint8_t *rbase = recon + ro;
            uint8_t t[16], l[16], tl;
            for (int i = 0; i < 16; ++i) {
                t[i] = top ? rbase[(my*16-1)*rs + mx*16+i]
                           : (mx == 0 ? 128 : rbase[(my*16)*rs + mx*16-16+i]);
                l[i] = left ? rbase[(my*16+i)*rs + mx*16-1] : 128;
            }
            tl = top ? rbase[(my*16-1)*rs + mx*16-1] : frame[0];
            uint8_t pred[256], rmb[256], trial[256];
            int lambda = vk_lambda(qp), best = 0x7fffffff, bestmode = 0;
            /* x264 i16x16_mode_available with b_fast_intra=0 (I-slice):
             * thresh=COST_MAX so plane mode 3 is always included. */
            static const int8_t pi16av[5][5] = {
                {6,-1,-1,-1,-1}, {4,1,-1,-1,-1}, {5,0,-1,-1,-1},
                {0,1,2,3,-1},   {0,1,2,3,-1}
            };
            int pmidx = (left&&top) ? ((mx>0&&my>0) ? 4 : 3)
                                    : (left ? 1 : (top ? 2 : 0));
            for (int pk = 0; pi16av[pmidx][pk] >= 0; ++pk) {
                int m = pi16av[pmidx][pk];
                vk_predict16(trial, t, l, tl, m);
                int sad = 0;
                for (int yy = 0; yy < 16; ++yy)
                    for (int xx = 0; xx < 16; ++xx) {
                        int dv = frame[(my*16+yy)*w + mx*16+xx] - trial[yy*16+xx];
                        sad += dv < 0 ? -dv : dv;
                    }
                int c = sad + lambda*vk_mode_bits(m);
                if (c < best) { best = c; bestmode = m; }
            }
            int imode = bestmode;
            vk_predict16(pred, t, l, tl, imode);
            int q[16] = {0}, acq[16][15], nnz[16], cbp_luma = 0;
            compute_i16_full(frame, w, mx*16, my*16, pred, qp,
                             q, acq, nnz, &cbp_luma, rmb);
            int nz = 0;
            for (int i = 0; i < 16; ++i) if (q[i]) nz = 1;
            /* chroma neighbours */
            uint8_t tu[8], lu[8], tv[8], lv[8], tlu, tlv;
            for (int i = 0; i < 8; ++i) {
                tu[i] = top ? rcu[co+(my*8-1)*cs+mx*8+i]
                            : (mx==0 ? 128 : rcu[co+(my*8)*cs+mx*8-8+i]);
                tv[i] = top ? rcv[co+(my*8-1)*cs+mx*8+i]
                            : (mx==0 ? 128 : rcv[co+(my*8)*cs+mx*8-8+i]);
                lu[i] = left ? rcu[co+(my*8+i)*cs+mx*8-1] : 128;
                lv[i] = left ? rcv[co+(my*8+i)*cs+mx*8-1] : 128;
            }
            tlu = top ? rcu[co+(my*8-1)*cs+mx*8-1] : frame[w*h];
            tlv = top ? rcv[co+(my*8-1)*cs+mx*8-1] : frame[w*h+(w/2)*(h/2)];
            /* x264 predict_chroma_mode_available (analyse.c:485). */
            static const int8_t pcav[5][5] = {
                {6,-1,-1,-1,-1}, {4,1,-1,-1,-1}, {5,2,-1,-1,-1},
                {2,1,0,-1,-1},   {2,1,0,3,-1}
            };
            int cavail = (left?1:0)|(top?2:0)|((left&&top&&mx>0&&my>0)?4:0);
            int cidx = (cavail & 3);
            if (cavail == 7) cidx = 4;
            int cmode = 6, cbest = 0x7fffffff;
            uint8_t cp[64];
            const uint8_t *cu = frame + w*h, *cv = cu + (w/2)*(h/2);
            for (int k = 0; pcav[cidx][k] >= 0; ++k) {
                int m = pcav[cidx][k];
                int fix = m <= 3 ? m : 0;
                int sad = 0;
                vk_predict_chroma8(cp, tu, lu, tlu, m);
                for (int yy=0; yy<8; ++yy) for (int xx=0; xx<8; ++xx) {
                    int dv = cu[(my*8+yy)*(w/2)+mx*8+xx] - cp[yy*8+xx];
                    sad += dv<0?-dv:dv;
                }
                vk_predict_chroma8(cp, tv, lv, tlv, m);
                for (int yy=0; yy<8; ++yy) for (int xx=0; xx<8; ++xx) {
                    int dv = cv[(my*8+yy)*(w/2)+mx*8+xx] - cp[yy*8+xx];
                    sad += dv<0?-dv:dv;
                }
                int c = sad + lambda*vk_mode_bits(fix);
                if (c < cbest) { cbest = c; cmode = m; }
            }
            int qu[4], qv[4], acu[4][15], acv[4][15], nnzu[4], nnzv[4];
            uint8_t pu[64], pv[64];
            compute_chroma_dc(cu, w/2, mx*8, my*8, tu, lu, tlu, cmode, qp, qu, acu, nnzu, pu);
            compute_chroma_dc(cv, w/2, mx*8, my*8, tv, lv, tlv, cmode, qp, qv, acv, nnzv, pv);
            int cbp_chroma = 0, has_dc = 0, has_ac = 0;
            for (int i=0;i<4;++i) {
                has_dc |= qu[i] || qv[i];
                has_ac |= nnzu[i] || nnzv[i];
            }
            cbp_chroma = has_ac ? 2 : (has_dc ? 1 : 0);
            int lc = left ? mbcbp[my*mw+mx-1] : 0x7ff;
            int tc = top ? mbcbp[(my-1)*mw+mx] : 0x7ff;
            int nza = (lc >> 8) & 1, nzb = (tc >> 8) & 1;
            int cmode_ctx = (left && cmode_fix[my*mw+mx-1] != 0)
                          + (top  && cmode_fix[(my-1)*mw+mx] != 0);
            cabac_mb_type_i16(cb, left, top, imode, cbp_luma, cbp_chroma);
            cabac_chroma_mode(cb, cmode, cmode_ctx);
            cmode_fix[my*mw+mx] = (uint8_t)(cmode <= 3 ? cmode : 0);
            cabac_qp_delta0(cb);
            vk_cabac_decision(cb, 85 + 2*nzb + nza, nz);
            if (nz) cabac_coeffs(cb, q, 0);
            /* luma AC: 16 4x4 blocks, x264 DCT-index order */
            if (cbp_luma) {
                static const uint8_t bidx[16] = {0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15};
                static const uint8_t s8[16] = {12,13,20,21,14,15,22,23,28,29,36,37,30,31,38,39};
                uint8_t cache[64] = {0};
                for (int i = 0; i < 4; ++i) {
                    cache[11+i*8] = left ? nnzc[my*mw+mx-1][i*4+3] : 0x80;
                    cache[4+i] = top ? nnzc[(my-1)*mw+mx][12+i] : 0x80;
                }
                for (int idx = 0; idx < 16; ++idx) {
                    int b = bidx[idx], p = s8[idx];
                    int za = cache[p-1], zb = cache[p-8];
                    /* x264 with b_intra=1: mask 0xff, then !! */
                    int ctx = 89 + 2*!!(zb & 0xff) + !!(za & 0xff);
                    if (nnz[b]) {
                        vk_cabac_decision(cb, ctx, 1);
                        cabac_coeffs(cb, acq[b], 1);
                    } else
                        vk_cabac_decision(cb, ctx, 0);
                    cache[p] = (uint8_t)nnz[b];
                }
            }
            for (int i = 0; i < 16; ++i) nnzc[my*mw+mx][i] = (uint8_t)nnz[i];
            if (cbp_chroma) {
                int ca = (lc>>9)&1, cb2 = (tc>>9)&1;  /* U DC cbf ctx */
                int nu = 0, nv = 0;
                for (int i = 0; i < 4; ++i) { nu |= qu[i] != 0; nv |= qv[i] != 0; }
                vk_cabac_decision(cb, 97 + 2*cb2 + ca, nu);
                if (nu) cabac_coeffs(cb, qu, 3);
                int cv_a = (lc>>10)&1, cv_b = (tc>>10)&1;
                vk_cabac_decision(cb, 97 + 2*cv_b + cv_a, nv);
                if (nv) cabac_coeffs(cb, qv, 3);
            }
            if (cbp_chroma == 2) {
                /* x264 macroblock_write_cabac_internal: chroma AC cbf at
                 * base 101, scan8 positions 52..61 (U) and 68..77 (V). */
                uint8_t ccache[128] = {0};
                /* x264_scan8: U blocks 52,53,60,61; V blocks 92,93,100,101 */
                ccache[51] = left ? cnnzc[my*mw+mx-1][0][1] : 0x80;
                ccache[59] = left ? cnnzc[my*mw+mx-1][0][3] : 0x80;
                ccache[44] = top  ? cnnzc[(my-1)*mw+mx][0][2] : 0x80;
                ccache[45] = top  ? cnnzc[(my-1)*mw+mx][0][3] : 0x80;
                ccache[91] = left ? cnnzc[my*mw+mx-1][1][1] : 0x80;
                ccache[99] = left ? cnnzc[my*mw+mx-1][1][3] : 0x80;
                ccache[84] = top  ? cnnzc[(my-1)*mw+mx][1][2] : 0x80;
                ccache[85] = top  ? cnnzc[(my-1)*mw+mx][1][3] : 0x80;
                static const int cp8[8] = {52,53,60,61, 92,93,100,101};
                for (int k = 0; k < 8; ++k) {
                    int pl = k >> 2, b = k & 3, p = cp8[k];
                    int za = ccache[p-1], zb = ccache[p-8];
                    int ctx = 101 + 2*!!(zb & 0xff) + !!(za & 0xff);
                    int n = pl ? nnzv[b] : nnzu[b];
                    if (n) {
                        vk_cabac_decision(cb, ctx, 1);
                        cabac_coeffs(cb, pl ? acv[b] : acu[b], 4);
                    } else
                        vk_cabac_decision(cb, ctx, 0);
                    ccache[p] = (uint8_t)n;
                }
            }
            for (int i = 0; i < 4; ++i) {
                cnnzc[my*mw+mx][0][i] = (uint8_t)nnzu[i];
                cnnzc[my*mw+mx][1][i] = (uint8_t)nnzv[i];
            }
            int nu_dc = 0, nv_dc = 0;
            for (int i = 0; i < 4; ++i) { nu_dc |= qu[i] != 0; nv_dc |= qv[i] != 0; }
            mbcbp[my*mw+mx] = cbp_luma | (nz ? 0x100 : 0)
                            | (nu_dc ? 0x200 : 0) | (nv_dc ? 0x400 : 0);
            for (int yy = 0; yy < 16; ++yy)
                memcpy(rbase + (my*16+yy)*rs + mx*16, rmb + yy*16, 16);
            /* reconstruct chroma: DC only */
            for (int yy=0; yy<8; ++yy) {
                memcpy(rcu+co+(my*8+yy)*cs+mx*8, pu+yy*8, 8);
                memcpy(rcv+co+(my*8+yy)*cs+mx*8, pv+yy*8, 8);
            }
        }
    if (recon_out) {
        uint8_t *rbase = recon + ro;
        for (int y = 0; y < h; ++y)
            memcpy(recon_out + y*w, rbase + y*rs, w);
        for (int y = 0; y < h/2; ++y) {
            memcpy(recon_out + (size_t)w*h + y*(w/2), rcu + co + y*cs, w/2);
            memcpy(recon_out + (size_t)w*h + (size_t)(w/2)*(h/2) + y*(w/2),
                   rcv + co + y*cs, w/2);
        }
    }
    free(nnzc);
    free(cnnzc);
    free(mbcbp);
    free(cmode_fix);
    free(recon);
    int bytes = vk_cabac_flush(cb, 0);
    bs->p = data + bytes;
    return bytes;
}

/* ========================== P-slice support ========================== */
/* MB analysis below mirrors pframe.c (proven byte-exact on moving footage);
 * only the entropy writer differs. */

static void p_mvp16_avail(int lx, int ly, int tx, int ty, int cx, int cy,
                          int have_l, int have_t, int have_c,
                          int *px, int *py)
{
    int n = have_l + have_t + have_c;
    if (n == 1) {
        if (have_l) { *px = lx; *py = ly; }
        else if (have_t) { *px = tx; *py = ty; }
        else { *px = cx; *py = cy; }
        return;
    }
    vk_mvp16(lx, ly, tx, ty, cx, cy, px, py);
}

static void p_pskip_mv(int lx, int ly, int tx, int ty, int cx, int cy,
                       int have_l, int have_t, int have_c,
                       int *px, int *py)
{
    if (!have_l || !have_t || (lx == 0 && ly == 0) || (tx == 0 && ty == 0)) {
        *px = *py = 0;
        return;
    }
    vk_mvp16(lx, ly, tx, ty, have_c ? cx : lx, have_c ? cy : ly, px, py);
}

static int p_decimate_score(const int *d, int n)
{
    static const uint8_t ds[16] = {3,2,2,1,1,1,0,0,0,0,0,0,0,0,0,0};
    int score = 0, idx = n-1;
    while (idx >= 0 && d[idx] == 0) idx--;
    while (idx >= 0) {
        if ((unsigned)(d[idx--] + 1) > 2) return 9;
        int run = 0;
        while (idx >= 0 && d[idx] == 0) { idx--; run++; }
        score += ds[run];
    }
    return score;
}

static const uint8_t p_zz4[16] = {0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15};

static int p_block_decimate(const uint8_t *src, int ss, const uint8_t *pred, int ps,
                            const int mf[16], const int bias[16], int *raw_dc,
                            int include_dc)
{
    uint8_t s[16], p[16];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            s[y*4+x] = src[y*ss+x];
            p[y*4+x] = pred[y*ps+x];
        }
    int d[16];
    vk_ref_dct4x4(d, s, p);
    *raw_dc = d[0];
    if (!include_dc) d[0] = 0;
    for (int i = 0; i < 16; ++i)
        d[i] = vk_ref_quant_one(d[i], mf[i], bias[i]);
    int z[16];
    for (int i = 0; i < 16; ++i) z[i] = d[p_zz4[i]];
    return include_dc ? p_decimate_score(z, 16) : p_decimate_score(z+1, 15);
}

static int p_probe(const uint8_t *cur, const uint8_t *refp,
                   int w, int h, int rs, int x, int y, int mvx_qp, int mvy_qp, int qp)
{
    int mf[16], bias[16], deq[16], cmf[16], cbias[16], cdeq[16];
    vk_ref_quant_tables(qp, 1, mf, bias, deq);
    vk_ref_quant_tables(qp, 3, cmf, cbias, cdeq);
    int mx = mvx_qp / 4, my = mvy_qp / 4;
    int cs = w/2, crs = rs/2, cp = CPAD;
    const uint8_t *cy = cur;
    const uint8_t *cu = cur + (size_t)w*h;
    const uint8_t *cv = cu + (size_t)cs*cs;
    size_t ysz = (size_t)rs*(h+2*LPAD);
    size_t csz = (size_t)crs*(h/2+2*CPAD);
    const uint8_t *ry = refp + LPAD*rs + LPAD;
    const uint8_t *ru = refp + ysz;
    const uint8_t *rv = ru + csz;
    int unused, dec = 0;
    for (int b = 0; b < 16; ++b) {
        int by = (b/4)*4, bx = (b%4)*4;
        dec += p_block_decimate(cy + (y+by)*w + (x+bx), w,
                                ry + (y+by+my)*rs + (x+bx+mx), rs, mf, bias,
                                &unused, 1);
        if (dec >= 6) return 0;
    }
    int cvx8 = mvx_qp, cvy8 = mvy_qp;
    int d8x = cvx8 & 7, d8y = cvy8 & 7;
    int cA = (8-d8x)*(8-d8y), cB = d8x*(8-d8y);
    int cC = (8-d8x)*d8y, cD = d8x*d8y;
    int coffx = (cvx8 >> 3) + CPAD, coffy = (cvy8 >> 3) + CPAD;
    static const int lambda2_tab[52] = {
        14,18,22,28,36,45,57,72,91,115,145,182,230,290,365,460,
        580,731,921,1161,1462,1843,2322,2925,3686,4644,5851,7372,9289,
        11703,14745,18578,23407,29491,37156,46814,58982,74313,93628,117964,
        148626,187257,235929,297252,374514,471859,594505,749029,943740,
        1189156,1500160,1891796
    };
    int l2 = lambda2_tab[qp < 0 ? 0 : (qp > 51 ? 51 : qp)];
    int thresh = (l2 + 32) >> 6;
    for (int pl = 0; pl < 2; ++pl) {
        const uint8_t *c = pl ? cv : cu;
        const uint8_t *r = pl ? rv : ru;
        uint8_t p8[64];
        int ssd = 0;
        for (int yy = 0; yy < 8; ++yy)
            for (int xx = 0; xx < 8; ++xx) {
                int sx = x/2 + xx + coffx, sy = y/2 + yy + coffy;
                int a = r[sy*crs+sx], bb = r[sy*crs+sx+1];
                int cc = r[(sy+1)*crs+sx], dd = r[(sy+1)*crs+sx+1];
                uint8_t v = (uint8_t)((cA*a + cB*bb + cC*cc + cD*dd + 32) >> 6);
                p8[yy*8+xx] = v;
                int e = c[(y/2+yy)*cs + (x/2+xx)] - v;
                ssd += e*e;
            }
        if (ssd < thresh) continue;
        int cdc[4];
        for (int b = 0; b < 4; ++b) {
            int by = (b/2)*4, bx = (b%2)*4;
            uint8_t pb[16];
            for (int yy = 0; yy < 4; ++yy)
                for (int xx = 0; xx < 4; ++xx)
                    pb[yy*4+xx] = p8[(by+yy)*8 + bx+xx];
            p_block_decimate(c + (y/2+by)*cs + (x/2+bx), cs,
                             pb, 4, cmf, cbias, &cdc[b], 0);
        }
        int a = cdc[0] + cdc[1], b2 = cdc[2] + cdc[3];
        int c1 = cdc[0] - cdc[1], d = cdc[2] - cdc[3];
        int ch[4] = { a+b2, a-b2, c1+d, c1-d };
        for (int i = 0; i < 4; ++i)
            if (vk_ref_quant_one(ch[i], cmf[0] >> 1, cbias[0] << 1)) return 0;
        if (ssd < thresh*4) continue;
        int cdec = 0;
        for (int b = 0; b < 4; ++b) {
            int by = (b/2)*4, bx = (b%2)*4;
            uint8_t pb[16];
            for (int yy = 0; yy < 4; ++yy)
                for (int xx = 0; xx < 4; ++xx)
                    pb[yy*4+xx] = p8[(by+yy)*8 + bx+xx];
            int tmp;
            cdec += p_block_decimate(c + (y/2+by)*cs + (x/2+bx), cs,
                                     pb, 4, cmf, cbias, &tmp, 0);
            if (cdec >= 7) return 0;
        }
    }
    (void)cp;
    return 1;
}

/* x264 cabac_mvd_cpn, non-RDO path (encoder/cabac.c:308). */
static void cabac_mvd_cpn(vk_cabac_t *cb, int l, int mvd, int ctx)
{
    int ctxbase = l ? 47 : 40;
    if (mvd == 0) { vk_cabac_decision(cb, ctxbase+ctx, 0); return; }
    int i_abs = mvd < 0 ? -mvd : mvd;
    vk_cabac_decision(cb, ctxbase+ctx, 1);
    static const uint8_t ctxes[8] = {3,4,5,6,6,6,6,6};
    if (i_abs < 9) {
        for (int i = 1; i < i_abs; ++i)
            vk_cabac_decision(cb, ctxbase+ctxes[i-1], 1);
        vk_cabac_decision(cb, ctxbase+ctxes[i_abs-1], 0);
    } else {
        for (int i = 1; i < 9; ++i)
            vk_cabac_decision(cb, ctxbase+ctxes[i-1], 1);
        vk_cabac_ue_bypass(cb, 3, i_abs - 9);
    }
    vk_cabac_bypass(cb, mvd >> 31);
}

/* x264_cabac_mvd_sum (common/base.h:259). */
static int cabac_mvd_sum(int lx, int ly, int tx, int ty)
{
    int a = lx + tx, b = ly + ty;
    a = (a > 2) + (a > 32);
    b = (b > 2) + (b > 32);
    return a + (b << 8);
}

int vk_cabac_encode_p(vk_bs_t *bs, vk_cabac_t *cb,
                      const uint8_t *cur, const uint8_t *ref,
                      int w, int h, int qp)
{
    int mw = w/16, mh = h/16;
    uint8_t *data = bs->p;
    vk_cabac_start(cb, data, bs->end);

    int rs = w + 2*LPAD, crs = w/2 + 2*CPAD;
    size_t ysz = (size_t)rs*(h+2*LPAD), csz = (size_t)crs*(h/2+2*CPAD);
    uint8_t *refp = calloc(1, ysz + 2*csz);
    vk_pad_plane(refp, rs, ref, w, w, h, LPAD);
    vk_pad_plane(refp + ysz, crs, ref + (size_t)w*h, w/2, w/2, h/2, CPAD);
    vk_pad_plane(refp + ysz + csz, crs,
                 ref + (size_t)w*h + (size_t)(w/2)*(h/2), w/2, w/2, h/2, CPAD);

    /* reconstructed current frame (separate from the reference!) */
    uint8_t *ry = malloc(ysz), *ru = malloc(csz), *rv = malloc(csz);
    memset(ry, 128, ysz); memset(ru, 128, csz); memset(rv, 128, csz);

    int (*mv)[2] = calloc((size_t)mw*mh, 2*sizeof(int));
    uint8_t *is_skip = calloc((size_t)mw*mh, 1);
    /* mvd cache per 4x4 block, packed {abs_x,abs_y} capped at 66 */
    uint8_t (*mvd_c)[16][2] = calloc((size_t)mw*mh, 32);
    int *cbp_c = calloc((size_t)mw*mh, sizeof(int));
    for (int i = 0; i < mw*mh; ++i) cbp_c[i] = 0;

    static const uint16_t lambda_tab[52] = {
        1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
        2,2,2,2,3,3,3,4, 4,4,5,6,6,7,8,9,
        10,11,13,14,16,18,20,23, 25,29,32,36,40,45,51,57,
        64,72,81,91
    };
    for (int my = 0; my < mh; ++my)
        for (int mx = 0; mx < mw; ++mx) {
            int idx = my*mw+mx;
            if (idx > 0) vk_cabac_terminal(cb);
            int have_l = mx > 0, have_t = my > 0;
            int have_c = have_t && mx+1 < mw;
            int have_tl = have_t && mx > 0;
            int lx = have_l ? mv[idx-1][0] : 0, ly = have_l ? mv[idx-1][1] : 0;
            int tx = have_t ? mv[idx-mw][0] : 0, ty = have_t ? mv[idx-mw][1] : 0;
            int cx = have_c ? mv[idx-mw+1][0]
                            : (have_tl ? mv[idx-mw-1][0] : 0);
            int cy = have_c ? mv[idx-mw+1][1]
                            : (have_tl ? mv[idx-mw-1][1] : 0);

            int psk_x, psk_y;
            p_pskip_mv(lx, ly, tx, ty, cx, cy, have_l, have_t,
                       have_c || have_tl, &psk_x, &psk_y);

            int skip_neigh = (have_l && is_skip[idx-1])
                           || (have_t && is_skip[idx-mw])
                           || (have_tl && is_skip[idx-mw-1])
                           || (have_c && is_skip[idx-mw+1]);
            int do_skip = 0;
            if (skip_neigh &&
                p_probe(cur, refp, w, h, rs, mx*16, my*16, psk_x, psk_y, qp))
                do_skip = 1;

            int mvpx = 0, mvpy = 0, mvx = 0, mvy = 0;
            if (!do_skip) {
                p_mvp16_avail(lx, ly, tx, ty, cx, cy, have_l, have_t,
                              have_c || have_tl, &mvpx, &mvpy);
                int cand[8], nc = 0;
                int cmv[4][2] = {
                    {lx,ly}, {tx,ty},
                    {have_tl ? mv[idx-mw-1][0] : 0, have_tl ? mv[idx-mw-1][1] : 0},
                    {cx,cy}
                };
                for (int k = 0; k < 4; ++k) {
                    int ax = (cmv[k][0]+2)>>2, ay = (cmv[k][1]+2)>>2;
                    if ((ax == 0 && ay == 0) ||
                        (ax == mvpx/4 && ay == mvpy/4)) continue;
                    cand[2*nc] = ax; cand[2*nc+1] = ay;
                    ++nc;
                }
                vk_me16_dia(cur, w, refp, rs, LPAD, mx*16, my*16,
                            mvpx/4, mvpy/4, 16,
                            lambda_tab[qp < 0 ? 0 : (qp > 51 ? 51 : qp)],
                            cand, nc, &mvx, &mvy);
                int qx = mvx*4, qy = mvy*4;
                if (qx == psk_x && qy == psk_y &&
                    p_probe(cur, refp, w, h, rs, mx*16, my*16, qx, qy, qp))
                    do_skip = 1;
            }

            /* ---- CABAC: mb_skip_flag ---- */
            int skip_ctx = 11 + (have_l && !is_skip[idx-1])
                              + (have_t && !is_skip[idx-mw]);
            vk_cabac_decision(cb, skip_ctx, do_skip);

            if (do_skip) {
                mv[idx][0] = psk_x; mv[idx][1] = psk_y;
                is_skip[idx] = 1;
                memset(mvd_c[idx], 0, 32);
                /* reconstruct from reference at predictor MV */
                int sx = psk_x/4, sy = psk_y/4;
                const uint8_t *ryref = refp + LPAD*rs + LPAD;
                for (int yy = 0; yy < 16; ++yy)
                    memcpy(ry + (my*16+yy+LPAD)*rs + LPAD + mx*16,
                           ryref + (my*16+yy+sy)*rs + LPAD + mx*16+sx, 16);
                for (int yy = 0; yy < 8; ++yy) {
                    int cx8 = psk_x/8, cy8 = psk_y/8;
                    memcpy(ru + (my*8+yy+CPAD)*crs + CPAD + mx*8,
                           refp + ysz + (my*8+yy+cy8+CPAD)*crs + CPAD + mx*8+cx8, 8);
                    memcpy(rv + (my*8+yy+CPAD)*crs + CPAD + mx*8,
                           refp + ysz + csz + (my*8+yy+cy8+CPAD)*crs + CPAD + mx*8+cx8, 8);
                }
                continue;
            }

            /* ---- CABAC: mb_type P_L0_16x16 ---- */
            vk_cabac_decision_noup(cb, 14, 0);
            vk_cabac_decision_noup(cb, 15, 0);
            vk_cabac_decision_noup(cb, 16, 0);

            /* ---- mvd ---- */
            int lmv_x = have_l ? mvd_c[idx-1][3][0] : 0;
            int lmv_y = have_l ? mvd_c[idx-1][3][1] : 0;
            int tmv_x = have_t ? mvd_c[idx-mw][12][0] : 0;
            int tmv_y = have_t ? mvd_c[idx-mw][12][1] : 0;
            int amvd = cabac_mvd_sum(lmv_x, lmv_y, tmv_x, tmv_y);
            int mdx = mvx*4 - mvpx, mdy = mvy*4 - mvpy;
            cabac_mvd_cpn(cb, 0, mdx, amvd & 0xff);
            cabac_mvd_cpn(cb, 1, mdy, amvd >> 8);

            /* store mvd cache (all 16 blocks) */
            int ax = mdx < 0 ? -mdx : mdx, ay = mdy < 0 ? -mdy : mdy;
            if (ax > 66) ax = 66;
            if (ay > 66) ay = 66;
            for (int b = 0; b < 16; ++b) {
                mvd_c[idx][b][0] = (uint8_t)ax;
                mvd_c[idx][b][1] = (uint8_t)ay;
            }

            /* ---- cbp luma + chroma (all zero: no residual) ---- */
            int cl = have_l ? cbp_c[idx-1] : -1;
            int ct = have_t ? cbp_c[idx-mw] : -1;
            vk_cabac_decision(cb, 76 - ((cl>>1)&1) - ((ct>>1)&2), 0);
            vk_cabac_decision(cb, 76 - ((0>>0)&1) - ((ct>>2)&2), 0);
            vk_cabac_decision(cb, 76 - ((cl>>3)&1) - ((0<<1)&2), 0);
            vk_cabac_decision_noup(cb, 76 - ((0>>2)&1) - ((0>>0)&2), 0);
            int cctx = 0;
            if ((cl & 0x30) && cl != -1) cctx++;
            if ((ct & 0x30) && ct != -1) cctx += 2;
            vk_cabac_decision_noup(cb, 77 + cctx, 0);

            mv[idx][0] = mvx*4; mv[idx][1] = mvy*4;
            /* reconstruct: copy reference at MV into the current-frame buffer */
            int sx = mvx, sy = mvy;
            const uint8_t *ryref = refp + LPAD*rs + LPAD;
            for (int yy = 0; yy < 16; ++yy)
                memcpy(ry + (my*16+yy+LPAD)*rs + LPAD + mx*16,
                       ryref + (my*16+yy+sy)*rs + LPAD + mx*16+sx, 16);
            for (int yy = 0; yy < 8; ++yy) {
                int cx8 = mvx/2, cy8 = mvy/2;
                memcpy(ru + (my*8+yy+CPAD)*crs + CPAD + mx*8,
                       refp + ysz + (my*8+yy+cy8+CPAD)*crs + CPAD + mx*8+cx8, 8);
                memcpy(rv + (my*8+yy+CPAD)*crs + CPAD + mx*8,
                       refp + ysz + csz + (my*8+yy+cy8+CPAD)*crs + CPAD + mx*8+cx8, 8);
            }
        }

    free(ry); free(ru); free(rv);
    free(mvd_c); free(cbp_c); free(is_skip); free(mv); free(refp);
    int bytes = vk_cabac_flush(cb, 1);
    bs->p = data + bytes;
    return bytes;
}



