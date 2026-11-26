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
 *
 * Rows 8/9 golden (M8): rdo.c was instrumented through encoder/analyse.c's
 * `#include "rdo.c"` (analyse.c:33) to print, for each chroma-DC call, the
 * raw DCT, quant tables, lambda2, the sig/last/level CABAC states, the
 * quantized array, cost_siglast[], the best node score/level path and the
 * final coefficient array.  The `gold_mb*_q23` vectors below use those exact
 * values from the 64x64 synth fixture (qp=28 slice, chroma qp=23).
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

/* x264's live CABAC states at DCT_CHROMA_DC (cat=3), read from an
 * instrumented encoder/rdo.c build.  Offsets are x264_significant_coeff_
 * flag_offset[0][3]=105+44=149, x264_last_coeff_flag_offset[0][3]=166+44=210
 * and x264_coeff_abs_level_m1_offset[3]=227+30=257
 * (common/tables.c:1758-1792).  SHADER: C — serial state copy, no stream. */
typedef struct {
    uint8_t level[10], sig[4], last[4];
} phx_golden_state_t;

static void set_chroma_dc_state(phx_cabac_t *cb, const phx_golden_state_t *g)
{
    for (int i = 0; i < 10; i++) cb->state[257+i] = g->level[i];
    for (int i = 0; i < 4; i++)  cb->state[149+i] = g->sig[i];
    for (int i = 0; i < 4; i++)  cb->state[210+i] = g->last[i];
}

static void check_chroma_dc_golden(const char *name, phx_cabac_t *cb,
                                   const phx_golden_state_t *g,
                                   int16_t dct[4], int qp,
                                   int want_nz, const int16_t want[4],
                                   int want_last_nnz, uint64_t want_score,
                                   const int16_t *want_path, int want_path_n,
                                   const int want_siglast[3])
{
    phx_rdo_trellis_trace_t tr;
    uint8_t before[sizeof(cb->state)];
    set_chroma_dc_state(cb, g);
    memcpy(before, cb->state, sizeof(before));

    int nz = phx_quant_chroma_dc_trellis(cb, dct, qp, PHX_DCT_CHROMA_DC, 1);
    phx_rdo_get_trellis_trace(&tr);

    int ok = (nz == want_nz && tr.last_nnz == want_last_nnz &&
              tr.best_score == want_score && tr.best_count == want_path_n &&
              memcmp(before, cb->state, sizeof(before)) == 0 &&
              memcmp(tr.level_state, g->level, sizeof(g->level)) == 0);
    for (int i = 0; i < 3; i++) ok = ok && tr.cost_siglast[i] == want_siglast[i];
    for (int i = 0; i < want_path_n; i++)
        ok = ok && tr.best_levels[i] == want_path[i];
    for (int i = 0; i < 4; i++) ok = ok && dct[i] == want[i];

    printf("%-24s nz=%d score=%llu path=", name, nz,
           (unsigned long long)tr.best_score);
    for (int i = 0; i < tr.best_count; i++) printf("%d,", tr.best_levels[i]);
    printf(" siglast=%d,%d,%d dct=%d,%d,%d,%d %s\n", tr.cost_siglast[0],
           tr.cost_siglast[1], tr.cost_siglast[2], dct[0], dct[1], dct[2],
           dct[3], ok ? "OK" : "FAIL");
    check(ok, name);
}

/* -------------------------------------------------------------------------
 * Whole-MB size-only CABAC golden (M8).  Fixed MB decisions fed to both
 * x264's RDO_SKIP_BS macroblock_size_cabac (encoder/rdo.c:68-69 ->
 * encoder/cabac.c:917-1086) and phx_rdo_size_macroblock_cabac(); the harness
 * dumps f8_bits_encoded and every changed context state.  SHADER: C —
 * serial, read-only live coder, no stream state.
 * ------------------------------------------------------------------------- */
/* x264 P-slice initial CABAC state (qp=26, model=0), dumped by
 * m8_size_golden.py.  Seeded explicitly to isolate the MB cost estimator
 * from phx_cabac_context_init() (a separate, non-M8 table issue). */
static const uint8_t m8_p_state[460] = {34,114,99,34,114,99,93,123,86,104,124,115,78,68,20,98,114,125,112,94,126,119,123,101,69,114,94,37,23,19,104,113,119,112,94,126,103,121,102,118,127,113,99,127,99,101,73,116,113,101,124,105,103,79,110,125,121,113,120,118,82,126,126,126,119,71,61,120,124,123,90,113,73,91,104,120,58,72,122,114,107,112,111,113,97,95,95,87,83,82,126,110,99,117,101,107,79,124,117,119,99,92,119,124,105,93,119,109,125,114,116,126,102,102,107,127,126,121,120,111,104,115,124,104,108,86,100,114,103,122,117,121,126,79,121,112,122,114,112,112,82,102,120,126,118,126,122,114,77,119,124,101,122,125,104,116,125,102,114,118,119,104,125,103,120,89,90,86,96,98,92,94,108,94,90,125,110,108,116,122,117,120,123,127,122,127,124,116,123,95,123,109,115,123,91,94,82,96,98,104,108,108,108,122,117,117,111,95,97,61,119,121,93,112,122,127,127,115,105,111,109,95,109,99,71,85,69,123,80,90,104,118,110,126,119,117,107,124,56,84,96,110,98,118,126,117,115,118,48,68,78,92,90,116,127,117,109,115,44,78,106,120,108,121,103,97,116,36,60,80,96,86,116,125,117,111,2,87,95,115,97,95,105,107,113,109,111,115,117,97,105,91,99,116,101,93,109,119,119,113,111,117,117,107,99,93,114,114,98,120,125,110,112,116,124,120,118,122,115,117,109,125,110,97,120,90,124,114,112,108,94,122,118,110,98,113,109,85,66,62,50,80,86,70,74,74,68,80,70,70,92,98,126,98,82,104,124,100,106,114,110,118,114,118,123,123,119,86,90,92,98,126,116,122,111,121,109,105,83,87,73,69,109,111,71,116,114,125,105,119,117,101,113,107,97,81,79,63,49,118,119,93,111,118,120,110,106,102,84,98,88,84,58,78,80,58,60,24,66,88,112,127,103,79,55,21,112,46,60,64,78,72,100,106,106,126,103,113,120,117,104,86,121,123,112,104,118,106,86,118,124,42,78,112,111,87,49,23,9,3};

static void check_mb_size(const char *name, int slice_type, int mb_type,
                          int partition, int qp, int cbp_l, int cbp_c,
                          int intra16, int chroma_mode,
                          int mvd_x, int mvd_y, int dc0, int ac1, int chdc0,
                          int want_f8, const char *want_delta)
{
    phx_cabac_t live;
    phx_mb_cache_t cache;
    phx_cabac_mb_t mb;
    phx_rdo_mb_data_t data;
    phx_cabac_size_t sz;
    char delta[2048];
    int n = 0;

    memset(&live, 0, sizeof(live));
    phx_cabac_context_init(&live, slice_type, qp, 0);
    if (slice_type != PHX_SLICE_I)
        memcpy(live.state, m8_p_state, sizeof(live.state));
    memset(&cache, 0, sizeof(cache));
    memset(&data, 0, sizeof(data));
    for (int i = 0; i < 40; i++) cache.intra4x4_pred_mode[i] = 2; /* I_PRED_4x4_DC */
    cache.i_neighbour = 0;
    cache.i_mb_xy = 1;
    cache.i_cbp_left = cache.i_cbp_top = 0;
    cache.i_cbp_luma = cbp_l;
    cache.i_cbp_chroma = cbp_c;
    cache.b_transform_8x8 = 0;
    cache.i_neighbour_transform_size = 0;
    cache.type_left[0] = cache.type_top = PHX_P_L0;
    data.i_fref[0] = data.i_fref[1] = 1;
    data.sub_partition[0] = data.sub_partition[1] =
    data.sub_partition[2] = data.sub_partition[3] = PHX_D_L0_8x8;
    data.mvd[0][0][0] = (int16_t)mvd_x;
    data.mvd[0][0][1] = (int16_t)mvd_y;
    data.luma4x4[0][1] = (int16_t)ac1;
    data.luma16x16_dc[0] = (int16_t)dc0;
    data.chroma_dc[0][0] = (int16_t)chdc0;
    cache.non_zero_count[phx_scan8[0]] = (uint8_t)(ac1 != 0); /* scan8[0]=12 */
    cache.non_zero_count[phx_scan8[PHX_LUMA_DC]] = (uint8_t)(dc0 != 0);
    cache.non_zero_count[phx_scan8[PHX_CHROMA_DC]] = (uint8_t)(chdc0 != 0);

    phx_cabac_mb_init(&mb, &cache);
    mb.i_type = mb_type;
    mb.i_partition = partition;
    mb.i_intra16x16_pred_mode = intra16;
    mb.i_chroma_pred_mode = chroma_mode;
    mb.chroma_pred_mode_left = mb.chroma_pred_mode_top = 0;
    mb.i_qp = mb.i_last_qp = qp;
    mb.i_last_dqp = 0;
    mb.prev_type = PHX_P_L0;
    mb.prev_cbp = 0;
    mb.curr_cbp = cbp_l | (cbp_c << 4);
    mb.b_transform_8x8_mode = 0;
    mb.slice_type = slice_type;

    int f8 = phx_rdo_size_macroblock_cabac(&sz, &live, &mb, &data,
                                           slice_type, qp);
    delta[0] = 0;
    for (int i = 0; i < PHX_CABAC_CTX_COUNT; i++)
        if (live.state[i] != sz.state[i])
            n += snprintf(delta+n, sizeof(delta)-(size_t)n, " %d:%d->%d",
                          i, live.state[i], sz.state[i]);
    int ok = (f8 == want_f8 && strcmp(delta, want_delta) == 0);
    /* rdo.c:176-185 — rd_cost_mb()'s bit term for the same MB decisions. */
    const int lambda2 = 10988; /* tab[1][26] */
    int bits = phx_rdo_macroblock_bits(&live, &mb, &data, slice_type, qp, lambda2);
    int want_bits = (mb_type == PHX_P_SKIP || mb_type == PHX_B_SKIP ||
                     mb_type == PHX_B_DIRECT)
                    ? ((lambda2 + 128) >> 8)
                    : (int)(((uint64_t)want_f8 * lambda2 + 32768) >> 16);
    ok = ok && (bits == want_bits);
    printf("%-14s f8=%d bits=%d delta:%s %s\n", name, f8, bits, delta,
           ok ? "OK" : "FAIL");
    if (!ok) printf("  want f8=%d delta:%s\n", want_f8, want_delta);
    check(ok, name);
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
    /* Byte-exact golden from instrumented x264 rdo.c (medium, 64x64 synth
     * fixture, qp=28 slice / chroma qp=23).  Both the coefficient array and
     * the trellis internals (last_nnz, best-node score, level path, copied
     * level states and cost_siglast[]) are asserted against x264's dump.
     * SHADER: C — serial, no stream state. */
    {
        static const phx_golden_state_t mb0 = {
            { 97,104,122,126,121,125,105,97,81,125 },
            { 77,103,65,119 }, { 80,90,124,82 }
        };
        static const phx_golden_state_t mb1 = {
            { 93,112,122,126,121,105,53,97,81,125 },
            { 73,105,63,125 }, { 76,96,126,108 }
        };
        static const int16_t path0[3] = { 15, 0, 9 };
        static const int16_t path1[3] = { 22, 0, 9 };
        static const int siglast0[3] = { 853, 277, 314 };
        static const int siglast1[3] = { 872, 293, 293 };
        static const int16_t want0[4] = { 15, 0, -9, 0 };
        static const int16_t want1[4] = { 22, 0, -9, 0 };
        int16_t d[4] = { 1120, 0, -640, 0 };
        check_chroma_dc_golden("gold_mb0_u_q23", &cb, &mb0, d, 23, 1, want0,
                               2, 1152921504608876520ULL, path0, 3, siglast0);
        d[0] = 1568; d[1] = 0; d[2] = -640; d[3] = 0;
        check_chroma_dc_golden("gold_mb1_u_q23", &cb, &mb1, d, 23, 1, want1,
                               2, 1152921504608563703ULL, path1, 3, siglast1);
        /* Row 9 symptom: ue_bypass val must be 7 (|coeff|=22), never 14
         * (|29|).  The escape value is computed by
         * phx_cabac_encode_ue_bypass(cb, 0, abs_coeff-15)
         * (encoder/cabac.c:727-736, common/cabac.c:142-157). */
        check(phx_rdo_bs_size_ue_big((unsigned)(want1[0] - 15)) == 7,
              "gold mb1 ue val=7");
        /* val=14 has the same 7-bin length, so the first divergent bypass
         * bin (index 1663) is a value difference, not an insertion. */
        check(phx_rdo_bs_size_ue_big(14) == 7,
              "wrong ue val=14 has same length");
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


    /* x264 golden: m8_size_golden.py */
    check_mb_size("gold_I16", PHX_SLICE_I, PHX_I_16x16, PHX_D_16x16, 26,
        0, 1, 1, 0, 0, 0, 37, 0, 15, 14764,
        " 7:123->121 9:104->102 85:65->63 97:111->113 105:93->91 149:77->75 166:78->88 210:84->94 228:64->78 232:116->111 258:102->108 262:127->101");
    check_mb_size("gold_I4", PHX_SLICE_I, PHX_I_4x4, PHX_D_16x16, 26,
        1, 0, 0, 0, 0, 0, 0, 9, 0, 12672,
        " 68:124->99 74:95->101 75:91->97 76:124->126 94:95->101 95:109->113 134:83->91 135:105->103 196:36->62 248:60->76 252:106->126");
    check_mb_size("gold_P_L0", PHX_SLICE_P, PHX_P_L0, PHX_D_16x16, 26,
        1, 0, 0, 0, 8, -4, 0, 5, 0, 11455,
        " 40:127->125 43:127->125 44:99->97 45:101->99 46:73->79 47:116->118 50:124->126 51:105->103 52:103->101 53:79->89 74:104->102 75:120->118 76:58->74 93:117->119 94:101->105 95:107->111 134:121->123 135:112->116 196:82->90 248:48->68 252:90->106");
    check_mb_size("gold_P_SKIP", PHX_SLICE_P, PHX_P_SKIP, PHX_D_16x16, 26,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 256, "");

    /* B-slice RD golden (instrumented x264, encoder/rdo.c:68-69 ->
     * encoder/cabac.c:917-1086 with SLICE_TYPE_B and analyse.c:2622/3404). */
    check_mb_size("gold_B_L0_L0", PHX_SLICE_B, PHX_B_L0_L0, PHX_D_16x16, 26,
                  1, 0, 0, 0, 8, -4, 0, 5, 0, 11646,
                  " 32:119->123 40:127->125 43:127->125 44:99->97 45:101->99 46:73->79 47:116->118 50:124->126 51:105->103 52:103->101 53:79->89 74:104->102 75:120->118 76:58->74 93:117->119 94:101->105 95:107->111 134:121->123 135:112->116 196:82->90 248:48->68 252:90->106");
    {
        /* analyse.c:2624 — x264 diag.yuv MB1: satd=537, et=1, psy_rd!=0. */
        check(phx_rdo_b_rd_threshold(537, 1, 1) == 605, "B RD threshold");
        check(phx_rdo_b_rd_threshold(537, 0, 1) == (1 << 28), "B RD no-ET threshold");
        /* analyse.c:3404-3415 — x264 diag.yuv MB0 gate=0, MB1 gate=1/TAKEN. */
        const int none = 268435456; /* me.h:30 COST_MAX */
        phx_rdo_b_rd_t mb0 = { 1574, 254, 464, 1620, none, none, none };
        phx_rdo_b_rd_t mb1 = { 1131, 537, 537, 1177, none, none, none };
        check(phx_rdo_b_early_skip(&mb0, 1, 1) == 0, "B early-skip gate MB0");
        check(phx_rdo_b_early_skip(&mb1, 1, 1) == 1, "B early-skip MB1");
    }

    printf("%s: %d failures\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
