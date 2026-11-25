# CPU function → shader mapping (the project's main goal)

Phoenix's CPU oracle is the specification for the eventual **Vk264 shader
backend**. This document classifies x264's work by how it maps to GPU shaders,
so the CPU port can preserve the right interfaces from day one.

## Class A — naturally data-parallel (direct shader mapping)

These operate independently per pixel/block/MB. One invocation per work item,
no cross-item state. They should be written as pure functions in Phoenix.

| x264 function family | source | shader mapping |
|---|---|---|
| DCT 4x4 / 8x8, Hadamard / IDCT | `common/dct.c` | one invocation per block; registers for 4x4, shared memory for 8x8 |
| SAD / SATD / SA8D | `common/pixel.c` | one invocation per candidate MV/mode, reduction over 16/64 lanes |
| Predictors (4x4/8x8/16x16/chroma, filtered edges) | `common/predict.c` | one invocation per block; neighbour samples via image loads |
| Quant / dequant / deadzone | `common/quant.c` | per coefficient, vectorizable |
| Trellis QNS inner candidate score | `encoder/rdo.c` | per coefficient candidate; keep the state machine in a later pass |
| Lowres plane construction | `common/mc.c:458` | per 2x2 source block |
| MB-tree cost propagation | `encoder/slicetype.c:1091` | per MB with neighbour reduction passes |
| Intra/inter mode cost computation | `encoder/analyse.c`, `me.c` | per MB, independent once neighbour caches are loaded |

**Phoenix requirement:** functions in this class must be pure — inputs passed
explicitly, outputs written explicitly, no hidden state. That makes the shader
translation mechanical.

## Class B — ordered but local (workgroup-level shaders)

These need neighbour information within a frame/MB but not across frames. Map
to a workgroup with shared memory or a two-pass reduce.

| function family | source | ordering need |
|---|---|---|
| Neighbour cache load/save | `common/macroblock.c:855/1680` | left/top MBs must be complete |
| Intra mode decision (I4/I8/I16/chroma) | `analyse.c` | uses the neighbour caches |
| Motion estimation candidates | `analyse.c`, `me.c` | uses neighbour MVs + cached lowres MVs |
| Transform-size RD | `analyse.c:2737` | compares two reconstructions of the same MB |
| CABAC trellis state | `rdo.c:1142` | per-block state array, but blocks ordered by scan |

## Class C — strictly serial (keep on CPU or one thread)

These cannot be naively parallelized because they mutate a single evolving
state. The shader backend must run them in a dedicated serial stage.

| function | source | why serial |
|---|---|---|
| `x264_cabac_encode_decision*` | `common/cabac.c` | arithmetic coder `i_low/i_range` + context state |
| `x264_macroblock_write_cabac/cavlc` | `encoder/cabac.c`, `cavlc.c` | emits one bitstream in scan order |
| `x264_slicetype_analyse` | `encoder/slicetype.c:1473` | walks `lookahead->last_nonb` sequentially |
| `scenecut` / flash detection | `slicetype.c:1430` | per-frame `b_scenecut` flags accumulate |
| Rate-control accumulators | `ratecontrol.c` | QP depends on previous frames |
| NAL assembly / emulation prevention | `common/bitstream.c` | single output stream |

This class is exactly where the current oracle's heuristics live (E96/E98/E99
and the 18→6 NAL-count work). **Porting them faithfully matters more here than
anywhere else**: a shader backend cannot guess them.

## Recommended shader pipeline

```
[CPU serial]   slicetype/lookahead -> frame types, QP plan
[GPU A]        lowres planes, intra/inter costs, ME candidates
[GPU A/B]      mode decisions, quant, reconstruct (per MB)
[CPU serial]   CABAC/CAVLC encode + NAL assembly
```

The boundary between A/B and C is an explicit array of per-MB decisions
(mode, MV, CBP, quantized coefficients, nnz flags). Phoenix must expose that
array as a named structure (`phx_mb_decision_t`) so the shader port can write
it in one stage and the serial entropy stage can read it in another.

## Documentation requirement per function

Every function comment in Phoenix should end with a line:

```
SHADER: A | B | C   <one-sentence mapping note>
```

That is the deliverable the shader work will consume.
