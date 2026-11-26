/* Phoenix trellis RD quantization — function-for-function port of the scalar
 * C path of reference/x264/encoder/rdo.c (8-bit, 4:2:0, HAVE_MMX == 0).
 *
 * Functions ported, in source order:
 *   x264_rdo_init                        rdo.c:384-415
 *   trellis_dc_shortcut                  rdo.c:462-491
 *   trellis_coef                         rdo.c:493-531
 *   trellis_coef0_0 / _0_1               rdo.c:537-567
 *   trellis_coef1_0 / _1_1               rdo.c:577-606
 *   trellis_coefn_0 / _n_1               rdo.c:608-639
 *   quant_trellis_cabac                  rdo.c:641-895
 *   quant_trellis_cavlc                  rdo.c:920-1096
 *   x264_quant_luma_dc_trellis           rdo.c:1098-1109
 *   x264_quant_chroma_dc_trellis         rdo.c:1114-1140
 *   x264_quant_4x4_trellis               rdo.c:1142-1157
 *   x264_quant_8x8_trellis               rdo.c:1159-1184
 *
 * Tables are copied verbatim (with x264 line citations in phx_rdo.c) from:
 *   common/tables.c:397-449, 1688-1750, 1758-1792
 *   common/macroblock.h:181-200, 273-289
 *   common/set.c:164-205 (unquant4_mf / quant4_bias0 formulas)
 *
 * SHADER: C — the coefficient state machine and entropy-cost bookkeeping are
 * strictly serial; keep this module in the entropy stage.  No byte stream is
 * written here: x264's RDO_SKIP_BS==1 bit counters are reproduced exactly.
 *
 * NOTE ON STREAM PARITY: matching x264's *rounded coefficients* and *bit-cost
 * counters* is the contract here.  Whole-stream parity additionally depends on
 * callers (MB analysis, psy-RD inputs, CABAC context history) and is not
 * claimed by this module.
 *
 * ROW 9 EVIDENCE: with fixed, identical inputs the chroma-DC trellis here is
 * byte-exact to instrumented x264 (tests/test_phx_rdo.c gold_mb0_u_q23 /
 * gold_mb1_u_q23 assert coefficients, node score, level path, copied CABAC
 * states and cost_siglast[]).  The remaining stream-level `ue val=14` (raw
 * record 1623) is an *input* defect outside this module: phx_chroma.c:309-310
 * reconstructs 4:2:0 chroma DC with `chroma_qp+3`, while x264
 * encoder/macroblock.c:420/425 uses `i_qp+3*chroma422` with chroma422==0.
 * That makes MB1 U's raw DC 2080 instead of x264's 1568, so the (correct)
 * trellis emits 29 instead of 22.  SHADER: C — no approximation is made here
 * to mask the caller's QP error.
 */
#ifndef PHX_RDO_H
#define PHX_RDO_H

#include <stdint.h>
#include "phx_cabac.h"
#include "phx_cabac_syntax.h"   /* phx_cabac_mb_t / phx_dctcoef_t (cbf context) */
#include "phx_mbcache.h"        /* phx_mb_cache_t (CAVLC nC helper) */
#include "phx_quant.h"          /* phx_quant_tables_t / phx_dctcoef */

#ifdef __cplusplus
extern "C" {
#endif

/* Stable API expected by phx_mbencode.  `ctx_block_cat` is one of the
 * PHX_DCT_* values from phx_cabac_syntax.h (common/macroblock.h:273-289);
 * `i_quant_cat` is a CQM_4* / CQM_8* index (common/set.h:32-42) and
 * `i_qp` is the actual (possibly >51) macroblock QP.  The CABAC context state
 * passed in `cb->state[]` is read-only: like x264, trellis copies it into
 * local node state and never advances the real entropy coder.
 *
 * These wrappers use the configuration set by phx_rdo_set_config(); defaults
 * are chroma_qp_index_offset=0, psy-trellis=0, b_interlaced=0 (x264 defaults),
 * which make them bit-exact calls to x264's own entry points with the same
 * arguments.  For nonzero psy-trellis / explicit indices / CQM tables, use
 * the *_full entry points below instead.
 *
 * All functions return the nonzero flag (0 or 1).  phx_quant_8x8_trellis is
 * implemented structurally but is NOT yet bit-exact to x264 for nontrivial
 * 64-coefficient blocks (see Unsupported below); callers needing stream parity
 * must fall back to plain 8x8 deadzone quantization for now. */
int phx_quant_4x4_trellis(phx_cabac_t *cb, int16_t *dct, int i_quant_cat,
                          int i_qp, intptr_t ctx_block_cat, int b_intra);
int phx_quant_8x8_trellis(phx_cabac_t *cb, int16_t *dct, int i_quant_cat,
                          int i_qp, intptr_t ctx_block_cat, int b_intra);
int phx_quant_luma_dc_trellis(phx_cabac_t *cb, int16_t *dct, int i_quant_cat,
                              int i_qp, intptr_t ctx_block_cat, int b_intra);
int phx_quant_chroma_dc_trellis(phx_cabac_t *cb, int16_t *dct, int i_qp,
                                intptr_t ctx_block_cat, int b_intra);

/* x264_rdo_init() (rdo.c:384-415) + flat-CQM table setup (set.c:164-205).
 * Must be called once before threaded use; the stable wrappers also perform a
 * lazy, non-thread-safe init for convenience.  SHADER: A/B (table build). */
void phx_rdo_init(void);

/* x264 encoder.c:1585 / analyse.c:257 and encoder.c:1219 equivalents for the
 * stable wrappers.  Change before calling the wrappers, not concurrently.
 * `i_psy_trellis` is FIX8(f_psy_trellis/4); `fenc_dct4/fenc_dct8` are the
 * original (unquantized) DCT blocks used by psy-trellis (rdo.c:822-829). */
typedef struct {
    int chroma_qp_index_offset;     /* h->pps->i_chroma_qp_index_offset */
    int i_psy_trellis;              /* h->mb.i_psy_trellis (FIX8) */
    int b_interlaced;               /* MB_INTERLACED */
    int idx;                        /* h->mb.i_mb_xy-like block index (psy) */
    const int16_t *fenc_dct4;       /* h->mb.pic.fenc_dct4[idx&15] */
    const int16_t *fenc_dct8;       /* h->mb.pic.fenc_dct8[idx&3] */
} phx_rdo_config_t;

void phx_rdo_set_config(const phx_rdo_config_t *cfg);
void phx_rdo_get_config(phx_rdo_config_t *cfg);

/* Direct ports of x264's internal functions.  Every parameter corresponds 1:1
 * to the x264 parameter of the same name (rdo.c line numbers in the header
 * comment above).  These do not consult the global config.
 *
 * `lambda2` is h->mb.i_trellis_lambda2[chroma][intra] (analyse.c:264-267).
 * `psy_fenc` / `i_psy_trellis` implement rdo.c:822-829; pass NULL / 0 to get
 * x264's default (psy-trellis disabled). */
int phx_rdo_quant_trellis_cabac(phx_cabac_t *cb, int16_t *dct,
                                const uint16_t *quant_mf,
                                const uint16_t *quant_bias0,
                                const int *unquant_mf, const uint8_t *zigzag,
                                int ctx_block_cat, int lambda2, int b_ac,
                                int b_chroma, int dc, int num_coefs, int idx,
                                int i_psy_trellis, const int16_t *psy_fenc,
                                int b_interlaced);

/* quant_trellis_cavlc (rdo.c:920-1096).  `nC` is
 * ct_index[x264_mb_predict_non_zero_code(h, idx)] (cavlc.c:198,
 * macroblock.h:432-442); `profile_idc` selects the 2000-bit overflow penalty
 * of cavlc.c:102-105 when RDO_SKIP_BS is active.  `quant_mf` is the 4x4 or 8x8
 * row selected by i_quant_cat/i_qp; `num_coefs` and `b_8x8` select the scan. */
int phx_rdo_quant_trellis_cavlc(int16_t *dct, const uint16_t *quant_mf,
                                const int *unquant_mf, const uint8_t *zigzag,
                                int ctx_block_cat, int lambda2, int b_ac,
                                int b_chroma, int dc, int num_coefs, int idx,
                                int b_8x8, int nC, int profile_idc,
                                int chroma_2x4_dc);

/* Access to the derived flat-CQM tables (set.c:164-205).  `list` must be in
 * [0,3] for 4x4 and [0,1] for 8x8; qp in [0,51].  NULL is returned for an
 * out-of-range index.  SHADER: A — immutable lookup after phx_rdo_init(). */
const uint16_t *phx_rdo_quant4_mf(int list, int qp);
const uint16_t *phx_rdo_quant4_bias0(int list, int qp);
const int      *phx_rdo_unquant4_mf(int list, int qp);
const uint16_t *phx_rdo_quant8_mf(int list, int qp);
const uint16_t *phx_rdo_quant8_bias0(int list, int qp);
const int      *phx_rdo_unquant8_mf(int list, int qp);

/* common/bitstream.h:283-289, used by rdo.c:615/630/484. */
int phx_rdo_bs_size_ue_big(unsigned int val);

/* -------------------------------------------------------------------------
 * Diagnostic trace of the last phx_rdo_quant_trellis_cabac() call.
 *
 * x264's golden harness prints exactly these values from an instrumented
 * rdo.c: last_nnz (rdo.c:695), the best node's score and level path
 * (rdo.c:862-891), the copied level CABAC states (rdo.c:747-752) and the
 * cost_siglast[] triple of the last-processed coefficient (rdo.c:789-808).
 * The struct is written by the entropy-stage C state machine and is safe to
 * read after the call returns; it is diagnostic only and never read by the
 * encoder.  SHADER: C — strictly serial, one writer per MB.
 * ------------------------------------------------------------------------- */
typedef struct {
    int last_nnz;
    uint64_t best_score;             /* rdo.c:863-866 */
    int16_t best_levels[64];         /* rdo.c:888-891, b_ac..last_nnz */
    int best_count;
    uint8_t level_state[16];         /* rdo.c:747-752 */
    int cost_siglast[3];             /* rdo.c:789-808, i == last_nnz */
    int levels_used;                 /* rdo.c:735+ */
} phx_rdo_trellis_trace_t;

void phx_rdo_get_trellis_trace(phx_rdo_trellis_trace_t *out);

/* common/macroblock.h:432-442 + cavlc.c:198.  `idx` is the scan8 block index. */
int phx_rdo_cavlc_predict_nc(const phx_mb_cache_t *cache, int idx);

/* -------------------------------------------------------------------------
 * Size-only CABAC accumulation (x264's RDO_SKIP_BS == 1 path).
 *
 * common/cabac.h:99-124 exposes a *cost-only* interface: the syntax writers
 * advance the 460 context states and add FIX8 entropy bits to
 * `cb->f8_bits_encoded`, but never touch the arithmetic coder.  rdo.c:31-69
 * redefines the real writers with these helpers (terminal +7, bypass +256,
 * ue_bypass `(bs_size_ue_big(v+(1<<e)-1)-e)<<8`) and rdo.c:162-197/315-347
 * uses the accumulated total as the exact `i_bits` value.
 *
 * Phoenix's live phx_cabac_t has no f8_bits_encoded member and its syntax
 * writers always emit bins, so copying the live coder (as an earlier MBRD
 * approximation did) produces neither x264's contexts nor x264's bit count.
 * This accumulator mirrors just the two fields x264's COPY_CABAC copies
 * (rdo.c:71-73): `state[0..459]` and `f8_bits_encoded`.  SHADER: B/C — no
 * bitstream and no arithmetic-coder state are touched.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t state[PHX_CABAC_CTX_COUNT]; /* common/cabac.h:45 (x264_cabac_t.state) */
    int f8_bits_encoded;                /* common/cabac.h:45 */
} phx_cabac_size_t;

/* Seed from a live coder's context states (COPY_CABAC, rdo.c:71-73).  Pass
 * NULL when standalone analysis has no live coder; the states are then built
 * with phx_cabac_context_init() as encoder/cabac.c does for a slice. */
void phx_rdo_size_init(phx_cabac_size_t *cb, const phx_cabac_t *live,
                       int slice_type, int qp);

/* Size-only primitives.  Source lines: cabac.h:101-106, :115-119, rdo.c:64-66. */
void phx_rdo_size_decision(phx_cabac_size_t *cb, int ctx, int b);
void phx_rdo_size_decision_noup(phx_cabac_size_t *cb, int ctx, int b);
void phx_rdo_size_terminal(phx_cabac_size_t *cb);
void phx_rdo_size_bypass(phx_cabac_size_t *cb);
void phx_rdo_size_ue_bypass(phx_cabac_size_t *cb, int exp_bits, int val);

/* Tables generated by rdo.c:384-415 (also used by quant_trellis_cabac).
 * `prefix` is in [0,14], ctx in [0,127]; NULL when out of range.  SHADER: A. */
const uint16_t *phx_rdo_size_unary(int prefix);
const uint8_t  *phx_rdo_transition_unary(int prefix);
const uint16_t *phx_rdo_size_5ones(void);
const uint8_t  *phx_rdo_transition_5ones(void);

/* cabac.c:83-96 — intra4x4_pred_mode size (i_pred is the raw neighbour
 * predictor, i_mode is the FIX4 mapping; predict.h:84-92). */
void phx_rdo_size_intra4x4_pred_mode(phx_cabac_size_t *cb,
                                     int i_pred, int i_mode);

/* cabac.c:775-858 + :892-903 — the RDO_SKIP_BS==1 residual (merged sigmap
 * and level coding) behind the coded-block flag.  `l` is the zigzag-scanned
 * 4x4 coefficient array; `mb->cache->non_zero_count[scan8[i_idx]]` supplies
 * the CBF exactly as x264_mb_encode_i4x4() sets it (macroblock.h:158). */
void phx_rdo_size_block_residual_cbf(phx_cabac_size_t *cb, phx_cabac_mb_t *mb,
                                     int ctx_block_cat, int i_idx,
                                     const phx_dctcoef_t *l, int b_intra);

/* partition_i4x4_size_cabac() (cabac.c:1203-1211) on a size-only coder.
 * `i_mode` is the raw 0..11 predictor; the FIX4 mapping is applied internally
 * (cabac.c:1207).  Returns the FIX8 bit total `f8_bits_encoded`, ready for
 * rdo.c:341's `(f8_bits*i_lambda2+128)>>8`. */
int phx_rdo_size_partition_i4x4(const phx_cabac_t *live, phx_cabac_mb_t *mb,
                                int i4, int i_pred, int i_mode,
                                const phx_dctcoef_t *l,
                                int slice_type, int qp);

/* Verbatim scan tables (common/macroblock.h:181-200), [frame/field]. */
extern const uint8_t phx_rdo_zigzag4[2][16];
extern const uint8_t phx_rdo_zigzag8[2][64];

/* -------------------------------------------------------------------------
 * Unsupported / out-of-scope paths (verified against the vendored x264):
 *   1. 8x8 CABAC trellis: phx_rdo_quant_trellis_cabac() runs the same state
 *      machine with num_coefs=64, but golden tests against x264's own
 *      x264_quant_8x8_trellis differ by a few coefficients.  Do not wire
 *      phx_quant_8x8_trellis() into a parity-critical path yet.
 *   2. 8x8 CAVLC split path (rdo.c:1170-1183): not ported.
 *   3. Psy-trellis via the stable wrappers: pass a phx_rdo_config_t with
 *      fenc_dct4/fenc_dct8 and i_psy_trellis, or call the *_full entry point.
 *   4. QP > 51 and non-flat custom quantization matrices: phx_quant_tables_t
 *      only builds flat CQM entries for QP 0..51; wrappers return -1.
 *   5. MBAFF / interlaced trellis: field zigzags and offset tables are present,
 *      but only the progressive (b_interlaced=0) path is golden-tested.
 * ------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif
#endif
