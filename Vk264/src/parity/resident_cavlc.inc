// Vk264 parity module: resident_cavlc.inc
// Mirrors: resident CAVLC path + raw-clip/motion helpers
// Split from dct_parity.cpp; see docs/parity-layout.md.

struct RawClip {
    const char* path;
    int width, height, left, top, cropWidth, cropHeight, firstFrame, frames;
};
using RawFrame = std::array<std::vector<std::uint8_t>, 3>;

std::vector<std::vector<int>> macroblockWaves(int width, int height, int rowStep) {
    std::vector<std::vector<int>> waves(width+(height-1)*rowStep);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            waves[x+y*rowStep].push_back(x+y*width);
    return waves;
}

class RawClipReader {
    RawClip clip;
    int paddedWidth, paddedHeight;
    std::ifstream input;
    std::int64_t lumaSize = 0, frameSize = 0;

public:
    RawClipReader(RawClip source, int width, int height)
        : clip(source), paddedWidth(width), paddedHeight(height), input(clip.path, std::ios::binary | std::ios::ate) {
        if (clip.width <= 0 || clip.height <= 0 || clip.left < 0 || clip.top < 0 ||
            ((clip.width | clip.height | clip.left | clip.top) & 1) ||
            clip.cropWidth > clip.width-clip.left || clip.cropHeight > clip.height-clip.top ||
            clip.firstFrame < 0 || clip.frames < 1)
            throw std::runtime_error("Invalid raw YUV420 source geometry or frame range");
        if (!input) throw std::runtime_error("Cannot open raw YUV420 source");
        lumaSize = static_cast<std::int64_t>(clip.width)*clip.height;
        frameSize = lumaSize+lumaSize/2;
        const std::streamoff size = input.tellg();
        if (size < 0 || size%frameSize != 0 ||
            static_cast<std::int64_t>(clip.firstFrame)+clip.frames > size/frameSize)
            throw std::runtime_error("Raw YUV420 input does not contain the requested complete frames");
    }

    RawFrame read(int index) {
        const HostTimer timer("raw_input");
        RawFrame frame;
        const auto frameOffset = (static_cast<std::int64_t>(clip.firstFrame)+index)*frameSize;
        for (int p = 0; p < 3; ++p) {
            const int shift = int(p != 0);
            const int stride = clip.width >> shift;
            const int visibleWidth = clip.cropWidth >> shift, visibleHeight = clip.cropHeight >> shift;
            const int width = paddedWidth >> shift, height = paddedHeight >> shift;
            const auto planeOffset = frameOffset+(p == 0 ? 0 : lumaSize+(p-1)*(lumaSize/4));
            auto& plane = frame[p];
            plane.resize(width*height);
            const auto visibleOffset = planeOffset+static_cast<std::int64_t>(clip.top >> shift)*stride+
                                       (clip.left >> shift);
            if (visibleWidth == stride && visibleWidth == width) {
                input.seekg(visibleOffset);
                if (!input.read(reinterpret_cast<char*>(plane.data()), width*visibleHeight))
                    throw std::runtime_error("Cannot read raw YUV420 crop");
            } else {
                for (int row = 0; row < visibleHeight; ++row) {
                    input.seekg(visibleOffset+static_cast<std::int64_t>(row)*stride);
                    auto* destination = plane.data()+row*width;
                    if (!input.read(reinterpret_cast<char*>(destination), visibleWidth))
                        throw std::runtime_error("Cannot read raw YUV420 crop");
                    std::fill(destination+visibleWidth, destination+width, destination[visibleWidth-1]);
                }
            }
            for (int row = visibleHeight; row < height; ++row)
                std::copy_n(plane.data()+(visibleHeight-1)*width, width, plane.data()+row*width);
        }
        return frame;
    }
};

struct FrameMotionBlock {
    mvPredictCorpus::Pair vector;
    std::array<std::uint32_t, 96> prediction;
};

std::vector<std::int32_t> packMotionInput(const RawFrame& source, const std::vector<pixel>& luma,
                                         const std::array<std::vector<pixel>, 2>& chroma, int width, int height,
                                         const motionSearchCorpus::Fixture& parameters, const char* filterShader) {
    const FilteredReference reference(luma, width, height, filterShader);
    HostTimer packing("frame_motion_pack");
    const int border = FilteredReference::border;
    const int planeSize = reference.stride*reference.rows;
    std::vector<std::int32_t> input(8);
    input.reserve(8+parameters.motionCost.size()+(width*height*3/2+4*planeSize)/4);
    input[0] = width;
    input[1] = height;
    input.insert(input.end(), parameters.motionCost.begin(), parameters.motionCost.end());
    const auto appendPlane = [&input](int bytes, auto sample) {
        const int offset = static_cast<int>(input.size());
        for (int i = 0; i < bytes; i += 4) {
            const std::uint32_t word = sample(i) | (std::uint32_t(sample(i+1)) << 8) |
                                       (std::uint32_t(sample(i+2)) << 16) | (std::uint32_t(sample(i+3)) << 24);
            input.push_back(static_cast<std::int32_t>(word));
        }
        return offset;
    };
    input[2] = appendPlane(width*height, [&](int i) { return source[0][i]; });
    input[3] = appendPlane(planeSize, [&](int i) {
        const int x = std::clamp(i%reference.stride-border, 0, width-1);
        const int y = std::clamp(i/reference.stride-border, 0, height-1);
        return luma[y*width+x];
    });
    input[4] = planeSize/4;
    for (int p = 0; p < 3; ++p)
        appendPlane(planeSize, [&](int i) { return reference.plane(p)[i]; });
    for (int p = 0; p < 2; ++p)
        input[5+p] = appendPlane(width*height/4, [&](int i) { return chroma[p][i]; });
    input[7] = static_cast<int>(input.size());
    return input;
}

std::vector<FrameMotionBlock> predictInterFrame(const RawFrame& source,
                                              const std::vector<pixel>& luma,
                                              const std::array<std::vector<pixel>, 2>& chroma,
                                              int width, int height,
                                              const motionSearchCorpus::Fixture& parameters,
                                              const char* filterShader, const char* frameShader) {
    const auto input = packMotionInput(source, luma, chroma, width, height, parameters, filterShader);
    const int mbWidth = width/16, mbHeight = height/16;
    std::vector<FrameMotionBlock> encoded(mbWidth*mbHeight);
    static_assert(sizeof(FrameMotionBlock) == 98*4);
    const auto subgroupSize = computeContext().motionSubgroupSize;
    const auto shader = subgroupSize > 1
        ? std::filesystem::path(frameShader).replace_filename("motion_frame_parallel.spv").string() : frameShader;
    execute(shader.c_str(), encoded, 1, input, mbHeight*subgroupSize, {{0, sizeof(FrameMotionBlock)}},
            mbWidth+2*(mbHeight-1), true);
    return encoded;
}

constexpr int cavlcOutputWords = cabacCorpus::frameMaxBytes/4, cavlcBodyWords = 768;

std::vector<std::int32_t> cavlcHeader(const std::uint32_t* codes, const sliceCorpus::Fixture& header,
                                      int width, int macroblocks, bool inter, int levelCount) {
    constexpr int codeCount = 732;
    const int headerWords = header.pointer/4, tailBits = header.bitPosition-8*header.pointer;
    const int outputStart = headerWords+levelCount;
    const int countStart = outputStart+1+cavlcOutputWords;
    const int lengthStart = countStart+24*macroblocks, bodyStart = lengthStart+macroblocks;
    std::vector<std::int32_t> input{width, macroblocks, int(inter), headerWords,
        static_cast<std::int32_t>(header.low), tailBits, headerWords, outputStart, countStart, lengthStart, bodyStart};
    input.reserve(input.size()+codeCount+headerWords);
    input.insert(input.end(), codes, codes+codeCount);
    for (int i = 0; i < header.pointer; i += 4) {
        const auto word = (header.bytes[i] << 24) | (header.bytes[i+1] << 16) |
                          (header.bytes[i+2] << 8) | header.bytes[i+3];
        input.push_back(static_cast<std::int32_t>(word));
    }
    return input;
}

motionSearchCorpus::Fixture motionSearchParameters(int qp, int meMethod, int subme, int range);
motionSearchCorpus::Fixture motionSearchParameters(int qp, const char* presetName) {
    if (presetName) {
        if (const auto* preset = x264_vulkan::presets::find(presetName)) {
            const int me = preset->me == x264_vulkan::presets::tesa ? X264_ME_TESA : static_cast<int>(preset->me);
            // Honour x264's i_me_range exactly (16 default, 24 for veryslow/placebo).
            return motionSearchParameters(qp, me, preset->subpelRefine, preset->meRange);
        }
        throw std::runtime_error(std::string("Unknown x264 preset '") + presetName + "'");
    }
    return motionSearchParameters(qp, X264_ME_UMH, 8, 8);
}
motionSearchCorpus::Fixture motionSearchParameters(int qp) {
    return motionSearchParameters(qp, nullptr);
}
motionSearchCorpus::Fixture motionSearchParameters(int qp, int meMethod, int subme, int range) {
    motionSearchCorpus::Fixture searchParameters{};
    searchParameters.method = meMethod;
    searchParameters.pixelSize = PIXEL_16x16;
    searchParameters.useSatd = 1;
    searchParameters.subme = subme;
    searchParameters.range = range;
    searchParameters.minFpel = {-range,-range};
    searchParameters.maxFpel = {range,range};
    searchParameters.minQpel = {-32,-32};
    searchParameters.maxQpel = {32,32};
    for (int d = 0; d <= motionSearchCorpus::costRadius; ++d) {
        const float bitEstimate = d == 0 ? .718f : std::log2(static_cast<float>(d+1))*2.f+1.718f;
        const int cost = static_cast<int>(x264_lambda_tab[qp]*bitEstimate+.5f);
        searchParameters.motionCost[motionSearchCorpus::costRadius-d] = cost;
        searchParameters.motionCost[motionSearchCorpus::costRadius+d] = cost;
    }
    return searchParameters;
}

struct ResidentFrameResult {
    std::vector<std::uint8_t> packet;
    std::uint32_t inter8x8;
};

ResidentFrameResult encodeResidentCavlcFrame(const std::filesystem::path& shaderDirectory,
                                             const std::uint32_t* codes, const sliceCorpus::Fixture& header,
                                             const RawFrame& source,
                                             std::vector<pixel>& lumaOutput, std::array<std::vector<pixel>, 2>& chromaOutput,
                                             int width, int height, int qp, int chromaQp,
                                             const motionSearchCorpus::Fixture& search,
                                             bool inter, bool readReconstruction, bool inter8x8 = true,
                                              const char* preset = nullptr) {
    const char* stageName = inter ? "resident_cavlc_p_frame" : "resident_cavlc_idr_frame";
    const HostTimer phase(stageName);
    auto& context = computeContext();
    const auto shaderPath = [&](const char* name) { return (shaderDirectory/name).string(); };
    constexpr std::size_t batchSize = 512, wordBytes = sizeof(std::uint32_t);
    const int columns = width/16, rows = height/16, macroblocks = columns*rows;
    const int levelWords = macroblocks*cabacCorpus::macroblockWords;
    const auto sliceInput = cavlcHeader(codes, header, columns, macroblocks, inter, levelWords);
    constexpr std::size_t sliceHeaderCapacity = 1024;
    if (sliceInput.size() > sliceHeaderCapacity) throw std::runtime_error("CAVLC frame header exceeds its storage");
    const int sourceWords = width*height/4, referenceWords = (width+64)*(height+64)/4;
    std::array<std::int32_t, 11> motionInput{};
    motionInput[0] = width;
    motionInput[1] = height;
    motionInput[8] = search.method;
    motionInput[9] = search.subme;
    motionInput[10] = search.range;
    const auto sourceStart = motionInput.size()+search.motionCost.size();
    const int referenceStart = static_cast<int>(sourceStart)+sourceWords;
    motionInput[2] = static_cast<int>(sourceStart);
    motionInput[3] = referenceStart;
    motionInput[4] = referenceWords;
    motionInput[7] = referenceStart+4*referenceWords;
    const std::array<int, 7> filterInput{columns, rows, qp, chromaQp, int(!inter), 0, 0};
    intraCorpus::Fixture q4{};
    q4.qp = qp;
    intraCorpus::setFlatQuant(q4, inter ? 11 : 21);
    inter8Corpus::Fixture q8{};
    q8.qp = qp;
    inter8Corpus::configure(q8);
    chromaCorpus::Fixture qc{};
    qc.qp = chromaQp;
    qc.mode = inter ? -1 : I_PRED_CHROMA_DC;
    chromaCorpus::configure(qc);

    std::vector<std::uint32_t> frameInput(16);
    // bit0 = intra; bit1 = x264 intra mode decision (V wins ties for luma);
    // bit2 = x264 mbcmp_init() uses SAD (not SATD) when subpel_refine <= 1.
    const auto* presetInfo = preset ? x264_vulkan::presets::find(preset) : nullptr;
    const bool useSad = presetInfo && presetInfo->subpelRefine <= 1;
    frameInput[13] = (inter ? 0u : 1u) |
        ((preset && std::strcmp(preset, "ultrafast") == 0) ? 2u : 0u) |
        (useSad ? 4u : 0u);
    const auto append = [&frameInput](const void* bytes, std::size_t length) {
        const auto start = static_cast<std::uint32_t>(frameInput.size());
        frameInput.resize(frameInput.size()+(length+3)/4);
        std::memcpy(frameInput.data()+start, bytes, length);
        return start;
    };
    frameInput[8] = append(&q4, offsetof(intraCorpus::Fixture, source));
    frameInput[9] = append(&q8, offsetof(inter8Corpus::Fixture, source));
    frameInput[10] = append(&qc, offsetof(chromaCorpus::Fixture, source));
    const std::uint32_t zero = 0;
    frameInput[11] = append(&zero, wordBytes);
    frameInput[3] = static_cast<std::uint32_t>(frameInput.size());
    frameInput[4] = frameInput[3]+sourceWords/4;

    const std::size_t alignment = context.physical.getProperties().limits.minStorageBufferOffsetAlignment;
    const std::size_t frameUploadBytes = frameInput.size()*wordBytes+source[1].size()+source[2].size();
    std::size_t cursor = frameUploadBytes;
    const auto reserve = [&](std::size_t bytes) {
        cursor = (cursor+alignment-1)&~(alignment-1);
        const auto start = cursor;
        cursor += bytes;
        return vk::DescriptorBufferInfo({}, start, bytes);
    };
    const auto motionSize = motionInput[7]*wordBytes+macroblocks*sizeof(FrameMotionBlock);
    auto motion = reserve(motionSize);
    const std::size_t filterPixels = (filterInput.size()+3*macroblocks)*wordBytes;
    const auto pixels = lumaOutput.size()+chromaOutput[0].size()+chromaOutput[1].size();
    auto filter = reserve(filterPixels+pixels);
    auto slice = reserve((sliceHeaderCapacity+levelWords+1+cavlcOutputWords+macroblocks*(25+cavlcBodyWords))*wordBytes);
    auto l4 = reserve(batchSize*sizeof(intraCorpus::Fixture));
    auto l8 = reserve(batchSize*sizeof(inter8Corpus::Fixture));
    auto uv = reserve(2*batchSize*sizeof(chromaCorpus::Fixture));
    constexpr std::size_t packetChunkBytes = 64; // cavlc_packet.comp
    constexpr std::size_t packetChunks = cabacCorpus::frameMaxBytes/packetChunkBytes;
    const auto packetScratch = reserve(packetChunks*wordBytes);
    constexpr std::size_t maxPacket = 2*cabacCorpus::frameMaxBytes+4;
    auto packet = reserve(wordBytes+maxPacket);

    frameInput[0] = columns;
    frameInput[1] = rows;
    frameInput[2] = static_cast<std::uint32_t>(motion.offset/wordBytes+motionInput[2]);
    frameInput[5] = static_cast<std::uint32_t>(motion.offset/wordBytes+motionInput[7]);
    frameInput[6] = static_cast<std::uint32_t>(slice.offset/wordBytes+sliceInput.size());
    frameInput[7] = static_cast<std::uint32_t>(filter.offset/wordBytes);
    frameInput[12] = static_cast<std::uint32_t>(motion.offset/wordBytes+referenceStart);
    frameInput[14] = static_cast<std::uint32_t>(slice.offset/wordBytes+sliceInput.size()+levelWords);
    frameInput[15] = static_cast<std::uint32_t>(packetScratch.offset/wordBytes);
    const auto referenceEnd = filter.offset+filterPixels;
    // Motion reads the previous chroma planes in place. The join dispatch
    // overwrites those planes only after the last motion wave has finished.
    motionInput[5] = static_cast<int>((referenceEnd-motion.offset)/wordBytes)+sourceWords;
    motionInput[6] = motionInput[5]+sourceWords/4;
    motion.range = filter.offset+filter.range-motion.offset;

    context.reserve(cursor);
    context.reserveDevice(cursor);
    const auto device = *context.device;
    const auto resident = *context.deviceBuffer, staging = *context.buffer;
    for (auto* view : {&motion, &slice, &filter, &l4, &l8, &uv}) view->buffer = resident;
    packet.buffer = staging;
    const vk::DescriptorBufferInfo arena(resident, 0, cursor);
    const auto& sets = context.interSets;
    const vk::DescriptorType storage = vk::DescriptorType::eStorageBuffer;
    const vk::WriteDescriptorSet bindings[]{
        {sets[0], 0, 0, storage, {}, motion}, {sets[1], 0, 0, storage, {}, arena},
        {sets[1], 1, 0, storage, {}, l4}, {sets[1], 2, 0, storage, {}, l8}, {sets[1], 3, 0, storage, {}, uv},
        {sets[1], 4, 0, storage, {}, packet},
        {sets[2], 0, 0, storage, {}, slice}, {sets[3], 0, 0, storage, {}, filter},
    };
    device.updateDescriptorSets(bindings, {});
    const vk::BufferCopy uploads[]{
        {0, 0, frameUploadBytes},
        {motion.offset, motion.offset, referenceStart*wordBytes},
        {slice.offset, slice.offset, sliceInput.size()*wordBytes},
        {filter.offset, filter.offset, filterInput.size()*wordBytes},
    };
    {
        const HostTimer stage("resident_staging_input");
        const auto copy = [&](std::size_t offset, const auto& data) {
            std::memcpy(context.mapped+offset, data.data(), data.size()*sizeof(data[0]));
        };
        copy(0, frameInput);
        copy(frameInput[3]*wordBytes, source[1]);
        copy(frameInput[4]*wordBytes, source[2]);
        copy(motion.offset, motionInput);
        copy(motion.offset+motionInput.size()*wordBytes, search.motionCost);
        copy(motion.offset+sourceStart*wordBytes, source[0]);
        copy(slice.offset, sliceInput);
        copy(filter.offset, filterInput);
    }
    const auto subgroupSize = context.motionSubgroupSize;
    const char* tileSetting = std::getenv("VK264_MOTION_TILE");
    const bool tiled = context.motionCandidateBatches && tileSetting &&
        std::strlen(tileSetting) == 3 && (tileSetting[0] == '2' || tileSetting[0] == '4') &&
        tileSetting[1] == 'x' && (tileSetting[2] == '1' || tileSetting[2] == '2');
    const int tileWidth = tiled ? tileSetting[0]-'0' : 0;
    const int tileHeight = tiled ? tileSetting[2]-'0' : 0;
    const char* motionShader = tileWidth ? "motion_frame_tiled.spv" :
                               context.motionCandidateBatches ? "motion_frame_batched.spv" :
                               subgroupSize > 1 ? "motion_frame_parallel.spv" : "motion_frame.spv";
    const auto motionPipeline = context.pipeline(shaderPath(motionShader).c_str());
    const auto bridge = context.pipeline(shaderPath("inter_frame.spv").c_str());
    const auto transform4 = context.pipeline(shaderPath("intra16_frame.spv").c_str());
    const auto transform8 = context.pipeline(shaderPath("inter8x8_frame.spv").c_str());
    const auto chroma = context.pipeline(shaderPath("intra_chroma_frame.spv").c_str());
    const auto cavlc = context.pipeline(shaderPath("cavlc_frame.spv").c_str());
    const auto packetPipeline = context.pipeline(shaderPath("cavlc_packet.spv").c_str());
    const auto deblock = context.pipeline(shaderPath(context.deblockSubgroupVote
        ? "deblock_frame_subgroup.spv" : "deblock_frame_tiled.spv").c_str());
    const auto cmd = context.cmd;
    const auto pipelineLayout = *context.pipelineLayout;
    const auto barrier = [&](vk::PipelineStageFlagBits from, vk::PipelineStageFlagBits to,
                             vk::AccessFlags write, vk::AccessFlags read) {
        cmd.pipelineBarrier(from, to, {}, vk::MemoryBarrier(write, read), {}, {});
    };
    constexpr auto transfer = vk::PipelineStageFlagBits::eTransfer;
    constexpr auto compute = vk::PipelineStageFlagBits::eComputeShader;
    constexpr auto shaderWrite = vk::AccessFlagBits::eShaderWrite;
    constexpr auto shaderAccess = vk::AccessFlagBits::eShaderRead | shaderWrite;
    const auto bind = [&](vk::Pipeline pipeline, int index) {
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipelineLayout, 0, sets[index], {});
    };
    const auto dispatch = [&](std::uint32_t groups, std::uint32_t pass) {
        cmd.pushConstants<std::uint32_t>(pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, pass);
        cmd.dispatch(groups, 1, 1);
        barrier(compute, compute, shaderWrite, shaderAccess);
    };

    device.resetCommandPool(*context.commandPool);
    cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    std::vector<const char*> timingStages;
    const auto timestamp = [&](const char* stage) {
        if (!context.timestamps) return;
        timingStages.push_back(stage);
        cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *context.timestamps,
                           static_cast<std::uint32_t>(timingStages.size()));
    };
    if (context.timestamps) {
        cmd.resetQueryPool(*context.timestamps, 0, ComputeContext::timestampCapacity);
        cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *context.timestamps, 0);
    }
    cmd.copyBuffer(staging, resident, uploads);
    barrier(transfer, compute, vk::AccessFlagBits::eTransferWrite, shaderAccess);
    timestamp(inter ? "resident_p_upload" : "resident_idr_upload");
    barrier(compute, compute, shaderWrite, shaderAccess);
    const auto residual = [&](std::uint32_t groups, std::uint32_t index, bool eight) {
        bind(bridge, 1);
        dispatch(groups, index);
        timestamp(eight ? "resident_p_prepare" : "resident_idr_prepare");
        bind(transform4, 1);
        dispatch(groups, 0);
        timestamp(eight ? "resident_p_luma4" : "resident_idr_luma");
        if (eight) {
            bind(transform8, 1);
            dispatch(groups*4, 0);
            timestamp("resident_p_luma8");
        }
        bind(chroma, 1);
        dispatch(groups, 0);
        timestamp(eight ? "resident_p_chroma" : "resident_idr_chroma");
        bind(bridge, 1);
        dispatch(groups, index|(1u<<31));
        timestamp(eight ? "resident_p_join" : "resident_idr_join");
    };
    if (inter) {
        bind(bridge, 1);
        dispatch((referenceWords+63)/64, 1u<<30);
        timestamp("resident_p_prefilter");
        bind(motionPipeline, 0);
        if (tileWidth) {
            const int tileRows = (rows+tileHeight-1)/tileHeight;
            const int tileColumns = (columns+rows-2)/tileWidth+1;
            const int groups = tileHeight == 1 ? (tileRows+1)/2 : tileRows;
            for (int wave = 0; wave < tileColumns+tileRows-1; ++wave)
                dispatch(groups, std::uint32_t(wave) | (std::uint32_t(tileWidth) << 16) |
                                 (std::uint32_t(tileHeight) << 24));
        } else {
            for (int wave = 0; wave < columns+2*(rows-1); ++wave)
                dispatch((rows*subgroupSize+63)/64, wave);
        }
        timestamp("resident_p_motion");
        for (std::uint32_t first = 0; first < unsigned(macroblocks); first += batchSize)
            residual((std::min<std::uint32_t>(batchSize, macroblocks-first)+63)/64, first, inter8x8);
    } else {
        for (int wave = 0; wave < columns+rows-1; ++wave)
            residual((rows+63)/64, wave, false);
    }
    bind(cavlc, 2);
    const auto groups = (macroblocks+63)/64;
    for (std::uint32_t pass = 0; pass < 4; ++pass) dispatch(groups, pass);
    timestamp(inter ? "resident_p_entropy" : "resident_idr_entropy");
    if (inter8x8) {
        bind(deblock, 3);
        const int tileRows = (rows+3)/4, tileColumns = (columns+rows+2)/4;
        for (int wave = 0; wave < tileRows+tileColumns-1; ++wave) dispatch(tileRows, wave);
        timestamp(inter ? "resident_p_deblock" : "resident_idr_deblock");
    } else {
        timestamp(inter ? "resident_p_deblock" : "resident_idr_deblock");
    }
    bind(packetPipeline, 1);
    dispatch(packetChunks/64, 0);
    dispatch(1, 1);
    dispatch(packetChunks/64, 2);
    timestamp(inter ? "resident_p_packet" : "resident_idr_packet");
    barrier(compute, transfer, shaderWrite, vk::AccessFlagBits::eTransferRead);
    const auto countOffset = frameInput[11]*wordBytes;
    std::vector<vk::BufferCopy> downloads{
        {countOffset, countOffset, wordBytes},
    };
    if (readReconstruction) downloads.emplace_back(referenceEnd, referenceEnd, pixels);
    const bool dumpCoef = std::getenv("VK264_DUMP_COEF") != nullptr;
    if (dumpCoef) downloads.emplace_back(frameInput[6]*wordBytes, 0, macroblocks*cabacCorpus::macroblockWords*wordBytes);
    cmd.copyBuffer(resident, staging, downloads);
    barrier(transfer, vk::PipelineStageFlagBits::eHost, vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead);
    barrier(compute, vk::PipelineStageFlagBits::eHost, shaderWrite, vk::AccessFlagBits::eHostRead);
    timestamp(inter ? "resident_p_download" : "resident_idr_download");
    cmd.end();
    const auto submitted = ComputeContext::Clock::now();
    context.queue.submit(vk::SubmitInfo({}, {}, cmd, {}));
    context.queue.waitIdle();
    const auto completed = ComputeContext::Clock::now();

    if (dumpCoef) {
        const auto* lv = reinterpret_cast<const std::int32_t*>(context.mapped);
        std::fprintf(stderr, "VK264DC ");
        for (int i = 0; i < 16; ++i) std::fprintf(stderr, "%d,", lv[4+i]);
        std::fprintf(stderr, "\nVK264META mode=%d luma=%d dcNZ=%d mask=%08x chroma=%d\n",
                     lv[0], lv[1], lv[2], lv[3], lv[260]);
        for (int b = 0; b < 16; ++b)
            std::fprintf(stderr, "VK264AC%d %d,%d,%d,%d\n", b,
                         lv[20+15*b], lv[21+15*b], lv[22+15*b], lv[23+15*b]);
    }
    if (readReconstruction) {
        auto reference = context.mapped+referenceEnd;
        const auto unpack = [&reference](std::vector<pixel>& plane) {
            std::memcpy(plane.data(), reference, plane.size());
            reference += plane.size();
        };
        unpack(lumaOutput);
        for (auto& plane : chromaOutput) unpack(plane);
    }
    const auto count = *reinterpret_cast<const std::uint32_t*>(context.mapped+countOffset);
    const auto packetSize = *reinterpret_cast<const std::uint32_t*>(context.mapped+packet.offset);
    if (packetSize < 5 || packetSize > maxPacket) throw std::runtime_error("Invalid CAVLC packet length");
    const auto* packetBytes = reinterpret_cast<const std::uint8_t*>(context.mapped+packet.offset+wordBytes);
    if (context.profiling) {
        auto& stats = context.profile[stageName];
        ++stats.calls;
        for (const auto& region : uploads) stats.uploadBytes += region.size;
        for (const auto& region : downloads) stats.readbackBytes += region.size;
        stats.readbackBytes += wordBytes+packetSize;
        stats.dispatch += completed-submitted;
        if (context.timestamps) {
            std::vector<std::uint64_t> ticks(timingStages.size()+1);
            const VkResult status = vkGetQueryPoolResults(static_cast<VkDevice>(device),
                static_cast<VkQueryPool>(*context.timestamps), 0, static_cast<std::uint32_t>(ticks.size()),
                ticks.size()*sizeof(ticks.front()), ticks.data(),
                sizeof(ticks.front()), VK_QUERY_RESULT_64_BIT);
            if (status == VK_SUCCESS) {
                const auto mask = UINT64_MAX >> (64-context.timestampBits);
                stats.gpuMilliseconds += double((ticks.back()-ticks.front()) & mask)*context.timestampPeriod/1e6;
                for (std::size_t i = 0; i < timingStages.size(); ++i) {
                    auto& stage = context.profile[timingStages[i]];
                    ++stage.calls;
                    stage.gpuMilliseconds += double((ticks[i+1]-ticks[i]) & mask)*context.timestampPeriod/1e6;
                }
            }
        }
    }
    return {std::vector<std::uint8_t>(packetBytes, packetBytes+packetSize), count};
}

// Replays the bin descriptors produced by cabac_bins.comp through x264's own
// arithmetic coder. The descriptor sequence is identical to the one cabac.comp
// codes on the GPU, so the resulting slice is byte-for-byte the same.
