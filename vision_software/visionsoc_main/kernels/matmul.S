/*
 * matmul -- C = A * B on the 2D fabric with FULL-PRECISION accumulation.
 *
 * Same H/V-mode formulation as matmul_8bitraw.S, but the per-column dot
 * product is computed without overflow and then scaled back to 8-bit for
 * display:
 *   - vwmulu  : u8 x u8 -> u16 EXACT products (no truncation)
 *   - vwredsumu: sum of 128 u16 products -> u32 EXACT dot product
 *   - vnsrl x2 : narrow u32 -> u16 -> u8 with a right-shift each step,
 *                i.e. C[r][k] >> (MM_SH1 + MM_SH2), to land in 0..255.
 *
 * A 128-wide u8.u8 dot product reaches ~2^23, so the >> total must be
 * tuned to B's magnitude: a dense camera-vs-dense-weights product needs
 * ~>>16; a normalized/sparse weight kernel needs much less. MM_SH1/MM_SH2
 * below are the knobs (edit + rebuild via sync_kernel.sh). Narrowing
 * TRUNCATES (does not saturate), so under-shifting wraps the bright end.
 *
 * Only lane 0 of the wide intermediates is consumed (it is masked into
 * the result row via v0), so the narrowing is done at vl=1 -- this avoids
 * an LMUL=4 e32 row and keeps every wide value in a single register.
 *
 * Treats A and B as UNSIGNED (camera luma is 0..255). For signed weights
 * (e.g. an edge kernel with negative taps) swap vwmulu->vwmul and
 * vwredsumu->vwredsum and use vnsra (arithmetic) for the narrowing.
 *
 * !!! VERIFY ON SIM FIRST !!!  The widening (vwmulu/vwredsumu) and
 * narrowing (vnsrl.wi) ops, plus the mid-kernel SEW/vl changes, are NOT
 * exercised by any existing 2D-fabric test (sew16_32_masktest covers
 * SEW=16/32 slides/gather/merge only, not widening MACs or widening
 * reductions). Run this against t1emu before trusting hardware output.
 *
 * Region layout (kept in lockstep with matmul_select.h MMK_* offsets):
 *   preamble  = words [0 .. 8)
 *   body      = words [8 .. 16)   (issued 128x)
 *   postamble = words [16 .. 17)
 *
 * Register map (big config, baseLMUL=1):
 *   v0     = lane-0-only predicate (gates the insert)
 *   v4     = A  (H-loaded; SEW=8)
 *   v5     = Bt (V-loaded; SEW=8)
 *   v6     = per-iter broadcast column of B (SEW=8)
 *   v7     = result row accumulator (SEW=8; the persistent C[r][:])
 *   v8     = scratch: vid (preamble), shifted result row (body)
 *   v16:v17= per-iter u16 products  (SEW=16, LMUL=2 widening dst)
 *   v20    = per-iter u32 dot product at lane 0 (SEW=32 widening-reduce dst)
 *   v22    = u16 narrow-down intermediate (lane 0)
 *   v24    = u8  final scaled scalar at lane 0 (merged into v7)
 *   v28    = zero, the widening-reduce scalar source (vd[0]=vs1[0]+sum)
 *
 * a0 = A source PA, a1 = B weights PA, a2 = C dest PA.
 * a4 names the vrgather index register; dispatcher injects k into rs1.
 */
.text

/* Display scale: total right shift = MM_SH1 + MM_SH2 applied to the u32
 * dot product. Defaults to >>2 (= /4) to match the demo weight matrix
 * staged by main.c (the [1,2,1] horizontal blur, weight sum 4, in
 * matmul_weights.h MMK_B_BLUR). For a DENSE u8 x u8 B the dot product
 * reaches ~2^23, so raise this toward >>16 (e.g. MM_SH1=8, MM_SH2=8). */
.equ MM_SH1, 2
.equ MM_SH2, 0

.global matmul
matmul:
    /* ---- preamble [0..8) ---- */
    vle8.v        v4, (a0)            /* 0  H: A -> v4                     */
    csrwi         0x7c0, 1            /* 1  -> V mode (dispatcher-only)    */
    vle8.v        v5, (a1)            /* 2  V: B -> v5 = transpose(B)      */
    csrwi         0x7c0, 0            /* 3  -> H mode                      */
    vid.v         v8                  /* 4  H: v8[c] = c                   */
    vmseq.vi      v0, v8, 0           /* 5  H: v0 = (c==0) -> lane-0 mask  */
    vmv.v.i       v7, 0               /* 6  H: zero the result row         */
    vmv.v.i       v28, 0              /* 7  H: zero the reduce scalar src  */

    /* ---- body [8..16): one output column k, issued 128x ---- */
    vrgather.vx   v6, v5, a4          /* 8  V: broadcast hw-row k of v5    */
    vwmulu.vv     v16, v4, v6         /* 9  H: u8*u8 -> u16 products (m2)  */
    vwredsumu.vs  v20, v16, v28       /* 10 H: sum u16 -> u32 at v20[0]    */
    vnsrl.wi      v22, v20, MM_SH1    /* 11 H: u32 -> u16, >>MM_SH1 (vl=1) */
    vnsrl.wi      v24, v22, MM_SH2    /* 12 H: u16 -> u8,  >>MM_SH2 (vl=1) */
    vslideup.vi   v8, v7, 1           /* 13 H: shift result row up by 1    */
    vmerge.vvm    v8, v8, v24, v0     /* 14 H: lane0 <- scaled C[r][k]     */
    vmv.v.v       v7, v8              /* 15 H: commit shifted+inserted row */

    /* ---- postamble [16..17) ---- */
    vse8.v        v7, (a2)            /* 16 H: store C row -> dst          */
