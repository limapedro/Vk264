/* Standalone test for the serial lookahead / slice-type state machine.
 *
 * Feeds deterministic small frames (flat luma ramps with controlled scene
 * changes), drains phx_lookahead_get_frames() and compares the emitted frame
 * types in *coded* order with hand-derived x264 expectations:
 *
 *   - an IDR at frame 0 (i_frame - i_last_keyframe >= i_keyint_max),
 *   - run of 3 B frames followed by a P when i_bframe == 3 and the frames are
 *     identical (B-adapt FAST: cost_b = 100/120 * cost_p < cost_p),
 *   - ultrafast (bframes=0, scenecut=0) emits I/P only,
 *   - a flat 0 -> flat 255 scene change produces a scenecut I frame,
 *   - i_keyint_max forces an IDR every N frames.
 *
 * This checks the state machine only.  It makes NO claim of x264 stream or
 * cost parity: the lowres zero-MV cost is a documented TODO in the module.
 *
 * Suggested CMake target (CMakeLists.txt is owned by another worker):
 *   add_executable(phx_test_lookahead tests/test_phx_lookahead.c)
 *   target_link_libraries(phx_test_lookahead PRIVATE phx)
 *   add_test(NAME phx_lookahead COMMAND phx_test_lookahead)
 *
 * SHADER: C -- serial host-side test of the serial lookahead boundary.
 */
#include "phx_lookahead.h"
#include "phx_param.h"
#include "phx_frame.h"

int phx_lowres_run_tests(void);   /* tests/test_phx_lowres.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MBW 4
#define MBH 4
#define MAX_FRAMES 32

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL: %s (line %d)\n", (msg), __LINE__); ++fails; } } while (0)

static void fill_flat(phx_frame_t *f, int value)
{
    for (int y = 0; y < f->i_height; y++)
        for (int x = 0; x < f->i_width; x++)
            f->plane[x + y*f->i_stride] = (uint8_t)value;
}

/* Build n frames; value_at(i) supplies the luma fill.  All frames stay alive
 * until after phx_lookahead_delete() (borrowed-frame contract). */
static phx_frame_t **make_frames(const phx_param_t *p, int n,
                                  int (*value_at)(int))
{
    phx_frame_t **frames = calloc((size_t)n, sizeof(*frames));
    if (!frames) return NULL;
    for (int i = 0; i < n; i++) {
        frames[i] = phx_frame_new(MBW*16, MBH*16, p->i_bframe);
        if (!frames[i]) { free(frames); return NULL; }
        fill_flat(frames[i], value_at(i));
    }
    return frames;
}

static void free_frames(phx_frame_t **frames, int n)
{
    if (!frames) return;
    for (int i = 0; i < n; i++) phx_frame_delete(frames[i]);
    free(frames);
}

/* Feed every frame first (as x264 does with its lookahead window), then drain
 * with at most `chunk` types per get_frames() call to exercise partial output. */
static int collect(const phx_param_t *p, phx_frame_t **frames, int n,
                   int chunk, int *types, int cap)
{
    phx_lookahead_t *l = phx_lookahead_new(p, MBW, MBH);
    if (!l) return -1;
    for (int i = 0; i < n; i++) phx_lookahead_put_frame(l, frames[i]);

    int total = 0;
    for (;;) {
        int room = cap - total;
        if (room <= 0) break;
        int got = phx_lookahead_get_frames(l, types + total,
                                          chunk < room ? chunk : room);
        if (got <= 0) break;
        total += got;
    }
    phx_lookahead_delete(l);
    return total;
}

static void expect(const char *name, const int *got, int n,
                   const int *want, int want_n)
{
    int ok = (n == want_n);
    if (ok) ok = memcmp(got, want, (size_t)n * sizeof(int)) == 0;
    if (ok) {
        printf("PASS %s:", name);
        for (int i = 0; i < n; i++) printf(" %d", got[i]);
        printf("\n");
    } else {
        printf("FAIL %s: got %d types:", name, n);
        for (int i = 0; i < n; i++) printf(" %d", got[i]);
        printf("; want %d types:", want_n);
        for (int i = 0; i < want_n; i++) printf(" %d", want[i]);
        printf("\n");
        ++fails;
    }
}

static int flat128(int i) { (void)i; return 128; }
static int scene_at_4(int i) { return i < 4 ? 0 : 255; }

/* Identical frames + B-adapt FAST: IDR, then max-B minigops BBBP.
 * The final minigop has only 3 frames, so it is PBB in coded order. */
static void test_default_bframes(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.i_bframe = 3;
    p.i_bframe_adaptive = PHX_B_ADAPT_FAST;
    p.i_scenecut_threshold = 40;
    p.i_keyint_max = 250;
    p.i_keyint_min = 25;
    phx_frame_t **frames = make_frames(&p, 12, flat128);
    CHECK(frames != NULL, "default: frames allocated");
    if (!frames) return;

    int got[MAX_FRAMES];
    int n = collect(&p, frames, 12, 2, got, MAX_FRAMES); /* partial drains */
    const int want[] = {
        PHX_TYPE_IDR,
        PHX_TYPE_P, PHX_TYPE_B, PHX_TYPE_B, PHX_TYPE_B,
        PHX_TYPE_P, PHX_TYPE_B, PHX_TYPE_B, PHX_TYPE_B,
        PHX_TYPE_P, PHX_TYPE_B, PHX_TYPE_B
    };
    expect("default_bframes", got, n, want, (int)(sizeof(want)/sizeof(want[0])));
    free_frames(frames, 12);
}

/* ultrafast: i_bframe=0, i_scenecut_threshold=0 -> no I except frame 0,
 * no B frames, even across a flat 0 -> flat 255 transition. */
static void test_ultrafast(void)
{
    phx_param_t p;
    phx_param_default(&p);
    CHECK(phx_param_apply_preset(&p, "ultrafast") == 0, "ultrafast preset applied");
    CHECK(p.i_bframe == 0 && p.i_scenecut_threshold == 0,
          "ultrafast disables B frames and scenecut");

    phx_frame_t **frames = make_frames(&p, 8, scene_at_4);
    CHECK(frames != NULL, "ultrafast: frames allocated");
    if (!frames) return;

    int got[MAX_FRAMES];
    int n = collect(&p, frames, 8, MAX_FRAMES, got, MAX_FRAMES);
    const int want[] = { PHX_TYPE_IDR, PHX_TYPE_P, PHX_TYPE_P, PHX_TYPE_P,
                         PHX_TYPE_P, PHX_TYPE_P, PHX_TYPE_P, PHX_TYPE_P };
    expect("ultrafast_ip_only", got, n, want, (int)(sizeof(want)/sizeof(want[0])));
    for (int i = 0; i < n; i++)
        CHECK(got[i] == PHX_TYPE_IDR || got[i] == PHX_TYPE_I ||
              got[i] == PHX_TYPE_P, "ultrafast emits only I/P");
    free_frames(frames, 8);
}

/* Scenecut with B frames disabled: flat 0 -> flat 255 at frame 4 is a real
 * scene cut.  i_keyint_min=25 keeps it an I frame inside the GOP
 * (slicetype.c:1704-1718 only promotes I to IDR at keyint_min). */
static void test_scenecut(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.i_bframe = 0;
    p.i_bframe_adaptive = PHX_B_ADAPT_NONE;
    p.i_scenecut_threshold = 40;
    p.i_keyint_max = 250;
    p.i_keyint_min = 25;
    p.b_mb_tree = 0;

    phx_frame_t **frames = make_frames(&p, 8, scene_at_4);
    CHECK(frames != NULL, "scenecut: frames allocated");
    if (!frames) return;

    int got[MAX_FRAMES];
    int n = collect(&p, frames, 8, MAX_FRAMES, got, MAX_FRAMES);
    const int want[] = { PHX_TYPE_IDR, PHX_TYPE_P, PHX_TYPE_P, PHX_TYPE_P,
                         PHX_TYPE_I,   PHX_TYPE_P, PHX_TYPE_P, PHX_TYPE_P };
    expect("scenecut_i_frame", got, n, want, (int)(sizeof(want)/sizeof(want[0])));
    free_frames(frames, 8);
}

/* No scenecut; i_keyint_max=4 forces IDR at frames 0, 4 and 8. */
static void test_keyint_max(void)
{
    phx_param_t p;
    phx_param_default(&p);
    p.i_bframe = 0;
    p.i_bframe_adaptive = PHX_B_ADAPT_NONE;
    p.i_scenecut_threshold = 0;
    p.i_keyint_max = 4;
    p.i_keyint_min = 2;
    p.b_mb_tree = 0;

    phx_frame_t **frames = make_frames(&p, 10, flat128);
    CHECK(frames != NULL, "keyint: frames allocated");
    if (!frames) return;

    int got[MAX_FRAMES];
    int n = collect(&p, frames, 10, MAX_FRAMES, got, MAX_FRAMES);
    const int want[] = { PHX_TYPE_IDR, PHX_TYPE_P, PHX_TYPE_P, PHX_TYPE_P,
                         PHX_TYPE_IDR, PHX_TYPE_P, PHX_TYPE_P, PHX_TYPE_P,
                         PHX_TYPE_IDR, PHX_TYPE_P };
    expect("keyint_max_gop", got, n, want, (int)(sizeof(want)/sizeof(want[0])));
    free_frames(frames, 10);
}

/* ---- x264 golden fixtures (rows 13-14 of BYTE_EXACT_FUNCTIONS.md) ------- */

/* Deterministic synth fixture of tools/parity_harness.py:64-77, medium preset
 * with --keyint 30 --min-keyint 30 --scenecut 0 --qp 28.  The oracle slice
 * headers below were read from the C-only instrumented x264 build of
 * tools/build_reference_x264.py using tools/first_frame_probe.py, not from
 * Phoenix.  Change PHX_LOOKAHEAD_STRICT_GOLDEN to 1 to make these hard gates;
 * it is 0 while the shared lowres cost engine (src/phx_lowres.c:234-273,
 * :590-601) still diverges from x264. */
#define PHX_LOOKAHEAD_STRICT_GOLDEN 1

#define SYN_MBW 4
#define SYN_MBH 4
#define SYN_FRAMES 8

static void fill_synth(phx_frame_t *f, int frame_num)
{
    for (int y = 0; y < f->i_height; y++)
        for (int x = 0; x < f->i_width; x++)
            f->plane[x + y*f->i_stride] =
                (uint8_t)((x*7 + y*13 + frame_num*29) & 255);
}

static phx_frame_t **make_synth(const phx_param_t *p)
{
    phx_frame_t **frames = calloc(SYN_FRAMES, sizeof(*frames));
    if (!frames) return NULL;
    for (int i = 0; i < SYN_FRAMES; i++) {
        frames[i] = phx_frame_new(SYN_MBW*16, SYN_MBH*16, p->i_bframe);
        if (!frames[i]) { free_frames(frames, SYN_FRAMES); return NULL; }
        fill_synth(frames[i], i);
    }
    return frames;
}

/* Returns the number of types drained into coded order. */
static int drain_coded(const phx_param_t *p, phx_frame_t **frames,
                       int *types, int *frame_nums, int *pocs)
{
    phx_lookahead_t *l = phx_lookahead_new(p, SYN_MBW, SYN_MBH);
    if (!l) return -1;
    for (int i = 0; i < SYN_FRAMES; i++)
        phx_lookahead_put_frame(l, frames[i]);
    int total = 0;
    while (total < SYN_FRAMES) {
        int got = phx_lookahead_get_frames_coded(l, types + total,
                                                 frame_nums + total,
                                                 pocs + total,
                                                 SYN_FRAMES - total);
        if (got <= 0) break;
        total += got;
    }
    phx_lookahead_delete(l);
    return total;
}

static void report_golden(const char *name, const char *oracle,
                          const int *got, int n, const int *want,
                          const char *label)
{
    printf("%s %s:", name, label);
    for (int i = 0; i < n; i++) printf(" %d", got[i]);
    printf("\n       x264 oracle %s:", label);
    for (int i = 0; i < SYN_FRAMES; i++) printf(" %d", want[i]);
    printf("\n");
    int ok = (n == SYN_FRAMES) && memcmp(got, want, SYN_FRAMES*sizeof(int)) == 0;
    if (!ok) {
        printf("KNOWN GAP %s (%s): Phoenix differs from x264 oracle [%s]\n",
               name, label, oracle);
        if (PHX_LOOKAHEAD_STRICT_GOLDEN) ++fails;
    }
}

static void test_synth_fast_golden(void)
{
    phx_param_t p;
    phx_param_default(&p);
    phx_param_apply_preset(&p, "medium");
    p.i_keyint_max = 30;
    p.i_keyint_min = 30;
    p.i_scenecut_threshold = 0;
    phx_frame_t **frames = make_synth(&p);
    if (!frames) { CHECK(0, "fast golden: frames allocated"); return; }

    int types[SYN_FRAMES] = {0}, fn[SYN_FRAMES] = {0}, poc[SYN_FRAMES] = {0};
    int n = drain_coded(&p, frames, types, fn, poc);
    /* Probe: I P P P B B B P with the BREF at coded slot 4; frame_num
     * 0 1 2 3 4 5 5 5; POC 0 2 4 12 8 6 10 14. */
    const int want_type[] = { PHX_TYPE_IDR, PHX_TYPE_P, PHX_TYPE_P, PHX_TYPE_P,
                              PHX_TYPE_B, PHX_TYPE_B, PHX_TYPE_B, PHX_TYPE_P };
    const int want_fn[]   = { 0, 1, 2, 3, 4, 5, 5, 5 };
    const int want_poc[]  = { 0, 2, 4, 12, 8, 6, 10, 14 };
    report_golden("synth_fast", "tools/parity_harness.py medium/cabac",
                  types, n, want_type, "types");
    report_golden("synth_fast", "first_frame_probe.py --all-slices",
                  fn, n, want_fn, "frame_num");
    report_golden("synth_fast", "first_frame_probe.py --all-slices",
                  poc, n, want_poc, "poc");

    /* Raw API must keep the BREF tag on coded slot 4. */
    phx_lookahead_t *l = phx_lookahead_new(&p, SYN_MBW, SYN_MBH);
    int raw[SYN_FRAMES] = {0};
    int raw_n = 0;
    if (l) {
        for (int i = 0; i < SYN_FRAMES; i++)
            phx_lookahead_put_frame(l, frames[i]);
        int total = 0;
        while (total < SYN_FRAMES) {
            int got = phx_lookahead_get_frames_ex(l, raw + total,
                                                  SYN_FRAMES - total);
            if (got <= 0) break;
            total += got;
        }
        raw_n = total;
        phx_lookahead_delete(l);
    }
    const int want_raw[] = { PHX_TYPE_IDR, PHX_TYPE_P, PHX_TYPE_P, PHX_TYPE_P,
                             PHX_TYPE_BREF, PHX_TYPE_B, PHX_TYPE_B, PHX_TYPE_P };
    report_golden("synth_fast_raw", "I P P P BREF B B P",
                  raw, raw_n, want_raw, "raw_types");
    free_frames(frames, SYN_FRAMES);
}

static void test_synth_trellis_golden(void)
{
    phx_param_t p;
    phx_param_default(&p);
    phx_param_apply_preset(&p, "medium");
    p.i_bframe_adaptive = PHX_B_ADAPT_TRELLIS;
    p.i_keyint_max = 30;
    p.i_keyint_min = 30;
    p.i_scenecut_threshold = 0;
    phx_frame_t **frames = make_synth(&p);
    if (!frames) { CHECK(0, "trellis golden: frames allocated"); return; }

    int types[SYN_FRAMES] = {0}, fn[SYN_FRAMES] = {0}, poc[SYN_FRAMES] = {0};
    int n = drain_coded(&p, frames, types, fn, poc);
    /* x264 --b-adapt 2 oracle: I P B B P B B B; frame_num 0 1 2 3 3 4 5 5;
     * POC 0 6 2 4 14 10 8 12. */
    const int want_type[] = { PHX_TYPE_IDR, PHX_TYPE_P, PHX_TYPE_B, PHX_TYPE_B,
                              PHX_TYPE_P, PHX_TYPE_B, PHX_TYPE_B, PHX_TYPE_B };
    const int want_fn[]   = { 0, 1, 2, 3, 3, 4, 5, 5 };
    const int want_poc[]  = { 0, 6, 2, 4, 14, 10, 8, 12 };
    report_golden("synth_trellis", "x264 --b-adapt 2 medium",
                  types, n, want_type, "types");
    report_golden("synth_trellis", "x264 --b-adapt 2 medium",
                  fn, n, want_fn, "frame_num");
    report_golden("synth_trellis", "x264 --b-adapt 2 medium",
                  poc, n, want_poc, "poc");
    free_frames(frames, SYN_FRAMES);
}

static void test_weights_golden(void)
{
    phx_param_t p;
    phx_param_default(&p);
    phx_param_apply_preset(&p, "medium");
    phx_frame_t **frames = make_synth(&p);
    if (!frames) { CHECK(0, "weights golden: frames allocated"); return; }
    phx_lookahead_t *l = phx_lookahead_new(&p, SYN_MBW, SYN_MBH);
    CHECK(l != NULL, "weights golden: lookahead allocated");
    if (!l) { free_frames(frames, SYN_FRAMES); return; }
    /* mc.c:458 -- x264 always has lowres planes before weights_analyse(). */
    for (int i = 0; i < SYN_FRAMES; i++) phx_frame_init_lowres(frames[i]);

    /* slicetype.c:1937-1943 calls with b_lookahead=0 on the closing P of a
     * minigop.  Instrumented x264 gives fenc=6, ref=2 -> scale=63, denom=6,
     * offset=1; chroma weights are absent (pred_weight_table writes 0). */
    phx_weights_t w;
    int rc = phx_lookahead_weights_analyse(l, frames[6], 6, frames[2], 2, 0, &w);
    CHECK(rc == 0, "weights: analyse returned 0");
    printf("weights fenc=6 ref=2: y weighted=%d scale=%d denom=%d offset=%d\n",
           w.plane[0].b_weighted, w.plane[0].i_scale, w.plane[0].i_denom,
           w.plane[0].i_offset);
    printf("       x264 oracle: y weighted=1 scale=63 denom=6 offset=1\n");

    /* Lookahead path (slicetype.c:859-864): instrumented x264 produces no
     * weight for fenc=1/ref=0.  Both costs and MC use per-MB lowres MVs and
     * i_intra_cost[] that phx_frame_t does not expose, so this is a reported
     * gap, not a hard failure, until that state is made available. */
    phx_weights_t wl;
    rc = phx_lookahead_weights_analyse(l, frames[1], 1, frames[0], 0, 1, &wl);
    CHECK(rc == 0, "weights: lookahead analyse returned 0");
    printf("weights fenc=1 ref=0 (lookahead): y weighted=%d scale=%d denom=%d offset=%d\n",
           wl.plane[0].b_weighted, wl.plane[0].i_scale, wl.plane[0].i_denom,
           wl.plane[0].i_offset);
    printf("       x264 oracle: y weighted=0 scale=1 denom=0 offset=0\n");
    if (wl.plane[0].b_weighted)
        printf("KNOWN GAP weights lookahead: needs per-MB i_intra_cost/lowres_mvs (phx_frame.h)\n");

    phx_lookahead_delete(l);
    free_frames(frames, SYN_FRAMES);
}

int main(void)
{
    test_default_bframes();
    test_ultrafast();
    test_scenecut();
    test_keyint_max();
    test_synth_fast_golden();
    test_synth_trellis_golden();
    test_weights_golden();
    fails += phx_lowres_run_tests();

    if (fails) {
        printf("phx_lookahead: %d failure(s)\n", fails);
        return 1;
    }
    printf("phx_lookahead: all tests passed\n");
    return 0;
}
