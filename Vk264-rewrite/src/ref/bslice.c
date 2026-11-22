/* SPDX-License-Identifier: GPL-2.0-or-later
 * Minimal CAVLC B-slice writer for x264-compatible B-frames whose MBs are
 * all skips (B_SKIP / P_SKIP) with zero residual. */
#include "bslice.h"

void vk_encode_b_slice_allskip(vk_bs_t *bs, const vk_sps_t *sps,
                               const vk_pps_t *pps, int frame_num,
                               int slice_qp, int num_mbs)
{
    vk_bs_write_ue(bs, 0);   /* first_mb_in_slice */
    vk_bs_write_ue(bs, 6);   /* slice_type: B (all slices B) */
    vk_bs_write_ue(bs, 0);   /* pps_id */
    vk_bs_write(bs, (unsigned)sps->i_log2_max_frame_num, (uint32_t)frame_num);
    if (sps->i_poc_type == 0)
        vk_bs_write(bs, (unsigned)sps->i_log2_max_poc_lsb,
                    (uint32_t)(2 * frame_num));  /* poc_lsb */
    vk_bs_write1(bs, 1);     /* direct_spatial_mv_pred_flag */
    vk_bs_write1(bs, 0);     /* num_ref_idx_active_override_flag */
    vk_bs_write1(bs, 0);     /* ref_pic_list_modification_flag_l0 */
    vk_bs_write1(bs, 0);     /* ref_pic_list_modification_flag_l1 */
    vk_bs_write1(bs, 0);     /* adaptive_ref_pic_marking_mode_flag */
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
