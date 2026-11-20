// Vk264 parity module: fixtures_aq.inc
// Mirrors: x264 encoder/ratecontrol.c AQ
// Split from dct_parity.cpp lines 6363-6429.

namespace aqCorpus {
struct Fixture {
    std::array<std::uint32_t, 256> luma;
    std::array<std::uint32_t, 64> chromaCb;
    std::array<std::uint32_t, 64> chromaCr;
    std::uint32_t sumLuma, sqrLuma, sumCb, sqrCb, sumCr, sqrCr;
    std::uint32_t energy;
};
static_assert(sizeof(Fixture) == (256+64+64+7)*4);
} // namespace aqCorpus

void verifyAqVariance(const char* shader) {
    using namespace aqCorpus;
    std::mt19937 random(0xA9FDA11);
    std::vector<Fixture> cases(1024);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& fixture = cases[i];
        const auto mode = i % 4;
        if (mode == 0) {
            std::fill(fixture.luma.begin(), fixture.luma.end(), 128u);
            std::fill(fixture.chromaCb.begin(), fixture.chromaCb.end(), 130u);
            std::fill(fixture.chromaCr.begin(), fixture.chromaCr.end(), 126u);
        } else if (mode == 1) { // gradient with a step
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x)
                    fixture.luma[y*16+x] = static_cast<std::uint32_t>((x*9 + (y < 8 ? 40 : 0)) & 255);
            for (int i2 = 0; i2 < 64; ++i2) {
                fixture.chromaCb[i2] = static_cast<std::uint32_t>((i2*4) & 255);
                fixture.chromaCr[i2] = static_cast<std::uint32_t>((i2*7 + 13) & 255);
            }
        } else if (mode == 2) { // checkerboard + alternating chroma
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x)
                    fixture.luma[y*16+x] = (((x >> 2) + (y >> 2)) & 1) ? 255u : 0u;
            for (int i2 = 0; i2 < 64; ++i2) {
                fixture.chromaCb[i2] = ((i2 >> 1) & 1) ? 255u : 0u;
                fixture.chromaCr[i2] = ((i2 >> 1) & 1) ? 0u : 255u;
            }
        } else { // seeded random
            for (auto& p : fixture.luma) p = random() & 255;
            for (auto& p : fixture.chromaCb) p = random() & 255;
            for (auto& p : fixture.chromaCr) p = random() & 255;
        }
    }
    for (auto& marker : cases) { marker.sumLuma = 0xdeadbeefu; marker.energy = 0xdeadbeefu; }
    const auto input = cases; // captured pre-dispatch state (pixels only)
    vk264::ratecontrol::aq(shader, cases);
    const auto acEnergyVar = [](std::uint32_t sum, std::uint32_t ssd, int shift) -> std::uint32_t {
        return std::uint32_t(uint64_t(ssd) - (((uint64_t)sum * sum) >> shift));
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& in = input[i], &out = cases[i];
        std::uint32_t sumL = 0, sqrL = 0, sumCb = 0, sqrCb = 0, sumCr = 0, sqrCr = 0;
        for (std::uint32_t p : in.luma) { sumL += p; sqrL += p * p; }
        for (std::uint32_t p : in.chromaCb) { sumCb += p; sqrCb += p * p; }
        for (std::uint32_t p : in.chromaCr) { sumCr += p; sqrCr += p * p; }
        if (out.sumLuma != sumL || out.sqrLuma != sqrL) throw std::runtime_error("AQ luma sums differ at fixture " + std::to_string(i));
        if (out.sumCb != sumCb || out.sqrCb != sqrCb) throw std::runtime_error("AQ Cb sums differ at fixture " + std::to_string(i));
        if (out.sumCr != sumCr || out.sqrCr != sqrCr) throw std::runtime_error("AQ Cr sums differ at fixture " + std::to_string(i));
        const std::uint32_t energy = acEnergyVar(sumL, sqrL, 8) + acEnergyVar(sumCb, sqrCb, 6) + acEnergyVar(sumCr, sqrCr, 6);
        if (out.energy != energy)
            throw std::runtime_error("AQ variance differ at fixture " + std::to_string(i) +
                                     " (host " + std::to_string(energy) + " != shader " + std::to_string(out.energy) + ")");
    }
    std::cout << cases.size() << " adaptive-quant variance fixtures matched x264 exactly.\n";
}

