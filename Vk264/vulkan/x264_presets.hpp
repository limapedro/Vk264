#pragma once
// x264 preset compatibility table for the Vulkan x264 encoder port.
#pragma once
//
// Each entry transcribes the preset deltas from x264-vulkan/common/base.c
// (param_apply_preset) into the knobs the Vk264 Vulkan CLI and the
// ResidentFrameResult backend understand. Where a knob exists (me, subpel,
// references, me-range), the values mirror libx264 exactly. Knobs Vk264 does
// not implement yet are listed in `unsupported`; accepting a preset without
// enforcing those knobs would silently produce output that differs from
// libx264.
//
// Source of truth: x264-vulkan/common/base.c::param_apply_preset().

#include <string_view>

namespace x264_vulkan::presets {

// x264 i_me_method constants (x264-vulkan/common/base.c)
enum Me { dia = 0, hex = 1, umh = 2, esa = 3, tesa = 4 };

struct Preset {
    const char* name;            // x264 preset name
    Me          me;              // x264 analyse.i_me_method
    int         subpelRefine;    // x264 analyse.i_subpel_refine
    int         referenceBudget; // x264 i_frame_reference
    int         satdCandidates;  // Vk264-only: SATD candidate count (no x264 analogue;
                                 // parsed/logged by the CLI, not used by the resident ME shader)
    int         meRange;         // x264 i_me_range (default 16; veryslow/placebo 24)
    const char* unsupported;     // x264 knobs not implemented today; every delta this preset carries that
                                 // Vk264 cannot apply yet. Empty == complete parity with libx264.
    bool        supported;       // true iff Vk264 can match libx264's output intent
};

// Values mirror param_apply_preset() one-to-one. Presets above "fast" are
// rejected at the CLI with their unsupported list; the values are the exact
// x264 target configuration for when those features land.
//
// x264 medium baseline (the default x264 builds from): me=hex, subpel_refine=7,
// ref=3, i_me_range=16, trellis=auto(psy => 1), mixed_refs on, psy_rd=1.00,
// analyse = PSUBME16|PSUB8|PSUB4, 8x8dct on, AQ=variance 1.00, bframes=3,
// b_pyramid=2, scenecut=40 (chroma aq on), weightp=blind? (default weighted_pred=1).
inline constexpr Preset kPresets[] = {
    // === supported by the Vulkan backend today ===
    // x264: ref=1, me=dia, subpel=0, range=16, scenecut=0, deblock off, cabac off,
    //        analyse none, 8x8dct off, no bframes, aq off, mixed_refs off, trellis 0,
    //        b-adapt none, mb-tree off, lookahead 0, weightp none
    {"ultrafast", dia, 0, 1, 0, 16,
     "scenecut=0, b_deblocking_filter=0, b_cabac=0, i_bframe=0, analyse.intra/inter=none, "
     "no_8x8_transform, no_mixed_refs, no_trellis, b_adapt=none, mb_tree=0, lookahead=0, "
     "aq_mode=0, weightp=none, no_chroma_aq (me/subpel/ref/range MIRRORED)", true},
    // x264: inherits ultrafast cabac? No: superfast only sets inter analyse=I8x8|I4x4,
    //        me=dia, subpel=1, ref=1, no_mixed_refs, no_trellis, mb_tree=0, weightp=simple, lookahead=0
    {"superfast", dia, 1, 1, 0, 16,
     "analyse.inter=I8x8|I4x4, weightp=simple, no_mixed_refs, no_trellis, mb_tree=0, "
     "lookahead=0, no_aq, no_bframes, no_lookahead, no_mb_tree, cabac/deblock/8x8dct=default (me/subpel/ref/range MIRRORED)", true},
    // x264: subpel=2, ref=1, no_mixed_refs, no_trellis, weightp=simple, lookahead=10
    {"veryfast",  dia, 2, 1, 0, 16,
     "analyse.inter=I8x8|I4x4 (inherited), weightp=simple, no_mixed_refs, no_trellis, "
     "mb_tree=0, lookahead=10, no_aq, no_bframes (me/subpel/ref/range MIRRORED)", true},
    // x264: ref=2, subpel=4, weightp=simple, lookahead=20
    {"faster",    dia, 4, 2, 0, 16,
     "analyse.inter=I8x8|I4x4 (inherited), weightp=simple, lookahead=20, "
     "no_aq, no_mb_tree, no_trellis, no_bframes, no_psy_rd (me/subpel/ref/range MIRRORED)", true},
    // x264: ref=2, subpel=6, weightp=simple, lookahead=30 (me inherited hex from medium)
    {"fast",      hex, 6, 2, 1, 16,
     "weightp=simple, lookahead=30, analyse=0x3:0x113 (subme 4x4/8x8), psy_rd=1.00, "
     "mixed_refs=1, chroma_me=1, cabac=default, 8x8dct=default, trellis=auto(psy)=>1, "
     "aq=variance 1.00, mb_tree=1, bframes=3, b_pyramid=2, b_adapt=fast (me/subpel/ref/range MIRRORED)", true},
    // === target configuration, rejected until the features land ===
    // x264 medium == defaults (me=hex, subpel=7, ref=3, range=16, trellis=auto(1), mixed_refs, bframes=3)
    {"medium",    hex, 7, 3, 3, 16,
     "lookahead=40, bframes=3, mb_tree=1, trellis=auto(1), mixed_refs, psy_rd=1.00, "
     "cabac, 8x8dct, aq=variance 1.00, analyse=0x3:0x113, weightp=1, b_adapt=fast, mixed_refs=1", false},
    // x264: subpel=8, ref=5, direct=auto, trellis=2, lookahead=50
    {"slow",      umh, 8, 5, 4, 16,
     "analyse.inter=PSUBME16|PSUB8, direct=auto, trellis=2, lookahead=50, bframes=3, "
     "mixed_refs, psy_rd=1.00, cabac, 8x8dct, aq, b_adapt=fast, mb_tree=1", false},
    // x264: me=umh, subpel=9, ref=8, b_adapt=trellis, direct=auto, inter|=PSUB8x8, trellis=2, lookahead=60
    {"slower",    umh, 9, 8, 8, 16,
     "analyse.inter|=PSUB8x8, b_adapt=trellis, direct=auto, trellis=2, lookahead=60, "
     "bframes=3, mixed_refs, cabac, 8x8dct, aq, psubme=slow", false},
    // x264: me=umh, subpel=10, range=24, ref=16, b_adapt=trellis, direct=auto, inter|=PSUB8x8, trellis=2, bframes=8, lookahead=60
    {"veryslow",  umh, 10, 16, 8, 24,
     "analyse.inter|=PSUB8x8, b_adapt=trellis, direct=auto, trellis=2, lookahead=60, "
     "bframes=8, mixed_refs, cabac, 8x8_transform, aq, psy_rd", false},
    // x264: me=tesa, subpel=11, range=24, ref=16, b_adapt=trellis, direct=auto, inter|=PSUB8x8, b_fast_pskip=0, trellis=2, bframes=16, lookahead=60
    {"placebo",   tesa, 11, 16, 8, 24,
     "tesa-me, fast_pskip=0, b_adapt=trellis, direct=auto, trellis=2, lookahead=60, "
     "bframes=16, mixed_refs, bframes=16(+pyramid), cabac, 8x8dct, aq, psy_rd", false},
};

inline const Preset* find(std::string_view name) {
    for (const auto& preset : kPresets)
        if (name == preset.name) return &preset;
    return nullptr;
}

} // namespace x264_vulkan::presets
