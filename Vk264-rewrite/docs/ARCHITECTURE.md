# Vk264-rewrite architecture — GPU-first, C only as host + oracle

## The goal
A byte-exact x264 reimplementation where **all per-pixel / per-macroblock
computation runs in Vulkan compute shaders**. The C host does only the work a
CPU is good at: Vulkan setup, buffer management, shader dispatch, file I/O,
CLI parsing, and NAL start-code/emulation-prevention encapsulation.

This is a correction to the early Milestone 0-2 code layout. The C functions
already written (`bitstream.c`, `nal.c`, `cavlc.c`, `slice.c`) are the
**reference port**, not the shipping encoder path.

## Three layers

```
  tests/  ── byte-compare ──>  x264 CLI reference bytes
     │
     ├── C oracle  (src/ref/)     ← x264 C functions ported 1:1, byte-exact
     │        │
     │        └── equivalence test: shader output == oracle output
     │                 │
     │                 v
     └── GPU encode (shaders/*.comp)  ← actual producer of frame data
                  │
                  v
        src/host/ (thin C) → Vulkan dispatch, NAL packets, file I/O
```

### 1. `src/ref/` — the oracle (test-only)
A direct C transcription of x264's C functions. It exists for three reasons:
- it makes byte-exactness checkable without a GPU;
- each GPU shader gets a 1:1 CPU spec to compare against;
- any mismatch localizes to one shader instead of a whole pipeline.

`bitstream.c`, `nal.c`, `cavlc.c`, `slice.c` move here. They are linked into the
test binaries only. **No shipped encode path calls them.**

### 2. `shaders/*.comp` — the actual encoder
Every compute-heavy step becomes a shader:
- intra prediction (16x16 / 8x8c / 4x4), SAD/SATD/SA8D metrics
- 4x4 DCT + Hadamard DC, quantisation, zigzag, dequant/idct
- motion search (full-pel + subpel), motion compensation
- deblocking (strength + filter)
- CAVLC/CABAC syntax element emission per MB

Each shader is proven against the oracle with a GPU-output-vs-C-output test
*before* being wired into the frame pipeline.

### 3. `src/host/` — thin C driver
- Vulkan instance/device/queues, descriptor sets, pipelines, command buffers
- staging buffers, readback, timing queries
- Annex-B packet assembly (start codes, NAL headers, `0x03` escaping)
- CLI and file I/O

## Entropy coding on GPU
CAVLC/CABAC is the serial-looking tail, but it parallelizes the same way x264
clusters bits: each workgroup encodes one MB into its own bit chunk, then a
second pass concatenates chunks in MB order and applies RBSP trailing. CABAC
needs per-MB context state; the plan is wavefront/slice-parallel contexts with
a serial merge only where the standard requires it. This is the part most
likely to need iteration; everything before it is embarrassingly parallel.

## Why this is the fast path to correctness
x264 is the oracle: for any input we can run
`x264.exe --preset <p> --qp <q>` and get the exact expected bytes. A shader
that differs by one coefficient or one VLC token is caught by a single
byte-diff. Once every shader matches, the GPU encoder is automatically
byte-exact — no heuristic "close enough" PSNR debugging.

## Performance target
Prediction, transforms, quantisation, SAD/SATD search and deblocking are all
data-parallel and should saturate the GTX 1660 SUPER. The CPU port is never on
the critical path. Once shaders match x264 medium/slow byte-for-byte, the same
bitstream at real-time speed is the intended result.

## Current state (milestones)
- M0 done: C11/Vulkan build, bitstream writer, 1/1 CTest green.
- M1 done: SPS/PPS byte-exact vs x264 CLI, 2/2 CTest green.
- M2 in progress: first IDR slice; header + mb_type already byte-match.
- M3 next: move ref code to `src/ref/`, add first equivalence-tested shader
  (intra DC prediction + 4x4 DCT/quant), then wire it into the slice.
