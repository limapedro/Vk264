/* Deterministic tests for the full-pel ME port in src/phx_me.c.
 *
 * The reference picture is filled with a fixed smooth 8-bit ramp whose period
 * (32 in each axis) exceeds the search radius; the current picture is that
 * same pattern shifted by a KNOWN integer MV.  A smooth ramp keeps the SAD
 * surface unimodal so the greedy DIA/HEX search converges, and the 32-pixel
 * period makes the true MV the unique zero-SAD candidate within +/-16.
 * x264's fpelcmp is SAD (common/pixel.c:55-73).  No stream parity is asserted:
 * inter-MB syntax integration belongs to the orchestrator.
 *
 * SHADER: A — host-side test harness; fixed inputs, fixed expected outputs.
 */
#include "phx_me.h"
#include "phx_param.h"
#include "phx_tables.h"

#include <stdio.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

/* Fixed smooth 8-bit ramp: 2x2 constant cells, period 32 in x and y. */
static uint8_t pat(int x, int y)
{
    return (uint8_t)(((x & 31) / 2) + 16*((y & 31) / 2));
}

/* Fill the whole padded luma plane so every MV candidate read is defined. */
static void fill_frames(phx_frame_t *fref, phx_frame_t *fenc,
                        int tx, int ty)
{
    int s = fref->i_stride;
    for (int y = -PHX_PADV; y < fref->i_height + PHX_PADV; y++)
        for (int x = -PHX_PADH; x < fref->i_width + PHX_PADH; x++)
            fref->plane[y*s + x] = pat(x, y);
    s = fenc->i_stride;
    for (int y = -PHX_PADV; y < fenc->i_height + PHX_PADV; y++)
        for (int x = -PHX_PADH; x < fenc->i_width + PHX_PADH; x++)
            fenc->plane[y*s + x] = pat(x + tx, y + ty);
}

/* One translated-block case.  pred_qpel is the caller-supplied MV predictor
 * in quarter-pel units; expect the full-pel MV (tx,ty). */
static void case_translation(const phx_cost_tables_t *cost, int lambda,
                             int bx, int by, int tx, int ty,
                             int pred_qpel_x, int pred_qpel_y,
                             int method, int expect_cost, const char *name)
{
    int16_t pred[2] = { (int16_t)pred_qpel_x, (int16_t)pred_qpel_y };
    int mv[2] = { 12345, 12345 };
    /* Build the two deterministic frames. */
    phx_frame_t *fref = phx_frame_new(64, 64, 0);
    phx_frame_t *fenc = phx_frame_new(64, 64, 0);
    CHECK(fref && fenc, "frame_new");
    if (!fref || !fenc) return;
    fill_frames(fref, fenc, tx, ty);

    int cost1 = phx_me16(bx, by, fenc, fref, cost, 16, lambda, method, pred, mv);
    int mv1x = mv[0], mv1y = mv[1];
    /* Reproducibility: identical inputs must give identical MV and cost. */
    int mv2[2] = { 0, 0 };
    int cost2 = phx_me16(bx, by, fenc, fref, cost, 16, lambda, method,
                         pred, mv2);
    /* The log2 fallback (analyse.c:186-188/156) must reproduce the table. */
    int mv3[2] = { 0, 0 };
    int cost3 = phx_me16(bx, by, fenc, fref, NULL, 16, lambda, method,
                         pred, mv3);

    printf("  %-22s method=%s mv=(%d,%d) cost=%d (repeat %d/%d,%d no-table %d)\n",
           name, method == PHX_ME_DIA ? "DIA" : "HEX",
           mv1x/4, mv1y/4, cost1, cost2, mv2[0]/4, mv2[1]/4, cost3);

    CHECK(cost1 >= 0 && cost1 < (int)PHX_ME_COST_MAX, "valid cost");
    CHECK(mv1x/4 == tx && mv1y/4 == ty, "MV found (full-pel)");
    CHECK(cost1 == cost2 && mv1x == mv2[0] && mv1y == mv2[1],
          "cost/MV reproducible");
    CHECK(cost1 == cost3 && mv1x == mv3[0] && mv1y == mv3[1],
          "table == log2 fallback");
    if (expect_cost >= 0)
        CHECK(cost1 == expect_cost, "golden cost");

    phx_frame_delete(fref);
    phx_frame_delete(fenc);
}

int main(void)
{
    const int mv_range = 16;
    const int qp = 28;                 /* phx_lambda_tab[28] == 6 (tables.c:102) */
    const int lambda = phx_lambda_tab[qp];

    phx_cost_tables_t cost;
    CHECK(phx_analyse_init_costs(&cost, mv_range, PHX_ME_HEX, qp, qp) == 0,
          "init_costs");

    /* (5,-3): starts at predictor (0,0), must move in both axes.
     * Golden cost = 0 SAD + cost_mv[20] + cost_mv[-12] = 63 + 55 = 118
     * (analyse.c:186-188/156, lambda=6). */
    case_translation(&cost, lambda, 16, 16,  5, -3, 0, 0, PHX_ME_DIA, 118, "DIA move +5,-3");
    case_translation(&cost, lambda, 16, 16,  5, -3, 0, 0, PHX_ME_HEX, 118, "HEX move +5,-3");

    /* (-7,4): negative x, positive y.  Golden = cost_mv[-28]+cost_mv[16]
     * = 69 + 59 = 128. */
    case_translation(&cost, lambda, 32, 32, -7,  4, 0, 0, PHX_ME_DIA, 128, "DIA move -7,+4");
    case_translation(&cost, lambda, 32, 32, -7,  4, 0, 0, PHX_ME_HEX, 128, "HEX move -7,+4");

    /* Zero MV: best stays at the predictor; cost is the pure MV bit cost. */
    case_translation(&cost, lambda, 16, 16,  0,  0, 0, 0, PHX_ME_DIA, 8, "DIA zero MV");
    case_translation(&cost, lambda, 16, 16,  0,  0, 0, 0, PHX_ME_HEX, 8, "HEX zero MV");

    /* Exact predictor: residual SAD is zero, reported cost is the pure MV
     * bit cost (cost_mv[qp][0] == 4, analyse.c:186/156 => 4 + 4 = 8). */
    case_translation(&cost, lambda, 16, 16,  5, -3, 5*4, -3*4,
                     PHX_ME_DIA, 8, "DIA exact pred");
    case_translation(&cost, lambda, 16, 16,  5, -3, 5*4, -3*4,
                     PHX_ME_HEX, 8, "HEX exact pred");

    /* Invalid inputs are rejected, never guessed. */
    {
        int mv[2];
        phx_frame_t *f = phx_frame_new(64, 64, 0);
        CHECK(f != NULL, "frame_new invalid-case");
        CHECK(phx_me16(0, 0, NULL, f, &cost, 16, lambda, PHX_ME_DIA,
                       NULL, mv) == -1, "null fenc rejected");
        CHECK(phx_me16(0, 0, f, f, &cost, 16, lambda, 2 /*UMH*/,
                       NULL, mv) == -1, "unsupported method rejected");
        phx_frame_delete(f);
    }

    phx_analyse_free_costs(&cost, mv_range);

    printf("%s: full-pel ME, %d failures\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
