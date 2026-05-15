# Decoder-Only CNN Digit Recogniser For 128x128 T1

This note defines the larger digit-recognition CNN intended to replace the
synthetic `cnn_digit` template-matcher demo. Runtime inference on the KV260
is decoder-only: it consumes one camera frame and a trained weight schedule.
If an encoder, denoising autoencoder, or contrastive pretext model is useful,
it belongs to offline training only and is not part of the board-side frame
loop.

## Runtime Shape

```text
128x128 greyscale camera frame
  -> vector edge/LUT stem
  -> ternary dilated conv block, d=1
  -> ternary dilated conv block, d=2
  -> ternary dilated conv block, d=4
  -> ternary dilated conv block, d=8
  -> ternary dilated conv block, d=16
  -> ternary dilated conv block, d=31
  -> ternary pointwise class head, 10 maps
  -> vector global max/sum pooling
  -> vector tagged argmax
```

The receptive field grows quickly without downsampling the 128x128 grid.
That is intentional: the current T1 configuration is strongest when each
instruction sweeps all 128 rows and keeps feature maps in the VRF or DDR
as 128-byte-pitched planes. Conventional max-pooling/flatten/dense layers
would move too much work back to the scalar core.

## Channel Plan

| Stage | Input maps | Output maps | Notes |
|---|---:|---:|---|
| Edge/LUT stem | 1 | 2 | Raw greyscale plus binary/quantised edges. |
| Dilated block `d=1` | 2 | 8 | Local strokes and corners. |
| Dilated block `d=2` | 8 | 12 | Stroke-width tolerance. |
| Dilated block `d=4` | 12 | 16 | Local digit-part context. |
| Dilated block `d=8` | 16 | 20 | Segment/loop context. |
| Dilated block `d=16` | 20 | 24 | Global upper/lower structure. |
| Dilated block `d=31` | 24 | 24 | Whole-digit context on 128x128 frames. |
| Class head | 24 | 10 | One heatmap per digit class. |

The first implementation should store intermediate feature maps in DDR.
Once correctness is stable, pairs of hot maps can be staged through the
32 KB scratchpad: two full 128x128 byte maps fit exactly.

## T1 Mapping

The new hardware primitive pack is
`vision_software/visionsoc_main/kernels/cnn2d_decoder_primitives.S`.
The C issue wrapper is `vision_software/visionsoc_main/cnn2d_decoder.c`;
it exposes the edge stem, generic primitive issue, row pooling, and
tagged argmax helpers for the real camera program.

Per-pixel work stays on the vector unit:

- horizontal taps use `vslideup.vx` / `vslidedown.vx` with
  `vertical_mode=0`;
- vertical taps use the same instruction words with `vertical_mode=1`;
- ternary weights map to saturated vector add/subtract;
- activation thresholds use `vmsgt.vx` plus `vmerge.vim`;
- class pooling uses `vredmax.vs` / `vredsum.vs`;
- class selection uses the existing tagged-logit `vredmax.vs` pattern.

The scalar core only issues the exported sparse schedule:

```c
struct cnn2d_tap {
    uint8_t in_ch;
    uint8_t out_ch;
    int8_t dy;
    int8_t dx;
    int8_t sign;  /* -1 or +1; zero weights omitted */
};
```

For a tap `(dy, dx, sign)`, the host loads the input channel, shifts in
the requested axis and dilation, and adds or subtracts the shifted map
from the current output accumulator. A layer is therefore a list of
primitive issues, not a scalar pixel loop.

## Quantisation

Use binary activations for the first board implementation:

```text
activation = (acc > threshold) ? 1 : 0
```

Use ternary weights:

```text
weight in {-1, 0, +1}
```

This avoids the poor fit of int8 multiply with wider accumulators on the
current LMUL=4 2D path. Batchnorm should be folded into the per-output-map
thresholds before export.

## Training Weights

Recommended offline flow:

1. Train in PyTorch with the decoder architecture above.
2. Start from MNIST plus EMNIST digits, USPS, SVHN cropped digits, and
   synthetic font digits rendered into 128x128 frames.
3. Add heavy augmentation: translation, rotation, perspective, blur,
   low contrast, uneven lighting, random backgrounds, stroke thickness,
   occlusion, and camera noise.
4. Fine-tune with actual KV260 camera frames once available.
5. Use quantisation-aware training with straight-through estimators for
   ternary weights and binary activations.
6. Fold batchnorm into thresholds and export sparse nonzero taps plus
   thresholds as C headers or a compact binary manifest.

For real camera robustness, collect negative examples too: blank frames,
partial digits, multi-digit views, hands, paper edges, and printed text
that is not a single digit. The decoder can expose a confidence margin
from the top-1/top-2 class logits so the application can reject uncertain
frames instead of forcing a digit prediction.

## Camera Input

The camera delivers UYVY. For the first reliable board path, strip Y on
the Cortex-A53 into a greyscale udmabuf before invoking T1. The
`benchmark_instructions` test covers stride-2 vector loads, so a later
T1-side UYVY-to-Y kernel is realistic, but it should be validated on the
FPGA before putting it in the frame loop.
