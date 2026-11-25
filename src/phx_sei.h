/* Phoenix version SEI writer — mirrors x264's self-identifying SEI NAL.
 *
 * Source:
 *   reference/x264/encoder/set.c:593-623  x264_sei_version_write
 *   reference/x264/encoder/set.c:80-99    x264_sei_write
 *   reference/x264/common/base.c:1428-1567 x264_param2string (b_res == 0)
 *   reference/x264/encoder/encoder.c:2117-2122 (emitted after SPS/PPS),
 *     :559-584 / :1109-1111 / :1273-1300 (runtime values the string shows),
 *     :1211-1231 (psy / chroma QP derivation already owned by phx_param.c).
 *   reference/x264/x264.h:48 (X264_BUILD), :94/:103 (NAL_SEI / ref_idc 0),
 *     :234 (motion-est names);
 *   reference/x264/common/base.h:127 (SEI_USER_DATA_UNREGISTERED == 5),
 *     :138-139 (thread caps);
 *   reference/x264/x264_config.h:5 (X264_VERSION ""),
 *   reference/x264/config.h:19 (HAVE_GPL 0).
 *
 * SHADER: C — one-shot serial header-stage writer; no thread or GPU state.
 * The returned buffer is self-contained (Annex-B start code + NAL header +
 * escaped RBSP) and is safe to hand to the same ordered output stage that
 * writes the SPS/PPS NALs from phx_headers.h.
 */
#ifndef PHX_SEI_H
#define PHX_SEI_H

#include <stdint.h>
#include "phx_param.h"

/* Constants mirrored from the reference build (see source list above). */
#define PHX_SEI_X264_BUILD   164   /* x264.h:48 */
#define PHX_SEI_X264_VERSION ""    /* x264_config.h:5 */
#define PHX_SEI_HAVE_GPL     0     /* config.h:19 -> "Copyright", not "Copyleft" */
#define PHX_SEI_PAYLOAD_TYPE 5     /* base.h:127, SEI_USER_DATA_UNREGISTERED */
#define PHX_SEI_NAL_TYPE     6     /* x264.h:94, NAL_SEI */

/* x264_sei_version_write (set.c:593) adapted to the explicit Phoenix API.
 * Writes a complete Annex-B NAL into dst (at most cap bytes) for the given
 * parameter set and coded picture size.  width/height are used for the same
 * runtime derivations x264_encoder_open applies (encoder.c:559-584,
 * :1109-1111, :1273-1300) — those values are visible in the option string.
 *
 * Returns the NAL byte count (>= 0), or -1 on bad arguments / insufficient
 * capacity.  The caller owns dst.
 *
 * SHADER: C — serial header stage (encoder.c:2117-2122).
 */
int phx_sei_version_write(uint8_t *dst, int cap, const phx_param_t *p,
                          int width, int height);

#endif /* PHX_SEI_H */
