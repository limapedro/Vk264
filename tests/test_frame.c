/* Golden behaviour from reference/x264/common/mc.c:458-507 and frame.c:30/535.
 * The expected lowres samples are recomputed here from the source formula,
 * independently of src/phx_frame.c.
 */
#include "phx_frame.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

/* The FILTER macro from mc.c:494, written independently. */
static uint8_t filt(int a, int b, int c, int d)
{
    return (uint8_t)(((((a+b+1)>>1) + ((c+d+1)>>1)) + 1) >> 1);
}

int main(void)
{
    CHECK(phx_align_stride(80, 16, 1024) == 80, "align_stride 80");
    CHECK(phx_align_stride(1024, 16, 1024) == 1040, "align_stride 1024");
    CHECK(phx_align_plane_size(1024, 1024) == 2048, "align_plane_size");

    const int W = 64, H = 64, B = 3;
    phx_frame_t *f = phx_frame_new(W, H, B);
    CHECK(f != NULL, "frame_new");
    if (!f) return 1;
    CHECK(f->i_stride == 128, "luma stride");           /* 64+64=128 */
    CHECK(f->i_width_lowres == 32 && f->i_lines_lowres == 32, "lowres dims");
    CHECK(f->i_stride_lowres == 96, "lowres stride");   /* 32+64=96 */

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            f->plane[y*f->i_stride + x] = (uint8_t)((x*7 + y*13 + (x*y)) & 255);

    phx_frame_init_lowres(f);
    int s = f->i_stride, ls = f->i_stride_lowres;
    uint8_t *p = f->plane;

    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int a = p[(2*y  )*s + 2*x  ], b = p[(2*y+1)*s + 2*x  ];
            int c = p[(2*y  )*s + 2*x+1], d = p[(2*y+1)*s + 2*x+1];
            CHECK(f->lowres[0][y*ls+x] == filt(a,b,c,d), "lowres orig");
            a = p[(2*y  )*s + 2*x+1]; b = p[(2*y+1)*s + 2*x+1];
            c = p[(2*y  )*s + 2*x+2]; d = p[(2*y+1)*s + 2*x+2];
            CHECK(f->lowres[1][y*ls+x] == filt(a,b,c,d), "lowres H");
            a = p[(2*y+1)*s + 2*x  ]; b = p[(2*y+2)*s + 2*x  ];
            c = p[(2*y+1)*s + 2*x+1]; d = p[(2*y+2)*s + 2*x+1];
            CHECK(f->lowres[2][y*ls+x] == filt(a,b,c,d), "lowres V");
            a = p[(2*y+1)*s + 2*x+1]; b = p[(2*y+2)*s + 2*x+1];
            c = p[(2*y+1)*s + 2*x+2]; d = p[(2*y+2)*s + 2*x+2];
            CHECK(f->lowres[3][y*ls+x] == filt(a,b,c,d), "lowres HV");
        }
    }

    /* frame.c:535-552 border expansion on all four planes. */
    for (int pl = 0; pl < 4; pl++) {
        uint8_t *q = f->lowres[pl];
        for (int y = 0; y < 32; y++) {
            CHECK(q[y*ls-1] == q[y*ls], "left border");
            CHECK(q[y*ls+32] == q[y*ls+31], "right border");
        }
        for (int x = -1; x < 33; x++) {
            CHECK(q[-ls+x] == q[x], "top border");
            CHECK(q[32*ls+x] == q[31*ls+x], "bottom border");
        }
    }

    CHECK(f->i_cost_est[0][0] == -1 && f->i_cost_est[B+1][B+1] == -1,
          "i_cost_est -1");
    /* x264 mc.c:477 only clears element [0] of each row_satds array. */
    CHECK(f->i_row_satds[0][0][0] == -1 && f->i_row_satds[B+1][B+1][0] == -1,
          "row_satds -1");
    CHECK(f->lowres_mvs[0][0][0] == 0x7FFF && f->lowres_mvs[1][B][0] == 0x7FFF,
          "lowres_mvs sentinel");
    CHECK(f->lowres_mvs[1][B+1][0] == 0, "lowres_mvs beyond range untouched");

    phx_frame_delete(f);
    printf("%s: frame/lowres, %d failures\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
