/* Standalone test for the phase-B macroblock encoder.
 * Exercises the two supported intra paths with deterministic pixels, checks
 * that every d->nnz/cache nnz flag agrees with the quantized coefficients,
 * and proves that two identical runs produce byte-identical decisions and
 * reconstructed samples.
 *
 * Source checks:
 *   reference/x264/encoder/macroblock.c:115-121 (nnz correctness rule)
 *   reference/x264/encoder/macroblock.h:112-117 (STORE_8x8_NNZ)
 *   reference/x264/encoder/macroblock.c:974 (x264_macroblock_encode)
 *
 * SHADER: B — verifies the B-stage B->C handoff record.
 */
#include "phx_mbencode.h"
#include "phx_analysis.h"
#include "phx_chroma.h"
#include "phx_param.h"
#include <stdio.h>
#include <string.h>

#define TEST_W 16
#define TEST_H 16
#define TEST_QP 26

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

/* Deterministic test fixture pixels; not a ported x264 function.  It stands
 * in for the fenc plane of reference/x264/common/frame.c:261.
 * SHADER: B — B-stage input fixture. */
static uint8_t phx_test_pixel(int x, int y)
{
    return (uint8_t)(x * 37 + y * 91 + (x + y) * 13 + ((x * y) & 15) * 7);
}

/* Fill the coded luma area (common/frame.h:37 plane layout).
 * SHADER: B — B-stage input fixture. */
static void phx_test_fill(phx_frame_t *f)
{
    for (int y = 0; y < TEST_H; y++)
        for (int x = 0; x < TEST_W; x++)
            f->plane[y * f->i_stride + x] = phx_test_pixel(x, y);
}

/* FNV-1a over the reconstructed 16x16 block written at encoder/macroblock.c:632.
 * SHADER: B — checksum of B-stage output bytes. */
static uint32_t phx_test_hash(const uint8_t *p, int stride)
{
    uint32_t h = 2166136261u;
    for (int y = 0; y < TEST_H; y++)
        for (int x = 0; x < TEST_W; x++) {
            h ^= p[y * stride + x];
            h *= 16777619u;
        }
    return h;
}

/* Verify the encoder/macroblock.c:115-121 nnz contract: every coefficient
 * block with any nonzero quantized coefficient has nnz==1, every empty block
 * has nnz==0, and the cache slot carries the same flag (encoder/macroblock.h:112).
 * SHADER: B — validation of the B-stage nnz output. */
static int phx_test_check_nnz(const phx_mb_decision_t *d,
                              const phx_mb_cache_t *c, int mb_type)
{
    int nz_blocks = 0;
    for (int b = 0; b < 16; b++) {
        const int slot = phx_scan8[b];
        int nonzero = 0;
        for (int i = 0; i < 16; i++)
            if (d->dct[slot][i] != 0) nonzero = 1;
        const int expect = nonzero ? 1 : 0;
        if (d->nnz[slot] != expect)
            printf("FAIL nnz block %d slot %d: got %d want %d\n",
                   b, slot, d->nnz[slot], expect), ++fails;
        if (c->non_zero_count[slot] != (uint8_t)expect)
            printf("FAIL cache nnz block %d slot %d: got %d want %d\n",
                   b, slot, c->non_zero_count[slot], expect), ++fails;
        nz_blocks += expect;
    }
    const int dc_slot = phx_scan8[48];
    int dc_nonzero = 0;
    for (int i = 0; i < 16; i++)
        if (d->dct[dc_slot][i] != 0) dc_nonzero = 1;
    const int dc_expect = (mb_type == PHX_I_16x16 && dc_nonzero) ? 1 : 0;
    if (d->nnz[dc_slot] != dc_expect)
        printf("FAIL nnz luma DC: got %d want %d\n", d->nnz[dc_slot], dc_expect), ++fails;
    if (c->non_zero_count[dc_slot] != (uint8_t)dc_expect)
        printf("FAIL cache nnz luma DC: got %d want %d\n",
               c->non_zero_count[dc_slot], dc_expect), ++fails;
    return nz_blocks + dc_expect;
}

/* One full encode of MB (0,0) through the real cache loader
 * (common/macroblock.c:855) and encoder entry point (encoder/macroblock.c:974).
 * SHADER: B — drives one B-stage invocation. */
static void phx_test_run(int mb_type, phx_mb_decision_t *out_d,
                         phx_mb_cache_t *out_c, uint8_t *out_recon,
                         int *out_nz, uint32_t *out_hash, int *out_diffs)
{
    phx_param_t p;
    phx_param_default(&p);
    p.b_cabac = 1;

    phx_frame_t *f = phx_frame_new(TEST_W, TEST_H, 0);
    CHECK(f != NULL, "phx_frame_new");
    if (!f) return;
    phx_test_fill(f);
    uint8_t before[TEST_W * TEST_H];
    for (int y = 0; y < TEST_H; y++)
        memcpy(before + y * TEST_W, f->plane + y * f->i_stride, TEST_W);

    phx_mb_state_t *s = phx_mb_state_new(1, 1);
    CHECK(s != NULL, "phx_mb_state_new");
    if (!s) { phx_frame_delete(f); return; }
    s->slice_type = 2; /* SLICE_TYPE_I, common/base.h */
    s->first_mb = 0;

    phx_mb_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    phx_macroblock_cache_load(s, &cache, 0, 0, 0);

    phx_mb_decision_t d;
    memset(&d, 0, sizeof(d));
    d.mb_type = mb_type;
    d.i_partition = 16; /* D_16x16, common/macroblock.h:139 */
    d.i_qp = TEST_QP;
    if (mb_type == PHX_I_16x16)
        d.intra16_pred_mode = 3; /* I_PRED_16x16_P, predict.c:132 */
    else if (mb_type == PHX_I_4x4)
        for (int i = 0; i < 16; i++)
            d.intra4x4_pred_mode[i] = (int8_t)((i * 3 + 1) % 12);
    d.chroma_pred_mode = 0;

    CHECK(phx_macroblock_encode(&p, f, &cache, 0, 0, &d) == 0,
          mb_type == PHX_I_16x16 ? "encode I16" : "encode I4");

    /* Golden B->C handoff values, source-derived from x264's
     * block_idx_xy_fenc/fdec (common/macroblock.h:225-246), predict_4x4_v_c
     * (common/predict.c:508-511) and x264_mb_encode_i4x4
     * (encoder/macroblock.h:132-166).  These slots are at block rows 1 and 3,
     * so they fail with the old one-pixel-row offset tables. */
    if (mb_type == PHX_I_4x4) {
        static const int16_t golden_b4[16] = {
            -14, -5, 13, 5, 3, -12, 5, 0, 3, 17, 11, -2, 0, 1, -8, 5
        };
        static const int16_t golden_b15[16] = {
            -6, -1, 2, 8, -5, -2, 3, 7, -3, 4, -14, -12, -3, -1, -4, -7
        };
        CHECK(memcmp(d.dct[phx_scan8[4]], golden_b4, sizeof(golden_b4)) == 0,
              "I4 golden coefficients at block 4 (second block row)");
        CHECK(memcmp(d.dct[phx_scan8[15]], golden_b15, sizeof(golden_b15)) == 0,
              "I4 golden coefficients at block 15 (last block row)");
    }

    *out_nz = phx_test_check_nnz(&d, &cache, mb_type);
    CHECK(*out_nz > 0, "at least one quantized coefficient block");
    CHECK(d.cbp_luma != 0, "cbp_luma set");
    CHECK((d.i_cbp & 0xf) == d.cbp_luma, "i_cbp low bits");
    if (mb_type == PHX_I_16x16)
        CHECK(d.i_cbp == (d.cbp_luma
                          | (p.b_cabac ? (int)d.nnz[phx_scan8[48]] << 8 : 0)),
              "i_cbp includes CABAC luma DC flag");
    CHECK(cache.i_cbp_luma == d.cbp_luma, "cache cbp_luma");

    *out_diffs = 0;
    for (int y = 0; y < TEST_H; y++)
        for (int x = 0; x < TEST_W; x++) {
            const uint8_t v = f->plane[y * f->i_stride + x];
            out_recon[y * TEST_W + x] = v;
            if (v != before[y * TEST_W + x]) ++(*out_diffs);
        }
    CHECK(*out_diffs > 0, "reconstruction changed source pixels");

    *out_hash = phx_test_hash(f->plane, f->i_stride);
    *out_d = d;
    *out_c = cache;

    phx_mb_state_delete(s);
    phx_frame_delete(f);
}

/* Run both modes twice and compare every decision/reconstruction byte.
 * SHADER: B — determinism check on the B-stage output. */
static void phx_test_case(int mb_type, const char *name)
{
    phx_mb_decision_t d1, d2;
    phx_mb_cache_t c1, c2;
    uint8_t r1[TEST_W * TEST_H], r2[TEST_W * TEST_H];
    int nz1 = 0, nz2 = 0, diff1 = 0, diff2 = 0;
    uint32_t h1 = 0, h2 = 0;

    phx_test_run(mb_type, &d1, &c1, r1, &nz1, &h1, &diff1);
    phx_test_run(mb_type, &d2, &c2, r2, &nz2, &h2, &diff2);

    CHECK(memcmp(&d1.dct, &d2.dct, sizeof(d1.dct)) == 0, "dct reproducible");
    CHECK(memcmp(d1.nnz, d2.nnz, sizeof(d1.nnz)) == 0, "nnz reproducible");
    CHECK(memcmp(r1, r2, sizeof(r1)) == 0, "reconstruction reproducible");
    CHECK(h1 == h2, "reconstruction hash reproducible");
    CHECK(nz1 == nz2, "nnz count reproducible");
    CHECK(d1.i_cbp == d2.i_cbp, "cbp reproducible");

    printf("%s: nz_blocks=%d diff_pixels=%d recon_hash=%08x\n",
           name, nz1, diff1, h1);
}

/* End-to-end B-stage handoff for the medium/cabac first MB: analyse writes
 * the real chroma_pred_mode, encode writes luma, encode_chroma writes the
 * chroma coefficients/nnz/CBP.  Golden state: mode 6, cbp_chroma 2, DC arrays
 * [15,0,-9,0] for U and V (encoder/macroblock.c:425-426/:457/:477/:487-489).
 * SHADER: B — verifies the explicit analysis -> encode -> chroma handoff. */
static void phx_test_first_mb_handoff(void)
{
    static const int16_t golden_dc[4] = { 15, 0, -9, 0 };
    phx_mb_decision_t d1, d2;
    phx_mb_cache_t c1, c2;

    for (int rep = 0; rep < 2; rep++) {
        phx_param_t p;
        phx_param_default(&p);
        phx_param_apply_preset(&p, "medium");
        p.b_cabac = 1;
        phx_param_finalize(&p);        /* chroma QP offset -2 at subme>=6 */

        phx_frame_t *f = phx_frame_new(TEST_W, TEST_H, 0);
        CHECK(f != NULL, "handoff frame allocation");
        if (!f) return;
        for (int y = 0; y < f->i_height; y++)
            for (int x = 0; x < f->i_width; x++)
                f->plane[y*f->i_stride + x] =
                    (uint8_t)((x*17 + y*29 + 11) & 0xff);
        const int s = f->i_stride_planes[1];
        for (int y = 0; y < f->i_lines_planes[1]; y++)
            for (int x = 0; x < f->i_width_planes[1]; x++) {
                const uint8_t v = (uint8_t)(128 + 5*(x & 7));
                f->plane_chroma[y*s + 2*x + 0] = v;
                f->plane_chroma[y*s + 2*x + 1] = v;
            }
        phx_frame_init_lowres(f);

        phx_mb_state_t *st = phx_mb_state_new(1, 1);
        CHECK(st != NULL, "handoff MB state");
        if (!st) { phx_frame_delete(f); return; }
        st->slice_type = 2;
        st->first_mb = 0;
        phx_mb_cache_t *c = rep ? &c2 : &c1;
        phx_mb_decision_t *d = rep ? &d2 : &d1;
        memset(c, 0, sizeof(*c));
        phx_macroblock_cache_load(st, c, 0, 0, 0);

        memset(d, 0, sizeof(*d));
        d->i_qp = 22;                  /* chroma QP 20 = qp_idx 32 (base.h) */
        CHECK(phx_macroblock_analyse(&p, f, c, 0, 0, d->i_qp, d) == 0,
              "handoff analysis");
        CHECK(d->chroma_pred_mode == 6, "analysis yields DC_128 mode 6");
        /* phx_mbencode.c rejects I_8x8; the encoder applies the same guard. */
        if (d->mb_type == PHX_I_8x8) d->mb_type = PHX_I_16x16;
        d->b_transform_8x8 = 0;
        CHECK(phx_macroblock_encode(&p, f, c, 0, 0, d) == 0,
              "handoff luma encode");
        CHECK(phx_macroblock_encode_chroma(&p, f, c, 0, 0, d) == 0,
              "handoff chroma encode");

        CHECK(d->chroma_pred_mode == 6, "handoff retains mode 6");
        CHECK(d->cbp_chroma == 2, "handoff cbp_chroma == 2");
        for (int ch = 0; ch < 2; ch++)
            CHECK(memcmp(d->chroma_dct[ch][0], golden_dc,
                         sizeof(golden_dc)) == 0,
                  ch ? "handoff V DC golden" : "handoff U DC golden");
        CHECK((d->i_cbp & 0x30) == 0x20, "handoff i_cbp chroma bits");
        CHECK((d->i_cbp & 0x600) == 0x600, "handoff CABAC DC flags");
        phx_mb_state_delete(st);
        phx_frame_delete(f);
    }
    CHECK(memcmp(&d1, &d2, sizeof(d1)) == 0, "golden handoff reproducible");
    CHECK(memcmp(c1.non_zero_count, c2.non_zero_count,
                 sizeof(c1.non_zero_count)) == 0, "golden cache nnz reproducible");
    CHECK(c1.i_cbp_luma == c2.i_cbp_luma &&
          c1.i_cbp_chroma == c2.i_cbp_chroma, "golden cache cbp reproducible");
    printf("first-MB handoff: mode=%d cbp_chroma=%d U_DC=%d,%d V_DC=%d,%d\n",
           d1.chroma_pred_mode, d1.cbp_chroma,
           d1.chroma_dct[0][0][0], d1.chroma_dct[0][0][2],
           d1.chroma_dct[1][0][0], d1.chroma_dct[1][0][2]);
}

/* Standalone test driver: x264_macroblock_encode (encoder/macroblock.c:974)
 * exercised through both intra branches and checked against the nnz rule of
 * encoder/macroblock.c:115-121.
 * SHADER: B — B-stage verification entry point. */
int main(void)
{
    phx_test_case(PHX_I_16x16, "I16");
    phx_test_case(PHX_I_4x4, "I4");
    phx_test_first_mb_handoff();

    if (fails) {
        printf("FAIL %d check(s)\n", fails);
        return 1;
    }
    printf("PASS test_phx_mbencode\n");
    return 0;
}
