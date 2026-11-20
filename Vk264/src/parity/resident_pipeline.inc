// Vk264 parity module: resident_pipeline.inc
// Mirrors: encodeCabacClip + public x264_vulkan::encode
// Split from dct_parity.cpp; see docs/parity-layout.md.

struct BFrameConfig { int frames, pyramid; };
static BFrameConfig x264BFrameConfig(const char* preset) {
    if (!preset) return {0, 0};
    // x264 defaults: i_bframe=3, i_bframe_pyramid=X264_B_PYRAMID_NORMAL(2).
    // ultrafast is the only accepted preset that disables B-frames; veryslow
    // and placebo raise the default count.
    if (std::strcmp(preset, "ultrafast") == 0) return {0, 0};
    if (std::strcmp(preset, "veryslow") == 0) return {8, 2};
    if (std::strcmp(preset, "placebo") == 0) return {16, 2};
    return {3, 2};
}

x264_vulkan::EncodeResult encodeCabacClip(const char* parameterShader, const char* sliceShader, const char* macroblockShader,
                     const char* chromaShader, const char* motionShader, const char* predictorShader,
                     const char* searchShader, const char* cabacShader, const char* nalShader,
                     const char* outputPath, const char* reconstructionPath, const RawClip* clip = nullptr,
                     const x264_vulkan::EncodeOptions* options = nullptr) {
    const auto startTime = std::chrono::steady_clock::now();
    const int visibleWidth = clip ? clip->cropWidth : 30, visibleHeight = clip ? clip->cropHeight : 30;
    if (visibleWidth < 2 || visibleHeight < 2 || visibleWidth > 2048 || visibleHeight > 2048 ||
        ((visibleWidth | visibleHeight) & 1))
        throw std::runtime_error("Expected even 4:2:0 crop dimensions up to 2048 per side");
    const int widthInMbs = (visibleWidth+15)/16, heightInMbs = (visibleHeight+15)/16;
    const int width = 16*widthInMbs, height = 16*heightInMbs, chromaWidth = width/2;
    const int macroblocks = widthInMbs*heightInMbs, intraPictures = clip ? 1 : 3;
    if (macroblocks > cabacCorpus::frameMaxOperations)
        throw std::runtime_error("The current CABAC slice path supports up to 8192 macroblocks");
    const int pictures = clip ? clip->frames : intraPictures+2;
    const int skippedPicture = clip ? -1 : intraPictures;
    const int initialQp = options ? options->initialQp : 26;
    const int gop = options ? options->gop : pictures;
    int qp = initialQp;
    // x264 encoder.c psy adjustments: subpel_refine>=6 enables psy_rd=1.00,
    // lowering chroma QP by 2 (see PPS below). This must also shift the
    // chroma quantizer used by the residual kernels, not just the PPS field.
    int chromaQpOffset = 0;
    if (options && options->preset) {
        if (const auto* preset = x264_vulkan::presets::find(options->preset))
            if (preset->subpelRefine >= 6) chromaQpOffset = -2;
    }
    // x264 set.c::x264_sps_init() uses i_frame_reference (the per-preset ref
    // budget) for both SPS max_num_ref_frames and level DPB selection. Consume
    // the value already transcribed in x264_presets.hpp instead of hard-coding 1.
    // x264 set.c::x264_sps_init() uses i_frame_reference (the per-preset ref
    // budget) for max_num_ref_frames and level DPB selection. Reconciled with
    // the B-frame header work (issue #4): one refBudget feeds both intents.
    int refBudget = 1;
    if (options && options->preset) {
        if (const auto* preset = x264_vulkan::presets::find(options->preset))
            refBudget = std::max(1, preset->referenceBudget);
    }
    constexpr int chromaQpMap[52] = {
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,
        29,30,31,32,32,33,34,34,35,35,36,36,37,37,37,38,38,38,39,39,39,39,
    };
    const auto chromaQp = [&](int q) {
        return chromaQpMap[std::clamp(q + chromaQpOffset, 0, 51)];
    };
    struct RateState { std::int32_t interQp, idrQp, bitDebt, budget, gop, fps, gopPosition, actualBits; };
    const int fps = options ? options->fpsNumerator / options->fpsDenominator : 60;
    std::vector<RateState> rate{{initialQp, initialQp, 0, options ? options->targetBits : 0, gop, fps, 0, 0}};
    const auto rateShader = std::filesystem::path(motionShader).replace_filename("rate_control.spv").string();
    const auto interFrame = [=](int i) { return clip ? i%gop != 0 : i >= intraPictures; };
    std::optional<RawClipReader> reader;
    if (clip) reader.emplace(*clip, width, height);
    x264_sps_t sps{};
    // x264 set.c::x264_sps_init(): profile from preset flags. Only
    // ultrafast disables CABAC, 8x8dct, B-frames and weighted prediction,
    // selecting Baseline; every other preset keeps the High defaults.
    const bool x264Baseline = options && options->preset &&
        std::strcmp(options->preset, "ultrafast") == 0;
    sps.i_profile_idc = x264Baseline ? PROFILE_BASELINE : PROFILE_HIGH;
    sps.b_constraint_set0 = x264Baseline ? 1 : 0;  // Baseline
    sps.b_constraint_set1 = x264Baseline ? 1 : 0;  // profile <= Main
    sps.b_constraint_set2 = 0;
    sps.b_constraint_set3 = 0;
    {
        x264_levels::LevelInput levelIn{};
        levelIn.mbsPerFrame = macroblocks;
        levelIn.widthInMbs = widthInMbs;
        levelIn.heightInMbs = heightInMbs;
        levelIn.maxDecFrameBuffering = gop == 1 ? 1 : refBudget;
        levelIn.fpsNum = options ? options->fpsNumerator : 60000;
        levelIn.fpsDen = options ? options->fpsDenominator : 1001;
        if (options && options->targetBits > 0) {
            // x264's ABR-VBV-equivalent: maxrate==bitrate (bufsize=2x). CBP_FACTOR for High profile = 5.
            levelIn.vbvKbps = options->targetBits * options->fpsNumerator / options->fpsDenominator / 1000;
            levelIn.cpbKbps = levelIn.vbvKbps * 2;
        }
        sps.i_level_idc = (options && options->levelIdc > 0)
            ? options->levelIdc
            : x264_levels::selectLevel(levelIn);
    }
    sps.i_chroma_format_idc = CHROMA_420;
    // x264 encoder/set.c::x264_sps_init(): presets with B-frames switch to POC
    // type 0, enlarge the DPB and signal reorder slots. This groundwork emits
    // the headers only; B-slices / DPB reordering remain outstanding (issue #4).
    // Gate on a real inter-GOP CABAC clip so every byte-exact path (gop==1,
    // ultrafast/CAVLC, synthetic fixtures) is untouched.
    const BFrameConfig bframes = x264BFrameConfig(options ? options->preset : nullptr);
    const bool bFrameHeaders = clip && gop > 1 && bframes.frames > 0 &&
                               (options == nullptr || options->cavlcCodes == nullptr);
    sps.i_log2_max_frame_num = 4;
    // Reconciled SPS-init block (Subaru's PR #11 review): Ram's refBudget
    // (preset i_frame_reference, #10) and the B-frame bFrameHeaders formula
    // (#11) are one set.c::x264_sps_init()-faithful expression. All existing
    // byte-exact paths (gop==1, ultrafast/CAVLC, bframes==0, fixtures) keep
    // their single-ref behaviour; only bframes>0 && gop>1 gets the DPB slots.
    sps.i_poc_type = bFrameHeaders ? 0 : 2;
    if (sps.i_poc_type == 0) {
        const int maxDeltaPoc = (bframes.frames + 2) * (bframes.pyramid ? 2 : 1) * 2;
        sps.i_log2_max_poc_lsb = 4;
        while ((1 << sps.i_log2_max_poc_lsb) <= maxDeltaPoc * 2) ++sps.i_log2_max_poc_lsb;
    }
    // x264 set.c::x264_sps_init(): intra-only streams have zero references.
    if (gop == 1) {
        sps.i_num_ref_frames = 0;
        sps.vui.i_num_reorder_frames = 0;
        sps.vui.i_max_dec_frame_buffering = 0;
    } else if (bFrameHeaders) {
        sps.vui.i_num_reorder_frames = bframes.pyramid ? 2 : (bframes.frames ? 1 : 0);
        sps.i_num_ref_frames = std::min(X264_REF_MAX, std::max({refBudget,
            1 + sps.vui.i_num_reorder_frames, bframes.pyramid ? 4 : 1, 0}));
        sps.vui.i_max_dec_frame_buffering = sps.i_num_ref_frames;
        const int maxFrameNum = sps.vui.i_max_dec_frame_buffering * (bframes.pyramid ? 2 : 1) + 1;
        sps.i_log2_max_frame_num = 4;
        while ((1 << sps.i_log2_max_frame_num) <= maxFrameNum) ++sps.i_log2_max_frame_num;
    } else {
        // No B-frames (or fixture): max_num_ref_frames is the preset budget.
        sps.i_num_ref_frames = refBudget;
        sps.vui.i_num_reorder_frames = 0;
        sps.vui.i_max_dec_frame_buffering = refBudget;
    }
    // x264's x264_sps_init(): intra-only streams (keyint==1) with High profile
    // set constraint_set3, which in turn suppresses VUI bitstream_restriction.
    sps.b_constraint_set3 = (gop == 1 && sps.i_profile_idc >= PROFILE_HIGH) ? 1 : 0;
    sps.i_mb_width = widthInMbs;
    sps.i_mb_height = heightInMbs;
    sps.b_frame_mbs_only = 1;
    sps.b_direct8x8_inference = 1;
    sps.b_crop = width != visibleWidth || height != visibleHeight;
    sps.crop.i_right = width-visibleWidth;
    sps.crop.i_bottom = height-visibleHeight;
    sps.b_vui = 1;
    // Write the BT.709 default colour description unless the caller passed a
    // different colourimetry (matches x264's coloured-signal emission for a
    // raw .yuv input with the harness metadata default).
    sps.vui.b_signal_type_present = 1;
    sps.vui.i_vidformat = 5;
    sps.vui.b_color_description_present = 1;
    sps.vui.i_colorprim = sps.vui.i_transfer = sps.vui.i_colmatrix = 1; // BT.709
    if (options && options->videoSignal) {
        const auto signal = options->videoSignal;
        sps.vui.b_fullrange = signal & 1;
        sps.vui.i_colorprim = (signal >> 8) & 255;
        sps.vui.i_transfer = (signal >> 16) & 255;
        sps.vui.i_colmatrix = (signal >> 24) & 255;
    }
    sps.vui.b_timing_info_present = 1;
    sps.vui.i_num_units_in_tick = options ? options->fpsDenominator : 1;
    sps.vui.i_time_scale = options ? 2u*options->fpsNumerator : 120;
    sps.vui.b_fixed_frame_rate = 1;
    // X264 writes this block for all profiles except constrained High (level-1b).
    sps.vui.b_bitstream_restriction = !(sps.b_constraint_set3 && sps.i_profile_idc >= PROFILE_HIGH);
    if (sps.vui.b_bitstream_restriction != 0) {
        sps.vui.b_motion_vectors_over_pic_boundaries = 1;
        sps.vui.i_max_bytes_per_pic_denom = 0;
        sps.vui.i_max_bits_per_mb_denom = 0;
        const auto* lvl = x264_levels::findLevel(sps.i_level_idc);
        const int mvLog2 = lvl ? x264_levels::mvLengthLog2(lvl->mv_range) : 9;
        sps.vui.i_log2_max_mv_length_horizontal = mvLog2;
        sps.vui.i_log2_max_mv_length_vertical = mvLog2;
        // bFrameHeaders already set the x264 reorder/buffering values above;
        // only single-ref streams use the legacy defaults here.
        if (!bFrameHeaders) {
            sps.vui.i_num_reorder_frames = 0;
            sps.vui.i_max_dec_frame_buffering = sps.i_num_ref_frames;
        }
    }

    const bool baselineProfile = sps.i_profile_idc == PROFILE_BASELINE;
    x264_pps_t pps{};
    const auto* cavlcCodes = options ? options->cavlcCodes : nullptr;
    const auto cavlcShader = std::filesystem::path(cabacShader).replace_filename("cavlc_frame.spv").string();
    // Both entropy modes use the resident single-command-buffer pipeline for
    // real clips; the staged host-orchestrated path remains for the synthetic
    // parity fixtures that exercise the per-macroblock operation stream.
    const bool resident = cavlcCodes != nullptr ||
        (clip != nullptr && std::getenv("VK264_STAGED_CABAC") == nullptr);
    pps.b_cabac = cavlcCodes == nullptr;
    // Keep 8x8 transform mode enabled regardless of profile: the CAVLC and
    // CABAC residual kernels emit 8x8 block syntax in the shipped pipeline.
    pps.b_transform_8x8_mode = clip != nullptr && !x264Baseline;
    pps.i_num_slice_groups = 1;
    // x264 encoder/set.c::x264_pps_init(): progressive streams leave
    // bottom_field_pic_order off; l0 default active mirrors frame_reference
    // while l1 stays at one.
    pps.b_pic_order = 0;
    pps.i_num_ref_idx_l0_default_active = bFrameHeaders ? std::max(1, refBudget) : 1;
    pps.i_num_ref_idx_l1_default_active = 1;
    pps.i_pic_init_qp = options && options->targetBits > 0 ? 26 : initialQp;
    pps.i_pic_init_qs = 26;
    // x264 encoder.c::x264_encoder_open() psy adjustments: psy_rd defaults to
    // 1.00 and is active when subpel_refine >= 6, which lowers the chroma QP
    // offset by 2.  psy_trellis defaults to 0 and 4:2:0 adds nothing, so this
    // is the only adjustment for any preset.  Mirrors libx264's PPS exactly.
    pps.i_chroma_qp_index_offset = chromaQpOffset;
    // x264 encoder/set.c::x264_pps_init(): weighted_pred follows weightp>0 and
    // weighted_bipred is 2 for the default b_weighted_bipred=1 (fast and above).
    // The P-slice header writer already emits the matching all-zero pred table.
    pps.b_weighted_pred = bFrameHeaders ? 1 : 0;
    pps.b_weighted_bipred = bFrameHeaders ? 2 : 0;
    pps.b_deblocking_filter_control = 1;

    std::vector<parameterCorpus::Fixture> parameters(2);
    for (int kind = 0; kind < 2; ++kind) {
        parameters[kind].kind = kind;
        parameters[kind].bitDepth = 8;
        parameterCorpus::store(parameters[kind], sps, pps);
    }
    execute(parameterShader, parameters, 1, parameterCorpus::tables());

    std::vector<sliceCorpus::Fixture> header(1);
    const auto pictureHeader = [&](int i, bool inter) {
        x264_slice_header_t sh{};
        sh.sps = &sps;
        sh.pps = &pps;
        sh.i_type = inter ? SLICE_TYPE_P : SLICE_TYPE_I;
        sh.i_idr_pic_id = inter ? -1 : (clip ? i/gop : i)%2;
        sh.i_frame_num = inter ? (clip ? i%gop : i-intraPictures+1) : 0;
        // x264 encoder/encoder.c: with POC type 0 the slice carries
        // pic_order_cnt_lsb = 2 * display index. Our scaffold is still P-only,
        // so P slices override the PPS default down to one active ref exactly
        // as x264-fast does when only one reference is available.
        sh.i_poc = bFrameHeaders ? 2*sh.i_frame_num : 0;
        sh.i_delta_poc_bottom = 0;
        sh.i_num_ref_idx_l0_active = 1;
        sh.b_num_ref_idx_override = (bFrameHeaders && inter) ? 1 : 0;
        sh.i_qp_delta = qp - pps.i_pic_init_qp;
        sh.i_disable_deblocking_filter_idc = clip ? (baselineProfile ? 1 : 0) : 1;
        header.front() = {};
        // x264 encoder/encoder.c: IDR uses NAL_PRIORITY_HIGHEST (3), reference
        // P/I pictures NAL_PRIORITY_HIGH (2). Gated with bFrameHeaders so the
        // byte-exact single-frame/CAVLC paths keep their existing header.
        header.front().nalRef = (bFrameHeaders && inter) ? 2 : 3;
        header.front().alignCabac = pps.b_cabac;
        sliceCorpus::store(header.front(), sh);
        execute(sliceShader, header);
    };

    const auto headerBytes = [](const auto& header, int length) {
        if (length <= 0 || static_cast<std::size_t>(length) > header.bytes.size())
            throw std::runtime_error("Invalid aligned RBSP header length");
        return std::vector<std::uint8_t>(header.bytes.begin(), header.bytes.begin()+length);
    };
    // Issue #5 sub-PR C groundwork: size the host reconstruction ring to
    // (refs + 1) slots so older frames can be retained once the resident GPU
    // path is extended to address them. For ref=1 this is identical to the
    // previous 2-slot modulo-2 ring; no behaviour change for single-ref.
    const int referenceSlots = clip ? std::max(2, refBudget+1) : pictures;
    const int storedPictures = referenceSlots;
    const auto storageIndex = [clip, referenceSlots](int picture) {
        return clip ? picture%referenceSlots : picture;
    };
    std::vector<std::vector<std::uint8_t>> reconstructed(storedPictures, std::vector<std::uint8_t>(width*height));
    std::array<std::vector<std::uint8_t>, 2> chromaPlanes{
        std::vector<std::uint8_t>(width*height/4), std::vector<std::uint8_t>(width*height/4)};
    std::vector<std::array<std::vector<std::uint8_t>, 2>> reconstructedChroma(storedPictures, chromaPlanes);
    std::vector<std::vector<intraCorpus::Fixture>> encoded(intraPictures,
        std::vector<intraCorpus::Fixture>(macroblocks));
    using ChromaPair = std::array<chromaCorpus::Fixture, 2>;
    std::vector<std::vector<ChromaPair>> encodedChroma(intraPictures, std::vector<ChromaPair>(macroblocks));
    const auto encodeIntra = [&](int picture) {
        const HostTimer phase("idr_prediction_and_reconstruction");
        const auto source = reader ? reader->read(picture) : RawFrame{};
        for (const auto& wave : macroblockWaves(widthInMbs, heightInMbs, 1)) {
            const auto count = wave.size()*intraPictures;
            std::vector<intraCorpus::Fixture> work(count);
            std::vector<chromaCorpus::Fixture> chromaWork(2*count);
            for (std::size_t slot = 0; slot < wave.size(); ++slot) {
                const int mb = wave[slot], mbX = mb%widthInMbs, mbY = mb/widthInMbs;
                const int start = 16*(mbY*width+mbX), chromaStart = 8*(mbY*chromaWidth+mbX);
                const auto base = slot*intraPictures;
                for (int i = 0; i < intraPictures; ++i) {
                    auto& block = work[base+i];
                    auto& frame = reconstructed[clip ? storageIndex(picture) : i];
                    block.qp = qp;
                    block.decimate = 1;
                    intraCorpus::setFlatQuant(block, 21);
                    block.mode = mbX && mbY ? I_PRED_16x16_DC : mbX ? I_PRED_16x16_DC_LEFT :
                                 mbY ? I_PRED_16x16_DC_TOP : I_PRED_16x16_DC_128;
                    for (int y = 0; y < 16; ++y) {
                        if (mbX) block.pixels[intraCorpus::origin+y*FDEC_STRIDE-1] = frame[start+y*width-1];
                        for (int x = 0; x < 16; ++x) {
                            const int sx = 16*mbX+x, sy = 16*mbY+y;
                            const int texture = (sx*5+sy*3+(sx/4 ^ sy/4)*7)%53;
                            block.source[y*16+x] = clip ? source[0][sy*width+sx] : i == 0 ? 128 : i == 1 ? 141 : 89+texture;
                        }
                    }
                    if (mbY)
                        std::copy_n(frame.begin()+start-width, 16, block.pixels.begin()+intraCorpus::origin-FDEC_STRIDE);
                }
                for (int i = 0; i < intraPictures; ++i) {
                    for (int plane = 0; plane < 2; ++plane) {
                        auto& block = chromaWork[2*(base+i)+plane];
                        const auto& samples = reconstructedChroma[clip ? storageIndex(picture) : i][plane];
                        block.qp = chromaQp(qp);
                        chromaCorpus::configure(block);
                        block.mode = mbX && mbY ? I_PRED_CHROMA_DC : mbX ? I_PRED_CHROMA_DC_LEFT :
                                     mbY ? I_PRED_CHROMA_DC_TOP : I_PRED_CHROMA_DC_128;
                        for (int y = 0; y < 8; ++y) {
                            if (mbX)
                                block.pixels[intraCorpus::origin+y*FDEC_STRIDE-1] = samples[chromaStart+y*chromaWidth-1];
                            for (int x = 0; x < 8; ++x) {
                                const int sx = 8*mbX+x, sy = 8*mbY+y;
                                const int texture = (sx*7+sy*5+plane*13)%41;
                                block.source[y*8+x] = clip ? source[plane+1][sy*chromaWidth+sx] :
                                                      i == 0 ? 128 : i == 1 ? 105+50*plane : 90+28*plane+texture;
                            }
                        }
                        if (mbY)
                            std::copy_n(samples.begin()+chromaStart-chromaWidth, 8,
                                        block.pixels.begin()+intraCorpus::origin-FDEC_STRIDE);
                    }
                }
            }
            execute(macroblockShader, work, 1, {}, 0, {}, 0, false, 0, "idr_luma");
            execute(chromaShader, chromaWork, 1, {}, count, {}, 0, false, 0, "idr_chroma");
            for (std::size_t slot = 0; slot < wave.size(); ++slot) {
                const int mb = wave[slot], mbX = mb%widthInMbs, mbY = mb/widthInMbs;
                const int start = 16*(mbY*width+mbX), chromaStart = 8*(mbY*chromaWidth+mbX);
                const auto base = slot*intraPictures;
                for (int i = 0; i < intraPictures; ++i) {
                    for (int y = 0; y < 16; ++y)
                        std::copy_n(work[base+i].pixels.begin()+intraCorpus::origin+y*FDEC_STRIDE, 16,
                                    reconstructed[clip ? storageIndex(picture) : i].begin()+start+y*width);
                    encoded[i][mb] = work[base+i];
                }
                for (int i = 0; i < intraPictures; ++i) {
                    for (int plane = 0; plane < 2; ++plane) {
                        const auto& block = chromaWork[2*(base+i)+plane];
                        for (int y = 0; y < 8; ++y)
                            std::copy_n(block.pixels.begin()+intraCorpus::origin+y*FDEC_STRIDE, 8,
                                            reconstructedChroma[clip ? storageIndex(picture) : i][plane].begin()+chromaStart+y*chromaWidth);
                        encodedChroma[i][mb][plane] = block;
                    }
                }
            }
        }
    };
    if (!clip) encodeIntra(0);
    if (skippedPicture >= 0) {
        reconstructed[skippedPicture] = reconstructed[intraPictures-1];
        reconstructedChroma[skippedPicture] = reconstructedChroma[intraPictures-1];
    }
    struct InterPicture {
        std::vector<mvPredictCorpus::Pair> vectors;
        std::vector<intraCorpus::Fixture> luma;
        std::vector<chromaCorpus::Fixture> chroma;
        std::vector<bool> transform8x8;
    };
    const auto encodeInter = [&](int picture) {
        const HostTimer phase("p_prediction_and_reconstruction");
        const int referencePicture = storageIndex(picture-1), currentPicture = storageIndex(picture);
        const auto source = reader ? reader->read(picture) : RawFrame{};
        const auto referenceShader = std::filesystem::path(motionShader).replace_filename("reference_frame.spv").string();
        // Seed the synthetic source with motion, then search the resulting samples independently.
        constexpr int sourceMotion[4][2]{{0,0}, {-3,7}, {6,3}, {-5,-4}};
        InterPicture frame{std::vector<mvPredictCorpus::Pair>(macroblocks),
                           std::vector<intraCorpus::Fixture>(macroblocks),
                           std::vector<chromaCorpus::Fixture>(2*macroblocks),
                           std::vector<bool>(macroblocks)};
        const auto metricsShader = std::filesystem::path(motionShader).replace_filename("motion_metrics.spv").string();
        const auto inter8Shader = std::filesystem::path(motionShader).replace_filename("inter8x8.spv").string();
        const auto searchParameters = motionSearchParameters(qp, options ? options->preset : nullptr);
        bool hasFractional = false;
        std::vector<FrameMotionBlock> frameMotion;
        auto batches = macroblockWaves(widthInMbs, heightInMbs, 2);
        if (clip) {
            const auto frameShader = std::filesystem::path(motionShader).replace_filename("motion_frame.spv").string();
            frameMotion = predictInterFrame(source, reconstructed[referencePicture], reconstructedChroma[referencePicture],
                                            width, height, searchParameters, referenceShader.c_str(), frameShader.c_str());
            constexpr int batchSize = 512;
            batches.clear();
            for (int first = 0; first < macroblocks; first += batchSize) {
                auto& batch = batches.emplace_back();
                for (int mb = first; mb < std::min(first+batchSize, macroblocks); ++mb) batch.push_back(mb);
            }
        }
        struct Prediction {
            std::array<std::uint32_t, motionCorpus::lumaStride*16> luma;
            std::array<std::uint32_t, motionCorpus::chromaStride*16> chromaU, chromaV;
        };
        for (const auto& wave : batches) {
            std::vector<Prediction> predictions(wave.size());
            std::vector<std::array<std::uint32_t, 256>> interSource(wave.size());
            std::vector<std::array<std::array<std::uint32_t, 64>, 2>> interChromaSource(wave.size());
            if (clip) {
                for (std::size_t slot = 0; slot < wave.size(); ++slot) {
                    const int mb = wave[slot], x = mb%widthInMbs, y = mb/widthInMbs;
                    const auto& block = frameMotion[mb];
                    frame.vectors[mb] = block.vector;
                    const auto sample = [&block](int index) { return (block.prediction[index/4] >> (8*(index&3))) & 255; };
                    for (int row = 0; row < 16; ++row)
                        for (int col = 0; col < 16; ++col) {
                            predictions[slot].luma[row*motionCorpus::lumaStride+col] = sample(16*row+col);
                            interSource[slot][16*row+col] = source[0][(16*y+row)*width+16*x+col];
                        }
                    for (int plane = 0; plane < 2; ++plane) {
                        auto& prediction = plane == 0 ? predictions[slot].chromaU : predictions[slot].chromaV;
                        for (int row = 0; row < 8; ++row)
                            for (int col = 0; col < 8; ++col) {
                                prediction[row*motionCorpus::chromaStride+col] = sample(256+64*plane+8*row+col);
                                interChromaSource[slot][plane][8*row+col] = source[plane+1][(8*y+row)*chromaWidth+8*x+col];
                            }
                    }
                }
            } else {
                std::vector<motionCorpus::Fixture> fixtures(wave.size());
                for (std::size_t slot = 0; slot < wave.size(); ++slot) {
                    const int mb = wave[slot];
                    auto& block = fixtures[slot];
                    block.lumaX = block.chromaX = sourceMotion[mb][0];
                    block.lumaY = block.chromaY = sourceMotion[mb][1];
                    block.lumaWidth = block.lumaHeight = 16;
                    block.chromaWidth = block.chromaHeight = 8;
                    constexpr int padStride = motionCorpus::stride;
                    constexpr int originX = motionCorpus::referenceOrigin%padStride;
                    constexpr int originY = motionCorpus::referenceOrigin/padStride;
                    const int mbX = mb%widthInMbs, mbY = mb/widthInMbs;
                    for (int y = 0; y < padStride; ++y) {
                        const int sy = std::clamp(16*mbY+y-originY, 0, height-1);
                        const int cy = std::clamp(8*mbY+y-originY, 0, height/2-1);
                        for (int x = 0; x < padStride; ++x) {
                            const int sx = std::clamp(16*mbX+x-originX, 0, width-1);
                            const int cx = std::clamp(8*mbX+x/2-originX/2, 0, chromaWidth-1);
                            block.source[y*padStride+x] = reconstructed[referencePicture][sy*width+sx];
                            block.chroma[y*padStride+x] =
                                reconstructedChroma[referencePicture][x%2][cy*chromaWidth+cx];
                        }
                    }
                }
                using Motion = motionCorpus::Fixture;
                const ReadbackSpan filtered{offsetof(Motion, hpel), sizeof(Motion::hpel)};
                const ReadbackSpan lumaPrediction{offsetof(Motion, luma), sizeof(Motion::luma)};
                const ReadbackSpan chromaPrediction{offsetof(Motion, chromaU), sizeof(Motion::chromaU)+sizeof(Motion::chromaV)};
                execute(motionShader, fixtures, 64, {}, 0, {filtered, lumaPrediction, chromaPrediction});
                for (std::size_t slot = 0; slot < wave.size(); ++slot) {
                    const int mb = wave[slot];
                    for (int y = 0; y < 16; ++y)
                        for (int x = 0; x < 16; ++x) {
                            const int prior = fixtures[slot].luma[y*motionCorpus::lumaStride+x];
                            const int residual = mb >= 2 ? (11*x+17*y)%37-18 : 0;
                            interSource[slot][y*16+x] = std::clamp(prior+residual, 0, 255);
                        }
                    for (int plane = 0; plane < 2; ++plane) {
                        const auto& samples = plane == 0 ? fixtures[slot].chromaU : fixtures[slot].chromaV;
                        for (int y = 0; y < 8; ++y)
                            for (int x = 0; x < 8; ++x) {
                                const int prior = samples[y*motionCorpus::chromaStride+x];
                                const int residual = mb == 0 ? 0 : mb == 1 ? 25 : (19*x+27*y+21*plane)%63-31;
                                interChromaSource[slot][plane][y*8+x] = std::clamp(prior+residual, 0, 255);
                            }
                    }
                }

                std::vector<mvPredictCorpus::Fixture> predictor(wave.size());
                for (std::size_t slot = 0; slot < wave.size(); ++slot) {
                    const int mb = wave[slot];
                    auto& spatial = predictor[slot];
                    const int x = mb%widthInMbs, y = mb/widthInMbs;
                    const int neighbors[]{x > 0 ? mb-1 : -1, y > 0 ? mb-widthInMbs : -1,
                        y > 0 && x+1 < widthInMbs ? mb-widthInMbs+1 : -1,
                        y > 0 && x > 0 ? mb-widthInMbs-1 : -1};
                    for (int i = 0; i < 4; ++i) {
                        spatial.neighbors[i] = neighbors[i] < 0 ? -2 : 0;
                        spatial.motion[i] = neighbors[i] < 0 ? mvPredictCorpus::Pair{} : frame.vectors[neighbors[i]];
                    }
                }
                using Spatial = mvPredictCorpus::Fixture;
                execute(predictorShader, predictor, 1, {}, 0, {{offsetof(Spatial, prediction), sizeof(Spatial::prediction)}});
                std::vector<motionSearchCorpus::Fixture> search(wave.size(), searchParameters);
                for (std::size_t slot = 0; slot < wave.size(); ++slot) {
                    auto& candidate = search[slot];
                    const auto& motion = fixtures[slot];
                    const auto& spatial = predictor[slot];
                    candidate.mvp = spatial.prediction;
                    candidate.source = interSource[slot];
                    std::copy(motion.source.begin(), motion.source.end(), candidate.reference.begin());
                    std::copy(motion.hpel.begin(), motion.hpel.end(), candidate.reference.begin()+motionCorpus::planeSize);
                }
                using Search = motionSearchCorpus::Fixture;
                execute(searchShader, search, 1, {}, 0, {{offsetof(Search, resultX), 2*sizeof(std::int32_t)}});
                for (std::size_t slot = 0; slot < wave.size(); ++slot) {
                    const int mb = wave[slot];
                    auto& motion = fixtures[slot];
                    const auto& candidate = search[slot];
                    frame.vectors[mb] = {candidate.resultX, candidate.resultY};
                    hasFractional |= ((candidate.resultX | candidate.resultY) & 3) != 0;
                    motion.lumaX = motion.chromaX = candidate.resultX;
                    motion.lumaY = motion.chromaY = candidate.resultY;
                    motion.prefiltered = 1;
                }
                execute(motionShader, fixtures, 64, {}, 0, {lumaPrediction, chromaPrediction});
                for (std::size_t slot = 0; slot < wave.size(); ++slot)
                    predictions[slot] = {fixtures[slot].luma, fixtures[slot].chromaU, fixtures[slot].chromaV};
            }
            std::vector<intraCorpus::Fixture> interLuma(wave.size());
            for (std::size_t slot = 0; slot < wave.size(); ++slot) {
                auto& block = interLuma[slot];
                block.mode = -1;
                block.qp = qp;
                block.decimate = 1;
                intraCorpus::setFlatQuant(block, 11);
                for (int y = 0; y < 16; ++y)
                    for (int x = 0; x < 16; ++x) {
                        const int prior = predictions[slot].luma[y*motionCorpus::lumaStride+x];
                        block.pixels[intraCorpus::origin+y*FDEC_STRIDE+x] = prior;
                        block.source[y*16+x] = interSource[slot][y*16+x];
                    }
            }
            std::vector<inter8Corpus::Fixture> inter8;
            std::vector<std::size_t> selected;
            if (clip) {
                std::vector<motionMetricsCorpus::Fixture> metrics(wave.size());
                for (std::size_t slot = 0; slot < wave.size(); ++slot) {
                    auto& input = metrics[slot];
                    input.width = input.height = 16;
                    input.count = 1;
                    input.source = interLuma[slot].source;
                    std::copy(predictions[slot].luma.begin(), predictions[slot].luma.end(), input.candidate.begin());
                }
                execute(metricsShader.c_str(), metrics, 4);
                for (std::size_t slot = 0; slot < wave.size(); ++slot) {
                    const auto& metric = metrics[slot];
                    // x264's non-RD decision in mb_analyse_transform.
                    if (metric.sa8d[0] >= metric.satd[0]) continue;
                    const auto& source = interLuma[slot];
                    auto& block = inter8.emplace_back();
                    block.qp = qp;
                    block.decimate = source.decimate;
                    block.source = source.source;
                    block.pixels = source.pixels;
                    inter8Corpus::configure(block);
                    selected.push_back(slot);
                }
            }
            execute(macroblockShader, interLuma, 1, {}, 0, {}, 0, false, 0, "p_luma_4x4");
            if (!inter8.empty()) {
                execute(inter8Shader.c_str(), inter8);
                for (std::size_t i = 0; i < selected.size(); ++i) {
                    auto& chosen = interLuma[selected[i]];
                    const auto& block = inter8[i];
                    chosen.cbp = block.cbp;
                    chosen.ac = block.coefficients;
                    chosen.nonzero = block.nonzero;
                    chosen.pixels = block.pixels;
                    frame.transform8x8[wave[selected[i]]] = block.cbp != 0;
                }
            }
            for (std::size_t slot = 0; slot < wave.size(); ++slot) {
                const int mb = wave[slot];
                frame.luma[mb] = interLuma[slot];
                const int start = 16*((mb/widthInMbs)*width+mb%widthInMbs);
                for (int y = 0; y < 16; ++y)
                    std::copy_n(interLuma[slot].pixels.begin()+intraCorpus::origin+y*FDEC_STRIDE, 16,
                                reconstructed[currentPicture].begin()+start+y*width);
            }
            std::vector<chromaCorpus::Fixture> interWork(2*wave.size());
            for (std::size_t slot = 0; slot < wave.size(); ++slot) {
                for (int plane = 0; plane < 2; ++plane) {
                    auto& block = interWork[2*slot+plane];
                    block.mode = -1;
                    block.qp = chromaQp(qp);
                    block.decimate = 1;
                    chromaCorpus::configure(block);
                    const auto& prediction = plane == 0 ? predictions[slot].chromaU : predictions[slot].chromaV;
                    for (int y = 0; y < 8; ++y) {
                        for (int x = 0; x < 8; ++x) {
                            const int prior = prediction[y*motionCorpus::chromaStride+x];
                            block.pixels[intraCorpus::origin+y*FDEC_STRIDE+x] = prior;
                            block.source[y*8+x] = interChromaSource[slot][plane][y*8+x];
                        }
                    }
                }
            }
            execute(chromaShader, interWork, 1, {}, wave.size(), {}, 0, false, 0, "p_chroma");
            for (std::size_t slot = 0; slot < wave.size(); ++slot) {
                const int mb = wave[slot];
                const int start = 8*((mb/widthInMbs)*chromaWidth+mb%widthInMbs);
                for (int plane = 0; plane < 2; ++plane) {
                    const auto& block = interWork[2*slot+plane];
                    frame.chroma[2*mb+plane] = block;
                    for (int y = 0; y < 8; ++y)
                        std::copy_n(block.pixels.begin()+intraCorpus::origin+y*FDEC_STRIDE, 8,
                                    reconstructedChroma[currentPicture][plane].begin()+start+y*chromaWidth);
                }
            }
        }
        if (!clip && !hasFractional)
            throw std::runtime_error("The P-picture motion search did not exercise fractional interpolation");
        return frame;
    };

    std::vector<cabacCorpus::FrameFixture> body(1);
    std::vector<std::int32_t> levels;
    int residualCounts[3]{};
    std::size_t codedInter8x8 = 0;
    int chromaResidualCounts[2][3]{};
    const auto lumaMask = [](const intraCorpus::Fixture& block) {
        int mask = 0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                mask |= static_cast<int>(block.nonzero[12+8*y+x]) << (4*y+x);
        return mask;
    };
    const auto appendChroma = [&levels](const chromaCorpus::Fixture* chroma) {
        const int cbp = std::max(chroma[0].cbp, chroma[1].cbp);
        levels.insert(levels.end(), {cbp, chroma[0].mode});
        for (int p = 0; p < 2; ++p) {
            const auto& plane = chroma[p];
            levels.insert(levels.end(), {plane.dcNonzero, plane.acMask});
            levels.insert(levels.end(), plane.dc.begin(), plane.dc.begin()+4);
            for (int b = 0; b < 4; ++b)
                levels.insert(levels.end(), plane.ac.begin()+16*b+1, plane.ac.begin()+16*(b+1));
        }
        return cbp;
    };
    const std::filesystem::path streamTemp = std::string(outputPath)+".part";
    const std::filesystem::path reconstructionTemp = reconstructionPath
        ? std::filesystem::path(std::string(reconstructionPath)+".part") : std::filesystem::path{};
    if (streamTemp == reconstructionTemp) throw std::runtime_error("Output and reconstruction paths must differ");
    std::ofstream output(streamTemp, std::ios::binary | std::ios::trunc);
    std::ofstream reconstruction;
    if (reconstructionPath) reconstruction.open(reconstructionTemp, std::ios::binary | std::ios::trunc);
    if (!output || !reconstruction) throw std::runtime_error("Cannot open GPU encode outputs");
    std::vector<NalPayload> nals;
    x264_vulkan::EncodeResult result{0, 0, 51, 0, 0, computeContext().physical.getProperties().deviceName.data()};
    for (int i = 0; i < pictures; ++i) {
        const bool inter = interFrame(i);
        qp = !inter && gop > 1 ? rate.front().idrQp : rate.front().interQp;
        // x264 ratecontrol.c::x264_init_rc_params(): in CQP mode
        //   qp_constant[I] = clip( i_qp_constant - ip_offset + 0.5 )
        // where ip_offset = 6*log2(ip_ratio=1.40) ~= 2.91, i.e. qp_I = max(0, qp-3).
        // ABR's idrQp already carries the offset, so only adjust fixed-QP runs.
        if (!inter && options && options->targetBits <= 0)
            qp = std::max(0, qp - 3);
        result.minQp = std::min(result.minQp, qp);
        result.maxQp = std::max(result.maxQp, qp);
        pictureHeader(i, inter);
        if (clip && !inter && !resident) encodeIntra(i);
        HostTimer phase("coefficient_pack_and_entropy");
        if (i == 0 || (clip && !inter)) {
            nals.push_back({headerBytes(parameters[0], parameters[0].position), 1, 1, 3, 7, 0, 0, 0});
            nals.push_back({headerBytes(parameters[1], parameters[1].position), 1, 1, 3, 8, 0, 0, 0});
        }
        auto& slice = body.front();
        slice.count = 0;
        levels.clear();
        std::vector<std::uint8_t> bytes;
        if (!cavlcCodes) {
            bytes = headerBytes(header.front(), header.front().pointer);
            slice.bytes[0] = bytes.back(); // CABAC carry may update the last header byte.
        }
        slice.qp = qp;
        slice.frame = i;
        slice.finish = 1;
        slice.model = int(inter); // P/B CABAC model zero, signaled by the slice header.
        if (i == skippedPicture) {
            for (int index = 0; index < macroblocks; ++index) {
                // All available neighbors are skipped, so the skip context stays at 11.
                slice.operations[slice.count++] = {cabacCorpus::macroblockSkip, 0, index+1 < macroblocks ? 3 : 1};
            }
        } else if (inter && !resident) {
            phase.finish();
            const auto picture = encodeInter(i);
            HostTimer pack("p_coefficient_pack");
            for (int index = 0; index < macroblocks; ++index) {
                const auto& luma = picture.luma[index];
                const bool transform8x8 = picture.transform8x8[index];
                codedInter8x8 += transform8x8;
                const int flags = (int(clip != nullptr) << 17) | (int(transform8x8) << 16);
                levels.insert(levels.end(), {picture.vectors[index][0], luma.cbp,
                                             picture.vectors[index][1], lumaMask(luma) | flags});
                levels.insert(levels.end(), luma.ac.begin(), luma.ac.end());
                ++chromaResidualCounts[1][appendChroma(&picture.chroma[2*index])];
                slice.operations[slice.count++] = {cabacCorpus::inter16Macroblock, 0,
                                                   (widthInMbs << 16) | index};
            }
        } else if (!inter && !resident) {
            for (int index = 0; index < macroblocks; ++index) {
                const int intraIndex = clip ? 0 : i;
                const auto& mb = encoded[intraIndex][index];
                const bool hasDc = mb.nonzero[0] != 0;
                const bool hasAc = mb.cbp != 0;
                ++residualCounts[hasAc ? 2 : hasDc ? 1 : 0];
                levels.insert(levels.end(), {mb.mode, int(hasAc), int(hasDc), lumaMask(mb)});
                levels.insert(levels.end(), mb.dc.begin(), mb.dc.end());
                for (int b = 0; b < 16; ++b)
                    levels.insert(levels.end(), mb.ac.begin()+16*b+1, mb.ac.begin()+16*(b+1));
                ++chromaResidualCounts[0][appendChroma(encodedChroma[intraIndex][index].data())];
                slice.operations[slice.count++] = {cabacCorpus::intra16Macroblock, 0,
                                                   (widthInMbs << 16) | index};
            }
        }
        phase.finish();
        HostTimer entropy("entropy_and_bitstream_pack");
        if (resident) {
            const int current = storageIndex(i);
            if (cavlcCodes) {
                auto encodedFrame = encodeResidentCavlcFrame(std::filesystem::path(motionShader).parent_path(),
                    cavlcCodes, header.front(), reader->read(i), reconstructed[current], reconstructedChroma[current],
                    width, height, qp, chromaQp(qp), motionSearchParameters(qp, options ? options->preset : nullptr),
                     inter, reconstruction.is_open(), !baselineProfile, options ? options->preset : nullptr);
                codedInter8x8 += encodedFrame.inter8x8;
                bytes = std::move(encodedFrame.packet);
            } else {
                auto encodedFrame = encodeResidentCabacFrame(std::filesystem::path(motionShader).parent_path(),
                    header.front(), reader->read(i), reconstructed[current], reconstructedChroma[current],
                    width, height, qp, chromaQp(qp), motionSearchParameters(qp, options ? options->preset : nullptr), inter, i, reconstruction.is_open(), !baselineProfile, options && options->preset != nullptr,
                     bFrameHeaders && inter, options ? options->preset : nullptr);
                codedInter8x8 += encodedFrame.inter8x8;
                bytes = std::move(encodedFrame.packet);
            }
        } else {
            using Slice = cabacCorpus::FrameFixture;
            if (const char* dump = std::getenv("VK264_DUMP_DIR")) {
                std::ofstream out(std::string(dump)+"/staged_bank_"+std::to_string(i)+".bin", std::ios::binary);
                out.write(reinterpret_cast<const char*>(levels.data()), std::streamsize(levels.size()*sizeof(levels[0])));
            }
            execute(cabacShader, body, 1, cabacTables(levels, cabacCorpus::frameResidualBankSize), 0,
                    {{offsetof(Slice, bytes), sizeof(Slice::bytes)}, {offsetof(Slice, position), sizeof(Slice::position)}},
                    0, true);
            if (slice.position <= 1 || slice.position > cabacCorpus::frameMaxBytes)
                throw std::runtime_error("Invalid CABAC slice length");
            if (i == skippedPicture) {
                x264_t reference{};
                reference.i_frame = slice.frame;
                reference.sh.i_type = SLICE_TYPE_P;
                std::uint8_t referenceBytes[cabacCorpus::maxBytes]{};
                referenceBytes[0] = bytes.back();
                x264_cabac_init(&reference);
                x264_cabac_context_init(&reference, &reference.cabac, SLICE_TYPE_P, qp, 0);
                x264_cabac_encode_init(&reference.cabac, referenceBytes+1, referenceBytes+std::size(referenceBytes));
                for (int index = 0; index < macroblocks; ++index) {
                    x264_cabac_mb_skip(&reference, 1);
                    if (index+1 < macroblocks) x264_cabac_encode_terminal_c(&reference.cabac);
                }
                x264_cabac_encode_flush(&reference, &reference.cabac);
                if (slice.position != reference.cabac.p-referenceBytes ||
                    !std::equal(slice.bytes.begin(), slice.bytes.begin()+slice.position, referenceBytes))
                    throw std::runtime_error("The integrated P-skip slice differs from x264 CABAC");
            }
            bytes.pop_back();
            bytes.insert(bytes.end(), slice.bytes.begin(), slice.bytes.begin()+slice.position);
        }
        if (!resident) nals.push_back({std::move(bytes), 1, 0,
            static_cast<std::uint32_t>((bFrameHeaders && inter) ? 2 : 3), inter ? 1u : 5u, 0, 0, 0});
        entropy.finish();
        HostTimer pack("nal_pack_and_output");
        int pictureBytes = 0;
        for (const auto& nal : packNals(nalShader, nals)) {
            output.write(reinterpret_cast<const char*>(nal.bytes.data()), nal.bytes.size());
            pictureBytes += static_cast<int>(nal.bytes.size());
        }
        if (resident) {
            output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            pictureBytes += static_cast<int>(bytes.size());
        }
        result.bytes += pictureBytes;
        pack.finish();
        if (rate.front().budget) {
            rate.front().gopPosition = i%gop;
            rate.front().actualBits = 8*pictureBytes;
            execute(rateShader.c_str(), rate);
        }
        nals.clear();
        const int current = storageIndex(i);
        if (clip && !resident) {
            const HostTimer deblock("deblock_pack_filter_and_unpack");
            std::vector<DeblockBlock> deblockBlocks(macroblocks);
            for (int mb = 0; mb < macroblocks; ++mb) {
                const int offset = mb*cabacCorpus::macroblockWords;
                deblockBlocks[mb] = {static_cast<std::uint32_t>(levels[offset+3]),
                                     inter ? levels[offset] : 0, inter ? levels[offset+2] : 0};
            }
            const char* stage = computeContext().deblockSubgroupVote
                ? "deblock_frame_subgroup.spv" : "deblock_frame_tiled.spv";
            const auto deblockShader = std::filesystem::path(motionShader).replace_filename(stage).string();
            deblockPicture(deblockShader.c_str(), width, height, qp, chromaQp(qp), !inter, deblockBlocks,
                           reconstructed[current], reconstructedChroma[current], 0, 0, true);
        }
        if (reconstruction.is_open()) {
            const auto& frame = reconstructed[current];
            for (int y = 0; y < visibleHeight; ++y)
                reconstruction.write(reinterpret_cast<const char*>(frame.data()+y*width), visibleWidth);
            for (const auto& plane : reconstructedChroma[current])
                for (int y = 0; y < visibleHeight/2; ++y)
                    reconstruction.write(reinterpret_cast<const char*>(plane.data()+y*chromaWidth), visibleWidth/2);
        }
        if (!output || !reconstruction) throw std::runtime_error("Failed to write GPU encode outputs");
    }
    if (!clip) {
        if (!residualCounts[0] || !residualCounts[1] || !residualCounts[2])
            throw std::runtime_error("The integration stream requires empty, DC-only and AC macroblocks");
        for (const auto& counts : chromaResidualCounts)
            if (!counts[0] || !counts[1] || !counts[2])
                throw std::runtime_error("The integration stream requires empty, DC-only and AC chroma residuals");
    }
    output.close();
    if (reconstruction.is_open()) reconstruction.close();
    if (!output || !reconstruction) throw std::runtime_error("Failed to close GPU encode outputs");
    if (reconstructionPath) std::filesystem::rename(reconstructionTemp, reconstructionPath);
    std::filesystem::rename(streamTemp, outputPath);
    result.inter8x8Blocks = codedInter8x8;
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-startTime).count();
    return result;
}

x264_vulkan::EncodeResult x264_vulkan::encode(const EncodeOptions& options) {
    const auto path = [](const char* stage) { return (std::filesystem::path(X264_VULKAN_SHADER_DIR) / stage).string(); };
    const RawClip clip{options.input, options.width, options.height, 0, 0, options.width, options.height, 0, options.frames};
    return encodeCabacClip(path("parameters.spv").c_str(), path("slice_header.spv").c_str(),
                           path("intra16.spv").c_str(), path("intra_chroma.spv").c_str(),
                           path("motion_compensation.spv").c_str(), path("mv_predict.spv").c_str(),
                           path("motion_search.spv").c_str(), path("cabac_frame.spv").c_str(), path("nal.spv").c_str(),
                           options.output, options.reconstruction, &clip, &options);
}

