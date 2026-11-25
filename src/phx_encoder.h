/* Phoenix public encoder API — an explicit struct-based mirror of x264's
 * encoder entry points.
 *
 * Source: reference/x264/encoder/api.c:76   (x264_encoder_open dispatch),
 *         reference/x264/encoder/api.c:159  (x264_encoder_headers dispatch),
 *         reference/x264/encoder/api.c:166  (x264_encoder_encode dispatch),
 *         reference/x264/encoder/api.c:130  (x264_encoder_close dispatch);
 *         reference/x264/encoder/encoder.c:1507 (x264_encoder_open core),
 *         :1579/:1581 (sps/pps init), :1590-1592 (mb dimensions),
 *         :1669 (cabac init), :1708 (cost init), :2096 (headers),
 *         :3323 (encode), :3358-3454 (put_frame / lookahead),
 *         :2855-2892 (slice/MB loop), :3857/:3978 (rate control),
 *         :4196/:4520 (close / cost free).
 *
 * Naming note: x264_encoder_headers' SPS/PPS byte assembly already exists as
 * phx_encoder_headers() in phx_headers.h:53.  The handle-based public mirror
 * is therefore phx_headers() here; it delegates to that function so there is
 * one header assembler.  phx_encoder_open/encode/close are the direct mirrors
 * and phx_open/phx_encode/phx_close are the short aliases from
 * docs/ORACLE_DESIGN.md:40-45.
 *
 * SHADER: C — every entry point drives the serial, single-threaded host
 * pipeline; no hidden thread or GPU state is introduced here.
 */
#ifndef PHX_ENCODER_H
#define PHX_ENCODER_H

#include <stdint.h>
#include "phx_param.h"
#include "phx_bitstream.h"
#include "phx_headers.h"
#include "phx_frame.h"
#include "phx_mbcache.h"
#include "phx_cost.h"
#include "phx_cabac.h"

/* Legacy sentinel kept only so old callers still compile: phx_encode() no
 * longer returns it once the per-MB pipeline is wired.  Source: the old
 * "no data out" envelope at encoder.c:3354-3356. */
#define PHX_ERR_UNIMPLEMENTED (-2)

/* FDEC_STRIDE (common/common.h:571).  The reconstructed plane uses the same
 * stride so phx_mb_cache_t.p_fdec can be indexed exactly like x264's. */
#define PHX_FDEC_STRIDE 32

/* One raw input picture.  x264_picture_t's I420 planes, reduced to the fields
 * the ported path uses.  Source: reference/x264/x264.h:862-904 (x264_picture_t)
 * and reference/x264/input/raw.c:113-124 (plane layout).
 * SHADER: C — host-side view over caller-owned memory. */
typedef struct {
    uint8_t *plane[3];              /* Y, U, V (I420) */
    int i_stride[3];                /* plane strides in bytes */
    int i_width, i_height;          /* luma dimensions */
    int64_t i_pts;                  /* presentation timestamp */
} phx_picture_t;

/* The encoder handle.  Unlike x264_t this is an open, explicit struct: every
 * piece of state has one owner and one stage that mutates it (ORACLE_DESIGN
 * "State ownership").  Field origins are x264 encoder.c lines above.
 * SHADER: C — serial host state; CABAC (phx_cabac_t) stays in the entropy
 * stage and is never parallelised. */
typedef struct phx_t {
    phx_param_t param;              /* encoder.c:1516 — private copy of param */
    int i_width, i_height;          /* coded picture size */
    int i_mb_width, i_mb_height;    /* encoder.c:1590-1592 */
    phx_sps_t sps;                  /* encoder.c:1579 */
    phx_pps_t pps;                  /* encoder.c:1581 */
    uint8_t header_bytes[1024];     /* Annex-B SPS+PPS+SEI (encoder.c:2106-2122) */
    phx_nal_t header_nal[3];        /* SPS, PPS, SEI NALs handed out by phx_headers */
    uint8_t *bitstream;             /* encoder.c:1724 — out.p_bitstream */
    int i_bitstream;                /* encoder.c:1724 — out.i_bitstream */
    phx_nal_t nal[2];               /* encoder.c:2130 / per-frame NAL array */
    phx_frame_t *fenc;              /* encoder.c:3368 — current input frame */
    phx_mb_state_t *mb_state;       /* encoder.c:1590-1592 MB arrays */
    phx_mb_cache_t mb_cache;        /* encoder.c:2855 — h->mb.cache */
    phx_cost_tables_t cost;         /* encoder.c:1708 / analyse.c:179 */
    phx_mb_analysis_t analysis;     /* analyse.c:248-292 */
    phx_cabac_t cabac;              /* encoder.c:1669 / cabac.c:32 */
    uint8_t *rbsp;                  /* slice-header + MB scratch RBSP (encoder.c:1724) */
    uint8_t *recon;                 /* reconstructed plane, FDEC_STRIDE layout */
    int i_frame;                    /* encoder.c:1563 */
    int i_frame_num;                /* encoder.c:1564 */
    int i_poc;                      /* encoder.c:1629 (fenc->i_poc) */
    int i_idr_pic_id;               /* encoder.c:142 */
    int i_input;                    /* encoder.c:1630 */
} phx_t;

/* x264_encoder_open (api.c:76 -> encoder.c:1507): validate params, copy them
 * into the handle, derive SPS/PPS + MB geometry, allocate frames, cost tables
 * and (when selected) CABAC contexts.  Returns NULL on invalid params/OOM.
 * SHADER: C — host-side serial initialisation (encoder.c:1552,1579,1581,
 * 1590,1669,1708). */
phx_t *phx_encoder_open(const phx_param_t *p);

/* Short alias required by docs/ORACLE_DESIGN.md:40.
 * SHADER: C — forwards to phx_encoder_open (api.c:76 / encoder.c:1507). */
phx_t *phx_open(const phx_param_t *p);

/* x264_encoder_headers (api.c:159 -> encoder.c:2096): emit SPS and PPS as
 * Annex-B NALs.  SEI (encoder.c:2118-2122) is not emitted yet — phx_headers
 * has no SEI writer.  *out points into handle-owned storage and stays valid
 * until the next phx_headers()/phx_close().  Returns the total byte count.
 * SHADER: C — serial header stage (encoder.c:2106-2133). */
int phx_headers(phx_t *h, phx_nal_t **out, int *nnal);

/* Direct byte-form of the above: calls phx_encoder_headers() (phx_headers.h:53,
 * the port of set.c:305/:505 + bitstream.c:55) with the handle's parameters.
 * SHADER: C — serial header stage. */
int phx_encoder_header_bytes(phx_t *h, uint8_t *dst, int capacity);

/* x264_encoder_encode (api.c:166 -> encoder.c:3323): stage order is
 *   put_frame/copy picture (encoder.c:3368-3426) ->
 *   lowres (mc.c:458) -> lookahead (lookahead.c:192/223, encoder.c:3445-3454)
 *   -> ratecontrol_start (encoder.c:3857) ->
 *   slice/MB loop (encoder.c:2855-2892: cache_load, analyse, encode,
 *   write_{cabac,cavlc}, cache_save) ->
 *   ratecontrol_end (encoder.c:3978) -> NAL assembly (encoder.c:2124).
 *
 * The wired path emits one all-intra (IDR for frame 0) slice per call:
 * SPS/PPS are NOT repeated here (x264.c:1977 emits them once), so callers
 * must call phx_headers() first.  P/B slice-type decision, lookahead and
 * rate control remain explicit TODOs; every frame is currently coded as I
 * so the stream stays self-contained without a reference list.  Passing
 * pic == NULL is the x264 flush call (x264.c:2049): there are no delayed
 * frames in this path, so it returns 0 with *out=NULL, *nnal=0.
 * SHADER: C — the whole staged pipeline is serial and seek-safe. */
int phx_encode(phx_t *h, const phx_picture_t *pic,
               phx_nal_t **out, int *nnal, uint8_t **recon);

/* Mirror name for phx_encode (api.c:166 / encoder.c:3323).
 * SHADER: C — forwards to the serial phx_encode pipeline. */
int phx_encoder_encode(phx_t *h, const phx_picture_t *pic,
                       phx_nal_t **out, int *nnal, uint8_t **recon);

/* x264_encoder_close (api.c:130 -> encoder.c:4196): release cost tables
 * (encoder.c:4520), frames, MB arrays, bitstream and the handle.
 * SHADER: C — host-side serial teardown. */
void phx_encoder_close(phx_t *h);

/* Short alias required by docs/ORACLE_DESIGN.md:45.
 * SHADER: C — forwards to phx_encoder_close (api.c:130 / encoder.c:4196). */
void phx_close(phx_t *h);

#endif /* PHX_ENCODER_H */
