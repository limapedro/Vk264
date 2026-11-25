/* Dump Phoenix SPS/PPS NAL bytes as hex for parity comparison.
 * Usage: phx_header_dump <preset> <qp> <cabac|cavlc>
 */
#include "phx_headers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    phx_param_t p;
    phx_param_default(&p);
    if (phx_param_apply_preset(&p, argv[1])) return 2;
    p.i_width = 64; p.i_height = 64;
    p.i_qp_constant = atoi(argv[2]);
    p.b_cabac = strcmp(argv[3], "cabac") == 0;
    phx_param_finalize(&p);
    uint8_t out[512];
    int n = phx_encoder_headers(out, sizeof(out), &p, 64, 64);
    if (n < 0) return 1;
    for (int i = 0; i < n; i++) printf("%02X", out[i]);
    printf("\n");
    return 0;
}
