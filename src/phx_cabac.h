/* Phoenix CABAC encoder — mirrors reference/x264/common/cabac.{h,c}.
 *
 * Source: common/cabac.h:30-52 (x264_cabac_t),
 *         common/cabac.c:32-52 (context init), :53-59 (encode init),
 *         :69-99 (putbyte), :101-108 (renorm), :113-163 (decision/bypass/
 *         ue_bypass/terminal), :165-183 (flush).
 *
 * SHADER: C — strictly serial arithmetic coder state; keep in the entropy stage.
 */
#ifndef PHX_CABAC_H
#define PHX_CABAC_H

#include <stdint.h>

#define PHX_SLICE_P 0
#define PHX_SLICE_B 1
#define PHX_SLICE_I 2
#define PHX_CABAC_CTX_COUNT 460   /* 4:2:0, cabac.c:34 */

typedef struct {
    int i_low, i_range;
    int i_queue;
    int i_bytes_outstanding;
    uint8_t *p_start, *p, *p_end;
    uint8_t state[1024];
} phx_cabac_t;

/* cabac.c:32 — build the 460 context states for a slice type/QP/model. */
void phx_cabac_context_init(phx_cabac_t *cb, int slice_type, int qp, int model);

/* cabac.c:53/61 — reset coder state and point it at the output buffer. */
void phx_cabac_encode_init_core(phx_cabac_t *cb);
void phx_cabac_encode_init(phx_cabac_t *cb, uint8_t *data, uint8_t *end);

/* cabac.c:113/128/142/159/165. */
void phx_cabac_encode_decision(phx_cabac_t *cb, int ctx, int b);
void phx_cabac_encode_bypass(phx_cabac_t *cb, int b);
void phx_cabac_encode_ue_bypass(phx_cabac_t *cb, int exp_bits, int val);
void phx_cabac_encode_terminal(phx_cabac_t *cb);
void phx_cabac_encode_flush(phx_cabac_t *cb, int frame);

#endif
