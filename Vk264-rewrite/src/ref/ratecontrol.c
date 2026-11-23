/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ratecontrol.h"
#include <math.h>

/* x264 encoder/ratecontrol.c:203-210, float arithmetic (QP_BD_OFFSET=0). */
float vk_qp2qscale(float qp)
{
    return 0.85f * powf(2.0f, (qp - 12.0f) / 6.0f);
}

float vk_qscale2qp(float qscale)
{
    return 12.0f + 6.0f * log2f(qscale / 0.85f);
}

uint16_t vk_rc_lambda(int qp)
{
    float v = powf(2.0f, qp / 6.0f - 2.0f);
    uint16_t l = (uint16_t)(v + 0.5f);
    return l < 1 ? 1 : l;
}

double vk_qscale2bits(double tex_bits, double qscale, double rce_qscale)
{
    if (qscale < 0.1) qscale = 0.1;
    return (tex_bits + 0.1) * pow(rce_qscale / qscale, 1.1);
}

double vk_rc_crf_constant(int mb_count,int has_bframes,double qcompress,
                          int mb_tree,double rf_constant)
{
    /* x264 encoder/ratecontrol.c:642-645 (8-bit, QP_BD_OFFSET=0). */
    double base_cplx = (double)mb_count * (has_bframes ? 120 : 80);
    float qc = (float)qcompress;   /* x264 rc.qcompress is float */
    double mbtree_offset = mb_tree ? (1.0 - qc) * 13.5 : 0.0;
    return pow(base_cplx, 1.0 - qc)
         / vk_qp2qscale((float)(rf_constant + mbtree_offset));
}

double vk_rc_get_qscale(double blurred_complexity,double qcompress,
                        double rate_factor)
{
    /* x264 ratecontrol.c:2014,2022: q = pow(cplx, 1-qcompress) / rate_factor.
     * rcc->qcompress is a float, hence the cast to reproduce its rounding. */
    float qc = (float)qcompress;
    return pow(blurred_complexity, 1.0 - qc) / rate_factor;
}

void vk_rc_state_init(vk_rc_state_t *rc,int mb_count,int bframes,
                      double qcompress,int mb_tree,double rf_constant)
{
    rc->short_term_cplxsum = 0;
    rc->short_term_cplxcount = 0;
    rc->qcompress = (float)qcompress;
    rc->rate_factor_constant =
        vk_rc_crf_constant(mb_count,bframes,rc->qcompress,mb_tree,rf_constant);
}

double vk_rc_frame_qscale(vk_rc_state_t *rc,double satd)
{
    /* x264 ratecontrol.c:2560-2566. */
    rc->short_term_cplxsum  *= 0.5;
    rc->short_term_cplxcount *= 0.5;
    rc->short_term_cplxsum  += satd;
    rc->short_term_cplxcount += 1.0;
    double blurred = rc->short_term_cplxsum / rc->short_term_cplxcount;
    return vk_rc_get_qscale(blurred,rc->qcompress,rc->rate_factor_constant);
}
