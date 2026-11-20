// Vk264 parity module: resident_cabac.inc
// Mirrors: resident CABAC bin replay + frame path
// Split from dct_parity.cpp; see docs/parity-layout.md.

std::vector<std::uint8_t> cabacEncodeBins(const std::uint32_t* bins, std::size_t count,
                                          int qp, bool inter, int pictureIndex,
                                          std::uint8_t precedingByte) {
    static const bool contextsReady = [] {
        x264_t seed{};
        x264_cabac_init(&seed);
        return true;
    }();
    (void)contextsReady;
    const std::size_t capacity = cabacCorpus::frameMaxBytes;
    std::vector<std::uint8_t> bytes(capacity);
    bytes[0] = precedingByte;
    x264_t state{};
    state.i_frame = pictureIndex;
    state.sh.i_type = inter ? SLICE_TYPE_P : SLICE_TYPE_I;
    x264_cabac_context_init(&state, &state.cabac, state.sh.i_type, qp, 0);
    x264_cabac_encode_init(&state.cabac, bytes.data()+1, bytes.data()+capacity);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t bin = bins[i];
        if ((bin & 0x80000000u) == 0) {
            if ((bin & 0x40000000u) == 0)
                x264_cabac_encode_decision_c(&state.cabac, int(bin >> 1), int(bin & 1));
            else
                x264_cabac_encode_bypass_c(&state.cabac, int(bin & 1));
        } else if ((bin & 0x40000000u) == 0) {
            x264_cabac_encode_ue_bypass(&state.cabac, int((bin >> 28) & 3), int(bin & 0x0fffffffu));
        } else if ((bin & 0x20000000u) == 0) {
            x264_cabac_encode_terminal_c(&state.cabac);
        } else {
            x264_cabac_encode_flush(&state, &state.cabac);
        }
    }
    x264_cabac_encode_flush(&state, &state.cabac);
    return std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + (state.cabac.p - bytes.data()));
}

// Host equivalent of nal.comp's Annex B path, used to drop the per-frame GPU
// round trip that the staged packer needed.
std::vector<std::uint8_t> packAnnexB(const std::vector<std::uint8_t>& rbsp,
                                     std::uint32_t refIdc, std::uint32_t type, bool longStart) {
    std::vector<std::uint8_t> out;
    out.reserve(rbsp.size()+rbsp.size()/2+16);
    if (longStart) out.push_back(0);
    out.insert(out.end(), {0, 0, 1, std::uint8_t((refIdc << 5) | type)});
    int zeros = 0;
    for (const auto byte : rbsp) {
        if (byte <= 3 && zeros >= 2) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(byte);
        zeros = byte == 0 ? zeros+1 : 0;
    }
    return out;
}

// CABAC equivalent of encodeResidentCavlcFrame. The motion, residual, join and
// deblock stages are unchanged; only the entropy stage differs. The join pass
// writes the 394-word macroblock records straight into the CABAC residual bank,
// so the arithmetic coder reads exactly what the CAVLC path reads, with no host
// round trip between prediction and entropy.
ResidentFrameResult encodeResidentCabacFrame(const std::filesystem::path& shaderDirectory,
                                             const sliceCorpus::Fixture& header,
                                             const RawFrame& source,
                                             std::vector<pixel>& lumaOutput, std::array<std::vector<pixel>, 2>& chromaOutput,
                                             int width, int height, int qp, int chromaQp,
                                             const motionSearchCorpus::Fixture& search,
                                             bool inter, int pictureIndex, bool readReconstruction, bool inter8x8 = true,
                                              bool x264ModeSelection = false, bool longStart = false,
                                               const char* preset = nullptr) {
    const char* stageName = inter ? "resident_cabac_p_frame" : "resident_cabac_idr_frame";
    const HostTimer phase(stageName);
    auto& context = computeContext();
    const auto shaderPath = [&](const char* name) { return (shaderDirectory/name).string(); };
    constexpr std::size_t batchSize = 512, wordBytes = sizeof(std::uint32_t);
    const int columns = width/16, rows = height/16, macroblocks = columns*rows;

    const auto tablePrefix = cabacTables({}, 0);
    const int bankBase = static_cast<int>(tablePrefix.size());
    constexpr int fixtureWords = static_cast<int>(sizeof(cabacCorpus::FrameFixture)/sizeof(std::uint32_t));
    constexpr int fixtureByteOffset = static_cast<int>(offsetof(cabacCorpus::FrameFixture, bytes));
    // Sparse per-macroblock bin slots, dense compacted stream, and the scan meta.
    constexpr std::uint32_t binStride = 4096, binCapacity = 16777216;
    (void)binStride; (void)binCapacity;

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
    const auto* presetInfo = preset ? x264_vulkan::presets::find(preset) : nullptr;
    const bool useSad = presetInfo && presetInfo->subpelRefine <= 1;
    frameInput[13] = (inter ? 0u : 1u) | (x264ModeSelection ? 2u : 0u) | (useSad ? 4u : 0u);
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
    // The CABAC fixture header (six scalars plus one operation), the first
    // operation and the leading header byte are staged after the frame payload.
    constexpr int operationsOffsetWords = 6;
    const std::size_t fixtureStaging = frameUploadBytes;
    const std::size_t fixtureOpsStaging = fixtureStaging+9*wordBytes;
    const std::size_t fixtureByteStaging = fixtureOpsStaging+3*wordBytes;
    std::size_t cursor = fixtureByteStaging+wordBytes;
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
    auto l4 = reserve(batchSize*sizeof(intraCorpus::Fixture));
    auto l8 = reserve(batchSize*sizeof(inter8Corpus::Fixture));
    auto uv = reserve(2*batchSize*sizeof(chromaCorpus::Fixture));
    auto corpus = reserve((bankBase+cabacCorpus::frameResidualBankSize+fixtureWords)*wordBytes);

    frameInput[0] = columns;
    frameInput[1] = rows;
    frameInput[2] = static_cast<std::uint32_t>(motion.offset/wordBytes+motionInput[2]);
    frameInput[5] = static_cast<std::uint32_t>(motion.offset/wordBytes+motionInput[7]);
    frameInput[6] = static_cast<std::uint32_t>(corpus.offset/wordBytes+bankBase);
    frameInput[7] = static_cast<std::uint32_t>(filter.offset/wordBytes);
    frameInput[12] = static_cast<std::uint32_t>(motion.offset/wordBytes+referenceStart);
    frameInput[14] = 0;
    const auto referenceEnd = filter.offset+filterPixels;
    // Motion reads the previous chroma planes in place; the join dispatch
    // overwrites those planes only after the last motion wave has finished.
    motionInput[5] = static_cast<int>((referenceEnd-motion.offset)/wordBytes)+sourceWords;
    motionInput[6] = motionInput[5]+sourceWords/4;
    motion.range = filter.offset+filter.range-motion.offset;

    context.reserve(cursor);
    context.reserveDevice(cursor);
    const auto device = *context.device;
    const auto resident = *context.deviceBuffer, staging = *context.buffer;
    for (auto* view : {&motion, &filter, &l4, &l8, &uv, &corpus}) view->buffer = resident;
    const vk::DescriptorBufferInfo arena(resident, 0, cursor);
    const auto& sets = context.interSets;
    const vk::DescriptorType storage = vk::DescriptorType::eStorageBuffer;
    const vk::WriteDescriptorSet bindings[]{
        {sets[0], 0, 0, storage, {}, motion}, {sets[1], 0, 0, storage, {}, arena},
        {sets[1], 1, 0, storage, {}, l4}, {sets[1], 2, 0, storage, {}, l8}, {sets[1], 3, 0, storage, {}, uv},
        {sets[1], 4, 0, storage, {}, arena}, {sets[3], 0, 0, storage, {}, filter},
        {sets[2], 0, 0, storage, {}, corpus}, {sets[2], 1, 0, storage, {}, corpus},
        {sets[2], 2, 0, storage, {}, corpus}, {sets[2], 3, 0, storage, {}, corpus},
    };
    device.updateDescriptorSets(bindings, {});

    const std::uint32_t lastHeaderByte = header.bytes[header.pointer-1];
    // Direct CABAC frame mode: qp, model (inter), chroma444 carries the macroblock row
    // width, count carries the macroblock total and finish appends the flush.
    const std::uint32_t fixtureHeader[9]{
        static_cast<std::uint32_t>(qp), static_cast<std::uint32_t>(inter ? 1 : 0),
        static_cast<std::uint32_t>(columns), static_cast<std::uint32_t>(pictureIndex),
        static_cast<std::uint32_t>(macroblocks), 1, 0, 0, 0};
    // Whole-frame op (kind 15) drives every macroblock from the residual bank.
    const std::uint32_t fixtureOps[3]{
        static_cast<std::uint32_t>(cabacCorpus::wholeFrame),
        static_cast<std::uint32_t>(inter ? 1 : 0),
        (static_cast<std::uint32_t>(columns) << 16) |
            static_cast<std::uint32_t>(macroblocks)};
    const std::size_t fixtureBase = corpus.offset+(bankBase+cabacCorpus::frameResidualBankSize)*wordBytes;
    const vk::BufferCopy uploads[]{
        {0, 0, frameUploadBytes},
        {motion.offset, motion.offset, referenceStart*wordBytes},
        {filter.offset, filter.offset, filterInput.size()*wordBytes},
        {corpus.offset, corpus.offset, tablePrefix.size()*wordBytes},
        {fixtureStaging, fixtureBase, 9*wordBytes},
        {fixtureOpsStaging, fixtureBase+operationsOffsetWords*wordBytes, 3*wordBytes},
        {fixtureByteStaging, fixtureBase+fixtureByteOffset, wordBytes},
    };
    {
        const HostTimer stage("resident_cabac_staging_input");
        const auto copy = [&](std::size_t offset, const auto& data) {
            std::memcpy(context.mapped+offset, data.data(), data.size()*sizeof(data[0]));
        };
        copy(0, frameInput);
        copy(frameInput[3]*wordBytes, source[1]);
        copy(frameInput[4]*wordBytes, source[2]);
        copy(motion.offset, motionInput);
        copy(motion.offset+motionInput.size()*wordBytes, search.motionCost);
        copy(motion.offset+sourceStart*wordBytes, source[0]);
        copy(filter.offset, filterInput);
        copy(corpus.offset, tablePrefix);
        std::memcpy(context.mapped+fixtureStaging, fixtureHeader, sizeof(fixtureHeader));
        std::memcpy(context.mapped+fixtureOpsStaging, fixtureOps, sizeof(fixtureOps));
        std::memcpy(context.mapped+fixtureByteStaging, &lastHeaderByte, wordBytes);
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
    const auto cabac = context.pipeline(shaderPath("cabac_frame.spv").c_str());
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
    bind(cabac, 2);
    const std::uint32_t binGroups = 1;
    dispatch(binGroups, 0);
    timestamp(inter ? "resident_p_cabac_bins" : "resident_idr_cabac_bins");
    if (inter8x8) {
        bind(deblock, 3);
        const int tileRows = (rows+3)/4, tileColumns = (columns+rows+2)/4;
        for (int wave = 0; wave < tileRows+tileColumns-1; ++wave) dispatch(tileRows, wave);
        timestamp(inter ? "resident_p_deblock" : "resident_idr_deblock");
    } else {
        // baseline-profile encodes: x264 writes disable_deblocking_filter_idc=1 and runs no filter
        timestamp(inter ? "resident_p_deblock" : "resident_idr_deblock");
    }
    barrier(compute, transfer, shaderWrite, vk::AccessFlagBits::eTransferRead);
    constexpr auto positionOffset = static_cast<std::size_t>(offsetof(cabacCorpus::FrameFixture, position));
    const char* dumpDir = std::getenv("VK264_DUMP_DIR");
    const std::size_t bankStart = corpus.offset+bankBase*wordBytes;
    const std::size_t bankBytes = std::size_t(macroblocks)*cabacCorpus::macroblockWords*wordBytes;
    constexpr auto bytesOffset = static_cast<std::size_t>(offsetof(cabacCorpus::FrameFixture, bytes));
    const std::size_t fixtureBytes = fixtureBase+bytesOffset;
    const std::size_t positionStaging = frameUploadBytes;
    std::vector<vk::BufferCopy> downloads{
        {fixtureBase+positionOffset, positionStaging, wordBytes},
    };
    if (dumpDir) downloads.emplace_back(bankStart, bankStart, bankBytes);
    if (readReconstruction) downloads.emplace_back(referenceEnd, referenceEnd, pixels);
    cmd.copyBuffer(resident, staging, downloads);
    barrier(transfer, vk::PipelineStageFlagBits::eHost, vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead);
    barrier(compute, vk::PipelineStageFlagBits::eHost, shaderWrite, vk::AccessFlagBits::eHostRead);
    timestamp(inter ? "resident_p_download" : "resident_idr_download");
    cmd.end();
    const auto submitted = ComputeContext::Clock::now();
    context.queue.submit(vk::SubmitInfo({}, {}, cmd, {}));
    context.queue.waitIdle();
    auto completed = ComputeContext::Clock::now();
    std::size_t denseBytes = 0;
    const auto fixturePosition = *reinterpret_cast<const std::int32_t*>(context.mapped+positionStaging);
    if (fixturePosition <= 0) throw std::runtime_error("CABAC encoder reported an empty slice");
    if (fixturePosition > static_cast<std::int32_t>(cabacCorpus::frameMaxBytes))
        throw std::runtime_error("CABAC encoder reported an out-of-range slice");
    // Copy only the bytes actually emitted by the device.
    {
        device.resetCommandPool(*context.commandPool);
        cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
        cmd.copyBuffer(resident, staging,
                       vk::BufferCopy(fixtureBytes, positionStaging, std::size_t(fixturePosition)*wordBytes));
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                            {}, vk::MemoryBarrier(vk::AccessFlagBits::eTransferWrite,
                                                  vk::AccessFlagBits::eHostRead), {}, {});
        cmd.end();
        context.queue.submit(vk::SubmitInfo({}, {}, cmd, {}));
        context.queue.waitIdle();
        denseBytes = std::size_t(fixturePosition)*wordBytes;
        completed = ComputeContext::Clock::now();
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
    if (dumpDir) {
        std::ofstream out(std::string(dumpDir)+"/resident_bank_"+std::to_string(pictureIndex)+".bin", std::ios::binary);
        out.write(reinterpret_cast<const char*>(context.mapped+bankStart), std::streamsize(bankBytes));
    }
    std::vector<std::uint8_t> payload(header.bytes.begin(), header.bytes.begin()+header.pointer-1);
    {
        const HostTimer encode("cabac_device_entropy");
        // The bytes emitted by the device are copied verbatim into the Annex B payload.
        const auto* sliceWords = reinterpret_cast<const std::uint32_t*>(context.mapped+positionStaging);
        const std::size_t sliceBytes = static_cast<std::size_t>(fixturePosition);
        for (std::size_t i = 0; i < sliceBytes; ++i) {
            payload.push_back(static_cast<std::uint8_t>(sliceWords[i]));
        }
    }
    auto packet = packAnnexB(payload, static_cast<std::uint32_t>(header.nalRef), inter ? 1u : 5u, longStart);
    if (context.profiling) {
        auto& stats = context.profile[stageName];
        ++stats.calls;
        for (const auto& region : uploads) stats.uploadBytes += region.size;
        for (const auto& region : downloads) stats.readbackBytes += region.size;
        stats.readbackBytes += wordBytes+denseBytes;
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
    return {std::move(packet), static_cast<std::uint32_t>(macroblocks)};
}

// x264 common/base.c::x264_param_default() + param_apply_preset(): B-frame count
// and pyramid mode a preset targets. Only presets with bframes>0 need POC type 0
// and reorder slots (encoder/set.c::x264_sps_init). Kept local to parameter
// construction so no shared preset table is touched (issue #4 groundwork).
