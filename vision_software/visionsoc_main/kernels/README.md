# VisionSoC Kernel Inventory

This folder holds RVV instruction streams for the Linux-side `libt1`
runtime. Assemble a `.S` file into a C header with
`../../libt1/build_kernel.sh`; the generated array is then issued one
instruction at a time through `t1_issue()`.

## Existing Kernels

### `grid_vadd.S` / `grid_vadd.h`

Simple four-instruction smoke kernel:

```text
vle8.v  v8,  (src_a)
vle8.v  v12, (src_b)
vsub.vv v8,  v8, v12
vse8.v  v8,  (dst)
```

Used by the current `visionsoc_main` camera/display loop as a minimal
frame-processing placeholder.

### `scratchpad_self.S`

DDR -> vector register -> scratchpad -> vector register -> DDR probe
sequence. This is mainly a scratchpad bring-up reference; several libt1
tests hard-code the validated instruction words instead of including a
generated header from this file.

## New Decoder-CNN Kernel

### `cnn2d_decoder_primitives.S`

Created for the larger 128x128 digit-recognition CNN decoder. Runtime
inference is decoder-only: it consumes a camera greyscale frame and
learned/exported ternary weights. Any encoder or autoencoder-style
component belongs only in the offline training pipeline, not in the
board-side frame loop.

This file is a primitive pack, not one monolithic CNN. The reason is the
T1 control model: `libt1` issues one instruction at a time, and the host
sets `rs1`, `rs2`, `vl`, `vtype`, and `vertical_mode` per issue. The
same instruction word can therefore be reused for many layers and
dilations.

Generate the header on the KV260 or another machine with the RISC-V
binutils installed:

```sh
cd ~/vision_software/visionsoc_main
../libt1/build_kernel.sh \
  kernels/cnn2d_decoder_primitives.S \
  kernels/cnn2d_decoder_primitives.h \
  cnn2d_decoder_primitives
```

The generated `cnn2d_decoder_primitives[]` array has this ABI:

| Indices | Purpose | Host-side usage |
|---|---|---|
| `0..22` | Sobel-like edge extractor plus LUT activation | Issue `0` with `rs1=input_pa`, `1` with `rs1=lut_pa`, `7..11` with `vertical_mode=1`, and `22` with `rs1=edges_pa`. Other words use `rs1=0`, `vertical_mode=0`. |
| `23..45` | Ternary/dilated convolution vocabulary | Load source map, zero/seed accumulator, issue horizontal or vertical slides with `rs1=dilation`, compose diagonal taps when needed, add/sub selected taps according to exported `{-1,0,+1}` weights, threshold with `rs1=threshold`, then store. |
| `46..54` | Pooling, class-head, and debug helpers | Use `vredmax.vs`/`vredsum.vs` for row reductions, tagged-logit `vredmax.vs` for vector argmax, and direct shifted-tap stores for bring-up checks. |
| `55..66` | Fixed-dilation immediate slides | `+/-1,+/-2,+/-4,+/-8,+/-16,+/-31` shifted taps for fixed CNN radii. Dynamic `vslide*.vx` helpers at `26/27` are also validated on 5o. |
| `67` | Tagged-winner store | Optional fallback: store `v20[0]` with e32/vl=1 after vector argmax. Normal scalar extraction should use `49` plus `t1_wait_rd()`. |

The intended decoder structure is:

```text
128x128 greyscale
  -> cnn2d_decoder_primitives[0..22]     # vector edge/LUT stem
  -> ternary dilated blocks d=1,2,4,8,16,31
  -> 1x1/pointwise class head scheduled as add/sub feature-map mixes
  -> vector max/sum pooling
  -> tagged vector argmax
```

The scalar core should only walk the exported layer schedule and choose
which primitive indices to issue. It should not perform per-pixel
convolution, activation, pooling, or class comparisons.

The C-side issue helpers live in `../cnn2d_decoder.c` / `../cnn2d_decoder.h`.
Use `cnn2d_issue_edge_lut()` for the stem, `cnn2d_issue_primitive()` for
exported tap schedules, `cnn2d_issue_row_pool()` for per-row class scores,
and `cnn2d_issue_tagged_argmax()` for final class selection.

### Weight Schedule Convention

For each ternary conv layer, export sparse taps instead of dense int8
weights:

```c
struct cnn2d_tap {
    uint8_t in_ch;
    uint8_t out_ch;
    int8_t dy;
    int8_t dx;
    int8_t sign;      /* -1 or +1; zero weights are omitted */
};
```

The host maps each tap to primitive issues:

- `dx != 0`: issue dynamic slide index `26` or `27` with `vertical_mode=0`,
  or use the matching fixed immediate slide from `55..66`.
- `dy != 0`: issue dynamic slide index `26` or `27` with `vertical_mode=1`,
  or use the matching fixed immediate slide from `55..66`; then consume that
  shifted tap with the corresponding add/sub primitive under
  `vertical_mode=1`.
- `dx != 0 && dy != 0`: first shift one axis into `v12` or `v16`, then
  issue one of `42..45` in the other axis before accumulating; keep the
  accumulation in the mode of the final shifted-tap view.
- `sign > 0`: issue `28`, `29`, or `30`.
- `sign < 0`: issue `31`, `32`, or `33`.

Thresholds and small scalar biases are supplied through `rs1` to indices
`34`, `35`, and `36`. Store either the signed accumulator with `40` or
the binary activation with `41`.

### Notes For Camera Frames

The live camera path currently provides UYVY. The safest first runtime
path is still PS-side Y extraction into a 128x128 greyscale udmabuf,
because the benchmark validates stride-2 instructions but production
UYVY-to-grey through T1 still needs board-level coverage. Once verified,
`vlse8.v stride=2` can replace that scalar conversion.
