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
    for (int i = 0; i <= count_m1; ++i) {
        if (l[i]) {
            coeffs[nc++] = l[i];
            vk_cabac_decision(cb, ctx_sig+i, 1);
            if (i == last) { vk_cabac_decision(cb, ctx_last+i, 1); break; }
            vk_cabac_decision(cb, ctx_last+i, 0);
        } else
            vk_cabac_decision(cb, ctx_sig+i, 0);
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
                              int mode)
{
    int ctx = 0;
    if (ctx_left) ctx++;   /* left MB type != I_4x4 */
    if (ctx_top) ctx++;
    int ctx0 = 3 + ctx;
    vk_cabac_decision_noup(cb, ctx0, 1);       /* I16 vs I4x4/I8x8/PCM */
    vk_cabac_terminal(cb);
    vk_cabac_decision_noup(cb, 3+3, 0);        /* cbp_luma == 0 */
    vk_cabac_decision_noup(cb, 3+4, 0);        /* cbp_chroma == 0 */
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
    uint8_t *recon = malloc((size_t)rs*(h+32));
    memset(recon, 128, (size_t)rs*(h+32));
    int *mbcbp = calloc((size_t)mw*mh, sizeof(int));  /* bit 8 = luma DC nonzero */
    int mbnum = 0;
    for (int my = 0; my < mh; ++my)
        for (int mx = 0; mx < mw; ++mx) {
            if (mbnum++ > 0) vk_cabac_terminal(cb);  /* x264 encoder.c:2870 */
            int left = mx > 0, top = my > 0;
            uint8_t t[16], l[16], tl;
            uint8_t *rbase = recon + ro;
            for (int i = 0; i < 16; ++i) {
                t[i] = top ? rbase[(my*16-1)*rs + mx*16+i]
                           : (mx == 0 ? 128 : rbase[(my*16)*rs + mx*16-16+i]);
                l[i] = left ? rbase[(my*16+i)*rs + mx*16-1] : 128;
            }
            tl = top ? rbase[(my*16-1)*rs + mx*16-1] : frame[0];
            uint8_t pred[256], rmb[256], trial[256];
            int lambda = vk_lambda(qp), best = 0x7fffffff, bestmode = 0;
            for (int m = 0; m < 4; ++m) {
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
            vk_predict16(pred, t, l, tl, my == 0 ? 2 : bestmode);
            int q[16] = {0};
            compute_i16_dc(frame, w, mx*16, my*16, pred, qp, q, rmb);
            int nz = 0;
            for (int i = 0; i < 16; ++i) if (q[i]) nz = 1;
            /* cbf context: 85 + 2*nzb + nza; nza/nzb from left/top cbp bit 8 */
            /* x264 i_cbp_left/top = -1 when unavailable => bit 8 is 1 */
            int lc = left ? mbcbp[my*mw+mx-1] : 0x1ff;
            int tc = top ? mbcbp[(my-1)*mw+mx] : 0x1ff;
            int nza = (lc >> 8) & 1, nzb = (tc >> 8) & 1;
            cabac_mb_type_i16(cb, left, top, my == 0 ? 2 : bestmode);
            cabac_chroma_mode(cb, 0);
            cabac_qp_delta0(cb);
            vk_cabac_decision(cb, 85 + 2*nzb + nza, nz);
            if (nz) cabac_coeffs(cb, q, 0);
            mbcbp[my*mw+mx] = nz ? 0x100 : 0;
            for (int yy = 0; yy < 16; ++yy)
                memcpy(rbase + (my*16+yy)*rs + mx*16, rmb + yy*16, 16);
        }
    free(mbcbp);
    free(recon);
    int bytes = vk_cabac_flush(cb, 0);
    bs->p = data + bytes;
    return bytes;
}

