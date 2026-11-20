#pragma once

#include "gpu.hpp"
#include "x264_levels.hpp"
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <string_view>

struct Preset {
    std::string_view name;
    std::uint32_t motionRadius, satdCandidates, references;
};

// Vk264 internal knob-space, broadened to cover the x264 preset order so that
// --preset <name> means the same thing across vk264 and libx264. Each vk264
// preset carries (motionRadius, satdCandidates, referenceFrames); the x264
// motion search method/subpel-refine/rate-distortion-settings used by libx264
// are assembled by the x264 backend at a different layer.
inline constexpr Preset defaultPreset{"slow", 16, 4, 5};
inline constexpr Preset presets[] = {
    // vk264 native names (kept for backwards compatibility).
    // motionRadius mirrors x264 i_me_range (16 default, 24 veryslow/placebo).
    {"fast",        16, 0, 2},
    {"medium",      16, 2, 3},
    defaultPreset,
    {"veryslow",    24, 8, 16},
    // x264-mapped presets (me_range 16; veryslow/placebo 24)
    {"ultrafast",   16, 0, 1},
    {"superfast",   16, 0, 1},
    {"veryfast",    16, 0, 1},
    {"faster",      16, 0, 2},
    {"slower",      16, 6, 8},
    {"placebo",     24, 8, 16},
};

inline Preset findPreset(std::string_view name) {
    for (const auto& preset : presets) if (preset.name == name) return preset;
    throw std::runtime_error("Unknown preset " + std::string(name) + " (choose ultrafast, superfast, veryfast, faster, fast, medium, slow, slower, veryslow or placebo)");
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
        throw std::runtime_error("Vk264 requires even dimensions from 2 to 4096 pixels per axis");
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
        throw std::runtime_error("Vk264 supports up to 36864 macroblocks per frame");
    config.slicesPerRow = (config.mbWidth + singleSliceWidth - 1u) / singleSliceWidth;
    config.sliceCount = config.slicesPerRow * config.mbHeight;
    config.slotWords = singleSliceWidth * macroblockSlotWords;
    config.initialQp = 30;
    config.gop = framesForSeconds(config, 2);
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
    config.levelIdc = x264_levels::selectLevel({
        static_cast<int>(picture), static_cast<int>(config.mbWidth), static_cast<int>(config.mbHeight),
        static_cast<int>(config.references), static_cast<int>(config.fpsNumerator), static_cast<int>(config.fpsDenominator),
        0, // vbv disabled for the Vulkan path; default CAVLC backend stays on unsigned average-rate
        0,
        0});
}
