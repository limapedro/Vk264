#pragma once

#include "gpu.hpp"
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <string_view>

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
    config.sliceRows = 2;
    config.flatIntra = 1;
    return config;
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
