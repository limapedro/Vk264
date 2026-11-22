/* SPDX-License-Identifier: GPL-2.0-or-later
 * CAVLC I8x8 macroblock encoder. Mirrors x264 x264_macroblock_write_cavlc
 * for I_8x8 MBs with 8x8 transform (b_transform_8x8 = 1). */
#include "i8x8enc.h"
#include "i8x8.h"
#include "cavlc.h"
#include "intra16.h"
#include "slice.h"
#include <string.h>

/* x264 cbp_to_golomb[1][1] (4:2:0, intra). */
static const uint8_t CBP2GOL[16] = {
    3,29,30,17, 31,18,37,8, 32,38,19,9, 20,10,11,2
};
/* x264_scan8[0..15] for the 4x4 blocks of an MB. */
static const uint8_t SCAN8[16] = {
    12,13,20,21, 14,15,22,23, 28,29,36,37, 30,31,38,39
};
static const uint8_t CT_INDEX[17] = {0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3};
/* x264 block_idx_xy_1d: x264 internal 4x4 index -> raster index */
static const uint8_t IDX2XY[16] = {
    0,1,4,5, 2,3,6,7, 8,9,12,13, 10,11,14,15
};

static int clip(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* 8x8 DC prediction for one 8x8 block (mode 2). */
static void pred8_dc(uint8_t out[64], const uint8_t left[8], const uint8_t top[8])
{
    int sum = 0;
    for (int i = 0; i < 8; ++i) sum += left[i] + top[i];
    int dc = (sum + 8) >> 4;
    for (int i = 0; i < 64; ++i) out[i] = (uint8_t)dc;
}

/* x264 internal pred modes: 9=DC_LEFT, 10=DC_TOP, 11=DC_128.
 * These map to DC(2) in the bitstream via x264_mb_pred_mode4x4_fix. */
static int fix_mode(int m) { return (m >= 0 && m <= 8) ? m : 2; }

static void pred8(uint8_t out[64], int mode,
                  const uint8_t left[8], const uint8_t top[8], uint8_t tl)
{
    (void)tl;
    if (mode == 11) { for (int i=0;i<64;++i) out[i]=128; return; }
    if (mode == 9) { /* DC_LEFT: average left only */
        int s = 0; for (int i=0;i<8;++i) s += left[i];
        int dc = (s + 4) >> 3;
        for (int i=0;i<64;++i) out[i]=(uint8_t)dc; return;
    }
    if (mode == 10) { /* DC_TOP: average top only */
        int s = 0; for (int i=0;i<8;++i) s += top[i];
        int dc = (s + 4) >> 3;
        for (int i=0;i<64;++i) out[i]=(uint8_t)dc; return;
    }
    if (mode == 2) { pred8_dc(out, left, top); return; }
    if (mode == 1) { for (int y=0;y<8;++y) for(int x=0;x<8;++x) out[y*8+x]=left[y]; return; }
    if (mode == 0) { for (int y=0;y<8;++y) for(int x=0;x<8;++x) out[y*8+x]=top[x]; return; }
    pred8_dc(out, left, top);
}

void vk_encode_i8x8_mb(vk_bs_t *bs, const uint8_t *src_y,
                       const uint8_t *src_u, const uint8_t *src_v,
                       uint8_t *recon_y, uint8_t *recon_u, uint8_t *recon_v,
                       int qp, const vk_neighbours_t *nb,
                       int stride_y, int stride_c,
                       const uint8_t modes[4], int chroma_mode,
                       uint8_t out_nnz[16], uint8_t out_cnnz[2][4],
                       uint8_t *out_dcnz)
{
    (void)stride_y; (void)stride_c; (void)src_u; (void)src_v;
    (void)recon_u; (void)recon_v; (void)out_cnnz; (void)out_dcnz;

    int mf[64], bias[64], deq[64];
    vk_ref_quant_tables8(qp, mf, bias, deq);

    /* ---- 4 8x8 luma blocks: predict, DCT, quant, reconstruct INLINE ----
     * x264 x264_mb_encode_i8x8 writes the prediction to fdec, then adds the
     * dequantised residual, so later 8x8 blocks predict from already-
     * reconstructed neighbours. */
    int dct8[4][64], cbp_luma = 0, nz8[4] = {0};
    uint8_t pred8b[4][64], sb8[4][64];
    int zz8[4][64];
    for (int g = 0; g < 4; ++g) {
        int gy = (g >> 1) * 8, gx = (g & 1) * 8;
        uint8_t left[8], top[8], tl = 128;
        for (int i = 0; i < 8; ++i) {
            int ly = gy + i;
            left[i] = (gx > 0) ? recon_y[ly*16 + gx-1] :
                      (nb && nb->left_y ? nb->left_y[ly] : 128);
            top[i]  = (gy > 0) ? recon_y[(gy-1)*16 + gx + i] :
                      (nb && nb->top_y ? nb->top_y[gx+i] : 128);
        }
        tl = (gx > 0 && gy > 0) ? recon_y[(gy-1)*16 + gx-1] :
             (nb && nb->tl_y ? nb->tl_y : 128);
        pred8(pred8b[g], modes[g], left, top, tl);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                sb8[g][y*8+x] = src_y[(gy+y)*16 + gx+x]; /* tight source */
        vk_ref_dct8x8(dct8[g], sb8[g], pred8b[g]);
        nz8[g] = vk_ref_quant8(dct8[g], mf, bias);
        vk_ref_zigzag8(zz8[g], dct8[g]); /* CAVLC copy (natural dct8 kept for recon) */
        if (nz8[g]) cbp_luma |= 1 << g;
        /* inline reconstruction */
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                recon_y[(gy+y)*16 + gx+x] = pred8b[g][y*8+x];
        if (nz8[g]) {
            int d[64]; memcpy(d, dct8[g], sizeof(d));
            /* helper works on a tight 8x8 (stride 8); copy pred in/out. */
            uint8_t tmp8[64];
            for (int y = 0; y < 8; ++y)
                memcpy(tmp8 + y*8, recon_y + (gy+y)*16 + gx, 8);
            vk_ref_dequant8_idct_add(tmp8, d, deq, qp);
            for (int y = 0; y < 8; ++y)
                memcpy(recon_y + (gy+y)*16 + gx, tmp8 + y*8, 8);
        }
    }

    /* ---- chroma: 4x4 DCT + Hadamard DC (same as I16) ---- */
    uint8_t pred_u[64], pred_v[64];
    {
        uint8_t t8[8], l8[8], t8v[8], l8v[8];
        for (int i = 0; i < 8; ++i) {
            t8[i]  = nb && nb->top_u  ? nb->top_u[i]  : 128;
            l8[i]  = nb && nb->left_u ? nb->left_u[i] : 128;
            t8v[i] = nb && nb->top_v  ? nb->top_v[i]  : 128;
            l8v[i] = nb && nb->left_v ? nb->left_v[i] : 128;
        }
        vk_predict_chroma8(pred_u, t8, l8, nb ? nb->tl_u : 128, chroma_mode);
        vk_predict_chroma8(pred_v, t8v, l8v, nb ? nb->tl_v : 128, chroma_mode);
    }
    /* Chroma prediction is written to fdec even with cbp_chroma=0. */
    if (recon_u) memcpy(recon_u, pred_u, 64);
    if (recon_v) memcpy(recon_v, pred_v, 64);

    /* ---- entropy: mb_type, transform8 flag, 4 pred modes, chroma, cbp ---- */
    vk_bs_write_ue(bs, 0);   /* mb_type: I4x4/I8x8 */
    vk_bs_write1(bs, 1);     /* b_transform_8x8 = 1 => I8x8 */
    /* pred modes: x264 writes the fixed mode; pred=min(left,top), default 2 */
    for (int g = 0; g < 4; ++g) {
        int left = (g & 1) ? modes[g-1] : 2;
        int top  = (g >= 2) ? modes[g-2] : 2;
        int pred = left < top ? left : top;
        int m = fix_mode(modes[g]);
        if (m == pred) vk_bs_write1(bs, 1);
        else vk_bs_write(bs, 4, (unsigned)(m - (m > pred)));
    }
    vk_bs_write_ue(bs, (uint32_t)(chroma_mode <= 3 ? chroma_mode : 0));
    vk_bs_write_ue(bs, (unsigned)CBP2GOL[cbp_luma]);
    vk_bs_write_se(bs, 0);  /* mb_qp_delta */

    /* ---- luma residual: for each coded 8x8 group, interleave + CAVLC ---- */
    uint8_t cache[64] = {0};
    static const int lpos[4] = {11,19,27,35}, tpos[4] = {4,5,6,7};
    for (int i = 0; i < 4; ++i) {
        cache[tpos[i]] = nb && nb->top_luma  ? nb->top_luma[i]  : 0x80;
        cache[lpos[i]] = nb && nb->left_luma ? nb->left_luma[i] : 0x80;
    }
    for (int g = 0; g < 4; ++g) {
        if (!(cbp_luma & (1 << g))) continue;
        int dst4[4][16];
        uint8_t nnz4[4];
        vk_ref_interleave8x8_cavlc(dst4, zz8[g], nnz4);
        int base = SCAN8[g*4];
        for (int i4 = 0; i4 < 4; ++i4) {
            int p = SCAN8[g*4 + i4];
            int sum = cache[p-1] + cache[p-8];
            int pr = sum < 0x80 ? (sum+1)>>1 : (sum & 0x7f);
            int nC = CT_INDEX[pr & 0x7f];
            vk_cavlc_block_residual(bs, VK_DCT_LUMA_4x4, dst4[i4], nC, 1, 16);
            cache[p] = nnz4[i4];
        }
        (void)base;
    }

    /* nnz out (x264 stores coefficient counts in scan order) */
    if (out_nnz) {
        for (int i = 0; i < 16; ++i) out_nnz[i] = 0;
        for (int g = 0; g < 4; ++g) {
            int dst4[4][16]; uint8_t nnz4[4];
            vk_ref_interleave8x8_cavlc(dst4, zz8[g], nnz4);
            for (int i4 = 0; i4 < 4; ++i4) {
                int n = 0;
                for (int j = 0; j < 16; ++j) n += dst4[i4][j] != 0;
                out_nnz[IDX2XY[g*4+i4]] = (uint8_t)n;
            }
        }
    }
}
