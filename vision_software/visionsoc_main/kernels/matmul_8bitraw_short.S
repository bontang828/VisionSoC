/*
 * matmul_8bitraw_short -- C = A * B on the 2D fabric, SEW=8, short loop.
 *
 * This is a T1-specific variant of matmul_8bitraw.S. It keeps the same
 * three host-side stages:
 *   preamble  = load A once, vertical-load B once, clear accumulators
 *   body      = issue 128 iterations with gather index k = 127..0
 *   postamble = store C once
 *
 * The difference is the body: it does not build a lane-0 mask and does not
 * merge a separate reduction register into v7. Instead it shifts the current
 * result row first, commits that shift to v7, and then writes the reduction
 * result directly into v7[0]. This intentionally relies on the 2D fabric's
 * observed reduction behaviour, rather than a conservative stock-RVV shape.
 * The body also keeps explicit CSR-mode markers around the vertical gather so
 * the .S shows the intended H/V sequence. The Linux/libt1 selector tracks and
 * skips those scalar CSR words, then supplies op.vertical_mode per T1 issue.
 *
 * Register map (LMUL=1, vl=128, SEW=8):
 *   v4  = A  (H-loaded, live for the whole kernel)
 *   v5  = Bt (V-loaded, live for the whole kernel)
 *   v6  = per-iteration broadcast column of B, then product
 *   v7  = persistent result row; vredsum writes the new dot product in place
 *   v8  = shifted-result scratch
 *   v28 = zero, used as the vredsum scalar source
 *
 * a0 = A source PA, a1 = B weights PA, a2 = C dest PA.
 * a4 names the vrgather index register; the selector injects k into rs1.
 */
.text
.global matmul_8bitraw_short
matmul_8bitraw_short:
    /* ---- preamble [0..6) ---- */
    vle8.v        v4, (a0)            /* 0  H: A -> v4                     */
    csrwi         0x7c0, 1            /* 1  -> V mode (selector-only)      */
    vle8.v        v5, (a1)            /* 2  V: B -> v5 = transpose(B)      */
    csrwi         0x7c0, 0            /* 3  -> H mode                      */
    vmv.v.i       v7, 0               /* 4  H: zero the result row         */
    vmv.v.i       v28, 0              /* 5  H: zero the vredsum scalar src */

    /* ---- body [6..13): one output column k, issued 128x ---- */
    vslideup.vi   v8, v7, 1           /* 6  H: shift previous results      */
    vmv.v.v       v7, v8              /* 7  H: commit shifted row to v7    */
    csrwi         0x7c0, 1            /* 8  -> V mode for vrgather         */
    vrgather.vx   v6, v5, a4          /* 9  V: broadcast hw-row k of v5    */
    csrwi         0x7c0, 0            /* 10 -> H mode for compute/reduce   */
    vmul.vv       v6, v4, v6          /* 11 H: A[r][c]*B[c][k] (low byte)  */
    vredsum.vs    v7, v6, v28         /* 12 H: v7[0] = sum_c (mod 256)     */

    /* ---- postamble [13..14) ---- */
    vse8.v        v7, (a2)            /* 13 H: store C row -> dst          */
