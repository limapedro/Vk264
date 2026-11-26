/* Phoenix slice/MB decoder driver (serial Annex-B -> reconstructed frames).
 *
 * This is the inverse of the ported x264 write path.  Every syntax element is
 * read in exactly the order the writer emits it; the source line of the writer
 * is named at the read site in phx_decode.c.  Nothing is guessed and no
 * "approximation" is used to force a frame to decode: unsupported syntax
 * returns an explicit error code.
 *
 * Exact x264/H.264 sources mirrored here:
 *   reference/x264/encoder/set.c:305-475 (SPS write), :505-570 (PPS write);
 *   reference/x264/encoder/encoder.c:213-364 (slice_header_write),
 *     :2791-3093 (slice/MB loop), :3504/3513/3882 (frame_num/POC/DPB);
 *   reference/x264/encoder/cavlc.c:241-543 (CAVLC header/MVD/residual),
 *   reference/x264/encoder/cabac.c:381-1096 (CABAC header/residual);
 *   H.264 7.3.1 (NAL/EBSP), 7.3.2.1.1 (SPS), 7.3.2.2 (PPS),
 *     7.3.3 (slice header), 7.3.5 (MB layer), 8.2/8.3 (transform/recon).
 *
 * SHADER: C — Annex-B splitting, RBSP unescaping, Exp-Golomb reads, slice
 * header parsing and entropy decoding are one strictly serial stream.
 * SHADER: A/B — pixel prediction, dequant/IDCT, motion compensation and the
 * in-loop deblock kernels are the parallel reconstruction stages; see the
 * per-kernel tags in phx_decode.c.
 */
#ifndef PHX_DECODE_H
#define PHX_DECODE_H

#include <stdint.h>
#include "phx_frame.h"
#include "phx_mbcache.h"
#include "phx_mb_decision.h"
#include "phx_headers.h"
#include "phx_slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decode a complete Annex-B byte stream.
 *
 * On success (0) *out / *state point to freshly allocated arrays of *nframes
 * entries: one phx_frame_t / phx_mb_state_t per codec frame (a frame starts at
 * a slice with first_mb_in_slice == 0).  The caller owns the arrays and each
 * object: the returned pointers are contiguous arrays of phx_frame_t /
 * phx_mb_state_t structs, so use phx_decode_free() (phx_frame_delete() and
 * phx_mb_state_delete() free the containing struct too).  On failure the
 * outputs are set to NULL / 0 and no partial array is returned.
 *
 * The DPB is built in coding order from the decoded slice frame_nums; frames
 * are stored by frame_num modulo MaxFrameNum.  Non-reference slices still
 * produce a frame, but do not replace the active reference. */
int phx_decode_annexb(const uint8_t *data, int size,
                      phx_frame_t **out, phx_mb_state_t **state, int *nframes);

/* Matching owner for the contiguous arrays returned by phx_decode_annexb. */
void phx_decode_free(phx_frame_t *frames, phx_mb_state_t *states, int nframes);

/* Decode one coded slice NAL into an existing frame/state and hand back the
 * per-MB decision sequence.
 *
 * `nal` points at the NAL header byte (a leading 3/4-byte Annex-B start code
 * is accepted and skipped).  `frame` and `state` must be allocated for the
 * size signalled by the SPS/PPS seen earlier in the stream and are written
 * in raster order.  `decisions` may be NULL; otherwise it must have room for
 * mb_width*mb_height entries and *ndecisions receives the number written.
 * Returns 0 on success, -1 for malformed/unsupported syntax. */
int phx_decode_nal(const uint8_t *nal, int size,
                   phx_frame_t *frame, phx_mb_state_t *state,
                   phx_mb_decision_t *decisions, int *ndecisions);

/* Extended form with the explicit parameter sets and the optional list-0
 * reference frame that phx_decode_nal() cannot carry in its fixed signature.
 * phx_decode_annexb() uses this directly. */
int phx_decode_nal_ex(const uint8_t *nal, int size,
                      const phx_sps_t *sps, const phx_pps_t *pps,
                      const phx_frame_t *ref,
                      phx_frame_t *frame, phx_mb_state_t *state,
                      phx_mb_decision_t *decisions, int *ndecisions);

/* Install the SPS/PPS that phx_decode_nal() (fixed signature) will use. */
int phx_decode_set_params(const phx_sps_t *sps, const phx_pps_t *pps);

/* ---- explicit serial-stage helpers (also used by the unit tests) -------- */

/* NAL header only: nal_unit_type and nal_ref_idc at bitstream.c:73. */
int phx_decode_nal_info(const uint8_t *nal, int size, int *type, int *ref_idc);
/* Split an Annex-B stream into NALs; type/offset/length are parallel arrays
 * of capacity `cap`.  Returns the number of NALs, or -1 if cap is too small. */
int phx_decode_annexb_nals(const uint8_t *data, int size,
                           int *types, int *offsets, int *lengths, int cap);
/* Remove emulation-prevention bytes (H.264 7.3.1, bitstream.c:29-39). */
int phx_decode_rbsp(const uint8_t *nal, int size, uint8_t *dst, int cap);

/* SPS/PPS parse: exact inverse of set.c:305-570 / phx_sps_write,phx_pps_write. */
int phx_decode_sps(const uint8_t *nal, int size, phx_sps_t *sps);
int phx_decode_pps(const uint8_t *nal, int size, phx_pps_t *pps);

/* Slice header parse: inverse of encoder.c:213-364.  `end_bit` (optional)
 * receives the bit position of the first MB syntax element, i.e. the exact
 * point where encoder.c:2795 aligns before CABAC data.  Caller supplies the
 * parameter sets; nal_ref_idc/nal_type come from the NAL header. */
int phx_decode_slice_header(const uint8_t *nal, int size,
                            const phx_sps_t *sps, const phx_pps_t *pps,
                            phx_slice_header_t *sh, int *end_bit);

#ifdef __cplusplus
}
#endif
#endif /* PHX_DECODE_H */
