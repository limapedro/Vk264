/* Phoenix SPS/PPS — mirrors reference/x264/encoder/set.c.
 *
 * Source: set.c:101 (x264_sps_init), :305 (x264_sps_write),
 *         :479 (x264_pps_init), :505 (x264_pps_write),
 *         common/bitstream.c:55 (NAL assembly).
 *
 * SHADER: C — emitted once per stream in the serial header stage.
 */
#ifndef PHX_HEADERS_H
#define PHX_HEADERS_H

#include <stdint.h>
#include "phx_param.h"
#include "phx_bitstream.h"

typedef struct {
    int i_id, i_mb_width, i_mb_height;
    int i_profile_idc, i_level_idc;
    int b_constraint_set0, b_constraint_set1, b_constraint_set2, b_constraint_set3;
    int i_chroma_format_idc;             /* 1 = 4:2:0 */
    int i_log2_max_frame_num, i_poc_type, i_log2_max_poc_lsb;
    int i_num_ref_frames;
    int b_gaps_in_frame_num_value_allowed, b_frame_mbs_only;
    int b_mb_adaptive_frame_field, b_direct8x8_inference;
    int b_crop, crop_left, crop_right, crop_top, crop_bottom;
    int b_vui, b_qpprime_y_zero_transform_bypass;
    int vui_sar, vui_timing, vui_bitstream_restriction;
    int vui_num_units_in_tick, vui_time_scale, vui_fixed_frame_rate;
    int vui_num_reorder_frames, vui_max_dec_frame_buffering;
    int vui_log2_max_mv_length;
} phx_sps_t;

typedef struct {
    int i_id, i_sps_id, b_cabac, b_pic_order, i_num_slice_groups;
    int i_num_ref_idx_l0, i_num_ref_idx_l1;
    int b_weighted_pred, b_weighted_bipred;
    int i_pic_init_qp, i_pic_init_qs, i_chroma_qp_index_offset;
    int b_deblocking_filter_control, b_constrained_intra_pred;
    int b_redundant_pic_cnt, b_transform_8x8_mode;
} phx_pps_t;

/* common/tables.c:29-49 (x264_levels) + encoder/encoder.c:1245-1266 — auto
 * level selection for the port's fixed 25 fps timebase.  set.c:240-241 maps
 * the selected level's mv_range to log2_max_mv_length.  SHADER: C — lookup. */
int phx_sps_auto_level(int mb_width, int mb_height, int num_ref_frames);
int phx_sps_auto_mv_length(int level_idc);

/* set.c:101 / :479 — derive SPS/PPS from the ported parameter set.
 * width/height are the coded picture size; the level is auto-selected with
 * x264's rule (10 for 64x64@25, 30 for 480x480@25). */
void phx_sps_init(phx_sps_t *sps, const phx_param_t *p, int width, int height);
void phx_pps_init(phx_pps_t *pps, const phx_sps_t *sps, const phx_param_t *p);

/* set.c:305 / :505 — write the RBSP (caller supplies a zeroed bitstream). */
void phx_sps_write(phx_bs_t *bs, const phx_sps_t *sps);
void phx_pps_write(phx_bs_t *bs, const phx_sps_t *sps, const phx_pps_t *pps);

/* Emit Annex-B SPS + PPS NALs into dst; return the number of bytes. */
int phx_encoder_headers(uint8_t *dst, int capacity,
                        const phx_param_t *p, int width, int height);

#endif
