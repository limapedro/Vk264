/* SPDX-License-Identifier: GPL-2.0-or-later
 * Slice header + I16x16 macroblock encoder. Transcribed from x264:
 *   encoder/encoder.c:213   slice_header_write
 *   encoder/macroblock.c:126 mb_encode_i16x16
 *   common/dct.c:157        sub4x4_dct / :47 dct4x4dc
 *   common/quant.c:67       quant_4x4 / :88 quant_4x4_dc / :96 quant_2x2_dc
 */
#include "slice.h"
#include "cavlc.h"
#include "intra16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const uint16_t quant4_scale[6][3] = {
    {13107,8066,5243},{11916,7490,4660},{10082,6554,4194},
    {9362,5825,3647},{8192,5243,3355},{7282,4559,2893}
};
static const uint8_t dequant4_scale[6][3] = {
    {10,13,16},{11,14,18},{13,16,20},{14,18,23},{16,20,25},{18,23,29}
};
/* x264 common/dct.c:817 ZIGZAG4_FRAME with ZIG(i,y,x)=dct[x*4+y].
 * This transposed order differs from the "standard" 0,1,4,8,... scan;
 * flat frames hide the difference because only DC is nonzero. */
static const uint8_t zigzag4[16] = {
    0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15
};

static int shift_r(int x, int s)
{
    return s <= 0 ? x << -s : (x + (1 << (s-1))) >> s;
}
static int div_r(int n, int d) { return (n + (d >> 1)) / d; }

/* x264 common/set.c::x264_cqm_init, flat CQM, lists 0=IY 2=IC. */
static void quant_tables(int qp, int list, int mf[16], int bias[16], int dequant[16])
{
    /* x264 base.c:456: i_luma_deadzone[0]=21,[1]=11; set.c:81 deadzone =
     * {32-[1], 32-[0], 32-11, 32-21} = {21, 11, 21, 11}. */
    const int deadzone = (list == 0 || list == 2) ? 21 : 11;
    int q = qp % 6, s = qp / 6;
    for (int i = 0; i < 16; ++i) {
        int par = (i & 1) + ((i >> 2) & 1);
        int base = quant4_scale[q][par];
        int j = shift_r(base, s - 1);
        mf[i] = shift_r(base, s - 1);
        bias[i] = j ? div_r(deadzone << 10, j) : 0;
        if (bias[i] > (1 << 15) / j) bias[i] = (1 << 15) / j;
        dequant[i] = dequant4_scale[q][par] * 16;
    }
}

static int quant_one(int coef, int mf, int bias);

int vk_ref_quant4x4(int dct[16], const int mf[16], const int bias[16])
{
    int nz = 0;
    for (int i = 0; i < 16; ++i) {
        dct[i] = quant_one(dct[i], mf[i], bias[i]);
        nz |= dct[i] != 0;
    }
    return nz;
}

static int quant_one(int coef, int mf, int bias)
{
    if (coef > 0) return (int)(((uint32_t)bias + (uint32_t)coef) * (uint32_t)mf >> 16);
    return -(int)(((uint32_t)bias + (uint32_t)(-coef)) * (uint32_t)mf >> 16);
}

static void sub4x4_dct(int dct[16], const uint8_t *src, const uint8_t *pred);

void vk_ref_dct4x4(int dct[16], const uint8_t src[16], const uint8_t pred[16])
{
    sub4x4_dct(dct, src, pred);
}

static void sub4x4_dct(int dct[16], const uint8_t *src, const uint8_t *pred)
{
    int d[16], tmp[16];
    /* Tight 4x4 input blocks (stride 4), unlike x264's in-frame FENC_STRIDE. */
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            d[x + 4*y] = src[y*4+x] - pred[y*4+x];
    for (int i = 0; i < 4; ++i) {
        int s03 = d[i*4+0]+d[i*4+3], s12 = d[i*4+1]+d[i*4+2];
        int d03 = d[i*4+0]-d[i*4+3], d12 = d[i*4+1]-d[i*4+2];
        tmp[0*4+i] = s03+s12; tmp[1*4+i] = 2*d03+d12;
        tmp[2*4+i] = s03-s12; tmp[3*4+i] = d03-2*d12;
    }
    for (int i = 0; i < 4; ++i) {
        int s03 = tmp[i*4+0]+tmp[i*4+3], s12 = tmp[i*4+1]+tmp[i*4+2];
        int d03 = tmp[i*4+0]-tmp[i*4+3], d12 = tmp[i*4+1]-tmp[i*4+2];
        dct[i*4+0] = s03+s12; dct[i*4+1] = 2*d03+d12;
        dct[i*4+2] = s03-s12; dct[i*4+3] = d03-2*d12;
    }
}

static void dct4x4dc(int d[16]);

void vk_ref_dct4x4dc(int d[16])
{
    dct4x4dc(d);
}

static void dct4x4dc(int d[16])
{
    int tmp[16];
    for (int i = 0; i < 4; ++i) {
        int s01=d[i*4+0]+d[i*4+1], dd01=d[i*4+0]-d[i*4+1];
        int s23=d[i*4+2]+d[i*4+3], dd23=d[i*4+2]-d[i*4+3];
        tmp[0*4+i]=s01+s23; tmp[1*4+i]=s01-s23;
        tmp[2*4+i]=dd01-dd23; tmp[3*4+i]=dd01+dd23;
    }
    for (int i = 0; i < 4; ++i) {
        int s01=tmp[i*4+0]+tmp[i*4+1], dd01=tmp[i*4+0]-tmp[i*4+1];
        int s23=tmp[i*4+2]+tmp[i*4+3], dd23=tmp[i*4+2]-tmp[i*4+3];
        d[i*4+0]=(s01+s23+1)>>1; d[i*4+1]=(s01-s23+1)>>1;
        d[i*4+2]=(dd01-dd23+1)>>1; d[i*4+3]=(dd01+dd23+1)>>1;
    }
}

static void sub8x8_dct_dc(int dct[4], const uint8_t *src, const uint8_t *pred)
{
    /* 8x8 chroma planes have stride 8, not the luma stride 16. */
    for (int b = 0; b < 4; ++b) {
        int sx = (b & 1) * 4, sy = (b >> 1) * 4;
        int sum = 0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                sum += src[(sy+y)*8+sx+x] - pred[(sy+y)*8+sx+x];
        dct[b] = sum;
    }
}

static void idct4x4dc(int d[16])
{
    int tmp[16];
    for (int i = 0; i < 4; ++i) {
        int s01=d[i*4+0]+d[i*4+1], dd01=d[i*4+0]-d[i*4+1];
        int s23=d[i*4+2]+d[i*4+3], dd23=d[i*4+2]-d[i*4+3];
        tmp[0*4+i]=s01+s23; tmp[1*4+i]=s01-s23;
        tmp[2*4+i]=dd01-dd23; tmp[3*4+i]=dd01+dd23;
    }
    for (int i = 0; i < 4; ++i) {
        int s01=tmp[i*4+0]+tmp[i*4+1], dd01=tmp[i*4+0]-tmp[i*4+1];
        int s23=tmp[i*4+2]+tmp[i*4+3], dd23=tmp[i*4+2]-tmp[i*4+3];
        d[i*4+0]=s01+s23; d[i*4+1]=s01-s23;
        d[i*4+2]=dd01-dd23; d[i*4+3]=dd01+dd23;
    }
}

static void dequant4(int dct[16], const int dm[16], int qp)
{
    int mf = qp % 6, qb = qp/6 - 4;
    if (qb >= 0) for (int i = 0; i < 16; ++i) dct[i] = (dct[i]*dm[i]) << qb;
    else {
        int f = 1 << (-qb-1);
        for (int i = 0; i < 16; ++i) dct[i] = (dct[i]*dm[i]+f) >> (-qb);
    }
}

static void dequant4dc(int dct[16], const int dm[16], int qp)
{
    int qb = qp/6 - 6;
    if (qb >= 0) {
        int d = dm[0] << qb;
        for (int i = 0; i < 16; ++i) dct[i] *= d;
    } else {
        int f = 1 << (-qb-1);
        for (int i = 0; i < 16; ++i) dct[i] = (dct[i]*dm[0]+f) >> (-qb);
    }
}

static void idct4(int d[16])
{
    int tmp[16];
    for (int i = 0; i < 4; ++i) {
        int s02=d[0*4+i]+d[2*4+i], dd02=d[0*4+i]-d[2*4+i];
        int s13=d[1*4+i]+(d[3*4+i]>>1), dd13=(d[1*4+i]>>1)-d[3*4+i];
        tmp[i*4+0]=s02+s13; tmp[i*4+1]=dd02+dd13;
        tmp[i*4+2]=dd02-dd13; tmp[i*4+3]=s02-s13;
    }
    for (int i = 0; i < 4; ++i) {
        int s02=tmp[0*4+i]+tmp[2*4+i], dd02=tmp[0*4+i]-tmp[2*4+i];
        int s13=tmp[1*4+i]+(tmp[3*4+i]>>1), dd13=(tmp[1*4+i]>>1)-tmp[3*4+i];
        d[0*4+i]=(s02+s13+32)>>6; d[1*4+i]=(dd02+dd13+32)>>6;
        d[2*4+i]=(dd02-dd13+32)>>6; d[3*4+i]=(s02-s13+32)>>6;
    }
}

static void add_block(uint8_t *dst, int stride, const int d[16])
{
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            int v = dst[y*stride+x] + d[y*4+x];
            dst[y*stride+x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
}

static void add_block_dc(uint8_t *dst, int stride, int dc)
{
    dc = (dc+32)>>6;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            int v = dst[y*stride+x] + dc;
            dst[y*stride+x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
}

void vk_ref_predict16x16(uint8_t out[256], const uint8_t top[16],
                         const uint8_t left[16], int mode)
{
    if (mode == 0) { /* V */
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) out[y*16+x] = top[x];
    } else if (mode == 1) { /* H */
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) out[y*16+x] = left[y];
    } else if (mode == 2) { /* DC */
        int s = 16;
        for (int i = 0; i < 16; ++i) s += top[i] + left[i];
        memset(out, s >> 5, 256);
    } else { /* DC_128 and the DC_LEFT/TOP fixups */
        memset(out, 128, 256);
    }
}

static void predict_dc(uint8_t *out, int size, int value)
{
    /* x264 replicates luma edge pixels into the fdec border (frame.c:535-553),
     * but chroma DC at the top-left corner falls back to 128. */
    memset(out, value, (size_t)size*size);
}

void vk_slice_header_init(vk_slice_header_t *sh, const vk_sps_t *sps,
                          const vk_pps_t *pps, int slice_qp)
{
    (void)sps;
    memset(sh, 0, sizeof(*sh));
    sh->i_type = 2;          /* SLICE_TYPE_I */
    sh->i_pps_id = pps->i_id;
    sh->i_idr_pic_id = 0;
    sh->i_qp_delta = slice_qp - pps->i_pic_init_qp;
    /* x264 ultrafast disables the deblocking filter (b_deblocking_filter=0). */
    sh->i_disable_deblocking_filter_idc = 1;
}

void vk_slice_header_write(vk_bs_t *s, const vk_slice_header_t *sh,
                           const vk_sps_t *sps, const vk_pps_t *pps, int nal_ref_idc)
{
    vk_bs_write_ue(s, (uint32_t)sh->i_first_mb);
    vk_bs_write_ue(s, (uint32_t)(sh->i_type + 5));
    vk_bs_write_ue(s, (uint32_t)sh->i_pps_id);
    vk_bs_write(s, (unsigned)sps->i_log2_max_frame_num,
                (uint32_t)sh->i_frame_num);
    if (sh->i_idr_pic_id >= 0)
        vk_bs_write_ue(s, (uint32_t)sh->i_idr_pic_id);
    if (sps->i_poc_type == 0)
        vk_bs_write(s, (unsigned)sps->i_log2_max_poc_lsb, 0);
    if (sh->i_type != 2 && sh->i_type != 7) { /* not I */
        vk_bs_write1(s, 0); /* num_ref_idx_active_override */
        vk_bs_write1(s, 0); /* ref_pic_list_reordering */
    }
    if (nal_ref_idc != 0) {
        if (sh->i_idr_pic_id >= 0) {
            vk_bs_write1(s, 0); vk_bs_write1(s, 0);
        } else {
            vk_bs_write1(s, 0);
        }
    }
    if (pps->b_cabac && sh->i_type != 2 && sh->i_type != 7)
        vk_bs_write_ue(s, 0);
    vk_bs_write_se(s, sh->i_qp_delta);
    if (pps->b_deblocking_filter_control) {
        vk_bs_write_ue(s, (uint32_t)sh->i_disable_deblocking_filter_idc);
        if (sh->i_disable_deblocking_filter_idc != 1) {
            vk_bs_write_se(s, sh->i_alpha_c0_offset >> 1);
            vk_bs_write_se(s, sh->i_beta_offset >> 1);
        }
    }
}

void vk_encode_i16_mb(vk_bs_t *s, const uint8_t *src_y, const uint8_t *src_u,
                      const uint8_t *src_v, uint8_t *recon_y, uint8_t *recon_u,
                      uint8_t *recon_v, int qp, int mb_x, int mb_y,
                      const vk_neighbours_t *nb, int stride_y, int stride_c,
                      uint8_t out_nnz[16], uint8_t out_cnnz[2][4], uint8_t *out_dcnz)
{
    (void)mb_x; (void)mb_y;
    int mf[16], bias[16], dequant[16], cmf[16], cbias[16], cdeq[16];
    uint8_t pred_y[256], pred_u[64], pred_v[64];
    int mode = 2; /* I16 DC */
    int chroma_pred_mode = 0;
    if (nb) {
        uint8_t top[16], left[16], tight[256];
        for (int i = 0; i < 16; ++i) { top[i] = nb->top_y[i]; left[i] = nb->left_y[i]; }
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) tight[y*16+x] = src_y[y*stride_y+x];
        mode = vk_select16(tight, top, left, nb->tl_y, nb->avail, qp);
        vk_predict16(pred_y, top, left, nb->tl_y, mode);
        /* chroma mode decision + prediction from neighbours */
        uint8_t t8[8], l8[8], su64[64], sv64[64];
        for (int i = 0; i < 8; ++i) {
            t8[i] = nb->top_u[i]; l8[i] = nb->left_u[i];
        }
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                su64[y*8+x] = src_u[y*stride_c+x];
                sv64[y*8+x] = src_v[y*stride_c+x];
            }
        uint8_t t8v[8], l8v[8];
        for (int i = 0; i < 8; ++i) { t8v[i] = nb->top_v[i]; l8v[i] = nb->left_v[i]; }
        chroma_pred_mode = vk_select_chroma(su64, sv64, t8, l8, t8v, l8v,
                                            nb->avail, qp);
        vk_predict_chroma8(pred_u, t8, l8, chroma_pred_mode);
        vk_predict_chroma8(pred_v, t8v, l8v, chroma_pred_mode);
    } else {
        predict_dc(pred_y, 16, 128);
        predict_dc(pred_u, 8, 128);
        predict_dc(pred_v, 8, 128);
    }
    quant_tables(qp, 0, mf, bias, dequant);
    quant_tables(qp, 2, cmf, cbias, cdeq);

    /* ---- luma: 16 4x4 DCTs, split DC, quantise AC ---- */
    int dct4[16][16], dc4[16], nnz[16] = {0}, acq[16][16];
    for (int b = 0; b < 16; ++b) {
        int by = (b/4)*4, bx = (b%4)*4;
        uint8_t sb[16], pb[16];
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                sb[y*4+x] = src_y[(by+y)*stride_y+bx+x];
                pb[y*4+x] = pred_y[(by+y)*16+bx+x];
            }
        sub4x4_dct(dct4[b], sb, pb);
        dc4[b] = dct4[b][0];
        dct4[b][0] = 0;                 /* DC goes through the Hadamard path */
        for (int i = 0; i < 16; ++i) {
            dct4[b][i] = quant_one(dct4[b][i], mf[i], bias[i]);
            nnz[b] += dct4[b][i] != 0;  /* x264 stores the coefficient count */
        }
        for (int i = 0; i < 16; ++i) acq[b][i] = dct4[b][zigzag4[i]];
    }
    int cbp_luma = 0;
    for (int b = 0; b < 16; ++b)
        if (nnz[b]) cbp_luma |= 1 << (b/4);
    /* x264 ultrafast: b_dct_decimate = 0, so no coefficient decimation. */

    /* ---- luma 16x16 DC Hadamard + quantisation ---- */
    int dcdc[16];
    memcpy(dcdc, dc4, sizeof(dcdc));
    dct4x4dc(dcdc);
    int dc_mf = mf[0] >> 1, dc_bias = bias[0] << 1;
    int dcnz = 0;
    for (int i = 0; i < 16; ++i) {
        dcdc[i] = quant_one(dcdc[i], dc_mf, dc_bias);
        dcnz |= dcdc[i] != 0;
    }
    int dc_q[16];
    for (int i = 0; i < 16; ++i) dc_q[i] = dcdc[zigzag4[i]];

    /* ---- chroma: 2 planes x 4 4x4 DCTs ---- */
    int cdct[2][4][16], cdc[2][4], cnnz[2][4] = {{0}}, cacq[2][4][16];
    for (int p = 0; p < 2; ++p) {
        const uint8_t *src = p ? src_v : src_u;
        const uint8_t *prd = p ? pred_v : pred_u;
        for (int b = 0; b < 4; ++b) {
            int by = (b/2)*4, bx = (b%2)*4;
            uint8_t sb[16], pb[16];
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    sb[y*4+x] = src[(by+y)*stride_c+bx+x];
                    pb[y*4+x] = prd[(by+y)*8+bx+x];
                }
            sub4x4_dct(cdct[p][b], sb, pb);
            cdc[p][b] = cdct[p][b][0];
            cdct[p][b][0] = 0;
            for (int i = 0; i < 16; ++i) {
                cdct[p][b][i] = quant_one(cdct[p][b][i], cmf[i], cbias[i]);
                cnnz[p][b] += cdct[p][b][i] != 0;  /* coefficient count, not bool */
            }
            for (int i = 0; i < 16; ++i) cacq[p][b][i] = cdct[p][b][zigzag4[i]];
        }
    }
    int chroma_dc[2][4], cbp_chroma = 0, cacbp = 0, cdnz[2] = {0}, chad[2][4];
    for (int p = 0; p < 2; ++p) {
        int a = cdc[p][0] + cdc[p][1], b = cdc[p][2] + cdc[p][3];
        int c = cdc[p][0] - cdc[p][1], d = cdc[p][2] - cdc[p][3];
        int had[4] = { a+b, a-b, c+d, c-d }, q[4];
        for (int i = 0; i < 4; ++i) {
            q[i] = quant_one(had[i], cmf[0] >> 1, cbias[0] << 1);
            chad[p][i] = q[i];
        }
        chroma_dc[p][0] = q[0]; chroma_dc[p][1] = q[2];
        chroma_dc[p][2] = q[1]; chroma_dc[p][3] = q[3];
        for (int i = 0; i < 4; ++i) {
            cbp_chroma |= chroma_dc[p][i] != 0;
            cdnz[p] += chroma_dc[p][i] != 0;
        }
        for (int i = 0; i < 4; ++i) cacbp |= cnnz[p][i];
    }
    if (cacbp) cbp_chroma = 2;

    /* ---- entropy ---- */
    int fix = mode <= 3 ? mode : 2;
    uint32_t mb_type = 1u + (uint32_t)fix + 4u*(uint32_t)cbp_chroma
                     + (cbp_luma ? 12u : 0u);
    vk_bs_write_ue(s, mb_type);
    vk_bs_write_ue(s, (uint32_t)(chroma_pred_mode <= 3 ? chroma_pred_mode : 0));
    vk_bs_write_se(s, 0);  /* mb_qp_delta */

    /* x264 predicts luma DC nz from neighbours of luma block 0 */
    {
        static const uint8_t ctt[17] = {0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3};
        int za = (nb && nb->left_luma) ? nb->left_luma[0] : 0x80;
        int zb = (nb && nb->top_luma) ? nb->top_luma[0] : 0x80;
        int sum = za+zb, pred = sum < 0x80 ? (sum+1)>>1 : sum & 0x7f;
        vk_cavlc_block_residual(s, VK_DCT_LUMA_DC, dc_q, ctt[pred & 0x7f], 1, 16);
    }
    /* x264's dct4x4[idx] enumeration maps to raster through block_idx_xy_1d;
     * entropy coding walks idx order, not raster order. */
    static const uint8_t block_idx_xy_1d[16] = {
        0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15
    };
    static const uint8_t ct_index[17] = {0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3};
    /* x264_scan8[0..15]: luma 4x4 block positions on an 8-wide cache grid. */
    static const uint8_t scan8[16] = {
        12,13,20,21, 14,15,22,23, 28,29,36,37, 30,31,38,39
    };
    uint8_t cache[64] = {0};
    static const int lpos[4] = {11,19,27,35};
    static const int tpos[4] = {4,5,6,7};
    for (int i = 0; i < 4; ++i) {
        cache[tpos[i]] = (nb && nb->top_luma) ? nb->top_luma[i] : 0x80;
        cache[lpos[i]] = (nb && nb->left_luma) ? nb->left_luma[i] : 0x80;
    }
    if (cbp_luma) for (int idx = 0; idx < 16; ++idx) {
        int b = block_idx_xy_1d[idx], p = scan8[idx];
        int za = cache[p-1], zb = cache[p-8];
        int sum = za + zb;
        int pred = sum < 0x80 ? (sum+1)>>1 : sum & 0x7f;
        int nC = ct_index[pred & 0x7f];
        /* x264 passes h->dct.luma4x4[i]+1: AC blocks have 15 coefficients. */
        vk_cavlc_block_residual(s, VK_DCT_LUMA_AC, acq[b]+1, nC, 1, 15);
        cache[p] = (uint8_t)nnz[b];
    }
    if (cbp_chroma) {
        for (int p = 0; p < 2; ++p) {
            /* chroma_dc is already in zigzag_scan_2x2_dc order (4 contiguous). */
            int block[16] = {0};
            for (int i = 0; i < 4; ++i) block[i] = chroma_dc[p][i];
            vk_cavlc_block_residual(s, VK_DCT_CHROMA_DC, block, 4, 1, 4);
        }
    }
    if (cbp_chroma == 2) {
        /* x264_scan8[16..19]=Cb AC, [32..35]=Cr AC, [49]/[50]=chroma DC. */
        static const uint8_t cscan8[8] = { 52,53,60,61, 92,93,100,101 };
        static const uint8_t ct_index_c[17] = {0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3};
        uint8_t ccache[128] = {0};
        /* top edge of the first AC row of each chroma plane */
        for (int i = 0; i < 2; ++i) {
            int tp = cscan8[i]-8, lp = cscan8[i*2]-1;
            ccache[tp] = (nb && nb->top_chroma[0]) ? nb->top_chroma[0][i] : 0x80;
            ccache[lp] = (nb && nb->left_chroma[0]) ? nb->left_chroma[0][i] : 0x80;
            int tp2 = cscan8[4+i]-8, lp2 = cscan8[4+i*2]-1;
            ccache[tp2] = (nb && nb->top_chroma[1]) ? nb->top_chroma[1][i] : 0x80;
            ccache[lp2] = (nb && nb->left_chroma[1]) ? nb->left_chroma[1][i] : 0x80;
        }
        /* chroma DC nnz feeds the neighbouring AC blocks */
        ccache[40] = cdnz[0];
        ccache[80] = cdnz[1];
        for (int p = 0; p < 2; ++p)
            for (int b = 0; b < 4; ++b) {
                int k = p*4+b, pos = cscan8[k];
                int sum = ccache[pos-1] + ccache[pos-8];
                int pred = sum < 0x80 ? (sum+1)>>1 : sum & 0x7f;
                int nC = ct_index_c[pred & 0x7f];
                vk_cavlc_block_residual(s, VK_DCT_CHROMA_AC, cacq[p][b]+1, nC, 1, 15);
                ccache[pos] = (uint8_t)cnnz[p][b];
            }
    }

    memcpy(recon_y, pred_y, 256);
    memcpy(recon_u, pred_u, 64);
    memcpy(recon_v, pred_v, 64);

    /* ---- reconstruction: dequant + IDCT + add to prediction ---- */
    int dcq[16];
    for (int i = 0; i < 16; ++i) dcq[i] = dcdc[i]; /* quantised DC, raster */
    if (dcnz) { idct4x4dc(dcq); dequant4dc(dcq, dequant, qp); }
    for (int b = 0; b < 16; ++b) {
        int by = (b/4)*4, bx = (b%4)*4;
        uint8_t *dst = recon_y + by*16+bx;
        if ((cbp_luma & (1 << (b/4))) && nnz[b]) {
            int blk[16];
            for (int i = 0; i < 16; ++i) blk[i] = dct4[b][i];
            dequant4(blk, dequant, qp);
            blk[0] = dcnz ? dcq[b] : 0;
            idct4(blk);
            add_block(dst, 16, blk);
        } else if (dcnz) {
            add_block_dc(dst, 16, dcq[b]);
        }
    }
    if (out_nnz) {
        for (int i = 0; i < 16; ++i) out_nnz[i] = (uint8_t)nnz[i];
        for (int p = 0; p < 2; ++p)
            for (int b = 0; b < 4; ++b) out_cnnz[p][b] = (uint8_t)cnnz[p][b];
        *out_dcnz = (uint8_t)dcnz;
    }

    /* chroma reconstruction */
    for (int p = 0; p < 2; ++p) {
        uint8_t *dst = p ? recon_v : recon_u;
        /* x264 encoder/macroblock.c:72 idct_dequant_2x2_dconly */
        int d0 = chad[p][0]+chad[p][1], d1 = chad[p][2]+chad[p][3];
        int d2 = chad[p][0]-chad[p][1], d3 = chad[p][2]-chad[p][3];
        int dmf = cdeq[0] << (qp/6);
        int cdq[16] = {0};
        cdq[0] = ((d0+d1)*dmf)>>5; cdq[1] = ((d0-d1)*dmf)>>5;
        cdq[2] = ((d2+d3)*dmf)>>5; cdq[3] = ((d2-d3)*dmf)>>5;
        for (int b = 0; b < 4; ++b) {
            int by = (b/2)*4, bx = (b%2)*4;
            uint8_t *d = dst + by*8+bx;
            if (cacbp && cnnz[p][b]) {
                int blk[16];
                for (int i = 0; i < 16; ++i) blk[i] = cdct[p][b][i];
                dequant4(blk, cdeq, qp);
                blk[0] = cdnz[p] ? cdq[b] : 0;
                idct4(blk);
                add_block(d, 8, blk);
            } else if (cdnz[p]) {
                add_block_dc(d, 8, cdq[b]);
            }
        }
    }
}

