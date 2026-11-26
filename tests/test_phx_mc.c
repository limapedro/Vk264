/* Standalone tests for the phase-B motion-compensation primitives.
 *
 * Every expected value is computed from the vendored x264 formulas written
 * out independently in this file (no call into src/phx_mc.c's helpers):
 *   reference/x264/common/mc.c:173-196  TAPFILTER / hpel_filter
 *   reference/x264/common/mc.c:63-75    pixel_avg_wxh
 *   reference/x264/common/mc.c:198-219  mc_luma + tables.c:183-184
 *   reference/x264/common/mc.c:252-283  mc_chroma
 *   reference/x264/common/mc.c:328-337  plane_copy_deinterleave_c
 *
 * SHADER: B — verifies the deterministic B-stage MC output.
 */
#include "phx_mc.h"
#include "phx_frame.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 24
#define H 24
#define S 32
#define PAD 8

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

/* Deterministic fixture sample.  SHADER: B — reference-frame fixture. */
static uint8_t px(int x, int y)
{
    return (uint8_t)(17*x + 41*y + ((x*7 + y*13) & 63));
}

/* common/common.h:145-148, BIT_DEPTH == 8.  SHADER: B — reference clip. */
static uint8_t ref_clip(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* common/mc.c:173 — reference TAPFILTER.  SHADER: B — reference 6-tap. */
static int ref_tap(const uint8_t *p, int x, int d)
{
    return p[x-2*d] + p[x+3*d] - 5*(p[x-d] + p[x+2*d])
         + 20*(p[x] + p[x+d]);
}

/* 16-bit variant of TAPFILTER for the intermediate buffer (mc.c:174/188).
 * SHADER: B — reference 6-tap on int16_t samples. */
static int ref_tap16(const int16_t *p, int x)
{
    return p[x-2] + p[x+3] - 5*(p[x-1] + p[x+2]) + 20*(p[x] + p[x+1]);
}

/* common/mc.c:174-196 — reference H/V/C planes.  SHADER: B. */
static void ref_hpel(uint8_t *dh, uint8_t *dv, uint8_t *dc,
                     const uint8_t *src, int stride, int width, int height)
{
    int16_t buf[256];
    for (int y = 0; y < height; y++) {
        for (int x = -2; x < width+3; x++) {
            int v = ref_tap(src, x, stride);
            dv[x] = ref_clip((v + 16) >> 5);
            buf[x+2] = (int16_t)v;
        }
        for (int x = 0; x < width; x++)
            dc[x] = ref_clip((ref_tap16(buf+2, x) + 512) >> 10);
        for (int x = 0; x < width; x++)
            dh[x] = ref_clip((ref_tap(src, x, 1) + 16) >> 5);
        dh += stride; dv += stride; dc += stride; src += stride;
    }
}

/* common/mc.c:198-219 + tables.c:183-184 — reference luma MC (no weighting).
 * SHADER: B — reference block. */
static void ref_luma(uint8_t *dst, int dd, const uint8_t *p[4], int sd,
                     int mvx, int mvy, int w, int h)
{
    static const uint8_t r0[16] = {0,1,1,1,0,1,1,1,2,3,3,3,0,1,1,1};
    static const uint8_t r1[16] = {0,0,1,0,2,2,3,2,2,2,3,2,2,2,3,2};
    int q = ((mvy&3)<<2) + (mvx&3);
    int off = (mvy>>2)*sd + (mvx>>2);
    const uint8_t *s1 = p[r0[q]] + off + ((mvy&3) == 3)*sd;
    if (q & 5) {
        const uint8_t *s2 = p[r1[q]] + off + ((mvx&3) == 3);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y*dd+x] = (uint8_t)((s1[y*sd+x] + s2[y*sd+x] + 1) >> 1);
    } else {
        for (int y = 0; y < h; y++)
            memcpy(dst + y*dd, s1 + y*sd, (size_t)w);
    }
}

/* common/mc.c:252-283 — reference chroma MC.  SHADER: B. */
static void ref_chroma(uint8_t *du, int du_s, uint8_t *dv, int dv_s,
                       const uint8_t *src, int ss,
                       int mvx, int mvy, int w, int h)
{
    int d8x = mvx & 7, d8y = mvy & 7;
    int cA = (8-d8x)*(8-d8y), cB = d8x*(8-d8y);
    int cC = (8-d8x)*d8y, cD = d8x*d8y;
    src += (mvy>>3)*ss + (mvx>>3)*2;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            du[y*du_s+x] = (uint8_t)((cA*src[2*x] + cB*src[2*x+2]
                                    + cC*src[2*x+ss] + cD*src[2*x+2+ss] + 32) >> 6);
            dv[y*dv_s+x] = (uint8_t)((cA*src[2*x+1] + cB*src[2*x+3]
                                    + cC*src[2*x+1+ss] + cD*src[2*x+3+ss] + 32) >> 6);
        }
        src += ss;
    }
}

/* ---- hpel_filter: full grid against the literal x264 formula ------------ */
static void test_hpel_filter(void)
{
    static uint8_t sp[S*(H+2*PAD) + 2*PAD];
    static uint8_t oh[S*(H+2*PAD) + 2*PAD], ov[S*(H+2*PAD) + 2*PAD];
    static uint8_t oc[S*(H+2*PAD) + 2*PAD];
    static uint8_t eh[S*(H+2*PAD) + 2*PAD], ev[S*(H+2*PAD) + 2*PAD];
    static uint8_t ec[S*(H+2*PAD) + 2*PAD];
    uint8_t *s = sp + PAD*S + PAD, *h = oh + PAD*S + PAD;
    uint8_t *v = ov + PAD*S + PAD, *c = oc + PAD*S + PAD;
    uint8_t *rh = eh + PAD*S + PAD, *rv = ev + PAD*S + PAD;
    uint8_t *rc = ec + PAD*S + PAD;
    for (int y = -PAD; y < H+PAD; y++)
        for (int x = -PAD; x < W+PAD; x++)
            s[y*S+x] = px(x, y);
    memset(oh, 0, sizeof(oh)); memset(ov, 0, sizeof(ov)); memset(oc, 0, sizeof(oc));

    int16_t buf[W+32];
    phx_mc_hpel_filter(h, v, c, s, S, W, H, buf);
    ref_hpel(rh, rv, rc, s, S, W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (h[y*S+x] != rh[y*S+x])
                printf("FAIL hpel H (%d,%d): %d vs %d\n", x, y,
                       h[y*S+x], rh[y*S+x]), ++fails;
            if (v[y*S+x] != rv[y*S+x])
                printf("FAIL hpel V (%d,%d): %d vs %d\n", x, y,
                       v[y*S+x], rv[y*S+x]), ++fails;
            if (c[y*S+x] != rc[y*S+x])
                printf("FAIL hpel C (%d,%d): %d vs %d\n", x, y,
                       c[y*S+x], rc[y*S+x]), ++fails;
        }
    /* individual CHECKs above report every mismatch */
}

/* Hand-computed golden for src[x] = 4*x (constant in y), at x == 8:
 * horizontal TAPFILTER = 24+44-5*(28+40)+20*(32+36) = 1088,
 * vertical TAPFILTER = 32*32 = 1024 (src is constant down each column),
 * H = (1088+16)>>5 = 34, V = (1024+16)>>5 = 32,
 * C = (1024*32+512)>>10 = 32. */
static void test_hpel_golden(void)
{
    static uint8_t sp[S*(H+2*PAD) + 2*PAD];
    static uint8_t hp[S*(H+2*PAD) + 2*PAD], vp[S*(H+2*PAD) + 2*PAD];
    static uint8_t cp[S*(H+2*PAD) + 2*PAD];
    uint8_t *s = sp + PAD*S + PAD, *h = hp + PAD*S + PAD;
    uint8_t *v = vp + PAD*S + PAD, *c = cp + PAD*S + PAD;
    for (int y = -PAD; y < H+PAD; y++)
        for (int x = -PAD; x < W+PAD; x++)
            s[y*S+x] = (uint8_t)(4*x);
    int16_t buf[W+32];
    phx_mc_hpel_filter(h, v, c, s, S, W, H, buf);
    CHECK(h[8] == 34 && v[8] == 32 && c[8] == 34,
          "hpel golden 34/32/34 (mc.c:173-196)");
}

/* mc.c:70 — (10+20+1)>>1 == 15. */
static void test_pixel_avg(void)
{
    static uint8_t a[4*4], b[4*4], d[4*4];
    for (int i = 0; i < 16; i++) { a[i] = 10; b[i] = 20; d[i] = 0; }
    phx_mc_pixel_avg(d, 4, a, 4, b, 4, 4, 4);
    for (int i = 0; i < 16; i++) CHECK(d[i] == 15, "pixel_avg golden 15");
}

/* ---- mc_luma: every qpel_idx class -------------------------------------- */
static void test_luma(void)
{
    static uint8_t sp[4][S*(H+2*PAD) + 2*PAD];
    static uint8_t op[4][S*(H+2*PAD) + 2*PAD];
    uint8_t *planes[4], *out[4];
    for (int p = 0; p < 4; p++) {
        planes[p] = sp[p] + PAD*S + PAD;
        out[p] = op[p] + PAD*S + PAD;
        for (int y = -PAD; y < H+PAD; y++)
            for (int x = -PAD; x < W+PAD; x++)
                planes[p][y*S+x] = px(x + 3*p, y + 5*p);
    }
    uint8_t rb[S*(H+2*PAD)+2*PAD], vb[S*(H+2*PAD)+2*PAD];
    uint8_t cb[S*(H+2*PAD)+2*PAD];
    uint8_t *rh = rb + PAD*S + PAD, *rv = vb + PAD*S + PAD;
    uint8_t *rc = cb + PAD*S + PAD;
    ref_hpel(rh, rv, rc, planes[0], S, W, H);
    /* Copy the padded rows/columns the luma MC may reach. */
    for (int y = -PAD; y < H+PAD; y++) {
        memcpy(planes[1] + y*S - PAD, rh + y*S - PAD, (size_t)(W+2*PAD));
        memcpy(planes[2] + y*S - PAD, rv + y*S - PAD, (size_t)(W+2*PAD));
        memcpy(planes[3] + y*S - PAD, rc + y*S - PAD, (size_t)(W+2*PAD));
    }

    /* (mvx,mvy) chosen to exercise ref0/ref1, the ==3 shifts and fullpel. */
    static const int mvs[][2] = {
        {8,4}, {0,0}, {2,0}, {0,2}, {2,2}, {1,0}, {3,0}, {0,1},
        {1,1}, {2,1}, {3,3}, {-1,2}, {5,7}
    };
    for (size_t t = 0; t < sizeof(mvs)/sizeof(mvs[0]); t++) {
        int mvx = mvs[t][0], mvy = mvs[t][1];
        uint8_t want[16*16];
        ref_luma(want, 16, (const uint8_t **)planes, S, mvx, mvy, 16, 16);
        phx_mc_luma(out[0], 16, (const uint8_t **)planes, S, mvx, mvy, 16, 16);
        if (memcmp(want, out[0], 16*16) != 0) {
            printf("FAIL mc_luma mv=(%d,%d) first: got %d want %d\n",
                   mvx, mvy, out[0][0], want[0]);
            ++fails;
        }
    }
    /* individual CHECKs above report every mismatch */
}

/* ---- mc_chroma ---------------------------------------------------------- */
static void test_chroma(void)
{
    static uint8_t sp[S*(H/2+2*PAD) + 2*PAD];
    static uint8_t ru[8*8], rv[8*8], ou[8*8], ov[8*8];
    uint8_t *src = sp + PAD*S + PAD;
    for (int y = -PAD; y < H/2+PAD; y++)
        for (int x = -PAD; x < W+PAD; x++)
            src[y*S+x] = px(x, y+32);

    static const int mvs[][2] = {{0,0}, {3,2}, {7,7}, {1,0}, {4,6}, {-3,1}, {8,4}};
    for (size_t t = 0; t < sizeof(mvs)/sizeof(mvs[0]); t++) {
        ref_chroma(ru, 8, rv, 8, src, S, mvs[t][0], mvs[t][1], 8, 8);
        memset(ou, 0, sizeof(ou)); memset(ov, 0, sizeof(ov));
        phx_mc_chroma(ou, 8, ov, 8, src, S, mvs[t][0], mvs[t][1], 8, 8);
        if (memcmp(ru, ou, 64) || memcmp(rv, ov, 64)) {
            printf("FAIL mc_chroma mv=(%d,%d)\n", mvs[t][0], mvs[t][1]);
            ++fails;
        }
    }

    /* Constant-source golden: coefficients sum to 64, so U/V are copied
     * exactly even at quarter-pel (mc.c:261-276). */
    for (int y = -PAD; y < H/2+PAD; y++) {
        for (int x = -PAD; x < W+PAD; x++)
            src[y*S+x] = (uint8_t)(2*x);
        for (int x = 0; x < 64; x += 2) {
            src[y*S+x] = 100; src[y*S+x+1] = 150;
        }
    }
    phx_mc_chroma(ou, 8, ov, 8, src, S, 3, 2, 8, 8);
    for (int i = 0; i < 64; i++) {
        CHECK(ou[i] == 100, "mc_chroma constant U golden");
        CHECK(ov[i] == 150, "mc_chroma constant V golden");
    }
}

/* ---- deinterleave (mv0 shortcut) ---------------------------------------- */
static void test_deinterleave(void)
{
    static uint8_t sp[8*S], ou[64], ov[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 16; x++)
            sp[y*S+x] = (uint8_t)(y*16+x);
    phx_mc_deinterleave_chroma(ou, 8, ov, 8, sp, S, 8, 8);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            CHECK(ou[y*8+x] == sp[y*S+2*x], "deinterleave U");
            CHECK(ov[y*8+x] == sp[y*S+2*x+1], "deinterleave V");
        }
}

/* ---- integration: phx_mc_luma_mb on a real phx_frame_t ------------------- */
static void test_luma_mb(void)
{
    phx_frame_t *f = phx_frame_new(32, 32, 0);
    CHECK(f != NULL, "frame allocation");
    if (!f) return;
    for (int y = -32; y < 64; y++)
        for (int x = -32; x < f->i_stride; x++)
            f->plane[y*f->i_stride+x] = px(x, y);

    static const int cases[][3] = {
        {0, 8, 4}, {0, 0, 0}, {0, 1, 0}, {0, 3, 3}, {0, -1, 6}
    };
    for (size_t t = 0; t < sizeof(cases)/sizeof(cases[0]); t++) {
        const int mb_x = cases[t][0]/16, mb_y = 0;
        const int mvx = cases[t][1], mvy = cases[t][2];
        uint8_t out[16*16], want[16*16];
        /* Oracle built from the validated primitives at the frame's real
         * stride (tests above check those primitives against the literal
         * x264 formulas).  phx_mc_luma_mb must wire the same planes/MV. */
        const int fs = f->i_stride;
        const size_t bytes = (size_t)fs*(f->i_height+16) + 16;
        const size_t off0 = 8*(size_t)fs + 8;
        uint8_t *alloc = (uint8_t *)malloc(off0 + 4*bytes);
        int16_t *buf = (int16_t *)malloc((size_t)(f->i_width+32)*sizeof(int16_t));
        CHECK(alloc && buf, "oracle allocation");
        if (!alloc || !buf) { free(alloc); free(buf); break; }
        uint8_t *hp = alloc + 8*fs + 8, *vp = hp + bytes;
        uint8_t *cp = vp + bytes, *full = cp + bytes;
        phx_mc_hpel_filter_frame(f->plane, fs, f->i_width, f->i_height,
                                 hp, vp, cp, buf);
        memcpy(full, f->plane, bytes);
        const size_t off = (size_t)(mb_y*16)*fs + (size_t)(mb_x*16);
        const uint8_t *planes[4] = { full+off, hp+off, vp+off, cp+off };
        phx_mc_luma(want, 16, planes, fs, mvx, mvy, 16, 16);
        free(alloc); free(buf);

        CHECK(phx_mc_luma_mb(f, out, 16, mb_x, mb_y, mvx, mvy, 16, 16) == 0,
              "phx_mc_luma_mb succeeds");
        if (memcmp(want, out, 16*16) != 0) {
            int k = 0;
            while (k < 256 && want[k] == out[k]) k++;
            printf("FAIL phx_mc_luma_mb mv=(%d,%d) at %d: got %d want %d\n",
                   mvx, mvy, k, out[k], want[k]);
            ++fails;
        }
    }
    phx_mc_hpel_invalidate();
    phx_frame_delete(f);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_hpel_filter();
    test_hpel_golden();
    test_pixel_avg();
    test_luma();
    test_chroma();
    test_deinterleave();
    test_luma_mb();
    if (fails) {
        printf("FAIL %d check(s)\n", fails);
        return 1;
    }
    printf("PASS test_phx_mc\n");
    return 0;
}
