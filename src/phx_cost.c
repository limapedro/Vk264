/* Function-for-function port of the cost-table setup in x264 analyse.c.
 * Every assignment carries its source line. No approximations.
 */
#include "phx_cost.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PHX_MIN(a,b) ((a) < (b) ? (a) : (b))
#define PHX_MAX(a,b) ((a) > (b) ? (a) : (b))
#define PHX_CLIP3(a,v,b) ((v) < (a) ? (a) : ((v) > (b) ? (b) : (v)))

/* x264 common/bitstream.h:301 — exactly the same two branches. */
int phx_bs_size_te(int x, int val)
{
    if (x == 1)
        return 1;
    return phx_ue_size_tab[val+1];
}

/* x264 encoder/analyse.c:143 — init_costs(). `logs` is the shared table built
 * by x264_analyse_init_costs (analyse.c:186-188). */
static int phx_init_costs(phx_cost_tables_t *t, const float *logs,
                          int qp, int mv_range, int me_method)
{
    if (t->cost_mv[qp])
        return 0;                                    /* analyse.c:145 */
    int lambda = phx_lambda_tab[qp];                  /* analyse.c:149 */
    uint16_t *base = malloc((4*4*mv_range + 1) * sizeof(uint16_t)); /* :151 */
    if (!base)
        return -1;                                   /* :175 fail */
    t->cost_mv[qp] = base + 2*4*mv_range;             /* analyse.c:152 */
    for (int i = 0; i <= 2*4*mv_range; i++)          /* analyse.c:153 */
    {
        int c = (int)(lambda * logs[i] + .5f);       /* analyse.c:156 */
        t->cost_mv[qp][-i] = t->cost_mv[qp][i] =
            (uint16_t)PHX_MIN(c, UINT16_MAX);         /* analyse.c:155-156 */
    }
    for (int i = 0; i < 3; i++)                      /* analyse.c:158 */
        for (int j = 0; j < 33; j++)                 /* analyse.c:159 */
            t->cost_ref[qp][i][j] = (uint16_t)(i
                ? PHX_MIN(lambda * phx_bs_size_te(i, j), UINT16_MAX) : 0);
                                                     /* analyse.c:160 */

    /* analyse.c:161 — only ESA/UMH/TESA need fullpel MV costs. */
    if (me_method >= 3 && !t->cost_mv_fpel[qp][0])   /* X264_ME_ESA */
    {
        for (int j = 0; j < 4; j++)                  /* analyse.c:163 */
        {
            uint16_t *fpel = malloc((4*mv_range + 1) * sizeof(uint16_t));
            if (!fpel)
                return -1;                           /* :175 fail */
            t->cost_mv_fpel[qp][j] = fpel + 2*mv_range; /* analyse.c:166 */
            for (int i = -2*mv_range; i < 2*mv_range; i++) /* analyse.c:167 */
                t->cost_mv_fpel[qp][j][i] = t->cost_mv[qp][i*4+j]; /* :168 */
        }
    }
    uint16_t *cost_i4x4_mode = t->cost_i4x4_mode[qp]; /* analyse.c:171 */
    for (int i = 0; i < 17; i++)                     /* analyse.c:172 */
        cost_i4x4_mode[i] = (uint16_t)(3*lambda*(i != 8)); /* :173 */
    return 0;
}

/* x264 encoder/analyse.c:179 — x264_analyse_init_costs(). */
int phx_analyse_init_costs(phx_cost_tables_t *t, int mv_range, int me_method,
                           int qp_min, int qp_max)
{
    memset(t, 0, sizeof(*t));
    float *logs = malloc((2*4*mv_range+1) * sizeof(float)); /* analyse.c:182 */
    if (!logs)
        return -1;
    logs[0] = 0.718f;                                 /* analyse.c:186 */
    for (int i = 1; i <= 2*4*mv_range; i++)           /* analyse.c:187 */
        logs[i] = log2f((float)(i+1)) * 2.0f + 1.718f; /* analyse.c:188 */

    int rc = 0;
    for (int qp = PHX_MIN(qp_min, PHX_QP_MAX_SPEC);   /* analyse.c:190 */
         qp <= qp_max; qp++) {
        if (phx_init_costs(t, logs, qp, mv_range, me_method)) { rc = -1; break; }
    }
    if (!rc && phx_init_costs(t, logs, PHX_LOOKAHEAD_QP, mv_range, me_method))
        rc = -1;                                      /* analyse.c:194 */
    free(logs);                                       /* analyse.c:197 */
    return rc;
}

/* x264 encoder/analyse.c:204 — x264_analyse_free_costs(). */
void phx_analyse_free_costs(phx_cost_tables_t *t, int mv_range)
{
    for (int i = 0; i <= PHX_QP_MAX; i++)             /* analyse.c:207 */
    {
        if (t->cost_mv[i])
            free(t->cost_mv[i] - 2*4*mv_range);       /* analyse.c:210 */
        for (int j = 0; j < 4; j++)
            if (t->cost_mv_fpel[i][j])
                free(t->cost_mv_fpel[i][j] - 2*mv_range); /* analyse.c:214 */
    }
    memset(t, 0, sizeof(*t));
}

/* x264 encoder/analyse.c:255 — mb_analyse_init_qp(). */
void phx_mb_analyse_init_qp(const phx_param_t *p, int qp,
                            int chroma_qp_index_offset, int mbrd,
                            phx_mb_analysis_t *a)
{
    int spec_qp = PHX_MIN(qp, PHX_QP_MAX_SPEC);       /* SPEC_QP, common.h:64 */
    int chroma_base = phx_chroma_qp_base[
        PHX_CLIP3(0, PHX_CHROMA_QP_BASE_OFFSET + chroma_qp_index_offset + spec_qp,
                  (int)(sizeof(phx_chroma_qp_base)-1))]; /* encoder.c:1585 */
    int effective_chroma_qp = chroma_base + PHX_MAX(qp - PHX_QP_MAX_SPEC, 0);
                                                     /* analyse.c:257 */
    a->i_lambda = phx_lambda_tab[qp];                 /* analyse.c:258 */
    a->i_lambda2 = phx_lambda2_tab[qp];               /* analyse.c:259 */

    a->b_trellis = p->i_trellis > 1 && mbrd;          /* analyse.c:261 */
    if (p->i_trellis)                                 /* analyse.c:262 */
    {
        a->i_trellis_lambda2[0][0] = phx_trellis_lambda2_tab[0][qp];
        a->i_trellis_lambda2[0][1] = phx_trellis_lambda2_tab[1][qp];
        a->i_trellis_lambda2[1][0] = phx_trellis_lambda2_tab[0][effective_chroma_qp];
        a->i_trellis_lambda2[1][1] = phx_trellis_lambda2_tab[1][effective_chroma_qp];
                                                     /* analyse.c:264-267 */
    }
    a->i_psy_rd_lambda = a->i_lambda;                 /* analyse.c:269 */
    int chroma_offset_idx = PHX_MIN(qp - effective_chroma_qp + 12,
                                    PHX_MAX_CHROMA_LAMBDA_OFFSET); /* :271 */
    chroma_offset_idx = PHX_MAX(chroma_offset_idx, 0);
    a->i_chroma_lambda2_offset = p->b_psy
        ? phx_chroma_lambda2_offset_tab[chroma_offset_idx] : 256;
                                                     /* analyse.c:272 */
    a->i_qp = PHX_MIN(qp, PHX_QP_MAX_SPEC);           /* analyse.c:280,290 */
}

/* x264 encoder/analyse.c:248 — mb_analyse_load_costs(). */
void phx_mb_analyse_load_costs(phx_cost_tables_t *t, phx_mb_analysis_t *a,
                               int num_ref_l0, int num_ref_l1)
{
    a->p_cost_mv = t->cost_mv[a->i_qp];               /* analyse.c:250 */
    a->p_cost_ref[0] = t->cost_ref[a->i_qp][PHX_CLIP3(0, num_ref_l0-1, 2)];
    a->p_cost_ref[1] = t->cost_ref[a->i_qp][PHX_CLIP3(0, num_ref_l1-1, 2)];
                                                     /* analyse.c:251-252 */
}
