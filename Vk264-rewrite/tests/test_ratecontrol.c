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
    /* CRF constant: oracle values dumped from x264 ratecontrol.c:644.
     * 64x64 => 16 MBs; superfast => bframes=3, qcompress=0.6, mbtree=0. */
    struct { double rf, want; } crf[] = {
        {18.0, 12.102399354}, {23.0, 6.792241744}, {28.0, 3.812016826}
    };
    for (unsigned i=0; i<sizeof(crf)/sizeof(crf[0]); ++i) {
        double got=vk_rc_crf_constant(16,1,0.6,0,crf[i].rf);
        if (fabs(got-crf[i].want) > 1e-6) {
            fprintf(stderr,"CRF FAIL rf=%.1f got=%.9f want=%.9f\n",
                    crf[i].rf,got,crf[i].want);
            return 1;
        }
    }
    printf("ratecontrol fixture PASS: qp2qscale/qscale2qp/lambda + CRF constant\n");
    return 0;
}
