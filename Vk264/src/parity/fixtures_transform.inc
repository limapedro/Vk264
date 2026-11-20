// Vk264 parity module: fixtures_transform.inc
// Mirrors: x264 common/dct.c + common/quant.c
// Split from dct_parity.cpp lines 866-1435.

template<int Side, int Group>
void verify(const char* shader) {
    static_assert(sizeof(Block<Group>) == 5 * Group * Group * sizeof(std::uint32_t));
    constexpr int blocksPerGroup = Group * Group / (Side * Side);
    auto blocks = fixtures<Group>();
    const auto inputs = blocks;
    vk264::dct::transform<Side, Group>(shader, blocks);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto& block = blocks[i];
        const auto& input = inputs[i];
        pixel src[Group * FENC_STRIDE]{}, pred[Group * FDEC_STRIDE]{};
        dctcoef expected[blocksPerGroup][Side * Side]{}, inverse[blocksPerGroup][Side * Side]{};
        for (int y = 0; y < Group; ++y) for (int x = 0; x < Group; ++x) {
            const int p = Group*y+x;
            src[y*FENC_STRIDE+x] = static_cast<pixel>(input.source[p]);
            pred[y*FDEC_STRIDE+x] = static_cast<pixel>(input.prediction[p]);
            inverse[p / (Side * Side)][p % (Side * Side)] = static_cast<dctcoef>(input.inverse[p]);
        }
        if constexpr (Side == 4 && Group == 4) {
            sub4x4_dct(expected[0], src, pred);
            add4x4_idct(pred, inverse[0]);
        } else if constexpr (Side == 4 && Group == 8) {
            sub8x8_dct(expected, src, pred);
            add8x8_idct(pred, inverse);
        } else if constexpr (Side == 4 && Group == 16) {
            sub16x16_dct(expected, src, pred);
            add16x16_idct(pred, inverse);
        } else if constexpr (Side == 8 && Group == 8) {
            sub8x8_dct8(expected[0], src, pred);
            add8x8_idct8(pred, inverse[0]);
        } else {
            sub16x16_dct8(expected, src, pred);
            add16x16_idct8(pred, inverse);
        }
        pixel expectedPixels[Group * Group]{};
        for (int y = 0; y < Group; ++y)
            std::memcpy(expectedPixels + y * Group, pred + y * FDEC_STRIDE, Group);
        dctcoef packedExpected[Group * Group], packedInverse[Group * Group];
        std::memcpy(packedExpected, expected, sizeof(packedExpected));
        std::memcpy(packedInverse, inverse, sizeof(packedInverse));
        if (!matchesNativeBytes(block.transformed, packedExpected))
            throw std::runtime_error("Forward coefficient byte mismatch in block " + std::to_string(i));
        if (!matchesNativeBytes(block.reconstructed, expectedPixels))
            throw std::runtime_error("Inverse pixel byte mismatch in block " + std::to_string(i));
        if (!matchesNativeBytes(block.inverse, packedInverse))
            throw std::runtime_error("Inverse coefficient byte mismatch in block " + std::to_string(i));
        if (block.source != input.source || block.prediction != input.prediction)
            throw std::runtime_error("Input pixel buffer modified in block " + std::to_string(i));
    }
    std::cout << blocks.size() << " " << Group << "x" << Group << " groups of " << Side << "x" << Side
              << " blocks matched x264 for forward transforms, inverse reconstruction and coefficient writes.\n";
}

template<int Side>
void verifyQuant(const char* shader) {
    constexpr int count = Side * Side;
    struct Fixture {
        std::int32_t qp, nonzero;
        std::array<std::int32_t, count> coefficients;
        std::array<std::uint32_t, count> multiplier, bias;
        std::array<std::int32_t, count> dequantMatrix, levels, dequantized;
    };
    static_assert(sizeof(Fixture) == (2 + count * 6) * sizeof(std::uint32_t));
    std::mt19937 random(0x264);
    std::vector<Fixture> cases(52 * 64);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& block = cases[i];
        block.qp = static_cast<int>(i / 64);
        for (int p = 0; p < count; ++p) {
            block.coefficients[p] = static_cast<int>(random() & 65535) - 32768;
            block.multiplier[p] = random() & 65535;
            block.bias[p] = random() & 4095;
            block.dequantMatrix[p] = 1 + random() % 2048;
        }
        if (i % 64 == 0) {
            block.coefficients.fill(0);
            block.bias.fill(0);
        }
    }
    vk264::quant::quantize(shader, cases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& block = cases[i];
        dctcoef dct[count];
        udctcoef multiplier[count], bias[count];
        int dequant[6][count]{};
        for (int p = 0; p < count; ++p) {
            dct[p] = static_cast<dctcoef>(block.coefficients[p]);
            multiplier[p] = static_cast<udctcoef>(block.multiplier[p]);
            bias[p] = static_cast<udctcoef>(block.bias[p]);
            dequant[block.qp % 6][p] = block.dequantMatrix[p];
        }
        int nonzero;
        if constexpr (Side == 4) nonzero = quant_4x4(dct, multiplier, bias);
        else nonzero = quant_8x8(dct, multiplier, bias);
        if (nonzero != block.nonzero || !matchesNativeBytes(block.levels, dct))
            throw std::runtime_error("Quantization mismatch in fixture " + std::to_string(i));
        if constexpr (Side == 4) dequant_4x4(dct, dequant, block.qp);
        else dequant_8x8(dct, dequant, block.qp);
        if (!matchesNativeBytes(block.dequantized, dct))
            throw std::runtime_error("Dequantization mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " " << Side << "x" << Side
              << " quantization fixtures matched x264 across QP 0 to 51.\n";
}

void verifyQuantGroup(const char* shader) {
    struct Fixture {
        std::array<std::int32_t, 64> coefficients;
        std::array<std::uint32_t, 16> multiplier, bias;
        std::int32_t nonzeroMask;
    };
    static_assert(sizeof(Fixture) == 97 * sizeof(std::uint32_t));
    std::mt19937 random(0x264444);
    std::vector<Fixture> cases(4096);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        for (auto& coefficient : input.coefficients)
            coefficient = static_cast<int>(random() & 65535) - 32768;
        for (auto& multiplier : input.multiplier) multiplier = random() & 65535;
        for (auto& bias : input.bias) bias = random() & 65535;
        if (i >= 16) continue;
        // Exercise every possible nonzero mask with one retained level per active block.
        input.coefficients.fill(0);
        input.multiplier.fill(65535);
        input.bias.fill(0);
        for (int block = 0; block < 4; ++block)
            if ((i >> block) & 1) input.coefficients[block*16] = block % 2 == 0 ? 32767 : -32768;
    }
    const auto inputs = cases;
    vk264::quant::quantize(shader, cases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        dctcoef coefficients[4][16], packed[64];
        udctcoef multiplier[16], bias[16];
        for (int p = 0; p < 64; ++p)
            coefficients[p / 16][p % 16] = static_cast<dctcoef>(input.coefficients[p]);
        std::copy(input.multiplier.begin(), input.multiplier.end(), multiplier);
        std::copy(input.bias.begin(), input.bias.end(), bias);
        const int nonzeroMask = quant_4x4x4(coefficients, multiplier, bias);
        std::memcpy(packed, coefficients, sizeof(packed));
        if (output.nonzeroMask != nonzeroMask || !matchesNativeBytes(output.coefficients, packed) ||
            !matchesNativeBytes(output.multiplier, multiplier) || !matchesNativeBytes(output.bias, bias))
            throw std::runtime_error("Grouped quantization mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " fixtures matched x264 quant_4x4x4 levels and nonzero masks.\n";
}

void verifyDc(const char* shader) {
    struct Fixture {
        std::int32_t qp;
        std::uint32_t multiplier, bias;
        std::int32_t dequantScale;
        std::array<std::int32_t, 16> source, forward, inverse, quantized, dequantized;
        std::array<std::int32_t, 4> chromaQuantized;
        std::int32_t lumaNonzero, chromaNonzero;
        std::array<std::int32_t, 4> chromaDcOnly;
        std::array<std::int32_t, 64> chromaBlocks;
        std::array<std::int32_t, 8> chroma422DcOnly;
        std::array<std::int32_t, 128> chroma422Blocks;
    };
    static_assert(sizeof(Fixture) == 294 * sizeof(std::uint32_t));
    std::mt19937 random(0x264dc);
    std::vector<Fixture> cases(52 * 64);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& block = cases[i];
        block.qp = static_cast<int>(i / 64);
        block.multiplier = random() & 65535;
        block.bias = random() & 4095;
        // Keep the reference C's signed 32-bit products defined at full coefficient range.
        block.dequantScale = 1 + random() % 31;
        for (auto& value : block.source) value = static_cast<int>(random() & 65535) - 32768;
        if (i % 64 == 0) {
            block.source.fill(0);
            block.bias = 0;
        }
        if (i % 64 == 1) block.source.fill(-32768);
        if (i % 64 == 2) block.source.fill(32767);
        for (auto& value : block.chromaBlocks) value = static_cast<int>(random() & 65535) - 32768;
        for (auto& value : block.chroma422Blocks) value = static_cast<int>(random() & 65535) - 32768;
    }
    const auto inputs = cases;
    vk264::dct::dc(shader, cases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& block = cases[i];
        dctcoef forward[16], inverse[16], quantized[16], dequantized[16], chroma[4];
        for (int p = 0; p < 16; ++p)
            forward[p] = inverse[p] = quantized[p] = dequantized[p] = static_cast<dctcoef>(inputs[i].source[p]);
        std::copy_n(quantized, 4, chroma);
        dct4x4dc(forward);
        idct4x4dc(inverse);
        int lumaNonzero = quant_4x4_dc(quantized, block.multiplier, block.bias);
        int chromaNonzero = quant_2x2_dc(chroma, block.multiplier, block.bias);
        int matrix[6][16]{};
        matrix[block.qp % 6][0] = block.dequantScale;
        dequant_4x4_dc(dequantized, matrix, block.qp);
        dctcoef chromaInput[4], chromaDcOnly[4], chromaBlocks[4][16], packedChroma[64];
        for (int p = 0; p < 4; ++p)
            chromaInput[p] = chromaDcOnly[p] = static_cast<dctcoef>(block.source[p]);
        for (int p = 0; p < 64; ++p)
            chromaBlocks[p / 16][p % 16] = static_cast<dctcoef>(inputs[i].chromaBlocks[p]);
        idct_dequant_2x2_dconly(chromaDcOnly, matrix, block.qp);
        idct_dequant_2x2_dc(chromaInput, chromaBlocks, matrix, block.qp);
        std::memcpy(packedChroma, chromaBlocks, sizeof(packedChroma));
        dctcoef chroma422Input[8], chroma422DcOnly[8], chroma422Blocks[8][16], packedChroma422[128];
        for (int p = 0; p < 8; ++p)
            chroma422Input[p] = chroma422DcOnly[p] = static_cast<dctcoef>(inputs[i].source[p]);
        for (int p = 0; p < 128; ++p)
            chroma422Blocks[p / 16][p % 16] = static_cast<dctcoef>(inputs[i].chroma422Blocks[p]);
        idct_dequant_2x4_dconly(chroma422DcOnly, matrix, block.qp);
        idct_dequant_2x4_dc(chroma422Input, chroma422Blocks, matrix, block.qp);
        std::memcpy(packedChroma422, chroma422Blocks, sizeof(packedChroma422));
        if (!matchesNativeBytes(block.forward, forward) || !matchesNativeBytes(block.inverse, inverse) ||
            !matchesNativeBytes(block.quantized, quantized) || !matchesNativeBytes(block.dequantized, dequantized) ||
            !matchesNativeBytes(block.chromaQuantized, chroma) ||
            !matchesNativeBytes(block.chromaDcOnly, chromaDcOnly) ||
            !matchesNativeBytes(block.chromaBlocks, packedChroma) ||
            !matchesNativeBytes(block.chroma422DcOnly, chroma422DcOnly) ||
            !matchesNativeBytes(block.chroma422Blocks, packedChroma422) ||
            !std::equal(std::begin(chromaInput), std::end(chromaInput), inputs[i].source.begin()) ||
            !std::equal(std::begin(chroma422Input), std::end(chroma422Input), inputs[i].source.begin()) ||
            block.source != inputs[i].source ||
            lumaNonzero != block.lumaNonzero || chromaNonzero != block.chromaNonzero)
            throw std::runtime_error("DC function output mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " DC fixtures matched nine x264 functions byte for byte across QP 0 to 51.\n";
}

void verifyZigzag(const char* shader) {
    struct Fixture {
        std::int32_t side, field, acOnly;
        std::array<std::int32_t, 64> coefficient;
        std::array<std::uint32_t, 64> source, destination;
        std::array<std::int32_t, 64> scan, residual, interleaved;
        std::array<std::uint32_t, 10> nonzeroContext;
        std::int32_t dc, nonzero;
    };
    static_assert(sizeof(Fixture) == (3 + 64 * 6 + 10 + 2) * sizeof(std::uint32_t));
    std::mt19937 random(0x264);
    std::vector<Fixture> cases(4096);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& block = cases[i];
        block.side = (i & 1) ? 8 : 4;
        block.field = (i >> 1) & 1;
        block.acOnly = block.side == 4 ? (i >> 2) & 1 : 0;
        block.scan.fill(-1234);
        block.residual.fill(-1234);
        block.interleaved.fill(-1234);
        block.nonzeroContext.fill(0xb7);
        block.dc = -1234;
        for (int p = 0; p < 64; ++p) {
            block.coefficient[p] = static_cast<int>(random() & 65535) - 32768;
            block.source[p] = random() & 255;
            block.destination[p] = random() & 255;
        }
        // Distinguish identical blocks, a DC-only change, and AC changes.
        if ((i >> 3) % 8 <= 1) {
            block.destination = block.source;
            block.coefficient.fill(0);
            if ((i >> 3) % 8 == 1) {
                block.destination[0] ^= 255;
                block.coefficient[0] = -32768;
            }
        }
    }
    const auto inputs = cases;
    vk264::dct::zigzag(shader, cases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& block = cases[i];
        const auto& input = inputs[i];
        int side = input.side;
        pixel src[8 * FENC_STRIDE]{}, dst[8 * FDEC_STRIDE]{};
        pixel destination[64]{};
        dctcoef coefficient[64], scan[64], residual[64], interleaved[64], dc = -1234;
        std::uint8_t context[10];
        for (int p = 0; p < 64; ++p) {
            coefficient[p] = static_cast<dctcoef>(input.coefficient[p]);
            destination[p] = static_cast<pixel>(input.destination[p]);
        }
        for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
            src[y * FENC_STRIDE + x] = static_cast<pixel>(input.source[y * side + x]);
            dst[y * FDEC_STRIDE + x] = destination[y * side + x];
        }
        std::fill_n(scan, 64, -1234);
        std::fill_n(residual, 64, -1234);
        std::fill_n(interleaved, 64, -1234);
        std::fill_n(context, 10, 0xb7);
        int nonzero;
        if (side == 4) {
            (input.field ? zigzag_scan_4x4_field : zigzag_scan_4x4_frame)(scan, coefficient);
            nonzero = input.acOnly
                ? (input.field ? zigzag_sub_4x4ac_field : zigzag_sub_4x4ac_frame)(residual, src, dst, &dc)
                : (input.field ? zigzag_sub_4x4_field : zigzag_sub_4x4_frame)(residual, src, dst);
        } else {
            (input.field ? zigzag_scan_8x8_field : zigzag_scan_8x8_frame)(scan, coefficient);
            nonzero = (input.field ? zigzag_sub_8x8_field : zigzag_sub_8x8_frame)(residual, src, dst);
            zigzag_interleave_8x8_cavlc(interleaved, scan, context);
        }
        for (int y = 0; y < side; ++y)
            std::memcpy(destination + y * side, dst + y * FDEC_STRIDE, side);
        if (!matchesNativeBytes(block.scan, scan) || !matchesNativeBytes(block.residual, residual) ||
            !matchesNativeBytes(block.destination, destination) || !matchesNativeBytes(block.interleaved, interleaved) ||
            !matchesNativeBytes(block.nonzeroContext, context) || block.dc != dc || block.nonzero != nonzero ||
            block.source != input.source || block.coefficient != input.coefficient)
            throw std::runtime_error("Zigzag function output mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " fixtures matched all 11 zigzag functions byte for byte.\n";
}

void verifyChromaDc(const char* shader) {
    struct Fixture {
        std::array<std::int32_t, 128> source, prediction;
        std::array<std::int32_t, 64> blocks420;
        std::array<std::int32_t, 128> blocks422;
        std::int32_t residualSum;
        std::array<std::int32_t, 4> residual420;
        std::array<std::int32_t, 8> residual422;
        std::array<std::int32_t, 4> transformed420;
        std::array<std::int32_t, 8> transformed422;
    };
    static_assert(sizeof(Fixture) == 473 * sizeof(std::uint32_t));
    std::mt19937 random(0x264420);
    std::vector<Fixture> cases(4096);
    for (auto& input : cases) {
        for (auto& sample : input.source) sample = random() & 255;
        for (auto& sample : input.prediction) sample = random() & 255;
        for (auto& level : input.blocks420) level = static_cast<int>(random() & 65535) - 32768;
        for (auto& level : input.blocks422) level = static_cast<int>(random() & 65535) - 32768;
    }
    cases[0].source.fill(255);
    cases[0].prediction.fill(0);
    cases[0].blocks420.fill(32767);
    cases[0].blocks422.fill(32767);
    cases[1].source.fill(0);
    cases[1].prediction.fill(255);
    cases[1].blocks420.fill(-32768);
    cases[1].blocks422.fill(-32768);
    cases[2].prediction = cases[2].source;
    for (int impulse = 0; impulse < 8; ++impulse) {
        auto& input = cases[3 + impulse];
        input.source.fill(0);
        input.prediction.fill(0);
        input.source[(impulse / 2)*32 + (impulse % 2)*4] = 255;
    }

    const auto inputs = cases;
    vk264::dct::chromaDc(shader, cases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        pixel source[16 * FENC_STRIDE]{}, prediction[16 * FDEC_STRIDE]{};
        for (int p = 0; p < 128; ++p) {
            source[(p / 8)*FENC_STRIDE + (p % 8)] = static_cast<pixel>(input.source[p]);
            prediction[(p / 8)*FDEC_STRIDE + (p % 8)] = static_cast<pixel>(input.prediction[p]);
        }
        dctcoef blocks420[4][16], blocks422[8][16];
        for (int p = 0; p < 64; ++p)
            blocks420[p / 16][p % 16] = static_cast<dctcoef>(input.blocks420[p]);
        for (int p = 0; p < 128; ++p)
            blocks422[p / 16][p % 16] = static_cast<dctcoef>(input.blocks422[p]);
        dctcoef residual420[4], residual422[8], transformed420[4], transformed422[8];
        const int residualSum = sub4x4_dct_dc(source, prediction);
        sub8x8_dct_dc(residual420, source, prediction);
        sub8x16_dct_dc(residual422, source, prediction);
        dct2x2dc(transformed420, blocks420);
        dct2x4dc(transformed422, blocks422);
        dctcoef packed420[64], packed422[128];
        std::memcpy(packed420, blocks420, sizeof(packed420));
        std::memcpy(packed422, blocks422, sizeof(packed422));
        if (output.residualSum != residualSum ||
            !matchesNativeBytes(output.residual420, residual420) ||
            !matchesNativeBytes(output.residual422, residual422) ||
            !matchesNativeBytes(output.transformed420, transformed420) ||
            !matchesNativeBytes(output.transformed422, transformed422) ||
            !matchesNativeBytes(output.blocks420, packed420) ||
            !matchesNativeBytes(output.blocks422, packed422) ||
            output.source != input.source || output.prediction != input.prediction)
            throw std::runtime_error("Chroma DC function output mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " fixtures matched all five forward chroma DC functions byte for byte.\n";
}

void verifyReconstructDc(const char* shader) {
    struct Fixture {
        std::int32_t side;
        std::array<std::int32_t, 16> coefficients;
        std::array<std::uint32_t, 256> pixels;
    };
    static_assert(sizeof(Fixture) == 273 * sizeof(std::uint32_t));
    const int boundaryCoefficients[] = {-32768, -97, -96, -95, -33, -32, -31, 0,
                                        31, 32, 33, 95, 96, 97, 32767};
    std::mt19937 random(0x264dca);
    std::vector<Fixture> cases(3 * 4096);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        input.side = 4 << (i % 3);
        for (auto& dc : input.coefficients) dc = static_cast<int>(random() & 65535) - 32768;
        for (auto& sample : input.pixels) sample = random() & 255;
        const std::size_t index = i / 3;
        if (index < std::size(boundaryCoefficients)) {
            input.coefficients.fill(boundaryCoefficients[index]);
            input.pixels.fill(128);
        }
    }
    const auto inputs = cases;
    vk264::dct::reconstructDc(shader, cases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        pixel dst[16 * FDEC_STRIDE]{}, packed[256];
        dctcoef coefficients[16];
        for (int p = 0; p < 256; ++p)
            dst[(p / 16)*FDEC_STRIDE + p % 16] = static_cast<pixel>(input.pixels[p]);
        for (int p = 0; p < 16; ++p) coefficients[p] = static_cast<dctcoef>(input.coefficients[p]);
        if (input.side == 4) add4x4_idct_dc(dst, coefficients[0]);
        else if (input.side == 8) add8x8_idct_dc(dst, coefficients);
        else add16x16_idct_dc(dst, coefficients);
        for (int y = 0; y < 16; ++y) std::memcpy(packed + 16*y, dst + FDEC_STRIDE*y, 16);
        if (!matchesNativeBytes(output.pixels, packed) ||
            !matchesNativeBytes(output.coefficients, coefficients) || output.side != input.side)
            throw std::runtime_error("DC reconstruction mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " fixtures matched all three DC-only reconstruction functions byte for byte.\n";
}

void verifyCoeffAnalysis(const char* shader) {
    struct RunLevel {
        std::int32_t last, mask, total;
        std::array<std::int32_t, 18> level;
    };
    struct Fixture {
        std::array<std::int32_t, 64> coefficients, denoised;
        std::array<std::uint32_t, 64> residualSum, noiseOffset;
        std::int32_t denoiseSize;
        std::array<std::int32_t, 3> scores;
        std::array<std::int32_t, 5> last;
        std::array<RunLevel, 4> runLevel;
    };
    static_assert(sizeof(Fixture) == 349 * sizeof(std::uint32_t));

    std::mt19937 random(0x264ca);
    std::vector<Fixture> cases(4096);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        input.denoiseSize = i % 2 == 0 ? 16 : 64;
        for (std::size_t p = 0; p < input.coefficients.size(); ++p) {
            const int level = static_cast<int>(random() & 65535) - 32768;
            if (random() % 5 == 0)
                input.coefficients[p] = i % 4 == 0 ? level : p % 2 == 0 ? 1 : -1;
            input.denoised[p] = level;
            input.residualSum[p] = random();
            input.noiseOffset[p] = random() & 65535;
        }
        for (auto& run : input.runLevel) {
            run.last = run.mask = run.total = -42;
            run.level.fill(-31337);
        }
    }
    cases[0].coefficients.fill(0);
    for (int p = 0; p < 64; ++p) {
        auto& input = cases[1 + p];
        input.coefficients.fill(0);
        input.coefficients[p] = p % 2 ? -1 : 1;
    }
    cases[65].coefficients.fill(1);
    cases[66].coefficients.fill(-1);
    cases[67].coefficients.fill(0);
    cases[67].coefficients[0] = -2;
    cases[68].coefficients.fill(0);
    cases[68].coefficients[63] = 2;
    cases[0].denoised.fill(32767);
    cases[1].denoised.fill(-32768);
    cases[0].noiseOffset.fill(0);
    cases[1].noiseOffset.fill(1);
    cases[0].residualSum.fill(UINT32_MAX);
    cases[1].residualSum.fill(UINT32_MAX);

    const auto inputs = cases;
    vk264::dct::coeffAnalysis(shader, cases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        dctcoef coefficients[64], denoised[64];
        udctcoef offset[64];
        std::uint32_t sum[64];
        std::copy(input.coefficients.begin(), input.coefficients.end(), coefficients);
        std::copy(input.denoised.begin(), input.denoised.end(), denoised);
        std::copy(input.noiseOffset.begin(), input.noiseOffset.end(), offset);
        std::copy(input.residualSum.begin(), input.residualSum.end(), sum);
        denoise_dct(denoised, sum, offset, input.denoiseSize);
        const std::array<int, 3> scores{decimate_score15(coefficients), decimate_score16(coefficients),
                                        decimate_score64(coefficients)};
        const std::array<int, 5> last{coeff_last4(coefficients), coeff_last8(coefficients),
                                      coeff_last15(coefficients), coeff_last16(coefficients),
                                      coeff_last64(coefficients)};
        if (output.scores != scores || output.last != last ||
            !matchesNativeBytes(output.coefficients, coefficients) ||
            !matchesNativeBytes(output.denoised, denoised) ||
            !matchesNativeBytes(output.residualSum, sum) ||
            !matchesNativeBytes(output.noiseOffset, offset) || output.denoiseSize != input.denoiseSize)
            throw std::runtime_error("Coefficient analysis mismatch in fixture " + std::to_string(i));
        const std::array runFunctions{coeff_level_run4, coeff_level_run8, coeff_level_run15, coeff_level_run16};
        for (std::size_t p = 0; p < runFunctions.size(); ++p) {
            const auto& before = input.runLevel[p];
            const auto& actual = output.runLevel[p];
            x264_run_level_t reference{before.last, before.mask, {}};
            std::copy(before.level.begin(), before.level.end(), reference.level);
            const int total = last[p] >= 0 ? runFunctions[p](coefficients, &reference) : before.total;
            if (actual.total != total || actual.last != reference.last || actual.mask != reference.mask ||
                !matchesNativeBytes(actual.level, reference.level))
                throw std::runtime_error("CAVLC level/run mismatch in fixture " + std::to_string(i));
        }
    }
    std::cout << cases.size() << " fixtures matched 13 coefficient analysis functions byte for byte.\n";
}

void verifyChromaOptimize(const char* shader) {
    struct Fixture {
        std::array<std::int32_t, 8> coefficients;
        std::int32_t scale, chroma422, nonzero;
    };
    static_assert(sizeof(Fixture) == 11 * sizeof(std::uint32_t));
    std::mt19937 random(0x264c0);
    std::vector<Fixture> cases(4096);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        input.chroma422 = i % 2 != 0;
        input.scale = 1 + random() % 2048;
        for (auto& coefficient : input.coefficients)
            coefficient = static_cast<int>(random() % 129) - 64;
        if (i % 8 == 0 || i % 8 == 1) {
            input.scale = 1 + random() % 32;
            for (auto& coefficient : input.coefficients)
                coefficient = static_cast<int>(random() % 5) - 2;
        }
        if (i % 32 == 2 || i % 32 == 3) {
            for (auto& coefficient : input.coefficients)
                coefficient = static_cast<int>(random() & 65535) - 32768;
        }
    }
    for (int format = 0; format < 2; ++format) {
        cases[format].coefficients.fill(0);
        cases[2 + format].coefficients.fill(32767);
        cases[4 + format].coefficients.fill(-32768);
        cases[2 + format].scale = cases[4 + format].scale = 2048;
    }
    const auto inputs = cases;
    vk264::dct::chromaOptimize(shader, cases);
    std::array<int, 2> zero{}, changed{}, kept{};
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        dctcoef expected[8];
        std::copy(input.coefficients.begin(), input.coefficients.end(), expected);
        const int format = input.chroma422;
        const int nonzero = format ? optimize_chroma_2x4_dc(expected, input.scale)
                                   : optimize_chroma_2x2_dc(expected, input.scale);
        if (output.nonzero != nonzero || !matchesNativeBytes(output.coefficients, expected) ||
            output.chroma422 != format || output.scale != input.scale)
            throw std::runtime_error("Chroma DC optimization mismatch in fixture " + std::to_string(i));
        if (!nonzero) ++zero[format];
        else if (!std::equal(std::begin(expected), std::end(expected), input.coefficients.begin()))
            ++changed[format];
        else ++kept[format];
    }
    for (int format = 0; format < 2; ++format)
        if (!zero[format] || !changed[format] || !kept[format])
            throw std::runtime_error("Chroma optimization fixture coverage is incomplete");
    std::cout << cases.size() << " fixtures matched both chroma DC optimizers byte for byte.\n";
}

