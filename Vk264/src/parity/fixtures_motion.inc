// Vk264 parity module: fixtures_motion.inc
// Mirrors: x264 common/mc.c + common/me.c + common/mvpred.c
// Split from dct_parity.cpp lines 2620-3572.

namespace mvPredictCorpus {
    using Pair = std::array<std::int32_t, 2>;
    struct Fixture {
        std::array<std::int32_t, 4> neighbors;
        std::array<Pair, 4> motion;
        Pair leftDelta, topDelta, prediction, pskip, contexts;
        std::int32_t reference, list;
    };
    static_assert(sizeof(Fixture) == 96);
}

void verifyMvPrediction(const char* shader) {
    using namespace mvPredictCorpus;
    constexpr int refs[]{-2, -1, 0, 1, 15};
    constexpr int motionSlots[]{X264_SCAN8_0-1, X264_SCAN8_0-8, X264_SCAN8_0-4, X264_SCAN8_0-9};
    constexpr int magnitudes[]{0, 1, 2, 3, 15, 16, 31, 32, 33, 65, 66};
    std::mt19937 random(0x26468);
    std::vector<Fixture> cases(5*5*5*5*12);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        std::size_t k = i;
        for (auto& neighbor : input.neighbors) {
            neighbor = refs[k%std::size(refs)];
            k /= std::size(refs);
        }
        input.reference = refs[2+k%3];
        input.list = k/3%2;
        for (auto& vector : input.motion)
            for (auto& component : vector) component = static_cast<int>(random() & 65535)-32768;
        if (k >= 6) input.motion[i%4].fill(0);
        if (i%11 == 0) input.motion[0] = {-32768,32767};
        for (int axis = 0; axis < 2; ++axis) {
            input.leftDelta[axis] = magnitudes[(i+axis)%std::size(magnitudes)];
            input.topDelta[axis] = magnitudes[(i/std::size(magnitudes)+axis)%std::size(magnitudes)];
        }
    }
    const auto inputs = cases;
    execute(shader, cases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        x264_t encoder{};
        for (int list = 0; list < 2; ++list)
            for (int neighbor = 0; neighbor < 4; ++neighbor) {
                const int slot = motionSlots[neighbor];
                encoder.mb.cache.ref[list][slot] = input.neighbors[neighbor];
                std::copy(input.motion[neighbor].begin(), input.motion[neighbor].end(),
                          encoder.mb.cache.mv[list][slot]);
            }
        std::int16_t predicted[2], pskip[2];
        x264_mb_predict_mv_16x16(&encoder, input.list, input.reference, predicted);
        x264_mb_predict_mv_pskip(&encoder, pskip);
        std::uint8_t left[2], top[2];
        std::copy(input.leftDelta.begin(), input.leftDelta.end(), left);
        std::copy(input.topDelta.begin(), input.topDelta.end(), top);
        const auto context = x264_cabac_mvd_sum(left, top);
        const std::uint8_t contexts[]{static_cast<std::uint8_t>(context), static_cast<std::uint8_t>(context >> 8)};
        if (!matchesNativeBytes(output.prediction, predicted) || !matchesNativeBytes(output.pskip, pskip) ||
            !matchesNativeBytes(output.contexts, contexts) || output.neighbors != input.neighbors ||
            output.motion != input.motion || output.leftDelta != input.leftDelta ||
            output.topDelta != input.topDelta || output.reference != input.reference || output.list != input.list)
            throw std::runtime_error("Motion prediction differs from x264 in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " motion predictor cases matched x264 16x16, P-skip and delta contexts.\n";
}

namespace motionCorpus {
    constexpr int stride = 64, planeSize = stride*stride;
    constexpr int filterOrigin = 5*stride + 5, filterWidth = 53, filterHeight = 53;
    constexpr int referenceOrigin = 24*stride + 24;
    constexpr int lumaStride = 24, chromaStride = 12, maxHeight = 16;
    struct Fixture {
        std::int32_t lumaX, lumaY, lumaWidth, lumaHeight;
        std::int32_t chromaX, chromaY, chromaWidth, chromaHeight;
        std::int32_t weighted, scale, denom, offset;
        std::int32_t refPlane, refOffset, refStride, prefiltered;
        std::array<std::uint32_t, planeSize> source, chroma;
        std::array<std::uint32_t, 3*planeSize> hpel;
        std::array<std::int32_t, filterWidth+5> scratch;
        std::array<std::uint32_t, lumaStride*maxHeight> luma, refScratch;
        std::array<std::uint32_t, chromaStride*maxHeight> chromaU, chromaV;
    };
    static_assert(sizeof(Fixture) == (16 + 5*planeSize + filterWidth+5 +
                  2*(lumaStride+chromaStride)*maxHeight) * sizeof(std::uint32_t));
}

void verifyMotionCompensation(const char* shader) {
    using namespace motionCorpus;
    constexpr int lumaShapes[][2] = {{4, 4}, {4, 8}, {8, 4}, {8, 8}, {8, 16},
                                    {16, 8}, {16, 16}, {16, 4}, {12, 12}, {20, 16}};
    constexpr int chromaShapes[][2] = {{2, 2}, {2, 4}, {2, 8}, {4, 4}, {4, 8},
                                      {4, 16}, {8, 8}, {8, 16}};
    std::mt19937 random(0x2646);
    std::vector<Fixture> cases(768);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        const auto& lumaShape = lumaShapes[i / 16 % std::size(lumaShapes)];
        const auto& chromaShape = chromaShapes[i / 64 % std::size(chromaShapes)];
        input.lumaWidth = lumaShape[0];
        input.lumaHeight = lumaShape[1];
        input.chromaWidth = chromaShape[0];
        input.chromaHeight = chromaShape[1];
        const int integerX = input.lumaWidth == 20 ? -2 : static_cast<int>(i / 32 % 5) - 2;
        input.lumaX = 4*integerX + static_cast<int>(i % 4);
        input.lumaY = 4*(static_cast<int>(i / 16 % 5) - 2) + static_cast<int>(i / 4 % 4);
        input.chromaX = 8*(static_cast<int>(i / 64 % 5) - 2) + static_cast<int>(i % 8);
        input.chromaY = 8*(static_cast<int>(i / 128 % 5) - 2) + static_cast<int>(i / 8 % 8);
        input.weighted = i / 64 % 2 != 0;
        input.denom = static_cast<int>(i / 16 % 8);
        input.scale = static_cast<int>(random() % 256) - 128;
        input.offset = static_cast<int>(random() % 256) - 128;
        for (int p = 0; p < planeSize; ++p) {
            switch (i / 16 % 8) {
                case 0: input.source[p] = (p % 2) ? 0 : 255; break;
                case 1: input.source[p] = ((p / stride) % 2) ? 255 : 0; break;
                case 2: input.source[p] = p % stride < stride / 2 ? 0 : 255; break;
                case 3: input.source[p] = 255; break;
                default: input.source[p] = random() & 255; break;
            }
            input.chroma[p] = random() & 255;
        }
        input.hpel.fill(211);
        input.luma.fill(219);
        input.refScratch.fill(231);
        input.chromaU.fill(241);
        input.chromaV.fill(243);
    }
    const auto inputs = cases;
    execute(shader, cases, 64);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        pixel source[planeSize], chroma[planeSize], hpel[3*planeSize];
        std::int16_t scratch[filterWidth+5]{};
        pixel luma[lumaStride*maxHeight], refScratch[lumaStride*maxHeight];
        pixel chromaU[chromaStride*maxHeight], chromaV[chromaStride*maxHeight];
        std::copy(input.source.begin(), input.source.end(), source);
        std::copy(input.chroma.begin(), input.chroma.end(), chroma);
        std::copy(input.hpel.begin(), input.hpel.end(), hpel);
        std::copy(input.luma.begin(), input.luma.end(), luma);
        std::copy(input.refScratch.begin(), input.refScratch.end(), refScratch);
        std::copy(input.chromaU.begin(), input.chromaU.end(), chromaU);
        std::copy(input.chromaV.begin(), input.chromaV.end(), chromaV);
        hpel_filter(hpel + filterOrigin, hpel + planeSize + filterOrigin, hpel + 2*planeSize + filterOrigin,
                    source + filterOrigin, stride, filterWidth, filterHeight, scratch);
        pixel* planeBase[] = {source, hpel, hpel + planeSize, hpel + 2*planeSize};
        pixel* planes[] = {source + referenceOrigin, hpel + referenceOrigin,
                           hpel + planeSize + referenceOrigin, hpel + 2*planeSize + referenceOrigin};
        x264_weight_t weight{};
        weight.i_scale = input.scale;
        weight.i_denom = input.denom;
        weight.i_offset = input.offset;
        if (input.weighted) weight.weightfn = mc_weight_wtab;
        mc_luma(luma, lumaStride, planes, stride, input.lumaX, input.lumaY, input.lumaWidth, input.lumaHeight, &weight);
        std::intptr_t refStride = lumaStride;
        const auto* ref = get_ref(refScratch, &refStride, planes, stride, input.lumaX, input.lumaY,
                                  input.lumaWidth, input.lumaHeight, &weight);
        const pixel* reportedRef = nullptr;
        if (output.refPlane == -1 && output.refOffset == 0) reportedRef = refScratch;
        else if (output.refPlane >= 0 && output.refPlane < 4 &&
                 output.refOffset >= 0 && output.refOffset < planeSize)
            reportedRef = planeBase[output.refPlane] + output.refOffset;
        mc_chroma(chromaU, chromaV, chromaStride, chroma + referenceOrigin, stride,
                  input.chromaX, input.chromaY, input.chromaWidth, input.chromaHeight);
        if (!matchesNativeBytes(output.hpel, hpel) || !matchesNativeBytes(output.scratch, scratch) ||
            !matchesNativeBytes(output.luma, luma) || !matchesNativeBytes(output.refScratch, refScratch) ||
            ref != reportedRef || output.refStride != refStride ||
            !matchesNativeBytes(output.chromaU, chromaU) || !matchesNativeBytes(output.chromaV, chromaV) ||
            !matchesNativeBytes(output.source, source) || !matchesNativeBytes(output.chroma, chroma))
            throw std::runtime_error("Motion compensation mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " fixtures matched x264 half-pel, luma, reference and chroma motion compensation.\n";
}

struct FilteredReference {
    static constexpr int border = 32, headerWords = 4;
    int width, height, stride, rows;
    std::vector<std::uint32_t> words;

    FilteredReference(const std::vector<pixel>& source, int w, int h, const char* shader)
        : width(w), height(h), stride(w+2*border), rows(h+2*border),
          words(headerWords+width*height+3*stride*rows) {
        words[0] = width;
        words[1] = height;
        words[2] = border;
        words[3] = stride*rows;
        std::copy(source.begin(), source.end(), words.begin()+headerWords);
        execute(shader, words, 1, {}, stride*rows);
    }

    const std::uint32_t* plane(int index) const {
        return words.data()+headerWords+width*height+index*stride*rows;
    }
};

void verifyReferenceFrame(const char* shader) {
    std::mt19937 random(0x26446);
    constexpr int shapes[][2]{{2,2}, {16,30}, {112,80}, {128,64}, {240,144}};
    for (const auto& shape : shapes) {
        const int width = shape[0], height = shape[1];
        std::vector<pixel> source(width*height);
        for (auto& sample : source) sample = static_cast<pixel>(random() & 255);
        const FilteredReference filtered(source, width, height, shader);
        constexpr int margin = 4;
        const int stride = filtered.stride+2*margin, rows = filtered.rows+2*margin;
        std::vector<pixel> padded(stride*rows);
        std::array<std::vector<pixel>, 3> reference;
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < stride; ++x) {
                const int sx = std::clamp(x-margin-filtered.border, 0, width-1);
                const int sy = std::clamp(y-margin-filtered.border, 0, height-1);
                padded[y*stride+x] = source[sy*width+sx];
            }
        for (auto& plane : reference) plane.resize(stride*rows);
        std::vector<std::int16_t> scratch(filtered.stride+5);
        const int offset = margin*stride+margin;
        hpel_filter(reference[0].data()+offset, reference[1].data()+offset, reference[2].data()+offset,
                    padded.data()+offset, stride, filtered.stride, filtered.rows, scratch.data());
        for (int p = 0; p < 3; ++p)
            for (int y = 0; y < filtered.rows; ++y) {
                const auto* actual = filtered.plane(p)+y*filtered.stride;
                const auto* expected = reference[p].data()+offset+y*stride;
                if (!std::equal(actual, actual+filtered.stride, expected))
                    throw std::runtime_error("Full-frame half-pel plane differs from x264");
            }
    }
    std::cout << std::size(shapes) << " padded reference frames matched x264 half-pel samples.\n";
}

namespace motionMetricsCorpus {
    constexpr int candidateStride = 24, rows = 16;
    struct Fixture {
        std::int32_t width, height, count;
        std::array<std::uint32_t, FENC_STRIDE*rows> source;
        std::array<std::uint32_t, 4*candidateStride*rows> candidate;
        std::array<std::int32_t, 4> sad, ssd, satd, sa8d;
    };
    static_assert(sizeof(Fixture) == (19 + (FENC_STRIDE + 4*candidateStride)*rows) * sizeof(std::uint32_t));
}

void verifyMotionMetrics(const char* shader) {
    using namespace motionMetricsCorpus;
    constexpr int sizes[][2] = {{16,16}, {16,8}, {8,16}, {8,8}, {8,4}, {4,8}, {4,4}, {4,16}};
    using PixelCmp = int (*)(pixel*, std::intptr_t, pixel*, std::intptr_t);
    struct Routines {
        PixelCmp sad, ssd, satd, sa8d;
        void (*sadX3)(pixel*, pixel*, pixel*, pixel*, std::intptr_t, int*);
        void (*sadX4)(pixel*, pixel*, pixel*, pixel*, pixel*, std::intptr_t, int*);
        void (*satdX3)(pixel*, pixel*, pixel*, pixel*, std::intptr_t, int*);
        void (*satdX4)(pixel*, pixel*, pixel*, pixel*, pixel*, std::intptr_t, int*);
    };
#define METRICS(size, sa8d) {x264_pixel_sad_##size, x264_pixel_ssd_##size, x264_pixel_satd_##size, sa8d, \
                             x264_pixel_sad_x3_##size, x264_pixel_sad_x4_##size, \
                             x264_pixel_satd_x3_##size, x264_pixel_satd_x4_##size}
    constexpr Routines comparisons[] = {
        METRICS(16x16, x264_pixel_sa8d_16x16), METRICS(16x8, nullptr), METRICS(8x16, nullptr),
        METRICS(8x8, x264_pixel_sa8d_8x8), METRICS(8x4, nullptr), METRICS(4x8, nullptr), METRICS(4x4, nullptr),
        {x264_pixel_sad_4x16, x264_pixel_ssd_4x16, x264_pixel_satd_4x16, nullptr,
         nullptr, nullptr, nullptr, nullptr},
    };
#undef METRICS
    std::mt19937 random(0x2645a);
    constexpr int untouched = -1234;
    std::vector<Fixture> cases(std::size(sizes) * 256);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        const auto size = i % std::size(sizes);
        const auto pattern = i / std::size(sizes);
        input.width = sizes[size][0];
        input.height = sizes[size][1];
        input.count = size == 7 || pattern % 3 == 0 ? 1 : (pattern % 3 == 1 ? 3 : 4);
        for (auto& value : input.source) value = random() & 255;
        for (auto& value : input.candidate) value = random() & 255;
        const auto boundary = pattern / 3;
        if (boundary == 0 || boundary == 1) {
            input.source.fill(boundary ? 255 : 0);
            input.candidate.fill(boundary ? 0 : 255);
        }
        if (boundary == 2) {
            for (std::size_t p = 0; p < input.source.size(); ++p) input.source[p] = (p % 2) ? 255 : 0;
            for (std::size_t p = 0; p < input.candidate.size(); ++p) input.candidate[p] = (p % 2) ? 0 : 255;
        }
        if (boundary == 3) {
            for (int candidate = 0; candidate < 4; ++candidate)
                for (int y = 0; y < rows; ++y)
                    std::copy_n(input.source.data() + y*FENC_STRIDE, FENC_STRIDE,
                                input.candidate.data() + (candidate*rows+y)*candidateStride);
        }
        input.sad.fill(untouched);
        input.ssd.fill(untouched);
        input.satd.fill(untouched);
        input.sa8d.fill(untouched);
    }
    const auto inputs = cases;
    execute(shader, cases, 4);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        const auto& reference = comparisons[i % std::size(sizes)];
        pixel source[FENC_STRIDE*rows], candidates[4*candidateStride*rows];
        std::copy(input.source.begin(), input.source.end(), source);
        std::copy(input.candidate.begin(), input.candidate.end(), candidates);
        pixel* a = candidates;
        pixel* b = a + candidateStride*rows;
        pixel* c = b + candidateStride*rows;
        pixel* d = c + candidateStride*rows;
        constexpr int blank[4] = {untouched, untouched, untouched, untouched};
        int sad[4], ssd[4], satd[4], sa8d[4];
        std::copy_n(blank, 4, sad);
        std::copy_n(blank, 4, ssd);
        std::copy_n(blank, 4, satd);
        std::copy_n(blank, 4, sa8d);
        if (input.count == 3) {
            reference.sadX3(source, a, b, c, candidateStride, sad);
            reference.satdX3(source, a, b, c, candidateStride, satd);
        } else if (input.count == 4) {
            reference.sadX4(source, a, b, c, d, candidateStride, sad);
            reference.satdX4(source, a, b, c, d, candidateStride, satd);
        } else {
            sad[0] = reference.sad(source, FENC_STRIDE, a, candidateStride);
            satd[0] = reference.satd(source, FENC_STRIDE, a, candidateStride);
        }
        for (int n = 0; n < input.count; ++n) {
            auto* candidate = candidates + n*candidateStride*rows;
            ssd[n] = reference.ssd(source, FENC_STRIDE, candidate, candidateStride);
            if (reference.sa8d) sa8d[n] = reference.sa8d(source, FENC_STRIDE, candidate, candidateStride);
        }
        if (!matchesNativeBytes(output.sad, sad) || !matchesNativeBytes(output.ssd, ssd) ||
            !matchesNativeBytes(output.satd, satd) || !matchesNativeBytes(output.sa8d, sa8d) ||
            !matchesNativeBytes(output.source, source) || !matchesNativeBytes(output.candidate, candidates))
            throw std::runtime_error("Motion metric mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " fixtures matched 54 x264 SAD, SSD, SATD and SA8D functions.\n";
}

void verifyMotionElimination(const char* shader) {
    constexpr int maxCandidates = 96;
    struct Fixture {
        std::int32_t width, delta, threshold, blocks;
        std::array<std::int32_t, 4> encodedDc;
        std::array<std::uint32_t, 2*maxCandidates+8> sums;
        std::array<std::uint32_t, maxCandidates> motionCost;
        std::array<std::int32_t, maxCandidates> motion;
        std::int32_t count;
    };
    static_assert(sizeof(Fixture) == (17 + 4*maxCandidates) * sizeof(std::uint32_t));
    std::mt19937 random(0x264ad5);
    std::vector<Fixture> cases(3 * 1024);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        const auto pattern = i / 3;
        input.blocks = 1 << (i % 3);
        input.width = pattern == 0 ? 0 : 1 + random() % maxCandidates;
        input.delta = 1 + random() % maxCandidates;
        input.threshold = random() % 163840;
        for (auto& value : input.encodedDc) value = random() & 65535;
        for (auto& value : input.sums) value = random() & 65535;
        for (auto& value : input.motionCost) value = random() & 65535;
        input.motion.fill(-256);
        if (pattern < 4) {
            input.encodedDc.fill(20);
            input.sums.fill(20);
            input.motionCost.fill(100);
            input.motionCost[0] = 99;
            input.threshold = pattern == 3 ? 101 : 100;
        }
    }
    const auto inputs = cases;
    execute(shader, cases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        int dc[4];
        std::uint16_t sums[2*maxCandidates+8], costs[maxCandidates];
        std::int16_t motion[maxCandidates];
        std::copy(input.encodedDc.begin(), input.encodedDc.end(), dc);
        std::copy(input.sums.begin(), input.sums.end(), sums);
        std::copy(input.motionCost.begin(), input.motionCost.end(), costs);
        std::copy(input.motion.begin(), input.motion.end(), motion);
        const auto ads = input.blocks == 4 ? x264_pixel_ads4 : input.blocks == 2 ? x264_pixel_ads2 : x264_pixel_ads1;
        const int count = ads(dc, sums, input.delta, costs, motion, input.width, input.threshold);
        if (output.count != count || !matchesNativeBytes(output.motion, motion) ||
            !matchesNativeBytes(output.encodedDc, dc) || !matchesNativeBytes(output.sums, sums) ||
            !matchesNativeBytes(output.motionCost, costs))
            throw std::runtime_error("Motion elimination mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " fixtures matched all three x264 successive elimination routines.\n";
}

void verifyMotionIntegral(const char* shader) {
    constexpr int maxStride = 64;
    struct Fixture {
        std::int32_t stride;
        std::array<std::uint32_t, maxStride> pixels;
        std::array<std::uint32_t, 9*maxStride> integral;
        std::array<std::uint32_t, maxStride> sum4h, sum8h, sum4v, sum8From4, sum8v;
    };
    static_assert(sizeof(Fixture) == (1 + 15*maxStride) * sizeof(std::uint32_t));
    std::mt19937 random(0x26414);
    std::vector<Fixture> cases(1024);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        input.stride = 8 + static_cast<int>(i % (maxStride-7));
        for (auto& value : input.pixels) value = random() & 255;
        for (auto& value : input.integral) value = random() & 65535;
        if (i % 5 == 0) input.integral.fill(65535);
        if (i % 7 == 0) input.pixels.fill(255);
        input.sum4h.fill(24117);
        input.sum8h.fill(28579);
        input.sum4v.fill(41427);
        std::copy_n(input.integral.begin(), maxStride, input.sum8From4.begin());
        input.sum8v = input.sum8From4;
    }
    const auto inputs = cases;
    execute(shader, cases, 64);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        const int stride = input.stride;
        pixel pixels[maxStride];
        std::uint16_t integral4h[2*maxStride], integral8h[2*maxStride], sum4h[maxStride], sum8h[maxStride];
        std::uint16_t integral4v[9*maxStride], integral8v[9*maxStride], sum4v[maxStride];
        std::uint16_t sum8From4[maxStride], sum8v[maxStride];
        std::copy(input.pixels.begin(), input.pixels.end(), pixels);
        std::copy_n(input.integral.begin(), stride, integral4h);
        std::copy_n(input.integral.begin(), stride, integral8h);
        std::copy(input.sum4h.begin(), input.sum4h.end(), integral4h + stride);
        std::copy(input.sum8h.begin(), input.sum8h.end(), integral8h + stride);
        std::copy(input.integral.begin(), input.integral.end(), integral4v);
        std::copy(input.integral.begin(), input.integral.end(), integral8v);
        std::copy(input.sum4v.begin(), input.sum4v.end(), sum4v);
        integral_init4h(integral4h + stride, pixels, stride);
        integral_init8h(integral8h + stride, pixels, stride);
        integral_init4v(integral4v, sum4v, stride);
        integral_init8v(integral8v, stride);
        std::copy_n(integral4h + stride, maxStride, sum4h);
        std::copy_n(integral8h + stride, maxStride, sum8h);
        std::copy_n(integral4v, maxStride, sum8From4);
        std::copy_n(integral8v, maxStride, sum8v);
        if (!matchesNativeBytes(output.pixels, pixels) || output.stride != stride ||
            output.integral != input.integral || !matchesNativeBytes(output.sum4h, sum4h) ||
            !matchesNativeBytes(output.sum8h, sum8h) || !matchesNativeBytes(output.sum4v, sum4v) ||
            !matchesNativeBytes(output.sum8From4, sum8From4) || !matchesNativeBytes(output.sum8v, sum8v))
            throw std::runtime_error("Motion integral mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " fixtures matched all four x264 integral-sum routines byte for byte.\n";
}

void verifyMotionLowres(const char* shader) {
    constexpr int inputSize = 37*33, outputSize = 21*16;
    struct Fixture {
        std::int32_t width, height, srcStride, dstStride;
        std::array<std::uint32_t, inputSize> source;
        std::array<std::uint32_t, 4*outputSize> outputPlanes;
    };
    static_assert(sizeof(Fixture) == (4 + inputSize + 4*outputSize)*sizeof(std::uint32_t));
    std::mt19937 random(0x26410);
    std::vector<Fixture> cases(1024);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        input.width = 1 + static_cast<int>(i % 16);
        input.height = 1 + static_cast<int>(i / 16 % 16);
        input.srcStride = 2*input.width + 1 + static_cast<int>(i / 256 % 4);
        input.dstStride = input.width + static_cast<int>(i / 32 % 6);
        for (auto& sample : input.source)
            sample = i % 3 == 0 ? random() % 5 : random() & 255;
        // The frame wrapper supplies a replicated column and row to the core.
        for (int y = 0; y < 2*input.height; ++y) {
            const int pos = y*input.srcStride + 2*input.width;
            input.source[pos] = input.source[pos-1];
        }
        std::copy_n(input.source.data() + (2*input.height-1)*input.srcStride, 2*input.width+1,
                    input.source.data() + 2*input.height*input.srcStride);
        input.outputPlanes.fill(207);
    }
    const auto inputs = cases;
    execute(shader, cases, 64);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        pixel source[inputSize], planes[4*outputSize];
        std::copy(input.source.begin(), input.source.end(), source);
        std::copy(input.outputPlanes.begin(), input.outputPlanes.end(), planes);
        frame_init_lowres_core(source, planes, planes + outputSize, planes + 2*outputSize, planes + 3*outputSize,
                               input.srcStride, input.dstStride, input.width, input.height);
        if (!matchesNativeBytes(output.outputPlanes, planes) || !matchesNativeBytes(output.source, source) ||
            output.width != input.width || output.height != input.height ||
            output.srcStride != input.srcStride || output.dstStride != input.dstStride)
            throw std::runtime_error("Low-resolution reference mismatch in fixture " + std::to_string(i));
    }
    std::cout << cases.size() << " fixtures matched all four x264 low-resolution reference planes byte for byte.\n";
}

thread_local int tracedAdsWidth = -1;
template<PixelAds Ads>
int traceAdsWidth(int* dc, std::uint16_t* sums, int delta, std::uint16_t* xCost,
                  std::int16_t* positions, int width, int threshold) {
    tracedAdsWidth = width;
    return Ads(dc, sums, delta, xCost, positions, width, threshold);
}

namespace motionSearchCorpus {
    constexpr int stride = motionCorpus::stride, planeSize = motionCorpus::planeSize, origin = motionCorpus::referenceOrigin;
    constexpr int costRadius = 192, maxPredictors = 14;
    struct alignas(8) Fixture {
        std::int32_t method, pixelSize, range, subme;
        std::array<std::int32_t, 2> minFpel, maxFpel, minQpel, maxQpel, mvp;
        std::int32_t predictorCount, useSatd, useThreshold, threshold;
        std::int32_t weighted, scale, denom, offset;
        std::int32_t chromaMode, interlaced, fieldY, refIndex;
        std::int32_t operation, refCost;
        std::array<std::int32_t, 8> chromaWeights;
        std::array<std::int32_t, 2*maxPredictors> predictors;
        std::array<std::uint32_t, 2*costRadius+1> motionCost;
        std::array<std::uint32_t, 16*16> source;
        std::array<std::uint32_t, 4*planeSize> reference;
        std::array<std::uint32_t, 2*16*16> chromaSource;
        std::array<std::uint32_t, 8*planeSize/4> chromaReference;
        std::int32_t resultX, resultY, resultCost, resultMvCost;
        alignas(8) std::array<std::int32_t, 2*planeSize> tesaCandidates;
    };
    static_assert(sizeof(Fixture) == 33990*sizeof(std::uint32_t));
    static_assert(offsetof(Fixture, resultX) == 25793*sizeof(std::uint32_t));
}

void verifyMotionSearch(const char* shader, bool exhaustive) {
    using namespace motionSearchCorpus;
    std::mt19937 random(0x264ce);
    constexpr int blockSizes = 7, submeCount = 12;
    const int methods = exhaustive ? 2 : 3, methodBase = exhaustive ? X264_ME_ESA : X264_ME_DIA;
    const int searchCases = methods * blockSizes * submeCount * 3;
    constexpr int refinementCases = 2 * blockSizes * submeCount;
    const int wideSearchStart = searchCases + refinementCases;
    const int wideCases = exhaustive ? 0 : methods;
    std::vector<Fixture> cases(wideSearchStart + wideCases);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        input.method = methodBase + static_cast<int>(i % methods);
        input.pixelSize = static_cast<int>(i / methods % blockSizes);
        input.subme = static_cast<int>(i / (methods*blockSizes) % submeCount);
        const auto pattern = std::min<std::size_t>(2, i / (methods*blockSizes*submeCount));
        const bool wideSearch = i >= wideSearchStart;
        if (i >= searchCases && !wideSearch) {
            const auto n = i-searchCases;
            input.operation = 1 + static_cast<int>(n % 2);
            input.pixelSize = static_cast<int>(n / 2 % blockSizes);
            input.subme = static_cast<int>(n / (2*blockSizes));
            input.refCost = 1 + random() % 200;
        }
        input.range = 4 + random() % 13;
        input.minFpel = {-13, -14};
        input.maxFpel = {12, 11};
        input.minQpel = {-54, -58};
        input.maxQpel = {50, 46};
        for (auto& component : input.mvp) component = static_cast<int>(random() % 129) - 64;
        input.predictorCount = static_cast<int>(random() % (maxPredictors+1));
        input.useSatd = i % 11 != 0 && (!exhaustive || input.subme > 1);
        input.useThreshold = i % 3 != (i / 3 % 3);
        input.threshold = pattern == 0 ? 500 : (pattern == 1 ? 6000 : COST_MAX);
        input.weighted = pattern == 2 && i % 4 != 0;
        input.denom = static_cast<int>(i % 4);
        input.scale = input.weighted ? (1 << input.denom) + static_cast<int>(random() % 3) - 1 : 1;
        input.offset = input.weighted ? static_cast<int>(random() % 33) - 16 : 0;
        input.chromaMode = pattern == 0 ? 0 : 1 + static_cast<int>((i / 3 + i / 21) % 3);
        input.interlaced = static_cast<int>(i / 2 % 2);
        input.fieldY = static_cast<int>(i / 3 % 2);
        input.refIndex = static_cast<int>(i / 5 % 4);
        for (int n = 0; n < 2; ++n) {
            auto* weights = input.chromaWeights.data() + 4*n;
            weights[0] = pattern == 2 && i % 3 != n;
            weights[2] = static_cast<int>((i + n) % 4);
            weights[1] = (1 << weights[2]) + static_cast<int>(random() % 3) - 1;
            weights[3] = static_cast<int>(random() % 33) - 16;
        }
        input.resultMvCost = -7931;
        if (input.operation) {
            input.resultX = i % 17 == 0 ? input.minQpel[0] : static_cast<int>(random() % 81)-40;
            input.resultY = i % 19 == 0 ? input.maxQpel[1] : static_cast<int>(random() % 81)-40;
            input.resultCost = input.refCost + static_cast<int>(random() % 28000);
            input.threshold = i % 4 == 0 ? 50 : 2200 + static_cast<int>(random() % 9000);
        }
        if (input.method == X264_ME_UMH && input.subme >= 8 && pattern == 2) input.range = 24;
        if (exhaustive) input.range = 4 + static_cast<int>(i / methods % 27);
        if (wideSearch) {
            input.method = static_cast<int>(i - wideSearchStart);
            input.pixelSize = PIXEL_8x8;
            input.subme = 8;
            input.range = 24;
            input.maxFpel[0] = 27;
            input.maxQpel[0] = 108;
            input.mvp = {26*4, 0};
            input.predictorCount = 0;
            input.useThreshold = input.chromaMode = input.weighted = 0;
        }
        for (auto& component : input.predictors) component = static_cast<int>(random() % 193) - 96;
        input.tesaCandidates.fill(-1234);
        if (i % 8 == 0) {
            input.predictors[0] = input.mvp[0];
            input.predictors[1] = input.mvp[1];
            input.predictors[2] = input.predictors[3] = 0;
        }
        const int lambda = 1 + random() % 32;
        for (int d = -costRadius; d <= costRadius; ++d) {
            int magnitude = std::abs(d);
            int bits = 1;
            for (; magnitude != 0; magnitude >>= 1) bits += 2;
            input.motionCost[d+costRadius] = (i % 13 == 0) ? 0 : lambda*bits;
        }
        pixel planes[4][planeSize]{};
        for (int p = 0; p < planeSize; ++p) {
            const int x = p % stride, y = p / stride;
            planes[0][p] = static_cast<pixel>(pattern == 0 && i % 4 == 0 ? 128 :
                pattern == 0 ? (x*3 + y*2) & 255 : pattern == 1 ? random() & 255 :
                (x*7 + y*13 + ((x/8) ^ (y/8))*11 + random()%9) & 255);
        }
        constexpr int filterOrigin = 5*stride+5, filterSize = 53;
        std::int16_t temp[filterSize + 5];
        hpel_filter(planes[1] + filterOrigin, planes[2] + filterOrigin, planes[3] + filterOrigin,
                    planes[0] + filterOrigin, stride, filterSize, filterSize, temp);
        for (int plane = 0; plane < 4; ++plane)
            std::copy_n(planes[plane], planeSize, input.reference.data() + plane*planeSize);
        for (auto& value : input.source) value = random() & 255;
        if (pattern != 1) {
            pixel* planePointers[4];
            for (int plane = 0; plane < 4; ++plane) planePointers[plane] = planes[plane] + origin;
            pixel block[FENC_STRIDE*16]{};
            x264_weight_t weight{};
            weight.i_scale = input.scale;
            weight.i_denom = input.denom;
            weight.i_offset = input.offset;
            if (input.weighted) weight.weightfn = mc_weight_wtab;
            const int mx = wideSearch ? 26*4 : static_cast<int>(random() % 65) - 32;
            const int my = wideSearch ? 0 : static_cast<int>(random() % 65) - 32;
            const auto shape = x264_pixel_size[input.pixelSize];
            mc_luma(block, FENC_STRIDE, planePointers, stride, mx, my, shape.w, shape.h, &weight);
            for (int y = 0; y < shape.h; ++y)
                for (int x = 0; x < shape.w; ++x)
                    input.source[y*FENC_STRIDE+x] = block[y*FENC_STRIDE+x];
        }
        if (input.chromaMode != 0) {
            pixel chroma[8][planeSize]{};
            for (auto& value : input.chromaSource) value = random() & 255;
            const int chromaPlanes = input.chromaMode == 3 ? 2 : 1;
            for (int n = 0; n < chromaPlanes; ++n) {
                auto* base = chroma + 4*n;
                for (auto& value : base[0]) value = random() & 255;
                if (input.chromaMode == 3)
                    hpel_filter(base[1] + filterOrigin, base[2] + filterOrigin, base[3] + filterOrigin,
                                base[0] + filterOrigin, stride, filterSize, filterSize, temp);
            }
            for (int plane = 0; plane < 8; ++plane)
                for (int p = 0; p < planeSize; p += 4) {
                    std::uint32_t packed = 0;
                    for (int byte = 0; byte < 4; ++byte) packed |= std::uint32_t(chroma[plane][p+byte]) << (8*byte);
                    input.chromaReference[(plane*planeSize+p)/4] = packed;
                }
        }
        if (exhaustive && input.operation == 0 && input.method == X264_ME_TESA &&
            input.pixelSize == PIXEL_16x16 && input.subme % 3 == 0) {
            input.mvp = {0, 0};
            input.predictorCount = 0;
            input.weighted = 0;
            input.motionCost.fill(0);
            input.source.fill(130);
            input.reference.fill(128);
        }
    }
    const auto inputs = cases;
    execute(shader, cases);
    x264_t encoder{};
    encoder.mc.get_ref = get_ref;
    encoder.mc.mc_luma = mc_luma;
    encoder.mc.mc_chroma = mc_chroma;
#define SAD_DISPATCH(size) x264_pixel_sad_##size, x264_pixel_sad_x3_##size, x264_pixel_sad_x4_##size, \
                          x264_pixel_satd_##size, x264_pixel_satd_x3_##size, x264_pixel_satd_x4_##size
    struct Metrics { PixelCmp sad; PixelCmpX3 sadX3; PixelCmpX4 sadX4; PixelCmp satd; PixelCmpX3 satdX3; PixelCmpX4 satdX4; };
    constexpr Metrics metrics[] = {
        {SAD_DISPATCH(16x16)}, {SAD_DISPATCH(16x8)}, {SAD_DISPATCH(8x16)}, {SAD_DISPATCH(8x8)},
        {SAD_DISPATCH(8x4)}, {SAD_DISPATCH(4x8)}, {SAD_DISPATCH(4x4)},
    };
#undef SAD_DISPATCH
    constexpr PixelAds ads[] = {traceAdsWidth<x264_pixel_ads4>, traceAdsWidth<x264_pixel_ads2>,
                                traceAdsWidth<x264_pixel_ads2>, traceAdsWidth<x264_pixel_ads1>,
                                traceAdsWidth<x264_pixel_ads2>, traceAdsWidth<x264_pixel_ads2>,
                                traceAdsWidth<x264_pixel_ads1>};
    constexpr int integralPlane = stride*(stride+2*PADV);
    ReferenceFrameLines frameLines{{stride}};
    encoder.fenc = &frameLines;
    alignas(8) std::int32_t scratch[2*planeSize+64]{};
    encoder.scratch_buffer = reinterpret_cast<std::int16_t*>(scratch);
    std::uint16_t phaseCosts[4][2*costRadius+1]{};
    for (int phase = 0; phase < 4; ++phase) encoder.cost_mv_fpel[0][phase] = phaseCosts[phase] + costRadius;
    int thresholdReturns = 0;
    int wideCandidateLists = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        const bool fullpelSatd = input.method == X264_ME_TESA && input.useSatd;
        for (int n = 0; n < blockSizes; ++n) {
            encoder.pixf.sad[n] = metrics[n].sad;
            encoder.pixf.sad_x3[n] = metrics[n].sadX3;
            encoder.pixf.sad_x4[n] = metrics[n].sadX4;
            encoder.pixf.ads[n] = ads[n];
            encoder.pixf.fpelcmp[n] = fullpelSatd ? metrics[n].satd : metrics[n].sad;
            encoder.pixf.fpelcmp_x3[n] = fullpelSatd ? metrics[n].satdX3 : metrics[n].sadX3;
            encoder.pixf.fpelcmp_x4[n] = fullpelSatd ? metrics[n].satdX4 : metrics[n].sadX4;
            encoder.pixf.mbcmp_unaligned[n] = input.useSatd ? metrics[n].satd : metrics[n].sad;
        }
        encoder.param.analyse.i_me_range = input.range;
        encoder.mb.i_me_method = input.method;
        encoder.mb.i_subpel_refine = input.subme;
        encoder.mb.b_chroma_me = input.chromaMode != 0;
        encoder.chroma444 = input.chromaMode == 3;
        encoder.chromaVShift = input.chromaMode == 1;
        encoder.interlaced = input.interlaced;
        encoder.mb.i_mb_y = input.fieldY;
        constexpr int chroma420[] = {PIXEL_8x8, PIXEL_8x4, PIXEL_4x8, PIXEL_4x4};
        constexpr int chroma422[] = {PIXEL_8x16, PIXEL_8x8, PIXEL_4x16, PIXEL_4x8};
        for (int n = 0; n < 4; ++n)
            encoder.luma2chroma_pixel[n] = input.chromaMode == 1 ? chroma420[n] : chroma422[n];
        for (int n = 0; n < blockSizes; ++n)
            encoder.pixf.mbcmp[n] = encoder.pixf.mbcmp_unaligned[n];
        encoder.pixf.mbcmp[PIXEL_4x16] = input.useSatd ? x264_pixel_satd_4x16 : x264_pixel_sad_4x16;
        for (int component = 0; component < 2; ++component) {
            encoder.mb.mv_limit_fpel[0][component] = static_cast<std::int16_t>(input.minFpel[component]);
            encoder.mb.mv_limit_fpel[1][component] = static_cast<std::int16_t>(input.maxFpel[component]);
            encoder.mb.mv_min_spel[component] = static_cast<std::int16_t>(input.minQpel[component]);
            encoder.mb.mv_max_spel[component] = static_cast<std::int16_t>(input.maxQpel[component]);
        }
        x264_me_t me{};
        me.i_pixel = input.pixelSize;
        me.i_stride[0] = stride;
        me.i_stride[1] = me.i_stride[2] = stride;
        me.i_ref = input.refIndex;
        me.i_ref_cost = input.refCost;
        me.cost_mv = input.resultMvCost;
        me.cost = input.resultCost;
        me.mv[0] = static_cast<std::int16_t>(input.resultX);
        me.mv[1] = static_cast<std::int16_t>(input.resultY);
        std::copy(input.mvp.begin(), input.mvp.end(), me.mvp);
        std::int16_t predictors[maxPredictors][2];
        for (int n = 0; n < maxPredictors; ++n)
            for (int component = 0; component < 2; ++component)
                predictors[n][component] = static_cast<std::int16_t>(input.predictors[2*n+component]);
        std::uint16_t motionCost[2*costRadius+1];
        std::copy(input.motionCost.begin(), input.motionCost.end(), motionCost);
        me.p_cost_mv = motionCost + costRadius;
        pixel source[FENC_STRIDE*16], reference[4*planeSize], weighted[planeSize];
        std::copy(input.source.begin(), input.source.end(), source);
        std::copy(input.reference.begin(), input.reference.end(), reference);
        for (int plane = 0; plane < 4; ++plane)
            me.p_fref[plane] = reference + plane*planeSize + origin;
        me.p_fenc[0] = source;
        x264_weight_t weight[3]{};
        weight[0].i_scale = input.scale;
        weight[0].i_denom = input.denom;
        weight[0].i_offset = input.offset;
        if (input.weighted) {
            weight[0].weightfn = mc_weight_wtab;
            mc_weight(weighted, stride, reference, stride, &weight[0], stride, stride);
        }
        pixel chromaReference[8*planeSize], chromaSource[2][16*16];
        for (std::size_t q = 0; q < input.chromaReference.size(); ++q)
            for (int b = 0; b < 4; ++b)
                chromaReference[4*q+b] = static_cast<pixel>(input.chromaReference[q] >> (8*b));
        for (int plane = 0; plane < 8; ++plane)
            me.p_fref[plane+4] = chromaReference + plane*planeSize + origin;
        for (int n = 0; n < 2; ++n) {
            std::copy_n(input.chromaSource.data() + 16*16*n, 16*16, chromaSource[n]);
            me.p_fenc[1+n] = chromaSource[n];
            auto& w = weight[1+n];
            w.weightfn = input.chromaWeights[4*n] ? mc_weight_wtab : nullptr;
            w.i_scale = input.chromaWeights[4*n+1];
            w.i_denom = input.chromaWeights[4*n+2];
            w.i_offset = input.chromaWeights[4*n+3];
        }
        me.weight = weight;
        me.p_fref_w = input.weighted ? weighted + origin : me.p_fref[0];
        std::uint16_t integral[2*integralPlane]{};
        if (input.method >= X264_ME_ESA && input.operation == 0) {
            for (int phase = 0; phase < 4; ++phase)
                for (int n = -costRadius/4; n < costRadius/4; ++n)
                    phaseCosts[phase][costRadius+n] = motionCost[costRadius+4*n+phase];
            // Generate each native integral plane from the same raw reference pixels.
            for (int side : {8, 4}) {
                const int plane = side == 8 ? 0 : integralPlane;
                for (int y = 0; y <= stride-side; ++y)
                    for (int x = 0; x <= stride-side; ++x) {
                        int sum = 0;
                        for (int row = 0; row < side; ++row)
                            for (int col = 0; col < side; ++col)
                                sum += reference[(y+row)*stride+x+col];
                        integral[plane+y*stride+x] = static_cast<std::uint16_t>(sum);
                    }
            }
            me.integral = integral + origin;
        }
        int threshold = input.threshold;
        tracedAdsWidth = -1;
        if (input.method == X264_ME_TESA) std::fill(std::begin(scratch), std::end(scratch), -1234);
        int* thresholdPtr = input.useThreshold ? &threshold : nullptr;
        if (input.operation == 1) x264_me_refine_qpel(&encoder, &me);
        else if (input.operation == 2) x264_me_refine_qpel_refdupe(&encoder, &me, thresholdPtr);
        else x264_me_search_ref(&encoder, &me, predictors, input.predictorCount, thresholdPtr);
        if (me.cost_mv == input.resultMvCost) ++thresholdReturns;
        if (i >= wideSearchStart && (me.mv[0] != 26*4 || me.mv[1] != 0))
            throw std::runtime_error("Wide motion search fixture missed its 26-pixel displacement");
        if (output.resultX != me.mv[0] || output.resultY != me.mv[1] ||
            output.resultCost != me.cost || output.resultMvCost != me.cost_mv || output.threshold != threshold ||
            !matchesNativeBytes(output.source, source) || !matchesNativeBytes(output.reference, reference) ||
            !matchesNativeBytes(output.motionCost, motionCost) || output.chromaSource != input.chromaSource ||
            output.chromaReference != input.chromaReference)
            throw std::runtime_error("Motion search mismatch in fixture " + std::to_string(i) +
                ", operation " + std::to_string(input.operation) + ", method " + std::to_string(input.method) +
                ", subme " + std::to_string(input.subme) +
                ", GPU " + std::to_string(output.resultX) + "," + std::to_string(output.resultY) + "/" +
                std::to_string(output.resultCost) + ", x264 " + std::to_string(me.mv[0]) + "," +
                std::to_string(me.mv[1]) + "/" + std::to_string(me.cost));
        if (input.method == X264_ME_TESA && input.operation == 0 && output.tesaCandidates[2*128] != -1234)
            ++wideCandidateLists;
        const bool sameShortlist = input.method == X264_ME_TESA && tracedAdsWidth >= 0 ?
            std::equal(output.tesaCandidates.begin(), output.tesaCandidates.end(),
                       scratch + (((tracedAdsWidth+31)&~31)+4)/2) : output.tesaCandidates == input.tesaCandidates;
        if (!sameShortlist) throw std::runtime_error("TESA shortlist scratch mismatch in fixture " + std::to_string(i));
    }
    if (thresholdReturns == 0) throw std::runtime_error("Motion search fixtures missed threshold early returns");
    if (exhaustive && wideCandidateLists == 0) throw std::runtime_error("TESA fixtures missed large candidate lists");
    std::cout << searchCases+wideCases << (exhaustive ? " ESA and TESA" : " DIA, HEX and UMH") << " searches and "
              << refinementCases << " quarter-pel refinements matched x264.\n";
}

void verifyMotionBidir(const char* shader) {
    constexpr int stride = 56, planeSize = stride*stride, origin = 20*stride+20;
    constexpr int costRadius = 128, sourceSize = FENC_STRIDE*16, scratchSize = FDEC_STRIDE*16;
    constexpr int filterSize = stride-8, filterOrigin = 4*stride+4;
    struct alignas(16) Fixture {
        std::int32_t pixelSize, weight, useSatd, reserved;
        std::array<std::int32_t, 4> mv, mvp, minMv, maxMv;
        std::array<std::uint32_t, 2*(2*costRadius+1)> motionCost;
        std::array<std::uint32_t, sourceSize> source;
        std::array<std::uint32_t, 8*planeSize> reference;
        std::array<std::uint32_t, scratchSize> scratch;
    };
    static_assert(sizeof(Fixture) == 26392*sizeof(std::uint32_t));
    constexpr PixelAvg average[] = {pixel_avg_16x16, pixel_avg_16x8, pixel_avg_8x16, pixel_avg_8x8,
                                    pixel_avg_8x4, pixel_avg_4x8, pixel_avg_4x4};
    constexpr PixelCmp satd[] = {x264_pixel_satd_16x16, x264_pixel_satd_16x8, x264_pixel_satd_8x16,
                                 x264_pixel_satd_8x8, x264_pixel_satd_8x4, x264_pixel_satd_4x8, x264_pixel_satd_4x4};
    constexpr PixelCmp sad[] = {x264_pixel_sad_16x16, x264_pixel_sad_16x8, x264_pixel_sad_8x16,
                                x264_pixel_sad_8x8, x264_pixel_sad_8x4, x264_pixel_sad_4x8, x264_pixel_sad_4x4};
    constexpr int weights[] = {32, 0, 64, -16, 80, -63, 127, 33, 7};
    std::mt19937 random(0x264b1d);
    std::vector<Fixture> cases(7*64);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto& input = cases[i];
        const auto pattern = i / 7;
        input.pixelSize = static_cast<int>(i % 7);
        input.weight = weights[pattern % std::size(weights)];
        input.useSatd = pattern % 5 != 0;
        // x264 uses the same bound for both list vectors.
        input.minMv = {-44, -40, -44, -40};
        input.maxMv = {40, 42, 40, 42};
        for (int n = 0; n < 4; ++n) {
            input.mv[n] = static_cast<int>(random() % 49)-24;
            input.mvp[n] = static_cast<int>(random() % 65)-32;
        }
        if (pattern % 11 == 0) {
            const int component = static_cast<int>(i % 4);
            input.mv[component] = component % 2 ? input.minMv[component]+7 : input.maxMv[component]-7;
        }
        if (pattern == 1) input.mv = {-36, -32, 32, 34};
        if (pattern == 2) input.mv = {32, 34, -36, -32};
        for (int list = 0; list < 2; ++list)
            for (int d = -costRadius; d <= costRadius; ++d) {
                int value = 1;
                for (int magnitude = std::abs(d); magnitude; magnitude >>= 1) value += 2;
                input.motionCost[list*(2*costRadius+1)+costRadius+d] =
                    pattern % 7 == 0 ? 0 : value * (1 + list + static_cast<int>(pattern % 19));
            }
        pixel frame[2][4][planeSize]{};
        for (int list = 0; list < 2; ++list) {
            for (int p = 0; p < planeSize; ++p)
                frame[list][0][p] = static_cast<pixel>(pattern % 13 == 0 ? (list == 0 ? 0 : 255) :
                    pattern % 2 == 0 ? random() & 255 : (p%stride*2 + p/stride*3 + list*59) & 255);
            std::int16_t temp[filterSize+5];
            hpel_filter(frame[list][1] + filterOrigin, frame[list][2] + filterOrigin, frame[list][3] + filterOrigin,
                        frame[list][0] + filterOrigin, stride, filterSize, filterSize, temp);
            for (int p = 0; p < 4; ++p)
                std::copy_n(frame[list][p], planeSize, input.reference.data() + (4*list+p)*planeSize);
        }
        for (auto& sample : input.source) sample = random() & 255;
        if (pattern % 3 != 0) {
            pixel prediction[2][sourceSize]{}, combined[sourceSize]{};
            const auto size = x264_pixel_size[input.pixelSize];
            for (int list = 0; list < 2; ++list) {
                pixel* planes[4];
                for (int p = 0; p < 4; ++p) planes[p] = frame[list][p] + origin;
                const int targetX = input.mv[2*list] + (list == 0 ? -3 : 2);
                const int targetY = input.mv[2*list+1] + (list == 0 ? 2 : -3);
                mc_luma(prediction[list], FENC_STRIDE, planes, stride,
                        targetX, targetY, size.w, size.h, &unweightedReference);
            }
            average[input.pixelSize](combined, FENC_STRIDE, prediction[0], FENC_STRIDE,
                                     prediction[1], FENC_STRIDE, input.weight);
            std::copy(std::begin(combined), std::end(combined), input.source.begin());
        }
        for (auto& sample : input.scratch) sample = random() & 255;
    }
    const auto inputs = cases;
    execute(shader, cases);
    int changed = 0, returned = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = inputs[i];
        const auto& output = cases[i];
        pixel source[sourceSize], reference[8*planeSize], scratch[scratchSize];
        std::copy(input.source.begin(), input.source.end(), source);
        std::copy(input.reference.begin(), input.reference.end(), reference);
        std::copy(input.scratch.begin(), input.scratch.end(), scratch);
        std::uint16_t cost[2][2*costRadius+1];
        x264_t encoder{};
        encoder.mc.get_ref = get_ref;
        encoder.mc.memzero_aligned = [](void* address, std::size_t bytes) { std::memset(address, 0, bytes); };
        encoder.mc.avg[input.pixelSize] = average[input.pixelSize];
        encoder.pixf.mbcmp[input.pixelSize] = input.useSatd ? satd[input.pixelSize] : sad[input.pixelSize];
        encoder.mb.pic.p_fdec[0] = scratch;
        std::copy_n(input.minMv.begin(), 2, encoder.mb.mv_min_spel);
        std::copy_n(input.maxMv.begin(), 2, encoder.mb.mv_max_spel);
        x264_me_t me[2]{};
        for (int list = 0; list < 2; ++list) {
            std::copy_n(input.motionCost.begin()+list*(2*costRadius+1), 2*costRadius+1, cost[list]);
            me[list].i_pixel = input.pixelSize;
            me[list].p_cost_mv = cost[list]+costRadius;
            me[list].p_fenc[0] = source;
            me[list].i_stride[0] = stride;
            std::copy_n(input.mv.begin()+2*list, 2, me[list].mv);
            std::copy_n(input.mvp.begin()+2*list, 2, me[list].mvp);
            for (int plane = 0; plane < 4; ++plane)
                me[list].p_fref[plane] = reference + (4*list+plane)*planeSize + origin;
        }
        x264_me_refine_bidir_satd(&encoder, &me[0], &me[1], input.weight);
        const std::array<std::int32_t, 4> expected{me[0].mv[0], me[0].mv[1], me[1].mv[0], me[1].mv[1]};
        if (expected != input.mv) ++changed;
        if (std::equal(std::begin(scratch), std::end(scratch), input.scratch.begin())) ++returned;
        if (output.mv != expected || !matchesNativeBytes(output.scratch, scratch) ||
            !matchesNativeBytes(output.reference, reference) || !matchesNativeBytes(output.source, source) ||
            output.mvp != input.mvp || output.motionCost != input.motionCost ||
            output.minMv != input.minMv || output.maxMv != input.maxMv)
            throw std::runtime_error("Bidirectional refinement mismatch in fixture " + std::to_string(i));
    }
    if (!changed || !returned) throw std::runtime_error("Incomplete bidirectional refinement coverage");
    std::cout << cases.size() << " bidirectional refinements matched x264 vectors and scratch pixels.\n";
}

