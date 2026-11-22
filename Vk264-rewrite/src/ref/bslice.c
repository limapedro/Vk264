/* SPDX-License-Identifier: GPL-2.0-or-later
 * Minimal CAVLC B-slice writer for x264-compatible B-frames whose MBs are
 * all skips (B_SKIP / P_SKIP) with zero residual. */
#include "bslice.h"

void vk_encode_b_slice_allskip(vk_bs_t *bs, const vk_sps_t *sps,
                               const vk_pps_t *pps, int frame_num, int poc,
                               int slice_qp, int num_mbs, int nal_ref_idc)
{
    vk_bs_write_ue(bs, 0);   /* first_mb_in_slice */
    vk_bs_write_ue(bs, 6);   /* slice_type: B (all slices B) */
    vk_bs_write_ue(bs, 0);   /* pps_id */
    vk_bs_write(bs, (unsigned)sps->i_log2_max_frame_num, (uint32_t)frame_num);
    if (sps->i_poc_type == 0)
        vk_bs_write(bs, (unsigned)sps->i_log2_max_poc_lsb, (uint32_t)poc);
    vk_bs_write1(bs, 1);     /* direct_spatial_mv_pred_flag */
    vk_bs_write1(bs, 0);     /* num_ref_idx_active_override_flag */
    vk_bs_write1(bs, 0);     /* ref_pic_list_modification_flag_l0 */
    vk_bs_write1(bs, 0);     /* ref_pic_list_modification_flag_l1 */
    if (nal_ref_idc != 0)
        vk_bs_write1(bs, 0);  /* adaptive_ref_pic_marking_mode_flag */
    vk_bs_write_se(bs, slice_qp - pps->i_pic_init_qp);
    if (pps->b_deblocking_filter_control) {
        vk_bs_write_ue(bs, (uint32_t)(pps->b_deblocking_filter ? 0 : 1));
        if (pps->b_deblocking_filter) {
            vk_bs_write_se(bs, 0);
            vk_bs_write_se(bs, 0);
        }
    }
    /* all MBs skipped */
    vk_bs_write_ue(bs, (uint32_t)num_mbs);
}

void vk_encode_p_slice_allskip(vk_bs_t *bs, const vk_sps_t *sps,
                               const vk_pps_t *pps, int frame_num, int poc,
                               int slice_qp, int num_mbs)
{
    vk_bs_write_ue(bs, 0);   /* first_mb_in_slice */
    vk_bs_write_ue(bs, 5);   /* slice_type: P (all slices P) */
    vk_bs_write_ue(bs, 0);   /* pps_id */
    vk_bs_write(bs, (unsigned)sps->i_log2_max_frame_num, (uint32_t)frame_num);
    if (sps->i_poc_type == 0)
        vk_bs_write(bs, (unsigned)sps->i_log2_max_poc_lsb, (uint32_t)poc);
    vk_bs_write1(bs, 0);  /* num_ref_idx_active_override_flag */
    vk_bs_write1(bs, 0);  /* ref_pic_list_modification_flag_l0 */
    if (pps->b_weighted_pred) {
        vk_bs_write_ue(bs, 0);
        vk_bs_write_ue(bs, 0);
        for (int i = 0; i < pps->i_num_ref_idx_l0_default_active; ++i) {
            vk_bs_write1(bs, 0);
            vk_bs_write1(bs, 0);
        }
    }
    vk_bs_write1(bs, 0);  /* adaptive_ref_pic_marking_mode_flag */
    vk_bs_write_se(bs, slice_qp - pps->i_pic_init_qp);
    if (pps->b_deblocking_filter_control) {
        vk_bs_write_ue(bs, (uint32_t)(pps->b_deblocking_filter ? 0 : 1));
        if (pps->b_deblocking_filter) {
            vk_bs_write_se(bs, 0);
            vk_bs_write_se(bs, 0);
        }
    }
    vk_bs_write_ue(bs, (uint32_t)num_mbs);
}
