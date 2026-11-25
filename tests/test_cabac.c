/* Golden bytes produced by the authoritative x264 C implementation:
 * x264_cabac_encode_init_core + state[]=0x40 + decisions
 * {1,0,1,1,0,0,1,0,1,0,0,1,1,1,0,0} on ctx 3..9 + bypass(0), bypass(1),
 * terminal -> bytes FC 64 25. Harness: common_cabac.obj from
 * reference/x264 (built by tools/build_reference_x264.py).
 */
#include "phx_cabac.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond,msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n",msg,__LINE__); ++fails; } } while (0)

int main(void)
{
    phx_cabac_t cb;
    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));
    phx_cabac_encode_init(&cb, buf, buf+sizeof(buf));
    CHECK(cb.i_low == 0 && cb.i_range == 0x01FE && cb.i_queue == -9,
          "encode_init_core");

    for (int i = 0; i < 1024; i++) cb.state[i] = 0x40;
    int seq[] = {1,0,1,1,0,0,1,0,1,0,0,1,1,1,0,0};
    for (int i = 0; i < 16; i++)
        phx_cabac_encode_decision(&cb, (i%7)+3, seq[i]);
    phx_cabac_encode_bypass(&cb, 0);
    phx_cabac_encode_bypass(&cb, 1);
    phx_cabac_encode_terminal(&cb);

    int n = (int)(cb.p - buf);
    CHECK(n == 3, "byte count vs x264");
    CHECK(n >= 3 && buf[0] == 0xFC && buf[1] == 0x64 && buf[2] == 0x25,
          "bytes vs x264 golden");

    /* Context initialisation must be deterministic and within state bounds. */
    memset(&cb, 0, sizeof(cb));
    phx_cabac_context_init(&cb, PHX_SLICE_P, 28, 0);
    for (int i = 0; i < PHX_CABAC_CTX_COUNT; i++)
        CHECK(cb.state[i] <= 127, "context state range");
    CHECK(cb.state[0] != 0, "context initialised");

    printf("%s: cabac, %d failures\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
