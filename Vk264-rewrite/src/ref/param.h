/* SPDX-License-Identifier: GPL-2.0-or-later
 * Minimal x264 parameter subset. Field names mirror x264_param_t where possible. */
#ifndef VK264_PARAM_H
#define VK264_PARAM_H

#include "bitstream.h"

typedef struct {
    int i_width, i_height;
    int i_fps_num, i_fps_den;
    int i_level_idc;        /* 10 = level 1.0 */
    int i_frame_reference;
    int i_keyint_max;
    int i_bframe;
    int i_csp;              /* 420 only for now */
    int b_cabac;
    int b_transform_8x8;
    int b_interlaced;
    int i_chroma_qp_offset;
    int i_qp_constant;
    int i_mv_range;         /* derived from level when 0 */
    int i_subme;            /* x264 analyse.i_subpel_refine (0=fullpel only) */
    int b_weighted_pred;    /* PPS weighted_pred / weighted_bipred_idc */
    int i_weighted_bipred;
    int b_vui;
    int i_sar_width, i_sar_height;
    int i_vidformat, b_fullrange, i_colorprim, i_transfer, i_colmatrix;
} vk_param_t;

void vk_param_default(vk_param_t *p);
/* Apply the SPS/PPS-relevant subset of an x264 preset. Returns -1 if unknown. */
int vk_param_apply_preset(vk_param_t *p, const char *preset);

/* x264 SPS/PPS structures, reduced to the fields the writers need. */
typedef struct {
    int i_id, i_profile_idc, i_level_idc;
    int b_constraint_set0, b_constraint_set1, b_constraint_set2, b_constraint_set3;
    int i_chroma_format_idc;
    int i_mb_width, i_mb_height, b_frame_mbs_only, b_mb_adaptive_frame_field;
    int b_direct8x8_inference;
    int b_qpprime_y_zero_transform_bypass, b_avcintra_hd, b_avcintra_4k, i_cqm_preset;
    int i_log2_max_frame_num, i_poc_type, i_log2_max_poc_lsb;
    int i_num_ref_frames, b_gaps_in_frame_num_value_allowed;
    int b_crop, crop_left, crop_right, crop_top, crop_bottom;
    int b_vui;
    struct {
        int b_aspect_ratio_info_present, i_sar_width, i_sar_height;
        int b_overscan_info_present, b_overscan_info;
        int b_signal_type_present, i_vidformat, b_fullrange, b_color_description_present;
        int i_colorprim, i_transfer, i_colmatrix;
        int b_chroma_loc_info_present, i_chroma_loc_top, i_chroma_loc_bottom;
        int b_timing_info_present, i_num_units_in_tick, i_time_scale, b_fixed_frame_rate;
        int b_nal_hrd_parameters_present, b_vcl_hrd_parameters_present;
        int b_pic_struct_present, b_bitstream_restriction;
        int b_motion_vectors_over_pic_boundaries;
        int i_max_bytes_per_pic_denom, i_max_bits_per_mb_denom;
        int i_log2_max_mv_length_horizontal, i_log2_max_mv_length_vertical;
        int i_num_reorder_frames, i_max_dec_frame_buffering;
    } vui;
} vk_sps_t;

typedef struct {
    int i_id, i_sps_id, b_cabac, b_pic_order, i_num_slice_groups;
    int i_num_ref_idx_l0_default_active, i_num_ref_idx_l1_default_active;
    int b_weighted_pred, b_weighted_bipred;
    int i_pic_init_qp, i_pic_init_qs, i_chroma_qp_index_offset;
    int b_deblocking_filter_control, b_constrained_intra_pred, b_redundant_pic_cnt;
    int b_transform_8x8_mode;
} vk_pps_t;

void vk_sps_init(vk_sps_t *sps, int id, const vk_param_t *p);
void vk_sps_write(vk_bs_t *s, const vk_sps_t *sps);
void vk_pps_init(vk_pps_t *pps, int id, const vk_param_t *p, const vk_sps_t *sps);
void vk_pps_write(vk_bs_t *s, const vk_sps_t *sps, const vk_pps_t *pps);

#endif /* VK264_PARAM_H */
