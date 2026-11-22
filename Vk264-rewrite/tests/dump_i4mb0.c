/* Dump vk_encode_i4x4_mb bitstream for the clip's MB0 with x264's known modes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "i4x4enc.h"
#include "bitstream.h"
#include "cavlc.h"
int main(int argc,char**argv){
    if(argc<3)return 2;
    FILE*f=fopen(argv[1],"rb"); if(!f)return 2;
    uint8_t *src=calloc(256+128+128,1);
    if(fread(src,1,256+128+128,f)<256){fclose(f);return 2;} fclose(f);
    static const uint8_t modes[16]={11,9,10,2,9,9,2,2,10,2,10,2,2,2,2,2};
    uint8_t *ry=malloc((size_t)64*128),*ru=malloc(4096),*rv=malloc(4096);
    memset(ry,128,(size_t)64*128);memset(ru,128,4096);memset(rv,128,4096);
    vk_neighbours_t nb;memset(&nb,0,sizeof(nb));
    static uint8_t buf[1<<16]; vk_bs_t bs; vk_cavlc_init();
    memset(buf,0,sizeof(buf));vk_bs_init(&bs,buf,sizeof(buf));
    uint8_t on[16],oc[2][4],od;
    vk_encode_i4x4_mb(&bs,src,src+256,src+384,
        ry+32*64+32,ru+32*64+32,rv+32*64+32,25,&nb,16,8,modes,on,oc,&od,0);
    int n=(int)((vk_bs_pos(&bs)+7)/8);
    FILE*o=fopen(argv[2],"wb"); fwrite(buf,1,n,o); fclose(o);
    printf("emitted %d bits / %d bytes\n",(int)vk_bs_pos(&bs),n);
    printf("luma nnz:");for(int i=0;i<16;++i)printf(" %d",on[i]);printf("\n");
    printf("chroma nnz:");for(int p=0;p<2;++p)for(int i=0;i<4;++i)printf(" %d",oc[p][i]);printf("\n");
    printf("dcnz: %d\n",od);
    free(src);free(ry);free(ru);free(rv);
    return 0;
}
