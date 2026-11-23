/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "i8x8decide.h"

static int clip(int v){ return v<0?0:(v>255?255:v); }

static void i8_build_edge(uint8_t e[36], const uint8_t l[8],
                          const uint8_t t[16], int lt, int have_tr);
static void i8_pred_edge(uint8_t out[64], int m, const uint8_t e[36]);

/* As vk_predict8_filtered with explicit top-right availability (x264's
 * final I8x8 encode uses a mode-dependent neighbour mask). */
void vk_predict8_filtered_ex(uint8_t out[64], int m, const uint8_t l[8],
                             const uint8_t t[16], int lt, int have_tr)
{
    uint8_t e[36];
    i8_build_edge(e,l,t,lt,have_tr);
    i8_pred_edge(out,m,e);
}

void vk_predict8_filtered(uint8_t out[64], int m, const uint8_t l[8],
                          const uint8_t t[16], int lt)
{
    vk_predict8_filtered_ex(out,m,l,t,lt,1);
}

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

/* x264 I16x16 mode-bit costs: lambda * bs_size_ue(mode).
 * bs_size_ue(0)=1, (1)=(2)=(3)=3. */
static const int i16_mode_bits[4]={1,3,3,3};

int vk_i16x16_decide(const uint8_t src[256],const uint8_t top[16],
                     const uint8_t left[16],int have_t,int have_l,int lambda,
                     int *cost_out)
{
    uint8_t pred[256];
    int best=0x7fffffff, best_mode=2;
    for(int m=0;m<4;++m){
        if(m==0 && !have_t) continue;
        if(m==1 && !have_l) continue;
        for(int y=0;y<16;++y) for(int x=0;x<16;++x){
            int v;
            if(m==0) v=top[x];
            else if(m==1) v=left[y];
            else {
                int s=0,n=0;
                if(!have_t && !have_l) v=128;
                else {
                    if(have_t){for(int i=0;i<16;++i){s+=top[i];++n;}}
                    if(have_l){for(int i=0;i<16;++i){s+=left[i];++n;}}
                    v=(s+n/2)/n;
                }
            }
            pred[y*16+x]=(uint8_t)v;
        }
        int c=0;
        for(int qy=0;qy<16;qy+=8) for(int qx=0;qx<16;qx+=8){
            uint8_t a[64],b[64];
            for(int y=0;y<8;++y) for(int x=0;x<8;++x){
                a[y*8+x]=src[(qy+y)*16+qx+x];
                b[y*8+x]=pred[(qy+y)*16+qx+x];
            }
            c+=vk_satd8(a,b);
        }
        c+=lambda*i16_mode_bits[m];
        if(c<best){best=c;best_mode=m;}
    }
    if(cost_out)*cost_out=best;
    return best_mode;
}

/* x264 8x8 intra mode decision: filtered edge + intra_sad_x3 + shortcut
 * (encoder/analyse.c:745-835, common/predict.c:627-621).
 * Keeps the existing public signature (l[8], t[8], tr[8]). */
static int i8_F1(int a,int b){ return (a+b+1)>>1; }
static int i8_F2(int a,int b,int c){ return (a+2*b+c+2)>>2; }

static void i8_build_edge(uint8_t e[36], const uint8_t l[8],
                          const uint8_t t[16], int lt, int have_tr)
{
    e[15]=(uint8_t)((t[0]+2*lt+l[0]+2)>>2);
    e[14]=(uint8_t)((lt+2*l[0]+l[1]+2)>>2);
    for(int y=1;y<7;++y) e[14-y]=(uint8_t)i8_F2(l[y-1],l[y],l[y+1]);
    e[7]=e[6]=(uint8_t)((l[6]+3*l[7]+2)>>2);
    e[16]=(uint8_t)((lt+2*t[0]+t[1]+2)>>2);
    for(int x=1;x<7;++x) e[16+x]=(uint8_t)i8_F2(t[x-1],t[x],t[x+1]);
    e[23]=(uint8_t)i8_F2(t[6],t[7],have_tr?t[8]:t[7]);
    if(have_tr){
        for(int x=8;x<15;++x) e[16+x]=(uint8_t)i8_F2(t[x-1],t[x],t[x+1]);
        e[31]=e[32]=(uint8_t)((t[14]+3*t[15]+2)>>2);
    } else {
        for(int x=24;x<33;++x) e[x]=t[7];
    }
}
#define EL(y) e[14-(y)]
#define ET(x) e[16+((x)<15?(x):15)]
#define ELT e[15]
static void i8_pred_edge(uint8_t out[64], int m, const uint8_t e[36])
{
    switch(m){
    case 0:
        for(int y=0;y<8;++y) for(int x=0;x<8;++x) out[y*8+x]=ET(x);
        break;
    case 1:
        for(int y=0;y<8;++y) for(int x=0;x<8;++x) out[y*8+x]=EL(y);
        break;
    case 2: case 9: case 10: case 11: {
        int s;
        if(m==2){s=0;for(int i=0;i<8;++i)s+=EL(i)+ET(i);s=(s+8)>>4;}
        else if(m==9){s=0;for(int i=0;i<8;++i)s+=EL(i);s=(s+4)>>3;}
        else if(m==10){s=0;for(int i=0;i<8;++i)s+=ET(i);s=(s+4)>>3;}
        else s=128;
        for(int i=0;i<64;++i) out[i]=(uint8_t)s;
        break;
    }
    case 3:
        for(int y=0;y<8;++y) for(int x=0;x<8;++x){
            int d=x+y; out[y*8+x]=(uint8_t)i8_F2(ET(d),ET(d+1),ET(d+2));
        }
        break;
    case 4:
        for(int y=0;y<8;++y) for(int x=0;x<8;++x){
            int k=7-y+x,v;
            if(k<=5)v=i8_F2(EL(7-k),EL(6-k),EL(5-k));
            else if(k==6)v=i8_F2(EL(1),EL(0),ELT);
            else if(k==7)v=i8_F2(EL(0),ELT,ET(0));
            else if(k==8)v=i8_F2(ELT,ET(0),ET(1));
            else v=i8_F2(ET(k-9),ET(k-8),ET(k-7));
            out[y*8+x]=(uint8_t)v;
        }
        break;
    case 5:
        for(int y=0;y<8;++y) for(int x=0;x<8;++x){
            int d=2*x-y,v;
            if(d>=0&&!(d&1)){int i=d/2;v=i8_F1(i?ET(i-1):ELT,ET(i));}
            else if(d>=0){ int k=(d-1)/2;
                v=i8_F2(k?ET(k-1):ELT, ET(k), ET(k+1)); }
            else {int n=-d;
                if(n==1)v=i8_F2(EL(0),ELT,ET(0));
                else if(n==2)v=i8_F2(EL(1),EL(0),ELT);
                else v=i8_F2(EL(n-1),EL(n-2),EL(n-3));
            }
            out[y*8+x]=(uint8_t)v;
        }
        break;
    case 6:
        for(int y=0;y<8;++y) for(int x=0;x<8;++x){
            int d=2*y-x,v;
            if(d>=0&&d<8) v=i8_F2(EL(d),(d+1<8)?EL(d+1):ELT,(d+2<8)?EL(d+2):ET(0));
            else if(d<0) v=i8_F2(ET(-d),ET(-d+1),ET(-d+2));
            else v=128;
            out[y*8+x]=(uint8_t)v;
        }
        break;
    case 7:
        for(int y=0;y<8;++y) for(int x=0;x<8;++x){
            int d=x+(y>>1); out[y*8+x]=(uint8_t)i8_F2(ET(d),ET(d+1),ET(d+2));
        }
        break;
    case 8:
        for(int y=0;y<8;++y) for(int x=0;x<8;++x){
            int d=y+(x>>1); out[y*8+x]=(uint8_t)(d<8?EL(d):EL(7));
        }
        break;
    default:
        for(int i=0;i<64;++i) out[i]=128;
        break;
    }
}
#undef EL
#undef ET
#undef ELT

static int i8_sad(const uint8_t a[64],const uint8_t b[64])
{
    int s=0;for(int i=0;i<64;++i){int d=a[i]-b[i];s+=d<0?-d:d;}return s;
}

/* intra_analysis_shortcut[avoid_topright=0][cand9][favor_vertical] */
static const int8_t I8_SC[2][2][5] = {
    {{8,-1,-1,-1,-1}, {3,7,-1,-1,-1}},
    {{4,6,8,-1,-1},  {3,4,5,7,-1}}
};

int vk_i8x8_decide_ex(const uint8_t src[64], const uint8_t l[8],
                   const uint8_t t[8], const uint8_t tr[8], int lt_in,
                   int have_l, int have_t, int lambda, int *cost_out,
                   int use_satd, int mb_gate, int have_tr)
{
    (void)use_satd;
    /* Exact x264 filtered-edge x3 + shortcut, only for MBs with all
     * three neighbours (mx>0 && my>0). Edge MBs keep the original
     * baseline all-candidate path — verified byte-exact for MB3/MB4. */
    if(mb_gate){
        uint8_t t16[16], e[36], pred[64];
        for(int i=0;i<8;++i) t16[i]=t[i];
        for(int i=0;i<8;++i) t16[8+i]=tr[i];
        i8_build_edge(e,l,t16,lt_in,have_tr);
        int pred_mode=2, satd[3];
        for(int m=0;m<3;++m){ i8_pred_edge(pred,m,e); satd[m]=i8_sad(src,pred); }
        int favor=satd[1]>satd[0];
        if(pred_mode<3) satd[pred_mode]-=3*lambda;
        int best=satd[2], bm=2;
        if(satd[1]<best){best=satd[1];bm=1;}
        if(satd[0]<best){best=satd[0];bm=0;}
        static const int8_t SHORT[2][5]={{4,6,8,-1,-1},{3,4,5,7,-1}};
        const int8_t *cands=SHORT[favor];
        if(best>0){
            for(int k=0;cands[k]>=0;++k){
                int m=cands[k];
                i8_pred_edge(pred,m,e);
                int c=i8_sad(src,pred);
                if(pred_mode==(m<=8?m:2)){ c-=3*lambda; if(c<=0){best=c;bm=m;break;} }
                if(c<best){best=c;bm=m;}
            }
        }
        if(cost_out)*cost_out=best+3*lambda;
        return bm;
    }

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
        int cost;
        if(use_satd) cost=vk_satd8(src,pred);
        else { cost=0; for(int i=0;i<64;++i){int d=src[i]-pred[i];cost+=d<0?-d:d;} }
        cost -= (m==pred_mode ? 3*lambda : 0);
        if (cost < best) { best=cost; best_mode=m; }
    }
    if(cost_out) *cost_out=best;
    return best_mode;
}

int vk_i8x8_decide(const uint8_t src[64], const uint8_t l[8],
                   const uint8_t t[8], const uint8_t tr[8], int lt_in,
                   int have_l, int have_t, int lambda, int *cost_out,
                   int use_satd, int mb_gate)
{
    return vk_i8x8_decide_ex(src,l,t,tr,lt_in,have_l,have_t,lambda,cost_out,
                             use_satd,mb_gate,1);
}
