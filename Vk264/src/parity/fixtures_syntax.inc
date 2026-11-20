// Vk264 parity module: fixtures_syntax.inc
// Mirrors: x264 common/bitstream.c + encoder/set.c + encoder/encoder.c
// Split from dct_parity.cpp lines 3573-4151.

namespace bitstreamCorpus {
constexpr int maxOps = 96, capacity = 768;
enum Kind : std::uint32_t {
    writeBits, write32, write1, align0, align1, align10, realign, flush, unsignedGolomb,
    unsignedGolombBig, signedGolomb, truncatedGolomb, trailing, sizeUe, sizeUeBig, sizeSe, sizeTe
};
struct Operation { Kind kind; std::uint32_t a, b; };
struct Fixture {
    std::int32_t start, count;
    std::array<Operation, maxOps> operations;
    std::array<std::int32_t, maxOps> observed;
    std::array<std::uint32_t, capacity> bytes;
    std::int32_t position, pointer, left;
    std::uint32_t curLow, curHigh;
};
static_assert(sizeof(std::uintptr_t) == 8, "This test uses the x264 64-bit bit writer");
static_assert(sizeof(Fixture) == (7 + 4*maxOps + capacity)*sizeof(std::uint32_t));
}

void verifyBitstream(const char* shader) {
    using namespace bitstreamCorpus;
    std::mt19937 random(0x264b5);
    std::vector<Fixture> cases(2048);
    using Word = std::uint32_t;
    constexpr Word unsignedValues[] = {0, 1, 126, 127, 128, 253, 254, 255, 256,
                                       65534, 65535, 65536, 0x1000000, 0x7ffffffe};
    constexpr int signedValues[] = {0, 1, -1, 63, -63, 127, -127, 128, -128, -32767, 32767};
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        input.start = 4 + static_cast<int>(i % 16);
        for (auto& value : input.bytes) value = random() & 255;
        input.observed.fill(-911);
        input.count = i < 16 ? 0 : maxOps;
        bool mustRealign = false;
        for (int p = 0; p < input.count; ++p) {
            auto& op = input.operations[p];
            op.kind = mustRealign ? realign : static_cast<Kind>(random() % (sizeTe+1));
            if (!mustRealign && op.kind == realign) op.kind = writeBits;
            if (op.kind == flush && p != input.count-1) op.kind = align0;
            mustRealign = op.kind == align0 || op.kind == align1 || op.kind == align10 || op.kind == flush;
            switch (op.kind) {
                case writeBits: {
                    op.a = static_cast<Word>(random() % 33);
                    op.b = static_cast<Word>(random());
                    if (op.a < 32) op.b &= (1u << op.a)-1u;
                    break;
                }
                case write32: op.a = static_cast<Word>(random()); break;
                case write1: op.a = random() & 1; break;
                case unsignedGolomb:
                case sizeUe: op.a = random() % 255; break;
                case unsignedGolombBig:
                    op.a = p % 2 == 0 ? unsignedValues[(i+p) % std::size(unsignedValues)] : random() & 0x3fffffffu;
                    break;
                case sizeUeBig: op.a = random() % 65535; break;
                case signedGolomb:
                case sizeSe:
                    op.a = static_cast<Word>(p % 2 == 0 ? signedValues[(i+p) % std::size(signedValues)] :
                                             static_cast<int>(random() % 65535)-32767);
                    break;
                case truncatedGolomb:
                case sizeTe:
                    op.a = 1 + random() % 20;
                    op.b = random() % (op.a+1);
                    break;
                default: break;
            }
        }
    }
    const auto inputs = cases;
    execute(shader, cases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        alignas(16) std::uint8_t bytes[capacity];
        std::copy(input.bytes.begin(), input.bytes.end(), bytes);
        bs_t writer{};
        bs_init(&writer, bytes+input.start, capacity-input.start);
        auto observed = input.observed;
        for (int p = 0; p < input.count; ++p) {
            const auto op = input.operations[p];
            int result = -1;
            switch (op.kind) {
                case writeBits: bs_write(&writer, op.a, op.b); break;
                case write32: bs_write32(&writer, op.a); break;
                case write1: bs_write1(&writer, op.a); break;
                case align0: bs_align_0(&writer); break;
                case align1: bs_align_1(&writer); break;
                case align10: bs_align_10(&writer); break;
                case realign: bs_realign(&writer); break;
                case flush: bs_flush(&writer); break;
                case unsignedGolomb: bs_write_ue(&writer, op.a); break;
                case unsignedGolombBig: bs_write_ue_big(&writer, op.a); break;
                case signedGolomb: bs_write_se(&writer, static_cast<int>(op.a)); break;
                case truncatedGolomb: bs_write_te(&writer, op.a, op.b); break;
                case trailing: bs_rbsp_trailing(&writer); break;
                case sizeUe: result = bs_size_ue(op.a); break;
                case sizeUeBig: result = bs_size_ue_big(op.a); break;
                case sizeSe: result = bs_size_se(static_cast<int>(op.a)); break;
                case sizeTe: result = bs_size_te(op.a, op.b); break;
            }
            observed[p] = result < 0 ? bs_pos(&writer) : result;
        }
        if (!matchesNativeBytes(output.bytes, bytes) || output.observed != observed ||
            output.position != bs_pos(&writer) || output.pointer != writer.p-bytes ||
            output.left != writer.i_left || output.curLow != static_cast<Word>(writer.cur_bits) ||
            output.curHigh != static_cast<Word>(writer.cur_bits >> 32) || output.start != input.start ||
            output.count != input.count ||
            std::memcmp(output.operations.data(), input.operations.data(), sizeof(input.operations)) != 0)
            throw std::runtime_error("Bit writer byte or state mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " bit writer sequences matched x264 bytes, positions and state.\n";
}

struct NalPayload {
    std::vector<std::uint8_t> rbsp;
    std::uint32_t annexB, longStartCode, refIdc, type, padding, escapeOnly, avcIntra;
};
struct NalOutput {
    std::vector<std::uint8_t> bytes;
    std::uint32_t padding;
};
std::vector<NalOutput> packNals(const char* shader, const std::vector<NalPayload>& inputs) {
    if (inputs.empty()) return {};
    constexpr std::size_t fields = 12;
    constexpr std::uint32_t guard = 177;
    const std::size_t headers = 1 + fields*inputs.size();
    std::vector<std::uint32_t> words(headers);
    words[0] = static_cast<std::uint32_t>(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto& in = inputs[i];
        const auto entry = 1 + fields*i;
        words[entry] = static_cast<std::uint32_t>(words.size());
        words[entry+1] = static_cast<std::uint32_t>(in.rbsp.size());
        words.insert(words.end(), in.rbsp.begin(), in.rbsp.end());
        words[entry+2] = static_cast<std::uint32_t>(words.size());
        words[entry+3] = in.annexB;
        words[entry+4] = in.longStartCode;
        words[entry+5] = in.refIdc;
        words[entry+6] = in.type;
        words[entry+7] = in.padding;
        words[entry+8] = in.escapeOnly;
        words[entry+9] = UINT32_MAX;
        words[entry+10] = in.avcIntra;
        words[entry+11] = in.padding;
        const auto capacity = in.rbsp.size()*2 + in.padding + 16;
        words.resize(words.size() + capacity, guard);
    }
    const auto before = words;
    execute(shader, words, 1, {}, inputs.size());
    std::vector<NalOutput> output(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto entry = 1 + fields*i;
        const auto src = before[entry], start = before[entry+2], count = words[entry+9];
        const auto limit = i+1 == inputs.size() ? words.size() : before[entry+fields];
        if (count > limit-start ||
            !std::equal(words.begin()+entry, words.begin()+entry+9, before.begin()+entry) ||
            words[entry+10] != before[entry+10] ||
            !std::equal(words.begin()+src, words.begin()+start, before.begin()+src) ||
            !std::all_of(words.begin()+start+count, words.begin()+limit,
                         [guard](std::uint32_t word) { return word == guard; }) ||
            !std::all_of(words.begin()+start, words.begin()+start+count,
                         [](std::uint32_t word) { return word <= 255; }))
            throw std::runtime_error("NAL packing touched bytes outside its output in fixture " + std::to_string(i));
        output[i].bytes.assign(words.begin()+start, words.begin()+start+count);
        output[i].padding = words[entry+11];
    }
    return output;
}

void verifyNal(const char* shader) {
    std::mt19937 random(0x2641a1);
    std::vector<NalPayload> cases(1024);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        const auto pattern = i / 16;
        input.annexB = (i & 1) != 0;
        input.longStartCode = (i & 2) != 0;
        input.escapeOnly = (i & 4) != 0;
        input.avcIntra = (i & 8) != 0;
        input.refIdc = static_cast<std::uint32_t>(pattern % 4);
        input.type = 1 + random() % 23;
        input.padding = random() % 256;
        const std::size_t length = pattern < 12 ? pattern : (pattern == 12 ? 65538 : random() % 1001);
        input.rbsp.resize(length);
        for (std::size_t p = 0; p < length; ++p) {
            const unsigned mode = static_cast<unsigned>(pattern % 4);
            input.rbsp[p] = static_cast<std::uint8_t>(mode == 0 ? 0 : mode == 1 ? p % 8 :
                                                      mode == 2 ? random() % 5 : random() & 255);
        }
    }
    const auto result = packNals(shader, cases);
    x264_t encoder{};
    encoder.bsf.nal_escape = nal_escape_c;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto input = cases[i];
        std::vector<std::uint8_t> bytes(input.rbsp.size()*2 + input.padding + 16);
        auto count = input.rbsp.size();
        if (input.rbsp.empty()) input.rbsp.push_back(0);
        if (input.escapeOnly) {
            count = nal_escape_c(bytes.data(), input.rbsp.data(), input.rbsp.data()+count) - bytes.data();
        } else {
            encoder.param.b_annexb = input.annexB != 0;
            encoder.param.i_avcintra_class = input.avcIntra ? 100 : 0;
            x264_nal_t nal{static_cast<int>(input.refIdc), static_cast<int>(input.type),
                           static_cast<int>(input.longStartCode), 0, 0, static_cast<int>(count),
                           input.rbsp.data(), static_cast<int>(input.padding)};
            x264_nal_encode(&encoder, bytes.data(), &nal);
            count = nal.i_payload;
            input.padding = nal.i_padding;
            if (nal.p_payload != bytes.data()) throw std::runtime_error("Unexpected x264 NAL pointer");
        }
        bytes.resize(count);
        if (result[i].bytes != bytes || result[i].padding != input.padding)
            throw std::runtime_error("NAL byte or padding mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " NAL fixtures matched x264 escaping, framing and padding.\n";
}

namespace parameterCorpus {
constexpr std::size_t spsSize = offsetof(x264_sps_t, i_cqm_preset) + sizeof(int);
constexpr int capacity = 1024;
struct Fixture {
    std::int32_t kind, bitDepth, start, position;
    std::array<std::uint32_t, spsSize/4> sps;
    std::array<std::uint32_t, sizeof(x264_pps_t)/4> pps;
    std::array<std::uint32_t, 8*64> scaling;
    std::array<std::uint32_t, capacity> bytes;
};
static_assert(sizeof(Fixture) == 4*(4+spsSize/4+sizeof(x264_pps_t)/4+8*64+capacity));

std::vector<std::int32_t> tables() {
    std::vector<std::int32_t> data(8*64);
    for (int i = 0; i < 8; ++i)
        std::copy_n(x264_cqm_jvt[i], i < 4 ? 16 : 64, data.begin()+i*64);
    data.insert(data.end(), std::begin(x264_zigzag_scan4[0]), std::end(x264_zigzag_scan4[0]));
    data.insert(data.end(), std::begin(x264_zigzag_scan8[0]), std::end(x264_zigzag_scan8[0]));
    return data;
}
void store(Fixture& fixture, const x264_sps_t& sps, const x264_pps_t& pps) {
    std::memcpy(fixture.sps.data(), &sps, spsSize);
    std::memcpy(fixture.pps.data(), &pps, sizeof(pps));
    for (int i = 0; i < 8; ++i) {
        if (sps.scaling_list[i])
            std::copy_n(sps.scaling_list[i], i < 4 ? 16 : 64, fixture.scaling.begin()+64*i);
    }
}
}

void verifyParameters(const char* shader) {
    using namespace parameterCorpus;
    std::mt19937 random(0x264575);
    std::vector<Fixture> cases(2048);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        const auto flags = i/2;
        const auto bit = [flags](int n) { return static_cast<int>((flags >> n)&1u); };
        input.kind = i % 2;
        input.bitDepth = bit(4) ? 10 : 8;
        input.start = 4 + ((i/2) % 12);
        for (auto& byte : input.bytes) byte = random() & 255;
        x264_sps_t sps{};
        x264_pps_t pps{};
        sps.i_id = random() % 32;
        constexpr int profiles[] = {66, 77, 100, 110, 122, 244};
        sps.i_profile_idc = profiles[flags % std::size(profiles)];
        sps.i_level_idc = 10 + random() % 52;
        sps.b_constraint_set0 = bit(0);
        sps.b_constraint_set1 = bit(1);
        sps.b_constraint_set2 = bit(2);
        sps.b_constraint_set3 = bit(3);
        sps.i_chroma_format_idc = (flags/3) % 4;
        sps.b_qpprime_y_zero_transform_bypass = bit(5);
        sps.i_cqm_preset = (flags/5) % 3;
        sps.b_avcintra_hd = flags % 11 == 1;
        sps.b_avcintra_4k = flags % 7 == 3 && !sps.b_avcintra_hd;
        sps.i_log2_max_frame_num = 4 + random() % 13;
        sps.i_poc_type = bit(6) ? 0 : 2;
        sps.i_log2_max_poc_lsb = 4 + random() % 13;
        sps.i_num_ref_frames = random() % 17;
        sps.b_gaps_in_frame_num_value_allowed = bit(4);
        sps.b_frame_mbs_only = bit(1);
        sps.i_mb_width = flags % 7 == 0 ? 512 : 1 + random() % 240;
        sps.i_mb_height = (sps.b_frame_mbs_only ? 1 : 2) * (1 + random() % 200);
        sps.b_mb_adaptive_frame_field = bit(0);
        sps.b_direct8x8_inference = bit(5);
        sps.b_crop = bit(2);
        sps.crop.i_left = bit(3)*4;
        sps.crop.i_right = bit(4)*8;
        sps.crop.i_top = bit(5)*4;
        sps.crop.i_bottom = bit(6)*12;
        sps.b_vui = flags % 7 != 0;
        auto& vui = sps.vui;
        vui.b_aspect_ratio_info_present = bit(0);
        constexpr int ratios[][2] = {{1,1},{12,11},{10,11},{16,11},{40,33},{24,11},{20,11},{32,11},
                                     {80,33},{18,11},{15,11},{64,33},{160,99},{4,3},{3,2},{2,1},{1234,4321}};
        const auto& ratio = ratios[flags/2 % std::size(ratios)];
        vui.i_sar_width = ratio[0];
        vui.i_sar_height = ratio[1];
        vui.b_overscan_info_present = bit(1);
        vui.b_overscan_info = bit(2);
        vui.b_signal_type_present = bit(3);
        vui.i_vidformat = random() % 6;
        vui.b_fullrange = bit(4);
        vui.b_color_description_present = bit(5);
        vui.i_colorprim = 1 + random() % 12;
        vui.i_transfer = 1 + random() % 18;
        vui.i_colmatrix = random() % 15;
        vui.b_chroma_loc_info_present = bit(6);
        vui.i_chroma_loc_top = random() % 6;
        vui.i_chroma_loc_bottom = random() % 6;
        vui.b_timing_info_present = bit(7);
        vui.i_num_units_in_tick = flags % 11 == 0 ? 1001 : random() | 1u;
        vui.i_time_scale = flags % 11 == 0 ? 120000 : random() | 1u;
        vui.b_fixed_frame_rate = bit(1);
        vui.b_nal_hrd_parameters_present = bit(8);
        vui.b_vcl_hrd_parameters_present = bit(9);
        vui.hrd.i_cpb_cnt = 1;
        vui.hrd.i_bit_rate_scale = random() % 16;
        vui.hrd.i_cpb_size_scale = random() % 16;
        vui.hrd.i_bit_rate_value = 1 + (random() & 0xffffffu);
        vui.hrd.i_cpb_size_value = 1 + (random() & 0xffffffu);
        vui.hrd.b_cbr_hrd = bit(0);
        vui.hrd.i_initial_cpb_removal_delay_length = 1 + random() % 32;
        vui.hrd.i_cpb_removal_delay_length = 1 + random() % 32;
        vui.hrd.i_dpb_output_delay_length = 1 + random() % 32;
        vui.hrd.i_time_offset_length = random() % 32;
        vui.b_pic_struct_present = bit(2);
        vui.b_bitstream_restriction = bit(4);
        vui.b_motion_vectors_over_pic_boundaries = bit(5);
        vui.i_max_bytes_per_pic_denom = random() % 17;
        vui.i_max_bits_per_mb_denom = random() % 17;
        vui.i_log2_max_mv_length_horizontal = random() % 17;
        vui.i_log2_max_mv_length_vertical = random() % 17;
        vui.i_num_reorder_frames = random() % 8;
        vui.i_max_dec_frame_buffering = vui.i_num_reorder_frames + random() % 8;

        std::uint8_t matrices[8][64]{};
        for (int list = 0; list < 8; ++list) {
            const int length = list < 4 ? 16 : 64;
            sps.scaling_list[list] = matrices[list];
            switch ((flags/7+list)%5) {
                case 0: std::copy_n(x264_cqm_jvt[list], length, matrices[list]); break;
                case 1: std::fill_n(matrices[list], length, 16); break;
                case 2:
                    if (list >= 2 && (list&2)) std::copy_n(matrices[list-2], length, matrices[list]);
                    else std::fill_n(matrices[list], length, 255);
                    break;
                case 3:
                    for (int p = 0; p < length; ++p) matrices[list][p] = 1 + random() % 255;
                    break;
                default: {
                    const auto* scan = list < 4 ? x264_zigzag_scan4[0] : x264_zigzag_scan8[0];
                    std::fill_n(matrices[list], length, 17);
                    for (int p = 0; p < length/2; ++p) matrices[list][scan[p]] = 1 + random() % 255;
                    break;
                }
            }
        }
        pps.i_id = random() % 256;
        pps.i_sps_id = sps.i_id;
        pps.b_cabac = bit(0);
        pps.b_pic_order = bit(1);
        pps.i_num_slice_groups = 1;
        pps.i_num_ref_idx_l0_default_active = 1 + random() % 16;
        pps.i_num_ref_idx_l1_default_active = 1 + random() % 16;
        pps.b_weighted_pred = bit(2);
        pps.b_weighted_bipred = random() % 3;
        pps.i_pic_init_qp = random() % (52+6*(input.bitDepth-8));
        pps.i_pic_init_qs = random() % (52+6*(input.bitDepth-8));
        pps.i_chroma_qp_index_offset = static_cast<int>(random() % 25)-12;
        pps.b_deblocking_filter_control = bit(3);
        pps.b_constrained_intra_pred = bit(4);
        pps.b_redundant_pic_cnt = bit(5);
        pps.b_transform_8x8_mode = bit(6);
        store(input, sps, pps);
    }
    const auto inputs = cases;
    execute(shader, cases, 1, tables());
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        x264_sps_t sps{};
        x264_pps_t pps{};
        std::memcpy(&sps, input.sps.data(), spsSize);
        std::memcpy(&pps, input.pps.data(), sizeof(pps));
        std::uint8_t scaling[8][64];
        for (int list = 0; list < 8; ++list) {
            std::copy_n(input.scaling.begin()+64*list, 64, scaling[list]);
            sps.scaling_list[list] = scaling[list];
        }
        alignas(16) std::uint8_t bytes[capacity];
        std::copy(input.bytes.begin(), input.bytes.end(), bytes);
        bs_t writer{};
        bs_init(&writer, bytes+input.start, capacity-input.start);
        parameterBitDepth = input.bitDepth;
        if (input.kind == 0) x264_sps_write(&writer, &sps);
        else x264_pps_write(&writer, &sps, &pps);
        if (!matchesNativeBytes(output.bytes, bytes) || output.position != writer.p-bytes ||
            output.kind != input.kind || output.bitDepth != input.bitDepth || output.start != input.start ||
            output.sps != input.sps || output.pps != input.pps || output.scaling != input.scaling)
            throw std::runtime_error("SPS/PPS bytes differ in fixture " + std::to_string(i));
    }
    parameterBitDepth = 8;
    std::cout << cases.size() << " SPS/PPS payloads matched x264 bytes for 8- and 10-bit syntax.\n";
}

namespace sliceCorpus {
constexpr int capacity = 1024;
struct Weight {
    std::int32_t enabled, denom, scale, offset;
};
struct Syntax {
    std::int32_t type, firstMb, ppsId, frameNumber;
    std::int32_t mbaff, field, bottomField, idrId;
    std::int32_t poc, deltaPocBottom, redundantCount, spatialDirect;
    std::int32_t overrideRefs, refs0, refs1;
    std::int32_t reorder[2];
    std::int32_t order[2][X264_REF_MAX][2];
    Weight weights[X264_REF_MAX][3];
    std::int32_t mmcoCount, mmcoDifference[X264_REF_MAX];
    std::int32_t cabacInit, qpDelta, deblock, alphaOffset, betaOffset;
};
static_assert(sizeof(Syntax) == 295*4);
struct Fixture {
    std::int32_t start, nalRef, alignCabac, weightedPred;
    std::array<std::uint32_t, parameterCorpus::spsSize/4> sps;
    std::array<std::uint32_t, sizeof(x264_pps_t)/4> pps;
    Syntax slice;
    std::array<std::uint32_t, capacity> bytes;
    std::int32_t bitPosition, pointer, left;
    std::uint32_t low, high;
};
static_assert(sizeof(Fixture) == (4+parameterCorpus::spsSize/4+sizeof(x264_pps_t)/4+295+capacity+5)*4);
void store(Fixture& fixture, const x264_slice_header_t& sh) {
    std::memcpy(fixture.sps.data(), sh.sps, parameterCorpus::spsSize);
    std::memcpy(fixture.pps.data(), sh.pps, sizeof(x264_pps_t));
    auto& wire = fixture.slice;
    wire.type = sh.i_type;
    wire.firstMb = sh.i_first_mb;
    wire.ppsId = sh.i_pps_id;
    wire.frameNumber = sh.i_frame_num;
    wire.mbaff = sh.b_mbaff;
    wire.field = sh.b_field_pic;
    wire.bottomField = sh.b_bottom_field;
    wire.idrId = sh.i_idr_pic_id;
    wire.poc = sh.i_poc;
    wire.deltaPocBottom = sh.i_delta_poc_bottom;
    wire.redundantCount = sh.i_redundant_pic_cnt;
    wire.spatialDirect = sh.b_direct_spatial_mv_pred;
    wire.overrideRefs = sh.b_num_ref_idx_override;
    wire.refs0 = sh.i_num_ref_idx_l0_active;
    wire.refs1 = sh.i_num_ref_idx_l1_active;
    wire.mmcoCount = sh.i_mmco_command_count;
    wire.cabacInit = sh.i_cabac_init_idc;
    wire.qpDelta = sh.i_qp_delta;
    wire.deblock = sh.i_disable_deblocking_filter_idc;
    wire.alphaOffset = sh.i_alpha_c0_offset;
    wire.betaOffset = sh.i_beta_offset;
    for (int list = 0; list < 2; ++list) {
        wire.reorder[list] = sh.b_ref_pic_list_reordering[list];
        for (int i = 0; i < X264_REF_MAX; ++i) {
            wire.order[list][i][0] = sh.ref_pic_list_order[list][i].idc;
            wire.order[list][i][1] = sh.ref_pic_list_order[list][i].arg;
        }
    }
    for (int i = 0; i < X264_REF_MAX; ++i) {
        wire.mmcoDifference[i] = sh.mmco[i].i_difference_of_pic_nums;
        for (int j = 0; j < 3; ++j) {
            const auto& weight = sh.weight[i][j];
            wire.weights[i][j] = {int(weight.weightfn != nullptr), weight.i_denom, weight.i_scale, weight.i_offset};
        }
    }
}
}

void verifySliceHeader(const char* shader) {
    using namespace sliceCorpus;
    struct Reference {
        x264_sps_t sps;
        x264_pps_t pps;
        x264_slice_header_t slice;
    };
    std::vector<Fixture> cases(2048);
    std::vector<Reference> native(cases.size());
    std::mt19937 random(0x264511ce);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        auto& ref = native[i];
        auto& sh = ref.slice;
        const auto bit = [i](int n) { return static_cast<int>((i>>n)&1); };
        input.start = 4 + i%12;
        input.nalRef = random() % 4;
        input.alignCabac = bit(4);
        input.weightedPred = -1;
        for (auto& sample : input.bytes) sample = random() & 255;
        sh.sps = &ref.sps;
        sh.pps = &ref.pps;
        auto& sps = ref.sps;
        auto& pps = ref.pps;
        sps.i_mb_width = 16;
        sps.i_log2_max_frame_num = 4 + random()%13;
        sps.i_log2_max_poc_lsb = 4 + random()%13;
        sps.i_poc_type = bit(0) ? 0 : 2;
        sps.b_frame_mbs_only = bit(1);
        sps.i_chroma_format_idc = (i/5)%4;
        pps.b_pic_order = bit(2);
        pps.b_redundant_pic_cnt = bit(3);
        pps.b_weighted_pred = bit(5);
        pps.b_weighted_bipred = random()%3;
        pps.b_cabac = bit(6);
        pps.b_deblocking_filter_control = bit(7);
        sh.i_type = i%3;
        sh.b_mbaff = !sps.b_frame_mbs_only && bit(8);
        int firstY = static_cast<int>(random()%15);
        if (sh.b_mbaff) firstY &= ~1;
        sh.i_first_mb = sps.i_mb_width*firstY + random()%sps.i_mb_width;
        sh.i_pps_id = random()%255;
        sh.i_frame_num = random() & 65535;
        sh.b_field_pic = !sps.b_frame_mbs_only && !sh.b_mbaff && bit(9);
        sh.b_bottom_field = bit(0);
        sh.i_idr_pic_id = sh.i_type == SLICE_TYPE_I && i%4 == 0 ? static_cast<int>(random()%255) : -1;
        if (sh.i_idr_pic_id >= 0) input.nalRef = 3;
        sh.i_poc = static_cast<int>(random() & 65535)-32768;
        sh.i_delta_poc_bottom = static_cast<int>(random()%257)-128;
        sh.i_redundant_pic_cnt = random()%32;
        sh.b_direct_spatial_mv_pred = bit(2);
        sh.b_num_ref_idx_override = bit(4);
        sh.i_num_ref_idx_l0_active = 1 + random()%X264_REF_MAX;
        sh.i_num_ref_idx_l1_active = 1 + random()%X264_REF_MAX;
        sh.i_mmco_command_count = bit(5) ? random()%(X264_REF_MAX+1) : 0;
        sh.i_cabac_init_idc = random()%3;
        sh.i_qp_delta = static_cast<int>(random()%103)-51;
        sh.i_disable_deblocking_filter_idc = random()%3;
        sh.i_alpha_c0_offset = 2*(static_cast<int>(random()%13)-6);
        sh.i_beta_offset = 2*(static_cast<int>(random()%13)-6);
        sh.b_ref_pic_list_reordering[0] = bit(0);
        sh.b_ref_pic_list_reordering[1] = bit(3);
        for (int j = 0; j < X264_REF_MAX; ++j) {
            sh.mmco[j].i_difference_of_pic_nums = 1 + random()%255;
            for (int list = 0; list < 2; ++list) {
                sh.ref_pic_list_order[list][j].idc = random()%3;
                sh.ref_pic_list_order[list][j].arg = random()%255;
            }
            for (int plane = 0; plane < 3; ++plane) {
                auto& weight = sh.weight[j][plane];
                weight.weightfn = random()%2 ? mc_weight_wtab : nullptr;
                weight.i_denom = random()%8;
                weight.i_scale = static_cast<int>(random()%256)-128;
                weight.i_offset = static_cast<int>(random()%256)-128;
            }
        }
        store(input, sh);
    }

    const auto inputs = cases;
    execute(shader, cases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        auto& sh = native[i].slice;
        alignas(16) std::uint8_t bytes[capacity];
        std::copy(input.bytes.begin(), input.bytes.end(), bytes);
        bs_t writer{};
        bs_init(&writer, bytes+input.start, capacity-input.start);
        slice_header_write(&writer, &sh, input.nalRef);
        if (input.alignCabac) bs_align_1(&writer);
        if (!matchesNativeBytes(output.bytes, bytes) || output.bitPosition != bs_pos(&writer) ||
            output.pointer != writer.p-bytes || output.left != writer.i_left || output.weightedPred != sh.b_weighted_pred ||
            output.low != static_cast<std::uint32_t>(writer.cur_bits) ||
            output.high != static_cast<std::uint32_t>(writer.cur_bits>>32) ||
            output.sps != input.sps || output.pps != input.pps ||
            std::memcmp(&output.slice, &input.slice, sizeof(Syntax)) != 0 ||
            output.start != input.start || output.nalRef != input.nalRef || output.alignCabac != input.alignCabac)
            throw std::runtime_error("Slice header differs from x264 in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " I/P/B slice headers matched x264 bytes, writer state and weighted prediction.\n";
}

