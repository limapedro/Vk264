/* Function-for-function port of x264 frame allocation / lowres setup.
 * Source lines appear on each function and assignment.
 */
#include "phx_frame.h"
#include <stdlib.h>
#include <string.h>

#define PHX_ALIGN(x,a) (((x)+(a)-1) & ~((a)-1))

/* x264 common/frame.c:30 — align_stride(). */
int phx_align_stride(int x, int align, int disalign)
{
    x = PHX_ALIGN(x, align);                         /* frame.c:32 */
    if (!(x & (disalign-1)))                         /* frame.c:33 */
        x += align;                                  /* frame.c:34 */
    return x;
}

/* x264 common/frame.c:38 — align_plane_size(). */
int phx_align_plane_size(int x, int disalign)
{
    if (!(x & (disalign-1)))                         /* frame.c:39 */
        x += 1024;                                   /* frame.c:40:
                                                     * X264_MAX(128,NATIVE_ALIGN)
                                                     * / SIZEOF_PIXEL = 1024
                                                     * for the scalar build */
    return x;
}

/* x264 common/frame.c:535 — plane_expand_border(). b_chroma is always 0 for
 * the lowres luma planes. */
void phx_plane_expand_border(uint8_t *pix, int stride, int width, int height,
                             int padh, int padv, int pad_top, int pad_bottom,
                             int chroma)
{
#define PPIXEL(x,y) (pix + (x) + (y)*stride)
    for (int y = 0; y < height; y++)                 /* frame.c:538 */
    {
        /* left band: pixel_memset repeats the sample at x=0 (frame.c:541) */
        for (int k = 0; k < (padh >> chroma); k++)
            PPIXEL(-padh+k, y)[0] = PPIXEL(0, y)[0];
        /* right band: repeats the sample at x=width-1 (frame.c:543) */
        for (int k = 0; k < (padh >> chroma); k++)
            PPIXEL(width+k, y)[0] = PPIXEL(width-1, y)[0];
    }
    if (pad_top)                                     /* frame.c:546 */
        for (int y = 0; y < padv; y++)
            memcpy(PPIXEL(-padh, -y-1), PPIXEL(-padh, 0),
                   (size_t)(width + 2*padh));        /* frame.c:548 */
    if (pad_bottom)                                  /* frame.c:550 */
        for (int y = 0; y < padv; y++)
            memcpy(PPIXEL(-padh, height+y), PPIXEL(-padh, height-1),
                   (size_t)(width + 2*padh));        /* frame.c:552 */
#undef PPIXEL
}

/* x264 common/frame.c:627 — x264_frame_expand_border_lowres(). */
void phx_frame_expand_border_lowres(phx_frame_t *f)
{
    for (int i = 0; i < PHX_LOWRES_PLANES; i++)      /* frame.c:629 */
        phx_plane_expand_border(f->lowres[i], f->i_stride_lowres,
                                f->i_width_lowres, f->i_lines_lowres,
                                PHX_PADH, PHX_PADV, 1, 1, 0); /* frame.c:630 */
}

/* x264 common/mc.c:484 — frame_init_lowres_core(). The FILTER macro and
 * pointer walks are copied verbatim (the "slower than naive bilinear, but
 * matches asm" rounding from mc.c:493). */
void phx_frame_init_lowres_core(uint8_t *src0, uint8_t *dst0, uint8_t *dsth,
                                uint8_t *dstv, uint8_t *dstc,
                                int src_stride, int dst_stride,
                                int width, int height)
{
#define FILTER(a,b,c,d) ((((a+b+1)>>1)+((c+d+1)>>1)+1)>>1) /* mc.c:494 */
    for (int y = 0; y < height; y++)                 /* mc.c:487 */
    {
        uint8_t *src1 = src0 + src_stride;           /* mc.c:489 */
        uint8_t *src2 = src1 + src_stride;           /* mc.c:490 */
        for (int x = 0; x < width; x++)              /* mc.c:491 */
        {
            dst0[x] = FILTER(src0[2*x  ], src1[2*x  ], src0[2*x+1], src1[2*x+1]);
            dsth[x] = FILTER(src0[2*x+1], src1[2*x+1], src0[2*x+2], src1[2*x+2]);
            dstv[x] = FILTER(src1[2*x  ], src2[2*x  ], src1[2*x+1], src2[2*x+1]);
            dstc[x] = FILTER(src1[2*x+1], src2[2*x+1], src1[2*x+2], src2[2*x+2]);
                                                     /* mc.c:495-498 */
        }
        src0 += src_stride*2;                        /* mc.c:501 */
        dst0 += dst_stride; dsth += dst_stride;
        dstv += dst_stride; dstc += dst_stride;      /* mc.c:502-505 */
    }
#undef FILTER
}

/* x264 common/mc.c:458 — x264_frame_init_lowres(). */
void phx_frame_init_lowres(phx_frame_t *f)
{
    uint8_t *src = f->plane;                         /* mc.c:460 */
    int i_stride = f->i_stride;                      /* mc.c:461 */
    int i_height = f->i_height;                      /* mc.c:462 */
    int i_width = f->i_width;                        /* mc.c:463 */

    /* duplicate last row and column so interpolation needs no special case */
    for (int y = 0; y < i_height; y++)               /* mc.c:466 */
        src[i_width + y*i_stride] = src[i_width-1 + y*i_stride]; /* mc.c:467 */
    memcpy(src + i_stride*i_height, src + i_stride*(i_height-1),
           (size_t)(i_width+1));                     /* mc.c:468 */

    phx_frame_init_lowres_core(src, f->lowres[0], f->lowres[1],
                               f->lowres[2], f->lowres[3],
                               i_stride, f->i_stride_lowres,
                               f->i_width_lowres, f->i_lines_lowres);
                                                     /* mc.c:469-470 */
    phx_frame_expand_border_lowres(f);               /* mc.c:471 */
    memset(f->i_cost_est, -1, sizeof(f->i_cost_est)); /* mc.c:473 */

    for (int y = 0; y < f->bframes + 2; y++)         /* mc.c:475 */
        for (int x = 0; x < f->bframes + 2; x++)
            f->i_row_satds[y][x][0] = -1;            /* mc.c:477 */

    for (int y = 0; y <= !!f->bframes; y++)          /* mc.c:479 */
        for (int x = 0; x <= f->bframes; x++)
            f->lowres_mvs[y][x][0] = 0x7FFF;         /* mc.c:481 */
}

/* x264 common/frame.c:60 — a reduced x264_frame_new() containing the fields
 * the Phoenix port needs. Allocation order and offsets follow frame.c. */
phx_frame_t *phx_frame_new(int width, int height, int bframes)
{
    if (width % 16 || height % 16) return NULL;      /* frame.c:85-86 */
    phx_frame_t *f = calloc(1, sizeof(*f));          /* frame.c:81 */
    if (!f) return NULL;
    f->bframes = bframes;
    f->i_width = width; f->i_height = height;        /* frame.c:85-86 */
    f->i_stride = phx_align_stride(width + PHX_PADH2, 16, 1024); /* frame.c:87 */
    f->i_width_lowres = width/2;                     /* frame.c:123 */
    f->i_lines_lowres = height/2;                    /* frame.c:124 */
    f->i_stride_lowres = phx_align_stride(f->i_width_lowres + PHX_PADH2, 16, 1024);
                                                     /* frame.c:125 */

    int luma_size = phx_align_plane_size(f->i_stride * (height + 2*PHX_PADV), 1024);
                                                     /* frame.c:164 */
    int lowres_size = phx_align_plane_size(f->i_stride_lowres *
                                           (f->i_lines_lowres + 2*PHX_PADV), 1024);
                                                     /* frame.c:207 */
    /* luma buffer (frame.c:169) + 4 lowres planes (frame.c:209) + asm pad */
    size_t total = (size_t)luma_size + 4*(size_t)lowres_size + 256;
    f->base = calloc(total, 1);                      /* frame.c:235 */
    if (!f->base) { free(f); return NULL; }
    uint8_t *buf = f->base;
    f->plane = buf + f->i_stride*PHX_PADV + PHX_PADH_ALIGN; /* frame.c:261 */
    buf += luma_size;
    for (int i = 0; i < 4; i++)
        f->lowres[i] = buf + f->i_stride_lowres*PHX_PADV + PHX_PADH_ALIGN
                       + (size_t)i*lowres_size;       /* frame.c:281 */

    int mb_rows = height/16;
    for (int y = 0; y < bframes+2; y++)
        for (int x = 0; x < bframes+2; x++) {
            f->i_row_satds[y][x] = calloc((size_t)mb_rows, sizeof(int)); /* :129 */
            if (!f->i_row_satds[y][x]) { phx_frame_delete(f); return NULL; }
        }
    return f;
}

/* x264 common/frame.c:312 — x264_frame_delete(). */
void phx_frame_delete(phx_frame_t *f)
{
    if (!f) return;
    for (int y = 0; y < PHX_BFRAME_MAX+2; y++)
        for (int x = 0; x < PHX_BFRAME_MAX+2; x++)
            free(f->i_row_satds[y][x]);              /* frame.c:314-317 */
    free(f->base);                                   /* frame.c:318 */
    free(f);
}
