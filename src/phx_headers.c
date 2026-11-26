/* Port of x264 encoder/set.c SPS/PPS init + write, with source lines. */
#include "phx_headers.h"
#include "phx_tables.h"
#include <math.h>
#include <string.h>

#define PHX_MIN(a,b) ((a)<(b)?(a):(b))
#define PHX_MAX(a,b) ((a)>(b)?(a):(b))

/* common/tables.c:29-49 — the fields x264's auto-level validation uses:
 * level_idc, MaxMBPS, MaxFS, MaxMVRange.  SHADER: C — pure lookup. */
/* columns: level_idc, MaxMBPS, MaxFS, MaxDPB (mbs), MaxMVRange — tables.c:31-49. */
static const struct {
    int level_idc, max_mbps, max_fs, max_dpb, max_mv_range;
} phx_levels[] = {
    { 10,    1485,    99,   396,  64 }, {  9,    1485,    99,   396, 128 },
    { 11,    3000,   396,   900, 128 }, { 12,    6000,   396,  2376, 128 },
    { 13,   11880,   396,  2376, 128 }, { 20,   11880,   396,  2376, 128 },
    { 21,   19800,   792,  4752, 256 }, { 22,   20250,  1620,  8100, 256 },
    { 30,   40500,  1620,  8100, 256 }, { 31,  108000,  3600, 18000, 512 },
    { 32,  216000,  5120, 20480, 512 }, { 40,  245760,  8192, 32768, 512 },
    { 41,  245760,  8192, 32768, 512 }, { 42,  522240,  8704, 34816, 512 },
    { 50,  589824, 22080,110400, 512 }, { 51,  983040, 36864,184320, 512 },
    { 52, 2073600, 36864,184320, 512 }
};

/* encoder/encoder.c:1245-1252 — x264 walks x264_levels from the first entry
 * until x264_validate_levels() passes.  With no VBV, the binding constraints
 * are MaxMBPS >= mb_width*mb_height*fps and MaxFS >= frame size (25 fps in
 * this port's VUI, phx_headers.c:63-67). */
int phx_sps_auto_level(int mb_width, int mb_height, int num_ref_frames)
{
    int fs = mb_width * mb_height;                       /* tables.c:31 MaxFS */
    int mbps = fs * 25;                                  /* tables.c:30 MaxMBPS */
    int dpb = fs * num_ref_frames;                       /* x264.h x264_level_t.dpb */
    for (size_t i = 0; i < sizeof(phx_levels)/sizeof(phx_levels[0]); i++)
        if (mbps <= phx_levels[i].max_mbps &&
            fs <= phx_levels[i].max_fs &&
            dpb <= phx_levels[i].max_dpb)
            return phx_levels[i].level_idc;
    return 52;
}

/* encoder.c:1265-1266 (i_mv_range = level's MaxMVRange) + encoder/set.c:
 * 240-241 (log2(Max(1, mv_range*4-1)) + 1). */
int phx_sps_auto_mv_length(int level_idc)
{
    int mv = 0;
    for (size_t i = 0; i < sizeof(phx_levels)/sizeof(phx_levels[0]); i++)
        if (phx_levels[i].level_idc == level_idc) {
            mv = phx_levels[i].max_mv_range;
            break;
        }
    int v = mv * 4 - 1;                                  /* set.c:241 */
    if (v < 1) v = 1;
    int n = 0;
    while ((1 << n) <= v) n++;                           /* floor(log2(v))+1 */
    return n;                                            /* set.c:241 */
}

/* set.c:101 — x264_sps_init(), restricted to the configurations the Phoenix
 * fixtures use: 8-bit 4:2:0, progressive, flat CQM, no AVC-Intra, no HRD,
 * timebase 1/25 (the parity harness passes --fps 25). */
void phx_sps_init(phx_sps_t *s, const phx_param_t *p, int width, int height)
{
    memset(s, 0, sizeof(*s));
    s->i_id = 0;                                        /* set.c:105 */
    s->i_mb_width = (width+15)/16;                      /* :106 */
    s->i_mb_height = (height+15)/16;                    /* :107 */
    s->b_frame_mbs_only = 1;                            /* :108 */
    s->i_chroma_format_idc = 1;                         /* CHROMA_420, :111-113 */
    s->b_qpprime_y_zero_transform_bypass = 0;           /* :115 (fixed QP != 0) */
    /* :117-127 — profile selection. High if 8x8dct, Main if CABAC/B-frames/
     * weighted prediction, else Baseline. */
    if (s->b_qpprime_y_zero_transform_bypass)
        s->i_profile_idc = 244;
    else if (p->b_transform_8x8)
        s->i_profile_idc = 100;
    else if (p->b_cabac || p->i_bframe > 0 || p->i_weighted_pred > 0)
        s->i_profile_idc = 77;
    else
        s->i_profile_idc = 66;
    s->b_constraint_set0 = s->i_profile_idc == 66;      /* :129 */
    s->b_constraint_set1 = s->i_profile_idc <= 77;      /* :132 */
    /* x264's default b_pyramid is 2 (normal) whenever B-frames are enabled. */
    int pyramid = p->i_bframe ? 2 : 0;
    int reorder = pyramid ? 2 : (p->i_bframe ? 1 : 0);  /* :147 */
    int refs = PHX_MAX(PHX_MAX(p->i_frame_reference, 1+reorder),
                       pyramid ? 4 : 1);               /* :151 X264_MAX4 */
    s->i_num_ref_frames = refs;                         /* :151 */
    /* encoder.c:1245-1252 — auto level from MBPS, frame size AND DPB size.
     * x264's MaxDPB constraint (x264_level_t.dpb) forces level 21 for 16 refs
     * at 320x180, while MBPS/FS alone would select level 12. */
    s->i_level_idc = phx_sps_auto_level(s->i_mb_width, s->i_mb_height, refs);
    s->vui_num_reorder_frames = reorder;                /* :147 */
    s->vui_max_dec_frame_buffering = refs;              /* :150 */

    int max_frame_num = refs*(pyramid ? 2 : 1) + 1;     /* :161 */
    s->i_log2_max_frame_num = 4;                        /* :169 */
    while ((1 << s->i_log2_max_frame_num) <= max_frame_num)
        s->i_log2_max_frame_num++;                      /* :170-171 */

    s->i_poc_type = p->i_bframe ? 0 : 2;                /* :173 */
    if (s->i_poc_type == 0) {
        int max_delta_poc = (p->i_bframe + 2) * (pyramid ? 2 : 1) * 2; /* :176 */
        s->i_log2_max_poc_lsb = 4;                      /* :177 */
        while ((1 << s->i_log2_max_poc_lsb) <= max_delta_poc*2)
            s->i_log2_max_poc_lsb++;                    /* :178-179 */
    }
    s->b_vui = 1;                                       /* :182 */
    s->b_gaps_in_frame_num_value_allowed = 0;           /* :184 */
    s->b_mb_adaptive_frame_field = 0;                   /* :185 */
    s->b_direct8x8_inference = 1;                       /* :186 */
    s->b_crop = 0;                                      /* set by CLI crop, not used here */

    /* :219-226 — VUI timing for --fps 25. */
    s->vui_timing = 1;
    s->vui_num_units_in_tick = 1;
    s->vui_time_scale = 25*2;
    s->vui_fixed_frame_rate = 1;
    s->vui_sar = 0;   /* no SAR in the parity CLI: aspect_info_present = 0 */
    s->vui_bitstream_restriction = 1;                   /* :234 */
    /* set.c:240-241 — mv length derives from the auto-selected level's
     * mv_range (level 1.0 -> 8, level 3.0 -> 10), not a fixed 8. */
    s->vui_log2_max_mv_length = phx_sps_auto_mv_length(s->i_level_idc);
}

/* set.c:479 — x264_pps_init(). */
void phx_pps_init(phx_pps_t *p, const phx_sps_t *s, const phx_param_t *param)
{
    memset(p, 0, sizeof(*p));
    p->i_id = 0; p->i_sps_id = s->i_id;                 /* :481-482 */
    p->b_cabac = param->b_cabac;                        /* :483 */
    p->b_pic_order = 0;                                 /* :485 (progressive) */
    p->i_num_slice_groups = 1;                          /* :486 */
    p->i_num_ref_idx_l0 = param->i_frame_reference;     /* :488 */
    p->i_num_ref_idx_l1 = 1;                            /* :489 */
    p->b_weighted_pred = param->i_weighted_pred > 0;    /* :491 */
    p->b_weighted_bipred = param->b_weighted_bipred ? 2 : 0; /* :492 */
    p->i_pic_init_qp = param->i_qp_constant >= 0
                     ? PHX_MIN(param->i_qp_constant, PHX_QP_MAX_SPEC) : 26; /* :494 */
    p->i_pic_init_qs = 26;                              /* :495 */
    p->i_chroma_qp_index_offset = param->i_chroma_qp_index_offset; /* :497 */
    p->b_deblocking_filter_control = 1;                 /* :498 */
    p->b_constrained_intra_pred = 0;                    /* :499 */
    p->b_redundant_pic_cnt = 0;                         /* :500 */
    p->b_transform_8x8_mode = param->b_transform_8x8 ? 1 : 0; /* :502 */
}

/* set.c:305 — x264_sps_write(). */
void phx_sps_write(phx_bs_t *b, const phx_sps_t *s)
{
    phx_bs_write(b, 8, (unsigned)s->i_profile_idc);     /* :308 */
    phx_bs_write1(b, s->b_constraint_set0);             /* :309 */
    phx_bs_write1(b, s->b_constraint_set1);             /* :310 */
    phx_bs_write1(b, s->b_constraint_set2);             /* :311 */
    phx_bs_write1(b, s->b_constraint_set3);             /* :312 */
    phx_bs_write(b, 4, 0);                              /* :314 */
    phx_bs_write(b, 8, (unsigned)s->i_level_idc);       /* :316 */
    phx_bs_write_ue(b, (unsigned)s->i_id);              /* :318 */
    if (s->i_profile_idc >= 100) {                      /* :320 */
        phx_bs_write_ue(b, (unsigned)s->i_chroma_format_idc);
        phx_bs_write_ue(b, 0);                          /* bit_depth_luma_minus8 */
        phx_bs_write_ue(b, 0);                          /* bit_depth_chroma_minus8 */
        phx_bs_write1(b, s->b_qpprime_y_zero_transform_bypass);
        phx_bs_write1(b, 0);                            /* seq_scaling_matrix_present */
    }
    phx_bs_write_ue(b, (unsigned)(s->i_log2_max_frame_num - 4)); /* :350 */
    phx_bs_write_ue(b, (unsigned)s->i_poc_type);        /* :351 */
    if (s->i_poc_type == 0)
        phx_bs_write_ue(b, (unsigned)(s->i_log2_max_poc_lsb - 4)); /* :353 */
    phx_bs_write_ue(b, (unsigned)s->i_num_ref_frames);  /* :354 */
    phx_bs_write1(b, s->b_gaps_in_frame_num_value_allowed); /* :355 */
    phx_bs_write_ue(b, (unsigned)(s->i_mb_width - 1));  /* :356 */
    phx_bs_write_ue(b, (unsigned)(s->i_mb_height - 1)); /* :357 */
    phx_bs_write1(b, s->b_frame_mbs_only);              /* :358 */
    if (!s->b_frame_mbs_only)
        phx_bs_write1(b, s->b_mb_adaptive_frame_field); /* :359-360 */
    phx_bs_write1(b, s->b_direct8x8_inference);         /* :361 */
    phx_bs_write1(b, s->b_crop);                        /* :363 */
    if (s->b_crop) {
        /* set.c:366-371 — v_shift = CHROMA_420 + !frame_mbs_only == 1 here.
         * Writing crop_top/bottom verbatim produced 12 where x264 writes 6. */
        phx_bs_write_ue(b, (unsigned)s->crop_left >> 1);
        phx_bs_write_ue(b, (unsigned)s->crop_right >> 1);
        phx_bs_write_ue(b, (unsigned)s->crop_top >> 1);
        phx_bs_write_ue(b, (unsigned)s->crop_bottom >> 1);
    }
    phx_bs_write1(b, s->b_vui);                         /* :374 */
    if (s->b_vui) {
        phx_bs_write1(b, s->vui_sar ? 1 : 0);           /* aspect_ratio_info_present :377 */
        if (s->vui_sar) phx_bs_write(b, 8, (unsigned)s->vui_sar); /* :397 */
        phx_bs_write1(b, 0);                            /* overscan_info_present :405 */
        phx_bs_write1(b, 0);                            /* signal_type_present :409 */
        phx_bs_write1(b, 0);                            /* chroma_loc_info_present :423 */
        phx_bs_write1(b, s->vui_timing);                /* :430 */
        if (s->vui_timing) {
            phx_bs_write(b, 32, (uint32_t)s->vui_num_units_in_tick); /* :433 */
            phx_bs_write(b, 32, (uint32_t)s->vui_time_scale);        /* :434 */
            phx_bs_write1(b, s->vui_fixed_frame_rate);               /* :435 */
        }
        phx_bs_write1(b, 0);                            /* nal_hrd :438 */
        phx_bs_write1(b, 0);                            /* vcl_hrd :456 */
        phx_bs_write1(b, 0);                            /* pic_struct :461 */
        phx_bs_write1(b, s->vui_bitstream_restriction);  /* :462 */
        if (s->vui_bitstream_restriction) {
            phx_bs_write1(b, 1);                        /* mv_over_boundaries :465 */
            phx_bs_write_ue(b, 0);                      /* max_bytes_pic_denom */
            phx_bs_write_ue(b, 0);                      /* max_bits_mb_denom */
            phx_bs_write_ue(b, (unsigned)s->vui_log2_max_mv_length);
            phx_bs_write_ue(b, (unsigned)s->vui_log2_max_mv_length);
            phx_bs_write_ue(b, (unsigned)s->vui_num_reorder_frames);
            phx_bs_write_ue(b, (unsigned)s->vui_max_dec_frame_buffering);
        }
    }
    phx_bs_rbsp_trailing(b);                            /* :475 */
}

/* set.c:505 — x264_pps_write(). */
void phx_pps_write(phx_bs_t *b, const phx_sps_t *s, const phx_pps_t *p)
{
    (void)s;
    phx_bs_write_ue(b, (unsigned)p->i_id);              /* :508 */
    phx_bs_write_ue(b, (unsigned)p->i_sps_id);          /* :509 */
    phx_bs_write1(b, p->b_cabac);                       /* :511 */
    phx_bs_write1(b, p->b_pic_order);                   /* :512 */
    phx_bs_write_ue(b, (unsigned)(p->i_num_slice_groups-1)); /* :513 */
    phx_bs_write_ue(b, (unsigned)(p->i_num_ref_idx_l0-1));   /* :515 */
    phx_bs_write_ue(b, (unsigned)(p->i_num_ref_idx_l1-1));   /* :516 */
    phx_bs_write1(b, p->b_weighted_pred);               /* :517 */
    phx_bs_write(b, 2, (unsigned)p->b_weighted_bipred); /* :518 */
    phx_bs_write_se(b, p->i_pic_init_qp - 26);          /* :520 */
    phx_bs_write_se(b, p->i_pic_init_qs - 26);          /* :521 */
    phx_bs_write_se(b, p->i_chroma_qp_index_offset);    /* :522 */
    phx_bs_write1(b, p->b_deblocking_filter_control);   /* :524 */
    phx_bs_write1(b, p->b_constrained_intra_pred);      /* :525 */
    phx_bs_write1(b, p->b_redundant_pic_cnt);           /* :526 */
    if (p->b_transform_8x8_mode) {                      /* :529 */
        phx_bs_write1(b, 1);                            /* transform_8x8_mode_flag */
        phx_bs_write1(b, 0);                            /* pic_scaling_matrix_present (flat CQM) */
        /* set.c:567 — x264 rewrites chroma_qp_index_offset inside the
         * transform_8x8/scaling-list block (after the scaling lists, which
         * are absent for X264_CQM_FLAT).  Omitting this shifts the RBSP stop
         * bit by one and changes the final byte of every PPS that enables
         * 8x8dct. */
        phx_bs_write_se(b, p->i_chroma_qp_index_offset); /* set.c:567 */
    }
    phx_bs_rbsp_trailing(b);                            /* set.c:570 */
}

/* Assemble the SPS/PPS NALs exactly as x264_encoder_headers does. */
int phx_encoder_headers(uint8_t *dst, int capacity,
                        const phx_param_t *p, int width, int height)
{
    phx_sps_t sps; phx_pps_t pps;
    phx_sps_init(&sps, p, width, height);
    phx_pps_init(&pps, &sps, p);

    uint8_t rbsp[128];
    phx_bs_t bs;
    phx_bs_init(&bs, rbsp, sizeof(rbsp));
    phx_sps_write(&bs, &sps);
    phx_nal_t nal = {rbsp, phx_bs_pos(&bs)/8 + (phx_bs_pos(&bs)%8 ? 1 : 0), 3, 7, 1};
    phx_nal_encode(dst, &nal, 1);
    int n = nal.i_payload;

    phx_bs_init(&bs, rbsp, sizeof(rbsp));
    phx_pps_write(&bs, &sps, &pps);
    nal.p_payload = rbsp;
    nal.i_payload = phx_bs_pos(&bs)/8 + (phx_bs_pos(&bs)%8 ? 1 : 0);
    nal.i_ref_idc = 3; nal.i_type = 8; nal.b_long_startcode = 1;
    phx_nal_encode(dst+n, &nal, 1);
    n += nal.i_payload;
    return n <= capacity ? n : -1;
}
