/* SPDX-License-Identifier: GPL-2.0-or-later
 * x264 rate-control primitives for the C oracle (encoder/ratecontrol.c).
 * Fixed-QP is the only wired mode today; these conversions are the shared
 * foundation for CRF/ABR/AQ/mb-tree. */
#ifndef VK264_RATECONTROL_H
#define VK264_RATECONTROL_H
#include <stdint.h>

/* x264 encoder/ratecontrol.c:203, qp2qscale (8-bit, QP_BD_OFFSET=0). */
float vk_qp2qscale(float qp);

/* x264 encoder/ratecontrol.c:207, qscale2qp (8-bit). */
float vk_qscale2qp(float qscale);

/* x264 common/tables.c lambda = pow(2, qp/6 - 2), rounded to uint16. */
uint16_t vk_rc_lambda(int qp);

/* qscale -> bits model (x264 qscale2bits, power 1.1). */
double vk_qscale2bits(double tex_bits, double qscale, double rce_qscale);

/* x264 encoder/ratecontrol.c:638-646: CRF rate_factor_constant.
 * base_cplx = mb_count * (bframes ? 120 : 80);
 * mbtree_offset = mb_tree ? (1-qcompress)*13.5 : 0;
 * constant = base_cplx^(1-qcompress) / qp2qscale(rf + mbtree_offset). */
double vk_rc_crf_constant(int mb_count,int has_bframes,double qcompress,
                          int mb_tree,double rf_constant);

#endif
