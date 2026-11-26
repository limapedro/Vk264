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

/* ratecontrol.c:632-646 / :2011 / :2022 / :203-210.  With MB-tree and
 * qcompress=1 the CRF rate factor is exactly qp2qscale(rf), so a P frame with
 * nonzero SATD derives QP == rf. */
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
        CHECK(qp == (int)rf_values[i], "CRF QP equals rf with mbtree/qcompress=1");
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

int main(void)
{
    check_cqp_mapping();
    check_crf_monotonic();
    check_abr_feedback();
    check_abr_bitrate_monotonic();
    check_aq_offsets();
    check_chroma_qp();
    if (!fails) printf("phx_ratecontrol: all checks passed\n");
    else printf("phx_ratecontrol: %d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
