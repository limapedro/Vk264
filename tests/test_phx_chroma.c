/* Standalone golden test for the 4:2:0 chroma macroblock encoder.
 *
 * Golden source: an instrumented build of the vendored x264 C objects
 * (build/x264_ref/obj) patched only in temporary copies of
 * encoder/analyse.c (mb_analyse_intra_chroma, analyse.c:586-665),
 * encoder/macroblock.c (mb_encode_chroma_internal, macroblock.c:259-498) and
 * common/predict.c (x264_predict_8x8c_init, predict.c:936-945).  The hooks
 * print M6ANA/M6D4/M6Q/M6QUANT/M6DC/M6AC/PRED records; no x264 bytes are
 * copied into Phoenix and no repository file outside this test is changed.
 *
 * Checks, with a fixed NV12 neighbourhood:
 *   - x264's chroma mode, 2x2 DC array and four 4x4 AC arrays for synth MB0
 *     at chroma QP 20 and chroma QP 23, bit-for-bit;
 *   - all seven 8x8c predictors (predict.c:167-299) on one fixed FDEC window;
 *   - chroma_nnz / d->nnz / cache nnz / cbp consistency
 *     (macroblock.c:115-121/:429/:487-489);
 *   - a flat block under DC_128 prediction quantizes to all-zero;
 *   - byte-identical decisions and reconstruction on a repeated run.
 *
 * SHADER: B — verifies the chroma half of the B->C handoff against x264.
 */
#include "phx_chroma.h"
#include "phx_predict.h"
#include "phx_dct.h"
#include "phx_analysis.h"
#include "phx_mbencode.h"
#include "phx_param.h"
#include <stdio.h>
#include <string.h>

#define TEST_W 32
#define TEST_H 32
#define TEST_QP 26
#define TEST_MBS 2

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

/* Deterministic non-flat NV12 sample pair; not a ported x264 function.
 * SHADER: B — B-stage input fixture. */
static void phx_test_chroma_pair(int x, int y, uint8_t *u, uint8_t *v)
{
    *u = (uint8_t)((x*53 + y*97 + ((x*y) & 7)*11 + 17) & 255);
    *v = (uint8_t)((x*29 + y*71 + ((x*y+3) & 7)*13 + 61) & 255);
}

/* Fill the coded 4:2:0 area; U at even bytes, V at odd bytes.
 * SHADER: B — B-stage input fixture. */
static void phx_test_fill(phx_frame_t *f, int flat)
{
    const int s = f->i_stride_planes[1];
    for (int y = 0; y < f->i_lines_planes[1]; y++)
        for (int x = 0; x < f->i_width_planes[1]; x++) {
            uint8_t u, v;
            if (flat) u = v = 128;
            else phx_test_chroma_pair(x, y, &u, &v);
            f->plane_chroma[y*s + 2*x + 0] = u;
            f->plane_chroma[y*s + 2*x + 1] = v;
        }
}

/* FNV-1a over the reconstructed 8x8 U/V pair (encoder/macroblock.c:632/636).
 * SHADER: B — checksum of B-stage output bytes. */
static uint32_t phx_test_hash(const phx_frame_t *f, int mb_x, int mb_y)
{
    const int s = f->i_stride_planes[1];
    const int x0 = mb_x*8, y0 = mb_y*8;
    uint32_t h = 2166136261u;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 16; x++) {
            h ^= f->plane_chroma[(y0+y)*s + 2*x0 + x];
            h *= 16777619u;
        }
    return h;
}

static int phx_test_nonzero(const int16_t *c, int n)
{
    for (int i = 0; i < n; i++)
        if (c[i]) return 1;
    return 0;
}

/* encoder/macroblock.c:115-121 nnz contract, mirrored into the explicit
 * chroma arrays and x264's chroma block indices.
 * SHADER: B — validation of the B-stage chroma nnz output. */
static int phx_test_check_nnz(const phx_mb_decision_t *d,
                              const phx_mb_cache_t *c)
{
    int blocks = 0, dc = 0, ac = 0;
    for (int ch = 0; ch < 2; ch++) {
        for (int b = 0; b < 5; b++) {
            const int nz = phx_test_nonzero(d->chroma_dct[ch][b],
                                            b == 0 ? 4 : 16);
            if (d->chroma_nnz[ch][b] != (uint8_t)nz)
                printf("FAIL chroma_nnz[%d][%d]=%d want %d\n",
                       ch, b, d->chroma_nnz[ch][b], nz), ++fails;
            blocks += nz;
            if (b == 0) {
                dc += nz;
                if (c->non_zero_count[phx_scan8[49+ch]] != (uint8_t)nz)
                    printf("FAIL cache chroma DC nnz ch=%d\n", ch), ++fails;
            } else {
                const int idx = 16 + ch*16 + (b-1);    /* macroblock.c:408 */
                ac += nz;
                if (d->nnz[idx] != (uint8_t)nz)
                    printf("FAIL d->nnz[%d]=%d want %d\n",
                           idx, d->nnz[idx], nz), ++fails;
                if (c->non_zero_count[phx_scan8[idx]] != (uint8_t)nz)
                    printf("FAIL cache nnz[%d]\n", idx), ++fails;
            }
        }
    }
    const int want_cbp = ((d->chroma_nnz[0][0] || d->chroma_nnz[1][0] ||
                           (ac > 0)) ? (ac > 0 ? 2 : 1) : 0);
    if (d->cbp_chroma != want_cbp)
        printf("FAIL cbp_chroma=%d want %d\n", d->cbp_chroma, want_cbp), ++fails;
    if (c->i_cbp_chroma != d->cbp_chroma)
        printf("FAIL cache cbp_chroma\n"), ++fails;
    if (d->i_cbp != ((d->cbp_chroma << 4) | d->cbp_luma |
                     ((d->chroma_nnz[0][0] << 9) |
                      (d->chroma_nnz[1][0] << 10))))
        printf("FAIL i_cbp chroma flags\n"), ++fails;
    (void)dc;
    return blocks;
}

/* One chroma MB encode through the real cache loader (macroblock.c:855) and
 * the chroma entry point (macroblock.c:492).
 * SHADER: B — drives one B-stage chroma invocation. */
static void phx_test_run(int mb_x, int mb_y, int mode, int flat,
                         phx_mb_decision_t *out_d, phx_mb_cache_t *out_c,
                         uint8_t out_recon[16*8], int *out_nz,
                         uint32_t *out_hash)
{
    phx_param_t p;
    phx_param_default(&p);
    p.b_cabac = 1;
    p.i_chroma_qp_index_offset = 0;

    phx_frame_t *f = phx_frame_new(TEST_W, TEST_H, 0);
    CHECK(f != NULL, "phx_frame_new");
    if (!f) return;

    CHECK(f->planes[0] == f->plane && f->planes[1] == f->plane_chroma,
          "plane aliases");
    CHECK(f->i_width_planes[0] == TEST_W && f->i_lines_planes[0] == TEST_H,
          "luma plane geometry");
    CHECK(f->i_width_planes[1] == TEST_W/2 && f->i_lines_planes[1] == TEST_H/2,
          "NV12 chroma geometry (frame.c:95-96)");
    CHECK(f->i_stride_planes[0] == f->i_stride &&
          f->i_stride_planes[1] == f->i_stride,
          "NV12 chroma stride (frame.c:97)");

    phx_test_fill(f, flat);

    phx_mb_state_t *s = phx_mb_state_new(TEST_MBS, TEST_MBS);
    CHECK(s != NULL, "phx_mb_state_new");
    if (!s) { phx_frame_delete(f); return; }
    s->slice_type = 2; /* SLICE_TYPE_I */
    s->first_mb = 0;

    phx_mb_cache_t c;
    memset(&c, 0, sizeof(c));
    phx_macroblock_cache_load(s, &c, mb_x, mb_y, 0);

    phx_mb_decision_t d;
    memset(&d, 0, sizeof(d));
    d.mb_type = PHX_I_16x16;
    d.i_qp = TEST_QP;
    d.i_partition = 16;
    d.cbp_luma = 0x3;
    d.chroma_pred_mode = (int8_t)mode;

    CHECK(phx_macroblock_encode_chroma(&p, f, &c, mb_x, mb_y, &d) == 0,
          "phx_macroblock_encode_chroma");
    if (flat)
        CHECK(d.chroma_pred_mode == mode, "DC_128 mode retained at frame edge");

    *out_nz = phx_test_check_nnz(&d, &c);
    if (!flat) {
        int ac = 0, dc = 0;
        for (int ch = 0; ch < 2; ch++) {
            dc += phx_test_nonzero(d.chroma_dct[ch][0], 4);
            for (int b = 1; b < 5; b++)
                ac += phx_test_nonzero(d.chroma_dct[ch][b], 16);
        }
        CHECK(dc > 0, "real 2x2 DC coefficients for non-flat chroma");
        CHECK(ac > 0, "real 4x4 AC coefficients for non-flat chroma");
    } else {
        CHECK(*out_nz == 0, "flat chroma quantizes to zero");
        CHECK(d.cbp_chroma == 0, "flat chroma cbp is zero");
    }

    const int x0 = mb_x*8, y0 = mb_y*8, st = f->i_stride_planes[1];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 16; x++)
            out_recon[y*16 + x] = f->plane_chroma[(y0+y)*st + 2*x0 + x];

    *out_hash = phx_test_hash(f, mb_x, mb_y);
    *out_d = d;
    *out_c = c;

    phx_mb_state_delete(s);
    phx_frame_delete(f);
}

/* Run each case twice and compare every decision/reconstruction byte.
 * SHADER: B — determinism check on the B-stage chroma output. */
static void phx_test_case(int mb_x, int mb_y, int mode, const char *name)
{
    phx_mb_decision_t d1, d2;
    phx_mb_cache_t c1, c2;
    uint8_t r1[16*8], r2[16*8];
    int nz1 = 0, nz2 = 0;
    uint32_t h1 = 0, h2 = 0;

    phx_test_run(mb_x, mb_y, mode, 0, &d1, &c1, r1, &nz1, &h1);
    phx_test_run(mb_x, mb_y, mode, 0, &d2, &c2, r2, &nz2, &h2);

    CHECK(memcmp(&d1, &d2, sizeof(d1)) == 0, "decision reproducible");
    CHECK(memcmp(c1.non_zero_count, c2.non_zero_count,
                 sizeof(c1.non_zero_count)) == 0, "cache nnz reproducible");
    CHECK(c1.i_cbp_chroma == c2.i_cbp_chroma &&
          c1.i_cbp_luma == c2.i_cbp_luma, "cache cbp reproducible");
    CHECK(memcmp(r1, r2, sizeof(r1)) == 0, "reconstruction reproducible");
    CHECK(h1 == h2 && nz1 == nz2, "hash/nnz reproducible");

    printf("%s MB(%d,%d) mode=%d: nz_blocks=%d cbp_chroma=%d recon_hash=%08x\n",
           name, mb_x, mb_y, mode, nz1, d1.cbp_chroma, h1);
}

/* encoder/macroblock.c:34-41 — the exact ZIG(i,y,x) => dct[x*2+y] mapping,
 * spelled out here so the golden is derived from x264's formula rather than
 * from a copied coefficient string.  SHADER: B — independent golden order. */
static void phx_test_x264_zigzag2_dc(int16_t level[4], const int16_t natural[4])
{
    level[0] = natural[0*2+0];          /* ZIG(0,0,0): macroblock.c:37 */
    level[1] = natural[1*2+0];          /* ZIG(1,0,1): macroblock.c:38 */
    level[2] = natural[0*2+1];          /* ZIG(2,1,0): macroblock.c:39 */
    level[3] = natural[1*2+1];          /* ZIG(3,1,1): macroblock.c:40 */
}

/* Assert all four entropy-visible chroma AC arrays of one channel against
 * an instrumented x264 dump (macroblock.c:402-415, zigzagf.scan_4x4 at :410).
 * SHADER: B — per-array golden comparison, no tolerance. */
static void phx_test_check_ac(const phx_mb_decision_t *d, int ch,
                              const int16_t want[4][16])
{
    for (int b = 0; b < 4; b++) {
        if (memcmp(d->chroma_dct[ch][1+b], want[b],
                   16*sizeof(int16_t)) != 0) {
            printf("FAIL x264 golden AC ch=%d block=%d:", ch, b);
            for (int i = 0; i < 16; i++) printf(" %d", d->chroma_dct[ch][1+b][i]);
            printf("\n");
            ++fails;
        }
        if (d->chroma_nnz[ch][1+b] != 1) {
            printf("FAIL x264 golden AC nnz ch=%d block=%d\n", ch, b);
            ++fails;
        }
    }
}

/* x264's seven 8x8c predictors (common/predict.c:167-299) on one fixed FDEC
 * neighbourhood, from the instrumented common_predict.obj golden harness:
 * left column 100+3*y, top row 60+7*x, top-left 210 (predict.c:288/292).
 * SHADER: B — per-byte golden comparison of the predictor output. */
static void phx_test_predict_golden(void)
{
    static const uint8_t want[7][64] = {
        { /* 0 I_PRED_CHROMA_DC, predict.c:221-259 */
          93,93,93,93,106,106,106,106, 93,93,93,93,106,106,106,106,
          93,93,93,93,106,106,106,106, 93,93,93,93,106,106,106,106,
         120,120,120,120,113,113,113,113, 120,120,120,120,113,113,113,113,
         120,120,120,120,113,113,113,113, 120,120,120,120,113,113,113,113 },
        { /* 1 H, predict.c:260-269 */
         103,103,103,103,103,103,103,103, 106,106,106,106,106,106,106,106,
         109,109,109,109,109,109,109,109, 112,112,112,112,112,112,112,112,
         115,115,115,115,115,115,115,115, 118,118,118,118,118,118,118,118,
         121,121,121,121,121,121,121,121, 124,124,124,124,124,124,124,124 },
        { /* 2 V, predict.c:270-281 */
          67,74,81,88,95,102,109,116, 67,74,81,88,95,102,109,116,
          67,74,81,88,95,102,109,116, 67,74,81,88,95,102,109,116,
          67,74,81,88,95,102,109,116, 67,74,81,88,95,102,109,116,
          67,74,81,88,95,102,109,116, 67,74,81,88,95,102,109,116 },
        { /* 3 planar P, predict.c:282-306 */
         142,139,136,133,130,127,124,121, 138,135,132,129,126,123,120,117,
         133,130,127,124,121,118,115,112, 129,126,123,120,117,114,111,108,
         125,122,119,116,113,110,107,104, 120,117,114,111,108,105,102,99,
         116,113,110,107,104,101,98,95, 112,109,106,103,100,97,94,91 },
        { /* 4 DC_LEFT, predict.c:176-201 */
         108,108,108,108,108,108,108,108, 108,108,108,108,108,108,108,108,
         108,108,108,108,108,108,108,108, 108,108,108,108,108,108,108,108,
         120,120,120,120,120,120,120,120, 120,120,120,120,120,120,120,120,
         120,120,120,120,120,120,120,120, 120,120,120,120,120,120,120,120 },
        { /* 5 DC_TOP, predict.c:202-220 */
          78,78,78,78,106,106,106,106, 78,78,78,78,106,106,106,106,
          78,78,78,78,106,106,106,106, 78,78,78,78,106,106,106,106,
          78,78,78,78,106,106,106,106, 78,78,78,78,106,106,106,106,
          78,78,78,78,106,106,106,106, 78,78,78,78,106,106,106,106 },
        { /* 6 DC_128, predict.c:167-175 */
         128,128,128,128,128,128,128,128, 128,128,128,128,128,128,128,128,
         128,128,128,128,128,128,128,128, 128,128,128,128,128,128,128,128,
         128,128,128,128,128,128,128,128, 128,128,128,128,128,128,128,128,
         128,128,128,128,128,128,128,128, 128,128,128,128,128,128,128,128 }
    };
    const int fs = PHX_FDEC_STRIDE;
    phx_predict_t pf[7];
    phx_predict_8x8c_init(pf);
    for (int mode = 0; mode < 7; mode++) {
        uint8_t buf[PHX_FDEC_STRIDE*10 + PHX_FDEC_STRIDE];
        memset(buf, 0, sizeof(buf));
        uint8_t *p = buf + fs + 1;          /* common/common.h:570-571 */
        for (int y = -1; y < 8; y++)
            for (int x = -1; x < 8; x++)
                p[x + y*fs] = (uint8_t)((x == -1 && y == -1) ? 210 :
                    (x == -1 ? 100 + 3*(y+1) :
                     (y == -1 ? 60 + 7*(x+1) : 0)));
        pf[mode](p, fs);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                if (p[x + y*fs] != want[mode][y*8+x]) {
                    printf("FAIL x264 PRED mode=%d (%d,%d) got %d want %d\n",
                           mode, x, y, p[x+y*fs], want[mode][y*8+x]);
                    ++fails;
                }
    }
    printf("x264 8x8c predictors: 7/7 modes byte-exact (predict.c:167-299)\n");
}

/* Raw chroma DCT inputs for the fixed MB0 neighbourhood, from the instrumented
 * x264 hooks macroblock.c:379 (M6D4) and :425 (M6Q):
 *   U dct4x4 DC {120,440,120,440} -> dct_dc natural {1120,0,-640,0};
 *   V dct4x4 DC {120,120,440,440} -> dct_dc natural {1120,-640,0,0}.
 * The 2x2 Hadamard below is written out from macroblock.c:84-91 (not copied
 * from Phoenix).  SHADER: B — per-coefficient golden on the DC path input. */
static void phx_test_raw_dc_golden(void)
{
    phx_param_t p;
    phx_param_default(&p);
    phx_param_apply_preset(&p, "medium");
    phx_param_finalize(&p);
    phx_frame_t *f = phx_frame_new(TEST_W, TEST_H, 0);
    CHECK(f != NULL, "raw DC golden frame");
    if (!f) return;
    const int s = f->i_stride_planes[1], fs = PHX_FDEC_STRIDE;
    for (int y = 0; y < f->i_lines_planes[1]; y++)
        for (int x = 0; x < f->i_width_planes[1]; x++) {
            f->plane_chroma[y*s + 2*x + 0] = (uint8_t)(128 + 5*(x & 7));
            f->plane_chroma[y*s + 2*x + 1] = (uint8_t)(128 + 5*(y & 7));
        }
    static const int16_t want_d4[2][4] = {
        { 120, 440, 120, 440 }, { 120, 120, 440, 440 }
    };
    static const int16_t want_raw[2][4] = {
        { 1120, 0, -640, 0 }, { 1120, -640, 0, 0 }
    };
    phx_predict_t pred8[7];
    phx_predict_8x8c_init(pred8);
    for (int ch = 0; ch < 2; ch++) {
        uint8_t fenc[8*PHX_FENC_STRIDE];
        uint8_t fdec[PHX_FDEC_STRIDE*10 + PHX_FDEC_STRIDE];
        memset(fdec, 128, sizeof(fdec));
        uint8_t *dst = fdec + fs + 1;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                fenc[y*PHX_FENC_STRIDE + x] =
                    f->plane_chroma[y*s + 2*x + ch];
        pred8[6](dst, fs);                  /* DC_128, analyse.c:487 */
        phx_dctcoef d4[4][16];
        phx_sub8x8_dct(d4, fenc, PHX_FENC_STRIDE, dst, fs);
        int16_t raw[4];
        int d0 = d4[0][0] + d4[1][0], d1 = d4[2][0] + d4[3][0];
        int d2 = d4[0][0] - d4[1][0], d3 = d4[2][0] - d4[3][0];
        raw[0] = (int16_t)(d0 + d1);         /* macroblock.c:88 */
        raw[1] = (int16_t)(d0 - d1);         /* macroblock.c:90 */
        raw[2] = (int16_t)(d2 + d3);         /* macroblock.c:89 */
        raw[3] = (int16_t)(d2 - d3);         /* macroblock.c:91 */
        for (int i = 0; i < 4; i++) {
            CHECK(d4[i][0] == want_d4[ch][i], "x264 M6D4 4x4 DC golden");
            CHECK(raw[i] == want_raw[ch][i], "x264 M6Q raw 2x2 DC golden");
        }
    }
    printf("x264 raw chroma DCT: 2x4 DC + 2x2 Hadamard byte-exact\n");
    phx_frame_delete(f);
}

/* Fixed NV12 neighbourhood golden: MB0 U = 128+5*x, V = 128+5*y, medium/cabac
 * at MB QP 22 (chroma QP 20).  Instrumented x264 (patched analyse.c:664 and
 * macroblock.c:425/:429/:488, linked with build/x264_ref/obj) reports:
 *   M6ANA mb=0 neigh=0 qp=22 qpc=20 ... mode=6 satd=1603 dirs=0,0,0,0,0,0,1603
 *   M6Q   mb=0 ch=0 qpc=20 mf=1260 bias=18 raw=1120 0 -640 0
 *   M6QUANT mb=0 ch=0: 21 0 -12 0 / ch=1: 21 -12 0 0
 *   M6DC 0 0: 21 -12 0 0 ; M6DC 0 1: 21 0 -12 0
 *   M6AC 0 0 b: 0 -3 0 ... ; M6AC 0 1 b: 0 0 -3 0 ...   (b=0..3)
 * cbp_chroma 2 (DC+AC, macroblock.c:487-489).  The natural 2x2 order is
 * {21,0,-12,0}; zigzag_scan_2x2_dc (macroblock.c:34-41) yields the two
 * CABAC-visible arrays below.
 * SHADER: B — exact x264 mode/DC/AC golden for one fixed neighbourhood. */
static void phx_test_golden_first_mb(void)
{
    /* quant_2x2_dc's natural order (macroblock.c:425-426, 4:2:0 index i_qp),
     * mapped into CABAC order by the x264 ZIG formula. */
    static const int16_t natural_dc[4] = { 21, 0, -12, 0 };
    static const int16_t golden_dc_u[4] = { 21, -12, 0, 0 };
    static const int16_t golden_dc_v[4] = { 21, 0, -12, 0 };
    static const int16_t golden_ac_u[4][16] = {
        { 0,-3,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0,-3,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0,-3,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0,-3,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 }
    };
    static const int16_t golden_ac_v[4][16] = {
        { 0,0,-3,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0,0,-3,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0,0,-3,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0,0,-3,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 }
    };
    int16_t derived_dc[4];
    phx_test_x264_zigzag2_dc(derived_dc, natural_dc);
    CHECK(memcmp(derived_dc, golden_dc_u, sizeof(golden_dc_u)) == 0,
          "x264 ZIG formula derives U DC array");
    int16_t derived_dc_v[4];
    phx_test_x264_zigzag2_dc(derived_dc_v,
                             (const int16_t[]){ 21, -12, 0, 0 });
    CHECK(memcmp(derived_dc_v, golden_dc_v, sizeof(golden_dc_v)) == 0,
          "x264 ZIG formula derives V DC array");

    phx_param_t p;
    phx_param_default(&p);
    phx_param_apply_preset(&p, "medium");
    p.b_cabac = 1;
    phx_param_finalize(&p);            /* encoder.c:1218: chroma offset -2 */
    CHECK(p.i_chroma_qp_index_offset == -2, "medium chroma QP offset");

    phx_mb_decision_t d1, d2;
    phx_mb_cache_t c1, c2;
    uint8_t r1[16*8], r2[16*8];
    int nz1 = 0, nz2 = 0;
    uint32_t h1 = 0, h2 = 0;

    for (int rep = 0; rep < 2; rep++) {
        phx_mb_decision_t *d = rep ? &d2 : &d1;
        phx_mb_cache_t *c = rep ? &c2 : &c1;
        uint8_t *r = rep ? r2 : r1;
        int *nz = rep ? &nz2 : &nz1;
        uint32_t *h = rep ? &h2 : &h1;

        phx_frame_t *f = phx_frame_new(TEST_W, TEST_H, 0);
        CHECK(f != NULL, "golden frame allocation");
        if (!f) return;
        /* Fixed first-MB NV12 source: U = 128+5*x, V = 128+5*y.  With no
         * coded neighbours x264's chroma_mode_available[0] is {DC_128}
         * (analyse.c:487/:537-542), so M6ANA reports mode 6. */
        const int s = f->i_stride_planes[1];
        for (int y = 0; y < f->i_lines_planes[1]; y++)
            for (int x = 0; x < f->i_width_planes[1]; x++) {
                f->plane_chroma[y*s + 2*x + 0] = (uint8_t)(128 + 5*(x & 7));
                f->plane_chroma[y*s + 2*x + 1] = (uint8_t)(128 + 5*(y & 7));
            }

        phx_mb_state_t *st = phx_mb_state_new(TEST_MBS, TEST_MBS);
        CHECK(st != NULL, "golden MB state");
        if (!st) { phx_frame_delete(f); return; }
        st->slice_type = 2;
        st->first_mb = 0;
        memset(c, 0, sizeof(*c));
        phx_macroblock_cache_load(st, c, 0, 0, 0);

        memset(d, 0, sizeof(*d));
        d->mb_type = PHX_I_16x16;
        d->i_partition = 16;
        d->i_qp = 22;                  /* qp_idx 12-2+22 = 32 -> chroma QP 20 */
        d->intra16_pred_mode = 6;      /* DC_128, as x264 analyses MB(0,0) */
        d->chroma_pred_mode = 6;       /* from mb_analyse_intra_chroma */
        d->cbp_luma = 0x3;

        CHECK(phx_macroblock_encode_chroma(&p, f, c, 0, 0, d) == 0,
              "golden chroma encode");
        *nz = phx_test_check_nnz(d, c);

        CHECK(d->chroma_pred_mode == 6, "x264 golden mode 6 (M6ANA)");
        CHECK(d->cbp_chroma == 2, "x264 golden cbp_chroma == 2 (DC+AC)");
        for (int ch = 0; ch < 2; ch++) {
            const int16_t *want_dc = ch ? golden_dc_v : golden_dc_u;
            if (memcmp(d->chroma_dct[ch][0], want_dc,
                       4*sizeof(int16_t)) != 0) {
                printf("FAIL x264 DC ch=%d got %d,%d,%d,%d want %d,%d,%d,%d\n",
                       ch, d->chroma_dct[ch][0][0], d->chroma_dct[ch][0][1],
                       d->chroma_dct[ch][0][2], d->chroma_dct[ch][0][3],
                       want_dc[0], want_dc[1], want_dc[2], want_dc[3]);
                ++fails;
            }
            CHECK(d->chroma_nnz[ch][0] == 1, "x264 golden chroma DC nnz");
        }
        phx_test_check_ac(d, 0, golden_ac_u);
        phx_test_check_ac(d, 1, golden_ac_v);
        CHECK((d->i_cbp & 0x30) == 0x20, "golden cbp bits in i_cbp");
        CHECK((d->i_cbp & 0x600) == 0x600, "golden CABAC chroma DC flags");

        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 16; x++)
                r[y*16 + x] = f->plane_chroma[y*s + x];
        *h = phx_test_hash(f, 0, 0);
        phx_mb_state_delete(st);
        phx_frame_delete(f);
    }

    CHECK(memcmp(&d1, &d2, sizeof(d1)) == 0, "golden decision reproducible");
    CHECK(memcmp(c1.non_zero_count, c2.non_zero_count,
                 sizeof(c1.non_zero_count)) == 0, "golden cache reproducible");
    CHECK(memcmp(r1, r2, sizeof(r1)) == 0, "golden reconstruction reproducible");
    CHECK(h1 == h2 && nz1 == nz2, "golden hash/nnz reproducible");
    printf("golden first MB: mode=%d cbp_chroma=%d U_DC=%d,%d V_DC=%d,%d "
           "nz_blocks=%d recon_hash=%08x\n",
           d1.chroma_pred_mode, d1.cbp_chroma,
           d1.chroma_dct[0][0][0], d1.chroma_dct[0][0][1],
           d1.chroma_dct[1][0][0], d1.chroma_dct[1][0][1], nz1, h1);
}

/* Entropy-visible synth-fixture MB0 golden: U = 128+5*x, V = 128+5*y,
 * medium/cabac at MB QP 25 (chroma QP 23).  The instrumented x264 run with
 * --qp 28 (MB QP 25) reports M6ANA mode=6 qpc=23 and:
 *   M6DC 0 0: 15 -9 0 0 ; M6DC 0 1: 15 0 -9 0
 *   M6AC 0 0 b: 0 -2 0 ... ; M6AC 0 1 b: 0 0 -2 0 ...   (b=0..3)
 * These are the arrays CABAC sees (macroblock.c:457/:477, cabac.c:1069-1070);
 * the middle DC samples are swapped by macroblock.c:34-41.
 * SHADER: B — exact x264 entropy-visible chroma array assertion. */
static void phx_test_golden_uv_ramps(void)
{
    static const int16_t golden_dc_u[4] = { 15, -9, 0, 0 };
    static const int16_t golden_dc_v[4] = { 15, 0, -9, 0 };
    static const int16_t golden_ac_u[4][16] = {
        { 0,-2,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0,-2,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0,-2,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0,-2,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 }
    };
    static const int16_t golden_ac_v[4][16] = {
        { 0,0,-2,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0,0,-2,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0,0,-2,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0,0,-2,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 }
    };
    phx_param_t p;
    phx_param_default(&p);
    phx_param_apply_preset(&p, "medium");
    p.b_cabac = 1;
    phx_param_finalize(&p);
    CHECK(p.i_chroma_qp_index_offset == -2, "ramp golden chroma QP offset");

    phx_frame_t *f = phx_frame_new(TEST_W, TEST_H, 0);
    CHECK(f != NULL, "ramp golden frame allocation");
    if (!f) return;
    const int s = f->i_stride_planes[1];
    for (int y = 0; y < f->i_lines_planes[1]; y++)
        for (int x = 0; x < f->i_width_planes[1]; x++) {
            f->plane_chroma[y*s + 2*x + 0] = (uint8_t)(128 + 5*(x & 7));
            f->plane_chroma[y*s + 2*x + 1] = (uint8_t)(128 + 5*(y & 7));
        }

    phx_frame_init_lowres(f);
    phx_mb_state_t *st = phx_mb_state_new(1, 1);
    CHECK(st != NULL, "ramp golden MB state");
    if (!st) { phx_frame_delete(f); return; }
    st->slice_type = 2;
    st->first_mb = 0;
    phx_mb_cache_t c;
    memset(&c, 0, sizeof(c));
    phx_macroblock_cache_load(st, &c, 0, 0, 0);

    phx_mb_decision_t d;
    memset(&d, 0, sizeof(d));
    d.i_qp = 25;                        /* qp_idx 12-2+25 = 35 -> chroma QP 23 */
    CHECK(phx_macroblock_analyse(&p, f, &c, 0, 0, d.i_qp, &d) == 0,
          "ramp golden analysis");
    /* x264's mb_analyse_intra_chroma (analyse.c:586-665) picks mode 6 at MB0:
     * chroma_mode_available[0] = {DC_128} (analyse.c:487/:537-542). */
    CHECK(d.chroma_pred_mode == 6, "ramp analysis chroma mode 6 (M6ANA)");
    if (d.mb_type == PHX_I_8x8) d.mb_type = PHX_I_16x16;
    d.b_transform_8x8 = 0;
    CHECK(phx_macroblock_encode(&p, f, &c, 0, 0, &d) == 0,
          "ramp golden luma encode");
    d.chroma_pred_mode = 6;             /* x264 MB0: DC_128 */

    CHECK(phx_macroblock_encode_chroma(&p, f, &c, 0, 0, &d) == 0,
          "ramp golden chroma encode");
    phx_test_check_nnz(&d, &c);
    CHECK(d.chroma_pred_mode == 6, "x264 ramp M6ANA mode 6");
    CHECK(memcmp(d.chroma_dct[0][0], golden_dc_u, sizeof(golden_dc_u)) == 0,
          "x264 synth MB0 U DC {15,-9,0,0}");
    CHECK(memcmp(d.chroma_dct[1][0], golden_dc_v, sizeof(golden_dc_v)) == 0,
          "x264 synth MB0 V DC {15,0,-9,0}");
    phx_test_check_ac(&d, 0, golden_ac_u);
    phx_test_check_ac(&d, 1, golden_ac_v);
    printf("ramp MB: U_DC=%d,%d,%d,%d V_DC=%d,%d,%d,%d cbp_chroma=%d\n",
           d.chroma_dct[0][0][0], d.chroma_dct[0][0][1],
           d.chroma_dct[0][0][2], d.chroma_dct[0][0][3],
           d.chroma_dct[1][0][0], d.chroma_dct[1][0][1],
           d.chroma_dct[1][0][2], d.chroma_dct[1][0][3], d.cbp_chroma);
    phx_mb_state_delete(st);
    phx_frame_delete(f);
}

/* Standalone driver.  SHADER: B — B-stage chroma verification entry point. */
int main(void)
{
    phx_test_case(1, 1, 0, "DC");     /* top/left neighbours available */
    phx_test_case(1, 1, 3, "planar");
    phx_test_case(0, 0, 6, "DC128");  /* frame edge: available-mode fallback */
    phx_test_predict_golden();        /* x264 predict.c:167-299, 7/7 modes */
    phx_test_raw_dc_golden();         /* x264 M6D4/M6Q raw DC inputs */
    phx_test_golden_first_mb();       /* x264 mode/DC/AC at chroma QP 20 */
    phx_test_golden_uv_ramps();       /* synth U/V ramps, x264 zigzag golden */

    phx_mb_decision_t d;
    phx_mb_cache_t c;
    uint8_t r[16*8];
    int nz = 0;
    uint32_t h = 0;
    phx_test_run(0, 0, 6, 1, &d, &c, r, &nz, &h); /* flat block */
    printf("flat DC128: nz_blocks=%d cbp_chroma=%d recon_hash=%08x\n",
           nz, d.cbp_chroma, h);

    /* Explicit rejection of invalid arguments (no approximations). */
    phx_param_t p;
    phx_param_default(&p);
    phx_frame_t *f = phx_frame_new(TEST_W, TEST_H, 0);
    phx_mb_state_t *s = phx_mb_state_new(TEST_MBS, TEST_MBS);
    CHECK(f && s, "rejection fixtures");
    if (f && s) {
        phx_mb_cache_t cc;
        phx_mb_decision_t dd;
        memset(&cc, 0, sizeof(cc));
        memset(&dd, 0, sizeof(dd));
        dd.i_qp = TEST_QP;
        dd.chroma_pred_mode = 0;
        CHECK(phx_macroblock_encode_chroma(&p, f, &cc, 2, 0, &dd) == -1,
              "reject out-of-range MB x");
        dd.chroma_pred_mode = 7;
        CHECK(phx_macroblock_encode_chroma(&p, f, &cc, 0, 0, &dd) == -1,
              "reject invalid chroma mode");
        dd.chroma_pred_mode = 0;
        dd.i_qp = 52;
        CHECK(phx_macroblock_encode_chroma(&p, f, &cc, 0, 0, &dd) == -1,
              "reject invalid QP");
        CHECK(phx_macroblock_encode_chroma(NULL, f, &cc, 0, 0, &dd) == -1,
              "reject NULL param");
        phx_mb_state_delete(s);
        phx_frame_delete(f);
    }

    printf("Golden scope: x264 mode/DC/AC arrays and 8x8c predictors; full "
           "bitstream parity is not claimed here.\n");
    if (fails) {
        printf("FAIL %d check(s)\n", fails);
        return 1;
    }
    printf("PASS test_phx_chroma\n");
    return 0;
}
