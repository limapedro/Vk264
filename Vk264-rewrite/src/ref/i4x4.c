/* SPDX-License-Identifier: GPL-2.0-or-later
 * I4x4 intra mode decision, byte-exact port of x264 analyse.c (subme<=1).
 * Algorithm from build/I4X4-ALGORITHM.md (verified 16/16 vs x264 raw dump). */
#include "i4x4.h"
#include "intra16.h"
#include "slice.h"
#include <stdlib.h>
#include <string.h>

/* decision order: block_idx_xy_fenc (x264 macroblock.h:225) */
static const int XY[16][2] = {
    {0,0},{1,0},{0,1},{1,1},{2,0},{3,0},{2,1},{3,1},
    {0,2},{1,2},{0,3},{1,3},{2,2},{3,2},{2,3},{3,3}
};
/* cache position for each decision index (x264 base.h:180) */
static const int SCAN8[16] = {12,13,20,21,14,15,22,23,28,29,36,37,30,31,38,39};
/* candidate mode tables: [avoid_topright][neighbour class] */
static const int MODE_AVAIL[2][5][10] = {
    { {11,-1},{9,1,8,-1},{10,0,3,7,-1},{2,1,0,3,7,8,-1},{2,1,0,3,4,5,6,7,8,-1} },
    { {11,-1},{9,1,8,-1},{10,0,-1},{2,1,0,8,-1},{2,1,0,4,5,6,8,-1} }
};
/* x264 i_neighbour4[] neighbour-class derivation (common/macroblock.h:
 * 1340-1351): bit0=LEFT, bit1=TOP, bit2=TOPLEFT. Within-MB neighbours are
 * always available; only the MB-edge blocks depend on have_l/t/tl. */
static int i4_neighbour_class(int idx,int have_l,int have_t,int have_tl)
{
    int f;
    switch(idx){
    case 0:  f=(have_l?1:0)|(have_t?2:0)|(have_tl?4:0); break;
    case 1: case 4: case 5:  f=1|(have_t?6:0); break;
    case 2: case 8: case 10: f=2|(have_l?5:0); break;
    default: f=7; break;  /* all 4 neighbours lie inside the MB */
    }
    /* predict_4x4_mode_available: all three -> class 4, else drop TOPLEFT */
    if(f==7) return 4;
    return f&3;
}

int vk_i4x4_fix(int m) { return (m >= 0 && m <= 8) ? m : 2; }

static int F2(int a,int b,int c) { return (a + 2*b + c + 2) >> 2; }

/* predictor for raw mode m into out[4][4] */
void vk_predict4(uint8_t out[16], int m, const uint8_t left[4], const uint8_t top[4],
                 uint8_t lt, const uint8_t tr[4])
{
    int l[8], t[8];
    for (int i=0;i<4;++i) { l[i]=left[i]; t[i]=top[i]; }
    for (int i=0;i<4;++i) { l[4+i]=128; t[4+i]=tr[i]; }
    switch (m) {
    case 0: for(int y=0;y<4;++y) for(int x=0;x<4;++x) out[y*4+x]=top[x]; break;
    case 1: for(int y=0;y<4;++y) for(int x=0;x<4;++x) out[y*4+x]=left[y]; break;
    case 2: case 9: case 10: case 11: {
        int v;
        if (m==2) v=(left[0]+left[1]+left[2]+left[3]+top[0]+top[1]+top[2]+top[3]+4)>>3;
        else if (m==9) v=(left[0]+left[1]+left[2]+left[3]+2)>>2;
        else if (m==10) v=(top[0]+top[1]+top[2]+top[3]+2)>>2;
        else v=128;
        for(int i=0;i<16;++i) out[i]=(uint8_t)v; break;
    }
    case 3: /* DDL */
        for(int y=0;y<4;++y) for(int x=0;x<4;++x) {
            int i=x+y;
            out[y*4+x]=(uint8_t)F2(t[i],t[i+1],t[i+2<8?i+2:7]);
        }
        break;
    case 4: /* DDR */
        for(int y=0;y<4;++y) for(int x=0;x<4;++x) {
            int i=3-y+x;
            out[y*4+x]=(uint8_t)F2(t[i],t[i+1],t[i+2]);
        }
        break;
    case 5: /* VR */
        for(int y=0;y<4;++y) for(int x=0;x<4;++x) {
            int i=3-x+y;
            out[y*4+x]=(uint8_t)(i>=0 && i+2<8 ? F2(t[i],t[i+1],t[i+2]) : 128);
        }
        break;
    case 6: /* HD */
        for(int y=0;y<4;++y) for(int x=0;x<4;++x) {
            int i=3-y+x;
            out[y*4+x]=(uint8_t)(i>=0 && i+2<8 ? F2(l[i],l[i+1],l[i+2]) : 128);
        }
        break;
    case 7: /* VL */
        for(int y=0;y<4;++y) for(int x=0;x<4;++x) {
            int i=x+2*y;
            if (i<3) out[y*4+x]=(uint8_t)F2(t[i],t[i+1],t[i+2]);
            else if (i==3) out[y*4+x]=(uint8_t)F2(t[3],lt,lt);
            else if (i==4) out[y*4+x]=(uint8_t)F2(lt,lt,lt);
            else out[y*4+x]=128;
        }
        break;
    case 8: /* HU */
        for(int y=0;y<4;++y) for(int x=0;x<4;++x) {
            int i=x+2*y;
            out[y*4+x]=(uint8_t)(i+2<8 ? F2(l[i],l[i+1],l[i+2]) : 128);
        }
        break;
    }
}

int vk_i4x4_decide(const uint8_t *src_y, uint8_t *recon_y, int rs,
                   int px, int py, int qp, int have_l, int have_t,
                   int have_tl, int have_tr, uint8_t modes_out[16])
{
    int mf[16],bias[16],deq[16];
    vk_ref_quant_tables(qp, 0, mf, bias, deq);
    int lambda = vk_lambda(qp);
    /* x264 analyse.c:867: base mode-bit cost lambda*(24+16), absent from
     * our model until now. */
    int total = lambda * (24 + 16);
    /* x264 analyse.c: cost_i4x4_mode = 3*lambda*(i!=8) */
    int raw[16];
    for (int i=0;i<16;++i) raw[i]=-1;

    /* cache position -> decision index for blocks inside this MB */
    int cache_idx[64];
    for(int i=0;i<64;++i) cache_idx[i]=-1;
    for(int i=0;i<16;++i) cache_idx[SCAN8[i]]=i;

    for (int idx=0; idx<16; ++idx) {
        int bx=XY[idx][0]*4, by=XY[idx][1]*4;
        uint8_t left[4],top[4],tr[4],lt;
        for(int i=0;i<4;++i) {
            left[i] = (bx>0) ? recon_y[(py+by+i)*rs + px+bx-1] : (have_l?recon_y[(py+by+i)*rs+px-1]:128);
            top[i]  = (by>0) ? recon_y[(py+by-1)*rs + px+bx+i] : (have_t?recon_y[(py-1)*rs+px+bx+i]:128);
            tr[i]   = (by>0) ? recon_y[(py+by-1)*rs + px+bx+4+i] : (have_tr?recon_y[(py-1)*rs+px+bx+4+i]:128);
        }
        lt = (bx>0&&by>0) ? recon_y[(py+by-1)*rs+px+bx-1]
           : ((have_tl || (have_l&&have_t)) ? recon_y[(py-1)*rs+px-1] : 128);
        /* predicted mode from fixed modes of cache left/top neighbours */
        int li = cache_idx[SCAN8[idx]-1], ti = cache_idx[SCAN8[idx]-8];
        int ma = (li>=0) ? vk_i4x4_fix(raw[li]) : -1;
        int mb = (ti>=0) ? vk_i4x4_fix(raw[ti]) : -1;
        int pred = ma<mb ? ma : mb;
        if (pred<0) pred=2;
        int cls = i4_neighbour_class(idx,have_l,have_t,have_tl);
        const int *cands = MODE_AVAIL[0][cls];
        int nc=0; while(cands[nc]>=0) nc++;
        int use_x9 = (nc>=9);
        for(int k=0;k<nc;++k) if(cands[k]<0 || cands[k]>8) { use_x9=0; break; }

        uint8_t srcblock[16], p[16];
        /* src_y points at the MB top-left with stride 16 */
        for(int y=0;y<4;++y) for(int x=0;x<4;++x)
            srcblock[y*4+x]=src_y[(by+y)*16 + bx+x];

        int best=0x7fffffff, bestm=2;
        /* x264 superfast has subme=1 -> mbcmp=SAD, mbrd=0, b_fast_intra=0,
         * no x9 SIMD: analyse.c:903-950 uses intra_sad_x3 (DC,H,V) followed
         * by the intra_analysis_shortcut table. Replicate that exactly. */
        const int8_t *loop=cands; int loop_n=nc;
        if (nc>=6) {
            int s3[3];  /* indexed by mode 0=V, 1=H, 2=DC */
            for (int m=0;m<3;++m) {
                vk_predict4(p,m,left,top,lt,tr);
                int sad=0;
                for(int i=0;i<16;++i){int d=srcblock[i]-p[i];sad+=d<0?-d:d;}
                s3[m]=sad;
            }
            if (pred<3) s3[pred]-=3*lambda;
            best=s3[2]; bestm=2;
            if (s3[1]<best){best=s3[1];bestm=1;}
            if (s3[0]<best){best=s3[0];bestm=0;}
            int favor_vertical = s3[1] > s3[0];
            /* intra_analysis_shortcut[avoid_topright=0][cand9][favor] */
            static const int8_t sc[2][2][5] = {
                {{8,-1,-1,-1,-1}, {3,7,-1,-1,-1}},
                {{4,6,8,-1,-1},  {3,4,5,7,-1}}
            };
            loop=sc[use_x9?1:0][favor_vertical];
            loop_n=0; while(loop[loop_n]>=0) loop_n++;
        }
        if (nc<6) {
            /* x264 full candidate loop (no x3 for small candidate sets). */
            for(int k=0;k<nc;++k) {
                int m=cands[k];
                vk_predict4(p,m,left,top,lt,tr);
                int sad=0;
                for(int i=0;i<16;++i){int d=srcblock[i]-p[i];sad+=d<0?-d:d;}
                int c = sad + (vk_i4x4_fix(m)==pred ? -3*lambda : 0);
                if (c<best){best=c;bestm=m;}
            }
        } else if (best>0) {
            for(int k=0;k<loop_n;++k) {
                int m=loop[k];
                vk_predict4(p,m,left,top,lt,tr);
                int sad=0;
                for(int i=0;i<16;++i){int d=srcblock[i]-p[i];sad+=d<0?-d:d;}
                if (vk_i4x4_fix(m)==pred) {
                    sad-=3*lambda;
                    if (sad<=0) { best=sad; bestm=m; break; }
                }
                if (sad<best){best=sad;bestm=m;}
            }
        }
        raw[idx]=bestm; modes_out[idx]=(uint8_t)bestm;
        total += best + 3*lambda;

        /* reconstruction feedback: predict, DCT, quant, dequant, IDCT, add */
        vk_predict4(p,bestm,left,top,lt,tr);
        int d[16]; uint8_t s[16];
        for(int i=0;i<16;++i) s[i]=srcblock[i];
        vk_ref_dct4x4(d, s, p);
        vk_ref_quant4x4(d, mf, bias);
        vk_ref_dequant4(d, deq, qp);
        vk_ref_idct4(d);
        /* write reconstructed block into recon_y (add residual to prediction) */
        for(int y=0;y<4;++y) for(int x=0;x<4;++x) {
            int v=p[y*4+x]+d[y*4+x];
            recon_y[(py+by+y)*rs+px+bx+x]=(uint8_t)(v<0?0:(v>255?255:v));
        }
    }
    return total;
}
