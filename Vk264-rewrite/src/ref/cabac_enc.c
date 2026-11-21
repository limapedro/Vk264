/* SPDX-License-Identifier: GPL-2.0-or-later
 * CABAC I-slice writer (I16 macroblocks, zero residual DC), ported from
 * x264 encoder/cabac.c. */
#include "cabac_enc.h"
#include "cabac_tables.h"
#include "intra16.h"
#include "slice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t clip8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

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
static void cabac_chroma_mode(vk_cabac_t *cb, int mode)
{
    int m = mode <= 3 ? mode : 0;
    vk_cabac_decision_noup(cb, 64, m > 0);
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
    /* x264 I16 decimation: accumulate the score over the whole MB, then
     * zero all blocks if total < 6 (cbp_luma is 0 or 15). */
    static const uint8_t ds[16] = {3,2,2,1,1,1,0,0,0,0,0,0,0,0,0,0};
    int dec = 0;
    for (int b = 0; b < 16; ++b) {
        if (!nnz[b]) continue;
        int zs[15];
        for (int i = 0; i < 15; ++i) zs[i] = dct[b][zz[i+1]];
        int idx = 14;
        while (idx >= 0 && zs[idx] == 0) idx--;
        while (idx >= 0) {
            if ((unsigned)(zs[idx--]+1) > 2) { dec = 9; break; }
            int run = 0;
            while (idx >= 0 && zs[idx] == 0) { idx--; run++; }
            dec += ds[run < 16 ? run : 15];
        }
    }
    *cbp_luma = (dec >= 6) ? 15 : 0;
    if (!*cbp_luma) for (int b = 0; b < 16; ++b) nnz[b] = 0;
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
static void compute_chroma_dc(const uint8_t *src, int cs, int x, int y,
                              const uint8_t top[8], const uint8_t left[8],
                              uint8_t tl, int mode, int qp, int q[4],
                              uint8_t pred[64])
{
    vk_predict_chroma8(pred, top, left, tl, mode);
    int cmf[16], cbias[16], cdeq[16];
    vk_ref_quant_tables(qp, 2, cmf, cbias, cdeq);
    int dc[4];
    for (int b = 0; b < 4; ++b) {
        int by = (b/2)*4, bx = (b%2)*4;
        uint8_t s[16], p[16];
        for (int yy = 0; yy < 4; ++yy)
            for (int xx = 0; xx < 4; ++xx) {
                s[yy*4+xx] = src[(y+by+yy)*cs + x+bx+xx];
                p[yy*4+xx] = pred[(by+yy)*8 + bx+xx];
            }
        int d[16];
        vk_ref_dct4x4(d, s, p);
        dc[b] = d[0];
    }
    int a = dc[0]+dc[1], b = dc[2]+dc[3];
    int c = dc[0]-dc[1], d = dc[2]-dc[3];
    int had[4] = {a+b, a-b, c+d, c-d};
    int dmf = cmf[0] >> 1, dbias = cbias[0] << 1;
    int qt[4];
    for (int i = 0; i < 4; ++i) qt[i] = vk_ref_quant_one(had[i], dmf, dbias);
    /* x264 zigzag_scan_2x2_dc: {0,2,1,3} */
    q[0]=qt[0]; q[1]=qt[2]; q[2]=qt[1]; q[3]=qt[3];
    /* DC-only chroma reconstruction: idct_dequant_2x2_dconly */
    int d0=qt[0]+qt[2], d1=qt[1]+qt[3];
    int d2=qt[0]-qt[2], d3=qt[1]-qt[3];
    int cdmf = cdeq[0] << (qp/6);
    int rr[4] = {((d0+d1)*cdmf)>>5, ((d0-d1)*cdmf)>>5,
                 ((d2+d3)*cdmf)>>5, ((d2-d3)*cdmf)>>5};
    for (int b = 0; b < 4; ++b) {
        int by=(b/2)*4, bx=(b%2)*4;
        for (int yy=0; yy<4; ++yy)
            for (int xx=0; xx<4; ++xx)
                pred[(by+yy)*8+bx+xx] = clip8(pred[(by+yy)*8+bx+xx] + ((rr[b]+32)>>6));
    }
}

int vk_cabac_encode_idr(vk_bs_t *bs, vk_cabac_t *cb,
                        const uint8_t *frame, int w, int h, int qp,
                        int frame_num)
{
    (void)frame_num;
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
                {2,1,0,3,-1},   {2,1,0,3,-1}
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
            /* chroma mode by SAD */
            int cmode = 0, cbest = 0x7fffffff;
            uint8_t cp[64];
            const uint8_t *cu = frame + w*h, *cv = cu + (w/2)*(h/2);
            for (int m = 0; m < 4; ++m) {
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
                int c = sad + lambda*vk_mode_bits(m);
                if (c < cbest) { cbest = c; cmode = m; }
            }
            int qu[4], qv[4];
            uint8_t pu[64], pv[64];
            compute_chroma_dc(cu, w/2, mx*8, my*8, tu, lu, tlu, cmode, qp, qu, pu);
            compute_chroma_dc(cv, w/2, mx*8, my*8, tv, lv, tlv, cmode, qp, qv, pv);
            int cbp_chroma = 0;
            for (int i=0;i<4;++i) if (qu[i]||qv[i]) cbp_chroma = 1;
            int lc = left ? mbcbp[my*mw+mx-1] : 0x7ff;
            int tc = top ? mbcbp[(my-1)*mw+mx] : 0x7ff;
            int nza = (lc >> 8) & 1, nzb = (tc >> 8) & 1;
            cabac_mb_type_i16(cb, left, top, imode, cbp_luma, cbp_chroma);
            cabac_chroma_mode(cb, 0);
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
                vk_cabac_decision(cb, 97 + 2*cb2 + ca, 1);
                cabac_coeffs(cb, qu, 3);
                int cv_a = (lc>>10)&1, cv_b = (tc>>10)&1;
                vk_cabac_decision(cb, 97 + 2*cv_b + cv_a, 1);
                cabac_coeffs(cb, qv, 3);
            }
            mbcbp[my*mw+mx] = cbp_luma | (nz ? 0x100 : 0) | (cbp_chroma ? 0x600 : 0);
            for (int yy = 0; yy < 16; ++yy)
                memcpy(rbase + (my*16+yy)*rs + mx*16, rmb + yy*16, 16);
            /* reconstruct chroma: DC only */
            for (int yy=0; yy<8; ++yy) {
                memcpy(rcu+co+(my*8+yy)*cs+mx*8, pu+yy*8, 8);
                memcpy(rcv+co+(my*8+yy)*cs+mx*8, pv+yy*8, 8);
            }
        }
    free(nnzc);
    free(mbcbp);
    free(recon);
    int bytes = vk_cabac_flush(cb, 0);
    bs->p = data + bytes;
    return bytes;
}



