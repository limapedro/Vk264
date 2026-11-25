/* Phoenix parameter set — a field-for-field mirror of x264's preset knobs.
 * Source: reference/x264/x264.h:310 (x264_param_t) and
 *         reference/x264/common/base.c:344/489 (defaults + presets).
 *
 * SHADER: A — pure configuration data, no runtime state.
 */
#ifndef PHX_PARAM_H
#define PHX_PARAM_H

#include <stdint.h>

typedef enum {
    PHX_ME_DIA = 0, PHX_ME_HEX, PHX_ME_UMH, PHX_ME_ESA, PHX_ME_TESA
} phx_me_method_t;

typedef enum {
    PHX_B_ADAPT_NONE = 0, PHX_B_ADAPT_FAST, PHX_B_ADAPT_TRELLIS
} phx_b_adapt_t;

typedef enum {
    PHX_RC_CQP = 0, PHX_RC_CRF = 1, PHX_RC_ABR = 2
} phx_rc_method_t;

typedef struct {
    /* Video / SPS */
    int i_csp;                  /* x264.h:324   I420 by default */
    int i_width, i_height;
    int i_log2_max_frame_num;   /* derived from level (SPS) */

    /* GOP / slice type — base.c:384-391 */
    int i_frame_reference;      /* 3 */
    int i_keyint_max;           /* 250 (audits use 30) */
    int i_keyint_min;           /* AUTO -> min(keyint/10, bframes+1) */
    int i_bframe;               /* 3 */
    int i_scenecut_threshold;   /* 40 */
    phx_b_adapt_t i_bframe_adaptive; /* FAST */
    int b_open_gop;

    /* Coding tools — base.c:395-455 */
    int b_cabac;                /* 1 */
    int b_deblocking_filter;    /* 1 */
    int b_transform_8x8;        /* 1 */
    phx_me_method_t i_me_method;/* HEX */
    int i_me_range;             /* 16 */
    int i_subpel_refine;        /* 7 (medium) */
    int b_mixed_references;     /* 1 */
    int b_fast_pskip;           /* 1 */
    int i_direct_mv_pred;       /* 1 = SPATIAL, 3 = AUTO (x264.h:197-200) */
    int b_weighted_bipred;      /* 1 */
    int i_weighted_pred;        /* SMART=2 */
    int i_trellis;              /* 1 */
    int b_psy;                  /* 1 */
    double f_psy_rd;            /* 1.0 */
    double f_psy_trellis;       /* 0.0 */
    int i_chroma_qp_offset;     /* 0 (psy sets -2 for subme>=6) */
    int b_dct_decimate;         /* 1 */

    /* Analysis flags */
    int analyse_intra;          /* I4x4 | I8x8 */
    int analyse_inter;          /* I4x4 | I8x8 | PSUB16x16 | BSUB16x16 */

    /* Rate control — base.c:402-427 */
    phx_rc_method_t rc_method;  /* CRF */
    int i_qp_constant;          /* -1 */
    double f_rf_constant;       /* 23.0 */
    double f_ip_factor;         /* 1.4 */
    double f_pb_factor;         /* 1.3 */
    int i_aq_mode;              /* variance AQ */
    double f_aq_strength;       /* 1.0 */
    int i_lookahead;            /* 40 */
    int b_mb_tree;              /* 1 */

    /* Derived at runtime by phx_encode_open() (x264 encoder.c:1218-1231) */
    int i_chroma_qp_index_offset;
} phx_param_t;

/* x264 common/base.c:344 — set medium defaults. */
void phx_param_default(phx_param_t *p);

/* x264 common/base.c:489 — apply one of the ten preset deltas.
 * Returns 0 on success, -1 for an unknown preset name. */
int phx_param_apply_preset(phx_param_t *p, const char *preset);

/* x264 encoder.c:1218 — psy/chroma-QP derivation from subpel_refine. */
void phx_param_finalize(phx_param_t *p);

/* x264 base.c:749 — baseline/main profile tool restrictions. */
int phx_param_apply_profile(phx_param_t *p, const char *profile);

#endif
