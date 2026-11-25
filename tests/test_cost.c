/* Golden values taken from the authoritative x264 source tables:
 *   lambda_tab[28]=6      reference/x264/common/tables.c:102
 *   lambda2_tab[28]=9289  reference/x264/common/tables.c:119
 *   trellis_lambda2_tab[0][28]=29828, [1][28]=17442  (tables.c:139,157)
 *   chroma_lambda2_offset_tab[12]=256  (tables.c:172)
 *   i_chroma_qp_table[12+28]=28  (common/macroblock.h:263)
 *   bs_size_te(3,2)=x264_ue_size_tab[3]=3  (common/bitstream.h:201,301)
 */
#include "phx_cost.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

int main(void)
{
    /* phx_bs_size_te — bitstream.h:301 */
    CHECK(phx_bs_size_te(1, 17) == 1, "bs_size_te x==1");
    CHECK(phx_bs_size_te(3, 0) == 1, "bs_size_te val=0");
    CHECK(phx_bs_size_te(3, 2) == 3, "bs_size_te val=2");

    phx_cost_tables_t t;
    phx_param_t p;
    phx_param_default(&p);
    /* MV range 16, HEX preset (no fpel table), QPs 0..51 as x264's default. */
    CHECK(phx_analyse_init_costs(&t, 16, p.i_me_method, 0, 51) == 0, "init_costs");

    /* analyse.c:186-188 + 156: lambda=6, logs[1]=3.718 -> 22 */
    CHECK(t.cost_mv[28][1] == 22, "cost_mv[28][1]");
    CHECK(t.cost_mv[28][-1] == 22, "cost_mv[28][-1]");
    /* logs[0]=0.718: 6*0.718+0.5 = 4 (analyse.c:186,156) */
    CHECK(t.cost_mv[28][0] == 4, "cost_mv[28][0]");
    /* 3*lambda*(i!=8): only index 8 is zero (analyse.c:173) */
    CHECK(t.cost_i4x4_mode[28][0] == 18, "cost_i4x4_mode[28][0]");
    CHECK(t.cost_i4x4_mode[28][1] == 18, "cost_i4x4_mode[28][1]");
    CHECK(t.cost_i4x4_mode[28][8] == 0, "cost_i4x4_mode[28][8]");
    CHECK(t.cost_ref[28][1][7] == 6, "cost_ref[28][1][7]");
    CHECK(t.cost_ref[28][2][2] == 18, "cost_ref[28][2][2]");
    CHECK(t.cost_ref[28][0][5] == 0, "cost_ref[28][0][5]");
    CHECK(t.cost_mv_fpel[28][0] == NULL, "fpel absent at HEX");

    phx_mb_analysis_t a;
    memset(&a, 0, sizeof(a));
    phx_mb_analyse_init_qp(&p, 28, 0, 0, &a);
    CHECK(a.i_qp == 28, "mb i_qp");
    CHECK(a.i_lambda == 6, "mb lambda");
    CHECK(a.i_lambda2 == 9289, "mb lambda2");
    CHECK(a.i_trellis_lambda2[0][0] == 29828, "mb trellis inter lambda2");
    CHECK(a.i_trellis_lambda2[0][1] == 17442, "mb trellis intra lambda2");
    CHECK(a.i_trellis_lambda2[1][0] == 29828, "mb trellis chroma inter lambda2");
    CHECK(a.i_trellis_lambda2[1][1] == 17442, "mb trellis chroma intra lambda2");
    CHECK(a.i_psy_rd_lambda == 6, "mb psy rd lambda");
    CHECK(a.i_chroma_lambda2_offset == 256, "mb chroma lambda2 offset");

    phx_mb_analyse_load_costs(&t, &a, 2, 1);
    CHECK(a.p_cost_mv == t.cost_mv[28], "loaded cost_mv");
    CHECK(a.p_cost_ref[0] == t.cost_ref[28][1], "loaded ref L0 (2 active)");
    CHECK(a.p_cost_ref[1] == t.cost_ref[28][0], "loaded ref L1 (1 active)");

    phx_analyse_free_costs(&t, 16);
    CHECK(t.cost_mv[28] == NULL, "freed cost_mv");

    /* ESA path populates the fpel arrays (analyse.c:161-170). */
    CHECK(phx_analyse_init_costs(&t, 16, 3, 28, 28) == 0, "init ESA costs");
    CHECK(t.cost_mv_fpel[28][0] != NULL, "fpel present at ESA");
    CHECK(t.cost_mv_fpel[28][0][1] == t.cost_mv[28][4], "fpel[1] == mv[4]");
    CHECK(t.cost_mv_fpel[28][3][0] == t.cost_mv[28][3], "fpel[0] == mv[3]");
    phx_analyse_free_costs(&t, 16);

    printf("%s: cost tables, %d failures\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
