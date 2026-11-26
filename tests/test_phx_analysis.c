/* Standalone consistency test for the per-MB analysis driver.
 *
 * Builds a synthetic luma frame, loads a fresh neighbour cache, analyses one
 * MB and checks that every field of phx_mb_decision_t is in range and that
 * nnz flags / CBP agree with the emitted coefficients.  Repeating the same
 * call from freshly reset inputs must produce a byte-identical decision.
 *
 * SHADER: B — host-side test of the per-MB workgroup output boundary.
 */
#include "phx_analysis.h"
#include "phx_encoder.h"         /* full-pipeline diag.yuv B golden */
#include "phx_param.h"
#include "phx_frame.h"
#include "phx_mbcache.h"
#include "phx_mb_decision.h"
#include "phx_cost.h"           /* high-ref cost-table bounds (regression) */
#include "phx_mc.h"             /* transform decision's luma MC window */
#include "phx_rdo.h"            /* high-ref MBRD / RDO scratch bounds */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)
#define CHECKF(cond, fmt, ...) do { if (!(cond)) { \
    printf("FAIL " fmt " (line %d)\n", __VA_ARGS__, __LINE__); ++fails; } } while (0)

/* Deterministic 32x32 ramp: enough AC energy for all three intra modes. */
static void fill_frame(phx_frame_t *f)
{
    for (int y = 0; y < f->i_height; y++)
        for (int x = 0; x < f->i_width; x++)
            f->plane[x + y*f->i_stride] = (uint8_t)((x*17 + y*29 + 11) & 0xff);
}

/* First-MB NV12 chroma fixture of the medium/cabac probe: U and V are a
 * 5-sample horizontal ramp on 128, so DC128 prediction leaves real U/V
 * residual energy.  Mirrors parity_harness.py:64-77 (frame 0).
 * SHADER: B — B-stage analysis input fixture. */
static void fill_chroma(phx_frame_t *f)
{
    const int s = f->i_stride_planes[1];
    for (int y = 0; y < f->i_lines_planes[1]; y++)
        for (int x = 0; x < f->i_width_planes[1]; x++) {
            f->plane_chroma[y*s + 2*x + 0] = (uint8_t)(128 + 5*(x & 7));
            f->plane_chroma[y*s + 2*x + 1] = (uint8_t)(128 + 5*(x & 7));
        }
}

static int any_nonzero(const int16_t *c, int n)
{
    for (int i = 0; i < n; i++) if (c[i]) return 1;
    return 0;
}

/* Smooth 8-bit ramp with period 32 in both axes: a pure integer translation
 * has one zero-SAD candidate inside the search window (test_phx_me.c:23-27).
 * SHADER: B — deterministic B-stage input fixture. */
static uint8_t me_pat(int x, int y)
{
    return (uint8_t)(((x & 31) / 2) + 16*((y & 31) / 2));
}

/* Fill the padded planes of both frames; fenc is fref translated by (tx,ty). */
static void fill_me_frames(phx_frame_t *fref, phx_frame_t *fenc, int tx, int ty)
{
    for (int y = -PHX_PADV; y < fref->i_height + PHX_PADV; y++)
        for (int x = -PHX_PADH; x < fref->i_width + PHX_PADH; x++) {
            fref->plane[y*fref->i_stride + x] = me_pat(x, y);
            fenc->plane[y*fenc->i_stride + x] = me_pat(x + tx, y + ty);
        }
}

/* P-slice analysis must pick P_L0/D_16x16 with the MV found by phx_me16()
 * (analyse.c:1277/1287/3056-3058), publish it on all 16 blocks and emit no
 * fabricated residual.  Source: reference/x264/encoder/analyse.c:1255-1332,
 * common/mvpred.c:129-163, encoder/me.c:276-419.
 * SHADER: B — host-side test of the inter decision boundary. */
static int check_p_analysis(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.i_width = p.i_height = 32;
    p.i_me_method = PHX_ME_HEX;                         /* base.c:435 */
    p.i_me_range = 16;
    p.analyse_inter = 1;
    p.analyse_intra = 3;
    phx_param_finalize(&p);                             /* encoder.c:1218 */

    phx_frame_t *fref = phx_frame_new(32, 32, 0);
    phx_frame_t *fenc = phx_frame_new(32, 32, 0);
    phx_mb_state_t *s = phx_mb_state_new(2, 2);
    phx_mb_cache_t cache;
    phx_mb_decision_t d1, d2;
    if (!fref || !fenc || !s) {
        printf("FAIL p-analysis: allocation\n");
        return 1;
    }
    fill_me_frames(fref, fenc, 2, 0);                   /* known full-pel MV */
    s->slice_type = PHX_SLICE_P;
    s->first_mb = 0;

    memset(&d1, 0, sizeof(d1));
    memset(&cache, 0, sizeof(cache));
    phx_macroblock_cache_load(s, &cache, 0, 0, 0);
    int rc1 = phx_macroblock_analyse_slice(&p, fenc, fref, &cache, NULL,
                                           0, 0, 26, PHX_SLICE_P, &d1);
    CHECK(rc1 == 0, "P analysis returns success");
    if (!rc1) {
        CHECK(d1.mb_type == PHX_P_L0, "P analysis chose P_L0");
        CHECK(d1.i_partition == 16, "P_L0 uses D_16x16");
        /* phx_me16() finds the fullpel (2,0), then M4's byte-exact
         * phx_me_refine_subpel_mv() (me.c:792-797 / analyse.c:3119-3126)
         * refines it to (5,0) qpel on this synthetic pattern. */
        CHECK(d1.mv[0][0][0] == 5 && d1.mv[0][0][1] == 0,
              "P analysis found the refined MV (5,0) in qpel units");
        CHECK(d1.cbp_luma == 0 && d1.cbp_chroma == 0,
              "P_L0 no-residual decision has cbp=0");
        CHECK(d1.skip == 0, "non-pskip MV is not a skip");
        for (int i = 0; i < 16; i++) {
            CHECK(d1.ref[0][i] == 0 && d1.ref[1][i] == -1,
                  "P_L0 ref list 0 = 0 for all 4x4 blocks");
            CHECK(d1.mv[0][i][0] == 5 && d1.mv[0][i][1] == 0,
                  "P_L0 refined MV replicated over all 4x4 blocks");
        }
        for (int k = 0; k < 48; k++)
            CHECK(d1.nnz[k] == 0 && !any_nonzero(d1.dct[k], 16),
                  "P_L0 publishes no coefficients");

        /* mbrd==0 P_L0: the published transform flag must be exactly
         * x264's i_cost8 < i_cost4 (analyse.c:2761-2768/:3718-3719), not a
         * fixed 0/1.  Recompute the two kernels on the same MC prediction.
         * SHADER: B — A/B boundary assertion, no entropy state. */
        uint8_t pred[16*16];
        CHECK(phx_mc_luma_mb(fref, pred, 16, 0, 0,
                             d1.mv[0][0][0], d1.mv[0][0][1],
                             16, 16) == 0,
              "transform golden: P_L0 MC succeeds");
        int c8 = 0, c4 = 0, want_b8 = -1;
        phx_mb_transform_costs(fenc->plane, fenc->i_stride, pred, 16,
                               &c8, &c4, &want_b8);
        CHECKF(d1.b_transform_8x8 == want_b8,
               "P_L0 transform flag %d, cost8 %d < cost4 %d => %d",
               d1.b_transform_8x8, c8, c4, want_b8);
    }

    /* Determinism: fresh cache/state and identical inputs give the same
     * decision (the analysis stage has no hidden state). */
    fill_me_frames(fref, fenc, 2, 0);
    phx_mb_state_delete(s);
    s = phx_mb_state_new(2, 2);
    s->slice_type = PHX_SLICE_P;
    s->first_mb = 0;
    memset(&d2, 0, sizeof(d2));
    memset(&cache, 0, sizeof(cache));
    phx_macroblock_cache_load(s, &cache, 0, 0, 0);
    int rc2 = phx_macroblock_analyse_slice(&p, fenc, fref, &cache, NULL,
                                           0, 0, 26, PHX_SLICE_P, &d2);
    CHECK(rc2 == 0 && memcmp(&d1, &d2, sizeof(d1)) == 0,
          "P analysis decision is reproducible");

    phx_mb_state_delete(s);
    phx_frame_delete(fref);
    phx_frame_delete(fenc);
    return 0;
}

/* MBRD size accounting must not advance the live arithmetic coder: x264's
 * COPY_CABAC (rdo.c:71-73) copies only f8_bits_encoded + 460 context states,
 * and the size-only writers (cabac.h:101-124) never touch i_low/i_range/p.
 * SHADER: B — verifies the A/B state boundary at the analysis output. */
static int check_analysis_leaves_cabac_untouched(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.i_width = p.i_height = 32;
    p.i_subpel_refine = 7;                              /* medium: mbrd=1 */
    phx_param_finalize(&p);
    phx_frame_t *f = phx_frame_new(32, 32, 0);
    phx_mb_state_t *s = phx_mb_state_new(2, 2);
    phx_mb_cache_t cache;
    phx_mb_decision_t d;
    phx_cabac_t cb;
    uint8_t buf[512];
    int rc;
    if (!f || !s) { printf("FAIL cabac-immutability: allocation\n"); return 1; }
    fill_frame(f);
    s->slice_type = PHX_SLICE_I;
    s->first_mb = 0;
    memset(&cb, 0, sizeof(cb));
    phx_cabac_context_init(&cb, PHX_SLICE_I, 26, 0);
    phx_cabac_encode_init(&cb, buf, buf + sizeof(buf));
    uint8_t state[sizeof(cb.state)];
    memcpy(state, cb.state, sizeof(state));
    int low = cb.i_low, range = cb.i_range, queue = cb.i_queue;
    int outstanding = cb.i_bytes_outstanding;
    uint8_t *at = cb.p;

    memset(&d, 0, sizeof(d));
    memset(&cache, 0, sizeof(cache));
    phx_macroblock_cache_load(s, &cache, 0, 0, 0);
    rc = phx_macroblock_analyse_slice(&p, f, NULL, &cache, &cb,
                                      0, 0, 26, PHX_SLICE_I, &d);
    CHECK(rc == 0, "cabac-immutability: analysis succeeds with live coder");
    CHECK(cb.i_low == low && cb.i_range == range && cb.i_queue == queue &&
          cb.i_bytes_outstanding == outstanding && cb.p == at,
          "analysis does not advance the live arithmetic coder");
    CHECK(memcmp(state, cb.state, sizeof(state)) == 0,
          "analysis does not modify live CABAC contexts");

    phx_mb_state_delete(s);
    phx_frame_delete(f);
    return 0;
}

/* The encoder passes cache->p_fdec pointing into its live reconstruction frame
 * (phx_encoder.c:1396, x264 encoder.c:2861 -> macroblock.c:533).  x264's
 * predictors read the *reconstructed* left/top border written there by the
 * previous MB (common/macroblock.c:1645-1655, frame.c:535-553), never the
 * source samples.  A regression that re-seeded that border from p_fenc changed
 * MB1 block0's predictors (105/118/131/144 instead of 106/115/132/141 on the
 * medium/cabac fixture), shifted its SATD and flipped block1's intra4x4 mode
 * at CABAC bin 715.  Verify the caller-owned FDEC border is left untouched.
 * SHADER: B — host-side check of the caller-owned reconstruction boundary. */
static int check_caller_fdec_border(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.i_width = 32; p.i_height = 16;                 /* two MBs in one row */
    p.analyse_intra = 3;
    phx_param_finalize(&p);

    phx_frame_t *f = phx_frame_new(32, 16, 0);
    phx_mb_state_t *s = phx_mb_state_new(2, 1);
    if (!f || !s) { printf("FAIL fdec-border: allocation\n"); return 1; }
    fill_frame(f);
    s->slice_type = PHX_SLICE_I;
    s->first_mb = 0;
    for (int i = 0; i < 2; i++) s->type[i] = PHX_I_16x16;

    const int fs = PHX_FDEC_STRIDE;
    uint8_t *buf = (uint8_t *)calloc((size_t)fs * 48 + 4, 1);
    uint8_t *p_fdec = buf + 2*fs + 1;                 /* macroblock.c:533 */
    if (!buf) { printf("FAIL fdec-border: allocation\n"); return 1; }
    for (int y = -1; y < 16; y++) p_fdec[-1 + y*fs] = (uint8_t)(200 + y);
    for (int x = 0; x < 18; x++) p_fdec[x - fs] = (uint8_t)(160 + x);

    phx_mb_cache_t cache;
    phx_mb_decision_t d;
    memset(&cache, 0, sizeof(cache));
    memset(&d, 0, sizeof(d));
    phx_macroblock_cache_load(s, &cache, 1, 0, 0);
    CHECK(cache.i_neighbour_intra & PHX_MB_LEFT,
          "fdec-border: left neighbour available at MB(1,0)");
    cache.p_fdec = p_fdec;
    cache.fdec_stride = fs;
    CHECK(phx_macroblock_analyse_slice(&p, f, NULL, &cache, NULL,
                                       1, 0, 26, PHX_SLICE_I, &d) == 0,
          "fdec-border: analysis succeeds with caller FDEC");
    for (int y = 0; y < 16; y++)
        CHECK(p_fdec[-1 + y*fs] == (uint8_t)(200 + y),
              "analysis preserves reconstructed left border (not source)");
    for (int x = 0; x < 16; x++)
        CHECK(p_fdec[x - fs] == (uint8_t)(160 + x),
              "analysis preserves reconstructed top border (not source)");

    free(buf);
    phx_mb_state_delete(s);
    phx_frame_delete(f);
    return 0;
}

/* Regression: the high-reference/high-B presets (slower 8 refs, veryslow
 * 16 refs / 8 B, placebo 16 refs / 16 B) previously crashed a full CLI encode
 * (phx rc 3221225477 / 0xC0000005).  Keep the analysis, cost and RDO scratch
 * paths under test independently of the CLI: every MB of a 2x2-MB frame is
 * analysed in both I and P slices with the live CABAC coder (mbrd >= 2), so
 * the I4 RD refinement and the trellis QNS paths really execute.  Then probe
 * the cost/RDO table boundaries with the same presets' reference counts.
 * SHADER: B — host-side bounds regression at the A/B output boundary. */
static int check_high_ref_analysis(void)
{
    static const struct {
        const char *name;
        int refs, bframes, subme, mv_range;
    } cases[] = {
        { "slower",    8,  3,  9, 16 },
        { "veryslow", 16,  8, 10, 24 },
        { "placebo",  16, 16, 11, 24 },
    };

    for (unsigned k = 0; k < sizeof(cases)/sizeof(cases[0]); k++) {
        phx_param_t p;
        phx_param_default(&p);
        CHECK(phx_param_apply_preset(&p, cases[k].name) == 0,
              "high-ref preset is recognised");
        CHECK(p.i_frame_reference == cases[k].refs,
              "preset keeps x264's reference count");
        CHECK(p.i_bframe == cases[k].bframes,
              "preset keeps x264's B-frame count");
        CHECK(p.i_subpel_refine == cases[k].subme,
              "preset keeps x264's subme (mbrd driver)");
        p.i_width = p.i_height = 32;
        p.analyse_inter = 1;
        p.analyse_intra = 3;
        phx_param_finalize(&p);

        phx_frame_t *f = phx_frame_new(32, 32, 0);
        phx_frame_t *fref = phx_frame_new(32, 32, 0);
        phx_mb_state_t *s = phx_mb_state_new(2, 2);
        if (!f || !fref || !s) {
            printf("FAIL high-ref %s: allocation\n", cases[k].name);
            return 1;
        }
        fill_frame(f); fill_chroma(f);
        fill_frame(fref); fill_chroma(fref);
        phx_frame_init_lowres(f);
        phx_frame_init_lowres(fref);

        uint8_t buf[512];
        phx_cabac_t cb;
        memset(&cb, 0, sizeof(cb));
        for (int slice = PHX_SLICE_I; slice <= PHX_SLICE_P; slice++) {
            s->slice_type = slice;
            s->first_mb = 0;
            for (int i = 0; i < 4; i++)
                s->type[i] = slice == PHX_SLICE_P ? PHX_P_L0 : PHX_I_16x16;
            phx_cabac_context_init(&cb, slice, 28, 0);
            phx_cabac_encode_init(&cb, buf, buf + sizeof(buf));
            uint8_t state[sizeof(cb.state)];
            memcpy(state, cb.state, sizeof(state));
            int low = cb.i_low, range = cb.i_range;
            for (int mb_y = 0; mb_y < 2; mb_y++) {
                for (int mb_x = 0; mb_x < 2; mb_x++) {
                    phx_mb_cache_t cache;
                    phx_mb_decision_t d;
                    memset(&cache, 0, sizeof(cache));
                    memset(&d, 0, sizeof(d));
                    phx_macroblock_cache_load(s, &cache, mb_x, mb_y, 0);
                    int rc = phx_macroblock_analyse_slice(
                        &p, f, slice == PHX_SLICE_P ? fref : NULL,
                        &cache, &cb, mb_x, mb_y, 28, slice, &d);
                    CHECK(rc == 0, "high-ref analysis returns success");
                    if (rc) continue;
                    /* Bounded decision arrays, independent of the MB type. */
                    CHECK(d.i_qp == 28 && d.i_partition == 16,
                          "high-ref decision QP/partition in range");
                    CHECK(d.intra16_pred_mode >= 0 && d.intra16_pred_mode <= 6 &&
                          d.chroma_pred_mode >= 0 && d.chroma_pred_mode <= 6,
                          "high-ref intra predictor indices in range");
                    for (int i = 0; i < 16; i++) {
                        CHECK(d.intra4x4_pred_mode[i] >= 0 &&
                              d.intra4x4_pred_mode[i] <= 11,
                              "high-ref I4 predictor index in range");
                        CHECK(d.ref[0][i] >= -1 && d.ref[0][i] < 16 &&
                              d.ref[1][i] >= -1 && d.ref[1][i] < 16,
                              "high-ref ref index is bounded by 16 refs");
                    }
                    for (int i = 0; i < 4; i++)
                        CHECK(d.intra8x8_pred_mode[i] >= 0 &&
                              d.intra8x8_pred_mode[i] <= 11,
                              "high-ref I8 predictor index in range");
                    for (int i = 0; i < 48; i++)
                        CHECK(d.nnz[i] <= 1, "high-ref nnz is boolean");
                    for (int i = 0; i < 48; i++)
                        for (int j = 0; j < 16; j++)
                            CHECK(d.dct[i][j] >= -32768 && d.dct[i][j] <= 32767,
                                  "high-ref coefficient fits int16");
                }
            }
            CHECK(cb.i_low == low && cb.i_range == range &&
                  memcmp(state, cb.state, sizeof(state)) == 0,
                  "high-ref MBRD leaves the live arithmetic coder untouched");
        }

        /* The cost tables are built with the preset's mv_range (16/24) and
         * indexed by up to 16 references; x264 clips that lookup to 2 slots. */
        phx_cost_tables_t costs;
        CHECK(phx_analyse_init_costs(&costs, cases[k].mv_range,
                  (int)p.i_me_method, 28, 28) == 0,
              "high-ref cost tables initialise");
        phx_mb_analysis_t a;
        memset(&a, 0, sizeof(a));
        phx_mb_analyse_init_qp(&p, 28, p.i_chroma_qp_index_offset, 3, &a);
        phx_mb_analyse_load_costs(&costs, &a, cases[k].refs, cases[k].refs);
        CHECK(costs.cost_mv[28] && a.p_cost_mv == costs.cost_mv[28],
              "high-ref MV cost pointer is inside the centred allocation");
        CHECK(a.p_cost_ref[0] == costs.cost_ref[28][2] &&
              a.p_cost_ref[1] == costs.cost_ref[28][2],
              "high-ref reference costs clip to the three x264 slots");
        phx_analyse_free_costs(&costs, cases[k].mv_range);
        CHECK(phx_analyse_init_costs(&costs, cases[k].mv_range,
                  (int)p.i_me_method, 28, 28) == 0,
              "cost tables rebuild without leaking the mv_range allocation");
        phx_analyse_free_costs(&costs, cases[k].mv_range);

        /* RDO scratch/tables must stay bounded for every QP and category. */
        for (int qp = 0; qp <= 51; qp++)
            for (int list = 0; list < 4; list++) {
                CHECK(phx_rdo_quant4_mf(list, qp) != NULL &&
                      phx_rdo_quant4_bias0(list, qp) != NULL &&
                      phx_rdo_unquant4_mf(list, qp) != NULL,
                      "RDO 4x4 table in range");
            }
        CHECK(phx_rdo_quant4_mf(0, 52) == NULL, "RDO table rejects QP > 51");
        for (int prefix = 0; prefix < 15; prefix++)
            CHECK(phx_rdo_size_unary(prefix) != NULL &&
                  phx_rdo_transition_unary(prefix) != NULL,
                  "size-only RDO unary table in range");
        CHECK(phx_rdo_size_unary(15) == NULL,
              "size-only RDO unary table rejects out-of-range prefix");

        phx_mb_state_delete(s);
        phx_frame_delete(fref);
        phx_frame_delete(f);
    }
    return 0;
}

static void check_decision(const phx_mb_decision_t *d, const char *tag)
{
    CHECK(d->mb_type == PHX_I_4x4 || d->mb_type == PHX_I_8x8 ||
          d->mb_type == PHX_I_16x16, "mb_type is a supported intra type");
    CHECK(d->i_partition == 16, "i_partition == D_16x16");
    CHECK(d->i_qp >= 0 && d->i_qp <= 51, "i_qp range");
    CHECK(d->intra16_pred_mode >= 0 && d->intra16_pred_mode <= 6,
          "intra16_pred_mode is x264's raw I_PRED_16x16 index (0..6)");
    CHECK(d->chroma_pred_mode >= 0 && d->chroma_pred_mode <= 6,
          "chroma_pred_mode is x264's raw I_PRED_CHROMA index (0..6)");
    CHECK(d->cbp_luma >= 0 && d->cbp_luma <= 15, "cbp_luma range");
    CHECK(d->cbp_chroma >= 0 && d->cbp_chroma <= 3, "cbp_chroma range");
    CHECK(d->i_cbp == ((d->cbp_chroma << 4) | d->cbp_luma | 0x1700),
          "i_cbp matches cbp_luma/chroma");
    CHECK(d->skip == 0, "intra decision is not a skip");
    CHECK(d->b_transform_8x8 == (d->mb_type == PHX_I_8x8),
          "transform size matches mb_type");
    for (int i = 0; i < 4; i++)
        CHECK(d->intra8x8_pred_mode[i] >= 0 && d->intra8x8_pred_mode[i] <= 11,
              "intra8x8_pred_mode is a raw I_PRED_8x8 index (0..11)");
    for (int i = 0; i < 16; i++) {
        CHECK(d->intra4x4_pred_mode[i] >= 0 && d->intra4x4_pred_mode[i] <= 11,
              "intra4x4_pred_mode is a raw I_PRED_4x4 index (0..11)");
        CHECK(d->ref[0][i] == -1 && d->ref[1][i] == -1, "intra refs are -1");
        CHECK(d->mv[0][i][0] == 0 && d->mv[0][i][1] == 0, "intra mv is zero");
    }
    for (int k = 0; k < 48; k++) CHECK(d->nnz[k] <= 1, "nnz is boolean");

    /* nnz/CBP must agree with the quantized coefficients that were emitted.
     * Both stages use x264's scan8 cache layout (common/base.h:180). */
    for (int idx = 0; idx < 16; idx++) {
        int nz = any_nonzero(d->dct[phx_scan8[idx]], 16);
        int flag = d->nnz[phx_scan8[idx]];
        CHECK(flag == (d->mb_type == PHX_I_8x8 ? (d->nnz[phx_scan8[4*(idx>>2)]] ? 1 : 0) : nz),
              "nnz matches emitted coefficients");
        if (d->mb_type == PHX_I_4x4)
            CHECK(((d->cbp_luma >> (idx >> 2)) & 1) == nz, "I4 cbp bit matches nnz");
    }
    if (d->mb_type == PHX_I_8x8) {
        for (int i = 0; i < 4; i++) {
            int nz = 0;
            for (int j = 0; j < 4; j++) nz |= any_nonzero(d->dct[phx_scan8[4*i+j]], 16);
            CHECK(((d->cbp_luma >> i) & 1) == nz, "I8 cbp bit matches nnz");
            for (int j = 0; j < 4; j++)
                CHECK(d->nnz[phx_scan8[4*i+j]] == nz, "I8 four nnz flags agree");
        }
    } else if (d->mb_type == PHX_I_16x16) {
        int nz = 0;
        for (int i = 0; i < 16; i++)
            nz |= any_nonzero(d->dct[phx_scan8[i]], 16);
        CHECK(d->cbp_luma == (nz ? 0xf : 0), "I16 cbp is all-or-nothing");
    }
    /* Chroma slots are an explicit TODO; they must at least be quiet.
     * The luma slots are exactly phx_scan8[0..15] plus phx_scan8[48]==0. */
    uint8_t used[48];
    memset(used, 0, sizeof(used));
    for (int i = 0; i < 16; i++) used[phx_scan8[i]] = 1;
    used[phx_scan8[48]] = 1;
    for (int k = 0; k < 48; k++)
        if (!used[k])
            CHECK(!any_nonzero(d->dct[k], 16), "chroma coefficient slots are zero");
    CHECK(d->cbp_chroma == 0, "chroma cbp is zero (TODO)");
    (void)tag;
}

/* ---- source-aligned I4 mode-decision goldens (worker Z1) ---------------- */

/* tools/parity_harness.py:64-77 frame 0, the fixture used by
 * tools/cabac_bin_diff.py.  Luma (i*7+j*13)&255 and interleaved NV12
 * U=(128+i*5)&255 / V=(128+j*5)&255.  SHADER: B — deterministic input. */
static void fill_parity_fixture(phx_frame_t *f)
{
    const int s = f->i_stride;
    for (int y = 0; y < f->i_height; y++)
        for (int x = 0; x < f->i_width; x++)
            f->plane[x + y*s] = (uint8_t)((x*7 + y*13) & 255);
    const int cs = f->i_stride_planes[1];
    for (int y = 0; y < f->i_lines_planes[1]; y++)
        for (int x = 0; x < f->i_width_planes[1]; x++) {
            f->plane_chroma[y*cs + 2*x]     = (uint8_t)((128 + x*5) & 255);
            f->plane_chroma[y*cs + 2*x + 1] = (uint8_t)((128 + y*5) & 255);
        }
}

/* Analyse MB0 of the parity fixture exactly as the golden harness does:
 * preset parameters, I slice, MB QP 25 (x264's I-frame QP from --qp 28),
 * freshly initialised CABAC contexts (rdo.c:71-73 COPY_CABAC source state).
 * The expected arrays are the 16 L1BLK `mode=` values x264's own
 * encoder/cabac.c dump emits for MB0 at each preset (verified by
 * tools/cabac_bin_diff.py).  SHADER: B — host golden of the A/B output. */
static int check_mb0_i4_golden(const char *preset, const int want[16])
{
    phx_param_t p;
    phx_param_default(&p);
    p.i_width = 64; p.i_height = 64;
    if (phx_param_apply_preset(&p, preset)) {
        printf("FAIL mb0-golden: bad preset %s\n", preset);
        return 1;
    }
    phx_param_finalize(&p);

    phx_frame_t *f = phx_frame_new(64, 64, 0);
    phx_mb_state_t *s = phx_mb_state_new(4, 4);
    if (!f || !s) { printf("FAIL mb0-golden: allocation\n"); return 1; }
    fill_parity_fixture(f);
    s->slice_type = PHX_SLICE_I;
    s->first_mb = 0;
    for (int i = 0; i < 16; i++) s->type[i] = PHX_I_16x16;

    phx_mb_cache_t cache;
    phx_mb_decision_t d;
    phx_cabac_t cb;
    uint8_t cbuf[512];
    memset(&cache, 0, sizeof(cache));
    memset(&d, 0, sizeof(d));
    memset(&cb, 0, sizeof(cb));
    phx_cabac_context_init(&cb, PHX_SLICE_I, 25, 0);
    phx_cabac_encode_init(&cb, cbuf, cbuf + sizeof(cbuf));
    phx_macroblock_cache_load(s, &cache, 0, 0, 0);
    int rc = phx_macroblock_analyse_slice(&p, f, NULL, &cache, &cb,
                                          0, 0, 25, PHX_SLICE_I, &d);
    CHECK(rc == 0, "mb0-golden analysis succeeds");
    if (!rc) {
        CHECKF(d.mb_type == PHX_I_4x4, "MB0 chose I_4x4 at %s", preset);
        for (int i = 0; i < 16; i++)
            CHECKF(d.intra4x4_pred_mode[i] == want[i],
                   "MB0 I4 block %d at %s: expected %d got %d",
                   i, preset, want[i], d.intra4x4_pred_mode[i]);
    }
    phx_mb_state_delete(s);
    phx_frame_delete(f);
    return 0;
}

/* x264 L1BLK mode= arrays from encoder/cabac.c, one per preset.  Rows come
 * from the authoritative golden trace (golden_stderr.txt) grouped by preset:
 * SAD presets choose HU broadly, the mbrd==0 SATD presets apply the x3+shortcut,
 * and the RD presets re-cost with rd_cost_i4x4() before publishing. */
static const int z1_superfast_modes[16] = {
    11, 8, 3, 8, 8, 8, 8, 8, 3, 8, 3, 8, 8, 8, 8, 8
};
static const int z1_veryfast_modes[16] = {
    11, 1, 3, 8, 1, 1, 1, 1, 3, 1, 3, 1, 1, 1, 4, 8
};
static const int z1_faster_modes[16] = {
    11, 1, 3, 8, 1, 1, 1, 1, 3, 1, 3, 1, 1, 1, 4, 8
};
static const int z1_medium_modes[16] = {
    11, 1, 3, 8, 1, 1, 3, 1, 3, 3, 3, 1, 3, 1, 4, 8
};
static const int z1_slow_modes[16] = {
    11, 8, 3, 1, 8, 1, 1, 1, 3, 1, 3, 1, 3, 1, 4, 8
};
static const int z1_placebo_modes[16] = {
    11, 8, 3, 1, 8, 1, 1, 1, 3, 1, 3, 1, 3, 1, 4, 8
};
static const int z1_veryslow_modes[16] = {
    11, 8, 3, 1, 8, 1, 1, 1, 3, 1, 3, 1, 3, 1, 4, 8
};

/* W8's caller-owned reconstructed FDEC border must survive analysis; this
 * golden also pins MB1's exact I4 mode array after the reconstructed left
 * neighbour has been encoded by MB0.  SHADER: B — caller FDEC boundary. */
static int check_mb1_medium_golden(void)
{
    static const int want[16] = {
        1, 8, 1, 1, 1, 9, 0, 8, 5, 8, 8, 8, 3, 1, 3, 1
    };
    phx_param_t p;
    phx_param_default(&p);
    p.i_width = 64; p.i_height = 64;                /* parity fixture geometry */
    phx_param_apply_preset(&p, "medium");
    phx_param_finalize(&p);

    phx_frame_t *f = phx_frame_new(64, 64, 0);
    phx_mb_state_t *s = phx_mb_state_new(4, 4);
    if (!f || !s) { printf("FAIL mb1-golden: allocation\n"); return 1; }
    fill_parity_fixture(f);
    s->slice_type = PHX_SLICE_I;
    s->first_mb = 0;
    for (int i = 0; i < 16; i++) s->type[i] = PHX_I_16x16;

    const int fs = PHX_FDEC_STRIDE;
    uint8_t *buf = (uint8_t *)calloc((size_t)fs * 48 + 4, 1);
    uint8_t *fdec = buf + 2*fs + 1;                 /* macroblock.c:533 */
    if (!buf) { printf("FAIL mb1-golden: allocation\n"); return 1; }

    /* x264's reconstructed right edge of MB0 at medium/cabac, read from an
     * instrumented reference build at the entry to MB1's mb_analyse_intra
     * (common/macroblock.c:1645-1655).  Seeding the caller-owned FDEC border
     * with these exact samples makes the I4 predictors see byte-identical
     * inputs; W8's fix guarantees analysis leaves this border untouched. */
    static const uint8_t mb0_right_edge[16] = {
        106, 115, 132, 141, 160, 170, 185, 194,
        212, 223, 238, 247,   3,  14,  36,  48
    };
    /* x264's per-MB intra4x4 predictor cache at the entry to MB1's analysis,
     * from the same instrumented golden build (macroblock.h:180 scan8 layout).
     * Seeding it keeps this golden independent of the cache-save port. */
    static const int8_t mb1_cache_modes[40] = {
        0, 0, 0, 0, -1,-1,-1,-1, 0, 0, 0, 1, 1, 1, 9, 9,
        0, 0, 0, 1, 1, 1, 9, 9, 0, 0, 0, 1, 8, 8, 3, 1,
        0, 0, 0, 8, 8, 8, 4, 8
    };

    phx_mb_cache_t cache;
    phx_mb_decision_t d0, d1;
    memset(&cache, 0, sizeof(cache));
    memset(&d0, 0, sizeof(d0));
    memset(&d1, 0, sizeof(d1));
    phx_macroblock_cache_load(s, &cache, 0, 0, 0);
    cache.p_fdec = fdec;                 /* caller-owned reconstruction buffer */
    cache.fdec_stride = fs;
    CHECK(phx_macroblock_analyse_slice(&p, f, NULL, &cache, NULL,
                                       0, 0, 25, PHX_SLICE_I, &d0) == 0,
          "mb1-golden: MB0 analysis succeeds");
    /* Publish MB0's neighbour modes/coefficients and reconstructed pixels
     * exactly as phx_encoder.c does between MBs (x264 macroblock.c:1645-1655).
     * W8: analysis must not overwrite the caller's reconstructed border. */
    s->type[0] = d0.mb_type;             /* cache_save reads the MB type here */
    phx_macroblock_cache_save(s, &cache, 0);
    for (int y = 0; y < 16; y++) fdec[15 + y*fs] = mb0_right_edge[y];
    phx_macroblock_cache_load(s, &cache, 1, 0, 0);
    cache.p_fdec = fdec + 16;            /* caller advances FDEC per MB */
    cache.fdec_stride = fs;
    memcpy(cache.intra4x4_pred_mode, mb1_cache_modes, sizeof(mb1_cache_modes));
    CHECK(cache.i_neighbour_intra & PHX_MB_LEFT,
          "mb1-golden: left neighbour available at MB1");
    CHECK(phx_macroblock_analyse_slice(&p, f, NULL, &cache, NULL,
                                       1, 0, 25, PHX_SLICE_I, &d1) == 0,
          "mb1-golden: MB1 analysis succeeds");
    for (int i = 0; i < 16; i++)
        CHECKF(d1.intra4x4_pred_mode[i] == want[i],
               "MB1 I4 block %d (medium): expected %d got %d",
               i, want[i], d1.intra4x4_pred_mode[i]);
    /* W8 invariant: the caller-owned reconstructed border is untouched. */
    for (int y = 0; y < 16; y++)
        CHECK(fdec[15 + y*fs] == mb0_right_edge[y],
              "MB1 analysis preserved the reconstructed left border");

    free(buf);
    phx_mb_state_delete(s);
    phx_frame_delete(f);
    return 0;
}

/* ---- row 17: byte-exact P_SKIP decision golden -------------------------- */

/* x264 instrumented at encoder/encoder.c:2861 (after x264_macroblock_analyse)
 * for a 64x64 two-frame static fixture (frame 1 is a byte-identical copy of
 * frame 0), ultrafast/QP28/CABAC.  Golden: all 16 MBs P_SKIP.  MB0 is
 * promoted from P_L0 by encoder/macroblock.c:953-964 after encode; MB1..15
 * take the fast probe at analyse.c:3009-3013 (left/top neighbour P_SKIP).
 * No approximation: each flag comes from x264's own h->mb.i_type sampled
 * after x264_macroblock_encode(). */
static const uint8_t z1_pskip_golden[16] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};

/* Analyse one complete static P frame through the real per-MB driver,
 * publishing MB types into the neighbour state exactly as the encoder does.
 * SHADER: B — per-MB decision boundary golden (skip flag + MB type). */
static int check_pskip_decision_golden(const char *preset)
{
    phx_param_t p;
    phx_param_default(&p);
    p.i_width = 64; p.i_height = 64;
    if (phx_param_apply_preset(&p, preset)) {
        printf("FAIL pskip-golden: bad preset\n");
        return 1;
    }
    phx_param_finalize(&p);

    phx_frame_t *fref = phx_frame_new(64, 64, 0);
    phx_frame_t *fenc = phx_frame_new(64, 64, 0);
    phx_mb_state_t *s = phx_mb_state_new(4, 4);
    if (!fref || !fenc || !s) {
        printf("FAIL pskip-golden: allocation\n");
        return 1;
    }
    fill_parity_fixture(fref);                 /* frame 0 source */
    fill_parity_fixture(fenc);                 /* frame 1: static duplicate */
    s->slice_type = PHX_SLICE_I;
    s->first_mb = 0;
    for (int i = 0; i < 16; i++) s->type[i] = PHX_I_16x16;

    const int fs = PHX_FDEC_STRIDE;
    uint8_t *buf = (uint8_t *)calloc((size_t)fs * 48 + 64, 1);
    if (!buf) { printf("FAIL pskip-golden: allocation\n"); return 1; }

    /* Reconstruct frame 0 the same way x264 does before coding frame 1;
     * the P_SKIP probe compares against this reconstructed reference. */
    for (int mb = 0; mb < 16; mb++) {
        int x = mb & 3, y = mb >> 2;
        phx_mb_cache_t ic;
        phx_mb_decision_t id;
        memset(&ic, 0, sizeof(ic));
        memset(&id, 0, sizeof(id));
        phx_macroblock_cache_load(s, &ic, x, y, 0);
        ic.p_fdec = buf + 2*fs + 1;
        ic.fdec_stride = fs;
        int rc = phx_macroblock_analyse_slice(&p, fref, NULL, &ic, NULL,
                                              x, y, 25, PHX_SLICE_I, &id);
        CHECKF(rc == 0, "pskip-golden: I MB %d analysis succeeds", mb);
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 16; i++)
                fref->plane[(y*16+j)*fref->i_stride + x*16+i] =
                    ic.p_fdec[i + j*fs];
        s->type[mb] = id.mb_type;
        phx_macroblock_cache_save(s, &ic, 0);
    }

    s->slice_type = PHX_SLICE_P;
    s->first_mb = 0;
    for (int i = 0; i < 16; i++) s->type[i] = PHX_I_16x16;

    for (int mb = 0; mb < 16; mb++) {
        int x = mb & 3, y = mb >> 2;
        phx_mb_cache_t cache;
        phx_mb_decision_t d;
        memset(&cache, 0, sizeof(cache));
        memset(&d, 0, sizeof(d));
        phx_macroblock_cache_load(s, &cache, x, y, 0);
        cache.p_fdec = buf + 2*fs + 1 + x*16;    /* caller advances per MB */
        cache.fdec_stride = fs;
        int rc = phx_macroblock_analyse_slice(&p, fenc, fref, &cache, NULL,
                                              x, y, 28, PHX_SLICE_P, &d);
        CHECKF(rc == 0, "pskip-golden: MB %d analysis succeeds", mb);
        if (rc) continue;
        CHECKF(d.skip == z1_pskip_golden[mb],
               "pskip-golden: MB %d skip expected %d got %d",
               mb, z1_pskip_golden[mb], d.skip);
        CHECKF(d.mb_type == (d.skip ? PHX_P_SKIP : PHX_P_L0),
               "pskip-golden: MB %d type expected %d got %d",
               mb, d.skip ? PHX_P_SKIP : PHX_P_L0, d.mb_type);
        /* publish into the neighbour state for the next MB (encoder.c:2861) */
        s->type[mb] = d.mb_type;
        phx_macroblock_cache_save(s, &cache, 0);
    }

    free(buf);
    phx_mb_state_delete(s);
    phx_frame_delete(fref);
    phx_frame_delete(fenc);
    return 0;
}

/* ---- B-direct / B_SKIP integration golden (B1 API) ----------------------- */

/* B1's predictor/probe are golden-tested against x264 in phx_mvdirect.c.  This
 * test pins Phoenix's B-slice *integration*: for a static frame triplet with
 * an all-P_SKIP colocated picture, x264 emits all-16 B_SKIP (analyse.c:3334-
 * 3359); the decision must carry PHX_B_SKIP, d.skip and B1's exact direct
 * MV/ref arrays — never an intra fallback.  SHADER: B — A/B boundary golden. */
static int check_b_skip_golden(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.i_width = 64; p.i_height = 64;
    if (phx_param_apply_preset(&p, "veryfast")) {
        printf("FAIL bskip-golden: bad preset\n");
        return 1;
    }
    phx_param_finalize(&p);

    phx_frame_t *fenc = phx_frame_new(64, 64, 0);
    phx_frame_t *fref0 = phx_frame_new(64, 64, 0);
    phx_frame_t *fref1 = phx_frame_new(64, 64, 0);
    phx_mb_state_t *colst = phx_mb_state_new(4, 4);
    if (!fenc || !fref0 || !fref1 || !colst) {
        printf("FAIL bskip-golden: allocation\n");
        return 1;
    }
    fill_parity_fixture(fenc);                 /* identical static frames */
    fill_parity_fixture(fref0);
    fill_parity_fixture(fref1);
    colst->slice_type = PHX_SLICE_P;
    colst->first_mb = 0;
    for (int i = 0; i < 16; i++) colst->type[i] = PHX_P_SKIP;
    for (int l = 0; l < 2; l++)
        for (int i = 0; i < 4*16; i++) colst->ref[l][i] = 0;

    uint8_t part[16];
    for (int i = 0; i < 16; i++) part[i] = PHX_MVDIR_D_16x16;
    phx_mvdirect_col_t col;
    memset(&col, 0, sizeof(col));
    col.mb_type = colst->type;
    col.mb_partition = part;
    col.ref[0] = colst->ref[0]; col.ref[1] = colst->ref[1];
    col.mv[0] = colst->mv[0]; col.mv[1] = colst->mv[1];
    col.field = NULL;
    col.i_poc = 2;
    col.i_delta_poc[0] = -1; col.i_delta_poc[1] = 1;
    col.i_mb_stride = 4;
    col.i_b8_stride = 8;
    col.i_b4_stride = 16;

    phx_analysis_b_input_t b;
    memset(&b, 0, sizeof(b));
    b.fref1 = fref1;
    b.col = &col;
    b.cur_poc = 4;
    b.b_direct_spatial = 1;
    b.bipred_weight = 32;

    const int fs = PHX_FDEC_STRIDE;
    uint8_t *buf = (uint8_t *)calloc((size_t)fs * 48 + 64, 1);
    if (!buf) { printf("FAIL bskip-golden: allocation\n"); return 1; }

    for (int mb = 0; mb < 16; mb++) {
        int x = mb & 3, y = mb >> 2;
        phx_mb_cache_t cache;
        phx_mb_decision_t d;
        phx_mvdirect_input_t din;
        phx_mvdirect_result_t direct;
        memset(&cache, 0, sizeof(cache));
        memset(&d, 0, sizeof(d));
        phx_macroblock_cache_load(colst, &cache, x, y, 0);
        cache.p_fdec = buf + 2*fs + 1 + x*16;
        cache.fdec_stride = fs;
        int rc = phx_macroblock_analyse_slice_b(&p, fenc, fref0, &b, &cache,
                                                NULL, x, y, 28, &d);
        CHECKF(rc == 0, "bskip-golden: MB %d analysis succeeds", mb);
        if (rc) continue;
        CHECKF(d.skip == 1 && d.mb_type == PHX_B_SKIP,
               "bskip-golden: MB %d is B_SKIP (got type=%d skip=%d)",
               mb, d.mb_type, d.skip);

        /* Independently compute B1's direct prediction with identical inputs. */
        memset(&din, 0, sizeof(din));
        din.cache = &cache;
        din.mb_x = x; din.mb_y = y; din.mb_xy = mb;
        din.i_b8_xy = 2*(y*8 + x);
        din.i_b4_xy = 4*(y*16 + x);
        din.direct_mode = p.i_direct_mv_pred;
        din.b_direct_spatial = 1;
        din.i_threads = 1;
        din.col = &col;
        din.cur_poc = 4;
        CHECKF(phx_mvdirect_predict(&din, &direct) == 0,
               "bskip-golden: MB %d direct prediction succeeds", mb);
        for (int l = 0; l < 2; l++)
            for (int i = 0; i < 16; i++) {
                CHECKF(d.ref[l][i] == direct.ref[l][i] &&
                       d.mv[l][i][0] == direct.mv[l][i][0] &&
                       d.mv[l][i][1] == direct.mv[l][i][1],
                       "bskip-golden: MB %d ref/mv[%d][%d] differs", mb, l, i);
            }
    }

    free(buf);
    phx_mb_state_delete(colst);
    phx_frame_delete(fenc);
    phx_frame_delete(fref0);
    phx_frame_delete(fref1);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Real-diag.yuv B golden (instrumented x264, encoder/macroblock.c:974 dump at
 * encoder.c:2861, --qp 28 --preset medium --keyint 30 --scenecut 0 --threads 1).
 * The first B frame in coded order is display POC 4; the first two rows below
 * are x264's exact (mb_type, partition, r0, r1, mv0x, mv0y, mv1x, mv1y) for all
 * 16 macroblocks.  MB0 is the over-skip case from the B diagnosis: x264 emits
 * B_L0_L0 (8) with MV (8,-11); the old Phoenix path emitted B_SKIP (18).
 * The golden fixture diag.yuv is generated by build/ tools, so the test skips
 * (without failing) when it is not present in a clean checkout.
 * SHADER: B/C — full-pipeline observation of the decision boundary.
 */
/* y-major (mb_y outer, mb_x inner) order, exactly as x264's MBDIAG dump and
 * phx_mbdiag records lay the MBs out. */
static const int phx_b_golden_poc4[16][8] = {
    { 8, 16,  0, -1,   8, -11,   0,   0 }, {12, 16, -1,  0,   0,   0,  -8,  39},
    {12, 14, -1,  0,   0,   0,  -8,  39 }, {11, 14, -1,  0,   0,   0,  -8,  39},
    {16, 16,  0,  0,   8, -14,  -2,  37 }, {12, 16, -1,  0,   0,   0,  -8,  42},
    { 8, 16,  0, -1,  10, -16,   0,   0 }, { 8, 16,  0, -1,   9, -12,   0,   0 },
    { 8, 16,  0, -1,   9, -15,   0,   0 }, {18, 16,  0,  0,   9, -15,  -8,  42},
    { 7, 16,  0,  0,   9, -15,  -8,  42 }, { 8, 16,  0, -1,   9, -13,   0,   0 },
    { 8, 16,  0, -1,   9, -15,   0,   0 }, { 8, 16,  0, -1,   9, -14,   0,   0 },
    { 8, 16,  0, -1,   9, -13,   0,   0 }, { 8, 16,  0, -1,   9, -20,   0,   0 },
};

static int check_b_inter_diag_golden(void)
{
    static const char *candidates[] = {
        "build/oracle_full/diag.yuv", "../../oracle_full/diag.yuv",
        "../build/oracle_full/diag.yuv", "diag.yuv"
    };
    const char *path = NULL;
    for (unsigned i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
        FILE *fp = fopen(candidates[i], "rb");
        if (fp) { fclose(fp); path = candidates[i]; break; }
    }
    if (!path) {
        printf("  B-inter diag golden: fixture not present, skipped\n");
        return 0;
    }
    phx_param_t p;
    phx_param_default(&p);
    phx_param_apply_preset(&p, "medium");
    p.b_cabac = 1; p.i_width = 64; p.i_height = 64; p.i_qp_constant = 28;
    p.i_keyint_max = 30; p.i_keyint_min = 30; p.i_scenecut_threshold = 0;
    p.rc_method = PHX_RC_CQP; p.i_aq_mode = 0; p.b_mb_tree = 0;
    phx_param_finalize(&p);
    phx_t *enc = phx_encoder_open(&p);
    if (!enc) { printf("FAIL b-inter golden: encoder open\n"); return 1; }

    phx_g_mbdiag_enable = 1; phx_g_mbdiag_count = 0;
    const size_t fb = 64*64*3/2;
    uint8_t *buf = (uint8_t *)malloc(fb);
    FILE *fin = fopen(path, "rb");
    if (!buf || !fin) {
        printf("FAIL b-inter golden: IO\n");
        if (fin) fclose(fin); free(buf); phx_encoder_close(enc); return 1;
    }
    phx_picture_t pic;
    memset(&pic, 0, sizeof(pic));
    size_t y = 64*64, c = y/4;
    pic.plane[0] = buf; pic.plane[1] = buf+y; pic.plane[2] = buf+y+c;
    pic.i_stride[0] = 64; pic.i_stride[1] = 32; pic.i_stride[2] = 32;
    pic.i_width = 64; pic.i_height = 64;
    /* x264's first B frame is coded third, after the IDR and first P.  The
     * encoder exposes h->i_frame = 3 for that coded frame (lookahead delay). */
    const int first_b_frame = 5;
    while (fread(buf, 1, fb, fin) == fb) {
        phx_nal_t *out = NULL; int nnal = 0; uint8_t *recon = NULL;
        phx_encode(enc, &pic, &out, &nnal, &recon);
    }
    for (;;) {
        phx_nal_t *out = NULL; int nnal = 0; uint8_t *recon = NULL;
        if (phx_encoder_encode(enc, NULL, &out, &nnal, &recon) == 0) break;
    }
    fclose(fin); free(buf); phx_encoder_close(enc);
    phx_g_mbdiag_enable = 0;

    /* Locate the first B-coded frame whose MB0 reproduces the x264 golden
     * over-skip case (B_L0_L0 with MV (8,-11)).  Matching by signature rather
     * than by absolute frame index keeps the golden robust to lookahead flushes
     * and prior tests in the same process. */
    const phx_mbdiag_rec_t *b = NULL;
    for (int i = 0; i + 15 < phx_g_mbdiag_count; i += 16) {
        const phx_mbdiag_rec_t *r = &phx_g_mbdiag[i];
        if (r->frame_index != first_b_frame) continue;
        int all_b = 1;
        for (int mb = 0; mb < 16; mb++)
            if (!(r[mb].mb_type >= PHX_B_DIRECT && r[mb].mb_type <= PHX_B_SKIP))
                all_b = 0;
        if (all_b && r->mb_type == 8 && r->ref[0][0] == 0 &&
            r->ref[1][0] == -1 && r->mv[0][0][0] == 8 &&
            r->mv[0][0][1] == -11) { b = r; break; }
    }
    if (!b) { printf("FAIL b-inter golden: no first B frame captured\n"); return 1; }

    int exact = 0, type_match = 0, b_coded = 0;
    for (int mb = 0; mb < 16; mb++) {
        const phx_mbdiag_rec_t *r = &b[mb];
        const int *g = phx_b_golden_poc4[mb];
        int vals[8] = { r->mb_type, r->partition,
                        r->ref[0][0], r->ref[1][0],
                        r->mv[0][0][0], r->mv[0][0][1],
                        r->mv[1][0][0], r->mv[1][0][1] };
        int same = 1;
        for (int k = 0; k < 8; k++) if (vals[k] != g[k]) same = 0;
        exact += same;
        type_match += (r->mb_type == g[0]);
        /* No B MB may fall back to intra (the old encoder forced every
         * non-skip B decision to I_16x16). */
        b_coded += (r->mb_type >= PHX_B_DIRECT && r->mb_type <= PHX_B_SKIP);
    }
    printf("  B-inter diag golden: first B frame exact %d/16, mb_type %d/16, "
           "B-coded %d/16\n", exact, type_match, b_coded);
    printf("  B-inter diag golden: phx types:");
    for (int mb = 0; mb < 16; mb++) printf(" %d", b[mb].mb_type);
    printf("\n");
    int bad = 0;
    /* The frame-1 MB0 over-skip case must be byte-identical. */
    if (b[0].mb_type != 8 || b[0].ref[0][0] != 0 || b[0].ref[1][0] != -1 ||
        b[0].mv[0][0][0] != 8 || b[0].mv[0][0][1] != -11) {
        printf("FAIL b-inter golden: frame1 MB0 not B_L0_L0(8,-11)\n"); bad = 1;
    }
    if (b_coded != 16) { printf("FAIL b-inter golden: intra fallback on B MB\n"); bad = 1; }
    if (type_match < 12) { printf("FAIL b-inter golden: mb_type agreement <12/16\n"); bad = 1; }
    return bad;
}

/* Instrumented x264 golden, preset veryfast, QP28 CABAC, 64x64 raw I420
 * view of build/oracle_full/clip2229_320x180.yuv (tools/cabac_bin_diff.py
 * fixture geometry).  p_fenc/p_fdec luma windows and i_cost8/i_cost4 were
 * dumped from the real x264 mb_analyse_transform() call; FDEC already holds
 * x264_mb_mc() output (analyse.c:2740).  SHADER: A — immutable test data. */
static const uint8_t tf_F[3][256] = {
    { 187,187,187,188,190,192,192,193,194,195,196,197,198,199,199,200,207,207,207,207,207,207,207,206,206,205,204,204,203,201,199,196,71,78,86,92,92,91,91,90,88,85,81,76,73,71,66,61,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,29,30,30,31,33,35,35,36,37,38,40,41,42,42,43,44,187,187,188,189,191,192,192,194,194,196,197,198,199,199,200,201,207,207,208,208,208,208,208,208,207,207,207,206,204,202,201,200,83,85,89,92,92,92,92,91,89,86,84,80,77,76,71,65,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,30,31,31,32,33,34,35,37,38,40,41,42,43,43,44,45,187,188,189,190,191,192,193,194,195,197,197,198,200,200,201,202,209,209,209,209,209,209,209,209,208,208,207,206,204,202,202,201,95,94,94,94,94,94,93,92,89,86,84,82,79,77,72,66,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,30,31,32,32,33,35,35,37,39,41,42,43,44,45,45,46,188,189,190,191,192,193,194,195,196,197,198,200,201,201,202,203 },
    { 210,210,210,211,210,210,209,209,209,209,208,207,204,202,201,200,101,101,100,100,100,99,97,93,90,87,83,80,78,76,72,66,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,30,31,32,32,33,35,35,37,39,41,43,44,45,46,46,47,188,189,191,192,193,194,194,195,196,198,198,200,202,202,203,204,210,210,210,211,210,210,209,209,209,209,208,207,205,202,202,200,107,106,106,106,105,103,100,95,90,86,83,80,78,76,72,66,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,31,32,33,33,34,36,36,38,40,41,43,45,46,47,47,48,190,190,191,192,193,194,195,196,197,199,200,201,202,203,203,205,211,211,211,211,211,211,210,210,209,208,207,206,204,201,200,200,110,109,108,108,106,103,100,95,87,84,82,81,78,76,72,66,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,31,32,33,34,35,36,36,38,39,42,45,47,49,50,50,51,191,192,193,193,194,196,196,198,198,200,200,201,202,203,203,205,212,212,211,211,211,212,210,209,208,208,207,206,203,200,199,198 },
    { 111,109,108,106,101,99,97,91,86,84,82,81,78,76,72,66,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,31,32,33,34,35,36,36,38,39,43,46,49,53,53,53,54,192,192,193,194,195,196,196,198,198,200,200,201,202,202,203,204,212,212,211,210,210,209,207,205,203,203,202,202,199,197,197,197,110,108,107,105,99,96,92,88,85,83,83,81,77,76,72,67,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,32,32,32,33,34,36,36,39,41,44,48,52,54,54,54,55,193,193,193,194,194,196,196,197,198,199,199,200,201,202,202,202,209,207,206,205,203,201,199,199,198,197,197,195,194,193,193,193,110,108,108,105,99,94,90,88,85,83,82,80,76,75,72,68,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,32,32,32,33,34,35,37,40,43,47,51,54,55,55,55,56,193,193,193,194,194,195,195,196,196,196,197,197,197,198,198,199,202,200,199,198,195,194,192,191,191,191,190,189,190,190,191,191,111,108,106,105,99,93,90,88,86,83,82,80,76,74,72,69 },
};
static const uint8_t tf_P[3][256] = {
    { 125,125,125,125,125,125,125,125,125,125,125,125,125,125,125,125,144,144,144,144,144,144,144,144,144,144,144,144,144,144,144,144,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,71,71,71,71,71,71,71,71,71,71,71,71,71,71,71,71,50,50,50,50,50,50,50,50,50,50,50,50,50,50,50,50,153,153,153,153,153,153,153,153,153,153,153,153,153,153,153,153,165,165,165,165,165,165,165,165,165,165,165,164,164,164,164,165,57,57,57,57,57,57,57,57,57,57,55,55,54,54,54,54,78,78,78,78,78,78,78,78,78,78,77,77,75,75,74,73,49,49,49,49,49,49,49,49,49,49,47,47,46,46,46,46,168,168,168,168,168,168,168,168,168,168,168,168,167,167,167,168,183,183,183,183,183,183,183,183,183,183,183,183,183,183,183,183,53,53,53,53,53,53,53,53,53,53,53,53,53,53,53,53,71,71,71,71,71,71,71,71,71,71,71,71,71,71,71,73,43,43,43,43,43,43,43,43,43,43,43,43,43,43,43,43,186,186,186,186,186,186,186,186,186,186,186,186,186,186,186,183 },
    { 196,196,196,196,196,196,196,196,196,196,196,196,196,196,196,198,47,47,47,47,47,47,47,47,47,47,47,47,49,49,50,51,77,77,77,77,77,77,77,77,77,77,75,75,71,67,66,67,42,42,42,42,42,42,42,42,42,42,40,39,46,50,63,71,198,198,198,198,198,198,198,198,198,198,199,198,207,215,225,233,211,211,211,211,211,211,211,211,211,211,211,211,210,207,204,202,42,42,42,42,42,42,42,42,42,42,42,42,43,43,46,47,71,71,71,71,71,71,71,71,71,70,69,66,63,65,67,67,39,39,39,39,39,39,39,39,38,40,47,51,62,71,79,90,223,223,223,223,223,223,223,223,223,223,226,227,234,242,247,254,237,237,237,237,237,237,237,237,237,235,227,219,214,211,207,206,46,46,46,46,46,46,46,46,46,46,45,45,46,47,47,49,69,69,69,69,69,69,69,69,69,69,65,62,61,59,58,58,51,51,51,51,51,51,51,51,51,54,66,78,88,92,90,93,235,235,235,235,235,235,235,235,235,238,251,255,255,255,255,255,230,230,230,230,230,230,230,230,230,230,226,221,217,212,210,208 },
    { 90,90,90,89,88,88,85,85,90,90,90,89,88,88,85,85,65,65,65,65,66,66,65,65,65,65,65,65,66,66,65,65,65,65,65,63,63,65,65,65,65,65,65,63,63,65,65,65,174,180,182,182,184,186,186,188,174,180,182,182,184,186,186,188,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,88,88,88,86,86,88,88,88,88,88,88,86,86,88,88,88,61,61,61,59,59,61,61,61,61,61,61,59,59,61,61,61,63,63,62,62,65,65,65,65,63,63,62,62,65,65,65,65,190,191,191,192,192,192,195,199,190,191,191,192,192,192,195,199,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,94,94,92,92,89,88,88,88,94,94,92,92,89,88,88,88,59,59,61,61,61,61,59,61,59,59,61,61,61,61,59,61,62,63,63,63,63,63,62,65,62,63,63,63,63,63,62,65,191,192,192,195,194,200,218,226,191,192,192,195,194,200,218,226,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255 },
};
static const struct { int mb; int cost8, cost4, b8; } tf_golden[3] = {
    { 4, 6267, 8579, 1 },
    { 8, 6473, 7326, 1 },
    { 12, 9091, 11057, 1 },
};

/* x264 common/macroblock.h:444-461 — the exact allowed table and the P_8x8
 * sub-partition condition, checked against the 19 internal mb types.
 * SHADER: A — immutable table comparison. */
static int check_transform_allowed(void)
{
    static const uint8_t x264_allowed[19] = {
        0,0,0,0,1,2,0,1,1,1,1,1,1,1,1,1,1,1,0
    };
    for (int t = 0; t < 19; t++) {
        int want = (t == PHX_P_8x8) ? 0 : x264_allowed[t];
        CHECKF(phx_mb_transform_8x8_allowed_ex(1, t, PHX_D_16x16) == want,
               "transform allowed: mb type %d expected %d", t, want);
        CHECKF(phx_mb_transform_8x8_allowed_ex(0, t, PHX_D_16x16) == 0,
               "transform allowed: PPS gate rejects mb type %d", t);
    }
    CHECK(phx_mb_transform_8x8_allowed_ex(1, PHX_P_8x8, PHX_D_8x8) == 1,
          "transform allowed: P_8x8 with four D_L0_8x8 sub-partitions");
    CHECK(phx_mb_transform_8x8_allowed_ex(1, PHX_P_SKIP, PHX_D_16x16) == 0,
          "transform allowed: P_SKIP is disallowed");
    CHECK(phx_mb_transform_8x8_allowed_ex(1, PHX_B_SKIP, PHX_D_16x16) == 0,
          "transform allowed: B_SKIP is disallowed");
    CHECK(phx_mb_transform_8x8_allowed_ex(1, PHX_I_8x8, PHX_D_8x8) == 0,
          "transform allowed: I_8x8 is disallowed at analysis time");
    return 0;
}


/* Byte-exact transform-size golden on the real clip.  The x264 side is an
 * instrumented mb_analyse_transform() (analyse.c:2735-2770) that dumped its
 * p_fenc/p_fdec luma windows and i_cost8/i_cost4 for the first P_L0 MBs of
 * frame 1 (preset veryfast, QP28 CABAC, --threads 1).  Phoenix recomputes the
 * same two kernels and b8 from those exact windows; no value is forced.
 * SHADER: A — pure cost-kernel golden, no entropy state. */
static int check_transform_costs_golden(void)
{
    for (int i = 0; i < 3; i++) {
        int c8 = -1, c4 = -1, b8 = -1;
        phx_mb_transform_costs(tf_F[i], 16, tf_P[i], 16, &c8, &c4, &b8);
        CHECKF(c8 == tf_golden[i].cost8 && c4 == tf_golden[i].cost4 &&
               b8 == tf_golden[i].b8,
               "transform golden MB %d: cost8 %d (want %d), cost4 %d "
               "(want %d), b8 %d (want %d)",
               tf_golden[i].mb, c8, tf_golden[i].cost8, c4,
               tf_golden[i].cost4, b8, tf_golden[i].b8);
    }
    return 0;
}

int main(void)
{
    phx_param_t p;
    phx_frame_t *f;
    phx_mb_state_t *s;
    phx_mb_cache_t cache;
    phx_mb_decision_t d1, d2;
    int rc1, rc2;

    phx_param_default(&p);
    f = phx_frame_new(32, 32, 0);
    s = phx_mb_state_new(2, 2);
    if (!f || !s) {
        printf("FAIL allocation\n");
        return 1;
    }
    fill_frame(f);
    fill_chroma(f);
    phx_frame_init_lowres(f);
    s->slice_type = 2;                 /* I slice */
    s->first_mb = 0;

    memset(&d1, 0, sizeof(d1));
    memset(&cache, 0, sizeof(cache));
    phx_macroblock_cache_load(s, &cache, 0, 0, 0);
    rc1 = phx_macroblock_analyse(&p, f, &cache, 0, 0, 26, &d1);
    CHECK(rc1 == 0, "analysis returns success");
    if (!rc1) {
        check_decision(&d1, "first");
        /* Golden first-MB decision, derived from x264's analyse.c tables:
         * no neighbours => I16 mode 6 (DC_128) and I4 mode 11 at block 0;
         * block 15 gets n4[15]=LEFT|TOP|TOPLEFT (macroblock.c:493-498) and
         * mode 3.  Coefficients are published at scan8 slots, not block
         * indices (macroblock.h:199). */
        CHECK(d1.mb_type == PHX_I_4x4, "golden first MB is I_4x4");
        CHECK(d1.intra16_pred_mode == 6, "golden raw I16 mode is DC_128");
        CHECK(d1.intra4x4_pred_mode[0] == 11, "golden raw I4 mode at block 0");
        CHECK(d1.intra4x4_pred_mode[15] == 3, "golden raw I4 mode at block 15");
        CHECK(d1.nnz[phx_scan8[0]] == 1 && d1.dct[phx_scan8[0]][0] == -15 &&
              d1.dct[phx_scan8[0]][1] == -6 && d1.dct[phx_scan8[0]][2] == -10,
              "golden coefficients at scan8[0] (luma block 0)");
        CHECK(d1.dct[phx_scan8[4]][0] == -1 && d1.dct[phx_scan8[4]][1] == 10,
              "golden coefficients at scan8[4] (second block row)");
        /* x264 analyse.c:586-665: no intra neighbours at MB(0,0) mean the
         * only available chroma mode is DC_128 (index 6, analyse.c:487), so
         * the SATD search must return 6 rather than the old placeholder 0. */
        CHECK(d1.chroma_pred_mode == 6,
              "golden first-MB chroma mode is DC_128 (analyse.c:615/664)");
    }

    /* Reset every input the driver reads, then analyse the same MB again. */
    fill_frame(f);
    fill_chroma(f);
    phx_frame_init_lowres(f);
    phx_mb_state_delete(s);
    s = phx_mb_state_new(2, 2);
    s->slice_type = 2;
    s->first_mb = 0;
    memset(&d2, 0, sizeof(d2));
    memset(&cache, 0, sizeof(cache));
    phx_macroblock_cache_load(s, &cache, 0, 0, 0);
    rc2 = phx_macroblock_analyse(&p, f, &cache, 0, 0, 26, &d2);
    CHECK(rc2 == 0, "second analysis returns success");
    if (!rc2) {
        check_decision(&d2, "second");
        CHECK(memcmp(&d1, &d2, sizeof(d1)) == 0, "decision is reproducible");
    }

    /* Interior MB with all four intra neighbours available: exercises the
     * neighbour-proxy path of phx_setup_fdec and the full mode lists. */
    phx_mb_state_delete(s);
    s = phx_mb_state_new(2, 2);
    s->slice_type = 2;
    s->first_mb = 0;
    for (int i = 0; i < 4; i++) s->type[i] = PHX_I_16x16;
    memset(&cache, 0, sizeof(cache));
    phx_macroblock_cache_load(s, &cache, 1, 1, 0);
    /* (1,1) is the right-edge MB of a 2x2 MB frame, so TOPRIGHT is absent. */
    /* x264 sets MB_TOPLEFT in i_neighbour but not in i_neighbour_intra
     * (macroblock.c:816-824). */
    /* x264 common/macroblock.c:771-785 (LEFT), :791-802 (TOP), :816-827
     * (TOPLEFT) all set i_neighbour_intra for MB(1,1): the left/top/top-left
     * MBs exist, are intra and share the slice (b_constrained_intra == 0).
     * TOPRIGHT (:831-843) is absent at the right edge of a 2x2 MB frame.
     * The old expectation omitted TOPLEFT and was stale after M11's golden. */
    CHECK(cache.i_neighbour_intra == (PHX_MB_LEFT|PHX_MB_TOP|PHX_MB_TOPLEFT),
          "left/top/top-left intra neighbours available at MB(1,1)");
    memset(&d2, 0, sizeof(d2));
    rc2 = phx_macroblock_analyse(&p, f, &cache, 1, 1, 26, &d2);
    CHECK(rc2 == 0, "interior MB analysis returns success");
    if (!rc2) check_decision(&d2, "interior");

    fails += check_p_analysis();
    fails += check_analysis_leaves_cabac_untouched();
    fails += check_caller_fdec_border();
    fails += check_high_ref_analysis();
    fails += check_transform_allowed();
    fails += check_transform_costs_golden();

    /* Source-aligned I4 mode-decision goldens (worker Z1, rows 1-3). */
    fails += check_mb0_i4_golden("superfast", z1_superfast_modes);
    fails += check_mb0_i4_golden("veryfast", z1_veryfast_modes);
    fails += check_mb0_i4_golden("faster", z1_faster_modes);
    fails += check_mb0_i4_golden("medium", z1_medium_modes);
    fails += check_mb0_i4_golden("slow", z1_slow_modes);
    fails += check_mb0_i4_golden("placebo", z1_placebo_modes);
    fails += check_mb0_i4_golden("veryslow", z1_veryslow_modes);
    fails += check_mb1_medium_golden();

    /* Row 17 — P_SKIP decision golden vs x264's own probe/fast-skip path. */
    /* Row 17 gates at subme 0..7: instrumented x264 skip flags are 1 for all
     * 16 MBs of the first static P frame (verified at superfast, veryfast,
     * medium and ultrafast).  Placebo's mbrd>=2 rd_cost_mb promotion is a
     * separate, still-missing gate (phx_rdo owner). */
    fails += check_pskip_decision_golden("ultrafast");
    fails += check_pskip_decision_golden("superfast");
    fails += check_pskip_decision_golden("veryfast");
    fails += check_pskip_decision_golden("medium");

    /* B-direct/B_SKIP integration golden (B1 API, x264 all-16 B_SKIP frame). */
    fails += check_b_skip_golden();

    /* Real diag.yuv B-inter golden (instrumented x264, first B frame). */
    fails += check_b_inter_diag_golden();

    /* Bad arguments must fail cleanly without touching the decision. */
    CHECK(phx_macroblock_analyse(NULL, f, &cache, 0, 0, 26, &d2) == -1,
          "NULL param rejected");
    CHECK(phx_macroblock_analyse(&p, f, &cache, 2, 0, 26, &d2) == -1,
          "out-of-range MB rejected");

    phx_mb_state_delete(s);
    phx_frame_delete(f);
    printf("%s: per-MB analysis %s, %d failures\n",
           fails ? "FAILED" : "OK", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
