#include <stdio.h>
#include <math.h>
#include "ratecontrol.h"

/* Host reference: x264 formulas copied verbatim. */
static double ref_qp2qscale(double qp) { return 0.85f * powf(2.0f,(qp-12.0f)/6.0f); }
static double ref_qscale2qp(double q) { return 12.0f + 6.0f * log2f(q/0.85f); }

int main(void)
{
    for (int qp=0; qp<=51; ++qp) {
        float a=vk_qp2qscale(qp), b=ref_qp2qscale(qp);
        if (a!=b) { fprintf(stderr,"qp2qscale FAIL qp=%d %.9f %.9f\n",qp,a,b); return 1; }
        float c=vk_qscale2qp(a), d=ref_qscale2qp(a);
        if (c!=d) { fprintf(stderr,"qscale2qp FAIL qp=%d\n",qp); return 1; }
    }
    /* lambda table: x264 lambda_tab values QP 0..51 */
    static const uint16_t tab[52] = {
        1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 2,2,2,2,3,3,3,4,
        4,4,5,6,6,7,8,9, 10,11,13,14,16,18,20,23, 25,29,32,36,40,45,51,57,
        64,72,81,91
    };
    for (int qp=0; qp<=51; ++qp)
        if (vk_rc_lambda(qp)!=tab[qp]) {
            fprintf(stderr,"lambda FAIL qp=%d got=%d want=%d\n",qp,vk_rc_lambda(qp),tab[qp]);
            return 1;
        }
    printf("ratecontrol fixture PASS: qp2qscale/qscale2qp/lambda QP 0..51\n");
    return 0;
}
