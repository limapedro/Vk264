/* Function-for-function port of x264's scalar motion compensation.
 * Every function below carries the exact vendored source line it follows.
 *
 * SHADER: B — one serial invocation per macroblock for all MC operations.
 */
#include "phx_mc.h"

#include <stdlib.h>
#include <string.h>

/* common/common.h:145-148 — x264_clip_pixel() for BIT_DEPTH == 8 (PIXEL_MAX
 * == 255).  Kept as the x264 bit expression rather than a saturated ternary.
 * SHADER: B — scalar clamp. */
static uint8_t phx_mc_clip_pixel(int x)
{
    return (uint8_t)((x & ~255) ? ((-x) >> 31) & 255 : x);
}

/* common/tables.c:183-184 — identical to x264_hpel_ref0 / x264_hpel_ref1.
 * SHADER: B — per-MV constant plane selection tables. */
const uint8_t phx_mc_hpel_ref0[16] = {0,1,1,1,0,1,1,1,2,3,3,3,0,1,1,1};
const uint8_t phx_mc_hpel_ref1[16] = {0,0,1,0,2,2,3,2,2,2,3,2,2,2,3,2};

/* common/mc.c:173 — TAPFILTER(pix, d), with the loop variable x implied by
 * the caller (hpel_filter below).  SHADER: B — 6-tap window. */
#define PHX_MC_TAPFILTER(pix, d) \
    ((pix)[x-2*(d)] + (pix)[x+3*(d)] - 5*((pix)[x-(d)] + (pix)[x+2*(d)]) \
     + 20*((pix)[x] + (pix)[x+(d)]))

/* common/mc.c:174-196 — hpel_filter().  BIT_DEPTH == 8 gives pad == 0 at
 * mc.c:177, hence the -32*pad and +512 terms stay exactly as written.  The
 * caller must provide the same padded addressable range x264 uses:
 * src/dst[x + y*stride] for x in [-8, width+7], y in [-8, height+7].
 * SHADER: B — local 6-tap filtering into three halfpel planes. */
void phx_mc_hpel_filter(uint8_t *dsth, uint8_t *dstv, uint8_t *dstc,
                        const uint8_t *src, int stride,
                        int width, int height, int16_t *buf)
{
    const int pad = 0; /* mc.c:177: (BIT_DEPTH > 9) ? (-10*PIXEL_MAX) : 0 */
    for (int y = 0; y < height; y++)                     /* mc.c:178 */
    {
        for (int x = -2; x < width+3; x++)                /* mc.c:180 */
        {
            int v = PHX_MC_TAPFILTER(src, stride);        /* mc.c:182 */
            dstv[x] = phx_mc_clip_pixel((v + 16) >> 5);   /* mc.c:183 */
            buf[x+2] = (int16_t)(v + pad);                /* mc.c:185 */
        }
        for (int x = 0; x < width; x++)                   /* mc.c:187 */
            dstc[x] = phx_mc_clip_pixel(
                (PHX_MC_TAPFILTER(buf+2, 1) - 32*pad + 512) >> 10); /* mc.c:188 */
        for (int x = 0; x < width; x++)                   /* mc.c:189 */
            dsth[x] = phx_mc_clip_pixel(
                (PHX_MC_TAPFILTER(src, 1) + 16) >> 5);    /* mc.c:190 */
        dsth += stride;                                   /* mc.c:191 */
        dstv += stride;                                   /* mc.c:192 */
        dstc += stride;                                   /* mc.c:193 */
        src  += stride;                                   /* mc.c:194 */
    }
}

/* common/mc.c:704-726 — x264_frame_filter(), progressive/b_end path only:
 * start = -8, offs = -8*stride-8, width+16, height-start = height+16.
 * SHADER: B — full-frame halfpel prepass. */
void phx_mc_hpel_filter_frame(const uint8_t *plane, int stride,
                              int width, int height,
                              uint8_t *dsth, uint8_t *dstv, uint8_t *dstc,
                              int16_t *buf)
{
    const int offs = -8*stride - 8;                       /* mc.c:717 */
    phx_mc_hpel_filter(dsth + offs, dstv + offs, dstc + offs,
                       plane + offs, stride,
                       width + 16, height + 16, buf);     /* mc.c:720-726 */
}

/* common/mc.c:63-75 — pixel_avg_wxh().  SHADER: B — qpel average. */
void phx_mc_pixel_avg(uint8_t *dst, int i_dst,
                      const uint8_t *src1, int i_src1,
                      const uint8_t *src2, int i_src2,
                      int width, int height)
{
    for (int y = 0; y < height; y++)                      /* mc.c:67 */
    {
        for (int x = 0; x < width; x++)                   /* mc.c:69 */
            dst[x] = (uint8_t)((src1[x] + src2[x] + 1) >> 1); /* mc.c:70 */
        src1 += i_src1;                                   /* mc.c:71 */
        src2 += i_src2;                                   /* mc.c:72 */
        dst  += i_dst;                                    /* mc.c:73 */
    }
}

/* common/mc.c:162-171 — mc_copy().  SHADER: B — fullpel row copy. */
void phx_mc_copy(const uint8_t *src, int i_src_stride,
                 uint8_t *dst, int i_dst_stride, int width, int height)
{
    for (int y = 0; y < height; y++)                      /* mc.c:164 */
    {
        memcpy(dst, src, (size_t)width);                  /* mc.c:166 */
        src += i_src_stride;                              /* mc.c:168 */
        dst += i_dst_stride;                              /* mc.c:169 */
    }
}

/* common/mc.c:198-219 — mc_luma(), with x264_weight_none (mc.c:216 is the
 * weighted branch; Phoenix never builds sh.weight, so the unweighted
 * mc_copy/mc_weight-else path at :218 is the authoritative one).
 * SHADER: B — one luma MC block. */
void phx_mc_luma(uint8_t *dst, int i_dst_stride,
                 const uint8_t *src[4], int i_src_stride,
                 int mvx, int mvy, int width, int height)
{
    int qpel_idx = ((mvy&3)<<2) + (mvx&3);                /* mc.c:203 */
    int offset = (mvy>>2)*i_src_stride + (mvx>>2);        /* mc.c:204 */
    const uint8_t *src1 = src[phx_mc_hpel_ref0[qpel_idx]]
                        + offset + ((mvy&3) == 3) * i_src_stride; /* mc.c:205 */

    if (qpel_idx & 5) /* qpel interpolation needed */      /* mc.c:207 */
    {
        const uint8_t *src2 = src[phx_mc_hpel_ref1[qpel_idx]]
                            + offset + ((mvx&3) == 3);    /* mc.c:209 */
        phx_mc_pixel_avg(dst, i_dst_stride, src1, i_src_stride,
                         src2, i_src_stride, width, height); /* mc.c:210-211 */
    }
    else
        phx_mc_copy(src1, i_src_stride, dst, i_dst_stride,
                    width, height);                       /* mc.c:218 */
}

/* common/mc.c:252-283 — mc_chroma(), full chroma MC to 1/8 pixel.  The
 * source is NV12 interleaved exactly like x264's p_fref[0][0][4] for an
 * internal 4:2:0 CSP (common/frame.c:45-57).  SHADER: B — one chroma block. */
void phx_mc_chroma(uint8_t *dstu, int i_dstu, uint8_t *dstv, int i_dstv,
                   const uint8_t *src, int i_src_stride,
                   int mvx, int mvy, int width, int height)
{
    const uint8_t *srcp;

    int d8x = mvx&0x07;                                   /* mc.c:259 */
    int d8y = mvy&0x07;                                   /* mc.c:260 */
    int cA = (8-d8x)*(8-d8y);                             /* mc.c:261 */
    int cB = d8x    *(8-d8y);                             /* mc.c:262 */
    int cC = (8-d8x)*d8y;                                 /* mc.c:263 */
    int cD = d8x    *d8y;                                 /* mc.c:264 */

    src += (mvy >> 3) * i_src_stride + (mvx >> 3)*2;      /* mc.c:266 */
    srcp = &src[i_src_stride];                            /* mc.c:267 */

    for (int y = 0; y < height; y++)                      /* mc.c:269 */
    {
        for (int x = 0; x < width; x++)                   /* mc.c:271 */
        {
            dstu[x] = (uint8_t)(( cA*src[2*x]    + cB*src[2*x+2]
                                + cC*srcp[2*x]  + cD*srcp[2*x+2] + 32) >> 6);
                                                          /* mc.c:273-274 */
            dstv[x] = (uint8_t)(( cA*src[2*x+1]  + cB*src[2*x+3]
                                + cC*srcp[2*x+1] + cD*srcp[2*x+3] + 32) >> 6);
                                                          /* mc.c:275-276 */
        }
        dstu += i_dstu;                                   /* mc.c:278 */
        dstv += i_dstv;                                   /* mc.c:279 */
        src   = srcp;                                     /* mc.c:280 */
        srcp += i_src_stride;                             /* mc.c:281 */
    }
}

/* common/mc.c:328-337 + :407-410 — x264_plane_copy_deinterleave_c() /
 * load_deinterleave_chroma_fdec(), the mv0 shortcut of
 * encoder/macroblock.c:674-681.  SHADER: B — planar U/V copy. */
void phx_mc_deinterleave_chroma(uint8_t *dstu, int i_dstu,
                                uint8_t *dstv, int i_dstv,
                                const uint8_t *src, int i_src_stride,
                                int width, int height)
{
    for (int y = 0; y < height; y++, dstu += i_dstu, dstv += i_dstv,
                                 src += i_src_stride)     /* mc.c:331 */
        for (int x = 0; x < width; x++)                   /* mc.c:332 */
        {
            dstu[x] = src[2*x];                           /* mc.c:334 */
            dstv[x] = src[2*x+1];                         /* mc.c:335 */
        }
}

/* Single-entry HPEL cache.  x264 keeps the filtered planes on the frame
 * (common/frame.h:82) and refreshes them in x264_frame_filter
 * (common/mc.c:704-726).  phx_frame_t has no `filtered` member, so Phoenix
 * keeps the same read-only planes here, keyed by the frame identity.
 * SHADER: B — serial per-MB cache, not thread-safe. */
typedef struct {
    const phx_frame_t *frame;
    int stride, width, height;
    uint8_t *alloc;
    uint8_t *h, *v, *c;
    int16_t *buf;
} phx_mc_hpel_cache_t;

static phx_mc_hpel_cache_t phx_mc_hpel_cache;

/* Drop the cached HPEL planes.  SHADER: B — cache invalidation. */
void phx_mc_hpel_invalidate(void)
{
    free(phx_mc_hpel_cache.alloc);
    memset(&phx_mc_hpel_cache, 0, sizeof(phx_mc_hpel_cache));
}

/* Prepare the H/V/C planes of fref, mirroring macroblock_load_pic_pointers
 * (common/macroblock.c:568-640): p_fref[0][0][0] = plane + 16*mb_y*stride +
 * 16*mb_x and [1..3] are the corresponding filtered planes.  The MV must
 * already be clipped by the caller (analyse.c:336-337).
 * SHADER: B — cached read-only reference preparation. */
int phx_mc_luma_mb(const phx_frame_t *fref, uint8_t *dst, int i_dst_stride,
                   int mb_x, int mb_y, int mvx, int mvy,
                   int width, int height)
{
    if (!fref || !fref->plane || !dst || fref->i_stride <= 0 ||
        fref->i_width <= 0 || fref->i_height <= 0 ||
        width <= 0 || height <= 0 || i_dst_stride <= 0)
        return -1;
    if (mb_x < 0 || mb_y < 0 || mb_x >= fref->i_width/16 ||
        mb_y >= fref->i_height/16)
        return -1;

    phx_mc_hpel_cache_t *hc = &phx_mc_hpel_cache;
    if (!hc->alloc || hc->frame != fref || hc->stride != fref->i_stride ||
        hc->width != fref->i_width || hc->height != fref->i_height)
    {
        phx_mc_hpel_invalidate();
        const int stride = fref->i_stride;
        const int width = fref->i_width;
        const int height = fref->i_height;
        size_t plane_bytes = (size_t)stride * (size_t)(height + 16) + 16;
        size_t buf_elems = (size_t)width + 32; /* filter sees width+16, :180/:185 */
        size_t off0 = 8*(size_t)stride + 8;
        size_t total = off0 + 3*plane_bytes + buf_elems*sizeof(int16_t);
        uint8_t *alloc = (uint8_t *)malloc(total);
        if (!alloc)
            return -1;
        uint8_t *h = alloc + off0;
        uint8_t *v = h + plane_bytes;
        uint8_t *c = v + plane_bytes;
        int16_t *buf = (int16_t *)(c + plane_bytes);
        phx_mc_hpel_filter_frame(fref->plane, stride, width, height,
                                 h, v, c, buf);
        hc->frame = fref;
        hc->stride = stride;
        hc->width = width;
        hc->height = height;
        hc->alloc = alloc;
        hc->h = h;
        hc->v = v;
        hc->c = c;
        hc->buf = buf;
    }

    const int stride = hc->stride;
    const size_t off = (size_t)(mb_y*16)*stride + (size_t)(mb_x*16);
    const uint8_t *src[4] = {
        fref->plane + off,
        hc->h + off,
        hc->v + off,
        hc->c + off
    };
    phx_mc_luma(dst, i_dst_stride, src, stride, mvx, mvy, width, height);
    return 0;
}
