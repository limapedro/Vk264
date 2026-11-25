/* Verify phx_param defaults/presets against x264 common/base.c:344-602.
 * Expected table mirrors docs/PRESET_PARAM_MATRIX.md §1 (x264 values).
 */
#include "../src/phx_param.h"
#include <stdio.h>
#include <string.h>

struct expected {
    const char *name;
    int refs, bframes, scenecut, badapt, deblock, cabac, t8x8;
    phx_me_method_t me;
    int subme, merange, mixedref, fastpskip, wpred, trellis, mbtree, lookahead;
};

static const struct expected E[] = {
    /* name       ref b scen bad debl cab t8 me         sub mer mix fst wp tre mbt look */
    {"ultrafast",  1, 0,   0,  0,   0,  0, 0, PHX_ME_DIA,  0, 16, 0, 1, 0, 0, 0,  0},
    {"superfast",  1, 3,  40,  1,   1,  1, 1, PHX_ME_DIA,  1, 16, 0, 1, 1, 0, 0,  0},
    {"veryfast",   1, 3,  40,  1,   1,  1, 1, PHX_ME_HEX,  2, 16, 0, 1, 1, 0, 1, 10},
    {"faster",     2, 3,  40,  1,   1,  1, 1, PHX_ME_HEX,  4, 16, 0, 1, 1, 1, 1, 20},
    {"fast",       2, 3,  40,  1,   1,  1, 1, PHX_ME_HEX,  6, 16, 1, 1, 1, 1, 1, 30},
    {"medium",     3, 3,  40,  1,   1,  1, 1, PHX_ME_HEX,  7, 16, 1, 1, 2, 1, 1, 40},
    {"slow",       5, 3,  40,  1,   1,  1, 1, PHX_ME_HEX,  8, 16, 1, 1, 2, 2, 1, 50},
    {"slower",     8, 3,  40,  2,   1,  1, 1, PHX_ME_UMH,  9, 16, 1, 1, 2, 2, 1, 60},
    {"veryslow",  16, 8,  40,  2,   1,  1, 1, PHX_ME_UMH, 10, 24, 1, 1, 2, 2, 1, 60},
    {"placebo",   16,16,  40,  2,   1,  1, 1, PHX_ME_TESA,11, 24, 1, 0, 2, 2, 1, 60},
};

int main(void)
{
    int fails = 0;
    for (size_t i = 0; i < sizeof(E)/sizeof(E[0]); ++i) {
        phx_param_t p;
        phx_param_default(&p);
        if (phx_param_apply_preset(&p, E[i].name)) {
            printf("FAIL %s: unknown preset\n", E[i].name); ++fails; continue;
        }
        struct { const char *f; int got, want; } c[] = {
            {"refs",p.i_frame_reference,E[i].refs},
            {"bframes",p.i_bframe,E[i].bframes},
            {"scenecut",p.i_scenecut_threshold,E[i].scenecut},
            {"badapt",(int)p.i_bframe_adaptive,E[i].badapt},
            {"deblock",p.b_deblocking_filter,E[i].deblock},
            {"cabac",p.b_cabac,E[i].cabac},
            {"8x8dct",p.b_transform_8x8,E[i].t8x8},
            {"me",(int)p.i_me_method,(int)E[i].me},
            {"subme",p.i_subpel_refine,E[i].subme},
            {"merange",p.i_me_range,E[i].merange},
            {"mixedref",p.b_mixed_references,E[i].mixedref},
            {"fastpskip",p.b_fast_pskip,E[i].fastpskip},
            {"wpred",p.i_weighted_pred,E[i].wpred},
            {"trellis",p.i_trellis,E[i].trellis},
            {"mbtree",p.b_mb_tree,E[i].mbtree},
            {"lookahead",p.i_lookahead,E[i].lookahead},
        };
        for (size_t k = 0; k < sizeof(c)/sizeof(c[0]); ++k)
            if (c[k].got != c[k].want) {
                printf("FAIL %-10s %-10s got %d want %d\n",
                       E[i].name, c[k].f, c[k].got, c[k].want); ++fails;
            }
        phx_param_finalize(&p);
        int want_chroma = (E[i].subme >= 6) ? -2 : 0;
        if (p.i_chroma_qp_index_offset != want_chroma) {
            printf("FAIL %-10s chroma-qp got %d want %d\n",
                   E[i].name, p.i_chroma_qp_index_offset, want_chroma); ++fails;
        }
    }
    printf("%s: %d presets, %d failures\n",
           fails ? "FAILED" : "OK", (int)(sizeof(E)/sizeof(E[0])), fails);
    return fails ? 1 : 0;
}
