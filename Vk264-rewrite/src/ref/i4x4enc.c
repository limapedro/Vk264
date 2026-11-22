/* SPDX-License-Identifier: GPL-2.0-or-later
 * CAVLC I4x4 macroblock encoder — x264 encoder/macroblock.c::x264_mb_encode_i4x4
 * + encoder/cavlc.c::cavlc_mb_header_i. I-slice, transform_8x8_mode=0. */
#include "i4x4enc.h"
#include "i4x4.h"
#include "intra16.h"
#include "cavlc.h"
#include <string.h>

static const int XY[16][2] = {
    {0,0},{1,0},{0,1},{1,1},{2,0},{3,0},{2,1},{3,1},
    {0,2},{1,2},{0,3},{1,3},{2,2},{3,2},{2,3},{3,3}
};
static const int SCAN8[16] = {12,13,20,21,14,15,22,23,28,29,36,37,30,31,38,39};
static const int IDX2XY[16] = {0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15};
/* x264 cbp_to_golomb[1][1] (4:2:0, intra I-slice). */
static const uint8_t CBP2GOL[16] = {
    3,29,30,17, 31,18,37,8, 32,38,19,9, 20,10,11,2
};
static const uint8_t CT_INDEX[17] = {0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3};

static int clip(int v){return v<0?0:(v>255?255:v);}
#define F2(a,b,c) (((a)+2*(b)+(c)+2)>>2)
static void pred4(int m,const uint8_t l[4],const uint8_t t[4],uint8_t lt,
                  const uint8_t tr[4],uint8_t out[16])
{
    int L[8],T[8];
    for(int i=0;i<4;++i){L[i]=l[i];T[i]=t[i];L[i+4]=128;T[i+4]=tr[i];}
    switch(m){
    case 0: for(int y=0;y<4;++y)for(int x=0;x<4;++x)out[y*4+x]=t[x];break;
    case 1: for(int y=0;y<4;++y)for(int x=0;x<4;++x)out[y*4+x]=l[y];break;
    case 2: case 9: case 10: case 11: {
        int v=m==2?(l[0]+l[1]+l[2]+l[3]+t[0]+t[1]+t[2]+t[3]+4)>>3
            :m==9?(l[0]+l[1]+l[2]+l[3]+2)>>2
            :m==10?(t[0]+t[1]+t[2]+t[3]+2)>>2:128;
        for(int i=0;i<16;++i)out[i]=(uint8_t)v;break;}
    case 3: for(int y=0;y<4;++y)for(int x=0;x<4;++x){int i=x+y;out[y*4+x]=(uint8_t)F2(T[i],T[i+1],T[i+2<8?i+2:7]);}break;
    case 4: for(int y=0;y<4;++y)for(int x=0;x<4;++x){int i=3-y+x;out[y*4+x]=(uint8_t)(i+2<8?F2(T[i],T[i+1],T[i+2]):128);}break;
    case 5: for(int y=0;y<4;++y)for(int x=0;x<4;++x){int i=3-x+y;out[y*4+x]=(uint8_t)(i>=0&&i+2<8?F2(T[i],T[i+1],T[i+2]):128);}break;
    case 6: for(int y=0;y<4;++y)for(int x=0;x<4;++x){int i=3-y+x;out[y*4+x]=(uint8_t)(i>=0&&i+2<8?F2(L[i],L[i+1],L[i+2]):128);}break;
    case 7: for(int y=0;y<4;++y)for(int x=0;x<4;++x){int i=x+2*y;out[y*4+x]=(uint8_t)(i<3?F2(T[i],T[i+1],T[i+2]):i==3?F2(T[3],lt,lt):i==4?lt:128);}break;
    case 8: for(int y=0;y<4;++y)for(int x=0;x<4;++x){int i=x+2*y;out[y*4+x]=(uint8_t)(i+2<8?F2(L[i],L[i+1],L[i+2]):128);}break;
    }
}
#undef F2

void vk_encode_i4x4_mb(vk_bs_t *bs, const uint8_t *src_y,
                       const uint8_t *src_u, const uint8_t *src_v,
                       uint8_t *recon_y, uint8_t *recon_u, uint8_t *recon_v,
                       int qp, const vk_neighbours_t *nb,
                       int stride_y, int stride_c,
                       const uint8_t modes[16],
                       uint8_t out_nnz[16], uint8_t out_cnnz[2][4],
                       uint8_t *out_dcnz, int mb_type_offset)
{
    int mf[16],bias[16],deq[16],cmf[16],cbias[16],cdeq[16];
    vk_ref_quant_tables(qp,0,mf,bias,deq);
    vk_ref_quant_tables(qp,2,cmf,cbias,cdeq);

    /* ---- chroma prediction mode (same as I16) ---- */
    uint8_t t8[8],l8[8],t8v[8],l8v[8],su[64],sv[64],pu[64],pv[64];
    for(int i=0;i<8;++i){
        t8[i]=nb&&nb->top_u?nb->top_u[i]:128;
        l8[i]=nb&&nb->left_u?nb->left_u[i]:128;
        t8v[i]=nb&&nb->top_v?nb->top_v[i]:128;
        l8v[i]=nb&&nb->left_v?nb->left_v[i]:128;
    }
    for(int y=0;y<8;++y)for(int x=0;x<8;++x){
        su[y*8+x]=src_u[y*stride_c+x]; sv[y*8+x]=src_v[y*stride_c+x];
    }
    int cmode=vk_select_chroma(su,sv,t8,l8,t8v,l8v,
        nb?nb->tl_u:128,nb?nb->tl_v:128,nb?nb->avail:0,qp);
    vk_predict_chroma8(pu,t8,l8,nb?nb->tl_u:128,cmode);
    vk_predict_chroma8(pv,t8v,l8v,nb?nb->tl_v:128,cmode);

    /* ---- mb_type = offset+0 (I4x4) + transform8 flag + 16 prediction modes ---- */
    vk_bs_write_ue(bs, (unsigned)mb_type_offset);
    /* x264 cavlc_mb_header_i: PPS b_transform_8x8_mode is on for every preset
     * except ultrafast (which never uses this I4x4 path), so write the
     * b_transform_8x8 flag; an I4x4 MB is always 0. */
    vk_bs_write1(bs,0);
    int cidx[64]; for(int i=0;i<64;++i)cidx[i]=-1;
    for(int i=0;i<16;++i)cidx[SCAN8[i]]=i;
    int dct[16][16],nnz[16]={0};
    uint8_t pred[256];
    /* recon_y is a TIGHT 16x16 block (stride 16) in this oracle path — the
     * caller's stride_y is the source stride and must not be used for fdec
     * feedback reads/writes. */
    const int fds = 16;
    (void)stride_y;
    for(int idx=0;idx<16;++idx){
        int bx=XY[idx][0]*4,by=XY[idx][1]*4;
        uint8_t l[4],t[4],tr[4],lt;
        for(int i=0;i<4;++i){
            l[i]=bx>0?recon_y[(by+i)*fds+bx-1]:(nb&&nb->left_y?nb->left_y[by+i]:128);
            t[i]=by>0?recon_y[(by-1)*fds+bx+i]:(nb&&nb->top_y?nb->top_y[bx+i]:128);
            tr[i]=by>0?recon_y[(by-1)*fds+bx+4+i]:128;
        }
        lt=(bx>0&&by>0)?recon_y[(by-1)*fds+bx-1]:128;
        int li=cidx[SCAN8[idx]-1],ti=cidx[SCAN8[idx]-8];
        int ma=li>=0?vk_i4x4_fix(modes[li]):-1;
        int mb=ti>=0?vk_i4x4_fix(modes[ti]):-1;
        int predm=ma<mb?ma:mb; if(predm<0)predm=2;
        int fixed=vk_i4x4_fix(modes[idx]);
        if(fixed==predm) vk_bs_write1(bs,1);
        else vk_bs_write(bs,4,(unsigned)(fixed-(fixed>predm)));
        uint8_t p[16],s[16];
        pred4(modes[idx],l,t,lt,tr,p);
        for(int y=0;y<4;++y)for(int x=0;x<4;++x)s[y*4+x]=src_y[(by+y)*stride_y+bx+x];
        for(int y=0;y<4;++y)for(int x=0;x<4;++x) pred[by*16+bx+y*4+x]=p[y*4+x];
        vk_ref_dct4x4(dct[idx],s,p);
        nnz[idx]=vk_ref_quant4x4(dct[idx],mf,bias);
        /* x264 x264_mb_encode_i4x4: write prediction to fdec for EVERY block,
         * then add the dequantised residual inline, so later 4x4 blocks in
         * this MB predict from already-reconstructed samples. */
        for(int y=0;y<4;++y) for(int x=0;x<4;++x)
            recon_y[(by+y)*16+bx+x]=p[y*4+x];
        if (nnz[idx]) {
            int blk[16];
            for(int i=0;i<16;++i) blk[i]=dct[idx][i];
            vk_ref_dequant4(blk,deq,qp);
            vk_ref_idct4(blk);
            vk_ref_add_block(recon_y+by*16+bx,16,blk);
        }
    }
    vk_bs_write_ue(bs,(uint32_t)(cmode<=3?cmode:0));

    /* H.264 cbp for I4x4: 4 luma bits (one per 8x8 group), 2 chroma bits.
     * x264's cbp_to_golomb index is luma | chroma<<4. */
    int cbp_luma=0;
    for(int i=0;i<16;++i) if(nnz[i]) {
        int gx=XY[i][0]/2, gy=XY[i][1]/2;
        cbp_luma |= 1 << (gy*2+gx);
    }
    int cbp=cbp_luma;
    vk_bs_write_ue(bs,(unsigned)CBP2GOL[cbp]);
    /* x264 cavlc_qp_delta comes AFTER cbp (and after transform8 flag when
     * 8x8dct is enabled in the PPS; superfast has it off). */
    vk_bs_write_se(bs,0);
    if(out_nnz) memcpy(out_nnz,nnz,16);

    /* ---- luma residuals (VK_DCT_LUMA_4x4, 16 coeffs incl. DC) ---- */
    uint8_t cache[64]={0};
    static const int lpos[4]={11,19,27,35},tpos[4]={4,5,6,7};
    for(int i=0;i<4;++i){
        cache[tpos[i]]=nb&&nb->top_luma?nb->top_luma[i]:0x80;
        cache[lpos[i]]=nb&&nb->left_luma?nb->left_luma[i]:0x80;
    }
    /* x264 cavlc_macroblock_luma_residual writes a coeff_token for EVERY
     * 4x4 block in each coded 8x8 group — including zero blocks (coeff0 token).
     * Skipping them (as this emitter previously did) drops bits and desyncs
     * everything after the first nonzero block. */
    for(int idx=0;idx<16;++idx){
        int b=IDX2XY[idx],p=SCAN8[idx];
        int gx=XY[b][0]/2, gy=XY[b][1]/2;
        if(!(cbp_luma & (1<<(gy*2+gx)))){cache[p]=0;continue;}
        int sum=cache[p-1]+cache[p-8];
        int pr=sum<0x80?(sum+1)>>1:sum&0x7f;
        int nC=CT_INDEX[pr&0x7f];
        int zig[16]={0};
        if(nnz[b]){
            static const int zz[16]={0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15};
            for(int i=0;i<16;++i)zig[i]=dct[b][zz[i]];
        }
        vk_cavlc_block_residual(bs,VK_DCT_LUMA_4x4,zig,nC,1,16);
        cache[p]=(uint8_t)nnz[b];
    }

    /* ---- chroma (same structure as I16 encoder) ---- */
    int cdct[2][4][16],cdc[2][4],cnnz[2][4]={{0}},cacq[2][4][16];
    for(int pl=0;pl<2;++pl){
        const uint8_t *src=pl?src_v:src_u;
        const uint8_t *prd=pl?pv:pu;
        for(int b=0;b<4;++b){
            int by=(b/2)*4,bx=(b%2)*4;
            uint8_t sb[16],pb[16];
            for(int y=0;y<4;++y)for(int x=0;x<4;++x){
                sb[y*4+x]=src[(by+y)*stride_c+bx+x];
                pb[y*4+x]=prd[(by+y)*8+bx+x];
            }
            vk_ref_dct4x4(cdct[pl][b],sb,pb);
            cdc[pl][b]=cdct[pl][b][0]; cdct[pl][b][0]=0;
            cnnz[pl][b]=vk_ref_quant4x4(cdct[pl][b],cmf,cbias);
            for(int i=0;i<16;++i)cacq[pl][b][i]=cdct[pl][b][i];
        }
    }
    int chad[2][4],cbp_chroma=0,cdnz[2]={0};
    for(int pl=0;pl<2;++pl){
        int a=cdc[pl][0]+cdc[pl][1],b2=cdc[pl][2]+cdc[pl][3];
        int c1=cdc[pl][0]-cdc[pl][1],dd=cdc[pl][2]-cdc[pl][3];
        int h[4]={a+b2,a-b2,c1+dd,c1-dd};
        for(int i=0;i<4;++i){chad[pl][i]=vk_ref_quant_one(h[i],cmf[0]>>1,cbias[0]<<1);if(chad[pl][i]){cbp_chroma|=1;cdnz[pl]++;}}
    }
    if(cbp_chroma) for(int pl=0;pl<2;++pl){
        int block[16]={0};for(int i=0;i<4;++i)block[i]=chad[pl][i];
        vk_cavlc_block_residual(bs,VK_DCT_CHROMA_DC,block,4,1,4);
    }
    if(cbp_chroma==2){
        static const uint8_t cscan8[8]={52,53,60,61,92,93,100,101};
        uint8_t cc[128]={0};
        for(int i=0;i<2;++i){
            int tp=cscan8[i]-8,lp=cscan8[i*2]-1;
            cc[tp]=nb&&nb->top_chroma[0]?nb->top_chroma[0][i]:0x80;
            cc[lp]=nb&&nb->left_chroma[0]?nb->left_chroma[0][i]:0x80;
            int tp2=cscan8[4+i]-8,lp2=cscan8[4+i*2]-1;
            cc[tp2]=nb&&nb->top_chroma[1]?nb->top_chroma[1][i]:0x80;
            cc[lp2]=nb&&nb->left_chroma[1]?nb->left_chroma[1][i]:0x80;
        }
        cc[40]=(uint8_t)cdnz[0]; cc[80]=(uint8_t)cdnz[1];
        for(int pl=0;pl<2;++pl)for(int b=0;b<4;++b){
            int k=pl*4+b,pos=cscan8[k];
            int sum=cc[pos-1]+cc[pos-8];
            int pr=sum<0x80?(sum+1)>>1:sum&0x7f;
            int nC=CT_INDEX[pr&0x7f];
            vk_cavlc_block_residual(bs,VK_DCT_CHROMA_AC,cacq[pl][b]+1,nC,1,15);
            cc[pos]=(uint8_t)cnnz[pl][b];
        }
    }
    if(out_cnnz) memcpy(out_cnnz,cnnz,8);
    if(out_dcnz)*out_dcnz=(uint8_t)((cdnz[0]?1:0)|(cdnz[1]?2:0));

    /* ---- reconstruction ---- */
    memcpy(recon_u,pu,64); memcpy(recon_v,pv,64);
    /* luma reconstruction (with DC residual) was added inline above */
    for(int pl=0;pl<2;++pl){
        uint8_t *dst=pl?recon_v:recon_u;
        int d0=chad[pl][0]+chad[pl][1],d1=chad[pl][2]+chad[pl][3];
        int d2=chad[pl][0]-chad[pl][1],d3=chad[pl][2]-chad[pl][3];
        int dmf=cdeq[0]<<(qp/6);
        int cdq[16]={0};
        cdq[0]=((d0+d1)*dmf)>>5;cdq[1]=((d0-d1)*dmf)>>5;
        cdq[2]=((d2+d3)*dmf)>>5;cdq[3]=((d2-d3)*dmf)>>5;
        for(int b=0;b<4;++b){
            int by=(b/2)*4,bx=(b%2)*4;uint8_t *d=dst+by*8+bx;
            if(cnnz[pl][b]){int blk[16];for(int i=0;i<16;++i)blk[i]=cdct[pl][b][i];
                vk_ref_dequant4(blk,cdeq,qp);blk[0]=cdnz[pl]?cdq[b]:0;
                vk_ref_idct4(blk);vk_ref_add_block(d,8,blk);}
            else if(cdnz[pl]) vk_ref_add_block_dc(d,8,cdq[b]);
        }
    }
}
