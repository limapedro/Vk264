// Vk264 parity module: dispatch.inc
// Mirrors: test dispatcher
// Split from dct_parity.cpp lines 6430-6700.

int runParityTests(int argc, char** argv) try {
    if (argc == 10 && (std::string_view(argv[1]) == "encode_bank" ||
                       std::string_view(argv[1]) == "record_bank")) {
        // Encodes a dumped residual bank with the direct GPU coder so the coder
        // and the parallel bin generator can be compared on identical input.
        const bool record = std::string_view(argv[1]) == "record_bank";
        std::ifstream in(argv[2], std::ios::binary);
        std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
        std::vector<std::uint32_t> words(raw.size()/4);
        std::memcpy(words.data(), raw.data(), words.size()*4);
        std::vector<std::int32_t> bankWords(words.size());
        std::transform(words.begin(), words.end(), bankWords.begin(),
                       [](std::uint32_t w) { return static_cast<std::int32_t>(w); });
        const int qp = std::stoi(argv[4]), flags = std::stoi(argv[5]), frame = std::stoi(argv[6]);
        const int headerByte = std::stoi(argv[7]);
        const int columns = std::stoi(argv[8]);
        const int inter = flags & 1, chroma444 = (flags >> 1) & 1;
        const int macroblocks = int(bankWords.size())/cabacCorpus::macroblockWords;
        std::vector<cabacCorpus::FrameFixture> body(1);
        auto& slice = body.front();
        slice.qp = qp;
        slice.model = inter;
        slice.chroma444 = chroma444;
        slice.frame = frame;
        slice.finish = 1;
        slice.bytes[0] = static_cast<std::uint32_t>(headerByte);
        // Shader-only op kind 15 drives every macroblock from the residual bank.
        slice.operations[slice.count++] = {15, inter, (columns << 16) | macroblocks};
        using Slice = cabacCorpus::FrameFixture;
        execute(argv[3], body, 1, cabacTables(bankWords, cabacCorpus::frameResidualBankSize), 0,
                {{offsetof(Slice, bytes), sizeof(Slice::bytes)}, {offsetof(Slice, position), sizeof(Slice::position)},
                 {offsetof(Slice, mvdMagnitude), sizeof(Slice::mvdMagnitude)}},
                0, true);
        std::ofstream out(argv[9], std::ios::binary);
        for (std::uint32_t i = 0; i < slice.position; ++i)
            out.put(static_cast<char>(slice.bytes[i] & 0xffu));
        if (record) {
            std::ofstream bins(std::string(argv[9])+".bins", std::ios::binary);
            bins.write(reinterpret_cast<const char*>(&slice.mvdMagnitude), sizeof(slice.mvdMagnitude));
            for (std::int32_t i = 0; i < slice.mvdMagnitude; ++i) {
                const std::uint32_t bin = slice.bytes[400+i];
                bins.write(reinterpret_cast<const char*>(&bin), sizeof(bin));
            }
            std::cout << "recorded bins " << slice.mvdMagnitude << "\n";
        }
        std::cout << "gpu bytes " << slice.position << "\n";
        if (slice.mvdMagnitude > 0) {
            std::ofstream writes(std::string(argv[9])+".writes", std::ios::binary);
            writes.write(reinterpret_cast<const char*>(&slice.bytes[2000000]), std::streamsize(60000));
            std::ofstream trace(std::string(argv[9])+".trace", std::ios::binary);
            for (std::int32_t i = 0; i < slice.mvdMagnitude; ++i)
                for (std::int32_t w = 0; w < 5; ++w) {
                    const std::uint32_t word = slice.bytes[1000000 + std::size_t(5*i) + w];
                    trace.write(reinterpret_cast<const char*>(&word), sizeof(word));
                }
            std::cout << "trace entries " << slice.mvdMagnitude << "\n";
            std::ofstream full(std::string(argv[9])+".full", std::ios::binary);
            full.write(reinterpret_cast<const char*>(slice.bytes.data()), std::streamsize(slice.bytes.size()*sizeof(std::uint32_t)));
        }
        return 0;
    }
    if (argc == 9 && std::string_view(argv[1]) == "replay_gpu") {
        // Replays a dumped bin stream through the GPU arithmetic coder so the
        // direct shader coder and the host coder can be compared bin for bin.
        std::ifstream in(argv[3], std::ios::binary);
        std::uint32_t count = 0;
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        std::vector<std::uint32_t> bins(count);
        in.read(reinterpret_cast<char*>(bins.data()), std::streamsize(count*sizeof(std::uint32_t)));
        const int qp = std::stoi(argv[4]), inter = std::stoi(argv[5]), frame = std::stoi(argv[6]);
        const int headerByte = std::stoi(argv[7]);
        std::vector<cabacCorpus::FrameFixture> body(1);
        auto& slice = body.front();
        slice.qp = qp;
        slice.model = inter;
        slice.frame = frame;
        slice.finish = 1;
        slice.bytes[0] = static_cast<std::uint32_t>(headerByte);
        for (const auto bin : bins) {
            if ((bin & 0x80000000u) == 0) {
                if ((bin & 0x40000000u) == 0)
                    slice.operations[slice.count++] = {cabacCorpus::decision, int(bin >> 1), int(bin & 1)};
                else
                    slice.operations[slice.count++] = {cabacCorpus::bypass, int(bin & 1), 0};
            } else if ((bin & 0x40000000u) == 0) {
                slice.operations[slice.count++] = {cabacCorpus::ueBypass, int((bin >> 28) & 3), int(bin & 0x0fffffffu)};
            } else if ((bin & 0x20000000u) == 0) {
                slice.operations[slice.count++] = {cabacCorpus::terminal, 0, 0};
            }
        }
        using Slice = cabacCorpus::FrameFixture;
        execute(argv[2], body, 1, cabacTables({}, cabacCorpus::frameResidualBankSize), 0,
                {{offsetof(Slice, bytes), sizeof(Slice::bytes)}, {offsetof(Slice, position), sizeof(Slice::position)},
                 {offsetof(Slice, mvdMagnitude), sizeof(Slice::mvdMagnitude)}},
                0, true);
        std::ofstream out(argv[8], std::ios::binary);
        for (std::uint32_t i = 0; i < slice.position; ++i)
            out.put(static_cast<char>(slice.bytes[i] & 0xffu));
        std::cout << "gpu bytes " << slice.position << "\n";
        if (slice.mvdMagnitude > 0) {
            std::ofstream writes(std::string(argv[8])+".writes", std::ios::binary);
            writes.write(reinterpret_cast<const char*>(&slice.bytes[2000000]), std::streamsize(60000));
            std::ofstream trace(std::string(argv[8])+".trace", std::ios::binary);
            for (std::int32_t i = 0; i < slice.mvdMagnitude; ++i)
                for (std::int32_t w = 0; w < 5; ++w) {
                    const std::uint32_t word = slice.bytes[1000000 + std::size_t(5*i) + w];
                    trace.write(reinterpret_cast<const char*>(&word), sizeof(word));
                }
            std::cout << "trace entries " << slice.mvdMagnitude << "\n";
            std::ofstream full(std::string(argv[8])+".full", std::ios::binary);
            full.write(reinterpret_cast<const char*>(slice.bytes.data()), std::streamsize(slice.bytes.size()*sizeof(std::uint32_t)));
        }
        return 0;
    }
    if (argc == 8 && std::string_view(argv[1]) == "replay_trace") {
        std::ifstream in(argv[2], std::ios::binary);
        std::uint32_t count = 0;
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        std::vector<std::uint32_t> bins(count);
        in.read(reinterpret_cast<char*>(bins.data()), std::streamsize(count*sizeof(std::uint32_t)));
        std::ifstream exp(argv[3], std::ios::binary);
        std::vector<std::uint8_t> expected((std::istreambuf_iterator<char>(exp)), std::istreambuf_iterator<char>());
        const int qp = std::stoi(argv[4]), inter = std::stoi(argv[5]), frame = std::stoi(argv[6]);
        const int headerByte = std::stoi(argv[7]);
        std::size_t previous = 0;
        for (std::size_t n = 1; n <= bins.size(); ++n) {
            const auto slice = cabacEncodeBins(bins.data(), n, qp, inter != 0, frame,
                                               static_cast<std::uint8_t>(headerByte));
            std::size_t common = 0;
            while (common < slice.size() && common < expected.size() && slice[common] == expected[common]) ++common;
            if (common != previous || (slice.size() >= 9 && n >= 20 && n <= 80)) {
                std::cout << "n=" << n << " pos=" << slice.size() << " common=" << common << " bytes=";
                for (std::size_t k = 0; k < slice.size() && k < 12; ++k)
                    std::cout << std::hex << int(slice[k]) << " ";
                std::cout << std::dec << "\n";
            }
            previous = common;
        }
        return 0;
    }
    if (argc == 8 && std::string_view(argv[1]) == "replay_search") {
        std::ifstream in(argv[2], std::ios::binary);
        std::uint32_t count = 0;
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        std::vector<std::uint32_t> bins(count);
        in.read(reinterpret_cast<char*>(bins.data()), std::streamsize(count*sizeof(std::uint32_t)));
        std::ifstream exp(argv[3], std::ios::binary);
        std::vector<std::uint8_t> expected((std::istreambuf_iterator<char>(exp)), std::istreambuf_iterator<char>());
        const int qp = std::stoi(argv[4]), inter = std::stoi(argv[5]), frame = std::stoi(argv[6]);
        const int headerByte = std::stoi(argv[7]);
        const auto encode = [&](const std::vector<std::uint32_t>& stream) {
            return cabacEncodeBins(stream.data(), stream.size(), qp, inter != 0, frame,
                                   static_cast<std::uint8_t>(headerByte));
        };
        if (encode(bins) == expected) { std::cout << "base matches\n"; return 0; }
        for (std::size_t i = 0; i < bins.size(); ++i) {
            std::vector<std::uint32_t> mutated = bins;
            const auto bin = bins[i];
            if ((bin & 0x80000000u) == 0) {
                if ((bin & 0x40000000u) == 0) mutated[i] = (bin & ~1u) | ((bin ^ 1u) & 1u);
                else mutated[i] = (bin & ~1u) | ((bin ^ 1u) & 1u);
            } else if ((bin & 0x40000000u) == 0) {
                const std::uint32_t value = bin & 0x0fffffffu;
                if (value > 0) mutated[i] = (bin & 0xf0000000u) | (value-1);
            }
            if (encode(mutated) == expected) {
                std::cout << "flip at " << i << " bin " << std::hex << bin << std::dec << "\n";
            }
            mutated = bins;
            mutated.erase(mutated.begin()+i);
            if (encode(mutated) == expected) {
                std::cout << "delete at " << i << " bin " << std::hex << bin << std::dec << "\n";
            }
            if ((bin & 0xc0000000u) == 0x80000000u) {
                for (std::uint32_t value = 0; value < 4096; ++value) {
                    if (value == (bin & 0x0fffffffu)) continue;
                    mutated = bins;
                    mutated[i] = (bin & 0xf0000000u) | value;
                    if (encode(mutated) == expected)
                        std::cout << "ue at " << i << " value " << value << " (was "
                                  << (bin & 0x0fffffffu) << ")\n";
                }
            }
        }
        std::cout << "search done\n";
        return 0;
    }
    if (argc == 8 && std::string_view(argv[1]) == "replay_cpu") {
        std::ifstream in(argv[2], std::ios::binary);
        std::uint32_t count = 0;
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        std::vector<std::uint32_t> bins(count);
        in.read(reinterpret_cast<char*>(bins.data()), std::streamsize(count*sizeof(std::uint32_t)));
        auto slice = cabacEncodeBins(bins.data(), count, std::stoi(argv[3]), std::stoi(argv[4]) != 0,
                                     std::stoi(argv[5]), static_cast<std::uint8_t>(std::stoi(argv[6])));
        std::ofstream out(argv[7], std::ios::binary);
        out.write(reinterpret_cast<const char*>(slice.data()), std::streamsize(slice.size()));
        std::cout << "cpu bytes " << slice.size() << "\n";
        return 0;
    }
    if (argc == 13 && std::string_view(argv[1]) == "cabac_smoke") {
        encodeCabacClip(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9],
                        argv[10], argv[11], argv[12]);
        return 0;
    }
    if (argc == 22 && std::string_view(argv[1]) == "cabac_raw") {
        const RawClip clip{argv[13], std::stoi(argv[14]), std::stoi(argv[15]), std::stoi(argv[16]),
                           std::stoi(argv[17]), std::stoi(argv[18]), std::stoi(argv[19]),
                           std::stoi(argv[20]), std::stoi(argv[21])};
        const auto result = encodeCabacClip(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9],
                                            argv[10], argv[11], argv[12], &clip);
        std::cout << "Coded " << result.inter8x8Blocks << " inter macroblocks with 8x8 luma transforms.\n";
        return 0;
    }
    if (argc != 3) throw std::runtime_error("Usage: dct_parity <test name> <shader.spv>");
    const std::string_view side = argv[1];
    if (side == "4x4") verify<4, 4>(argv[2]);
    else if (side == "4x8") verify<4, 8>(argv[2]);
    else if (side == "4x16") verify<4, 16>(argv[2]);
    else if (side == "8x8") verify<8, 8>(argv[2]);
    else if (side == "8x16") verify<8, 16>(argv[2]);
    else if (side == "quant4") verifyQuant<4>(argv[2]);
    else if (side == "quant8") verifyQuant<8>(argv[2]);
    else if (side == "quant_group") verifyQuantGroup(argv[2]);
    else if (side == "dc") verifyDc(argv[2]);
    else if (side == "zigzag") verifyZigzag(argv[2]);
    else if (side == "chroma_dc") verifyChromaDc(argv[2]);
    else if (side == "reconstruct_dc") verifyReconstructDc(argv[2]);
    else if (side == "coeff_analysis") verifyCoeffAnalysis(argv[2]);
    else if (side == "chroma_optimize") verifyChromaOptimize(argv[2]);
    else if (side == "predict_intra") verifyPredictIntra(argv[2]);
    else if (side == "predict_directional") verifyPredictDirectional(argv[2]);
    else if (side == "i16_mode") verifyIntra16Mode();
    else if (side == "i4x4_decision") verifyI4x4Decision();
    else if (side == "i4x4_cabac") encodeI4x4Frame();
    else if (side == "deblock_filter") verifyDeblockFilter(argv[2]);
    else if (side == "deblock_frame") verifyDeblockFrame(argv[2]);
    else if (side == "deblock_frame_tiled") verifyDeblockFrame(argv[2], true);
    else if (side == "deblock_frame_subgroup") {
        if (!computeContext().deblockSubgroupVote) return 77;
        verifyDeblockFrame(argv[2], true);
    }
    else if (side == "deblock_strength") verifyDeblockStrength(argv[2]);
    else if (side == "cabac") verifyCabac(argv[2]);
    else if (side == "cabac_refidx") verifyCabacRefIdx(argv[2]);
    else if (side == "cavlc_residual") verifyCavlcResidual(argv[2]);
    else if (side == "mv_predict") verifyMvPrediction(argv[2]);
    else if (side == "motion_compensation") verifyMotionCompensation(argv[2]);
    else if (side == "reference_frame") verifyReferenceFrame(argv[2]);
    else if (side == "motion_metrics") verifyMotionMetrics(argv[2]);
    else if (side == "motion_elimination") verifyMotionElimination(argv[2]);
    else if (side == "motion_integral") verifyMotionIntegral(argv[2]);
    else if (side == "motion_lowres") verifyMotionLowres(argv[2]);
    else if (side == "motion_search") verifyMotionSearch(argv[2], false);
    else if (side == "motion_exhaustive") verifyMotionSearch(argv[2], true);
    else if (side == "motion_bidir") verifyMotionBidir(argv[2]);
    else if (side == "bitstream") verifyBitstream(argv[2]);
    else if (side == "nal") verifyNal(argv[2]);
    else if (side == "parameters") verifyParameters(argv[2]);
    else if (side == "slice_header") verifySliceHeader(argv[2]);
    else if (side == "intra16") verifyLuma4x4(argv[2], false);
    else if (side == "inter4x4") verifyLuma4x4(argv[2], true);
    else if (side == "inter8x8") verifyInter8x8(argv[2]);
    else if (side == "aq") verifyAqVariance(argv[2]);
    else if (side == "intra_chroma") verifyIntraChroma(argv[2]);
    else throw std::runtime_error("Unknown kernel test");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
