// Vk264 parity module: fixtures_macroblock.inc
// Mirrors: x264 encoder/macroblock.c
// Split from dct_parity.cpp lines 4152-4601.

namespace intraCorpus {
constexpr int origin = FDEC_STRIDE+8;
struct Fixture {
    std::int32_t qp, mode, plane, field, decimate, denoise, cbp;
    std::array<std::uint32_t, 16> multiplier, bias, noiseOffset, residualSum;
    std::array<std::int32_t, 16> dequant;
    std::array<std::uint32_t, 256> source;
    std::array<std::uint32_t, 17*FDEC_STRIDE> pixels;
    std::array<std::uint32_t, 256> interPixels; // scratch: inter prediction backup
    std::int32_t intraInP;                      // 1 => P-slice MB coded as intra
    std::array<std::int32_t, 256> ac;
    std::array<std::int32_t, 16> dc;
    std::array<std::uint32_t, X264_SCAN8_SIZE> nonzero;
};
static_assert(sizeof(Fixture) == (7+5*16+256+17*FDEC_STRIDE+256+1+256+16+X264_SCAN8_SIZE)*4);

template<class Block>
void setFlatQuant(Block& block, int deadzone) {
    constexpr int quant4[][3] = {
        {13107,8066,5243}, {11916,7490,4660}, {10082,6554,4194},
        {9362,5825,3647}, {8192,5243,3355}, {7282,4559,2893},
    };
    constexpr int dequant4[][3] = {{10,13,16}, {11,14,18}, {13,16,20}, {14,18,23}, {16,20,25}, {18,23,29}};
    const int shift = block.qp/6-1;
    for (int i = 0; i < 16; ++i) {
        const int parity = (i&1)+((i>>2)&1);
        const int value = quant4[block.qp%6][parity];
        const int shifted = shift <= 0 ? value << -shift : (value+(1 << (shift-1))) >> shift;
        block.multiplier[i] = shifted;
        block.bias[i] = std::min((deadzone*1024+shifted/2)/shifted, 32768/shifted);
        block.dequant[i] = 16*dequant4[block.qp%6][parity];
    }
}
}

void verifyLuma4x4(const char* shader, bool inter) {
    using namespace intraCorpus;
    constexpr auto predictors = std::array{x264_predict_16x16_v_c, x264_predict_16x16_h_c,
        x264_predict_16x16_dc_c, x264_predict_16x16_p_c, predict_16x16_dc_left_c,
        predict_16x16_dc_top_c, predict_16x16_dc_128_c};
    std::mt19937 random(0x264116);
    std::vector<Fixture> cases(52*84);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& block = cases[i];
        const auto pattern = i/52;
        block.qp = i%52;
        block.mode = inter ? -1 : static_cast<int>(pattern%predictors.size());
        block.plane = inter ? 0 : (pattern/predictors.size())%3;
        block.field = (pattern/3)%2;
        block.decimate = (pattern/2)%2;
        block.denoise = (pattern/7)%2;
        block.cbp = pattern%7 == 0 ? 5 : 0;
        setFlatQuant(block, inter ? 11 : 21);
        for (auto& value : block.source) value = random() & 255;
        for (auto& value : block.pixels) value = random() & 255;
        for (auto& value : block.noiseOffset) value = random()%32;
        for (auto& value : block.residualSum) value = random();
        block.ac.fill(-167);
        block.dc.fill(163);
        block.nonzero.fill(109);
        if (pattern%6 < 3) {
            block.pixels.fill(128);
            block.source.fill(pattern%6 == 0 ? 128 : 140);
            if (pattern%6 == 2) block.source[(pattern%16)*16+(pattern%13)] = 144;
        }
        if (inter) {
            for (std::size_t index = 0; index < block.source.size(); ++index) {
                const int x = index%16, y = index/16;
                const std::size_t pos = origin+y*FDEC_STRIDE+x;
                if (pattern%7 < 4) {
                    const auto prediction = 96+random()%64;
                    block.pixels[pos] = prediction;
                    int difference = 0;
                    if (pattern%7 == 1 && x < 8 && y < 8) difference = 18;
                    if (pattern%7 == 2 && x < 4 && y < 4) difference = 2;
                    if (pattern%7 == 3 && x < 8) difference = static_cast<int>(random()%31)-15;
                    block.source[index] = prediction+difference;
                }
            }
        }
    }
    // Real-content reproducer: 16x16 gradient crop at (1792,896). This case
    // diverges in the full encode (offset 42) while simple gradients do not.
    if (!inter) {
        Fixture repro{};
        repro.qp = 25;
        repro.mode = 6;            // DC_128, the only mode for a first MB
        repro.plane = 0;
        repro.field = 0;
        repro.decimate = 1;
        repro.denoise = 0;
        repro.cbp = 0;
        setFlatQuant(repro, 21);
        static constexpr std::uint8_t reproY[256] = {50,50,50,50,50,51,51,52,52,53,54,55,55,55,56,56,54,54,54,54,54,54,54,55,55,55,56,56,56,56,56,56,56,56,56,56,56,55,55,55,56,56,56,56,56,56,56,56,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,56,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,56,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,55,54,54,54,54,54,54,54,55,55,55,55,55,55,55,55,54,54,54,54,54,54,54,54,54,54,55,55,55,55,55,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,54,53,54,53,53,53,54,54,54,54,54,54,54,54,54,53,53,53,53,52,53,52,54,54,54,54,54,54,53,53,52,52,51,51,51,51,51,50,54,54,54,54,53,53,53,52,51,51,50,50,49,49,49,49};
        for (int i = 0; i < 256; ++i) repro.source[i] = reproY[i];
        repro.pixels.fill(128);
        repro.noiseOffset.fill(0);
        repro.residualSum.fill(0);
        repro.ac.fill(-167);
        repro.dc.fill(163);
        repro.nonzero.fill(0);
        cases.push_back(repro);
    }
    const auto inputs = cases;
    vk264::macroblock::encode(shader, cases);
    Intra16Reference reference{};
    std::copy(predictors.begin(), predictors.end(), reference.predict_16x16);
    reference.dctf = {sub16x16_dct, add16x16_idct, add16x16_idct_dc, dct4x4dc, idct4x4dc};
    reference.dctf.add8x8_idct = add8x8_idct;
    reference.quantf = {denoise_dct, quant_4x4x4, quant_4x4_dc, dequant_4x4,
                        dequant_4x4_dc, decimate_score15};
    reference.quantf.decimate_score16 = decimate_score16;
    int dcOnly = 0, empty = 0, withAc = 0, decimated = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& in = inputs[i];
        const auto& out = cases[i];
        const int p = in.plane, cat = inter ? CQM_4PY : p ? CQM_4IC : CQM_4IY;
        pixel src[256], dst[17*FDEC_STRIDE], nnz[X264_SCAN8_SIZE];
        std::copy(in.source.begin(), in.source.end(), src);
        std::copy(in.pixels.begin(), in.pixels.end(), dst);
        std::copy(in.nonzero.begin(), in.nonzero.end(), reference.mb.cache.non_zero_count);
        for (int b = 0; b < 16; ++b)
            std::copy_n(in.ac.begin()+16*b, 16, reference.dct.luma4x4[p*16+b]);
        std::copy(in.dc.begin(), in.dc.end(), reference.dct.luma16x16_dc[p]);
        std::copy(in.dequant.begin(), in.dequant.end(), reference.dequant4_mf[cat][in.qp%6]);
        std::copy(in.multiplier.begin(), in.multiplier.end(), reference.quant4_mf[cat][in.qp]);
        std::copy(in.bias.begin(), in.bias.end(), reference.quant4_bias[cat][in.qp]);
        std::copy(in.noiseOffset.begin(), in.noiseOffset.end(), reference.nr_offset[0]);
        std::copy(in.residualSum.begin(), in.residualSum.end(), reference.nr_residual_sum[0]);
        reference.mb.pic.p_fenc[p] = src;
        reference.mb.pic.p_fdec[p] = dst+origin;
        reference.mb.i_intra16x16_pred_mode = in.mode;
        reference.mb.b_dct_decimate = in.decimate;
        reference.mb.b_noise_reduction = in.denoise;
        reference.mb.i_cbp_luma = in.cbp;
        reference.zigzagf.scan_4x4 = in.field ? zigzag_scan_4x4_field : zigzag_scan_4x4_frame;
        if (inter) reference_inter4x4(&reference, in.qp, 1, in.decimate);
        else mb_encode_i16x16(&reference, p, in.qp);
        dctcoef ac[256];
        std::memcpy(ac, reference.dct.luma4x4+16*p, sizeof(ac));
        std::copy_n(reference.mb.cache.non_zero_count, X264_SCAN8_SIZE, nnz);
        if (out.cbp != reference.mb.i_cbp_luma || !matchesNativeBytes(out.pixels, dst) ||
            !matchesNativeBytes(out.ac, ac) || !matchesNativeBytes(out.dc, reference.dct.luma16x16_dc[p]) ||
            !matchesNativeBytes(out.nonzero, nnz) ||
            !std::equal(out.residualSum.begin(), out.residualSum.end(), reference.nr_residual_sum[0]) ||
            !matchesNativeBytes(out.source, src) || out.noiseOffset != in.noiseOffset ||
            out.multiplier != in.multiplier || out.bias != in.bias || out.dequant != in.dequant)
            throw std::runtime_error("Luma macroblock differs from x264 at fixture " + std::to_string(i));
        if (out.cbp == in.cbp) {
            if (!inter && nnz[x264_scan8[48+p]]) ++dcOnly;
            else ++empty;
            if (out.ac != in.ac) ++decimated;
        } else {
            ++withAc;
        }
    }
    if ((!inter && !dcOnly) || !empty || !withAc || !decimated)
        throw std::runtime_error("Luma macroblock fixture coverage is incomplete");
    std::cout << cases.size() << (inter ? " inter 4x4-transform" : " I_16x16")
              << " macroblocks matched x264 coefficients, flags and reconstruction.\n";
}

namespace inter8Corpus {
struct Fixture {
    std::int32_t qp, field, decimate, denoise, cbp;
    std::uint32_t noiseCount;
    std::array<std::uint32_t, 64> multiplier, bias, noiseOffset, residualSum;
    std::array<std::int32_t, 64> dequant;
    std::array<std::uint32_t, 256> source;
    std::array<std::uint32_t, 17*FDEC_STRIDE> pixels;
    std::array<std::int32_t, 256> coefficients;
    std::array<std::uint32_t, X264_SCAN8_SIZE> nonzero;
};
static_assert(sizeof(Fixture) == (6+5*64+512+17*FDEC_STRIDE+X264_SCAN8_SIZE)*4);
void configure(Fixture& fixture) {
    const int q = fixture.qp%6, shift = fixture.qp/6;
    for (int i = 0; i < 64; ++i) {
        const int index = quant8_scan[((i>>1)&12) | (i&3)];
        const int scale = quant8_scale[q][index];
        const int multiplier = shift == 0 ? scale : (scale+(1 << (shift-1))) >> shift;
        fixture.multiplier[i] = multiplier;
        fixture.bias[i] = std::min((11*1024+multiplier/2)/multiplier, 32768/multiplier);
        fixture.dequant[i] = 16*dequant8_scale[q][index];
    }
}
}

void verifyInter8x8(const char* shader, bool frame = false) {
    using namespace inter8Corpus;
    std::vector<Fixture> cases(52*2*2*2*6);
    std::mt19937 random(0x264888);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& fixture = cases[i];
        fixture.qp = i%52;
        fixture.field = (i/52)&1;
        fixture.decimate = (i/(52*2))&1;
        fixture.denoise = (i/(52*4))&1;
        const auto pattern = i/(52*8);
        // Production inter8x8_frame hardcodes scalar quantization, progressive
        // scan and no denoising.
        if (frame) { fixture.field = 0; fixture.denoise = 0; }
        configure(fixture);
        fixture.cbp = pattern == 5 ? 4 : 0;
        fixture.noiseCount = 500;
        for (auto& sum : fixture.residualSum) sum = random();
        for (auto& offset : fixture.noiseOffset) offset = random()%11;
        for (auto& pixel : fixture.pixels) pixel = random() & 255;
        fixture.nonzero.fill(77);
        fixture.coefficients.fill(-321);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                auto& pred = fixture.pixels[intraCorpus::origin+y*FDEC_STRIDE+x];
                if (pattern < 4) pred = 128;
                int delta = 0;
                if (pattern == 1 && x < 8 && y < 8) delta = 2;
                if (pattern == 2 && x < 8) delta = 16;
                if (pattern == 3 && y < 8) delta = (x+y)%3-1;
                if (pattern >= 4) delta = static_cast<int>(random()%255)-127;
                fixture.source[y*16+x] = std::clamp(static_cast<int>(pred)+delta, 0, 255);
            }
    }
    const auto inputs = cases;
    // inter8x8_frame main() maps 4 invocations per fixture (one per 8x8 block).
    if (frame) execute(shader, cases, 4);
    else vk264::macroblock::inter(shader, cases);
    Intra16Reference ref{};
    ref.param.b_cabac = 1;
    ref.dctf.sub16x16_dct8 = sub16x16_dct8;
    ref.dctf.add8x8_idct8 = add8x8_idct8;
    ref.quantf.quant_8x8 = quant_8x8;
    ref.quantf.dequant_8x8 = dequant_8x8;
    ref.quantf.decimate_score64 = decimate_score64;
    ref.quantf.denoise_dct = denoise_dct;
    int empty = 0, coded = 0, decimated = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& in = inputs[i];
        const auto& out = cases[i];
        pixel source[256], pixels[17*FDEC_STRIDE];
        std::copy(in.source.begin(), in.source.end(), source);
        std::copy(in.pixels.begin(), in.pixels.end(), pixels);
        ref.mb.pic.p_fenc[0] = source;
        ref.mb.pic.p_fdec[0] = pixels+intraCorpus::origin;
        ref.mb.b_noise_reduction = in.denoise;
        ref.mb.i_cbp_luma = in.cbp;
        ref.nr_count[1] = in.noiseCount;
        ref.zigzagf.scan_8x8 = in.field ? zigzag_scan_8x8_field : zigzag_scan_8x8_frame;
        std::copy(in.nonzero.begin(), in.nonzero.end(), ref.mb.cache.non_zero_count);
        std::copy(in.multiplier.begin(), in.multiplier.end(), ref.quant8_mf[CQM_8PY][in.qp]);
        std::copy(in.bias.begin(), in.bias.end(), ref.quant8_bias[CQM_8PY][in.qp]);
        std::copy(in.dequant.begin(), in.dequant.end(), ref.dequant8_mf[CQM_8PY][in.qp%6]);
        std::copy(in.noiseOffset.begin(), in.noiseOffset.end(), ref.nr_offset[1]);
        std::copy(in.residualSum.begin(), in.residualSum.end(), ref.nr_residual_sum[1]);
        for (int block = 0; block < 4; ++block)
            std::copy_n(in.coefficients.begin()+64*block, 64, ref.dct.luma8x8[block]);
        reference_inter8x8(&ref, in.qp, 1, in.decimate);
        dctcoef levels[256];
        std::memcpy(levels, ref.dct.luma8x8, sizeof(levels));
        if (!matchesNativeBytes(out.pixels, pixels) || !matchesNativeBytes(out.coefficients, levels) ||
            !matchesNativeBytes(out.nonzero, ref.mb.cache.non_zero_count) ||
            !matchesNativeBytes(out.residualSum, ref.nr_residual_sum[1]) || out.noiseCount != ref.nr_count[1] ||
            out.cbp != ref.mb.i_cbp_luma || out.source != in.source || out.multiplier != in.multiplier ||
            out.bias != in.bias || out.dequant != in.dequant || out.noiseOffset != in.noiseOffset)
            throw std::runtime_error("Inter 8x8 differs from x264 at fixture " + std::to_string(i));
        if (in.cbp != out.cbp) ++coded;
        else if (out.coefficients == in.coefficients) ++empty;
        else if (in.decimate && in.cbp == 0) ++decimated;
    }
    if (!empty || !coded || !decimated) throw std::runtime_error("Inter 8x8 fixture coverage is incomplete");
    std::cout << cases.size() << " scalar inter 8x8 luma cases matched x264 byte for byte.\n";
}

namespace chromaCorpus {
struct Fixture {
    std::int32_t qp, mode, chroma422, field, denoise, lossless, decimate, lambda2, cbp, dcNonzero, acMask;
    std::uint32_t dcMultiplier, dcBias;
    std::int32_t dcDequant;
    std::array<std::uint32_t, 16> multiplier, bias, noiseOffset, residualSum;
    std::array<std::int32_t, 16> dequant;
    std::array<std::uint32_t, 128> source;
    std::array<std::uint32_t, 17*FDEC_STRIDE> pixels;
    std::array<std::int32_t, 128> ac;
    std::array<std::int32_t, 8> dc;
};
static_assert(sizeof(Fixture) == 902*4);
void configure(Fixture& block) {
    const int bias = block.mode >= 0 ? 21 : 11;
    intraCorpus::setFlatQuant(block, bias);
    block.lambda2 = x264_lambda2_tab[block.qp];
    struct Quant {
        int qp;
        std::array<std::uint32_t, 16> multiplier, bias;
        std::array<std::int32_t, 16> dequant;
    } dc{block.qp+3*block.chroma422};
    intraCorpus::setFlatQuant(dc, bias);
    block.dcMultiplier = dc.multiplier[0];
    block.dcBias = dc.bias[0];
    block.dcDequant = dc.dequant[0];
}
}

void verifyIntraChroma(const char* shader, bool frame = false) {
    using namespace chromaCorpus;
    constexpr auto p420 = std::array{x264_predict_8x8c_dc_c, x264_predict_8x8c_h_c, x264_predict_8x8c_v_c,
        x264_predict_8x8c_p_c, predict_8x8c_dc_left_c, predict_8x8c_dc_top_c, predict_8x8c_dc_128_c};
    constexpr auto p422 = std::array{x264_predict_8x16c_dc_c, x264_predict_8x16c_h_c, x264_predict_8x16c_v_c,
        x264_predict_8x16c_p_c, predict_8x16c_dc_left_c, predict_8x16c_dc_top_c, predict_8x16c_dc_128_c};
    std::mt19937 random(0x264C42);
    constexpr int intraCases = 52*2*7*4*2*2, interCases = 52*2*2*10*2;
    std::vector<Fixture> cases(intraCases+interCases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& block = cases[i];
        const auto k = i/2;
        block.qp = k%52;
        block.mode = k/52%7;
        block.chroma422 = k/(52*7)%2;
        const auto pattern = k/(52*7*2)%4;
        block.lossless = k/(52*7*2*4);
        block.field = pattern&1;
        block.denoise = (pattern>>1)&1;
        for (auto& v : block.noiseOffset) v = k%31;
        for (auto& v : block.source) v = random() & 255;
        for (auto& v : block.pixels) v = random() & 255;
        if (pattern < 2 || k%5 == 0) {
            block.pixels.fill(128);
            block.source.fill(pattern == 0 ? 128 : static_cast<std::uint32_t>(110+i%39));
            if (i%3 == 1) block.source[i%64] = 141;
        }
        if (i >= intraCases) {
            const auto index = (i-intraCases)/2;
            const auto kind = index/(52*2*2);
            block.mode = -1;
            block.field = index/52%2;
            block.chroma422 = index/(52*2)%2;
            block.qp = index%52;
            block.denoise = kind == 5 || kind == 9;
            block.decimate = kind != 6 && kind != 7;
            block.lossless = kind == 6;
            if (block.denoise) block.noiseOffset.fill(kind == 5 ? 0 : 3);
            for (std::size_t p = 0; p < block.source.size(); ++p) {
                const auto position = intraCorpus::origin+(p/8)*FDEC_STRIDE+p%8;
                const int prior = 96+static_cast<int>(random()%64);
                int difference = 0;
                switch (kind) {
                    case 1: difference = i%2 ? 0 : 34; break;
                    case 2: difference = static_cast<int>((p/4+3*(p/32))%9)-4+static_cast<int>(i%2)*17; break;
                    case 3: difference = p%2 ? -9 : 9; break;
                    case 4: difference = static_cast<int>(random()%193)-96; break;
                    case 5: difference = p < 4 && i%2 == 0 ? 5 : 0; break;
                    case 6:
                        if (block.qp%3 == 1) difference = p%4 == 0 && p/8%4 == 0 ? 12 : 0;
                        if (block.qp%3 == 2) difference = static_cast<int>(p%21)-10;
                        break;
                    case 7: difference = p%5 ? 0 : -13; break;
                    case 8: difference = i%2 == 0 ? static_cast<int>(random()%127)-63 : 2; break;
                    case 9: difference = static_cast<int>(random()%9)-4; break;
                }
                block.pixels[position] = prior;
                block.source[p] = prior+difference;
            }
        }
        // The production intra_chroma_frame main() hardcodes progressive scan
        // and no denoising; frame mode restricts fixtures to that subset.
        if (frame) { block.field = 0; block.denoise = 0; }
        configure(block);
        block.ac.fill(-149);
        block.dc.fill(198);
    }
    const auto inputs = cases;
    execute(shader, cases, 1, {}, cases.size()/2);
    Intra16Reference ref{};
    ref.dctf.sub8x8_dct = sub8x8_dct;
    ref.dctf.sub8x8_dct_dc = sub8x8_dct_dc;
    ref.dctf.sub8x16_dct_dc = sub8x16_dct_dc;
    ref.dctf.dct2x4dc = dct2x4dc;
    ref.dctf.add8x8_idct = add8x8_idct;
    ref.dctf.add8x8_idct_dc = add8x8_idct_dc;
    ref.mc.copy[PIXEL_8x8] = mc_copy_w8;
    ref.quantf.denoise_dct = denoise_dct;
    ref.quantf.quant_4x4x4 = quant_4x4x4;
    ref.quantf.dequant_4x4 = dequant_4x4;
    ref.quantf.decimate_score15 = decimate_score15;
    ref.quantf.optimize_chroma_2x2_dc = optimize_chroma_2x2_dc;
    ref.quantf.optimize_chroma_2x4_dc = optimize_chroma_2x4_dc;
    ref.quantf.quant_2x2_dc = quant_2x2_dc;
    ref.quantf.idct_dequant_2x4_dc = idct_dequant_2x4_dc;
    ref.quantf.idct_dequant_2x4_dconly = idct_dequant_2x4_dconly;
    ref.pixf.var2[PIXEL_8x8] = pixel_var2_8x8;
    ref.pixf.var2[PIXEL_8x16] = pixel_var2_8x16;
    int coverage[2][2][2][3]{}, earlyCoverage[2][2]{}, decimated[2]{};
    for (std::size_t i = 0; i < cases.size(); i += 2) {
        const auto& in = inputs[i];
        const bool inter = in.mode < 0;
        const int cat = CQM_4IC+inter, dcQp = in.qp+3*in.chroma422;
        const auto& pred = in.chroma422 ? p422 : p420;
        ref.chromaFormat = in.chroma422 ? CHROMA_422 : CHROMA_420;
        std::copy(pred.begin(), pred.end(), ref.predict_chroma);
        ref.mb.b_noise_reduction = in.denoise;
        ref.mb.b_lossless = in.lossless;
        ref.mb.b_dct_decimate = in.decimate;
        ref.zigzagf.scan_4x4 = in.field ? zigzag_scan_4x4_field : zigzag_scan_4x4_frame;
        ref.zigzagf.sub_4x4ac = in.field ? zigzag_sub_4x4ac_field : zigzag_sub_4x4ac_frame;
        std::copy(in.multiplier.begin(), in.multiplier.end(), ref.quant4_mf[cat][in.qp]);
        std::copy(in.bias.begin(), in.bias.end(), ref.quant4_bias[cat][in.qp]);
        ref.quant4_mf[cat][dcQp][0] = in.dcMultiplier;
        ref.quant4_bias[cat][dcQp][0] = in.dcBias;
        std::copy(in.dequant.begin(), in.dequant.end(), ref.dequant4_mf[cat][in.qp%6]);
        ref.dequant4_mf[cat][dcQp%6][0] = in.dcDequant;
        std::fill_n(ref.nr_residual_sum[2], 16, 0);
        std::copy(in.noiseOffset.begin(), in.noiseOffset.end(), ref.nr_offset[2]);
        pixel src[2][18*FENC_STRIDE]{}, dst[2][17*FDEC_STRIDE];
        for (int ch = 0; ch < 2; ++ch) {
            const auto& source = inputs[i+ch];
            for (int y = 0; y < 16; ++y) std::copy_n(source.source.begin()+y*8, 8, src[ch]+(y+1)*FENC_STRIDE+1);
            std::copy(source.pixels.begin(), source.pixels.end(), dst[ch]);
            ref.mb.pic.p_fenc[1+ch] = src[ch]+FENC_STRIDE+1;
            ref.mb.pic.p_fdec[1+ch] = dst[ch]+intraCorpus::origin;
            if (!inter && !in.lossless) pred[source.mode](ref.mb.pic.p_fdec[1+ch]);
            for (int b = 0; b < 8; ++b)
                std::copy_n(source.ac.begin()+b*16, 16, ref.dct.luma4x4[16+16*ch+b+(b&4)]);
            std::copy(source.dc.begin(), source.dc.end(), ref.dct.chroma_dc[ch]);
        }
        if (!inter && in.lossless) x264_predict_lossless_chroma(&ref, in.mode);
        const int height = in.chroma422 ? 16 : 8;
        const bool gateEligible = inter && in.decimate && in.qp >= 18 && !in.denoise;
        if (gateEligible) {
            for (int y = 0; y < height; ++y) {
                std::copy_n(ref.mb.pic.p_fenc[2]+y*FENC_STRIDE, 8,
                            ref.mb.pic.p_fenc[1]+y*FENC_STRIDE+FENC_STRIDE/2);
                std::copy_n(ref.mb.pic.p_fdec[2]+y*FDEC_STRIDE, 8,
                            ref.mb.pic.p_fdec[1]+y*FDEC_STRIDE+FDEC_STRIDE/2);
            }
            int squared[2];
            const int variance = ref.pixf.var2[in.chroma422 ? PIXEL_8x16 : PIXEL_8x8](
                ref.mb.pic.p_fenc[1], ref.mb.pic.p_fdec[1], squared);
            const int threshold = in.chroma422 ? (in.lambda2+16)>>5 : (in.lambda2+32)>>6;
            if (variance < threshold*4)
                for (const int ssd : squared) ++earlyCoverage[in.chroma422][ssd > threshold];
        }
        mb_encode_chroma_internal(&ref, inter, in.qp, in.chroma422);
        if (gateEligible) {
            for (int y = 0; y < height; ++y) {
                const int offset = intraCorpus::origin+y*FDEC_STRIDE+FDEC_STRIDE/2;
                std::copy_n(in.pixels.begin()+offset, 8, dst[0]+offset);
            }
        }
        for (int ch = 0; ch < 2; ++ch) {
            const auto& actual = cases[i+ch];
            const int dc = ref.mb.cache.non_zero_count[x264_scan8[49+ch]];
            int acMask = 0;
            dctcoef ac[128];
            for (int b = 0; b < 8; ++b) {
                const int index = 16+16*ch+b+(b&4);
                if (b < (in.chroma422 ? 8 : 4)) acMask |= ref.mb.cache.non_zero_count[x264_scan8[index]] << b;
                std::copy_n(ref.dct.luma4x4[index], 16, ac+16*b);
            }
            const int cbp = acMask ? 2 : dc;
            if (actual.acMask != acMask || actual.dcNonzero != dc || actual.cbp != cbp ||
                !matchesNativeBytes(actual.pixels, dst[ch]) || !matchesNativeBytes(actual.ac, ac) ||
                !matchesNativeBytes(actual.dc, ref.dct.chroma_dc[ch]))
                throw std::runtime_error("Intra chroma differs from x264 in fixture " + std::to_string(i+ch));
            ++coverage[inter][in.lossless][in.chroma422][cbp];
            if (inter && in.decimate && acMask == 0 && actual.ac != inputs[i+ch].ac)
                ++decimated[in.chroma422];
        }
        if (std::max(cases[i].cbp, cases[i+1].cbp) != ref.mb.i_cbp_chroma)
            throw std::runtime_error("Combined chroma CBP differs from x264");
        for (int j = 0; j < 16; ++j)
            if (cases[i].residualSum[j]+cases[i+1].residualSum[j] != ref.nr_residual_sum[2][j])
                throw std::runtime_error("Chroma denoising sums differ from x264");
    }
    for (const auto& prediction : coverage)
        for (const auto& path : prediction)
            for (const auto& format : path)
                if (!format[0] || !format[1] || !format[2])
                    throw std::runtime_error("Chroma fixtures missed a coded-block pattern");
    for (int format = 0; format < 2; ++format)
        if (!earlyCoverage[format][0] || !earlyCoverage[format][1] || !decimated[format])
            throw std::runtime_error("Chroma fixtures missed variance-gated or decimated residuals");
    std::cout << intraCases << " intra and " << interCases
              << " inter chroma planes matched x264 across 4:2:0 and 4:2:2.\n";
}

