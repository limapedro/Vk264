/* Standalone test for src/phx_deblock.{h,c} — scalar 8-bit 4:2:0 deblocking.
 *
 * Golden samples are literal values produced by an independent transliteration
 * of reference/x264/common/deblock.c:79-275 (the formulas, not the port):
 *   alpha(26)=15, beta(26)=6, tc0(26)[2]=1, tc0(26)[3]=1  (deblock.c:32-76)
 *   inter luma : p2=10 p1=12 p0=14 q0=20 q1=22 q2=24 -> 10,13,16,18,21,24
 *   intra luma : p3..q3 = 8,10,12,14,16,18,20,22 (deblock.c:192-213)
 *   inter chroma: 12,12,14,14,20,20,22,22 -> ...,15,15,19,19,... (deblock.c:146)
 *   intra chroma: 10,10,12,12,20,20,22,22 -> ...,14,14,19,19,... (deblock.c:250)
 * The 2x2-MB frame check uses the full x264 edge order
 * (deblock.c:449,536-541,543,597-602) with bs=2 on every edge.
 *
 * SHADER: B — validates the B-stage in-loop filter and its explicit cache.
 */
#include "phx_deblock.h"
#include "phx_mbcache.h"
#include "phx_tables.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)
#define CHECK_EQ(got, want, msg) do { if ((got) != (want)) { \
    printf("FAIL %s: got %d want %d (line %d)\n", msg, (int)(got), \
           (int)(want), __LINE__); ++fails; } } while (0)

/* deblock.c:32-76 — literal table probes. */
static void test_tables(void)
{
    CHECK_EQ(phx_deblock_alpha(26), 15, "alpha(26)");
    CHECK_EQ(phx_deblock_beta(26), 6, "beta(26)");
    CHECK_EQ(phx_deblock_tc0(26, 2), 1, "tc0(26)[2]");
    CHECK_EQ(phx_deblock_tc0(26, 3), 1, "tc0(26)[3]");
    CHECK_EQ(phx_deblock_tc0(26, 0), -1, "tc0(26)[0]");
    CHECK_EQ(phx_deblock_tc0(20, 3), 1, "tc0(20)[3]");
    CHECK_EQ(phx_deblock_alpha(51), 255, "alpha(51)");
    CHECK_EQ(phx_deblock_beta(51), 18, "beta(51)");
    CHECK_EQ(phx_deblock_tc0(51, 3), 25, "tc0(51)[3]");
}

/* deblock.c:277-304 — hand-built scan8 cache. */
static void test_strength(void)
{
    phx_mb_cache_t c;
    uint8_t bs[2][8][4];
    memset(&c, 0, sizeof(c));

    /* deblock.c:286 walks scan8 *slots* 12..15 (vertical) and
     * 12/20/28/36 (horizontal), not phx_scan8[block] values. */
    c.non_zero_count[12] = 1;
    c.non_zero_count[15] = 1;
    c.non_zero_count[20] = 1;
    phx_deblock_strength_c(&c, bs, 4, 0);
    /* [dir][edge][i]: i walks +8 scan8 slots for dir 0, +1 for dir 1. */
    const uint8_t want0[2][4][4] = {
        {{2,2,0,0},{2,2,0,0},{0,0,0,0},{2,0,0,0}},
        {{2,0,0,2},{2,0,0,2},{2,0,0,0},{0,0,0,0}}
    };
    for (int d = 0; d < 2; d++)
        for (int e = 0; e < 4; e++)
            for (int i = 0; i < 4; i++)
                CHECK_EQ(bs[d][e][i], want0[d][e][i], "bs nnz golden");

    /* Second pass: ref/mv differences only (bframe=1 exercises list 1).
     * ref[1][4] is set equal to ref[1][12] so horizontal edge 0 stays 0. */
    memset(&c, 0, sizeof(c));
    c.ref[1][12] = 1; c.ref[1][4] = 1;             /* vertical edge 0 */
    c.mv[0][13][1] = 4;                             /* vertical edge 1, mvy */
    c.mv[0][14][0] = 4;                             /* vertical edge 2 */
    c.ref[0][15] = 2;                               /* vertical edge 3 */
    c.ref[0][20] = 1;                               /* horizontal edges 1,2 */
    phx_deblock_strength_c(&c, bs, 4, 1);
    const uint8_t want1[2][4][4] = {
        {{1,1,0,0},{1,1,0,0},{1,0,0,0},{1,0,0,0}},
        {{0,1,1,1},{1,1,1,1},{1,0,0,0},{0,0,0,0}}
    };
    for (int d = 0; d < 2; d++)
        for (int e = 0; e < 4; e++)
            for (int i = 0; i < 4; i++)
                CHECK_EQ(bs[d][e][i], want1[d][e][i], "bs ref/mv golden");

    /* macroblock.c:1443-1446 — intra seeds edges 1 and 2 only. */
    phx_mb_state_t *s = phx_mb_state_new(1, 1);
    phx_mb_cache_t ic;
    memset(&ic, 0, sizeof(ic));
    s->type[0] = PHX_I_16x16;
    phx_macroblock_deblock_strength(s, &ic, bs, 1, 0, 0);
    for (int d = 0; d < 2; d++)
        for (int e = 0; e < 8; e++)
            for (int i = 0; i < 4; i++) {
                int want = (e == 1 || e == 2) ? 3 : 0;
                CHECK_EQ(bs[d][e][i], want, "intra bs seed");
            }
    phx_mb_state_delete(s);
}

/* deblock.c:79-135, golden: 10,12,14,20,22,24 -> 10,13,16,18,21,24. */
static void test_luma_inter(void)
{
    uint8_t buf[8*16];
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 16; x++) buf[y*16+x] = (uint8_t)(x*37+y*17+11);
        memcpy(buf+y*16, "\x0a\x0c\x0e\x14\x16\x18", 6);
    }
    int8_t tc[4] = {1, -1, -1, -1};
    phx_deblock_h_luma_c(buf+3, 16, 15, 6, tc);
    const uint8_t want[6] = {10,13,16,18,21,24};
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 6; x++)
            CHECK_EQ(buf[y*16+x], want[x], "luma inter golden");
    CHECK_EQ(buf[4*16+3], 20, "luma inter skipped row");
}

/* deblock.c:183-239, golden bytes for row 3 after the 16-edge sweep. */
static void test_luma_intra(void)
{
    uint8_t buf[24*16];
    const uint8_t pat[8] = {8,10,12,14,16,18,20,22};
    for (int y = 0; y < 24; y++) {
        for (int x = 0; x < 16; x++) buf[y*16+x] = (uint8_t)(x*37+y*17+11);
        memcpy(buf+y*16+2, pat, 7);
    }
    phx_deblock_h_luma_intra_c(buf+3*16+5, 16, 15, 6);
    const uint8_t want_row3[7] = {32,11,12,14,15,17,20}; /* x=2..8 */
    for (int x = 0; x < 7; x++)
        CHECK_EQ(buf[3*16+2+x], want_row3[x], "luma intra row3 golden");
    for (int y = 3; y < 19; y++)
        CHECK_EQ(buf[y*16+5], 14, "luma intra col5 golden");
    CHECK_EQ(buf[19*16+5], 14, "luma intra untouched row");
}

/* deblock.c:137-177, golden per U/V pair. */
static void test_chroma_inter(void)
{
    uint8_t buf[4*32];
    const uint8_t pat[8] = {12,12,14,14,20,20,22,22};
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 32; x++) buf[y*32+x] = (uint8_t)(x*11+y*5+3);
        memcpy(buf+y*32, pat, 8);
    }
    int8_t tc[4] = {1, 0, 0, 0};
    phx_deblock_h_chroma_c(buf+4, 32, 15, 6, tc);
    const uint8_t want[8] = {12,12,15,15,19,19,22,22};
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 8; x++)
            CHECK_EQ(buf[y*32+x], want[x], "chroma inter golden");
}

/* deblock.c:241-271. */
static void test_chroma_intra(void)
{
    uint8_t buf[10*32];
    const uint8_t pat[8] = {10,10,12,12,20,20,22,22};
    for (int y = 0; y < 10; y++) {
        for (int x = 0; x < 32; x++) buf[y*32+x] = (uint8_t)(x*11+y*5+3);
        memcpy(buf+y*32, pat, 8);
    }
    phx_deblock_h_chroma_intra_c(buf+4, 32, 15, 6);
    const uint8_t want[8] = {10,10,14,14,19,19,22,22};
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            CHECK_EQ(buf[y*32+x], want[x], "chroma intra golden");
    CHECK_EQ(buf[8*32+2], 12, "chroma intra untouched row");
}

static uint32_t fnv1a(const uint8_t *b, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* x264_frame_deblock_row's inner loop, deblock.c:378-606. */
static void test_frame(void)
{
    static uint8_t luma[32*32], chroma[32*32];
    phx_frame_t f;
    memset(&f, 0, sizeof(f));
    f.i_width = 32; f.i_height = 32; f.i_stride = 32;
    f.plane = luma; f.planes[0] = luma;
    f.plane_chroma = chroma; f.planes[1] = chroma;
    f.i_stride_planes[1] = 32; f.i_lines_planes[1] = 16;

    phx_mb_state_t *s = phx_mb_state_new(2, 2);
    CHECK(s != NULL, "state alloc");
    if (!s) return;
    for (int xy = 0; xy < 4; xy++) {
        s->type[xy] = PHX_P_L0;
        s->cbp[xy] = 0x1700;
        s->slice_table[xy] = 0;
    }

    for (int my = 0; my < 2; my++)
        for (int mx = 0; mx < 2; mx++)
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 32; x++) {
                    luma[y*32+x] = (uint8_t)(x*13 + y*7 + 5);
                    chroma[y*32+x] = (uint8_t)(x*11 + y*5 + 3);
                }

    phx_mb_cache_t c;
    memset(&c, 0, sizeof(c));
    for (int k = 0; k < 120; k++) c.non_zero_count[k] = 1;

    for (int my = 0; my < 2; my++)
        for (int mx = 0; mx < 2; mx++) {
            int xy = my*2 + mx;
            c.i_mb_xy = xy;
            c.top = xy - 2;
            c.left[0] = c.left[1] = xy - 1;
            c.i_neighbour = 0;
            if (mx) c.i_neighbour |= PHX_MB_LEFT;
            if (my) c.i_neighbour |= PHX_MB_TOP;
            CHECK_EQ(phx_frame_deblock(&f, s, &c, mx, my, 26, 1), 0,
                     "phx_frame_deblock return");
        }

    const struct { int x, y, want; } ls[] = {
        {0,0,5}, {7,3,117}, {15,15,49}, {16,8,13}, {31,31,113}
    };
    const struct { int x, y, want; } cs[] = {
        {0,0,3}, {8,4,110}, {16,8,218}, {31,31,243}
    };
    for (unsigned k = 0; k < sizeof(ls)/sizeof(ls[0]); k++)
        CHECK_EQ(luma[ls[k].y*32+ls[k].x], ls[k].want, "frame luma golden");
    for (unsigned k = 0; k < sizeof(cs)/sizeof(cs[0]); k++)
        CHECK_EQ(chroma[cs[k].y*32+cs[k].x], cs[k].want, "frame chroma golden");
    CHECK(fnv1a(luma, sizeof(luma)) == 0x9b8456c5u, "frame luma FNV");
    CHECK(fnv1a(chroma, sizeof(chroma)) == 0x9891efe5u, "frame chroma FNV");

    /* disable == 1 must be a no-op (encoder.c:2417). */
    phx_deblock_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.disable_deblocking_filter_idc = 1;
    CHECK_EQ(phx_frame_deblock_ex(&f, s, &c, 0, 0, 26, 1, &cfg), 0,
             "disabled filter return");
    CHECK(fnv1a(luma, sizeof(luma)) == 0x9b8456c5u, "disabled filter no-op");

    CHECK_EQ(phx_frame_deblock(&f, s, &c, -1, 0, 26, 1), -1, "bad mb_x");
    CHECK_EQ(phx_frame_deblock(&f, s, &c, 0, 0, 70, 1), -1, "bad qp");
    CHECK_EQ(phx_frame_deblock(NULL, s, &c, 0, 0, 26, 1), -1, "bad frame");
    phx_mb_state_delete(s);
}

int main(void)
{
    test_tables();
    test_strength();
    test_luma_inter();
    test_luma_intra();
    test_chroma_inter();
    test_chroma_intra();
    test_frame();
    if (fails) {
        printf("phx_deblock: %d failure(s)\n", fails);
        return 1;
    }
    printf("phx_deblock: all checks passed\n");
    return 0;
}
