/* SPDX-License-Identifier: GPL-2.0-or-later
 * P-frame encoder for the C oracle.
 *
 * Scope: x264 ultrafast CAVLC, 16x16 partitions only, integer-pel ME,
 * P_SKIP decided by macroblock_probe_pskip at the predicted MV. Residual
 * coding for non-skip MBs is not implemented here; tests using this path
 * must select material where every coded MB quantises to cbp=0.
 */
#include "pframe.h"
#include "motion.h"
#include "slice.h"
#include <stdlib.h>
#include <string.h>

#define LPAD 16   /* luma reference border */
#define CPAD 8    /* chroma reference border */

/* P_SKIP MV predictor (quarter-pel): zero if left/top unavailable or zero,
 * otherwise the 16x16 median of left/top/top-right. */
static void pskip_mv(int lx, int ly, int tx, int ty, int cx, int cy,
                     int have_l, int have_t, int have_c,
                     int *px, int *py)
{
    if (!have_l || !have_t || (lx == 0 && ly == 0) || (tx == 0 && ty == 0)) {
        *px = *py = 0;
        return;
    }
    vk_mvp16(lx, ly, tx, ty, have_c ? cx : lx, have_c ? cy : ly, px, py);
}

/* Quantise one 4x4 residual block; return 1 if all 16 coefficients are zero.
 * `dc` receives the raw DC coefficient for the caller's Hadamard transform. */
static int block_zero(const uint8_t *src, int ss, const uint8_t *pred, int ps,
                      const int mf[16], const int bias[16], int *dc)
{
    uint8_t s[16], p[16];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            s[y*4+x] = src[y*ss+x];
            p[y*4+x] = pred[y*ps+x];
        }
    int d[16];
    vk_ref_dct4x4(d, s, p);
    *dc = d[0];
    d[0] = 0;
    for (int i = 0; i < 16; ++i)
        if (vk_ref_quant_one(d[i], mf[i], bias[i])) return 0;
    return 1;
}

/* Decide P_SKIP: 1 iff the residual at the P_SKIP MV predictor quantises
 * to zero in both planes (x264 macroblock_probe_pskip). */
static int probe(const uint8_t *cur, const uint8_t *refp,
                 int w, int h, int rs,
                 int x, int y, int mvx_qp, int mvy_qp, int qp)
{
    int mf[16], bias[16], deq[16], cmf[16], cbias[16], cdeq[16];
    vk_ref_quant_tables(qp, 0, mf, bias, deq);
    vk_ref_quant_tables(qp, 2, cmf, cbias, cdeq);
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

    /* luma AC + DC */
    int dc4[16];
    for (int b = 0; b < 16; ++b) {
        int by = (b/4)*4, bx = (b%4)*4;
        if (!block_zero(cy + (y+by)*w + (x+bx), w,
                        ry + (y+by+my)*rs + (x+bx+mx), rs, mf, bias, &dc4[b]))
            return 0;
    }
    int had[16];
    memcpy(had, dc4, sizeof(had));
    vk_ref_dct4x4dc(had);
    for (int i = 0; i < 16; ++i)
        if (vk_ref_quant_one(had[i], mf[0] >> 1, bias[0] << 1)) return 0;

    /* chroma */
    int cmx = mvx_qp / 8, cmy = mvy_qp / 8;
    for (int pl = 0; pl < 2; ++pl) {
        const uint8_t *c = pl ? cv : cu;
        const uint8_t *r = pl ? rv : ru;
        int cdc[4];
        for (int b = 0; b < 4; ++b) {
            int by = (b/2)*4, bx = (b%2)*4;
            int dc;
            if (!block_zero(c + (y/2+by)*cs + (x/2+bx), cs,
                            r + (y/2+by+cmy+CPAD)*crs + (x/2+bx+cmx+CPAD), crs,
                            cmf, cbias, &dc))
                return 0;
            cdc[b] = dc;
        }
        int a = cdc[0] + cdc[1], b2 = cdc[2] + cdc[3];
        int c1 = cdc[0] - cdc[1], d = cdc[2] - cdc[3];
        int ch[4] = { a+b2, a-b2, c1+d, c1-d };
        for (int i = 0; i < 4; ++i)
            if (vk_ref_quant_one(ch[i], cmf[0] >> 1, cbias[0] << 1)) return 0;
    }
    return 1;
}

void vk_encode_p_frame(vk_bs_t *bs, const vk_sps_t *sps, const vk_pps_t *pps,
                       const uint8_t *cur, const uint8_t *ref,
                       int w, int h, int qp, int frame_num)
{
    int mw = w/16, mh = h/16;
    int rs = w + 2*LPAD;
    int crs = w/2 + 2*CPAD;
    size_t ysz = (size_t)rs*(h+2*LPAD);
    size_t csz = (size_t)crs*(h/2+2*CPAD);
    uint8_t *refp = calloc(1, ysz + 2*csz);
    vk_pad_plane(refp, rs, ref, w, w, h, LPAD);
    vk_pad_plane(refp + ysz, crs, ref + (size_t)w*h, w/2, w/2, h/2, CPAD);
    vk_pad_plane(refp + ysz + csz, crs,
                 ref + (size_t)w*h + (size_t)(w/2)*(h/2), w/2, w/2, h/2, CPAD);

    vk_bs_write_ue(bs, 0);
    vk_bs_write_ue(bs, 5);
    vk_bs_write_ue(bs, 0);
    vk_bs_write(bs, (unsigned)sps->i_log2_max_frame_num, (uint32_t)frame_num);
    vk_bs_write1(bs, 0);
    vk_bs_write1(bs, 0);
    vk_bs_write1(bs, 0);
    vk_bs_write_se(bs, qp - pps->i_pic_init_qp);
    vk_bs_write_ue(bs, 1);

    /* MV cache in quarter-pel units. */
    int (*mv)[2] = calloc((size_t)mw*mh, 2*sizeof(int));
    int skip_run = 0;
    for (int my = 0; my < mh; ++my)
        for (int mx = 0; mx < mw; ++mx) {
            int idx = my*mw+mx;
            int have_l = mx > 0, have_t = my > 0;
            int have_c = my > 0 && mx+1 < mw;
            int lx = have_l ? mv[idx-1][0] : 0, ly = have_l ? mv[idx-1][1] : 0;
            int tx = have_t ? mv[idx-mw][0] : 0, ty = have_t ? mv[idx-mw][1] : 0;
            int have_tl = have_t && (have_c || mx > 0);
            int cx = have_c ? mv[idx-mw+1][0]
                            : (have_tl ? mv[idx-mw-1][0] : 0);
            int cy = have_c ? mv[idx-mw+1][1]
                            : (have_tl ? mv[idx-mw-1][1] : 0);

            int psk_x, psk_y;
            pskip_mv(lx, ly, tx, ty, cx, cy, have_l, have_t, have_tl,
                     &psk_x, &psk_y);

            if (probe(cur, refp, w, h, rs, mx*16, my*16, psk_x, psk_y, qp)) {
                mv[idx][0] = psk_x; mv[idx][1] = psk_y;
                ++skip_run;
                continue;
            }
            if (skip_run) { vk_bs_write_ue(bs, (uint32_t)skip_run); skip_run = 0; }

            int mvpx, mvpy;
            vk_mvp16(lx, ly, tx, ty, cx, cy, &mvpx, &mvpy);
            int mvx, mvy;
            vk_me16(cur, w, refp, rs, LPAD, mx*16, my*16,
                    mvpx/4, mvpy/4, 16, &mvx, &mvy);
            mv[idx][0] = mvx*4; mv[idx][1] = mvy*4;
            vk_bs_write_ue(bs, 0);                          /* P_L0_16x16 */
            vk_bs_write_se(bs, mvx*4 - mvpx);               /* mvd_x */
            vk_bs_write_se(bs, mvy*4 - mvpy);               /* mvd_y */
            vk_bs_write_ue(bs, 0);                          /* cbp (0) */
        }
    if (skip_run) vk_bs_write_ue(bs, (uint32_t)skip_run);
    vk_bs_rbsp_trailing(bs);
    free(mv); free(refp);
}
