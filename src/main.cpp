#include "presets.hpp"
#include "encoder_backend.hpp"
#include "cavlc_codes.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

void listPresets() {
    std::cout << "{\"default\":\"" << defaultPreset.name << "\",\"presets\":[";
    const char* delimiter = "";
    for (const auto& preset : presets) {
        std::cout << delimiter << "{\"name\":\"" << preset.name << "\",\"search_radius\":" << preset.motionRadius
                  << ",\"satd_candidates\":" << preset.satdCandidates
                  << ",\"reference_frames\":" << preset.references << "}";
        delimiter = ",";
    }
    std::cout << "]}\n";
}

double seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::uint32_t number(std::string_view s, std::uint32_t low, std::uint32_t high, const char* name) {
    std::uint32_t n = 0;
    auto parsed = std::from_chars(s.data(), s.data() + s.size(), n);
    if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size() || n < low || n > high)
        throw std::runtime_error(std::string("Invalid ") + name + " (expected " + std::to_string(low) + "–" + std::to_string(high) + ")");
    return n;
}

bool samePath(const fs::path& a, const fs::path& b) {
    auto first = fs::weakly_canonical(a).native();
    auto second = fs::weakly_canonical(b).native();
#ifdef _WIN32
    if (_wcsicmp(first.c_str(), second.c_str()) == 0) return true;
#else
    if (first == second) return true;
#endif
    return fs::exists(a) && fs::exists(b) && fs::equivalent(a, b);
}

void usage() {
    std::cerr << "Usage: wx264 input.yuv output.h264 width height frames fps [--preset fast|medium|slow|veryslow]\n"
              << "             [--qp N] [--bitrate KBPS] [--satd-candidates N] [--ref N]\n"
              << "             [--gop N] [--search N] [--subpel 0|1] [--partitions 0|1] [--deblock 0|1|2]\n"
              << "             [--p-slice-rows 1|2] [--flat-intra 0|1] [--aq 0|1] [--recon padded.yuv]\n"
              << "             [--colorimetry bt709|smpte170m|bt470bg|bt2020-sdr] [--range limited|full]\n"
              << "             [--backend cavlc|vulkan] (vulkan uses CABAC and its own search configuration)\n"
              << "             [--entropy cabac|cavlc] (Vulkan backend only)\n"
              << "       wx264 --list-presets\n";
}
}

int main(int argc, char** argv) try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") { usage(); return 0; }
    if (argc == 2 && std::string_view(argv[1]) == "--list-presets") { listPresets(); return 0; }
    if (argc < 7 || (argc - 7) % 2 != 0) { usage(); return 1; }

    Preset preset = defaultPreset;
    bool useVulkanPort = false;
    bool useCavlc = false;
    for (int i = 7; i < argc; i += 2) {
        std::string_view option = argv[i];
        if (option == "--preset") preset = findPreset(argv[i + 1]);
        if (option == "--backend") {
            std::string_view backend = argv[i + 1];
            if (backend != "cavlc" && backend != "vulkan") throw std::runtime_error("Unknown encoder backend");
            useVulkanPort = backend == "vulkan";
        }
    }
    auto width = number(argv[3], 2, maxFrameDimension, "width");
    auto height = number(argv[4], 2, maxFrameDimension, "height");
    auto frameCount = number(argv[5], 1, 1000000, "frame count");
    std::string_view frameRate = argv[6];
    auto slash = frameRate.find('/');
    auto numerator = number(frameRate.substr(0, slash), 1, 0xffffffffu, "frame-rate numerator");
    auto denominator = slash == std::string_view::npos ? 1u :
        number(frameRate.substr(slash + 1), 1, 0xffffffffu, "frame-rate denominator");
    auto config = makeFrameConfig(width, height, numerator, denominator, preset);
    fs::path reconPath;
    for (int i = 7; i < argc; i += 2) {
        std::string_view option = argv[i];
        if (useVulkanPort && (option == "--preset" || option == "--search" || option == "--satd-candidates" ||
            option == "--ref" || option == "--subpel" || option == "--partitions" || option == "--deblock" ||
            option == "--flat-intra" || option == "--aq" || option == "--p-slice-rows"))
            throw std::runtime_error("The Vulkan port does not support " + std::string(option));
        if (option == "--preset" || option == "--backend") continue;
        if (option == "--entropy") {
            const std::string_view entropy = argv[i + 1];
            if (!useVulkanPort || (entropy != "cabac" && entropy != "cavlc"))
                throw std::runtime_error("--entropy requires the Vulkan backend and cabac or cavlc");
            useCavlc = entropy == "cavlc";
            continue;
        }
        if (option == "--qp") config.initialQp = number(argv[i + 1], 0, 51, "QP");
        else if (option == "--gop") config.gop = number(argv[i + 1], 1, 65536, "GOP length");
        else if (option == "--search") config.motionRadius = number(argv[i + 1], 0, 32, "motion search radius");
        else if (option == "--satd-candidates") config.satdCandidates = number(argv[i + 1], 0, 8, "SATD candidate count");
        else if (option == "--ref") config.references = number(argv[i + 1], 1, 16, "reference picture count");
        else if (option == "--subpel") config.subpel = number(argv[i + 1], 0, 1, "subpixel refinement flag");
        else if (option == "--partitions") config.partitions = number(argv[i + 1], 0, 1, "8x8 partition flag");
        else if (option == "--deblock") config.deblock = number(argv[i + 1], 0, 2, "deblocking mode");
        else if (option == "--flat-intra") config.flatIntra = number(argv[i + 1], 0, 1, "flat intra flag");
        else if (option == "--aq") config.aq = number(argv[i + 1], 0, 1, "adaptive quantization flag");
        else if (option == "--p-slice-rows") config.sliceRows = number(argv[i + 1], 1, 2, "P-slice row count");
        else if (option == "--bitrate") config.targetBits = bitsPerFrame(config, number(argv[i + 1], 1, 50000, "bitrate"));
        else if (option == "--colorimetry") {
            std::string_view value = argv[i + 1];
            std::uint32_t color;
            if (value == "bt709") color = colorSignal(1, 1, 1);
            else if (value == "smpte170m") color = colorSignal(6, 6, 6);
            else if (value == "bt470bg") color = colorSignal(5, 5, 5);
            else if (value == "bt2020-sdr") color = colorSignal(9, 14, 9);
            else throw std::runtime_error("Unknown colorimetry");
            config.videoSignal = color | (config.videoSignal & 1u);
        } else if (option == "--range") {
            std::string_view value = argv[i + 1];
            if (value != "limited" && value != "full")
                throw std::runtime_error("Range must be limited or full");
            if (!config.videoSignal) config.videoSignal = colorSignal(2, 2, 2);
            config.videoSignal = (config.videoSignal & ~1u) | std::uint32_t(value == "full");
        }
        else if (option == "--recon") reconPath = argv[i + 1];
        else throw std::runtime_error("Unknown option " + std::string(option));
    }
    auto requestedReferences = config.references;
    if (config.sliceRows != 2 || config.partitions) {
        config.references = 1;
        config.flatIntra = 0;
        config.aq = 0;
    }
    if (useVulkanPort) config.references = 1;
    finalizeFrameConfig(config);
    fs::path inputPath = argv[1], outputPath = argv[2];
    if (samePath(inputPath, outputPath) || (!reconPath.empty() &&
        (samePath(inputPath, reconPath) || samePath(outputPath, reconPath))))
        throw std::runtime_error("Input, output and reconstruction must be distinct files");

    std::uint32_t frameBytes = config.width * config.height * 3 / 2;
    const std::uint32_t reconBytes = config.mbWidth * config.mbHeight * 384;
    if (fs::file_size(inputPath) < std::uint64_t(frameCount) * frameBytes)
        throw std::runtime_error("Input is shorter than the requested frame count");
    if (useVulkanPort) {
        const auto recon = reconPath.string();
        const auto result = x264_vulkan::encode({
            argv[1], argv[2], recon.empty() ? nullptr : recon.c_str(), int(width), int(height), int(frameCount),
            int(config.fpsNumerator), int(config.fpsDenominator), int(config.gop), int(config.initialQp),
            int(config.targetBits), config.videoSignal, int(config.levelIdc), useCavlc ? cavlcCodes : nullptr,
        });
        std::cout << std::fixed << std::setprecision(4)
                  << "{\"backend\":\"x264-vulkan\",\"preset\":\"prototype\",\"gpu\":"
                  << std::quoted(result.deviceName) << ",\"settings\":{"
                  << "\"entropy\":\"" << (useCavlc ? "cavlc" : "cabac")
                  << "\",\"gop\":" << config.gop << ",\"reference_frames\":1"
                  << ",\"b_frames\":0,\"lookahead_frames\":0,\"slices_per_frame\":1,\"inter_8x8_transform\":1"
                  << ",\"rate_control\":\"" << (config.targetBits ? "average-rate" : "fixed-qp") << "\"}"
                  << ",\"frames\":" << frameCount << ",\"bytes\":" << result.bytes
                  << ",\"encode_seconds\":" << result.seconds << ",\"min_qp\":" << result.minQp
                  << ",\"max_qp\":" << result.maxQp << ",\"inter_8x8_blocks\":" << result.inter8x8Blocks << "}\n";
        return 0;
    }
    std::ifstream input(inputPath, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open input");
    auto init = Clock::now();
    EncoderGpu gpu(config, !reconPath.empty());
    double initSeconds = seconds(init);
    std::ofstream output(outputPath, std::ios::binary);
    if (!output) throw std::runtime_error("Could not open H.264 output");
    std::ofstream recon;
    if (!reconPath.empty()) {
        recon.open(reconPath, std::ios::binary);
        if (!recon) throw std::runtime_error("Could not open reconstruction output");
    }

    std::uint64_t bytes = 0, olderReferenceBlocks = 0, flatIntraBlocks = 0;
    std::array<double, 13> stageMs{};
    std::vector<double> frameMs;
    frameMs.reserve(frameCount);
    unsigned minQp = 51, maxQp = 0, selectedReferenceDepth = 0;
    auto loop = Clock::now();
    for (config.frameIndex = 0; config.frameIndex < frameCount; ++config.frameIndex) {
        auto start = Clock::now();
        if (!input.read(gpu.input(), frameBytes)) throw std::runtime_error("Could not read source frame");
        auto frame = gpu.encode(config);
        if (!output.write(frame.annexB, frame.bytes)) throw std::runtime_error("Could not write H.264 frame");
        if (frame.reconstruction && !recon.write(frame.reconstruction, reconBytes))
            throw std::runtime_error("Could not write reconstruction frame");
        bytes += frame.bytes;
        olderReferenceBlocks += frame.olderReferenceBlocks;
        flatIntraBlocks += frame.flatIntraBlocks;
        selectedReferenceDepth = std::max(selectedReferenceDepth, frame.selectedReferenceDepth);
        minQp = std::min(minQp, frame.qp);
        maxQp = std::max(maxQp, frame.qp);
        for (std::size_t i = 0; i < stageMs.size(); ++i) stageMs[i] += frame.gpuMilliseconds[i];
        frameMs.push_back(seconds(start) * 1000);
    }
    output.close();
    if (recon.is_open()) recon.close();
    if (!output || !recon) throw std::runtime_error("Could not finalize output files");
    double elapsed = seconds(loop);
    std::sort(frameMs.begin(), frameMs.end());
    bool spatial = config.gop != 1 && !config.partitions && config.subpel && config.motionRadius >= 4;
    std::cout << std::fixed << std::setprecision(4)
              << "{\"gpu\":" << std::quoted(gpu.deviceName())
              << ",\"preset\":\"" << preset.name << "\""
              << ",\"settings\":{\"search_radius\":" << config.motionRadius
              << ",\"quarter_pel\":" << bool(config.subpel && config.motionRadius && config.gop != 1)
              << ",\"spatial_search\":" << spatial
              << ",\"satd_candidates\":" << (spatial ? config.satdCandidates : 0)
              << ",\"inter_8x8\":" << bool(config.partitions && config.motionRadius && config.gop != 1)
              << ",\"deblock\":" << bool(config.deblock)
              << ",\"deblock_mode\":" << config.deblock
              << ",\"flat_intra\":" << config.flatIntra
              << ",\"p_slice_rows\":" << (config.partitions ? 1 : config.sliceRows)
              << ",\"requested_reference_frames\":" << requestedReferences
              << ",\"reference_frames\":" << config.references << ",\"level_idc\":" << config.levelIdc
              << ",\"b_frames\":0,\"lookahead_frames\":0"
              << ",\"entropy\":\"cavlc\",\"aq\":" << config.aq << ",\"trellis\":0,\"vbv\":0"
              << ",\"gop\":" << config.gop << ",\"initial_qp\":" << config.initialQp
              << ",\"rate_control\":\"" << (config.targetBits ? "average-rate" : "fixed-qp") << "\"}"
              << ",\"frames\":" << frameCount << ",\"bytes\":" << bytes
              << ",\"older_reference_macroblocks\":" << olderReferenceBlocks
              << ",\"flat_intra_macroblocks\":" << flatIntraBlocks
              << ",\"selected_reference_depth\":" << selectedReferenceDepth
              << ",\"achieved_kbps\":" << double(bytes) * 8 * config.fpsNumerator /
                      (1000.0 * frameCount * config.fpsDenominator)
              << ",\"initialization_seconds\":" << initSeconds << ",\"encode_loop_seconds\":" << elapsed
              << ",\"encode_loop_fps\":" << frameCount / elapsed
              << ",\"frame_p95_ms\":" << frameMs[(frameCount * 95u + 99u) / 100u - 1u]
              << ",\"min_qp\":" << minQp << ",\"max_qp\":" << maxQp
              << ",\"gpu_stages_mean_ms\":{\"pyramid\":" << stageMs[0] / frameCount
              << ",\"halfpel\":" << stageMs[1] / frameCount << ",\"motion\":" << stageMs[2] / frameCount
              << ",\"spatial\":" << stageMs[3] / frameCount << ",\"older_references\":" << stageMs[4] / frameCount
              << ",\"residual\":" << stageMs[5] / frameCount << ",\"flat_intra\":" << stageMs[6] / frameCount
              << ",\"mb_flags\":" << stageMs[7] / frameCount << ",\"deblock\":" << stageMs[8] / frameCount
              << ",\"mb_entropy\":" << stageMs[9] / frameCount << ",\"entropy\":" << stageMs[10] / frameCount
              << ",\"scan\":" << stageMs[11] / frameCount << ",\"assemble\":" << stageMs[12] / frameCount << "}}\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "wx264: " << error.what() << '\n';
    return 1;
}
