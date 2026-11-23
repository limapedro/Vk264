/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "i8x8decide.h"

static int clip(int v){ return v<0?0:(v>255?255:v); }

void vk_predict8(uint8_t out[64], int m, const uint8_t l[8],
                 const uint8_t t[8], const uint8_t tr[8])
{
    int s;
    switch (m) {
    case 0: /* V */
        for (int y=0;y<8;++y) for(int x=0;x<8;++x) out[y*8+x]=t[x];
        break;
    case 1: /* H */
        for (int y=0;y<8;++y) for(int x=0;x<8;++x) out[y*8+x]=l[y];
        break;
    case 2: /* DC */
        s=0; for(int i=0;i<8;++i) s+=l[i]+t[i];
        s=(s+8)>>4;
        for(int i=0;i<64;++i) out[i]=(uint8_t)s;
        break;
    case 3: /* DDL (diagonal down-left) */
        for (int y=0;y<8;++y) for(int x=0;x<8;++x) {
            int i=x+y;
            out[y*8+x] = i<8 ? t[i] : (i==8 ? tr[0] : tr[i-8]);
        }
        break;
    case 4: /* DDR */
        for (int y=0;y<8;++y) for(int x=0;x<8;++x) {
            int i=7-y+x;
            out[y*8+x] = i<8 ? t[i] : (i==8 ? tr[0] : tr[i-8]);
        }
        break;
    case 5: /* VR (vertical-right) */
        for (int y=0;y<8;++y) for(int x=0;x<8;++x) {
            int i=4+(x>>1)-y;
            if (i>=0 && i<8) out[y*8+x]=t[i];
            else if (i<0) out[y*8+x]=l[7+((x+1)>>1)];
            else out[y*8+x]=tr[(i*2)&7];
        }
        break;
    case 6: /* HD (horizontal-down) */
        for (int y=0;y<8;++y) for(int x=0;x<8;++x) {
            int i=4+(y>>1)-x;
            if (i>=0 && i<8) out[y*8+x]=l[i];
            else if (i<0) out[y*8+x]=t[7+((y+1)>>1)];
            else out[y*8+x]=tr[0];
        }
        break;
    case 7: /* VL */
        for (int y=0;y<8;++y) for(int x=0;x<8;++x) {
            int i=x+(y>>1);
            out[y*8+x] = i<8 ? t[i] : tr[(i-8)>>1];
        }
        break;
    case 8: /* HU */
        for (int y=0;y<8;++y) for(int x=0;x<8;++x) {
            int i=y+(x>>1);
            out[y*8+x] = i<8 ? l[i] : l[7];
        }
        break;
    case 9: /* DC_LEFT */
        s=0; for(int i=0;i<8;++i) s+=l[i];
        s=(s+4)>>3; for(int i=0;i<64;++i) out[i]=(uint8_t)s;
        break;
    case 10: /* DC_TOP */
        s=0; for(int i=0;i<8;++i) s+=t[i];
        s=(s+4)>>3; for(int i=0;i<64;++i) out[i]=(uint8_t)s;
        break;
    default: /* DC_128 */
        for(int i=0;i<64;++i) out[i]=128;
        break;
    }
}

/* 4x4 Hadamard SATD reused across the 8x8 block. */
static int satd4(const uint8_t *a, int sa, const uint8_t *b, int sb)
{
    int t[16], d[16], s=0;
    for(int y=0;y<4;++y) for(int x=0;x<4;++x) d[y*4+x]=(int)a[y*sa+x]-(int)b[y*sb+x];
#define HAD2(dst,a,b,c,d) do{int p=(a)+(b),m=(a)-(b),q=(c)+(d),r=(c)-(d);\
    (dst)[0]=p+q;(dst)[2]=p-q;(dst)[1]=m+r;(dst)[3]=m-r;}while(0)
    for(int i=0;i<4;++i) HAD2(t+i*4,d[0*4+i],d[2*4+i],d[1*4+i],d[3*4+i]);
    for(int i=0;i<4;++i) HAD2(d+i*4,t[0*4+i],t[2*4+i],t[1*4+i],t[3*4+i]);
#undef HAD2
    for(int i=0;i<16;++i) s+=d[i]<0?-d[i]:d[i];
    return s>>1;
}

int vk_satd8(const uint8_t a[64], const uint8_t b[64])
{
    int total=0;
    for(int by=0;by<8;by+=4) for(int bx=0;bx<8;bx+=4)
        total += satd4(a+by*8+bx,8,b+by*8+bx,8);
    return total;
}

int vk_i8x8_decide(const uint8_t src[64], const uint8_t l[8],
                   const uint8_t t[8], const uint8_t tr[8],
                   int have_l, int have_t, int lambda)
{
    uint8_t pred[64];
    int best_mode = (have_l && have_t) ? 2 : (have_l ? 9 : (have_t ? 10 : 11));
    int best = 0x7fffffff;
    int modes[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
    for (int k=0;k<12;++k) {
        int m=modes[k];
        if (!have_l && (m==1||m==4||m==5||m==6||m==8)) continue;
        if (!have_t && (m==0||m==3||m==4||m==5||m==7)) continue;
        /* Edge DC variants only when the corresponding neighbour is absent. */
        if (m==9 && !(have_l && !have_t)) continue;
        if (m==10 && !(have_t && !have_l)) continue;
        if (m==11 && (have_l||have_t)) continue;
        vk_predict8(pred,m,l,t,tr);
        /* x264 analyse.c:816-821: the mode decision uses SA8D with a -3*lambda
         * DISCOUNT for the predicted mode (typically DC when both neighbours
         * exist), not a positive per-mode bit penalty. */
        int pred_mode = (have_l && have_t) ? 2 : (have_l ? 9 : (have_t ? 10 : 11));
        int cost = vk_satd8(src,pred) - (m==pred_mode ? 3*lambda : 0);
        if (cost < best) { best=cost; best_mode=m; }
    }
    return best_mode;
}
