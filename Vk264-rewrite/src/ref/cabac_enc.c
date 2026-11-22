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

/* x264's mc_chroma: luma qpel MV is a 1/8-chroma-pixel offset, so integer
 * luma MVs still land on fractional chroma positions and need bilinear
 * interpolation (mvx8/mvy8 are luma quarter-pel MVs). */
static void cabac_mc_chroma8(uint8_t *du, uint8_t *dv, int dstride,
                             const uint8_t *ru, const uint8_t *rv, int rstride,
                             int x, int y, int mvx8, int mvy8)
{
    int dx = mvx8 & 7, dy = mvy8 & 7;
    int cA=(8-dx)*(8-dy), cB=dx*(8-dy), cC=(8-dx)*dy, cD=dx*dy;
    int ox = (mvx8 >> 3) + CPAD, oy = (mvy8 >> 3) + CPAD;
    for (int yy = 0; yy < 8; ++yy)
        for (int xx = 0; xx < 8; ++xx) {
            int sx = x/2 + xx + ox, sy = y/2 + yy + oy;
            int a=ru[sy*rstride+sx], b=ru[sy*rstride+sx+1];
            int c=ru[(sy+1)*rstride+sx], d=ru[(sy+1)*rstride+sx+1];
            du[yy*dstride+xx] = (uint8_t)((cA*a+cB*b+cC*c+cD*d+32)>>6);
            a=rv[sy*rstride+sx]; b=rv[sy*rstride+sx+1];
            c=rv[(sy+1)*rstride+sx]; d=rv[(sy+1)*rstride+sx+1];
            dv[yy*dstride+xx] = (uint8_t)((cA*a+cB*b+cC*c+cD*d+32)>>6);
        }
}

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
                             int *cbp_luma, uint8_t recon[256], int b_decimate)
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
    /* x264: I-frames have b_dct_decimate=0 (score starts at 9, no decimation);
     * P-frames have it enabled (score starts at 0, decimate when < 6). */
    int any_nz = 0;
    for (int b = 0; b < 16; ++b) any_nz |= nnz[b] != 0;
    *cbp_luma = any_nz ? 15 : 0;
    if (b_decimate && any_nz) {
        static const uint8_t ds[16] = {3,2,2,1,1,1,0,0,0,0,0,0,0,0,0,0};
        int dec = 0;
        for (int b = 0; b < 16; ++b) {
            if (!nnz[b] || dec >= 6) continue;
            int idx = 14;
            while (idx >= 0 && acq[b][idx] == 0) idx--;
            while (idx >= 0) {
                if ((unsigned)(acq[b][idx--] + 1) > 2) { dec = 9; break; }
                int run = 0;
                while (idx >= 0 && acq[b][idx] == 0) { idx--; run++; }
                dec += ds[run < 16 ? run : 15];
            }
        }
        if (dec < 6) {
            *cbp_luma = 0;
            for (int b = 0; b < 16; ++b) { nnz[b] = 0; memset(acq[b], 0, 15*sizeof(int)); }
        }
    }
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
                             q, acq, nnz, &cbp_luma, rmb, 0);
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
                          int lref, int tref, int cref,
                          int *px, int *py)
{
    /* x264_mb_predict_mv_16x16: only neighbours with ref==0 participate.
     * Intra MBs carry ref=-1; unavailable edges carry ref=-2. */
    int cnt = (lref == 0) + (tref == 0) + (cref == 0);
    if (cnt > 1) {
        vk_mvp16(lx, ly, tx, ty, cx, cy, px, py);
    } else if (cnt == 1) {
        if (lref == 0) { *px = lx; *py = ly; }
        else if (tref == 0) { *px = tx; *py = ty; }
        else { *px = cx; *py = cy; }
    } else if (tref == -2 && cref == -2 && lref != -2) {
        *px = lx; *py = ly;
    } else {
        vk_mvp16(lx, ly, tx, ty, cx, cy, px, py);
    }
}

/* x264_mb_predict_mv_pskip: bail to MV=0 only for an edge neighbour
 * (ref=-2) or a ref==0 neighbour whose MV is zero. Intra neighbours
 * (ref=-1) still participate, and the ref-aware 16x16 selection picks
 * a single valid neighbour directly rather than a component median. */
static void p_pskip_mv(int lx, int ly, int tx, int ty, int cx, int cy,
                       int lref, int tref, int cref,
                       int *px, int *py)
{
    if (lref == -2 || tref == -2 ||
        (lref == 0 && lx == 0 && ly == 0) ||
        (tref == 0 && tx == 0 && ty == 0)) {
        *px = *py = 0;
        return;
    }
    p_mvp16_avail(lx, ly, tx, ty, cx, cy, lref, tref, cref, px, py);
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

/* ---- P-slice inter residual (mirrors pframe.c code_residual computation) ---- */
typedef struct {
    int cbp_luma, cbp_chroma;
    int q[16][16], nnz[16];
    int dcq[2][4], dcnz[2];
    int cacq[2][4][16], cnnz[2][4];
} p_resid_t;

static void compute_p_residual(const uint8_t *cur, const uint8_t *refp,
                               int w, int h, int rs, int x, int y,
                               int mvx_qp, int mvy_qp, int qp, p_resid_t *out)
{
    int mf[16], bias[16], deq[16], cmf[16], cbias[16], cdeq[16];
    vk_ref_quant_tables(qp, 1, mf, bias, deq);    /* CQM_4PY */
    vk_ref_quant_tables(qp, 3, cmf, cbias, cdeq); /* CQM_4PC */
    int mx = mvx_qp/4, my = mvy_qp/4;
    int cs = w/2, crs = rs/2, cp = CPAD;
    const uint8_t *cy = cur, *cu = cur + (size_t)w*h, *cv = cu + (size_t)cs*cs;
    const uint8_t *ry = refp + LPAD*rs + LPAD;
    const uint8_t *ru = refp + (size_t)rs*(h+2*LPAD);
    const uint8_t *rv = ru + (size_t)crs*(h/2+2*CPAD);
    static const uint8_t zz4[16] = {0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15};

    /* luma inter residual */
    int d[16][16];
    for (int b = 0; b < 16; ++b) {
        int by = (b/4)*4, bx = (b%4)*4;
        uint8_t s[16], p[16];
        for (int yy = 0; yy < 4; ++yy)
            for (int xx = 0; xx < 4; ++xx) {
                s[yy*4+xx] = cy[(y+by+yy)*w + (x+bx+xx)];
                p[yy*4+xx] = ry[(y+by+yy+my)*rs + (x+bx+xx+mx)];
            }
        vk_ref_dct4x4(d[b], s, p);
        for (int i = 0; i < 16; ++i) d[b][i] = vk_ref_quant_one(d[b][i], mf[i], bias[i]);
        for (int i = 0; i < 16; ++i) out->q[b][i] = d[b][zz4[i]];
        out->nnz[b] = 0;
        for (int i = 0; i < 16; ++i) out->nnz[b] += d[b][i] != 0;
    }
    static const int grp[4][4] = {{0,1,4,5},{2,3,6,7},{8,9,12,13},{10,11,14,15}};
    out->cbp_luma = 0;
    int dec_total = 0;
    for (int g = 0; g < 4; ++g) {
        int dec8 = 0, any = 0;
        for (int k = 0; k < 4; ++k) {
            int b = grp[g][k];
            if (!out->nnz[b]) continue;
            any = 1;
            if (dec8 < 6) dec8 += p_decimate_score(out->q[b], 16);
        }
        if (any) {
            dec_total += dec8;
            if (dec8 >= 4) out->cbp_luma |= 1 << g;
            else for (int k = 0; k < 4; ++k) {
                int b = grp[g][k];
                memset(out->q[b], 0, sizeof(out->q[b]));
                out->nnz[b] = 0;
            }
        }
    }
    if (dec_total < 6) {
        out->cbp_luma = 0;
        for (int b = 0; b < 16; ++b) {
            memset(out->q[b], 0, sizeof(out->q[b]));
            out->nnz[b] = 0;
        }
    }

    /* chroma inter residual */
    int cvx8 = mvx_qp, cvy8 = mvy_qp;
    int d8x = cvx8&7, d8y = cvy8&7;
    int cA=(8-d8x)*(8-d8y), cB=d8x*(8-d8y), cC=(8-d8x)*d8y, cD=d8x*d8y;
    int cox = (cvx8>>3)+cp, coy = (cvy8>>3)+cp;
    int cdc[2][4];
    out->cbp_chroma = 0;
    for (int pl = 0; pl < 2; ++pl) {
        const uint8_t *cptr = pl ? cv : cu, *r = pl ? rv : ru;
        out->dcnz[pl] = 0;
        for (int b = 0; b < 4; ++b) {
            int by = (b/2)*4, bx = (b%2)*4;
            uint8_t s[16], p[16];
            for (int yy = 0; yy < 4; ++yy)
                for (int xx = 0; xx < 4; ++xx) {
                    int sx = x/2+bx+xx+cox, sy = y/2+by+yy+coy;
                    s[yy*4+xx] = cptr[(y/2+by+yy)*cs + (x/2+bx+xx)];
                    int a=r[sy*crs+sx], b2=r[sy*crs+sx+1];
                    int cc=r[(sy+1)*crs+sx], dd=r[(sy+1)*crs+sx+1];
                    p[yy*4+xx] = (uint8_t)((cA*a+cB*b2+cC*cc+cD*dd+32)>>6);
                }
            int dd[16];
            vk_ref_dct4x4(dd, s, p);
            cdc[pl][b] = dd[0];
            dd[0] = 0;
            for (int i = 0; i < 16; ++i) dd[i] = vk_ref_quant_one(dd[i], cmf[i], cbias[i]);
            /* x264 passes h->dct.luma4x4[idx]+1: zigzag AC coefficients, DC skipped */
            for (int i = 0; i < 15; ++i) out->cacq[pl][b][i] = dd[zz4[i+1]];
            out->cnnz[pl][b] = 0;
            for (int i = 0; i < 16; ++i) out->cnnz[pl][b] += dd[i] != 0;
        }
        int a=cdc[pl][0]+cdc[pl][1], b=cdc[pl][2]+cdc[pl][3];
        int c=cdc[pl][0]-cdc[pl][1], dd2=cdc[pl][2]-cdc[pl][3];
        int had[4]={a+b,a-b,c+dd2,c-dd2}, t[4];
        for (int i = 0; i < 4; ++i)
            t[i] = vk_ref_quant_one(had[i], cmf[0]>>1, cbias[0]<<1);
        out->dcq[pl][0]=t[0]; out->dcq[pl][1]=t[2];
        out->dcq[pl][2]=t[1]; out->dcq[pl][3]=t[3];
        for (int i = 0; i < 4; ++i) {
            out->dcnz[pl] |= t[i] != 0;
            out->cbp_chroma |= t[i] != 0;
        }
        /* x264's mb_encode_chroma_internal decimates the whole plane's AC
         * when the JVT-B118 score stays below 7 (CAVLC path does the same). */
        int dec = 0;
        for (int b = 0; b < 4; ++b) {
            if (!out->cnnz[pl][b] || dec >= 7) continue;
            dec += p_decimate_score(out->cacq[pl][b], 15);
        }
        if (dec < 7) {
            for (int b = 0; b < 4; ++b) {
                out->cnnz[pl][b] = 0;
                memset(out->cacq[pl][b], 0, sizeof(out->cacq[pl][b]));
            }
        } else
            out->cbp_chroma = 2;
    }
    /* H.264 cbp_chroma is 0/1/2: value 2 means "AC present" and DC may
     * also be present.  x264 never emits 3; keep the flag in range so the
     * AC-coding branch (cb_c == 2) is taken when both DC and AC survive. */
    if (out->cbp_chroma == 3) out->cbp_chroma = 2;
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
    /* x264 mvr: inter MV considered during analysis, retained even for MBs that
     * end up intra (only P_SKIP zeroes it). Used for ME candidate generation. */
    int (*mvr)[2] = calloc((size_t)mw*mh, 2*sizeof(int));
    int *mv_ref = calloc((size_t)mw*mh, sizeof(int));  /* -1 for intra, 0 inter */
    uint8_t *is_skip = calloc((size_t)mw*mh, 1);
    /* mvd cache per 4x4 block, packed {abs_x,abs_y} capped at 66 */
    uint8_t (*mvd_c)[16][2] = calloc((size_t)mw*mh, 32);
    int *cbp_c = calloc((size_t)mw*mh, sizeof(int));
    for (int i = 0; i < mw*mh; ++i) cbp_c[i] = 0;
    /* intra-in-P neighbour caches (mirrors the I-slice path) */
    int *mbcbp = calloc((size_t)mw*mh, sizeof(int));
    uint8_t (*nnzc)[16] = calloc((size_t)mw*mh, 16);
    uint8_t (*cnnzc)[2][4] = calloc((size_t)mw*mh, 8);
    uint8_t *cmode_fix = calloc((size_t)mw*mh, 1);

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
            p_pskip_mv(lx, ly, tx, ty, cx, cy,
                       have_l ? mv_ref[idx-1] : -2,
                       have_t ? mv_ref[idx-mw] : -2,
                       have_c ? mv_ref[idx-mw+1]
                              : (have_tl ? mv_ref[idx-mw-1] : -2),
                       &psk_x, &psk_y);

            int skip_neigh = (have_l && is_skip[idx-1])
                           || (have_t && is_skip[idx-mw])
                           || (have_tl && is_skip[idx-mw-1])
                           || (have_c && is_skip[idx-mw+1]);
            int do_skip = 0, fast_skip = 0;
            if (skip_neigh &&
                p_probe(cur, refp, w, h, rs, mx*16, my*16, psk_x, psk_y, qp)) {
                do_skip = 1;
                fast_skip = 1;  /* x264 returns early: intra not considered */
            }

            int mvpx = 0, mvpy = 0, mvx = 0, mvy = 0, inter_cost = 0;
            if (!do_skip) {
                p_mvp16_avail(lx, ly, tx, ty, cx, cy,
                              have_l ? mv_ref[idx-1] : -2,
                              have_t ? mv_ref[idx-mw] : -2,
                              have_c ? mv_ref[idx-mw+1]
                                     : (have_tl ? mv_ref[idx-mw-1] : -2),
                              &mvpx, &mvpy);
                int cand[8], nc = 0;
                /* x264 x264_mb_predict_mv_ref16x16: 4 mvr spatial predictors
                 * (left, top, top-left, top-right), zero when unavailable. */
                int cmv[4][2] = {
                    {have_l ? mvr[idx-1][0] : 0,   have_l ? mvr[idx-1][1] : 0},
                    {have_t ? mvr[idx-mw][0] : 0,  have_t ? mvr[idx-mw][1] : 0},
                    {have_tl ? mvr[idx-mw-1][0] : 0, have_tl ? mvr[idx-mw-1][1] : 0},
                    {have_c ? mvr[idx-mw+1][0] : 0,  have_c ? mvr[idx-mw+1][1] : 0}
                };
                for (int k = 0; k < 4; ++k) {
                    int ax = (cmv[k][0]+2)>>2, ay = (cmv[k][1]+2)>>2;
                    if ((ax == 0 && ay == 0) ||
                        (ax == mvpx/4 && ay == mvpy/4)) continue;
                    cand[2*nc] = ax; cand[2*nc+1] = ay;
                    ++nc;
                }
                inter_cost = vk_me16_dia(cur, w, refp, rs, LPAD, mx*16, my*16,
                            mvpx/4, mvpy/4, 16,
                            lambda_tab[qp < 0 ? 0 : (qp > 51 ? 51 : qp)],
                            cand, nc, &mvx, &mvy, h);
                /* x264 stores the inter MV in mvr immediately after inter
                 * analysis, before skip/intra decisions may override the type. */
                mvr[idx][0] = mvx*4; mvr[idx][1] = mvy*4;
                int qx = mvx*4, qy = mvy*4;
                if (qx == psk_x && qy == psk_y &&
                    p_probe(cur, refp, w, h, rs, mx*16, my*16, qx, qy, qp))
                    do_skip = 1;
            }

            /* ---- intra-vs-inter decision (subme=0: SAD + lambda*bits) ---- */
            int do_intra = 0, imode = 2, icmode = 0;
            int i_lambda = vk_lambda(qp);
            uint8_t it[16], il[16], itl;
            if (!fast_skip && inter_cost > 0) {
                uint8_t *rbase = ry + LPAD*rs + LPAD;
                for (int i = 0; i < 16; ++i) {
                    it[i] = have_t ? rbase[(my*16-1)*rs + mx*16+i]
                                   : (mx == 0 ? 128 : rbase[(my*16)*rs + mx*16-16+i]);
                    il[i] = have_l ? rbase[(my*16+i)*rs + mx*16-1] : 128;
                }
                itl = (have_t && have_l) ? rbase[(my*16-1)*rs + mx*16-1] : 128;
                static const int8_t pi16av[5][5] = {
                    {6,-1,-1,-1,-1}, {4,1,-1,-1,-1}, {5,0,-1,-1,-1},
                    {0,1,2,3,-1},   {0,1,2,3,-1}
                };
                int pmidx = (have_l&&have_t) ? ((mx>0&&my>0)?4:3)
                                            : (have_l?1:(have_t?2:0));
                uint8_t trial[256];
                int ibest = 0x7fffffff, bm = 2;
                for (int k = 0; pi16av[pmidx][k] >= 0; ++k) {
                    int m = pi16av[pmidx][k];
                    vk_predict16(trial, it, il, itl, m);
                    int sad = 0;
                    for (int yy = 0; yy < 16; ++yy)
                        for (int xx = 0; xx < 16; ++xx) {
                            int dv = cur[(my*16+yy)*w + mx*16+xx] - trial[yy*16+xx];
                            sad += dv < 0 ? -dv : dv;
                        }
                    int c = sad + i_lambda*vk_mode_bits(m);
                    if (c < ibest) { ibest = c; bm = m; }
                }
                if (ibest < inter_cost) {
                    do_intra = 1; imode = bm;
                    do_skip = 0;  /* x264: intra overrides tentative P_SKIP */
                }
            }

            /* ---- CABAC: mb_skip_flag ---- */
            /* x264 promotes P_L0/cbp==0/MV==pskip_mv to P_SKIP at encode time
             * (encoder/macroblock.c:953-964). Save the whole CABAC state so
             * the header bits can be replaced by skip_flag=1 if that happens. */
            vk_cabac_t mb_start = *cb;
            int skip_ctx = 11 + (have_l && !is_skip[idx-1])
                              + (have_t && !is_skip[idx-mw]);
            vk_cabac_decision(cb, skip_ctx, do_skip);

            if (do_skip) {
                mv[idx][0] = psk_x; mv[idx][1] = psk_y;
                mvr[idx][0] = mvr[idx][1] = 0;  /* x264 P_SKIP zeroes mvr */
                is_skip[idx] = 1;
                memset(mvd_c[idx], 0, 32);
                /* reconstruct from reference at predictor MV */
                int sx = psk_x/4, sy = psk_y/4;
                const uint8_t *ryref = refp + LPAD*rs + LPAD;
                for (int yy = 0; yy < 16; ++yy)
                    memcpy(ry + (my*16+yy+LPAD)*rs + LPAD + mx*16,
                           ryref + (my*16+yy+sy)*rs + mx*16+sx, 16);
                cabac_mc_chroma8(ru + (my*8+CPAD)*crs + CPAD + mx*8,
                                 rv + (my*8+CPAD)*crs + CPAD + mx*8, crs,
                                 refp + ysz, refp + ysz + csz, crs,
                                 mx*16, my*16, psk_x, psk_y);
                continue;
            }

            if (do_intra) {
                /* ---- I16 macroblock inside a P-slice ---- */
                uint8_t itu[8], ilu[8], itv[8], ilv[8], itlu, itlv;
                uint8_t *rbu = ru + CPAD*crs + CPAD, *rbv = rv + CPAD*crs + CPAD;
                for (int i = 0; i < 8; ++i) {
                    itu[i] = have_t ? rbu[(my*8-1)*crs + mx*8+i]
                                    : (mx == 0 ? 128 : rbu[(my*8)*crs + mx*8-8+i]);
                    itv[i] = have_t ? rbv[(my*8-1)*crs + mx*8+i]
                                    : (mx == 0 ? 128 : rbv[(my*8)*crs + mx*8-8+i]);
                    ilu[i] = have_l ? rbu[(my*8+i)*crs + mx*8-1] : 128;
                    ilv[i] = have_l ? rbv[(my*8+i)*crs + mx*8-1] : 128;
                }
                itlu = (have_t && have_l) ? rbu[(my*8-1)*crs + mx*8-1] : 128;
                itlv = (have_t && have_l) ? rbv[(my*8-1)*crs + mx*8-1] : 128;
                /* chroma mode: availability-based, SAD + lambda*bits */
                static const int8_t pcav[5][5] = {
                    {6,-1,-1,-1,-1}, {4,1,-1,-1,-1}, {5,2,-1,-1,-1},
                    {2,1,0,-1,-1},   {2,1,0,3,-1}
                };
                int cav = (have_l?1:0)|(have_t?2:0)|((have_l&&have_t&&mx>0&&my>0)?4:0);
                int cidx = (cav & 3); if (cav == 7) cidx = 4;
                icmode = 6;
                int cbest = 0x7fffffff;
                uint8_t cp8[64];
                const uint8_t *cu = cur + (size_t)w*h, *cv = cu + (size_t)(w/2)*(h/2);
                for (int k = 0; pcav[cidx][k] >= 0; ++k) {
                    int m = pcav[cidx][k], fix = m <= 3 ? m : 0, sad = 0;
                    vk_predict_chroma8(cp8, itu, ilu, itlu, m);
                    for (int yy=0; yy<8; ++yy) for (int xx=0; xx<8; ++xx) {
                        int dv = cu[(my*8+yy)*(w/2)+mx*8+xx] - cp8[yy*8+xx];
                        sad += dv<0?-dv:dv;
                    }
                    vk_predict_chroma8(cp8, itv, ilv, itlv, m);
                    for (int yy=0; yy<8; ++yy) for (int xx=0; xx<8; ++xx) {
                        int dv = cv[(my*8+yy)*(w/2)+mx*8+xx] - cp8[yy*8+xx];
                        sad += dv<0?-dv:dv;
                    }
                    int c = sad + i_lambda*vk_mode_bits(fix);
                    if (c < cbest) { cbest = c; icmode = m; }
                }
                uint8_t pred16[256];
                vk_predict16(pred16, it, il, itl, imode);
                int q[16] = {0}, acq[16][15], nnz[16], cbp_luma = 0;
                uint8_t rmb[256];
                compute_i16_full(cur, w, mx*16, my*16, pred16, qp,
                                 q, acq, nnz, &cbp_luma, rmb, 1);
                int qu[4], qv[4], acu[4][15], acv[4][15], nnzu[4], nnzv[4];
                uint8_t pu[64], pv[64];
                compute_chroma_dc(cu, w/2, mx*8, my*8, itu, ilu, itlu, icmode, qp,
                                  qu, acu, nnzu, pu);
                compute_chroma_dc(cv, w/2, mx*8, my*8, itv, ilv, itlv, icmode, qp,
                                  qv, acv, nnzv, pv);
                int has_dc = 0, has_ac = 0;
                for (int i = 0; i < 4; ++i) {
                    has_dc |= qu[i] || qv[i];
                    has_ac |= nnzu[i] || nnzv[i];
                }
                int cbp_chroma = has_ac ? 2 : (has_dc ? 1 : 0);
                int nzl = 0;
                for (int i = 0; i < 16; ++i) if (q[i]) nzl = 1;

                /* CABAC mb_type: I16 in P-slice */
                vk_cabac_decision_noup(cb, 14, 1);
                vk_cabac_decision_noup(cb, 17, 1);
                vk_cabac_terminal(cb);
                vk_cabac_decision_noup(cb, 18, !!cbp_luma);
                if (!cbp_chroma)
                    vk_cabac_decision_noup(cb, 19, 0);
                else {
                    vk_cabac_decision(cb, 19, 1);
                    vk_cabac_decision_noup(cb, 19, cbp_chroma >> 1);
                }
                int predmode = imode <= 3 ? imode : 2;
                vk_cabac_decision(cb, 20, predmode >> 1);
                vk_cabac_decision_noup(cb, 20, predmode & 1);
                int cmctx = (have_l && cmode_fix[idx-1] != 0)
                          + (have_t && cmode_fix[idx-mw] != 0);
                cabac_chroma_mode(cb, icmode, cmctx);
                cmode_fix[idx] = (uint8_t)(icmode <= 3 ? icmode : 0);
                cabac_qp_delta0(cb);

                /* residual: luma DC */
                int lc = have_l ? mbcbp[idx-1] : 0x7ff;
                int tc = have_t ? mbcbp[idx-mw] : 0x7ff;
                int nza = (lc >> 8) & 1, nzb = (tc >> 8) & 1;
                vk_cabac_decision(cb, 85 + 2*nzb + nza, nzl);
                if (nzl) cabac_coeffs(cb, q, 0);
                /* luma AC */
                if (cbp_luma) {
                    static const uint8_t bidx[16] = {0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15};
                    static const int s8[16] = {12,13,20,21,14,15,22,23,28,29,36,37,30,31,38,39};
                    uint8_t cache[64] = {0};
                    for (int i = 0; i < 4; ++i) {
                        cache[11+i*8] = have_l ? nnzc[idx-1][i*4+3] : 0x80;
                        cache[4+i] = have_t ? nnzc[idx-mw][12+i] : 0x80;
                    }
                    for (int i = 0; i < 16; ++i) {
                        int b = bidx[i], p = s8[i];
                        int za = cache[p-1], zb = cache[p-8];
                        int ctx = 89 + 2*!!(zb & 0xff) + !!(za & 0xff);
                        if (nnz[b]) {
                            vk_cabac_decision(cb, ctx, 1);
                            cabac_coeffs(cb, acq[b], 1);
                        } else
                            vk_cabac_decision(cb, ctx, 0);
                        cache[p] = (uint8_t)nnz[b];
                    }
                }
                for (int i = 0; i < 16; ++i) nnzc[idx][i] = nnz[i] ? 1 : 0;
                /* chroma DC */
                if (cbp_chroma) {
                    int ca = (lc>>9)&1, cb2 = (tc>>9)&1, nu = 0, nv = 0;
                    for (int i = 0; i < 4; ++i) { nu |= qu[i]!=0; nv |= qv[i]!=0; }
                    vk_cabac_decision(cb, 97+2*cb2+ca, nu);
                    if (nu) cabac_coeffs(cb, qu, 3);
                    int cv_a = (lc>>10)&1, cv_b = (tc>>10)&1;
                    vk_cabac_decision(cb, 97+2*cv_b+cv_a, nv);
                    if (nv) cabac_coeffs(cb, qv, 3);
                }
                if (cbp_chroma == 2) {
                    uint8_t ccache[128] = {0};
                    ccache[51] = have_l ? cnnzc[idx-1][0][1] : 0x80;
                    ccache[59] = have_l ? cnnzc[idx-1][0][3] : 0x80;
                    ccache[44] = have_t ? cnnzc[idx-mw][0][2] : 0x80;
                    ccache[45] = have_t ? cnnzc[idx-mw][0][3] : 0x80;
                    ccache[91] = have_l ? cnnzc[idx-1][1][1] : 0x80;
                    ccache[99] = have_l ? cnnzc[idx-1][1][3] : 0x80;
                    ccache[84] = have_t ? cnnzc[idx-mw][1][2] : 0x80;
                    ccache[85] = have_t ? cnnzc[idx-mw][1][3] : 0x80;
                    static const int cp8[8] = {52,53,60,61, 92,93,100,101};
                    for (int k = 0; k < 8; ++k) {
                        int pl = k>>2, b = k&3, p = cp8[k];
                        int za = ccache[p-1], zb = ccache[p-8];
                        int ctx = 101 + 2*!!(zb&0xff) + !!(za&0xff);
                        int n = pl ? nnzv[b] : nnzu[b];
                        if (n) {
                            vk_cabac_decision(cb, ctx, 1);
                            cabac_coeffs(cb, pl?acv[b]:acu[b], 4);
                        } else
                            vk_cabac_decision(cb, ctx, 0);
                        ccache[p] = (uint8_t)n;
                    }
                }
                for (int i = 0; i < 4; ++i) {
                    cnnzc[idx][0][i] = nnzu[i] ? 1 : 0;
                    cnnzc[idx][1][i] = nnzv[i] ? 1 : 0;
                }
                int nu_dc = 0, nv_dc = 0;
                for (int i = 0; i < 4; ++i) { nu_dc |= qu[i]!=0; nv_dc |= qv[i]!=0; }
                mbcbp[idx] = cbp_luma | (nzl?0x100:0) | (nu_dc?0x200:0) | (nv_dc?0x400:0);
                /* x264's mb.cbp[] stores (i_cbp_chroma<<4)|i_cbp_luma; the
                 * chroma bits drive neighbours' cbp_chroma context (77..84). */
                cbp_c[idx] = mbcbp[idx] | (cbp_chroma << 4);

                /* reconstruct I16 MB into the current-frame buffers */
                uint8_t *rbase = ry + LPAD*rs + LPAD;
                for (int yy = 0; yy < 16; ++yy)
                    memcpy(rbase + (my*16+yy)*rs + mx*16, rmb + yy*16, 16);
                for (int yy = 0; yy < 8; ++yy) {
                    memcpy(rbu + (my*8+yy)*crs + mx*8, pu + yy*8, 8);
                    memcpy(rbv + (my*8+yy)*crs + mx*8, pv + yy*8, 8);
                }
                mv[idx][0] = mv[idx][1] = 0;
                mv_ref[idx] = -1;  /* intra: ref=-1, MV still participates */
                is_skip[idx] = 0;
                memset(mvd_c[idx], 0, 32);
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

            /* ---- compute P_L0 residual at the MV ---- */
            p_resid_t rr;
            compute_p_residual(cur, refp, w, h, rs, mx*16, my*16,
                               mvx*4, mvy*4, qp, &rr);
            int cl = have_l ? cbp_c[idx-1] : -1;
            int ct = have_t ? cbp_c[idx-mw] : -1;
            int cb_l = rr.cbp_luma, cb_c = rr.cbp_chroma;
            if (!do_skip && !do_intra && cb_l == 0 && cb_c == 0 &&
                mvx*4 == psk_x && mvy*4 == psk_y) {
                /* Encode-time P_SKIP promotion: discard the mb_type/mvd/cbp
                 * decisions and emit only skip_flag=1 from the saved state. */
                *cb = mb_start;
                vk_cabac_decision(cb, skip_ctx, 1);
                mv[idx][0] = psk_x; mv[idx][1] = psk_y;
                mvr[idx][0] = mvr[idx][1] = 0;
                is_skip[idx] = 1;
                memset(mvd_c[idx], 0, 32);
                const uint8_t *ryref = refp + LPAD*rs + LPAD;
                int psx = psk_x/4, psy = psk_y/4;
                for (int yy = 0; yy < 16; ++yy)
                    memcpy(ry + (my*16+yy+LPAD)*rs + LPAD + mx*16,
                           ryref + (my*16+yy+psy)*rs + mx*16+psx, 16);
                cabac_mc_chroma8(ru + (my*8+CPAD)*crs + CPAD + mx*8,
                                 rv + (my*8+CPAD)*crs + CPAD + mx*8, crs,
                                 refp + ysz, refp + ysz + csz, crs,
                                 mx*16, my*16, psk_x, psk_y);
                continue;
            }
            /* cbp luma (4 decisions, P-slice neighbour contexts) */
            vk_cabac_decision(cb, 76 - ((cl>>1)&1) - ((ct>>1)&2), (cb_l>>0)&1);
            vk_cabac_decision(cb, 76 - ((cb_l>>0)&1) - ((ct>>2)&2), (cb_l>>1)&1);
            vk_cabac_decision(cb, 76 - ((cl>>3)&1) - ((cb_l<<1)&2), (cb_l>>2)&1);
            vk_cabac_decision_noup(cb, 76 - ((cb_l>>2)&1) - ((cb_l>>0)&2), (cb_l>>3)&1);
            /* cbp chroma */
            int cpa = (cl & 0x30), cpb = (ct & 0x30), cctx = 0;
            if (cpa && cl != -1) cctx++;
            if (cpb && ct != -1) cctx += 2;
            if (!cb_c)
                vk_cabac_decision_noup(cb, 77 + cctx, 0);
            else {
                vk_cabac_decision(cb, 77 + cctx, 1);
                int x2 = 4;
                if (cpa == 0x20) x2++;
                if (cpb == 0x20) x2 += 2;
                vk_cabac_decision_noup(cb, 77 + x2, cb_c >> 1);
            }
            if (cb_l || cb_c) cabac_qp_delta0(cb);

            /* luma 4x4 AC blocks (category 2, 16 coeffs) */
            if (cb_l) {
                static const uint8_t bidx[16] = {0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15};
                static const int s8[16] = {12,13,20,21,14,15,22,23,28,29,36,37,30,31,38,39};
                static const int grp[16] = {0,0,1,1,0,0,1,1,2,2,3,3,2,2,3,3};
                uint8_t cache[64] = {0};
                for (int i = 0; i < 4; ++i) {
                    cache[11+i*8] = have_l ? nnzc[idx-1][i*4+3] : 0x80;
                    cache[4+i] = have_t ? nnzc[idx-mw][12+i] : 0x80;
                }
                for (int i = 0; i < 16; ++i) {
                    int b = bidx[i], p = s8[i];
                    if (!(cb_l & (1 << grp[b]))) continue;
                    int za = cache[p-1], zb = cache[p-8];
                    int ctx = 93 + ((2*(zb&0x7f) + (za&0x7f)) & 0x7f);
                    if (rr.nnz[b]) {
                        vk_cabac_decision(cb, ctx, 1);
                        cabac_coeffs(cb, rr.q[b], 2);
                    } else
                        vk_cabac_decision(cb, ctx, 0);
                    cache[p] = rr.nnz[b] ? 1 : 0;
                }
            }
            for (int i = 0; i < 16; ++i) nnzc[idx][i] = rr.nnz[i] ? 1 : 0;

            /* chroma DC (category 3) */
            if (cb_c) {
                int ca = (cl!=-1)?(cl>>9)&1:0, cb2=(ct!=-1)?(ct>>9)&1:0;
                vk_cabac_decision(cb, 97+2*cb2+ca, rr.dcnz[0]!=0);
                if (rr.dcnz[0]) cabac_coeffs(cb, rr.dcq[0], 3);
                int cva=(cl!=-1)?(cl>>10)&1:0, cvb=(ct!=-1)?(ct>>10)&1:0;
                vk_cabac_decision(cb, 97+2*cvb+cva, rr.dcnz[1]!=0);
                if (rr.dcnz[1]) cabac_coeffs(cb, rr.dcq[1], 3);
            }
            /* chroma AC (category 4) */
            if (cb_c == 2) {
                uint8_t ccache[128] = {0};
                ccache[51] = have_l ? cnnzc[idx-1][0][1] : 0x80;
                ccache[59] = have_l ? cnnzc[idx-1][0][3] : 0x80;
                ccache[44] = have_t ? cnnzc[idx-mw][0][2] : 0x80;
                ccache[45] = have_t ? cnnzc[idx-mw][0][3] : 0x80;
                ccache[91] = have_l ? cnnzc[idx-1][1][1] : 0x80;
                ccache[99] = have_l ? cnnzc[idx-1][1][3] : 0x80;
                ccache[84] = have_t ? cnnzc[idx-mw][1][2] : 0x80;
                ccache[85] = have_t ? cnnzc[idx-mw][1][3] : 0x80;
                static const int cp8[8] = {52,53,60,61, 92,93,100,101};
                for (int k = 0; k < 8; ++k) {
                    int pl = k>>2, b = k&3, p = cp8[k];
                    int za = ccache[p-1], zb = ccache[p-8];
                    int ctx = 101 + ((2*(zb&0x7f) + (za&0x7f)) & 0x7f);
                    int n = rr.cnnz[pl][b];
                    if (n) {
                        vk_cabac_decision(cb, ctx, 1);
                        cabac_coeffs(cb, rr.cacq[pl][b], 4);
                    } else
                        vk_cabac_decision(cb, ctx, 0);
                    ccache[p] = n ? 1 : 0;
                }
            }
            for (int i = 0; i < 4; ++i) {
                cnnzc[idx][0][i] = rr.cnnz[0][i] ? 1 : 0;
                cnnzc[idx][1][i] = rr.cnnz[1][i] ? 1 : 0;
            }
            int nz_dc = (rr.dcnz[0] || rr.dcnz[1]) ? 1 : 0;
            cbp_c[idx] = cb_l | (cb_c<<4) | (nz_dc?0x100:0)
                       | ((rr.dcnz[0]?1:0)<<9) | ((rr.dcnz[1]?1:0)<<10);
            if (cb_c) mbcbp[idx] = cb_l
                                | ((rr.dcnz[0]?1:0)<<9) | ((rr.dcnz[1]?1:0)<<10);

            mv[idx][0] = mvx*4; mv[idx][1] = mvy*4;
            /* reconstruct: copy reference at MV into the current-frame buffer */
            int sx = mvx, sy = mvy;
            const uint8_t *ryref = refp + LPAD*rs + LPAD;
            for (int yy = 0; yy < 16; ++yy)
                memcpy(ry + (my*16+yy+LPAD)*rs + LPAD + mx*16,
                       ryref + (my*16+yy+sy)*rs + mx*16+sx, 16);
            cabac_mc_chroma8(ru + (my*8+CPAD)*crs + CPAD + mx*8,
                             rv + (my*8+CPAD)*crs + CPAD + mx*8, crs,
                             refp + ysz, refp + ysz + csz, crs,
                             mx*16, my*16, mvx*4, mvy*4);
            /* add luma residual */
            if (cb_l) {
                int mfx[16], biasx[16], deqx[16];
                vk_ref_quant_tables(qp, 1, mfx, biasx, deqx);
                for (int b = 0; b < 16; ++b) {
                    static const int rgrp[16] = {0,0,1,1,0,0,1,1,2,2,3,3,2,2,3,3};
                    if (!(cb_l & (1 << rgrp[b]))) continue;
                    if (!rr.nnz[b]) continue;
                    int blk4[16];
                    /* rr.q is in zigzag order (as coded into the bitstream);
                     * IDCT needs natural order, like CAVLC's blk[rz4[i]]=q[i]. */
                    for (int i = 0; i < 16; ++i) blk4[p_zz4[i]] = rr.q[b][i];
                    vk_ref_dequant4(blk4, deqx, qp);
                    vk_ref_idct4(blk4);
                    uint8_t *dst = ry + (my*16+(b/4)*4+LPAD)*rs + LPAD + mx*16+(b%4)*4;
                    for (int yy = 0; yy < 4; ++yy)
                        for (int xx = 0; xx < 4; ++xx)
                            dst[yy*rs+xx] = clip8(dst[yy*rs+xx] + blk4[yy*4+xx]);
                }
            }
            /* add chroma residual into the reconstructed planes, like CAVLC's
             * code_residual: without this, later MBs predict chroma from a
             * prediction-only reconstruction and choose different modes. */
            if (cb_c) {
                int cmfx[16], cbiasx[16], cdeqx[16];
                vk_ref_quant_tables(qp, 3, cmfx, cbiasx, cdeqx);
                for (int pl = 0; pl < 2; ++pl) {
                    int h0 = rr.dcq[pl][0], h1 = rr.dcq[pl][2];
                    int h2 = rr.dcq[pl][1], h3 = rr.dcq[pl][3];
                    int d0 = h0+h1, d1 = h2+h3, d2 = h0-h1, d3 = h2-h3;
                    int dmf = cdeqx[0] << (qp/6);
                    int cdq[4] = { ((d0+d1)*dmf)>>5, ((d0-d1)*dmf)>>5,
                                   ((d2+d3)*dmf)>>5, ((d2-d3)*dmf)>>5 };
                    uint8_t *dst = (pl ? rv : ru) + (my*8+CPAD)*crs + CPAD + mx*8;
                    for (int b = 0; b < 4; ++b) {
                        int by = (b/2)*4, bx = (b%2)*4;
                        if (rr.cnnz[pl][b]) {
                            int blk[16] = {0};
                            for (int i = 0; i < 15; ++i)
                                blk[p_zz4[i+1]] = rr.cacq[pl][b][i];
                            vk_ref_dequant4(blk, cdeqx, qp);
                            blk[0] = rr.dcnz[pl] ? cdq[b] : 0;
                            vk_ref_idct4(blk);
                            vk_ref_add_block(dst + by*crs + bx, crs, blk);
                        } else if (rr.dcnz[pl]) {
                            vk_ref_add_block_dc(dst + by*crs + bx, crs, cdq[b]);
                        }
                    }
                }
            }
        }

    free(ry); free(ru); free(rv);
    free(cmode_fix); free(cnnzc); free(nnzc); free(mbcbp);
    free(mvd_c); free(cbp_c); free(is_skip); free(mv); free(mvr); free(mv_ref); free(refp);
    int bytes = vk_cabac_flush(cb, 1);
    bs->p = data + bytes;
    return bytes;
}



