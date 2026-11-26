/* Standalone test for the version-SEI writer (src/phx_sei.c).
 *
 * Checks the NAL header/type, the leading `06 05 ff ff` bytes, the option
 * tokens of a known preset, and — when a saved x264 reference stream is
 * available — compares the SEI NAL byte-for-byte.  A mismatch is reported
 * with its first differing offset and fails the test; no parity is claimed
 * until that comparison is exact.
 *
 * SHADER: C — host-side test harness only; no runtime state.
 */
#include "phx_sei.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_W 64
#define TEST_H 64
#define TEST_QP 28
#define SEI_CAP 8192

static int g_fail = 0;

#define CHECK(cond, msg) do {                                    \
    if (!(cond)) {                                               \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
        g_fail++;                                                \
    }                                                            \
} while (0)

/* Locate a saved reference stream.  An explicit path may be passed as argv[1];
 * otherwise try the repo-root and build-directory-relative spellings so the
 * test works both from the repo root and from build/tests. */
static const char *find_ref(const char *arg)
{
    static const char *candidates[] = {
        NULL,                                                    /* argv[1] */
        "build/parity/synth-medium-cabac.ref.h264",
        "../../build/parity/synth-medium-cabac.ref.h264",
        "../build/parity/synth-medium-cabac.ref.h264",
    };
    if (arg && *arg) {
        FILE *f = fopen(arg, "rb");
        if (f) { fclose(f); return arg; }
        fprintf(stderr, "note: explicit ref '%s' not found; trying defaults\n", arg);
    }
    for (size_t i = 1; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) { fclose(f); return candidates[i]; }
    }
    return NULL;
}

/* Copy the type-6 NAL out of an Annex-B stream (without its start code).
 * Returns the byte count, or 0 if no SEI is present. */
static size_t extract_sei_nal(const uint8_t *buf, size_t n,
                              uint8_t *out, size_t cap)
{
    for (size_t i = 0; i + 3 < n; i++) {
        if (buf[i] != 0 || buf[i + 1] != 0 || buf[i + 2] != 1) continue;
        size_t h = i + 3;                                        /* NAL header */
        if (h >= n || (buf[h] & 0x1f) != PHX_SEI_NAL_TYPE) continue;

        size_t end = n;                                          /* next start code */
        for (size_t j = h + 1; j + 2 < n; j++) {
            if (buf[j] == 0 && buf[j + 1] == 0 && buf[j + 2] == 1) {
                end = j;                                         /* skip trailing 0(s) */
                if (j > h + 1 && buf[j - 1] == 0) end = j - 1;    /* 4-byte start code */
                break;
            }
        }
        size_t len = end - h;
        if (len > cap) return 0;
        memcpy(out, buf + h, len);
        return len;
    }
    return 0;
}

/* The parity CLI settings (tools/parity_harness.py:227-230), expressed on
 * phx_param_t.  x264's own encoder_open clips --min-keyint 30 with
 * --keyint 30 down to 16 (encoder.c:1111); phx_sei_version_write does the
 * same derivation, so we deliberately pass 30 here. */
static void setup_parity_params(phx_param_t *p, const char *preset, int cabac)
{
    phx_param_default(p);                                        /* base.c:344 */
    CHECK(phx_param_apply_preset(p, preset) == 0, "preset applied"); /* base.c:489 */
    p->i_width = TEST_W;
    p->i_height = TEST_H;
    p->b_cabac = cabac;                                          /* x264.c eflag */
    p->i_qp_constant = TEST_QP;                                  /* --qp 28 */
    p->rc_method = PHX_RC_CQP;                                   /* --qp -> CQP */
    p->i_keyint_max = 30;                                        /* --keyint 30 */
    p->i_keyint_min = 30;                                        /* --min-keyint 30 */
    p->i_scenecut_threshold = 0;                                 /* --scenecut 0 */
    p->i_aq_mode = 0;                                            /* CQP: aq=0 */
    p->b_mb_tree = 0;                                            /* CQP: mbtree=0 */
    phx_param_finalize(p);                                       /* encoder.c:1218-1231 */
}

static void check_tokens(const uint8_t *nal, int n, const char *preset)
{
    if (n <= 4) return;
    const char *s = (const char *)nal + 5;   /* after Annex-B + NAL header */
    static const char *common[] = {
        "options: ", "cabac=", " ref=", " deblock=", " analyse=",
        " me=", " subme=", " psy=1 psy_rd=1.00:0.00", " mixed_ref=",
        " me_range=16 chroma_me=1", " trellis=", " 8x8dct=",
        " cqm=0 deadzone=21,11 fast_pskip=", " chroma_qp_offset=",
        " threads=", " lookahead_threads=", " sliced_threads=0",
        " nr=0 decimate=1 interlaced=0 bluray_compat=0 constrained_intra=0",
        " bframes=", " weightp=", " keyint=30 keyint_min=16 scenecut=0",
        " intra_refresh=0 rc=cqp mbtree=0 qp=28 ip_ratio=1.40",
        " aq=0"
    };
    for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++) {
        if (!strstr(s, common[i]))
            fprintf(stderr, "  missing common token [%s]\n", common[i]);
        CHECK(strstr(s, common[i]) != NULL, "expected option token present");
    }

    if (!strcmp(preset, "medium")) {
        const char *medium[] = {
            "cabac=1 ref=3", "analyse=0x3:0x113 me=hex subme=7",
            " trellis=1 8x8dct=1", "chroma_qp_offset=-2",
            " b_pyramid=2 b_adapt=1 b_bias=0 direct=1 weightb=1 open_gop=0",
            " weightp=2", " pb_ratio=1.30"
        };
        for (size_t i = 0; i < sizeof(medium) / sizeof(medium[0]); i++) {
            if (!strstr(s, medium[i]))
                fprintf(stderr, "  missing medium token [%s]\n", medium[i]);
            CHECK(strstr(s, medium[i]) != NULL, "medium-specific token present");
        }
    } else if (!strcmp(preset, "ultrafast")) {
        const char *fast[] = {
            "deblock=0:0:0 analyse=0:0 me=dia subme=0",
            " trellis=0 8x8dct=0", "chroma_qp_offset=0",
            " bframes=0 weightp=0", " ref=1"
        };
        for (size_t i = 0; i < sizeof(fast) / sizeof(fast[0]); i++)
            CHECK(strstr(s, fast[i]) != NULL, "ultrafast-specific token present");
    }
}

int main(int argc, char **argv)
{
    uint8_t nal[SEI_CAP];

    /* --- primary case: medium + cabac, the preset used in the goal --- */
    phx_param_t p;
    setup_parity_params(&p, "medium", 1);
    int n = phx_sei_version_write(nal, (int)sizeof(nal), &p, TEST_W, TEST_H);
    CHECK(n > 0, "SEI NAL written");
    printf("medium/cabac: NAL bytes = %d\n", n);

    if (n >= 8) {
        /* x264 uses a 3-byte start code for SEI (encoder.c:2082-2083). */
        printf("  start code: %02x %02x %02x\n", nal[0], nal[1], nal[2]);
        CHECK(nal[0] == 0 && nal[1] == 0 && nal[2] == 1,
              "Annex-B 3-byte start code");
        CHECK((nal[3] & 0x1f) == PHX_SEI_NAL_TYPE, "NAL type 6 (SEI)");
        CHECK((nal[3] >> 5) == 0, "nal_ref_idc 0 (disposable)");
        printf("  NAL header: %02x (type %u, ref_idc %u)\n", nal[3],
               (unsigned)(nal[3] & 0x1f), (unsigned)(nal[3] >> 5));
        CHECK(nal[4] == 0x05, "SEI payload type 5 (user_data_unregistered)");
        CHECK(nal[5] == 0xff && nal[6] == 0xff && nal[7] == 0x6c,
              "SEI payload size 618 encoded as ff ff 6c");
        printf("  leading bytes after header: %02x %02x %02x %02x %02x\n",
               nal[4], nal[5], nal[6], nal[7], nal[8]);
        check_tokens(nal, n, "medium");
        /* payload starts at 5; type (1) + size (3) + UUID (16) = 20 bytes */
        printf("  option string: %s\n", (const char *)nal + 4 + 1 + 3 + 16);
    }

    /* --- second case: a contrasting preset, structural checks only --- */
    phx_param_t p2;
    uint8_t nal2[SEI_CAP];   /* separate buffer: keep the medium NAL intact */
    setup_parity_params(&p2, "ultrafast", 0);
    int n2 = phx_sei_version_write(nal2, (int)sizeof(nal2), &p2, TEST_W, TEST_H);
    CHECK(n2 > 0, "ultrafast/cavlc SEI NAL written");
    if (n2 > 0) {
        printf("ultrafast/cavlc: NAL bytes = %d\n", n2);
        CHECK((nal2[3] & 0x1f) == PHX_SEI_NAL_TYPE, "ultrafast NAL type 6");
        CHECK(nal2[4] == 0x05, "ultrafast SEI payload type 5");
        check_tokens(nal2, n2, "ultrafast");
    }

    /* --- byte-for-byte comparison with a saved x264 reference stream --- */
    const char *ref_path = find_ref(argc > 1 ? argv[1] : NULL);
    if (!ref_path) {
        fprintf(stderr,
                "note: no saved ref stream found (tried build/parity/"
                "synth-medium-cabac.ref.h264); skipping byte-exact check. "
                "No parity claim made.\n");
    } else {
        FILE *f = fopen(ref_path, "rb");
        CHECK(f != NULL, "open reference stream");
        if (f) {
            CHECK(fseek(f, 0, SEEK_END) == 0, "seek ref");
            long sz = ftell(f);
            CHECK(sz > 0, "ref size");
            CHECK(fseek(f, 0, SEEK_SET) == 0, "rewind ref");
            uint8_t *stream = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
            CHECK(stream != NULL, "allocate ref buffer");
            if (stream && fread(stream, 1, (size_t)sz, f) == (size_t)sz) {
                uint8_t ref_nal[SEI_CAP];
                size_t rn = extract_sei_nal(stream, (size_t)sz,
                                            ref_nal, sizeof(ref_nal));
                printf("reference: %s (SEI NAL bytes = %u)\n", ref_path,
                       (unsigned)rn);
                if (rn == 0) {
                    fprintf(stderr, "note: no SEI NAL in reference stream; "
                                    "skipping byte-exact check.\n");
                } else {
                    size_t got = n > 3 ? (size_t)(n - 3) : 0; /* drop our 3-byte start code */
                    int exact = (got == rn && memcmp(nal + 3, ref_nal, rn) == 0);
                    if (exact) {
                        printf("BYTE-EXACT MATCH vs saved reference SEI "
                               "(%u bytes)\n", (unsigned)rn);
                    } else {
                        size_t i = 0;
                        while (i < got && i < rn && nal[3 + i] == ref_nal[i]) i++;
                        fprintf(stderr,
                                "MISMATCH vs saved reference SEI: got %u bytes, "
                                "ref %u bytes, first diff at %u\n",
                                (unsigned)got, (unsigned)rn, (unsigned)i);
                        fprintf(stderr, "  got[%u] = %02x, ref[%u] = %02x\n",
                                (unsigned)i, (unsigned char)(i < got ? nal[4+i] : 0),
                                (unsigned)i, (unsigned char)(i < rn ? ref_nal[i] : 0));
                        for (size_t k = 0; k < 12 && k < got && k < rn; k++)
                            fprintf(stderr, "%02x ", (unsigned char)nal[4+k]);
                        fprintf(stderr, "\n  ref:");
                        for (size_t k = 0; k < 12 && k < rn; k++)
                            fprintf(stderr, " %02x", (unsigned char)ref_nal[k]);
                        fprintf(stderr, "\n");
                        g_fail++;
                    }
                }
            }
            free(stream);
            fclose(f);
        }
    }

    if (g_fail) {
        fprintf(stderr, "test_phx_sei: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_phx_sei: PASS\n");
    return 0;
}
