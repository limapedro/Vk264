/* Phoenix analysis cost tables — mirrors reference/x264/encoder/analyse.c.
 *
 * Functions ported here:
 *   init_costs                analyse.c:143
 *   x264_analyse_init_costs   analyse.c:179
 *   x264_analyse_free_costs   analyse.c:204
 *   mb_analyse_load_costs     analyse.c:248
 *   mb_analyse_init_qp        analyse.c:255
 *   bs_size_te                common/bitstream.h:301
 *
 * SHADER: A/B — table construction is host-side (A); the resulting arrays are
 * read by every per-MB mode decision (B). No hidden state.
 */
#ifndef PHX_COST_H
#define PHX_COST_H

#include <stdint.h>
#include "phx_param.h"
#include "phx_tables.h"

/* x264_t's cost storage (common/common.h:351-354), reduced to the fields the
 * ported functions touch. cost_mv[] pointers are centred at +/- 2*4*mv_range. */
typedef struct {
    uint16_t *cost_mv[PHX_QP_MAX+1];
    uint16_t *cost_mv_fpel[PHX_QP_MAX+1][4];
    uint16_t cost_ref[PHX_QP_MAX+1][3][33];
    uint16_t cost_i4x4_mode[PHX_QP_MAX+1][17];
} phx_cost_tables_t;

/* The fields of x264_mb_analysis_t used by analyse.c:248-292. */
typedef struct {
    int i_qp;
    int i_lambda;
    int i_lambda2;
    int b_trellis;
    int i_trellis_lambda2[2][2];
    int i_psy_rd_lambda;
    int i_chroma_lambda2_offset;
    const uint16_t *p_cost_mv;
    const uint16_t *p_cost_ref[2];
} phx_mb_analysis_t;

/* x264 analyse.c:179. Initialises MV, reference and I4-mode costs for all
 * QPs in [qp_min, qp_max] plus X264_LOOKAHEAD_QP. Returns 0 / -1 (OOM). */
int phx_analyse_init_costs(phx_cost_tables_t *t, int mv_range, int me_method,
                           int qp_min, int qp_max);

/* x264 analyse.c:204. Releases the centred allocations. */
void phx_analyse_free_costs(phx_cost_tables_t *t, int mv_range);

/* x264 common/bitstream.h:301. Exp-Golomb signed bit size. */
int phx_bs_size_te(int x, int val);

/* x264 analyse.c:255. Sets lambda/lambda2, trellis lambdas, chroma lambda
 * offset and QP fields of a per-MB analysis state. */
void phx_mb_analyse_init_qp(const phx_param_t *p, int qp,
                            int chroma_qp_index_offset, int mbrd,
                            phx_mb_analysis_t *a);

/* x264 analyse.c:248. Wires the analysis state to the MV/ref cost arrays,
 * selecting the reference-cost table by active reference count. */
void phx_mb_analyse_load_costs(phx_cost_tables_t *t, phx_mb_analysis_t *a,
                               int num_ref_l0, int num_ref_l1);

#endif
