#include <stdio.h>
#include <string.h>
#include "i8x8.h"
int main(void)
{
    uint8_t dst[64];
    int dct[64], mf[64], bias[64], deq[64];
    vk_ref_quant_tables8(25, mf, bias, deq);
    printf("mf0=%d bias0=%d\n", mf[0], bias[0]);

    /* raw DCT DC=64 (x264 MB1 group0), quantize */
    memset(dct, 0, sizeof(dct));
    dct[0] = 64;
    int nz = vk_ref_quant8(dct, mf, bias);
    printf("quant nz=%d q0=%d\n", nz, dct[0]);

    /* dequant + IDCT add onto prediction 15 */
    memset(dst, 15, sizeof(dst));
    vk_ref_dequant8_idct_add(dst, dct, deq, 25);
    printf("recon rows:\n");
    for (int y = 0; y < 8; ++y) {
        printf("  ");
        for (int x = 0; x < 8; ++x) printf("%d ", dst[y*8+x]);
        printf("\n");
    }
    return 0;
}
