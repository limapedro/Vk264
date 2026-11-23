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
    /* CRF get_qscale, oracle values from x264 ratecontrol.c:2014 dumped with
     * build/crf_qscale_dump.py (cplx, qcompress, rf, expected qscale). */
    struct { double cplx, qc, rf, want; } gq[] = {
        {2843.999755859, 0.6, 12.102399354, 24.075301776/12.102399354},
        {1432.799926758, 0.6,  6.792241744, 18.300971513/ 6.792241744},
        {1955.314086914, 0.6,  6.792241744, 20.724617947/ 6.792241744},
        {3710.559814453, 0.6,  3.812016826, 26.777849085/ 3.812016826},
        {4581.290039062, 0.6,  3.812016826, 29.133658672/ 3.812016826}
    };
    for (unsigned i=0; i<sizeof(gq)/sizeof(gq[0]); ++i) {
        double got=vk_rc_get_qscale(gq[i].cplx,gq[i].qc,gq[i].rf);
        if (fabs(got-gq[i].want) > 2e-6) {
            fprintf(stderr,"CRF qscale FAIL cplx=%.3f got=%.9f want=%.9f\n",
                    gq[i].cplx,got,gq[i].want);
            return 1;
        }
    }
    /* Stateful CRF blur: SATD values derived from x264 crf_state_dump.py
     * (sumnew = 0.5*sumprev + satd), expected qscales from crf_qscale_dump.py.
     * 64x64 superfast CRF23: mb=16, bframes=3, qcompress=0.6. */
    vk_rc_state_t rc;
    vk_rc_state_init(&rc,16,1,0.6,0,23.0);
    if (fabs(rc.rate_factor_constant-6.792241744)>1e-6) {
        fprintf(stderr,"CRF state init constant FAIL\n"); return 1;
    }
    struct { double satd, want_blur, want_q; } sf[] = {
        {2844.0,     2843.999755859, 24.075301776/6.792241744},
        { 727.2,     1432.799926758, 18.300971513/6.792241744},
        {2347.2,     1955.314086914, 20.724617947/6.792241744},
        {5246.4,     3710.559814453, 26.777849085/6.792241744},
        {5397.6,     4581.290039062, 29.133658672/6.792241744}
    };
    for (unsigned i=0; i<sizeof(sf)/sizeof(sf[0]); ++i) {
        double q=vk_rc_frame_qscale(&rc,sf[i].satd);
        double q_want=vk_rc_get_qscale(sf[i].want_blur,0.6,6.792241744);
        if (fabs(q-q_want)>2e-5) {
            fprintf(stderr,"CRF state FAIL i=%u q=%.9f want=%.9f (blur %.3f)\n",
                    i,q,q_want,sf[i].want_blur);
            return 1;
        }
    }
    /* x264 single-pass CRF frame-0 I QP (build/crf_qp_dump.py):
     * CRF23, ip_factor=1.4 -> q = qp2qscale(23)/1.4 = 2.163611412. */
    vk_rc_state_t rc0;
    vk_rc_state_init(&rc0,16,1,0.6,0,23.0);
    double qp0=vk_rc_frame_qp(&rc0,2844.0,1,0,1.4f);
    double want0=vk_qscale2qp(vk_qp2qscale(23.0f)/1.4f);
    if (fabs(qp0-want0)>1e-4) {
        fprintf(stderr,"CRF frame0 QP FAIL got=%.6f want=%.6f\n",qp0,want0);
        return 1;
    }
    printf("ratecontrol PASS: primitives + CRF const/qscale + stateful blur + frame0 QP\n");
    return 0;
}
