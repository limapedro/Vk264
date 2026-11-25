/* Port of x264 encoder/encoder.c slice_header_init + slice_header_write.
 * Every emitted syntax element carries its source line in a trailing comment.
 */
#include "phx_slice.h"
#include "phx_tables.h"
#include <stdlib.h>
#include <string.h>

#define PHX_MIN(a,b) ((a)<(b)?(a):(b))

/* encoder.c:122 — slice_header_init().  x264 reads most values from x264_t;
 * Phoenix takes them from the explicit phx_slice_frame_t supplied by the
 * caller, so this function has no hidden state. */
void phx_slice_header_init(phx_slice_header_t *sh, const phx_param_t *param,
                           const phx_sps_t *sps, const phx_pps_t *pps,
                           const phx_slice_frame_t *frame)
{
    memset(sh, 0, sizeof(*sh));

    sh->sps = sps;                                          /* encoder.c:129 */
    sh->pps = pps;                                          /* encoder.c:130 */

    sh->i_type     = frame->i_type;                         /* :158 (set by caller in x264) */
    sh->i_first_mb = frame->i_first_mb;                     /* :132 */
    sh->i_last_mb  = frame->i_last_mb;                      /* :133 (mb_count-1) */
    sh->i_pps_id   = pps->i_id;                             /* :134 */

    sh->i_frame_num = frame->i_frame_num;                   /* :136 */

    sh->b_mbaff       = frame->b_mbaff;                     /* :138 PARAM_INTERLACED */
    sh->b_field_pic   = 0;                                  /* :139 (no field support) */
    sh->b_bottom_field = 0;                                 /* :140 (not yet used) */

    /* :142 + :236 — x264 signals IDR with i_idr_pic_id >= 0 and passes -1 for
     * non-IDR NALs.  The explicit b_idr flag selects that representation. */
    sh->i_idr_pic_id = frame->b_idr
                     ? (frame->i_idr_pic_id >= 0 ? frame->i_idr_pic_id : 0)
                     : -1;

    sh->i_poc = frame->i_poc;                               /* :145 (fixed later in x264) */
    sh->i_delta_poc_bottom = 0;                             /* :146 */

    sh->i_redundant_pic_cnt = 0;                            /* :150 */

    /* :156-170 — derived from 2-pass stats in x264; supplied explicitly here. */
    sh->b_direct_spatial_mv_pred = frame->b_direct_spatial_mv_pred;

    sh->b_num_ref_idx_override = 0;                         /* :173 */
    sh->i_num_ref_idx_l0_active = 1;                        /* :174 */
    sh->i_num_ref_idx_l1_active = 1;                        /* :175 */

    sh->b_ref_pic_list_reordering[0] = frame->b_ref_reorder[0]; /* :177 */
    sh->b_ref_pic_list_reordering[1] = frame->b_ref_reorder[1]; /* :178 */

    /* :180-194 — construct reordering from the reference frame_nums. */
    for (int list = 0; list < 2; list++) {
        if (sh->b_ref_pic_list_reordering[list]) {
            int pred_frame_num = sh->i_frame_num;           /* :185 */
            int n = PHX_MIN(frame->i_ref[list], PHX_SLICE_REF_MAX);
            for (int i = 0; i < n; i++) {
                int diff = frame->ref_frame_num[list][i] - pred_frame_num; /* :188 */
                sh->ref_pic_list_order[list][i].idc = (diff > 0);         /* :189 */
                sh->ref_pic_list_order[list][i].arg =
                    (abs(diff) - 1) & ((1 << sps->i_log2_max_frame_num) - 1); /* :190 */
                pred_frame_num = frame->ref_frame_num[list][i];           /* :191 */
            }
        }
    }

    sh->i_cabac_init_idc = frame->i_cabac_init_idc;         /* :196 */

    sh->i_qp = PHX_MIN(frame->i_qp, PHX_QP_MAX_SPEC);       /* :198 SPEC_QP */
    sh->i_qp_delta = sh->i_qp - pps->i_pic_init_qp;         /* :199 */

    /* :203-208 — deblocking decision (phx_param_t has no alpha/beta/sliced
     * threads knobs, so the frame context carries them). */
    int alpha = frame->i_deblocking_filter_alphac0;
    int beta  = frame->i_deblocking_filter_beta;
    int deblock_thresh = frame->i_qp + 2 * PHX_MIN(alpha, beta); /* :203 */
    if (param->b_deblocking_filter && (frame->b_variable_qp || 15 < deblock_thresh))
        sh->i_disable_deblocking_filter_idc = frame->b_sliced_threads ? 2 : 0; /* :205-206 */
    else
        sh->i_disable_deblocking_filter_idc = 1;            /* :208 */
    sh->i_alpha_c0_offset = alpha * 2;                      /* :209 */
    sh->i_beta_offset     = beta  * 2;                      /* :210 */

    sh->i_nal_ref_idc = frame->i_nal_ref_idc;               /* :328 */
}

/* encoder.c:213 — slice_header_write(). */
void phx_slice_header_write(phx_bs_t *bs, const phx_param_t *param,
                            const phx_slice_header_t *sh)
{
    const phx_sps_t *sps = sh->sps;                         /* common.h:155 */
    const phx_pps_t *pps = sh->pps;                         /* common.h:156 */
    if (!bs || !sps || !pps || !param)
        return;

    /* first_mb_in_slice (encoder.c:215-223). */
    if (sh->b_mbaff) {
        int first_x = sh->i_first_mb % sps->i_mb_width;     /* :217 */
        int first_y = sh->i_first_mb / sps->i_mb_width;     /* :218 */
        phx_bs_write_ue(bs, (unsigned)((2*first_x +
                        sps->i_mb_width*(first_y & ~1) + (first_y & 1)) >> 1)); /* :220 */
    } else
        phx_bs_write_ue(bs, (unsigned)sh->i_first_mb);      /* :223 */

    phx_bs_write_ue(bs, (unsigned)(sh->i_type + 5));        /* :225 */
    phx_bs_write_ue(bs, (unsigned)sh->i_pps_id);            /* :226 */
    phx_bs_write(bs, sps->i_log2_max_frame_num,
        (unsigned)sh->i_frame_num & ((1u << sps->i_log2_max_frame_num) - 1u)); /* :227 */

    if (!sps->b_frame_mbs_only) {                           /* :229 */
        phx_bs_write1(bs, (unsigned)sh->b_field_pic);       /* :231 */
        if (sh->b_field_pic)
            phx_bs_write1(bs, (unsigned)sh->b_bottom_field); /* :233 */
    }

    int b_idr = sh->i_idr_pic_id >= 0;                      /* :236 */
    if (b_idr)
        phx_bs_write_ue(bs, (unsigned)sh->i_idr_pic_id);    /* :237 */

    if (sps->i_poc_type == 0) {                             /* :239 */
        phx_bs_write(bs, sps->i_log2_max_poc_lsb,
            (unsigned)sh->i_poc & ((1u << sps->i_log2_max_poc_lsb) - 1u)); /* :241 */
        if (pps->b_pic_order && !sh->b_field_pic)
            phx_bs_write_se(bs, sh->i_delta_poc_bottom);    /* :243 */
    }
    /* poc_type 2 emits no POC fields (:239 is false). */

    if (pps->b_redundant_pic_cnt)
        phx_bs_write_ue(bs, (unsigned)sh->i_redundant_pic_cnt); /* :247 */

    if (sh->i_type == PHX_SLICE_B)
        phx_bs_write1(bs, (unsigned)sh->b_direct_spatial_mv_pred); /* :250 */

    if (sh->i_type == PHX_SLICE_P || sh->i_type == PHX_SLICE_B) { /* :252 */
        phx_bs_write1(bs, (unsigned)sh->b_num_ref_idx_override); /* :254 */
        if (sh->b_num_ref_idx_override) {
            phx_bs_write_ue(bs, (unsigned)(sh->i_num_ref_idx_l0_active - 1)); /* :257 */
            if (sh->i_type == PHX_SLICE_B)
                phx_bs_write_ue(bs, (unsigned)(sh->i_num_ref_idx_l1_active - 1)); /* :259 */
        }
    }

    /* ref_pic_list_modification, list 0 (encoder.c:263-276). */
    if (sh->i_type != PHX_SLICE_I) {
        phx_bs_write1(bs, (unsigned)sh->b_ref_pic_list_reordering[0]); /* :266 */
        if (sh->b_ref_pic_list_reordering[0]) {
            int n = PHX_MIN(sh->i_num_ref_idx_l0_active, PHX_SLICE_REF_MAX);
            for (int i = 0; i < n; i++) {
                phx_bs_write_ue(bs, (unsigned)sh->ref_pic_list_order[0][i].idc); /* :271 */
                phx_bs_write_ue(bs, (unsigned)sh->ref_pic_list_order[0][i].arg); /* :272 */
            }
            phx_bs_write_ue(bs, 3);                         /* :274 (end of list) */
        }
    }

    /* ref_pic_list_modification, list 1 (encoder.c:277-289). */
    if (sh->i_type == PHX_SLICE_B) {
        phx_bs_write1(bs, (unsigned)sh->b_ref_pic_list_reordering[1]); /* :279 */
        if (sh->b_ref_pic_list_reordering[1]) {
            int n = PHX_MIN(sh->i_num_ref_idx_l1_active, PHX_SLICE_REF_MAX);
            for (int i = 0; i < n; i++) {
                phx_bs_write_ue(bs, (unsigned)sh->ref_pic_list_order[1][i].idc); /* :284 */
                phx_bs_write_ue(bs, (unsigned)sh->ref_pic_list_order[1][i].arg); /* :285 */
            }
            phx_bs_write_ue(bs, 3);                         /* :288 */
        }
    }

    /* pred_weight_table() (encoder.c:291-322).  x264 writes explicit weights
     * for P slices only; the B-slice implicit case (:323-326) is a TODO there
     * and emits nothing. */
    if (pps->b_weighted_pred && sh->i_type == PHX_SLICE_P) { /* :292 */
        phx_bs_write_ue(bs, (unsigned)sh->weight[0][0].i_denom); /* :296 */
        if (sps->i_chroma_format_idc)
            phx_bs_write_ue(bs, (unsigned)sh->weight[0][1].i_denom); /* :298 */
        int n = PHX_MIN(sh->i_num_ref_idx_l0_active, PHX_SLICE_REF_MAX*2);
        for (int i = 0; i < n; i++) {
            int luma_flag = !!sh->weight[i][0].b_weighted;  /* :301 */
            phx_bs_write1(bs, (unsigned)luma_flag);         /* :302 */
            if (luma_flag) {
                phx_bs_write_se(bs, sh->weight[i][0].i_scale);  /* :305 */
                phx_bs_write_se(bs, sh->weight[i][0].i_offset); /* :306 */
            }
            if (sps->i_chroma_format_idc) {
                int chroma_flag = !!sh->weight[i][1].b_weighted ||
                                  !!sh->weight[i][2].b_weighted; /* :310 */
                phx_bs_write1(bs, (unsigned)chroma_flag);   /* :311 */
                if (chroma_flag) {
                    for (int j = 1; j < 3; j++) {           /* :314-318 */
                        phx_bs_write_se(bs, sh->weight[i][j].i_scale);
                        phx_bs_write_se(bs, sh->weight[i][j].i_offset);
                    }
                }
            }
        }
    }

    /* dec_ref_pic_marking() (encoder.c:328-348). */
    if (sh->i_nal_ref_idc != 0) {                           /* :328 */
        if (b_idr) {
            phx_bs_write1(bs, 0);                           /* :332 no_output_of_prior_pics */
            phx_bs_write1(bs, 0);                           /* :333 long_term_reference_flag */
        } else {
            phx_bs_write1(bs, (unsigned)(sh->i_mmco_command_count > 0)); /* :337 */
            if (sh->i_mmco_command_count > 0) {
                int n = PHX_MIN(sh->i_mmco_command_count, PHX_SLICE_REF_MAX);
                for (int i = 0; i < n; i++) {
                    phx_bs_write_ue(bs, 1);                 /* :342 mmco=1 */
                    phx_bs_write_ue(bs,
                        (unsigned)(sh->mmco_diff[i] - 1));  /* :343 */
                }
                phx_bs_write_ue(bs, 0);                     /* :345 end of commands */
            }
        }
    }

    if (pps->b_cabac && sh->i_type != PHX_SLICE_I)
        phx_bs_write_ue(bs, (unsigned)sh->i_cabac_init_idc); /* :350-351 */

    phx_bs_write_se(bs, sh->i_qp_delta);                    /* :353 */

    if (pps->b_deblocking_filter_control) {                 /* :355 */
        int disable = sh->i_disable_deblocking_filter_idc;
        /* encoder.c:203-208 folds the global switch into this field during
         * slice_header_init; re-apply it for hand-built headers. */
        if (!param->b_deblocking_filter)
            disable = 1;
        phx_bs_write_ue(bs, (unsigned)disable);             /* :357 */
        if (disable != 1) {                                 /* :358 */
            phx_bs_write_se(bs, sh->i_alpha_c0_offset >> 1); /* :360 */
            phx_bs_write_se(bs, sh->i_beta_offset >> 1);     /* :361 */
        }
    }

    /* slice_group_change_cycle (H.264 7.3.3/7.4.3, map type 6).  x264 never
     * emits it: set.c:486 forces num_slice_groups_minus1 = 0. */
    if (pps->i_num_slice_groups > 1)
        phx_bs_write(bs, phx_slice_group_change_bits(sps),
                     sh->i_slice_group_change_cycle);
}

/* H.264 7.4.3: Ceil(Log2(PicSizeInMapUnits)); for a frame
 * PicSizeInMapUnits = PicWidthInMbs * FrameHeightInMbs (x264 is frame-only). */
int phx_slice_group_change_bits(const phx_sps_t *sps)
{
    unsigned size = (unsigned)(sps->i_mb_width * sps->i_mb_height);
    int n = 0;
    while (n < 31 && (1u << n) < size)
        n++;
    return n;
}
