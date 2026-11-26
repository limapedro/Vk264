/* Byte-exact golden tests for the full-pel + sub-pel ME port in src/phx_me.c.
 *
 * Goldens were produced by an instrumented build of the real x264 objects
 * (reference/x264/encoder/me.c + common/mc.c, MSVC C-only, BIT_DEPTH=8):
 * a temporary harness constructed x264_t/x264_me_t, called the real
 * x264_me_search_ref() and x264_me_refine_qpel() on the fixed frame pair,
 * and printed MV, SAD+lambda*bits and get_ref() samples.  No Phoenix code is
 * involved in producing the expected values.  The harness lives only in
 * C:\Users\multi\AppData\Local\Temp\opencode\m4_golden (SHADER: C — observe only).
 *
 * Source lines (exact, reference/x264/):
 *   encoder/me.c:276-419        fullpel MVP + DIA + HEX
 *   encoder/me.c:422-616        UMH uneven-cross multi-hexagon grid
 *   encoder/me.c:774-798        qpel output / subpel dispatch
 *   encoder/me.c:801-810        x264_me_refine_qpel (SAD hpel, SATD qpel)
 *   encoder/me.c:865-992        refine_subpel (subme=1 SAD shortcut :965-986)
 *   encoder/analyse.c:330-396   MV limits (i_fpel_border = 6, :333)
 *   common/mc.c:173-249         hpel_filter / mc_luma / get_ref
 *   common/tables.c:183-184     hpel_ref0 / hpel_ref1
 *   encoder/encoder.c:1409-1427 mbcmp_init (SAD for subme=1)
 *
 * SHADER: A — host-side test harness; fixed inputs, fixed expected outputs.
 */
#include "phx_me.h"
#include "phx_param.h"
#include "phx_tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

/* Fixed smooth 8-bit ramp: 2x2 constant cells, period 32 in x and y
 * (the same pattern the x264 instrumentation used). */
static uint8_t pat(int x, int y)
{
    return (uint8_t)(((x & 31) / 2) + 16*((y & 31) / 2));
}

/* Fill the whole padded luma plane so every MV candidate read is defined. */
static void fill_frames(phx_frame_t *fref, phx_frame_t *fenc,
                        int tx, int ty)
{
    for (int y = -PHX_PADV; y < fref->i_height + PHX_PADV; y++)
        for (int x = -PHX_PADH; x < fref->i_width + PHX_PADH; x++) {
            fref->plane[y*fref->i_stride + x] = pat(x, y);
            fenc->plane[y*fenc->i_stride + x] = pat(x + tx, y + ty);
        }
}

/* mc.c:704-726 — prepare the four hpel reference planes.  phx_frame_new()
 * gives stride 128 for a 64x64 frame, matching the harness. */
static int build_refplanes(phx_frame_t *fref, phx_me_refplanes_t *rp)
{
    return phx_me_refplanes_init(fref, rp);
}

/* Build fenc by quarter-pel interpolation of fref (mc.c:198-219).  Used for
 * the fractional-MV golden cases; phx_mc_luma16 is itself validated below by
 * the byte-exact REF table. */
static void make_frac_fenc(phx_frame_t *fenc, const phx_me_refplanes_t *rp,
                           int tx, int ty, int fdx, int fdy)
{
    uint8_t tmp[16*16];
    for (int by = 0; by < fenc->i_height/16; by++)
        for (int bx = 0; bx < fenc->i_width/16; bx++) {
            phx_mc_luma16(rp, (bx*16+tx)*4+fdx, (by*16+ty)*4+fdy,
                          tmp, 16, 16, 16);
            for (int yy = 0; yy < 16; yy++)
                memcpy(fenc->plane + (by*16+yy)*fenc->i_stride + bx*16,
                       tmp + yy*16, 16);
        }
}

/* One instrumented-x264 golden case.  fdx/fdy != 0 selects a fractional fenc. */
typedef struct {
    int method, mb_x, mb_y, tx, ty, mvpx, mvpy, fdx, fdy;
    int full_mv[2], full_cost;          /* x264_me_search_ref() output */
    int sub_mv[2], sub_cost, sub_cost_mv; /* x264_me_refine_qpel() output */
} me_golden_t;

static const me_golden_t goldens[] = {
    /* method, mb,    shift,   mvp,     frac,  fullpel MV/cost,   subpel MV/cost/cost_mv */
    {0,1,1,  5,-3,   0, 0,  0, 0, { 20,-12}, 118, { 20,-12}, 118, 118},
    {1,1,1,  5,-3,   0, 0,  0, 0, { 20,-12}, 118, { 20,-12}, 118, 118},
    {2,1,1,  5,-3,   0, 0,  0, 0, { 20,-12}, 118, { 20,-12}, 118, 118},
    {0,2,2, -7, 4,   0, 0,  0, 0, {-28, 16}, 128, {-28, 16}, 128, 128},
    {1,2,2, -7, 4,   0, 0,  0, 0, {-28, 16}, 128, {-28, 16}, 128, 128},
    {2,2,2, -7, 4,   0, 0,  0, 0, {-28, 16}, 128, {-28, 16}, 128, 128},
    {0,1,1,  0, 0,   0, 0,  0, 0, {  0,  0},   8, {  0,  0},   8,   8},
    {1,1,1,  0, 0,   0, 0,  0, 0, {  0,  0},   8, {  0,  0},   8,   8},
    {2,1,1,  0, 0,   0, 0,  0, 0, {  0,  0},   8, {  0,  0},   8,   8},
    {0,1,1,  5,-3,  20,-12, 0, 0, { 20,-12},   8, { 20,-12},   8,   8},
    {1,1,1,  5,-3,  20,-12, 0, 0, { 20,-12},   8, { 20,-12},   8,   8},
    {2,1,1,  5,-3,  20,-12, 0, 0, { 20,-12},   8, { 20,-12},   8,   8},
    {2,1,1,  9, 7,   0, 0,  0, 0, { 36, 28}, 142, { 36, 28}, 142, 142},
    {2,3,1, -9,-7,   0, 0,  0, 0, {-36,-28}, 142, {-36,-28}, 142, 142},
    {1,1,1,  2, 0,   0, 0,  0, 0, {  8,  0},  52, {  8,  0},  52,  52},
    {2,1,1,  2, 0,   0, 0,  0, 0, {  8,  0},  52, {  8,  0},  52,  52},
    /* fractional fenc: subpel refinement must move to a non-integer MV. */
    {1,1,1,  5,-3,   0, 0,  1, 1, { 20,-12}, 742, { 20,-11}, 292, 116},
    {2,1,1,  5,-3,   0, 0,  3,-2, { 24,-12},1161, { 23,-14}, 122, 122},
    {1,2,2, -7, 4,   0, 0, -1, 2, {-28, 20},1172, {-29, 18}, 130, 130},
};

/* x264 get_ref() 16x16 samples at absolute quarter-pel MVs, dumped by the
 * instrumented harness (common/mc.c:221-249).  512 hex digits = 256 bytes. */
typedef struct { int mvx, mvy; const char *hex; } ref_golden_t;
static const ref_golden_t ref_goldens[] = {
    {64,64,"888889898a8a8b8b8c8c8d8d8e8e8f8f888889898a8a8b8b8c8c8d8d8e8e8f8f989899999a9a9b9b9c9c9d9d9e9e9f9f989899999a9a9b9b9c9c9d9d9e9e9f9fa8a8a9a9aaaaababacacadadaeaeafafa8a8a9a9aaaaababacacadadaeaeafafb8b8b9b9bababbbbbcbcbdbdbebebfbfb8b8b9b9bababbbbbcbcbdbdbebebfbfc8c8c9c9cacacbcbcccccdcdcececfcfc8c8c9c9cacacbcbcccccdcdcececfcfd8d8d9d9dadadbdbdcdcdddddededfdfd8d8d9d9dadadbdbdcdcdddddededfdfe8e8e9e9eaeaebebececededeeeeefefe8e8e9e9eaeaebebececededeeeeefeff8f8f9f9fafafbfbfcfcfdfdfefefffff8f8f9f9fafafbfbfcfcfdfdfefeffff"},
    {68,64,"8889898a8a8b8b8c8c8d8d8e8e8f8f808889898a8a8b8b8c8c8d8d8e8e8f8f809899999a9a9b9b9c9c9d9d9e9e9f9f909899999a9a9b9b9c9c9d9d9e9e9f9f90a8a9a9aaaaababacacadadaeaeafafa0a8a9a9aaaaababacacadadaeaeafafa0b8b9b9bababbbbbcbcbdbdbebebfbfb0b8b9b9bababbbbbcbcbdbdbebebfbfb0c8c9c9cacacbcbcccccdcdcececfcfc0c8c9c9cacacbcbcccccdcdcececfcfc0d8d9d9dadadbdbdcdcdddddededfdfd0d8d9d9dadadbdbdcdcdddddededfdfd0e8e9e9eaeaebebececededeeeeefefe0e8e9e9eaeaebebececededeeeeefefe0f8f9f9fafafbfbfcfcfdfdfefefffff0f8f9f9fafafbfbfcfcfdfdfefefffff0"},
    {64,68,"888889898a8a8b8b8c8c8d8d8e8e8f8f989899999a9a9b9b9c9c9d9d9e9e9f9f989899999a9a9b9b9c9c9d9d9e9e9f9fa8a8a9a9aaaaababacacadadaeaeafafa8a8a9a9aaaaababacacadadaeaeafafb8b8b9b9bababbbbbcbcbdbdbebebfbfb8b8b9b9bababbbbbcbcbdbdbebebfbfc8c8c9c9cacacbcbcccccdcdcececfcfc8c8c9c9cacacbcbcccccdcdcececfcfd8d8d9d9dadadbdbdcdcdddddededfdfd8d8d9d9dadadbdbdcdcdddddededfdfe8e8e9e9eaeaebebececededeeeeefefe8e8e9e9eaeaebebececededeeeeefeff8f8f9f9fafafbfbfcfcfdfdfefefffff8f8f9f9fafafbfbfcfcfdfdfefeffff080809090a0a0b0b0c0c0d0d0e0e0f0f"},
    {68,68,"8889898a8a8b8b8c8c8d8d8e8e8f8f809899999a9a9b9b9c9c9d9d9e9e9f9f909899999a9a9b9b9c9c9d9d9e9e9f9f90a8a9a9aaaaababacacadadaeaeafafa0a8a9a9aaaaababacacadadaeaeafafa0b8b9b9bababbbbbcbcbdbdbebebfbfb0b8b9b9bababbbbbcbcbdbdbebebfbfb0c8c9c9cacacbcbcccccdcdcececfcfc0c8c9c9cacacbcbcccccdcdcececfcfc0d8d9d9dadadbdbdcdcdddddededfdfd0d8d9d9dadadbdbdcdcdddddededfdfd0e8e9e9eaeaebebececededeeeeefefe0e8e9e9eaeaebebececededeeeeefefe0f8f9f9fafafbfbfcfcfdfdfefefffff0f8f9f9fafafbfbfcfcfdfdfefefffff00809090a0a0b0b0c0c0d0d0e0e0f0f00"},
    {72,68,"89898a8a8b8b8c8c8d8d8e8e8f8f808099999a9a9b9b9c9c9d9d9e9e9f9f909099999a9a9b9b9c9c9d9d9e9e9f9f9090a9a9aaaaababacacadadaeaeafafa0a0a9a9aaaaababacacadadaeaeafafa0a0b9b9bababbbbbcbcbdbdbebebfbfb0b0b9b9bababbbbbcbcbdbdbebebfbfb0b0c9c9cacacbcbcccccdcdcececfcfc0c0c9c9cacacbcbcccccdcdcececfcfc0c0d9d9dadadbdbdcdcdddddededfdfd0d0d9d9dadadbdbdcdcdddddededfdfd0d0e9e9eaeaebebececededeeeeefefe0e0e9e9eaeaebebececededeeeeefefe0e0f9f9fafafbfbfcfcfdfdfefefffff0f0f9f9fafafbfbfcfcfdfdfefefffff0f009090a0a0b0b0c0c0d0d0e0e0f0f0000"},
    {76,76,"999a9a9b9b9c9c9d9d9e9e9f9f909091a9aaaaababacacadadaeaeafafa0a0a1a9aaaaababacacadadaeaeafafa0a0a1b9bababbbbbcbcbdbdbebebfbfb0b0b1b9bababbbbbcbcbdbdbebebfbfb0b0b1c9cacacbcbcccccdcdcececfcfc0c0c1c9cacacbcbcccccdcdcececfcfc0c0c1d9dadadbdbdcdcdddddededfdfd0d0d1d9dadadbdbdcdcdddddededfdfd0d0d1e9eaeaebebececededeeeeefefe0e0e1e9eaeaebebececededeeeeefefe0e0e1f9fafafbfbfcfcfdfdfefefffff0f0f1f9fafafbfbfcfcfdfdfefefffff0f0f1090a0a0b0b0c0c0d0d0e0e0f0f000001090a0a0b0b0c0c0d0d0e0e0f0f000001191a1a1b1b1c1c1d1d1e1e1f1f101011"},
    {60,72,"97989899999a9a9b9b9c9c9d9d9e9e9f97989899999a9a9b9b9c9c9d9d9e9e9fa7a8a8a9a9aaaaababacacadadaeaeafa7a8a8a9a9aaaaababacacadadaeaeafb7b8b8b9b9bababbbbbcbcbdbdbebebfb7b8b8b9b9bababbbbbcbcbdbdbebebfc7c8c8c9c9cacacbcbcccccdcdcececfc7c8c8c9c9cacacbcbcccccdcdcececfd7d8d8d9d9dadadbdbdcdcdddddededfd7d8d8d9d9dadadbdbdcdcdddddededfe7e8e8e9e9eaeaebebececededeeeeefe7e8e8e9e9eaeaebebececededeeeeeff7f8f8f9f9fafafbfbfcfcfdfdfefefff7f8f8f9f9fafafbfbfcfcfdfdfefeff07080809090a0a0b0b0c0c0d0d0e0e0f07080809090a0a0b0b0c0c0d0d0e0e0f"},
    {84,52,"6a6b6b6c6c6d6d6e6e6f6f60606161627a7b7b7c7c7d7d7e7e7f7f70707171727a7b7b7c7c7d7d7e7e7f7f70707171728a8b8b8c8c8d8d8e8e8f8f80808181828a8b8b8c8c8d8d8e8e8f8f80808181829a9b9b9c9c9d9d9e9e9f9f90909191929a9b9b9c9c9d9d9e9e9f9f9090919192aaababacacadadaeaeafafa0a0a1a1a2aaababacacadadaeaeafafa0a0a1a1a2babbbbbcbcbdbdbebebfbfb0b0b1b1b2babbbbbcbcbdbdbebebfbfb0b0b1b1b2cacbcbcccccdcdcececfcfc0c0c1c1c2cacbcbcccccdcdcececfcfc0c0c1c1c2dadbdbdcdcdddddededfdfd0d0d1d1d2dadbdbdcdcdddddededfdfd0d0d1d1d2eaebebececededeeeeefefe0e0e1e1e2"},
    {88,92,"bbbbbcbcbdbdbebebfbfb0b0b1b1b2b2cbcbcccccdcdcececfcfc0c0c1c1c2c2cbcbcccccdcdcececfcfc0c0c1c1c2c2dbdbdcdcdddddededfdfd0d0d1d1d2d2dbdbdcdcdddddededfdfd0d0d1d1d2d2ebebececededeeeeefefe0e0e1e1e2e2ebebececededeeeeefefe0e0e1e1e2e2fbfbfcfcfdfdfefefffff0f0f1f1f2f2fbfbfcfcfdfdfefefffff0f0f1f1f2f20b0b0c0c0d0d0e0e0f0f0000010102020b0b0c0c0d0d0e0e0f0f0000010102021b1b1c1c1d1d1e1e1f1f1010111112121b1b1c1c1d1d1e1e1f1f1010111112122b2b2c2c2d2d2e2e2f2f2020212122222b2b2c2c2d2d2e2e2f2f2020212122223b3b3c3c3d3d3e3e3f3f303031313232"},
    {52,44,"565757585859595a5a5b5b5c5c5d5d5e666767686869696a6a6b6b6c6c6d6d6e666767686869696a6a6b6b6c6c6d6d6e767777787879797a7a7b7b7c7c7d7d7e767777787879797a7a7b7b7c7c7d7d7e868787888889898a8a8b8b8c8c8d8d8e868787888889898a8a8b8b8c8c8d8d8e969797989899999a9a9b9b9c9c9d9d9e969797989899999a9a9b9b9c9c9d9d9ea6a7a7a8a8a9a9aaaaababacacadadaea6a7a7a8a8a9a9aaaaababacacadadaeb6b7b7b8b8b9b9bababbbbbcbcbdbdbeb6b7b7b8b8b9b9bababbbbbcbcbdbdbec6c7c7c8c8c9c9cacacbcbcccccdcdcec6c7c7c8c8c9c9cacacbcbcccccdcdced6d7d7d8d8d9d9dadadbdbdcdcddddde"},
};

/* Verify one instrumented-x264 golden end to end. */
static void case_golden(const phx_cost_tables_t *cost, int lambda,
                        const me_golden_t *g)
{
    int16_t pred[2] = { (int16_t)g->mvpx, (int16_t)g->mvpy };
    phx_frame_t *fref = phx_frame_new(64, 64, 0);
    phx_frame_t *fenc = phx_frame_new(64, 64, 0);
    phx_me_refplanes_t rp;
    CHECK(fref && fenc, "frame_new");
    if (!fref || !fenc) return;
    fill_frames(fref, fenc, g->tx, g->ty);
    CHECK(build_refplanes(fref, &rp) == 0, "refplanes_init");
    if (g->fdx || g->fdy)
        make_frac_fenc(fenc, &rp, g->tx, g->ty, g->fdx, g->fdy);

    int mv[2] = { 12345, 12345 };
    int full = phx_me16(g->mb_x*16, g->mb_y*16, fenc, fref, cost, 16, lambda,
                        g->method, pred, mv);
    /* Reproducibility: identical inputs must give identical MV and cost. */
    int mv2[2] = { 0, 0 };
    int full2 = phx_me16(g->mb_x*16, g->mb_y*16, fenc, fref, cost, 16, lambda,
                         g->method, pred, mv2);
    /* The log2 fallback (analyse.c:186-188/156) must reproduce the table. */
    int mv3[2] = { 0, 0 };
    int full3 = phx_me16(g->mb_x*16, g->mb_y*16, fenc, fref, NULL, 16, lambda,
                         g->method, pred, mv3);

    printf("  %s mb=(%d,%d) shift=(%d,%d) full=(%d,%d)/%d\n",
           g->method == 0 ? "DIA" : g->method == 1 ? "HEX" : "UMH",
           g->mb_x, g->mb_y, g->tx, g->ty, mv[0]/4, mv[1]/4, full);

    CHECK(full >= 0 && full < (int)PHX_ME_COST_MAX, "valid cost");
    CHECK(mv[0] == g->full_mv[0] && mv[1] == g->full_mv[1],
          "x264 fullpel MV golden");
    CHECK(full == g->full_cost, "x264 fullpel cost golden");
    CHECK(full == full2 && mv[0] == mv2[0] && mv[1] == mv2[1],
          "cost/MV reproducible");
    CHECK(full == full3 && mv[0] == mv3[0] && mv[1] == mv3[1],
          "table == log2 fallback");

    int scost = full, scmv = 0;
    CHECK(phx_me_refine_subpel(g->mb_x, g->mb_y, 4, 4, fenc, &rp, cost, 16,
                               lambda, 1, 1, pred, mv, &scost, &scmv) == 0,
          "refine_subpel");
    printf("    subpel=(%d,%d)/%d cost_mv=%d\n", mv[0]/4, mv[1]/4, scost, scmv);
    CHECK(mv[0] == g->sub_mv[0] && mv[1] == g->sub_mv[1],
          "x264 subpel MV golden");
    CHECK(scost == g->sub_cost, "x264 subpel cost golden");
    CHECK(scmv == g->sub_cost_mv, "x264 subpel cost_mv golden");

    phx_me_refplanes_free(&rp);
    phx_frame_delete(fref);
    phx_frame_delete(fenc);
}

/* Byte-exact interpolation check against x264 get_ref() samples. */
static void check_interpolation(const phx_cost_tables_t *cost)
{
    (void)cost;
    phx_frame_t *fref = phx_frame_new(64, 64, 0);
    phx_me_refplanes_t rp;
    CHECK(fref != NULL, "frame_new interpolation");
    if (!fref) return;
    for (int y = -PHX_PADV; y < fref->i_height + PHX_PADV; y++)
        for (int x = -PHX_PADH; x < fref->i_width + PHX_PADH; x++)
            fref->plane[y*fref->i_stride + x] = pat(x, y);
    CHECK(build_refplanes(fref, &rp) == 0, "refplanes_init interpolation");

    char hex[513];
    uint8_t dst[16*16];
    for (unsigned i = 0; i < sizeof(ref_goldens)/sizeof(ref_goldens[0]); i++) {
        const ref_golden_t *g = &ref_goldens[i];
        phx_mc_luma16(&rp, g->mvx, g->mvy, dst, 16, 16, 16);
        for (int j = 0; j < 256; j++)
            snprintf(hex + 2*j, 3, "%02x", dst[j]);
        hex[512] = '\0';
        if (strcmp(hex, g->hex))
            printf("FAIL interpolation MV=(%d,%d)\n  phx=%s\n  x264=%s\n",
                   g->mvx, g->mvy, hex, g->hex);
        CHECK(strcmp(hex, g->hex) == 0, "x264 interpolated samples golden");
    }
    printf("  interpolation: %u x 256 samples byte-exact\n",
           (unsigned)(sizeof(ref_goldens)/sizeof(ref_goldens[0])));
    phx_me_refplanes_free(&rp);
    phx_frame_delete(fref);
}

/* ---- real-clip input comparison (build/oracle_full/diag.yuv) ------------ */
/* Instrumented x264 (real encoder objects, encoder/me.c:796 and :809) and a
 * traced Phoenix encode of the same 64x64 clip (--qp 28 --keyint 30
 * --min-keyint 30 --scenecut 0 --cabac) were compared at the two reported
 * over-skip MBs.  Replaying x264's EXACT inputs through phx_me_refine_subpel_mv
 * reproduces x264's refined MV/cost byte-for-byte, so the divergence is in
 * the inputs supplied by the caller, not in this file:
 *
 *   medium frame=1 MB(1,1), site me.c:796 (subme=7, hpel=2, qpel=2, br=0):
 *     x264: mv=(0,0) cost=368 mvp=(0,0) qp=30 lambda=8,
 *           cost_mv[-8..8]=[64,62,59,55,51,46,39,30,6,30,39,46,51,55,59,62,64]
 *           -> refined (0,0)/599, cost_mv=12.
 *     Phoenix replayer with those exact bytes -> (0,0)/599, cost_mv=12 (exact).
 *     The real Phoenix trial at its MB(1,1) instead had mv=(12,-52),
 *     cost=1071, mvp=(11,-47), lambda=6 (qp=28), table [48,46,...,4,...].
 *
 *   veryfast frame=3 MB(0,0), site me.c:809 (subme=2, hpel=0, qpel=1, br=1):
 *     x264: mv=(0,20) cost=507 mvp=(0,0) i_ref_cost=0 qp=30 lambda=8
 *           -> refined (1,20)/485, cost_mv=114.
 *     Phoenix replayer with those exact bytes -> (1,20)/485, cost_mv=114.
 *
 * Derived upstream differences (all outside M4's owned files):
 *   1. QP/lambda: x264's per-MB qp at these sites is 30 (lambda=8,
 *      tables.c:97-104), Phoenix passes the CQP qp=28 (lambda=6).
 *   2. Frame pairing: Phoenix's first analysed P frame has the exact luma of
 *      x264's DISPLAY frame 4 (16x16 ENC bytes match); x264's frames 1-3 are
 *      B frames (analyse.c:2953-3733), so the reported frame 1/3 trials are
 *      not the same source pictures in the two encoders.
 *   3. Reference contents: x264's p_fref[] is the reconstructed/deblocked
 *      frame (macroblock.c:1438-1622), Phoenix's fref is the raw source frame
 *      (its 4x4 sample block equals x264's frame-1 ENC, not x264's REF).
 *   4. MVP/fullpel MV/cost follow from (1)-(3): e.g. x264 mvp=(0,0)/mv=(0,0)
 *      vs Phoenix mvp=(11,-47)/mv=(12,-52) at the matched MB.
 * hpel-filtered planes (mc.c:173-196/704-726) produced from x264's own fref
 * bytes give byte-identical refinement, so the refplanes state is not at fault.
 * SHADER: C — observation only; no approximation in the golden values above. */

/* ---- public one-call refinement API (x264_me_refine_qpel analogue) ------ */

/* tools/parity_harness.py:64-77 fixture frame N luma is
 * (x*7 + y*13 + 29*N) & 255, with frame.c:535-552 edge replication.  The
 * instrumented x264 harness used the identical construction, so both sides
 * see byte-identical padded planes.  SHADER: A — deterministic input. */
static uint8_t fixture_pix(int x, int y, int frame)
{
    return (uint8_t)((x*7 + y*13 + 29*frame) & 255);
}

static void fill_fixture(phx_frame_t *f, int frame)
{
    int s = f->i_stride;
    for (int y = -PHX_PADV; y < f->i_height + PHX_PADV; y++)
        for (int x = -PHX_PADH; x < f->i_width + PHX_PADH; x++)
            f->plane[y*s + x] = fixture_pix(x, y, frame);
    for (int y = 0; y < f->i_height; y++) {
        uint8_t l = f->plane[y*s], r = f->plane[y*s + f->i_width - 1];
        for (int k = 0; k < PHX_PADH; k++) {
            f->plane[y*s - PHX_PADH + k] = l;
            f->plane[y*s + f->i_width + k] = r;
        }
    }
    for (int k = 0; k < PHX_PADV; k++) {
        memcpy(f->plane - (k+1)*s - PHX_PADH, f->plane - PHX_PADH,
               (size_t)(f->i_width + 2*PHX_PADH));
        memcpy(f->plane + (f->i_height+k)*s - PHX_PADH,
               f->plane + (f->i_height-1)*s - PHX_PADH,
               (size_t)(f->i_width + 2*PHX_PADH));
    }
}

/* SAD at the zero MV for the given MB (me.c:291: the MVP, no MV bits). */
static int fixture_sad(const phx_frame_t *fenc, const phx_frame_t *fref,
                       int mb_x, int mb_y)
{
    const uint8_t *p = fenc->plane + (mb_y*16)*fenc->i_stride + mb_x*16;
    const uint8_t *q = fref->plane + (mb_y*16)*fref->i_stride + mb_x*16;
    int sum = 0;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) sum += abs((int)p[x] - (int)q[x]);
        p += fenc->i_stride; q += fref->i_stride;
    }
    return sum;
}

/* Goldens from the real instrumented x264 refine path on the fixed fixture:
 * b_refine_qpel == 0 reproduces the me.c:792-797 call inside
 * x264_me_search_ref(); == 1 reproduces analyse.c:3125 x264_me_refine_qpel().
 * medium-frame1-mb5 / qpel02-frame1-mb5 exercise Z1's P_SKIP case; the
 * veryfast-frame3-mb0 case is the reported (0,0) -> (0,2) qpel divergence. */
typedef struct {
    const char *name;
    int rf, ef, mb_x, mb_y, subme, br;
    int mv[2], cost, cost_mv;
} api_golden_t;

static const api_golden_t api_goldens[] = {
    {"medium-frame1-mb5",    0,1, 1,1, 7,0, { 0, 6}, 12763, 48},
    {"veryfast-frame3-mb0",  2,3, 0,0, 2,0, { 0, 2}, 17202, 33},
    {"qpel02-frame1-mb5",    0,1, 1,1, 2,0, { 0, 2}, 21721, 33},
    {"superfast-sad-subme1", 0,1, 1,1, 1,1, { 0, 3},  9853, 38},
    {"refineqpel-subme2",    2,3, 0,0, 2,1, { 0, 0}, 14750,  8},
};

static void case_api(const phx_cost_tables_t *cost, int lambda,
                     const api_golden_t *g)
{
    phx_frame_t *fref = phx_frame_new(64, 64, 0);
    phx_frame_t *fenc = phx_frame_new(64, 64, 0);
    CHECK(fref && fenc, "frame_new public API");
    if (!fref || !fenc) return;
    fill_fixture(fref, g->rf);
    fill_fixture(fenc, g->ef);

    int16_t mvp[2] = { 0, 0 };
    int16_t mv[2] = { 0, 0 };
    int c = fixture_sad(fenc, fref, g->mb_x, g->mb_y), cmv = 0;
    /* Primary path: no caller-supplied planes (self-contained). */
    int rc = phx_me_refine_subpel_mv(fenc, fref, g->mb_x, g->mb_y, cost,
                                     lambda, 16, g->subme, g->br, mvp,
                                     mv, &c, &cmv, NULL);
    CHECK(rc == 0, "refine_subpel_mv succeeds");
    printf("  %-22s subme=%d br=%d mv=(%d,%d) cost=%d cost_mv=%d\n",
           g->name, g->subme, g->br, mv[0], mv[1], c, cmv);
    CHECK(mv[0] == g->mv[0] && mv[1] == g->mv[1],
          "x264 public-API MV golden");
    CHECK(c == g->cost, "x264 public-API cost golden");
    CHECK(cmv == g->cost_mv, "x264 public-API cost_mv golden");

    /* Per-frame reuse path: same output with caller-supplied hpel planes. */
    phx_me_refplanes_t rp;
    CHECK(phx_me_refplanes_init(fref, &rp) == 0, "refplanes_init public API");
    int16_t mv2[2] = { 0, 0 };
    int c2 = fixture_sad(fenc, fref, g->mb_x, g->mb_y), cmv2 = 0;
    CHECK(phx_me_refine_subpel_mv(fenc, fref, g->mb_x, g->mb_y, cost,
                                  lambda, 16, g->subme, g->br, mvp,
                                  mv2, &c2, &cmv2, &rp) == 0,
          "refine_subpel_mv prepared succeeds");
    CHECK(mv2[0] == mv[0] && mv2[1] == mv[1] && c2 == c && cmv2 == cmv,
          "prepared planes == self-contained planes");
    phx_me_refplanes_free(&rp);
    phx_frame_delete(fref);
    phx_frame_delete(fenc);
}

int main(void)
{
    const int mv_range = 16;
    const int qp = 28;                 /* phx_lambda_tab[28] == 6 (tables.c:102) */
    const int lambda = phx_lambda_tab[qp];

    phx_cost_tables_t cost;
    CHECK(phx_analyse_init_costs(&cost, mv_range, PHX_ME_UMH, qp, qp) == 0,
          "init_costs");

    printf("x264-instrumented fullpel + subpel ME goldens:\n");
    for (unsigned i = 0; i < sizeof(goldens)/sizeof(goldens[0]); i++)
        case_golden(&cost, lambda, &goldens[i]);
    check_interpolation(&cost);

    printf("x264-instrumented public refine API goldens (real-clip P_SKIP cases):\n");
    for (unsigned i = 0; i < sizeof(api_goldens)/sizeof(api_goldens[0]); i++)
        case_api(&cost, lambda, &api_goldens[i]);

    /* Invalid inputs are rejected, never guessed.  UMH is now supported. */
    {
        int mv[2] = { 0, 0 };
        phx_frame_t *f = phx_frame_new(64, 64, 0);
        CHECK(f != NULL, "frame_new invalid-case");
        CHECK(phx_me16(0, 0, NULL, f, &cost, 16, lambda, PHX_ME_DIA,
                       NULL, mv) == -1, "null fenc rejected");
        CHECK(phx_me16(0, 0, f, f, &cost, 16, lambda, PHX_ME_ESA,
                       NULL, mv) == -1, "ESA rejected");
        CHECK(phx_me16(0, 0, f, f, &cost, 16, lambda, PHX_ME_TESA,
                       NULL, mv) == -1, "TESA rejected");
        phx_me_refplanes_t rp;
        CHECK(phx_me_refplanes_init(f, &rp) == 0, "refplanes invalid-case");
        int c = 0, cmv = 0;
        CHECK(phx_me_refine_subpel(0, 0, 4, 4, f, &rp, &cost, 16, lambda,
                                   1, 1, NULL, mv, &c, &cmv) == 0,
              "refine valid");
        CHECK(phx_me_refine_subpel(0, 0, 4, 4, NULL, &rp, &cost, 16, lambda,
                                   1, 1, NULL, mv, &c, &cmv) == -1,
              "refine null fenc rejected");
        int16_t smv[2] = { 0, 0 };
        CHECK(phx_me_refine_subpel_mv(NULL, f, 0, 0, &cost, lambda, 16, 1,
                                      1, NULL, smv, &c, &cmv, NULL) == -1,
              "public API null fenc rejected");
        CHECK(phx_me_refine_subpel_mv(f, f, 4, 0, &cost, lambda, 16, 1,
                                      1, NULL, smv, &c, &cmv, NULL) == -1,
              "public API out-of-range MB rejected");
        phx_me_refplanes_free(&rp);
        phx_frame_delete(f);
    }

    phx_analyse_free_costs(&cost, mv_range);

    printf("%s: full-pel + sub-pel ME, %d failures\n",
           fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
