// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace x264_vulkan {
struct EncodeOptions {
    const char* input;
    const char* output;
    const char* reconstruction;
    int width, height, frames;
    int fpsNumerator, fpsDenominator, gop, initialQp, targetBits;
    std::uint32_t videoSignal;
    int levelIdc;
    const std::uint32_t* cavlcCodes = nullptr;
};

struct EncodeResult {
    std::uint64_t bytes;
    std::size_t inter8x8Blocks;
    int minQp, maxQp;
    double seconds;
    std::string deviceName;
};

EncodeResult encode(const EncodeOptions& options);
}
