/* SPDX-License-Identifier: GPL-2.0-or-later
 * SPS/PPS generation. Transcribed from x264 encoder/set.c:
 *   x264_sps_init()            lines 101-247
 *   x264_sps_write()           lines 305-477
 *   x264_pps_init()            lines 479-503
 *   x264_pps_write()           lines 505-...
 * The subset below covers 8-bit 4:2:0 progressive CAVLC, which is enough for
 * the first byte-exactness milestone. Extra fields are written in the same
 * order as x264 so future presets only need new initialisation values. */

#include "param.h"
#include <string.h>
#include <stdlib.h>

#define PROFILE_BASELINE 66
#define PROFILE_MAIN 77
#define PROFILE_HIGH 100
#define CHROMA_420 1
#define QP_BD_OFFSET 0

int vk_chroma_qp_offset = 0;
int vk_i4_analysis_enabled = 0;
int vk_i8_analysis_enabled = 0;
int vk_cur_subme = 0;
int vk_cur_trellis = 0;
int vk_cur_fast_pskip = 1;
int vk_cur_me_range = 16;
int vk_cur_psy_rd = 0;

void vk_param_default(vk_param_t *p)
{
    memset(p, 0, sizeof(*p));
    p->i_width = 16; p->i_height = 16;
    p->i_fps_num = 1; p->i_fps_den = 60;
    p->i_level_idc = 0;  /* 0 = auto-select from size/framerate, like x264 */
    p->i_frame_reference = 1;
    p->i_keyint_max = 1;
    p->i_csp = CHROMA_420;
    p->i_qp_constant = 28;
    /* Default param (used by the sweep tests) keeps the historical
     * deblocking-disabled behaviour; only explicit non-ultrafast presets
     * enable it, matching x264. */
    p->b_deblocking_filter = 0;
    p->b_vui = 1;
    p->i_sar_width = 1; p->i_sar_height = 1;
    p->i_vidformat = 5;    /* unspecified */
    p->b_fullrange = 0;
    p->i_colorprim = 2; p->i_transfer = 2; p->i_colmatrix = 2;
}

/* Case-insensitive preset name compare — portable replacement for the
 * strcasecmp() used in x264's param_apply_preset(). Returns 1 on match. */
static int preset_eq(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
        ++a; ++b;
    }
    return *a == *b;
}

/* Mirror the SPS/PPS-relevant subset of x264 common/base.c
 * param_apply_preset().  Analysis knobs (me/subme/trellis/lookahead) are not
 * modelled here; this only aligns headers for each preset. Returns -1 if
 * the preset name is unknown. */
int vk_param_apply_preset(vk_param_t *p, const char *preset)
{
    if (!preset || !*preset) preset = "ultrafast";
    /* x264 defaults (medium): 3 refs, 3 B-frames, CABAC + 8x8dct + deblock. */
    p->i_frame_reference = 3;
    p->i_bframe = 3;
    p->b_transform_8x8 = 1;
    p->b_cabac = 1;
    p->i_subme = 7;  /* x264 medium default */
    /* x264 default analyse.i_weighted_pred = SMART(2) => PPS 1/2; ultrafast
     * overrides to NONE(0). */
    p->b_weighted_pred = 1; p->i_weighted_bipred = 2;
    p->b_deblocking_filter = 1;
    if (preset_eq(preset, "ultrafast")) {
        p->i_frame_reference = 1; p->i_bframe = 0;
        p->b_transform_8x8 = 0; p->b_cabac = 0;
        p->i_subme = 0;
        p->b_deblocking_filter = 0;
        p->b_weighted_pred = 0; p->i_weighted_bipred = 0;
    } else if (preset_eq(preset, "superfast")) {
        p->i_frame_reference = 1; p->i_subme = 1;
    } else if (preset_eq(preset, "veryfast")) {
        p->i_frame_reference = 1; p->i_subme = 2;
    } else if (preset_eq(preset, "faster")) {
        p->i_frame_reference = 2; p->i_subme = 4;
    } else if (preset_eq(preset, "fast")) {
        p->i_frame_reference = 2; p->i_subme = 6;
    } else if (preset_eq(preset, "medium")) {
        /* x264 defaults already set above */
    } else if (preset_eq(preset, "slow")) {
        p->i_frame_reference = 5; p->i_subme = 8;
    } else if (preset_eq(preset, "slower")) {
        p->i_frame_reference = 8; p->i_subme = 9;
    } else if (preset_eq(preset, "veryslow")) {
        p->i_frame_reference = 16; p->i_bframe = 8; p->i_subme = 10;
    } else if (preset_eq(preset, "placebo")) {
        p->i_frame_reference = 16; p->i_bframe = 16; p->i_subme = 11;
    } else {
        return -1;
    }
    /* x264 encoder.c:1218-1231: psy-RD is active when i_subpel_refine >= 6,
     * lowering chroma QP offset by 2 (default f_psy_rd = 1.0). Applies to
     * fast and above; ultrafast..faster keep 0. */
    p->i_chroma_qp_offset = (p->i_subme >= 6) ? -2 : 0;
    vk_chroma_qp_offset = p->i_chroma_qp_offset;
    vk_i4_analysis_enabled = (p->i_subme >= 1) ? 1 : 0;
    vk_i8_analysis_enabled = p->b_transform_8x8 ? 1 : 0;
    vk_cur_fast_pskip = preset_eq(preset, "placebo") ? 0 : 1;
    vk_cur_me_range = (preset_eq(preset, "veryslow") || preset_eq(preset, "placebo")) ? 24 : 16;
    vk_cur_psy_rd = (p->i_subme >= 6) ? 256 : 0;
    vk_cur_subme = p->i_subme;
    /* Trellis is OFF by default in the oracle. The naive analysis-time
     * wiring (PR #290) regressed faster 17->11 and veryfast 49->11 because
     * x264 applies trellis at CABAC *write* time with live context state,
     * not during analysis. Keep the foundation selectable for experiments:
     *   VK_TRELLIS=1  (0=off default, 1/2=experimental)
     * x264 preset defaults for reference: ultrafast/superfast/veryfast 0,
     * faster/fast/medium 1, slow..placebo 2. */
    /* Parameter parity with x264 common/base.c defaults: trellis is 0 for
     * ultrafast/superfast/veryfast, 1 for faster/fast/medium, 2 for slow+.
     * The oracle must use the same values as x264 to have any chance of a
     * byte-exact match. E45 verified this alignment changes bitstreams but
     * does not regress any metric. VK_TRELLIS still overrides for A/B tests. */
    if (p->i_subme <= 2) vk_cur_trellis = 0;
    else if (p->i_subme <= 6) vk_cur_trellis = 1;
    else vk_cur_trellis = 2;
    { const char *te=getenv("VK_TRELLIS"); if(te) vk_cur_trellis=atoi(te); }
    return 0;
}

static int log2i(int v)
{
    int n = 0;
    while ((1 << (n + 1)) <= v) ++n;
    return n;
}

void vk_sps_init(vk_sps_t *s, int id, const vk_param_t *p)
{
    memset(s, 0, sizeof(*s));
    s->i_id = id;
    s->i_mb_width = (p->i_width + 15) / 16;
    s->i_mb_height = (p->i_height + 15) / 16;
    s->b_frame_mbs_only = !p->b_interlaced;
    s->i_chroma_format_idc = CHROMA_420;

    if (p->b_cabac || p->i_bframe > 0 || p->b_interlaced || p->b_transform_8x8)
        s->i_profile_idc = p->b_transform_8x8 ? PROFILE_HIGH : PROFILE_MAIN;
    else
        s->i_profile_idc = PROFILE_BASELINE;

    s->b_constraint_set0 = s->i_profile_idc == PROFILE_BASELINE;
    s->b_constraint_set1 = s->i_profile_idc <= PROFILE_MAIN;
    /* x264 auto-selects the smallest level for the frame size and frame rate
     * (common/tables.c x264_levels + encoder.c x264_validate_levels). */
    if (p->i_level_idc == 0) {
        /* fps is the reciprocal of the timebase (default 1/60 -> 60 fps). */
        int fps = (p->i_fps_num ? p->i_fps_den/p->i_fps_num : 30);
        int mbps = ((p->i_width+15)/16) * ((p->i_height+15)/16) * fps;
        /* Full x264 level table: {level_idc, max_mbps, max_frame_size_in_MBs,
         * max_mv_range} — mirrors references/x264-master/common/tables.c.
         * The previous truncated table fell through to level 52 for anything
         * above 640x480, causing SPS mismatches at larger resolutions. */
        static const int levels[][4] = {
            {10,1485,99,64}, {9,1485,99,128}, {11,3000,396,128}, {12,6000,396,128},
            {13,11880,396,128}, {20,11880,396,128}, {21,19800,792,256}, {22,20250,1620,256},
            {30,40500,1620,256}, {31,108000,3600,512}, {32,216000,5120,512},
            {40,245760,8192,512}, {41,245760,8192,512}, {42,522240,8704,512},
            {50,589824,22080,512}, {51,983040,36864,512}, {52,2073600,36864,512},
            {60,4177920,139264,8192}, {61,8355840,139264,8192}, {62,16711680,139264,8192}
        };
        int fs = ((p->i_width+15)/16)*((p->i_height+15)/16);
        s->i_level_idc = 52;
        int mv_range = 8192;
        for (unsigned i = 0; i < sizeof(levels)/sizeof(levels[0]); ++i)
            if (mbps <= levels[i][1] && fs <= levels[i][2]) {
                s->i_level_idc = levels[i][0];
                mv_range = levels[i][3];
                break;
            }
        s->vui.i_log2_max_mv_length_horizontal =
        s->vui.i_log2_max_mv_length_vertical = log2i(mv_range*4-1)+1;
    } else {
        s->i_level_idc = p->i_level_idc;
    }

    /* x264 set.c: i_bframe_pyramid defaults to NORMAL(1), so num_reorder=2
     * whenever B-frames are enabled, and num_ref_frames is at least 4. */
    s->vui.i_num_reorder_frames = p->i_bframe ? 2 : 0;
    int refs = p->i_frame_reference > 1 ? p->i_frame_reference : 1;
    int minrefs = 1 + s->vui.i_num_reorder_frames;
    /* x264 forces i_bframe_pyramid=0 when there are no B-frames. */
    if (p->i_bframe > 0 && minrefs < 4) minrefs = 4;
    if (refs < minrefs) refs = minrefs;
    s->vui.i_max_dec_frame_buffering = s->i_num_ref_frames = refs;
    if (p->i_keyint_max == 1) {
        s->i_num_ref_frames = 0;
        s->vui.i_max_dec_frame_buffering = 0;
    }

    /* x264 set.c:161: with B-pyramid enabled the DPB holds two frame_num
     * generations, so max_frame_num = max_dec_frame_buffering*2 + 1. */
    int max_frame_num = s->vui.i_max_dec_frame_buffering * (p->i_bframe > 0 ? 2 : 1) + 1;
    s->i_log2_max_frame_num = 4;
    while ((1 << s->i_log2_max_frame_num) <= max_frame_num) ++s->i_log2_max_frame_num;

    s->i_poc_type = p->i_bframe || p->b_interlaced ? 0 : 2;
    if (s->i_poc_type == 0) {
        /* x264 set.c: (bframe+2) * (!!bframe_pyramid+1) * 2; pyramid=1 */
        int max_delta_poc = (p->i_bframe + 2) * 2 * 2;
        s->i_log2_max_poc_lsb = 4;
        while ((1 << s->i_log2_max_poc_lsb) <= max_delta_poc * 2) ++s->i_log2_max_poc_lsb;
    }

    s->b_gaps_in_frame_num_value_allowed = 0;
    s->b_mb_adaptive_frame_field = p->b_interlaced;
    s->b_direct8x8_inference = 1;
    {
        int cw = s->i_mb_width * 16;
        int ch = s->i_mb_height * 16 * (s->b_frame_mbs_only ? 1 : 2);
        int crop_right = cw - p->i_width;
        int crop_bottom = ch - p->i_height;
        s->crop_left = s->crop_top = 0;
        s->crop_right = crop_right;
        s->crop_bottom = crop_bottom;
        s->b_crop = (crop_right || crop_bottom) ? 1 : 0;
    }
    s->b_vui = p->b_vui;

    s->vui.b_aspect_ratio_info_present = 0;
    s->vui.i_sar_width = p->i_sar_width;
    s->vui.i_sar_height = p->i_sar_height;
    s->vui.b_overscan_info_present = 0;
    s->vui.i_vidformat = (p->i_vidformat >= 0 && p->i_vidformat <= 5) ? p->i_vidformat : 5;
    s->vui.b_fullrange = p->b_fullrange;
    s->vui.i_colorprim = p->i_colorprim;
    s->vui.i_transfer = p->i_transfer;
    s->vui.i_colmatrix = p->i_colmatrix;
    s->vui.b_color_description_present =
        (p->i_colorprim != 2 || p->i_transfer != 2 || p->i_colmatrix != 2);
    s->vui.b_signal_type_present =
        (s->vui.i_vidformat != 5 || s->vui.b_fullrange || s->vui.b_color_description_present);
    s->vui.b_chroma_loc_info_present = 0;

    s->vui.b_timing_info_present = p->i_fps_num > 0 && p->i_fps_den > 0;
    if (s->vui.b_timing_info_present) {
        s->vui.i_num_units_in_tick = p->i_fps_num;
        s->vui.i_time_scale = p->i_fps_den * 2;
        s->vui.b_fixed_frame_rate = 1;
    }
    s->vui.b_vcl_hrd_parameters_present = 0;
    s->vui.b_nal_hrd_parameters_present = 0;
    s->vui.b_pic_struct_present = 0;
    s->vui.b_bitstream_restriction = 1;
    s->vui.b_motion_vectors_over_pic_boundaries = 1;
    s->vui.i_max_bytes_per_pic_denom = 0;
    s->vui.i_max_bits_per_mb_denom = 0;
    /* Auto-selected level already set these; explicit mv_range overrides. */
    if (p->i_mv_range) {
        int mv = p->i_mv_range * 4 - 1;
        if (mv < 1) mv = 1;
        s->vui.i_log2_max_mv_length_horizontal = log2i(mv) + 1;
        s->vui.i_log2_max_mv_length_vertical = log2i(mv) + 1;
    }
}

static const struct { unsigned char w, h, sar; } kSar[] = {
    {1,1,1},{12,11,2},{10,11,3},{16,11,4},{40,33,5},{24,11,6},{20,11,7},{32,11,8},
    {80,33,9},{18,11,10},{15,11,11},{64,33,12},{160,99,13},{4,3,14},{3,2,15},{2,1,16}
};

void vk_sps_write(vk_bs_t *s, const vk_sps_t *q)
{
    vk_bs_align(s); /* bs_realign */
    vk_bs_write(s, 8, (uint32_t)q->i_profile_idc);
    vk_bs_write1(s, (uint32_t)q->b_constraint_set0);
    vk_bs_write1(s, (uint32_t)q->b_constraint_set1);
    vk_bs_write1(s, (uint32_t)q->b_constraint_set2);
    vk_bs_write1(s, (uint32_t)q->b_constraint_set3);
    vk_bs_write(s, 4, 0);
    vk_bs_write(s, 8, (uint32_t)q->i_level_idc);
    vk_bs_write_ue(s, (uint32_t)q->i_id);

    if (q->i_profile_idc >= PROFILE_HIGH) {
        vk_bs_write_ue(s, (uint32_t)q->i_chroma_format_idc);
        vk_bs_write_ue(s, 0); /* bit_depth_luma_minus8 */
        vk_bs_write_ue(s, 0); /* bit_depth_chroma_minus8 */
        vk_bs_write1(s, (uint32_t)q->b_qpprime_y_zero_transform_bypass);
        vk_bs_write1(s, (uint32_t)q->b_avcintra_hd); /* seq_scaling_matrix_present_flag */
        /* AVC-Intra scaling lists are not supported yet. */
    }

    vk_bs_write_ue(s, (uint32_t)(q->i_log2_max_frame_num - 4));
    vk_bs_write_ue(s, (uint32_t)q->i_poc_type);
    if (q->i_poc_type == 0)
        vk_bs_write_ue(s, (uint32_t)(q->i_log2_max_poc_lsb - 4));
    vk_bs_write_ue(s, (uint32_t)q->i_num_ref_frames);
    vk_bs_write1(s, (uint32_t)q->b_gaps_in_frame_num_value_allowed);
    vk_bs_write_ue(s, (uint32_t)(q->i_mb_width - 1));
    vk_bs_write_ue(s, (uint32_t)((q->i_mb_height >> !q->b_frame_mbs_only) - 1));
    vk_bs_write1(s, (uint32_t)q->b_frame_mbs_only);
    if (!q->b_frame_mbs_only)
        vk_bs_write1(s, (uint32_t)q->b_mb_adaptive_frame_field);
    vk_bs_write1(s, (uint32_t)q->b_direct8x8_inference);

    vk_bs_write1(s, (uint32_t)q->b_crop);
    if (q->b_crop) {
        int h_shift = q->i_chroma_format_idc == CHROMA_420 ? 1 : 0;
        int v_shift = (q->i_chroma_format_idc == CHROMA_420) + !q->b_frame_mbs_only;
        vk_bs_write_ue(s, (uint32_t)(q->crop_left >> h_shift));
        vk_bs_write_ue(s, (uint32_t)(q->crop_right >> h_shift));
        vk_bs_write_ue(s, (uint32_t)(q->crop_top >> v_shift));
        vk_bs_write_ue(s, (uint32_t)(q->crop_bottom >> v_shift));
    }

    vk_bs_write1(s, (uint32_t)q->b_vui);
    if (q->b_vui) {
        vk_bs_write1(s, (uint32_t)q->vui.b_aspect_ratio_info_present);
        if (q->vui.b_aspect_ratio_info_present) {
            unsigned i;
            unsigned char id = 255;
            for (i = 0; i < sizeof(kSar)/sizeof(kSar[0]); ++i)
                if (kSar[i].w == q->vui.i_sar_width && kSar[i].h == q->vui.i_sar_height) {
                    id = kSar[i].sar; break;
                }
            vk_bs_write(s, 8, id);
            if (id == 255) {
                vk_bs_write(s, 16, (uint32_t)q->vui.i_sar_width);
                vk_bs_write(s, 16, (uint32_t)q->vui.i_sar_height);
            }
        }
        vk_bs_write1(s, (uint32_t)q->vui.b_overscan_info_present);
        if (q->vui.b_overscan_info_present)
            vk_bs_write1(s, (uint32_t)q->vui.b_overscan_info);
        vk_bs_write1(s, (uint32_t)q->vui.b_signal_type_present);
        if (q->vui.b_signal_type_present) {
            vk_bs_write(s, 3, (uint32_t)q->vui.i_vidformat);
            vk_bs_write1(s, (uint32_t)q->vui.b_fullrange);
            vk_bs_write1(s, (uint32_t)q->vui.b_color_description_present);
            if (q->vui.b_color_description_present) {
                vk_bs_write(s, 8, (uint32_t)q->vui.i_colorprim);
                vk_bs_write(s, 8, (uint32_t)q->vui.i_transfer);
                vk_bs_write(s, 8, (uint32_t)q->vui.i_colmatrix);
            }
        }
        vk_bs_write1(s, (uint32_t)q->vui.b_chroma_loc_info_present);
        if (q->vui.b_chroma_loc_info_present) {
            vk_bs_write_ue(s, (uint32_t)q->vui.i_chroma_loc_top);
            vk_bs_write_ue(s, (uint32_t)q->vui.i_chroma_loc_bottom);
        }
        vk_bs_write1(s, (uint32_t)q->vui.b_timing_info_present);
        if (q->vui.b_timing_info_present) {
            vk_bs_write(s, 32, (uint32_t)q->vui.i_num_units_in_tick);
            vk_bs_write(s, 32, (uint32_t)q->vui.i_time_scale);
            vk_bs_write1(s, (uint32_t)q->vui.b_fixed_frame_rate);
        }
        vk_bs_write1(s, (uint32_t)q->vui.b_nal_hrd_parameters_present);
        vk_bs_write1(s, (uint32_t)q->vui.b_vcl_hrd_parameters_present);
        if (q->vui.b_nal_hrd_parameters_present || q->vui.b_vcl_hrd_parameters_present)
            vk_bs_write1(s, 0); /* low_delay_hrd_flag */
        vk_bs_write1(s, (uint32_t)q->vui.b_pic_struct_present);
        vk_bs_write1(s, (uint32_t)q->vui.b_bitstream_restriction);
        if (q->vui.b_bitstream_restriction) {
            vk_bs_write1(s, (uint32_t)q->vui.b_motion_vectors_over_pic_boundaries);
            vk_bs_write_ue(s, (uint32_t)q->vui.i_max_bytes_per_pic_denom);
            vk_bs_write_ue(s, (uint32_t)q->vui.i_max_bits_per_mb_denom);
            vk_bs_write_ue(s, (uint32_t)q->vui.i_log2_max_mv_length_horizontal);
            vk_bs_write_ue(s, (uint32_t)q->vui.i_log2_max_mv_length_vertical);
            vk_bs_write_ue(s, (uint32_t)q->vui.i_num_reorder_frames);
            vk_bs_write_ue(s, (uint32_t)q->vui.i_max_dec_frame_buffering);
        }
    }
    vk_bs_rbsp_trailing(s);
}

void vk_pps_init(vk_pps_t *p, int id, const vk_param_t *param, const vk_sps_t *sps)
{
    memset(p, 0, sizeof(*p));
    p->i_id = id;
    p->i_sps_id = sps->i_id;
    p->b_cabac = param->b_cabac;
    p->b_pic_order = 0;
    p->i_num_slice_groups = 1;
    p->i_num_ref_idx_l0_default_active = param->i_frame_reference;
    p->i_num_ref_idx_l1_default_active = 1;
    p->b_weighted_pred = param->b_weighted_pred;
    p->b_weighted_bipred = param->i_weighted_bipred;
    p->i_pic_init_qp = param->i_qp_constant;
    p->i_pic_init_qs = 26 + QP_BD_OFFSET;
    p->i_chroma_qp_index_offset = param->i_chroma_qp_offset;
    p->b_deblocking_filter_control = 1;
    p->b_deblocking_filter = param->b_deblocking_filter;
    p->b_constrained_intra_pred = 0;
    p->b_redundant_pic_cnt = 0;
    p->b_transform_8x8_mode = param->b_transform_8x8 ? 1 : 0;
}

void vk_pps_write(vk_bs_t *s, const vk_sps_t *sps, const vk_pps_t *p)
{
    (void)sps;
    vk_bs_align(s);
    vk_bs_write_ue(s, (uint32_t)p->i_id);
    vk_bs_write_ue(s, (uint32_t)p->i_sps_id);
    vk_bs_write1(s, (uint32_t)p->b_cabac);
    vk_bs_write1(s, (uint32_t)p->b_pic_order);
    vk_bs_write_ue(s, (uint32_t)(p->i_num_slice_groups - 1));
    vk_bs_write_ue(s, (uint32_t)(p->i_num_ref_idx_l0_default_active - 1));
    vk_bs_write_ue(s, (uint32_t)(p->i_num_ref_idx_l1_default_active - 1));
    vk_bs_write1(s, (uint32_t)p->b_weighted_pred);
    vk_bs_write(s, 2, (uint32_t)p->b_weighted_bipred);
    vk_bs_write_se(s, p->i_pic_init_qp - 26 - QP_BD_OFFSET);
    vk_bs_write_se(s, p->i_pic_init_qs - 26 - QP_BD_OFFSET);
    vk_bs_write_se(s, p->i_chroma_qp_index_offset);
    vk_bs_write1(s, (uint32_t)p->b_deblocking_filter_control);
    vk_bs_write1(s, (uint32_t)p->b_constrained_intra_pred);
    vk_bs_write1(s, (uint32_t)p->b_redundant_pic_cnt);
    /* x264 set.c: writes transform_8x8_mode followed by b_scaling_list when
     * either is set. CQM is always FLAT here, so b_scaling_list = 0. */
    if (p->b_transform_8x8_mode) {
        vk_bs_write1(s, 1);
        vk_bs_write1(s, 0);   /* b_scaling_list = 0 (CQM FLAT) */
        /* x264 writes second_chroma_qp_index_offset in this block too. */
        vk_bs_write_se(s, p->i_chroma_qp_index_offset);
    }
    (void)sps;
    vk_bs_rbsp_trailing(s);
}
