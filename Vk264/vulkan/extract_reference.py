# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract upstream scalar routines verbatim for the standalone GPU tests."""

from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
source = path.read_text(encoding="utf-8")
intra16 = Path(sys.argv[2]).name == "upstream_intra16.inc"
if intra16:
    macros = functions = ()
elif path.name == "quant.c":
    macros = ("QUANT_ONE", "DEQUANT_SHL", "DEQUANT_SHR", "IDCT_DEQUANT_2X4_START")
    functions = ("quant_4x4", "quant_8x8", "quant_4x4x4", "dequant_4x4", "dequant_8x8",
                 "quant_4x4_dc", "quant_2x2_dc", "dequant_4x4_dc",
                 "idct_dequant_2x4_dc", "idct_dequant_2x4_dconly")
elif path.name == "macroblock.c":
    macros = ("IDCT_DEQUANT_2X2_START",)
    functions = ("idct_dequant_2x2_dc", "idct_dequant_2x2_dconly", "dct2x2dc")
elif path.name == "dct.c":
    macros = ("DCT8_1D", "IDCT8_1D")
    functions = ("pixel_sub_wxh", "sub4x4_dct", "add4x4_idct", "sub8x8_dct8", "add8x8_idct8",
                 "dct4x4dc", "idct4x4dc", "sub4x4_dct_dc", "sub8x8_dct_dc",
                 "sub8x16_dct_dc", "dct2x4dc", "sub8x8_dct", "sub16x16_dct",
                 "add8x8_idct", "add16x16_idct", "sub16x16_dct8", "add16x16_idct8",
                 "add4x4_idct_dc", "add8x8_idct_dc", "add16x16_idct_dc")
elif path.name in ("tables.c", "predict.c", "deblock.c", "cabac.c", "mvpred.c", "mc.c", "pixel.c", "me.c",
                   "bitstream.c", "bitstream.h", "set.c", "set.h", "encoder.c"):
    macros = functions = ()
else:
    raise SystemExit(f"Unsupported reference source {path}")
output = []
for macro in macros:
    match = re.search(rf"^#define {macro}(?=[ (])", source, flags=re.MULTILINE)
    if not match:
        raise SystemExit(f"Cannot find {macro} in {path}")
    lines = source[match.start():].splitlines()
    last = next(i for i, line in enumerate(lines) if not line.rstrip().endswith(chr(92)))
    output.append("\n".join(lines[:last + 1]))
for name in functions:
    pattern = rf"^static (?:inline )?(?:void|int) {name}\([^{{]+\n\{{.*?^\}}"
    match = re.search(pattern, source, flags=re.MULTILINE | re.DOTALL)
    if not match:
        raise SystemExit(f"Cannot find {name} in {path}")
    output.append(match[0])
if path.name == "dct.c":
    start = source.index("#define ZIG(i,y,x) level[i] = dct[x*8+y];")
    end = source.index("void x264_zigzag_init(", start)
    output.extend((
        "#define CPPIXEL_X4(dst, src) std::memcpy((dst), (src), 4 * sizeof(pixel))",
        source[start:end],
        "#undef CPPIXEL_X4",
    ))
elif path.name == "quant.c":
    start = source.index("static ALWAYS_INLINE void optimize_chroma_idct_dequant_2x4(")
    end = source.index("level_run(16)", start) + len("level_run(16)")
    output.extend(("#define ALWAYS_INLINE inline", source[start:end],
                   "#undef level_run", "#undef last", "#undef ALWAYS_INLINE"))
elif path.name == "tables.c":
    start = source.index("const uint16_t x264_lambda_tab")
    output.append(source[start:source.index("};", start)+2].replace("QP_MAX_MAX+1", "82"))
    start = source.index("const int x264_lambda2_tab")
    output.append(source[start:source.index("};", start)+2].replace("QP_MAX_MAX+1", "82"))
    start = source.index("const uint8_t x264_cqm_jvt4i[16]")
    end = source.index("// 720p_avci50", start)
    output.append(source[start:end])
    for suffix in (4, 8):
        pattern = rf"^const uint8_t x264_decimate_table{suffix}\[\d+\] =\n\{{.*?^\}};"
        match = re.search(pattern, source, flags=re.MULTILINE | re.DOTALL)
        if not match:
            raise SystemExit(f"Cannot find decimation table {suffix} in {path}")
        output.append(match[0])
    for plane in (0, 1):
        match = re.search(rf"^const uint8_t x264_hpel_ref{plane}\[16\] = .*?;", source, flags=re.MULTILINE)
        if not match:
            raise SystemExit(f"Cannot find motion compensation plane {plane} in {path}")
        output.append(match[0])
    for table in ("context_init_I", "context_init_PB", "range_lps", "transition", "renorm_shift"):
        pattern = rf"^const u?int8_t x264_cabac_{table}\[[^=]+ =\n\{{.*?^\}};"
        match = re.search(pattern, source, flags=re.MULTILINE | re.DOTALL)
        if not match:
            raise SystemExit(f"Cannot find CABAC table {table} in {path}")
        output.append(match[0])
    for table in ("significant_coeff_flag_offset", "last_coeff_flag_offset", "coeff_abs_level_m1_offset",
                  "count_cat_m1", "significant_coeff_flag_offset_8x8", "last_coeff_flag_offset_8x8",
                  "coeff_flag_offset_chroma_422_dc"):
        match = re.search(rf"^const uint(?:8|16)_t x264_{table}\[[^=]+ =.*?;", source,
                          flags=re.MULTILINE | re.DOTALL)
        if not match:
            raise SystemExit(f"Cannot find CABAC residual table {table} in {path}")
        output.append(match[0])
elif path.name == "predict.c":
    start = source.index("#define PREDICT_16x16_DC")
    end = source.index("void x264_predict_16x16_init(", start)
    output.append(source[start:end])
    output.extend(f"#undef {name}" for name in
                  ("PREDICT_16x16_DC", "SRC", "SRC_X4", "PREDICT_4x4_DC", "PREDICT_4x4_LOAD_LEFT",
                   "PREDICT_4x4_LOAD_TOP", "PREDICT_4x4_LOAD_TOP_RIGHT", "F1", "F2", "PL", "PT",
                   "PREDICT_8x8_LOAD_TOPLEFT", "PREDICT_8x8_LOAD_LEFT", "PREDICT_8x8_LOAD_TOP",
                   "PREDICT_8x8_LOAD_TOPRIGHT", "PREDICT_8x8_DC"))
elif path.name == "deblock.c":
    if Path(sys.argv[2]).suffix == ".glsl":
        for table, glsl_name in (("alpha", "alphaTable"), ("beta", "betaTable"), ("tc0", "tcTable")):
            match = re.search(rf"i_{table}_table\[[^=]+ =\n\{{(.*?)\n\}};", source, flags=re.DOTALL)
            if not match:
                raise SystemExit(f"Cannot find deblocking table {table} in {path}")
            values = [int(value) for value in re.findall(r"-?\d+", match[1])]
            if table == "tc0":
                if len(values) != 88 * 4:
                    raise SystemExit("Expected 88 four-component clipping rows")
                rows = ["ivec4(" + ", ".join(map(str, values[i:i + 4])) + ")" for i in range(0, len(values), 4)]
                output.append(f"const ivec4 {glsl_name}[88] = ivec4[88](\n    " +
                              ",\n    ".join(rows) + "\n);")
            else:
                if len(values) != 88:
                    raise SystemExit(f"Expected 88 values for {table}")
                output.append(f"const int {glsl_name}[88] = int[88](\n    " +
                              ", ".join(map(str, values)) + "\n);")
    else:
        start = source.index("static const uint8_t i_alpha_table")
        end = source.index("static ALWAYS_INLINE void macroblock_cache_load_neighbours_deblock(", start)
        output.extend(("#define ALWAYS_INLINE inline", source[start:end], "#undef ALWAYS_INLINE",
                       "#undef alpha_table", "#undef beta_table", "#undef tc0_table"))
elif path.name == "cabac.c":
    if path.parent.name == "encoder":
        predict = (path.parent.parent / "common/predict.h").read_text(encoding="utf-8")
        start = predict.index("enum intra_chroma_pred_e")
        end = predict.index("enum intra4x4_pred_e", start)
        output.append(predict[start:end])
        for first, last in (
            ("static inline void cabac_mb_type_intra(", "#if !RDO_SKIP_BS\nstatic void cabac_field_decoding_flag("),
            ("static void cabac_intra4x4_pred_mode(", "static inline void cabac_subpartition_p("),
            ("static ALWAYS_INLINE void cabac_transform_size(", "static ALWAYS_INLINE void cabac_ref_internal("),
            ("static ALWAYS_INLINE int cabac_mvd_cpn(", "static NOINLINE uint16_t cabac_mvd("),
        ):
            start = source.index(first)
            output.append(source[start:source.index(last, start)])
        start = source.index("// node ctx: 0..3:")
        end = source.index("static ALWAYS_INLINE void cabac_block_residual(", start)
        output.append(source[start:end] + "#endif\n#undef WRITE_SIGMAP")
    else:
        output.append(source[source.index("static uint8_t cabac_contexts["):])
elif path.name == "mvpred.c":
    base = (path.parent / "base.h").read_text(encoding="utf-8")
    for first, last in (
        ("static ALWAYS_INLINE int x264_median(", "static ALWAYS_INLINE int x264_predictor_difference("),
        ("static ALWAYS_INLINE uint16_t x264_cabac_mvd_sum(", "/*****"),
    ):
        start = base.index(first)
        output.append(base[start:base.index(last, start)])
    start = source.index("void x264_mb_predict_mv_16x16(")
    output.append(source[start:source.index("static int mb_predict_mv_direct16x16_temporal(", start)])
elif path.name == "mc.c":
    start = source.index("static inline void pixel_avg(")
    end = source.index("void x264_plane_copy_c(", start)
    integral = source.index("static void integral_init4h(", end)
    integral_end = source.index("void x264_frame_init_lowres(", integral)
    lowres = source.index("static void frame_init_lowres_core(", integral_end)
    lowres_end = source.index("static void mbtree_propagate_cost(", lowres)
    output.extend((source[start:end], "#undef PIXEL_AVG_C", "#undef MC_WEIGHT_C",
                   "#undef MC_COPY", "#undef TAPFILTER", "#undef opscale", "#undef opscale_noden",
                   source[integral:integral_end], source[lowres:lowres_end]))
elif path.name == "pixel.c":
    sad = source.index("#define PIXEL_SAD_C")
    ssd_end = source.index("uint64_t x264_pixel_ssd_wxh(", sad)
    satd = source.index("#if BIT_DEPTH > 8", ssd_end)
    satd_end = source.index("static NOINLINE uint64_t pixel_hadamard_ac(", satd)
    batch = source.index("#define SAD_X(", satd_end)
    batch_end = source.index("#if HAVE_MMX", batch)
    var2 = source.index("#define PIXEL_VAR2_C")
    var2_end = source.index("#if BIT_DEPTH > 8", var2)
    ads = source.index("static int x264_pixel_ads4(", batch_end)
    ads_end = source.index("void x264_pixel_init(", ads)
    output.extend(("#define ALWAYS_INLINE inline", "#define NOINLINE", "#define BIT_DEPTH 8",
                   source[sad:ssd_end], source[satd:satd_end], source[batch:batch_end],
                   source[ads:ads_end], source[var2:var2_end], "#undef PIXEL_VAR2_C"))
    output.extend(f"#undef {name}" for name in
                  ("ALWAYS_INLINE", "NOINLINE", "BIT_DEPTH", "PIXEL_SAD_C", "PIXEL_SSD_C",
                   "PIXEL_SATD_C", "HADAMARD4", "BITS_PER_SUM", "SAD_X", "SATD_X", "SATD_X_DECL6", "SATD_X_DECL7"))
elif path.name == "me.c":
    root = path.parent.parent
    for relative, first, last in (
        ("common/macroblock.h", "static ALWAYS_INLINE uint32_t pack16to32_mask(",
         "static ALWAYS_INLINE uint64_t pack32to64("),
        ("common/base.h", "static ALWAYS_INLINE int x264_predictor_difference(",
         "static ALWAYS_INLINE uint16_t x264_cabac_mvd_sum("),
        ("common/common.h", "static ALWAYS_INLINE int x264_predictor_roundclip(",
         "#if ARCH_X86 || ARCH_X86_64"),
        ("encoder/me.h", "#define COPY1_IF_LT", "#define COPY2_IF_GT"),
    ):
        included = (root / relative).read_text(encoding="utf-8")
        start = included.index(first)
        output.append(included[start:included.index(last, start)])
    start = source.index("static const uint8_t subpel_iterations")
    end = source.index("void x264_me_refine_bidir_rd(", start)
    output.append(source[start:end])
    output.extend(f"#undef {name}" for name in
                  ("COPY1_IF_LT", "COPY2_IF_LT", "COPY3_IF_LT", "COPY4_IF_LT", "BITS_MVD", "COST_MV_HPEL",
                   "COST_MV_X3_DIR", "COST_MV_X4_DIR", "COST_MV_X4", "COST_MV_X3_ABS", "DIA1_ITER", "CROSS",
                   "FPEL", "SPEL", "SPELx2", "pack16to32_mask2", "CHECK_MVRANGE", "SAD_THRESH",
                   "COST_MV_SAD", "COST_MV_SATD", "BIME_CACHE", "SATD_THRESH", "x264_iter_kludge"))
elif path.name == "bitstream.c":
    escape = source.index("static uint8_t *nal_escape_c(")
    escape_end = source.index("#if HAVE_MMX", escape)
    nal = source.index("void x264_nal_encode(", escape_end)
    nal_end = source.index("void x264_bitstream_init(", nal)
    output.extend((source[escape:escape_end], source[nal:nal_end]))
elif path.name == "bitstream.h":
    start = source.index("static inline void bs_init(")
    output.append(source[start:source.rindex("#endif")])
elif path.name == "set.c":
    for first, last in (
        ("static void scaling_list_write(", "void x264_sei_write("),
        ("void x264_sps_write(", "void x264_pps_init("),
        ("void x264_pps_write(", "void x264_sei_recovery_point_write("),
    ):
        start = source.index(first)
        output.append(source[start:source.index(last, start)])
elif path.name == "set.h":
    start = source.index("enum cqm4_e")
    end = source.index("#define x264_cqm_init", start)
    declarations = source[start:end]
    if Path(sys.argv[2]).suffix == ".glsl":
        declarations = re.sub(r"/\*.*?\*/|//[^\n]*", "", declarations, flags=re.DOTALL)
        declarations = re.sub(r"^enum .*?^};\s*", "", declarations, flags=re.MULTILINE | re.DOTALL)
        declarations = re.sub(r"^\s*const uint8_t \*scaling_list\[8\];", "", declarations, flags=re.MULTILINE)
        declarations = declarations.replace("uint32_t", "uint")
        nested = re.compile(r"\bstruct\s*\{([^{}]*)\}\s*(crop|hrd|vui);")
        while match := nested.search(declarations):
            name = "Sps" + match[2].title()
            members = "\n".join("    " + line.strip() for line in match[1].splitlines() if line.strip())
            output.append(f"struct {name} {{\n{members}\n}};")
            declarations = declarations[:match.start()] + f"{name} {match[2]};" + declarations[match.end():]
        for original, translated in (("x264_sps_t", "Sps"), ("x264_pps_t", "Pps")):
            match = re.search(rf"typedef struct\s*\{{([^{{}}]+)\}}\s*{original};", declarations)
            if not match:
                raise SystemExit(f"Cannot find {original} in {path}")
            members = "\n".join("    " + line.strip() for line in match[1].splitlines() if line.strip())
            output.append(f"struct {translated} {{\n{members}\n}};")
    else:
        output.append(declarations)
        macroblock = (path.parent / "macroblock.h").read_text(encoding="utf-8")
        start = macroblock.index("static const uint8_t x264_zigzag_scan4")
        end = macroblock.index("static const uint8_t block_idx_x", start)
        output.append(macroblock[start:end])
elif path.name == "encoder.c":
    common = (path.parent.parent / "common/common.h").read_text(encoding="utf-8")
    end = common.index("} x264_slice_header_t;") + len("} x264_slice_header_t;")
    output.append(common[common.rindex("typedef struct", 0, end):end])
    start = source.index("static void slice_header_write(")
    end = source.index("/* If we are within a reasonable distance", start)
    output.append(source[start:end])
if intra16:
    root = path.parent.parent
    for relative, first, last in (
        ("common/base.h", "#define LUMA_DC", "/*****"),
        ("common/macroblock.h", "static const uint8_t block_idx_xy_1d", "#define QP("),
        ("common/macroblock.h", "enum cabac_ctx_block_cat_e", "/* Per-frame allocation:"),
        ("encoder/macroblock.h", "#define STORE_8x8_NNZ", "static ALWAYS_INLINE void x264_mb_encode_i4x4("),
    ):
        header = (root / relative).read_text(encoding="utf-8")
        start = header.index(first)
        output.append(header[start:header.index(last, start)])
    start = source.index("static ALWAYS_INLINE int array_non_zero(")
    end = source.index("void x264_mb_encode_chroma(", start)
    dc_scan = source.index("#define ZIG(i,y,x)")
    common_macroblock = (root / "common/macroblock.c").read_text(encoding="utf-8")
    copy_column = common_macroblock.index("NOINLINE void x264_copy_column8(")
    lossless = source.index("void x264_predict_lossless_chroma(")
    inter = source.index("else    /* Inter MB */")
    inter8 = source.index("            ALIGNED_ARRAY_64( dctcoef, dct8x8,[4],[64] );", inter)
    inter8_end = source.index("\n        }\n        else\n", inter8)
    macroblock_header = (root / "encoder/macroblock.h").read_text(encoding="utf-8")
    quant8 = macroblock_header.index("static ALWAYS_INLINE int x264_quant_8x8(")
    scales = (root / "common/set.c").read_text(encoding="utf-8")
    scan8 = scales.index("static const uint8_t quant8_scan[")
    inter4 = source.index("            ALIGNED_ARRAY_64( dctcoef, dct4x4,[16],[16] );", inter)
    inter4_end = source.index("\n        }\n    }\n\n    /* encode chroma */", inter4)
    output.extend((source[dc_scan:source.index("#define IDCT_DEQUANT_2X2_START", dc_scan)],
                   common_macroblock[copy_column:common_macroblock.index("static ALWAYS_INLINE", copy_column)],
                   source[lossless:source.index("void x264_predict_lossless_4x4(", lossless)],
                   source[start:end],
                   scales[scan8:scales.index("int x264_cqm_init(", scan8)],
                   macroblock_header[quant8:macroblock_header.index("#define STORE_8x8_NNZ", quant8)],
                   "static void reference_inter8x8(x264_t *h, int i_qp, int plane_count, int b_decimate)\n"
                   "{\n    int nz, i_decimate_mb = 0;\n" + source[inter8:inter8_end] + "\n}",
                   "static void reference_inter4x4(x264_t *h, int i_qp, int plane_count, int b_decimate)\n"
                   "{\n    int nz, i_decimate_mb = 0;\n" + source[inter4:inter4_end] + "\n}",
                   "#undef CLEAR_16x16_NNZ", "#undef STORE_8x8_NNZ", "#undef FOREACH_BIT",
                   "#undef LUMA_DC", "#undef CHROMA_DC"))
Path(sys.argv[2]).write_text("\n\n".join(output) + "\n", encoding="utf-8")
