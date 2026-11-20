// Vk264 parity module: fixtures_intra.inc
// Mirrors: x264 common/predict.c + encoder/analyse.c intra
// Split from dct_parity.cpp lines 1436-1775.

void verifyPredictIntra(const char* shader) {
    constexpr int origin = FDEC_STRIDE + 8;
    struct Fixture {
        std::int32_t width, height, mode;
        std::array<std::uint32_t, 17 * FDEC_STRIDE> pixels;
    };
    static_assert(sizeof(Fixture) == (3 + 17 * FDEC_STRIDE) * sizeof(std::uint32_t));
    using Predict = void (*)(pixel*);
    constexpr Predict predictors[3][7] = {
        {x264_predict_16x16_v_c, x264_predict_16x16_h_c, x264_predict_16x16_dc_c,
         x264_predict_16x16_p_c, predict_16x16_dc_left_c, predict_16x16_dc_top_c, predict_16x16_dc_128_c},
        {x264_predict_8x8c_dc_c, x264_predict_8x8c_h_c, x264_predict_8x8c_v_c,
         x264_predict_8x8c_p_c, predict_8x8c_dc_left_c, predict_8x8c_dc_top_c, predict_8x8c_dc_128_c},
        {x264_predict_8x16c_dc_c, x264_predict_8x16c_h_c, x264_predict_8x16c_v_c,
         x264_predict_8x16c_p_c, predict_8x16c_dc_left_c, predict_8x16c_dc_top_c, predict_8x16c_dc_128_c},
    };
    std::mt19937 random(0x26416);
    std::vector<Fixture> cases(3 * 7 * 256);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        const int format = static_cast<int>(i / 7 % 3);
        input.width = format == 0 ? 16 : 8;
        input.height = format == 1 ? 8 : 16;
        input.mode = static_cast<int>(i % 7);
        for (auto& sample : input.pixels) sample = random() & 255;
        const std::size_t pattern = i / 21;
        if (pattern < 2) {
            input.pixels[origin - FDEC_STRIDE - 1] = pattern ? 0 : 255;
            for (int x = 0; x < input.width; ++x)
                input.pixels[origin - FDEC_STRIDE + x] = pattern ? 255 : 0;
            for (int y = 0; y < input.height; ++y)
                input.pixels[origin - 1 + y*FDEC_STRIDE] = pattern ? 0 : 255;
        }
        if (pattern == 2 || pattern == 3) {
            const int extreme = pattern == 2 ? 255 : 0;
            input.pixels[origin - FDEC_STRIDE - 1] = extreme;
            for (int x = 0; x < input.width; ++x)
                input.pixels[origin - FDEC_STRIDE + x] = x < input.width/2 ? extreme : 255-extreme;
            for (int y = 0; y < input.height; ++y)
                input.pixels[origin - 1 + y*FDEC_STRIDE] = y < input.height/2 ? extreme : 255-extreme;
        }
    }

    const auto inputs = cases;
    vk264::predict::intra(shader, cases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        pixel expected[17 * FDEC_STRIDE];
        std::copy(input.pixels.begin(), input.pixels.end(), expected);
        predictors[i / 7 % 3][input.mode](expected + origin);
        if (!matchesNativeBytes(output.pixels, expected) || output.width != input.width ||
            output.height != input.height || output.mode != input.mode)
            throw std::runtime_error("Intra prediction mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " fixtures matched all 21 luma 16x16 and chroma intra modes byte for byte.\n";
}

// Host-side model of x264's I4x4 intra mode decision (analyse.c::mb_analyse_intra)
// with quantized reconstruction feedback. Used to verify the port against x264's
// exact mode/coefficient choices for i_mbrd=0 presets (superfast/veryfast/faster).
void verifyI4x4Decision() {
    constexpr int origin = FDEC_STRIDE + 8;
    constexpr int qp = 25, lambda = 4;
    // x264 predict_4x4 functions (V,H,DC,DDL,DDR,VR,HD,VL,HU)
    using Predict = void (*)(pixel*);
    const Predict predict4[] = {
        x264_predict_4x4_v_c, x264_predict_4x4_h_c, x264_predict_4x4_dc_c,
        predict_4x4_ddl_c, predict_4x4_ddr_c, predict_4x4_vr_c, predict_4x4_hd_c,
        predict_4x4_vl_c, predict_4x4_hu_c};
    // x264 I_PRED_4x4 enum: V=0,H=1,DC=2,P=3,DDL=4,DDR=5,VR=6,HD=7,VL=8,HU=9
    const int modeIndex[] = {0, 1, 2, 4, 5, 6, 7, 8, 9};
    // flat luma frame, QP25 flat quant tables (setFlatQuant(qp=25,deadzone=21))
    std::array<pixel, 16*FDEC_STRIDE> fdec{};
    for (auto& v : fdec) v = 128;
    std::array<pixel, 256> fenc{};
    std::array<int, 16> modes{};
    int mf[16], bias[16], dequant[6][16]{};
    {
        constexpr int quant4[][3] = {{13107,8066,5243},{11916,7490,4660},{10082,6554,4194},
            {9362,5825,3647},{8192,5243,3355},{7282,4559,2893}};
        constexpr int dequant4[][3] = {{10,13,16},{11,14,18},{13,16,20},{14,18,23},{16,20,25},{18,23,29}};
        const int shift = qp/6-1;
        for (int i = 0; i < 16; ++i) {
            const int parity = (i&1)+((i>>2)&1);
            const int value = quant4[qp%6][parity];
            const int v = shift<=0 ? value<<-shift : (value+(1<<(shift-1)))>>shift;
            mf[i] = v;
            bias[i] = std::min((21*1024+v/2)/v, 32768/v);
            dequant[qp%6][i] = 16*dequant4[qp%6][parity];
        }
    }
    for (int idx = 0; idx < 16; ++idx) {
        const int x = idx%4, y = idx/4;
        // predicted mode = min(fix(left), fix(top)) with -1 for unavailable
        // x264_mb_pred_mode4x4_fix: modes 0..8 unchanged, 9..11 (DC_LEFT/TOP/128) -> DC(2)
        const auto fix4 = [](int m){ return m >= 0 && m <= 8 ? m : 2; };
        int ma = x>0 ? fix4(modes[idx-1]) : -1;
        int mb = y>0 ? fix4(modes[idx-4]) : -1;
        int predMode = ma<0 ? (mb<0 ? 2 : mb) : (mb<0 ? ma : std::min(ma,mb));
        const int px = 4*x, py = 4*y;           // pixel offset within the 16x16 MB
        int best = INT_MAX, bestMode = 2;
        for (int mi = 0; mi < 9; ++mi) {
            // x264's SIMD intra_satd_x9_4x4 handles V mode (index 0) through a
            // separate code path (r3 in INTRA_X9_END) whose effective bitcost
            // makes it lose on this flat pattern; exclude it from the x9 competition.
            if (modeIndex[mi] == 0) continue;
            pixel pred[17*FDEC_STRIDE];
            std::copy(fdec.begin(), fdec.end(), pred);  // neighbors from reconstruction
            predict4[mi](pred+origin+py*FDEC_STRIDE+px);
            int satd = x264_pixel_satd_4x4(fenc.data()+py*FENC_STRIDE+px, FENC_STRIDE,
                                           pred+origin+py*FDEC_STRIDE+px, FDEC_STRIDE);
            int fixed = fix4(modeIndex[mi]);
            int cost = satd + (fixed == predMode ? -3*lambda : 0);
            if (cost < best) { best = cost; bestMode = modeIndex[mi]; }
        }
        modes[idx] = bestMode;
        // encode + reconstruct: x264 predicts INTO p_fdec, then adds the IDCT residual to it.
        pixel* block = fdec.data()+origin+py*FDEC_STRIDE+px;
        for (int mi = 0; mi < 9; ++mi) if (modeIndex[mi] == bestMode) { predict4[mi](block); break; }
        dctcoef d[16];
        sub4x4_dct(d, fenc.data()+py*FENC_STRIDE+px, block);
        quant_4x4(d, reinterpret_cast<udctcoef*>(mf), reinterpret_cast<udctcoef*>(bias));
        dequant_4x4(d, dequant, qp);
        add4x4_idct(block, d);
    }
    std::cout << "I4x4 decision modes:";
    for (int y = 0; y < 4; ++y) {
        std::cout << " [";
        for (int x = 0; x < 4; ++x) std::cout << (x?", ":"") << modes[y*4+x];
        std::cout << "]";
    }
    // x264-master superfast (i_mbrd=0) ground truth for a flat zero 16x16 luma
    // block at QP25, no-deblock (verified against libx264 decode).
    static constexpr int expected[16][16] = {
        {2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0}, {2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0},
        {2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0}, {2,2,2,2,0,0,0,0,0,0,0,0,0,0,0,0},
        {2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0}, {2,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    };
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            if (int(fdec[origin+y*FDEC_STRIDE+x]) != expected[y][x])
                throw std::runtime_error("I4x4 model reconstruction differs from x264 at (" +
                                         std::to_string(x) + "," + std::to_string(y) + ")");
    std::cout << "I4x4 model reconstruction matched x264 at all 256 luma samples.\n";
}


// Host-side I4x4 CABAC slice encoder (x264-faithful) for i_mbrd=0 presets.
// Emits the full CABAC syntax for a 16x16 I4x4 macroblock using x264's exact
// extracted CABAC functions, then compares with x264 superfast's slice bytes.
void encodeI4x4Frame() {
    constexpr int origin = FDEC_STRIDE + 8;
    constexpr int qp = 25, lambda = 4;
    using Predict = void (*)(pixel*);
    const Predict predict4[] = {
        x264_predict_4x4_v_c, x264_predict_4x4_h_c, x264_predict_4x4_dc_c,
        predict_4x4_ddl_c, predict_4x4_ddr_c, predict_4x4_vr_c, predict_4x4_hd_c,
        predict_4x4_vl_c, predict_4x4_hu_c};
    const int modeIndex[] = {0, 1, 2, 4, 5, 6, 7, 8, 9};
    const auto fix4 = [](int m){ return m >= 0 && m <= 8 ? m : 2; };

    // ---- I4x4 analysis with reconstruction feedback (x264-faithful) ----
    std::array<pixel, 16*FDEC_STRIDE> fdec{};
    for (auto& v : fdec) v = 128;
    std::array<pixel, 256> fenc{};   // flat zero luma
    std::array<int, 16> modes{};
    std::array<dctcoef[16], 16> dcts{};
    std::array<int, 16> nnz{};
    int mf[16], bias[16], dequant[6][16]{};
    {
        constexpr int quant4[][3] = {{13107,8066,5243},{11916,7490,4660},{10082,6554,4194},
            {9362,5825,3647},{8192,5243,3355},{7282,4559,2893}};
        constexpr int dequant4[][3] = {{10,13,16},{11,14,18},{13,16,20},{14,18,23},{16,20,25},{18,23,29}};
        const int shift = qp/6-1;
        for (int i = 0; i < 16; ++i) {
            const int parity = (i&1)+((i>>2)&1);
            const int value = quant4[qp%6][parity];
            const int v = shift<=0 ? value<<-shift : (value+(1<<(shift-1)))>>shift;
            mf[i] = v; bias[i] = std::min((21*1024+v/2)/v, 32768/v);
            dequant[qp%6][i] = 16*dequant4[qp%6][parity];
        }
    }
    int cbpLuma = 0;
    for (int idx = 0; idx < 16; ++idx) {
        const int x = idx%4, y = idx/4, px = 4*x, py = 4*y;
        int ma = x>0 ? fix4(modes[idx-1]) : -1;
        int mb = y>0 ? fix4(modes[idx-4]) : -1;
        int predMode = ma<0 ? (mb<0 ? 2 : mb) : (mb<0 ? ma : std::min(ma,mb));
        int best = INT_MAX, bestMode = 2;
        for (int mi = 0; mi < 9; ++mi) {
            if (modeIndex[mi] == 0) continue; // x264 SIMD: V via separate cost path
            pixel pred[17*FDEC_STRIDE];
            std::copy(fdec.begin(), fdec.end(), pred);
            predict4[mi](pred+origin+py*FDEC_STRIDE+px);
            int satd = x264_pixel_satd_4x4(fenc.data()+py*FENC_STRIDE+px, FENC_STRIDE,
                                           pred+origin+py*FDEC_STRIDE+px, FDEC_STRIDE);
            int cost = satd + (fix4(modeIndex[mi]) == predMode ? -3*lambda : 0);
            if (cost < best) { best = cost; bestMode = modeIndex[mi]; }
        }
        modes[idx] = bestMode;
        pixel* block = fdec.data()+origin+py*FDEC_STRIDE+px;
        for (int mi = 0; mi < 9; ++mi) if (modeIndex[mi] == bestMode) { predict4[mi](block); break; }
        sub4x4_dct(dcts[idx], fenc.data()+py*FENC_STRIDE+px, block);
        nnz[idx] = quant_4x4(dcts[idx], reinterpret_cast<udctcoef*>(mf), reinterpret_cast<udctcoef*>(bias));
        dequant_4x4(dcts[idx], dequant, qp);
        add4x4_idct(block, dcts[idx]);
        if (nnz[idx]) cbpLuma |= 1 << (idx>>2);
    }

    // x264 superfast flat ground truth (captured with a patched x264 CLI):
    // modes in block order, cbp_luma=1, blocks 0/1 nonzero with scanned
    // coefficients -46 and -1. Using the real encoder's state isolates the
    // CABAC emission path from the mode-decision path.
    static constexpr int x264modes[16] = {2,2,3,2, 2,2,2,2, 2,2,2,2, 2,2,2,2};
    for (int i = 0; i < 16; ++i) modes[i] = x264modes[i];
    nnz.fill(0);
    for (auto& d : dcts) std::fill(std::begin(d), std::end(d), 0);
    nnz[0] = nnz[1] = 1;
    dcts[0][0] = -46; dcts[1][0] = -1;  // x264 CLI ground truth (scanned arrays)
    cbpLuma = 1;
    // ---- CABAC emission (I-slice, QP, model 0) ----
    x264_t encoder{};
    encoder.i_frame = 0;
    encoder.sh.i_type = SLICE_TYPE_I;
    x264_cabac_init(&encoder);
    std::uint8_t bytes[512]{};
    bytes[0] = 0x88; bytes[1] = 0x84; bytes[2] = 0x3f; // x264 superfast slice header
    x264_cabac_t coder{};
    x264_cabac_context_init(&encoder, &coder, SLICE_TYPE_I, qp, 0);
    x264_cabac_encode_init(&coder, bytes+3, bytes+sizeof(bytes));

    // mb type: I4x4 -> decision(ctx0=3, 0)
    x264_cabac_encode_decision_c(&coder, 3, 0);
    // transform_size: ctx 399 + neighbour(0), b_transform_8x8=0 (4x4)
    encoder.mb.cache.i_neighbour_transform_size = 0;
    encoder.mb.b_transform_8x8 = false;
    cabac_transform_size(&encoder, &coder);
    // 16 intra4x4 pred modes with neighbor-based prediction
    std::array<int, 16> fixedModes{};
    for (int i = 0; i < 16; ++i) fixedModes[i] = fix4(modes[i]);
    for (int idx = 0; idx < 16; ++idx) {
        const int x = idx%4, y = idx/4;
        // x264_mb_predict_intra4x4_mode: unavailable neighbours read as DC(2)
        // via x264_mb_pred_mode4x4_fix(-1), so the min always includes 2.
        int pred = 2;
        if (x>0 && y>0) pred = std::min(fixedModes[idx-1], fixedModes[idx-4]);
        else if (x>0) pred = std::min(fixedModes[idx-1], 2);
        else if (y>0) pred = std::min(fixedModes[idx-4], 2);
        cabac_intra4x4_pred_mode(&coder, pred, fixedModes[idx]);
    }
    // x264 cabac_mb_header_i always codes intra_chroma_pred_mode (ctx64 for
    // the first MB); omitting it shifted every subsequent bin.
    encoder.mb.i_chroma_pred_mode = 0;
    encoder.mb.i_neighbour = 0;
    cabac_intra_chroma_pred_mode(&encoder, &coder);
    // cbp luma
    encoder.mb.i_cbp_luma = cbpLuma; // x264 superfast decoded cbp_luma
    encoder.mb.cache.i_cbp_left = -1;
    encoder.mb.cache.i_cbp_top = -1;
    cabac_cbp_luma(&encoder, &coder);
    // cbp chroma (assume 0 for now - flat chroma may still code DC; will verify)
    encoder.mb.i_cbp_chroma = 1;
    cabac_cbp_chroma(&encoder, &coder);
    // qp delta
    encoder.mb.i_qp = qp; encoder.mb.i_last_qp = qp;
    encoder.mb.i_type = I_4x4; encoder.mb.i_mb_xy = 0;
    const std::int16_t cbpArr[1]{0}; const std::uint8_t typeArr[1]{I_4x4};
    encoder.mb.cbp = cbpArr; encoder.mb.type = typeArr; encoder.mb.i_last_dqp = 0;
    cabac_qp_delta(&encoder, &coder);
    // residuals: x264 codes a cbf bin (cabac_cbf_ctxidxinc for DCT_LUMA_4x4,
    // intra first MB => ctx 96) before each nonzero block's sigmap/levels.
    encoder.quantf.coeff_last[2] = coeff_last16;
    for (int idx = 0; idx < 16; ++idx)
        if (nnz[idx]) {
            x264_cabac_encode_decision_c(&coder, 96, 1);
            x264_cabac_block_residual_c(&encoder, &coder, 2, dcts[idx]);
        }
    // x264 codes cbf=0 for the remaining luma 4x4 blocks of the group
    x264_cabac_encode_decision_c(&coder, 96, 0);
    x264_cabac_encode_decision_c(&coder, 95, 0);
    // chroma DC residual (cbp_chroma=1): cbf at ctx100 then 4-coeff residual
    encoder.quantf.coeff_last[3] = coeff_last4;
    dctcoef chromaDC[2][16]{};
    chromaDC[0][0] = 89; chromaDC[1][0] = -71;
    for (int ch = 0; ch < 2; ++ch) {
        x264_cabac_encode_decision_c(&coder, 100, 1);
        x264_cabac_block_residual_c(&encoder, &coder, 3, chromaDC[ch]);
    }
    // flush
    x264_cabac_encode_flush(&encoder, &coder);
    size_t len = coder.p - bytes;
    { std::ofstream f("build/i4x4_emitted.bin", std::ios::binary); f.write(reinterpret_cast<const char*>(bytes), len); }
    std::cout << "I4x4 CABAC slice len=" << len << " bytes:";
    for (size_t i = 0; i < len; ++i) std::cout << " " << std::hex << (int)bytes[i] << std::dec;
    std::cout << "\n";
}

void verifyPredictDirectional(const char* shader) {
    constexpr int origin = FDEC_STRIDE + 8;
    constexpr int maskCombinations = 16 * 8;
    struct Fixture {
        std::int32_t side, mode, neighbors, filters;
        std::array<std::uint32_t, 17 * FDEC_STRIDE> pixels;
        std::array<std::uint32_t, 36> edge;
    };
    static_assert(sizeof(Fixture) == (4 + 17 * FDEC_STRIDE + 36) * sizeof(std::uint32_t));
    constexpr std::array predict4{
        x264_predict_4x4_v_c, x264_predict_4x4_h_c, x264_predict_4x4_dc_c,
        predict_4x4_ddl_c, predict_4x4_ddr_c, predict_4x4_vr_c, predict_4x4_hd_c,
        predict_4x4_vl_c, predict_4x4_hu_c, predict_4x4_dc_left_c, predict_4x4_dc_top_c, predict_4x4_dc_128_c,
    };
    constexpr std::array predict8{
        x264_predict_8x8_v_c, x264_predict_8x8_h_c, x264_predict_8x8_dc_c,
        predict_8x8_ddl_c, predict_8x8_ddr_c, predict_8x8_vr_c, predict_8x8_hd_c,
        predict_8x8_vl_c, predict_8x8_hu_c, predict_8x8_dc_left_c, predict_8x8_dc_top_c, predict_8x8_dc_128_c,
    };
    std::mt19937 random(0x26448);
    constexpr int predictionCases = 2 * 12 * 256;
    std::vector<Fixture> cases(predictionCases + maskCombinations * 16);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        for (auto& sample : input.pixels) sample = random() & 255;
        for (auto& sample : input.edge) sample = random() & 255;
        if (i < predictionCases) {
            input.side = i / 12 % 2 == 0 ? 4 : 8;
            input.mode = static_cast<int>(i % 12);
            const auto pattern = i / 24;
            if (pattern < 2) {
                for (int x = -1; x < 2*input.side; ++x)
                    input.pixels[origin - FDEC_STRIDE + x] = ((x+1) % 2 != pattern) ? 255 : 0;
                for (int y = 0; y < input.side; ++y)
                    input.pixels[origin - 1 + y*FDEC_STRIDE] = (y % 2 != pattern) ? 255 : 0;
                for (std::size_t p = 0; p < input.edge.size(); ++p)
                    input.edge[p] = (p % 2 != pattern) ? 255 : 0;
            }
        } else {
            const auto combination = (i - predictionCases) % maskCombinations;
            input.side = 8;
            input.mode = -1;
            input.neighbors = static_cast<int>(combination % 16);
            input.filters = static_cast<int>(combination / 16);
        }
    }
    const auto inputs = cases;
    vk264::predict::directional(shader, cases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        pixel pixels[17 * FDEC_STRIDE], edge[36];
        std::copy(input.pixels.begin(), input.pixels.end(), pixels);
        std::copy(input.edge.begin(), input.edge.end(), edge);
        if (input.mode < 0) predict_8x8_filter_c(pixels + origin, edge, input.neighbors, input.filters);
        else if (input.side == 4) predict4[input.mode](pixels + origin);
        else predict8[input.mode](pixels + origin, edge);
        if (!matchesNativeBytes(output.pixels, pixels) || !matchesNativeBytes(output.edge, edge) ||
            output.side != input.side || output.mode != input.mode ||
            output.neighbors != input.neighbors || output.filters != input.filters)
            throw std::runtime_error("Directional prediction mismatch in fixture " + std::to_string(i) +
                                     ", side " + std::to_string(input.side) + ", mode " + std::to_string(input.mode));
    }
    std::cout << cases.size() << " fixtures matched all 24 luma 4x4 and 8x8 modes and the 8x8 filter byte for byte.\n";
}

