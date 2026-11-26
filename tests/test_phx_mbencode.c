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
#include "phx_dct.h"
#include "phx_cabac_syntax.h"
#include "phx_mc.h"
#include <stdio.h>
#include <stdlib.h>
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
 * U=V={15,-9,0,0} after macroblock.c:35-41's zigzag_scan_2x2_dc() (this
 * fixture uses identical U/V samples; see the distinct-V golden below).
 * (encoder/macroblock.c:425-426/:457/:477/:487-489).  SHADER: B — verifies the
 * explicit analysis -> encode -> chroma handoff. */
static void phx_test_first_mb_handoff(void)
{
    /* This fixture fills U and V with the SAME samples
     * (v = 128 + 5*(x&7) into both interleaved bytes above), so both planes
     * quantize to the same zigzag DC array {15,-9,0,0}.  The distinct
     * V={15,0,-9,0} golden belongs to phx_test_first_mb_chroma_trellis(),
     * whose fixture uses V = 128+5*y. */
    /* x264's instrumented encoder/cabac.c DC hook at chroma QP 20 emits raw
     * {21,0,-12,0}; zigzag_scan_2x2_dc() (macroblock.c:35-41) hands CABAC
     * {21,-12,0,0} (phx_chroma.c:295-307 cites this exact golden).  The
     * {15,-9} values belonged to the trellis-QNS variant of this fixture. */
    static const int16_t golden_dc_u[4] = { 21, -12, 0, 0 };
    static const int16_t golden_dc_v[4] = { 21, -12, 0, 0 };

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
        CHECK(memcmp(d->chroma_dct[0][0], golden_dc_u,
                     sizeof(golden_dc_u)) == 0, "handoff U DC golden");
        CHECK(memcmp(d->chroma_dct[1][0], golden_dc_v,
                     sizeof(golden_dc_v)) == 0, "handoff V DC golden");
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
/* Golden first-MB chroma-DC handoff from tools/first_frame_probe.py's
 * synth fixture (preset medium, MB qp 25, chroma qp 23).  x264's instrumented
 * encoder produces natural 2x2 arrays U={15,0,-9,0}, V={-26,-23,-12,0}; after
 * macroblock.c:35-41's zigzag_scan_2x2_dc() the arrays handed to CABAC are
 * U={15,-9,0,0}, V={-26,-12,-23,0}.  SHADER: B — one MB, serial. */
static void phx_test_first_mb_chroma_trellis(void)
{
    static const uint8_t y0[16][16] = {
        {0,7,14,21,28,35,42,49,56,63,70,77,84,91,98,105},
        {13,20,27,34,41,48,55,62,69,76,83,90,97,104,111,118},
        {26,33,40,47,54,61,68,75,82,89,96,103,110,117,124,131},
        {39,46,53,60,67,74,81,88,95,102,109,116,123,130,137,144},
        {52,59,66,73,80,87,94,101,108,115,122,129,136,143,150,157},
        {65,72,79,86,93,100,107,114,121,128,135,142,149,156,163,170},
        {78,85,92,99,106,113,120,127,134,141,148,155,162,169,176,183},
        {91,98,105,112,119,126,133,140,147,154,161,168,175,182,189,196},
        {104,111,118,125,132,139,146,153,160,167,174,181,188,195,202,209},
        {117,124,131,138,145,152,159,166,173,180,187,194,201,208,215,222},
        {130,137,144,151,158,165,172,179,186,193,200,207,214,221,228,235},
        {143,150,157,164,171,178,185,192,199,206,213,220,227,234,241,248},
        {156,163,170,177,184,191,198,205,212,219,226,233,240,247,254,5},
        {169,176,183,190,197,204,211,218,225,232,239,246,253,4,11,18},
        {182,189,196,203,210,217,224,231,238,245,252,3,10,17,24,31},
        {195,202,209,216,223,230,237,244,251,2,9,16,23,30,37,44}
    };
    /* I420 planes of the real synth fixture's first MB
     * (tools/parity_harness.py:73-76): U = 128+5*x, V = 128+5*y.
     * These are the exact samples the x264 golden CABAC trace used. */
    static const uint8_t uv0[8][8] = {
        {128,133,138,143,148,153,158,163},
        {128,133,138,143,148,153,158,163},
        {128,133,138,143,148,153,158,163},
        {128,133,138,143,148,153,158,163},
        {128,133,138,143,148,153,158,163},
        {128,133,138,143,148,153,158,163},
        {128,133,138,143,148,153,158,163},
        {128,133,138,143,148,153,158,163}
    };
    static uint8_t vv0[8][8];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            vv0[y][x] = (uint8_t)(128 + 5 * y);
    phx_param_t p;
    phx_param_default(&p);
    phx_param_apply_preset(&p, "medium");
    p.b_cabac = 1;
    phx_param_finalize(&p);            /* chroma QP offset -2 (subme>=6) */
    /* analyse.c:3724 — final encode uses b_trellis = i_trellis, so medium
     * (trellis=1) still runs QNS; the golden trace below was produced with
     * that setting. */
    CHECK(p.i_trellis > 0, "medium preset enables trellis");

    phx_frame_t *f = phx_frame_new(TEST_W, TEST_H, 0);
    CHECK(f != NULL, "trellis handoff frame allocation");
    if (!f) return;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            f->plane[y*f->i_stride + x] = y0[y][x];
    const int s = f->i_stride_planes[1];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            f->plane_chroma[y*s + 2*x] = uv0[y][x];
            f->plane_chroma[y*s + 2*x + 1] = vv0[y][x];
        }
    phx_frame_init_lowres(f);

    phx_mb_state_t *st = phx_mb_state_new(1, 1);
    CHECK(st != NULL, "trellis handoff MB state");
    if (!st) { phx_frame_delete(f); return; }
    st->slice_type = 2; st->first_mb = 0;
    phx_mb_cache_t c;
    phx_mb_decision_t d;
    memset(&c, 0, sizeof(c));
    memset(&d, 0, sizeof(d));
    phx_macroblock_cache_load(st, &c, 0, 0, 0);
    d.i_qp = 25;
    CHECK(phx_macroblock_analyse(&p, f, &c, 0, 0, d.i_qp, &d) == 0,
          "trellis handoff analysis");
    if (d.mb_type == PHX_I_8x8) d.mb_type = PHX_I_4x4;
    d.chroma_pred_mode = 6;            /* x264 MB0 golden: chroma=6 */
    d.b_transform_8x8 = 0;

    phx_cabac_t cb;
    memset(&cb, 0, sizeof(cb));
    phx_cabac_context_init(&cb, PHX_SLICE_I, d.i_qp, 0);
    CHECK(phx_macroblock_encode_ex(&p, f, NULL, &c, 0, 0, &d, &cb) == 0,
          "trellis handoff luma encode");
    CHECK(phx_macroblock_encode_chroma(&p, f, &c, 0, 0, &d) == 0,
          "trellis handoff chroma encode");

    /* x264 instrumented encoder/cabac.c L1DC hook on this fixture:
     * U = {15,-9,0,0}, V = {15,0,-9,0} (zigzag_scan_2x2_dc, macroblock.c:35). */
    static const int16_t want_u[4] = { 15, -9, 0, 0 };
    static const int16_t want_v[4] = { 15, 0, -9, 0 };
    CHECK(memcmp(d.chroma_dct[0][0], want_u, sizeof(want_u)) == 0,
          "trellis handoff U DC golden");
    CHECK(memcmp(d.chroma_dct[1][0], want_v, sizeof(want_v)) == 0,
          "trellis handoff V DC golden");
    CHECK(d.chroma_nnz[0][0] == 1 && d.chroma_nnz[1][0] == 1,
          "trellis handoff chroma DC nnz");
    /* x264 stores the FULL 16-coefficient zigzag scan in h->dct.luma4x4[j]
     * (encoder/macroblock.c:391/402-415) and the entropy stage applies the
     * single +1 (cabac.c:1078).  Phoenix must do the same: U scan8 16..19
     * -> [3,-2,0,...] and V -> [3,0,-2,...], so CABAC sees [-2,0,...] and
     * [0,-2,...].  phx_encoder.c:505-508 maps the scan8 ranges to
     * chroma_dct[ch][1+i].  SHADER: B — B->C chroma handoff. */
    for (int i = 0; i < 4; i++) {
        CHECK(d.chroma_dct[0][1+i][0] == 0 && d.chroma_dct[0][1+i][1] == -2
              && d.chroma_dct[0][1+i][2] == 0,
              "trellis handoff U AC golden (scan8 16..19)");
        CHECK(d.chroma_dct[1][1+i][0] == 0 && d.chroma_dct[1][1+i][1] == 0
              && d.chroma_dct[1][1+i][2] == -2,
              "trellis handoff V AC golden (scan8 32..35)");
    }
    CHECK(c.non_zero_count[phx_scan8[49]] == 1 &&
          c.non_zero_count[phx_scan8[50]] == 1,
          "trellis handoff cache DC nnz");
    printf("trellis handoff: U=%d,%d V=%d,%d cbp=%d "
           "UAC=%d,%d,%d VAC=%d,%d,%d\n",
           d.chroma_dct[0][0][0], d.chroma_dct[0][0][1],
           d.chroma_dct[1][0][0], d.chroma_dct[1][0][1], d.cbp_chroma,
           d.chroma_dct[0][1][0], d.chroma_dct[0][1][1], d.chroma_dct[0][1][2],
           d.chroma_dct[1][1][0], d.chroma_dct[1][1][1], d.chroma_dct[1][1][2]);
    phx_mb_state_delete(st);
    phx_frame_delete(f);
}

/* Reference luma/chroma MC oracle for the B-stage integration tests.  The
 * primitives themselves are checked against the literal x264 formulas in
 * tests/test_phx_mc.c; here we verify that phx_mbencode.c wires the correct
 * planes, MV and chroma scaling into them (macroblock.c:654-681,
 * common/macroblock.c:61-64).  SHADER: B — test fixture oracle. */
static void phx_test_ref_mc(const phx_frame_t *ref, int mb_x, int mb_y,
                             int mvx, int mvy, uint8_t luma[256],
                             uint8_t chroma[128])
{
    const int s = ref->i_stride;
    const size_t bytes = (size_t)s*(ref->i_height + 16) + 16;
    const size_t off0 = 8*(size_t)s + 8;  /* mc.c:717 full-frame -8 offset */
    uint8_t *base = (uint8_t *)malloc(off0 + 3*bytes);
    uint8_t *hp = base + off0, *vp = hp + bytes, *cp = vp + bytes;
    int16_t *buf = (int16_t *)malloc((size_t)(ref->i_width + 32)*sizeof(int16_t));
    if (!base || !buf) {
        free(base); free(buf);
        printf("FAIL oracle allocation\n"), ++fails;
        return;
    }
    phx_mc_hpel_filter_frame(ref->plane, s, ref->i_width, ref->i_height,
                             hp, vp, cp, buf);
    const size_t off = (size_t)(mb_y*16)*s + (size_t)(mb_x*16);
    const uint8_t *planes[4] = { ref->plane + off, hp + off, vp + off, cp + off };
    phx_mc_luma(luma, 16, planes, s, mvx, mvy, 16, 16);

    uint8_t cu[64], cv[64];
    const int cs = ref->i_stride_planes[1];
    const uint8_t *csrc = ref->plane_chroma
                        + (size_t)(mb_y*8)*cs + (size_t)(mb_x*16);
    if (mvx | mvy)
        phx_mc_chroma(cu, 8, cv, 8, csrc, cs, mvx, 2*mvy >> 1, 8, 8); /* macroblock.c:64 */
    else
        phx_mc_deinterleave_chroma(cu, 8, cv, 8, csrc, cs, 8, 8);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            chroma[y*16+2*x]   = cu[y*8+x];
            chroma[y*16+2*x+1] = cv[y*8+x];
        }
    free(base); free(buf);
}

/* Fill both planes of a reference frame deterministically, including the
 * padded border, so fractional MC never reads fabricated zero samples.
 * SHADER: B — test fixture. */
static void phx_test_fill_ref(phx_frame_t *ref)
{
    for (int y = -PHX_PADV; y < ref->i_height + PHX_PADV; y++)
        for (int x = -PHX_PADH; x < ref->i_stride; x++)
            ref->plane[y*ref->i_stride+x] = phx_test_pixel(x, y);
    const int cs = ref->i_stride_planes[1];
    /* NV12: x counts chroma samples, each occupying two bytes; the left pad
     * is PHX_PADH/2 samples and the right edge is cs/2 - PHX_PADH/2. */
    for (int y = -(PHX_PADV/2); y < ref->i_height/2 + PHX_PADV/2; y++)
        for (int x = -(PHX_PADH/2); x < cs/2 - PHX_PADH/2; x++) {
            ref->plane_chroma[y*cs+2*x]   = (uint8_t)(11*x + 5*y + 17);
            ref->plane_chroma[y*cs+2*x+1] = (uint8_t)(7*x + 13*y + 41);
        }
}

/* P_L0/D_16x16 no-residual reconstruction from a caller-supplied reference.
 * The MV is fixed at (8,4) qpel, i.e. (2,1) fullpel via me.c:178 FPEL(); the
 * reconstructed MB must equal the reference at (x+2,y+1) for all 256 samples.
 * The decision's coefficient/nnz arrays are poisoned first so the B stage
 * must explicitly clear them (encoder/macroblock.c:624-626, :946-951).
 * A missing reference must be rejected, not replaced with fabricated pixels.
 * SHADER: B — verifies the B->C inter handoff and the MC reconstruction. */
static void phx_test_p_l0_mc(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.b_cabac = 1;

    phx_frame_t *f = phx_frame_new(32, 32, 0);
    phx_frame_t *ref = phx_frame_new(32, 32, 0);
    CHECK(f && ref, "P_L0 frame allocation");
    if (!f || !ref) {
        if (f) phx_frame_delete(f);
        if (ref) phx_frame_delete(ref);
        return;
    }
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            f->plane[y*f->i_stride + x] = (uint8_t)(phx_test_pixel(x, y) ^ 0x5a);
            ref->plane[y*ref->i_stride + x] = phx_test_pixel(x, y);
        }
    }
    phx_plane_expand_border(f->plane, f->i_stride, 32, 32,
                            PHX_PADH, PHX_PADV, 1, 1, 0);
    phx_test_fill_ref(ref);

    phx_mb_state_t *s = phx_mb_state_new(2, 2);
    CHECK(s != NULL, "P_L0 MB state");
    if (!s) { phx_frame_delete(f); phx_frame_delete(ref); return; }
    s->slice_type = 0; /* SLICE_TYPE_P, common/base.h */
    s->first_mb = 0;

    phx_mb_decision_t d1, d2;
    phx_mb_cache_t c1, c2;
    uint8_t r1[PHX_FDEC_STRIDE*18 + PHX_FDEC_STRIDE];
    uint8_t r2[PHX_FDEC_STRIDE*18 + PHX_FDEC_STRIDE];
    for (int rep = 0; rep < 2; rep++) {
        phx_mb_decision_t *d = rep ? &d2 : &d1;
        phx_mb_cache_t *c = rep ? &c2 : &c1;
        uint8_t *recon = rep ? r2 : r1;
        memset(c, 0, sizeof(*c));
        phx_macroblock_cache_load(s, c, 0, 0, 0);
        c->p_fdec = recon + PHX_FDEC_STRIDE + 1;   /* common/common.h:571 */
        c->fdec_stride = PHX_FDEC_STRIDE;
        memset(recon, 0, sizeof(r1));

        memset(d, 0, sizeof(*d));
        d->mb_type = PHX_P_L0;
        d->i_partition = PHX_D_16x16;
        d->i_qp = TEST_QP;
        d->ref[0][0] = 0;                          /* p_fref[0][0], macroblock.c:666 */
        d->mv[0][0][0] = 8;                        /* qpel, me.h:55 */
        d->mv[0][0][1] = 4;
        for (int i = 0; i < 48; i++) {
            d->nnz[i] = 1;                         /* poison trial coefficients */
            for (int k = 0; k < 16; k++) d->dct[i][k] = 1234;
        }

        CHECK(phx_macroblock_encode_ex(&p, f, ref, c, 0, 0, d, NULL) == 0,
              "P_L0 encode accepts no-residual inter decision");
        CHECK(d->cbp_luma == 0 && d->cbp_chroma == 0 && d->i_cbp == 0,
              "P_L0 cbp stays zero");
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                const uint8_t got = c->p_fdec[y*PHX_FDEC_STRIDE + x];
                const uint8_t want = ref->plane[(y+1)*ref->i_stride + (x+2)];
                if (got != want) {
                    printf("FAIL P_L0 MC (%d,%d): got %d want %d\n",
                           x, y, got, want);
                    ++fails;
                }
            }
        }
        uint8_t want_l[256], want_c[128];
        phx_test_ref_mc(ref, 0, 0, 8, 4, want_l, want_c);
        const int cs = f->i_stride_planes[1];
        for (int y = 0; y < 8; y++)
            for (int k = 0; k < 16; k++) {
                const uint8_t got = f->plane_chroma[y*cs+k];
                if (got != want_c[y*16+k]) {
                    printf("FAIL P_L0 chroma (%d,%d): got %d want %d\n",
                           k/2, y, got, want_c[y*16+k]);
                    ++fails;
                }
            }
        for (int i = 0; i < 48; i++) {
            CHECK(d->nnz[i] == 0, "P_L0 nnz cleared");
            CHECK(c->non_zero_count[phx_scan8[i]] == 0,
                  "P_L0 cache nnz cleared");
            for (int k = 0; k < 16; k++)
                CHECK(d->dct[i][k] == 0, "P_L0 coefficients cleared");
        }
    }
    CHECK(memcmp(&d1, &d2, sizeof(d1)) == 0, "P_L0 decision reproducible");
    CHECK(memcmp(r1, r2, sizeof(r1)) == 0, "P_L0 reconstruction reproducible");
    CHECK(memcmp(c1.non_zero_count, c2.non_zero_count,
                 sizeof(c1.non_zero_count)) == 0, "P_L0 cache reproducible");

    /* No fabricated pixels: without a list-0 reference the mode is rejected. */
    memset(&d1, 0, sizeof(d1));
    d1.mb_type = PHX_P_L0;
    d1.i_partition = PHX_D_16x16;
    d1.i_qp = TEST_QP;
    d1.ref[0][0] = 0;
    CHECK(phx_macroblock_encode_ex(&p, f, NULL, &c1, 0, 0, &d1, NULL) == -1,
          "P_L0 without reference is rejected");

    printf("P_L0 MC: MV=(8,4) qpel -> fullpel (2,1) luma, chroma mvy=2 (halfpel), cbp=0\n");
    phx_mb_state_delete(s);
    phx_frame_delete(f);
    phx_frame_delete(ref);
}

/* Fractional-MV P_L0 reconstruction: the B stage must interpolate luma with
 * x264's HPEL/qpel path (common/mc.c:174-219) and scale chroma MV vertically
 * by 1/2 (common/macroblock.c:53/64), not round to fullpel via FPEL().
 * Integer MV (0,0) must remain an exact copy (fullpel path unchanged).
 * SHADER: B — verifies the B-stage MC handoff for subpel decisions. */
static void phx_test_p_l0_subpel(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.b_cabac = 1;

    phx_frame_t *f = phx_frame_new(32, 32, 0);
    phx_frame_t *ref = phx_frame_new(32, 32, 0);
    CHECK(f && ref, "subpel frame allocation");
    if (!f || !ref) {
        if (f) phx_frame_delete(f);
        if (ref) phx_frame_delete(ref);
        return;
    }
    phx_test_fill_ref(ref);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            f->plane[y*f->i_stride+x] = (uint8_t)(phx_test_pixel(x,y) ^ 0x3c);

    phx_mb_state_t *s = phx_mb_state_new(2, 2);
    CHECK(s != NULL, "subpel MB state");
    if (!s) { phx_frame_delete(f); phx_frame_delete(ref); return; }
    s->slice_type = 0;
    s->first_mb = 0;

    /* (mvx,mvy) quarter-pel cases: horizontal halfpel, qpel average, both
     * ==3 pointer shifts, vertical qpel, and the integer-MV control. */
    static const int mvs[][2] = {
        {2,0}, {1,0}, {3,3}, {0,1}, {0,0}
    };
    for (size_t t = 0; t < sizeof(mvs)/sizeof(mvs[0]); t++) {
        phx_mb_decision_t d;
        phx_mb_cache_t c;
        uint8_t recon[PHX_FDEC_STRIDE*18 + PHX_FDEC_STRIDE];
        memset(&c, 0, sizeof(c));
        phx_macroblock_cache_load(s, &c, 0, 0, 0);
        c.p_fdec = recon + PHX_FDEC_STRIDE + 1;
        c.fdec_stride = PHX_FDEC_STRIDE;
        memset(recon, 0, sizeof(recon));
        memset(&d, 0, sizeof(d));
        d.mb_type = PHX_P_L0;
        d.i_partition = PHX_D_16x16;
        d.i_qp = TEST_QP;
        d.ref[0][0] = 0;
        d.mv[0][0][0] = (int16_t)mvs[t][0];
        d.mv[0][0][1] = (int16_t)mvs[t][1];

        CHECK(phx_macroblock_encode_ex(&p, f, ref, &c, 0, 0, &d, NULL) == 0,
              "subpel P_L0 encode accepted");
        uint8_t want_l[256], want_c[128];
        phx_test_ref_mc(ref, 0, 0, mvs[t][0], mvs[t][1], want_l, want_c);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                if (c.p_fdec[y*PHX_FDEC_STRIDE+x] != want_l[y*16+x]) {
                    printf("FAIL subpel luma mv=(%d,%d) (%d,%d): %d vs %d\n",
                           mvs[t][0], mvs[t][1], x, y,
                           c.p_fdec[y*PHX_FDEC_STRIDE+x], want_l[y*16+x]);
                    ++fails;
                }
        const int cs = f->i_stride_planes[1];
        for (int y = 0; y < 8; y++)
            for (int k = 0; k < 16; k++)
                if (f->plane_chroma[y*cs+k] != want_c[y*16+k]) {
                    printf("FAIL subpel chroma mv=(%d,%d) (%d,%d): %d vs %d\n",
                           mvs[t][0], mvs[t][1], k/2, y,
                           f->plane_chroma[y*cs+k], want_c[y*16+k]);
                    ++fails;
                }
        if (mvs[t][0] == 0 && mvs[t][1] == 0) {
            /* Integer MV: old fullpel path must be byte-identical. */
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++)
                    CHECK(c.p_fdec[y*PHX_FDEC_STRIDE+x] ==
                          ref->plane[y*ref->i_stride+x],
                          "integer-MV output unchanged");
        }
    }
    printf("P_L0 subpel MC: 5 MV cases matched x264 hpel/qpel + chroma formulas\n");
    phx_mb_state_delete(s);
    phx_frame_delete(f);
    phx_frame_delete(ref);
}

/* ===== I8X8: type=1 qp=23 cbp_l=15 cbp_c=2 b8=1 ===== */
static const uint8_t gold_I8X8_fenc[256] = {
    11, 18, 25, 32, 39, 46, 53, 60, 67, 74, 81, 88, 95, 102, 109, 116,
    24, 31, 38, 45, 52, 59, 66, 73, 80, 87, 94, 101, 108, 115, 122, 129,
    37, 44, 51, 58, 65, 72, 79, 86, 93, 100, 107, 114, 121, 128, 135, 142,
    50, 57, 64, 71, 78, 85, 92, 99, 106, 113, 120, 127, 134, 141, 148, 155,
    63, 70, 77, 84, 91, 98, 105, 112, 119, 126, 133, 140, 147, 154, 161, 168,
    76, 83, 90, 97, 104, 111, 118, 125, 132, 139, 146, 153, 160, 167, 174, 181,
    89, 96, 103, 110, 117, 124, 131, 138, 145, 152, 159, 166, 173, 180, 187, 194,
    102, 109, 116, 123, 130, 137, 144, 151, 158, 165, 172, 179, 186, 193, 200, 207,
    115, 122, 129, 136, 143, 150, 157, 164, 171, 178, 185, 192, 199, 206, 213, 220,
    128, 135, 142, 149, 156, 163, 170, 177, 184, 191, 198, 205, 212, 219, 226, 233,
    141, 148, 155, 162, 169, 176, 183, 190, 197, 204, 211, 218, 225, 232, 239, 246,
    154, 161, 168, 175, 182, 189, 196, 203, 210, 217, 224, 231, 238, 245, 252, 3,
    167, 174, 181, 188, 195, 202, 209, 216, 223, 230, 237, 244, 251, 2, 9, 16,
    180, 187, 194, 201, 208, 215, 222, 229, 236, 243, 250, 1, 8, 15, 22, 29,
    193, 200, 207, 214, 221, 228, 235, 242, 249, 0, 7, 14, 21, 28, 35, 42,
    206, 213, 220, 227, 234, 241, 248, 255, 6, 13, 20, 27, 34, 41, 48, 55,
};
static const uint8_t gold_I8X8_fdec[256] = {
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    24, 31, 37, 45, 51, 59, 65, 72, 80, 86, 93, 100, 107, 114, 121, 128,
    38, 45, 51, 59, 65, 73, 80, 86, 94, 101, 107, 115, 121, 129, 136, 142,
    50, 56, 63, 71, 77, 85, 91, 98, 107, 113, 120, 128, 134, 142, 148, 155,
    64, 71, 77, 85, 91, 99, 106, 112, 119, 126, 132, 140, 146, 154, 161, 167,
    76, 82, 89, 97, 103, 111, 117, 124, 132, 138, 145, 153, 159, 167, 173, 180,
    90, 97, 103, 111, 117, 125, 131, 138, 146, 153, 160, 167, 174, 181, 188, 194,
    101, 108, 115, 122, 129, 136, 143, 150, 157, 164, 171, 178, 185, 192, 199, 205,
    116, 123, 129, 137, 143, 151, 157, 164, 169, 180, 189, 193, 202, 205, 216, 216,
    128, 134, 141, 148, 155, 162, 169, 176, 182, 195, 198, 204, 211, 220, 225, 235,
    142, 148, 155, 162, 169, 177, 183, 190, 199, 203, 208, 215, 227, 232, 237, 247,
    153, 160, 166, 174, 181, 188, 195, 201, 205, 219, 221, 228, 238, 245, 251, 11,
    168, 174, 181, 188, 195, 203, 209, 216, 220, 227, 236, 243, 249, 1, 5, 15,
    179, 186, 192, 200, 207, 214, 221, 227, 230, 238, 255, 3, 9, 16, 24, 27,
    193, 200, 207, 214, 221, 228, 235, 241, 248, 6, 9, 16, 26, 27, 37, 43,
    205, 212, 218, 226, 232, 240, 246, 253, 0, 13, 23, 30, 37, 40, 49, 50,
};
static const uint8_t gold_I8X8_border[35] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0,
};
static const int16_t gold_I8X8_dct4[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const int16_t gold_I8X8_dct8[256] = {
    72, -14, -26, 0, 0, 0, -1, 0, 0, -2, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0,
    0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    28, -14, -1, 0, 0, 0, -1, 0, 0, -1, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    108, -14, -26, 0, 0, 0, -1, 0, 0, -2, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0,
    0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    -48, 24, 63, -20, -12, 1, 3, -7, -24, -7, 0, 23, 10, -1, 0, 2,
    -4, 1, 7, 0, 0, 0, -5, -18, -6, 4, -1, 0, 8, -4, 1, 1,
    0, 5, -2, 2, 0, 5, 14, 0, -3, 5, -4, -6, 4, 0, -2, -9,
    -2, -3, -9, 8, -9, 6, 0, 2, -5, 17, -2, 3, -1, 0, -1, 0,
};
static const uint8_t gold_I8X8_nnz[16] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
static const uint8_t gold_I8X8_cfenc[128] = {
    128, 133, 138, 143, 148, 153, 158, 163, 131, 136, 141, 146, 151, 156, 161, 166,
    134, 139, 144, 149, 154, 159, 164, 169, 137, 142, 147, 152, 157, 162, 167, 172,
    140, 145, 150, 155, 160, 165, 170, 175, 143, 148, 153, 158, 163, 168, 173, 178,
    146, 151, 156, 161, 166, 171, 176, 181, 149, 154, 159, 164, 169, 174, 179, 184,
    128, 130, 132, 134, 136, 138, 140, 142, 133, 135, 137, 139, 141, 143, 145, 147,
    138, 140, 142, 144, 146, 148, 150, 152, 143, 145, 147, 149, 151, 153, 155, 157,
    148, 150, 152, 154, 156, 158, 160, 162, 153, 155, 157, 159, 161, 163, 165, 167,
    158, 160, 162, 164, 166, 168, 170, 172, 163, 165, 167, 169, 171, 173, 175, 177,
};
static const uint8_t gold_I8X8_cfdec[128] = {
    132, 135, 140, 143, 152, 155, 161, 164, 133, 136, 142, 145, 153, 156, 162, 165,
    136, 139, 145, 148, 156, 159, 165, 168, 138, 140, 146, 149, 158, 161, 166, 169,
    143, 146, 152, 155, 163, 166, 172, 175, 144, 147, 153, 156, 165, 168, 173, 176,
    147, 150, 156, 159, 168, 170, 176, 179, 149, 152, 157, 160, 169, 172, 178, 181,
    131, 132, 135, 136, 137, 139, 142, 143, 134, 135, 138, 139, 140, 142, 145, 146,
    139, 141, 144, 145, 146, 147, 150, 152, 142, 144, 146, 148, 149, 150, 153, 155,
    151, 152, 155, 157, 158, 159, 162, 163, 154, 155, 158, 160, 161, 162, 165, 166,
    160, 161, 164, 165, 166, 168, 171, 172, 162, 164, 167, 168, 169, 171, 173, 175,
};
static const int16_t gold_I8X8_cdct[128] = {
    0, -2, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, -2, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, -2, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, -2, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, -1, -2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, -1, -2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, -1, -2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, -1, -2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const int16_t gold_I8X8_cdcd[8] = {
    25, -9, -5, 0,
    22, -3, -9, 0,
};
static const uint8_t gold_I8X8_cnnz[10] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
/* ===== PL0: type=4 qp=26 cbp_l=15 cbp_c=1 b8=0 ===== */
static const uint8_t gold_PL0_fenc[256] = {
    48, 55, 62, 69, 76, 83, 90, 97, 104, 111, 118, 125, 132, 139, 146, 153,
    61, 68, 75, 82, 89, 96, 103, 110, 117, 124, 131, 138, 145, 152, 159, 166,
    74, 81, 88, 95, 102, 109, 116, 123, 130, 137, 144, 151, 158, 165, 172, 179,
    87, 94, 101, 108, 115, 122, 129, 136, 143, 150, 157, 164, 171, 178, 185, 192,
    100, 107, 114, 121, 128, 135, 142, 149, 156, 163, 170, 177, 184, 191, 198, 205,
    113, 120, 127, 134, 141, 148, 155, 162, 169, 176, 183, 190, 197, 204, 211, 218,
    126, 133, 140, 147, 154, 161, 168, 175, 182, 189, 196, 203, 210, 217, 224, 231,
    139, 146, 153, 160, 167, 174, 181, 188, 195, 202, 209, 216, 223, 230, 237, 244,
    152, 159, 166, 173, 180, 187, 194, 201, 208, 215, 222, 229, 236, 243, 250, 1,
    165, 172, 179, 186, 193, 200, 207, 214, 221, 228, 235, 242, 249, 0, 7, 14,
    178, 185, 192, 199, 206, 213, 220, 227, 234, 241, 248, 255, 6, 13, 20, 27,
    191, 198, 205, 212, 219, 226, 233, 240, 247, 254, 5, 12, 19, 26, 33, 40,
    204, 211, 218, 225, 232, 239, 246, 253, 4, 11, 18, 25, 32, 39, 46, 53,
    217, 224, 231, 238, 245, 252, 3, 10, 17, 24, 31, 38, 45, 52, 59, 66,
    230, 237, 244, 251, 2, 9, 16, 23, 30, 37, 44, 51, 58, 65, 72, 79,
    243, 250, 1, 8, 15, 22, 29, 36, 43, 50, 57, 64, 71, 78, 85, 92,
};
static const uint8_t gold_PL0_fdec[256] = {
    49, 56, 63, 70, 77, 84, 92, 98, 105, 112, 119, 126, 133, 140, 148, 153,
    60, 68, 74, 82, 88, 95, 103, 109, 116, 123, 130, 137, 144, 151, 161, 166,
    74, 82, 88, 96, 103, 109, 117, 124, 130, 138, 144, 152, 159, 165, 173, 177,
    86, 94, 100, 108, 114, 121, 130, 136, 143, 151, 157, 165, 171, 178, 186, 190,
    100, 108, 114, 122, 129, 135, 142, 149, 155, 163, 169, 177, 184, 190, 200, 204,
    112, 120, 126, 134, 140, 147, 155, 161, 168, 176, 182, 190, 196, 203, 213, 217,
    126, 134, 140, 148, 154, 161, 169, 176, 183, 190, 197, 204, 211, 217, 225, 229,
    138, 145, 152, 159, 166, 173, 180, 187, 194, 201, 208, 215, 222, 228, 238, 242,
    152, 160, 166, 174, 180, 187, 192, 203, 208, 212, 226, 229, 237, 236, 244, 5,
    164, 171, 178, 185, 192, 199, 205, 218, 224, 230, 237, 246, 246, 8, 11, 19,
    178, 185, 192, 200, 206, 213, 222, 226, 234, 241, 253, 255, 8, 20, 26, 25,
    189, 197, 204, 211, 218, 224, 228, 242, 245, 252, 17, 24, 22, 31, 28, 39,
    200, 207, 219, 227, 229, 245, 248, 250, 7, 13, 23, 25, 28, 38, 48, 53,
    218, 226, 233, 240, 245, 248, 2, 4, 21, 19, 29, 35, 47, 50, 61, 66,
    233, 240, 247, 254, 5, 13, 21, 18, 24, 36, 50, 45, 60, 66, 73, 77,
    242, 250, 11, 19, 13, 29, 25, 33, 47, 48, 59, 67, 72, 73, 86, 90,
};
static const uint8_t gold_PL0_border[35] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0,
};
static const int16_t gold_PL0_dct4[256] = {
    7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    -2, 9, 12, -10, -12, 0, -3, 0, 9, 6, -6, 0, 4, -3, 0, 2,
    -27, -6, 16, 15, 22, 4, -3, -3, -13, 0, -14, -5, -4, 9, 6, -2,
    -2, 9, 12, -10, -12, 0, -3, 0, 9, 6, -6, 0, 4, -3, 0, 2,
    -32, 0, 0, 19, 24, 0, 0, 0, 0, 0, -8, -19, 12, 0, 0, -4,
    -13, -13, -22, -9, -12, 0, -6, 3, 0, -3, 4, 10, -6, 0, 9, 2,
    7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const int16_t gold_PL0_dct8[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const uint8_t gold_PL0_nnz[16] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
static const uint8_t gold_PL0_cfenc[128] = {
    145, 150, 155, 160, 165, 170, 175, 180, 148, 153, 158, 163, 168, 173, 178, 183,
    151, 156, 161, 166, 171, 176, 181, 186, 154, 159, 164, 169, 174, 179, 184, 189,
    157, 162, 167, 172, 177, 182, 187, 192, 160, 165, 170, 175, 180, 185, 190, 195,
    163, 168, 173, 178, 183, 188, 193, 198, 166, 171, 176, 181, 186, 191, 196, 201,
    151, 153, 155, 157, 159, 161, 163, 165, 156, 158, 160, 162, 164, 166, 168, 170,
    161, 163, 165, 167, 169, 171, 173, 175, 166, 168, 170, 172, 174, 176, 178, 180,
    171, 173, 175, 177, 179, 181, 183, 185, 176, 178, 180, 182, 184, 186, 188, 190,
    181, 183, 185, 187, 189, 191, 193, 195, 186, 188, 190, 192, 194, 196, 198, 200,
};
static const uint8_t gold_PL0_cfdec[128] = {
    146, 151, 154, 163, 166, 172, 175, 183, 147, 153, 156, 164, 167, 173, 176, 184,
    150, 156, 159, 167, 170, 176, 179, 187, 151, 157, 160, 169, 172, 177, 180, 188,
    157, 163, 166, 174, 177, 183, 186, 194, 158, 164, 167, 176, 179, 184, 187, 195,
    161, 167, 170, 179, 181, 187, 190, 198, 163, 168, 171, 180, 183, 189, 192, 200,
    152, 155, 156, 157, 159, 162, 163, 166, 155, 158, 159, 160, 162, 165, 166, 169,
    161, 164, 165, 166, 167, 170, 172, 175, 164, 166, 168, 169, 170, 173, 175, 178,
    172, 175, 177, 178, 179, 182, 183, 186, 175, 178, 180, 181, 182, 185, 186, 189,
    181, 184, 185, 186, 188, 191, 192, 195, 184, 187, 188, 189, 191, 193, 195, 198,
};
static const int16_t gold_PL0_cdct[128] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const int16_t gold_PL0_cdcd[8] = {
    7, 0, 0, 0,
    12, 0, 0, 0,
};
static const uint8_t gold_PL0_cnnz[10] = {
    1, 0, 0, 0, 0, 1, 0, 0, 0, 0,
};
static const uint8_t gold_PL0_cfr[512] = {
    132, 132, 132, 132, 132, 135, 140, 143, 152, 155, 161, 164, 172, 175, 180, 183,
    132, 132, 132, 132, 132, 135, 140, 143, 152, 155, 161, 164, 172, 175, 180, 183,
    132, 132, 132, 132, 132, 135, 140, 143, 152, 155, 161, 164, 172, 175, 180, 183,
    132, 132, 132, 132, 132, 135, 140, 143, 152, 155, 161, 164, 172, 175, 180, 183,
    132, 132, 132, 132, 132, 135, 140, 143, 152, 155, 161, 164, 172, 175, 180, 183,
    133, 133, 133, 133, 133, 136, 142, 145, 153, 156, 162, 165, 173, 176, 181, 184,
    136, 136, 136, 136, 136, 139, 145, 148, 156, 159, 165, 168, 176, 179, 184, 187,
    138, 138, 138, 138, 138, 140, 146, 149, 158, 161, 166, 169, 177, 180, 185, 188,
    143, 143, 143, 143, 143, 146, 152, 155, 163, 166, 172, 175, 183, 186, 191, 194,
    144, 144, 144, 144, 144, 147, 153, 156, 165, 168, 173, 176, 184, 187, 192, 195,
    147, 147, 147, 147, 147, 150, 156, 159, 168, 170, 176, 179, 187, 190, 195, 198,
    149, 149, 149, 149, 149, 152, 157, 160, 169, 172, 178, 181, 189, 192, 197, 200,
    155, 155, 155, 155, 155, 158, 164, 167, 175, 178, 184, 187, 192, 197, 202, 207,
    157, 157, 157, 157, 157, 160, 165, 168, 177, 180, 185, 188, 195, 200, 205, 210,
    160, 160, 160, 160, 160, 163, 168, 171, 180, 183, 188, 191, 198, 203, 208, 213,
    161, 161, 161, 161, 161, 164, 170, 173, 181, 184, 190, 193, 201, 206, 211, 216,
    131, 131, 131, 131, 131, 132, 135, 136, 137, 139, 142, 143, 146, 147, 150, 152,
    131, 131, 131, 131, 131, 132, 135, 136, 137, 139, 142, 143, 146, 147, 150, 152,
    131, 131, 131, 131, 131, 132, 135, 136, 137, 139, 142, 143, 146, 147, 150, 152,
    131, 131, 131, 131, 131, 132, 135, 136, 137, 139, 142, 143, 146, 147, 150, 152,
    131, 131, 131, 131, 131, 132, 135, 136, 137, 139, 142, 143, 146, 147, 150, 152,
    134, 134, 134, 134, 134, 135, 138, 139, 140, 142, 145, 146, 149, 150, 153, 155,
    139, 139, 139, 139, 139, 141, 144, 145, 146, 147, 150, 152, 155, 156, 159, 161,
    142, 142, 142, 142, 142, 144, 146, 148, 149, 150, 153, 155, 158, 159, 162, 164,
    151, 151, 151, 151, 151, 152, 155, 157, 158, 159, 162, 163, 166, 167, 170, 172,
    154, 154, 154, 154, 154, 155, 158, 160, 161, 162, 165, 166, 169, 170, 173, 175,
    160, 160, 160, 160, 160, 161, 164, 165, 166, 168, 171, 172, 175, 176, 179, 181,
    162, 162, 162, 162, 162, 164, 167, 168, 169, 171, 173, 175, 178, 179, 182, 184,
    170, 170, 170, 170, 170, 171, 174, 176, 177, 178, 181, 183, 185, 187, 188, 190,
    173, 173, 173, 173, 173, 174, 177, 179, 180, 181, 184, 186, 190, 192, 193, 195,
    179, 179, 179, 179, 179, 180, 183, 184, 186, 187, 190, 191, 195, 196, 198, 200,
    181, 181, 181, 181, 181, 183, 186, 187, 188, 190, 193, 194, 200, 201, 203, 205,
};
static const uint8_t gold_PL0_fref[2304] = {
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 19, 26, 33, 40, 47, 54, 61, 69, 75, 82, 89, 96, 103, 110, 117,
    125, 130, 138, 143, 154, 158, 166, 171, 182, 186, 195, 199, 206, 215, 220, 228,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 31, 37, 45, 51, 59, 65, 72, 80, 86, 93, 100, 107, 114, 121, 128,
    138, 143, 151, 156, 166, 171, 179, 184, 195, 199, 208, 212, 217, 229, 230, 243,
    38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38,
    38, 45, 51, 59, 65, 73, 80, 86, 94, 101, 107, 115, 121, 129, 136, 142,
    150, 154, 163, 167, 178, 182, 191, 195, 206, 210, 219, 223, 237, 242, 247, 252,
    50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50,
    50, 56, 63, 71, 77, 85, 91, 98, 107, 113, 120, 128, 134, 142, 148, 155,
    163, 167, 176, 180, 191, 195, 204, 208, 219, 223, 232, 236, 248, 252, 10, 13,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 71, 77, 85, 91, 99, 106, 112, 119, 126, 132, 140, 146, 154, 161, 167,
    177, 181, 190, 194, 202, 210, 215, 224, 227, 240, 241, 253, 7, 12, 20, 25,
    76, 76, 76, 76, 76, 76, 76, 76, 76, 76, 76, 76, 76, 76, 76, 76,
    76, 82, 89, 97, 103, 111, 117, 124, 132, 138, 145, 153, 159, 167, 173, 180,
    190, 194, 203, 207, 213, 225, 226, 238, 244, 248, 6, 9, 20, 25, 33, 37,
    90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90,
    90, 97, 103, 111, 117, 125, 131, 138, 146, 153, 160, 167, 174, 181, 188, 194,
    202, 206, 215, 219, 232, 237, 242, 247, 1, 9, 15, 23, 32, 36, 45, 49,
    101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101,
    101, 108, 115, 122, 129, 136, 143, 150, 157, 164, 171, 178, 185, 192, 199, 205,
    215, 219, 227, 232, 244, 247, 6, 9, 17, 22, 27, 32, 45, 49, 58, 62,
    116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116,
    116, 123, 129, 137, 143, 151, 157, 164, 169, 180, 189, 193, 202, 205, 216, 216,
    224, 234, 240, 250, 246, 5, 9, 21, 30, 34, 43, 47, 58, 62, 71, 75,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 134, 141, 148, 155, 162, 169, 176, 182, 195, 198, 204, 211, 220, 225, 235,
    238, 248, 254, 5, 14, 19, 25, 35, 43, 47, 56, 60, 71, 75, 84, 88,
    142, 142, 142, 142, 142, 142, 142, 142, 142, 142, 142, 142, 142, 142, 142, 142,
    142, 148, 155, 162, 169, 177, 183, 190, 199, 203, 208, 215, 227, 232, 237, 247,
    253, 4, 10, 20, 21, 28, 45, 46, 54, 59, 67, 71, 82, 87, 95, 100,
    153, 153, 153, 153, 153, 153, 153, 153, 153, 153, 153, 153, 153, 153, 153, 153,
    153, 160, 166, 174, 181, 188, 195, 201, 205, 219, 221, 228, 238, 245, 251, 11,
    8, 18, 24, 34, 41, 47, 54, 59, 67, 71, 80, 84, 95, 100, 108, 113,
    168, 168, 168, 168, 168, 168, 168, 168, 168, 168, 168, 168, 168, 168, 168, 168,
    168, 174, 181, 188, 195, 203, 209, 216, 220, 227, 236, 243, 249, 1, 5, 15,
    25, 30, 38, 43, 53, 58, 66, 71, 82, 86, 94, 99, 110, 114, 123, 127,
    179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179,
    179, 186, 192, 200, 207, 214, 221, 227, 230, 238, 255, 3, 9, 16, 24, 27,
    38, 43, 51, 55, 66, 71, 79, 84, 94, 99, 107, 112, 123, 127, 136, 140,
    193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193,
    193, 200, 207, 214, 221, 228, 235, 241, 248, 6, 9, 16, 26, 27, 37, 43,
    50, 54, 63, 67, 78, 82, 91, 95, 106, 110, 119, 123, 134, 138, 147, 151,
    205, 205, 205, 205, 205, 205, 205, 205, 205, 205, 205, 205, 205, 205, 205, 205,
    205, 212, 218, 226, 232, 240, 246, 253, 0, 13, 23, 30, 37, 40, 49, 50,
    63, 67, 76, 80, 91, 95, 104, 108, 119, 123, 132, 136, 147, 151, 160, 164,
    222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222,
    222, 227, 232, 237, 247, 250, 9, 12, 22, 27, 35, 40, 50, 55, 63, 68,
    77, 81, 90, 94, 105, 109, 118, 122, 134, 138, 147, 151, 162, 167, 175, 179,
    231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231,
    231, 239, 245, 253, 9, 14, 19, 24, 35, 40, 48, 52, 63, 68, 76, 81,
    90, 94, 103, 107, 118, 122, 131, 135, 147, 151, 160, 164, 175, 179, 188, 192,
    245, 245, 245, 245, 245, 245, 245, 245, 245, 245, 245, 245, 245, 245, 245, 245,
    245, 248, 7, 10, 18, 30, 31, 44, 47, 51, 60, 64, 75, 79, 88, 92,
    101, 105, 114, 118, 129, 134, 142, 147, 159, 163, 171, 176, 187, 191, 200, 204,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 13, 14, 27, 32, 41, 46, 55, 60, 64, 73, 77, 88, 92, 101, 105,
    114, 118, 127, 131, 142, 147, 155, 159, 171, 176, 184, 189, 200, 204, 213, 217,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,
    18, 22, 31, 35, 46, 50, 59, 63, 74, 78, 87, 91, 102, 106, 115, 119,
    130, 134, 143, 147, 158, 162, 171, 175, 185, 189, 198, 202, 211, 215, 226, 236,
    31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
    31, 35, 44, 48, 59, 63, 72, 76, 87, 91, 100, 104, 115, 119, 128, 132,
    143, 147, 155, 160, 171, 175, 184, 188, 198, 202, 211, 215, 226, 234, 238, 246,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 47, 55, 59, 70, 75, 83, 88, 98, 103, 111, 116, 127, 131, 140, 144,
    154, 158, 167, 171, 182, 186, 195, 199, 209, 213, 222, 226, 232, 241, 248, 5,
    55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55,
    55, 59, 68, 72, 83, 88, 96, 101, 111, 116, 124, 129, 140, 144, 152, 157,
    167, 171, 180, 184, 195, 199, 208, 212, 222, 226, 235, 239, 247, 8, 8, 15,
    70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
    70, 74, 82, 87, 98, 102, 111, 115, 126, 130, 139, 143, 154, 158, 167, 171,
    180, 185, 193, 197, 205, 213, 218, 227, 240, 241, 248, 0, 9, 14, 22, 26,
    82, 82, 82, 82, 82, 82, 82, 82, 82, 82, 82, 82, 82, 82, 82, 82,
    82, 87, 95, 100, 111, 115, 124, 128, 139, 143, 152, 156, 167, 171, 180, 184,
    193, 197, 206, 210, 216, 228, 229, 241, 246, 252, 11, 11, 22, 26, 35, 39,
    94, 94, 94, 94, 94, 94, 94, 94, 94, 94, 94, 94, 94, 94, 94, 94,
    94, 98, 107, 111, 122, 126, 135, 139, 150, 155, 163, 167, 178, 183, 191, 196,
    205, 209, 218, 222, 235, 240, 245, 250, 8, 13, 19, 30, 34, 38, 47, 51,
    107, 107, 107, 107, 107, 107, 107, 107, 107, 107, 107, 107, 107, 107, 107, 107,
    107, 111, 120, 124, 135, 139, 148, 152, 163, 167, 176, 180, 191, 196, 204, 209,
    218, 222, 231, 235, 247, 250, 9, 12, 20, 24, 34, 38, 47, 51, 60, 64,
    121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121,
    121, 126, 134, 139, 149, 154, 162, 167, 180, 184, 193, 197, 200, 208, 214, 222,
    227, 239, 240, 253, 7, 11, 20, 24, 34, 38, 47, 51, 62, 66, 75, 79,
    134, 134, 134, 134, 134, 134, 134, 134, 134, 134, 134, 134, 134, 134, 134, 134,
    134, 139, 147, 151, 162, 167, 175, 180, 193, 197, 206, 210, 211, 223, 224, 236,
    244, 247, 6, 9, 20, 24, 33, 37, 47, 51, 60, 64, 75, 79, 88, 92,
    146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146,
    146, 150, 159, 163, 174, 178, 187, 191, 204, 209, 217, 221, 231, 236, 240, 245,
    0, 9, 14, 23, 31, 36, 44, 49, 58, 63, 71, 76, 87, 91, 99, 104,
    159, 159, 159, 159, 159, 159, 159, 159, 159, 159, 159, 159, 159, 159, 159, 159,
    159, 163, 172, 176, 187, 191, 200, 204, 217, 221, 230, 234, 242, 245, 4, 7,
    17, 22, 27, 32, 44, 49, 57, 62, 71, 76, 84, 89, 99, 104, 112, 117,
};


/* -------------------------------------------------------------------------
 * Rows 18-19 goldens.  The arrays below were captured from an instrumented
 * build of reference/x264/encoder/macroblock.c: force MB0 to I_8x8
 * (macroblock.c:713-737, macroblock.h:168-211) or to P_L0/D_16x16 with a
 * (+2,0)-luma-pixel MV (macroblock.c:768-914, common/macroblock.c:34-72),
 * then dump h->mb.pic.p_fenc/p_fdec, p_fref, h->dct.luma8x8/luma4x4/
 * chroma_dc, the nnz cache and the CABAC-bound cbp at :946-951.  Phoenix is
 * fed the SAME fenc/fref/MV/ref and must reproduce every sample and
 * coefficient byte-for-byte.  No bytes are copied from x264 at runtime.
 * SHADER: B — two serial-per-MB golden invocations.
 * ------------------------------------------------------------------------- */

/* Row 19: I_8x8 64-coefficient array + reconstruction. */
static void phx_test_gold_i8x8(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.b_cabac = 0;                 /* the golden harness used ultrafast */
    p.i_trellis = 0;
    p.b_dct_decimate = 0;

    phx_frame_t *f = phx_frame_new(64, 64, 0);
    CHECK(f != NULL, "I8 golden frame allocation");
    if (!f) return;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            f->plane[y*f->i_stride + x] = gold_I8X8_fenc[y*16 + x];

    phx_mb_state_t *st = phx_mb_state_new(4, 4);
    CHECK(st != NULL, "I8 golden MB state");
    if (!st) { phx_frame_delete(f); return; }
    st->slice_type = 2; st->first_mb = 0;

    phx_mb_cache_t c;
    memset(&c, 0, sizeof(c));
    phx_macroblock_cache_load(st, &c, 0, 0, 0);
    uint8_t fdec[PHX_FDEC_STRIDE*20 + PHX_FDEC_STRIDE];
    uint8_t *pdec = fdec + PHX_FDEC_STRIDE + 1;
    memset(fdec, 0, sizeof(fdec));
    /* x264's p_fdec neighbour border at frame MB0 (macroblock.h:31-41). */
    for (int y = -1; y < 16; y++)
        pdec[-1 + y*PHX_FDEC_STRIDE] = gold_I8X8_border[y+1];
    for (int x = 0; x < 18; x++)
        pdec[x - PHX_FDEC_STRIDE] = gold_I8X8_border[17+x];
    c.p_fdec = pdec;
    c.fdec_stride = PHX_FDEC_STRIDE;

    phx_mb_decision_t d;
    memset(&d, 0, sizeof(d));
    d.mb_type = PHX_I_8x8;
    d.i_partition = PHX_D_16x16;
    d.i_qp = 23;                   /* x264's instrumented MB0 QP */
    d.b_transform_8x8 = 1;
    const int8_t modes[4] = { 0, 1, 2, 3 };
    for (int i = 0; i < 4; i++) d.intra8x8_pred_mode[i] = modes[i];

    phx_mb_i8x8_block_t out[4];
    memset(out, 0x5a, sizeof(out));
    CHECK(phx_macroblock_encode_ex2(&p, f, NULL, &c, 0, 0, &d, NULL, out) == 0,
          "I8 golden encode accepted");
    for (int i = 0; i < 4; i++) {
        CHECK(out[i].nnz == 1, "I8 golden STORE_8x8_NNZ");
        for (int k = 0; k < 64; k++)
            if (out[i].coef[k] != gold_I8X8_dct8[i*64+k]) {
                printf("FAIL I8 golden coeff block %d[%d]: %d vs %d\n",
                       i, k, out[i].coef[k], gold_I8X8_dct8[i*64+k]);
                ++fails;
                break;
            }
        const int base[4] = { 0, 2, 8, 10 };
        for (int j = 0; j < 4; j++)
            CHECK(d.nnz[phx_scan8[base[i]+j]] == 1, "I8 golden cache nnz");
    }
    CHECK(d.cbp_luma == 15, "I8 golden cbp_luma");
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            if (pdec[y*PHX_FDEC_STRIDE+x] != gold_I8X8_fdec[y*16+x]) {
                printf("FAIL I8 golden recon (%d,%d): %d vs %d\n", y, x,
                       pdec[y*PHX_FDEC_STRIDE+x], gold_I8X8_fdec[y*16+x]);
                ++fails;
                goto i8_done;
            }
i8_done:
    printf("gold I_8x8: 4x64 coefficients + 256 recon samples byte-exact vs x264\n");
    phx_mb_state_delete(st);
    phx_frame_delete(f);
}

/* Row 18: P_L0/D_16x16 residual — MC, 4x4 residual, inter chroma. */
static void phx_test_gold_p_l0_residual(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.b_cabac = 0;
    p.i_trellis = 0;
    p.b_dct_decimate = 1;          /* x264 default retained by ultrafast */

    phx_frame_t *f = phx_frame_new(64, 64, 0);
    phx_frame_t *ref = phx_frame_new(64, 64, 0);
    CHECK(f && ref, "PL0 golden frame allocation");
    if (!f || !ref) {
        if (f) phx_frame_delete(f);
        if (ref) phx_frame_delete(ref);
        return;
    }
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            f->plane[y*f->i_stride + x] = gold_PL0_fenc[y*16 + x];
    const int cs = f->i_stride_planes[1];
    for (int ch = 0; ch < 2; ch++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                f->plane_chroma[y*cs + 2*x + ch] = gold_PL0_cfenc[ch*64+y*8+x];

    /* The x264 dump is a 48x48 window at p_fref[0][0][0] (MB0 origin),
     * i.e. plane[y*stride+x] for y,x in [-16,32). */
    for (int y = -16; y < 32; y++)
        for (int x = -16; x < 32; x++)
            ref->plane[y*ref->i_stride + x] = gold_PL0_fref[(y+16)*48+(x+16)];
    const int rcs = ref->i_stride_planes[1];
    for (int ch = 0; ch < 2; ch++)
        for (int y = -4; y < 12; y++)
            for (int x = -4; x < 12; x++)
                ref->plane_chroma[y*rcs + 2*x + ch] =
                    gold_PL0_cfr[ch*256+(y+4)*16+(x+4)];

    phx_mb_state_t *st = phx_mb_state_new(4, 4);
    CHECK(st != NULL, "PL0 golden MB state");
    if (!st) { phx_frame_delete(f); phx_frame_delete(ref); return; }
    st->slice_type = 0; st->first_mb = 0;

    phx_mb_cache_t c;
    memset(&c, 0, sizeof(c));
    phx_macroblock_cache_load(st, &c, 0, 0, 0);
    uint8_t fdec[PHX_FDEC_STRIDE*20 + PHX_FDEC_STRIDE];
    uint8_t *pdec = fdec + PHX_FDEC_STRIDE + 1;
    memset(fdec, 0, sizeof(fdec));
    c.p_fdec = pdec;
    c.fdec_stride = PHX_FDEC_STRIDE;

    phx_mb_decision_t d;
    memset(&d, 0, sizeof(d));
    d.mb_type = PHX_P_L0;
    d.i_partition = PHX_D_16x16;
    d.i_qp = 26;
    d.ref[0][0] = 0;               /* p_fref[0][0], macroblock.c:666 */
    d.mv[0][0][0] = 8;             /* qpel: +2 luma pixels */
    d.mv[0][0][1] = 0;
    d.cbp_luma = 15;               /* x264's instrumented encode: cbp_luma 15 */
    d.cbp_chroma = 1;              /* DC-only chroma residual */
    for (int i = 0; i < 48; i++) {
        d.nnz[i] = 1;
        for (int k = 0; k < 16; k++) d.dct[i][k] = 1234;
    }

    CHECK(phx_macroblock_encode_ex2(&p, f, ref, &c, 0, 0, &d, NULL, NULL) == 0,
          "PL0 golden residual encode accepted");
    CHECK(d.cbp_luma == 15, "PL0 golden cbp_luma");
    CHECK(d.cbp_chroma == 1, "PL0 golden cbp_chroma (DC only)");
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            if (pdec[y*PHX_FDEC_STRIDE+x] != gold_PL0_fdec[y*16+x]) {
                printf("FAIL PL0 golden luma recon (%d,%d): %d vs %d\n", y, x,
                       pdec[y*PHX_FDEC_STRIDE+x], gold_PL0_fdec[y*16+x]);
                ++fails;
                goto pl0_done;
            }
    for (int i = 0; i < 16; i++) {
        const int slot = phx_scan8[i];
        CHECK(d.nnz[slot] == gold_PL0_nnz[i], "PL0 golden nnz");
        CHECK(c.non_zero_count[slot] == gold_PL0_nnz[i], "PL0 golden cache nnz");
        for (int k = 0; k < 16; k++)
            if (d.dct[slot][k] != gold_PL0_dct4[i*16+k]) {
                printf("FAIL PL0 golden coeff block %d[%d]: %d vs %d\n",
                       i, k, d.dct[slot][k], gold_PL0_dct4[i*16+k]);
                ++fails;
                break;
            }
    }
    for (int ch = 0; ch < 2; ch++) {
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                if (f->plane_chroma[y*cs+2*x+ch] != gold_PL0_cfdec[ch*64+y*8+x]) {
                    printf("FAIL PL0 golden chroma recon (%d,%d,%d): %d vs %d\n",
                           ch, y, x, f->plane_chroma[y*cs+2*x+ch],
                           gold_PL0_cfdec[ch*64+y*8+x]);
                    ++fails;
                    goto pl0_done;
                }
        for (int k = 0; k < 4; k++) {
            const int want[4] = { gold_PL0_cdcd[ch*4+0], gold_PL0_cdcd[ch*4+2],
                                  gold_PL0_cdcd[ch*4+1], gold_PL0_cdcd[ch*4+3] };
            CHECK(d.chroma_dct[ch][0][k] == want[k], "PL0 golden chroma DC");
        }
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 16; k++)
                if (d.chroma_dct[ch][1+i][k] != gold_PL0_cdct[(ch*4+i)*16+k]) {
                    printf("FAIL PL0 golden chroma AC %d[%d][%d]: %d vs %d\n",
                           ch, i, k, d.chroma_dct[ch][1+i][k],
                           gold_PL0_cdct[(ch*4+i)*16+k]);
                    ++fails;
                    goto pl0_done;
                }
        for (int b = 0; b < 5; b++)
            CHECK(d.chroma_nnz[ch][b] == gold_PL0_cnnz[ch*5+b],
                  "PL0 golden chroma nnz");
    }
pl0_done:
    printf("gold P_L0 residual: 256 luma + 128 chroma recon, 16x16 + chroma coeffs byte-exact vs x264\n");
    phx_mb_state_delete(st);
    phx_frame_delete(f);
    phx_frame_delete(ref);
}


/* P_L0/D_8x8 partition MC: x264's x264_mb_mc() reads each partition's MV
 * from its own 4x4 cache slot and adds 16*x/16*y qpel when pointing mc_luma
 * at the MB origin (common/macroblock.c:34-38/:158-172/:200-207).  Four
 * distinct 8x8 source regions must be reconstructed; cbp is zero so this
 * isolates the partition-MC wiring.  SHADER: B — one serial MB, four MC
 * partitions, checked against the per-partition x264 formula. */
static void phx_test_p_l0_partitions(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.b_cabac = 1;

    phx_frame_t *f = phx_frame_new(64, 64, 0);
    phx_frame_t *ref = phx_frame_new(64, 64, 0);
    CHECK(f && ref, "partition frame allocation");
    if (!f || !ref) {
        if (f) phx_frame_delete(f);
        if (ref) phx_frame_delete(ref);
        return;
    }
    phx_test_fill_ref(ref);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            f->plane[y*f->i_stride+x] = (uint8_t)(phx_test_pixel(x, y) ^ 0x11);

    phx_mb_state_t *st = phx_mb_state_new(4, 4);
    CHECK(st != NULL, "partition MB state");
    if (!st) { phx_frame_delete(f); phx_frame_delete(ref); return; }
    st->slice_type = 0; st->first_mb = 0;

    phx_mb_cache_t c;
    uint8_t recon[PHX_FDEC_STRIDE*20 + PHX_FDEC_STRIDE];
    memset(&c, 0, sizeof(c));
    phx_macroblock_cache_load(st, &c, 0, 0, 0);
    c.p_fdec = recon + PHX_FDEC_STRIDE + 1;
    c.fdec_stride = PHX_FDEC_STRIDE;
    memset(recon, 0, sizeof(recon));

    phx_mb_decision_t d;
    memset(&d, 0, sizeof(d));
    d.mb_type = PHX_P_L0;
    d.i_partition = PHX_D_8x8;
    d.i_qp = TEST_QP;
    d.ref[0][0] = d.ref[0][2] = d.ref[0][8] = d.ref[0][10] = 0;
    const int blk[4] = { 0, 2, 8, 10 };
    for (int i = 0; i < 4; i++) {
        d.mv[0][blk[i]][0] = 8;   /* +2 luma px relative to this partition */
        d.mv[0][blk[i]][1] = 0;
    }
    CHECK(phx_macroblock_encode_ex(&p, f, ref, &c, 0, 0, &d, NULL) == 0,
          "D_8x8 partition encode accepted");
    const int x4[4] = { 0, 2, 0, 2 }, y4[4] = { 0, 0, 2, 2 };
    for (int i = 0; i < 4; i++) {
        uint8_t want[256], want_c[128];
        phx_test_ref_mc(ref, 0, 0, 8 + 16*x4[i], 16*y4[i], want, want_c);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                if (c.p_fdec[(y4[i]*4+y)*PHX_FDEC_STRIDE + x4[i]*4+x]
                    != want[y*16+x]) {
                    printf("FAIL D_8x8 partition %d luma (%d,%d): %d vs %d\n",
                           i, x, y,
                           c.p_fdec[(y4[i]*4+y)*PHX_FDEC_STRIDE+x4[i]*4+x],
                           want[y*16+x]);
                    ++fails;
                }
    }
    printf("P_L0 D_8x8: four per-partition MVs matched x264 mb_mc_0xywh offsets\n");
    phx_mb_state_delete(st);
    phx_frame_delete(f);
    phx_frame_delete(ref);
}

/* P1B golden: qp=28 weight=32 */
static const uint8_t gold_B_fenc[256] = {
    50, 61, 72, 83, 94, 105, 116, 127, 138, 149, 160, 171, 182, 193, 204, 215,
    67, 78, 89, 100, 111, 122, 133, 144, 155, 166, 177, 188, 199, 210, 221, 232,
    84, 95, 106, 117, 128, 139, 150, 161, 172, 183, 194, 205, 216, 227, 238, 249,
    101, 112, 123, 134, 145, 156, 167, 178, 189, 200, 211, 222, 233, 244, 255, 10,
    118, 129, 140, 151, 162, 173, 184, 195, 206, 217, 228, 239, 250, 5, 16, 27,
    135, 146, 157, 168, 179, 190, 201, 212, 223, 234, 245, 0, 11, 22, 33, 44,
    152, 163, 174, 185, 196, 207, 218, 229, 240, 251, 6, 17, 28, 39, 50, 61,
    169, 180, 191, 202, 213, 224, 235, 246, 1, 12, 23, 34, 45, 56, 67, 78,
    186, 197, 208, 219, 230, 241, 252, 7, 18, 29, 40, 51, 62, 73, 84, 95,
    203, 214, 225, 236, 247, 2, 13, 24, 35, 46, 57, 68, 79, 90, 101, 112,
    220, 231, 242, 253, 8, 19, 30, 41, 52, 63, 74, 85, 96, 107, 118, 129,
    237, 248, 3, 14, 25, 36, 47, 58, 69, 80, 91, 102, 113, 124, 135, 146,
    254, 9, 20, 31, 42, 53, 64, 75, 86, 97, 108, 119, 130, 141, 152, 163,
    15, 26, 37, 48, 59, 70, 81, 92, 103, 114, 125, 136, 147, 158, 169, 180,
    32, 43, 54, 65, 76, 87, 98, 109, 120, 131, 142, 153, 164, 175, 186, 197,
    49, 60, 71, 82, 93, 104, 115, 126, 137, 148, 159, 170, 181, 192, 203, 214,
};
static const uint8_t gold_B_cfenc[128] = {
    143, 150, 157, 164, 171, 178, 185, 192, 146, 153, 160, 167, 174, 181, 188, 195,
    149, 156, 163, 170, 177, 184, 191, 198, 152, 159, 166, 173, 180, 187, 194, 201,
    155, 162, 169, 176, 183, 190, 197, 204, 158, 165, 172, 179, 186, 193, 200, 207,
    161, 168, 175, 182, 189, 196, 203, 210, 164, 171, 178, 185, 192, 199, 206, 213,
    149, 151, 153, 155, 157, 159, 161, 163, 158, 160, 162, 164, 166, 168, 170, 172,
    167, 169, 171, 173, 175, 177, 179, 181, 176, 178, 180, 182, 184, 186, 188, 190,
    185, 187, 189, 191, 193, 195, 197, 199, 194, 196, 198, 200, 202, 204, 206, 208,
    203, 205, 207, 209, 211, 213, 215, 217, 212, 214, 216, 218, 220, 222, 224, 226,
};
static const uint8_t gold_B_fdec[256] = {
    71, 77, 87, 97, 107, 119, 131, 141, 151, 164, 175, 185, 195, 206, 216, 102,
    87, 93, 103, 113, 123, 135, 147, 157, 167, 180, 191, 201, 211, 96, 105, 118,
    106, 112, 121, 132, 142, 152, 164, 176, 186, 197, 207, 216, 102, 114, 127, 136,
    122, 127, 137, 148, 157, 167, 182, 196, 203, 211, 96, 109, 119, 130, 142, 150,
    138, 144, 155, 165, 175, 188, 199, 211, 220, 103, 114, 125, 134, 150, 156, 41,
    154, 160, 171, 181, 191, 203, 218, 97, 110, 117, 130, 145, 150, 37, 49, 57,
    170, 176, 188, 199, 210, 100, 106, 120, 124, 137, 148, 158, 44, 54, 63, 72,
    186, 192, 204, 215, 97, 109, 120, 132, 143, 153, 38, 48, 59, 67, 83, 91,
    206, 212, 93, 105, 117, 126, 136, 152, 157, 44, 56, 66, 76, 87, 99, 109,
    93, 99, 109, 122, 132, 146, 154, 40, 51, 61, 72, 81, 92, 103, 115, 125,
    112, 117, 131, 139, 150, 161, 45, 55, 65, 77, 90, 100, 110, 122, 133, 143,
    128, 133, 146, 153, 42, 51, 59, 70, 84, 94, 106, 116, 126, 137, 149, 159,
    145, 152, 159, 44, 54, 68, 79, 89, 100, 111, 123, 133, 143, 154, 166, 176,
    162, 39, 52, 60, 72, 85, 95, 105, 115, 127, 138, 149, 159, 170, 182, 192,
    52, 58, 67, 76, 93, 102, 113, 123, 133, 144, 153, 166, 174, 189, 200, 209,
    68, 74, 86, 95, 107, 118, 128, 138, 148, 160, 167, 185, 194, 202, 215, 100,
};
static const uint8_t gold_B_cfdec[128] = {
    149, 154, 160, 168, 174, 181, 187, 196, 151, 157, 162, 170, 176, 184, 189, 198,
    154, 160, 165, 173, 179, 187, 192, 201, 156, 161, 167, 175, 180, 188, 194, 203,
    160, 166, 171, 179, 185, 193, 198, 207, 163, 169, 174, 182, 188, 196, 201, 210,
    166, 172, 177, 185, 191, 199, 204, 213, 168, 173, 179, 187, 193, 201, 207, 216,
    154, 156, 158, 159, 161, 163, 164, 166, 162, 163, 165, 167, 168, 170, 172, 176,
    172, 173, 175, 177, 178, 180, 182, 183, 179, 181, 183, 184, 185, 187, 189, 192,
    190, 191, 193, 195, 196, 198, 200, 201, 197, 199, 200, 202, 204, 206, 207, 211,
    207, 209, 211, 213, 214, 216, 218, 219, 213, 215, 217, 219, 221, 224, 225, 228,
};
static const uint8_t gold_B_fref0[2304] = {
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 23, 32, 43, 52, 64, 72, 84, 97, 109, 117, 129, 140, 152, 160, 172,
    181, 193, 204, 215, 230, 239, 246, 2, 17, 28, 37, 49, 60, 71, 80, 91,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 39, 48, 59, 68, 80, 88, 100, 113, 125, 133, 145, 156, 167, 176, 188,
    195, 210, 217, 232, 240, 253, 12, 19, 33, 44, 53, 64, 76, 87, 96, 107,
    45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45,
    45, 56, 65, 77, 85, 97, 106, 117, 130, 142, 151, 162, 173, 185, 193, 205,
    221, 229, 239, 247, 7, 20, 26, 44, 50, 62, 70, 82, 93, 104, 113, 124,
    61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61,
    61, 72, 81, 92, 101, 113, 121, 133, 146, 158, 166, 178, 189, 200, 209, 221,
    235, 241, 6, 12, 22, 33, 44, 55, 66, 77, 86, 97, 109, 120, 129, 140,
    77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
    77, 89, 97, 109, 120, 131, 140, 151, 165, 176, 185, 196, 208, 220, 227, 245,
    244, 6, 16, 30, 41, 52, 61, 72, 86, 97, 106, 117, 128, 140, 148, 160,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110,
    110, 122, 130, 142, 153, 164, 173, 185, 198, 209, 218, 230, 240, 250, 11, 21,
    27, 37, 59, 63, 74, 85, 94, 105, 119, 130, 139, 150, 161, 173, 182, 193,
    126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126,
    126, 137, 146, 158, 169, 180, 189, 200, 214, 225, 234, 245, 4, 15, 25, 30,
    50, 59, 72, 80, 89, 101, 110, 121, 134, 146, 155, 166, 177, 189, 197, 209,
    146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146,
    146, 157, 166, 177, 186, 198, 209, 220, 231, 241, 247, 10, 22, 33, 42, 54,
    65, 77, 85, 97, 108, 120, 128, 140, 151, 162, 171, 182, 195, 202, 219, 232,
    162, 162, 162, 162, 162, 162, 162, 162, 162, 162, 162, 162, 162, 162, 162, 162,
    162, 173, 182, 193, 200, 215, 222, 237, 244, 3, 17, 29, 38, 49, 58, 69,
    81, 93, 101, 113, 124, 135, 144, 156, 167, 178, 187, 198, 214, 224, 234, 245,
    179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179,
    179, 190, 199, 210, 226, 234, 244, 252, 9, 20, 29, 40, 55, 67, 75, 87,
    98, 110, 119, 130, 141, 153, 161, 173, 184, 195, 204, 216, 225, 237, 250, 9,
    195, 195, 195, 195, 195, 195, 195, 195, 195, 195, 195, 195, 195, 195, 195, 195,
    195, 206, 215, 226, 240, 246, 11, 17, 22, 34, 51, 58, 71, 82, 91, 102,
    114, 126, 134, 146, 157, 168, 177, 189, 200, 211, 220, 231, 243, 6, 12, 22,
    209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209,
    209, 222, 233, 246, 248, 10, 19, 34, 44, 56, 65, 76, 87, 99, 107, 119,
    132, 143, 152, 163, 175, 183, 196, 205, 226, 231, 241, 252, 10, 22, 30, 42,
    226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226,
    226, 239, 250, 4, 18, 26, 38, 51, 60, 72, 80, 92, 103, 115, 123, 135,
    148, 159, 168, 179, 192, 196, 218, 228, 235, 245, 6, 15, 26, 38, 46, 58,
    246, 246, 246, 246, 246, 246, 246, 246, 246, 246, 246, 246, 246, 246, 246, 246,
    246, 0, 12, 25, 31, 41, 63, 67, 78, 89, 98, 109, 120, 132, 140, 152,
    165, 176, 185, 196, 208, 221, 233, 240, 254, 14, 18, 31, 43, 55, 63, 75,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 17, 29, 42, 54, 63, 76, 84, 93, 105, 113, 125, 136, 148, 156, 168,
    181, 192, 201, 212, 225, 239, 249, 11, 10, 28, 35, 47, 59, 71, 79, 91,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 36, 44, 56, 69, 81, 89, 101, 113, 125, 133, 145, 156, 167, 176, 187,
    198, 205, 222, 235, 243, 249, 14, 19, 32, 44, 52, 64, 76, 88, 96, 108,
    40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40,
    40, 51, 60, 72, 85, 96, 105, 117, 129, 140, 149, 160, 172, 183, 192, 203,
    216, 227, 237, 248, 8, 16, 26, 34, 48, 60, 68, 80, 92, 103, 112, 124,
    57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57,
    57, 69, 77, 89, 102, 114, 122, 134, 146, 158, 166, 178, 189, 200, 209, 220,
    228, 240, 253, 12, 22, 38, 44, 59, 65, 77, 85, 97, 109, 121, 129, 141,
    73, 73, 73, 73, 73, 73, 73, 73, 73, 73, 73, 73, 73, 73, 73, 73,
    73, 85, 93, 105, 118, 130, 138, 150, 162, 173, 182, 194, 205, 216, 225, 236,
    246, 9, 15, 25, 40, 51, 62, 73, 81, 93, 101, 113, 125, 137, 145, 157,
    91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91,
    91, 102, 111, 122, 136, 147, 156, 167, 178, 186, 199, 207, 226, 235, 242, 0,
    13, 25, 33, 45, 57, 69, 77, 89, 101, 112, 121, 133, 145, 156, 165, 176,
    106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106,
    106, 118, 126, 138, 151, 163, 171, 183, 194, 198, 221, 231, 235, 249, 7, 15,
    29, 40, 49, 61, 73, 84, 93, 104, 117, 128, 137, 148, 161, 172, 181, 192,
    124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124,
    124, 135, 144, 155, 169, 180, 189, 200, 210, 224, 236, 243, 3, 16, 21, 40,
    46, 58, 66, 78, 90, 102, 110, 122, 134, 145, 154, 166, 178, 189, 198, 209,
    139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
    139, 151, 160, 171, 184, 196, 205, 216, 227, 242, 252, 14, 18, 29, 40, 51,
    62, 74, 82, 94, 106, 117, 126, 138, 150, 161, 170, 181, 194, 205, 214, 225,
    160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160,
    160, 172, 180, 192, 203, 214, 225, 235, 239, 1, 10, 25, 38, 49, 58, 69,
    80, 91, 100, 111, 126, 137, 146, 157, 170, 181, 190, 201, 209, 222, 233, 246,
    176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176,
    176, 188, 196, 208, 214, 232, 238, 250, 9, 17, 29, 42, 53, 65, 74, 85,
    95, 107, 115, 127, 141, 153, 162, 173, 185, 197, 205, 217, 226, 239, 250, 5,
    193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193,
    193, 205, 213, 225, 239, 246, 5, 18, 22, 32, 54, 58, 71, 82, 91, 102,
    113, 124, 133, 144, 159, 170, 179, 190, 203, 214, 223, 234, 246, 0, 12, 25,
    209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209,
    209, 221, 229, 241, 255, 12, 19, 27, 45, 54, 67, 75, 87, 98, 107, 118,
    128, 140, 149, 160, 175, 186, 195, 206, 218, 230, 239, 250, 5, 18, 29, 42,
    231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231,
    231, 240, 246, 3, 17, 29, 38, 49, 61, 73, 81, 93, 104, 116, 124, 136,
    148, 160, 168, 180, 191, 206, 213, 222, 230, 247, 4, 15, 26, 37, 46, 57,
    240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240,
    240, 253, 12, 19, 33, 45, 53, 65, 77, 89, 97, 109, 120, 131, 140, 151,
    164, 175, 184, 196, 210, 218, 230, 238, 251, 11, 16, 28, 41, 53, 61, 73,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 21, 26, 44, 50, 62, 71, 82, 94, 106, 114, 126, 137, 149, 157, 169,
    181, 193, 201, 213, 226, 238, 243, 3, 17, 24, 37, 44, 59, 70, 79, 90,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 34, 44, 55, 66, 78, 86, 98, 110, 122, 130, 142, 153, 164, 173, 185,
    197, 209, 217, 229, 240, 250, 8, 24, 32, 41, 48, 63, 74, 86, 95, 106,
};
static const uint8_t gold_B_cfr0[512] = {
    124, 124, 124, 124, 124, 129, 137, 142, 151, 156, 164, 169, 181, 185, 194, 198,
    124, 124, 124, 124, 124, 129, 137, 142, 151, 156, 164, 169, 181, 185, 194, 198,
    124, 124, 124, 124, 124, 129, 137, 142, 151, 156, 164, 169, 181, 185, 194, 198,
    124, 124, 124, 124, 124, 129, 137, 142, 151, 156, 164, 169, 181, 185, 194, 198,
    124, 124, 124, 124, 124, 129, 137, 142, 151, 156, 164, 169, 181, 185, 194, 198,
    126, 126, 126, 126, 126, 130, 139, 143, 153, 157, 166, 170, 182, 186, 195, 199,
    129, 129, 129, 129, 129, 133, 142, 146, 156, 160, 169, 173, 185, 189, 198, 202,
    130, 130, 130, 130, 130, 134, 143, 147, 157, 161, 170, 174, 186, 190, 199, 203,
    136, 136, 136, 136, 136, 140, 149, 153, 163, 167, 176, 180, 192, 196, 205, 209,
    137, 137, 137, 137, 137, 141, 150, 154, 164, 168, 177, 181, 193, 197, 206, 210,
    140, 140, 140, 140, 140, 144, 153, 157, 167, 171, 180, 184, 196, 200, 209, 213,
    141, 141, 141, 141, 141, 146, 154, 159, 168, 173, 181, 186, 198, 202, 211, 215,
    148, 148, 148, 148, 148, 153, 161, 166, 175, 180, 188, 193, 202, 207, 215, 220,
    150, 150, 150, 150, 150, 155, 163, 168, 177, 182, 190, 195, 204, 209, 217, 222,
    153, 153, 153, 153, 153, 158, 166, 171, 180, 185, 193, 198, 207, 212, 220, 225,
    154, 154, 154, 154, 154, 159, 167, 172, 181, 186, 194, 199, 208, 213, 221, 226,
    133, 133, 133, 133, 133, 134, 137, 139, 140, 141, 144, 145, 146, 148, 151, 152,
    133, 133, 133, 133, 133, 134, 137, 139, 140, 141, 144, 145, 146, 148, 151, 152,
    133, 133, 133, 133, 133, 134, 137, 139, 140, 141, 144, 145, 146, 148, 151, 152,
    133, 133, 133, 133, 133, 134, 137, 139, 140, 141, 144, 145, 146, 148, 151, 152,
    133, 133, 133, 133, 133, 134, 137, 139, 140, 141, 144, 145, 146, 148, 151, 152,
    139, 139, 139, 139, 139, 140, 143, 144, 145, 147, 150, 151, 157, 158, 161, 162,
    150, 150, 150, 150, 150, 151, 154, 156, 157, 158, 161, 163, 163, 164, 167, 169,
    156, 156, 156, 156, 156, 157, 160, 162, 163, 164, 167, 168, 172, 174, 177, 178,
    169, 169, 169, 169, 169, 170, 173, 175, 176, 177, 180, 181, 182, 184, 187, 188,
    175, 175, 175, 175, 175, 176, 179, 180, 181, 183, 186, 187, 193, 194, 197, 198,
    186, 186, 186, 186, 186, 187, 190, 192, 193, 194, 197, 199, 199, 200, 203, 205,
    192, 192, 192, 192, 192, 193, 196, 198, 199, 200, 203, 204, 208, 210, 213, 214,
    204, 204, 204, 204, 204, 205, 208, 210, 211, 212, 215, 216, 220, 221, 224, 226,
    210, 210, 210, 210, 210, 211, 214, 216, 217, 218, 221, 222, 226, 227, 230, 232,
    221, 221, 221, 221, 221, 222, 225, 227, 228, 229, 232, 233, 237, 238, 241, 243,
    227, 227, 227, 227, 227, 228, 231, 233, 234, 235, 238, 239, 243, 244, 247, 249,
};
static const uint8_t gold_B_fref1[2304] = {
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93, 93,
    93, 104, 113, 125, 136, 147, 156, 167, 181, 192, 201, 212, 225, 238, 241, 2,
    15, 22, 34, 47, 56, 68, 77, 88, 101, 113, 122, 133, 144, 156, 164, 176,
    110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110,
    110, 122, 130, 142, 153, 164, 173, 185, 198, 209, 218, 230, 240, 250, 11, 21,
    27, 37, 59, 63, 74, 85, 94, 105, 119, 130, 139, 150, 161, 173, 182, 193,
    126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126,
    126, 137, 146, 158, 169, 180, 189, 200, 214, 225, 234, 245, 4, 15, 25, 30,
    50, 59, 72, 80, 89, 101, 110, 121, 134, 146, 155, 166, 177, 189, 197, 209,
    146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146, 146,
    146, 157, 166, 177, 186, 198, 209, 220, 231, 241, 247, 10, 22, 33, 42, 54,
    65, 77, 85, 97, 108, 120, 128, 140, 151, 162, 171, 182, 195, 202, 219, 232,
    162, 162, 162, 162, 162, 162, 162, 162, 162, 162, 162, 162, 162, 162, 162, 162,
    162, 173, 182, 193, 200, 218, 233, 240, 244, 3, 17, 29, 38, 49, 58, 69,
    81, 93, 101, 113, 124, 135, 144, 156, 167, 178, 187, 198, 214, 224, 234, 245,
    179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179, 179,
    179, 190, 199, 210, 224, 232, 246, 254, 9, 20, 29, 40, 55, 67, 75, 87,
    98, 110, 119, 130, 141, 153, 161, 173, 184, 195, 204, 216, 225, 237, 250, 9,
    195, 195, 195, 195, 195, 195, 195, 195, 195, 195, 195, 195, 195, 195, 195, 195,
    195, 206, 215, 226, 238, 255, 1, 19, 22, 34, 51, 58, 71, 82, 91, 102,
    114, 126, 134, 146, 157, 168, 177, 189, 200, 211, 220, 231, 243, 6, 12, 22,
    209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209,
    209, 222, 233, 246, 15, 13, 30, 30, 44, 56, 65, 76, 87, 99, 107, 119,
    132, 143, 152, 163, 175, 183, 196, 205, 226, 231, 241, 252, 10, 22, 30, 42,
    226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226,
    226, 239, 250, 4, 18, 26, 38, 51, 60, 72, 80, 92, 103, 115, 123, 135,
    148, 159, 168, 179, 192, 196, 218, 228, 235, 245, 6, 15, 26, 38, 46, 58,
    246, 246, 246, 246, 246, 246, 246, 246, 246, 246, 246, 246, 246, 246, 246, 246,
    246, 0, 12, 25, 31, 41, 63, 67, 78, 89, 98, 109, 120, 132, 140, 152,
    165, 176, 185, 196, 208, 221, 233, 240, 254, 14, 18, 31, 43, 55, 63, 75,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 17, 29, 42, 54, 63, 76, 84, 93, 105, 113, 125, 136, 148, 156, 168,
    181, 192, 201, 212, 225, 239, 249, 11, 10, 28, 35, 47, 59, 71, 79, 91,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 36, 44, 56, 69, 81, 89, 101, 113, 125, 133, 145, 156, 167, 176, 187,
    198, 205, 222, 235, 243, 249, 14, 19, 32, 44, 52, 64, 76, 88, 96, 108,
    40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40,
    40, 51, 60, 72, 85, 96, 105, 117, 129, 140, 149, 160, 172, 183, 192, 203,
    216, 227, 237, 248, 8, 16, 26, 34, 48, 60, 68, 80, 92, 103, 112, 124,
    57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57,
    57, 69, 77, 89, 102, 114, 122, 134, 146, 158, 166, 178, 189, 200, 209, 220,
    235, 244, 12, 19, 22, 38, 44, 59, 65, 77, 85, 97, 109, 121, 129, 141,
    73, 73, 73, 73, 73, 73, 73, 73, 73, 73, 73, 73, 73, 73, 73, 73,
    73, 85, 93, 105, 118, 130, 138, 150, 162, 173, 182, 194, 205, 216, 225, 236,
    253, 8, 19, 32, 40, 51, 62, 73, 81, 93, 101, 113, 125, 137, 145, 157,
    91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91,
    91, 102, 111, 122, 136, 147, 156, 167, 178, 186, 199, 207, 226, 235, 242, 0,
    13, 25, 33, 45, 57, 69, 77, 89, 101, 112, 121, 133, 145, 156, 165, 176,
    106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106,
    106, 118, 126, 138, 151, 163, 171, 183, 194, 198, 221, 231, 235, 249, 7, 15,
    29, 40, 49, 61, 73, 84, 93, 104, 117, 128, 137, 148, 161, 172, 181, 192,
    124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124,
    124, 135, 144, 155, 169, 180, 189, 200, 210, 224, 236, 243, 3, 16, 21, 40,
    46, 58, 66, 78, 90, 102, 110, 122, 134, 145, 154, 166, 178, 189, 198, 209,
    139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
    139, 151, 160, 171, 184, 196, 205, 216, 227, 242, 252, 14, 18, 29, 40, 51,
    62, 74, 82, 94, 106, 117, 126, 138, 150, 161, 170, 181, 194, 205, 214, 225,
    160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160,
    160, 172, 180, 192, 203, 214, 225, 235, 239, 1, 10, 25, 38, 49, 58, 69,
    80, 91, 100, 111, 126, 137, 146, 157, 170, 181, 190, 201, 209, 222, 233, 246,
    176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176,
    176, 188, 196, 208, 214, 232, 238, 250, 9, 17, 29, 42, 53, 65, 74, 85,
    95, 107, 115, 127, 141, 153, 162, 173, 185, 197, 205, 217, 226, 239, 250, 5,
    193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193,
    193, 205, 213, 225, 239, 246, 5, 18, 22, 32, 54, 58, 71, 82, 91, 102,
    113, 124, 133, 144, 159, 170, 179, 190, 203, 214, 223, 234, 246, 0, 12, 25,
    209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209, 209,
    209, 221, 229, 241, 255, 12, 19, 27, 45, 54, 67, 75, 87, 98, 107, 118,
    128, 140, 149, 160, 175, 186, 195, 206, 218, 230, 239, 250, 5, 18, 29, 42,
    231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231, 231,
    231, 240, 246, 3, 17, 29, 38, 49, 61, 73, 81, 93, 104, 116, 124, 136,
    148, 160, 168, 180, 191, 206, 213, 222, 230, 247, 4, 15, 26, 37, 46, 57,
    244, 244, 244, 244, 244, 244, 244, 244, 244, 244, 244, 244, 244, 244, 244, 244,
    244, 13, 12, 26, 33, 45, 53, 65, 77, 89, 97, 109, 120, 131, 140, 151,
    164, 175, 184, 196, 210, 218, 230, 238, 251, 11, 16, 28, 41, 53, 61, 73,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 9, 24, 34, 50, 62, 71, 82, 94, 106, 114, 126, 137, 149, 157, 169,
    181, 193, 201, 213, 226, 238, 243, 3, 17, 24, 37, 44, 59, 70, 79, 90,
    21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 40, 40, 58, 66, 78, 86, 98, 110, 122, 130, 142, 153, 164, 173, 185,
    197, 209, 217, 229, 240, 250, 8, 24, 32, 41, 48, 63, 74, 86, 95, 106,
    45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45,
    45, 50, 59, 74, 85, 97, 105, 117, 129, 141, 149, 161, 172, 183, 192, 203,
    213, 226, 237, 250, 6, 18, 26, 38, 48, 59, 68, 79, 93, 104, 113, 124,
    58, 58, 58, 58, 58, 58, 58, 58, 58, 58, 58, 58, 58, 58, 58, 58,
    58, 70, 78, 90, 101, 112, 121, 133, 145, 156, 165, 176, 194, 197, 216, 220,
    230, 243, 255, 9, 22, 33, 42, 54, 64, 75, 84, 95, 109, 120, 129, 140,
    75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75,
    75, 87, 96, 107, 118, 130, 138, 150, 162, 174, 182, 194, 202, 217, 231, 236,
    250, 5, 16, 29, 39, 51, 59, 71, 81, 92, 101, 112, 126, 137, 146, 157,
    91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91,
    91, 103, 111, 123, 134, 146, 154, 166, 178, 189, 198, 210, 221, 238, 242, 9,
    9, 22, 33, 46, 55, 66, 75, 87, 97, 108, 117, 128, 142, 153, 162, 173,
    110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110,
    110, 121, 130, 142, 153, 164, 173, 184, 193, 201, 218, 230, 238, 251, 6, 20,
    29, 40, 49, 60, 74, 85, 94, 105, 117, 129, 138, 149, 160, 172, 180, 192,
};
static const uint8_t gold_B_cfr1[512] = {
    167, 167, 167, 167, 167, 171, 180, 184, 194, 198, 207, 211, 220, 224, 233, 237,
    167, 167, 167, 167, 167, 171, 180, 184, 194, 198, 207, 211, 220, 224, 233, 237,
    167, 167, 167, 167, 167, 171, 180, 184, 194, 198, 207, 211, 220, 224, 233, 237,
    167, 167, 167, 167, 167, 171, 180, 184, 194, 198, 207, 211, 220, 224, 233, 237,
    167, 167, 167, 167, 167, 171, 180, 184, 194, 198, 207, 211, 220, 224, 233, 237,
    170, 170, 170, 170, 170, 174, 183, 187, 197, 201, 210, 214, 223, 227, 236, 240,
    174, 174, 174, 174, 174, 178, 187, 191, 201, 205, 214, 218, 227, 231, 240, 244,
    176, 176, 176, 176, 176, 180, 189, 193, 203, 207, 216, 220, 229, 233, 242, 246,
    178, 178, 178, 178, 178, 182, 191, 195, 205, 209, 218, 222, 238, 240, 250, 252,
    182, 182, 182, 182, 182, 187, 195, 200, 209, 214, 222, 227, 237, 250, 254, 7,
    186, 186, 186, 186, 186, 191, 199, 204, 213, 218, 226, 231, 236, 249, 253, 6,
    189, 189, 189, 189, 189, 194, 202, 207, 216, 221, 229, 234, 244, 247, 6, 9,
    190, 190, 190, 190, 190, 195, 203, 208, 220, 225, 233, 238, 239, 246, 5, 9,
    193, 193, 193, 193, 193, 198, 206, 211, 223, 228, 236, 241, 244, 3, 8, 13,
    197, 197, 197, 197, 197, 202, 210, 215, 227, 232, 240, 245, 249, 8, 13, 18,
    199, 199, 199, 199, 199, 204, 212, 217, 229, 234, 242, 247, 251, 12, 22, 20,
    168, 168, 168, 168, 168, 169, 172, 174, 175, 176, 179, 181, 183, 184, 187, 189,
    168, 168, 168, 168, 168, 169, 172, 174, 175, 176, 179, 181, 183, 184, 187, 189,
    168, 168, 168, 168, 168, 169, 172, 174, 175, 176, 179, 181, 183, 184, 187, 189,
    168, 168, 168, 168, 168, 169, 172, 174, 175, 176, 179, 181, 183, 184, 187, 189,
    168, 168, 168, 168, 168, 169, 172, 174, 175, 176, 179, 181, 183, 184, 187, 189,
    178, 178, 178, 178, 178, 179, 182, 184, 185, 186, 189, 190, 192, 194, 197, 198,
    187, 187, 187, 187, 187, 188, 191, 193, 194, 195, 198, 199, 203, 204, 207, 208,
    196, 196, 196, 196, 196, 197, 200, 201, 202, 204, 207, 208, 211, 212, 215, 217,
    204, 204, 204, 204, 204, 205, 208, 210, 211, 212, 215, 217, 219, 220, 223, 225,
    213, 213, 213, 213, 213, 214, 217, 219, 220, 221, 224, 225, 229, 231, 234, 235,
    222, 222, 222, 222, 222, 223, 226, 228, 229, 230, 233, 234, 238, 239, 242, 244,
    231, 231, 231, 231, 231, 232, 235, 237, 238, 239, 242, 243, 247, 248, 251, 253,
    234, 234, 234, 234, 234, 235, 238, 240, 249, 254, 247, 255, 4, 5, 8, 10,
    249, 249, 249, 249, 249, 250, 253, 255, 5, 10, 11, 11, 13, 16, 16, 15,
    6, 6, 6, 6, 6, 7, 10, 12, 23, 8, 15, 25, 21, 24, 20, 22,
    10, 10, 10, 10, 10, 11, 14, 16, 29, 15, 22, 26, 26, 27, 29, 31,
};
static const int16_t gold_B_mv[64] = {
    8, 0, 8, 0, 8, 0, 8, 0,
    8, 0, 8, 0, 8, 0, 8, 0,
    8, 0, 8, 0, 8, 0, 8, 0,
    8, 0, 8, 0, 8, 0, 8, 0,
    -4, 4, -4, 4, -4, 4, -4, 4,
    -4, 4, -4, 4, -4, 4, -4, 4,
    -4, 4, -4, 4, -4, 4, -4, 4,
    -4, 4, -4, 4, -4, 4, -4, 4,
};
static const uint8_t gold_B_ref[32] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/* -------------------------------------------------------------------------
 * B_SKIP golden.  The arrays were captured from an instrumented x264 build:
 * MB0 of a B slice was forced to B_SKIP with a two-reference direct pair
 * (L0 MV (+2,0), L1 MV (-1,+1), both ref 0, implicit bipred weight 32);
 * encoder/macroblock.c:697-703 and common/macroblock.c:37-153/:112-152 were
 * allowed to run, then p_fenc, p_fdec and both list-0/list-1 p_fref windows
 * were dumped.  Phoenix is fed the identical fenc/fref0/fref1, direct MVs,
 * refs and weight; every reconstructed sample must be byte-identical.
 * No bytes are copied at runtime.  SHADER: B — one serial per-MB golden.
 * ------------------------------------------------------------------------- */
static void phx_test_gold_b_skip(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.b_cabac = 0;

    phx_frame_t *f = phx_frame_new(64, 64, 0);
    phx_frame_t *ref0 = phx_frame_new(64, 64, 0);
    phx_frame_t *ref1 = phx_frame_new(64, 64, 0);
    CHECK(f && ref0 && ref1, "B golden frame allocation");
    if (!f || !ref0 || !ref1) {
        if (f) phx_frame_delete(f);
        if (ref0) phx_frame_delete(ref0);
        if (ref1) phx_frame_delete(ref1);
        return;
    }
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            f->plane[y*f->i_stride+x] = gold_B_fenc[y*16+x];
    const int cs = f->i_stride_planes[1];
    for (int ch = 0; ch < 2; ch++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                f->plane_chroma[y*cs+2*x+ch] = gold_B_cfenc[ch*64+y*8+x];

    /* Each dump is a 48x48 window at p_fref[l][0][0] (MB0 origin), with
     * y,x in [-16,32); chroma is a 16x16 interleaved window in [-4,12). */
    phx_frame_t *refs[2] = { ref0, ref1 };
    for (int l = 0; l < 2; l++) {
        const int rs = refs[l]->i_stride, rcs = refs[l]->i_stride_planes[1];
        for (int y = -16; y < 32; y++)
            for (int x = -16; x < 32; x++)
                refs[l]->plane[y*rs+x] = (l ? gold_B_fref1 : gold_B_fref0)
                                          [(y+16)*48+(x+16)];
        for (int ch = 0; ch < 2; ch++)
            for (int y = -4; y < 12; y++)
                for (int x = -4; x < 12; x++)
                    refs[l]->plane_chroma[y*rcs+2*x+ch] =
                        (l ? gold_B_cfr1 : gold_B_cfr0)[ch*256+(y+4)*16+(x+4)];
    }

    phx_mb_state_t *st = phx_mb_state_new(4, 4);
    CHECK(st != NULL, "B golden MB state");
    if (!st) { phx_frame_delete(f); phx_frame_delete(ref0); phx_frame_delete(ref1); return; }
    st->slice_type = 1; st->first_mb = 0;      /* SLICE_TYPE_B */

    phx_mb_cache_t c;
    uint8_t recon[PHX_FDEC_STRIDE*20 + PHX_FDEC_STRIDE];
    memset(&c, 0, sizeof(c));
    phx_macroblock_cache_load(st, &c, 0, 0, 0);
    c.p_fdec = recon + PHX_FDEC_STRIDE + 1;
    c.fdec_stride = PHX_FDEC_STRIDE;
    memset(recon, 0, sizeof(recon));

    phx_mb_decision_t d;
    memset(&d, 0, sizeof(d));
    d.mb_type = PHX_B_SKIP;
    d.i_partition = PHX_D_16x16;
    d.i_qp = 28;
    d.skip = 1;
    for (int l = 0; l < 2; l++)
        for (int b = 0; b < 16; b++) {
            d.mv[l][b][0] = gold_B_mv[(l*16+b)*2+0];
            d.mv[l][b][1] = gold_B_mv[(l*16+b)*2+1];
            d.ref[l][b] = (int8_t)gold_B_ref[l*16+b];
        }
    /* Poison the residual arrays: B_SKIP must clear every one of them. */
    for (int i = 0; i < 48; i++) {
        d.nnz[i] = 1;
        for (int k = 0; k < 16; k++) d.dct[i][k] = 1234;
    }

    CHECK(phx_macroblock_encode_b(&p, f, ref0, ref1, &c, 0, 0, &d, 32) == 0,
          "B golden B_SKIP encode accepted");
    CHECK(d.cbp_luma == 0 && d.cbp_chroma == 0 && d.i_cbp == 0,
          "B golden cbp stays zero");
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            if (c.p_fdec[y*PHX_FDEC_STRIDE+x] != gold_B_fdec[y*16+x]) {
                printf("FAIL B golden luma (%d,%d): %d vs %d\n", y, x,
                       c.p_fdec[y*PHX_FDEC_STRIDE+x], gold_B_fdec[y*16+x]);
                ++fails;
                goto bdone;
            }
    for (int ch = 0; ch < 2; ch++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                if (f->plane_chroma[y*cs+2*x+ch] != gold_B_cfdec[ch*64+y*8+x]) {
                    printf("FAIL B golden chroma (%d,%d,%d): %d vs %d\n", ch, y, x,
                           f->plane_chroma[y*cs+2*x+ch], gold_B_cfdec[ch*64+y*8+x]);
                    ++fails;
                    goto bdone;
                }
    for (int i = 0; i < 48; i++) {
        CHECK(d.nnz[i] == 0, "B golden nnz cleared");
        CHECK(c.non_zero_count[phx_scan8[i]] == 0, "B golden cache nnz cleared");
        for (int k = 0; k < 16; k++)
            CHECK(d.dct[i][k] == 0, "B golden coefficients cleared");
    }
bdone:
    printf("gold B_SKIP: 256 luma + 128 chroma recon byte-exact vs x264 (L0+L1 bi-pred)\n");
    phx_mb_state_delete(st);
    phx_frame_delete(f);
    phx_frame_delete(ref0);
    phx_frame_delete(ref1);
}


/* Unidirectional B_SKIP (one list unavailable) must reduce to that list's
 * mc_luma/mc_chroma path (common/macroblock.c:37-102); checked against the
 * same x264 oracle used by the P_L0 MC tests.  SHADER: B — two serial MBs. */
static void phx_test_b_skip_uni(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.b_cabac = 1;

    /* list: 0 = L0-only, 1 = L1-only; MV in qpel. */
    static const int cases[2][3] = { {0, 8, 4}, {1, -4, 8} };
    for (int t = 0; t < 2; t++) {
        phx_frame_t *f = phx_frame_new(32, 32, 0);
        phx_frame_t *r0 = phx_frame_new(32, 32, 0);
        phx_frame_t *r1 = phx_frame_new(32, 32, 0);
        CHECK(f && r0 && r1, "B unidir frame allocation");
        if (!f || !r0 || !r1) {
            if (f) phx_frame_delete(f);
            if (r0) phx_frame_delete(r0);
            if (r1) phx_frame_delete(r1);
            return;
        }
        phx_test_fill_ref(r0);
        phx_test_fill_ref(r1);
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++)
                f->plane[y*f->i_stride+x] = (uint8_t)(phx_test_pixel(x,y) ^ 0x5a);
        phx_mb_state_t *st = phx_mb_state_new(2, 2);
        CHECK(st != NULL, "B unidir MB state");
        if (!st) { phx_frame_delete(f); phx_frame_delete(r0); phx_frame_delete(r1); return; }
        st->slice_type = 1; st->first_mb = 0;

        phx_mb_cache_t c;
        uint8_t recon[PHX_FDEC_STRIDE*20 + PHX_FDEC_STRIDE];
        memset(&c, 0, sizeof(c));
        phx_macroblock_cache_load(st, &c, 0, 0, 0);
        c.p_fdec = recon + PHX_FDEC_STRIDE + 1;
        c.fdec_stride = PHX_FDEC_STRIDE;
        memset(recon, 0, sizeof(recon));
        phx_mb_decision_t d;
        memset(&d, 0, sizeof(d));
        d.mb_type = PHX_B_SKIP;
        d.i_partition = PHX_D_16x16;
        d.i_qp = TEST_QP;
        d.skip = 1;
        const int list = cases[t][0], mvx = cases[t][1], mvy = cases[t][2];
        d.ref[list][0] = 0;
        d.ref[1-list][0] = -1;
        d.mv[list][0][0] = (int16_t)mvx;
        d.mv[list][0][1] = (int16_t)mvy;
        CHECK(phx_macroblock_encode_b(&p, f, r0, r1, &c, 0, 0, &d, 32) == 0,
              "B unidir encode accepted");
        uint8_t want_l[256], want_c[128];
        phx_test_ref_mc(list ? r1 : r0, 0, 0, mvx, mvy, want_l, want_c);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                if (c.p_fdec[y*PHX_FDEC_STRIDE+x] != want_l[y*16+x]) {
                    printf("FAIL B unidir list %d luma (%d,%d): %d vs %d\n",
                           list, x, y, c.p_fdec[y*PHX_FDEC_STRIDE+x], want_l[y*16+x]);
                    ++fails;
                }
        const int cs = f->i_stride_planes[1];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                if (f->plane_chroma[y*cs+2*x] != want_c[y*16+2*x] ||
                    f->plane_chroma[y*cs+2*x+1] != want_c[y*16+2*x+1]) {
                    printf("FAIL B unidir list %d chroma (%d,%d)\n", list, x, y);
                    ++fails;
                }
        phx_mb_state_delete(st);
        phx_frame_delete(f); phx_frame_delete(r0); phx_frame_delete(r1);
    }
    printf("B_SKIP unidir: L0-only and L1-only matched x264 mc_luma/mc_chroma\n");
}

/* Coded B 16x16 residual path (encoder/macroblock.c:801-914 layered on the
 * bidirectional MC of :697-703).  Fenc differs from both references by a
 * deterministic ramp, so the quantizer must emit some nonzero coefficient;
 * the old entry point rejected every non-skip/direct B type.  SHADER: B —
 * one serial per-MB encode, no hidden state. */
static void phx_test_b_residual(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.b_cabac = 1;
    phx_frame_t *f = phx_frame_new(64, 64, 0);
    phx_frame_t *r0 = phx_frame_new(64, 64, 0);
    phx_frame_t *r1 = phx_frame_new(64, 64, 0);
    CHECK(f && r0 && r1, "B residual frame allocation");
    if (!f || !r0 || !r1) {
        if (f) phx_frame_delete(f);
        if (r0) phx_frame_delete(r0);
        if (r1) phx_frame_delete(r1);
        return;
    }
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            f->plane[y*f->i_stride+x]   = (uint8_t)(80 + 3*x + 5*y);
            r0->plane[y*r0->i_stride+x] = (uint8_t)(60 + x + y);
            r1->plane[y*r1->i_stride+x] = (uint8_t)(100 + x + 2*y);
        }
    const int cs = f->i_stride_planes[1];
    for (int ch = 0; ch < 2; ch++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                f->plane_chroma[y*cs+2*x+ch]  = (uint8_t)(96 + 4*x + 2*y);
                r0->plane_chroma[y*cs+2*x+ch] = (uint8_t)(90 + 2*x + y);
                r1->plane_chroma[y*cs+2*x+ch] = (uint8_t)(102 + 3*x + y);
            }
    phx_mb_state_t *st = phx_mb_state_new(4, 4);
    phx_mb_cache_t c;
    phx_mb_decision_t d;
    memset(&c, 0, sizeof(c));
    memset(&d, 0, sizeof(d));
    phx_macroblock_cache_load(st, &c, 0, 0, 0);
    uint8_t *scratch = (uint8_t *)calloc((size_t)PHX_FDEC_STRIDE*48+64, 1);
    CHECK(scratch && st, "B residual scratch allocation");
    if (scratch && st) {
        c.p_fdec = scratch + 2*PHX_FDEC_STRIDE + 1;
        c.fdec_stride = PHX_FDEC_STRIDE;
        d.mb_type = PHX_B_L0_L0;
        d.i_partition = PHX_D_16x16;
        d.i_qp = 26;
        for (int i = 0; i < 16; i++) {
            d.ref[0][i] = 0; d.ref[1][i] = -1;
            d.mv[0][i][0] = 8; d.mv[0][i][1] = -4;
        }
        /* Nonzero residual decided by analysis (row-18 path). */
        d.cbp_luma = 0x0f; d.cbp_chroma = 2;
        CHECK(phx_macroblock_encode_b_residual(&p, f, r0, r1, &c, 0, 0,
                                               &d, NULL, 32) == 0,
              "B residual encode accepted");
        CHECK(d.skip == 0, "B residual decision is coded (not skip)");
        CHECK(d.mb_type == PHX_B_L0_L0, "B residual keeps mb_type");
        int nz = 0;
        for (int i = 0; i < 48; i++) nz += d.nnz[i] ? 1 : 0;
        CHECK(nz > 0, "B residual emitted at least one nonzero block");
        CHECK((d.i_cbp & 0x0f) == (unsigned)d.cbp_luma,
              "B residual publishes recomputed luma cbp");
        /* The old no-residual entry point must still reject this decision. */
        CHECK(phx_macroblock_encode_b(&p, f, r0, r1, &c, 0, 0, &d, 32) == -1,
              "B_SKIP-only entry point rejects coded B residual");
    }
    free(scratch);
    phx_mb_state_delete(st);
    phx_frame_delete(f); phx_frame_delete(r0); phx_frame_delete(r1);
}

int main(void)
{
    phx_test_case(PHX_I_16x16, "I16");
    phx_test_case(PHX_I_4x4, "I4");
    phx_test_first_mb_handoff();
    phx_test_first_mb_chroma_trellis();
    phx_test_p_l0_mc();
    phx_test_p_l0_subpel();
    phx_test_p_l0_partitions();
    phx_test_gold_i8x8();
    phx_test_gold_p_l0_residual();
    phx_test_gold_b_skip();
    phx_test_b_skip_uni();
    phx_test_b_residual();

    if (fails) {
        printf("FAIL %d check(s)\n", fails);
        return 1;
    }
    printf("PASS test_phx_mbencode\n");
    return 0;
}
