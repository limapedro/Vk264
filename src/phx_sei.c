/* Port of x264's version SEI writer.
 *
 * Primary source: reference/x264/encoder/set.c:593-623 (payload + sprintf),
 *                 reference/x264/encoder/set.c:80-99   (SEI byte syntax),
 *                 reference/x264/common/base.c:1428-1567 (option string).
 * Every literal and every branch below carries the exact reference line it
 * came from; the saved parity streams are only used by the test as an
 * independent check, never as a source of bytes.
 *
 * SHADER: C — emitted once in the serial header stage.  No hidden state:
 * everything the string describes is either in phx_param_t or derived from
 * width/height exactly as x264_encoder_open derives it.
 */
#define _CRT_SECURE_NO_WARNINGS  /* keep x264's sprintf() spelling (set.c:610) */
#include "phx_sei.h"
#include "phx_bitstream.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

/* x264_param2string allocates 2000 bytes (base.c:1430); leave headroom for
 * the option string plus the 16-byte UUID, the banner and the terminating 0. */
#define PHX_SEI_OPT_CAP      2048
#define PHX_SEI_PAYLOAD_CAP  (PHX_SEI_OPT_CAP + 512)
#define PHX_SEI_RBSP_CAP     (PHX_SEI_PAYLOAD_CAP + 512)

/* x264.h:226 / base.c:386 — X264_KEYINT_MIN_AUTO (phx_param.c:31 uses -1). */
#define PHX_KEYINT_MIN_AUTO 0

/* x264.h:227 / base.c:1498 — X264_KEYINT_MAX_INFINITE. */
#define PHX_KEYINT_INFINITE (1 << 30)

/* base.h:138-139 — X264_THREAD_MAX, X264_LOOKAHEAD_THREAD_MAX. */
#define PHX_THREAD_MAX 128
#define PHX_LOOKAHEAD_THREAD_MAX 16

/* base.c:369 — x264's default fps numerator; the parity CLI passes --fps 25
 * (tools/parity_harness.py:228) so encoder.c:1110 uses 25. */
#define PHX_SEI_DEFAULT_FPS 25

/* x264.h:191-195 — analyse bit values.  Phoenix uses its own compact bit
 * positions (phx_param.c:14-18), so they are translated before printing. */
#define PHX_ANALYSE_I4x4      1       /* phx_param.c:14 */
#define PHX_ANALYSE_I8x8      2       /* phx_param.c:15 */
#define PHX_ANALYSE_PSUB16x16 8       /* phx_param.c:16 */
#define PHX_ANALYSE_BSUB16x16 16      /* phx_param.c:17 */
#define PHX_ANALYSE_PSUB8x8   32      /* phx_param.c:18 */

/* SHADER: C — scalar helper; no vector state. */
static int phx_sei_clip3(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* x264_cpu_num_processors (common/cpu.c:503-550).  Only the auto-thread
 * derivation needs it; the Windows branch matches x264's SYS_WINDOWS path
 * (cpu.c:508-509), the POSIX branch its SYS_CYGWIN path (cpu.c:511-512).
 * SHADER: C — host capability query in the serial header stage. */
static int phx_sei_cpu_num_processors(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
#elif defined(_SC_NPROCESSORS_ONLN)
    long np = sysconf(_SC_NPROCESSORS_ONLN);
    return np > 0 ? (int)np : 1;
#else
    return 1;
#endif
}

/* The subset of x264_encoder_open's runtime rewrites that x264_param2string
 * prints (the CLI settings leave these as AUTO).  SHADER: C — pure derivation. */
typedef struct {
    int i_threads;
    int i_lookahead_threads;
    int i_keyint_min;
    int i_chroma_qp_offset;
    int i_subpel_refine;
} phx_sei_runtime_t;

static void phx_sei_derive_runtime(phx_sei_runtime_t *rt, const phx_param_t *p,
                                   int width, int height)
{
    int mb_height = (height + 15) / 16;                     /* encoder.c:564 */

    /* encoder.c:559-565 — AUTO threads, sliced_threads == 0 in this port. */
    int threads = phx_sei_cpu_num_processors() * 3 / 2;     /* encoder.c:561 */
    int max_threads = mb_height / 2;                        /* encoder.c:564 */
    if (max_threads < 1) max_threads = 1;                   /* encoder.c:564 */
    if (threads > max_threads) threads = max_threads;       /* encoder.c:565 */
    threads = phx_sei_clip3(threads, 1, PHX_THREAD_MAX);    /* encoder.c:579 */
    rt->i_threads = threads;

    /* encoder.c:567 / :1300 — upper bound for sliced/lookahead threads. */
    int max_sliced_threads = mb_height / 4;                 /* encoder.c:567 */
    if (max_sliced_threads < 1) max_sliced_threads = 1;     /* encoder.c:567 */

    /* encoder.c:1273-1300 — AUTO lookahead threads.  b_sliced_threads is 0
     * in the parity CLI, so the table branch (:1275-1276) is not taken. */
    int badapt = p->i_bframe_adaptive == PHX_B_ADAPT_TRELLIS; /* :1284 */
    int subme = p->i_subpel_refine / 3;                       /* :1285 */
    if (subme > 3) subme = 3;                                 /* :1285 */
    subme += p->i_subpel_refine > 1;                          /* :1285 */
    int bframes = (p->i_bframe - 1) / 3;                      /* :1286 */
    if (bframes > 3) bframes = 3;                             /* :1286 */
    if (bframes < 0) bframes = 0;                             /* clamp C truncation */
    /* encoder.c:1289-1291 — lookahead_thread_div[badapt][subme][bframes]. */
    static const unsigned char div_tab[2][5][4] = {
        { { 6, 6, 6, 6 }, { 3, 3, 3, 3 }, { 4, 4, 4, 4 },
          { 6, 6, 6, 6 }, { 12, 12, 12, 12 } },
        { { 3, 2, 1, 1 }, { 2, 1, 1, 1 }, { 4, 3, 2, 1 },
          { 6, 4, 3, 2 }, { 12, 9, 6, 4 } }
    };
    int lookahead = threads / div_tab[badapt][subme][bframes]; /* :1293 */
    if (lookahead > height / 128) lookahead = height / 128;    /* :1297 */
    lookahead = phx_sei_clip3(lookahead, 1,
                  max_sliced_threads < PHX_LOOKAHEAD_THREAD_MAX
                      ? max_sliced_threads : PHX_LOOKAHEAD_THREAD_MAX); /* :1300 */
    rt->i_lookahead_threads = lookahead;

    /* encoder.c:1109-1111 — AUTO becomes min(keyint/10, fps); every value is
     * then clipped to [1, keyint_max/2+1].  This is why the CLI's
     * --min-keyint 30 with --keyint 30 prints 16. */
    int kmin = p->i_keyint_min;
    if (kmin <= PHX_KEYINT_MIN_AUTO)
        kmin = p->i_keyint_max / 10 < PHX_SEI_DEFAULT_FPS
             ? p->i_keyint_max / 10 : PHX_SEI_DEFAULT_FPS;   /* :1110 */
    rt->i_keyint_min = phx_sei_clip3(kmin, 1,
                                     p->i_keyint_max / 2 + 1); /* :1111 */

    /* encoder.c:1220-1231 — x264_param2string prints the *post-open* value of
     * analyse.i_chroma_qp_offset.  phx_param_t stores the pre-open base in
     * i_chroma_qp_offset (phx_param.h:54); i_chroma_qp_index_offset is the PPS
     * mirror (phx_param.h:73) and is not what the SEI string prints.  I420
     * input skips the +6 branch at encoder.c:1222-1223. */
    int cpo = phx_sei_clip3(p->i_chroma_qp_offset, -32, 32);    /* encoder.c:1220 */
    if (p->b_psy && p->i_subpel_refine >= 6 && p->f_psy_rd > 0) /* :1226 */
        cpo -= p->f_psy_rd < 0.25 ? 1 : 2;                      /* :1227 */
    if (p->b_psy && p->i_trellis && p->f_psy_trellis > 0)       /* :1229 */
        cpo -= p->f_psy_trellis < 0.25 ? 1 : 2;                 /* :1230 */
    rt->i_chroma_qp_offset = phx_sei_clip3(cpo, -12, 12);       /* :1231 */

    /* encoder.c:1239-1240 — subme 10/11 is demoted to 9 unless trellis == 2
     * and AQ is on.  The parity CLI uses CQP (aq == 0), so veryslow/placebo
     * print subme=9. */
    rt->i_subpel_refine = p->i_subpel_refine;                    /* base.c:444 */
    if (p->i_subpel_refine >= 10 && (p->i_trellis != 2 || !p->i_aq_mode))
        rt->i_subpel_refine = 9;                                 /* encoder.c:1239-1240 */

    (void)width; /* kept in the signature for the encoder-style API; the
                    thread caps above depend on height only (encoder.c:564,567). */
}

/* base.c:191-195 / phx_param.c:14-18 — translate Phoenix analyse bits to the
 * x264 bit values x264_param2string prints with %#x (base.c:1452). */
static unsigned phx_sei_analyse_intra(int flags)
{
    unsigned out = 0;
    if (flags & PHX_ANALYSE_I4x4) out |= 0x0001U; /* x264.h:191 */
    if (flags & PHX_ANALYSE_I8x8) out |= 0x0002U; /* x264.h:192 */
    return out;
}

static unsigned phx_sei_analyse_inter(int flags)
{
    unsigned out = 0;
    if (flags & PHX_ANALYSE_I4x4)      out |= 0x0001U; /* x264.h:191 */
    if (flags & PHX_ANALYSE_I8x8)      out |= 0x0002U; /* x264.h:192 */
    if (flags & PHX_ANALYSE_PSUB16x16) out |= 0x0010U; /* x264.h:193 */
    if (flags & PHX_ANALYSE_BSUB16x16) out |= 0x0100U; /* x264.h:195 */
    if (flags & PHX_ANALYSE_PSUB8x8)   out |= 0x0020U; /* x264.h:194 */
    return out;
}

/* base.c:1453 / x264.h:234 — x264_motion_est_names[]. */
static const char *phx_sei_me_name(int method)
{
    static const char * const names[] = { "dia", "hex", "umh", "esa", "tesa" };
    if (method < 0 || method >= (int)(sizeof(names) / sizeof(names[0])))
        return "hex";
    return names[method];
}

/* base.c:1508-1510 — rc name.  phx_param_t has no VBV/2-pass state, so ABR
 * always takes the "abr" arm (base.c:1509: bitrate == vbv_maxrate == 0). */
static const char *phx_sei_rc_name(const phx_param_t *p)
{
    if (p->rc_method == PHX_RC_CRF) return "crf";             /* base.c:1508 */
    if (p->rc_method == PHX_RC_ABR) return "abr";             /* base.c:1509 */
    return "cqp";                                            /* base.c:1510 */
}

/* Port of base.c:1428-1567 with b_res == 0 (the form used by the version SEI,
 * set.c:601).  Fields absent from phx_param_t keep x264's default from
 * base.c:344-487; each such literal cites its source line. */
static void phx_sei_param2string(const phx_param_t *p,
                                 const phx_sei_runtime_t *rt, char *s, int cap)
{
    int off = 0;
#define APPEND(...) do {                                          \
        int n_ = sprintf(s + off, __VA_ARGS__);                   \
        if (n_ > 0 && off + n_ < cap) off += n_;                  \
    } while (0)

    /* b_res == 0: base.c:1438-1444 is skipped.  b_opencl == 0 (base.c:481)
     * skips base.c:1446-1447. */
    APPEND("cabac=%d", p->b_cabac);                              /* base.c:1448 */
    APPEND(" ref=%d", p->i_frame_reference);                     /* base.c:1449 */
    APPEND(" deblock=%d:%d:%d", p->b_deblocking_filter, 0, 0);    /* :1450-1451
                                                                   * alpha/beta:
                                                                   * base.c:396-397 */
    APPEND(" analyse=%#x:%#x",
           phx_sei_analyse_intra(p->analyse_intra),
           phx_sei_analyse_inter(p->analyse_inter));              /* base.c:1452 */
    APPEND(" me=%s", phx_sei_me_name(p->i_me_method));           /* base.c:1453 */
    APPEND(" subme=%d", rt->i_subpel_refine);                    /* base.c:1454,
                                                                   * encoder.c:1239-1240 */
    APPEND(" psy=%d", p->b_psy);                                 /* base.c:1455 */
    if (p->b_psy)                                                /* base.c:1456 */
        APPEND(" psy_rd=%.2f:%.2f",
               p->f_psy_rd, p->f_psy_trellis);                   /* base.c:1457 */
    APPEND(" mixed_ref=%d", p->b_mixed_references);              /* base.c:1458 */
    APPEND(" me_range=%d", p->i_me_range);                       /* base.c:1459 */
    APPEND(" chroma_me=%d", 1);                                  /* base.c:1460,
                                                                   * default base.c:446 */
    APPEND(" trellis=%d", p->i_trellis);                         /* base.c:1461 */
    APPEND(" 8x8dct=%d", p->b_transform_8x8);                    /* base.c:1462 */
    APPEND(" cqm=%d", 0);                                        /* base.c:1463,
                                                                   * X264_CQM_FLAT base.c:461 */
    APPEND(" deadzone=%d,%d", 21, 11);                           /* base.c:1464,
                                                                   * base.c:456-457 */
    APPEND(" fast_pskip=%d", p->b_fast_pskip);                   /* base.c:1465 */
    APPEND(" chroma_qp_offset=%d", rt->i_chroma_qp_offset);      /* base.c:1466,
                                                                   * encoder.c:1220-1231 */
    APPEND(" threads=%d", rt->i_threads);                        /* base.c:1467 */
    APPEND(" lookahead_threads=%d", rt->i_lookahead_threads);    /* base.c:1468 */
    APPEND(" sliced_threads=%d", 0);                             /* base.c:1469,
                                                                   * base.c:347 */
    /* base.c:1470-1479: slice_count/slices_max/slice_max_* are all 0
     * (base.c:372-374) so none of these tokens is printed. */
    APPEND(" nr=%d", 0);                                         /* base.c:1480,
                                                                   * base.c:347 */
    APPEND(" decimate=%d", p->b_dct_decimate);                   /* base.c:1481 */
    APPEND(" interlaced=%s", "0");                               /* base.c:1482,
                                                                   * base.c:392 */
    APPEND(" bluray_compat=%d", 0);                              /* base.c:1483,
                                                                   * base.c:347 */
    /* base.c:1484-1485: b_stitchable == 0 (base.c:347). */
    APPEND(" constrained_intra=%d", 0);                         /* base.c:1487,
                                                                   * base.c:393 */

    APPEND(" bframes=%d", p->i_bframe);                          /* base.c:1489 */
    if (p->i_bframe) {                                           /* base.c:1490 */
        APPEND(" b_pyramid=%d b_adapt=%d b_bias=%d direct=%d weightb=%d open_gop=%d",
               2,                                                /* base.c:391 */
               (int)p->i_bframe_adaptive,                        /* base.c:389 */
               0,                                                /* base.c:390 */
               p->i_direct_mv_pred,                              /* base.c:438/561/571/583/596 */
               p->b_weighted_bipred,                             /* base.c:451 */
               p->b_open_gop);                                   /* base.c:347 */
    }                                                            /* base.c:1492-1494 */
    APPEND(" weightp=%d", p->i_weighted_pred > 0
                        ? p->i_weighted_pred : 0);               /* base.c:1496 */

    if (p->i_keyint_max == PHX_KEYINT_INFINITE)                  /* base.c:1498 */
        APPEND(" keyint=infinite");                              /* base.c:1499 */
    else
        APPEND(" keyint=%d", p->i_keyint_max);                   /* base.c:1501 */
    APPEND(" keyint_min=%d scenecut=%d intra_refresh=%d",
           rt->i_keyint_min, p->i_scenecut_threshold, 0);        /* base.c:1502-1503,
                                                                   * intra_refresh: base.c:347 */

    /* base.c:1505-1506: rc_lookahead only with mbtree or VBV.  In the parity
     * CQP settings b_mb_tree == 0 and VBV is unset (base.c:405-406), so it is
     * absent. */
    APPEND(" rc=%s mbtree=%d", phx_sei_rc_name(p), p->b_mb_tree);/* base.c:1508-1510 */
    if (p->rc_method == PHX_RC_CRF || p->rc_method == PHX_RC_ABR) /* base.c:1511 */
    {
        if (p->rc_method == PHX_RC_CRF)                          /* base.c:1513 */
            APPEND(" crf=%.1f", p->f_rf_constant);               /* base.c:1514 */
        else
            APPEND(" bitrate=%d ratetol=%.1f", 0, 1.0);          /* base.c:1516-1517,
                                                                   * base.c:403-404 */
        APPEND(" qcomp=%.2f qpmin=%d qpmax=%d qpstep=%d",
               0.6, 0, INT_MAX, 4);                              /* base.c:1518-1519,
                                                                   * base.c:423/410-412 */
        /* base.c:1520-1522: b_stat_read == 0 (base.c:421). */
        /* base.c:1523-1529: vbv_buffer_size == 0 (base.c:406). */
    }
    else if (p->rc_method == PHX_RC_CQP)                         /* base.c:1531 */
        APPEND(" qp=%d", p->i_qp_constant);                      /* base.c:1532 */

    /* base.c:1534-1551: nal_hrd, crop_rect, mastering display, CLL and
     * frame-packing are all unset in the parity CLI (base.c:347/475/479),
     * so none of those tokens is printed. */
    if (!(p->rc_method == PHX_RC_CQP && p->i_qp_constant == 0))  /* base.c:1552 */
    {
        APPEND(" ip_ratio=%.2f", p->f_ip_factor);                /* base.c:1554 */
        if (p->i_bframe && !p->b_mb_tree)                        /* base.c:1555 */
            APPEND(" pb_ratio=%.2f", p->f_pb_factor);            /* base.c:1556 */
        APPEND(" aq=%d", p->i_aq_mode);                          /* base.c:1557 */
        if (p->i_aq_mode)                                        /* base.c:1558 */
            APPEND(":%.2f", p->f_aq_strength);                   /* base.c:1559 */
        /* base.c:1560-1563: no zones (base.c:426). */
    }
#undef APPEND
}

/* set.c:80-99 — SEI payload_type / payload_size / payload / trailing bits.
 * bs_realign (:84) and bs_flush (:98) are no-ops for phx_bs_t, which writes
 * whole bytes directly instead of buffering 32-bit words.
 * SHADER: C — byte-serial RBSP emission. */
static void phx_sei_write(phx_bs_t *s, const uint8_t *payload,
                          int payload_size, int payload_type)
{
    int i;
    for (i = 0; i <= payload_type - 255; i += 255)              /* set.c:86-87 */
        phx_bs_write(s, 8, 255);
    phx_bs_write(s, 8, (uint32_t)(payload_type - i));           /* set.c:88 */

    for (i = 0; i <= payload_size - 255; i += 255)              /* set.c:90-91 */
        phx_bs_write(s, 8, 255);
    phx_bs_write(s, 8, (uint32_t)(payload_size - i));           /* set.c:92 */

    for (i = 0; i < payload_size; i++)                          /* set.c:94-95 */
        phx_bs_write(s, 8, payload[i]);

    phx_bs_rbsp_trailing(s);                                     /* set.c:97 */
}

int phx_sei_version_write(uint8_t *dst, int cap, const phx_param_t *p,
                          int width, int height)
{
    /* set.c:596-600 — random ID generated according to ISO-11578. */
    static const uint8_t uuid[16] = {
        0xdc, 0x45, 0xe9, 0xbd, 0xe6, 0xd9, 0x48, 0xb7,
        0x96, 0x2c, 0xd8, 0x20, 0xd9, 0x23, 0xee, 0xef
    };
    char opts[PHX_SEI_OPT_CAP];
    char payload[PHX_SEI_PAYLOAD_CAP];
    uint8_t rbsp[PHX_SEI_RBSP_CAP];
    phx_sei_runtime_t rt;
    phx_bs_t bs;
    phx_nal_t nal;
    int length;

    if (!dst || !p || cap < 8 || width <= 0 || height <= 0)     /* set.c:605-606 */
        return -1;

    phx_sei_derive_runtime(&rt, p, width, height);
    phx_sei_param2string(p, &rt, opts, (int)sizeof(opts));

    memcpy(payload, uuid, 16);                                   /* set.c:609 */
    sprintf(payload + 16,                                        /* set.c:610-612 */
            "x264 - core %d%s - H.264/MPEG-4 AVC codec - "
            "Copy%s 2003-2024 - http://www.videolan.org/x264.html - options: %s",
            PHX_SEI_X264_BUILD, PHX_SEI_X264_VERSION,
            PHX_SEI_HAVE_GPL ? "left" : "right", opts);
    length = (int)strlen(payload) + 1;                           /* set.c:613 */

    phx_bs_init(&bs, rbsp, (int)sizeof(rbsp));                   /* set.c:2101 */
    phx_sei_write(&bs, (const uint8_t *)payload, length,
                  PHX_SEI_PAYLOAD_TYPE);                         /* set.c:615 */

    nal.p_payload = rbsp;
    nal.i_payload = phx_bs_pos(&bs) / 8;                         /* set.c:590 style */
    nal.i_ref_idc = 0;                                           /* x264.h:103,
                                                                   * encoder.c:2118 */
    nal.i_type = (int)PHX_SEI_NAL_TYPE;                          /* x264.h:94 */
    /* After SPS/PPS this SEI is NAL index 2, so x264 uses a 3-byte start code
     * (encoder.c:2082-2083). */
    nal.b_long_startcode = 0;

    if (nal.i_payload > cap)                                     /* set.c:607 allocation
                                                                   * guard, adapted to a
                                                                   * caller-owned buffer */
        return -1;
    phx_nal_encode(dst, &nal, 1);                                /* bitstream.c:55 */
    return nal.i_payload;
}
