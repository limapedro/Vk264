/* Phoenix parameter defaults + presets.
 * Field-for-field port of reference/x264/common/base.c:344-602.
 * Every assignment below has a source line reference in its comment.
 */
#include "phx_param.h"
#include <string.h>
#ifdef _MSC_VER
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

/* x264 analysis flag bit values (x264.h analyse.intra/inter bitfields). */
#define PHX_ANALYSE_I4x4    1
#define PHX_ANALYSE_I8x8    2
#define PHX_ANALYSE_PSUB16x16 8
#define PHX_ANALYSE_BSUB16x16 16
#define PHX_ANALYSE_PSUB8x8   32

#define PHX_WEIGHTP_NONE   0
#define PHX_WEIGHTP_SIMPLE 1
#define PHX_WEIGHTP_SMART  2

/* x264 common/base.c:344 — medium is the unmodified default set. */
void phx_param_default(phx_param_t *p)
{
    memset(p, 0, sizeof(*p));
    p->i_csp = 0;                       /* X264_CSP_I420 */
    p->i_frame_reference = 3;           /* base.c:384 */
    p->i_keyint_max = 250;              /* base.c:385 */
    p->i_keyint_min = -1;               /* X264_KEYINT_MIN_AUTO */
    p->i_bframe = 3;                    /* base.c:387 */
    p->i_scenecut_threshold = 40;       /* base.c:388 */
    p->i_bframe_adaptive = PHX_B_ADAPT_FAST; /* base.c:389 */
    p->b_deblocking_filter = 1;         /* base.c:395 */
    p->b_cabac = 1;                     /* base.c:399 */
    p->rc_method = PHX_RC_CRF;          /* base.c:402 */
    p->f_rf_constant = 23.0;            /* base.c:409 */
    p->f_ip_factor = 1.4;               /* base.c:413 */
    p->f_pb_factor = 1.3;               /* base.c:414 */
    p->i_aq_mode = 1;                   /* X264_AQ_VARIANCE, base.c:415 */
    p->f_aq_strength = 1.0;             /* base.c:416 */
    p->i_lookahead = 40;                /* base.c:417 */
    p->b_mb_tree = 1;                   /* base.c:427 */
    p->analyse_intra = PHX_ANALYSE_I4x4 | PHX_ANALYSE_I8x8; /* base.c:435 */
    p->analyse_inter = PHX_ANALYSE_I4x4 | PHX_ANALYSE_I8x8 |
                       PHX_ANALYSE_PSUB16x16 | PHX_ANALYSE_BSUB16x16; /* base.c:436 */
    p->i_me_method = PHX_ME_HEX;        /* base.c:439 */
    p->f_psy_rd = 1.0;                  /* base.c:440 */
    p->b_psy = 1;                       /* base.c:441 */
    p->f_psy_trellis = 0.0;             /* base.c:442 */
    p->i_me_range = 16;                 /* base.c:443 */
    p->i_subpel_refine = 7;             /* base.c:444 */
    p->b_mixed_references = 1;          /* base.c:445 */
    p->b_weighted_bipred = 1;           /* base.c:451 */
    p->i_weighted_pred = PHX_WEIGHTP_SMART; /* base.c:452 */
    p->b_dct_decimate = 1;              /* base.c:453 */
    p->b_transform_8x8 = 1;             /* base.c:454 */
    p->i_trellis = 1;                   /* base.c:455 */
    p->b_fast_pskip = 1;                /* base.c:450 */
    p->i_direct_mv_pred = 1;            /* X264_DIRECT_PRED_SPATIAL, base.c:438 */
}

/* x264 common/base.c:489 — exact preset deltas, in source order. */
int phx_param_apply_preset(phx_param_t *p, const char *preset)
{
    if (!strcasecmp(preset, "ultrafast")) {                 /* base.c:496 */
        p->i_frame_reference = 1; p->i_scenecut_threshold = 0;
        p->b_deblocking_filter = 0; p->b_cabac = 0; p->i_bframe = 0;
        p->analyse_intra = 0; p->analyse_inter = 0;
        p->b_transform_8x8 = 0; p->i_me_method = PHX_ME_DIA;
        p->i_subpel_refine = 0; p->i_aq_mode = 0;
        p->b_mixed_references = 0; p->i_trellis = 0;
        p->i_bframe_adaptive = PHX_B_ADAPT_NONE; p->b_mb_tree = 0;
        p->i_weighted_pred = PHX_WEIGHTP_NONE; p->b_weighted_bipred = 0;
        p->i_lookahead = 0;
    } else if (!strcasecmp(preset, "superfast")) {           /* base.c:517 */
        p->analyse_inter = PHX_ANALYSE_I8x8 | PHX_ANALYSE_I4x4;
        p->i_me_method = PHX_ME_DIA; p->i_subpel_refine = 1;
        p->i_frame_reference = 1; p->b_mixed_references = 0;
        p->i_trellis = 0; p->b_mb_tree = 0;
        p->i_weighted_pred = PHX_WEIGHTP_SIMPLE; p->i_lookahead = 0;
    } else if (!strcasecmp(preset, "veryfast")) {            /* base.c:529 */
        p->i_subpel_refine = 2; p->i_frame_reference = 1;
        p->b_mixed_references = 0; p->i_trellis = 0;
        p->i_weighted_pred = PHX_WEIGHTP_SIMPLE; p->i_lookahead = 10;
    } else if (!strcasecmp(preset, "faster")) {              /* base.c:538 */
        p->b_mixed_references = 0; p->i_frame_reference = 2;
        p->i_subpel_refine = 4; p->i_weighted_pred = PHX_WEIGHTP_SIMPLE;
        p->i_lookahead = 20;
    } else if (!strcasecmp(preset, "fast")) {                /* base.c:546 */
        p->i_frame_reference = 2; p->i_subpel_refine = 6;
        p->i_weighted_pred = PHX_WEIGHTP_SIMPLE; p->i_lookahead = 30;
    } else if (!strcasecmp(preset, "medium")) {              /* base.c:553 */
        /* medium = defaults */
    } else if (!strcasecmp(preset, "slow")) {                /* base.c:557 */
        p->i_subpel_refine = 8; p->i_frame_reference = 5;
        p->i_trellis = 2; p->i_lookahead = 50;
        p->i_direct_mv_pred = 3;                        /* base.c:561 AUTO */
    } else if (!strcasecmp(preset, "slower")) {              /* base.c:565 */
        p->i_me_method = PHX_ME_UMH; p->i_subpel_refine = 9;
        p->i_frame_reference = 8; p->i_bframe_adaptive = PHX_B_ADAPT_TRELLIS;
        p->analyse_inter |= PHX_ANALYSE_PSUB8x8;
        p->i_trellis = 2; p->i_lookahead = 60;
        p->i_direct_mv_pred = 3;                        /* base.c:571 AUTO */
    } else if (!strcasecmp(preset, "veryslow")) {            /* base.c:576 */
        p->i_me_method = PHX_ME_UMH; p->i_subpel_refine = 10;
        p->i_me_range = 24; p->i_frame_reference = 16;
        p->i_bframe_adaptive = PHX_B_ADAPT_TRELLIS;
        p->analyse_inter |= PHX_ANALYSE_PSUB8x8;
        p->i_trellis = 2; p->i_bframe = 8; p->i_lookahead = 60;
        p->i_direct_mv_pred = 3;                        /* base.c:583 AUTO */
    } else if (!strcasecmp(preset, "placebo")) {             /* base.c:589 */
        p->i_me_method = PHX_ME_TESA; p->i_subpel_refine = 11;
        p->i_me_range = 24; p->i_frame_reference = 16;
        p->i_bframe_adaptive = PHX_B_ADAPT_TRELLIS;
        p->analyse_inter |= PHX_ANALYSE_PSUB8x8;
        p->b_fast_pskip = 0; p->i_trellis = 2;
        p->i_bframe = 16; p->i_lookahead = 60;
        p->i_direct_mv_pred = 3;                        /* base.c:596 AUTO */
    } else {
        return -1;
    }
    return 0;
}

/* x264 encoder.c:1218-1231 — psy-RD lowers the chroma QP offset at subme>=6.
 * x264's condition is (analyse.b_psy && i_subpel_refine >= 6). */
void phx_param_finalize(phx_param_t *p)
{
    p->i_chroma_qp_index_offset =
        (p->b_psy && p->i_subpel_refine >= 6) ? -2 : 0;
    if (p->i_keyint_min < 0) {
        int k = p->i_keyint_max / 10;
        int b = p->i_bframe + 1;
        p->i_keyint_min = k < b ? k : b;
        if (p->i_keyint_min < 1) p->i_keyint_min = 1;
    }
}

/* x264 common/base.c:749 — baseline/main restrictions. */
int phx_param_apply_profile(phx_param_t *p, const char *profile)
{
    if (!strcasecmp(profile, "baseline")) {
        p->b_transform_8x8 = 0; p->b_cabac = 0;
        p->i_bframe = 0; p->i_weighted_pred = PHX_WEIGHTP_NONE;
    } else if (!strcasecmp(profile, "main")) {
        p->b_transform_8x8 = 0;
    } else {
        return -1;
    }
    return 0;
}
