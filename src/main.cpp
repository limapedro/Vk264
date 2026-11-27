#include "presets.hpp"
#include "encoder_backend.hpp"
#include "cavlc_codes.hpp"
#include "profile.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <thread>
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

double decimal(std::string_view s, double low, double high, const char* name) {
    double value = 0;
    auto parsed = std::from_chars(s.data(), s.data() + s.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size() ||
        !std::isfinite(value) || value < low || value > high)
        throw std::runtime_error(std::string("Invalid ") + name);
    return value;
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
 << "             [--smooth-abr 0|1] (opt-in smoothed rate controller)\n"
 << "             [--twopass 1] (opt-in offline two-pass ABR; requires --bitrate)\n"
 << "             [--twopass-alpha X] [--twopass-slope X] [--twopass-smooth N] [--twopass-idr-boost X] [--twopass-dither 0|1]\n"
              << "             [--p-slice-rows 1|2] [--flat-intra 0|1] [--aq 0|1] [--recon padded.yuv]\n"
              << "             [--colorimetry bt709|smpte170m|bt470bg|bt2020-sdr] [--range limited|full]\n"
              << "             [--backend cavlc|vulkan] (vulkan uses CABAC and its own search configuration)\n"
              << "             [--entropy cabac|cavlc] (Vulkan backend only)\n"
              << "             [--timestamps] (enable GPU timestamp queries; off by default)\n"
               << "       wx264 --list-presets\n";
}
}

int main(int argc, char** argv) try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") { usage(); return 0; }
    if (argc == 2 && std::string_view(argv[1]) == "--list-presets") { listPresets(); return 0; }
    bool useTimestamps = false;
    {
        int out = 7;
        for (int i = 7; i < argc; ++i)
            if (std::string_view(argv[i]) == "--timestamps") useTimestamps = true;
            else argv[out++] = argv[i];
        argc = out;
    }
    if (argc < 7 || (argc - 7) % 2 != 0) { usage(); return 1; }

    Preset preset = defaultPreset;
    bool useVulkanPort = false;
    bool useCavlc = false;
    bool qpExplicit = false;
    bool smoothAbr = false;
    bool twoPass = false;
    double twoPassAlpha = 1.0, twoPassIdrBoost = 1.0, twoPassSlope = 6.0;
    std::uint32_t twoPassSmooth = 0, twoPassDither = 1;
    for (int i = 7; i < argc; i += 2) {
        std::string_view option = argv[i];
        if (option == "--timestamps") continue;
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
            option == "--flat-intra" || option == "--aq" || option == "--p-slice-rows" ||
            option == "--smooth-abr" || option == "--twopass" || option == "--twopass-alpha" ||
            option == "--twopass-slope" || option == "--twopass-smooth" || option == "--twopass-idr-boost" ||
            option == "--twopass-dither"))
            throw std::runtime_error("The Vulkan port does not support " + std::string(option));
        if (option == "--preset" || option == "--backend") continue;
        if (option == "--entropy") {
            const std::string_view entropy = argv[i + 1];
            if (!useVulkanPort || (entropy != "cabac" && entropy != "cavlc"))
                throw std::runtime_error("--entropy requires the Vulkan backend and cabac or cavlc");
            useCavlc = entropy == "cavlc";
            continue;
        }
        if (option == "--qp") {
            config.initialQp = number(argv[i + 1], 0, 51, "QP");
            qpExplicit = true;
        }
        else if (option == "--gop") config.gop = number(argv[i + 1], 1, 65536, "GOP length");
        else if (option == "--search") config.motionRadius = number(argv[i + 1], 0, 32, "motion search radius");
        else if (option == "--satd-candidates") config.satdCandidates = number(argv[i + 1], 0, 8, "SATD candidate count");
        else if (option == "--ref") config.references = number(argv[i + 1], 1, 16, "reference picture count");
        else if (option == "--subpel") config.subpel = number(argv[i + 1], 0, 1, "subpixel refinement flag");
        else if (option == "--partitions") config.partitions = number(argv[i + 1], 0, 1, "8x8 partition flag");
        else if (option == "--deblock") config.deblock = number(argv[i + 1], 0, 2, "deblocking mode");
        else if (option == "--flat-intra") config.flatIntra = number(argv[i + 1], 0, 1, "flat intra flag");
        else if (option == "--aq") config.aq = number(argv[i + 1], 0, 1, "adaptive quantization flag");
        else if (option == "--smooth-abr") smoothAbr = number(argv[i + 1], 0, 1, "smoothed ABR flag") != 0u;
        else if (option == "--twopass") twoPass = number(argv[i + 1], 0, 1, "two-pass flag") != 0u;
        else if (option == "--twopass-alpha") twoPassAlpha = decimal(argv[i + 1], 0.3, 1.0, "two-pass qcomp exponent");
        else if (option == "--twopass-slope") twoPassSlope = decimal(argv[i + 1], 4.0, 12.0, "two-pass QP per bit doubling");
        else if (option == "--twopass-smooth") twoPassSmooth = number(argv[i + 1], 0, 12, "two-pass QP smoothing step");
        else if (option == "--twopass-dither") twoPassDither = number(argv[i + 1], 0, 1, "two-pass QP dither flag");
        else if (option == "--twopass-idr-boost") twoPassIdrBoost = decimal(argv[i + 1], 1.0, 2.0, "two-pass IDR boost");
        else if (option == "--p-slice-rows") config.sliceRows = number(argv[i + 1], 1, 4, "P-slice row count");
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
    config.smoothAbr = smoothAbr ? 1u : 0u;
    if (twoPass && config.targetBits == 0u)
        throw std::runtime_error("--twopass requires --bitrate");
    if (twoPass && smoothAbr)
        throw std::runtime_error("--twopass and --smooth-abr are mutually exclusive");
    if (twoPass && useVulkanPort)
        throw std::runtime_error("--twopass is only available on the native CAVLC backend");
    auto requestedReferences = config.references;
    if (config.sliceRows < 2 || config.partitions) {
        config.references = 1;
        config.flatIntra = 0;
        config.aq = 0;
    }
    if (useVulkanPort) config.references = 1;
    // In average-rate mode a high --qp is only a STARTING point: the shader
    // rate controller adjusts at most +/-2 QP per frame, so starting at 34
    // leaves the first IDR (and every keyframe) far too coarse and permanently
    // lowers mean quality at the same achieved bitrate. Measured 640x360/60f
    // at 6 Mbps: initial 34 -> 14 raises PSNR-Y 51.28 -> 52.38 dB on one clip
    // and 55.10 -> 56.34 dB on another, both at ~6.2 Mb/s. Clamp only when
    // --bitrate was requested; fixed-QP mode keeps the caller's exact value.
    if (qpExplicit && config.targetBits != 0u && !smoothAbr && config.initialQp > 14)
        config.initialQp = 14;
    finalizeFrameConfig(config);
    fs::path inputPath = argv[1], outputPath = argv[2];
    fs::path probePath = twoPass ? fs::path(outputPath.string() + ".probe34.h264") : fs::path{};
    if (samePath(inputPath, outputPath) || (!reconPath.empty() &&
        (samePath(inputPath, reconPath) || samePath(outputPath, reconPath))) ||
        (!probePath.empty() && (samePath(probePath, inputPath) || samePath(probePath, outputPath) ||
                                (!reconPath.empty() && samePath(probePath, reconPath)))))
        throw std::runtime_error("Input, output, probe and reconstruction must be distinct files");

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
    // Read the whole source once: per-frame ifstream::read of multi-MB frames
    // shows up as host overhead between GPU submissions at 1080p.
    std::vector<char> source(static_cast<std::size_t>(frameCount) * frameBytes);
#ifdef WX_BG_PRELOAD
    std::exception_ptr loadError;
    std::thread loader([&] {
        try {
            std::ifstream input(inputPath, std::ios::binary);
            if (!input) throw std::runtime_error("Could not open input");
            if (!input.read(source.data(), static_cast<std::streamsize>(source.size())))
                throw std::runtime_error("Could not read source frames");
        } catch (...) {
            loadError = std::current_exception();
        }
    });
#else
    {
        std::ifstream input(inputPath, std::ios::binary);
        if (!input) throw std::runtime_error("Could not open input");
        if (!input.read(source.data(), static_cast<std::streamsize>(source.size())))
            throw std::runtime_error("Could not read source frames");
    }
#endif
    // ---- Offline probe stage -----------------------------------------
    // x264's rate-control notes (doc/ratecontrol.txt) define complexity as
    // frame size at constant QP and predict size at another QP via qscale =
    // 0.85 * 2^((QP-12)/6); Netflix's Dynamic Optimizer uses the same idea
    // (fixed-QP probes driving per-shot/per-frame decisions). We probe once
    // at QP 34, estimate the needed QP, probe once more at that QP to fit the
    // effective rate slope for this clip, then plan every frame's QP offline.
    // The final pass has no feedback, so the async host ring stays enabled.
    std::vector<std::uint32_t> plannedQp;
    double probeSeconds = 0.0, fittedSlope = 0.0;
    std::uint64_t probeBytes = 0;
    std::uint32_t calibrationQp = 0;
    if (twoPass) {
        auto probeStart = Clock::now();
        auto runProbe = [&](std::uint32_t qpValue, std::vector<std::uint32_t>& bits) {
            FrameConfig probeConfig = config;
            probeConfig.targetBits = 0u;
            probeConfig.initialQp = qpValue;
            probeConfig.plannedQp = 0u;
            probeConfig.twoPass = 0u;
            const fs::path path = outputPath.string() + ".probe" + std::to_string(qpValue) + ".h264";
            if (samePath(path, inputPath) || samePath(path, outputPath) ||
                (!reconPath.empty() && samePath(path, reconPath)))
                throw std::runtime_error("Probe output must be distinct from input, output and reconstruction");
            EncoderGpu probeGpu(probeConfig, false);
            std::ofstream probeOutput(path, std::ios::binary);
            if (!probeOutput) throw std::runtime_error("Could not open probe H.264 output");
            bits.assign(frameCount, 0u);
            std::uint64_t bytes = 0;
            auto record = [&](const EncodedFrame& frame, std::uint32_t index) {
                if (!frame.bytes) return;
                bytes += frame.bytes;
                // scan.comp counts bits as 32 * words; its output size is 4 * words.
                bits[index] = frame.bytes * 8u;
                if (!probeOutput.write(frame.annexB, frame.bytes))
                    throw std::runtime_error("Could not write probe H.264 frame");
            };
#ifdef WX_RING
            const bool ring = probeGpu.ringEnabled();
            const auto ringDepth = probeGpu.ringDepth();
#else
            const bool ring = false;
            constexpr std::uint32_t ringDepth = 0u;
#endif
            for (probeConfig.frameIndex = 0; probeConfig.frameIndex < frameCount; ++probeConfig.frameIndex) {
#ifdef WX_RING
                probeGpu.beginFrame(probeConfig.frameIndex);
#endif
                std::copy_n(source.data() + static_cast<std::size_t>(probeConfig.frameIndex) * frameBytes,
                            frameBytes,
#ifdef WX_RING
                            probeGpu.input(probeConfig.frameIndex)
#else
                            probeGpu.input()
#endif
                );
                auto frame = probeGpu.encode(probeConfig);
                if (ring) {
                    if (probeConfig.frameIndex >= ringDepth)
                        record(frame, probeConfig.frameIndex - ringDepth);
                } else {
                    record(frame, probeConfig.frameIndex);
                }
            }
#ifdef WX_RING
            if (ring) {
                auto drained = probeGpu.drainAll(frameCount);
                if (drained.size() != ringDepth) throw std::runtime_error("Unexpected probe ring drain size");
                for (std::uint32_t k = 0; k < drained.size(); ++k)
                    record(drained[k], frameCount - ringDepth + k);
            }
#endif
            probeOutput.close();
            if (!probeOutput) throw std::runtime_error("Could not finalize probe H.264 output");
            return bytes;
        };
        constexpr std::uint32_t baseQp = 34u;
        const double targetTotal = double(config.targetBits) * frameCount;
        std::vector<std::uint32_t> bitsA, bitsB;
        probeBytes = runProbe(baseQp, bitsA);
        double sumA = 0.0;
        for (std::uint32_t value : bitsA) sumA += double(std::max(value, 1u));
        if (!(sumA > 0.0)) throw std::runtime_error("QP34 probe produced no measurable complexity");
        double estimate = double(baseQp) - twoPassSlope * std::log2(targetTotal / sumA);
        calibrationQp = static_cast<std::uint32_t>(std::clamp(double(std::lround(estimate)), 0.0, 51.0));
        if (calibrationQp != baseQp) probeBytes += runProbe(calibrationQp, bitsB);
        else bitsB = bitsA;
        probeSeconds = seconds(probeStart);

        double sumB = 0.0;
        for (std::uint32_t value : bitsB) sumB += double(std::max(value, 1u));
        fittedSlope = twoPassSlope;
        if (calibrationQp != baseQp && sumB > 0.0 && sumA > 0.0) {
            double ratio = std::log2(sumB / sumA);
            if (std::abs(ratio) > 0.01) {
                // A lower QP probe produces MORE bits; both directions gives
                // the same positive QP-per-doubling slope.
                double measured = std::abs(double(int(calibrationQp) - int(baseQp))) / std::abs(ratio);
                if (measured >= 3.0 && measured <= 12.0) fittedSlope = measured;
            }
        }

        // ---- Fit the rate model and plan one QP per frame -----------------
        std::vector<double> weight(frameCount), qp(frameCount);
        double totalWeight = 0.0;
        for (std::uint32_t i = 0; i < frameCount; ++i) {
            double bits = double(std::max(bitsB[i], 1u));
            // x264 2pass uses complexity^0.6; the pure rate model is ^1.0.
            // IDR frames get an optional extra share, paid for by P frames
            // through the global scale.
            weight[i] = std::pow(bits, twoPassAlpha) * ((i % config.gop == 0u) ? twoPassIdrBoost : 1.0);
            totalWeight += weight[i];
        }
        if (!(totalWeight > 0.0) || !std::isfinite(totalWeight))
            throw std::runtime_error("Two-pass probe produced no measurable complexity");
        const double scale = targetTotal / totalWeight;
        for (std::uint32_t i = 0; i < frameCount; ++i) {
            double bits = double(std::max(bitsB[i], 1u));
            qp[i] = std::clamp(double(calibrationQp) - fittedSlope * std::log2(scale * weight[i] / bits),
                               0.0, 51.0);
        }
        if (twoPassSmooth) {
            double step = double(twoPassSmooth);
            for (std::uint32_t i = 1; i < frameCount; ++i)
                qp[i] = std::clamp(qp[i], std::max(0.0, qp[i - 1] - step),
                                            std::min(51.0, qp[i - 1] + step));
        }
        // Clamping and smoothing distort the sum of predicted bits; rebalance
        // with a uniform QP shift (still no per-frame feedback in pass 2).
        for (int pass = 0; pass < 2; ++pass) {
            double predicted = 0.0;
            for (std::uint32_t i = 0; i < frameCount; ++i)
                predicted += double(std::max(bitsB[i], 1u)) *
                             std::exp2(-(qp[i] - double(calibrationQp)) / fittedSlope);
            if (!(predicted > 0.0) || !std::isfinite(predicted)) break;
            double correction = -fittedSlope * std::log2(targetTotal / predicted);
            for (std::uint32_t i = 0; i < frameCount; ++i)
                qp[i] = std::clamp(qp[i] + correction, 0.0, 51.0);
        }
        plannedQp.resize(frameCount);
        if (twoPassDither) {
            // Integer QPs can leave several percent of rate on the table (a
            // single step is ~1.4x bits). Spend the fractional part by giving
            // a lower QP to the frames with the largest bit savings, and
            // round up the cheap ones until the modelled total matches.
            std::vector<double> lowBits(frameCount), saving(frameCount);
            double lowTotal = 0.0, totalSaving = 0.0;
            for (std::uint32_t i = 0; i < frameCount; ++i) {
                double base = std::clamp(std::floor(qp[i]), 0.0, 50.0);
                double bits = double(std::max(bitsB[i], 1u));
                lowBits[i] = bits * std::exp2(-(base - double(calibrationQp)) / fittedSlope);
                saving[i] = lowBits[i] * (1.0 - std::exp2(-1.0 / fittedSlope));
                lowTotal += lowBits[i];
                totalSaving += saving[i];
            }
            double remove = std::clamp(lowTotal - targetTotal, 0.0, totalSaving);
            std::vector<std::uint32_t> order(frameCount);
            std::iota(order.begin(), order.end(), 0u);
            std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
                return saving[a] < saving[b];
            });
            std::vector<std::uint32_t> chosen(frameCount);
            for (std::uint32_t index : order) {
                chosen[index] = static_cast<std::uint32_t>(std::clamp(std::floor(qp[index]), 0.0, 50.0));
                if (remove > 0.0) {
                    chosen[index] += 1u;
                    remove -= saving[index];
                }
            }
            plannedQp.swap(chosen);
        } else {
            for (std::uint32_t i = 0; i < frameCount; ++i)
                plannedQp[i] = static_cast<std::uint32_t>(std::lround(qp[i]));
        }
    }

    auto init = Clock::now();
    EncoderGpu gpu(config, !reconPath.empty(), {}, useTimestamps);
    RT("main: gpu constructed");
    double initSeconds = seconds(init);
#ifdef WX_BG_PRELOAD
    loader.join();
    if (loadError) std::rethrow_exception(loadError);
#endif
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
    auto account = [&](const EncodedFrame& frame) {
        if (!frame.bytes) return;
        bytes += frame.bytes;
        olderReferenceBlocks += frame.olderReferenceBlocks;
        flatIntraBlocks += frame.flatIntraBlocks;
        selectedReferenceDepth = std::max(selectedReferenceDepth, frame.selectedReferenceDepth);
        minQp = std::min(minQp, frame.qp);
        maxQp = std::max(maxQp, frame.qp);
        for (std::size_t i = 0; i < stageMs.size(); ++i) stageMs[i] += frame.gpuMilliseconds[i];
    };
    auto loop = Clock::now();
    for (config.frameIndex = 0; config.frameIndex < frameCount; ++config.frameIndex) {
        auto start = Clock::now();
        if (twoPass) {
            // QP+1 on the wire: 0 is reserved for "no plan".
            config.plannedQp = plannedQp[config.frameIndex] + 1u;
            config.twoPass = 1u;
        }
#ifdef WX_RING
        gpu.beginFrame(config.frameIndex);
#endif
        WX_PROF_START(copyIn);
        std::copy_n(source.data() + static_cast<std::size_t>(config.frameIndex) * frameBytes,
                    frameBytes,
#ifdef WX_RING
                    gpu.input(config.frameIndex)
#else
                    gpu.input()
#endif
        );
        WX_PROF_ADD(copyIn);
        auto frame = gpu.encode(config);
        RT("main: encoded");
        WX_PROF_START(h264Write);
#ifdef WX_RING
        if (frame.bytes && !output.write(frame.annexB, frame.bytes)) throw std::runtime_error("Could not write H.264 frame");
#else
        if (!output.write(frame.annexB, frame.bytes)) throw std::runtime_error("Could not write H.264 frame");
#endif
        WX_PROF_ADD(h264Write);
        WX_PROF_START(reconWrite);
        if (frame.reconstruction && !recon.write(frame.reconstruction, reconBytes))
            throw std::runtime_error("Could not write reconstruction frame");
        WX_PROF_ADD(reconWrite);
        account(frame);
        frameMs.push_back(seconds(start) * 1000);
    }
#ifdef WX_RING
    for (const auto& frame : gpu.drainAll(frameCount)) {
        WX_PROF_START(h264Write);
        if (!output.write(frame.annexB, frame.bytes)) throw std::runtime_error("Could not write drained H.264 frame");
        WX_PROF_ADD(h264Write);
        account(frame);
    }
#endif
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
              << ",\"rate_control\":\"" << (twoPass ? "two-pass offline"
                                                    : config.targetBits ? "average-rate" : "fixed-qp") << "\"}"
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
              << ",\"two_pass\":" << (twoPass ? 1u : 0u)
              << ",\"probe_seconds\":" << probeSeconds << ",\"probe_bytes\":" << probeBytes
              << ",\"planner\":{\"alpha\":" << twoPassAlpha << ",\"slope\":" << twoPassSlope
              << ",\"fitted_slope\":" << fittedSlope << ",\"calibration_qp\":" << calibrationQp
              << ",\"idr_boost\":" << twoPassIdrBoost << ",\"smooth\":" << twoPassSmooth
              << ",\"dither\":" << twoPassDither << "}"
              << ",\"gpu_stages_mean_ms\":{\"pyramid\":" << stageMs[0] / frameCount
              << ",\"halfpel\":" << stageMs[1] / frameCount << ",\"motion\":" << stageMs[2] / frameCount
              << ",\"spatial\":" << stageMs[3] / frameCount << ",\"older_references\":" << stageMs[4] / frameCount
              << ",\"residual\":" << stageMs[5] / frameCount << ",\"flat_intra\":" << stageMs[6] / frameCount
              << ",\"mb_flags\":" << stageMs[7] / frameCount << ",\"deblock\":" << stageMs[8] / frameCount
              << ",\"mb_entropy\":" << stageMs[9] / frameCount << ",\"entropy\":" << stageMs[10] / frameCount
              << ",\"scan\":" << stageMs[11] / frameCount << ",\"assemble\":" << stageMs[12] / frameCount << "}}\n";
    profileDump();
    return 0;
} catch (const std::exception& error) {
    std::cerr << "wx264: " << error.what() << '\n';
    return 1;
}
