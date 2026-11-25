/* Phoenix forward DCT / Hadamard / IDCT — function-for-function port of the
 * scalar C path of reference/x264/common/dct.c (8-bit build, dctcoef=int16_t,
 * common.h:101-108). Strides are explicit so every function is pure.
 *
 * Source: reference/x264/common/dct.c:47-471;
 *         common.h:145-148 (x264_clip_pixel), common.h:570-571 (FENC/FDEC).
 *
 * SHADER: A — one invocation per block; registers for 4x4, shared memory 8x8.
 */
#ifndef PHX_DCT_H
#define PHX_DCT_H

#include <stdint.h>

#define PHX_FENC_STRIDE 16              /* common.h:570 */
#define PHX_FDEC_STRIDE 32              /* common.h:571 */

typedef int16_t phx_dctcoef;
typedef uint16_t phx_udctcoef;

#ifdef __cplusplus
extern "C" {
#endif

/* Hadamard transforms of DC coefficients — dct.c:47/78/109. */
void phx_dct4x4dc(phx_dctcoef d[16]);
void phx_idct4x4dc(phx_dctcoef d[16]);
void phx_dct2x4dc(phx_dctcoef dct[8], phx_dctcoef dct4x4[8][16]);

/* Forward 4x4 DCT and its block compositions — dct.c:145/157/191/199. */
void phx_sub4x4_dct(phx_dctcoef dct[16], const uint8_t *pix1, int i_pix1,
                    const uint8_t *pix2, int i_pix2);
void phx_sub8x8_dct(phx_dctcoef dct[4][16], const uint8_t *pix1, int i_pix1,
                    const uint8_t *pix2, int i_pix2);
void phx_sub16x16_dct(phx_dctcoef dct[16][16], const uint8_t *pix1, int i_pix1,
                      const uint8_t *pix2, int i_pix2);

/* Pure-DC (Hadamard) forward transforms — dct.c:207/216/234. */
int  phx_sub4x4_dct_dc(const uint8_t *pix1, int i_pix1,
                       const uint8_t *pix2, int i_pix2);
void phx_sub8x8_dct_dc(phx_dctcoef dct[4], const uint8_t *pix1, int i_pix1,
                       const uint8_t *pix2, int i_pix2);
void phx_sub8x16_dct_dc(phx_dctcoef dct[8], const uint8_t *pix1, int i_pix1,
                        const uint8_t *pix2, int i_pix2);

/* Inverse 4x4 DCT (add to reconstruction) and compositions — dct.c:272/312/320. */
void phx_add4x4_idct(uint8_t *dst, int stride, const phx_dctcoef dct[16]);
void phx_add8x8_idct(uint8_t *dst, int stride, const phx_dctcoef dct[4][16]);
void phx_add16x16_idct(uint8_t *dst, int stride, const phx_dctcoef dct[16][16]);

/* 8x8 DCT / IDCT (high profile) — dct.c:332/359/380/388/415/434. */
void phx_sub8x8_dct8(phx_dctcoef dct[64], const uint8_t *pix1, int i_pix1,
                     const uint8_t *pix2, int i_pix2);
void phx_sub16x16_dct8(phx_dctcoef dct[4][64], const uint8_t *pix1, int i_pix1,
                       const uint8_t *pix2, int i_pix2);
void phx_add8x8_idct8(uint8_t *dst, int stride, phx_dctcoef dct[64]);
void phx_add16x16_idct8(uint8_t *dst, int stride, phx_dctcoef dct[4][64]);

/* DC-only inverse transforms — dct.c:442/454/462. */
void phx_add4x4_idct_dc(uint8_t *dst, int stride, phx_dctcoef dc);
void phx_add8x8_idct_dc(uint8_t *dst, int stride, const phx_dctcoef dct[4]);
void phx_add16x16_idct_dc(uint8_t *dst, int stride, const phx_dctcoef dct[16]);

#ifdef __cplusplus
}
#endif
#endif
