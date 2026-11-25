/* Phoenix slice header — mirrors reference/x264/encoder/encoder.c.
 *
 * Source: encoder.c:122-211  (slice_header_init),
 *         encoder.c:213-364  (slice_header_write),
 *         encoder.c:2613/2637/2791 (call sites),
 *         common/common.h:153-215 (x264_slice_header_t layout),
 *         common/base.h:111-116 (slice_type_e), :137 (X264_REF_MAX).
 *
 * slice_group_change_cycle is the only field not emitted by x264's writer:
 * x264 pins pps.i_num_slice_groups = 1 (encoder/set.c:486).  It is included
 * here for the H.264 syntax (7.3.3 / 7.4.3) and written only when a caller
 * supplies more than one slice group.  phx_pps_t has no slice_group_map_type,
 * so callers must use this path only for map type 6 (the only type that
 * carries slice_group_change_cycle).
 *
 * SHADER: C — strictly serial entropy-stage output; the header is emitted in
 * the same ordered stream as SPS/PPS before any macroblock data.
 */
#ifndef PHX_SLICE_H
#define PHX_SLICE_H

#include <stdint.h>
#include "phx_param.h"
#include "phx_bitstream.h"
#include "phx_headers.h"

#define PHX_SLICE_REF_MAX 16          /* common/base.h:137 (X264_REF_MAX) */

/* common/base.h:111-116 — x264 only emits P/B/I. */
#define PHX_SLICE_P 0
#define PHX_SLICE_B 1
#define PHX_SLICE_I 2
/* SP/SI exist in the H.264 syntax but are absent from x264's enum. */
#define PHX_SLICE_SP 3
#define PHX_SLICE_SI 4

/* one ref_pic_list_modification entry (encoder.c:185-192). */
typedef struct {
    int idc;                          /* 0 = short-term, 1 = long-term (:189) */
    int arg;                          /* abs(diff)-1 masked to frame_num (:190) */
} phx_ref_pic_list_t;

/* x264_weight_t subset used by pred_weight_table() (encoder.c:291-321). */
typedef struct {
    int i_denom;                      /* log2_weight_denom (:296 / :298) */
    int b_weighted;                   /* x264: weightfn != NULL (:301 / :310) */
    int i_scale;                      /* i_scale (:305 / :316) */
    int i_offset;                     /* i_offset (:306 / :317) */
} phx_slice_weight_t;

/* Explicit per-frame context.  Everything the writer needs that is not a
 * parameter-set field comes from the caller: no globals, no environment.
 * Comment numbers refer to the x264 source line that supplies the value. */
typedef struct {
    int i_frame_num;                  /* encoder.c:136 */
    int i_poc;                        /* encoder.c:145, fixed up by the caller */
    int b_idr;                        /* NAL type 5 flag (encoder.c:236) */
    int i_idr_pic_id;                 /* encoder.c:142; ignored unless b_idr */
    int i_nal_ref_idc;                /* encoder.c:328 (write() argument) */
    int i_type;                       /* PHX_SLICE_* (encoder.c:225) */
    int i_first_mb, i_last_mb;        /* encoder.c:132-133 */
    int i_qp;                         /* global QP, encoder.c:2613 i_global_qp */
    int b_variable_qp;                /* encoder.c:205 */
    int b_sliced_threads;             /* encoder.c:206 (no phx_param_t field) */
    int b_mbaff;                      /* encoder.c:138 PARAM_INTERLACED */
    int b_direct_spatial_mv_pred;     /* encoder.c:156-170 (2-pass in x264) */
    int b_ref_reorder[2];             /* encoder.c:177-178 h->b_ref_reorder */
    int i_ref[2];                     /* encoder.c:186 h->i_ref[list] */
    int ref_frame_num[2][PHX_SLICE_REF_MAX]; /* encoder.c:188, fref[][].i_frame_num */
    int i_cabac_init_idc;             /* encoder.c:196 */
    int i_deblocking_filter_alphac0;  /* encoder.c:203 */
    int i_deblocking_filter_beta;     /* encoder.c:203 */
} phx_slice_frame_t;

/* Full per-slice header state (Phoenix mirror of x264_slice_header_t). */
typedef struct {
    const phx_sps_t *sps;             /* encoder.c:129 */
    const phx_pps_t *pps;             /* encoder.c:130 */

    int i_type;                       /* encoder.c:158 / :225 */
    int i_first_mb, i_last_mb;        /* encoder.c:159-160 */
    int i_pps_id;                     /* encoder.c:162 / :226 */
    int i_frame_num;                  /* encoder.c:164 / :227 */
    int b_mbaff, b_field_pic, b_bottom_field; /* encoder.c:166-168, :229-234 */
    int i_idr_pic_id;                 /* encoder.c:170; >= 0 means IDR */
    int i_poc, i_delta_poc_bottom;    /* encoder.c:172-173, :239-244 */
    int i_redundant_pic_cnt;          /* encoder.c:176, :246-247 */
    int b_direct_spatial_mv_pred;     /* encoder.c:178, :249-250 */
    int b_num_ref_idx_override;       /* encoder.c:180, :254 */
    int i_num_ref_idx_l0_active;      /* encoder.c:181, :257 */
    int i_num_ref_idx_l1_active;      /* encoder.c:182, :259 */
    int b_ref_pic_list_reordering[2]; /* encoder.c:184, :266/:279 */
    phx_ref_pic_list_t ref_pic_list_order[2][PHX_SLICE_REF_MAX]; /* :189 */
    phx_slice_weight_t weight[PHX_SLICE_REF_MAX*2][3]; /* :193, :296-318 */
    int i_mmco_command_count;         /* encoder.c:196 (struct), :337-345 */
    int mmco_diff[PHX_SLICE_REF_MAX]; /* i_difference_of_pic_nums, 1-based (:343) */
    int i_cabac_init_idc;             /* encoder.c:203 (struct), :350-351 */
    int i_qp, i_qp_delta;             /* encoder.c:205-206, :353 */
    int i_disable_deblocking_filter_idc; /* encoder.c:211, :357 */
    int i_alpha_c0_offset, i_beta_offset; /* encoder.c:212-213, :360-361 */
    unsigned i_slice_group_change_cycle; /* H.264 7.3.3, after :363 */
    int i_nal_ref_idc;                /* carried from frame context (:328) */
} phx_slice_header_t;

/* encoder.c:122 — fill defaults from the parameter sets and per-frame context.
 * The caller may then override any field (mmco, weights, ref counts, ...)
 * before calling phx_slice_header_write(). */
void phx_slice_header_init(phx_slice_header_t *sh, const phx_param_t *param,
                           const phx_sps_t *sps, const phx_pps_t *pps,
                           const phx_slice_frame_t *frame);

/* encoder.c:213 — write the slice header RBSP (no rbsp_trailing: x264 appends
 * macroblock data first).  param is consulted for the global deblocking switch
 * folded into sh->i_disable_deblocking_filter_idc by encoder.c:203-208. */
void phx_slice_header_write(phx_bs_t *bs, const phx_param_t *param,
                            const phx_slice_header_t *sh);

/* Ceil(Log2(PicSizeInMapUnits)) bits used by slice_group_change_cycle. */
int phx_slice_group_change_bits(const phx_sps_t *sps);

#endif
