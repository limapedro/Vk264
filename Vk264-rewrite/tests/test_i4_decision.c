/* Measure x264-style I16-vs-I4 macroblock decision on audit clips. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lowres.h"
#include "i4x4.h"
#include "i8x8decide.h"
#include "intra16.h"
#include "motion.h"
#include "slice.h"

/* x264 satd16 over four 8x8 quadrants (vk_satd8 = 4x 4x4 Hadamard). */
static int satd16(const uint8_t*src,int w,int x,int y,const uint8_t*pred)
{
    int s=0;
    for(int by=0;by<16;by+=8)for(int bx=0;bx<16;bx+=8){
        uint8_t a[64],b[64];
        for(int j=0;j<8;++j)for(int i=0;i<8;++i){
            a[j*8+i]=src[(y+by+j)*w+x+bx+i];
            b[j*8+i]=pred[(by+j)*16+bx+i];
        }
        s+=vk_satd8(a,b);
    }
    return s;
}
int main(int argc,char**argv)
{
    if(argc<2)return 2;
    FILE*f=fopen(argv[1],"rb");if(!f)return 2;
    int w=64,h=64;size_t fs=(size_t)w*h*3/2;
    uint8_t*fr=malloc(fs);fread(fr,fs,1,f);fclose(f);
    uint8_t recon[256], modes[16];
    memset(recon,128,sizeof(recon));
    int qp=25;
    uint8_t src16[256],t[16],l[16],pred[256];
    for(int y=0;y<16;++y)for(int x=0;x<16;++x){
        src16[y*16+x]=fr[y*w+x];t[x]=128;l[y]=128;
    }
    int lambda=vk_lambda(qp);
    int b16satd=0x7fffffff, b16sad=0x7fffffff, bm=2;
    for(int m=0;m<4;++m){
        vk_predict16(pred,t,l,128,m);
        int cs=satd16(fr,w,0,0,pred)+lambda*vk_mode_bits(m);
        int sad=0;
        for(int y=0;y<16;++y)for(int x=0;x<16;++x){
            int d=(int)fr[y*w+x]-pred[y*16+x];sad+=d<0?-d:d;
        }
        sad+=lambda*vk_mode_bits(m);
        if(cs<b16satd)b16satd=cs;
        if(sad<b16sad)b16sad=sad;
    }
    int cost4=vk_i4x4_decide(src16,recon,16,0,0,qp,0,0,0,0,modes);
    /* x264 ultrafast (subme=0) has X264_ANALYSE_I4x4/I8x8 disabled: cost=COST_MAX.
     * Only subme>=1 presets actually consider I4. */
    int subme = argc>2 ? atoi(argv[2]) : 0;
    int want_i4 = (subme>=1) && (cost4 < b16sad);
    printf("I16 satd=%d sad=%d (mode %d) I4=%d lambda=%d subme=%d -> %s\n",
           b16satd,b16sad,bm,cost4,lambda,subme,
           want_i4?"I4":"I16");
    return 0;
}
