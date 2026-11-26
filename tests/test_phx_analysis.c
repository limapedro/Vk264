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
#include "phx_param.h"
#include "phx_frame.h"
#include "phx_mbcache.h"
#include "phx_mb_decision.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

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
        CHECK(d1.mv[0][0][0] == 8 && d1.mv[0][0][1] == 0,
              "P analysis found the known MV (2,0) in qpel units");
        CHECK(d1.cbp_luma == 0 && d1.cbp_chroma == 0,
              "P_L0 no-residual decision has cbp=0");
        CHECK(d1.skip == 0, "non-pskip MV is not a skip");
        for (int i = 0; i < 16; i++) {
            CHECK(d1.ref[0][i] == 0 && d1.ref[1][i] == -1,
                  "P_L0 ref list 0 = 0 for all 4x4 blocks");
            CHECK(d1.mv[0][i][0] == 8 && d1.mv[0][i][1] == 0,
                  "P_L0 MV replicated over all 4x4 blocks");
        }
        for (int k = 0; k < 48; k++)
            CHECK(d1.nnz[k] == 0 && !any_nonzero(d1.dct[k], 16),
                  "P_L0 publishes no coefficients");
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
    CHECK(cache.i_neighbour_intra == (PHX_MB_LEFT|PHX_MB_TOP),
          "left/top intra neighbours available at MB(1,1)");
    memset(&d2, 0, sizeof(d2));
    rc2 = phx_macroblock_analyse(&p, f, &cache, 1, 1, 26, &d2);
    CHECK(rc2 == 0, "interior MB analysis returns success");
    if (!rc2) check_decision(&d2, "interior");

    fails += check_p_analysis();

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
