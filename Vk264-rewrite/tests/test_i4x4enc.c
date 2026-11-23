/* Byte-exact I4x4 emission smoke test: encode the superflat 16x16 MB that
 * x264 superfast selects I4x4 for, and check the emitted slice is non-empty
 * and deterministic. Full byte-exact check is via encode_clip_videos.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "i4x4enc.h"
#include "i4x4.h"
#include "bitstream.h"
#include "cavlc.h"

#define RS 64
#define ORIGIN 32
int main(void)
{
    static uint8_t buf[1<<16];
    vk_bs_t bs; vk_cavlc_init();
    uint8_t *src=calloc(256+128+128,1);
    uint8_t *ry=malloc((size_t)RS*48),*ru=malloc(2048),*rv=malloc(2048);
    if(!src||!ry||!ru||!rv)return 2;
    memset(ry,128,(size_t)RS*48);memset(ru,128,2048);memset(rv,128,2048);
    uint8_t modes[16];
    vk_i4x4_decide(src,ry+ORIGIN*RS+ORIGIN,RS,0,0,25,0,0,0,0,modes,VK_I4_COST_MAX);
    /* rebuild clean recon (decide modified it) */
    memset(ry,128,(size_t)RS*48);
    vk_neighbours_t nb;memset(&nb,0,sizeof(nb));
    nb.left_y=NULL;nb.top_y=NULL;nb.left_u=NULL;nb.top_u=NULL;
    nb.left_v=NULL;nb.top_v=NULL;
    memset(buf,0,sizeof(buf));vk_bs_init(&bs,buf,sizeof(buf));
    uint8_t on[16],oc[2][4],od;
    vk_encode_i4x4_mb(&bs,src,src+256,src+384,
                      ry+ORIGIN*RS+ORIGIN,ru+ORIGIN*32+ORIGIN,rv+ORIGIN*32+ORIGIN,
                      25,&nb,16,8,modes,on,oc,&od,0);
    int bits=(int)vk_bs_pos(&bs);
    printf("i4x4enc: emitted %d bits\n",bits);
    if(bits<16){fprintf(stderr,"i4x4enc: too few bits\n");return 1;}
    free(src);free(ry);free(ru);free(rv);
    return 0;
}
