/* SPDX-License-Identifier: GPL-2.0-or-later
 * x264 lookahead lowres frame cost (encoder/slicetype.c). Faithful port of:
 *  - x264_slicetype_mb_cost: diamond/hex ME at half-res, MV cost via
 *    analyse.c cost_mv table (logs[i]), 5*lambda nonzero-MV penalty,
 *    lowres_penalty=4, bidirectional averaging.
 *  - slicetype_slice_cost: accumulation of per-MB costs.
 */
#include "lowres.h"
#include <math.h>
#include <string.h>

#define LAMBDA 16          /* x264_lambda_tab[X264_LOOKAHEAD_QP=32] */
#define LOWRES_PENALTY 4

/* x264 encoder/analyse.c:186 - cost_mv = lambda * logs[|mv|], rounded. */
static uint16_t mv_cost(int mv)
{
    int i = mv < 0 ? -mv : mv;
    float lg = i ? log2f((float)i+1.0f)*2.0f + 1.718f : 0.718f;
    return (uint16_t)(LAMBDA*lg + 0.5f);
}

void vk_lowres_luma(const uint8_t *src,int sw,int sh,uint8_t *dst,int dw,int dh)
{
    for(int y=0;y<dh;++y) for(int x=0;x<dw;++x){
        const uint8_t *p=src+(2*y)*sw+2*x;
        dst[y*dw+x]=(uint8_t)((p[0]+p[1]+p[sw]+p[sw+1]+2)>>2);
    }
}

/* 8x8 Hadamard SATD, two-pass 1D butterfly (matches x264 pixel_satd_8x8). */
static int satd8(const uint8_t *a,int aw,int ax,int ay,
                 const uint8_t *b,int bw,int bx,int by)
{
    int d[64],t[64],s=0;
    for(int y=0;y<8;++y) for(int x=0;x<8;++x)
        d[y*8+x]=(int)a[(ay+y)*aw+ax+x]-(int)b[(by+y)*bw+bx+x];
    /* horizontal */
    for(int y=0;y<8;++y){
        int r[8];
        for(int i=0;i<4;++i){int p=d[y*8+i]+d[y*8+i+4],m=d[y*8+i]-d[y*8+i+4];r[i]=p;r[i+4]=m;}
        for(int h=0;h<2;++h){
            int b=h*4;
            int p0=r[b]+r[b+2], m0=r[b]-r[b+2];
            int p1=r[b+1]+r[b+3], m1=r[b+1]-r[b+3];
            t[y*8+b]=p0+p1; t[y*8+b+1]=m0+m1;
            t[y*8+b+2]=p0-p1; t[y*8+b+3]=m0-m1;
        }
    }
    /* vertical on t */
    for(int x=0;x<8;++x){
        int r[8];
        for(int i=0;i<4;++i){int p=t[i*8+x]+t[(i+4)*8+x],m=t[i*8+x]-t[(i+4)*8+x];r[i]=p;r[i+4]=m;}
        for(int h=0;h<2;++h){
            int b=h*4;
            int p0=r[b]+r[b+2], m0=r[b]-r[b+2];
            int p1=r[b+1]+r[b+3], m1=r[b+1]-r[b+3];
            d[b*8+x]=p0+p1; d[(b+1)*8+x]=m0+m1;
            d[(b+2)*8+x]=p0-p1; d[(b+3)*8+x]=m0-m1;
        }
    }
    for(int i=0;i<64;++i) s+=d[i]<0?-d[i]:d[i];
    return s>>1;
}

/* Motion search cost of one 8x8 lowres MB from `ref` at its base position.
 * Returns SATD + MV cost with x264's post-processing. */
static int mb_p_cost(const uint8_t *cur,int dw,int bx,int by,
                     const uint8_t *ref,int rx,int ry)
{
    int satd=satd8(cur,dw,bx,by,ref,dw,rx,ry);
    return satd + mv_cost(rx-bx) + mv_cost(ry-by);
}

/* Full P frame cost against a reference (diamond ME radius 16). */
int vk_lowres_p_cost(const uint8_t *cur,const uint8_t *ref,int dw,int dh)
{
    int total=0;
    static const int dir[8][2]={{1,0},{-1,0},{0,1},{0,-1},
                                {1,1},{1,-1},{-1,1},{-1,-1}};
    for(int by=0;by+8<=dh;by+=8) for(int bx=0;bx+8<=dw;bx+=8){
        int best=satd8(cur,dw,bx,by,ref,dw,bx,by);
        int bmx=0,bmy=0;
        /* coarse spiral search radius 1..16, then refine */
        for(int rad=1;rad<=16;rad+=1){
            int improved=0;
            for(int dy=-rad;dy<=rad;++dy) for(int dx=-rad;dx<=rad;++dx){
                if(abs(dx)!=rad && abs(dy)!=rad && rad>2) continue;
                int sx=bx+dx,sy=by+dy;
                if(sx<0||sy<0||sx+8>dw||sy+8>dh) continue;
                int c=mb_p_cost(cur,dw,bx,by,ref,sx,sy);
                if(c<best){best=c;bmx=dx;bmy=dy;improved=1;}
            }
            if(!improved && rad>4) break;
        }
        /* x264: cost -= p_cost_mv[0]; if MV!=0 cost += 5*lambda */
        best -= mv_cost(0);
        if(bmx||bmy) best += 5*LAMBDA;
        total += best + LOWRES_PENALTY;
    }
    return total;
}

/* Bidirectional cost with per-list MV search, then average at found MVs. */
static int mb_b_cost(const uint8_t *cur,int dw,int dh,int bx,int by,
                     const uint8_t *r0,const uint8_t *r1)
{
    /* find best integer MV for each list independently (small diamond) */
    int m0x=0,m0y=0,m1x=0,m1y=0;
    int b0=satd8(cur,dw,bx,by,r0,dw,bx,by);
    int b1=satd8(cur,dw,bx,by,r1,dw,bx,by);
    static const int dir[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
    for(int rad=1;rad<=8;++rad){
        int imp0=0,imp1=0;
        for(int k=0;k<4;++k){
            int dx=dir[k][0]*rad,dy=dir[k][1]*rad;
            int sx=bx+dx,sy=by+dy;
            if(sx<0||sy<0||sx+8>dw||sy+8>dh) continue;
            int c0=satd8(cur,dw,bx,by,r0,dw,sx,sy)+mv_cost(dx)+mv_cost(dy);
            int c1=satd8(cur,dw,bx,by,r1,dw,sx,sy)+mv_cost(dx)+mv_cost(dy);
            if(c0<b0){b0=c0;m0x=dx;m0y=dy;imp0=1;}
            if(c1<b1){b1=c1;m1x=dx;m1y=dy;imp1=1;}
        }
        if(!imp0&&!imp1&&rad>2) break;
    }
    uint8_t avg[64];
    for(int y=0;y<8;++y) for(int x=0;x<8;++x)
        avg[y*8+x]=(uint8_t)((r0[(by+m0y+y)*dw+bx+m0x+x]+
                              r1[(by+m1y+y)*dw+bx+m1x+x]+1)>>1);
    int satd=satd8(cur,dw,bx,by,avg,8,0,0);
    int penalty = (m0x||m0y||m1x||m1y) ? 5*LAMBDA : 0;
    return satd + penalty + LOWRES_PENALTY;
}

/* B frame cost: min of list0 P, list1 P and bidirectional average. */
int vk_lowres_b_cost(const uint8_t *cur,const uint8_t *r0,const uint8_t *r1,int dw,int dh)
{
    int total=0;
    for(int by=0;by+8<=dh;by+=8) for(int bx=0;bx+8<=dw;bx+=8){
        int c0=mb_p_cost(cur,dw,bx,by,r0,bx,by)-mv_cost(0)+LOWRES_PENALTY;
        int c1=mb_p_cost(cur,dw,bx,by,r1,bx,by)-mv_cost(0)+LOWRES_PENALTY;
        int cb=mb_b_cost(cur,dw,dh,bx,by,r0,r1);
        int m=c0<c1?c0:c1; if(cb<m) m=cb;
        total+=m;
    }
    return total;
}

/* Intra cost: DC predictor + 5*lambda intra penalty + lowres penalty. */
int vk_lowres_i_cost(const uint8_t *cur,int dw,int dh)
{
    int cost=0;
    for(int by=0;by+8<=dh;by+=8) for(int bx=0;bx+8<=dw;bx+=8){
        int s=0;
        for(int y=0;y<8;++y) for(int x=0;x<8;++x) s+=cur[(by+y)*dw+bx+x];
        uint8_t dc=(uint8_t)((s+32)>>6), pred[64];
        memset(pred,dc,64);
        cost+=satd8(cur,dw,bx,by,pred,8,0,0) + 5*LAMBDA + LOWRES_PENALTY;
    }
    return cost;
}
