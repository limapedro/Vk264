#pragma once

#include "gpu.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string_view>

// Causal duplicate-cadence signal: compare a 16x16 luma thumbnail of the
// CURRENT frame with the previous one.  OBS supplies the current frame in its
// encode callback, so this needs no source preload and no lookahead.  It is
// packed into the unused high bit of FrameConfig::aq (bit 0 remains the AQ
// toggle), preserving the 128-byte push-constant ABI.
inline constexpr std::uint32_t duplicateFrameFlag = 1u << 16;

inline void sampleThumbnail(const unsigned char* luma, std::size_t stride,
                            std::uint32_t width, std::uint32_t height,
                            std::array<unsigned char, 256>& out) {
    for (unsigned gy = 0; gy < 16; ++gy) {
        const std::size_t y = std::size_t((2 * gy + 1) * height / 32);
        for (unsigned gx = 0; gx < 16; ++gx) {
            const std::size_t x = std::size_t((2 * gx + 1) * width / 32);
            out[gy * 16 + gx] = luma[y * stride + x];
        }
    }
}

inline bool thumbnailDuplicate(const std::array<unsigned char, 256>& previous,
                               const std::array<unsigned char, 256>& current) {
    unsigned diff = 0;
    for (std::size_t i = 0; i < current.size(); ++i)
        diff += previous[i] > current[i] ? previous[i] - current[i] : current[i] - previous[i];
    return diff <= 2 * 256; // matches the offline planner's 16x16 threshold
}

struct Preset {
    std::string_view name;
    std::uint32_t motionRadius, satdCandidates, references;
};

inline constexpr Preset defaultPreset{"slow", 24, 4, 5};
inline constexpr Preset presets[] = {{"fast", 12, 0, 2}, {"medium", 16, 2, 3}, defaultPreset, {"veryslow", 32, 8, 16}};

inline Preset findPreset(std::string_view name) {
    for (const auto& preset : presets) if (preset.name == name) return preset;
    throw std::runtime_error("Unknown preset " + std::string(name) + " (choose fast, medium, slow or veryslow)");
}

inline std::uint32_t framesForSeconds(const FrameConfig& config, std::uint32_t seconds) {
    return static_cast<std::uint32_t>((std::uint64_t(config.fpsNumerator) * seconds +
        config.fpsDenominator / 2) / config.fpsDenominator);
}

inline std::uint32_t bitsPerFrame(const FrameConfig& config, std::uint32_t kbps) {
    return static_cast<std::uint32_t>((std::uint64_t(kbps) * 1000 * config.fpsDenominator +
        config.fpsNumerator / 2) / config.fpsNumerator);
}

inline constexpr std::uint32_t colorSignal(std::uint32_t primaries, std::uint32_t transfer,
                                           std::uint32_t matrix) {
    return (primaries << 8) | (transfer << 16) | (matrix << 24);
}

inline FrameConfig makeFrameConfig(std::uint32_t width, std::uint32_t height,
                                   std::uint32_t numerator, std::uint32_t denominator,
                                   Preset preset) {
    if (!width || !height || width > maxFrameDimension || height > maxFrameDimension ||
        ((width | height) & 1u))
        throw std::runtime_error("Wx264 requires even dimensions from 2 to 4096 pixels per axis");
    if (!denominator || numerator < denominator || numerator > 240ull * denominator)
        throw std::runtime_error("Frame rate must be between 1 and 240 fps");
    auto divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
    if (numerator > 0x7fffffffu)
        throw std::runtime_error("Frame rate exceeds AVC VUI timing precision");
    FrameConfig config{};
    config.width = width;
    config.height = height;
    config.fps = numerator / denominator;
    config.fpsNumerator = numerator;
    config.fpsDenominator = denominator;
    config.mbWidth = (config.width + 15u) / 16u;
    config.mbHeight = (config.height + 15u) / 16u;
    if (config.mbWidth * config.mbHeight > maxFrameMacroblocks)
        throw std::runtime_error("Wx264 supports up to 36864 macroblocks per frame");
    config.slicesPerRow = (config.mbWidth + singleSliceWidth - 1u) / singleSliceWidth;
    config.sliceCount = config.slicesPerRow * config.mbHeight;
    config.slotWords = singleSliceWidth * macroblockSlotWords;
    config.initialQp = 30;
    // A 2 s GOP spends too many frames on IDRs: the parallel CAVLC path makes
    // IDRs cheap in time but they still cost bits that P frames use more
    // efficiently. 5 s (250f at 50fps) matches x264 keyint practice and
    // measured +0.33 VMAF at 6000 kb/s plus higher throughput.
    config.gop = framesForSeconds(config, 5);
    config.motionRadius = preset.motionRadius;
    config.satdCandidates = preset.satdCandidates;
    config.references = preset.references;
    config.subpel = 1;
    config.partitions = 0;
    config.deblock = 2;
    // Four rows per P slice is the measured sweet spot at 1080p (+8-9%
    // process fps, equal/better VMAF).  Keep the shared work array within
    // maxPairedSliceMbs (512) for wider pictures.
    config.sliceRows = std::min(4u, std::max(2u, 512u / config.mbWidth));
    config.flatIntra = 1;
    return config;
}

// Starting QP for the model rate controller at >=50 fps.  Calibrated on
// wx264's own two-pass operating points (8000 kbit/s/60 fps -> QP 30,
// 6000 kbit/s/60 fps -> QP 33-34): starting at QP 14 overspends the first
// second on duplicated capture, starving the rest of the stream at the same
// aggregate rate (-1.9 VMAF at 6000).  No source preload is involved: this
// uses only the requested bits per pixel.
inline std::uint32_t estimateAbrInitialQp(const FrameConfig& config) {
    const double pixels = double(config.width) * double(config.height);
    const double bpp = pixels > 0.0 ? double(config.targetBits) / pixels : 0.1;
    const double qp = -3.0 - 8.43 * std::log2(bpp > 0.0 ? bpp : 0.001);
    return static_cast<std::uint32_t>(std::clamp(static_cast<double>(std::lround(qp)), 0.0, 51.0));
}

inline void finalizeFrameConfig(FrameConfig& config) {
    config.references = std::min(config.references, std::max(1u, config.gop - 1));
    // Annex A bounds the decoded picture buffer in macroblocks.
    auto picture = config.mbWidth * config.mbHeight;
    auto dpbMacroblocks = picture * config.references;
    auto rate = std::uint64_t(picture) * config.fpsNumerator;
    struct Level { std::uint32_t id, maxPicture, maxDpb, maxRate; };
    constexpr Level levels[] = {
        {42, 8704, 34816, 522240}, {50, 22080, 110400, 589824},
        {51, 36864, 184320, 983040}, {52, 36864, 184320, 2073600},
        {60, 139264, 696320, 4177920}, {61, 139264, 696320, 8355840},
        {62, 139264, 696320, 16711680},
    };
    for (const auto& level : levels) {
        if (picture <= level.maxPicture && dpbMacroblocks <= level.maxDpb &&
            rate <= std::uint64_t(level.maxRate) * config.fpsDenominator) {
            config.levelIdc = level.id;
            return;
        }
    }
    throw std::runtime_error("The picture size, frame rate and reference pool exceed H.264 Level 6.2");
}
