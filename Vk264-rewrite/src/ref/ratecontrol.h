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

/* x264 encoder/ratecontrol.c:2014-2023 get_qscale: CRF qscale from the
 * frame's blurred complexity: pow(cplx, 1-qcompress) / rate_factor. */
double vk_rc_get_qscale(double blurred_complexity,double qcompress,
                        double rate_factor);

/* x264 single-pass CRF state: short_term_cplxsum/count (ratecontrol.c:2560-
 * 2566). Call vk_rc_frame_qscale once per coded frame with its SATD. */
typedef struct {
    double short_term_cplxsum, short_term_cplxcount;
    double rate_factor_constant;
    double rf_constant;
    float qcompress;
} vk_rc_state_t;

void vk_rc_state_init(vk_rc_state_t *rc,int mb_count,int bframes,
                      double qcompress,int mb_tree,double rf_constant);
double vk_rc_frame_qscale(vk_rc_state_t *rc,double satd);

/* x264 single-pass CRF QP selection (ratecontrol.c:2602-2629):
 * frame 0 uses qp2qscale(rf)/ip_factor; later frames use get_qscale.
 * ip_factor for superfast is 1.4. */
double vk_rc_frame_qp(vk_rc_state_t *rc,double satd,int is_idr,int frame_index,
                      float ip_factor);

#endif
