#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lowres.h"
int main(int argc,char**argv){
    const char*path=argc>1?argv[1]:"build/long64.yuv";
    int w=64,h=64,dw=w/2,dh=h/2,n=6;
    size_t fs=(size_t)w*h*3/2;
    FILE*f=fopen(path,"rb"); if(!f){perror("open");return 2;}
    uint8_t*raw=malloc(fs*n); fread(raw,fs,n,f); fclose(f);
    uint8_t**lo=malloc(n*sizeof(*lo));
    for(int i=0;i<n;++i){lo[i]=malloc(dw*dh);
        vk_lowres_luma(raw+i*fs,w,h,lo[i],dw,dh);}
    for(int i=1;i<n-1;++i){
        int p0=vk_lowres_p_cost(lo[i],lo[i-1],dw,dh);
        int p2=vk_lowres_p_cost(lo[i],lo[i+1],dw,dh);
        int b=vk_lowres_b_cost(lo[i],lo[i-1],lo[i+1],dw,dh);
        printf("frame %d: P(prev)=%d P(next)=%d B=%d\n",i,p0,p2,b);
    }
    return 0;
}
