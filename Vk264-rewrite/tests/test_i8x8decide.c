#include <stdio.h>
#include <string.h>
#include "i8x8decide.h"

int main(void)
{
    /* Fixture: flat block with both neighbours available -> DC (2). */
    uint8_t src[64], l[8], t[8], tr[8];
    memset(src,50,64); memset(l,50,8); memset(t,50,8); memset(tr,50,8);
    int m=vk_i8x8_decide(src,l,t,tr,1,1,1);
    uint8_t pr[64]; vk_predict8(pr,m,l,t,tr);
    if (vk_satd8(src,pr)!=0) { fprintf(stderr,"FAIL flat: mode %d\n",m); return 1; }
    /* Vertical gradient: top row 0..63, source matches top -> V (0). */
    for(int x=0;x<8;++x){t[x]=x*8;l[x]=x*8;tr[x]=x*8;}
    for(int y=0;y<8;++y) for(int x=0;x<8;++x) src[y*8+x]=x*8;
    m=vk_i8x8_decide(src,l,t,tr,1,1,1);
    if (m!=0) { fprintf(stderr,"FAIL vertical: got %d\n",m); return 1; }
    /* Left-edge block (no top) -> DC_LEFT (9) for flat source. */
    memset(src,60,64); memset(l,60,8); memset(t,128,8); memset(tr,128,8);
    m=vk_i8x8_decide(src,l,t,tr,1,0,1);
    if (m!=9) { fprintf(stderr,"FAIL DC_LEFT: got %d\n",m); return 1; }
    /* No neighbours -> DC_128 (11) for flat source 128. */
    memset(src,128,64); memset(l,128,8); memset(t,128,8); memset(tr,128,8);
    m=vk_i8x8_decide(src,l,t,tr,0,0,1);
    if (m!=11) { fprintf(stderr,"FAIL DC_128: got %d\n",m); return 1; }
    printf("i8x8decide fixture PASS (DC/V/DC_LEFT/DC_128)\n");
    return 0;
}
