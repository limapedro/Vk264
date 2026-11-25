/* Standalone test for the 4:2:0 chroma macroblock encoder.
 *
 * Checks, with deterministic NV12 input:
 *   - appended chroma plane geometry (interleaved U/V, frame.c:89-99/240);
 *   - the 2x2 DC and four 4x4 AC coefficient blocks really contain nonzero
 *     values for a non-flat chroma block (macroblock.c:379/:402/:425);
 *   - chroma_nnz / d->nnz / cache nnz / cbp consistency
 *     (macroblock.c:115-121/:429/:487-489);
 *   - a flat block under DC_128 prediction quantizes to all-zero;
 *   - byte-identical decisions and reconstruction on a repeated run.
 *
 * No bitstream parity is claimed: this verifies the B-stage chroma record and
 * reconstruction, not equality with an x264 bitstream.
 *
 * SHADER: B — verifies the chroma half of the B->C handoff.
 */
#include "phx_chroma.h"
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

/* Golden first-MB medium/cabac chroma state reported by the probe worker:
 * chroma_pred_mode 6 (DC_128), cbp_chroma 2 (DC+AC) and zigzag chroma DC
 * arrays [15,0,-9,0] for U and V (macroblock.c:329/:457/:477 -> :487-489).
 * The U/V ramps leave 15/-9 after quant_2x2_dc with chroma QP 20 (medium's
 * psy chroma offset -2 applied to luma QP 22).  x264's chroma DC quantizer
 * is quant4_mf[CQM_4IC][qp+3][0]>>1 / quant4_bias<<1 (macroblock.c:425-426).
 * SHADER: B — golden B-stage chroma handoff assertion. */
static void phx_test_golden_first_mb(void)
{
    static const int16_t golden_dc[4] = { 15, 0, -9, 0 };

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
        /* First-MB NV12 source U/V: 128 + 5*x (no neighbours => DC_128). */
        const int s = f->i_stride_planes[1];
        for (int y = 0; y < f->i_lines_planes[1]; y++)
            for (int x = 0; x < f->i_width_planes[1]; x++) {
                const uint8_t v = (uint8_t)(128 + 5*(x & 7));
                f->plane_chroma[y*s + 2*x + 0] = v;
                f->plane_chroma[y*s + 2*x + 1] = v;
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

        CHECK(d->chroma_pred_mode == 6, "golden mode 6 retained");
        CHECK(d->cbp_chroma == 2, "golden cbp_chroma == 2 (DC+AC)");
        for (int ch = 0; ch < 2; ch++) {
            CHECK(memcmp(d->chroma_dct[ch][0], golden_dc,
                         sizeof(golden_dc)) == 0,
                  ch ? "golden V DC [15,0,-9,0]"
                     : "golden U DC [15,0,-9,0]");
            CHECK(d->chroma_nnz[ch][0] == 1, "golden chroma DC nnz");
            for (int b = 1; b < 5; b++)
                CHECK(d->chroma_nnz[ch][b] == 1, "golden chroma AC nnz");
        }
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
           d1.chroma_dct[0][0][0], d1.chroma_dct[0][0][2],
           d1.chroma_dct[1][0][0], d1.chroma_dct[1][0][2], nz1, h1);
}

/* Standalone driver.  SHADER: B — B-stage chroma verification entry point. */
int main(void)
{
    phx_test_case(1, 1, 0, "DC");     /* top/left neighbours available */
    phx_test_case(1, 1, 3, "planar");
    phx_test_case(0, 0, 6, "DC128");  /* frame edge: available-mode fallback */
    phx_test_golden_first_mb();       /* medium/cabac first-MB golden state */

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

    printf("No bitstream parity claim: coefficients checked for internal "
           "consistency only.\n");
    if (fails) {
        printf("FAIL %d check(s)\n", fails);
        return 1;
    }
    printf("PASS test_phx_chroma\n");
    return 0;
}
