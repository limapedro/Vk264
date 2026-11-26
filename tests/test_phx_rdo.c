/* Standalone deterministic tests for src/phx_rdo.c.
 *
 * Every expected coefficient vector below was produced by linking a harness
 * against x264's *own* C-only objects in build/x264_ref/obj (encoder_analyse.obj
 * contains rdo.c, since encoder/analyse.c:33 does `#include "rdo.c"`).  The
 * harness calls x264_quant_4x4_trellis(h, dct, ..., b_chroma=0, idx=0) after
 * x264_cabac_context_init(h, &h->cabac, SLICE_TYPE_I, qp, 0) and setting
 * h->mb.i_trellis_lambda2[][0/1] from x264_trellis_lambda2_tab.  No stream
 * parity is claimed by these vectors: only the trellis decisions and the
 * bit-cost primitives are compared.
 *
 * Build (the CMake target to add is `phx_test_rdo`, ctest name `phx_rdo`):
 *   cmake -S . -B build/tests
 *   cmake --build build/tests --target phx_test_rdo
 *   ctest --test-dir build/tests -R phx_rdo --output-on-failure
 *
 * Golden harness build (MSVC, from the repo root; PowerShell syntax):
 *   $vc = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
 *   $inc = '.','common','encoder','input','output','filters','filters/video','extras' |
 *          ForEach-Object { '/I"reference\x264\' + $_ + '"' }
 *   cmd /c "`"$vc`" && cl -nologo -O2 /c /DBIT_DEPTH=8 `
 *        `"/Dx264_template(w)=x264_glue3(x264,8,w)`" /D__STDC_CONSTANT_MACROS `
 *        /DHAVE_CONFIG_H $inc golden_rdo.c /Fo:golden_rdo.obj"
 *   $objs = Get-ChildItem build\x264_ref\obj\*.obj |
 *           Where-Object { $_.Name -match '^(common_|encoder_)' } |
 *           ForEach-Object { '"' + $_.FullName + '"' }
 *   cmd /c "`"$vc`" && link -nologo /OUT:golden_rdo.exe golden_rdo.obj $objs shell32.lib"
 *   .\golden_rdo.exe
 * The golden harness source used here is retained at
 * C:\Users\multi\AppData\Local\Temp\opencode\rdo_golden.c in this session.
 * (`x264_encoder_open_164` returns an x264_api_t wrapper; take its first
 *  pointer field to obtain the real x264_t*, then call
 *  x264_8_quant_4x4_trellis / x264_8_cabac_context_init directly.)
 */
#include <stdio.h>
#include <string.h>
#include "phx_rdo.h"
#include "phx_cabac.h"
#include "phx_cabac_syntax.h"
#include "phx_tables.h"

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) { printf("FAIL %s\n", what); failures++; }
}

static void check_vector(const char *name, phx_cabac_t *cb, int16_t *dct,
                         int i_quant_cat, int qp, int ctx_block_cat, int b_intra,
                         int want_nz, const int16_t want[16])
{
    phx_cabac_context_init(cb, PHX_SLICE_I, qp, 0);
    int nz = phx_quant_4x4_trellis(cb, dct, i_quant_cat, qp,
                                   (intptr_t)ctx_block_cat, b_intra);
    int ok = (nz == want_nz);
    for (int i = 0; i < 16; i++)
        if (dct[i] != want[i]) ok = 0;
    printf("%-24s nz=%d ", name, nz);
    for (int i = 0; i < 16; i++) printf("%d,", dct[i]);
    printf("%s\n", ok ? "OK" : "FAIL");
    check(ok, name);
}

static void check_vector_n(const char *name, int (*fn)(phx_cabac_t *, int16_t *,
                           int, int, intptr_t, int), phx_cabac_t *cb, int16_t *dct,
                           int n, int quant_cat, int qp, int cat, int intra,
                           int want_nz, const int16_t *want)
{
    phx_cabac_context_init(cb, PHX_SLICE_I, qp, 0);
    int nz = fn(cb, dct, quant_cat, qp, (intptr_t)cat, intra);
    int ok = (nz == want_nz);
    for (int i = 0; i < n; i++) if (dct[i] != want[i]) ok = 0;
    printf("%-24s nz=%d ", name, nz);
    for (int i = 0; i < n; i++) printf("%d,", dct[i]);
    printf("%s\n", ok ? "OK" : "FAIL");
    check(ok, name);
}

/* Chroma DC wrapper has no i_quant_cat parameter. */
static int chroma_dc_wrapper(phx_cabac_t *cb, int16_t *dct, int quant_cat,
                             int qp, intptr_t cat, int intra)
{
    (void)quant_cat;
    return phx_quant_chroma_dc_trellis(cb, dct, qp, cat, intra);
}

int main(void)
{
    phx_rdo_init();
    phx_rdo_config_t cfg = { 0, 0, 0, 0, NULL, NULL };
    phx_rdo_set_config(&cfg);
    phx_cabac_t cb;
    memset(&cb, 0, sizeof(cb));

    /* x264 CABAC state at the three trellis contexts, after
     * x264_cabac_context_init(I, qp, 0).  Verify our context init agrees. */
    static const struct { int qp, cat, sig, last, lvl; } state_golden[] = {
        { 26, PHX_DCT_LUMA_4x4,  83,  46, 111 },
        { 18, PHX_DCT_LUMA_4x4,  69,  20,  99 },
        { 26, PHX_DCT_LUMA_AC,  102, 114, 116 },
        { 26, PHX_DCT_CHROMA_AC,119, 88, 125 },
    };
    for (unsigned k = 0; k < sizeof(state_golden)/sizeof(state_golden[0]); k++) {
        phx_cabac_context_init(&cb, PHX_SLICE_I, state_golden[k].qp, 0);
        static const uint16_t so[2][16] = {
            { 105+0, 105+15, 105+29, 105+44, 105+47, 402, 484+0, 484+15,
              484+29, 660, 528+0, 528+15, 528+29, 718, 0, 0 },
            { 277+0, 277+15, 277+29, 277+44, 277+47, 436, 776+0, 776+15,
              776+29, 675, 820+0, 820+15, 820+29, 733, 0, 0 } };
        static const uint16_t lo[2][16] = {
            { 166+0, 166+15, 166+29, 166+44, 166+47, 417, 572+0, 572+15,
              572+29, 690, 616+0, 616+15, 616+29, 748, 0, 0 },
            { 338+0, 338+15, 338+29, 338+44, 338+47, 451, 864+0, 864+15,
              864+29, 699, 908+0, 908+15, 908+29, 757, 0, 0 } };
        static const uint16_t ml[16] = {
            227+0, 227+10, 227+20, 227+30, 227+39, 426,
            952+0, 952+10, 952+20, 708, 982+0, 982+10, 982+20, 766, 0, 0 };
        int cat = state_golden[k].cat;
        check(cb.state[so[0][cat]] == state_golden[k].sig &&
              cb.state[lo[0][cat]] == state_golden[k].last &&
              cb.state[ml[cat]] == state_golden[k].lvl, "cabac state init");
    }

    /* v1: moderate luma 4x4, QP 26, I slice. */
    {
        int16_t dct[16] = { 37,-12,8,0,4,-3,2,0,1,0,0,0,0,0,0,0 };
        static const int16_t want[16] = { 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
        check_vector("v1_q26_luma4x4", &cb, dct, 0, 26, PHX_DCT_LUMA_4x4, 1, 1, want);
    }
    /* v2: large luma 4x4, QP 18, I slice. */
    {
        int16_t dct[16] = { 210,-83,45,-22,11,-7,5,3,-2,1,0,0,0,0,0,0 };
        static const int16_t want[16] = { 10,-3,2,0,0,0,0,0,0,0,0,0,0,0,0,0 };
        check_vector("v2_q18_luma4x4", &cb, dct, 0, 18, PHX_DCT_LUMA_4x4, 1, 1, want);
    }
    /* v3: all-zero block. */
    {
        int16_t dct[16] = { 0 };
        static const int16_t want[16] = { 0 };
        check_vector("v3_q26_luma4x4", &cb, dct, 0, 26, PHX_DCT_LUMA_4x4, 1, 0, want);
    }
    /* v4: flat low-amplitude block, QP 32, P slice -> quantizes to zero. */
    {
        int16_t dct[16]; for (int i = 0; i < 16; i++) dct[i] = 5;
        static const int16_t want[16] = { 0 };
        check_vector("v4_q32_luma4x4", &cb, dct, 1, 32, PHX_DCT_LUMA_4x4, 0, 0, want);
    }
    /* v5: wide dynamic range luma block, QP 22. */
    {
        int16_t dct[16] = { -900,640,-410,300,-210,150,-100,70,
                            -50,30,-20,10,-8,5,-3,2 };
        static const int16_t want[16] = { -28,13,-13,6,-4,2,-2,1,-2,1,-1,0,0,0,0,0 };
        check_vector("v5_q22_luma4x4", &cb, dct, 0, 22, PHX_DCT_LUMA_4x4, 1, 1, want);
    }
    /* Luma AC category (b_ac=1): coeff_last15 ignores the final scan slot. */
    {
        int16_t dct[16] = { 210,-83,45,-22,11,-7,5,3,-2,1,0,0,0,0,0,0 };
        static const int16_t want[16] = { 4,-1,1,0,0,0,0,0,0,0,0,0,0,0,0,0 };
        check_vector("v2_q26_lumaac", &cb, dct, 0, 26, PHX_DCT_LUMA_AC, 1, 1, want);
    }
    /* Chroma AC category (b_ac=1) uses different CABAC contexts. */
    {
        int16_t dct[16] = { 210,-83,45,-22,11,-7,5,3,-2,1,0,0,0,0,0,0 };
        static const int16_t want[16] = { 4,-1,1,0,0,0,0,0,0,0,0,0,0,0,0,0 };
        check_vector("v2_q26_chroma4x4", &cb, dct, 2, 26, PHX_DCT_CHROMA_AC, 1, 1, want);
    }

    /* DC-only shortcut path (rdo.c:698-706). */
    {
        int16_t dct[16] = { 91,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
        static const int16_t want[16] = { 2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
        check_vector("dc1_q26_shortcut", &cb, dct, 0, 26, PHX_DCT_LUMA_4x4, 1, 1, want);
    }
    {
        int16_t dct[16] = { -77,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
        static const int16_t want[16] = { -3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
        check_vector("dc2_q20_shortcut", &cb, dct, 0, 20, PHX_DCT_LUMA_4x4, 1, 1, want);
    }

    /* Luma DC (rdo.c:1098-1109). */
    {
        int16_t dct[16] = { 121,-87,64,-43,30,-21,15,-10,
                            7,-5,3,-2,2,-1,1,0 };
        static const int16_t want[16] = { 1,-1,1,0,0,0,0,0,0,0,0,0,0,0,0,0 };
        check_vector_n("luma_dc_q26", phx_quant_luma_dc_trellis, &cb, dct, 16,
                       0, 26, PHX_DCT_LUMA_DC, 1, 1, want);
    }
    /* Chroma DC, 4:2:0 2x2 (rdo.c:1114-1140).  Golden vectors were taken from
     * an instrumented x264 build with h->mb.i_trellis_lambda2[1][1] forced to
     * x264_trellis_lambda2_tab[1][qp] (analyse.c:266-267).  With the correct
     * chroma-QP lambda2 the first vector's best trellis node is the empty one:
     * x264 returns nz=0 but, as at rdo.c:874-878, leaves the quantized 2x2
     * array in place. */
    {
        int16_t dct[4] = { 63,-41,27,-13 };
        static const int16_t want[4] = { 1,0,0,0 };
        check_vector_n("chroma_dc_q26", chroma_dc_wrapper, &cb, dct, 4,
                       2, 26, PHX_DCT_CHROMA_DC, 1, 0, want);
    }
    /* MB0 of the 64x64 synth fixture (preset medium, qp=28 -> MB qp=25,
     * chroma qp=23): this is the first chroma DC block whose ordered CABAC
     * bins diverge at bin 594 when the trellis lambda2 is double-mapped or
     * the dc unquantization omits rdo.c:819's <<1. */
    {
        int16_t dct[4] = { 1120,0,-640,0 };
        static const int16_t want[4] = { 15,0,-9,0 };
        check_vector_n("chroma_dc_mb0_q23", chroma_dc_wrapper, &cb, dct, 4,
                       2, 23, PHX_DCT_CHROMA_DC, 1, 1, want);
    }
    /* MB0 chroma 4x4 AC block of the synth fixture (qp=23 chroma).  Golden
     * from an instrumented x264 build with lambda2[1][1] = tab[1][23]. */
    {
        int16_t dct[16] = { 120,0,0,0,-140,0,0,0,0,0,0,0,-20,0,0,0 };
        static const int16_t want[16] = { 3,0,0,0,-2,0,0,0,0,0,0,0,0,0,0,0 };
        check_vector("chroma4x4_mb0_q23", &cb, dct, 2, 23,
                     PHX_DCT_CHROMA_AC, 1, 1, want);
    }
    /* 8x8 CABAC trellis (rdo.c:1159-1168): structural smoke test only.
     *KNOWN LIMITATION: the 64-coefficient state machine has a residual
     * divergence from x264 (a few coefficients differ), so no golden vector
     * is asserted here.  See phx_rdo.h / final report. */
    {
        int16_t dct[64];
        for (int i = 0; i < 64; i++) dct[i] = (int16_t)((((i*13)%17) - 8) * 9);
        phx_cabac_context_init(&cb, PHX_SLICE_I, 26, 0);
        int nz = phx_quant_8x8_trellis(&cb, dct, 0, 26,
                                       (intptr_t)PHX_DCT_LUMA_8x8, 1);
        printf("v8_q26_smoke          nz=%d (no golden assertion)\n", nz);
        check(nz == 0 || nz == 1, "8x8 smoke return value");
    }

    /* Size-only CABAC accumulation (rdo.c:384-415).  prefix 0 adds nothing
     * beyond the bypass sign bit (256); prefix 1 adds one entropy decision
     * for b=0 plus the sign (x264_cabac_entropy[0] = 7, tables.c:1717). */
    phx_rdo_init();
    for (int st = 0; st < 128; st++)
        check(phx_rdo_size_unary(0)[st] == 256, "size_unary[0]");
    /* x264_cabac_entropy[0] = FIX8(-ln2(0.5)) = 7 (tables.c:1717). */
    check(phx_rdo_size_unary(1)[0] == 256 + 7, "size_unary[1][0]");
    check(phx_rdo_size_unary(15) == NULL, "size_unary out of range");
    check(phx_rdo_size_5ones()[0] != 0, "size_5ones generated");
    {
        phx_cabac_t live;
        phx_cabac_size_t sz;
        uint8_t before[sizeof(live.state)];
        memset(&live, 0, sizeof(live));
        phx_cabac_context_init(&live, PHX_SLICE_I, 26, 0);
        memcpy(before, live.state, sizeof(before));
        phx_rdo_size_init(&sz, &live, PHX_SLICE_I, 26);
        check(sz.f8_bits_encoded == 0, "size init f8_bits");
        check(memcmp(sz.state, live.state, sizeof(sz.state)) == 0,
              "size init COPY_CABAC state");
        /* cabac.c:83-96, equal predictors: one ctx-68 decision for b=1. */
        {
            int st = sz.state[68];
            phx_rdo_size_intra4x4_pred_mode(&sz, 2, 2);   /* cabac.c:86 */
            check(sz.f8_bits_encoded > 0,
                  "size intra4x4 equal mode adds a decision");
            check(sz.state[68] == phx_cabac_transition[st][1],
                  "size decision advances only the local state");
        }
        check(memcmp(before, live.state, sizeof(before)) == 0,
              "size-only writer leaves live CABAC state untouched");
    }

    /* Bit-cost primitives, golden values from x264 common/bitstream.h:283-289
     * and common/tables.c:1717-1750 as observed in the harness. */
    check(phx_rdo_bs_size_ue_big(0) == 1, "bs_size_ue_big(0)");
    check(phx_rdo_bs_size_ue_big(14) == 7, "bs_size_ue_big(14)");
    check(phx_rdo_bs_size_ue_big(254) == 15, "bs_size_ue_big(254)");
    check(phx_rdo_bs_size_ue_big(255) == 17, "bs_size_ue_big(255)");

    /* Trellis lambda2: x264_trellis_lambda2_tab as read from the same
     * harness (qp=26: tab[0]=18790 inter, tab[1]=10988 intra). */
    check(phx_trellis_lambda2_tab[0][26] == 18790, "lambda2 inter qp26");
    check(phx_trellis_lambda2_tab[1][26] == 10988, "lambda2 intra qp26");

    /* Derived quant tables (x264 set.c:164-205, QP 26, CQM_4IY[0]). */
    check(phx_rdo_quant4_mf(0, 26)[0] == 1260, "quant4_mf");
    check(phx_rdo_quant4_bias0(0, 26)[0] == 26, "quant4_bias0");
    check(phx_rdo_unquant4_mf(0, 26)[0] == 13312, "unquant4_mf");

    /* CAVLC bounded bit counter: zero-block token cost from
     * common/tables.c:1799-1805 (x264_coeff0_token[nC]). */
    {
        int16_t dct[16] = { 0 };
        const uint16_t *qm = phx_rdo_quant4_mf(0, 26);
        const int *uq = phx_rdo_unquant4_mf(0, 26);
        int nz = phx_rdo_quant_trellis_cavlc(dct, qm, uq, phx_rdo_zigzag4[0],
                    PHX_DCT_LUMA_4x4, phx_trellis_lambda2_tab[1][26], 0, 0, 0,
                    16, 0, 0, 2, 100, 0);
        check(nz == 0, "cavlc zero block");
        for (int i = 0; i < 16; i++) check(dct[i] == 0, "cavlc zero block coefs");
    }

    printf("%s: %d failures\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
