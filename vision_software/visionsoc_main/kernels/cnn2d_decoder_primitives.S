.text
.global cnn2d_decoder_primitives_kernel
cnn2d_decoder_primitives_kernel:
    /*
     * Decoder-side CNN primitive pack for 128x128 e8/m4 T1 kernels.
     *
     * This file is intended to be assembled with:
     *   ../libt1/build_kernel.sh kernels/cnn2d_decoder_primitives.S \
     *       kernels/cnn2d_decoder_primitives.h cnn2d_decoder_primitives
     *
     * libt1 issues one word at a time. The host chooses rs1/rs2,
     * vertical_mode, vtype, and vl for each word. The instruction indices
     * below are the ABI; keep kernels/README.md in sync.
     */

    /* 0..22: Sobel-like edge extractor with LUT activation.
     *
     * Host issue contract:
     *   0      rs1=input_pa, vertical=0
     *   1      rs1=lut_pa,   vertical=0
     *   2..6   rs1=0,        vertical=0
     *   7..11  rs1=0,        vertical=1
     *   12..21 rs1=0,        vertical=0
     *   22     rs1=edges_pa, vertical=0
     */
    vle8.v  v8,  (a0)          /*  0 input image */
    vle8.v  v4,  (a0)          /*  1 128-byte replicated activation LUT */
    vmv.v.i v12, 0             /*  2 */
    vmv.v.i v16, 0             /*  3 */
    vslidedown.vi v12, v8, 1   /*  4 right neighbour */
    vslideup.vi   v16, v8, 1   /*  5 left neighbour */
    vsub.vv v20, v12, v16      /*  6 Gx = right - left */
    vmv.v.i v12, 0             /*  7 */
    vmv.v.i v16, 0             /*  8 */
    vslidedown.vi v12, v8, 1   /*  9 below neighbour, vertical mode */
    vslideup.vi   v16, v8, 1   /* 10 above neighbour, vertical mode */
    vsub.vv v24, v12, v16      /* 11 Gy = below - above */
    vrsub.vi v12, v20, 0       /* 12 -Gx */
    vmax.vv  v20, v20, v12     /* 13 abs(Gx), with int8 -128 behaviour */
    vrsub.vi v16, v24, 0       /* 14 -Gy */
    vmax.vv  v24, v24, v16     /* 15 abs(Gy), with int8 -128 behaviour */
    vsadd.vv v20, v20, v24     /* 16 saturated edge magnitude */
    vmv.v.i v12, 0             /* 17 */
    vmax.vv v20, v20, v12      /* 18 ReLU clamp after abs corner case */
    vrgather.vv v28, v4, v20   /* 19 LUT activation */
    vmv.v.v v8, v28            /* 20 expose edge map as normal src group */
    vmv.v.v v20, v28           /* 21 expose edge map as accumulator seed */
    vse8.v  v28, (a0)          /* 22 store binary/quantised edge map */

    /* 23..45: Host-scheduled ternary/dilated convolution primitives.
     *
     * The decoder uses these as a small instruction vocabulary:
     *   - load a source feature map into v8;
     *   - zero or seed v20 as the output accumulator;
     *   - create shifted copies in v12/v16 with rs1=dilation;
     *   - add or subtract selected taps according to exported ternary
     *     weights {-1,0,+1};
     *   - threshold v20 into a binary feature map through v0+vmerge;
     *   - store v28.
     *
     * For horizontal offsets, issue the slide word with vertical=0.
     * For vertical offsets, issue the same slide word with vertical=1.
     */
    vle8.v  v8,  (a0)          /* 23 load source feature map */
    vmv.v.i v20, 0             /* 24 accumulator = 0 */
    vmv.v.v v20, v8            /* 25 accumulator = source */
    vslideup.vx   v12, v8, a0  /* 26 shifted positive axis by rs1 */
    vslidedown.vx v16, v8, a0  /* 27 shifted negative axis by rs1 */
    vsadd.vv v20, v20, v8      /* 28 acc += centre */
    vsadd.vv v20, v20, v12     /* 29 acc += positive shifted tap */
    vsadd.vv v20, v20, v16     /* 30 acc += negative shifted tap */
    vssub.vv v20, v20, v8      /* 31 acc -= centre */
    vssub.vv v20, v20, v12     /* 32 acc -= positive shifted tap */
    vssub.vv v20, v20, v16     /* 33 acc -= negative shifted tap */
    vsadd.vx v20, v20, a0      /* 34 acc += scalar bias/offset */
    vssub.vx v20, v20, a0      /* 35 acc -= scalar bias/offset */
    vmsgt.vx v0, v20, a0       /* 36 v0 = acc > threshold */
    vmv.v.i v28, 0             /* 37 threshold output baseline */
    vmerge.vim v28, v28, 1, v0 /* 38 binary output = 1 where v0 */
    vmax.vv v20, v20, v28      /* 39 ReLU-ish clamp if v28 is zero */
    vse8.v  v20, (a0)          /* 40 store signed accumulator/activation */
    vse8.v  v28, (a0)          /* 41 store binary thresholded feature */
    vslideup.vx   v12, v12, a0 /* 42 compose diagonal from v12, +axis */
    vslidedown.vx v12, v12, a0 /* 43 compose diagonal from v12, -axis */
    vslideup.vx   v16, v16, a0 /* 44 compose diagonal from v16, +axis */
    vslidedown.vx v16, v16, a0 /* 45 compose diagonal from v16, -axis */

    /* 46..54: Per-map pooling, class-result, and bring-up helpers.
     *
     * `vredmax.vs` is used for global-max style CNN heads. In horizontal
     * mode it gives one max per image row at v20[0]. The host can then
     * either store row maxima with vl=1, or do a second vertical reduction
     * pass depending on the final class-head layout.
     */
    vmv.v.i v20, 0             /* 46 reduction seed */
    vredmax.vs v20, v8, v20    /* 47 per-row max of v8 */
    vredsum.vs v20, v8, v20    /* 48 per-row sum of v8 */
    vmv.x.s a0, v20            /* 49 scalar extract for debug/bringup */
    vle32.v v4, (a0)           /* 50 load tagged logits, e32/m4/vl=10 */
    vmv.v.i v20, 0             /* 51 argmax seed */
    vredmax.vs v20, v4, v20    /* 52 tagged-logit argmax, extract with 49 */
    vse8.v  v12, (a0)          /* 53 store positive shifted tap/debug */
    vse8.v  v16, (a0)          /* 54 store negative shifted tap/debug */

    /* 55..66: fixed-dilation immediate slides for the CNN schedule.
     *
     * These cover the planned dilation radii without depending on the
     * scalar-source `vslide*.vx` path. Host still chooses horizontal vs
     * vertical by setting vertical_mode on the issued word.
     */
    vslideup.vi   v12, v8, 1   /* 55 +1 */
    vslidedown.vi v16, v8, 1   /* 56 -1 */
    vslideup.vi   v12, v8, 2   /* 57 +2 */
    vslidedown.vi v16, v8, 2   /* 58 -2 */
    vslideup.vi   v12, v8, 4   /* 59 +4 */
    vslidedown.vi v16, v8, 4   /* 60 -4 */
    vslideup.vi   v12, v8, 8   /* 61 +8 */
    vslidedown.vi v16, v8, 8   /* 62 -8 */
    vslideup.vi   v12, v8, 16  /* 63 +16 */
    vslidedown.vi v16, v8, 16  /* 64 -16 */
    vslideup.vi   v12, v8, 31  /* 65 +31 */
    vslidedown.vi v16, v8, 31  /* 66 -31 */
    vse32.v v20, (a0)          /* 67 store tagged winner/result, e32 */
