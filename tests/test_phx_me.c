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

/* ---- POC4 MB0/MB1 L1 mvc[] golden (real diag.yuv, instrumented x264) ---- */
/* x264: --qp 28 --preset medium (subme=7, method=HEX, i_me_range=16,
 * i_mv_range=64 -> mv limits x=-34..50, y=-18..18), lambda=7,
 * cost_mv[-8..8] = 56 54 51 48 45 40 34 26 5 26 34 40 45 48 51 54 56.
 *
 * MB0 L1 walk (me.c:344-418): CM(0,6)=2736; X3(-2,0/-1,2/1,2)=3106/1352/
 * 1217 -> (1,8); X3(-1,2/1,2/2,0)=800/1308/1381 -> (0,10);
 * X3(-2,0/-1,2/1,2)=452/2298/2802 -> (-2,10); square refine keeps it.
 * Final MV (-1,24) qpel / cost 797 / cost_mv 103 after me.c:792-797.
 *
 * MB1 L1 walk: CM(0,6)=2736; X3(-2,0/-1,2/1,2)=3106/1352/1217 -> (1,8);
 * X3(-1,2/1,2/2,0)=800/1308/1381 -> (0,10);
 * X3(-2,0/-1,2/1,2)=452/2298/2802 -> (-2,10); X3(-1,-2/-2,0/-3,2) no gain;
 * square refine keeps (-2,10)/452.  Final MV (-8,39) / 654 / 143.
 *
 * The fullpel walk is byte-exact because phx_me16_mvc() takes i_mv_range=64
 * separately from i_me_range=16: using 16 for both stops the half-hexagon at
 * (0,10) (y=10 > fpel y_max 9) and never evaluates (-2,10).
 * SHADER: A — fixed inputs, fixed expected outputs. */
/* MB 00 */
static const char mvc_ref_00[] = "a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaafafafafafafafafafafafafafafafafafafafafafafafafafafafafafafafafafb3bbc0c3c3c3c4c4c3c3c3c2c2c1bfbcbabababababbbbbdbec1c3c5c8c9cacbcccdcfd2d4d5d6d7d7d7d7d7d7d7d6d6d6d6d6d2cec9c4c2bebab7b2afaba9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9bfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfc0c2c2c3c3c3c4c4c3c3c3c3c3c2c0bbb9b9b9b9b9bababcbdc0c2c4c7c9cacccdced1d4d5d6d8d7d7d7d7d7d7d7d7d8d8d8d7d3cfc9c4c2bebab7b2afaba9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c5c4c4c4c4c4c4c3c4c4c4c3c3c0bcb9b9b9b9b9bababcbdc0c2c4c7c9cacdced0d2d5d6d7d9d8d8d8d8d8d8d9d9dadadad9d4d0cac6c3c0bcb9b3b1adabababababababababababababababababababababababababababababababababc4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c5c5c4c4c4c4c4c4c4c4c5c4c3c0bab7b7b7b7b7b8b9babcbfc1c3c6c8c9cccdcfd1d4d5d6d7d6d6d5d5d5d5d5d7dadbd9d8d4d1cbc8c5c2bebcb6b3afadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadc4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c5c5c4c4c4c4c4c4c4c5c6c4c2bfbab7b7b7b7b7b8b8babbbec0c2c5c7c8cacbcccfd2d3d4d5d4d4d3d3d3d4d4d5d5d6d5d5d1cfcac7c5c2bebbb6b3afadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadc5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c4c4c5c5c5c5c5c6c7c4bcb9b8b6b6b6b5b5b6b6b9babcbebfc2c4c5c6c8c9cbcdced0d1cfd0cfcfcfcfcfcecececdcdcac8c3c1c0bdbab7b2afaba9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c6c7c8c4bab7b6b6b6b6b5b4b5b6b8b9bcbdbec1c2c3c4c5c7c8cacbcbcccbcbcbcbcbcbcacac9c8c8c8c3c1bfbcbbb8b5b1abaaa6a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c5c5c6c6c6c6c7c7c8c4bab8b6b5b5b4b3b3b4b4b7b8babbbcbcbdbfc2c4c4c5c6c6c6c7c8c7c7c7c7c7c7c6c5c5c3c0bfbcbab9b8b5aeaba6a19f9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9bc6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c5c5c6c6c6c7c8c8c8c3b8b6b3b3b3b2b1b0b1b1b3b4b6b6b6b6b6b9bec0c0c0c0c0c0c1c1c1c2c2c2c1c1c0bebdbbb9b7b6b3b2b0aca7a39f9a96909090909090909090909090909090909090909090909090909090909090909090c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c7c7c6c6c7c7c7c8c9c9c8c3b7b0b1b1b0afaeadaeaeafb0b1b1b1b2b3b3b5b6b6b7b7b7b8b9b9babbbbbbbabab9b7b6b5b4b3b3b1afa5a1a09d9a948f898989898989898989898989898989898989898989898989898989898989898989c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c8c7c7c8c8c8c8c9c9c8c3b7b0b0afaeadacabababacacadadadadadadacacacacacacadadaeaeafafafafafaeaeaeadadaeaeaeae98969895948e88868686868686868686868686868686868686868686868686868686868686868686c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c8c8c8c9c8c8c9c8c8c8c7c2b6b3afaeadacabaaa9a9a8a9a9a9a9a9a8a8a7a6a6a5a5a5a5a5a5a5a5a5a5a5a5a5a6a6a6a6a7a7a7a7918e8e8c8a8886858585858585858585858585858585858585858585858585858585858585858585c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c8c8c8c9c8c8c9c8c8c8c7c2b6b2aeacaba9a8a7a6a5a5a4a4a4a4a4a4a4a4a4a4a4a4a4a3a2a1a09f9f9f9f9f9fa0a0a0a09f9f9c9b8e868483838383838383838383838383838383838383838383838383838383838383838383838383c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c8c8c8c9c8c8c8c8c7c6c9bdaeaaa7a6a5a3a2a1a09f9e9e9d9d9d9d9e9e9e9f9f9f9f9f9e9e9c9c9b9b9b9b9a9a9998979492918d8c87817c7a797979797979797979797979797979797979797979797979797979797979797979797979c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c8c8c9c9c8c8c8c7c7c5c5b8aaa6a4a3a2a09e9e9c9b9a99989898989897979796969696969797979898989797969493908d8a888584827b7775717172727272727272727272727272727272727272727272727272727272727272727272c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c7c8c6c3c2b3aaa3a2a09e9c9b9a9998979594939392918f8b8a8b8c8c8d8d8c8c8b8a8a89898784817e7d7c787573706e7270706d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6dc8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c9c9c8c8c8c8c6bfbbaca3a09e9f9d9a9997969493908f8e8e8d8b8b88888787878686868483828281807f7c79777474716e6c6a6a6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6cc9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c8c8c8c9c6bbb6a79e9c9c9c9a989693908e8d8c8c8c8c8b8b8a88878585838281817f7d7c7b7a7a7874726f6d6c6a68676969696a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6acacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacac9c9c9c9c9c9c9c9c6bab3a49c9a9a999693908e8c8b8b8b8b8b8b8b8988868582807f7c7a79777574737272706e6b6a68676666666868686969696969696969696969696969696969696969696969696969696969696969696969696969cacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacac9c9c9c9c9c9c9c9c5bab2a29b9997958f8c8c8a8a8a8a8a8a89898987868482807e7a78767572706f6e6d6d6b68676665656565656565656565656565656565656565656565656565656565656565656565656565656565656565656565cacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacac9c9c9c8c8c8c4b9b2a29b9794908b898a8a8a8a898989898787878682817f7d7c7b7874716d6a6866666463626160616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161cbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcacacacac8c8c8c8c4bab3a29994918d8a8a898988888888888785858584817e7c7b7a7673716d696561606060605f5f5f5f5f5f5f5f5e5e5e5e5e5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5fcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcac9c8c8c7c8c3b3ac9e96908d8b89878685848484848383838383827e7c78767572706d696462615f5f5e5e5d5d5c5c5c5c5c5c5b5c5c5c5d5d5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5ecbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbc9c9c7c7c5c7beaea79a918d8b8986858381808080808180818181807c797674706d696562605e5d5c5c5b5b5b5a5a5a5a5a59595959595a5b5c5c5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5dcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbc9c9c9c8c6c4c3b8a7a0928b89888683817f7e7e7e7e7e7e7e7f7f7f7e7874716e6864625e5c5b5a5a5a5a59595858575756565555545555565758595a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5acbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcacacac8c7c6c5bfc0b6a49b8e898582807d7b7a7a7a7a7a7a7a7b7b7b7b79726e6b68615d5b595857575757575655535251525252515150505051525253535353535353535353535353535353535353535353535353535353535353535353cacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacac9c8c6c5c3c2c0bbad9e938a86807e7b76757373737373747474757574736a66615f5c595756555453525251515150505051515050504f4f4e4d4e4e4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4fcacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacac8c6c2c1bfbfbab1a4988d86807b77746f6e6e6f6e6e6e6e6f6f6f6e6e6c65615d5b5958555453525151515050504f5050504f4f4e4e4f4e4d4b4a4a49494949494949494949494949494949494949494949494949494949494949494949cacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacac9c6c3c1bebcb8aca89f9289817a75716c67666666676768686868696a6a686c67635c57545251515050504f4f4f4f4f4f4f4f4f4e4d4e4e4c4a47464645454545454545454545454545454545454545454545454545454545454545454545c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c6c4c1bebbb4aaa0a1988e867c746e625c585653545454535557595e636769635c56535151504f50504f4f4f4f4f4f4e4e4e4f4f4f4e504e4a454241403f3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3ec7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c4beb6b1b0aaa1a19f9f8d8378716d5f5a554f4240403d393f464a4f575e6256524f4f4f4e4e4e4f4f4f4f4f4f4f4f4e4e4e4f4f5050504e453f3b393837373737373737373737373737373737373737373737373737373737373737373737c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c2c1bebab4aba0a1998d8a7f76706d5e574e4534332f2e2f3134363c434b4f504e4d4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e50504e483c3a3736353433323232323232323232323232323232323232323232323232323232323232323232c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2beb7b3b2aaa19ea09c8e877b736f6a5d534a413534302e2d2d2d2d32383f474d4d4d4e4e4d4d4d4d4d4d4d4d4d4d4d4d4d4d4e4f4e484035343231302f2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2ebdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdb6aea8a8a39f9fa0a3918478706d65584c453e35342f2d2d2e2f2f2f3339424c4c4d4d4d4c4c4c4d4d4d4d4c4c4c4c4c4c4c4d4d4a433c353230302e2d2d2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2cb4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4afa9a4a49f9d9c9d9d8d81756e6961554b423c36332f2d2d2d2d2e2e2f333b484c4d4d4d4c4c4c4d4c4c4c4b4b4b4b4b4b4c4b4b443c3632302e2e2c2b2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2ab0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0a9a39d9e9b9a99999c8b7f736c655b564d3f3836332f2d2c2c2c2d2d2f2f33444b4c4c4c4c4c4c4c4c4c4b4a4a4a4a4b4b4c4b4740383230302f2e2d2c2b2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2aa9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a4a09c9b9998969598877b726c655b564d3f3836332f2d2c2c2c2c2c2c2e35494c4d4d4d4d4d4d4d4c4c4b494949494a4a4b4a453e3631302f2e2d2c2b2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2aa0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a09e9c9c9a9a999897968679726c665e564d3f3836332f2c2c2c2b2b2b2d36434e4f4f4f4f4f4f4f4d4d4c4a4949494949494948433c3530302f2e2d2c2b2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9a9a9c9b9e9fa0a0a19083756d675d564d3f3836332f2c2b2b2b2b2e36414b54535353535251514e4c4b4a4949494849484848423b3430302e2e2d2c2b2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a999999999999999999999999999999999999999999999999999999999999999999999a9c9ea2a4a7a7a8978e776d665d574d3e3836332e2b2a2a282b3742494f5555555555555453504d4c4a4949494949484746413a34302f2f2e2e2c2b2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a98989898989898989898989898989898989898989898989898989898989898989897989ea2a5aaadafb1a08d7a6c655d574f423a383532302f31313542494d525655565656565654504d4c4a4949494948474745403a33302f2f2f2e2d2c2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b9696969696969696969696969696969696969696969696969696969696969696969497a0a6a9afb4b5b7a98e8071675f5853493f3e3b3937373a3d40484d52565757585858585855514c4a4a48484848484746443f3933302f2f2f2e2d2c2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b9494949494949494949494949494949494949494949494949494949494949494949199a5aaadb1b4b7b7ac9785796b625a574e4543403d3b3b3e40444e5256575858595958585856524c4a4a48484848484645443e3933302f2f2f2e2d2c2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b9292929292929292929292929292929292929292929292929292929292929292928e98a8adafb3b3bbb9ae9d8a8072665d5a534b4a4643414043474a515557585959595959595956524c4a4949484747474645433e3a35323131302f2e2d2c2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f92a0acb1b4b6b7bdb9ad9f8d82776a605b56514b48454343454a4e535758595a5a5a5a5a5a5956524c4a4a49494848474644433f3c38363434333231302f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f9393939393939393939393939393939393939393939393939393939393939393939eacb1b5b8b9babeb9ad9e8e81766c625d5a544b4845434445494e5457595a5b5b5b5b5a5a5a57524c4a4a49494848474644423f3d3937363534333232313131313131313131313131313131313131313131313131313131313131313131319d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9da8afb3b8bababbbeb9ad9e8e82786c65605d574e4b474646474b4f54575a5b5d5d5c5c5c5a5a57534d4b4b49494848484745423e3d393735353434333232313131313131313131313131313131313131313131313131313131313131313131a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2acb1b7babbbbbbbeb9ad9e8e82796e666260594f4d49484a4b4f5155595b5d5e5e5d5d5c5b5a57534e4b4a49494949484745433e3c383635353534343433333333333333333333333333333333333333333333333333333333333333333333a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8b1b6babbbbbbbbbeb9ad9e8e82796f6965635d514f4c4b4d4f5355575a5e5f605f5e5e5d5c5b59534f4a4a4a494949484846433f3c383635353535343434343434343434343434343434343434343434343434343434343434343434343434b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b9b9babbbbbbbbbeb9ae9e8f837a716c69686155534f4e505257595b5d606262615f5f5e5d5c59554f4d4c4b4a4a4948484643403d3a3837373737373737363636363636363636363636363636363636363636363636363636363636363636b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5bbb9babbbbbbbbbdb8ad9e8f837a736f6b6b665b57525254555a5c6062646666646261605e5c5955504d4c4c4b4a4a48484644423f3d3b3a3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3bb9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9bbbbbbbbbbbbbbbdb8ac9d8d837c75716e6d685e59525053565b5e6164666869676562615f5e5b55504d4d4c4b4b4a4a4948464545424040404040404040404040404040404040404040404040404040404040404040404040404040404040babababababababababababababababababababababababababababababababababcbbbbbbbbbbbbbdb8ac9d8c827c777473726e6c69666465676868696a6b6a6a6a686763615f5c58514e4d4d4d4d4c4c4c4b4a4848474646464646464646464646464646464646464646464646464646464646464646464646464646464646bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbcb7ab9d8a817c79777876767878797978767575747473727070706e67635f5d5b575050515151515151504f4e4d4d4d4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4cbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbcb7ab9c89827e7b7b7b7878787878787776767676777776757575706b65615f5d5b595857575757585856535353525251515151515151515151515151515151515151515151515151515151515151515151515151515151bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbbbbbbbbbbbbbbbcb6aa9c8b847f7d7c7d7b7a7b7b7b7b7a79797979797978777575726e6a64615f5e5f5e5d5d5d5d5b5b5b5b5b5b5a5a58585757575757575757575757575757575757575757575757575757575757575757575757575757bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbbbbbbbbbbbbbbbcb6ab9c8b84807e7e7e7d7c7d7d7d7d7c7b7b7b7b7b797a78777574706c676362626262626262626161616161616060605f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5fbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbcb6aa9c89837f7e7f7f7e7e7f7f7f7f7e7d7d7c7b7b7b7a79787675716d6b686564646464646464646464646464636363626262626262626262626262626262626262626262626262626262626262626262626262626262bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcb7ab9c88827e7d7e7e7e7f7f7f7f7f7e7d7d7c7b7b7b7a7978777673706c6b6967666666666666666666666666656565646464646464646464646464646464646464646464646464646464646464646464646464646464bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbdb7ab9d89817e7d7e7e7e7e7f7e7f7e7e7d7d7c7b7a79787777767776726e6c6c6c696868686868686868686868676767666666666666666666666666666666666666666666666666666666666666666666666666666666bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcb7ab9686827e7d7e7e7e7e7e7e7e7e7e7d7d7d7b79767575747271726f6e6d6d6e6d6a69686868686868686868676767666666666666666666666666666666666666666666666666666666666666666666666666666666bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbdb8aa8f84827e7d7e7e7e7e7e7e7e7e7e7e7d7d7b77747272716d6a696a6a6a6a6a6a6a69696868686868686868676767666666666666666666666666666666666666666666666666666666666666666666666666666666bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565";
static const char mvc_enc_00[] = "c6c6c6c6c6c6c6c6c5c5c5c5c4c4c1bcc6c6c6c6c6c6c6c6c6c6c6c5c4c4bfbbc7c7c7c7c7c7c7c7c7c7c7c6c4c4beb8c8c8c8c8c8c8c8c8c7c7c7c6c4c4bdb6c8c8c8c8c8c8c8c8c8c8c7c6c4c4bcb5c8c8c9c9c9c9c9c9c8c8c8c7c4c4b8b1c9c9c9c9c9c9c9c9c9c9c8c7c4c4b4adc9c9c9c9c9c9c9c9c9c9c8c6c4c2afaacacacacac9c9c9c9c9c9cac6c0bca8a5cacacacacacac9c9c9c9cac5bab5a3a1cacacacacacac9c9c9c9cac4b7b19f9ecacbcbcbcacacacac9c9c9c3b6af9e9dcbcbcbcbcacacacac9c9c9c3b6af9e9ccccbcbcbcacac9c9c8c8c8c2b6b09c9bcbcbcbcbcac9c8c8c7c7c8c0b3ad9b98cbcbcbcbcac9c8c8c7c6c7bbaca69995";

/* MB 10 */
static const char mvc_ref_10[] = "a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7acb8bfc2c3c3c4c4c3c3c3c2c1c0bfbdbdbcbbbbbbbcbcbebfc2c4c6c9cacbcccccdcfd2d3d5d6d7d7d7d7d7d7d7d6d6d6d6d6d3d1cbc6c2bfbbb8b2b0acaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaafafafafafafafafafafafafafafafafafafafafafafafafafafafafafafafafafb3bbc0c3c3c3c4c4c3c3c3c2c2c1bfbcbabababababbbbbdbec1c3c5c8c9cacbcccdcfd2d4d5d6d7d7d7d7d7d7d7d6d6d6d6d6d2cec9c4c2bebab7b2afaba9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9bfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfbfc0c2c2c3c3c3c4c4c3c3c3c3c3c2c0bbb9b9b9b9b9bababcbdc0c2c4c7c9cacccdced1d4d5d6d8d7d7d7d7d7d7d7d7d8d8d8d7d3cfc9c4c2bebab7b2afaba9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c5c4c4c4c4c4c4c3c4c4c4c3c3c0bcb9b9b9b9b9bababcbdc0c2c4c7c9cacdced0d2d5d6d7d9d8d8d8d8d8d8d9d9dadadad9d4d0cac6c3c0bcb9b3b1adabababababababababababababababababababababababababababababababababc4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c5c5c4c4c4c4c4c4c4c4c5c4c3c0bab7b7b7b7b7b8b9babcbfc1c3c6c8c9cccdcfd1d4d5d6d7d6d6d5d5d5d5d5d7dadbd9d8d4d1cbc8c5c2bebcb6b3afadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadc4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c5c5c4c4c4c4c4c4c4c5c6c4c2bfbab7b7b7b7b7b8b8babbbec0c2c5c7c8cacbcccfd2d3d4d5d4d4d3d3d3d4d4d5d5d6d5d5d1cfcac7c5c2bebbb6b3afadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadadc5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c4c4c5c5c5c5c5c6c7c4bcb9b8b6b6b6b5b5b6b6b9babcbebfc2c4c5c6c8c9cbcdced0d1cfd0cfcfcfcfcfcecececdcdcac8c3c1c0bdbab7b2afaba9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c6c7c8c4bab7b6b6b6b6b5b4b5b6b8b9bcbdbec1c2c3c4c5c7c8cacbcbcccbcbcbcbcbcbcacac9c8c8c8c3c1bfbcbbb8b5b1abaaa6a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c5c5c6c6c6c6c7c7c8c4bab8b6b5b5b4b3b3b4b4b7b8babbbcbcbdbfc2c4c4c5c6c6c6c7c8c7c7c7c7c7c7c6c5c5c3c0bfbcbab9b8b5aeaba6a19f9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9bc6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c5c5c6c6c6c7c8c8c8c3b8b6b3b3b3b2b1b0b1b1b3b4b6b6b6b6b6b9bec0c0c0c0c0c0c1c1c1c2c2c2c1c1c0bebdbbb9b7b6b3b2b0aca7a39f9a96909090909090909090909090909090909090909090909090909090909090909090c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c7c7c6c6c7c7c7c8c9c9c8c3b7b0b1b1b0afaeadaeaeafb0b1b1b1b2b3b3b5b6b6b7b7b7b8b9b9babbbbbbbabab9b7b6b5b4b3b3b1afa5a1a09d9a948f898989898989898989898989898989898989898989898989898989898989898989c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c8c7c7c8c8c8c8c9c9c8c3b7b0b0afaeadacabababacacadadadadadadacacacacacacadadaeaeafafafafafaeaeaeadadaeaeaeae98969895948e88868686868686868686868686868686868686868686868686868686868686868686c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c8c8c8c9c8c8c9c8c8c8c7c2b6b3afaeadacabaaa9a9a8a9a9a9a9a9a8a8a7a6a6a5a5a5a5a5a5a5a5a5a5a5a5a5a6a6a6a6a7a7a7a7918e8e8c8a8886858585858585858585858585858585858585858585858585858585858585858585c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c8c8c8c9c8c8c9c8c8c8c7c2b6b2aeacaba9a8a7a6a5a5a4a4a4a4a4a4a4a4a4a4a4a4a4a3a2a1a09f9f9f9f9f9fa0a0a0a09f9f9c9b8e868483838383838383838383838383838383838383838383838383838383838383838383838383c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c8c8c8c9c8c8c8c8c7c6c9bdaeaaa7a6a5a3a2a1a09f9e9e9d9d9d9d9e9e9e9f9f9f9f9f9e9e9c9c9b9b9b9b9a9a9998979492918d8c87817c7a797979797979797979797979797979797979797979797979797979797979797979797979c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c8c8c9c9c8c8c8c7c7c5c5b8aaa6a4a3a2a09e9e9c9b9a99989898989897979796969696969797979898989797969493908d8a888584827b7775717172727272727272727272727272727272727272727272727272727272727272727272c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c7c8c6c3c2b3aaa3a2a09e9c9b9a9998979594939392918f8b8a8b8c8c8d8d8c8c8b8a8a89898784817e7d7c787573706e7270706d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6dc8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c9c9c8c8c8c8c6bfbbaca3a09e9f9d9a9997969493908f8e8e8d8b8b88888787878686868483828281807f7c79777474716e6c6a6a6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6cc9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c9c8c8c8c9c6bbb6a79e9c9c9c9a989693908e8d8c8c8c8c8b8b8a88878585838281817f7d7c7b7a7a7874726f6d6c6a68676969696a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6acacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacac9c9c9c9c9c9c9c9c6bab3a49c9a9a999693908e8c8b8b8b8b8b8b8b8988868582807f7c7a79777574737272706e6b6a68676666666868686969696969696969696969696969696969696969696969696969696969696969696969696969cacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacac9c9c9c9c9c9c9c9c5bab2a29b9997958f8c8c8a8a8a8a8a8a89898987868482807e7a78767572706f6e6d6d6b68676665656565656565656565656565656565656565656565656565656565656565656565656565656565656565656565cacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacac9c9c9c8c8c8c4b9b2a29b9794908b898a8a8a8a898989898787878682817f7d7c7b7874716d6a6866666463626160616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161cbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcacacacac8c8c8c8c4bab3a29994918d8a8a898988888888888785858584817e7c7b7a7673716d696561606060605f5f5f5f5f5f5f5f5e5e5e5e5e5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5fcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcac9c8c8c7c8c3b3ac9e96908d8b89878685848484848383838383827e7c78767572706d696462615f5f5e5e5d5d5c5c5c5c5c5c5b5c5c5c5d5d5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5ecbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbc9c9c7c7c5c7beaea79a918d8b8986858381808080808180818181807c797674706d696562605e5d5c5c5b5b5b5a5a5a5a5a59595959595a5b5c5c5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5dcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbc9c9c9c8c6c4c3b8a7a0928b89888683817f7e7e7e7e7e7e7e7f7f7f7e7874716e6864625e5c5b5a5a5a5a59595858575756565555545555565758595a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5acbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcacacac8c7c6c5bfc0b6a49b8e898582807d7b7a7a7a7a7a7a7a7b7b7b7b79726e6b68615d5b595857575757575655535251525252515150505051525253535353535353535353535353535353535353535353535353535353535353535353cacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacac9c8c6c5c3c2c0bbad9e938a86807e7b76757373737373747474757574736a66615f5c595756555453525251515150505051515050504f4f4e4d4e4e4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4fcacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacac8c6c2c1bfbfbab1a4988d86807b77746f6e6e6f6e6e6e6e6f6f6f6e6e6c65615d5b5958555453525151515050504f5050504f4f4e4e4f4e4d4b4a4a49494949494949494949494949494949494949494949494949494949494949494949cacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacacac9c6c3c1bebcb8aca89f9289817a75716c67666666676768686868696a6a686c67635c57545251515050504f4f4f4f4f4f4f4f4f4e4d4e4e4c4a47464645454545454545454545454545454545454545454545454545454545454545454545c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c6c4c1bebbb4aaa0a1988e867c746e625c585653545454535557595e636769635c56535151504f50504f4f4f4f4f4f4e4e4e4f4f4f4e504e4a454241403f3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3ec7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c4beb6b1b0aaa1a19f9f8d8378716d5f5a554f4240403d393f464a4f575e6256524f4f4f4e4e4e4f4f4f4f4f4f4f4f4e4e4e4f4f5050504e453f3b393837373737373737373737373737373737373737373737373737373737373737373737c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c2c1bebab4aba0a1998d8a7f76706d5e574e4534332f2e2f3134363c434b4f504e4d4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e50504e483c3a3736353433323232323232323232323232323232323232323232323232323232323232323232c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2beb7b3b2aaa19ea09c8e877b736f6a5d534a413534302e2d2d2d2d32383f474d4d4d4e4e4d4d4d4d4d4d4d4d4d4d4d4d4d4d4e4f4e484035343231302f2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2e2ebdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdb6aea8a8a39f9fa0a3918478706d65584c453e35342f2d2d2e2f2f2f3339424c4c4d4d4d4c4c4c4d4d4d4d4c4c4c4c4c4c4c4d4d4a433c353230302e2d2d2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2cb4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4afa9a4a49f9d9c9d9d8d81756e6961554b423c36332f2d2d2d2d2e2e2f333b484c4d4d4d4c4c4c4d4c4c4c4b4b4b4b4b4b4c4b4b443c3632302e2e2c2b2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2ab0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0a9a39d9e9b9a99999c8b7f736c655b564d3f3836332f2d2c2c2c2d2d2f2f33444b4c4c4c4c4c4c4c4c4c4b4a4a4a4a4b4b4c4b4740383230302f2e2d2c2b2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2aa9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a4a09c9b9998969598877b726c655b564d3f3836332f2d2c2c2c2c2c2c2e35494c4d4d4d4d4d4d4d4c4c4b494949494a4a4b4a453e3631302f2e2d2c2b2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2aa0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a09e9c9c9a9a999897968679726c665e564d3f3836332f2c2c2c2b2b2b2d36434e4f4f4f4f4f4f4f4d4d4c4a4949494949494948433c3530302f2e2d2c2b2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9a9a9c9b9e9fa0a0a19083756d675d564d3f3836332f2c2b2b2b2b2e36414b54535353535251514e4c4b4a4949494849484848423b3430302e2e2d2c2b2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a999999999999999999999999999999999999999999999999999999999999999999999a9c9ea2a4a7a7a8978e776d665d574d3e3836332e2b2a2a282b3742494f5555555555555453504d4c4a4949494949484746413a34302f2f2e2e2c2b2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a98989898989898989898989898989898989898989898989898989898989898989897989ea2a5aaadafb1a08d7a6c655d574f423a383532302f31313542494d525655565656565654504d4c4a4949494948474745403a33302f2f2f2e2d2c2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b9696969696969696969696969696969696969696969696969696969696969696969497a0a6a9afb4b5b7a98e8071675f5853493f3e3b3937373a3d40484d52565757585858585855514c4a4a48484848484746443f3933302f2f2f2e2d2c2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b9494949494949494949494949494949494949494949494949494949494949494949199a5aaadb1b4b7b7ac9785796b625a574e4543403d3b3b3e40444e5256575858595958585856524c4a4a48484848484645443e3933302f2f2f2e2d2c2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b9292929292929292929292929292929292929292929292929292929292929292928e98a8adafb3b3bbb9ae9d8a8072665d5a534b4a4643414043474a515557585959595959595956524c4a4949484747474645433e3a35323131302f2e2d2c2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f8f92a0acb1b4b6b7bdb9ad9f8d82776a605b56514b48454343454a4e535758595a5a5a5a5a5a5956524c4a4a49494848474644433f3c38363434333231302f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f2f9393939393939393939393939393939393939393939393939393939393939393939eacb1b5b8b9babeb9ad9e8e81766c625d5a544b4845434445494e5457595a5b5b5b5b5a5a5a57524c4a4a49494848474644423f3d3937363534333232313131313131313131313131313131313131313131313131313131313131313131319d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9d9da8afb3b8bababbbeb9ad9e8e82786c65605d574e4b474646474b4f54575a5b5d5d5c5c5c5a5a57534d4b4b49494848484745423e3d393735353434333232313131313131313131313131313131313131313131313131313131313131313131a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2acb1b7babbbbbbbeb9ad9e8e82796e666260594f4d49484a4b4f5155595b5d5e5e5d5d5c5b5a57534e4b4a49494949484745433e3c383635353534343433333333333333333333333333333333333333333333333333333333333333333333a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8b1b6babbbbbbbbbeb9ad9e8e82796f6965635d514f4c4b4d4f5355575a5e5f605f5e5e5d5c5b59534f4a4a4a494949484846433f3c383635353535343434343434343434343434343434343434343434343434343434343434343434343434b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b9b9babbbbbbbbbeb9ae9e8f837a716c69686155534f4e505257595b5d606262615f5f5e5d5c59554f4d4c4b4a4a4948484643403d3a3837373737373737363636363636363636363636363636363636363636363636363636363636363636b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5bbb9babbbbbbbbbdb8ad9e8f837a736f6b6b665b57525254555a5c6062646666646261605e5c5955504d4c4c4b4a4a48484644423f3d3b3a3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3b3bb9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9b9bbbbbbbbbbbbbbbdb8ac9d8d837c75716e6d685e59525053565b5e6164666869676562615f5e5b55504d4d4c4b4b4a4a4948464545424040404040404040404040404040404040404040404040404040404040404040404040404040404040babababababababababababababababababababababababababababababababababcbbbbbbbbbbbbbdb8ac9d8c827c777473726e6c69666465676868696a6b6a6a6a686763615f5c58514e4d4d4d4d4c4c4c4b4a4848474646464646464646464646464646464646464646464646464646464646464646464646464646464646bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbcb7ab9d8a817c79777876767878797978767575747473727070706e67635f5d5b575050515151515151504f4e4d4d4d4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4cbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbcb7ab9c89827e7b7b7b7878787878787776767676777776757575706b65615f5d5b595857575757585856535353525251515151515151515151515151515151515151515151515151515151515151515151515151515151bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbbbbbbbbbbbbbbbcb6aa9c8b847f7d7c7d7b7a7b7b7b7b7a79797979797978777575726e6a64615f5e5f5e5d5d5d5d5b5b5b5b5b5b5a5a58585757575757575757575757575757575757575757575757575757575757575757575757575757bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbbbbbbbbbbbbbbbcb6ab9c8b84807e7e7e7d7c7d7d7d7d7c7b7b7b7b7b797a78777574706c676362626262626262626161616161616060605f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5f5fbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbcb6aa9c89837f7e7f7f7e7e7f7f7f7f7e7d7d7c7b7b7b7a79787675716d6b686564646464646464646464646464636363626262626262626262626262626262626262626262626262626262626262626262626262626262bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcb7ab9c88827e7d7e7e7e7f7f7f7f7f7e7d7d7c7b7b7b7a7978777673706c6b6967666666666666666666666666656565646464646464646464646464646464646464646464646464646464646464646464646464646464bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbdb7ab9d89817e7d7e7e7e7e7f7e7f7e7e7d7d7c7b7a79787777767776726e6c6c6c696868686868686868686868676767666666666666666666666666666666666666666666666666666666666666666666666666666666bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcb7ab9686827e7d7e7e7e7e7e7e7e7e7e7d7d7d7b79767575747271726f6e6d6d6e6d6a69686868686868686868676767666666666666666666666666666666666666666666666666666666666666666666666666666666bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbdb8aa8f84827e7d7e7e7e7e7e7e7e7e7e7e7d7d7b77747272716d6a696a6a6a6a6a6a6a69696868686868686868676767666666666666666666666666666666666666666666666666666666666666666666666666666666bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbcbdbcb8ad9887837e7e7e7e7e7e7e7e7e7e7e7e7d7d7b76716f6e6d6867676767676767676767676767676767676767666666656565656565656565656565656565656565656565656565656565656565656565656565656565";
static const char mvc_enc_10[] = "b8b5b2b1b0afaeaeaeaeafb0b2b2b2b2b7b3b1b0aeadacacadacadacadadaeaeb5b2aeadabaaaaa9a9a9a9a8a9a9a9a9b3afabaaa8a8a6a6a5a5a5a5a4a4a5a5b0aba8a7a5a4a2a2a1a1a1a1a09fa1a1ada9a6a3a19f9f9e9d9d9c9c9c9b9a9aaaa6a3a29e9c9b9a9998989897969494a7a4a19f9c9a98969594939392918f8da3a19f9d9997949491908f8f8f8e8c8b9f9e9c99969392908e8d8d8c8b8b8a899c9a9896918f8d8b8c8b8b8a8a8989889b9895918e8b8a8a8a898989888887879997938e8b888788878787878686858598938f8b888786858585858584848383948e8a89868483828282828281818180908b88868381807f7f7f7f7f7e7e7d7d";


typedef struct {
    const char *name;
    const char *ref_hex, *enc_hex;
    int mb_x, mb_y, mvp[2], mvc[6][2], i_mvc;
    int want_mv[2], want_cost, want_cost_mv;
} mvc_golden_t;

static const mvc_golden_t mvc_goldens[] = {
    {"POC4-MB0-L1", mvc_ref_00, mvc_enc_00, 0,0, {0,0},
     {{0,0},{0,0},{0,0},{0,0},{0,0}}, 6, {-1,24}, 797, 103},
    {"POC4-MB1-L1", mvc_ref_10, mvc_enc_10, 1,0, {0,0},
     {{0,0},{-1,24},{0,0},{0,0},{0,0}}, 5, {-8,39}, 654, 143},
};

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
}

static void load_hex_plane(const char *hex, phx_frame_t *f)
{
    for (int y = -PHX_PADV; y < f->i_height + PHX_PADV; y++)
        for (int x = -PHX_PADH; x < f->i_width + PHX_PADH; x++) {
            int i = 2*((y+PHX_PADV)*128 + (x+PHX_PADH));
            f->plane[y*f->i_stride + x] =
                (uint8_t)((hexval(hex[i])<<4) | hexval(hex[i+1]));
        }
}

static void load_hex_block(const char *hex, phx_frame_t *f, int mx, int my)
{
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int i = 2*(y*16+x);
            f->plane[(my*16+y)*f->i_stride + mx*16 + x] =
                (uint8_t)((hexval(hex[i])<<4) | hexval(hex[i+1]));
        }
}

static void case_mvc_golden(const phx_cost_tables_t *cost, int lambda,
                            const mvc_golden_t *g)
{
    phx_frame_t *ref = phx_frame_new(64,64,0), *enc = phx_frame_new(64,64,0);
    CHECK(ref && enc, "frame_new mvc golden");
    if (!ref || !enc) return;
    load_hex_plane(g->ref_hex, ref);
    load_hex_block(g->enc_hex, enc, g->mb_x, g->mb_y);
    int16_t mvp[2] = {(int16_t)g->mvp[0],(int16_t)g->mvp[1]};
    int16_t mvc[6][2];
    for (int i = 0; i < g->i_mvc; i++) {
        mvc[i][0] = (int16_t)g->mvc[i][0];
        mvc[i][1] = (int16_t)g->mvc[i][1];
    }
    int mv[2], out_cost = 0, out_cost_mv = 0;
    int rc = phx_me16_mvc(g->mb_x*16, g->mb_y*16, enc, ref, cost,
                          16, 64, lambda, PHX_ME_HEX, 7, mvp,
                          (const int16_t(*)[2])mvc, g->i_mvc, NULL,
                          mv, &out_cost, &out_cost_mv);
    printf("  %s rc=%d mv=(%d,%d) cost=%d cost_mv=%d\n",
           g->name, rc, mv[0], mv[1], out_cost, out_cost_mv);
    CHECK(rc == out_cost, "mvc golden return == cost");
    CHECK(mv[0] == g->want_mv[0] && mv[1] == g->want_mv[1],
          "x264 mvc MV golden");
    CHECK(out_cost == g->want_cost, "x264 mvc cost golden");
    CHECK(out_cost_mv == g->want_cost_mv, "x264 mvc cost_mv golden");
    phx_frame_delete(ref);
    phx_frame_delete(enc);
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

    /* x264's i_mv_range is 64 on this clip, so the MV-cost table is built
     * at that width (analyse.c:148-156); i_me_range stays 16. */
    phx_cost_tables_t cost64;
    const int qp64 = 29, lambda64 = phx_lambda_tab[qp64];
    CHECK(phx_analyse_init_costs(&cost64, 64, PHX_ME_HEX, qp64, qp64) == 0,
          "init_costs mv_range=64");
    printf("x264-instrumented mvc[] (diag.yuv POC4 L1) goldens:\n");
    for (unsigned i = 0; i < sizeof(mvc_goldens)/sizeof(mvc_goldens[0]); i++)
        case_mvc_golden(&cost64, lambda64, &mvc_goldens[i]);
    phx_analyse_free_costs(&cost64, 64);

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
