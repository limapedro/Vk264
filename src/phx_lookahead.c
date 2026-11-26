/* Phoenix lookahead / slice-type state machine -- serial (SHADER C) port.
 *
 * Every non-trivial function below names the exact x264 lines it follows.
 * The state machine is ported function-for-function; the lowres frame/path
 * costs now come from src/phx_lowres.c (slicetype.c:514-791, :836-995,
 * :1288-1327) with a real full/half/quarter-pel search instead of the old
 * zero-MV placeholder.  MB-tree propagation, trellis B-adapt and VBV lookahead
 * remain explicit TODOs.  No x264 stream parity is claimed.
 *
 * SHADER: A -- the per-8x8 kernels live in phx_lowres.c; this file is the C
 *              serial scheduler around them.
 */
#include "phx_lookahead.h"
#include "phx_lowres.h"

#include <stdlib.h>
#include <string.h>

/* ---- constants ---------------------------------------------------------- */

#define PHX_BFRAME_MAX      16   /* common/base.h:136 (already in phx_frame.h:15) */
#define PHX_LOOKAHEAD_MAX   250  /* common/base.h:140 */
#define PHX_LOOKAHEAD_QP    12   /* common/common.h:63, BIT_DEPTH=8 */
#define PHX_LOWRES_PENALTY  4    /* slicetype.c:542 */
#define PHX_BFRAME_BIAS     0    /* common/base.c:390 (not in phx_param.h: TODO) */
#define PHX_COST_MAX64      (1ULL << 60) /* encoder/me.h:31 */
#define PHX_COST_MAX32      0x7fffffff   /* sentinel for the int cost caches */

/* x264 common/tables.c:97-104 -- lambda = pow(2, qp/6-2) in 8.8 fixed point.
 * Only X264_LOOKAHEAD_QP is read below; the table is reproduced in full so
 * future ports of lowres_context_init() (slicetype.c:45) can use it. */
static const uint16_t phx_lambda_tab[82] = {
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    2,2,2,2,3,3,3,4, 4,4,5,6,6,7,8,9,
    10,11,13,14,16,18,20,23, 25,29,32,36,40,45,51,57,
    64,72,81,91,102,114,128,144, 161,181,203,228,256,287,323,362,
    406,456,512,575,645,724,813,912, 1024,1149,1290,1448,1625,1825,2048,2299,
    2580,2896
};

/* ---- private types ------------------------------------------------------ */

/* The subset of x264_frame_t (common/frame.h:37-140) the slice-type machine
 * touches but which phx_frame.h does not expose.  Kept beside the borrowed
 * phx_frame_t pointer; the cost arrays stay in phx_frame_t and the per-MB MV
 * caches stay in the phx_lowres_t context (see phx_lowres.h). */
typedef struct {
    phx_frame_t *f;
    int i_frame;         /* frame.h:56   presentation frame number */
    int i_type;          /* frame.h:43   current slice-type decision */
    int i_forced_type;   /* frame.h:42   always AUTO through this API */
    int i_bframes;       /* frame.h:65   B frames following this non-B */
    int b_scenecut;      /* frame.h:138  candidate for a real scene cut */
    int i_intra_mbs;     /* frame.h:125  intra MBs in the last P-cost pass */
    int b_intra_calculated; /* frame.h:126 */
    int b_keyframe;      /* frame.h:61 */
    int b_last_minigop_bframe; /* frame.h:66 */
} phx_lf_frame_t;

struct phx_lookahead {
    phx_param_t p;                       /* private copy of the parameters */
    phx_lowres_t *lowres;                /* slicetype.c lowres cost caches */
    int mb_width, mb_height;
    int i_frame_counter;                 /* next presentation number */
    int i_last_keyframe;                 /* lookahead.c:116 */
    int i_keyint_min;                    /* encoder.c:1109-1111 (AUTO resolved) */
    int i_slicetype_length;              /* lookahead.c:118 */

    /* lookahead.c:131-133: ifbuf is merged into next in serial mode. */
    phx_lf_frame_t next[PHX_LOOKAHEAD_MAX+3];
    int next_size;
    phx_lf_frame_t ofbuf[PHX_BFRAME_MAX+3]; /* lookahead.c:212-221 */
    int ofbuf_size;

    /* lookahead.c:59-63; a value, not a queue pointer, so shifting next[] can
     * never leave a dangling last_nonb. */
    phx_lf_frame_t last_nonb;
    int have_last_nonb;
};

#define PHX_MIN(a,b) ((a) < (b) ? (a) : (b))
#define PHX_MAX(a,b) ((a) > (b) ? (a) : (b))

/* ---- lowres cost bridge (SHADER C) -------------------------------------- */

/* slicetype.c:836-995 now lives in phx_lowres.c (a real full/hpel/qpel search);
 * this file only marshals phx_lf_frame_t metadata into a phx_frame_t** array.
 * SHADER: C -- serial pointer marshalling; kernels are SHADER A there. */
static int phx_lh_frame_cost(phx_lookahead_t *l, phx_lf_frame_t **frames,
                              int p0, int p1, int b)
{
    phx_frame_t *lr[PHX_BFRAME_MAX+3] = { NULL };
    int max_i = PHX_MAX(p0, PHX_MAX(p1, b));
    if (max_i >= PHX_BFRAME_MAX+3 || !frames[b]) return PHX_COST_MAX32;
    for (int i = 0; i <= max_i; i++)
        lr[i] = frames[i] ? frames[i]->f : NULL;

    int score = phx_lowres_frame_cost(l->lowres, lr, p0, p1, b);
    if (p1 == b && frames[b]) frames[b]->b_intra_calculated = 1; /* :986 */
    return score;
}


/* x264 encoder/slicetype.c:1288-1327 -- slicetype_path_cost().  `path` is
 * 1-indexed from frames+last_nonb.  The real implementation, including the
 * B-pyramid split at :1310-1318, lives in phx_lowres.c.
 * SHADER: C -- serial prefix-sum over the candidate B/P path. */
static uint64_t phx_lh_path_cost(phx_lookahead_t *l, phx_lf_frame_t **frames,
                                  const char *path, uint64_t threshold)
{
    phx_frame_t *lr[PHX_LOOKAHEAD_MAX+3] = { NULL };
    for (int i = 0; i < PHX_LOOKAHEAD_MAX+3 && frames[i]; i++)
        lr[i] = frames[i]->f;
    return phx_lowres_path_cost(l->lowres, lr, path, threshold);
}

/* ---- scenecut (SHADER C) ------------------------------------------------ */

/* x264 encoder/slicetype.c:1384-1428.
 * SHADER: C -- single scenecut threshold test, serial. */
static int phx_lh_scenecut_internal(phx_lookahead_t *l, phx_lf_frame_t **frames,
                                    int p0, int p1, int real_scenecut)
{
    phx_lf_frame_t *frame = frames[p1];

    /* frame-packed right-view suppression: phx_param_t has no
     * i_frame_packing.  TODO: add it, then port slicetype.c:1388-1390. */
    phx_lh_frame_cost(l, frames, p0, p1, p1);    /* slicetype.c:1392 */

    int icost = frame->f->i_cost_est[0][0];      /* slicetype.c:1394 */
    int pcost = frame->f->i_cost_est[p1-p0][0];  /* slicetype.c:1395 */

    int i_gop_size = frame->i_frame - l->i_last_keyframe; /* :1396 */
    float f_thresh_max = l->p.i_scenecut_threshold / 100.0f; /* :1397 */
    float f_thresh_min = f_thresh_max * 0.25f;   /* slicetype.c:1399 */
    float f_bias;

    /* b_intra_refresh is absent from phx_param_t: treated as 0.
     * TODO: expose it, then restore slicetype.c:1401-1404 verbatim. */
    if (l->p.i_keyint_min == l->p.i_keyint_max)
        f_thresh_min = f_thresh_max;             /* slicetype.c:1401 */
    if (i_gop_size <= l->p.i_keyint_min / 4 || 0 /* b_intra_refresh=0 */)
        f_bias = f_thresh_min / 4;               /* slicetype.c:1403 */
    else if (i_gop_size <= l->p.i_keyint_min)
        f_bias = f_thresh_min * i_gop_size / l->p.i_keyint_min; /* :1405 */
    else
        f_bias = f_thresh_min
               + (f_thresh_max - f_thresh_min)
               * (i_gop_size - l->p.i_keyint_min)
               / (l->p.i_keyint_max - l->p.i_keyint_min);      /* :1409-1412 */

    /* slicetype.c:1415.  Debug logging (:1416-1423) omitted: no x264_log. */
    return pcost >= (1.0f - f_bias) * icost;
}

/* x264 encoder/slicetype.c:1430-1471.
 * SHADER: C -- flash detection followed by the real scenecut test. */
static int phx_lh_scenecut(phx_lookahead_t *l, phx_lf_frame_t **frames,
                           int p0, int p1, int real_scenecut,
                           int num_frames, int i_max_search)
{
    if (real_scenecut && l->p.i_bframe) {
        int origmaxp1 = p0 + 1;                  /* slicetype.c:1435 */
        if (l->p.i_bframe_adaptive == PHX_B_ADAPT_TRELLIS)
            origmaxp1 += l->p.i_bframe;          /* slicetype.c:1437 (TODO) */
        else
            origmaxp1++;                         /* slicetype.c:1439 */
        int maxp1 = PHX_MIN(origmaxp1, num_frames); /* slicetype.c:1441 */

        /* AAAAAABBBAAAAAA -- short flashes are not scene cuts. */
        for (int curp1 = p1; curp1 <= maxp1; curp1++)        /* :1446 */
            if (!phx_lh_scenecut_internal(l, frames, p0, curp1, 0))
                for (int i = curp1; i > p0; i--)             /* :1449 */
                    frames[i]->b_scenecut = 0;

        /* AAAAABBCCDDEEFFFFFF -- the first F becomes the scene cut. */
        for (int curp0 = p0; curp0 <= maxp1; curp0++)        /* :1459 */
            if (origmaxp1 > i_max_search ||
                (curp0 < maxp1 &&
                 phx_lh_scenecut_internal(l, frames, curp0, maxp1, 0)))
                frames[curp0]->b_scenecut = 0;               /* :1463 */
    }

    if (!frames[p1]->b_scenecut) return 0;       /* slicetype.c:1468-1469 */
    return phx_lh_scenecut_internal(l, frames, p0, p1, real_scenecut);
}

/* ---- slicetype analyse (SHADER C) -------------------------------------- */

/* x264 encoder/slicetype.c:1473-1742.  Only the B-adapt FAST branch
 * (slicetype.c:1599-1631) is implemented.  Trellis B-adapt, MB-tree and VBV
 * are left as explicit TODOs rather than being approximated.
 * SHADER: C -- serial analysis over the next[] window. */
static void phx_lh_slicetype_analyse(phx_lookahead_t *l, int intra_minigop)
{
    phx_lf_frame_t *frames[PHX_LOOKAHEAD_MAX+3] = { NULL };
    int num_frames, orig_num_frames, keyint_limit, framecnt;
    int i_max_search = PHX_MIN(l->next_size, PHX_LOOKAHEAD_MAX); /* :1479 */
    /* No b_deterministic field in phx_param_t; TODO: add it and apply the
     * i_slicetype_length cap from slicetype.c:1483-1484. */
    int keyframe = !!intra_minigop;              /* slicetype.c:1485 */

    if (!l->have_last_nonb) return;              /* slicetype.c:1489-1490 */
    frames[0] = &l->last_nonb;                   /* slicetype.c:1491 */
    for (framecnt = 0; framecnt < i_max_search; framecnt++)
        frames[framecnt+1] = &l->next[framecnt];  /* slicetype.c:1492-1493 */

    if (!framecnt) {
        /* TODO: macroblock_tree() (slicetype.c:1497-1499). */
        return;
    }

    keyint_limit = l->p.i_keyint_max - frames[0]->i_frame
                 + l->i_last_keyframe - 1;       /* slicetype.c:1502 */
    orig_num_frames = num_frames = PHX_MIN(framecnt, keyint_limit); /* :1503 */

    /* slicetype.c:1507-1514.  b_intra_refresh is treated as 0. */
    if ((l->p.b_psy && l->p.b_mb_tree))
        num_frames = framecnt;                   /* slicetype.c:1508 */
    else if (l->p.b_open_gop && num_frames < framecnt)
        num_frames++;                            /* slicetype.c:1510 */
    else if (num_frames == 0) {
        frames[1]->i_type = PHX_TYPE_I;          /* slicetype.c:1512-1514 */
        return;
    }

    if (PHX_IS_AUTO_OR_I(frames[1]->i_type) &&  /* slicetype.c:1517 */
        l->p.i_scenecut_threshold &&
        phx_lh_scenecut(l, frames, 0, 1, 1, orig_num_frames, i_max_search)) {
        if (frames[1]->i_type == PHX_TYPE_AUTO)
            frames[1]->i_type = PHX_TYPE_I;      /* slicetype.c:1520 */
        return;                                  /* slicetype.c:1521 */
    }

    /* Forced keyframes -> I/IDR.  No public API sets a forced type, so this
     * is a no-op today; kept for parity with slicetype.c:1525-1530. */
    for (int j = 1; j <= num_frames; j++)
        if (frames[j]->i_type == PHX_TYPE_KEYFRAME)
            frames[j]->i_type = l->p.b_open_gop ? PHX_TYPE_I : PHX_TYPE_IDR;

    /* Close GOP at IDR frames (slicetype.c:1533-1537). */
    for (int j = 2; j <= num_frames; j++)
        if (frames[j]->i_type == PHX_TYPE_IDR &&
            PHX_IS_AUTO_OR_B(frames[j-1]->i_type))
            frames[j-1]->i_type = PHX_TYPE_P;

    int num_analysed_frames = num_frames;        /* slicetype.c:1539 */
    int reset_start;

    if (l->p.i_bframe) {
        if (l->p.i_bframe_adaptive == PHX_B_ADAPT_TRELLIS) {
            /* TODO: slicetype_path()/Viterbi (slicetype.c:1333-1382,
             * :1542-1565).  Deliberately not approximated. */
        } else if (l->p.i_bframe_adaptive == PHX_B_ADAPT_FAST) {
            /* slicetype.c:1567-1619 -- sequential last_nonb handling. */
            int last_nonb = 0;                   /* slicetype.c:1568 */
            int num_bframes = l->p.i_bframe;     /* slicetype.c:1569 */
            char path[PHX_LOOKAHEAD_MAX+1];

            for (int j = 1; j < num_frames; j++) { /* slicetype.c:1571 */
                if (j-1 > 0 && PHX_IS_B(frames[j-1]->i_type)) {
                    num_bframes--;               /* slicetype.c:1573 */
                } else {
                    last_nonb = j-1;             /* slicetype.c:1576 */
                    num_bframes = l->p.i_bframe; /* slicetype.c:1577 */
                }
                if (!num_bframes) {              /* slicetype.c:1579 */
                    if (PHX_IS_AUTO_OR_B(frames[j]->i_type))
                        frames[j]->i_type = PHX_TYPE_P;
                    continue;
                }
                if (frames[j]->i_type != PHX_TYPE_AUTO)
                    continue;                    /* slicetype.c:1583-1584 */
                if (PHX_IS_B(frames[j+1]->i_type)) {
                    frames[j]->i_type = PHX_TYPE_P; /* slicetype.c:1587-1590 */
                    continue;
                }

                int bframes = j - last_nonb - 1; /* slicetype.c:1592 */
                memset(path, 'B', (size_t)bframes);
                memcpy(path + bframes, "PP", 3); /* slicetype.c:1594 */
                uint64_t cost_p = phx_lh_path_cost(l, frames+last_nonb, path,
                                                   PHX_COST_MAX64); /* :1595 */
                memcpy(path + bframes, "BP", 3); /* slicetype.c:1596 */
                uint64_t cost_b = phx_lh_path_cost(l, frames+last_nonb, path,
                                                   cost_p);        /* :1597 */
                frames[j]->i_type = cost_b < cost_p ? PHX_TYPE_B : PHX_TYPE_P;
                                                 /* slicetype.c:1599-1603 */
            }
        } else {
            /* B_ADAPT_NONE (slicetype.c:1605-1622). */
            int num_bframes = l->p.i_bframe;     /* slicetype.c:1606 */
            for (int j = 1; j < num_frames; j++) {
                if (!num_bframes) {
                    if (PHX_IS_AUTO_OR_B(frames[j]->i_type))
                        frames[j]->i_type = PHX_TYPE_P; /* :1609 */
                } else if (frames[j]->i_type == PHX_TYPE_AUTO) {
                    frames[j]->i_type = PHX_IS_B(frames[j+1]->i_type)
                                      ? PHX_TYPE_P : PHX_TYPE_B; /* :1613-1616 */
                }
                if (PHX_IS_B(frames[j]->i_type)) num_bframes--;
                else num_bframes = l->p.i_bframe; /* :1618-1621 */
            }
        }

        if (PHX_IS_AUTO_OR_B(frames[num_frames]->i_type))     /* :1624 */
            frames[num_frames]->i_type = PHX_TYPE_P;

        int num_bframes = 0;                     /* slicetype.c:1627 */
        while (num_bframes < num_frames &&
               PHX_IS_B(frames[num_bframes+1]->i_type))
            num_bframes++;                       /* slicetype.c:1628-1629 */

        /* Check scenecut on the first minigop (slicetype.c:1632-1639). */
        for (int j = 1; j < num_bframes+1; j++) {
            if (frames[j]->i_forced_type == PHX_TYPE_AUTO &&
                PHX_IS_AUTO_OR_I(frames[j+1]->i_forced_type) &&
                l->p.i_scenecut_threshold &&
                phx_lh_scenecut(l, frames, j, j+1, 0,
                                orig_num_frames, i_max_search)) {
                frames[j]->i_type = PHX_TYPE_P;
                num_analysed_frames = j;
                break;
            }
        }
        reset_start = keyframe ? 1 : PHX_MIN(num_bframes+2,
                                             num_analysed_frames+1); /* :1642 */
    } else {
        for (int j = 1; j <= num_frames; j++)    /* slicetype.c:1646-1647 */
            if (PHX_IS_AUTO_OR_B(frames[j]->i_type))
                frames[j]->i_type = PHX_TYPE_P;
        reset_start = !keyframe + 1;             /* slicetype.c:1648 */
    }

    /* TODO: macroblock_tree() propagation (slicetype.c:1674-1677).
     * TODO: vbv_lookahead() (slicetype.c:1732-1734); phx_param_t has no VBV
     * buffer size, so this path is intentionally absent. */

    /* Enforce keyframe limit (slicetype.c:1679-1729). */
    {
        int last_keyframe = l->i_last_keyframe;  /* slicetype.c:1682 */
        int last_possible = 0;                   /* slicetype.c:1683 */
        for (int j = 1; j <= num_frames; j++) {
            phx_lf_frame_t *frm = frames[j];
            int keyframe_dist = frm->i_frame - last_keyframe; /* :1687 */

            if (PHX_IS_AUTO_OR_I(frm->i_forced_type)) {
                /* b_open_gop / intra-refresh conditions simplified; TODO. */
                if (l->p.b_open_gop ||
                    !PHX_IS_B(frames[j-1]->i_forced_type))
                    last_possible = j;           /* slicetype.c:1689-1691 */
            }
            if (keyframe_dist >= l->p.i_keyint_max) {
                if (last_possible != 0 && last_possible != j) {
                    j = last_possible;           /* slicetype.c:1695 */
                    frm = frames[j];
                    keyframe_dist = frm->i_frame - last_keyframe;
                }
                last_possible = 0;
                if (frm->i_type != PHX_TYPE_IDR)
                    frm->i_type = l->p.b_open_gop ? PHX_TYPE_I : PHX_TYPE_IDR;
                                                 /* slicetype.c:1701-1702 */
            }
            if (frm->i_type == PHX_TYPE_I &&
                keyframe_dist >= l->p.i_keyint_min) { /* :1704 */
                if (l->p.b_open_gop)
                    last_keyframe = frm->i_frame; /* :1707 (no bluray compat) */
                else if (frm->i_forced_type != PHX_TYPE_I)
                    frm->i_type = PHX_TYPE_IDR;  /* :1718 */
            }
            if (frm->i_type == PHX_TYPE_IDR) {
                last_keyframe = frm->i_frame;    /* slicetype.c:1721 */
                if (j > 1 && PHX_IS_B(frames[j-1]->i_type))
                    frames[j-1]->i_type = PHX_TYPE_P; /* :1722-1723 */
            }
        }
    }

    /* Restore undecided frames (slicetype.c:1737-1739). */
    for (int j = reset_start; j <= num_frames; j++)
        frames[j]->i_type = frames[j]->i_forced_type;
}

/* ---- slicetype decide (SHADER C) --------------------------------------- */

/* x264 encoder/slicetype.c:1745-1985 -- the final slice-type state machine.
 * The rate-control, duration, reordering-PTS, weighted-P and VRV/second-pass
 * blocks are outside this module's scope and marked TODO below.
 * SHADER: C -- serial, one minigop per call. */
static void phx_lh_slicetype_decide(phx_lookahead_t *l)
{
    if (!l->next_size) return;                   /* slicetype.c:1751 */

    if ((l->p.i_bframe && l->p.i_bframe_adaptive) ||
        l->p.i_scenecut_threshold || l->p.b_mb_tree)
        phx_lh_slicetype_analyse(l, 0);          /* slicetype.c:1783-1787 */

    int bframes = 0, brefs = 0;                  /* slicetype.c:1790 */
    for (;; bframes++) {
        phx_lf_frame_t *frm = &l->next[bframes];
        if (!frm || bframes >= l->next_size) break; /* x264 assumes a full window */

        /* i_forced_type is always AUTO via the public API; the warning at
         * slicetype.c:1795-1802 is therefore unreachable.  TODO: expose
         * forced frame types. */

        /* Normal B-pyramid (base.c:391, i_bframe_pyramid == 2) keeps BREF
         * frames as reference B slices; only strict/low pyramid demotes them
         * (slicetype.c:1805-1824).  phx_param_t has no i_bframe_pyramid, so
         * this port keeps the medium default and never demotes here.  The
         * FAST branch above never emits BREF itself; one is inserted at
         * slicetype.c:1892-1895 after the minigop is decided. */

        if (frm->i_type == PHX_TYPE_KEYFRAME)
            frm->i_type = l->p.b_open_gop ? PHX_TYPE_I : PHX_TYPE_IDR;
                                                 /* slicetype.c:1822 */

        /* Limit GOP size (slicetype.c:1825-1841). */
        if (frm->i_frame - l->i_last_keyframe >= l->p.i_keyint_max) {
            if (frm->i_type == PHX_TYPE_AUTO || frm->i_type == PHX_TYPE_I)
                frm->i_type = (l->p.b_open_gop && l->i_last_keyframe >= 0)
                            ? PHX_TYPE_I : PHX_TYPE_IDR;
            int warn = frm->i_type != PHX_TYPE_IDR;
            if (warn && l->p.b_open_gop)
                warn &= frm->i_type != PHX_TYPE_I;
            if (warn)
                frm->i_type = (l->p.b_open_gop && l->i_last_keyframe >= 0)
                            ? PHX_TYPE_I : PHX_TYPE_IDR;
        }
        if (frm->i_type == PHX_TYPE_I &&
            frm->i_frame - l->i_last_keyframe >= l->p.i_keyint_min) {
            if (l->p.b_open_gop) {
                l->i_last_keyframe = frm->i_frame; /* slicetype.c:1848 */
                /* b_bluray_compat absent from phx_param.h: TODO. */
                frm->b_keyframe = 1;             /* slicetype.c:1851 */
            } else {
                frm->i_type = PHX_TYPE_IDR;      /* slicetype.c:1853 */
            }
        }
        if (frm->i_type == PHX_TYPE_IDR) {
            l->i_last_keyframe = frm->i_frame;   /* slicetype.c:1857 */
            frm->b_keyframe = 1;                 /* slicetype.c:1858 */
            if (bframes > 0) {                   /* close GOP :1859-1862 */
                bframes--;
                l->next[bframes].i_type = PHX_TYPE_P;
            }
        }

        if (bframes == l->p.i_bframe || bframes + 1 >= l->next_size) {
            /* x264's list is NULL-terminated at the live end (slicetype.c:1865-1872);
             * reading a stale slot here lets bframes reach next_size and the
             * reorder shift below memmoves a negative count. */
            /* End of minigop: no more B frames are possible.
             * slicetype.c:1865-1872. */
            if (frm->i_type == PHX_TYPE_AUTO || PHX_IS_B(frm->i_type))
                frm->i_type = PHX_TYPE_P;
        }
        if (frm->i_type == PHX_TYPE_BREF) brefs++; /* slicetype.c:1875 */
        if (frm->i_type == PHX_TYPE_AUTO)
            frm->i_type = PHX_TYPE_B;              /* slicetype.c:1878 */
        else if (!PHX_IS_B(frm->i_type))
            break;                                 /* slicetype.c:1879 */
    }

    if (bframes) l->next[bframes-1].b_last_minigop_bframe = 1; /* :1882 */
    l->next[bframes].i_bframes = bframes;          /* slicetype.c:1883 */

    /* slicetype.c:1892-1895 — insert one BREF into the minigop when B-pyramid
     * is enabled and more than one B frame was decided.  x264's medium
     * default is X264_B_PYRAMID_NORMAL == 2 (base.c:391); phx_param_t has no
     * i_bframe_pyramid field, so this port keeps that default explicitly
     * (TODO: expose i_bframe_pyramid and apply the strict/low demotion at
     * slicetype.c:1810-1824).  BREF is still a B slice in the bitstream, but
     * nal_ref_idc=2 and frame_num advances for it (encoder.c:3546/3882).
     * SHADER: C — serial metadata tag, no new queue or threading state. */
    if (bframes > 1 && !brefs) {                   /* slicetype.c:1892 */
        l->next[(bframes-1)/2].i_type = PHX_TYPE_BREF; /* :1894 */
        brefs++;                                   /* :1895 */
    }

    /* TODO: RC pre-analysis (slicetype.c:1886-1915), weighted P (:1918-1924),
     * duration/CPB fields (:1927-1946), and b_stat_read (:1789). */

    /* Shift to coded order (slicetype.c:1950-1964).  The B frames are moved
     * behind the P frame that closes the minigop; i_reordered_pts (:1955)
     * is not representable in phx_frame_t and is omitted. */
    if (bframes) {
        phx_lf_frame_t tmp[PHX_BFRAME_MAX+2];
        int idx_list[] = { brefs+1, 1 };          /* slicetype.c:1953 */
        for (int i = 0; i < bframes; i++) {
            int idx = idx_list[l->next[i].i_type == PHX_TYPE_BREF]++;
            tmp[idx] = l->next[i];                /* slicetype.c:1957-1958 */
        }
        tmp[0] = l->next[bframes];                /* slicetype.c:1960 */
        memcpy(l->next, tmp, (size_t)(bframes+1) * sizeof(phx_lf_frame_t));
                                                  /* slicetype.c:1962 */
    }
}

/* ---- public API (SHADER C) --------------------------------------------- */

/* x264 encoder/lookahead.c:110-150 (x264_lookahead_init), serial subset.
 * SHADER: C -- serial lookahead construction. */
phx_lookahead_t *phx_lookahead_new(const phx_param_t *p, int mb_width,
                                   int mb_height)
{
    if (!p || mb_width <= 0 || mb_height <= 0 ||
        mb_width > 2*PHX_LOOKAHEAD_MAX || mb_height > 2*PHX_LOOKAHEAD_MAX)
        return NULL;

    phx_lookahead_t *l = calloc(1, sizeof(*l));  /* lookahead.c:113 */
    if (!l) return NULL;
    l->p = *p;
    /* B-pyramid normal is x264's medium default (base.c:391); phx_param_t has
     * no i_bframe_pyramid field, so the cost path uses that default too. */
    /* x264 resolves param.analyse.i_mv_range from the H.264 level at
     * encoder.c:1265-1268 (tables.c:29-42): a 64x64 frame selects level 1.0
     * with mv_range=64, not base.c:443's raw default of 16.  phx_param_t has
     * no resolved field, so derive it from the macroblock count here.
     * TODO: expose resolved i_mv_range in phx_param_finalize() and pass it. */
    int frame_mbs = mb_width * mb_height;
    int mv_range = frame_mbs <= 99   ? 64   : /* tables.c:31 (level 1.0) */
                   frame_mbs <= 396 ? 128  : /* tables.c:33 (level 1.1) */
                   frame_mbs <= 792 ? 256  : /* tables.c:37 (level 2.1) */
                   frame_mbs <= 1620? 256  : /* tables.c:38 (level 2.2) */
                                       512;  /* tables.c:40 (level 3.1) */
    l->lowres = phx_lowres_new(mb_width, mb_height, mv_range,
                               p->i_bframe, 1, PHX_BFRAME_BIAS);
    if (!l->lowres) { free(l); return NULL; }  /* slicetype.c:836 cost state */
    phx_lowres_set_mbtree(l->lowres, p->b_mb_tree); /* slicetype.c:823 */
    l->mb_width = mb_width;
    l->mb_height = mb_height;
    l->i_last_keyframe = -p->i_keyint_max;       /* lookahead.c:116 */
    l->i_slicetype_length = PHX_MAX(1, PHX_MIN(p->i_lookahead,
                                              PHX_LOOKAHEAD_MAX)); /* :118 */

    /* X264_KEYINT_MIN_AUTO: encoder.c:1109-1111 uses fps, which
     * phx_param_t does not carry.  phx_param.h:33 documents the Phoenix
     * substitute min(keyint_max/10, bframes+1), clipped as x264 does.
     * TODO: expose fps/i_keyint_min resolution in phx_param_finalize(). */
    if (p->i_keyint_min < 0) {
        int auto_min = PHX_MIN(p->i_keyint_max / 10, p->i_bframe + 1);
        auto_min = PHX_MAX(1, PHX_MIN(auto_min, p->i_keyint_max/2 + 1));
        l->i_keyint_min = auto_min;
    } else {
        l->i_keyint_min = p->i_keyint_max ? p->i_keyint_min : 1;
    }
    return l;
}

/* x264 encoder/lookahead.c:192-197 (x264_lookahead_put_frame), serial path.
 * SHADER: C -- serial lookahead queue append. */
void phx_lookahead_put_frame(phx_lookahead_t *l, phx_frame_t *f)
{
    if (!l || !f || l->next_size >= PHX_LOOKAHEAD_MAX+3) return;

    phx_lf_frame_t *lf = &l->next[l->next_size++];
    lf->f = f;
    lf->i_frame = l->i_frame_counter++;           /* frame.h:56 */
    lf->i_type = PHX_TYPE_AUTO;                  /* x264.h:272 */
    lf->i_forced_type = PHX_TYPE_AUTO;
    lf->i_bframes = 0;                           /* frame.h:65 */
    lf->b_scenecut = 1;                          /* frame.h:138 */
    lf->b_intra_calculated = 0;                  /* frame.h:126 */
    lf->i_intra_mbs = 0;                         /* frame.h:125 */
    lf->b_keyframe = 0;                          /* frame.h:61 */
    lf->b_last_minigop_bframe = 0;               /* frame.h:66 */

    phx_frame_init_lowres(f);                    /* common/mc.c:458 */
    /* mc.c:473-481 invalidates frame.h:123/126/103 caches; mirror that in the
     * per-MB lowres caches owned by phx_lowres.c. */
    phx_lowres_frame_reset(l->lowres, f);
}

/* lookahead.c:42-57 (lookahead_shift) followed by :59-63
 * (lookahead_update_last_nonb).  SHADER: C -- serial queue shift. */
static void phx_lh_shift_to_ofbuf(phx_lookahead_t *l, int count)
{
    while (count-- > 0) {
        l->ofbuf[l->ofbuf_size++] = l->next[0];  /* lookahead.c:48 */
        memmove(&l->next[0], &l->next[1],
                (size_t)(l->next_size-1) * sizeof(phx_lf_frame_t));
        l->next_size--;                          /* lookahead.c:49 */
    }
}

/* x264 encoder/lookahead.c:223-254, serial branch (:234-252).
 * The threaded ifbuf path (:225-232) does not exist in the serial port.
 * Returns the raw decided types (including the BREF inserted at
 * slicetype.c:1894) and, optionally, each frame's display-order i_frame
 * (frame.h:56).  SHADER: C -- serial lookahead output, one minigop. */
int phx_lookahead_get_frames_full(phx_lookahead_t *l, int *frame_types,
                                  int *frame_numbers, int n)
{
    if (!l || n < 0 || (!frame_types && n > 0)) return 0;

    if (!l->ofbuf_size && l->next_size) {        /* lookahead.c:235 */
        phx_lh_slicetype_decide(l);              /* lookahead.c:239 */

        l->last_nonb = l->next[0];               /* lookahead.c:59-63 */
        l->have_last_nonb = 1;
        int shift_frames = l->next[0].i_bframes + 1; /* lookahead.c:240 */
        phx_lh_shift_to_ofbuf(l, shift_frames);  /* lookahead.c:241 */

        /* For MB-tree/VBV the I-frame minigop is analysed again after the
         * shift.  TODO: x264_slicetype_analyse(shift_frames) -- requires the
         * MB-tree/VBV work above.  lookahead.c:244-246. */
    }

    /* lookahead_encoder_shift(): emit the decided frames in order. */
    int count = PHX_MIN(n, l->ofbuf_size);       /* lookahead.c:212-221 */
    for (int i = 0; i < count; i++) {
        if (frame_types) frame_types[i] = l->ofbuf[i].i_type;
        if (frame_numbers) frame_numbers[i] = l->ofbuf[i].i_frame; /* frame.h:56 */
    }
    if (count && frame_types) {
        if (count < l->ofbuf_size)
            memmove(&l->ofbuf[0], &l->ofbuf[count],
                    (size_t)(l->ofbuf_size-count) * sizeof(phx_lf_frame_t));
        l->ofbuf_size -= count;
    }
    return count;
}

/* Raw types without display numbers (encoder.c:3447 call site).
 * SHADER: C -- forwards to the full serial lookahead output. */
int phx_lookahead_get_frames_ex(phx_lookahead_t *l, int *frame_types, int n)
{
    return phx_lookahead_get_frames_full(l, frame_types, NULL, n);
}

/* Backward-compatible view of the decided types: BREF is reported as B,
 * matching phx_lookahead.h:46-47 (B/BREF -> PHX_SLICE_B).  The encoder uses
 * phx_lookahead_get_frames_full() instead so it can keep the reference flag
 * and the display-order frame number.
 * SHADER: C -- serial type mapping, no state change. */
int phx_lookahead_get_frames(phx_lookahead_t *l, int *frame_types, int n)
{
    int got = phx_lookahead_get_frames_full(l, frame_types, NULL, n);
    for (int i = 0; i < got; i++)
        if (frame_types && frame_types[i] == PHX_TYPE_BREF)
            frame_types[i] = PHX_TYPE_B;           /* x264.h:276/280 */
    return got;
}

/* x264 encoder/lookahead.c:177-187.  SHADER: C -- serial teardown. */
void phx_lookahead_delete(phx_lookahead_t *l)
{
    if (!l) return;
    phx_lowres_delete(l->lowres);                /* lookahead.c:177-187 */
    free(l);                                     /* lookahead.c:186 */
}