/* Phoenix rate control — 8-bit scalar implementation.
 *
 * Every non-trivial derivation below names the exact x264 source line it
 * follows.  Nothing is fitted to a metric; the only input substitutions are
 * documented in phx_ratecontrol.h (SATD and per-MB QP feedback are not part of
 * the stable Phoenix API, so callers may supply them explicitly).
 *
 * SHADER: C — serial per-frame state; AQ writes one independent result per MB.
 */
#include "phx_ratecontrol.h"
#include "phx_tables.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* x264 common/common.h:58-60 with BIT_DEPTH=8. */
#define PHX_RC_QP_BD_OFFSET 0
#define PHX_RC_SPEC_QP(q) ((q) > PHX_RC_QP_MAX ? PHX_RC_QP_MAX : (q))

/* encoder/ratecontrol.h:34-40 (frame-packing == 0). */
#define PHX_RC_BASE_FRAME_DURATION 0.04
#define PHX_RC_MIN_FRAME_DURATION  0.01
#define PHX_RC_MAX_FRAME_DURATION  1.00

/* x264 common/base.h:218-224 / :226-230; tables.c:58-90. */
static const uint8_t phx_rc_exp2_lut[64] = {
      0,   3,   6,   8,  11,  14,  17,  20,  23,  26,  29,  32,  36,  39,  42,  45,
     48,  52,  55,  58,  62,  65,  69,  72,  76,  80,  83,  87,  91,  94,  98, 102,
    106, 110, 114, 118, 122, 126, 130, 135, 139, 143, 147, 152, 156, 161, 165, 170,
    175, 179, 184, 189, 194, 198, 203, 208, 214, 219, 224, 229, 234, 240, 245, 250
};
static const float phx_rc_log2_lut[128] = {
    0.00000f, 0.01123f, 0.02237f, 0.03342f, 0.04439f, 0.05528f, 0.06609f, 0.07682f,
    0.08746f, 0.09803f, 0.10852f, 0.11894f, 0.12928f, 0.13955f, 0.14975f, 0.15987f,
    0.16993f, 0.17991f, 0.18982f, 0.19967f, 0.20945f, 0.21917f, 0.22882f, 0.23840f,
    0.24793f, 0.25739f, 0.26679f, 0.27612f, 0.28540f, 0.29462f, 0.30378f, 0.31288f,
    0.32193f, 0.33092f, 0.33985f, 0.34873f, 0.35755f, 0.36632f, 0.37504f, 0.38370f,
    0.39232f, 0.40088f, 0.40939f, 0.41785f, 0.42626f, 0.43463f, 0.44294f, 0.45121f,
    0.45943f, 0.46761f, 0.47573f, 0.48382f, 0.49185f, 0.49985f, 0.50779f, 0.51570f,
    0.52356f, 0.53138f, 0.53916f, 0.54689f, 0.55459f, 0.56224f, 0.56986f, 0.57743f,
    0.58496f, 0.59246f, 0.59991f, 0.60733f, 0.61471f, 0.62205f, 0.62936f, 0.63662f,
    0.64386f, 0.65105f, 0.65821f, 0.66534f, 0.67243f, 0.67948f, 0.68650f, 0.69349f,
    0.70044f, 0.70736f, 0.71425f, 0.72110f, 0.72792f, 0.73471f, 0.74147f, 0.74819f,
    0.75489f, 0.76155f, 0.76818f, 0.77479f, 0.78136f, 0.78790f, 0.79442f, 0.80090f,
    0.80735f, 0.81378f, 0.82018f, 0.82655f, 0.83289f, 0.83920f, 0.84549f, 0.85175f,
    0.85798f, 0.86419f, 0.87036f, 0.87652f, 0.88264f, 0.88874f, 0.89482f, 0.90087f,
    0.90689f, 0.91289f, 0.91886f, 0.92481f, 0.93074f, 0.93664f, 0.94251f, 0.94837f,
    0.95420f, 0.96000f, 0.96578f, 0.97154f, 0.97728f, 0.98299f, 0.98868f, 0.99435f
};
static const float phx_rc_log2_lz_lut[32] = {
    31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,
    15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0
};

static int rc_clip3i(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}
static double rc_clip3f(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}
static double rc_max2(double a, double b) { return a > b ? a : b; }

/* ratecontrol.c:203-210 with QP_BD_OFFSET=0 (common.h:58). */
static float rc_qp2qscale(float qp)
{
    return 0.85f * powf(2.0f, (qp - (12.0f + PHX_RC_QP_BD_OFFSET)) / 6.0f);
}
static float rc_qscale2qp(float qscale)
{
    return (12.0f + PHX_RC_QP_BD_OFFSET) + 6.0f * log2f(qscale / 0.85f);
}

/* base.h:218-224 — converts a QP offset into the 8.8 fixed-point inverse
 * qscale factor used by MB-tree (ratecontrol.c:325/:402). */
static int rc_exp2fix8(float x)
{
    int i = (int)(x * (-64.f / 6.f) + 512.5f);   /* base.h:220 */
    if (i < 0) return 0;                         /* base.h:221 */
    if (i > 1023) return 0xffff;                 /* base.h:222 */
    return (phx_rc_exp2_lut[i & 63] + 256) << (i >> 6) >> 8; /* base.h:223 */
}

/* base.h:226-230 — x264's clz+LUT log2 used by AQ variance mode
 * (ratecontrol.c:395). */
static int rc_clz(uint32_t x)
{
    int n = 0;
    if (!x) return 32;
    while (!(x & 0x80000000u)) { ++n; x <<= 1; }
    return n;
}
static float rc_log2_u32(uint32_t x)
{
    int lz;
    if (!x) x = 1;                               /* ratecontrol.c:395 uses max(energy,1) */
    lz = rc_clz(x);                              /* base.h:228 */
    return phx_rc_log2_lut[(x << lz >> 24) & 0x7f]
         + phx_rc_log2_lz_lut[lz < 32 ? lz : 31]; /* base.h:229 */
}

/* encoder/ratecontrol.h:38-40. */
static double rc_clip_duration(double d)
{
    return rc_clip3f(d, PHX_RC_MIN_FRAME_DURATION, PHX_RC_MAX_FRAME_DURATION);
}

struct phx_ratecontrol_t {
    phx_rc_method_t method;
    int mb_count, mb_width, mb_height;
    int mb_tree;
    int bframe;                     /* param.i_bframe (ratecontrol.c:642) */
    int lowres;                     /* h->frames.b_have_lowres */
    int keyint_max;                 /* param.i_keyint_max (ratecontrol.c:2602) */
    int weighted_pred;              /* param.analyse.i_weighted_pred */
    int i_aq_mode;                  /* param.rc.i_aq_mode */

    /* ratecontrol.c:821-822.  x264 rounds these through log2f (float);
     * the CQP constants must instead keep phx_encoder.c:473-475's double
     * log2 so that --qp remains byte-identical. */
    double ip_offset, pb_offset;    /* ratecontrol.c:821-822 */
    int ip_offset_int;              /* h->mb.ip_offset, :826 */
    double f_ip_factor, f_pb_factor;
    double f_rf_constant;
    double abr_init_qp;             /* ABR_INIT_QP, ratecontrol.c:812 */
    int qp_constant[3];             /* :823-825 */

    phx_ratecontrol_config_t cfg;
    double qcompress;
    double rate_factor_constant;    /* :644-645 */
    double accum_p_norm, accum_p_qp; /* :813-814, :1410-1420 */
    double cplxr_sum, wanted_bits_window; /* :816-817, :1918-1927 */
    double last_qscale, last_qscale_for[3]; /* :829, :836, :2634-2638 */
    double lmin[3], lmax[3], lstep; /* :828, :837-838 */
    double last_rceq;               /* :2021 */
    double short_term_cplxsum, short_term_cplxcount; /* :2560-2566 */
    int last_satd, satd_override;   /* :2559 / explicit caller input */
    int frame_count;                /* h->i_frame */
    int last_non_b_pict_type;       /* :818, :1536-1537 */
    int64_t total_bits;             /* :2406-2409 */
    double overflow;                /* :2596 */
    float qpm, qpa_rc, qpa_aq;      /* :1526-1530, :1842-1843 */
    double qp_novbv;                /* :2452, :2629 */

    float *f_qp_offset;             /* :304-403 */
    float *f_qp_offset_aq;
    uint16_t *i_inv_qscale_factor;
    float *quant_offsets;           /* optional MB-tree offsets, :319/:398 */
    uint64_t i_pixel_sum[3], i_pixel_ssd[3], i_pixel_ssd_centered[3]; /* :306-311, :406-414 */
};

/* ratecontrol.c:750-825 + :632-646.
 * SHADER: C — one-time per-encode uniform setup. */
int phx_ratecontrol_init(phx_ratecontrol_t **rc_out, const phx_param_t *p, int mb_count)
{
    phx_ratecontrol_t *rc;
    int base_qp;

    if (!rc_out || !p || mb_count <= 0) return -1;
    *rc_out = NULL;
    if (p->rc_method != PHX_RC_CQP && p->rc_method != PHX_RC_CRF &&
        p->rc_method != PHX_RC_ABR) return -1;

    rc = (phx_ratecontrol_t *)calloc(1, sizeof(*rc));
    if (!rc) return -1;

    rc->method = p->rc_method;
    rc->mb_count = mb_count;
    rc->mb_tree = p->b_mb_tree;
    rc->bframe = p->i_bframe;
    rc->keyint_max = p->i_keyint_max;
    rc->weighted_pred = p->i_weighted_pred;
    rc->i_aq_mode = p->i_aq_mode;
    /* encoder.c:1618-1624 — b_have_lowres.  One-pass CRF/ABR always have
     * lowres; CQP needs b-adapt/scenecut/weightp/MB-tree. */
    rc->lowres = (rc->method == PHX_RC_CRF || rc->method == PHX_RC_ABR ||
                  p->i_bframe_adaptive != PHX_B_ADAPT_NONE ||
                  p->i_scenecut_threshold > 0 || p->b_mb_tree ||
                  p->i_weighted_pred > 0);
    rc->f_ip_factor = p->f_ip_factor > 0 ? p->f_ip_factor : 1.0;
    rc->f_pb_factor = p->f_pb_factor > 0 ? p->f_pb_factor : 1.0;
    /* ratecontrol.c:762-766 — MB-tree forces the param pb factor to 1 before
     * :822 computes pb_offset.  The CQP constants intentionally keep the
     * phx_encoder.c:475 double-log2 mapping (orchestrator integration). */
    if (p->b_mb_tree && p->rc_method != PHX_RC_CQP) rc->f_pb_factor = 1.0;
    rc->f_rf_constant = p->f_rf_constant;
    rc->satd_override = -1;

    if (p->i_width >= 16 && p->i_height >= 16 &&
        !(p->i_width % 16) && !(p->i_height % 16)) {
        rc->mb_width = p->i_width / 16;           /* frame.h:85-86 */
        rc->mb_height = p->i_height / 16;
        if (rc->mb_width * rc->mb_height != mb_count) { phx_ratecontrol_delete(rc); return -1; }
    } else {
        /* No geometry in the parameter set: use an integral near-square grid
         * purely for MB indexing.  Real encodes always supply i_width/i_height. */
        int h = (int)sqrt((double)mb_count);
        if (h < 1) h = 1;
        while (h > 1 && mb_count % h) --h;
        rc->mb_height = h;
        rc->mb_width = mb_count / h;
    }

    rc->f_qp_offset = (float *)calloc((size_t)mb_count, sizeof(float));
    rc->f_qp_offset_aq = (float *)calloc((size_t)mb_count, sizeof(float));
    rc->i_inv_qscale_factor = (uint16_t *)calloc((size_t)mb_count, sizeof(uint16_t));
    rc->quant_offsets = (float *)calloc((size_t)mb_count, sizeof(float));
    if (!rc->f_qp_offset || !rc->f_qp_offset_aq ||
        !rc->i_inv_qscale_factor || !rc->quant_offsets) {
        phx_ratecontrol_delete(rc);
        return -1;
    }

    /* ratecontrol.c:821-826.  x264 uses log2f (float) for the RC offsets;
     * the CQP constants below keep phx_encoder.c:473-475's double log2 so
     * that the --qp mapping stays byte-identical to the encoder's. */
    rc->ip_offset = 6.0 * log2f((float)rc->f_ip_factor);   /* :821 */
    rc->pb_offset = 6.0 * log2f((float)rc->f_pb_factor);   /* :822 */
    rc->ip_offset_int = (int)(rc->ip_offset + 0.5);        /* :826 */

    base_qp = p->i_qp_constant >= 0 ? p->i_qp_constant : 26; /* phx_encoder.c:469-470, set.c:494 */
    rc->qp_constant[PHX_RC_SLICE_P] = rc_clip3i(base_qp, 0, PHX_RC_QP_MAX); /* :823 + phx_encoder.c:484 */
    rc->qp_constant[PHX_RC_SLICE_I] =
        rc_clip3i((int)(base_qp - 6.0 * log2(rc->f_ip_factor) + 0.5), 0, PHX_RC_QP_MAX); /* :824 + phx_encoder.c:473 */
    rc->qp_constant[PHX_RC_SLICE_B] =
        rc_clip3i((int)(base_qp + 6.0 * log2(rc->f_pb_factor) + 0.5), 0, PHX_RC_QP_MAX); /* :825 + phx_encoder.c:475 */

    rc->cfg.bitrate = 0.0;                        /* base.c:403 */
    rc->cfg.fps = 25.0;                           /* ratecontrol.c:760 */
    rc->cfg.duration = 1.0 / rc->cfg.fps;
    rc->cfg.qcompress = 0.6;                      /* base.c:423 */
    rc->cfg.rate_tolerance = 1.0;                 /* base.c:404 */
    rc->cfg.qp_min = 0;                           /* base.c:410 */
    rc->cfg.qp_max = PHX_RC_QP_MAX;               /* base.c:411 clipped by common.h:59 */
    rc->cfg.qp_step = 4;                          /* base.c:412 */
    rc->qcompress = rc->mb_tree ? 1.0 : rc->cfg.qcompress; /* :762-768 */
    rc->abr_init_qp = (rc->method == PHX_RC_CRF)
                    ? rc->f_rf_constant : 24.0;   /* ABR_INIT_QP, :812 */

    if (rc->method == PHX_RC_CRF) {
        double base_cplx = (double)mb_count * (rc->bframe ? 120.0 : 80.0); /* :642 */
        /* :643 uses the PARAM f_qcompress (0.6), not the forced rc->qcompress
         * (1.0 that :765 writes).  Using rc->qcompress here loses the 5.4 QP
         * MB-tree offset at rf=23. */
        double mbtree_offset = rc->mb_tree ? (1.0 - rc->cfg.qcompress) * 13.5 : 0.0; /* :643 */
        rc->rate_factor_constant =
            pow(base_cplx, 1.0 - rc->qcompress) /
            rc_qp2qscale((float)(rc->f_rf_constant + mbtree_offset)); /* :644-645 */
    }

    if (rc->method != PHX_RC_CQP) {
        rc->accum_p_norm = .01;                   /* :813 */
        rc->accum_p_qp = rc->abr_init_qp * rc->accum_p_norm; /* :814 */
        rc->cplxr_sum = .01 * pow(7.0e5, rc->qcompress) *
                        sqrt((double)mb_count);   /* :816 */
        rc->wanted_bits_window = 1.0 * rc->cfg.bitrate / rc->cfg.fps; /* :817 */
        rc->last_non_b_pict_type = PHX_RC_SLICE_I; /* :818 */
    } else {
        rc->last_non_b_pict_type = PHX_RC_SLICE_I;
    }

    rc->lstep = pow(2.0, rc->cfg.qp_step / 6.0); /* :828 */
    rc->last_qscale = rc_qp2qscale(26.0f + PHX_RC_QP_BD_OFFSET); /* :829 */
    for (int i = 0; i < 3; i++) {
        rc->last_qscale_for[i] = rc_qp2qscale((float)rc->abr_init_qp); /* :836 */
        rc->lmin[i] = rc_qp2qscale((float)rc->cfg.qp_min);            /* :837 */
        rc->lmax[i] = rc_qp2qscale((float)rc->cfg.qp_max);            /* :838 */
    }

    *rc_out = rc;
    return 0;
}

void phx_ratecontrol_delete(phx_ratecontrol_t *rc)
{
    if (!rc) return;
    free(rc->f_qp_offset);                        /* ratecontrol.c:1390-1407 */
    free(rc->f_qp_offset_aq);
    free(rc->i_inv_qscale_factor);
    free(rc->quant_offsets);
    free(rc);
}

int phx_ratecontrol_configure(phx_ratecontrol_t *rc, const phx_ratecontrol_config_t *cfg)
{
    if (!rc || !cfg || rc->frame_count > 0) return -1;
    if (!(cfg->fps > 0.0) || cfg->duration < 0.0 ||
        !(cfg->qcompress >= 0.0 && cfg->qcompress <= 1.0) ||
        !(cfg->rate_tolerance >= 0.01) ||
        cfg->qp_min < 0 || cfg->qp_min > PHX_RC_QP_MAX ||
        cfg->qp_max < cfg->qp_min || cfg->qp_max > PHX_RC_QP_MAX ||
        cfg->qp_step < 1) return -1;

    rc->cfg = *cfg;
    if (rc->cfg.duration <= 0.0) rc->cfg.duration = 1.0 / rc->cfg.fps;
    rc->qcompress = rc->mb_tree ? 1.0 : rc->cfg.qcompress; /* :762-768 */
    rc->lstep = pow(2.0, rc->cfg.qp_step / 6.0);           /* :828 */
    for (int i = 0; i < 3; i++) {
        rc->lmin[i] = rc_qp2qscale((float)rc->cfg.qp_min); /* :837 */
        rc->lmax[i] = rc_qp2qscale((float)rc->cfg.qp_max); /* :838 */
    }
    if (rc->method == PHX_RC_CRF) {
        double base_cplx = (double)rc->mb_count * (rc->bframe ? 120.0 : 80.0); /* :642 */
        /* :643 uses the PARAM f_qcompress (0.6), not the forced rc->qcompress
         * (1.0 that :765 writes).  Using rc->qcompress here loses the 5.4 QP
         * MB-tree offset at rf=23. */
        double mbtree_offset = rc->mb_tree ? (1.0 - rc->cfg.qcompress) * 13.5 : 0.0; /* :643 */
        rc->rate_factor_constant =
            pow(base_cplx, 1.0 - rc->qcompress) /
            rc_qp2qscale((float)(rc->f_rf_constant + mbtree_offset)); /* :644-645 */
    }
    if (rc->method != PHX_RC_CQP) {
        rc->cplxr_sum = .01 * pow(7.0e5, rc->qcompress) * sqrt((double)rc->mb_count); /* :816 */
        rc->wanted_bits_window = 1.0 * rc->cfg.bitrate / rc->cfg.fps; /* :817 */
    }
    return 0;
}

int phx_ratecontrol_set_complexity(phx_ratecontrol_t *rc, int satd)
{
    if (!rc || satd < 0) return -1;
    rc->satd_override = satd;                     /* ratecontrol.c:2559 input */
    return 0;
}

int phx_ratecontrol_set_quant_offsets(phx_ratecontrol_t *rc, const float *quant_offsets)
{
    if (!rc) return -1;
    if (quant_offsets)
        memcpy(rc->quant_offsets, quant_offsets, (size_t)rc->mb_count * sizeof(float));
    else
        memset(rc->quant_offsets, 0, (size_t)rc->mb_count * sizeof(float));
    return 0;
}

/* ratecontrol.c:1410-1420 — h->rc->qpm is a float; no extra 0.5 here
 * (that rounding belongs only to x264_ratecontrol_qp at :1751). */
static void rc_accum_p_qp_update(phx_ratecontrol_t *rc, float qp, int slice_type)
{
    rc->accum_p_qp *= .95;                        /* :1413 */
    rc->accum_p_norm *= .95;                      /* :1414 */
    rc->accum_p_norm += 1;                        /* :1415 */
    rc->accum_p_qp += (slice_type == PHX_RC_SLICE_I)
                    ? qp + rc->ip_offset : qp;    /* :1416-1419 */
}

/* ratecontrol.c:1431-1538.
 * SHADER: C — one scalar pass per decided frame. */
int phx_ratecontrol_start(phx_ratecontrol_t *rc, int slice_type, int frame_bits, int *qp_out)
{
    /* ratecontrol.c:2402 — rate_estimate_qscale returns float; every
     * assignment to q below therefore rounds to float before x264 stores
     * that value into its double RC fields (:2634-2638). */
    float q;
    double qp_f;

    if (!rc || !qp_out || slice_type < 0 || slice_type > 2) return -1;
    if (frame_bits < 0) frame_bits = 0;

    rc->qpa_rc = rc->qpa_aq = 0.0f;               /* :1526-1527 */
    if (rc->method == PHX_RC_CQP) {
        /* phx_encoder.c:466-486 — CQP bypasses rate_estimate_qscale (:1506);
         * keep byte-identical to the encoder's mapping including its clip. */
        int qp = rc_clip3i(rc->qp_constant[slice_type],
                           rc->cfg.qp_min, rc->cfg.qp_max); /* :1524 */
        rc->qpm = (float)qp;                      /* :1530 */
        *qp_out = (int)rc_clip3f(rc->qpm + 0.5f,
                                 rc->cfg.qp_min, rc->cfg.qp_max); /* :1751 */
        rc_accum_p_qp_update(rc, rc->qpm, slice_type);   /* :1534 */
        if (slice_type != PHX_RC_SLICE_B)
            rc->last_non_b_pict_type = slice_type;      /* :1536-1537 */
        return 0;
    }

    if (slice_type == PHX_RC_SLICE_B) {
        /* ratecontrol.c:2421-2455.  Without POC/ref-QP history the two
         * adjacent P frames collapse to the most recent P QP (:2445); BREF
         * is treated as a plain B frame (:2450). */
        double qp_p = rc_qscale2qp((float)rc->last_qscale_for[PHX_RC_SLICE_P]); /* :2430/:2445 */
        q = (float)(qp_p + rc->pb_offset);        /* :2450, float q */
        rc->qp_novbv = q;                         /* :2452 */
        q = rc_qp2qscale(q);                      /* :2453 */
        rc->last_qscale = rc->last_qscale_for[slice_type] = q; /* :2634-2635 */
    } else {
        double duration = rc->cfg.duration > 0 ? rc->cfg.duration : 1.0 / rc->cfg.fps;
        int satd = rc->satd_override >= 0 ? rc->satd_override : frame_bits;
        double qd, rate_factor, blurred;

        if (satd > 0) {                           /* :2559 last_satd */
            rc->last_satd = satd;
            rc->short_term_cplxsum *= .5;         /* :2560 */
            rc->short_term_cplxcount *= .5;       /* :2561 */
            rc->short_term_cplxsum += satd /
                (rc_clip_duration(duration) / PHX_RC_BASE_FRAME_DURATION); /* :2562 */
            rc->short_term_cplxcount += 1.0;      /* :2563 */
        }
        blurred = rc->short_term_cplxcount > 0
                ? rc->short_term_cplxsum / rc->short_term_cplxcount : 0.0; /* :2566 */

        if (rc->mb_tree)
            qd = pow(PHX_RC_BASE_FRAME_DURATION / rc_clip_duration(duration),
                     1.0 - rc->qcompress);       /* :2011 (mbtree path) */
        else
            qd = pow(blurred > 0 ? blurred : 1.0, 1.0 - rc->qcompress); /* :2014 */

        if (!isfinite(qd) || satd == 0)           /* :2016-2018 */
            q = (float)rc->last_qscale_for[slice_type];
        else {
            rc->last_rceq = qd;                   /* :2021 (double, before rate factor) */
            rate_factor = (rc->method == PHX_RC_CRF)
                        ? rc->rate_factor_constant /* :2577 */
                        : (rc->cplxr_sum > 0
                           ? rc->wanted_bits_window / rc->cplxr_sum : 0.0); /* :2581 */
            if (!(rate_factor > 0.0) || !isfinite(rate_factor))
                /* x264 requires a positive target bitrate for ABR; phx_param_t
                 * has no bitrate field, so the documented fallback is ABR_INIT_QP. */
                rate_factor = 1.0 / rc_qp2qscale((float)rc->abr_init_qp);
            qd /= rate_factor;                    /* :2022 */
            rc->last_qscale = qd;                 /* :2023 */
            q = (float)qd;                        /* :2402 float q = get_qscale(...) */
        }

        rc->overflow = 1.0;                       /* :2557 */
        if (rc->method == PHX_RC_ABR && rc->cfg.bitrate > 0.0 && rc->last_satd > 0) {
            double time_done = rc->frame_count / rc->cfg.fps; /* :2589 */
            double wanted_bits = time_done * rc->cfg.bitrate; /* :2592 */
            if (wanted_bits > 0.0) {
                double abr_buffer = 2.0 * rc->cfg.rate_tolerance * rc->cfg.bitrate; /* :2479 */
                /* :2480 — single-thread 1pass: predicted_bits is the bits of
                 * already-encoded frames only, NOT the current frame. */
                double predicted_bits = (double)rc->total_bits;
                abr_buffer *= rc_max2(1.0, sqrt(time_done)); /* :2595 */
                rc->overflow = rc_clip3f(1.0 + (predicted_bits - wanted_bits) / abr_buffer,
                                         .5, 2.0); /* :2596 */
                q = (float)(q * rc->overflow);    /* :2597 */
            }
        }

        if (slice_type == PHX_RC_SLICE_I && rc->keyint_max > 1 &&
            rc->last_non_b_pict_type != PHX_RC_SLICE_I) {
            /* :2602-2608 — force I quantizer from the running P average. */
            if (rc->accum_p_norm > 0.0) {
                q = rc_qp2qscale((float)(rc->accum_p_qp / rc->accum_p_norm)); /* :2606 */
                q = (float)(q / rc->f_ip_factor); /* :2607 */
            }
        } else if (rc->frame_count > 0) {
            if (rc->method == PHX_RC_ABR) {       /* :2611 ABR-only clipping */
                double lmin = rc->last_qscale_for[slice_type] / rc->lstep; /* :2615 */
                double lmax = rc->last_qscale_for[slice_type] * rc->lstep; /* :2616 */
                if (rc->overflow > 1.1 && rc->frame_count > 3) lmax *= rc->lstep; /* :2617-2618 */
                else if (rc->overflow < 0.9) lmin /= rc->lstep; /* :2619-2620 */
                q = (float)rc_clip3f(q, lmin, lmax); /* :2622 */
            }
        } else if (rc->method == PHX_RC_CRF && rc->qcompress != 1.0) {
            q = (float)(rc_qp2qscale((float)rc->abr_init_qp) / rc->f_ip_factor); /* :2625-2627 */
        }

        if (!(q > 0.0f) || !isfinite(q))
            q = (float)rc->last_qscale_for[slice_type];
        rc->qp_novbv = rc_qscale2qp(q);           /* :2629 */
        rc->last_qscale_for[slice_type] = rc->last_qscale = q; /* :2634-2635 */

        if (rc->frame_count == 0 && slice_type != PHX_RC_SLICE_P)
            /* :2637-2638 — q is float, the stored field is double. */
            rc->last_qscale_for[PHX_RC_SLICE_P] = (double)q * rc->f_ip_factor;
    }

    qp_f = rc_qscale2qp(q);                       /* :1499 */
    if (!isfinite(qp_f)) qp_f = rc->qp_constant[PHX_RC_SLICE_P];
    qp_f = rc_clip3f(qp_f, rc->cfg.qp_min, rc->cfg.qp_max); /* :1524 */
    rc->qpm = (float)qp_f;                        /* :1530 */
    *qp_out = (int)rc_clip3f(rc->qpm + 0.5f,
                             rc->cfg.qp_min, rc->cfg.qp_max); /* :1751 */
    rc_accum_p_qp_update(rc, rc->qpm, slice_type); /* :1534 */
    if (slice_type != PHX_RC_SLICE_B)
        rc->last_non_b_pict_type = slice_type;    /* :1536-1537 */
    return 0;
}

/* ratecontrol.c:1754-1768 — shared core.  qp is h->rc->qpm (a float), not
 * the int returned by x264_ratecontrol_qp(); feeding the int would lose the
 * fractional part that AQ is added to. */
static int rc_mb_qp(const phx_ratecontrol_t *rc, int mb_xy, float qp, int b_kept_as_ref)
{
    float offset;
    if (rc->method != PHX_RC_CQP && rc->i_aq_mode) {
        /* :1761 — MB-tree's f_qp_offset applies only to kept references. */
        offset = b_kept_as_ref ? rc->f_qp_offset[mb_xy] : rc->f_qp_offset_aq[mb_xy];
        if (qp > PHX_RC_QP_MAX)                   /* :1763-1764 */
            offset *= (float)(PHX_RC_QP_INTERNAL_MAX - qp) /
                      (PHX_RC_QP_INTERNAL_MAX - PHX_RC_QP_MAX);
        qp += offset;                             /* :1765 */
    }
    return (int)rc_clip3f(qp + 0.5f, rc->cfg.qp_min, rc->cfg.qp_max); /* :1767 */
}

/* ratecontrol.c:1828-1928.
 * SHADER: C — one scalar pass per finished frame. */
int phx_ratecontrol_end(phx_ratecontrol_t *rc, int slice_type, int bits)
{
    double denom;

    if (!rc || slice_type < 0 || slice_type > 2 || bits < 0) return -1;

    /* ratecontrol.c:1602 adds qpm * mb_width once per MB row, in float.
     * :1596 adds each MB's integer QP (AQ included) into qpa_aq.  For a
     * non-reference frame x264 uses f_qp_offset_aq instead of f_qp_offset;
     * with MB-tree disabled the two arrays are identical. */
    {
        float qpa_rc = 0.0f, qpa_aq = 0.0f;
        for (int row = 0; row < rc->mb_height; row++)
            qpa_rc += rc->qpm * (float)rc->mb_width; /* :1602 */
        for (int mb = 0; mb < rc->mb_count; mb++)
            qpa_aq += (float)rc_mb_qp(rc, mb, rc->qpm, 1); /* :1596 */
        rc->qpa_rc = qpa_rc;
        rc->qpa_aq = qpa_aq;
    }
    rc->qpa_rc /= (float)rc->mb_count;            /* :1842 */
    rc->qpa_aq /= (float)rc->mb_count;            /* :1843 */

    if (rc->method != PHX_RC_CQP) {
        denom = rc->last_rceq * (slice_type == PHX_RC_SLICE_B ? rc->f_pb_factor : 1.0);
        if (denom > 0.0 && isfinite(denom)) {
            rc->cplxr_sum += bits * rc_qp2qscale(rc->qpa_rc) / denom; /* :1918 / :1923 */
        }
        rc->cplxr_sum *= 1.0;                     /* cbr_decay, :1925 */
        rc->wanted_bits_window += rc->cfg.duration * rc->cfg.bitrate; /* :1926 */
        rc->wanted_bits_window *= 1.0;            /* cbr_decay, :1927 */
    }
    rc->total_bits += bits;                       /* :2406-2409 accumulation */
    ++rc->frame_count;                            /* x264 h->i_frame */
    return 0;
}

/* ratecontrol.c:225-257 — scalar C ac_energy_var / ac_energy_plane.
 * Returns the AC energy of one MB and accumulates plane sum/SSD (:229-233).
 * SHADER: C — one independent 16x16 reduction per MB. */
static uint32_t rc_ac_energy_mb(phx_ratecontrol_t *rc, const phx_frame_t *f,
                                int mb_x, int mb_y)
{
    uint64_t pixel_sum[3] = {0, 0, 0};
    uint64_t pixel_ssd[3] = {0, 0, 0};
    uint32_t energy = 0;

    /* Luma: pixf.var[PIXEL_16x16] with shift=8 (ratecontrol.c:256,
     * common/pixel.c:183-199). */
    const uint8_t *p = f->plane + 16 * mb_x + 16 * mb_y * f->i_stride; /* :243 */
    for (int y = 0; y < 16; y++, p += f->i_stride) {  /* pixel.c:187-195 */
        for (int x = 0; x < 16; x++) {
            uint32_t v = p[x];
            pixel_sum[0] += v;
            pixel_ssd[0] += (uint64_t)v * v;
        }
    }
    energy += (uint32_t)(pixel_ssd[0] - ((pixel_sum[0] * pixel_sum[0]) >> 8)); /* :234 */

    /* 4:2:0 chroma: deinterleave NV12 (common/mc.c:402-405), 8x8 per plane,
     * shift = 7 - CHROMA_V_SHIFT = 6 (ratecontrol.c:239/:249/:252-253). */
    if (f->plane_chroma) {   /* CHROMA_FORMAT != 0 for the I420/NV12 port */
        const uint8_t *c = f->plane_chroma + 16 * mb_x + 8 * mb_y * f->i_stride_planes[1];
        for (int y = 0; y < 8; y++, c += f->i_stride_planes[1]) {
            for (int x = 0; x < 8; x++) {
                uint32_t u = c[2 * x], v = c[2 * x + 1];
                pixel_sum[1] += u; pixel_ssd[1] += (uint64_t)u * u;
                pixel_sum[2] += v; pixel_ssd[2] += (uint64_t)v * v;
            }
        }
        energy += (uint32_t)(pixel_ssd[1] - ((pixel_sum[1] * pixel_sum[1]) >> 6)); /* :252 */
        energy += (uint32_t)(pixel_ssd[2] - ((pixel_sum[2] * pixel_sum[2]) >> 6)); /* :253 */
    }
    /* ratecontrol.c:229-233 — b_store=1 for all three planes in ac_energy_mb. */
    rc->i_pixel_sum[0] += pixel_sum[0];
    rc->i_pixel_sum[1] += pixel_sum[1];
    rc->i_pixel_sum[2] += pixel_sum[2];
    rc->i_pixel_ssd[0] += pixel_ssd[0];
    rc->i_pixel_ssd[1] += pixel_ssd[1];
    rc->i_pixel_ssd[2] += pixel_ssd[2];
    return energy;
}

/* ratecontrol.c:304-415 — x264_adaptive_quant_frame.
 * SHADER: C — first pass computes energy, second pass writes one offset/MB. */
int phx_ratecontrol_aq_frame(phx_ratecontrol_t *rc, phx_frame_t *f, const phx_param_t *p)
{
    int mode, lowres;
    float strength, avg_adj = 0.f, bias_strength = 0.f, avg_adj_pow2 = 0.f;

    if (!rc || !f || !p) return -1;
    if (f->i_width > 0 && f->i_width / 16 != rc->mb_width) return -1;
    if (f->i_height > 0 && f->i_height / 16 != rc->mb_height) return -1;

    for (int i = 0; i < 3; i++)                    /* :307-311 */
        rc->i_pixel_sum[i] = rc->i_pixel_ssd[i] = 0;

    mode = p->i_aq_mode;
    lowres = rc->lowres;

    if (mode == PHX_RC_AQ_NONE || p->f_aq_strength == 0.0) { /* :314 */
        if (mode && p->f_aq_strength == 0.0) {      /* :317 — MB-tree still needs offsets */
            for (int mb_xy = 0; mb_xy < rc->mb_count; mb_xy++) { /* :321-322 */
                rc->f_qp_offset[mb_xy] = rc->f_qp_offset_aq[mb_xy] = rc->quant_offsets[mb_xy];
                if (lowres)
                    rc->i_inv_qscale_factor[mb_xy] =
                        (uint16_t)rc_clip3i(rc_exp2fix8(rc->f_qp_offset[mb_xy]), 0, 0xffff); /* :325 */
            }
        } else {
            memset(rc->f_qp_offset, 0, (size_t)rc->mb_count * sizeof(float)); /* :329 */
            memset(rc->f_qp_offset_aq, 0, (size_t)rc->mb_count * sizeof(float)); /* :330 */
            if (lowres)
                for (int mb_xy = 0; mb_xy < rc->mb_count; mb_xy++)
                    rc->i_inv_qscale_factor[mb_xy] = 256; /* :333 */
        }
        if (p->i_weighted_pred > 0) {              /* :337-341 — variance for WP */
            for (int mb_y = 0; mb_y < rc->mb_height; mb_y++)
                for (int mb_x = 0; mb_x < rc->mb_width; mb_x++)
                    (void)rc_ac_energy_mb(rc, f, mb_x, mb_y);
        } else {
            return 0;                              /* :344 */
        }
    } else {
        if (mode == PHX_RC_AQ_AUTOVARIANCE || mode == PHX_RC_AQ_AUTOVARIANCE_BIASED) {
            /* :355-373 — BIT_DEPTH=8 => bit_depth_correction = 1/1 = 1. */
            for (int mb_y = 0; mb_y < rc->mb_height; mb_y++) {
                for (int mb_x = 0; mb_x < rc->mb_width; mb_x++) {
                    uint32_t energy = rc_ac_energy_mb(rc, f, mb_x, mb_y); /* :362 */
                    float qp_adj = powf(energy + 1.f, 0.125f);           /* :363 */
                    int idx = mb_x + mb_y * rc->mb_width;                 /* :364 (mb_stride=mb_width) */
                    rc->f_qp_offset[idx] = qp_adj;                        /* :364 */
                    avg_adj += qp_adj;                                    /* :365 */
                    avg_adj_pow2 += qp_adj * qp_adj;                      /* :366 */
                }
            }
            avg_adj /= (float)rc->mb_count;                               /* :368 */
            avg_adj_pow2 /= (float)rc->mb_count;                           /* :369 */
            strength = (float)p->f_aq_strength * avg_adj;                 /* :370 */
            avg_adj = avg_adj - 0.5f * (avg_adj_pow2 - 14.f) / avg_adj;   /* :371 */
            bias_strength = (float)p->f_aq_strength;                       /* :372 */
        } else {
            strength = (float)p->f_aq_strength * 1.0397f;                 /* :375 */
        }

        for (int mb_y = 0; mb_y < rc->mb_height; mb_y++) {
            for (int mb_x = 0; mb_x < rc->mb_width; mb_x++) {
                float qp_adj;
                int mb_xy = mb_x + mb_y * rc->mb_width;                    /* :381 */
                if (mode == PHX_RC_AQ_AUTOVARIANCE_BIASED) {
                    qp_adj = rc->f_qp_offset[mb_xy];                       /* :384 */
                    qp_adj = strength * (qp_adj - avg_adj) +
                             bias_strength * (1.f - 14.f / (qp_adj * qp_adj)); /* :385 */
                } else if (mode == PHX_RC_AQ_AUTOVARIANCE) {
                    qp_adj = rc->f_qp_offset[mb_xy];                       /* :389 */
                    qp_adj = strength * (qp_adj - avg_adj);                /* :390 */
                } else {
                    uint32_t energy = rc_ac_energy_mb(rc, f, mb_x, mb_y);  /* :394 */
                    /* :395 — x264_log2 is the clz+LUT form from base.h:226-230. */
                    qp_adj = strength * (rc_log2_u32(energy > 1 ? energy : 1) - 14.427f);
                }
                qp_adj += rc->quant_offsets[mb_xy];                        /* :397-398 */
                rc->f_qp_offset[mb_xy] = rc->f_qp_offset_aq[mb_xy] = qp_adj; /* :399-400 */
                if (lowres)
                    rc->i_inv_qscale_factor[mb_xy] =
                        (uint16_t)rc_clip3i(rc_exp2fix8(qp_adj), 0, 0xffff); /* :402 */
            }
        }
    }

    /* ratecontrol.c:406-414 — remove the mean from the plane SSDs. */
    for (int i = 0; i < 3; i++) {
        uint64_t ssd = rc->i_pixel_ssd[i];
        uint64_t sum = rc->i_pixel_sum[i];
        int width  = 16 * rc->mb_width  >> (i ? 1 : 0); /* CHROMA_H_SHIFT=1 */
        int height = 16 * rc->mb_height >> (i ? 1 : 0); /* CHROMA_V_SHIFT=1 */
        rc->i_pixel_ssd_centered[i] = ssd - (sum * sum + (uint64_t)width * height / 2) /
                                            ((uint64_t)width * height); /* :413 */
    }
    return 0;
}

/* ratecontrol.c:1754-1768 — frame_qp is x264's h->rc->qpm (float), so the
 * caller must pass the exact frame QP, not x264_ratecontrol_qp()'s int. */
int phx_ratecontrol_mb_qp(const phx_ratecontrol_t *rc, int mb_xy, float frame_qp, int b_kept_as_ref)
{
    if (!rc || mb_xy < 0 || mb_xy >= rc->mb_count) return -1;
    return rc_mb_qp(rc, mb_xy, frame_qp, b_kept_as_ref);
}

const float *phx_ratecontrol_qp_offsets(const phx_ratecontrol_t *rc)
{
    return rc ? rc->f_qp_offset : NULL;
}
const float *phx_ratecontrol_qp_offsets_aq(const phx_ratecontrol_t *rc)
{
    return rc ? rc->f_qp_offset_aq : NULL;
}
const uint16_t *phx_ratecontrol_inv_qscale(const phx_ratecontrol_t *rc)
{
    return rc ? rc->i_inv_qscale_factor : NULL;
}
int phx_ratecontrol_qp_constant(const phx_ratecontrol_t *rc, int slice_type)
{
    if (!rc || slice_type < 0 || slice_type > 2) return -1;
    return rc->qp_constant[slice_type];
}

/* common/common.h:58-64 + common/macroblock.h:249 + encoder/set.c:1585 —
 * same lookup as phx_cost.c:106-110 / phx_chroma.c:224-231. */
int phx_ratecontrol_chroma_qp(const phx_param_t *p, int qp)
{
    int spec_qp, idx;
    if (!p) return -1;
    spec_qp = PHX_RC_SPEC_QP(qp);                                        /* common.h:64 */
    idx = rc_clip3i(PHX_CHROMA_QP_BASE_OFFSET + p->i_chroma_qp_index_offset + spec_qp,
                    0, (int)sizeof(phx_chroma_qp_base) - 1);             /* encoder.c:1585 */
    return phx_chroma_qp_base[idx] + (qp > PHX_RC_QP_MAX ? qp - PHX_RC_QP_MAX : 0); /* analyse.c:257 */
}
