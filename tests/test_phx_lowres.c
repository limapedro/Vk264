/* Standalone test for the lowres slice-type cost engine and its lookahead
 * integration.  Included by tests/test_phx_lookahead.c (CMakeLists.txt is
 * owned by another worker) and also buildable on its own with
 * -DPHX_LOWRES_STANDALONE.
 *
 * The frame content is the deterministic "synth" fixture of
 * tools/parity_harness.py:64-77 -- 64x64, 8 frames,
 *     luma(i,j,f) = (i*7 + j*13 + f*29) & 255.
 * No bytes are read from either .ref.h264 or .phx.h264.  The x264 decoded
 * slice types used as an oracle below come only from the output of
 * tools/first_frame_probe.py --preset medium --entropy cabac --all-slices:
 *     coded order: I P P P B B B P   (one of those B is BREF)
 * We assert the weaker property required of this worker: B-adapt FAST must
 * emit at least one B/BREF where x264 does, rather than an all-P sequence.
 *
 * SHADER: C -- host-side serial test of the SHADER A lowres kernels.
 */
#include "phx_lowres.h"
#include "phx_lookahead.h"
#include "phx_param.h"
#include "phx_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LR_MBW 4
#define LR_MBH 4
#define LR_FRAMES 8

static int lr_fails;
#define LR_CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL: %s (line %d)\n", (msg), __LINE__); ++lr_fails; } } while (0)

/* parity_harness.py:64-77, luma only (chroma is unused by lowres costs). */
static void lr_fill_synth(phx_frame_t *f, int frame_num)
{
    for (int y = 0; y < f->i_height; y++)
        for (int x = 0; x < f->i_width; x++)
            f->plane[x + y*f->i_stride] =
                (uint8_t)((x*7 + y*13 + frame_num*29) & 255);
}

static phx_frame_t **lr_make_frames(const phx_param_t *p, int n)
{
    phx_frame_t **frames = calloc((size_t)n, sizeof(*frames));
    if (!frames) return NULL;
    for (int i = 0; i < n; i++) {
        frames[i] = phx_frame_new(LR_MBW*16, LR_MBH*16, p->i_bframe);
        if (!frames[i]) { free(frames); return NULL; }
        lr_fill_synth(frames[i], i);
    }
    return frames;
}

static void lr_free_frames(phx_frame_t **frames, int n)
{
    if (!frames) return;
    for (int i = 0; i < n; i++) phx_frame_delete(frames[i]);
    free(frames);
}

/* slicetype.c:836-995 via the stable API.  Determinism is checked by calling
 * the same cost twice: the second hit must come from i_cost_est[][]. */
static void lr_test_costs(void)
{
    phx_param_t p;
    phx_param_default(&p);
    phx_param_apply_preset(&p, "medium");
    phx_frame_t **frames = lr_make_frames(&p, LR_FRAMES);
    LR_CHECK(frames != NULL, "synth frames allocated");
    if (!frames) return;

    /* Level 1.0 mv_range=64 (x264 common/tables.c:31, encoder.c:1265-1268). */
    phx_lowres_t *c = phx_lowres_new(LR_MBW, LR_MBH, 64,
                                     p.i_bframe, 1, 0);
    LR_CHECK(c != NULL, "lowres context allocated");
    if (!c) { lr_free_frames(frames, LR_FRAMES); return; }
    phx_lowres_set_mbtree(c, p.b_mb_tree);          /* slicetype.c:823 */
    for (int i = 0; i < LR_FRAMES; i++) {
        phx_frame_init_lowres(frames[i]);            /* mc.c:458 */
        phx_lowres_frame_reset(c, frames[i]);        /* mc.c:473-481 */
    }

    /* P cost (p0=0,p1=1,b=1), then the same call again (cache hit), then a
     * B-path cost (p0=0,p1=2,b=1) which gets the 100/120 discount. */
    int cp1 = phx_lowres_frame_cost(c, frames, 0, 1, 1);
    int cp2 = phx_lowres_frame_cost(c, frames, 0, 1, 1);
    int cb1 = phx_lowres_frame_cost(c, frames, 0, 2, 1);
    printf("costs: P=%d P(again)=%d B=%d\n", cp1, cp2, cb1);
    LR_CHECK(cp1 > 0, "P frame cost is nonzero");
    LR_CHECK(cp2 > 0 && cp2 == cp1, "P frame cost is reproducible / cached");
    LR_CHECK(cb1 > 0, "B path frame cost is nonzero");

    /* The standalone signature uses its own lazily created context. */
    phx_frame_init_lowres(frames[0]);
    int single = phx_slicetype_frame_cost(frames, 0, 1, 1, 1);
    printf("standalone phx_slicetype_frame_cost = %d\n", single);
    LR_CHECK(single > 0, "standalone frame-cost API is nonzero");

    phx_lowres_delete(c);
    lr_free_frames(frames, LR_FRAMES);
}

/* x264 medium B-adapt FAST on the synth fixture: display order
 * I,P,P,P,BREF,B,B,P (probe: I P P P B B B P in coded order). */
static void lr_test_lookahead_types(void)
{
    phx_param_t p;
    phx_param_default(&p);
    phx_param_apply_preset(&p, "medium");
    p.i_keyint_max = 30;                 /* parity_harness.py:229 */
    p.i_keyint_min = 30;                 /* parity_harness.py:229 */
    p.i_scenecut_threshold = 0;          /* parity_harness.py:229 */

    phx_frame_t **frames = lr_make_frames(&p, LR_FRAMES);
    LR_CHECK(frames != NULL, "lookahead synth frames allocated");
    if (!frames) return;

    phx_lookahead_t *l = phx_lookahead_new(&p, LR_MBW, LR_MBH);
    LR_CHECK(l != NULL, "lookahead created");
    if (!l) { lr_free_frames(frames, LR_FRAMES); return; }
    for (int i = 0; i < LR_FRAMES; i++)
        phx_lookahead_put_frame(l, frames[i]);

    int types[LR_FRAMES] = {0};
    int total = 0;
    while (total < LR_FRAMES) {
        int got = phx_lookahead_get_frames_ex(l, types + total,
                                              LR_FRAMES - total);
        if (got <= 0) break;
        total += got;
    }
    printf("synth types (coded order):");
    for (int i = 0; i < total; i++) printf(" %d", types[i]);
    printf("\n");
    LR_CHECK(total == LR_FRAMES, "all 8 synth frames decided");
    int nb = 0;
    for (int i = 0; i < total; i++)
        if (PHX_IS_B(types[i])) nb++;
    LR_CHECK(nb >= 1, "B-adapt FAST emits at least one B/BREF on synth");
    LR_CHECK(total > 0 && types[0] == PHX_TYPE_IDR, "first frame is IDR");

    phx_lookahead_delete(l);
    lr_free_frames(frames, LR_FRAMES);
}

int phx_lowres_run_tests(void)
{
    lr_fails = 0;
    lr_test_costs();
    lr_test_lookahead_types();
    if (lr_fails) printf("phx_lowres: %d failure(s)\n", lr_fails);
    else printf("phx_lowres: all tests passed\n");
    return lr_fails;
}

#ifdef PHX_LOWRES_STANDALONE
int main(void)
{
    return phx_lowres_run_tests() ? 1 : 0;
}
#endif
