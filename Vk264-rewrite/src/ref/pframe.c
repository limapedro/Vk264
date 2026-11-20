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
#include "cavlc.h"
#include <stdlib.h>
#include <string.h>

#define LPAD 16   /* luma reference border */
#define CPAD 8    /* chroma reference border */

/* x264_lambda_tab (common/tables.c), QP 0..51. */
static const uint16_t lambda_tab[52] = {
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    2,2,2,2,3,3,3,4, 4,4,5,6,6,7,8,9,
    10,11,13,14,16,18,20,23, 25,29,32,36,40,45,51,57,
    64,72,81,91
};

/* 16x16 MV predictor with x264's reference-availability logic: with a
 * single valid neighbour use it directly, otherwise the component median. */
static void mvp16_avail(int lx, int ly, int tx, int ty, int cx, int cy,
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

/* x264 JVT-B118 decimate score: low score means coefficients can be emptied
 * to zero during P_SKIP probing. */
static int decimate_score(const int *d, int n)
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

static const uint8_t pframe_zz4[16] = {
    0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15
};

/* Quantise one 4x4 residual block and return its decimate score. `raw_dc`
 * receives the raw DC coefficient (for the chroma Hadamard). `include_dc`
 * selects inter-luma (DC in-block) vs chroma AC (DC is zeroed). */
static int block_decimate(const uint8_t *src, int ss, const uint8_t *pred, int ps,
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
    for (int i = 0; i < 16; ++i) z[i] = d[pframe_zz4[i]];
    return include_dc ? decimate_score(z, 16) : decimate_score(z+1, 15);
}

/* Decide P_SKIP: 1 iff the residual at the P_SKIP MV predictor quantises
 * to zero in both planes (x264 macroblock_probe_pskip). */
static int probe(const uint8_t *cur, const uint8_t *refp,
                 int w, int h, int rs,
                 int x, int y, int mvx_qp, int mvy_qp, int qp)
{
    int mf[16], bias[16], deq[16], cmf[16], cbias[16], cdeq[16];
    /* P-frame probe uses the INTER quant categories CQM_4PY / CQM_4PC,
     * whose deadzone is 11 (intra categories use 21). */
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

    /* inter luma: 16 4x4 blocks, DC quantised in-block; x264's probe only
     * fails when the accumulated JVT-B118 decimate score reaches 6. */
    int unused, dec = 0;
    for (int b = 0; b < 16; ++b) {
        int by = (b/4)*4, bx = (b%4)*4;
        dec += block_decimate(cy + (y+by)*w + (x+bx), w,
                              ry + (y+by+my)*rs + (x+bx+mx), rs, mf, bias,
                              &unused, 1);
        if (dec >= 6) return 0;
    }

    /* x264 mc_chroma takes the luma qpel MV directly: one luma qpel
     * (1/4 luma px) equals 1/8 chroma px in 4:2:0. */
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
        if (ssd < thresh) continue;  /* x264: chroma SSD shortcut */
        int cdc[4];
        for (int b = 0; b < 4; ++b) {
            int by = (b/2)*4, bx = (b%2)*4;
            uint8_t pb[16];
            for (int yy = 0; yy < 4; ++yy)
                for (int xx = 0; xx < 4; ++xx)
                    pb[yy*4+xx] = p8[(by+yy)*8 + bx+xx];
            block_decimate(c + (y/2+by)*cs + (x/2+bx), cs,
                           pb, 4, cmf, cbias, &cdc[b], 0);
        }
        int a = cdc[0] + cdc[1], b2 = cdc[2] + cdc[3];
        int c1 = cdc[0] - cdc[1], d = cdc[2] - cdc[3];
        int ch[4] = { a+b2, a-b2, c1+d, c1-d };
        for (int i = 0; i < 4; ++i)
            if (vk_ref_quant_one(ch[i], cmf[0] >> 1, cbias[0] << 1)) return 0;
        if (ssd < thresh*4) continue;  /* x264: skip chroma AC check */
        int cdec = 0;
        for (int b = 0; b < 4; ++b) {
            int by = (b/2)*4, bx = (b%2)*4;
            uint8_t pb[16];
            for (int yy = 0; yy < 4; ++yy)
                for (int xx = 0; xx < 4; ++xx)
                    pb[yy*4+xx] = p8[(by+yy)*8 + bx+xx];
            int tmp;
            cdec += block_decimate(c + (y/2+by)*cs + (x/2+bx), cs,
                                   pb, 4, cmf, cbias, &tmp, 0);
            if (cdec >= 7) return 0;
        }
    }
    return 1;
}

/* x264 tables for inter residual CAVLC. */
static const uint8_t rz4[16] = {0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15};
static const uint8_t rblock_idx[16] = {0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15};
static const uint8_t rscan8[16] = {12,13,20,21,14,15,22,23,28,29,36,37,30,31,38,39};
static const uint8_t rct_index[17] = {0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3};
static const uint8_t cbp_golomb[48] = {
    0,2,3,7,4,8,17,13,5,18,9,14,10,15,16,11,
    1,32,33,36,34,37,44,40,35,45,38,41,39,42,43,19,
    6,24,25,20,26,21,46,28,27,47,22,29,23,30,31,12
};

/* Compute the inter residual at the given MV, emit cbp + CAVLC coefficient
 * data, and record neighbour nnz edges. Returns cbp. */
static int code_residual(vk_bs_t *bs, const uint8_t *cur, const uint8_t *refp,
                         int w, int h, int rs, int x, int y,
                         int mvx_qp, int mvy_qp, int qp,
                         uint8_t on[16], uint8_t oc[2][4], uint8_t *od,
                         const uint8_t *ll, const uint8_t *tl,
                         const uint8_t lc[2][2], const uint8_t tc[2][2])
{
    int mf[16], bias[16], deq[16], cmf[16], cbias[16], cdeq[16];
    vk_ref_quant_tables(qp, 1, mf, bias, deq);
    vk_ref_quant_tables(qp, 3, cmf, cbias, cdeq);
    int mx = mvx_qp/4, my = mvy_qp/4;
    int cs = w/2, crs = rs/2, cp = CPAD;
    size_t ysz = (size_t)rs*(h+2*LPAD), csz = (size_t)crs*(h/2+2*CPAD);
    const uint8_t *cy = cur, *cu = cur + (size_t)w*h, *cv = cu + (size_t)cs*cs;
    const uint8_t *ry = refp + LPAD*rs + LPAD;
    const uint8_t *ru = refp + ysz, *rv = ru + csz;

    int q[16][16], nnz[16] = {0}, dec8 = 0, dec_total = 0, cbp_luma = 0;
    for (int b = 0; b < 16; ++b) {
        int by = (b/4)*4, bx = (b%4)*4;
        uint8_t sb[16], pb[16];
        for (int yy = 0; yy < 4; ++yy)
            for (int xx = 0; xx < 4; ++xx) {
                sb[yy*4+xx] = cy[(y+by+yy)*w + (x+bx+xx)];
                pb[yy*4+xx] = ry[(y+by+yy+my)*rs + (x+bx+xx+mx)];
            }
        int d[16];
        vk_ref_dct4x4(d, sb, pb);
        for (int i = 0; i < 16; ++i) d[i] = vk_ref_quant_one(d[i], mf[i], bias[i]);
        for (int i = 0; i < 16; ++i) q[b][i] = d[rz4[i]];
        for (int i = 0; i < 16; ++i) nnz[b] += d[i] != 0;
        int zs[16];
        for (int i = 0; i < 16; ++i) zs[i] = q[b][i];
        if (dec8 < 6) dec8 += decimate_score(zs, 16);
        if ((b & 3) == 3) {  /* end of an 8x8 group */
            if (dec8 >= 4) cbp_luma |= 1 << (b/4);
            else for (int k = b-3; k <= b; ++k) {
                memset(q[k], 0, sizeof(q[k]));
                nnz[k] = 0;
            }
            dec_total += dec8;
            dec8 = 0;
        }
    }
    if (dec_total < 6) {
        cbp_luma = 0;
        for (int b = 0; b < 16; ++b) { memset(q[b], 0, sizeof(q[b])); nnz[b] = 0; }
    }
    for (int b = 0; b < 16; ++b) on[b] = (uint8_t)nnz[b];

    int cdc[2][4], cnnz[2][4] = {{0}}, cacq[2][4][16];
    int cvx8 = mvx_qp, cvy8 = mvy_qp;
    int d8x = cvx8&7, d8y = cvy8&7;
    int cA=(8-d8x)*(8-d8y), cB=d8x*(8-d8y), cC=(8-d8x)*d8y, cD=d8x*d8y;
    int cox = (cvx8>>3)+cp, coy = (cvy8>>3)+cp;
    for (int pl = 0; pl < 2; ++pl) {
        const uint8_t *c = pl ? cv : cu, *r = pl ? rv : ru;
        for (int b = 0; b < 4; ++b) {
            int by = (b/2)*4, bx = (b%2)*4;
            uint8_t sb[16], pb[16];
            for (int yy = 0; yy < 4; ++yy)
                for (int xx = 0; xx < 4; ++xx) {
                    int sx = x/2+bx+xx+cox, sy = y/2+by+yy+coy;
                    sb[yy*4+xx] = c[(y/2+by+yy)*cs + (x/2+bx+xx)];
                    int a=r[sy*crs+sx], b2=r[sy*crs+sx+1];
                    int cc=r[(sy+1)*crs+sx], dd=r[(sy+1)*crs+sx+1];
                    pb[yy*4+xx] = (uint8_t)((cA*a+cB*b2+cC*cc+cD*dd+32)>>6);
                }
            int d[16];
            vk_ref_dct4x4(d, sb, pb);
            cdc[pl][b] = d[0];
            d[0] = 0;
            for (int i = 0; i < 16; ++i) d[i] = vk_ref_quant_one(d[i], cmf[i], cbias[i]);
            for (int i = 0; i < 16; ++i) cacq[pl][b][i] = d[rz4[i]];
            for (int i = 0; i < 16; ++i) cnnz[pl][b] += d[i] != 0;
            oc[pl][b] = (uint8_t)cnnz[pl][b];
        }
    }
    int chroma_dc[2][4], cbp_chroma = 0, cdnz[2] = {0};
    for (int pl = 0; pl < 2; ++pl) {
        int a=cdc[pl][0]+cdc[pl][1], b=cdc[pl][2]+cdc[pl][3];
        int c=cdc[pl][0]-cdc[pl][1], d=cdc[pl][2]-cdc[pl][3];
        int had[4]={a+b,a-b,c+d,c-d}, t[4];
        for (int i = 0; i < 4; ++i)
            t[i] = vk_ref_quant_one(had[i], cmf[0]>>1, cbias[0]<<1);
        chroma_dc[pl][0]=t[0]; chroma_dc[pl][1]=t[2];
        chroma_dc[pl][2]=t[1]; chroma_dc[pl][3]=t[3];
        for (int i = 0; i < 4; ++i) {
            cbp_chroma |= t[i] != 0;
            cdnz[pl] += t[i] != 0;
        }
    }
    for (int pl = 0; pl < 2; ++pl)
        for (int b = 0; b < 4; ++b) if (cnnz[pl][b]) cbp_chroma = 2;
    *od = (uint8_t)(cdnz[0] | (cdnz[1]<<4));

    int cbp = cbp_luma | (cbp_chroma<<4);
    vk_bs_write_ue(bs, cbp_golomb[cbp]);
    if (!cbp) return 0;
    vk_bs_write_se(bs, 0);
    if (cbp_luma) {
        uint8_t cache[64] = {0};
        static const int lp[4] = {11,19,27,35}, tp[4] = {4,5,6,7};
        for (int i = 0; i < 4; ++i) { cache[tp[i]] = tl[i]; cache[lp[i]] = ll[i]; }
        for (int idx = 0; idx < 16; ++idx) {
            int b = rblock_idx[idx], p = rscan8[idx];
            if (!(cbp_luma & (1 << (b/4)))) continue;
            int sum = cache[p-1] + cache[p-8];
            int pred = sum < 0x80 ? (sum+1)>>1 : sum & 0x7f;
            vk_cavlc_block_residual(bs, VK_DCT_LUMA_4x4, q[b],
                                    rct_index[pred & 0x7f], 1, 16);
            cache[p] = (uint8_t)nnz[b];
        }
    }
    if (cbp_chroma) {
        for (int pl = 0; pl < 2; ++pl) {
            int block[16] = {0};
            for (int i = 0; i < 4; ++i) block[i] = chroma_dc[pl][i];
            vk_cavlc_block_residual(bs, VK_DCT_CHROMA_DC, block, 4, 1, 4);
        }
        if (cbp_chroma == 2) {
            static const uint8_t cs8[8] = {52,53,60,61,92,93,100,101};
            uint8_t cc[128] = {0};
            for (int i = 0; i < 2; ++i) {
                cc[cs8[i]-8] = tc[0][i]; cc[cs8[i*2]-1] = lc[0][i];
                cc[cs8[4+i]-8] = tc[1][i]; cc[cs8[4+i*2]-1] = lc[1][i];
            }
            cc[40] = (uint8_t)cdnz[0]; cc[80] = (uint8_t)cdnz[1];
            for (int pl = 0; pl < 2; ++pl)
                for (int b = 0; b < 4; ++b) {
                    int k = pl*4+b, p = cs8[k];
                    int sum = cc[p-1] + cc[p-8];
                    int pred = sum < 0x80 ? (sum+1)>>1 : sum & 0x7f;
                    vk_cavlc_block_residual(bs, VK_DCT_CHROMA_AC,
                                            cacq[pl][b], rct_index[pred & 0x7f], 1, 15);
                    cc[p] = (uint8_t)cnnz[pl][b];
                }
        }
    }
    return cbp;
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

    /* MV cache (quarter-pel) and skip flags. x264 stores MV=0 in the cache
     * for skipped MBs even though the bitstream carries the predictor MV. */
    int (*mv)[2] = calloc((size_t)mw*mh, 2*sizeof(int));
    uint8_t *is_skip = calloc((size_t)mw*mh, 1);
    uint8_t (*nnz_c)[16] = calloc((size_t)mw*mh, 16);
    uint8_t (*cnnz_c)[2][4] = calloc((size_t)mw*mh, 8);
    uint8_t *dcnz_c = calloc((size_t)mw*mh, 1);
    int skip_run = 0;
    for (int my = 0; my < mh; ++my)
        for (int mx = 0; mx < mw; ++mx) {
            int idx = my*mw+mx;
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
            pskip_mv(lx, ly, tx, ty, cx, cy, have_l, have_t, have_c || have_tl,
                     &psk_x, &psk_y);

            /* Fast P_SKIP is only probed when a neighbour is already P_SKIP. */
            int skip_neigh = (have_l && is_skip[idx-1])
                           || (have_t && is_skip[idx-mw])
                           || (have_tl && is_skip[idx-mw-1])
                           || (have_c && is_skip[idx-mw+1]);
            int do_skip = 0;
            if (skip_neigh &&
                probe(cur, refp, w, h, rs, mx*16, my*16, psk_x, psk_y, qp))
                do_skip = 1;

            int mvpx = 0, mvpy = 0, mvx = 0, mvy = 0;
            if (!do_skip) {
                mvp16_avail(lx, ly, tx, ty, cx, cy, have_l, have_t,
                            have_c || have_tl, &mvpx, &mvpy);
                vk_me16_dia(cur, w, refp, rs, LPAD, mx*16, my*16,
                            mvpx/4, mvpy/4, 16,
                            lambda_tab[qp < 0 ? 0 : (qp > 51 ? 51 : qp)],
                            &mvx, &mvy);
                /* x264 promotes P16 to P_SKIP when cbp==0 and MV==pskip_mv. */
                int qx = mvx*4, qy = mvy*4;
                if (qx == psk_x && qy == psk_y &&
                    probe(cur, refp, w, h, rs, mx*16, my*16, qx, qy, qp))
                    do_skip = 1;
            }

            if (do_skip) {
                mv[idx][0] = psk_x; mv[idx][1] = psk_y;
                is_skip[idx] = 1;
                ++skip_run;
                continue;
            }
            /* CAVLC requires mb_skip_run before every coded MB, including 0. */
            vk_bs_write_ue(bs, (uint32_t)skip_run);
            skip_run = 0;
            mv[idx][0] = mvx*4; mv[idx][1] = mvy*4;
            vk_bs_write_ue(bs, 0);                          /* P_L0_16x16 */
            vk_bs_write_se(bs, mvx*4 - mvpx);               /* mvd_x */
            vk_bs_write_se(bs, mvy*4 - mvpy);               /* mvd_y */

            uint8_t ll[4], tl[4], lc[2][2], tc[2][2];
            for (int i = 0; i < 4; ++i) {
                ll[i] = have_l ? nnz_c[idx-1][i*4+3] : 0x80;
                tl[i] = have_t ? nnz_c[idx-mw][12+i] : 0x80;
            }
            for (int p = 0; p < 2; ++p)
                for (int i = 0; i < 2; ++i) {
                    lc[p][i] = have_l ? cnnz_c[idx-1][p][i*2+1] : 0x80;
                    tc[p][i] = have_t ? cnnz_c[idx-mw][p][2+i] : 0x80;
                }
            uint8_t on[16], oc[2][4], od;
            code_residual(bs, cur, refp, w, h, rs, mx*16, my*16,
                          mvx*4, mvy*4, qp, on, oc, &od, ll, tl, lc, tc);
            memcpy(nnz_c[idx], on, 16);
            memcpy(cnnz_c[idx], oc, 8);
            dcnz_c[idx] = od;
        }
    if (skip_run) vk_bs_write_ue(bs, (uint32_t)skip_run);
    vk_bs_rbsp_trailing(bs);
    free(dcnz_c); free(cnnz_c); free(nnz_c);
    free(is_skip); free(mv); free(refp);
}
