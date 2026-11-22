/* Compare vk_i4x4_decide modes against x264 MB0 raw dump for the clip. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "i4x4.h"
int main(int argc,char**argv){
    if(argc<2)return 2;
    FILE*f=fopen(argv[1],"rb"); if(!f)return 2;
    uint8_t *src=calloc(256,1);
    if(fread(src,1,256,f)!=256){fclose(f);return 2;} fclose(f);
    uint8_t *recon=malloc((size_t)64*128); memset(recon,128,(size_t)64*128);
    uint8_t modes[16];
    vk_i4x4_decide(src,recon+32*64+32,64,0,0,25,0,0,0,0,modes);
    static const uint8_t ex[16]={11,9,10,2,9,9,2,2,10,2,10,2,2,2,2,2};
    int bad=0;
    for(int i=0;i<16;++i){printf("%d,",modes[i]);if(modes[i]!=ex[i])bad++;}
    printf("\nbad=%d\n",bad);
    free(src);free(recon);return bad?1:0;
}
