// SPDX-License-Identifier: GPL-2.0-or-later
// dct_parity.cpp - byte-exact parity fixture library (master include)
//
// The x264 scalar reference and the GPU fixtures are physically split into
// parity/*.inc modules that mirror x264's source layout (common/*, encoder/*).
// They are textually included here into one translation unit because the
// extracted x264 reference uses file-static functions and namespace-scope
// constants; this preserves upstream semantics byte for byte. Each module
// documents the x264 files it mirrors. See docs/parity-layout.md.

#include "parity/x264_reference.inc"
// 1:1 x264 mirror modules (see docs/parity-layout.md); included before the
// fixtures so vk264::* shader wrappers are visible to them.
#include "../common/dct.cpp"
#include "../common/quant.cpp"
#include "../common/predict.cpp"
#include "../common/deblock.cpp"
#include "../encoder/cabac.cpp"
#include "../encoder/cavlc.cpp"
#include "../encoder/analyse.cpp"
#include "../common/mc.cpp"
#include "../common/me.cpp"
#include "../common/mvpred.cpp"
#include "../common/pixel.cpp"
#include "../common/bitstream.cpp"
#include "../common/set.cpp"
#include "../encoder/macroblock.cpp"
#include "../encoder/encoder.cpp"
#include "../encoder/ratecontrol.cpp"
#include "parity/fixtures_transform.inc"
#include "parity/fixtures_intra.inc"
#include "parity/fixtures_deblock.inc"
#include "parity/fixtures_cabac.inc"
#include "parity/fixtures_cavlc.inc"
#include "parity/fixtures_motion.inc"
#include "parity/fixtures_syntax.inc"
#include "parity/fixtures_macroblock.inc"
#include "parity/resident_cavlc.inc"
#include "parity/resident_cabac.inc"
#include "parity/resident_pipeline.inc"
#include "parity/fixtures_aq.inc"
#include "parity/dispatch.inc"
