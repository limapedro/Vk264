// Vk264 parity module: fixtures_cabac.inc
// Mirrors: x264 encoder/cabac.c
// Split from dct_parity.cpp lines 2053-2466.

namespace cabacCorpus {
constexpr int residualBankSize = 8192;
constexpr int maxOperations = 160, maxBytes = 768;
constexpr int frameMaxBytes = 2097152, frameMaxOperations = 8192, macroblockWords = 394;
constexpr int frameResidualBankSize = frameMaxOperations*macroblockWords;
enum Kind {
    decision, bypass, ueBypass, terminal, residual, intraType, intra4x4Mode, intraChromaMode,
    intra16Macroblock, codedBlockPattern, qpDelta, transformSize, macroblockSkip, mvdComponent, inter16Macroblock,
    wholeFrame, refIdx, mbTypeB
};
struct Operation {
    std::int32_t kind, first, second;
};
template<int Capacity, int Operations = maxOperations>
struct FixtureStorage {
    std::int32_t qp, model, chroma444, frame, count, finish;
    std::array<Operation, Operations> operations;
    std::array<std::int32_t, 64> coefficients;
    std::array<std::uint32_t, 1024> context;
    std::array<std::uint32_t, Capacity> bytes;
    std::int32_t low, range, queue, outstanding, position, effectiveQp, mvdMagnitude;
};
using Fixture = FixtureStorage<maxBytes>;
using FrameFixture = FixtureStorage<frameMaxBytes, frameMaxOperations>;
static_assert(sizeof(Operation) == 3 * sizeof(std::int32_t));
static_assert(sizeof(Fixture) == (13 + 3 * maxOperations + 64 + 1024 + maxBytes) * sizeof(std::uint32_t));
static_assert(sizeof(FrameFixture) == (13 + 3 * frameMaxOperations + 64 + 1024 + frameMaxBytes)*sizeof(std::uint32_t));

Operation intraTypeOp(const std::array<int, 6>& contexts, int type, int mode, bool luma, int chroma) {
    int packed = 0;
    for (int i = 0; i < 5; ++i) packed |= contexts[i] << (i*6);
    return {intraType, packed, contexts[5] | (type << 6) | (mode << 8) | (int(luma) << 11) | (chroma << 12)};
}
}

std::vector<std::int32_t> cabacTables(const std::vector<std::int32_t>& residuals = {},
                                      int capacity = cabacCorpus::residualBankSize) {
    std::vector<std::int32_t> tables;
    for (const auto& row : x264_cabac_context_init_I)
        tables.insert(tables.end(), std::begin(row), std::end(row));
    for (const auto& model : x264_cabac_context_init_PB)
        for (const auto& row : model)
            tables.insert(tables.end(), std::begin(row), std::end(row));
    for (const auto& row : x264_cabac_range_lps)
        tables.insert(tables.end(), std::begin(row), std::end(row));
    for (const auto& row : x264_cabac_transition)
        tables.insert(tables.end(), std::begin(row), std::end(row));
    tables.insert(tables.end(), std::begin(x264_cabac_renorm_shift), std::end(x264_cabac_renorm_shift));
    for (const auto& row : x264_significant_coeff_flag_offset)
        tables.insert(tables.end(), std::begin(row), std::end(row));
    for (const auto& row : x264_last_coeff_flag_offset)
        tables.insert(tables.end(), std::begin(row), std::end(row));
    tables.insert(tables.end(), std::begin(x264_coeff_abs_level_m1_offset), std::end(x264_coeff_abs_level_m1_offset));
    tables.insert(tables.end(), std::begin(x264_count_cat_m1), std::end(x264_count_cat_m1));
    for (const auto& row : x264_significant_coeff_flag_offset_8x8)
        tables.insert(tables.end(), std::begin(row), std::end(row));
    tables.insert(tables.end(), std::begin(x264_last_coeff_flag_offset_8x8), std::end(x264_last_coeff_flag_offset_8x8));
    tables.insert(tables.end(), std::begin(x264_coeff_flag_offset_chroma_422_dc), std::end(x264_coeff_flag_offset_chroma_422_dc));
    if (residuals.size() > static_cast<std::size_t>(capacity))
        throw std::runtime_error("CABAC residual bank capacity exceeded");
    const auto size = tables.size() + capacity;
    tables.insert(tables.end(), residuals.begin(), residuals.end());
    tables.resize(size);
    return tables;
}

void verifyCabac(const char* shader) {
    using namespace cabacCorpus;
    const auto tables = cabacTables();
    constexpr int initializationCases = 52 * 4 * 2;
    constexpr int residualStart = initializationCases + 2048;
    constexpr int categoryVariants = 15; // 14 standard categories plus the 4:2:2 chroma DC path.
    constexpr int residualCases = 2 * categoryVariants * 32;
    constexpr int syntaxStart = residualStart + residualCases;
    constexpr int syntaxCases = 1024;
    constexpr int macroblockSyntaxStart = syntaxStart+syntaxCases;
    constexpr int macroblockSyntaxCases = 2*52*52;
    // B mb_type header fixtures (issue #4): B_DIRECT + the nine 16x16/16x8/8x16
    // B modes, three partitions and three neighbour-context values.
    constexpr int bMbTypeStart = macroblockSyntaxStart + macroblockSyntaxCases;
    constexpr int bMbTypeCases = 10*3*3;
    std::vector<Fixture> cases(bMbTypeStart + bMbTypeCases);
    std::mt19937 random(0x264cab);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        input.qp = static_cast<int>(i % 52);
        input.model = static_cast<int>(i / 52 % 4);
        input.chroma444 = static_cast<int>(i / (52*4) % 2);
        input.frame = static_cast<int>(i % 97);
        input.finish = i >= initializationCases && i % 3 != 0;
        for (auto& state : input.context) state = random() & 255;
        for (auto& byte : input.bytes) byte = random() & 255;
        input.bytes[0] = 165; // The slice header byte preceding the arithmetic stream.
        if (i < initializationCases) continue;
        if (i >= bMbTypeStart) {
            const int index = static_cast<int>(i)-bMbTypeStart;
            const int type = 7 + index % 10;    // B_DIRECT .. B_BI_BI
            const int part = (index / 10) % 3;  // 0=D_16x8, 1=D_8x16, 2=D_16x16
            const int ctx = (index / 30) % 3;   // left/top neighbour context
            input.qp = (index * 7) % 52;
            input.model = 1 + (index / 3) % 3;  // P/B context model 0..2
            input.operations[input.count++] = {mbTypeB, ctx | (type << 2) | (part << 7), 0};
            continue;
        }
        if (i >= macroblockSyntaxStart) {
            const int index = static_cast<int>(i)-macroblockSyntaxStart;
            const int qp = index%52, previousQp = index/52%52;
            const int currentCbp = static_cast<int>(random()%48);
            const int left = static_cast<int>(random()%49), top = static_cast<int>(random()%49);
            const int flags = index >= 52*52 ? 1 : static_cast<int>(random() & 3);
            input.operations[input.count++] = {codedBlockPattern, currentCbp, left | (top << 7)};
            input.operations[input.count++] = {qpDelta, qp | (previousQp << 6),
                                                flags | static_cast<int>(random() & 28)};
            input.operations[input.count++] = {transformSize, index%3, index%2};
            input.operations[input.count++] = {macroblockSkip, index%3 | (index%2 << 2),
                                               static_cast<int>(random() & 3)};
            constexpr int deltas[]{-32768, -256, -67, -66, -65, -9, -8, -3, -2, -1, 0, 1, 2, 3, 7, 8,
                                   9, 10, 32, 33, 65, 66, 67, 32767};
            const int component = index/static_cast<int>(std::size(deltas));
            input.operations[input.count++] = {mvdComponent, deltas[index%std::size(deltas)], component%6};
            continue;
        }
        if (i >= syntaxStart) {
            constexpr std::array<int, 6> contexts[] = {{3,6,7,8,9,10}, {17,18,19,19,20,20}, {32,33,34,34,35,35}};
            const auto index = i-syntaxStart;
            const int type = static_cast<int>(index % 4);
            input.operations[input.count++] = {intra4x4Mode, static_cast<int>(index/7 % 9), static_cast<int>(index % 9)};
            input.operations[input.count++] = {intraChromaMode, static_cast<int>(index/16 % 7),
                                                static_cast<int>(index % 16)};
            auto ctx = contexts[index/11 % std::size(contexts)];
            if (ctx[0] == 3) ctx[0] += index/13 % 3;
            input.operations[input.count++] = intraTypeOp(ctx, type, static_cast<int>(index/5 % 7),
                                                          index % 3 != 0, static_cast<int>(index/3 % 3));
            if (type == I_PCM) input.finish = 0; // This type terminates the arithmetic coder.
            continue;
        }
        if (i >= residualStart) {
            const auto index = i - residualStart;
            const int variant = static_cast<int>(index / 2 % categoryVariants);
            const int category = variant == 14 ? 3 : variant;
            const bool chroma422dc = variant == 14;
            const int field = static_cast<int>(index % 2);
            const int count = chroma422dc ? 8 : 1 + x264_count_cat_m1[category];
            input.chroma444 = category >= 6;
            input.count = 3;
            input.operations[0] = {decision, 0, static_cast<int>(random() & 1)};
            input.operations[1] = {residual, category, field | (int(chroma422dc) << 1)};
            input.operations[2] = {terminal, 0, 0};
            const auto pattern = index / (2*categoryVariants);
            if (pattern == 0) input.coefficients[0] = 1;
            else if (pattern == 1) input.coefficients[count-1] = -1;
            else {
                constexpr int levels[] = {-32768, -16, -15, -14, -2, -1, 1, 2, 14, 15, 16, 32767};
                for (int p = 0; p < count; ++p)
                    if (pattern % 2 == 0 || random() % 4 == 0)
                        input.coefficients[p] = levels[random() % std::size(levels)];
                if (std::none_of(input.coefficients.begin(), input.coefficients.end(), [](int v) { return v != 0; }))
                    input.coefficients[random() % count] = -15;
            }
            continue;
        }
        const auto sequence = (i - initializationCases) % 16;
        input.count = sequence < 6 ? maxOperations : static_cast<int>(random() % (maxOperations+1));
        const int contexts = input.chroma444 ? 1024 : 460;
        for (int j = 0; j < input.count; ++j) {
            auto& op = input.operations[j];
            switch (sequence) {
                case 0: op = {bypass, -1, 0}; break;
                case 1: op = {bypass, j < input.count-12 ? -1 : 0, 0}; break;
                case 2: op = {decision, contexts-1, j % 2}; break;
                case 3: op = {terminal, 0, 0}; break;
                case 4: op = {ueBypass, 0, j % 2 ? 32767 : 0}; break;
                case 5: op = {ueBypass, 3, j % 2 ? 32767 : 0}; break;
                default: {
                    const auto kind = random() % 10;
                    if (kind < 6) op = {decision, static_cast<int>(random() % contexts),
                                        static_cast<int>(random() & 1)};
                    else if (kind < 8) op = {bypass, -static_cast<int>(random() & 1), 0};
                    else if (kind == 8) op = {ueBypass, static_cast<int>(random() % 4),
                                              static_cast<int>(random() % 256)};
                    else op = {terminal, 0, 0};
                    break;
                }
            }
        }
    }
    const auto inputs = cases;
    execute(shader, cases, 1, tables);
    x264_t encoder{};
    encoder.chroma444 = true;
    x264_cabac_init(&encoder);
    bool sawPending = false, sawCarry = false;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        std::uint8_t bytes[maxBytes];
        std::copy(input.bytes.begin(), input.bytes.end(), bytes);
        x264_cabac_t coder{};
        dctcoef coefficients[64];
        std::copy(input.coefficients.begin(), input.coefficients.end(), coefficients);
        std::copy(input.context.begin(), input.context.end(), coder.state);
        encoder.i_frame = input.frame;
        encoder.chroma444 = input.chroma444 != 0;
        x264_cabac_context_init(&encoder, &coder,
            input.operations[0].kind == mbTypeB ? SLICE_TYPE_B : (input.model == 0 ? SLICE_TYPE_I : SLICE_TYPE_P),
                                input.qp, input.model-1);
        x264_cabac_encode_init(&coder, bytes+1, bytes+maxBytes);
        int effectiveQp = input.effectiveQp;
        int mvdMagnitude = input.mvdMagnitude;
        for (int j = 0; j < input.count; ++j) {
            const auto& op = input.operations[j];
            const std::uint8_t* previous = coder.p-1;
            const auto previousByte = *previous;
            switch (op.kind) {
                case decision: x264_cabac_encode_decision_c(&coder, op.first, op.second); break;
                case bypass: x264_cabac_encode_bypass_c(&coder, op.first); break;
                case ueBypass: x264_cabac_encode_ue_bypass(&coder, op.first, op.second); break;
                case terminal: x264_cabac_encode_terminal_c(&coder); break;
                case residual: {
                    const int category = op.first;
                    const bool chroma422dc = (op.second & 2) != 0;
                    encoder.interlaced = op.second & 1;
                    constexpr std::array lastBySize{coeff_last4, coeff_last15, coeff_last16, coeff_last64};
                    const auto countM1 = x264_count_cat_m1[category];
                    encoder.quantf.coeff_last[category] = chroma422dc ? coeff_last8 :
                        lastBySize[countM1 == 3 ? 0 : countM1 == 14 ? 1 : countM1 == 15 ? 2 : 3];
                    if (chroma422dc) cabac_block_residual_internal(&encoder, &coder, category, coefficients, 1);
                    else x264_cabac_block_residual_c(&encoder, &coder, category, coefficients);
                    break;
                }
                case intraType:
                    encoder.mb.i_intra16x16_pred_mode = (op.second >> 8) & 7;
                    encoder.mb.i_cbp_luma = (op.second >> 11) & 1;
                    encoder.mb.i_cbp_chroma = (op.second >> 12) & 3;
                    cabac_mb_type_intra(&encoder, &coder, (op.second >> 6) & 3,
                                       op.first & 63, (op.first >> 6) & 63, (op.first >> 12) & 63,
                                       (op.first >> 18) & 63, (op.first >> 24) & 63, op.second & 63);
                    break;
                case intra4x4Mode:
                    cabac_intra4x4_pred_mode(&coder, op.first, op.second);
                    break;
                case intraChromaMode: {
                    const std::uint8_t neighbors[]{static_cast<std::uint8_t>((op.second >> 2) & 1),
                                                  static_cast<std::uint8_t>((op.second >> 3) & 1)};
                    encoder.mb.chroma_pred_mode = neighbors;
                    encoder.mb.i_mb_left_xy[0] = 0;
                    encoder.mb.i_mb_top_xy = 1;
                    encoder.mb.i_neighbour = op.second & 3;
                    encoder.mb.i_chroma_pred_mode = op.first;
                    cabac_intra_chroma_pred_mode(&encoder, &coder);
                    encoder.mb.chroma_pred_mode = nullptr;
                    break;
                }
                case codedBlockPattern:
                    encoder.mb.i_cbp_luma = op.first & 15;
                    encoder.mb.i_cbp_chroma = op.first >> 4;
                    encoder.mb.cache.i_cbp_left = (op.second & 127)-1;
                    encoder.mb.cache.i_cbp_top = (op.second >> 7)-1;
                    cabac_cbp_luma(&encoder, &coder);
                    cabac_cbp_chroma(&encoder, &coder);
                    break;
                case qpDelta: {
                    const std::int16_t cbp[]{static_cast<std::int16_t>(op.second & 2 ? 16 : 0),
                                             static_cast<std::int16_t>(op.second & 16 ? 1 : 0)};
                    const std::uint8_t type[]{0, static_cast<std::uint8_t>(op.second & 8 ? I_16x16 : I_4x4)};
                    encoder.mb.cbp = cbp;
                    encoder.mb.type = type;
                    encoder.mb.i_mb_xy = 0;
                    encoder.mb.i_mb_prev_xy = 1;
                    encoder.mb.i_type = op.second & 1 ? I_16x16 : I_4x4;
                    encoder.mb.i_last_dqp = (op.second & 4) != 0;
                    encoder.mb.i_qp = op.first & 63;
                    encoder.mb.i_last_qp = op.first >> 6;
                    cabac_qp_delta(&encoder, &coder);
                    effectiveQp = encoder.mb.i_qp;
                    encoder.mb.cbp = nullptr;
                    encoder.mb.type = nullptr;
                    break;
                }
                case transformSize:
                    encoder.mb.cache.i_neighbour_transform_size = op.first;
                    encoder.mb.b_transform_8x8 = op.second;
                    cabac_transform_size(&encoder, &coder);
                    break;
                case macroblockSkip:
                    encoder.cabac = coder;
                    encoder.sh.i_type = (op.first >> 2) & 1;
                    encoder.mb.cache.i_neighbour_skip = op.first & 3;
                    x264_cabac_mb_skip(&encoder, op.second & 1);
                    coder = encoder.cabac;
                    if (op.second & 2) x264_cabac_encode_terminal_c(&coder);
                    break;
                case mvdComponent:
                    mvdMagnitude = cabac_mvd_cpn(&encoder, &coder, 0, 0, op.second & 1,
                                                 op.first, op.second >> 1);
                    break;
                case mbTypeB: {
                    // x264 encoder/cabac.c::cabac_mb_header_b (lines 499-573),
                    // mb_type bins only; ref/mvd/subpartitions need issue #5.
                    const int ctx = op.first & 3;
                    const int type = (op.first >> 2) & 31;
                    const int part = (op.first >> 7) & 3;
                    const auto bin = [&](int index, int bit) {
                        x264_cabac_encode_decision_c(&coder, index, bit);
                    };
                    if (type == 7) { bin(27+ctx, 0); break; }  // B_DIRECT
                    bin(27+ctx, 1);
                    if (type == 17) {  // B_8x8 header prefix only
                        bin(27+3, 1); bin(27+4, 1);
                        bin(27+5, 1); bin(27+5, 1); bin(27+5, 1);
                        break;
                    }
                    static const std::uint8_t mbBits[9*3] = {
                        0x31, 0x29, 0x04, 0x35, 0x2d, 0x00, 0x43, 0x63, 0x00,
                        0x3d, 0x2f, 0x00, 0x39, 0x25, 0x06, 0x53, 0x73, 0x00,
                        0x4b, 0x6b, 0x00, 0x5b, 0x7b, 0x00, 0x47, 0x67, 0x21};
                    int bits = mbBits[(type-8)*3 + part];
                    bin(27+3, bits & 1);
                    bin(27+5-(bits & 1), (bits >> 1) & 1);
                    bits >>= 2;
                    if (bits != 1) {
                        bin(27+5, bits & 1); bits >>= 1;
                        bin(27+5, bits & 1); bits >>= 1;
                        bin(27+5, bits & 1); bits >>= 1;
                        if (bits != 1) bin(27+5, bits & 1);
                    }
                    break;
                }
            }
            sawPending |= coder.i_bytes_outstanding > 0;
            sawCarry |= *previous != previousByte;
        }
        if (input.finish) x264_cabac_encode_flush(&encoder, &coder);
        if (!matchesNativeBytes(output.bytes, bytes) || !matchesNativeBytes(output.context, coder.state) ||
            !matchesNativeBytes(output.coefficients, coefficients) ||
            output.low != coder.i_low || output.range != coder.i_range || output.queue != coder.i_queue ||
            output.outstanding != coder.i_bytes_outstanding || output.position != coder.p-bytes ||
            output.qp != input.qp || output.model != input.model || output.chroma444 != input.chroma444 ||
            output.frame != input.frame || output.count != input.count || output.finish != input.finish ||
            output.effectiveQp != effectiveQp || output.mvdMagnitude != mvdMagnitude ||
            std::memcmp(output.operations.data(), input.operations.data(), sizeof(input.operations)) != 0)
            throw std::runtime_error("CABAC byte or context mismatch in fixture " + std::to_string(i));
    }
    if (!sawPending || !sawCarry) throw std::runtime_error("Missing CABAC carry coverage");
    std::cout << cases.size() << " CABAC sequences matched x264 bytes, contexts and arithmetic state, including "
              << residualCases << " residual, " << syntaxCases << " intra syntax, "
              << macroblockSyntaxCases << " coded-block, QP-delta, transform-size, skip and motion syntax, and "
              << bMbTypeCases << " B macroblock-type header sequences.\n";
}

// Issue #5 sub-PR B: verify the P-slice reference-index bin syntax against
// x264 encoder/cabac.c::cabac_ref_internal (bframe == 0 path). The host side
// calls x264's own decision coder with the exact contexts/bins from that
// function; the GPU side runs the new refIdx operation.
void verifyCabacRefIdx(const char* shader) {
    using namespace cabacCorpus;
    const auto tables = cabacTables();
    std::vector<Fixture> cases;
    for (int ref : {0, 1, 2, 3, 7, 15})
        for (int left : {-1, 0, 1, 3})
            for (int top : {-1, 0, 1, 3})
                for (int qp : {0, 17, 34, 51})
                    for (int model = 0; model < 4; ++model) {
                        Fixture input{};
                        input.qp = qp;
                        input.model = model;
                        input.chroma444 = 0;
                        input.frame = static_cast<int>(cases.size() % 97);
                        input.finish = static_cast<int>(cases.size() % 2);
                        for (std::size_t b = 0; b < input.bytes.size(); ++b)
                            input.bytes[b] = static_cast<std::uint32_t>((b*37+165) & 255);
                        input.bytes[0] = 165; // slice-header byte preceding the arithmetic stream
                        input.operations[input.count++] =
                            {refIdx, ref, (left > 0 ? 1 : 0) | ((top > 0 ? 1 : 0) << 1)};
                        cases.push_back(input);
                    }
    const auto inputs = cases;
    execute(shader, cases, 1, tables);
    x264_t encoder{};
    encoder.chroma444 = true;
    x264_cabac_init(&encoder);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        std::uint8_t bytes[maxBytes];
        std::copy(input.bytes.begin(), input.bytes.end(), bytes);
        x264_cabac_t coder{};
        encoder.i_frame = input.frame;
        encoder.chroma444 = false;
        x264_cabac_context_init(&encoder, &coder, input.model == 0 ? SLICE_TYPE_I : 0,
                                input.qp, input.model-1);
        x264_cabac_encode_init(&coder, bytes+1, bytes+maxBytes);
        // Verbatim translation of x264 cabac.c:246-258 for P slices (bframe=0).
        const int left = (input.operations[0].second & 1) != 0;
        const int top = (input.operations[0].second >> 1) & 1;
        int ctx = left + 2*top;
        for (int r = input.operations[0].first; r > 0; --r) {
            x264_cabac_encode_decision_c(&coder, 54+ctx, 1);
            ctx = (ctx >> 2)+4;
        }
        x264_cabac_encode_decision_c(&coder, 54+ctx, 0);
        if (input.finish) x264_cabac_encode_flush(&encoder, &coder);
        if (!matchesNativeBytes(output.bytes, bytes) ||
            !matchesNativeBytes(output.context, coder.state) ||
            output.low != coder.i_low || output.range != coder.i_range ||
            output.queue != coder.i_queue || output.outstanding != coder.i_bytes_outstanding ||
            output.position != coder.p-bytes ||
            std::memcmp(output.operations.data(), input.operations.data(), sizeof(input.operations)) != 0)
            throw std::runtime_error("CABAC ref_idx mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " CABAC ref_idx sequences matched x264 cabac_ref_internal bytes, contexts and "
                 "arithmetic state.\n";
}

