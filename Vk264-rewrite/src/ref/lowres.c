/* SPDX-License-Identifier: GPL-2.0-or-later
 * x264 lookahead lowres frame cost (encoder/slicetype.c slicetype_frame_cost
 * and slicetype_slice_cost). Half-resolution luma, 8x8 blocks, diamond ME,
 * SATD + lambda*mv cost, plus the b-adapt=fast path cost. */
#include "lowres.h"
#include <string.h>

/* x264 X264_LOOKAHEAD_QP = 32, lambda_tab[32] = 16. */
#define LOOKAHEAD_QP 32
#define LOOKAHEAD_LAMBDA 16

/* Downsample luma by 2 (box filter), x264 frame_init_lowres. */
void vk_lowres_luma(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh)
{
    for (int y=0; y<dh; ++y)
        for (int x=0; x<dw; ++x) {
            const uint8_t *p = src + (2*y)*sw + 2*x;
            dst[y*dw+x] = (uint8_t)((p[0] + p[1] + p[sw] + p[sw+1] + 2) >> 2);
        }
}

/* SATD of an 8x8 lowres block with a displacement. */
static int satd8_off(const uint8_t *a, int aw, int ax, int ay,
                     const uint8_t *b, int bw, int bx, int by)
{
    int t[64], d[64], s=0;
    for(int y=0;y<8;++y) for(int x=0;x<8;++x)
        d[y*8+x]=(int)a[(ay+y)*aw+ax+x]-(int)b[(by+y)*bw+bx+x];
    /* 8x8 Hadamard via two 4x4 stages on rows then cols */
    for(int y=0;y<8;++y){
        int r[8];
        for(int x=0;x<4;++x){int p=d[y*8+x]+d[y*8+x+4],m=d[y*8+x]-d[y*8+x+4];r[x]=p;r[x+4]=m;}
        for(int x=0;x<4;++x){int p=r[x]+r[x+2],m=r[x]-r[x+2];
            t[y*8+x]=p;t[y*8+x+2]=m;
            p=r[x+1]+r[x+3];m=r[x+1]-r[x+3];t[y*8+x+1]=p;t[y*8+x+3]=m;}
        /* second butterfly on the 4-wide halves */
        for(int x=0;x<4;++x){
            int p=t[y*8+x]+t[y*8+x+1],m=t[y*8+x]-t[y*8+x+1];
            d[y*8+x]=p;d[y*8+x+1]=m;
            p=t[y*8+x+2]+t[y*8+x+3];m=t[y*8+x+2]-t[y*8+x+3];
            d[y*8+x+2]=p;d[y*8+x+3]=m;
        }
    }
    for(int x=0;x<8;++x){
        int r[8];
        for(int y=0;y<4;++y){int p=d[y*8+x]+d[(y+4)*8+x],m=d[y*8+x]-d[(y+4)*8+x];r[y]=p;r[y+4]=m;}
        for(int y=0;y<4;++y){int p=r[y]+r[y+2],m=r[y]-r[y+2];
            t[y*8+x]=p;t[(y+2)*8+x]=m;
            p=r[y+1]+r[y+3];m=r[y+1]-r[y+3];t[(y+1)*8+x]=p;t[(y+3)*8+x]=m;}
        for(int y=0;y<4;++y){
            int p=t[y*8+x]+t[(y+1)*8+x],m=t[y*8+x]-t[(y+1)*8+x];
            d[y*8+x]=p;d[(y+1)*8+x]=m;
            p=t[(y+2)*8+x]+t[(y+3)*8+x];m=t[(y+2)*8+x]-t[(y+3)*8+x];
            d[(y+2)*8+x]=p;d[(y+3)*8+x]=m;
        }
    }
    for(int i=0;i<64;++i) s+=d[i]<0?-d[i]:d[i];
    return s>>1;
}

/* Cost of coding `cur` against reference `ref` (P cost) at a given distance.
 * cur/ref are half-res luma planes dw*dh. Diamond ME radius 16 (lowres). */
int vk_lowres_p_cost(const uint8_t *cur, const uint8_t *ref, int dw, int dh)
{
    int cost=0;
    static const int mv0[9][2]={{0,0},{1,0},{-1,0},{0,1},{0,-1},
                                {2,0},{-2,0},{0,2},{0,-2}};
    for(int by=0;by+8<=dh;by+=8) for(int bx=0;bx+8<=dw;bx+=8){
        int bc=satd8_off(cur,dw,bx,by,ref,dw,bx,by);
        int bm=0,bx0=0,by0=0;
        /* coarse 2-step diamond */
        for(int rad=2;rad<=16;rad*=2){
            for(int dy=-rad;dy<=rad;dy+=rad) for(int dx=-rad;dx<=rad;dx+=rad){
                int sx=bx+dx,sy=by+dy;
                if(sx<0||sy<0||sx+8>dw||sy+8>dh) continue;
                int c=satd8_off(cur,dw,bx,by,ref,dw,sx,sy) + LOOKAHEAD_LAMBDA*(abs(dx)+abs(dy));
                if(c<bc){bc=c;bm=LOOKAHEAD_LAMBDA*(abs(dx)+abs(dy));bx0=dx;by0=dy;}
            }
        }
        for(int k=1;k<9;++k){
            int dx=bx0+mv0[k][0],dy=by0+mv0[k][1];
            int sx=bx+dx,sy=by+dy;
            if(sx<0||sy<0||sx+8>dw||sy+8>dh) continue;
            int mcost=LOOKAHEAD_LAMBDA*(abs(dx)+abs(dy));
            int c=satd8_off(cur,dw,bx,by,ref,dw,sx,sy)+mcost;
            if(c<bc){bc=c;bm=mcost;}
        }
        cost+=bc+bm; /* x264 adds mv cost separately into lowres_costs */
    }
    return cost;
}

/* Intra cost: min chroma-like SATD of the block against itself is zero;
 * x264's real i_icost uses 8x8c predictors. Use DC predictor + penalty. */
int vk_lowres_i_cost(const uint8_t *cur, int dw, int dh)
{
    int cost=0;
    for(int by=0;by+8<=dh;by+=8) for(int bx=0;bx+8<=dw;bx+=8){
        int sum=0;
        for(int y=0;y<8;++y) for(int x=0;x<8;++x) sum+=cur[(by+y)*dw+bx+x];
        uint8_t dc=(uint8_t)((sum+32)>>6);
        uint8_t pred[64]; memset(pred,dc,64);
        int satd=satd8_off(cur,dw,bx,by,pred,8,0,0);
        cost+=satd + 5*LOOKAHEAD_LAMBDA;  /* lowres_penalty included in path cost */
    }
    return cost;
}
