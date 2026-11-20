// Vk264 parity module: fixtures_cavlc.inc
// Mirrors: x264 encoder/cavlc.c
// Split from dct_parity.cpp lines 2467-2619.

// Issue #7: byte-exact CAVLC residual fixture. The scalar reference is
// transcribed from x264 encoder/cavlc.c::cavlc_block_residual_internal and
// uses the VLC codewords generated from x264's own tables. The shader side
// includes the same cavlc_residual.glsl as the resident CAVLC encoder, so this
// compares the real GPU bit writer against x264-derived data and logic.
#include "x264_cavlc_codes.hpp"

namespace cavlcResidualCorpus {
constexpr int recordWords = 84, byteCapacity = 64;
struct Fixture {
    std::array<std::int32_t, 16> coefficients;
    std::int32_t nc, count, chromaDc, emittedBits;
    std::array<std::uint32_t, byteCapacity> bytes;
};
static_assert(sizeof(Fixture) == recordWords * sizeof(std::int32_t));

struct BitWriter {
    std::array<std::uint32_t, byteCapacity> words{};
    std::uint32_t count = 0, pending = 0, accumulator = 0;
    bool overflow = false;
    void put(std::uint32_t value, int bits) {
        while (bits > 0) {
            const int take = std::min<int>(32 - static_cast<int>(pending), bits);
            bits -= take;
            const std::uint32_t mask = take == 32 ? 0xffffffffu : ((1u << take) - 1u);
            accumulator |= ((value >> bits) & mask) << (32 - pending - take);
            pending += take;
            if (pending == 32) {
                if (count < words.size()) words[count] = accumulator;
                else overflow = true;
                ++count;
                pending = accumulator = 0;
            }
        }
    }
    std::uint32_t finish() {
        if (pending != 0) {
            if (count < words.size()) words[count] = accumulator;
            else overflow = true;
        }
        return overflow ? 0xffffffffu : 32 * count + pending;
    }
};

static void x264CavlcBlockResidual(const std::int32_t samples[16], int nc, int count, bool chromaDc,
                                   BitWriter& out) {
    int level[16], gap[16], total = 0, last = -1;
    for (int i = 0; i < count; ++i) {
        const int v = samples[i];
        if (v == 0) continue;
        level[total] = v;
        gap[total++] = i - last - 1;
        last = i;
    }
    int trailing = 0;
    for (int i = total - 1; i >= 0 && trailing < 3; --i) {
        if (std::abs(level[i]) != 1) break;
        ++trailing;
    }
    const int table = chromaDc ? 4 : (nc < 2 ? 0 : nc < 4 ? 1 : nc < 8 ? 2 : 3);
    const auto vlc = [&out](unsigned index) {
        const std::uint32_t entry = x264CavlcCodes[index];
        out.put(entry & 0xffffu, static_cast<int>(entry >> 16));
    };
    const auto encodeLevel = [&out](int code, int suffix) {
        const int prefix = code >> suffix;
        if (prefix < 14 || (suffix != 0 && prefix == 14)) {
            out.put(1u, prefix + 1);
            out.put(static_cast<std::uint32_t>(code), suffix);
        } else if (suffix == 0 && code < 30) {
            out.put(1u, 15);
            out.put(static_cast<std::uint32_t>(code - 14), 4);
        } else {
            std::uint32_t escape = static_cast<std::uint32_t>(code - (15 << suffix) - (suffix == 0 ? 15 : 0));
            int tail = 12;
            while (escape >= (1u << tail)) { escape -= 1u << tail; ++tail; }
            out.put(1u, tail + 4);
            out.put(escape, tail);
        }
    };
    vlc(static_cast<unsigned>((table * 17 + total) * 4 + trailing));
    if (total == 0) return;
    for (int i = 0; i < trailing; ++i) out.put(static_cast<std::uint32_t>(level[total - 1 - i] < 0), 1);
    int suffix = (total > 10 && trailing < 3) ? 1 : 0;
    for (int i = trailing; i < total; ++i) {
        const int value = level[total - 1 - i];
        int code = 2 * std::abs(value) - (value > 0 ? 2 : 1);
        if (i == trailing && trailing < 3) code -= 2;
        encodeLevel(code, suffix);
        suffix = std::max(suffix, 1);
        if (suffix < 6 && std::abs(value) > (3 << (suffix - 1))) ++suffix;
    }
    int zeros = last + 1 - total;
    if (total != count)
        vlc(static_cast<unsigned>((chromaDc ? 596 + 4 * total : 340 + 16 * total) + zeros));
    for (int i = total - 1; i > 0 && zeros != 0; --i) {
        vlc(static_cast<unsigned>(612 + 15 * std::min(zeros, 7) + gap[i]));
        zeros -= gap[i];
    }
}
} // namespace cavlcResidualCorpus

void verifyCavlcResidual(const char* shader) {
    using namespace cavlcResidualCorpus;
    std::mt19937 random(0x264ca7);
    std::vector<Fixture> cases;
    const auto add = [&](const std::array<std::int32_t, 16>& coeffs, int nc, int count, bool chromaDc) {
        Fixture f{};
        f.coefficients = coeffs;
        f.nc = nc;
        f.count = count;
        f.chromaDc = chromaDc ? 1 : 0;
        cases.push_back(f);
    };
    std::array<std::int32_t, 16> zero{};
    add(zero, 0, 16, false); add(zero, 3, 16, false); add(zero, 0, 4, true);
    for (int p = 0; p < 16; ++p) {
        auto a = zero; a[p] = p % 2 ? -1 : 1;
        add(a, p % 4, 16, false);
    }
    { auto a = zero; a.fill(1); add(a, 2, 16, false); }
    { auto a = zero; a.fill(-1); add(a, 3, 16, false); }
    { auto a = zero; a[0] = -32768; add(a, 0, 16, false); }
    { auto a = zero; a[15] = 32767; add(a, 3, 16, false); }
    for (int nc = 0; nc < 4; ++nc) for (int rep = 0; rep < 192; ++rep)
        for (const int count : {16, 15, 4}) {
            std::array<std::int32_t, 16> a{};
            for (int i = 0; i < count; ++i) {
                const unsigned r = random() % 100;
                if (r < 55) a[i] = 0;
                else if (r < 85) a[i] = (random() & 1) ? 1 : -1;
                else a[i] = static_cast<std::int32_t>(random() % 65535) - 32768;
            }
            add(a, nc, count, count == 4);
        }
    std::vector<std::int32_t> tables;
    tables.reserve(std::size(x264CavlcCodes));
    for (const auto code : x264CavlcCodes) tables.push_back(static_cast<std::int32_t>(code));
    const auto inputs = cases;
    execute(shader, cases, 1, tables);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        BitWriter reference;
        x264CavlcBlockResidual(input.coefficients.data(), input.nc, input.count, input.chromaDc != 0, reference);
        const auto bits = reference.finish();
        if (output.emittedBits != static_cast<std::int32_t>(bits) ||
            std::memcmp(output.bytes.data(), reference.words.data(), byteCapacity * sizeof(std::uint32_t)) != 0)
            throw std::runtime_error("CAVLC residual mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " CAVLC residual sequences matched x264-derived VLC codes and bit writer bytes.\n";
}

