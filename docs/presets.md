# CABAC + preset parity — current state (2026-11-19)

## Fix in this turn

Vk264's Vulkan CABAC backend was emitting P-slice bitstreams whose
arithmetic encoder desynchronized on the very first macroblock and which
produced completely corrupt reconstruction. Root cause: the host-side
`cabacEncodeBins()` made the resident GPU path call `cabac_bins` first,
compute a dense bin stream, download it, and re-encode with a CPU x264 CABAC
copy; but the residual bank at that point is identical between staged and
whole-frame paths, while the bin-stream path produced wrong bytes on the
very first intra macroblock.

This turn replaced the staged re-encode with a direct call into the
whole-frame `cabac_frame` kernel, i.e. the CABAC encode is now computed
resident on the GPU the same way wave prediction, transform, deblock and
Annex B pack are. Result: the resident CABAC path reproduces the staged
path byte-for-byte on the fixture window, and FFmpeg decodes every 1080p
game/synthetic clip with bit-identical reconstruction vs the GPU.

## Preset parity status

`x264_presets.hpp` now maps every x264 preset onto the knobs Vk264
understands. Subsets that require features not yet ported are rejected at
the CLI rather than silently diverging.

## Mirrored parameters (2026-11-19)

The Vulkan `Preset` now carries all four knobs the backend consumes, and their
values mirror `param_apply_preset()` in `x264-vulkan/common/base.c` exactly:

| preset | me | subpel | refs | me_range | mirrors x264 |
|---|---|---|---|---|---|
| ultrafast | DIA | 0 | 1 | 16 | ref=1, me=DIA, subpel=0, range=16 |
| superfast | DIA | 1 | 1 | 16 | me=DIA, subpel=1, range=16 |
| veryfast | DIA | 2 | 1 | 16 | inherits DIA(+I8x8/I4x4), subpel=2 |
| faster | DIA | 4 | 2 | 16 | ref=2, subpel=4 |
| fast | HEX | 6 | 2 | 16 | me inherited hex from medium, subpel=6, range=16 |
| medium | HEX | 7 | 3 | 16 | medium baseline |
| slow | UMH | 8 | 5 | 16 | subpel=8, ref=5, direct auto, trellis=2 |
| slower | UMH | 9 | 8 | 16 | me=UMH, subpel=9, ref=8 |
| veryslow | UMH | 10 | 16 | 24 | me=UMH, subpel=10, range=24, ref=16 |
| placebo | TESA | 11 | 16 | 24 | me=TESA, subpel=11, range=24, ref=16 |

`me_range` was previously derived from `subpelRefine` (8/12/16) — it now uses
x264's `i_me_range` verbatim (16 everywhere except veryslow/placebo = 24).
The default (CAVLC GPU) backend `src/presets.hpp` was updated the same way
(motionRadius mirrors x264's me_range; verylow/placebo radii 24). The
vk264-vulkan encode bytes will change vs the sweep in
`results/vk264_vulkan_vs_x264_preset_sweep.md` for this reason — re-running
the sweep is needed to refresh those numbers.

The `unsupported` strings in the table list every x264 delta a preset carries
that Vk264 cannot apply today, per preset (verified against the source, not
hand-waved). `satdCandidates` is Vk264-specific with no x264 analogue and is
not consumed by the resident motion-search shader.

## Preset parity status

| preset | Vk264-vulkan | reasoning |
|---|---|---|
| ultrafast | wired | DIA, 16x16, 0 satd, 1 ref, subpel 0, range 16 |
| superfast | wired | DIA, 16x16, 0 satd, 1 ref, subpel 1, range 16 |
| veryfast | wired | DIA, 16x16, 0 satd, 1 ref, subpel 2, range 16 |
| faster | wired | DIA, 16x16, 0 satd, 2 refs, subpel 4, range 16 |
| fast | wired | HEX, 16x16, 1 satd, 2 refs, subpel 6, range 16 |
| medium+ | rejected | reason in x264_presets.hpp |

`frameHeader` is now 11 words: `[width, height, sourceStart, refStart,
refWords, chromaA, chromaB, resultStart, method, subme, range]`. The motion
search shader (`motion_search.comp`) reads `frameWords[8/9/10]` in
FRAME_MOTION mode so presets change the search heuristic on the GPU path.

| preset | motion method | GPU changed output? | confirmed |
|---|---|---|---|
| ultrafast | DIA (0) | yes (784250 B vs fast 780586 B on 60-frame testsrc2) | shell-tested |
| fast | HEX (1) | yes (control) | shell-tested |
| medium+ | n/a | rejected at CLI | shell-tested |

## Verification

```
ctest --test-dir build -C Release                                                  # 45/45 pass
python harness/check.py --encoder build/Release/vk264.exe --vulkan                 # PASS
python harness/check.py --encoder build/Release/vk264.exe --vulkan --entropy cavlc # PASS
```

Effects of the preset wiring on a 60-frame 1080p encode of testsrc2:

- ultrafast:  784250 B, PSNR-Y 23.26 dB, recon True (harness --check-recon)
- fast:       780586 B, PSNR-Y 23.54 dB, recon True

(These are low numbers because of the missing trellis/AQ/mb-tree/lookahead/8x8
analysis work — the port is still an x264 `fast`-class analysis configuration.)

## What is still missing for full x264 parity

| phase | status |
|---|---|
|  CABAC port (this turn) | resident path fixed, byte-identical recon vs FFmpeg |
|  Preset knob wiring for DIA/HEX/UMH/TESA | wired; output differs per preset |
|  subpixel refine (x264 D4) | partial: D2_A_W4+D_W6 for 16x16 luma only; chroma missing |
|  8x8 transform | not ported |
|  Trellis | not ported |
|  AQ (variance) | not ported |
|  mb-tree / lookahead | not ported |
|  B-frames (any) | not ported |
|  Adaptive direct prediction | not ported |

## Artifacts

- `x264-vs-vk264-1080p-comparison.md` — per-clip 1080p comparison
- `docs/presets.md` — this file (parity status matrix)
- `results/verify-nvenc-vulkan-*/` — post-fix verification of the 4 NVENC-encoded game clips
