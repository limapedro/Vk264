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
#include "intra16.h"
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
                        int lref, int tref, int cref,
                        int *px, int *py)
{
    /* x264_mb_predict_mv_16x16: only ref==0 neighbours participate.
     * Intra MBs carry ref=-1; unavailable edges ref=-2. */
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

/* P_SKIP MV predictor (quarter-pel), ref-aware like x264_mb_predict_mv_pskip.
 * x264 bails to MV=0 only for an edge-neighbour (ref=-2) or a ref==0 neighbour
 * whose MV is zero; intra neighbours (ref=-1) still participate, and the
 * ref-aware 16x16 selection picks the single valid neighbour directly rather
 * than taking a component median. */
static void pskip_mv(int lx, int ly, int tx, int ty, int cx, int cy,
                     int lref, int tref, int cref,
                     int *px, int *py)
{
    if (lref == -2 || tref == -2 ||
        (lref == 0 && lx == 0 && ly == 0) ||
        (tref == 0 && tx == 0 && ty == 0)) {
        *px = *py = 0;
        return;
    }
    mvp16_avail(lx, ly, tx, ty, cx, cy, lref, tref, cref, px, py);
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
    0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15
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

/* x264 pixel SATD: sum of absolute Hadamard coefficients of the residual,
 * rounded >>1 once per 8x4 block (x264_pixel_satd_8x4). */
static int satd4_raw(const uint8_t *a, int sa, const uint8_t *b, int sb)
{
    int d[4][4];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            d[y][x] = (int)a[y*sa+x] - (int)b[y*sb+x];
    for (int y = 0; y < 4; ++y) {
        int s0 = d[y][0]+d[y][3], s1 = d[y][1]+d[y][2];
        int e0 = d[y][0]-d[y][3], e1 = d[y][1]-d[y][2];
        d[y][0]=s0+s1; d[y][1]=e0+e1; d[y][2]=s0-s1; d[y][3]=e0-e1;
    }
    for (int x = 0; x < 4; ++x) {
        int s0 = d[0][x]+d[3][x], s1 = d[1][x]+d[2][x];
        int e0 = d[0][x]-d[3][x], e1 = d[1][x]-d[2][x];
        d[0][x]=s0+s1; d[1][x]=e0+e1; d[2][x]=s0-s1; d[3][x]=e0-e1;
    }
    int sum = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            int v = d[y][x];
            sum += v < 0 ? -v : v;
        }
    return sum;
}

static int satd8x4(const uint8_t *a, int sa, const uint8_t *b, int sb)
{
    return (satd4_raw(a, sa, b, sb) + satd4_raw(a+4, sa, b+4, sb)) >> 1;
}

static int satd16(const uint8_t *a, int sa, const uint8_t *b, int sb)
{
    int s = 0;
    for (int y = 0; y < 16; y += 4)
        for (int x = 0; x < 16; x += 8)
            s += satd8x4(a + y*sa + x, sa, b + y*sb + x, sb);
    return s;
}

static int satd8(const uint8_t *a, int sa, const uint8_t *b, int sb)
{
    int s = 0;
    for (int y = 0; y < 8; y += 4)
        s += satd8x4(a + y*sa, sa, b + y*sb, sb);
    return s;
}

/* x264 tables for inter residual CAVLC. */
static const uint8_t rz4[16] = {0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15};
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
                         const uint8_t lc[2][2], const uint8_t tc[2][2],
                         uint8_t *rcony, uint8_t *rconu, uint8_t *rconv,
                         int rdy, int rdu, int rdv)
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

    int q[16][16], nnz[16] = {0};
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
    }
    /* x264 groups 4x4 DCT blocks into 8x8 spatial groups via
     * block_idx_xy_1d: group g holds raster blocks {0,1,4,5}+2*(g&1)+8*(g>>1). */
    static const uint8_t grp_blocks[4][4] = {
        {0,1,4,5}, {2,3,6,7}, {8,9,12,13}, {10,11,14,15}
    };
    uint8_t block_group[16];
    for (int g = 0; g < 4; ++g)
        for (int i = 0; i < 4; ++i) block_group[grp_blocks[g][i]] = (uint8_t)g;
    int cbp_luma = 0, dec_total = 0;
    for (int g = 0; g < 4; ++g) {
        int dec8 = 0, any = 0;
        for (int i = 0; i < 4; ++i) {
            int b = grp_blocks[g][i];
            if (!nnz[b]) continue;
            any = 1;
            if (dec8 < 6) dec8 += decimate_score(q[b], 16);
        }
        if (any) {
            dec_total += dec8;
            if (dec8 >= 4) cbp_luma |= 1 << g;
            else for (int i = 0; i < 4; ++i) {
                int b = grp_blocks[g][i];
                memset(q[b], 0, sizeof(q[b]));
                nnz[b] = 0;
            }
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
    /* x264 mb_encode_chroma_internal: b_dct_decimate=1 for P-slices, so
     * each chroma plane's AC is decimated when its JVT-B118 score < 7.
     * decimate_score() ignores the zeroed DC at index 0, matching
     * x264's decimate_score15() on the zigzag-scanned AC coefficients. */
    for (int pl = 0; pl < 2; ++pl) {
        int dec = 0;
        for (int b = 0; b < 4; ++b) {
            if (!cnnz[pl][b] || dec >= 7) continue;
            dec += decimate_score(cacq[pl][b], 16);
        }
        if (dec < 7) {
            for (int b = 0; b < 4; ++b) {
                cnnz[pl][b] = 0;
                memset(cacq[pl][b], 0, sizeof(cacq[pl][b]));
            }
        } else {
            cbp_chroma = 2;   /* AC survives decimation */
        }
    }
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
            if (!(cbp_luma & (1 << block_group[b]))) continue;
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
    /* ---- residual reconstruction into the current-frame ref planes ---- */
    if (cbp_luma) {
        for (int b = 0; b < 16; ++b) {
            if (!nnz[b]) continue;
            int blk[16];
            for (int i = 0; i < 16; ++i) blk[rz4[i]] = q[b][i];
            vk_ref_dequant4(blk, deq, qp);
            vk_ref_idct4(blk);
            int by = (b/4)*4, bx = (b%4)*4;
            vk_ref_add_block(rcony + by*rdy + bx, rdy, blk);
        }
    }
    if (cbp_chroma) {
        for (int pl = 0; pl < 2; ++pl) {
            int d0 = chroma_dc[pl][0]+chroma_dc[pl][1];
            int d1 = chroma_dc[pl][2]+chroma_dc[pl][3];
            int d2 = chroma_dc[pl][0]-chroma_dc[pl][1];
            int d3 = chroma_dc[pl][2]-chroma_dc[pl][3];
            int dmf = cdeq[0] << (qp/6);
            int cdq[4] = { ((d0+d1)*dmf)>>5, ((d0-d1)*dmf)>>5,
                           ((d2+d3)*dmf)>>5, ((d2-d3)*dmf)>>5 };
            uint8_t *dst = pl ? rconv : rconu;
            int stride = pl ? rdv : rdu;
            for (int b = 0; b < 4; ++b) {
                int by = (b/2)*4, bx = (b%2)*4;
                if (cnnz[pl][b]) {
                    int blk[16];
                    for (int i = 0; i < 16; ++i) blk[rz4[i]] = cacq[pl][b][i];
                    vk_ref_dequant4(blk, cdeq, qp);
                    blk[0] = cdnz[pl] ? cdq[b] : 0;
                    vk_ref_idct4(blk);
                    vk_ref_add_block(dst + by*stride + bx, stride, blk);
                } else if (cdnz[pl]) {
                    vk_ref_add_block_dc(dst + by*stride + bx, stride, cdq[b]);
                }
            }
        }
    }
    return cbp;
}

/* Code the luma DC residual block of an I16 MB in a P-slice. x264 always
 * codes the 16x16 DC block for I16, even when cbp_luma==0. */
static int code_i16_dc(vk_bs_t *bs, const uint8_t *cur, int w, int x, int y,
                       const uint8_t pred[256], int qp,
                       const uint8_t *ll, const uint8_t *tl,
                       uint8_t *rcony, int rdy, int write)
{
    static const uint8_t zz[16] = {0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15};
    static const uint8_t ds[16] = {3,2,2,1,1,1,0,0,0,0,0,0,0,0,0,0};
    static const int ctt[17] = {0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3};
    int mf[16], bias[16], deq[16];
    vk_ref_quant_tables(qp, 0, mf, bias, deq);
    int dc[16], dct[16][16], nnz[16];
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
        nnz[b] = 0;
        for (int i = 0; i < 16; ++i) nnz[b] += dct[b][i] != 0;
    }
    /* x264: cbp_luma is all-or-nothing. block_cbp=0xf if ANY 4x4 block is
     * nonzero, and a single decimate_score accumulates over all nonzero
     * blocks (capped at 6). If decimate_score < 6, all nnz are cleared and
     * cbp becomes 0; otherwise cbp=0xf. Per-group cbp bits are NOT used. */
    int cbp_luma = 0, decimate = 0;
    for (int b = 0; b < 16; ++b) {
        if (!nnz[b]) continue;
        cbp_luma = 0xf;
        if (decimate < 6) {
            int z[16];
            for (int i = 0; i < 16; ++i) z[i] = dct[b][zz[i]];
            /* x264 uses decimate_score15 for I16 luma AC, i.e. it skips the
             * DC coefficient (z[0], already zeroed here). Scoring all 16
             * inflates the leading zero run by one and lowers the score,
             * wrongly decimating blocks x264 keeps. */
            decimate += decimate_score(z+1, 15);
        }
    }
    if (decimate < 6) {
        cbp_luma = 0;
        for (int b = 0; b < 16; ++b) nnz[b] = 0;
    }
    /* DC Hadamard quantized */
    vk_ref_dct4x4dc(dc);
    int dcmf = mf[0] >> 1, dcbias = bias[0] << 1;
    int q[16];
    for (int i = 0; i < 16; ++i) q[i] = vk_ref_quant_one(dc[i], dcmf, dcbias);
    if (write) {
        int za = ll[0], zb = tl[0], sum = za+zb;
        int prednc = sum < 0x80 ? (sum+1)>>1 : sum & 0x7f;
        int zq[16];
        for (int i = 0; i < 16; ++i) zq[i] = q[zz[i]];
        vk_cavlc_block_residual(bs, VK_DCT_LUMA_DC, zq, ctt[prednc & 0x7f], 1, 16);
        if (cbp_luma) {
            static const int bidx[16] = {0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15};
            static const int s8[16] = {12,13,20,21,14,15,22,23,28,29,36,37,30,31,38,39};
            static const int bgrp[16] = {0,0,1,1,0,0,1,1,2,2,3,3,2,2,3,3};
            uint8_t cache[64] = {0};
            /* ll[i]/tl[i] are already the neighbour MB's relevant nnz
             * counts (left blocks i*4+3, top blocks 12+i) — packed by the
             * caller. Do not index them again (out-of-bounds read). */
            for (int i = 0; i < 4; ++i) {
                cache[11+i*8] = ll[i];
                cache[4+i] = tl[i];
            }
            for (int i = 0; i < 16; ++i) {
                int b = bidx[i], p = s8[i];
                if (!(cbp_luma & (1<<bgrp[b]))) continue;
                int s = cache[p-1] + cache[p-8];
                int pred = s < 0x80 ? (s+1)>>1 : s & 0x7f;
                int ac[16];
                for (int j = 0; j < 15; ++j) ac[j] = dct[b][zz[j+1]];  /* skips DC */
                vk_cavlc_block_residual(bs, VK_DCT_LUMA_4x4, ac, ctt[pred & 0x7f], 1, 15);
                /* x264 stores the coefficient COUNT in non_zero_count, not a
                 * boolean — the next block's nC depends on the exact count. */
                cache[p] = (uint8_t)nnz[b];
            }
        }
        /* reconstruction */
        int rec[16];
        for (int i = 0; i < 16; ++i) rec[i] = q[i];
        vk_ref_idct4x4dc(rec);
        vk_ref_dequant4dc(rec, deq, qp);
        for (int b = 0; b < 16; ++b) {
            int by = (b/4)*4, bx = (b%4)*4;
            uint8_t *dst = rcony + by*rdy + bx;
            if (nnz[b]) {
                int blk[16];
                for (int i = 0; i < 16; ++i) blk[i] = dct[b][i];
                vk_ref_dequant4(blk, deq, qp);
                blk[0] = rec[b];
                vk_ref_idct4(blk);
                vk_ref_add_block(dst, rdy, blk);
            } else
                vk_ref_add_block_dc(dst, rdy, rec[b]);
        }
    }
    return cbp_luma;
}

/* Compute and CAVLC-code the 2x2 chroma DC residual for an I16 MB in a
 * P-slice. Returns cbp_chroma (0/1/2) and writes reconstruction into rconu/rconv. */
static int code_i16_chroma_dc(vk_bs_t *bs, const uint8_t *cur, int w, int h,
                              int x, int y, const uint8_t *tu, const uint8_t *lu,
                              uint8_t tlu, const uint8_t *tv, const uint8_t *lv,
                              uint8_t tlv, int cmode, int qp,
                              uint8_t *rconu, uint8_t *rconv, int rdu, int rdv,
                              int write, int recon)
{
    int cmf[16], cbias[16], cdeq[16];
    vk_ref_quant_tables(qp, 2, cmf, cbias, cdeq);
    const uint8_t *cu = cur + (size_t)w*h, *cv = cu + (size_t)(w/2)*(h/2);
    int cs = w/2;
    uint8_t pu[64], pv[64];
    vk_predict_chroma8(pu, tu, lu, tlu, cmode);
    vk_predict_chroma8(pv, tv, lv, tlv, cmode);
    int cdc[2][4], cbp = 0, q[2][4];
    for (int pl = 0; pl < 2; ++pl) {
        const uint8_t *src = pl ? cv : cu, *pred = pl ? pv : pu;
        for (int b = 0; b < 4; ++b) {
            int by = (b/2)*4, bx = (b%2)*4;
            int sum = 0;
            for (int yy = 0; yy < 4; ++yy)
                for (int xx = 0; xx < 4; ++xx)
                    sum += src[(y/2+by+yy)*cs + x/2+bx+xx] - pred[(by+yy)*8+bx+xx];
            cdc[pl][b] = sum;
        }
        int a = cdc[pl][0]+cdc[pl][1], b = cdc[pl][2]+cdc[pl][3];
        int c = cdc[pl][0]-cdc[pl][1], d = cdc[pl][2]-cdc[pl][3];
        int had[4] = {a+b,a-b,c+d,c-d};
        for (int i = 0; i < 4; ++i) {
            q[pl][i] = vk_ref_quant_one(had[i], cmf[0]>>1, cbias[0]<<1);
            cbp |= q[pl][i] != 0;
        }
    }
    if (cbp && write) {
        for (int pl = 0; pl < 2; ++pl) {
            int block[16] = {0};
            /* zigzag_scan_2x2_dc: {0,2,1,3} */
            block[0]=q[pl][0]; block[1]=q[pl][2];
            block[2]=q[pl][1]; block[3]=q[pl][3];
            vk_cavlc_block_residual(bs, VK_DCT_CHROMA_DC, block, 4, 1, 4);
        }
    }
    if (cbp && recon) {
        /* DC reconstruction: idct_dequant_2x2_dconly + add to prediction */
        int dmf = cdeq[0] << (qp/6);
        for (int pl = 0; pl < 2; ++pl) {
            int a=q[pl][0]+q[pl][1], b=q[pl][2]+q[pl][3];
            int c=q[pl][0]-q[pl][1], d=q[pl][2]-q[pl][3];
            int had2[4]={a+b,a-b,c+d,c-d};
            int rr[4];
            for (int i = 0; i < 4; ++i) rr[i]=((had2[i]*dmf)>>5);
            int mby = y/16, mbx = x/16;
            uint8_t *dst = pl ? rconv : rconu;
            int stride = pl ? rdv : rdu;
            const uint8_t *p = pl ? pv : pu;
            for (int i = 0; i < 4; ++i) {
                int by=(i/2)*4, bx=(i%2)*4;
                for (int yy=0; yy<4; ++yy)
                    for (int xx=0; xx<4; ++xx) {
                        int v = p[(by+yy)*8+bx+xx] + ((rr[i]+32)>>6);
                        dst[(mby*8+by+yy)*stride+mbx*8+bx+xx] = (uint8_t)(v<0?0:v>255?255:v);
                    }
            }
        }
    }
    return cbp;
}

/* Decide whether to code a P MB as I16x16 (x264 subme=0 rule):
 * choose the I16 mode / chroma mode minimising SATD + lambda*bits, then
 * compare the total against the inter cost. */
static int intra_decide(const uint8_t *cy, const uint8_t *cu, const uint8_t *cv,
                        int w, int x, int y,
                        const uint8_t *top, const uint8_t *left, uint8_t tl,
                        const uint8_t *topu, const uint8_t *leftu, uint8_t tlu,
                        const uint8_t *topv, const uint8_t *leftv, uint8_t tlv,
                        int have_l, int have_t, int have_tl,
                        int qp, int inter_cost,
                        int *mode, int *cmode, int *cost)
{
    int lambda = vk_lambda(qp);
    uint8_t pred[256], p8[64];
    int best = 0x7fffffff, bm = 2;
    /* x264 predict_16x16_mode_available: availability-based mode list */
    static const int8_t pi16av[5][5] = {
        {6,-1,-1,-1,-1}, {4,1,-1,-1,-1}, {5,0,-1,-1,-1},
        {0,1,2,3,-1},    {0,1,2,3,-1}
    };
    int iavail = (have_l?1:0)|(have_t?2:0)|(have_tl?4:0);
    int midx = (iavail == 7) ? 4 : (iavail & 3);
    for (int k = 0; pi16av[midx][k] >= 0; ++k) {
        int m = pi16av[midx][k];
        vk_predict16(pred, top, left, tl, m);
        /* subme=0: x264 uses SAD (mbcmp = sad_aligned), not SATD. */
        const uint8_t *s = cy + y*w + x;
        int sad = 0;
        for (int yy = 0; yy < 16; ++yy)
            for (int xx = 0; xx < 16; ++xx) {
                int dv = s[yy*w+xx] - pred[yy*16+xx];
                sad += dv < 0 ? -dv : dv;
            }
        int c = sad + lambda*vk_mode_bits(m);
        if (c < best) { best = c; bm = m; }
    }
    /* x264 at subme=0 has b_chroma_me=0: chroma is not part of the
     * intra-vs-inter cost, but the chroma prediction mode is still chosen
     * by minimising SAD over both 8x8 planes + lambda*bs_size_ue(mode). */
    int cs = w/2, cbest = 0x7fffffff, cm = 0;
    int avail = (have_l ? 1 : 0) | (have_t ? 2 : 0) | (have_tl ? 4 : 0);
    static const int8_t cmavail[5][5] = {
        {6,-1,-1,-1,-1}, {4,1,-1,-1,-1}, {5,2,-1,-1,-1},
        {2,1,0,-1,-1},   {2,1,0,3,-1}
    };
    int idx = avail & 7;
    idx = (idx == 7) ? 4 : (idx & 3);
    for (int k = 0; cmavail[idx][k] >= 0; ++k) {
        int m = cmavail[idx][k];
        int fix = m <= 3 ? m : 0;
        int c = 0;
        vk_predict_chroma8(p8, topu, leftu, tlu, m);
        for (int yy = 0; yy < 8; ++yy)
            for (int xx = 0; xx < 8; ++xx) {
                int dv = cu[(y/2+yy)*cs + x/2+xx] - p8[yy*8+xx];
                c += dv < 0 ? -dv : dv;
            }
        vk_predict_chroma8(p8, topv, leftv, tlv, m);
        for (int yy = 0; yy < 8; ++yy)
            for (int xx = 0; xx < 8; ++xx) {
                int dv = cv[(y/2+yy)*cs + x/2+xx] - p8[yy*8+xx];
                c += dv < 0 ? -dv : dv;
            }
        c += lambda*vk_mode_bits(fix);
        if (c < cbest) { cbest = c; cm = m; }
    }
    *cost = best;
    *mode = bm;
    *cmode = cm;
    return *cost < inter_cost;
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
    int (*mvr)[2] = calloc((size_t)mw*mh, 2*sizeof(int));
    int *mv_ref = calloc((size_t)mw*mh, sizeof(int));
    uint8_t *is_skip = calloc((size_t)mw*mh, 1);
    uint8_t (*nnz_c)[16] = calloc((size_t)mw*mh, 16);
    uint8_t (*cnnz_c)[2][4] = calloc((size_t)mw*mh, 8);
    uint8_t *dcnz_c = calloc((size_t)mw*mh, 1);
    /* reconstructed current-frame planes (with border) for intra prediction */
    int pcrs = rs/2;
    size_t rys = (size_t)rs*(h+2*LPAD), rcs = (size_t)pcrs*(h/2+2*CPAD);
    uint8_t *ry = malloc(rys), *ru = malloc(rcs), *rv = malloc(rcs);
    memset(ry, 128, rys); memset(ru, 128, rcs); memset(rv, 128, rcs);
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

            /* reconstructed neighbours for intra prediction */
            uint8_t top[16], left[16], topu[8], leftu[8], topv[8], leftv[8];
            uint8_t tly = 128, tlu = 128, tlv = 128;
            for (int i = 0; i < 16; ++i) {
                top[i] = have_t ? ry[(my*16-1+LPAD)*rs + LPAD + mx*16+i] : 128;
                left[i] = have_l ? ry[(my*16+i+LPAD)*rs + LPAD + mx*16-1] : 128;
            }
            if (have_t && have_l) tly = ry[(my*16-1+LPAD)*rs + LPAD + mx*16-1];
            for (int i = 0; i < 8; ++i) {
                topu[i] = have_t ? ru[(my*8-1+CPAD)*crs + CPAD + mx*8+i] : 128;
                leftu[i] = have_l ? ru[(my*8+i+CPAD)*crs + CPAD + mx*8-1] : 128;
                topv[i] = have_t ? rv[(my*8-1+CPAD)*crs + CPAD + mx*8+i] : 128;
                leftv[i] = have_l ? rv[(my*8+i+CPAD)*crs + CPAD + mx*8-1] : 128;
            }
            if (have_t && have_l) {
                tlu = ru[(my*8-1+CPAD)*crs + CPAD + mx*8-1];
                tlv = rv[(my*8-1+CPAD)*crs + CPAD + mx*8-1];
            }

            int psk_x, psk_y;
            pskip_mv(lx, ly, tx, ty, cx, cy,
                     have_l ? mv_ref[idx-1] : -2,
                     have_t ? mv_ref[idx-mw] : -2,
                     have_c ? mv_ref[idx-mw+1]
                            : (have_tl ? mv_ref[idx-mw-1] : -2),
                     &psk_x, &psk_y);

            /* Fast P_SKIP is only probed when a neighbour is already P_SKIP. */
            int skip_neigh = (have_l && is_skip[idx-1])
                           || (have_t && is_skip[idx-mw])
                           || (have_tl && is_skip[idx-mw-1])
                           || (have_c && is_skip[idx-mw+1]);
            int do_skip = 0, fast_skip = 0;
            if (skip_neigh &&
                probe(cur, refp, w, h, rs, mx*16, my*16, psk_x, psk_y, qp)) {
                do_skip = 1;
                fast_skip = 1;
            }

            int mvpx = 0, mvpy = 0, mvx = 0, mvy = 0, inter_cost = 0;
            if (!do_skip) {
                mvp16_avail(lx, ly, tx, ty, cx, cy,
                            have_l ? mv_ref[idx-1] : -2,
                            have_t ? mv_ref[idx-mw] : -2,
                            have_c ? mv_ref[idx-mw+1]
                                   : (have_tl ? mv_ref[idx-mw-1] : -2),
                            &mvpx, &mvpy);
                /* x264_mb_predict_mv_ref16x16: left/top/top-left/top-right
                 * MVs, rounded to fullpel, zero and mvp excluded. */
                int cand[8], nc = 0;
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
                            cand, nc, &mvx, &mvy);
                mvr[idx][0] = mvx*4; mvr[idx][1] = mvy*4;
                /* x264 promotes P16 to P_SKIP when cbp==0 and MV==pskip_mv. */
                int qx = mvx*4, qy = mvy*4;
                if (qx == psk_x && qy == psk_y &&
                    probe(cur, refp, w, h, rs, mx*16, my*16, qx, qy, qp))
                    do_skip = 1;
            }

            /* x264 subme=0 intra-vs-inter decision (before skip override) */
            int imode = 2, icmode = 0, icost = 0;
            int use_intra = !fast_skip && inter_cost > 0 &&
                intra_decide(cur, cur + (size_t)w*h,
                             cur + (size_t)w*h + (size_t)(w/2)*(h/2),
                             w, mx*16, my*16, top, left, tly,
                             topu, leftu, tlu, topv, leftv, tlv,
                             have_l, have_t, have_tl,
                             qp, inter_cost, &imode, &icmode, &icost);
            if (use_intra) do_skip = 0;  /* intra overrides tentative P_SKIP */

            if (do_skip) {
                mv[idx][0] = psk_x; mv[idx][1] = psk_y;
                /* x264 retains the ME MV in mvr[0][0] for P_SKIP found via
                 * mb_analyse_inter_p16x16 early termination (analyse.c:1291,
                 * 3040-3045). It is a motion-prediction candidate for later
                 * MBs. Do not zero it here. */
                is_skip[idx] = 1;
                ++skip_run;
                /* reconstruct skip MB from reference */
                int sx = mv[idx][0]/4, sy = mv[idx][1]/4;
                for (int yy = 0; yy < 16; ++yy)
                    memcpy(ry + (my*16+yy+LPAD)*rs + LPAD + mx*16,
                           refp + (my*16+yy+sy+LPAD)*rs + LPAD + mx*16+sx, 16);
                for (int yy = 0; yy < 8; ++yy) {
                    int cx = mv[idx][0]/8, cyp = mv[idx][1]/8;
                    memcpy(ru + (my*8+yy+CPAD)*crs + CPAD + mx*8,
                           refp + rys + (my*8+yy+cyp+CPAD)*crs + CPAD + mx*8+cx, 8);
                    memcpy(rv + (my*8+yy+CPAD)*crs + CPAD + mx*8,
                           refp + rys + rcs + (my*8+yy+cyp+CPAD)*crs + CPAD + mx*8+cx, 8);
                }
                continue;
            }

            /* CAVLC requires mb_skip_run before every coded MB, including 0. */
            vk_bs_write_ue(bs, (uint32_t)skip_run);
            skip_run = 0;
            mv[idx][0] = mvx*4; mv[idx][1] = mvy*4;

            if (use_intra) {
                /* x264 zeroes the decoded-MV cache for intra MBs (ref=-1),
                 * while mvr retains the inter MV for ME candidates. */
                mv[idx][0] = mv[idx][1] = 0;
                mv_ref[idx] = -1;  /* intra: ref=-1 for neighbour predictors */
                /* chroma DC residual (must be computed before mb_type since
                 * cbp_chroma is encoded in the I16 mb_type value) */
                uint8_t *rconu_i = ru + (my*8+CPAD)*crs + CPAD + mx*8;
                uint8_t *rconv_i = rv + (my*8+CPAD)*crs + CPAD + mx*8;
                int cbp_chroma_i = code_i16_chroma_dc(bs, cur, w, h, mx*16, my*16,
                        topu, leftu, tlu, topv, leftv, tlv, icmode, qp,
                        rconu_i, rconv_i, crs, crs, 0, 0);
                uint8_t pred16[256], pred8[64];
                vk_predict16(pred16, top, left, tly, imode);
                uint8_t il[4], it[4];
                for (int i = 0; i < 4; ++i) {
                    il[i] = have_l ? nnz_c[idx-1][i*4+3] : 0x80;
                    it[i] = have_t ? nnz_c[idx-mw][12+i] : 0x80;
                }
                /* compute cbp_luma first (encoded in mb_type) */
                int cbp_luma_i = code_i16_dc(bs, cur, w, mx*16, my*16, pred16, qp,
                        il, it, ry + (my*16+LPAD)*rs + LPAD + mx*16, rs, 0);
                /* x264: prediction written to fdec FIRST, then DC residual added */
                for (int yy = 0; yy < 16; ++yy)
                    memcpy(ry + (my*16+yy+LPAD)*rs + LPAD + mx*16, pred16 + yy*16, 16);
                vk_predict_chroma8(pred8, topu, leftu, tlu, icmode);
                for (int yy = 0; yy < 8; ++yy)
                    memcpy(ru + (my*8+yy+CPAD)*crs + CPAD + mx*8, pred8 + yy*8, 8);
                vk_predict_chroma8(pred8, topv, leftv, tlv, icmode);
                for (int yy = 0; yy < 8; ++yy)
                    memcpy(rv + (my*8+yy+CPAD)*crs + CPAD + mx*8, pred8 + yy*8, 8);
                /* x264: mb_type = 6 + fix + 4*cbp_chroma + 12*(cbp_luma!=0) */
                int imode_fix = imode <= 3 ? imode : 2;
                vk_bs_write_ue(bs, (uint32_t)(6 + imode_fix + 4*cbp_chroma_i
                                              + (cbp_luma_i ? 12 : 0)));  /* I16 in P */
                vk_bs_write_ue(bs, (uint32_t)(icmode <= 3 ? icmode : 0));  /* chroma mode fix */
                vk_bs_write_se(bs, 0);                           /* mb_qp_delta */
                code_i16_dc(bs, cur, w, mx*16, my*16, pred16, qp, il, it,
                            ry + (my*16+LPAD)*rs + LPAD + mx*16, rs, 1);
                /* chroma DC residual written after luma DC */
                if (cbp_chroma_i)
                    code_i16_chroma_dc(bs, cur, w, h, mx*16, my*16,
                        topu, leftu, tlu, topv, leftv, tlv, icmode, qp,
                        rconu_i, rconv_i, crs, crs, 1, 1);
                continue;
            }
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
            /* MC prediction first, then code_residual adds the residual */
            int sx = mvx, sy = mvy;
            for (int yy = 0; yy < 16; ++yy)
                memcpy(ry + (my*16+yy+LPAD)*rs + LPAD + mx*16,
                       refp + (my*16+yy+sy+LPAD)*rs + LPAD + mx*16+sx, 16);
            for (int yy = 0; yy < 8; ++yy) {
                int cx = mvx/2, cyp = mvy/2;
                memcpy(ru + (my*8+yy+CPAD)*crs + CPAD + mx*8,
                       refp + rys + (my*8+yy+cyp+CPAD)*crs + CPAD + mx*8+cx, 8);
                memcpy(rv + (my*8+yy+CPAD)*crs + CPAD + mx*8,
                       refp + rys + rcs + (my*8+yy+cyp+CPAD)*crs + CPAD + mx*8+cx, 8);
            }
            uint8_t *rcony = ry + (my*16+LPAD)*rs + LPAD + mx*16;
            uint8_t *rconu = ru + (my*8+CPAD)*crs + CPAD + mx*8;
            uint8_t *rconv = rv + (my*8+CPAD)*crs + CPAD + mx*8;
            uint8_t on[16], oc[2][4], od;
            code_residual(bs, cur, refp, w, h, rs, mx*16, my*16,
                          mvx*4, mvy*4, qp, on, oc, &od, ll, tl, lc, tc,
                          rcony, rconu, rconv, rs, crs, crs);
            memcpy(nnz_c[idx], on, 16);
            memcpy(cnnz_c[idx], oc, 8);
            dcnz_c[idx] = od;
        }
    if (skip_run) vk_bs_write_ue(bs, (uint32_t)skip_run);
    vk_bs_rbsp_trailing(bs);
    free(dcnz_c); free(cnnz_c); free(nnz_c);
    free(ry); free(ru); free(rv);
    free(is_skip); free(mv); free(mvr); free(mv_ref); free(refp);
}
