/* Byte-exact golden tests for src/phx_mvdirect.c.
 *
 * Goldens were produced by an instrumented MSVC harness that links the REAL
 * reference/x264 objects (build/x264_ref/obj/common_mvpred.obj +
 * encoder_macroblock.obj + the scalar C kernels) and calls
 * x264_mb_predict_mv_direct16x16() and x264_macroblock_probe_bskip() on the
 * fixed fenc/fref0/fref1, neighbour ref/MV cache and POC values below.  No
 * Phoenix code is involved in producing the expected values.  The harness
 * lives only in C:\Users\multi\AppData\Local\Temp\opencode\b1_golden
 * (SHADER: C — observe only).
 *
 * Exact source lines of the functions under test:
 *   common/mvpred.c:183-287    temporal direct (map_col_to_list0 :260,
 *                              dsf scaling :264-273)
 *   common/mvpred.c:289-441    spatial direct (unsigned MIN3 :307,
 *                              col_zero_flag :411-438)
 *   common/mvpred.c:454-516    x264_mb_predict_mv_direct16x16
 *   common/macroblock.c:443-455 map_col_to_list0, :1883-1925 bipred_init
 *   encoder/analyse.c:3334-3359 B_SKIP gate
 *   encoder/macroblock.h:38-41 x264_macroblock_probe_bskip macro
 *   encoder/macroblock.c:966-970 B_SKIP promotion, :988-1127 probe
 *
 * SHADER: A — host-side test harness; fixed inputs, fixed expected outputs.
 */
#include "phx_mvdirect.h"
#include "phx_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define MBX 1
#define MBY 1
#define MBXY (MBY*4 + MBX)

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

static uint8_t pat(int x, int y)
{
    return (uint8_t)(((x & 31) / 2) + 16*((y & 31) / 2));
}

/* Colocated-frame arrays and the dsf/map tables shared by the instrumented
 * x264 harness and this test.  Frame-level strides: i_mb_stride=4,
 * i_b8_stride=8, i_b4_stride=16 (common/macroblock.c:252-254). */
typedef struct {
    uint8_t  type[16], part[16];
    int8_t   ref0[64], ref1[64];
    int16_t  mv0[256][2], mv1[256][2];
    int8_t   map[16+2];        /* x264 map_col_to_list0, +2 offset applied */
    int16_t  dsf[16*2][4];
    phx_mvdirect_col_t col;
} fixture_t;

static void fixture_init(fixture_t *fx)
{
    memset(fx, 0, sizeof(*fx));
    for (int i = 0; i < 16; i++) {
        fx->type[i] = PHX_P_L0;                 /* P_L0 */
        fx->part[i] = PHX_MVDIR_D_16x16;        /* D_16x16 */
        fx->ref0[4*i] = fx->ref1[4*i] = 0;
    }
    for (int i = 0; i < 16+2; i++) fx->map[i] = -2;
    fx->map[0] = 0; fx->map[1] = -1; fx->map[2+0] = 0;
    for (int i = 0; i < 16*2; i++)
        for (int j = 0; j < 4; j++) fx->dsf[i][j] = 128;

    fx->col.mb_type = fx->type;
    fx->col.mb_partition = fx->part;
    fx->col.ref[0] = fx->ref0; fx->col.ref[1] = fx->ref1;
    fx->col.mv[0] = fx->mv0;   fx->col.mv[1] = fx->mv1;
    fx->col.field = NULL;
    fx->col.i_poc = 8;
    fx->col.i_delta_poc[0] = fx->col.i_delta_poc[1] = 0;
    fx->col.i_mb_stride = 4;
    fx->col.i_b8_stride = 8;
    fx->col.i_b4_stride = 16;
}

/* x264's cache positions for the current MB (common/macroblock.c:682-683). */
#define B8XY (2*(8*MBY + MBX))    /* 18 */
#define B4XY (4*(16*MBY + MBX))   /* 68 */

static void cache_init(phx_mb_cache_t *c)
{
    memset(c, 0, sizeof(*c));
    c->i_mb_x = MBX; c->i_mb_y = MBY; c->i_mb_xy = MBXY;
    c->i_b8_xy = B8XY; c->i_b4_xy = B4XY;
    for (int l = 0; l < 2; l++)
        for (int i = 0; i < 40; i++) c->ref[l][i] = -2;
}

static void set_nb(phx_mb_cache_t *c, int l, int pos, int ref, int mx, int my)
{
    c->ref[l][pos] = (int8_t)ref;
    c->mv[l][pos][0] = (int16_t)mx;
    c->mv[l][pos][1] = (int16_t)my;
}

static void input_init(phx_mvdirect_input_t *in, const phx_mb_cache_t *c,
                       fixture_t *fx, int spatial)
{
    memset(in, 0, sizeof(*in));
    in->cache = c;
    in->mb_x = MBX; in->mb_y = MBY; in->mb_xy = MBXY;
    in->i_b8_xy = B8XY; in->i_b4_xy = B4XY;
    in->direct_mode = PHX_MVDIR_SPATIAL;
    in->b_direct_spatial = spatial;
    in->b_interlaced = 0;
    in->i_threads = 1;
    in->mv_max_spel_y = 64*4 - 1;
    in->col = &fx->col;
    in->cur_poc = 12;
    in->dist_scale_factor = (const int16_t (*)[4])fx->dsf;
    in->map_col_to_list0 = &fx->map[2];   /* macro, indexed as map[col] */
    in->prev = NULL;
}

/* Verify one direct-prediction golden.  ref/mv are uniform over all 16 4x4
 * blocks in every case below, so one expected triple per list suffices. */
static void check_direct(const char *name, const phx_mvdirect_result_t *r,
                         int avail, int part,
                         int r0, int x0, int y0, int r1, int x1, int y1)
{
    CHECK(r->available == avail, "available");
    CHECK(r->i_partition == part, "partition");
    for (int l = 0; l < 2; l++) {
        const int er = l ? r1 : r0, ex = l ? x1 : x0, ey = l ? y1 : y0;
        for (int k = 0; k < 16; k++) {
            if (r->ref[l][k] != er || r->mv[l][k][0] != ex ||
                r->mv[l][k][1] != ey) {
                printf("FAIL %s list%d block%d got ref=%d mv=%d,%d "
                       "want ref=%d mv=%d,%d\n", name, l, k,
                       r->ref[l][k], r->mv[l][k][0], r->mv[l][k][1],
                       er, ex, ey);
                ++fails;
                break;
            }
        }
    }
}

static void test_direct(void)
{
    fixture_t fx;
    phx_mb_cache_t c;
    phx_mvdirect_input_t in;
    phx_mvdirect_result_t r;
    const int p = phx_scan8[0];

    /* S0 — spatial, 4 matching neighbours per list, colocated P_L0 MV(4,-2).
     * x264 goldens: L0 (0,0,2), L1 (0,2,0), avail=1, D_16x16. */
    fixture_init(&fx); cache_init(&c);
    for (int l = 0; l < 2; l++) {
        set_nb(&c, l, p-1,   0, l?2:8,   l?2:4);
        set_nb(&c, l, p-8,   0, l?-2:-4, l?-2:2);
        set_nb(&c, l, p-8+4, 0, l?4:0,   l?0:0);
        set_nb(&c, l, p-8-1, 0, l?0:12,  l?4:-8);
    }
    fx.type[MBXY] = PHX_P_L0; fx.part[MBXY] = PHX_MVDIR_D_16x16;
    fx.ref0[B8XY] = 0;
    fx.mv0[B4XY][0] = 4; fx.mv0[B4XY][1] = -2;
    input_init(&in, &c, &fx, 1);
    CHECK(phx_mvdirect_predict(&in, &r) == 0, "S0 return");
    check_direct("S0", &r, 1, PHX_MVDIR_D_16x16, 0,0,2, 0,2,0);
    CHECK(r.direct_ref[0][0] == 0 && r.direct_mv[0][0][0] == 0 &&
          r.direct_mv[0][0][1] == 2, "S0 direct cache L0");
    CHECK(r.direct_ref[1][0] == 0 && r.direct_mv[1][0][0] == 2 &&
          r.direct_mv[1][0][1] == 0, "S0 direct cache L1");

    /* S1 — spatial, all neighbours unavailable, colocated intra.
     * x264 sets ref[0]=ref[1]=-1, then the (M16 & 0x8080) early-out forces
     * both lists to ref 0 / MV 0. */
    fixture_init(&fx); cache_init(&c);
    fx.type[MBXY] = PHX_I_16x16; fx.part[MBXY] = PHX_MVDIR_D_16x16;
    input_init(&in, &c, &fx, 1);
    CHECK(phx_mvdirect_predict(&in, &r) == 0, "S1 return");
    check_direct("S1", &r, 1, PHX_MVDIR_D_16x16, 0,0,0, 0,0,0);

    /* T0 — temporal, colocated P_L0 ref 0 MV(20,-12), dsf=128.
     * x264 goldens: mv_y=(20? no: (-12*2)/2=-12);
     * L0 ((128*20+128)>>8, (128*-12+128)>>8) = (10,-6);
     * L1 (10-20, -6-(-12)) = (-10,6). */
    fixture_init(&fx); cache_init(&c);
    fx.type[MBXY] = PHX_P_L0; fx.part[MBXY] = PHX_MVDIR_D_16x16;
    fx.ref0[B8XY] = 0;
    fx.mv0[B4XY][0] = 20; fx.mv0[B4XY][1] = -12;
    fx.dsf[0][0] = 128;
    input_init(&in, &c, &fx, 0);
    CHECK(phx_mvdirect_predict(&in, &r) == 0, "T0 return");
    check_direct("T0", &r, 1, PHX_MVDIR_D_16x16, 0,10,-6, 0,-10,6);

    /* T1 — temporal, colocated intra: zero MVs, refs forced to 0. */
    fixture_init(&fx); cache_init(&c);
    fx.type[MBXY] = PHX_I_16x16; fx.part[MBXY] = PHX_MVDIR_D_16x16;
    input_init(&in, &c, &fx, 0);
    CHECK(phx_mvdirect_predict(&in, &r) == 0, "T1 return");
    check_direct("T1", &r, 1, PHX_MVDIR_D_16x16, 0,0,0, 0,0,0);

    /* T2 — temporal, colocated ref 1 not present in list 0 (map=-2):
     * x264 returns avail=0; L0 ref stays -2, L1 ref is set to 0. */
    fixture_init(&fx); cache_init(&c);
    fx.type[MBXY] = PHX_P_L0; fx.part[MBXY] = PHX_MVDIR_D_16x16;
    fx.ref0[B8XY] = 1; fx.map[2+1] = -2;
    input_init(&in, &c, &fx, 0);
    CHECK(phx_mvdirect_predict(&in, &r) == 0, "T2 return");
    check_direct("T2", &r, 0, PHX_MVDIR_D_16x16, -2,0,0, 0,0,0);
}

/* The probe gate is golden-tested on a static frame pair, where the x264
 * harness produced P3 static16=16 and P4 perturb=15:
 *   P3: f0 == f1 == fenc, colocated MV 0 => all 16 MBs are B_SKIP.
 *   P4: one luma sample of MB(1,1) raised by +80 => exactly 15 B_SKIP.
 * Avoiding a baked MC-result dump keeps the golden independent of the
 * harness's chroma-plane layout; the nonzero-MV direct MVs are themselves
 * golden-tested byte-exactly in test_direct() above. */
static void fill_static_frames(phx_frame_t *f0, phx_frame_t *f1, phx_frame_t *fenc)
{
    for (int y = -PHX_PADV; y < H+PHX_PADV; y++)
        for (int x = -PHX_PADH; x < W+PHX_PADH; x++) {
            f0->plane[y*f0->i_stride + x] = pat(x, y);
            f1->plane[y*f1->i_stride + x] = pat(x, y);
            fenc->plane[y*fenc->i_stride + x] = pat(x, y);
        }
    /* 4:2:0 NV12 has half the vertical chroma padding (frame.c:152/239). */
    for (int y = -PHX_PADV/2; y < H/2+PHX_PADV/2; y++)
        for (int x = -PHX_PADH/2; x < W/2+PHX_PADH/2; x++)
            for (int p = 0; p < 2; p++) {
                const uint8_t c = (uint8_t)(x + 2*y);
                f0->plane_chroma[y*f0->i_stride_planes[1] + 2*x + p] = c;
                f1->plane_chroma[y*f1->i_stride_planes[1] + 2*x + p] = c;
                fenc->plane_chroma[y*fenc->i_stride_planes[1] + 2*x + p] = c;
            }
}

/* Sweep all 16 MBs with zero colocated MVs (=> direct MVs 0) and count
 * B_SKIP results, exactly as the instrumented x264 harness did. */
static int sweep_bskip(phx_frame_t *fenc, phx_frame_t *f0, phx_frame_t *f1)
{
    fixture_t fx;
    phx_mb_cache_t c;
    phx_mvdirect_input_t in;
    fixture_init(&fx);
    memset(fx.mv0, 0, sizeof(fx.mv0));
    int count = 0;
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++) {
            cache_init(&c);
            c.i_mb_x = bx; c.i_mb_y = by; c.i_mb_xy = by*4+bx;
            c.i_b8_xy = 2*(8*by+bx); c.i_b4_xy = 4*(16*by+bx);
            input_init(&in, &c, &fx, 0);
            phx_mvdirect_result_t dr;
            CHECK(phx_mvdirect_predict(&in, &dr) == 0, "sweep predict");
            check_direct("sweep", &dr, 1, PHX_MVDIR_D_16x16, 0,0,0, 0,0,0);
            count += phx_mvdirect_probe_bskip(fenc, f0, f1, &dr,
                                              bx, by, 26, 29, 32);
        }
    return count;
}

static void test_probe(void)
{
    phx_frame_t *f0 = phx_frame_new(W, H, 0);
    phx_frame_t *f1 = phx_frame_new(W, H, 0);
    phx_frame_t *fenc = phx_frame_new(W, H, 0);
    CHECK(f0 && f1 && fenc, "frame allocation");
    if (!f0 || !f1 || !fenc) return;

    fill_static_frames(f0, f1, fenc);
    int p3 = sweep_bskip(fenc, f0, f1);
    CHECK(p3 == 16, "P3 static 16/16 B_SKIP");

    /* P4: perturb one luma sample of the MB(1,1) reconstruction target. */
    fenc->plane[(MBY*16+3)*fenc->i_stride + MBX*16+3] =
        (uint8_t)(fenc->plane[(MBY*16+3)*fenc->i_stride + MBX*16+3] + 80);
    int p4 = sweep_bskip(fenc, f0, f1);
    CHECK(p4 == 15, "P4 perturb -> 15 B_SKIP");

    phx_frame_delete(f0); phx_frame_delete(f1); phx_frame_delete(fenc);
}

int main(void)
{
    test_direct();
    test_probe();
    if (fails) {
        printf("phx_mvdirect: %d failure(s)\n", fails);
        return 1;
    }
    printf("phx_mvdirect: all golden tests passed\n");
    return 0;
}
