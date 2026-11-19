// SPDX-License-Identifier: GPL-2.0-or-later
// x264 H.264 level table and selection algorithm, ported from
// x264-master/common/tables.c (x264_levels) and encoder/set.c
// (x264_validate_levels). Used to emit i_level_idc and the matching
// bitstream_restriction/mv_length VUI fields, exactly as libx264 chooses
// for a given configuration.
#pragma once

#include <cstdint>
#include <cmath>

namespace x264_levels {

struct X264Level {
    int level_idc;
    int mbps;        // max macroblock processing rate (macroblocks/sec)
    int frame_size;  // max frame size (macroblocks)
    int dpb;         // max decoded picture buffer (mbs)
    int bitrate;     // max bitrate (kbit/s)
    int cpb;         // max vbv buffer (kbit)
    int mv_range;    // max vertical mv component range (pixels)
    int mvs_per_2mb;
    int slice_rate;
    int mincr;
    int bipred8x8;
    int direct8x8;
    int frame_only;
};

// Table copied verbatim from x264/common/tables.c x264_levels
constexpr X264Level kLevels[] = {
    { 10,     1485,     99,    396,     64,    175,  64, 64,  0, 2, 0, 0, 1 },
    {  9,     1485,     99,    396,    128,    350,  64, 64,  0, 2, 0, 0, 1 }, // "1b"
    { 11,     3000,    396,    900,    192,    500, 128, 64,  0, 2, 0, 0, 1 },
    { 12,     6000,    396,   2376,    384,   1000, 128, 64,  0, 2, 0, 0, 1 },
    { 13,    11880,    396,   2376,    768,   2000, 128, 64,  0, 2, 0, 0, 1 },
    { 20,    11880,    396,   2376,   2000,   2000, 128, 64,  0, 2, 0, 0, 1 },
    { 21,    19800,    792,   4752,   4000,   4000, 256, 64,  0, 2, 0, 0, 0 },
    { 22,    20250,   1620,   8100,   4000,   4000, 256, 64,  0, 2, 0, 0, 0 },
    { 30,    40500,   1620,   8100,  10000,  10000, 256, 32, 22, 2, 0, 1, 0 },
    { 31,   108000,   3600,  18000,  14000,  14000, 512, 16, 60, 4, 1, 1, 0 },
    { 32,   216000,   5120,  20480,  20000,  20000, 512, 16, 60, 4, 1, 1, 0 },
    { 40,   245760,   8192,  32768,  20000,  25000, 512, 16, 60, 4, 1, 1, 0 },
    { 41,   245760,   8192,  32768,  50000,  62500, 512, 16, 24, 2, 1, 1, 0 },
    { 42,   522240,   8704,  34816,  50000,  62500, 512, 16, 24, 2, 1, 1, 1 },
    { 50,   589824,  22080, 110400, 135000, 135000, 512, 16, 24, 2, 1, 1, 1 },
    { 51,   983040,  36864, 184320, 240000, 240000, 512, 16, 24, 2, 1, 1, 1 },
    { 52,  2073600,  36864, 184320, 240000, 240000, 512, 16, 24, 2, 1, 1, 1 },
    { 60,  4177920, 139264, 696320, 240000, 240000, 8192, 16, 24, 2, 1, 1, 1 },
    { 61,  8355840, 139264, 696320, 480000, 480000, 8192, 16, 24, 2, 1, 1, 1 },
    { 62, 16711680, 139264, 696320, 800000, 800000, 8192, 16, 24, 2, 1, 1, 1 },
    { 0 },
};

struct LevelInput {
    int mbsPerFrame = 0;
    int widthInMbs = 0, heightInMbs = 0;
    int maxDecFrameBuffering = 1;  // x264: X264_MIN(X264_REF_MAX, refs + reorder + pyramid)
    int fpsNum = 60000, fpsDen = 1001;
    int vbvKbps = -1;              // -1 = disabled (match x264 when no VBV RC is active)
    int cpbKbps = -1;              // -1 = disabled
    int mvRange = 0;               // x264 param after default assignment; used only via mv_range from the chosen level
};

// Mirrors x264_validate_levels() + the outer loop that picks the smallest
// passing level, without the VBV checks when VBV is inactive (matching the
// x264 behaviour for raw-CQP sessions).
inline int selectLevel(const LevelInput& in) {
    int dpb = in.mbsPerFrame * in.maxDecFrameBuffering;
    for (const auto& l : kLevels) {
        if (l.level_idc == 0) return 62; // unreachable; table has 62
        if (l.frame_size < in.mbsPerFrame) continue;
        if (l.frame_size * 8 < in.widthInMbs * in.widthInMbs) continue;
        if (l.frame_size * 8 < in.heightInMbs * in.heightInMbs) continue;
        if (dpb > l.dpb) continue;
        const int cbpFactor = 4; // BYTE: 8-bit 4:2:0 non-422 -> x264 uses 4
        if (in.vbvKbps > 0 && l.cpb > 0) {
            if (l.bitrate * cbpFactor / 4 < in.vbvKbps) continue;
            if (l.cpb * cbpFactor / 4 < in.cpbKbps) continue;
        }
        if ((int64_t)l.mbps < (int64_t)in.mbsPerFrame * in.fpsNum / in.fpsDen) continue;
        return l.level_idc;
    }
    return 62;
}

// Find the level descriptor for a given level_idc (non-const view into the table).
inline const X264Level* findLevel(int levelIdc) {
    for (const auto& l : kLevels)
        if (l.level_idc == levelIdc) return &l;
    return nullptr;
}

// x264: log2_max_mv_length_* = (int)log2f(X264_MAX(1, mv_range*4-1)) + 1
inline int mvLengthLog2(int mvRange) {
    return (int)log2f((double)(mvRange * 4 - 1)) + 1;
}

} // namespace x264_levels
