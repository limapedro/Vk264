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
    static const int16_t golden_dc_u[4] = { 15, -9, 0, 0 };
    static const int16_t golden_dc_v[4] = { 15, -9, 0, 0 };
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
    /* x264's instrumented encoder/cabac.c hook reports the 15-bin residual
     * window (h->dct.luma4x4[j]+1, encoder/cabac.c:1078):
     *   U scan8 16..19 -> [-2, 0, ...]  (the first sig bin is ctx=152, b=1)
     *   V scan8 32..35 -> [ 0,-2, ...]  (first sig bin ctx=152 is b=0)
     * phx_encoder.c:505-508 maps scan8 16..19 to chroma_dct[0][1+i] and
     * scan8 32..35 to chroma_dct[1][1+i]; store plane ch in slot ch.
     * SHADER: B — asserts the B->C chroma AC plane/order handoff. */
    for (int i = 0; i < 4; i++) {
        CHECK(d.chroma_dct[0][1+i][0] == -2 && d.chroma_dct[0][1+i][1] == 0,
              "trellis handoff U AC golden (scan8 16..19)");
        CHECK(d.chroma_dct[1][1+i][0] == 0 && d.chroma_dct[1][1+i][1] == -2,
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
        phx_mc_chroma(cu, 8, cv, 8, csrc, cs, mvx, mvy >> 1, 8, 8);
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

int main(void)
{
    phx_test_case(PHX_I_16x16, "I16");
    phx_test_case(PHX_I_4x4, "I4");
    phx_test_first_mb_handoff();
    phx_test_first_mb_chroma_trellis();
    phx_test_p_l0_mc();
    phx_test_p_l0_subpel();

    if (fails) {
        printf("FAIL %d check(s)\n", fails);
        return 1;
    }
    printf("PASS test_phx_mbencode\n");
    return 0;
}
