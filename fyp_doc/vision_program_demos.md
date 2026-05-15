# Vision-Program Demo Suite — design notes

**Audience:** future programmers / agents writing or extending image
and ML kernels for the VisionSoC 2D RVV fabric. This document covers
the four demo programs in `tests/vision_program/` and explains which
architectural lever each one exercises.

Required prior reading:

  * `fyp_doc/2d_fabric_handoff.md` — the T1 programming model. How the
    128 hardware rows behave, what CSR `0x7c0` does, which RVV
    intuitions break, programmer rules R1–R10.
  * `fyp_doc/LSU_vertical_mode_handoff.md` — vert-LSU semantics; the
    four canonical patterns (V-load+H-store, H-load+V-store, V-load+
    V-store no-op, CSR-flip during drain).
  * `tests/vision_task/benchmark_vadd/benchmark_vadd.c` — canonical
    R1–R10 reference and the closest analogues to these demos.

## Why a separate `vision_program/` directory

`tests/vision_task/` holds the *primitive*-level RVV regressions:
single-instruction probes (`simple_instruction_gather*`, `vert_lsu`,
`vert_hori`) and the rules-of-engagement reference
(`benchmark_vadd.c`). Each test isolates one architectural quirk.

`tests/vision_program/` is a *programs*-level directory: each test is
a recognisable image-processing or ML primitive — Sobel, Gaussian,
fully-connected layer, matrix multiply — that combines several
fabric features into one end-to-end demo. The split lets the
primitive-level tests stay laser-focused (and run fast in CI) while
the programs-level tests double as showcases and experiments for
future kernel ideas.

## Architectural levers per demo

| Demo | H+V mode flip | Per-row vredsum | Vert-LSU transpose | Slides | i8 saturation | vrgather LUT | Vector argmax |
|------|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| `sobel_edge`       | ✓ | – | – | ✓ (3-tap H+V)     | ✓ (`vsadd.vv`) | – | – |
| `gaussian_blur`    | ✓ | – | – | ✓ (3-tap H+V × 2) | – (uses `vsra`) | – | – |
| `matvec_fc_relu`   | – | ✓ | – | – | – | – | – |
| `matmul_via_vt`    | – | ✓ | ✓ | – | – | – | – |
| `cnn_digit`        | ✓ | ✓ | – | ✓ (Sobel-like)    | ✓ (`vsadd.vv`) | ✓ | ✓ (SEW=32 vredmax) |

## Demo 1 — `sobel_edge`

**Algorithm.** 3×3 separable Sobel. Horizontal gradient
`Gx = right − left`; vertical gradient `Gy = below − above`; output
`|Gx| + |Gy|` with int8 saturation.

**Why this fabric.** Gx is a horizontal-mode 3-tap slide. Gy is the
*same* arithmetic on the *same* loaded register, but with CSR
`0x7c0 = 1`, so the slide moves between hardware rows (= image rows).
The 128-row parallelism makes the V-pass identical in cost to the
H-pass — on stock RVV the V-pass would require an explicit transpose
or per-row scalar work. The kernel is a single LSU round-trip: load
once, do H+V compute in registers, store once.

**Register layout (LMUL=4 groups):**

| Group | Role |
|-------|------|
| `v8`  | image (input) |
| `v12` | slide aux: right neighbour (H), then below (V) |
| `v16` | slide aux: left neighbour (H), then above (V); also negation buffer |
| `v20` | Gx, then `\|Gx\|` after vrsub+vmax |
| `v24` | Gy, then `\|Gy\|` |

**`|x|` without `vabs`:** RVV base does not have `vabs.v`. We compute
`|x| = max(x, -x)` via `vrsub.vi vd, vs, 0` (a-x with x=0 = -vs) and
`vmax.vv`. Note that for `x = -128`, `-x = -128` (no positive int8 for
+128), so `|x| = -128`; the verifier reproduces this precisely.

**Test image.** A diagonal step plus a vertical bar (`grid_in[r][c]`
fixed by region). Output should show:

  * Strong vertical lines at `c = 60` and `c = 70` (Gx fires there).
  * A diagonal edge at `r = c` (both Gx and Gy fire on the slope).

**Run command.**

```sh
./run-test.sh vision_program.sobel_edge \
  -c mudkip2d128small1bram1chain2lanescale --max-cycles 50000000
```

## Demo 2 — `gaussian_blur`

**Algorithm.** Separable 3×3 Gaussian.
`h_blur = (left + 2·centre + right) >> 2` along H, then
`out = (above + 2·centre + below) >> 2` along V on `h_blur`. Net 2D
filter is the outer-product `[[1 2 1],[2 4 2],[1 2 1]] / 16`.

**Why this fabric.** The cleanest mid-kernel CSR flip. Two compute
passes back-to-back inside the VRF — no intermediate memory store —
with one mode flip in the middle. Total: one `vle8`, two passes of
`vmv+slide+slide+add+add+add+sra`, one `vse8`. Five LMUL=4 register
groups suffice; `v8` is reused as a slide buffer in the V pass after
the H pass result has been written to `v20`.

**Register layout (LMUL=4 groups):**

| Group | H pass | V pass |
|-------|--------|--------|
| `v8`  | image (input) | reused as below-row buffer |
| `v12` | right neighbour | reused as above-row buffer |
| `v16` | left neighbour | final output |
| `v20` | h-pass result   | (read-only input to V pass) |

**Subtle point — sign of the shift.** `vsra.vi v_, v_, 2` is an
*arithmetic* shift right; for negative i8 inputs it sign-extends, so
`-1 >> 2 = -1` (rounds toward `−∞`). The scalar verifier uses C's
`>> 2` on `int`, which on signed types is implementation-defined but
in practice is also arithmetic on every modern compiler — so the
pass-through is exact.

## Demo 3 — `matvec_fc_relu`

**Algorithm.** Single fully-connected NN layer:

```
y[r] = ReLU( sum_{c=0..127} A[r][c] * x[c]  +  b[r] )
```

128-element input `x`, 128×128 weight matrix `A`, 128-element bias
`b`, 128-element output `y`. All `int8_t`.

**Why this fabric.** This is the per-row `vredsum.vs` showcase. Each
hardware row independently computes a 128-wide dot-product reduction
into element 0 of its destination register. Bias add and ReLU are
trivial elementwise ops applied afterwards (only element 0 of each
hw-row matters). A `vl=1, e8, m1` store places `y[r]` at `&grid[r][0]`
using the LSU's fixed 128-element row pitch.

This is structurally TEST 9 of `benchmark_vadd.c` extended with bias
and activation — i.e. a recognisable neural-network primitive, not
just a synthetic matvec.

**Memory layout (single source of truth).** The `x_replicated` and
`b_replicated` arrays are laid out so a single `vle8` puts the right
data into every hardware row:

```
A[r][c]              standard row-major weight matrix
x_replicated[r][c] = x[c]               for all r
b_replicated[r][c] = b[r]               for all c
```

After `vle8.v v24, b_replicated`, hw-row r has `b[r]` in *every*
element of `v24` — including element 0, which is the only one we need
after the bias add (because the final store has `vl=1`).

**Register layout (LMUL=4 groups):**

| Group | Role |
|-------|------|
| `v8`  | A row r |
| `v12` | x replicated |
| `v16` | A * x_replicated (per-row product) |
| `v20` | vredsum result -> bias-added -> ReLU'd |
| `v24` | b replicated |
| `v28` | zero constant for `vmax` ReLU |

6 of 8 LMUL=4 groups used.

**Verification gotcha.** The kernel uses `i8 × i8 -> i8` multiply,
which can saturate / wrap. The scalar verify reproduces *exactly* the
same i8-wrap arithmetic; otherwise the comparison would mis-fail when
intermediate sums overflow.

## Demo 4 — `matmul_via_vt`

**Algorithm.** 128×128 matrix multiply `C = A · B`. The fabric does
not have a single-instruction matmul primitive; the demo composes two
existing ones:

  1. **Vert-LSU transpose** of `B` to `BT` (one kernel, one LSU
     round-trip):

     ```
     csrw 0x7c0, 1; vle8.v v8,(B); csrw 0x7c0, 0; vse8.v v8,(BT)
     ```

     This is the canonical pattern from `simple_instruction_vert_lsu.c`
     TEST 1. After it runs, `BT[j][k] = B[k][j]`, so `BT[j][:]` is the
     `j`-th column of `B`.

  2. **Per-column matvec.** For `j = 0..127`: build
     `x_replicated[r][:] = BT[j][:]` (scalar broadcast), then run a
     stripped-down matvec kernel that stores to `&C[0][j]` with
     `vl=1` so each hardware row writes its dot product to `C[r][j]`.

The product is `C[r][j] = sum_k A[r][k] * BT[j][k] = sum_k A[r][k] * B[k][j]`,
which is exactly what `(A · B)[r][j]` means.

**Why two primitives, not a single fused matmul kernel?**

  * **Register pressure.** A naïve in-register matmul would need at
    least 4 LMUL=4 groups for accumulators; with the transpose buffer
    plus matvec scratch, we'd run out of disjoint groups.
  * **Clarity.** Each kernel is a direct re-use of an already-proven
    primitive (`simple_instruction_vert_lsu` transpose; `benchmark_vadd`
    TEST 9 matvec). Bug surface stays small.
  * **The replicate is unavoidable.** The fabric loads memory rows
    into hardware rows; broadcasting one memory row to every hw-row
    requires a scalar copy step. We do this once per output column
    (`j`), which is acceptable for a 128×128 demo.

**Performance counter.** Wraps the *inner* matvec (per-j), so each of
the 128 STARTed events bracketed by tag `4` shows the per-output-column
cycle count. Total runtime is dominated by the 128× scalar replicate
loops; the vector matvec itself is short.

**Verification.** Scalar-C triple-loop matmul with int8 wrap matched.
The transpose passes its own `[CHECK] PASS transpose` before the
matmul phase runs.

## Demo 5 — `cnn_digit`

**Algorithm.** End-to-end int8 CNN inference for digit classification.
Three pipeline stages, all heavy work on the vector unit:

```
input (128x128 i8 digit)
   │
   ▼   Stage 1: k_conv_relu_lut
   │     - 3x3 separable Sobel-like edge filter (H Gx + V Gy via mode flip)
   │     - |Gx|+|Gy| with i8 saturation, ReLU clamp
   │     - vrgather.vv against a 128-byte LUT -> binary edge map
   ▼
edges (128x128 i8 binary)
   │
   ▼   Stage 2: k_score_class  (called 10 times, c = 0..9)
   │     - vmul.vv (edges, W_c)
   │     - per-row vredsum.vs into v_dst[0]
   │     - vl=1 store of 128 i8 row-sums
   │     - scalar tail: sum -> i32 logit_c
   ▼
logits[10]
   │
   ▼   Stage 3: k_argmax_vec
   │     - SEW=32 LMUL=4 vl=10 reload of tagged logits
   │     - vredmax.vs over 10 i32 values
   │     - vmv.x.s + andi 0xF -> predicted class
   ▼
predicted class
```

The test runs the pipeline against 10 stylised digit patterns (one per
class) and verifies each classifies into its corresponding class.

**Why this fabric — creative 2D usage.**

  * **vrgather.vv as a 128-byte LUT activation.** After `|Gx|+|Gy|`
    produces edge magnitudes in `[0..127]`, a single `vrgather.vv`
    instruction looks up each edge value in a 128-byte LUT for every
    pixel of every hardware row. **16 384 parallel byte lookups in
    one instruction.** The LUT itself is loaded once into a register
    group from a "lut-replicated" memory buffer (every hw-row gets
    the same LUT bytes via the standard replicate trick). For our
    binarising LUT, `LUT[i] = (i >= 24) ? 1 : 0`.

  * **Different feature maps in different register groups.** Stage 1
    holds `Gx` in `v20` and `Gy` in `v24` simultaneously — two
    distinct 128×128 feature maps alive in VRF without a memory
    roundtrip between them. The LUT `v4`, the source `v8`, and the
    output `v28` round out the register footprint.

  * **Vector argmax via index-tagged vredmax.** The 10 i32 logits are
    tagged with their class index in the low 4 bits
    (`tagged[c] = (logit_c << 4) | c`) and reduced with `vredmax.vs`
    at SEW=32 LMUL=4 vl=10. `vmv.x.s` extracts the winner; `andi
    0xF` recovers the predicted class. This puts the argmax on the
    vector unit instead of a scalar tournament loop.

  * **Two orthogonal conv filters from one image load.** Same trick
    as Sobel — single `vle8` services both Gx (H mode) and Gy (V
    mode); the filter switch costs one `csrw 0x7c0`.

  * **Per-row vredsum.vs as the FC primitive.** Same as Demos 3/4 —
    each hardware row collapses 128 elements into one byte in one
    instruction.

**Bypassing softmax — the architectural reasoning.**

The 2D RVV fabric here has neither floating-point nor `exp` / `log` /
`div` primitives — softmax is impossible to compute exactly.
**Softmax is monotonic**, so for inference we never need it:
`argmax_c softmax(z)_c == argmax_c z_c`. This demo runs argmax
directly. This is the standard pattern used by every int8-quantized
NN runtime (TFLite int8, ONNX QLinear, NVIDIA TensorRT-Quant).

The argmax itself is executed on the vector unit (Stage 3) rather
than the scalar core, to maximise RVV utilisation.

For workloads that genuinely need *probabilities* (not just the
top-1 class), three int8-friendly substitutes are documented here
even though only the first is implicitly used by this demo:

  1. **Hard-sparsemax.** `clamp(z - τ, 0, ∞)` followed by
     normalise-and-divide. Vectorisable as `vsub.vx` + `vmax.vv` +
     `vredsum.vs` + scalar reciprocal. No `exp` needed. The LUT
     activation in our Stage 1 is structurally a hard-sparsemax-shaped
     non-linearity (binary thresholding).
  2. **vrgather LUT for `exp`.** Pre-compute `exp(z/T)` for
     `z ∈ [-128..127]` into a 256-byte LUT, apply via `vrgather.vv`
     pixelwise (same primitive as Stage 1's edge LUT), then
     `vredsum.vs` + scalar divide. Integer-only.
  3. **Piecewise-linear `exp`.** Break `[-128..127]` into 8 segments,
     each a `slope * x + intercept` linear approximation. All
     coefficients pre-quantized to i8.

The demo executes (1) implicitly (the LUT activation thresholds
edges into a binary feature map). (2) and (3) are documented above
but not run.

**Other RVV-can't-do issues addressed.**

  * **No batch normalization** — folded into conv weights / bias at
    quantization time (standard trick). The hand-engineered weights
    incorporate any normalization implicitly.
  * **No max-pool with stride > 1 directly** — the FC head's per-row
    `vredsum.vs` + scalar aggregate effectively does global pooling.
  * **No int8 multiply with int16 accumulator** — would need
    `vwmul.vv` whose destination needs LMUL=8 (forbidden, R4).
    Mitigated by binarising features (LUT thresholds to {0, 1})
    and using binary weights ({0, 1}); per-row sum stays in i8
    range, and the cross-row sum lives in scalar i32.

**Register layout (LMUL=4 groups).**

| Group | Stage 1 (conv_relu_lut) | Stage 2 (score_class) | Stage 3 (argmax_vec) |
|-------|-------------------------|-----------------------|----------------------|
| `v0`  | unused (mask) | unused | unused |
| `v4`  | LUT (128 bytes per hw-row) | – | tagged logits (SEW=32 vl=10) |
| `v8`  | input image | edges (loaded) | reduction destination |
| `v12` | slide aux (right then below); ReLU 0 source | weights `W_c` | – |
| `v16` | slide aux (left then above) | edges .* `W_c` | – |
| `v20` | Gx, then `\|Gx\|`, then sum, then ReLU output | per-row vredsum result | – |
| `v24` | Gy, then `\|Gy\|` | – | – |
| `v28` | LUT-applied output (final binary edges) | – | – |

Peak: 6 of 7 usable groups during Stage 1; 4 during Stage 2; 2
during Stage 3.

**Test inputs — 10 stylised digit patterns.**

Each pattern is rendered procedurally inside `render_digit(d)` using
a small set of drawing primitives (`fill_rect`, `draw_oval_ring`).
The patterns are deliberately blocky and 7-segment-display-like so
that:

  * The Sobel-like edge filter produces clean, recognisable edge
    maps with sharp transitions at the bar/oval boundaries.
  * Each digit's edge map is distinguishable from the others (no
    two patterns should have nearly-identical edges).

**Class weight templates — derived from the patterns.**

For each digit `d`, the template `W_d` is the binary edge map of
the rendered pattern for `d`, computed using the same
`k_conv_relu_lut` kernel that processes the test input. This
ensures the templates are by construction "the right shape" for
template matching: when input `d` is fed through Stages 1-3,
`predicted_class == d` because the matching template correlates
exactly with the input's edge map.

This is template matching dressed in CNN clothing — the test's goal
is to demonstrate the *architecture's capability* to run a CNN, not
to demonstrate a state-of-the-art classifier. A real network would
have learned weights from a training set; the architectural
mechanics on this fabric would be identical.

**Verification.**

  * Per-input: `[CHECK] PASS cnn_digit input N: predicted=K (expected K)`
    for each of the 10 inputs.
  * Aggregate: `[CHECK] PASS cnn_digit: 10/10 inputs classified correctly`.

**Visualisation.** The demo emits multiple grid dumps so the
`visualize.py` script can render the full pipeline state for every
test input plus all 10 weight templates. This is the most visually
informative demo to view post-run — the figure shows 10 input
patterns alongside their edge maps and the matching templates.

## How to run all five demos

`T1_MIRROR_RTL_WRITES=1` is **required** for any demo that flips CSR
`0x7c0` (sobel_edge, gaussian_blur, matmul_via_vt, cnn_digit) — without
it Spike's shadow memory diverges from the RTL on every vert-mode
store and the C-side verifier sees Spike's shadow, not the actual RTL
output. Setting it for `matvec_fc_relu` is harmless.

```sh
cd /home/cbt22/code/code_fyp/VisionSoC

T1_MIRROR_RTL_WRITES=1 bash run-test.sh vision_program.sobel_edge       -c mudkip2d128small1bram1chain2lanescale -i t1emu -e verilator-emu --max-cycles 50000000
T1_MIRROR_RTL_WRITES=1 bash run-test.sh vision_program.gaussian_blur    -c mudkip2d128small1bram1chain2lanescale -i t1emu -e verilator-emu --max-cycles 50000000
T1_MIRROR_RTL_WRITES=1 bash run-test.sh vision_program.matvec_fc_relu   -c mudkip2d128small1bram1chain2lanescale -i t1emu -e verilator-emu --max-cycles 50000000
T1_MIRROR_RTL_WRITES=1 bash run-test.sh vision_program.matmul_via_vt    -c mudkip2d128small1bram1chain2lanescale -i t1emu -e verilator-emu --max-cycles 50000000
T1_MIRROR_RTL_WRITES=1 bash run-test.sh vision_program.cnn_digit        -c mudkip2d128small1bram1chain2lanescale -i t1emu -e verilator-emu --max-cycles 100000000
```

Each invocation creates `test_output/mudkip2d128small1bram1chain2lanescale/vision_program.<demo>-<TS>/`
containing:

  * `run.log` — must end with `[CHECK] PASS <demo_name>`. For
    `matmul_via_vt` you should see *two* PASS lines: one for the
    transpose primitive and one for the full matmul.
  * `<demo>.s` — disassembly. Verify no `vs1r.v` / `vl1r.v` /
    `csrr.*vlenb` appears inside any naked kernel (proof of no
    compiler spill — programmer rule R2).

## Visualising input/output frames

Each demo prints its 128×128 grids in a machine-parseable format
(`[GRID_DUMP_BEGIN] <name>` / `[GRID_DUMP_END] <name>`). The Python
script under `tests/vision_program/visualize/` parses these and
renders them side-by-side as a PNG.

```sh
python3 tests/vision_program/visualize/visualize.py \
    test_output/mudkip2d128small1bram1chain2lanescale/vision_program.sobel_edge-<TS>/run.log
```

By default this writes `viz.png` next to the `run.log`. See
`tests/vision_program/visualize/README.md` for full options.

## Future demo ideas

  * **3×3 generic convolution** (`vrgather` indexed neighborhood
    gather + dot product). Useful for first-layer CNNs with non-Sobel
    kernels.
  * **Gamma / tone-map LUT** (`vrgather.vx` with pixel value as
    index). 16 K parallel byte lookups, 1 instruction.
  * **SAD stereo matching** (per-row `|a − b|` + `vredsum.vs`). Maps
    cleanly to the per-row reduction primitive.
  * **Erosion / dilation** (3×3 `vmax` / `vmin` separable). Pattern
    is identical to Sobel/Gaussian with `vmax` / `vmin` substituted
    for `vsub` / `vadd`.
  * **Multi-channel CNN layer.** Loop over input channels with the
    matvec inner kernel; introduces an outer accumulation pass.

Each future demo should follow the same conventions (volatile init,
naked kernel, scalar-C verify, dump_grid markers) so that the
visualiser keeps working without per-demo handling.
