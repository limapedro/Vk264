// Vk264 parity module: x264_reference.inc
// Mirrors: x264 common/* + encoder/* extracted C, shared types, Vulkan fixture harness
// Split from dct_parity.cpp lines 1-865.

// SPDX-License-Identifier: GPL-2.0-or-later
#include "encoder_backend.hpp"
#include "x264_presets.hpp"
#include "x264_levels.hpp"
#include "x264_levels.hpp"

#include <vulkan/vulkan.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <map>
#include <optional>
#include <random>
#include <string_view>

using pixel = std::uint8_t;
using dctcoef = std::int16_t;
using udctcoef = std::uint16_t;
constexpr int FENC_STRIDE = 16;
constexpr int FDEC_STRIDE = 32;
pixel x264_clip_pixel(int value) { return static_cast<pixel>(std::clamp(value, 0, 255)); }
struct x264_run_level_t {
    std::int32_t last, mask;
    alignas(16) dctcoef level[18];
};
#include "upstream_dct.inc"
#include "upstream_tables.inc"
#include "upstream_quant.inc"
#include "upstream_macroblock.inc"

using pixel4 = std::uint32_t;
constexpr int BIT_DEPTH = 8;
struct Pixel4Access {
    pixel* data;
    operator pixel4() const {
        pixel4 result;
        std::memcpy(&result, data, sizeof(result));
        return result;
    }
    Pixel4Access& operator=(pixel4 value) {
        std::memcpy(data, &value, sizeof(value));
        return *this;
    }
    Pixel4Access& operator=(const Pixel4Access& other) { return *this = static_cast<pixel4>(other); }
};
#define PIXEL_SPLAT_X4(value) (pixel4(value) * 0x01010101u)
#define MPIXEL_X4(address) Pixel4Access{address}
constexpr int MB_LEFT = 1, MB_TOP = 2, MB_TOPRIGHT = 4, MB_TOPLEFT = 8;
std::uint16_t pack_pixel_1to2(pixel a, pixel b) {
    const pixel values[] = {a, b};
    std::uint16_t packed;
    std::memcpy(&packed, values, sizeof(packed));
    return packed;
}
pixel4 pack_pixel_2to4(std::uint16_t a, std::uint16_t b) {
    const std::uint16_t values[] = {a, b};
    pixel4 packed;
    std::memcpy(&packed, values, sizeof(packed));
    return packed;
}
#define UNUSED [[maybe_unused]]
#include "upstream_predict.inc"
#undef UNUSED
#undef MPIXEL_X4
#undef PIXEL_SPLAT_X4

using x264_deblock_inter_t = void (*)(pixel*, std::intptr_t, int, int, std::int8_t*);
using x264_deblock_intra_t = void (*)(pixel*, std::intptr_t, int, int);
struct x264_weight_t;
using weight_fn_t = void (*)(pixel*, std::intptr_t, pixel*, std::intptr_t, const x264_weight_t*, int);
using PixelCmp = int (*)(pixel*, std::intptr_t, pixel*, std::intptr_t);
using PixelCmpX3 = void (*)(pixel*, pixel*, pixel*, pixel*, std::intptr_t, int*);
using PixelCmpX4 = void (*)(pixel*, pixel*, pixel*, pixel*, pixel*, std::intptr_t, int*);
using PixelAds = int (*)(int*, std::uint16_t*, int, std::uint16_t*, std::int16_t*, int, int);
using PixelAvg = void (*)(pixel*, std::intptr_t, pixel*, std::intptr_t, pixel*, std::intptr_t, int);
constexpr int X264_SCAN8_0 = 12, X264_SCAN8_LUMA_SIZE = 40, X264_SCAN8_SIZE = 120;
struct ReferenceFrameLines {
    int i_lines[1];
};
struct x264_cabac_t {
    int i_low, i_range, i_queue, i_bytes_outstanding;
    std::uint8_t *p_start, *p, *p_end;
    alignas(64) int f8_bits_encoded;
    std::uint8_t state[1024];
    std::uint8_t padding[12];
};
constexpr int SLICE_TYPE_P = 0, SLICE_TYPE_B = 1, SLICE_TYPE_I = 2;
struct x264_t {
    int i_frame;
    bool chroma444;
    int chromaVShift;
    int interlaced;
    x264_cabac_t cabac;
    struct { int i_type; } sh;
    struct {
        int (*coeff_last[14])(dctcoef*);
    } quantf;
    struct {
        weight_fn_t* weight;
        pixel* (*get_ref)(pixel*, std::intptr_t*, pixel**, std::intptr_t, int, int, int, int, const x264_weight_t*);
        void (*mc_luma)(pixel*, std::intptr_t, pixel**, std::intptr_t, int, int, int, int, const x264_weight_t*);
        void (*mc_chroma)(pixel*, pixel*, std::intptr_t, pixel*, std::intptr_t, int, int, int, int);
        PixelAvg avg[12];
        void (*memzero_aligned)(void*, std::size_t);
    } mc;
    struct {
        PixelCmp fpelcmp[7], mbcmp_unaligned[12], mbcmp[12], sad[7];
        PixelCmpX3 fpelcmp_x3[7], sad_x3[7];
        PixelCmpX4 fpelcmp_x4[7], sad_x4[7];
        PixelAds ads[7];
    } pixf;
    struct {
        bool b_annexb;
        int i_avcintra_class;
        struct {
            int i_me_range;
        } analyse;
    } param;
    struct {
        std::uint8_t* (*nal_escape)(std::uint8_t*, std::uint8_t*, std::uint8_t*);
    } bsf;
    struct {
        std::int16_t mv_limit_fpel[2][2];
        std::int16_t mv_min_spel[2], mv_max_spel[2];
        int i_subpel_refine, i_me_method, i_mb_y, i_qp;
        bool b_chroma_me;
        int i_intra16x16_pred_mode, i_chroma_pred_mode, i_cbp_luma, i_cbp_chroma;
        int i_neighbour, i_mb_left_xy[2], i_mb_top_xy;
        int i_type, i_last_qp, i_last_dqp, i_mb_xy, i_mb_prev_xy, b_transform_8x8;
        const std::uint8_t* type;
        const std::int16_t* cbp;
        const std::uint8_t* chroma_pred_mode;
        struct {
            std::int16_t mv[2][X264_SCAN8_LUMA_SIZE][2];
            std::int8_t ref[2][X264_SCAN8_LUMA_SIZE];
            int i_cbp_left, i_cbp_top, i_neighbour_transform_size, i_neighbour_skip;
        } cache;
        struct {
            pixel* p_fdec[3];
        } pic;
    } mb;
    int luma2chroma_pixel[7];
    std::uint16_t* cost_mv_fpel[52][4];
    std::int16_t* scratch_buffer;
    ReferenceFrameLines* fenc;
};
int x264_clip3(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}
std::uint32_t loadStrengths(const std::uint8_t* strengths) {
    std::uint32_t value;
    std::memcpy(&value, strengths, sizeof(value));
    return value;
}
#define M32(address) loadStrengths(address)
#include "upstream_deblock.inc"
#undef M32

constexpr int QP_MAX_SPEC = 51;
enum { I_4x4, I_8x8, I_16x16, I_PCM };
int x264_clz(std::uint32_t value) {
    int count = 0;
    for (std::uint32_t bit = 1u << 31; bit != 0 && (value & bit) == 0; bit >>= 1) ++count;
    return count;
}
#define CHROMA444 (h->chroma444)
#define X264_MIN(a, b) std::min((a), (b))
#include "upstream_cabac.inc"
#undef X264_MIN
#undef CHROMA444
#define ALWAYS_INLINE inline
#define MB_INTERLACED (h->interlaced)
#define X264_MIN(a, b) std::min((a), (b))
#define RDO_SKIP_BS 0
#define x264_cabac_encode_decision x264_cabac_encode_decision_c
#define x264_cabac_encode_decision_noup x264_cabac_encode_decision_c
#define x264_cabac_encode_terminal x264_cabac_encode_terminal_c
#define x264_cabac_encode_bypass x264_cabac_encode_bypass_c
#include "upstream_cabac_residual.inc"
#undef x264_cabac_encode_bypass
#undef x264_cabac_encode_terminal
#undef x264_cabac_encode_decision_noup
#undef x264_cabac_encode_decision
#undef RDO_SKIP_BS
#undef X264_MIN
#undef MB_INTERLACED
#undef ALWAYS_INLINE

struct x264_weight_t {
    alignas(16) std::int16_t cachea[8];
    std::int16_t cacheb[8];
    std::int32_t i_denom, i_scale, i_offset;
    weight_fn_t* weightfn;
};
constexpr int PIXEL_MAX = 255, SIZEOF_PIXEL = sizeof(pixel);
#include "upstream_mc.inc"
#include "upstream_pixel.inc"

struct x264_me_t {
    int i_pixel;
    std::uint16_t* p_cost_mv;
    int i_ref_cost, i_ref;
    const x264_weight_t* weight;
    pixel* p_fref[12];
    pixel* p_fref_w;
    pixel* p_fenc[3];
    std::uint16_t* integral;
    std::int32_t i_stride[3];
    std::int16_t mvp[2], mv[2];
    int cost_mv, cost;
};
template<class Word>
struct PackedAccess {
    void* address;
    operator Word() const {
        Word value;
        std::memcpy(&value, address, sizeof(value));
        return value;
    }
    PackedAccess& operator=(Word value) {
        std::memcpy(address, &value, sizeof(value));
        return *this;
    }
    PackedAccess& operator=(const PackedAccess& other) { return *this = static_cast<Word>(other); }
};
enum { PIXEL_16x16, PIXEL_16x8, PIXEL_8x16, PIXEL_8x8, PIXEL_8x4, PIXEL_4x8, PIXEL_4x4, PIXEL_4x16 };
enum { X264_ME_DIA, X264_ME_HEX, X264_ME_UMH, X264_ME_ESA, X264_ME_TESA };
struct mvsad_t { int sad; std::int16_t mv[2]; };
constexpr int PADV = 32, WORD_SIZE = sizeof(void*);
alignas(64) static pixel x264_zero[16*16]{};
constexpr int COST_MAX = 1 << 28;
constexpr std::uint64_t COST_MAX64 = 1ull << 60;
constexpr struct { int w, h; } x264_pixel_size[] = {
    {16,16}, {16,8}, {8,16}, {8,8}, {8,4}, {4,8}, {4,4},
};
static const x264_weight_t unweightedReference{};
[[noreturn]] void x264_mb_predict_mv(x264_t*, int, int, int, std::int16_t*) {
    throw std::logic_error("Unexpected RD motion predictor in the SATD fixture");
}
[[noreturn]] void x264_macroblock_cache_mv(x264_t*, int, int, int, int, int, std::uint32_t) {
    throw std::logic_error("Unexpected RD motion cache write in the SATD fixture");
}
[[noreturn]] void x264_macroblock_cache_mvd(x264_t*, int, int, int, int, int, std::uint16_t) {
    throw std::logic_error("Unexpected RD motion delta cache write in the SATD fixture");
}
[[noreturn]] std::uint64_t x264_rd_cost_part(x264_t*, int, int, int) {
    throw std::logic_error("Unexpected RD cost call in the SATD fixture");
}
std::uint16_t pack8to16(int a, int b) {
    const std::uint8_t pair[] = {static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b)};
    std::uint16_t value;
    std::memcpy(&value, pair, sizeof(value));
    return value;
}
std::uint16_t load16(const void* address) {
    std::uint16_t value;
    std::memcpy(&value, address, sizeof(value));
    return value;
}
#define ALWAYS_INLINE inline
#define x264_template(name) name
#define x264_weight_none (&unweightedReference)
#define M16(address) load16(address)
#define M32(address) PackedAccess<std::uint32_t>{address}
#define M64(address) PackedAccess<std::uint64_t>{address}
#define CP32(dst,src) std::memmove((dst), (src), sizeof(std::uint32_t))
#define CP64(dst,src) std::memmove((dst), (src), sizeof(std::uint64_t))
#define X264_MIN(a,b) ((a) < (b) ? (a) : (b))
#define X264_MAX(a,b) ((a) > (b) ? (a) : (b))
#define X264_MIN4(a,b,c,d) std::min({(a), (b), (c), (d)})
#define ALIGNED_ARRAY_8(type, name, shape, ...) alignas(8) type name shape __VA_ARGS__
#define ALIGNED_ARRAY_16(type, name, shape, ...) alignas(16) type name shape __VA_ARGS__
#define ALIGNED_ARRAY_32(type, name, shape, ...) alignas(32) type name shape __VA_ARGS__
#define ALIGNED_ARRAY_64(type, name, shape, ...) alignas(64) type name shape __VA_ARGS__
#define ALIGNED_4(declaration) alignas(4) declaration
#define CHROMA444 (h->chroma444)
#define CHROMA_V_SHIFT (h->chromaVShift)
#define CHROMA_H_SHIFT 1
#define CHROMA_FORMAT 0
#define MB_INTERLACED (h->interlaced)
#include "upstream_me.inc"
#include "upstream_mvpred.inc"
#undef MB_INTERLACED
#undef CHROMA_FORMAT
#undef CHROMA_H_SHIFT
#undef CHROMA_V_SHIFT
#undef CHROMA444
#undef ALIGNED_ARRAY_32
#undef ALIGNED_ARRAY_64
#undef ALIGNED_4
#undef ALIGNED_ARRAY_16
#undef ALIGNED_ARRAY_8
#undef X264_MIN4
#undef X264_MIN
#undef X264_MAX
#undef CP64
#undef CP32
#undef M64
#undef M32
#undef M16
#undef x264_weight_none
#undef x264_template
#undef ALWAYS_INLINE

struct bs_t {
    std::uint8_t *p_start, *p, *p_end;
    std::uintptr_t cur_bits;
    int i_left, i_bits_encoded;
};
struct x264_nal_t {
    int i_ref_idc, i_type, b_long_startcode, i_first_mb, i_last_mb, i_payload;
    std::uint8_t* p_payload;
    int i_padding;
};
std::uint32_t endian_fix32(std::uint32_t value) {
    return (value >> 24) | ((value >> 8) & 0xff00u) | ((value << 8) & 0xff0000u) | (value << 24);
}
std::uintptr_t endian_fix(std::uintptr_t value) {
#if UINTPTR_MAX > UINT32_MAX
    return (std::uintptr_t(endian_fix32(static_cast<std::uint32_t>(value))) << 32) |
            endian_fix32(static_cast<std::uint32_t>(value >> 32));
#else
    return endian_fix32(value);
#endif
}
constexpr int NALU_OVERHEAD = 5;
void x264_emms() {}
#define ALWAYS_INLINE inline
#define M32(address) PackedAccess<std::uint32_t>{address}
#define X264_MAX(a,b) ((a) > (b) ? (a) : (b))
#include "upstream_bitwriter.inc"
#include "upstream_bitstream.inc"
#undef X264_MAX
#undef M32
#undef ALWAYS_INLINE

#include "upstream_set_types.inc"
constexpr int PROFILE_BASELINE = 66, PROFILE_MAIN = 77, PROFILE_HIGH = 100, CHROMA_420 = 1, CHROMA_422 = 2, CHROMA_444 = 3, X264_CQM_FLAT = 0;
thread_local int parameterBitDepth = 8;
#define BIT_DEPTH parameterBitDepth
#define QP_BD_OFFSET (6*(BIT_DEPTH-8))
#define bs_write_ue bs_write_ue_big
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4804) // The upstream C SPS writer shifts by a logical expression.
#endif
#include "upstream_set.inc"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#undef bs_write_ue
#undef QP_BD_OFFSET
#undef BIT_DEPTH

constexpr int X264_REF_MAX = 16;
#include "upstream_encoder.inc"

struct Intra16Reference {
    struct { int b_cabac; } param;
    struct {
        struct {
            pixel* p_fenc[3];
            pixel* p_fdec[3];
        } pic;
        struct { std::uint8_t non_zero_count[X264_SCAN8_SIZE]; } cache;
        int b_dct_decimate, b_lossless, b_noise_reduction, b_trellis;
        int i_intra16x16_pred_mode, i_cbp_luma, i_chroma_qp;
        int i_cbp_chroma;
    } mb;
    struct {
        dctcoef luma4x4[48][16], luma16x16_dc[3][16], chroma_dc[2][8], luma8x8[12][64];
    } dct;
    decltype(&x264_predict_16x16_dc_c) predict_16x16[7];
    decltype(&x264_predict_8x8c_dc_c) predict_chroma[7];
    int chromaFormat;
    struct {
        void (*copy[12])(pixel*, std::intptr_t, pixel*, std::intptr_t, int);
    } mc;
    struct {
        decltype(&sub16x16_dct) sub16x16_dct;
        decltype(&add16x16_idct) add16x16_idct;
        decltype(&add16x16_idct_dc) add16x16_idct_dc;
        decltype(&dct4x4dc) dct4x4dc;
        decltype(&idct4x4dc) idct4x4dc;
        decltype(&sub8x8_dct) sub8x8_dct;
        decltype(&sub8x8_dct_dc) sub8x8_dct_dc;
        decltype(&sub8x16_dct_dc) sub8x16_dct_dc;
        decltype(&dct2x4dc) dct2x4dc;
        decltype(&add8x8_idct) add8x8_idct;
        decltype(&add8x8_idct_dc) add8x8_idct_dc;
        decltype(&sub16x16_dct8) sub16x16_dct8;
        decltype(&add8x8_idct8) add8x8_idct8;
    } dctf;
    struct {
        decltype(&zigzag_scan_4x4_frame) scan_4x4;
        decltype(&zigzag_sub_4x4ac_frame) sub_4x4ac;
        decltype(&zigzag_scan_8x8_frame) scan_8x8;
    } zigzagf;
    struct {
        decltype(&denoise_dct) denoise_dct;
        decltype(&quant_4x4x4) quant_4x4x4;
        decltype(&quant_4x4_dc) quant_4x4_dc;
        decltype(&dequant_4x4) dequant_4x4;
        decltype(&dequant_4x4_dc) dequant_4x4_dc;
        decltype(&decimate_score15) decimate_score15;
        decltype(&optimize_chroma_2x2_dc) optimize_chroma_2x2_dc;
        decltype(&optimize_chroma_2x4_dc) optimize_chroma_2x4_dc;
        decltype(&quant_2x2_dc) quant_2x2_dc;
        decltype(&idct_dequant_2x4_dc) idct_dequant_2x4_dc;
        decltype(&idct_dequant_2x4_dconly) idct_dequant_2x4_dconly;
        decltype(&decimate_score16) decimate_score16;
        decltype(&quant_8x8) quant_8x8;
        decltype(&dequant_8x8) dequant_8x8;
        decltype(&decimate_score64) decimate_score64;
    } quantf;
    struct { int (*var2[12])(pixel*, pixel*, int*); } pixf;
    int dequant4_mf[4][6][16];
    udctcoef quant4_mf[4][55][16], quant4_bias[4][55][16];
    int dequant8_mf[4][6][64];
    udctcoef quant8_mf[4][52][64], quant8_bias[4][52][64];
    std::uint32_t nr_residual_sum[4][64], nr_count[4];
    udctcoef nr_offset[4][64];
};
[[noreturn]] int x264_quant_8x8_trellis(Intra16Reference*, dctcoef*, int, int, int, int, int, int) {
    throw std::logic_error("Trellis used in a scalar 8x8 fixture");
}
[[noreturn]] void x264_predict_lossless_16x16(Intra16Reference*, int, int) {
    throw std::logic_error("Lossless prediction used in a lossy I_16x16 fixture");
}
[[noreturn]] int x264_quant_4x4_trellis(Intra16Reference*, dctcoef*, int, int, int, int, int, int) {
    throw std::logic_error("Trellis used in a scalar-quantization I_16x16 fixture");
}
[[noreturn]] int x264_quant_luma_dc_trellis(Intra16Reference*, dctcoef*, int, int, int, int, int) {
    throw std::logic_error("DC trellis used in a scalar-quantization I_16x16 fixture");
}
[[noreturn]] int x264_quant_chroma_dc_trellis(Intra16Reference*, dctcoef*, int, int, int) {
    throw std::logic_error("Chroma trellis used in a scalar-quantization fixture");
}
int x264_ctz_4bit(int value) {
    return (value & 1) ? 0 : (value & 2) ? 1 : (value & 4) ? 2 : 3;
}
static void mc_copy_w8(pixel*, std::intptr_t, pixel*, std::intptr_t, int);
#define x264_t Intra16Reference
#define ALWAYS_INLINE inline
#define NOINLINE
#define CHROMA_FORMAT (h->chromaFormat)
#define CHROMA_V_SHIFT int(h->chromaFormat == CHROMA_420)
#define ALIGNED_ARRAY_64(type, name, shape, ...) alignas(64) type name shape __VA_ARGS__
#define ALIGNED_ARRAY_16(type, name, shape, ...) alignas(16) type name shape __VA_ARGS__
#define ALIGNED_ARRAY_8(type, name, shape, ...) alignas(8) type name shape __VA_ARGS__
#define M16(address) PackedAccess<std::uint16_t>{address}
#define M32(address) PackedAccess<std::uint32_t>{address}
#define M64(address) PackedAccess<std::uint64_t>{address}
#include "upstream_intra16.inc"
#undef M64
#undef M32
#undef M16
#undef ALIGNED_ARRAY_8
#undef ALIGNED_ARRAY_16
#undef ALIGNED_ARRAY_64
#undef ALWAYS_INLINE
#undef CHROMA_V_SHIFT
#undef CHROMA_FORMAT
#undef NOINLINE
#undef x264_t

template<int Side>
struct Block {
    std::array<std::uint32_t, Side * Side> source, prediction;
    std::array<std::int32_t, Side * Side> inverse, transformed;
    std::array<std::uint32_t, Side * Side> reconstructed;
};
static_assert(sizeof(Block<4>) == 80 * sizeof(std::uint32_t));

template<int Side>
std::vector<Block<Side>> fixtures() {
    std::mt19937 random(0x264);
    std::vector<Block<Side>> blocks(4096);
    for (auto& block : blocks) {
        for (int p = 0; p < Side * Side; ++p) {
            block.source[p] = random() & 255;
            block.prediction[p] = random() & 255;
            block.inverse[p] = static_cast<int>(random() & 65535) - 32768;
        }
    }
    blocks[0] = {}; // flat zero block
    blocks[1].source.fill(255);
    blocks[1].prediction.fill(0);
    blocks[1].inverse.fill(0);
    blocks[1].inverse[0] = 4096;
    blocks[2] = {};
    blocks[2].prediction.fill(255);
    blocks[2].inverse[0] = -4096;
    for (int p = 0; p < Side * Side; ++p) {
        auto& block = blocks[3 + p];
        block = {};
        block.source[p] = 255;
        block.prediction.fill(128);
        block.inverse[p] = p % 2 ? -256 : 255;
    }
    blocks[3 + Side * Side].inverse.fill(32767);
    blocks[4 + Side * Side].inverse.fill(-32768);
    return blocks;
}

struct ComputeContext {
    using Clock = std::chrono::steady_clock;
    struct DispatchTotals {
        std::uint64_t calls = 0, uploadBytes = 0, readbackBytes = 0;
        Clock::duration setup{}, upload{}, dispatch{}, readback{};
        double gpuMilliseconds = 0;
    };
    struct HostTotals {
        std::uint64_t calls = 0;
        Clock::duration elapsed{};
    };
    const bool profiling = std::getenv("VK264_PROFILE") != nullptr;
    std::map<std::string, DispatchTotals, std::less<>> profile;
    std::map<std::string, HostTotals, std::less<>> hostProfile;
    vk::UniqueInstance instance;
    vk::PhysicalDevice physical;
    std::uint32_t family = 0;
    vk::UniqueDevice device;
    vk::Queue queue;
    vk::UniqueDescriptorSetLayout layout;
    vk::UniquePipelineLayout pipelineLayout;
    std::map<std::string, vk::UniquePipeline, std::less<>> pipelines;
    vk::UniqueDescriptorPool descriptors;
    vk::DescriptorSet set;
    std::array<vk::DescriptorSet, 4> interSets;
    vk::UniqueCommandPool commandPool;
    vk::UniqueQueryPool timestamps;
    // A 2048x2048 IDR has 255 dependency waves and four timed stages per wave.
    static constexpr std::uint32_t timestampCapacity = 1100;
    std::uint32_t timestampBits = 0;
    std::uint32_t motionSubgroupSize = 1;
    bool motionCandidateBatches = false;
    bool deblockSubgroupVote = false;
    double timestampPeriod = 0;
    vk::CommandBuffer cmd;
    vk::UniqueDeviceMemory memory;
    vk::UniqueBuffer buffer;
    vk::UniqueDeviceMemory deviceMemory;
    vk::UniqueBuffer deviceBuffer;
    std::size_t deviceCapacity = 0;
    std::size_t capacity = 0;
    std::byte* mapped = nullptr;

    ComputeContext() {
        vk::ApplicationInfo app("x264 Vulkan parity", 1, nullptr, 0, VK_API_VERSION_1_1);
        instance = vk::createInstanceUnique(vk::InstanceCreateInfo({}, &app));
        for (auto candidate : instance->enumeratePhysicalDevices()) {
            if (candidate.getProperties().apiVersion < VK_API_VERSION_1_1) continue;
            const auto queues = candidate.getQueueFamilyProperties();
            const auto it = std::find_if(queues.begin(), queues.end(), [](const auto& queue) {
                return bool(queue.queueFlags & vk::QueueFlagBits::eCompute);
            });
            if (it == queues.end()) continue;
            physical = candidate;
            family = static_cast<std::uint32_t>(it - queues.begin());
            if (candidate.getProperties().deviceType == vk::PhysicalDeviceType::eDiscreteGpu) break;
        }
        if (!physical) throw std::runtime_error("No Vulkan 1.1 compute device");
        const auto deviceProperties = physical.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceSubgroupProperties>();
        const auto& subgroup = deviceProperties.get<vk::PhysicalDeviceSubgroupProperties>();
        const auto operations = vk::SubgroupFeatureFlagBits::eBasic | vk::SubgroupFeatureFlagBits::eArithmetic;
        deblockSubgroupVote = subgroup.subgroupSize == 32 &&
            bool(subgroup.supportedOperations & vk::SubgroupFeatureFlagBits::eVote) &&
            bool(subgroup.supportedStages & vk::ShaderStageFlagBits::eCompute);
        if ((subgroup.supportedOperations & operations) == operations &&
            (subgroup.supportedStages & vk::ShaderStageFlagBits::eCompute) &&
            subgroup.subgroupSize && 64 % subgroup.subgroupSize == 0)
            motionSubgroupSize = subgroup.subgroupSize;
        motionCandidateBatches = motionSubgroupSize == 32 &&
            bool(subgroup.supportedOperations & vk::SubgroupFeatureFlagBits::eShuffle);
        const float priority = 1;
        const vk::DeviceQueueCreateInfo queueInfo({}, family, 1, &priority);
        device = physical.createDeviceUnique(vk::DeviceCreateInfo({}, queueInfo));
        queue = device->getQueue(family, 0);
        std::array<vk::DescriptorSetLayoutBinding, 5> bindings;
        for (std::uint32_t i = 0; i < bindings.size(); ++i)
            bindings[i] = vk::DescriptorSetLayoutBinding(i, vk::DescriptorType::eStorageBuffer, 1,
                                                        vk::ShaderStageFlagBits::eCompute);
        layout = device->createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo({}, bindings));
        const vk::PushConstantRange pass(vk::ShaderStageFlagBits::eCompute, 0, sizeof(std::uint32_t));
        pipelineLayout = device->createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({}, *layout, pass));
        const vk::DescriptorPoolSize poolSize(vk::DescriptorType::eStorageBuffer, 25);
        descriptors = device->createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo({}, 5, poolSize));
        std::array<vk::DescriptorSetLayout, 5> layouts;
        layouts.fill(*layout);
        const auto sets = device->allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptors, layouts));
        set = sets.front();
        std::copy_n(sets.begin()+1, interSets.size(), interSets.begin());
        commandPool = device->createCommandPoolUnique(vk::CommandPoolCreateInfo({}, family));
        cmd = device->allocateCommandBuffers(
            vk::CommandBufferAllocateInfo(*commandPool, vk::CommandBufferLevel::ePrimary, 1)).front();
        if (profiling) {
            timestampBits = physical.getQueueFamilyProperties()[family].timestampValidBits;
            timestampPeriod = physical.getProperties().limits.timestampPeriod;
            if (timestampBits)
                timestamps = device->createQueryPoolUnique(
                    vk::QueryPoolCreateInfo({}, vk::QueryType::eTimestamp, timestampCapacity));
        }
    }

    ~ComputeContext() {
        if (mapped) device->unmapMemory(*memory);
        const auto milliseconds = [](Clock::duration time) {
            return std::chrono::duration<double, std::milli>(time).count();
        };
        if (profiling) {
            for (const auto& [name, stage] : hostProfile)
                std::cerr << name << " calls=" << stage.calls << " host_ms=" << milliseconds(stage.elapsed) << '\n';
            for (const auto& [path, stage] : profile)
                std::cerr << std::filesystem::path(path).filename().string()
                          << " calls=" << stage.calls << " uploaded_MiB=" << stage.uploadBytes/1048576.0
                          << " readback_MiB=" << stage.readbackBytes/1048576.0
                          << " setup_ms=" << milliseconds(stage.setup)
                          << " upload_ms=" << milliseconds(stage.upload)
                          << " dispatch_ms=" << milliseconds(stage.dispatch)
                          << " gpu_ms=" << stage.gpuMilliseconds
                          << " readback_ms=" << milliseconds(stage.readback) << '\n';
        }
    }

    void reserve(std::size_t size) {
        if (size <= capacity) return;
        if (size > physical.getProperties().limits.maxStorageBufferRange)
            throw std::runtime_error("Requested dispatch exceeds the Vulkan storage buffer range");
        if (mapped) {
            device->unmapMemory(*memory);
            mapped = nullptr;
        }
        buffer.reset();
        memory.reset();
        capacity = 0;
        const auto usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                           vk::BufferUsageFlagBits::eTransferDst;
        buffer = device->createBufferUnique(vk::BufferCreateInfo({}, size, usage));
        const auto requirements = device->getBufferMemoryRequirements(*buffer);
        const auto hostFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        const auto type = memoryType(requirements.memoryTypeBits, hostFlags, vk::MemoryPropertyFlagBits::eHostCached);
        memory = device->allocateMemoryUnique(vk::MemoryAllocateInfo(requirements.size, type));
        device->bindBufferMemory(*buffer, *memory, 0);
        mapped = static_cast<std::byte*>(device->mapMemory(*memory, 0, size));
        capacity = size;
    }

    std::uint32_t memoryType(std::uint32_t supported, vk::MemoryPropertyFlags required,
                             vk::MemoryPropertyFlags preferred = {}) const {
        const auto properties = physical.getMemoryProperties();
        std::optional<std::uint32_t> fallback;
        for (std::uint32_t type = 0; type < properties.memoryTypeCount; ++type) {
            const auto flags = properties.memoryTypes[type].propertyFlags;
            if ((supported & (1u << type)) == 0 || (flags & required) != required) continue;
            if ((flags & preferred) == preferred) return type;
            if (!fallback) fallback = type;
        }
        if (fallback) return *fallback;
        throw std::runtime_error("No compatible Vulkan memory type");
    }

    void reserveDevice(std::size_t size) {
        if (size <= deviceCapacity) return;
        deviceBuffer.reset();
        deviceMemory.reset();
        const auto usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                           vk::BufferUsageFlagBits::eTransferDst;
        deviceBuffer = device->createBufferUnique(vk::BufferCreateInfo({}, size, usage));
        const auto requirements = device->getBufferMemoryRequirements(*deviceBuffer);
        const auto type = memoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        deviceMemory = device->allocateMemoryUnique(vk::MemoryAllocateInfo(requirements.size, type));
        device->bindBufferMemory(*deviceBuffer, *deviceMemory, 0);
        deviceCapacity = size;
    }

    vk::Pipeline pipeline(const char* spv) {
        const auto existing = pipelines.find(spv);
        if (existing != pipelines.end()) return *existing->second;
        std::ifstream file(spv, std::ios::binary | std::ios::ate);
        if (!file || file.tellg() <= 0 || file.tellg() % 4 != 0)
            throw std::runtime_error("Cannot read the SPIR-V shader");
        const auto shaderSize = static_cast<std::size_t>(file.tellg());
        std::vector<std::uint32_t> words(shaderSize / sizeof(std::uint32_t));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(words.data()), shaderSize))
            throw std::runtime_error("Incomplete SPIR-V shader");
        auto shader = device->createShaderModuleUnique(vk::ShaderModuleCreateInfo({}, shaderSize, words.data()));
        const vk::PipelineShaderStageCreateInfo stage({}, vk::ShaderStageFlagBits::eCompute, *shader, "main");
        auto compiled = device->createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo({}, stage, *pipelineLayout)).value;
        return *pipelines.emplace(spv, std::move(compiled)).first->second;
    }
};

ComputeContext& computeContext() {
    static ComputeContext context;
    return context;
}

class HostTimer {
    ComputeContext::HostTotals* totals = nullptr;
    ComputeContext::Clock::time_point start;

public:
    explicit HostTimer(const char* label) {
        auto& context = computeContext();
        if (context.profiling) {
            totals = &context.hostProfile[label];
            start = ComputeContext::Clock::now();
        }
    }
    void finish() {
        if (!totals) return;
        ++totals->calls;
        totals->elapsed += ComputeContext::Clock::now()-start;
        totals = nullptr;
    }
    ~HostTimer() { finish(); }
};

struct ReadbackSpan {
    std::size_t offset, bytes;
};

template<class Case>
void execute(const char* spv, std::vector<Case>& blocks, int workItemsPerCase = 1,
             const std::vector<std::int32_t>& tables = {}, std::size_t invocationCount = 0,
             std::initializer_list<ReadbackSpan> outputs = {}, std::uint32_t passes = 0, bool deviceStorage = false,
             std::size_t outputPrefixBytes = 0, const char* profileLabel = nullptr) {
    auto& context = computeContext();
    const auto now = [&context] { return context.profiling ? ComputeContext::Clock::now() : ComputeContext::Clock::time_point{}; };
    const auto start = now();
    const auto& device = context.device;
    const auto pipeline = context.pipeline(spv);

    const auto tableBytes = tables.size() * sizeof(std::int32_t);
    const auto fixtureBytes = blocks.size() * sizeof(Case);
    const auto size = tableBytes + fixtureBytes;
    const auto uploadBytes = outputPrefixBytes ? tableBytes : size;
    context.reserve(size);
    if (deviceStorage) context.reserveDevice(size);
    auto* mapped = context.mapped;
    const auto uploadStart = now();
    if (tableBytes) std::memcpy(mapped, tables.data(), tableBytes);
    if (!outputPrefixBytes) std::memcpy(mapped + tableBytes, blocks.data(), fixtureBytes);
    const auto dispatchStart = now();

    const auto storage = vk::DescriptorType::eStorageBuffer;
    const auto set = context.set;
    const vk::DescriptorBufferInfo data(deviceStorage ? *context.deviceBuffer : *context.buffer, 0, size);
    device->updateDescriptorSets(vk::WriteDescriptorSet(set, 0, 0, storage, {}, data), {});

    device->resetCommandPool(*context.commandPool);
    const auto cmd = context.cmd;
    cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    if (deviceStorage) {
        cmd.copyBuffer(*context.buffer, *context.deviceBuffer, vk::BufferCopy(0, 0, uploadBytes));
        const vk::MemoryBarrier uploadBarrier(vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
                            {}, uploadBarrier, {}, {});
    }
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *context.pipelineLayout, 0, set, {});
    if (context.timestamps) {
        cmd.resetQueryPool(*context.timestamps, 0, 2);
        cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *context.timestamps, 0);
    }
    const auto invocations = invocationCount ? invocationCount : blocks.size() * workItemsPerCase;
    const auto groups = static_cast<std::uint32_t>((invocations + 63) / 64);
    if (passes == 0) {
        cmd.dispatch(groups, 1, 1);
    } else {
        const vk::MemoryBarrier dependency(vk::AccessFlagBits::eShaderWrite,
            vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
        for (std::uint32_t pass = 0; pass < passes; ++pass) {
            cmd.pushConstants<std::uint32_t>(*context.pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, pass);
            cmd.dispatch(groups, 1, 1);
            if (pass+1 < passes)
                cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                                    {}, dependency, {}, {});
        }
    }
    if (context.timestamps)
        cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *context.timestamps, 1);
    if (deviceStorage) {
        const vk::MemoryBarrier downloadBarrier(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                            {}, downloadBarrier, {}, {});
        const auto copyBytes = outputPrefixBytes ? outputPrefixBytes : fixtureBytes;
        cmd.copyBuffer(*context.deviceBuffer, *context.buffer, vk::BufferCopy(tableBytes, tableBytes, copyBytes));
    }
    const auto writeAccess = deviceStorage ? vk::AccessFlagBits::eTransferWrite : vk::AccessFlagBits::eShaderWrite;
    const auto writeStage = deviceStorage ? vk::PipelineStageFlagBits::eTransfer : vk::PipelineStageFlagBits::eComputeShader;
    const vk::MemoryBarrier readback(writeAccess, vk::AccessFlagBits::eHostRead);
    cmd.pipelineBarrier(writeStage, vk::PipelineStageFlagBits::eHost,
                        {}, readback, {}, {});
    cmd.end();
    context.queue.submit(vk::SubmitInfo({}, {}, cmd, {}));
    context.queue.waitIdle();
    const auto readbackStart = now();
    std::size_t readbackBytes = 0;
    if (outputPrefixBytes) {
        std::memcpy(blocks.data(), mapped+tableBytes, outputPrefixBytes);
        readbackBytes = outputPrefixBytes;
    } else if (outputs.size() == 0) {
        if (tableBytes && std::memcmp(mapped, tables.data(), tableBytes) != 0)
            throw std::runtime_error("Shader modified the shared tables");
        std::memcpy(blocks.data(), mapped+tableBytes, fixtureBytes);
        readbackBytes = size;
    } else {
        auto* destination = reinterpret_cast<std::byte*>(blocks.data());
        for (const auto span : outputs) {
            if (span.offset > sizeof(Case) || span.bytes > sizeof(Case)-span.offset)
                throw std::runtime_error("Invalid GPU readback span");
            for (std::size_t i = 0; i < blocks.size(); ++i) {
                const auto offset = i*sizeof(Case)+span.offset;
                std::memcpy(destination+offset, mapped+tableBytes+offset, span.bytes);
            }
            readbackBytes += blocks.size()*span.bytes;
        }
    }
    if (context.profiling) {
        auto& profile = context.profile[profileLabel ? profileLabel : spv];
        ++profile.calls;
        profile.uploadBytes += uploadBytes;
        profile.readbackBytes += readbackBytes;
        profile.setup += uploadStart-start;
        profile.upload += dispatchStart-uploadStart;
        profile.dispatch += readbackStart-dispatchStart;
        profile.readback += now()-readbackStart;
        if (context.timestamps) {
            std::array<std::uint64_t, 2> ticks{};
            const VkResult result = vkGetQueryPoolResults(static_cast<VkDevice>(*device),
                static_cast<VkQueryPool>(*context.timestamps), 0, 2, sizeof(ticks), ticks.data(),
                sizeof(ticks.front()), VK_QUERY_RESULT_64_BIT);
            if (result == VK_SUCCESS) {
                const auto mask = UINT64_MAX >> (64-context.timestampBits);
                profile.gpuMilliseconds += double((ticks[1]-ticks[0]) & mask)*context.timestampPeriod/1e6;
            }
        }
    }
}

template<class Native, class Wire, std::size_t Count>
bool matchesNativeBytes(const std::array<Wire, Count>& gpu, const Native (&cpu)[Count]) {
    std::array<Native, Count> packed{};
    for (std::size_t i = 0; i < Count; ++i) {
        packed[i] = static_cast<Native>(gpu[i]);
        if (static_cast<Wire>(packed[i]) != gpu[i]) return false;
    }
    return std::memcmp(packed.data(), cpu, sizeof(cpu)) == 0;
}

template<int Side, int Group>
