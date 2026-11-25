/* Explicit per-MB decision boundary between the parallel analysis stages
 * (shader classes A/B) and the serial entropy stage (class C).
 *
 * Specified by docs/SHADER_MAPPING.md:67-70 and docs/SHADER_NOTES.md.
 * Workers must treat this as read-only/shared; it is owned by the orchestrator.
 *
 * SHADER: B — produced by workgroup stages, consumed by the serial writer.
 */
#ifndef PHX_MB_DECISION_H
#define PHX_MB_DECISION_H

#include <stdint.h>

typedef struct {
    int mb_type;                      /* PHX_I_4x4 / PHX_I_8x8 / PHX_I_16x16 /
                                       * PHX_I_PCM / PHX_P_L0 / PHX_P_SKIP / ... */
    int i_partition;                  /* D_16x16, D_8x8, ... (x264 enum) */
    int i_qp;
    int b_transform_8x8;
    int cbp_luma, cbp_chroma;
    int i_cbp;

    int8_t  intra4x4_pred_mode[16];   /* x264 intra4x4_pred_mode, scan8 order */
    int8_t  intra8x8_pred_mode[4];
    int8_t  intra16_pred_mode;
    int8_t  chroma_pred_mode;

    int16_t mv[2][16][2];             /* [list][4x4 block][x,y], qpel */
    int8_t  ref[2][16];               /* [list][4x4 block] */

    uint8_t nnz[48];                  /* non_zero_count per scan8 block */
    int16_t dct[48][16];              /* quantized coefficients per block */
    uint8_t skip;                     /* P_SKIP / B_SKIP candidate */
} phx_mb_decision_t;

#endif
