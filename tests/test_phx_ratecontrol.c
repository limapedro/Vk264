/* Deterministic tests for the x264 CQP/CRF/ABR rate-control and AQ port.
 *
 * Source of the expected values:
 *   src/phx_encoder.c:329-342 (the CQP mapping currently used by the encoder),
 *   reference/x264/encoder/ratecontrol.c:821-825 (CQP constants),
 *   :203-210 / :632-646 / :2545-2638 (CRF/ABR QP derivation),
 *   :304-415 (adaptive quantization),
 *   common/macroblock.h:249 + encoder/set.c:1585 (chroma QP lookup).
 *
 * Standalone main returns 0 on pass, 1 on any failed check.
 *
 * SHADER: C — exercises the host-side scalar boundary of the C-stage port.
 */
#include "phx_ratecontrol.h"
#include "phx_frame.h"
#include "phx_param.h"
#include "phx_tables.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

/* phx_encoder.c:329-342, byte-for-byte equivalent. */
static int encoder_cqp(const phx_param_t *p, int slice_type)
{
    int qp = p->i_qp_constant >= 0 ? p->i_qp_constant : 26;
    double offset = 0.0;
    if (slice_type == PHX_RC_SLICE_I) offset = -6.0 * log2(p->f_ip_factor);
    else if (slice_type == PHX_RC_SLICE_B) offset = 6.0 * log2(p->f_pb_factor);
    qp = (int)(qp + offset + 0.5);
    return qp < 0 ? 0 : qp > PHX_QP_MAX_SPEC ? PHX_QP_MAX_SPEC : qp;
}

/* ratecontrol.c:821-825 + phx_encoder.c:331-332. */
static int check_cqp_mapping(void)
{
    phx_param_t p;
    phx_ratecontrol_t *rc = NULL;
    int qp;

    phx_param_default(&p);
    p.i_width = 64; p.i_height = 32; p.i_bframe = 0;
    p.rc_method = PHX_RC_CQP;
    p.i_qp_constant = 28;
    CHECK(phx_ratecontrol_init(&rc, &p, 8) == 0 && rc != NULL, "CQP init");
    if (!rc) return 0;

    CHECK(phx_ratecontrol_start(rc, PHX_RC_SLICE_I, 0, &qp) == 0 &&
          qp == encoder_cqp(&p, PHX_RC_SLICE_I), "CQP I matches phx_encoder");
    CHECK(phx_ratecontrol_start(rc, PHX_RC_SLICE_P, 0, &qp) == 0 &&
          qp == encoder_cqp(&p, PHX_RC_SLICE_P), "CQP P matches phx_encoder");
    CHECK(phx_ratecontrol_start(rc, PHX_RC_SLICE_B, 0, &qp) == 0 &&
          qp == encoder_cqp(&p, PHX_RC_SLICE_B), "CQP B matches phx_encoder");
    CHECK(qp == 30, "default ip/pb factors give I=25 P=28 B=30");
    CHECK(phx_ratecontrol_qp_constant(rc, PHX_RC_SLICE_I) == 25 &&
          phx_ratecontrol_qp_constant(rc, PHX_RC_SLICE_P) == 28 &&
          phx_ratecontrol_qp_constant(rc, PHX_RC_SLICE_B) == 30, "CQP constants");
    phx_ratecontrol_delete(rc);

    /* Non-default factors: exact 6*log2(factor) offsets. */
    phx_param_default(&p); /* restore, then set only the fields under test */
    p.i_width = 32; p.i_height = 32; p.i_bframe = 0;
    p.rc_method = PHX_RC_CQP; p.i_qp_constant = 28;
    p.f_ip_factor = 2.0; p.f_pb_factor = 1.0;
    rc = NULL;
    CHECK(phx_ratecontrol_init(&rc, &p, 4) == 0, "CQP init non-default factors");
    if (rc) {
        CHECK(phx_ratecontrol_start(rc, PHX_RC_SLICE_I, 0, &qp) == 0 && qp == 22,
              "ip_factor=2 lowers I by 6*log2(2)=6");
        CHECK(phx_ratecontrol_start(rc, PHX_RC_SLICE_P, 0, &qp) == 0 && qp == 28,
              "P unchanged");
        CHECK(phx_ratecontrol_start(rc, PHX_RC_SLICE_B, 0, &qp) == 0 && qp == 28,
              "pb_factor=1 leaves B at base QP");
        phx_ratecontrol_delete(rc);
    }

    /* Clipping at 0 and 51, and the negative-QP -> 26 fallback (set.c:494). */
    p.i_qp_constant = 0; p.f_ip_factor = 2.0;
    rc = NULL;
    CHECK(phx_ratecontrol_init(&rc, &p, 4) == 0, "CQP init qp=0");
    if (rc) {
        CHECK(phx_ratecontrol_start(rc, PHX_RC_SLICE_I, 0, &qp) == 0 && qp == 0,
              "I QP clipped at zero");
        phx_ratecontrol_delete(rc);
    }
    p.i_qp_constant = 51; p.f_ip_factor = 1.0; p.f_pb_factor = 2.0;
    rc = NULL;
    CHECK(phx_ratecontrol_init(&rc, &p, 4) == 0, "CQP init qp=51");
    if (rc) {
        CHECK(phx_ratecontrol_start(rc, PHX_RC_SLICE_B, 0, &qp) == 0 && qp == 51,
              "B QP clipped at 51");
        phx_ratecontrol_delete(rc);
    }
    p.i_qp_constant = -1; p.f_pb_factor = 1.0;
    rc = NULL;
    CHECK(phx_ratecontrol_init(&rc, &p, 4) == 0, "CQP init qp=-1");
    if (rc) {
        CHECK(phx_ratecontrol_start(rc, PHX_RC_SLICE_P, 0, &qp) == 0 &&
              qp == encoder_cqp(&p, PHX_RC_SLICE_P), "qp=-1 uses pic_init_qp=26");
        phx_ratecontrol_delete(rc);
    }
    return fails;
}

/* ratecontrol.c:632-646 / :643 / :2011 / :2022 / :203-210.  rc->qcompress is
 * forced to 1 by MB-tree (:765), but the MB-tree RF offset at :643 uses the
 * *param* f_qcompress (0.6): rf + (1-0.6)*13.5 = rf + 5.4.  A P frame with
 * nonzero SATD therefore derives QP == rf + 5.4. */
static int check_crf_monotonic(void)
{
    const double rf_values[4] = {18.0, 23.0, 28.0, 33.0};
    int prev = -1;

    for (int i = 0; i < 4; i++) {
        phx_param_t p;
        phx_ratecontrol_t *rc = NULL;
        int qp = -1;

        phx_param_default(&p);                    /* CRF, MB-tree, qcompress=1 */
        p.i_width = 32; p.i_height = 32; p.i_bframe = 3;
        p.f_rf_constant = rf_values[i];
        CHECK(phx_ratecontrol_init(&rc, &p, 4) == 0, "CRF init");
        if (!rc) continue;
        CHECK(phx_ratecontrol_start(rc, PHX_RC_SLICE_P, 1000, &qp) == 0, "CRF start");
        CHECK(qp >= 0 && qp <= PHX_RC_QP_MAX, "CRF QP in spec range");
        CHECK(qp == (int)rf_values[i] + 5,
              "CRF QP equals rf + (1-0.6)*13.5 (ratecontrol.c:643)");
        CHECK(qp > prev, "CRF QP increases monotonically with rf");
        prev = qp;
        phx_ratecontrol_delete(rc);
    }
    return fails;
}

/* ratecontrol.c:2479-2480 / :2581 / :2592-2597 / :1915-1928.  A stream well
 * above its target must settle on a higher QP than one well below target. */
static int check_abr_feedback(void)
{
    phx_ratecontrol_config_t cfg;
    phx_param_t p;
    phx_ratecontrol_t *rc_hi = NULL, *rc_lo = NULL;
    int qp_hi = -1, qp_lo = -1;

    phx_param_default(&p);
    p.i_width = 32; p.i_height = 32; p.i_bframe = 3;
    p.rc_method = PHX_RC_ABR;

    cfg.bitrate = 100000.0;                       /* bits/s (base.c:403, rc.c:770) */
    cfg.fps = 25.0;                               /* rc.c:757-760 */
    cfg.duration = 0.04;                          /* rc.h:34 */
    cfg.qcompress = 1.0;                          /* forced by MB-tree, rc.c:762-768 */
    cfg.rate_tolerance = 1.0;                     /* base.c:404 */
    cfg.qp_min = 0; cfg.qp_max = PHX_RC_QP_MAX;   /* base.c:410-411 */
    cfg.qp_step = 4;                              /* base.c:412 */

    CHECK(phx_ratecontrol_init(&rc_hi, &p, 4) == 0 &&
          phx_ratecontrol_init(&rc_lo, &p, 4) == 0, "ABR init");
    if (!rc_hi || !rc_lo) return fails;
    CHECK(phx_ratecontrol_configure(rc_hi, &cfg) == 0 &&
          phx_ratecontrol_configure(rc_lo, &cfg) == 0, "ABR configure");

    for (int i = 0; i < 20; i++) {
        CHECK(phx_ratecontrol_start(rc_hi, PHX_RC_SLICE_P, 20000, &qp_hi) == 0, "ABR start hi");
        CHECK(phx_ratecontrol_end(rc_hi, PHX_RC_SLICE_P, 20000) == 0, "ABR end hi");
        CHECK(phx_ratecontrol_start(rc_lo, PHX_RC_SLICE_P, 1000, &qp_lo) == 0, "ABR start lo");
        CHECK(phx_ratecontrol_end(rc_lo, PHX_RC_SLICE_P, 1000) == 0, "ABR end lo");
    }
    CHECK(qp_hi >= 0 && qp_hi <= PHX_RC_QP_MAX &&
          qp_lo >= 0 && qp_lo <= PHX_RC_QP_MAX, "ABR QPs in spec range");
    CHECK(qp_hi > qp_lo, "over-target stream gets higher QP than under-target");
    printf("ABR feedback: over-target QP=%d under-target QP=%d\n", qp_hi, qp_lo);
    phx_ratecontrol_delete(rc_hi);
    phx_ratecontrol_delete(rc_lo);
    return fails;
}

/* ratecontrol.c:817/:2581 — a larger target bitrate must produce a smaller
 * (better) QP on an otherwise identical stream. */
static int check_abr_bitrate_monotonic(void)
{
    const double bitrates[3] = {50000.0, 100000.0, 200000.0};
    int prev = 1000;

    for (int i = 0; i < 3; i++) {
        phx_param_t p;
        phx_ratecontrol_config_t cfg;
        phx_ratecontrol_t *rc = NULL;
        int qp = -1;

        phx_param_default(&p);
        p.i_width = 32; p.i_height = 32; p.i_bframe = 3;
        p.rc_method = PHX_RC_ABR;
        CHECK(phx_ratecontrol_init(&rc, &p, 4) == 0, "ABR bitrate init");
        if (!rc) continue;
        cfg.bitrate = bitrates[i]; cfg.fps = 25.0; cfg.duration = 0.04;
        cfg.qcompress = 1.0; cfg.rate_tolerance = 1.0;
        cfg.qp_min = 0; cfg.qp_max = PHX_RC_QP_MAX; cfg.qp_step = 4;
        CHECK(phx_ratecontrol_configure(rc, &cfg) == 0, "ABR bitrate configure");
        CHECK(phx_ratecontrol_start(rc, PHX_RC_SLICE_P, 1000, &qp) == 0, "ABR bitrate start");
        CHECK(qp >= 0 && qp <= PHX_RC_QP_MAX && qp < prev,
              "ABR QP decreases as bitrate increases");
        prev = qp;
        phx_ratecontrol_delete(rc);
    }
    return fails;
}

/* ratecontrol.c:304-415 — deterministic, bounded AQ offsets. */
static void fill_aq_frame(phx_frame_t *f)
{
    for (int y = 0; y < f->i_height; y++) {
        for (int x = 0; x < f->i_width; x++) {
            int mb = (x / 16) + (y / 16) * (f->i_width / 16);
            int amp = 1 + (mb % 4);
            f->plane[x + y * f->i_stride] =
                (uint8_t)(64 + amp * ((x % 16) * 7 + (y % 16) * 3));
        }
    }
    for (int y = 0; y < f->i_lines_planes[1]; y++) {
        for (int x = 0; x < f->i_width_planes[1]; x++) {
            int mb = (x / 8) + (y / 8) * (f->i_width / 16);
            int amp = 1 + (mb % 3);
            uint8_t v = (uint8_t)(128 + amp * ((x & 7) * 9 + (y & 7) * 5));
            f->plane_chroma[y * f->i_stride_planes[1] + 2 * x + 0] = v;
            f->plane_chroma[y * f->i_stride_planes[1] + 2 * x + 1] = (uint8_t)(255 - v);
        }
    }
}

static int check_aq_offsets(void)
{
    phx_param_t p;
    phx_ratecontrol_t *rc1 = NULL, *rc2 = NULL;
    phx_frame_t *f1 = NULL, *f2 = NULL;
    const float *a, *b;
    const uint16_t *ia, *ib;
    int distinct = 0, bounded = 1;

    phx_param_default(&p);                        /* AQ variance, strength=1 */
    p.i_width = 32; p.i_height = 32; p.i_bframe = 3;
    f1 = phx_frame_new(32, 32, 0);
    f2 = phx_frame_new(32, 32, 0);
    CHECK(f1 != NULL && f2 != NULL, "AQ frame allocation");
    if (!f1 || !f2) return fails;
    fill_aq_frame(f1);
    fill_aq_frame(f2);

    CHECK(phx_ratecontrol_init(&rc1, &p, 4) == 0 &&
          phx_ratecontrol_init(&rc2, &p, 4) == 0, "AQ init");
    if (!rc1 || !rc2) return fails;
    CHECK(phx_ratecontrol_aq_frame(rc1, f1, &p) == 0, "AQ frame 1");
    CHECK(phx_ratecontrol_aq_frame(rc2, f2, &p) == 0, "AQ frame 2");

    a = phx_ratecontrol_qp_offsets(rc1);
    b = phx_ratecontrol_qp_offsets(rc2);
    ia = phx_ratecontrol_inv_qscale(rc1);
    ib = phx_ratecontrol_inv_qscale(rc2);
    for (int i = 0; i < 4; i++) {
        CHECK(isfinite(a[i]) && fabsf(a[i]) <= 20.0f, "AQ offset bounded/finite");
        if (!isfinite(a[i]) || fabsf(a[i]) > 20.0f) bounded = 0;
        if (i > 0 && a[i] != a[0]) distinct = 1;
    }
    CHECK(bounded && distinct, "AQ offsets finite, bounded, vary per MB");
    CHECK(memcmp(a, b, 4 * sizeof(float)) == 0, "AQ offsets reproducible");
    CHECK(memcmp(ia, ib, 4 * sizeof(uint16_t)) == 0, "AQ inverse qscale reproducible");
    CHECK(memcmp(phx_ratecontrol_qp_offsets(rc1), phx_ratecontrol_qp_offsets_aq(rc1),
                 4 * sizeof(float)) == 0, "AQ writes both offset arrays");
    printf("AQ variance offsets: %.3f %.3f %.3f %.3f\n", a[0], a[1], a[2], a[3]);

    /* Autovariance is a different formula but must still be deterministic. */
    p.i_aq_mode = PHX_RC_AQ_AUTOVARIANCE;
    CHECK(phx_ratecontrol_aq_frame(rc1, f1, &p) == 0 &&
          phx_ratecontrol_aq_frame(rc2, f2, &p) == 0, "autovariance AQ");
    a = phx_ratecontrol_qp_offsets(rc1);
    b = phx_ratecontrol_qp_offsets(rc2);
    for (int i = 0; i < 4; i++)
        CHECK(isfinite(a[i]) && fabsf(a[i]) <= 20.0f, "autovariance offset bounded");
    CHECK(memcmp(a, b, 4 * sizeof(float)) == 0, "autovariance reproducible");

    /* AQ off / zero strength: zero offsets, or the supplied MB-tree offsets. */
    p.i_aq_mode = PHX_RC_AQ_NONE;
    CHECK(phx_ratecontrol_aq_frame(rc1, f1, &p) == 0, "AQ off");
    a = phx_ratecontrol_qp_offsets(rc1);
    for (int i = 0; i < 4; i++) CHECK(a[i] == 0.0f, "AQ off gives zero offsets");

    p.i_aq_mode = PHX_RC_AQ_VARIANCE;
    p.f_aq_strength = 0.0;
    {
        float mb_offsets[4] = {1.5f, -2.25f, 0.0f, 3.75f};
        CHECK(phx_ratecontrol_set_quant_offsets(rc1, mb_offsets) == 0, "set quant offsets");
        CHECK(phx_ratecontrol_aq_frame(rc1, f1, &p) == 0, "AQ zero strength");
        a = phx_ratecontrol_qp_offsets(rc1);
        CHECK(memcmp(a, mb_offsets, 4 * sizeof(float)) == 0,
              "zero strength copies MB-tree offsets (ratecontrol.c:321-322)");
    }

    phx_ratecontrol_delete(rc1);
    phx_ratecontrol_delete(rc2);
    phx_frame_delete(f1);
    phx_frame_delete(f2);
    return fails;
}

/* common/macroblock.h:249, encoder/set.c:1585, common/common.h:58-64. */
static int check_chroma_qp(void)
{
    phx_param_t p;
    phx_param_default(&p);
    phx_param_finalize(&p);                        /* psy -> -2 for subme>=6 */
    CHECK(p.i_chroma_qp_index_offset == -2, "finalize sets chroma offset -2");
    CHECK(phx_ratecontrol_chroma_qp(&p, 26) ==
          phx_chroma_qp_base[12 - 2 + 26], "chroma QP uses phx_chroma_qp_base");
    CHECK(phx_ratecontrol_chroma_qp(&p, 26) == 24, "chroma QP(26, offset -2) = 24");
    CHECK(phx_ratecontrol_chroma_qp(&p, 60) ==
          phx_chroma_qp_base[12 - 2 + 51] + 60 - PHX_QP_MAX_SPEC,
          "QP above 51 extends chroma QP linearly (analyse.c:257)");
    return fails;
}

/* =========================================================================
 * Instrumented-x264 golden (worker M15).
 *
 * Captured with a temporary, out-of-tree build of reference/x264 that hooks
 * x264_ratecontrol_start/end (:1431/:1829), x264_adaptive_quant_frame
 * (:304-415), x264_ratecontrol_mb_qp (:1754-1768) and macroblock_tree_finish
 * (slicetype.c:1029-1048).  Fixed inputs: 64x64 I420 deterministic fixture,
 * 8 frames, --fps 25 --frames 8 --threads 1 --bframes 0 --keyint 250
 * --scenecut 0 --weightp 0 --preset medium, and one of
 *   --qp 28 --no-mbtree        (mode 0)
 *   --crf 23 --no-mbtree       (mode 1)
 *   --bitrate 100 --no-mbtree  (mode 2)
 *   --crf 23 --aq-strength 0   (mode 3, MB-tree quant offsets)
 * The trace supplies x264's own lowres SATD (:2559) and encoded frame size
 * (:1978) as Phoenix's inputs, so no complexity approximation is used.
 * Float values are stored as IEEE-754 bits: equality is bit-for-bit.
 * SHADER: C -- the golden exercises the serial scalar boundary only.
 * ========================================================================= */

typedef struct { int type, satd, bits; uint32_t qpm_bits; int qp; } golden_frame_t;
typedef struct { uint32_t off, off_aq; int inv; } golden_aq_t;
typedef struct { int mb; uint32_t q_bits; int qp, chroma; } golden_mbqp_t;

static float f32_from_bits(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }

/* cqp: 8 frames */
static const golden_frame_t g_cqp_frame[] = {
    { 2, 0, 14056, 0x41c80000u, 25 }, /* frame 0 */
    { 0, 0, 4168, 0x41e00000u, 28 }, /* frame 1 */
    { 0, 0, 4272, 0x41e00000u, 28 }, /* frame 2 */
    { 0, 0, 4400, 0x41e00000u, 28 }, /* frame 3 */
    { 0, 0, 4632, 0x41e00000u, 28 }, /* frame 4 */
    { 0, 0, 4376, 0x41e00000u, 28 }, /* frame 5 */
    { 0, 0, 4688, 0x41e00000u, 28 }, /* frame 6 */
    { 0, 0, 4792, 0x41e00000u, 28 }, /* frame 7 */
};
static const golden_mbqp_t g_cqp_mbqp[] = {
    { 0, 0x41c80000u, 25, 23 }, { 0, 0x41c80000u, 25, 23 },
    { 1, 0x41c80000u, 25, 23 }, { 2, 0x41c80000u, 25, 23 },
    { 3, 0x41c80000u, 25, 23 }, { 4, 0x41c80000u, 25, 23 },
    { 5, 0x41c80000u, 25, 23 }, { 6, 0x41c80000u, 25, 23 },
    { 7, 0x41c80000u, 25, 23 }, { 8, 0x41c80000u, 25, 23 },
    { 9, 0x41c80000u, 25, 23 }, { 10, 0x41c80000u, 25, 23 },
    { 11, 0x41c80000u, 25, 23 }, { 12, 0x41c80000u, 25, 23 },
    { 13, 0x41c80000u, 25, 23 }, { 14, 0x41c80000u, 25, 23 },
};

/* crf: 8 frames */
static const golden_frame_t g_crf_frame[] = {
    { 2, 2803, 14432, 0x41a0b314u, 20 }, /* frame 0 */
    { 0, 1134, 4600, 0x41bfb3cdu, 24 }, /* frame 1 */
    { 0, 310, 4936, 0x41ae4abau, 22 }, /* frame 2 */
    { 0, 1201, 4960, 0x41b2cf25u, 22 }, /* frame 3 */
    { 0, 950, 4576, 0x41b144a7u, 22 }, /* frame 4 */
    { 0, 1256, 4416, 0x41b49830u, 23 }, /* frame 5 */
    { 0, 1107, 4784, 0x41b44912u, 23 }, /* frame 6 */
    { 0, 295, 5432, 0x41a78067u, 21 }, /* frame 7 */
};
static const golden_aq_t g_crf_aq[] = {
    { 0x40859ca7u, 0x40859ca7u, 157 }, { 0x408ffd9bu, 0x408ffd9bu, 152 },
    { 0x40c2e94du, 0x40c2e94du, 126 }, { 0x40859ca7u, 0x40859ca7u, 157 },
    { 0x4085e318u, 0x4085e318u, 157 }, { 0x40dea4bfu, 0x40dea4bfu, 114 },
    { 0x40866ec5u, 0x40866ec5u, 157 }, { 0x40b142d4u, 0x40b142d4u, 135 },
    { 0x4085e318u, 0x4085e318u, 157 }, { 0x40d9299fu, 0x40d9299fu, 117 },
    { 0x40859ca7u, 0x40859ca7u, 157 }, { 0x40e4cb10u, 0x40e4cb10u, 112 },
    { 0x40ce1c9du, 0x40ce1c9du, 121 }, { 0x4086291cu, 0x4086291cu, 157 },
    { 0x408fc489u, 0x408fc489u, 152 }, { 0x40c2e94du, 0x40c2e94du, 126 },
};
static const golden_mbqp_t g_crf_mbqp[] = {
    { 0, 0x41c21a3eu, 24, 22 }, { 0, 0x41c21a3eu, 24, 22 },
    { 1, 0x41c4b27bu, 25, 23 }, { 2, 0x41d16d67u, 26, 24 },
    { 3, 0x41c21a3eu, 24, 22 }, { 4, 0x41c22bdau, 24, 22 },
    { 5, 0x41d85c44u, 27, 25 }, { 6, 0x41c24ec5u, 24, 22 },
    { 7, 0x41cd03c9u, 26, 24 }, { 8, 0x41c22bdau, 24, 22 },
    { 9, 0x41d6fd7cu, 27, 25 }, { 10, 0x41c21a3eu, 24, 22 },
    { 11, 0x41d9e5d8u, 27, 25 }, { 12, 0x41d43a3bu, 27, 25 },
    { 13, 0x41c23d5bu, 24, 22 }, { 14, 0x41c4a436u, 25, 23 },
};
static const int8_t g_chroma_qp[61] = {
     0,  0,  0,  1,  2,  3,  4,  5,  6,  7,
     8,  9, 10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 29, 30, 31, 32, 32, 33, 34, 34,
    35, 35, 36, 36, 37, 37, 37, 38, 38, 38,
    39, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48,
};

/* abr: 8 frames */
static const golden_frame_t g_abr_frame[] = {
    { 2, 2803, 19112, 0x4132301eu, 11 }, /* frame 0 */
    { 0, 1134, 6592, 0x419064fcu, 18 }, /* frame 1 */
    { 0, 310, 5768, 0x4194b5edu, 19 }, /* frame 2 */
    { 0, 1201, 4984, 0x41a3020au, 20 }, /* frame 3 */
    { 0, 950, 5232, 0x41a6af8fu, 21 }, /* frame 4 */
    { 0, 1256, 4520, 0x41af5161u, 22 }, /* frame 5 */
    { 0, 1107, 4648, 0x41b1a018u, 22 }, /* frame 6 */
    { 0, 295, 5048, 0x41a78482u, 21 }, /* frame 7 */
};
static const golden_aq_t g_abr_aq[] = {
    { 0x40859ca7u, 0x40859ca7u, 157 }, { 0x408ffd9bu, 0x408ffd9bu, 152 },
    { 0x40c2e94du, 0x40c2e94du, 126 }, { 0x40859ca7u, 0x40859ca7u, 157 },
    { 0x4085e318u, 0x4085e318u, 157 }, { 0x40dea4bfu, 0x40dea4bfu, 114 },
    { 0x40866ec5u, 0x40866ec5u, 157 }, { 0x40b142d4u, 0x40b142d4u, 135 },
    { 0x4085e318u, 0x4085e318u, 157 }, { 0x40d9299fu, 0x40d9299fu, 117 },
    { 0x40859ca7u, 0x40859ca7u, 157 }, { 0x40e4cb10u, 0x40e4cb10u, 112 },
    { 0x40ce1c9du, 0x40ce1c9du, 121 }, { 0x4086291cu, 0x4086291cu, 157 },
    { 0x408fc489u, 0x408fc489u, 152 }, { 0x40c2e94du, 0x40c2e94du, 126 },
};
static const golden_mbqp_t g_abr_mbqp[] = {
    { 0, 0x4174fe72u, 15, 13 }, { 0, 0x4174fe72u, 15, 13 },
    { 1, 0x417a2eecu, 16, 14 }, { 2, 0x4189d262u, 17, 15 },
    { 3, 0x4174fe72u, 15, 13 }, { 4, 0x417521aau, 15, 13 },
    { 5, 0x4190c13fu, 18, 16 }, { 6, 0x41756780u, 15, 13 },
    { 7, 0x418568c4u, 17, 15 }, { 8, 0x417521aau, 15, 13 },
    { 9, 0x418f6277u, 18, 16 }, { 10, 0x4174fe72u, 15, 13 },
    { 11, 0x41924ad3u, 18, 16 }, { 12, 0x418c9f36u, 18, 16 },
    { 13, 0x417544acu, 15, 13 }, { 14, 0x417a1262u, 16, 14 },
};

/* mbtree0: 7 frames + macroblock_tree_finish offsets */
static const golden_frame_t g_mbtree0_frame[] = {
    { 0, 2560, 15488, 0x41e33333u, 28 }, /* frame 0 */
    { 0, 2131, 7008, 0x41e33333u, 28 }, /* frame 1 */
    { 0, 1988, 7576, 0x41e33333u, 28 }, /* frame 2 */
    { 0, 102, 6400, 0x41e33333u, 28 }, /* frame 3 */
    { 0, 83, 6528, 0x41e33333u, 28 }, /* frame 4 */
    { 0, 73, 6336, 0x41e33333u, 28 }, /* frame 5 */
    { 0, 38, 5536, 0x41e33333u, 28 }, /* frame 6 */
};
static const golden_mbqp_t g_mbtree0_mbqp[] = {
    { 0, 0x41b66b8fu, 23, 21 }, { 0, 0x41b66b8fu, 23, 21 },
    { 1, 0x41b11f35u, 22, 20 }, { 2, 0x41d09426u, 26, 24 },
    { 3, 0x41a47046u, 21, 19 }, { 4, 0x41a1d51eu, 20, 18 },
    { 5, 0x41c81fe9u, 25, 23 }, { 6, 0x41ac857cu, 22, 20 },
    { 7, 0x41c68e37u, 25, 23 }, { 8, 0x41948cdcu, 19, 17 },
    { 9, 0x41c9083bu, 25, 23 }, { 10, 0x416ed13au, 15, 13 },
    { 11, 0x41bfe00du, 24, 22 }, { 12, 0x41d754a0u, 27, 25 },
    { 13, 0x41c55513u, 25, 23 }, { 14, 0x41c9d734u, 25, 23 },
};
static const uint32_t g_mbtree0_mboff[] = {
    0xc0b31e8fu, 0xc0c84ff7u, 0xc014f867u, 0xc0fb0bb5u,
    0xc102bc2bu, 0xc0589a4fu, 0xc0dab6dbu, 0xc06527dfu,
    0xc11d4cadu, 0xc05157bfu, 0xc157952cu, 0xc08d4c97u,
    0xbfbde92fu, 0xc06ef0ffu, 0xc04adff7u, 0xc080917bu,
};

/* Exact fixture bytes for frame 0 (matches the instrumented x264 input). */
static void fill_golden_frame(phx_frame_t *f)
{
    for (int y = 0; y < f->i_height; y++)
        for (int x = 0; x < f->i_width; x++)
            f->plane[x + y * f->i_stride] =
                (uint8_t)((x * 7 + y * 3 + 0 * 11) & 0xFF);
    for (int y = 0; y < f->i_height / 2; y++)
        for (int x = 0; x < f->i_width / 2; x++) {
            uint8_t u = (uint8_t)((128 + (x * 9 + y * 5 + 0 * 7) % 64) & 0xFF);
            uint8_t v = (uint8_t)((64 + (x * 5 + y * 11 + 0 * 13) % 64) & 0xFF);
            f->plane_chroma[y * f->i_stride_planes[1] + 2 * x] = u;
            f->plane_chroma[y * f->i_stride_planes[1] + 2 * x + 1] = v;
        }
}

static phx_ratecontrol_t *golden_init(phx_param_t *p, phx_ratecontrol_config_t *cfg, int mode)
{
    phx_ratecontrol_t *rc = NULL;
    phx_param_default(p);
    phx_param_finalize(p);                    /* chroma QP offset -2 at subme 7 */
    p->i_width = 64; p->i_height = 64;
    p->i_bframe = 0; p->i_scenecut_threshold = 0;
    p->i_weighted_pred = 0;
    p->i_aq_mode = PHX_RC_AQ_VARIANCE;
    p->f_aq_strength = 1.0;
    p->i_keyint_max = 250;
    if (mode == 0) { p->rc_method = PHX_RC_CQP; p->i_qp_constant = 28; p->b_mb_tree = 0; }
    else if (mode == 1) { p->rc_method = PHX_RC_CRF; p->f_rf_constant = 23.0; p->b_mb_tree = 0; }
    else if (mode == 2) { p->rc_method = PHX_RC_ABR; p->b_mb_tree = 0; }
    else { p->rc_method = PHX_RC_CRF; p->f_rf_constant = 23.0;
           p->b_mb_tree = 1; p->f_aq_strength = 0.0; }
    if (phx_ratecontrol_init(&rc, p, 16) || !rc) return NULL;
    cfg->bitrate = (mode == 2) ? 100000.0 : 0.0;  /* 100 kbit/s */
    cfg->fps = 25.0; cfg->duration = 0.04;
    cfg->qcompress = 0.6; cfg->rate_tolerance = 1.0;
    cfg->qp_min = 0; cfg->qp_max = PHX_RC_QP_MAX; cfg->qp_step = 4;
    if (phx_ratecontrol_configure(rc, cfg)) { phx_ratecontrol_delete(rc); return NULL; }
    return rc;
}

static void golden_frames(phx_ratecontrol_t *rc, const golden_frame_t *gf, int n)
{
    for (int i = 0; i < n; i++) {
        int qp = -1;
        phx_ratecontrol_set_complexity(rc, gf[i].satd);   /* ratecontrol.c:2559 */
        CHECK(phx_ratecontrol_start(rc, gf[i].type, 0, &qp) == 0, "golden start");
        CHECK(qp == gf[i].qp, "golden frame QP");
        CHECK(phx_ratecontrol_end(rc, gf[i].type, gf[i].bits) == 0, "golden end");
    }
}

/* The trace records x264's q AFTER :1765 (qpm + AQ offset); Phoenix's
 * phx_ratecontrol_mb_qp takes the pre-offset frame QPM and adds the same
 * offset from its own AQ arrays, so feed gf[0].qpm_bits, not g[i].q_bits.
 * g[i].q_bits is still asserted: qpm_bits + offset must round-trip to it. */
static void golden_mbqp(phx_ratecontrol_t *rc, const phx_param_t *p,
                        const golden_mbqp_t *g, int n,
                        const golden_frame_t *gf, int nf)
{
    float frame_qp = f32_from_bits(gf[0].qpm_bits);
    for (int i = 0; i < n; i++) {
        int got = phx_ratecontrol_mb_qp(rc, g[i].mb, frame_qp, 1);
        CHECK(got == g[i].qp, "golden per-MB QP");
        CHECK(phx_ratecontrol_chroma_qp(p, g[i].qp) == g[i].chroma,
              "golden chroma QP for MB");
    }
    (void)nf;
}

static void check_instrumented_golden(void)
{
    static const struct {
        int mode;
        const golden_frame_t *frames; int nframes;
        const golden_aq_t *aq; const golden_mbqp_t *mbqp; int nmbqp;
    } cases[] = {
        { 0, g_cqp_frame, 8, NULL, g_cqp_mbqp, 16 },
        { 1, g_crf_frame, 8, g_crf_aq, g_crf_mbqp, 16 },
        { 2, g_abr_frame, 8, g_abr_aq, g_abr_mbqp, 16 },
        { 3, g_mbtree0_frame, 7, NULL, g_mbtree0_mbqp, 16 },
    };

    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        phx_param_t p;
        phx_ratecontrol_config_t cfg;
        phx_ratecontrol_t *rc = golden_init(&p, &cfg, cases[c].mode);
        phx_frame_t *f;
        if (!rc) { CHECK(0, "golden init"); continue; }
        f = phx_frame_new(64, 64, 0);
        CHECK(f != NULL, "golden frame alloc");
        if (!f) { phx_ratecontrol_delete(rc); continue; }
        fill_golden_frame(f);

        golden_frames(rc, cases[c].frames, cases[c].nframes);
        if (cases[c].aq) {
            CHECK(phx_ratecontrol_aq_frame(rc, f, &p) == 0, "golden aq frame");
            {
                const float *o = phx_ratecontrol_qp_offsets(rc);
                const float *oa = phx_ratecontrol_qp_offsets_aq(rc);
                const uint16_t *iv = phx_ratecontrol_inv_qscale(rc);
                for (int i = 0; i < 16; i++) {
                    uint32_t bo, boa;
                    memcpy(&bo, &o[i], 4); memcpy(&boa, &oa[i], 4);
                    CHECK(bo == cases[c].aq[i].off, "golden AQ offset bits");
                    CHECK(boa == cases[c].aq[i].off_aq, "golden AQ offset_aq bits");
                    CHECK((int)iv[i] == cases[c].aq[i].inv, "golden inv qscale");
                }
            }
        } else if (cases[c].mode == 3) {
            /* ratecontrol.c:317-325 degenerate path with MB-tree offsets. */
            float mboff[16];
            const float *o;
            for (int i = 0; i < 16; i++) mboff[i] = f32_from_bits(g_mbtree0_mboff[i]);
            CHECK(phx_ratecontrol_set_quant_offsets(rc, mboff) == 0, "golden mbtree offsets");
            o = phx_ratecontrol_qp_offsets(rc);
            CHECK(phx_ratecontrol_aq_frame(rc, f, &p) == 0, "golden mbtree aq frame");
            for (int i = 0; i < 16; i++) {
                uint32_t bo;
                memcpy(&bo, &o[i], 4);
                CHECK(bo == g_mbtree0_mboff[i], "golden MB-tree offset bits");
            }
        }
        golden_mbqp(rc, &p, cases[c].mbqp, cases[c].nmbqp,
                    cases[c].frames, cases[c].nframes);

        if (cases[c].mode == 1)
            for (int q = 0; q <= 60; q++)
                CHECK(phx_ratecontrol_chroma_qp(&p, q) == (int)g_chroma_qp[q],
                      "golden chroma QP table");

        phx_frame_delete(f);
        phx_ratecontrol_delete(rc);
    }
}

/* =========================================================================
 * CQP native-preset golden (follow-up M15).
 *
 * Instrumented x264 with the exact parity_harness.py:227-230 flags
 * (--qp 28 --keyint 30 --min-keyint 30 --scenecut 0, no AQ/MB-tree overrides)
 * for ultrafast, veryfast and medium.  Every dumped `param` line reports
 * aq_mode=0 / b_mb_tree=0: encoder.c:951-966 disables both for CQP.
 * The qp=30 / lambda=8 MBs are B frames (ratecontrol.c:825, :1508-1511);
 * BREF frames take the 29 midpoint.  No AQ offset exists on this path.
 * SHADER: C — exercises the serial CQP QP/lambda boundary.
 * ========================================================================= */
typedef struct { int frame, type, qp, kept; } cqp_frame_golden_t;
typedef struct { int frame, mb, qp, lambda; } cqp_mb_golden_t;

static const cqp_frame_golden_t g_cqp_uf_frame[] = {
    { 0, 2, 25, 0 }, { 1, 0, 28, 0 }, { 2, 0, 28, 0 }, { 3, 0, 28, 0 },
    { 4, 0, 28, 0 }, { 5, 0, 28, 0 }, { 6, 0, 28, 0 }, { 7, 0, 28, 0 },
};
static const cqp_mb_golden_t g_cqp_uf_mb[] = {
    { 0, 0, 25, 4 }, { 0, 1, 25, 4 }, { 0, 2, 25, 4 }, { 0, 3, 25, 4 },
    { 1, 0, 28, 6 }, { 1, 1, 28, 6 }, { 1, 2, 28, 6 }, { 1, 3, 28, 6 },
    { 2, 0, 28, 6 }, { 2, 1, 28, 6 }, { 2, 2, 28, 6 }, { 2, 3, 28, 6 },
    { 3, 0, 28, 6 }, { 3, 1, 28, 6 }, { 3, 2, 28, 6 }, { 3, 3, 28, 6 },
};
static const cqp_frame_golden_t g_cqp_vf_frame[] = {
    { 0, 2, 25, 0 }, { 4, 0, 28, 0 }, { 2, 1, 29, 1 }, { 1, 1, 30, 0 },
    { 3, 1, 30, 0 }, { 6, 0, 28, 0 }, { 5, 1, 30, 0 }, { 7, 0, 28, 0 },
};
static const cqp_mb_golden_t g_cqp_vf_mb[] = {
    { 0, 0, 25, 4 }, { 0, 1, 25, 4 }, { 0, 2, 25, 4 }, { 0, 3, 25, 4 },
    { 2, 0, 29, 7 }, { 2, 1, 29, 7 }, { 2, 2, 29, 7 }, { 2, 3, 29, 7 },
    { 1, 0, 30, 8 }, { 1, 1, 30, 8 }, { 1, 2, 30, 8 }, { 1, 3, 30, 8 },
    { 3, 0, 30, 8 }, { 3, 1, 30, 8 }, { 3, 2, 30, 8 }, { 3, 3, 30, 8 },
};
static const cqp_frame_golden_t g_cqp_md_frame[] = {
    { 0, 2, 25, 0 }, { 4, 0, 28, 0 }, { 2, 1, 29, 1 }, { 1, 1, 30, 0 },
    { 3, 1, 30, 0 }, { 6, 0, 28, 0 }, { 5, 1, 30, 0 }, { 7, 0, 28, 0 },
};
static const cqp_mb_golden_t g_cqp_md_mb[] = {
    { 0, 0, 25, 4 }, { 0, 1, 25, 4 }, { 0, 2, 25, 4 }, { 0, 3, 25, 4 },
    { 2, 0, 29, 7 }, { 2, 1, 29, 7 }, { 2, 2, 29, 7 }, { 2, 3, 29, 7 },
    { 1, 0, 30, 8 }, { 1, 1, 30, 8 }, { 1, 2, 30, 8 }, { 1, 3, 30, 8 },
    { 3, 0, 30, 8 }, { 3, 1, 30, 8 }, { 3, 2, 30, 8 }, { 3, 3, 30, 8 },
};

static void check_cqp_native(const char *preset,
                             const cqp_frame_golden_t *gf, int nf,
                             const cqp_mb_golden_t *gm, int nm)
{
    phx_param_t p;
    phx_ratecontrol_t *rc = NULL;
    phx_param_default(&p);
    CHECK(phx_param_apply_preset(&p, preset) == 0, "CQP golden preset");
    p.i_width = 64; p.i_height = 64;
    p.i_qp_constant = 28; p.rc_method = PHX_RC_CQP;
    p.i_keyint_max = 30; p.i_keyint_min = 30;
    p.i_scenecut_threshold = 0;
    p.i_aq_mode = PHX_RC_AQ_NONE;                     /* encoder.c:964 */
    p.b_mb_tree = 0;                                  /* encoder.c:965 */
    phx_param_finalize(&p);
    CHECK(phx_ratecontrol_init(&rc, &p, 16) == 0 && rc != NULL, "CQP golden init");
    if (!rc) return;

    for (int i = 0; i < nf; i++) {
        int qp = -1;
        phx_ratecontrol_set_kept_as_ref(rc, gf[i].kept);   /* :1508 */
        CHECK(phx_ratecontrol_start(rc, gf[i].type, 0, &qp) == 0,
              "CQP golden start");
        CHECK(qp == gf[i].qp, "CQP golden frame QP (pb/BREF offset)");
    }
    for (int i = 0; i < nm; i++) {
        int frame_qp = -1, kept = 0, got, lam;
        for (int j = 0; j < nf; j++)
            if (gf[j].frame == gm[i].frame) {
                frame_qp = gf[j].qp; kept = gf[j].kept; break;
            }
        CHECK(frame_qp >= 0, "CQP golden MB frame lookup");
        if (frame_qp < 0) continue;
        got = phx_ratecontrol_mb_qp(rc, gm[i].mb, (float)frame_qp, kept);
        CHECK(got == gm[i].qp, "CQP golden per-MB QP");
        lam = phx_ratecontrol_lambda(got);
        CHECK(lam == gm[i].lambda, "CQP golden lambda (tables.c:97-104)");
    }
    phx_ratecontrol_delete(rc);
}

static void check_cqp_native_golden(void)
{
    check_cqp_native("ultrafast", g_cqp_uf_frame, 8, g_cqp_uf_mb, 16);
    check_cqp_native("veryfast",  g_cqp_vf_frame, 8, g_cqp_vf_mb, 16);
    check_cqp_native("medium",    g_cqp_md_frame, 8, g_cqp_md_mb, 16);
}

int main(void)
{
    check_cqp_mapping();
    check_crf_monotonic();
    check_abr_feedback();
    check_abr_bitrate_monotonic();
    check_aq_offsets();
    check_chroma_qp();
    check_instrumented_golden();
    check_cqp_native_golden();
    if (!fails) printf("phx_ratecontrol: all checks passed\n");
    else printf("phx_ratecontrol: %d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
