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
