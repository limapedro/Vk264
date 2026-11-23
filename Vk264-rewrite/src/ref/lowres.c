/* SPDX-License-Identifier: GPL-2.0-or-later
 * x264 lookahead lowres frame cost (encoder/slicetype.c). Faithful port of:
 *  - x264_slicetype_mb_cost: diamond/hex ME at half-res, MV cost via
 *    analyse.c cost_mv table (logs[i]), 5*lambda nonzero-MV penalty,
 *    lowres_penalty=4, bidirectional averaging.
 *  - slicetype_slice_cost: accumulation of per-MB costs.
 */
#include "lowres.h"
#include "mbtree.h"
#include <math.h>
#include <string.h>

#define LAMBDA 1           /* x264_lambda_tab[X264_LOOKAHEAD_QP=12] = 1 */
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
    /* x264 common/mc.c frame_init_lowres_core:484-506: generate all 4 planes.
     * FILTER(a,b,c,d) = (((a+b+1)>>1)+((c+d+1)>>1)+1)>>1. x264 duplicates the
     * last row/column of `plane[0]` before filtering so the right/bottom edges
     * don't special-case; we clamp to the last valid sample instead. */
#define LF(a,b,c,d) ((((a)+(b)+1)>>1)+(((c)+(d)+1)>>1)+1)>>1
#define PX(x,y) src[((y)<sh?(y):sh-1)*sw+((x)<sw?(x):sw-1)]
    uint8_t *p0=dst, *p1=LR_PLANE(dst,dw,dh,1),
            *p2=LR_PLANE(dst,dw,dh,2), *p3=LR_PLANE(dst,dw,dh,3);
    for(int y=0;y<dh;++y) for(int x=0;x<dw;++x){
        int sx=2*x, sy=2*y;
        p0[y*dw+x]=(uint8_t)(LF(PX(sx,sy),PX(sx,sy+1),PX(sx+1,sy),PX(sx+1,sy+1)));
        p1[y*dw+x]=(uint8_t)(LF(PX(sx+1,sy),PX(sx+1,sy+1),PX(sx+2,sy),PX(sx+2,sy+1)));
        p2[y*dw+x]=(uint8_t)(LF(PX(sx,sy+1),PX(sx,sy+2),PX(sx+1,sy+1),PX(sx+1,sy+2)));
        p3[y*dw+x]=(uint8_t)(LF(PX(sx+1,sy+1),PX(sx+1,sy+2),PX(sx+2,sy+1),PX(sx+2,sy+2)));
    }
#undef PX
#undef LF
}

/* SAD of an 8x8 block against a reference plane at a halfpel MV.
 * hx,hy in {0,1} select the shifted plane; (fx,fy) is the fullpel offset. */
static int sad_hpel(const uint8_t *cur,int dw,int bx,int by,
                    const uint8_t *refbase,int rw,int rh,
                    int fx,int fy,int hx,int hy)
{
    const uint8_t *ref=LR_PLANE(refbase,rw,rh,hx+2*hy);
    int s=0;
    for(int y=0;y<8;++y) for(int x=0;x<8;++x){
        int rx=bx+fx+x, ry=by+fy+y;
        if(rx<0||ry<0||rx>=rw||ry>=rh) return 0x7fffffff;
        int d=(int)cur[(by+y)*dw+bx+x]-(int)ref[ry*rw+rx];
        s+=d<0?-d:d;
    }
    return s;
}

/* x264 refine_subpel: pick the best halfpel offset at a fullpel MV. MVs are
 * stored in halfpel units: mv = 2*fullpel + hpel_bit, so plane index for
 * TRY_BIDIR is (mvx&1) + 2*(mvy&1) and fullpel offset is (mv>>1). */
static int hpel_best_offset(const uint8_t *cur,int dw,int bx,int by,
                            const uint8_t *refbase,int rw,int rh,int fx,int fy,
                            int *bhx,int *bhy)
{
    int best=0x7fffffff;
    *bhx=*bhy=0;
    for(int hy=0;hy<2;++hy) for(int hx=0;hx<2;++hx){
        int s=sad_hpel(cur,dw,bx,by,refbase,rw,rh,fx,fy,hx,hy);
        if(s<best){best=s;*bhx=hx;*bhy=hy;}
    }
    return best;
}

/* Bidirectional prediction cost with per-list hpel-shifted planes selected by
 * the stored MVs' halfpel bits (x264 TRY_BIDIR / hpel_idx). */
static int bipred_cost(const uint8_t *cur,int dw,int bx,int by,
                       const uint8_t *r0,const uint8_t *r1,int rw,int rh,
                       int mv0x,int mv0y,int mv1x,int mv1y)
{
    const uint8_t *p0=LR_PLANE(r0,rw,rh,(mv0x&1)+2*(mv0y&1));
    const uint8_t *p1=LR_PLANE(r1,rw,rh,(mv1x&1)+2*(mv1y&1));
    int fx0=mv0x>>1, fy0=mv0y>>1, fx1=mv1x>>1, fy1=mv1y>>1;
    int s=0;
    for(int y=0;y<8;++y) for(int x=0;x<8;++x){
        int ax=bx+fx0+x, ay=by+fy0+y;
        int cx=bx+fx1+x, cy=by+fy1+y;
        if(ax<0||ay<0||ax>=rw||ay>=rh||cx<0||cy<0||cx>=rw||cy>=rh) return 0x7fffffff;
        int avg=(p0[ay*rw+ax]+p1[cy*rw+cx]+1)>>1;
        int d=cur[(by+y)*dw+bx+x]-avg;
        s+=d<0?-d:d;
    }
    return s;
}

void vk_lowres_mv_init(vk_lowres_mv_t *st,int dw,int dh)
{
    st->n_mbs = (dw/8)*(dh/8);
    memset(st->mv,0,sizeof(st->mv));
}

/* x264 mbcmp_init (encoder/encoder.c:1411): for subpel_refine<=1, mbcmp is
 * SAD (not SATD). superfast uses i_subpel_refine=1, so lowres costs are SAD. */
static int satd8(const uint8_t *a,int aw,int ax,int ay,
                 const uint8_t *b,int bw,int bx,int by)
{
    int s=0;
    for(int y=0;y<8;++y) for(int x=0;x<8;++x){
        int d=(int)a[(ay+y)*aw+ax+x]-(int)b[(by+y)*bw+bx+x];
        s+=d<0?-d:d;
    }
    return s;
}

/* x264 me.c:322 X264_ME_DIA: radius-1 diamond, step toward the best neighbour
 * until no improvement. Search cost is SAD + BITS_MVD (mv_cost). Seeded by the
 * neighbour predictor (x264 me.c:313 bmx=m->mvp). */
static void dia_search(const uint8_t *cur,int dw,int bx,int by,
                       const uint8_t *ref,int rw,int rh,int *pmx,int *pmy)
{
    int bmx=*pmx, bmy=*pmy;
    int maxx=rw-8-bx, maxy=rh-8-by;
    if(bmx<-bx)bmx=-bx; if(bmx>maxx)bmx=maxx;
    if(bmy<-by)bmy=-by; if(bmy>maxy)bmy=maxy;
    int bcost=satd8(cur,dw,bx,by,ref,dw,bx+bmx,by+bmy)+mv_cost(bmx)+mv_cost(bmy);
    /* x264 me.c:316-317: if(pmv) COST_MV(0,0) */
    if(bmx||bmy){
        int c=satd8(cur,dw,bx,by,ref,dw,bx,by)+2*mv_cost(0);
        if(c<bcost){bcost=c;bmx=0;bmy=0;}
    }
    static const int dd[4][2]={{0,-1},{0,1},{-1,0},{1,0}};
    for(int it=0;it<64;++it){
        int bestk=-1;
        for(int k=0;k<4;++k){
            int nx=bmx+dd[k][0], ny=bmy+dd[k][1];
            if(nx<-bx||ny<-by||nx>maxx||ny>maxy) continue;
            int c=satd8(cur,dw,bx,by,ref,dw,bx+nx,by+ny)+mv_cost(nx)+mv_cost(ny);
            if(c<bcost){bcost=c;bestk=k;}
        }
        if(bestk<0) break;
        bmx+=dd[bestk][0]; bmy+=dd[bestk][1];
    }
    *pmx=bmx; *pmy=bmy;
}

static int median3(int a,int b,int c)
{
    int t;
    if(a>b){t=a;a=b;b=t;}
    if(b>c){t=b;b=c;c=t;}
    if(a>b){t=a;a=b;b=t;}
    return b;
}

/* Full P frame cost: x264 reverse-order per-MB ME with neighbour-mv predictor,
 * DIA search, 1 hpel iteration, and interior-MB gating. */
int vk_lowres_p_cost(const uint8_t *cur,const uint8_t *ref,int dw,int dh,
                     vk_lowres_mv_t *st)
{
    return vk_lowres_p_cost_mb(cur,ref,dw,dh,st,NULL);
}

int vk_lowres_p_cost_mb(const uint8_t *cur,const uint8_t *ref,int dw,int dh,
                        vk_lowres_mv_t *st,uint16_t *costs_out)
{
    int nbx=dw/8, nby=dh/8, total=0;
    int16_t mvs[VK_LOWRES_MAX_MBS][2];
    memset(mvs,0,sizeof(mvs));
    for(int my=nby-1;my>=0;--my) for(int mx=nbx-1;mx>=0;--mx){
        int mb=my*nbx+mx, bx=mx*8, by=my*8;
        /* reverse-order neighbour MV prediction (x264 slicetype.c:662-680) */
        int mc[4][2],n=0;
        if(mx<nbx-1){mc[n][0]=mvs[mb+1][0];mc[n][1]=mvs[mb+1][1];n++;}
        if(my<nby-1){
            mc[n][0]=mvs[mb+nbx][0];mc[n][1]=mvs[mb+nbx][1];n++;
            if(mx>0){mc[n][0]=mvs[mb+nbx-1][0];mc[n][1]=mvs[mb+nbx-1][1];n++;}
            if(mx<nbx-1){mc[n][0]=mvs[mb+nbx+1][0];mc[n][1]=mvs[mb+nbx+1][1];n++;}
        }
        int px=0,py=0;
        if(n==1){px=mc[0][0];py=mc[0][1];}
        else if(n>=2){px=median3(mc[0][0],mc[1][0],mc[2][0]);py=median3(mc[0][1],mc[1][1],mc[2][1]);}
        int mxv=0,myv=0;
        if(px==0&&py==0&&satd8(cur,dw,bx,by,ref,dw,bx,by)<64)
            ; /* x264 skip: mv=0, cost=SAD0 */
        else{
            mxv=px; myv=py;
            dia_search(cur,dw,bx,by,ref,dw,dh,&mxv,&myv);
        }
        int hx=0,hy=0;
        int hbest=hpel_best_offset(cur,dw,bx,by,ref,dw,dh,mxv,myv,&hx,&hy);
        if(hbest>=0x7fffffff) hbest=satd8(cur,dw,bx,by,ref,dw,bx+mxv,by+myv);
        hbest -= mv_cost(0);
        if(mxv||myv) hbest += 5*LAMBDA;
        if(st && mb<VK_LOWRES_MAX_MBS){
            st->mv[mb][0]=2*mxv+hx; st->mv[mb][1]=2*myv+hy;
        }
        mvs[mb][0]=mxv; mvs[mb][1]=myv;
        if(nbx>2&&nby>2&&mx>0&&mx<nbx-1&&my>0&&my<nby-1)
            total += hbest + LOWRES_PENALTY;
        if(costs_out && mb<VK_LOWRES_MAX_MBS){
            int c=hbest+LOWRES_PENALTY;
            costs_out[mb]=(uint16_t)((1<<VK_LOWRES_COST_SHIFT)|
                                     (c&VK_LOWRES_COST_MASK));
        }
    }
    return total;
}

/* x264 me.c:322 X264_ME_DIA - radius-1 diamond stepping from the predictor. */
static int me_search_pred(const uint8_t *cur,int dw,int bx,int by,
                          const uint8_t *ref,int rh,int mvpx,int mvpy,
                          int *bmx,int *bmy)
{
    int maxx=dw-8-bx, maxy=rh-8-by;
    int cx = mvpx<-bx ? -bx : (mvpx>maxx?maxx:mvpx);
    int cy = mvpy<-by ? -by : (mvpy>maxy?maxy:mvpy);
    int best=satd8(cur,dw,bx,by,ref,dw,bx+cx,by+cy);
    static const int dd[4][2]={{0,-1},{0,1},{-1,0},{1,0}};
    for(int it=0;it<24;++it){
        int bi=-1,bv=best;
        for(int k=0;k<4;++k){
            int sx=cx+dd[k][0], sy=cy+dd[k][1];
            if(bx+sx<0||by+sy<0||bx+sx>dw-8||by+sy>rh-8) continue;
            int c=satd8(cur,dw,bx,by,ref,dw,bx+sx,by+sy);
            if(c<bv){bv=c;bi=k;}
        }
        if(bi<0) break;
        cx+=dd[bi][0]; cy+=dd[bi][1]; best=bv;
    }
    *bmx=cx; *bmy=cy;
    return best;
}

/* B frame cost with x264's reverse-order per-MB ME and neighbour MV prediction
 * (slicetype.c:654-709), penalty-0 dmv/zero-MV bipred candidates, and
 * penalty-5*lambda searched bipred. */
int vk_lowres_b_cost(const uint8_t *cur,const uint8_t *r0,const uint8_t *r1,
                     int dw,int dh,const vk_lowres_mv_t *st_p0,
                     const vk_lowres_mv_t *st_p1,int p0,int p1,int b)
{
    int nbx=dw/8, nby=dh/8, total=0;
    int dsf=(p1!=p0)?(((b-p0)<<8)+((p1-p0)>>1))/(p1-p0):128;
    int16_t mv[2][VK_LOWRES_MAX_MBS][2];
    memset(mv,0,sizeof(mv));
    /* reverse scan: high x/y first so right/bottom neighbours are available */
    for(int my=nby-1;my>=0;--my) for(int mx=nbx-1;mx>=0;--mx){
        int mb=my*nbx+mx, bx=mx*8, by=my*8;
        for(int l=0;l<2;++l){
            int mc[4][2],n=0;
            if(mx<nbx-1){mc[n][0]=mv[l][mb+1][0];mc[n][1]=mv[l][mb+1][1];n++;}
            if(my<nby-1){
                mc[n][0]=mv[l][mb+nbx][0];mc[n][1]=mv[l][mb+nbx][1];n++;
                if(mx>0){mc[n][0]=mv[l][mb+nbx-1][0];mc[n][1]=mv[l][mb+nbx-1][1];n++;}
                if(mx<nbx-1){mc[n][0]=mv[l][mb+nbx+1][0];mc[n][1]=mv[l][mb+nbx+1][1];n++;}
            }
            int px=0,py=0;
            if(n==1){px=mc[0][0];py=mc[0][1];}
            else if(n>=2){px=median3(mc[0][0],mc[1][0],mc[2][0]);py=median3(mc[0][1],mc[1][1],mc[2][1]);}
            const uint8_t *ref=l?r1:r0;
            int mxv=0,myv=0;
            if(px==0&&py==0&&satd8(cur,dw,bx,by,ref,dw,bx,by)<64)
                ; /* x264 skip: mv=0, cost=SAD0 */
            else{
                me_search_pred(cur,dw,bx,by,ref,dh,px>>1,py>>1,&mxv,&myv);
                int hx=0,hy=0;
                hpel_best_offset(cur,dw,bx,by,ref,dw,dh,mxv,myv,&hx,&hy);
                mxv=2*mxv+hx; myv=2*myv+hy;  /* store halfpel bits */
            }
            mv[l][mb][0]=mxv; mv[l][mb][1]=myv;
        }
        /* list0 / list1 single-prediction costs at the MVs' own hpel plane */
        int c0=sad_hpel(cur,dw,bx,by,r0,dw,dh,mv[0][mb][0]>>1,mv[0][mb][1]>>1,
                        mv[0][mb][0]&1,mv[0][mb][1]&1)+LOWRES_PENALTY;
        int c1=sad_hpel(cur,dw,bx,by,r1,dw,dh,mv[1][mb][0]>>1,mv[1][mb][1]>>1,
                        mv[1][mb][0]&1,mv[1][mb][1]&1)+LOWRES_PENALTY;
        if(mv[0][mb][0]||mv[0][mb][1]) c0+=5*LAMBDA;
        if(mv[1][mb][0]||mv[1][mb][1]) c1+=5*LAMBDA;
        /* zero-MV bipred (penalty 0): hpel_idx=0 reads the base planes. */
        int m=bipred_cost(cur,dw,bx,by,r0,r1,dw,dh,0,0,0,0)+LOWRES_PENALTY;
        if(c0<m)m=c0; if(c1<m)m=c1;
        /* dmv-predicted bipred from future ref (penalty 0) */
        if(st_p1 && mb<st_p1->n_mbs){
            int mvr0=st_p1->mv[mb][0], mvr1=st_p1->mv[mb][1];
            int dmx=(mvr0*dsf+64)>>8, dmy=(mvr1*dsf+64)>>8;
            int dmx2=dmx-mvr0, dmy2=dmy-mvr1;
            int cd=bipred_cost(cur,dw,bx,by,r0,r1,dw,dh,dmx,dmy,dmx2,dmy2);
            if(cd<0x7fffffff){cd+=LOWRES_PENALTY; if(cd<m)m=cd;}
        }
        /* searched bipred (penalty 5*lambda) at the MVs' own hpel planes */
        if(mv[0][mb][0]||mv[0][mb][1]||mv[1][mb][0]||mv[1][mb][1]){
            int cs=bipred_cost(cur,dw,bx,by,r0,r1,dw,dh,
                        mv[0][mb][0],mv[0][mb][1],mv[1][mb][0],mv[1][mb][1]);
            if(cs<0x7fffffff){cs+=5*LAMBDA+LOWRES_PENALTY; if(cs<m)m=cs;}
        }
        if(nbx<=2||nby<=2||(mx>0&&mx<nbx-1&&my>0&&my<nby-1)) total+=m;
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

/* Exact x264 lowres ME component (candidate-seeded DIA + hpel). */
int vk_lowres_me_exact(const uint8_t fenc[64],const uint8_t *ref,int rw,int rh,
                       int bx,int by,int cand[4][2],int ncand,int mvp[2],
                       int mvspel[2])
{
    int maxx=rw-8-bx, maxy=rh-8-by;
    /* me.c:291: bcost at rounded median MVP is plain SAD. */
    int bmx=mvp[0], bmy=mvp[1];
    if(bmx<-bx)bmx=-bx; if(bmx>maxx)bmx=maxx;
    if(bmy<-by)bmy=-by; if(bmy>maxy)bmy=maxy;
    int bcost=0x7fffffff;
    /* fenc is an 8x8 block with stride 8; ref plane 0 has stride rw. */
#define FSAD(mx,my) sad_hpel(fenc,8,0,0,ref,rw,rh,bx+(mx),by+(my),0,0)
    bcost=FSAD(bmx,bmy);
    /* me.c:293-313: evaluate all FPEL-rounded candidates with BITS_MVD. */
    for(int i=0;i<ncand;++i){
        int nx=cand[i][0], ny=cand[i][1];
        if(nx<-bx||ny<-by||nx>maxx||ny>maxy) continue;
        int c=FSAD(nx,ny)+mv_cost(nx*4)+mv_cost(ny*4);
        if(c<bcost){bcost=c;bmx=nx;bmy=ny;}
    }
    static const int dd[4][2]={{0,-1},{0,1},{-1,0},{1,0}};
    for(int it=0;it<16;++it){
        int bestk=-1;
        for(int k=0;k<4;++k){
            int nx=bmx+dd[k][0], ny=bmy+dd[k][1];
            if(nx<-bx||ny<-by||nx>maxx||ny>maxy) continue;
            int c=FSAD(nx,ny)+mv_cost(nx*4)+mv_cost(ny*4);
            if(c<bcost){bcost=c;bestk=k;}
        }
        if(bestk<0) break;
        bmx+=dd[bestk][0]; bmy+=dd[bestk][1];
    }
    /* refine_subpel hpel: planes 1/2 at +-1 fullpel positions (me.c:903-911). */
    int best=FSAD(bmx,bmy);
    int fx=bmx,fy=bmy,hx=0,hy=0;
    const int p[4][2]={{bmx,bmy-1},{bmx,bmy+1},{bmx-1,bmy},{bmx+1,bmy}};
    const int pl[4]={2,2,1,1};
    for(int i=0;i<4;++i){
        const uint8_t *refp=LR_PLANE(ref,rw,rh,pl[i]);
        int rx=bx+p[i][0], ry=by+p[i][1];
        if(rx<0||ry<0||rx+8>rw||ry+8>rh) continue;
        int s=0;
        for(int y=0;y<8;++y)for(int x=0;x<8;++x){
            int d=fenc[y*8+x]-refp[(ry+y)*rw+rx+x];s+=d<0?-d:d;
        }
        if(s<best){best=s;fx=p[i][0];fy=p[i][1];hx=pl[i]&1;hy=pl[i]>>1;}
    }
#undef FSAD
    mvspel[0]=4*fx+2*hx; mvspel[1]=4*fy+2*hy;
    return best;
}

void vk_lowres_intra_costs(const uint8_t *cur,int dw,int dh,uint16_t *out)
{
    int mb=0;
    for(int by=0;by+8<=dh;by+=8) for(int bx=0;bx+8<=dw;bx+=8){
        int s=0;
        for(int y=0;y<8;++y) for(int x=0;x<8;++x) s+=cur[(by+y)*dw+bx+x];
        uint8_t dc=(uint8_t)((s+32)>>6), pred[64];
        memset(pred,dc,64);
        int c=satd8(cur,dw,bx,by,pred,8,0,0)+5*LAMBDA+LOWRES_PENALTY;
        out[mb++]=(uint16_t)(c<65535?c:65535);
    }
}
