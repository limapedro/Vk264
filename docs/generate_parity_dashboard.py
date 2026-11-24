#!/usr/bin/env python3
"""Generate docs/parity-dashboard.svg — a coverage dashboard for the
byte-exact oracle port.  Data is kept here so the image can be regenerated
after every parity PR:

    python docs/generate_parity_dashboard.py

Status colors:
    EXACT   byte-exact fixture passes against the verbatim x264 C oracle
    PARTIAL GPU/C++ port exists but is exact only for restricted inputs
    C++     faithfully ported in C++ (control / tables / state)
    MISSING no counterpart yet
    N/A     platform / CLI / threading infrastructure, out of scope

The dashboard is intentionally very detailed: every function family in every
pipeline stage gets its own progress bar, every byte-exact verification suite
gets a bar with its real case count, and the open (incomplete) targets are
tracked separately from the passing gates.
"""
from pathlib import Path
from datetime import date

# ---------------------------------------------------------------------------
# Data — update these after each parity PR, then regenerate the SVG.
# ---------------------------------------------------------------------------
SNAPSHOT = "1fcf6e4"
GENERATED = date.today().isoformat()

# Overall numbers (from build/*_parity_sweep.py, ctest, tests/video_experiments.json)
# FUNCTION PARITY is derived from the weighted STAGES table below so the
# dashboard stays self-consistent when a family changes status.
CAVLC_P_EXACT, CAVLC_P_TOTAL = 1360, 1360
CABAC_P_EXACT, CABAC_P_TOTAL = 1360, 1360
CAVLC_I_EXACT, CAVLC_I_TOTAL = 72, 72
VIDEO_EXACT, VIDEO_TOTAL = 56, 56
FUNC_PORTED, FUNC_TOTAL_INV = 23, 831
STATS = [
    ("CAVLC I-FRAMES", f"{CAVLC_I_EXACT} / {CAVLC_I_TOTAL}",
     f"{CAVLC_I_EXACT/CAVLC_I_TOTAL*100:.1f}%", "#2ea043"),
    ("CAVLC P-SLICES", f"{CAVLC_P_EXACT} / {CAVLC_P_TOTAL}",
     f"{CAVLC_P_EXACT/CAVLC_P_TOTAL*100:.1f}%", "#2ea043"),
    ("CABAC P-SLICES", f"{CABAC_P_EXACT} / {CABAC_P_TOTAL}",
     f"{CABAC_P_EXACT/CABAC_P_TOTAL*100:.1f}%", "#2ea043"),
    ("CTEST GATES", "29 + 65 / 94", "100.0%", "#2ea043"),
    # Video-level MP4 experiments (tests/video_experiments.json). These are
    # 8-frame real-clip encodes; 2-frame fixtures alone are not sufficient.
    ("VIDEO MP4 EXPERIMENTS", f"{VIDEO_EXACT} / {VIDEO_TOTAL} exact",
     f"{VIDEO_EXACT/VIDEO_TOTAL*100:.1f}%",
     "#2ea043" if VIDEO_EXACT == VIDEO_TOTAL else "#d29922"),
    ("x264 INVENTORY", f"{FUNC_TOTAL_INV} funcs / {FUNC_PORTED+3} ported",
     f"{(FUNC_PORTED+3)/FUNC_TOTAL_INV*100:.1f}%", "#d29922"),
    # Fresh all-preset audit (tests/preset_audit/): header parity is complete
    # for all 10 presets; slice/POC parity remains open.
    ("PRESET SPS/PPS HEADERS", "10 / 10 presets exact", "100.0%", "#2ea043"),
    ("PRESET BYTE-EXACT VIDEO", "2 / 10 presets", "20.0%", "#d29922"),
    # Non-ultrafast macroblock payload: byte-exact prefix of the IDR slice
    # NAL2 for the superfast preset against x264 (build/progress_metrics.py).
    ("SUPERFAST NAL2 PREFIX", "130 / 130 B EXACT", "100%", "#2ea043"),
]

# Superfast IDR slice NAL2 byte-exact prefix progression, by merged PR.
# Measured with build/progress_metrics.py against the 64x64 c004 clip.
# 130 bytes is the full x264 NAL2; higher prefix = closer to byte-exact.
NAL2_PREFIX_PROGRESSION = [
    ("baseline", 5, "#484f58"),
    ("#210", 5, "#6e7681"),
    ("#211", 5, "#8b949e"),
    ("#212", 8, "#c9a227"),
    ("#215", 17, "#d29922"),
    ("#216", 43, "#d29922"),
    ("#217", 49, "#2ea043"),
    ("#251", 67, "#3fb950"),
    ("#255", 80, "#2ea043"),
    ("#256", 130, "#2ea043"),
]
NAL2_FULL = 130

# Byte-exact sweep progression (each row is one merged PR)
SWEEPS = [
    ("baseline", 782, "#484f58"),
    ("#109", 782, "#484f58"),
    ("#110", 800, "#6e7681"),
    ("#111", 819, "#8b949e"),
    ("#112", 837, "#a5a5a5"),
    ("#113", 837, "#a5a5a5"),
    ("#114", 860, "#c9a227"),
    ("#115", 860, "#c9a227"),
    ("#116", 874, "#d29922"),
    ("#117", 896, "#2ea043"),
    ("#118", 896, "#2ea043"),
    ("#119", 912, "#2ea043"),
    ("#120", 933, "#2ea043"),
    ("#121", 975, "#2ea043"),
    ("#123", 1005, "#2ea043"),
    ("#125", 1005, "#2ea043"),
    ("#126", 1022, "#2ea043"),
    ("#128", 1089, "#2ea043"),
    ("#130", 1160, "#2ea043"),
    ("#132", 1189, "#2ea043"),
    ("#134", 1189, "#2ea043"),
    ("#136", 1195, "#2ea043"),
    ("#138", 1214, "#2ea043"),
    ("#140", 1215, "#2ea043"),
    ("#142", 1250, "#2ea043"),
    ("#144", 1284, "#2ea043"),
    ("#146", 1325, "#2ea043"),
    ("#148", 1325, "#2ea043"),
    ("#151", 1328, "#2ea043"),
    ("#153", 1339, "#2ea043"),
    ("#155", 1339, "#2ea043"),
    ("#157", 1339, "#2ea043"),
    ("#175", 1343, "#2ea043"),
    ("#177", 1344, "#2ea043"),
    ("#179", 1350, "#2ea043"),
    ("#181", 1355, "#2ea043"),
    ("#183", 1360, "#2ea043"),
]

# CABAC P-slice byte-exact sweep progression (parallel to SWEEPS). CABAC was
# introduced in #127.
CABAC_SWEEPS = [
    ("#127",  920, "#484f58"),
    ("#134",  952, "#6e7681"),
    ("#138",  960, "#8b949e"),
    ("#148",  987, "#a5a5a5"),
    ("#151",  987, "#a5a5a5"),
    ("#153",  990, "#c9a227"),
    ("#155", 1079, "#388bfd"),
    ("#157", 1102, "#388bfd"),
    ("#159", 1149, "#2ea043"),
    ("#161", 1217, "#2ea043"),
    ("#163", 1219, "#2ea043"),
    ("#165", 1254, "#2ea043"),
    ("#167", 1268, "#2ea043"),
    ("#169", 1349, "#2ea043"),
    ("#171", 1351, "#2ea043"),
    ("#173", 1355, "#2ea043"),
    ("#175", 1359, "#2ea043"),
    ("#177", 1360, "#2ea043"),
]

# Pipeline stages → function families → (name, weight, status).
# Weights are approximate function counts from docs/function-parity-matrix.md.
STAGES = [
    ("01  Parameters, SPS/PPS, headers", "base.c · common/set.c · encoder/set.c", [
        ("param_default / presets", 2, "PARTIAL"),
        ("cqm_init / parse_file", 2, "MISSING"),
        ("sps_init / sps_write", 2, "EXACT"),
        ("pps_init / pps_write", 2, "EXACT"),
        ("slice_header_init / write", 2, "EXACT"),
        ("sei_* (10 functions)", 10, "PARTIAL"),
        ("validate_levels", 1, "C++"),
    ]),
    ("02  Frame management", "common/frame.c", [
        ("frame_new / copy_picture", 8, "C++"),
        ("expand_border*", 6, "C++"),
        ("sync-frame lists", 9, "C++"),
        ("weight scale / lowres", 6, "C++"),
    ]),
    ("03  Lookahead & slice type", "lookahead.c · slicetype.c · slicetype-cl.c", [
        ("x264_lookahead_*", 9, "MISSING"),
        ("slicetype_*", 26, "MISSING"),
        ("macroblock_tree*", 5, "MISSING"),
        ("scenecut*", 3, "MISSING"),
        ("OpenCL slicetype-cl", 8, "N/A"),
    ]),
    ("04  Analysis / mode decision", "encoder/analyse.c · rdo.c", [
        ("macroblock_analyse", 1, "PARTIAL"),
        ("mb_analyse_intra", 1, "PARTIAL"),
        ("mb_analyse_intra_chroma", 1, "EXACT"),
        ("mb_analyse_inter_p*", 4, "PARTIAL"),
        ("mb_analyse_inter_b*", 9, "MISSING"),
        ("macroblock_probe_skip", 1, "EXACT"),
        ("rd_cost* / trellis", 26, "MISSING"),
        ("analyse_weight_frame", 1, "PARTIAL"),
    ]),
    ("05  Intra prediction", "common/predict.c", [
        ("predict_4x4_* (9 modes)", 9, "EXACT"),
        ("predict_8x8* / 8x8c / 8x16c", 20, "EXACT"),
        ("predict_16x16_*", 16, "EXACT"),
        ("predict_lossless_*", 6, "PARTIAL"),
    ]),
    ("06  Transform, quant, zigzag", "common/dct.c · common/quant.c", [
        ("sub/add_dct / idct 4x4,8x8,16x16", 20, "EXACT"),
        ("dct4x4dc / idct4x4dc", 3, "EXACT"),
        ("quant_4x4 / 8x8 / dequant_*", 30, "EXACT"),
        ("zigzag_scan_* / zigzag_sub_*", 10, "EXACT"),
        ("optimize_chroma_* (6)", 6, "EXACT"),
        ("decimate_score* / denoise_dct", 4, "EXACT"),
    ]),
    ("07  Motion", "mc.c · me.c · mvpred.c · pixel.c", [
        ("pixel_satd / sa8d / ssd*", 60, "EXACT"),
        ("mc_luma / mc_chroma / hpel", 6, "EXACT"),
        ("frame_init_lowres*", 2, "EXACT"),
        ("integral_init*", 4, "EXACT"),
        ("me_search_ref / refine_*", 10, "EXACT"),
        ("mb_predict_mv*", 8, "EXACT"),
        ("mbtree_* / frame_filter", 6, "MISSING"),
    ]),
    ("08  Macroblock encode / reconstruct", "encoder/macroblock.c", [
        ("mb_encode_i16x16", 2, "EXACT"),
        ("mb_encode_chroma", 2, "EXACT"),
        ("I4x4 encode path", 1, "PARTIAL"),
        ("macroblock_encode_skip", 2, "EXACT"),
        ("macroblock_deblock", 1, "EXACT"),
    ]),
    ("09  Deblocking", "common/deblock.c", [
        ("deblock_*_c / deblock_edge*", 22, "EXACT"),
        ("x264_frame_deblock_row", 8, "EXACT"),
    ]),
    ("10  Rate control", "encoder/ratecontrol.c", [
        ("adaptive_quant_frame / ac_energy", 6, "EXACT"),
        ("x264_ratecontrol_* (45)", 45, "PARTIAL"),
    ]),
    ("11  Entropy coding", "cabac.c · cavlc.c · vlc.c · bitstream.c", [
        ("cabac_encode_* / init", 30, "EXACT"),
        ("cabac_ref / mvd / residual", 20, "EXACT"),
        ("cabac mb_header_i I4x4", 1, "EXACT"),
        ("cabac header decoder (oracle)", 1, "EXACT"),
        ("B mb_type header", 1, "EXACT"),
        ("cavlc_block_residual* / mvd", 10, "EXACT"),
        ("nal_encode / bitstream_init", 2, "EXACT"),
    ]),
    ("12  Encoder orchestration", "encoder/encoder.c · api.c", [
        ("x264_encoder_encode", 1, "PARTIAL"),
        ("x264_encoder_headers", 1, "EXACT"),
        ("slice_write / nal_end", 2, "EXACT"),
    ]),
]

# Byte-exact verification suites: (group, [(suite_name, exact, total), ...]).
# total == 0 means the CTest passes but no case count is published — shown as
# a full bar labelled "pass". Real case counts are taken from
# docs/function-parity-matrix.md and the parity sweep scripts.
SUITES = [
    ("01  Headers / SPS-PPS / NAL", [
        ("x264_parameters_parity", 0, 0),
        ("x264_slice_header_parity", 0, 0),
        ("x264_nal_parity", 0, 0),
        ("x264_bitstream_parity", 0, 0),
        ("gop3_nal_structure", 0, 0),
        ("rewrite_nal / rewrite_slice", 0, 0),
        ("rewrite_bitstream", 0, 0),
    ]),
    ("04  Analysis / mode decision", [
        ("x264_intra16_mode_parity", 16640, 16640),
        ("x264_chroma_mode_parity", 16640, 16640),
        ("x264_inter8x8_parity", 0, 0),
        ("x264_inter4x4_parity", 0, 0),
        ("x264_motion_elimination_parity", 0, 0),
        ("rewrite_oracle_pskip / pme", 0, 0),
        ("rewrite_cabac_pskip / pme", 0, 0),
        ("rewrite_cabac_textured / intra_p", 0, 0),
        ("rewrite_oracle_flat / real / multimb", 0, 0),
        ("x264_i4x4_analysis", 0, 0),
    ]),
    ("05  Intra prediction", [
        ("x264_predict_directional_parity", 0, 0),
        ("x264_predict_intra_parity", 0, 0),
        ("x264_intra16_parity", 0, 0),
        ("x264_intra_chroma_parity", 0, 0),
        ("rewrite_shader_predict", 0, 0),
    ]),
    ("06  Transform / quant / zigzag", [
        ("x264_dc_parity", 0, 0),
        ("x264_reconstruct_dc_parity", 0, 0),
        ("x264_chroma_dc_parity", 0, 0),
        ("x264_zigzag_parity", 0, 0),
        ("x264_quant_group_parity", 0, 0),
        ("x264_quant4x4_parity", 0, 0),
        ("x264_quant8x8_parity", 0, 0),
        ("x264_dct4x4_group4/8/16", 0, 0),
        ("x264_dct8x8_group8/16", 0, 0),
        ("x264_chroma_optimize_parity", 0, 0),
        ("x264_coeff_analysis_parity", 0, 0),
        ("rewrite_shader_dct / dc / quant", 0, 0),
        ("shader_reconstruction", 0, 0),
    ]),
    ("07  Motion", [
        ("x264_motion_metrics_parity", 0, 0),
        ("x264_motion_compensation_parity", 0, 0),
        ("x264_reference_frame_parity", 0, 0),
        ("x264_motion_lowres_parity", 0, 0),
        ("x264_motion_integral_parity", 0, 0),
        ("x264_motion_search_parity", 0, 0),
        ("x264_motion_exhaustive_parity", 0, 0),
        ("x264_motion_bidir_parity", 0, 0),
        ("x264_mv_predict_parity", 0, 0),
        ("rewrite_motion", 0, 0),
    ]),
    ("08  Macroblock encode", [
        ("x264_i4x4_frame_parity", 0, 0),
        ("x264_i4x4_cabac_parity", 0, 0),
        ("vulkan_encoder_reconstruction", 0, 0),
        ("vulkan_cavlc_reconstruction", 0, 0),
    ]),
    ("09  Deblocking", [
        ("x264_deblock_filter_parity", 0, 0),
        ("x264_deblock_frame_parity", 0, 0),
        ("x264_deblock_frame_tiled_parity", 0, 0),
        ("x264_deblock_frame_subgroup_parity", 0, 0),
        ("x264_deblock_strength_parity", 0, 0),
    ]),
    ("10  Rate control", [
        ("x264_aq_parity", 0, 0),
    ]),
    ("11  Entropy coding", [
        ("x264_cabac_parity", 9946, 9946),
        ("x264_cabac_refidx_parity", 1536, 1536),
        ("x264_cabac_stream / raw decode", 0, 0),
        ("x264_cabac_decoder_oracle", 0, 0),
        ("x264_cavlc_residual_parity", 2327, 2327),
        ("x264_cavlc_context_parity", 400, 400),
        ("rewrite_cabac", 0, 0),
    ]),
    ("12  Orchestration / presets", [
        ("x264_parity_ultrafast_flat_16x16", 0, 0),
        ("x264_parity_ultrafast_flat_64x64", 0, 0),
        ("x264_parity_ultrafast_noise_32x32", 0, 0),
        ("x264_parity_fast_flat_16x16 / 64x64", 0, 0),
        ("x264_parity_superfast_flat_16x16", 0, 0),
    ]),
    ("INTEGRATION SWEEPS / CLIPS", [
        ("CAVLC P-slice sweep", CAVLC_P_EXACT, CAVLC_P_TOTAL),
        ("CABAC P-slice sweep", CABAC_P_EXACT, CABAC_P_TOTAL),
        ("CAVLC I-frame sweep", CAVLC_I_EXACT, CAVLC_I_TOTAL),
        ("Video MP4 experiments", VIDEO_EXACT, VIDEO_TOTAL),
        ("x264 function port inventory", FUNC_PORTED, FUNC_TOTAL_INV),
    ]),
]

# Open (incomplete) byte-exact targets — the things still being chased.
# exact/total are real numbers where available, else 0/total with a note.
OPEN_TARGETS = [
    ("Video MP4 experiments (real 8f clips)", VIDEO_EXACT, VIDEO_TOTAL, "#d29922"),
    ("x264 functions ported (inventory)", FUNC_PORTED, FUNC_TOTAL_INV, "#d29922"),
    ("Presets > ultrafast (SPS High profile)", 1, 1, "#2ea043"),
    ("1080p60 multi-frame stability (3f+)", 0, 1, "#f85149"),
    ("Lookahead / slicetype (03 family)", 0, 43, "#f85149"),
    ("B-frame inter analysis / CABAC / CAVLC", 0, 9, "#f85149"),
    ("RD cost / trellis (rdo.c)", 0, 26, "#f85149"),
    ("mbtree / frame_filter", 0, 6, "#f85149"),
    ("CQM scaling lists", 0, 2, "#f85149"),
    ("Rate control VBV / CRF", 0, 45, "#d29922"),
    ("I4x4/I8x8 CABAC emission (superfast)", 1, 1, "#2ea043"),
    ("Superfast NAL2 macroblock prefix", 130, 130, "#2ea043"),
    ("Full NAL2 byte-exact (all presets)", 2, 20, "#f85149"),
    ("04 Analysis weighted family coverage", 2, 10, "#d29922"),
]

COLOR = {
    "EXACT":  "#2ea043",
    "PARTIAL": "#d29922",
    "C++":    "#388bfd",
    "MISSING": "#30363d",
    "N/A":    "#484f58",
}
TINT = {
    "EXACT":  "#1b4726",
    "PARTIAL": "#4a3a12",
    "C++":    "#12325c",
    "MISSING": "#21262d",
    "N/A":    "#262b33",
}
# Fraction of a family's bar that counts as "byte-exact / done".
EXACTNESS = {"EXACT": 1.0, "C++": 1.0, "PARTIAL": 0.5, "MISSING": 0.0, "N/A": 0.0}

# ---------------------------------------------------------------------------
# SVG helpers
# ---------------------------------------------------------------------------
def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def pct(fams):
    exact = sum(w for _, w, st in fams if st == "EXACT")
    total = sum(w for _, w, st in fams if st not in ("N/A",)) or 1
    return exact / total


def stat_color(p):
    return COLOR["EXACT"] if p >= 0.75 else (COLOR["PARTIAL"] if p >= 0.4 else COLOR["MISSING"])


def bar(x, y, w, h, frac, fill, bg="#21262d", rx=None):
    """A single-progress bar: background + filled portion."""
    rx = rx if rx is not None else min(3, h / 2)
    s = f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h}" fill="{bg}" rx="{rx}"/>'
    if frac > 0:
        s += (f'<rect x="{x:.1f}" y="{y:.1f}" width="{max(1.0, w*min(1.0, frac)):.1f}" '
              f'height="{h}" fill="{fill}" rx="{rx}"/>')
    return s


def stackbar(x, y, w, h, fams):
    """A stacked bar showing the weighted status mix of a stage."""
    s = f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h}" fill="#21262d" rx="2"/>'
    total = sum(wt for _, wt, st in fams if st != "N/A") or 1
    cx = x
    for st in ("EXACT", "C++", "PARTIAL", "MISSING"):
        sw = sum(wt for _, wt, s2 in fams if s2 == st)
        if not sw:
            continue
        fw = w * sw / total
        s += f'<rect x="{cx:.1f}" y="{y:.1f}" width="{fw:.1f}" height="{h}" fill="{COLOR[st]}"/>'
        cx += fw
    return s


out = []
def add(s):
    out.append(s)

# Weighted function totals (derived from STAGES, excludes N/A)
_weight = {}
for _t, _s, _fams in STAGES:
    for _n, _w, _st in _fams:
        _weight[_st] = _weight.get(_st, 0) + _w
FUNC_EXACT = _weight.get("EXACT", 0)
FUNC_TOTAL = sum(v for k, v in _weight.items() if k != "N/A")
FUNC_PCT = FUNC_EXACT / FUNC_TOTAL * 100

# --- Layout -----------------------------------------------------------------
W = 1680
M = 32                       # left/right margin
gx, gy = M, 222
cols, rows = 4, 3
gap = 14
ch_base = 182                # stage card height (header + % + bar + tiles)
sweep_row_h = 15

# stage treemap band
treemap_h = 14 + rows * (ch_base + gap)
treemap_bottom = gy + treemap_h + 6

# families detail band: 3 columns, grouped by stage
fam_col_w = (W - 2 * M - 2 * gap) // 3
def fam_block_h(fams):
    return 20 + len(fams) * 15
# distribute the 12 stages round-robin into 3 columns, compute max column height
fam_cols = [[STAGES[i] for i in range(len(STAGES)) if i % 3 == c] for c in range(3)]
fam_band_h = max(sum(fam_block_h(f) + 6 for _, _, f in col) for col in fam_cols)
fam_top = treemap_bottom + 34

# suites detail band: 3 columns, grouped by suite group
suite_col_w = (W - 2 * M - 2 * gap) // 3
def suite_block_h(rows_):
    return 20 + len(rows_) * 15
suite_cols = [[SUITES[i] for i in range(len(SUITES)) if i % 3 == c] for c in range(3)]
suite_band_h = max(sum(suite_block_h(r) + 8 for _, r in col) for col in suite_cols)
suite_top = fam_top + fam_band_h + 40

# open targets band: 2 columns
ot_col_w = (W - 2 * M - gap) // 2
ot_rows_per_col = (len(OPEN_TARGETS) + 1) // 2
ot_band_h = 24 + ot_rows_per_col * 19
ot_top = suite_top + suite_band_h + 40

# sweeps band: two side-by-side sections (CAVLC | CABAC)
sweep_top = ot_top + ot_band_h + 34
sweep_max_rows = max(len(SWEEPS), len(CABAC_SWEEPS))
sweep_col_w = (W - 2 * M - gap) // 2
sweep_band_h = 30 + sweep_max_rows * sweep_row_h
sweep_bottom = sweep_top + sweep_band_h

# NAL2 macroblock prefix band (superfast progress toward byte-exact)
nal2_top = sweep_bottom + 34
nal2_band_h = 78

# legend
H = nal2_top + nal2_band_h + 70

add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
    f'viewBox="0 0 {W} {H}" font-family="Consolas, SFMono-Regular, Menlo, monospace">')
add(f'''<defs>
  <pattern id="grid-exact" width="9" height="9" patternUnits="userSpaceOnUse">
    <rect width="9" height="9" fill="{COLOR['EXACT']}"/>
    <rect x="1" y="1" width="7" height="7" fill="{TINT['EXACT']}"/>
  </pattern>
  <pattern id="grid-partial" width="9" height="9" patternUnits="userSpaceOnUse">
    <rect width="9" height="9" fill="{COLOR['PARTIAL']}"/>
    <rect x="1" y="1" width="7" height="7" fill="{TINT['PARTIAL']}"/>
  </pattern>
  <pattern id="grid-cpp" width="9" height="9" patternUnits="userSpaceOnUse">
    <rect width="9" height="9" fill="{COLOR['C++']}"/>
    <rect x="1" y="1" width="7" height="7" fill="{TINT['C++']}"/>
  </pattern>
  <pattern id="grid-missing" width="9" height="9" patternUnits="userSpaceOnUse">
    <rect width="9" height="9" fill="{COLOR['MISSING']}"/>
    <rect x="1" y="1" width="7" height="7" fill="{TINT['MISSING']}"/>
  </pattern>
  <pattern id="grid-na" width="9" height="9" patternUnits="userSpaceOnUse">
    <rect width="9" height="9" fill="{COLOR['N/A']}"/>
    <rect x="1" y="1" width="7" height="7" fill="{TINT['N/A']}"/>
  </pattern>
</defs>''')

# Background
add(f'<rect width="{W}" height="{H}" fill="#0d1117"/>')

# Title
add('<text x="32" y="48" fill="#e6edf3" font-size="30" font-weight="bold">'
    'Vk264 / Oracle Byte-Exact Parity Dashboard</text>')
add('<text x="32" y="74" fill="#8b949e" font-size="14">'
    f'function-by-function x264 &#8594; Vulkan port · snapshot {SNAPSHOT} · generated {GENERATED}'
    '</text>')

# Overall progress bar
ox, oy, ow = M, 92, W - 2 * M
add(bar(ox, oy, ow, 11, FUNC_EXACT / FUNC_TOTAL, COLOR["EXACT"], rx=5))
add(f'<text x="{ox+ow-4}" y="{oy+10}" fill="#c9d1d9" font-size="15" text-anchor="end">'
    f'byte-exact function coverage {FUNC_PCT:.1f}% ({FUNC_EXACT}/{FUNC_TOTAL} weighted families)</text>')

# Stat cards
sx, sy, sh = M, 120, 74
sw = (W - 2 * M - (len(STATS)-1)*16)//len(STATS)
for i, (label, value, percent, color) in enumerate(STATS):
    x = sx + i*(sw+16)
    add(f'<rect x="{x}" y="{sy}" width="{sw}" height="{sh}" fill="#161b22" '
        f'stroke="#30363d" rx="6"/>')
    add(f'<rect x="{x}" y="{sy}" width="4" height="{sh}" fill="{color}"/>')
    add(f'<text x="{x+14}" y="{sy+22}" fill="#8b949e" font-size="11">{esc(label)}</text>')
    # Shrink long values so they never collide with the right-hand percentage.
    vf = 16 if len(value) > 16 else (19 if len(value) > 12 else 22)
    add(f'<text x="{x+14}" y="{sy+48}" fill="#e6edf3" font-size="{vf}" font-weight="bold">{esc(value)}</text>')
    add(f'<text x="{x+sw-12}" y="{sy+48}" fill="{color}" font-size="16" text-anchor="end">{esc(percent)}</text>')

# --- Stage treemap (with stacked progress bar + per-family mini bars) --------
cw = (W - 2 * M - gap*(cols-1)) // cols
add(f'<text x="{gx}" y="{gy-10}" fill="#e6edf3" font-size="17" font-weight="bold">'
    'Pipeline stage coverage — stacked status bar + a progress bar on every function family</text>')

for idx, (title, subtitle, fams) in enumerate(STAGES):
    col, row = idx % cols, idx // cols
    x = gx + col*(cw+gap)
    y = gy + row*(ch_base+gap)
    add(f'<rect x="{x}" y="{y}" width="{cw}" height="{ch_base}" fill="#161b22" '
        f'stroke="#30363d" rx="6"/>')
    p = pct(fams)
    add(f'<text x="{x+12}" y="{y+22}" fill="#e6edf3" font-size="13" font-weight="bold">{esc(title)}</text>')
    add(f'<text x="{x+12}" y="{y+39}" fill="#6e7681" font-size="10">{esc(subtitle)}</text>')
    add(f'<text x="{x+cw-12}" y="{y+22}" fill="{stat_color(p)}" font-size="16" font-weight="bold" '
        f'text-anchor="end">{p*100:.0f}%</text>')
    # stacked status bar for the whole stage
    add(stackbar(x+12, y+46, cw-24, 8, fams))

    # family tiles packed into rows, each with a 3px exactness bar
    tx, ty = x+12, y+62
    tw_max = cw-24
    total_w = sum(w for _, w, _ in fams)
    scale = tw_max / max(total_w, 1)
    th, tgap = 32, 4
    curx, cury = tx, ty
    for name, w, st in fams:
        fw = max(40, w*scale - tgap)
        if curx + fw > tx + tw_max:
            curx = tx
            cury += th + tgap
        add(f'<rect x="{curx:.1f}" y="{cury}" width="{fw:.1f}" height="{th}" '
            f'fill="url(#grid-{st.lower().replace("+", "p")})" stroke="{COLOR[st]}" rx="2"/>')
        label = name if fw > 92 else (name[:11] + "…" if fw > 50 else "")
        if label:
            add(f'<text x="{curx+5:.1f}" y="{cury+16}" fill="#ffffff" font-size="9" '
                f'font-weight="bold">{esc(label)}</text>')
        # per-family progress bar at the bottom of every tile
        add(bar(curx+5, cury+th-8, fw-10, 4, EXACTNESS[st],
                COLOR[st], bg="#0d1117", rx=1))
        curx += fw + tgap

# --- Per-family parity detail (every family, grouped by stage) --------------
add(f'<text x="{gx}" y="{fam_top-10}" fill="#e6edf3" font-size="17" font-weight="bold">'
    'Function-family parity detail — every family has a byte-exact progress bar</text>')
for c, stage_list in enumerate(fam_cols):
    cx = gx + c * (fam_col_w + gap)
    cy = fam_top
    for title, subtitle, fams in stage_list:
        add(f'<text x="{cx}" y="{cy+12}" fill="#58a6ff" font-size="12" font-weight="bold">{esc(title)}</text>')
        cy += 20
        for name, w, st in fams:
            add(f'<text x="{cx}" y="{cy+11}" fill="#c9d1d9" font-size="10">{esc(name)}</text>')
            add(f'<text x="{cx+int(fam_col_w*0.52):d}" y="{cy+11}" fill="{COLOR[st]}" font-size="9">{st}</text>')
            add(bar(cx + int(fam_col_w*0.60), cy+3, int(fam_col_w*0.40), 9,
                    EXACTNESS[st], COLOR[st], rx=2))
            cy += 15
        cy += 6

# --- Byte-exact verification suites (real case counts) ----------------------
add(f'<text x="{gx}" y="{suite_top-10}" fill="#e6edf3" font-size="17" font-weight="bold">'
    'Byte-exact verification suites — pass bars with real case counts</text>')
for c, suite_list in enumerate(suite_cols):
    cx = gx + c * (suite_col_w + gap)
    cy = suite_top
    for group, rows_ in suite_list:
        add(f'<text x="{cx}" y="{cy+12}" fill="#58a6ff" font-size="12" font-weight="bold">{esc(group)}</text>')
        cy += 20
        for name, exact, total in rows_:
            add(f'<text x="{cx}" y="{cy+11}" fill="#c9d1d9" font-size="10">{esc(name)}</text>')
            frac = 1.0 if total == 0 else exact / total
            col = COLOR["EXACT"] if frac >= 0.999 else (COLOR["PARTIAL"] if frac > 0 else COLOR["MISSING"])
            lbl = "pass" if total == 0 else f"{exact}/{total}"
            add(bar(cx + int(suite_col_w*0.66), cy+3, int(suite_col_w*0.24), 9, frac, col, rx=2))
            add(f'<text x="{cx+suite_col_w-2}" y="{cy+11}" fill="{col}" font-size="9" '
                f'text-anchor="end">{lbl}</text>')
            cy += 15
        cy += 8

# --- Open byte-exact targets -------------------------------------------------
add(f'<text x="{gx}" y="{ot_top-10}" fill="#e6edf3" font-size="17" font-weight="bold">'
    'Open byte-exact targets — progress on what is still incomplete</text>')
for i, (name, exact, total, col) in enumerate(OPEN_TARGETS):
    c = 0 if i < ot_rows_per_col else 1
    r = i if i < ot_rows_per_col else i - ot_rows_per_col
    cx = gx + c * (ot_col_w + gap)
    cy = ot_top + 16 + r * 19
    add(f'<text x="{cx}" y="{cy+11}" fill="#c9d1d9" font-size="10">{esc(name)}</text>')
    frac = 0.0 if total == 0 else exact / total
    add(bar(cx + int(ot_col_w*0.62), cy+3, int(ot_col_w*0.24), 10, frac, col, rx=2))
    add(f'<text x="{cx+ot_col_w-2}" y="{cy+11}" fill="{col}" font-size="9" text-anchor="end">'
        f'{exact}/{total}</text>')

# --- Sweep progression sections (side by side) ------------------------------
add(f'<text x="{gx}" y="{sweep_top}" fill="#e6edf3" font-size="17" font-weight="bold">'
    'CAVLC P-slice byte-exact sweep — by merged PR</text>')
add(f'<text x="{gx+ot_col_w+gap}" y="{sweep_top}" fill="#e6edf3" font-size="17" font-weight="bold">'
    'CABAC P-slice byte-exact sweep — by merged PR</text>')
for c, (sweeps, bx0) in enumerate(((SWEEPS, gx), (CABAC_SWEEPS, gx+ot_col_w+gap))):
    bar_x = bx0 + 96
    bar_w = sweep_col_w - 96 - 76
    for i, (label, val, color) in enumerate(sweeps):
        y = sweep_top + 16 + i*sweep_row_h
        add(f'<text x="{bx0+8}" y="{y+11}" fill="#8b949e" font-size="11">{esc(label)}</text>')
        add(bar(bar_x, y, bar_w, 11, val/1360, color, rx=2))
        add(f'<text x="{bar_x+bar_w+8}" y="{y+11}" fill="#e6edf3" font-size="11">{val}/1360</text>')

# --- Superfast NAL2 macroblock byte-exact prefix progression ------------------
add(f'<text x="{gx}" y="{nal2_top}" fill="#e6edf3" font-size="17" font-weight="bold">'
    'Superfast IDR NAL2 byte-exact prefix — by merged PR (macroblock payload parity)</text>')
add(f'<text x="{gx}" y="{nal2_top+16}" fill="#8b949e" font-size="11">'
    f'Bytes of the slice NAL2 identical to x264 before the first divergence (full NAL2 = {NAL2_FULL} bytes)</text>')
nal2_x = gx + 8
nal2_y = nal2_top + 28
nal2_cell = (W - 2 * M - 16) // len(NAL2_PREFIX_PROGRESSION)
for i, (label, val, color) in enumerate(NAL2_PREFIX_PROGRESSION):
    cx = nal2_x + i * nal2_cell
    add(f'<text x="{cx+6}" y="{nal2_y+12}" fill="#8b949e" font-size="11">{esc(label)}</text>')
    add(f'<text x="{cx+6}" y="{nal2_y+42}" fill="{color}" font-size="20" font-weight="bold">{val}</text>')
    add(bar(cx+6, nal2_y+50, nal2_cell-16, 8, val/NAL2_FULL, color, rx=2))
    add(f'<text x="{cx+6}" y="{nal2_y+66}" fill="#6e7681" font-size="10">bytes exact</text>')

# --- Legend ------------------------------------------------------------------
ly = H - 28
lx = M
add(f'<text x="{lx}" y="{ly}" fill="#8b949e" font-size="12">LEGEND:</text>')
lx += 80
for st in ["EXACT", "PARTIAL", "C++", "MISSING", "N/A"]:
    add(f'<rect x="{lx}" y="{ly-11}" width="14" height="14" fill="url(#grid-{st.lower().replace("+", "p")})" '
        f'stroke="{COLOR[st]}" rx="2"/>')
    add(f'<text x="{lx+20}" y="{ly}" fill="#c9d1d9" font-size="12">{st}</text>')
    lx += 130
add(f'<text x="{lx}" y="{ly}" fill="#8b949e" font-size="11">'
    'mini-bar fill = byte-exact fraction (EXACT/C++ 100%, PARTIAL 50%, MISSING 0%)</text>')
add(f'<text x="{W-M}" y="{ly}" fill="#6e7681" font-size="11" text-anchor="end">'
    'regenerate: python docs/generate_parity_dashboard.py</text>')

add('</svg>')

dest = Path(__file__).with_name("parity-dashboard.svg")
dest.write_text("\n".join(out), encoding="utf-8")
print(f"wrote {dest}  ({len(out)} svg elements, {dest.stat().st_size} bytes)")
print(f"layout: H={H}  stages={len(STAGES)}  families={sum(len(f) for _,_,f in STAGES)}  "
      f"suite groups={len(SUITES)}  open targets={len(OPEN_TARGETS)}")
