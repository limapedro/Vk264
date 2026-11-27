#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

inline constexpr std::uint32_t singleSliceWidth = 64;
inline constexpr std::uint32_t macroblockSlotWords = 768;
inline constexpr std::uint32_t maxFrameDimension = 4096;
inline constexpr std::uint32_t maxFrameMacroblocks = 36864;

struct FrameConfig {
    std::uint32_t width, height, mbWidth, mbHeight;
    std::uint32_t slicesPerRow, sliceCount, slotWords, frameIndex;
    std::uint32_t gop, fps, initialQp, targetBits;
    std::uint32_t motionRadius, subpel, partitions, deblock, sliceRows, satdCandidates;
    std::uint32_t references, levelIdc, referenceStrideWords, previousWords, deblockWave, flatIntra;
    std::uint32_t aq;
    std::uint32_t fpsNumerator, fpsDenominator, videoSignal;
    std::uint32_t idrWave; // diagonal wave index for whole-frame IDR residual
    std::uint32_t smoothAbr; // opt-in smoothed rate controller
};

struct EncodedFrame {
    const char* annexB;
    std::uint32_t bytes, qp, olderReferenceBlocks, selectedReferenceDepth, flatIntraBlocks;
    const char* reconstruction;
    std::array<double, 13> gpuMilliseconds;
};

class EncoderGpu {
public:
    EncoderGpu(const FrameConfig& config, bool readRecon, std::filesystem::path shaderDirectory = {}, bool timestamps = false);
    ~EncoderGpu();
    char* input();
    char* input(std::uint32_t frameIndex);
    void beginFrame(std::uint32_t frameIndex);
    EncodedFrame encode(const FrameConfig& config);
    std::string deviceName() const;
#ifdef WX_RING
    bool ringEnabled() const;
    unsigned ringDepth() const;
    std::vector<EncodedFrame> drainAll(std::uint32_t frameCount);
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
