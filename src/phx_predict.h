/* Phoenix intra predictors — function-for-function port of x264's scalar
 * common/predict.c (8-bit, HIGH_BIT_DEPTH=0, BIT_DEPTH=8).
 *
 * All functions are pure: the destination buffer and its stride are passed
 * explicitly, and all neighbour samples are read from that same buffer at
 * dst[-1] (left), dst[-stride] (top), dst[-stride-1] (top-left), etc. This is
 * exactly x264's FDEC_STRIDE convention (common.h:571) with a variable stride
 * instead of the hard-coded 32, so an invocation needs no encoder state.
 *
 * Source: reference/x264/common/predict.c:57-884;
 *         neighbour flags from reference/x264/common/macroblock.h:31-41;
 *         pixel clip from reference/x264/common/common.h:145-148.
 *
 * SHADER: A — one invocation per block; neighbour samples via image loads.
 */
#ifndef PHX_PREDICT_H
#define PHX_PREDICT_H

#include <stdint.h>

/* common/macroblock.h:31-41 (enum macroblock_position_e). */
#define PHX_MB_LEFT     0x01
#define PHX_MB_TOP      0x02
#define PHX_MB_TOPRIGHT 0x04
#define PHX_MB_TOPLEFT  0x08
#define PHX_MB_ALL      0x0f

#define PHX_PRED_BIT_DEPTH 8
#define PHX_PRED_PIXEL_MAX 255

#ifdef __cplusplus
extern "C" {
#endif

/* 16x16 luma — predict.c:67/80/90/100/104/116/132. */
void phx_predict_16x16_dc(uint8_t *dst, int stride);
void phx_predict_16x16_dc_left(uint8_t *dst, int stride);
void phx_predict_16x16_dc_top(uint8_t *dst, int stride);
void phx_predict_16x16_dc_128(uint8_t *dst, int stride);
void phx_predict_16x16_h(uint8_t *dst, int stride);
void phx_predict_16x16_v(uint8_t *dst, int stride);
void phx_predict_16x16_p(uint8_t *dst, int stride);

/* 8x8 chroma (4:2:0) — predict.c:167/176/202/221/260/270/282. */
void phx_predict_8x8c_dc(uint8_t *dst, int stride);
void phx_predict_8x8c_dc_left(uint8_t *dst, int stride);
void phx_predict_8x8c_dc_top(uint8_t *dst, int stride);
void phx_predict_8x8c_dc_128(uint8_t *dst, int stride);
void phx_predict_8x8c_h(uint8_t *dst, int stride);
void phx_predict_8x8c_v(uint8_t *dst, int stride);
void phx_predict_8x8c_p(uint8_t *dst, int stride);

/* 8x16 chroma (4:2:2) — predict.c:314/323/342/361/421/431/443. */
void phx_predict_8x16c_dc(uint8_t *dst, int stride);
void phx_predict_8x16c_dc_left(uint8_t *dst, int stride);
void phx_predict_8x16c_dc_top(uint8_t *dst, int stride);
void phx_predict_8x16c_dc_128(uint8_t *dst, int stride);
void phx_predict_8x16c_h(uint8_t *dst, int stride);
void phx_predict_8x16c_v(uint8_t *dst, int stride);
void phx_predict_8x16c_p(uint8_t *dst, int stride);

/* 4x4 luma — predict.c:481/485/490/495/501/508/534/546/560/577/594/610. */
void phx_predict_4x4_dc(uint8_t *dst, int stride);
void phx_predict_4x4_dc_left(uint8_t *dst, int stride);
void phx_predict_4x4_dc_top(uint8_t *dst, int stride);
void phx_predict_4x4_dc_128(uint8_t *dst, int stride);
void phx_predict_4x4_h(uint8_t *dst, int stride);
void phx_predict_4x4_v(uint8_t *dst, int stride);
void phx_predict_4x4_ddl(uint8_t *dst, int stride);
void phx_predict_4x4_ddr(uint8_t *dst, int stride);
void phx_predict_4x4_vr(uint8_t *dst, int stride);
void phx_predict_4x4_hd(uint8_t *dst, int stride);
void phx_predict_4x4_vl(uint8_t *dst, int stride);
void phx_predict_4x4_hu(uint8_t *dst, int stride);

/* 8x8 luma with optional edge filtering — predict.c:632/700/704/710/716/
 * 723/731/741/761/783/811/838/865. edge[36] layout is documented at
 * predict.c:634-637: left l7..l0 at [7..14], lt at [15], top t0..t15 at
 * [16..31], t15 duplicate at [32]. */
void phx_predict_8x8_filter(uint8_t *dst, uint8_t edge[36], int stride,
                            int i_neighbor, int i_filters);
void phx_predict_8x8_dc(uint8_t *dst, const uint8_t edge[36], int stride);
void phx_predict_8x8_dc_left(uint8_t *dst, const uint8_t edge[36], int stride);
void phx_predict_8x8_dc_top(uint8_t *dst, const uint8_t edge[36], int stride);
void phx_predict_8x8_dc_128(uint8_t *dst, const uint8_t edge[36], int stride);
void phx_predict_8x8_h(uint8_t *dst, const uint8_t edge[36], int stride);
void phx_predict_8x8_v(uint8_t *dst, const uint8_t edge[36], int stride);
void phx_predict_8x8_ddl(uint8_t *dst, const uint8_t edge[36], int stride);
void phx_predict_8x8_ddr(uint8_t *dst, const uint8_t edge[36], int stride);
void phx_predict_8x8_vr(uint8_t *dst, const uint8_t edge[36], int stride);
void phx_predict_8x8_hd(uint8_t *dst, const uint8_t edge[36], int stride);
void phx_predict_8x8_vl(uint8_t *dst, const uint8_t edge[36], int stride);
void phx_predict_8x8_hu(uint8_t *dst, const uint8_t edge[36], int stride);

/* Function-pointer tables, x264_predict*_init (predict.c:889/936/977/
 * 1000/1042) without the CPU-specific asm dispatch. */
typedef void (*phx_predict_t)(uint8_t *, int);
typedef void (*phx_predict8x8_t)(uint8_t *, const uint8_t *, int);
void phx_predict_16x16_init(phx_predict_t pf[7]);
void phx_predict_8x8c_init(phx_predict_t pf[7]);
void phx_predict_8x16c_init(phx_predict_t pf[7]);
void phx_predict_8x8_init(phx_predict8x8_t pf[12],
                          void (**filter)(uint8_t *, uint8_t *, int, int, int));
void phx_predict_4x4_init(phx_predict_t pf[12]);

#ifdef __cplusplus
}
#endif
#endif
