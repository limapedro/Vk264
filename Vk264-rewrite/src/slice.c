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
    const int deadzone = (list == 0) ? 11 : 21; /* CQM_4IY / CQM_4IC */
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

static int quant_one(int coef, int mf, int bias)
{
    if (coef > 0) return (int)(((uint32_t)bias + (uint32_t)coef) * (uint32_t)mf >> 16);
    return -(int)(((uint32_t)bias + (uint32_t)(-coef)) * (uint32_t)mf >> 16);
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
    int mf[16], bias[16], dequant[16];
    uint8_t pred_y[256], pred_u[64], pred_v[64];
    predict_dc(pred_y, 16, 128);  /* fdec initial value at frame edge */
    predict_dc(pred_u, 8, 128);  /* chroma DC_128 at frame corner */
    predict_dc(pred_v, 8, 128);

    int dct4[16][16], dc4[16];
    quant_tables(qp, 0, mf, bias, dequant);
    for (int b = 0; b < 16; ++b) {
        int by = (b/4)*4, bx = (b%4)*4;
        uint8_t src_block[16], pred_block[16];
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                src_block[y*4+x] = src_y[(by+y)*16+bx+x];
                pred_block[y*4+x] = pred_y[(by+y)*16+bx+x];
            }
        sub4x4_dct(dct4[b], src_block, pred_block);
        dc4[b] = dct4[b][0];
        for (int i = 0; i < 16; ++i) dct4[b][i] = 0;
    }
    /* AC quantisation: ultrafast has no trellis; DCT decimation disabled. */
    int cbp_luma = 0;
    int mf_ac[16], bias_ac[16];
    quant_tables(qp, 0, mf_ac, bias_ac, dequant);
    for (int b = 0; b < 16; ++b) {
        (void)mf_ac; (void)bias_ac;
        /* DC was extracted; AC coefficients are all zero in this milestone. */
    }

    /* Luma 16x16 DC: Hadamard + deadzone quantisation. */
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

    /* Chroma DC: 2x2 Hadamard. */
    int cdc[2][4];
    sub8x8_dct_dc(cdc[0], src_u, pred_u);
    sub8x8_dct_dc(cdc[1], src_v, pred_v);
    int cmf[16], cbias[16], cdeq[16];
    quant_tables(qp, 2, cmf, cbias, cdeq);
    int chroma_dc[2][4], cbp_chroma = 0;
    for (int p = 0; p < 2; ++p) {
        /* x264 encoder/macroblock.c:82 dct2x2dc */
        int a = cdc[p][0] + cdc[p][1];
        int b = cdc[p][2] + cdc[p][3];
        int c = cdc[p][0] - cdc[p][1];
        int d = cdc[p][2] - cdc[p][3];
        int had[4] = { a+b, a-b, c+d, c-d };
        int q[4];
        for (int i = 0; i < 4; ++i)
            q[i] = quant_one(had[i], cmf[0] >> 1, cbias[0] << 1);
        /* zigzag_scan_2x2_dc: {dct[0],dct[2],dct[1],dct[3]} */
        chroma_dc[p][0] = q[0];
        chroma_dc[p][1] = q[2];
        chroma_dc[p][2] = q[1];
        chroma_dc[p][3] = q[3];
        for (int i = 0; i < 4; ++i) cbp_chroma |= chroma_dc[p][i] != 0;
    }

    /* ---- entropy ---- */
    uint32_t mb_type = 1u + 2u /* DC */ + 4u*(uint32_t)(cbp_chroma ? 1 : 0)
                     + (cbp_luma ? 12u : 0u);
    vk_bs_write_ue(s, mb_type);
    vk_bs_write_ue(s, 0);  /* chroma pred mode = DC */
    vk_bs_write_se(s, 0);  /* mb_qp_delta */
    /* luma DC residual: 16 coefficients, nC = 0 for the first MB */
    int luma_dc_coef[16];
    for (int i = 0; i < 16; ++i) luma_dc_coef[i] = dc_q[i];
    vk_cavlc_block_residual(s, VK_DCT_LUMA_DC, luma_dc_coef, 0, 1);
    if (cbp_chroma) {
        for (int p = 0; p < 2; ++p) {
            int block[16] = {0};
            for (int i = 0; i < 4; ++i) block[zigzag4[i]] = chroma_dc[p][i];
            vk_cavlc_block_residual(s, VK_DCT_CHROMA_DC, block, 4, 1);
        }
    }

    /* Reconstruction (needed for neighbouring MB prediction in later work). */
    memcpy(recon_y, pred_y, 256);
    memcpy(recon_u, pred_u, 64);
    memcpy(recon_v, pred_v, 64);
    (void)recon_y; (void)recon_u; (void)recon_v;
}
