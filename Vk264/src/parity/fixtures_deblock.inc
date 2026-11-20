// Vk264 parity module: fixtures_deblock.inc
// Mirrors: x264 common/deblock.c
// Split from dct_parity.cpp lines 1776-2052.

void verifyDeblockFilter(const char* shader) {
    constexpr int maxStride = 48;
    constexpr int geometryCount = 7;
    constexpr std::array<x264_deblock_inter_t, geometryCount> inter{
        deblock_v_luma_c, deblock_h_luma_c, deblock_h_luma_mbaff_c,
        deblock_v_chroma_c, deblock_h_chroma_c, deblock_h_chroma_422_c, deblock_h_chroma_mbaff_c,
    };
    constexpr std::array<x264_deblock_intra_t, geometryCount> intra{
        deblock_v_luma_intra_c, deblock_h_luma_intra_c, deblock_h_luma_intra_mbaff_c,
        deblock_v_chroma_intra_c, deblock_h_chroma_intra_c, deblock_h_chroma_422_intra_c,
        deblock_h_chroma_intra_mbaff_c,
    };
    struct Fixture {
        std::int32_t stride, geometry, intra, alpha, beta;
        std::array<std::int32_t, 4> tc;
        std::int32_t qp, offsetA, offsetB;
        std::array<std::uint32_t, 4> strength;
        std::array<std::uint32_t, 20 * maxStride> pixels;
    };
    static_assert(sizeof(Fixture) == (16 + 20 * maxStride) * sizeof(std::uint32_t));
    std::mt19937 random(0x264db);
    std::vector<Fixture> cases(geometryCount * 2 * 256);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        const auto pattern = i / (geometryCount * 2);
        input.stride = 24 + 8 * (pattern % 4);
        input.geometry = static_cast<int>(i % geometryCount);
        input.intra = static_cast<int>(i / geometryCount % 2);
        input.alpha = pattern % 4 == 0 ? 255 : 1 + random() % 128;
        input.beta = pattern % 4 == 0 ? 18 : 1 + random() % 18;
        for (auto& limit : input.tc) limit = static_cast<int>(random() % 27) - 1;
        input.qp = pattern < 96 ? -1 : static_cast<int>((pattern - 96) % 52);
        input.offsetA = static_cast<int>(random() % 25) - 12;
        input.offsetB = static_cast<int>(random() % 25) - 12;
        for (auto& strength : input.strength) strength = random() % 4;
        if (pattern % 11 == 0) input.strength.fill(0);
        if (pattern == 96 || pattern == 147 || pattern == 199)
            input.offsetA = input.offsetB = pattern == 96 ? -12 : 12;
        const int base = pattern % 5 == 0 ? 0 : pattern % 5 == 1 ? 255 : 128;
        const int variation = pattern % 3 == 0 ? 8 : 16;
        for (auto& sample : input.pixels)
            sample = std::clamp(base + static_cast<int>(random() % (2*variation+1)) - variation, 0, 255);
        if (pattern == 0) input.tc = {-1, 0, 1, 25};
        if (pattern == 1) input.alpha = input.beta = 0;
    }
    const auto inputs = cases;
    execute(shader, cases);
    int changed[2][geometryCount][2]{};
    int skipped[2][geometryCount][2]{};
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        pixel expected[20 * maxStride];
        std::copy(input.pixels.begin(), input.pixels.end(), expected);
        pixel* origin = expected + 4*input.stride + 8;
        std::int8_t tc[4];
        std::uint8_t strength[4];
        std::copy(input.tc.begin(), input.tc.end(), tc);
        std::copy(input.strength.begin(), input.strength.end(), strength);
        if (input.qp >= 0) {
            if (input.intra)
                deblock_edge_intra(nullptr, origin, input.stride, strength, input.qp,
                                   input.offsetA, input.offsetB, input.geometry >= 3, intra[input.geometry]);
            else
                deblock_edge(nullptr, origin, input.stride, strength, input.qp,
                             input.offsetA, input.offsetB, input.geometry >= 3, inter[input.geometry]);
        } else {
            if (input.intra) intra[input.geometry](origin, input.stride, input.alpha, input.beta);
            else inter[input.geometry](origin, input.stride, input.alpha, input.beta, tc);
        }
        if (!matchesNativeBytes(output.pixels, expected) ||
            output.stride != input.stride || output.geometry != input.geometry || output.intra != input.intra ||
            output.alpha != input.alpha || output.beta != input.beta || output.tc != input.tc ||
            output.qp != input.qp || output.offsetA != input.offsetA || output.offsetB != input.offsetB ||
            output.strength != input.strength)
            throw std::runtime_error("Deblock filter mismatch in fixture " + std::to_string(i) +
                                     ", geometry " + std::to_string(input.geometry));
        const bool noChange = std::equal(std::begin(expected), std::end(expected), input.pixels.begin());
        auto& coverage = noChange ? skipped : changed;
        ++coverage[input.intra][input.geometry][input.qp >= 0];
    }
    for (int type = 0; type < 2; ++type)
        for (int geometry = 0; geometry < geometryCount; ++geometry)
            for (int useQp = 0; useQp < 2; ++useQp)
                if (!changed[type][geometry][useQp] || !skipped[type][geometry][useQp])
                    throw std::runtime_error("Missing deblock filter coverage");
    std::cout << cases.size() << " fixtures matched all 14 deblock filters and both QP wrappers byte for byte.\n";
}

void verifyDeblockStrength(const char* shader) {
    struct Fixture {
        std::array<std::uint32_t, X264_SCAN8_SIZE> nonzero;
        std::array<std::int32_t, 2 * X264_SCAN8_LUMA_SIZE> reference;
        std::array<std::int32_t, 2 * X264_SCAN8_LUMA_SIZE * 2> motion;
        std::array<std::uint32_t, 2 * 8 * 4> strength;
        std::int32_t verticalLimit, bframe;
    };
    static_assert(sizeof(Fixture) == 426 * sizeof(std::uint32_t));
    std::mt19937 random(0x264b5);
    std::vector<Fixture> cases(4096);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        input.bframe = i % 2 != 0;
        input.verticalLimit = i % 4 < 2 ? 4 : 2;
        input.strength.fill(255);
        const auto pattern = i / 4;
        if (pattern == 0) continue;
        if (pattern < 2 * X264_SCAN8_LUMA_SIZE + 1) {
            const auto p = pattern - 1;
            input.motion[2*p] = i % 4 < 2 ? 4 : 3;
            input.motion[2*p + 1] = i % 4 < 2 ? 3 : 2;
            continue;
        }
        if (pattern % 4 == 0)
            for (auto& count : input.nonzero) count = random() % 9 == 0 ? 1 + random() % 16 : 0;
        if (pattern % 3 == 0)
            for (auto& ref : input.reference) ref = random() % 8 == 0 ? static_cast<int>(random() % 6) - 2 : 0;
        for (auto& component : input.motion)
            component = pattern % 17 == 0 ? static_cast<int>(random() & 65535) - 32768 :
                                           static_cast<int>(random() % 7) - 3;
    }
    const auto inputs = cases;
    execute(shader, cases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        std::uint8_t nonzero[X264_SCAN8_SIZE], strength[2][8][4], packed[64];
        std::int8_t reference[2][X264_SCAN8_LUMA_SIZE];
        std::int16_t motion[2][X264_SCAN8_LUMA_SIZE][2];
        std::copy(input.nonzero.begin(), input.nonzero.end(), nonzero);
        std::memset(strength, 255, sizeof(strength));
        for (int list = 0; list < 2; ++list)
            for (int loc = 0; loc < X264_SCAN8_LUMA_SIZE; ++loc) {
                const int p = list*X264_SCAN8_LUMA_SIZE + loc;
                reference[list][loc] = static_cast<std::int8_t>(input.reference[p]);
                motion[list][loc][0] = static_cast<std::int16_t>(input.motion[2*p]);
                motion[list][loc][1] = static_cast<std::int16_t>(input.motion[2*p + 1]);
            }
        deblock_strength_c(nonzero, reference, motion, strength, input.verticalLimit, input.bframe);
        std::memcpy(packed, strength, sizeof(packed));
        if (!matchesNativeBytes(output.strength, packed) || output.nonzero != input.nonzero ||
            output.reference != input.reference || output.motion != input.motion ||
            output.verticalLimit != input.verticalLimit || output.bframe != input.bframe)
            throw std::runtime_error("Deblock boundary strength mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " fixtures matched x264 deblock boundary strengths byte for byte.\n";
}

struct DeblockBlock {
    std::uint32_t mask;
    std::int32_t mvX, mvY;
};

void deblockPicture(const char* shader, int width, int height, int qp, int chromaQp, bool intra,
                    const std::vector<DeblockBlock>& macroblocks, std::vector<pixel>& luma,
                    std::array<std::vector<pixel>, 2>& chroma, int offsetA = 0, int offsetB = 0, bool tiled = false) {
    const int columns = width/16, rows = height/16;
    std::vector<std::uint32_t> words{static_cast<std::uint32_t>(columns), static_cast<std::uint32_t>(rows),
        static_cast<std::uint32_t>(qp), static_cast<std::uint32_t>(chromaQp), static_cast<std::uint32_t>(intra),
        static_cast<std::uint32_t>(offsetA), static_cast<std::uint32_t>(offsetB)};
    const auto pixelWords = (luma.size()+chroma[0].size()+chroma[1].size())/(tiled ? 4 : 1);
    words.reserve(words.size()+3*macroblocks.size()+pixelWords);
    for (const auto& mb : macroblocks)
        words.insert(words.end(), {mb.mask, static_cast<std::uint32_t>(mb.mvX), static_cast<std::uint32_t>(mb.mvY)});
    const auto pixelOffset = words.size();
    if (tiled) {
        const auto append = [&words](const std::vector<pixel>& plane) {
            for (std::size_t i = 0; i < plane.size(); i += 4)
                words.push_back(plane[i] | (std::uint32_t(plane[i+1]) << 8) |
                                  (std::uint32_t(plane[i+2]) << 16) | (std::uint32_t(plane[i+3]) << 24));
        };
        append(luma);
        for (const auto& plane : chroma) append(plane);
        const int tileRows = (rows+3)/4, tileColumns = (columns+rows+2)/4;
        execute(shader, words, 1, {}, 64*tileRows, {}, tileRows+tileColumns-1, true);
        std::size_t offset = pixelOffset;
        const auto restore = [&](std::vector<pixel>& plane) {
            for (std::size_t i = 0; i < plane.size(); i += 4) {
                const auto word = words[offset++];
                for (int j = 0; j < 4; ++j) plane[i+j] = static_cast<pixel>(word >> (8*j));
            }
        };
        restore(luma);
        for (auto& plane : chroma) restore(plane);
        return;
    }
    words.insert(words.end(), luma.begin(), luma.end());
    for (const auto& plane : chroma) words.insert(words.end(), plane.begin(), plane.end());
    execute(shader, words, 1, {}, rows, {}, columns+2*(rows-1));
    auto samples = words.begin()+pixelOffset;
    std::copy_n(samples, luma.size(), luma.begin());
    samples += luma.size();
    for (auto& plane : chroma) {
        std::copy_n(samples, plane.size(), plane.begin());
        samples += plane.size();
    }
}

void verifyDeblockFrame(const char* shader, bool tiled = false) {
    constexpr x264_deblock_inter_t lumaFilter[]{deblock_h_luma_c, deblock_v_luma_c};
    constexpr x264_deblock_intra_t strongLuma[]{deblock_h_luma_intra_c, deblock_v_luma_intra_c};
    constexpr x264_deblock_inter_t chromaFilter[]{deblock_h_chroma_c, deblock_v_chroma_c};
    constexpr x264_deblock_intra_t strongChroma[]{deblock_h_chroma_intra_c, deblock_v_chroma_intra_c};
    std::mt19937 random(0x264420);
    for (int test = 0; test < 24; ++test) {
        const int columns = 2+test%5, rows = 2+test/4%4;
        const int width = 16*columns, height = 16*rows;
        const bool intra = test%2 != 0;
        const int qp = 22+test, chromaQp = std::min(qp, 36), offsetA = 2*(test%5-2), offsetB = 2*(test%3-1);
        std::vector<DeblockBlock> blocks(columns*rows);
        for (auto& block : blocks) {
            block.mask = test%3 == 0 ? 0 : random() & 0x7f;
            block.mvX = static_cast<int>(random()%15)-7;
            block.mvY = static_cast<int>(random()%15)-7;
        }
        std::vector<pixel> luma(width*height);
        std::array<std::vector<pixel>, 2> chroma{std::vector<pixel>(width*height/4), std::vector<pixel>(width*height/4)};
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                luma[y*width+x] = 64+(3*(x/4)+2*(y/4)+((x/16)^(y/16)))%43+random()%3;
        for (int p = 0; p < 2; ++p)
            for (int y = 0; y < height/2; ++y)
                for (int x = 0; x < width/2; ++x)
                    chroma[p][y*width/2+x] = 96+8*p+(3*(x/4)+2*(y/4))%33+random()%3;
        auto cpuLuma = luma;
        std::vector<pixel> cpuChroma(width*height/2);
        for (std::size_t i = 0; i < chroma[0].size(); ++i) {
            cpuChroma[2*i] = chroma[0][i];
            cpuChroma[2*i+1] = chroma[1][i];
        }
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < columns; ++x) {
                const int mb = y*columns+x;
                std::uint8_t nnz[X264_SCAN8_SIZE]{}, strength[2][8][4]{};
                std::int8_t references[2][X264_SCAN8_LUMA_SIZE]{};
                std::int16_t motion[2][X264_SCAN8_LUMA_SIZE][2]{};
                const auto fill = [&](int location, int neighbor, int block) {
                    nnz[location] = (blocks[neighbor].mask >> block) & 1;
                    motion[0][location][0] = static_cast<std::int16_t>(blocks[neighbor].mvX);
                    motion[0][location][1] = static_cast<std::int16_t>(blocks[neighbor].mvY);
                };
                for (int row = 0; row < 4; ++row)
                    for (int col = 0; col < 4; ++col)
                        fill(X264_SCAN8_0+row*8+col, mb, row*4+col);
                for (int i = 0; i < 4; ++i) {
                    if (x) fill(X264_SCAN8_0+8*i-1, mb-1, 4*i+3);
                    if (y) fill(X264_SCAN8_0+i-8, mb-columns, 12+i);
                }
                deblock_strength_c(nnz, references, motion, strength, 4, 0);
                for (int dir = 0; dir < 2; ++dir) {
                    for (int edge = (dir ? y : x) != 0 ? 0 : 1; edge < 4; ++edge) {
                        auto* line = cpuLuma.data()+16*(y*width+x)+4*edge*(dir ? width : 1);
                        auto* uv = cpuChroma.data()+8*y*width+16*x+edge*(dir ? 2*width : 4);
                        auto* bs = strength[dir][edge];
                        if (intra && edge == 0) {
                            deblock_edge_intra(nullptr, line, width, bs, qp, offsetA, offsetB, 0, strongLuma[dir]);
                            deblock_edge_intra(nullptr, uv, width, bs, chromaQp, offsetA, offsetB, 1, strongChroma[dir]);
                        } else {
                            if (intra) std::fill_n(bs, 4, 3);
                            deblock_edge(nullptr, line, width, bs, qp, offsetA, offsetB, 0, lumaFilter[dir]);
                            if ((edge & 1) == 0)
                                deblock_edge(nullptr, uv, width, bs, chromaQp, offsetA, offsetB, 1, chromaFilter[dir]);
                        }
                    }
                }
            }
        if (test == 0 && cpuLuma == luma)
            throw std::runtime_error("Deblock frame test did not change luma samples");
        deblockPicture(shader, width, height, qp, chromaQp, intra, blocks, luma, chroma, offsetA, offsetB, tiled);
        if (cpuLuma != luma) throw std::runtime_error("Deblocked luma differs from native x264 filters");
        for (std::size_t i = 0; i < chroma[0].size(); ++i)
            if (cpuChroma[2*i] != chroma[0][i] || cpuChroma[2*i+1] != chroma[1][i])
                throw std::runtime_error("Deblocked chroma differs from native x264 filters");
    }
    std::cout << "24 progressive pictures matched x264 row-order deblocking.\n";
}

