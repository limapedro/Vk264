/* Phoenix deadzone quantization — function-for-function port of the scalar C
 * path of reference/x264/common/quant.c (8-bit build), plus the flat-CQM
 * quantizer/dequantizer table construction from common/set.c:29-205.
 *
 * Source: reference/x264/common/quant.c:50-315;
 *         reference/x264/common/set.c:29-71,73-205;
 *         common.h:93-108 (dctcoef/udctcoef), common.h:342-347 (bias0),
 *         base.c:456-465 (defaults).
 *
 * SHADER: A — per coefficient, vectorizable; no cross-coefficient state.
 */
#ifndef PHX_QUANT_H
#define PHX_QUANT_H

#include <stdint.h>
#include "phx_dct.h"

#define PHX_QUANT_QP_MAX_SPEC 51         /* common.h:59, BIT_DEPTH=8 */

/* x264's h->quant4_mf/quant8_mf/quant4_bias/quant8_bias/
 * quant4_bias0/quant8_bias0/dequant4_mf/dequant8_mf
 * (common.h:336-347) for the default flat scaling lists.  bias0 is the
 * trellis QNS bias of set.c:180/200, NOT the deadzone bias of :179/199.
 * SHADER: B — table storage, built once per encode. */
typedef struct {
    uint16_t quant4_mf[4][PHX_QUANT_QP_MAX_SPEC + 1][16];
    uint16_t quant8_mf[2][PHX_QUANT_QP_MAX_SPEC + 1][64];
    uint16_t quant4_bias[4][PHX_QUANT_QP_MAX_SPEC + 1][16];
    uint16_t quant8_bias[2][PHX_QUANT_QP_MAX_SPEC + 1][64];
    int dequant4_mf[4][6][16];
    int dequant8_mf[2][6][64];
    uint16_t quant4_bias0[4][PHX_QUANT_QP_MAX_SPEC + 1][16];
    uint16_t quant8_bias0[2][PHX_QUANT_QP_MAX_SPEC + 1][64];
} phx_quant_tables_t;

#ifdef __cplusplus
extern "C" {
#endif

/* set.c:73-205 — x264_cqm_init() for flat lists (all entries 16) and the
 * default deadzones {21,11,21,11}. SHADER: B — per-encode table setup. */
void phx_quant_tables_init(phx_quant_tables_t *t);

/* quant.c:50-57 — QUANT_ONE(coef, mf, f). Returns the quantized coefficient
 * and ORs it into *nz, exactly as the x264 macro does.
 * SHADER: A — per coefficient. */
static inline phx_dctcoef phx_quant_one(phx_dctcoef coef, uint16_t mf,
                                        uint16_t bias, int *nz)
{
    if (coef > 0)
        coef = (phx_dctcoef)(((uint32_t)bias + (uint32_t)coef) * mf >> 16);
    else
        coef = (phx_dctcoef)-((int32_t)(((uint32_t)bias + (uint32_t)(-coef)) * mf >> 16));
    *nz |= coef;
    return coef;
}

/* quant.c:59/67/75/88/96 — deadzone quantizers.
 * SHADER: A — one invocation per block/coefficient group. */
int phx_quant_8x8(phx_dctcoef dct[64], const uint16_t mf[64], const uint16_t bias[64]);
int phx_quant_4x4(phx_dctcoef dct[16], const uint16_t mf[16], const uint16_t bias[16]);
int phx_quant_4x4x4(phx_dctcoef dct[4][16], const uint16_t mf[16], const uint16_t bias[16]);
int phx_quant_4x4_dc(phx_dctcoef dct[16], int mf, int bias);
int phx_quant_2x2_dc(phx_dctcoef dct[4], int mf, int bias);

/* quant.c:112/130/148 — dequantizers.
 * SHADER: A — per coefficient. */
void phx_dequant_4x4(phx_dctcoef dct[16], const int dequant_mf[6][16], int i_qp);
void phx_dequant_8x8(phx_dctcoef dct[64], const int dequant_mf[6][64], int i_qp);
void phx_dequant_4x4_dc(phx_dctcoef dct[16], const int dequant_mf[6][16], int i_qp);

/* quant.c:167-211 — 2x4 DC dequantization fused with the 4x4 IDCT.
 * SHADER: A — one invocation per chroma DC block. */
void phx_idct_dequant_2x4_dc(phx_dctcoef dct[8], phx_dctcoef dct4x4[8][16],
                             const int dequant_mf[6][16], int i_qp);
void phx_idct_dequant_2x4_dconly(phx_dctcoef dct[8],
                                 const int dequant_mf[6][16], int i_qp);

/* quant.c:213-237 — chroma DC reconstruction kernels.
 * SHADER: A — one invocation per chroma DC block. */
void phx_optimize_chroma_idct_dequant_2x4(phx_dctcoef out[8], const phx_dctcoef dct[8], int dmf);
void phx_optimize_chroma_idct_dequant_2x2(phx_dctcoef out[4], const phx_dctcoef dct[4], int dmf);
int  phx_optimize_chroma_round(const phx_dctcoef *ref, const phx_dctcoef *dct,
                               int dequant_mf, int chroma422);
int  phx_optimize_chroma_dc_internal(phx_dctcoef *dct, int dequant_mf, int chroma422);
int  phx_optimize_chroma_2x2_dc(phx_dctcoef dct[4], int dequant_mf);
int  phx_optimize_chroma_2x4_dc(phx_dctcoef dct[8], int dequant_mf);

/* quant.c:304-315 — noise-reduction offset application.
 * SHADER: A — per coefficient. */
void phx_denoise_dct(phx_dctcoef *dct, uint32_t *sum, const phx_udctcoef *offset, int size);

#ifdef __cplusplus
}
#endif
#endif
