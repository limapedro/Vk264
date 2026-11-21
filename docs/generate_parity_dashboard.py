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
"""
from pathlib import Path
from datetime import date

# ---------------------------------------------------------------------------
# Data — update these after each parity PR, then regenerate the SVG.
# ---------------------------------------------------------------------------
SNAPSHOT = "605296f"
GENERATED = date.today().isoformat()

# Overall numbers (from build/cavlc_parity_sweep.py, cavlc_iframe_sweep.py, ctest)
# FUNCTION PARITY is derived from the weighted STAGES table below so the
# dashboard stays self-consistent when a family changes status.
CAVLC_P_EXACT, CAVLC_P_TOTAL = 1215, 1360
CABAC_P_EXACT, CABAC_P_TOTAL = 960, 1360
STATS = [
    ("CAVLC I-FRAMES", "72 / 72", "100.0%", "#2ea043"),
    ("CAVLC P-SLICES", f"{CAVLC_P_EXACT} / {CAVLC_P_TOTAL}",
     f"{CAVLC_P_EXACT/CAVLC_P_TOTAL*100:.1f}%", "#d29922"),
    ("CABAC P-SLICES", f"{CABAC_P_EXACT} / {CABAC_P_TOTAL}",
     f"{CABAC_P_EXACT/CABAC_P_TOTAL*100:.1f}%", "#d29922"),
    ("CTEST GATES", "65 + 18 / 83", "100.0%", "#2ea043"),
    ("x264 INVENTORY", "669 funcs / 35 files", "42 N/A", "#388bfd"),
]

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

# ---------------------------------------------------------------------------
# SVG helpers
# ---------------------------------------------------------------------------
def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def pct(fams):
    exact = sum(w for _, w, st in fams if st == "EXACT")
    total = sum(w for _, w, st in fams if st not in ("N/A",)) or 1
    return exact / total


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

W, H = 1680, 1720
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
ox, oy, ow = 32, 92, W - 64
add(f'<rect x="{ox}" y="{oy}" width="{ow}" height="10" fill="#21262d" rx="5"/>')
add(f'<rect x="{ox}" y="{oy}" width="{ow*FUNC_EXACT/FUNC_TOTAL:.1f}" height="10" '
    f'fill="{COLOR["EXACT"]}" rx="5"/>')
add(f'<text x="{ox+ow-4}" y="{oy+9}" fill="#8b949e" font-size="12" text-anchor="end">'
    f'byte-exact function coverage {FUNC_PCT:.1f}% ({FUNC_EXACT}/{FUNC_TOTAL} weighted families)</text>')

# Stat cards
sx, sy, sh = 32, 120, 74
sw = (W - 64 - (len(STATS)-1)*16)//len(STATS)
for i, (label, value, percent, color) in enumerate(STATS):
    x = sx + i*(sw+16)
    add(f'<rect x="{x}" y="{sy}" width="{sw}" height="{sh}" fill="#161b22" '
        f'stroke="#30363d" rx="6"/>')
    add(f'<rect x="{x}" y="{sy}" width="4" height="{sh}" fill="{color}"/>')
    add(f'<text x="{x+14}" y="{sy+22}" fill="#8b949e" font-size="11">{esc(label)}</text>')
    add(f'<text x="{x+14}" y="{sy+48}" fill="#e6edf3" font-size="22" font-weight="bold">{esc(value)}</text>')
    add(f'<text x="{x+sw-12}" y="{sy+48}" fill="{color}" font-size="18" text-anchor="end">{esc(percent)}</text>')

# Stage treemap — 4 columns x 3 rows
gx, gy = 32, 214
cols, rows = 4, 3
gap = 14
cw = (W - 64 - gap*(cols-1)) // cols
ch_base = 300
add(f'<text x="{gx}" y="{gy-10}" fill="#e6edf3" font-size="17" font-weight="bold">'
    'Pipeline stage coverage (tile size ~ function count)</text>')

for idx, (title, subtitle, fams) in enumerate(STAGES):
    col, row = idx % cols, idx // cols
    x = gx + col*(cw+gap)
    y = gy + 14 + row*(ch_base+gap)
    add(f'<rect x="{x}" y="{y}" width="{cw}" height="{ch_base}" fill="#161b22" '
        f'stroke="#30363d" rx="6"/>')
    p = pct(fams)
    pcol = COLOR["EXACT"] if p >= 0.75 else (COLOR["PARTIAL"] if p >= 0.4 else COLOR["MISSING"])
    add(f'<text x="{x+12}" y="{y+24}" fill="#e6edf3" font-size="13" font-weight="bold">{esc(title)}</text>')
    add(f'<text x="{x+12}" y="{y+42}" fill="#6e7681" font-size="10">{esc(subtitle)}</text>')
    add(f'<text x="{x+cw-12}" y="{y+24}" fill="{pcol}" font-size="16" font-weight="bold" '
        f'text-anchor="end">{p*100:.0f}%</text>')

    # Family tiles packed into rows
    tx, ty = x+12, y+54
    tw_max = cw-24
    total_w = sum(w for _, w, _ in fams)
    scale = tw_max / max(total_w, 1)
    th = 30
    tgap = 4
    curx, cury = tx, ty
    for name, w, st in fams:
        fw = max(36, w*scale - tgap)
        if curx + fw > tx + tw_max:
            curx = tx
            cury += th + tgap
        add(f'<rect x="{curx:.1f}" y="{cury}" width="{fw:.1f}" height="{th}" '
            f'fill="url(#grid-{st.lower().replace("+", "p")})" stroke="{COLOR[st]}" rx="2"/>')
        label = name if fw > 90 else (name[:10] + "…" if fw > 46 else "")
        if label:
            add(f'<text x="{curx+5:.1f}" y="{cury+19}" fill="#ffffff" font-size="9" '
                f'font-weight="bold">{esc(label)}</text>')
        curx += fw + tgap

# Sweep progression section
by = gy + 14 + rows*(ch_base+gap) + 8
add(f'<text x="{gx}" y="{by}" fill="#e6edf3" font-size="17" font-weight="bold">'
    'CAVLC P-slice byte-exact sweep — progress by merged PR</text>')
bar_x, bar_w = gx+240, W-64-240-120
for i, (label, val, color) in enumerate(SWEEPS):
    y = by + 24 + i*19
    add(f'<text x="{gx+8}" y="{y+11}" fill="#8b949e" font-size="12">{esc(label)}</text>')
    add(f'<rect x="{bar_x}" y="{y}" width="{bar_w}" height="13" fill="#21262d" rx="3"/>')
    add(f'<rect x="{bar_x}" y="{y}" width="{bar_w*val/1360:.1f}" height="13" fill="{color}" rx="3"/>')
    add(f'<text x="{bar_x+bar_w+10}" y="{y+11}" fill="#e6edf3" font-size="12">{val}/1360</text>')

# Legend
ly, lx = H-40, 32
add(f'<text x="{lx}" y="{ly}" fill="#8b949e" font-size="12">LEGEND:</text>')
lx += 80
for st in ["EXACT", "PARTIAL", "C++", "MISSING", "N/A"]:
    add(f'<rect x="{lx}" y="{ly-11}" width="14" height="14" fill="url(#grid-{st.lower().replace("+", "p")})" '
        f'stroke="{COLOR[st]}" rx="2"/>')
    add(f'<text x="{lx+20}" y="{ly}" fill="#c9d1d9" font-size="12">{st}</text>')
    lx += 130
add(f'<text x="{W-32}" y="{ly}" fill="#6e7681" font-size="11" text-anchor="end">'
    'regenerate: python docs/generate_parity_dashboard.py</text>')

add('</svg>')

dest = Path(__file__).with_name("parity-dashboard.svg")
dest.write_text("\n".join(out), encoding="utf-8")
print(f"wrote {dest}  ({len(out)} svg elements, {dest.stat().st_size} bytes)")
