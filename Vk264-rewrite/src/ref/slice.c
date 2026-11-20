/* SPDX-License-Identifier: GPL-2.0-or-later
 * Slice header + I16x16 macroblock encoder. Transcribed from x264:
 *   encoder/encoder.c:213   slice_header_write
 *   encoder/macroblock.c:126 mb_encode_i16x16
 *   common/dct.c:157        sub4x4_dct / :47 dct4x4dc
 *   common/quant.c:67       quant_4x4 / :88 quant_4x4_dc / :96 quant_2x2_dc
 */
#include "slice.h"
#include "cavlc.h"
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
static const uint8_t zigzag4[16] = {
    0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15
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
                      uint8_t *recon_v, int qp, int mb_x, int mb_y)
{
    (void)mb_x; (void)mb_y;
    int mf[16], bias[16], dequant[16], cmf[16], cbias[16], cdeq[16];
    uint8_t pred_y[256], pred_u[64], pred_v[64];
    predict_dc(pred_y, 16, 128);
    predict_dc(pred_u, 8, 128);
    predict_dc(pred_v, 8, 128);
    quant_tables(qp, 0, mf, bias, dequant);
    quant_tables(qp, 2, cmf, cbias, cdeq);

    /* ---- luma: 16 4x4 DCTs, split DC, quantise AC ---- */
    int dct4[16][16], dc4[16], nnz[16] = {0}, acq[16][16];
    for (int b = 0; b < 16; ++b) {
        int by = (b/4)*4, bx = (b%4)*4;
        uint8_t sb[16], pb[16];
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                sb[y*4+x] = src_y[(by+y)*16+bx+x];
                pb[y*4+x] = pred_y[(by+y)*16+bx+x];
            }
        sub4x4_dct(dct4[b], sb, pb);
        dc4[b] = dct4[b][0];
        dct4[b][0] = 0;                 /* DC goes through the Hadamard path */
        for (int i = 0; i < 16; ++i) {
            dct4[b][i] = quant_one(dct4[b][i], mf[i], bias[i]);
            nnz[b] |= dct4[b][i] != 0;
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
    for (int i = 0; i < 16; ++i) dcdc[i] = quant_one(dcdc[i], dc_mf, dc_bias);
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
                    sb[y*4+x] = src[(by+y)*8+bx+x];
                    pb[y*4+x] = prd[(by+y)*8+bx+x];
                }
            sub4x4_dct(cdct[p][b], sb, pb);
            cdc[p][b] = cdct[p][b][0];
            cdct[p][b][0] = 0;
            for (int i = 0; i < 16; ++i) {
                cdct[p][b][i] = quant_one(cdct[p][b][i], cmf[i], cbias[i]);
                cnnz[p][b] |= cdct[p][b][i] != 0;
            }
            for (int i = 0; i < 16; ++i) cacq[p][b][i] = cdct[p][b][zigzag4[i]];
        }
    }
    int chroma_dc[2][4], cbp_chroma = 0, cacbp = 0;
    for (int p = 0; p < 2; ++p) {
        int a = cdc[p][0] + cdc[p][1], b = cdc[p][2] + cdc[p][3];
        int c = cdc[p][0] - cdc[p][1], d = cdc[p][2] - cdc[p][3];
        int had[4] = { a+b, a-b, c+d, c-d }, q[4];
        for (int i = 0; i < 4; ++i)
            q[i] = quant_one(had[i], cmf[0] >> 1, cbias[0] << 1);
        chroma_dc[p][0] = q[0]; chroma_dc[p][1] = q[2];
        chroma_dc[p][2] = q[1]; chroma_dc[p][3] = q[3];
        for (int i = 0; i < 4; ++i) cbp_chroma |= chroma_dc[p][i] != 0;
        for (int i = 0; i < 4; ++i) cacbp |= cnnz[p][i];
    }
    if (cacbp) cbp_chroma = 2;

    /* ---- entropy ---- */
    uint32_t mb_type = 1u + 2u /* I16 DC */ + 4u*(uint32_t)(cbp_chroma ? 1 : 0)
                     + (cbp_luma ? 12u : 0u);
    vk_bs_write_ue(s, mb_type);
    vk_bs_write_ue(s, 0);  /* chroma pred mode = DC */
    vk_bs_write_se(s, 0);  /* mb_qp_delta */

    vk_cavlc_block_residual(s, VK_DCT_LUMA_DC, dc_q, 0, 1);
    for (int b = 0; b < 16; ++b) {
        if (!(cbp_luma & (1 << (b/4))) || !nnz[b]) continue;
        int left = (b % 4) ? nnz[b-1] : 0;
        int top = (b >= 4) ? nnz[b-4] : 0;
        static const uint8_t ct_index[17] = {0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3};
        int nC = ct_index[(left + top + 1) >> 1];
        vk_cavlc_block_residual(s, VK_DCT_LUMA_AC, acq[b], nC, 1);
    }
    if (cbp_chroma) {
        for (int p = 0; p < 2; ++p) {
            int block[16] = {0};
            for (int i = 0; i < 4; ++i) block[zigzag4[i]] = chroma_dc[p][i];
            vk_cavlc_block_residual(s, VK_DCT_CHROMA_DC, block, 4, 1);
        }
    }
    if (cbp_chroma == 2) {
        for (int p = 0; p < 2; ++p)
            for (int b = 0; b < 4; ++b) {
                if (!cnnz[p][b]) continue;
                int left = (b % 2) ? cnnz[p][b-1] : 0;
                int top = (b >= 2) ? cnnz[p][b-2] : 0;
                static const uint8_t ct_index_c[17] = {0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3};
                int nC = ct_index_c[(left + top + 1) >> 1];
                vk_cavlc_block_residual(s, VK_DCT_CHROMA_AC, cacq[p][b], nC, 1);
            }
    }

    memcpy(recon_y, pred_y, 256);
    memcpy(recon_u, pred_u, 64);
    memcpy(recon_v, pred_v, 64);
    (void)recon_y; (void)recon_u; (void)recon_v;
}
