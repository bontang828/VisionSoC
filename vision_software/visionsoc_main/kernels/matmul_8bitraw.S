/*
 * matmul_8bitraw -- C = A * B on the 2D fabric, ENTIRELY in SEW=8.
 *
 * This is the "rawest" matmul: every value (input, product, dot-product
 * sum, output) is an 8-bit element. The 128-wide dot product therefore
 * WRAPS mod 256 at both the product (vmul truncates to the low byte) and
 * the sum (vredsum wraps). The output is the low byte of C[i][j]. It is
 * only meaningful as a demo when B is extremely sparse / small (e.g. a
 * 1-tap selector); for a real weighted matmul use the widening sibling
 * `matmul.S`, which keeps full precision and scales down for display.
 *
 * Algorithm (the user's H/V-mode formulation), per output column k:
 *   A is H-loaded   -> v4[hwrow r][elem c] = A[r][c]            (row r of A)
 *   B is V-loaded   -> v5 = Bt, so hw-row k of v5 = column k of B
 *   broadcast hw-row k of v5 (V-mode vrgather.vx) -> v6[r][c] = B[c][k]
 *   H vmul          -> v6[r][c] = A[r][c]*B[c][k]
 *   H vredsum       -> v24[r][0] = sum_c A[r][c]*B[c][k] = C[r][k]
 *   shift result row v7 right by 1, insert C[r][k] at lane 0
 * Iterating k = 127..0 builds v7 = [C[r][0], C[r][1], ... C[r][127]].
 *
 * The 128 iterations are driven by matmul_8bitraw_select.h (a C loop that
 * re-issues the BODY block with the gather index injected as op.rs1 and
 * the H/V mode + vl/vtype set per body position). This .S is therefore
 * laid out as three regions whose index ranges the select.h knows:
 *   preamble  = words [0 .. 8)
 *   body      = words [8 .. 14)   (issued 128x)
 *   postamble = words [14 .. 15)
 * Keep this layout and matmul_8bitraw_select.h's MMK_* offsets in lockstep.
 *
 * Register map (LMUL=1, big config, vl=128, SEW=8):
 *   v0  = lane-0-only predicate (built from vid+vmseq; gates the insert)
 *   v4  = A  (H-loaded, live for the whole kernel)
 *   v5  = Bt (V-loaded, live for the whole kernel)
 *   v6  = per-iter: broadcast column of B, then the elementwise product
 *   v7  = result row accumulator (the persistent C[r][:])
 *   v8  = scratch: vid (preamble), then the shifted result row (body)
 *   v24 = per-iter dot-product scalar at lane 0
 *   v28 = zero, used as the vredsum scalar source (vd[0] = vs1[0] + sum)
 *
 * a0 = A source PA (camera frame), a1 = B weights PA, a2 = C dest PA.
 * a4 names the vrgather index register; the dispatcher injects k into rs1.
 */
.text
.global matmul_8bitraw
matmul_8bitraw:
    /* ---- preamble [0..8) ---- */
    vle8.v        v4, (a0)            /* 0  H: A -> v4 (row r in hw-row r) */
    csrwi         0x7c0, 1            /* 1  -> V mode (dispatcher-only)    */
    vle8.v        v5, (a1)            /* 2  V: B -> v5 = transpose(B)      */
    csrwi         0x7c0, 0            /* 3  -> H mode                      */
    vid.v         v8                  /* 4  H: v8[c] = c (0..127 per row)  */
    vmseq.vi      v0, v8, 0           /* 5  H: v0 = (c==0) -> lane-0 mask  */
    vmv.v.i       v7, 0               /* 6  H: zero the result row         */
    vmv.v.i       v28, 0              /* 7  H: zero the vredsum scalar src */

    /* ---- body [8..14): one output column k, issued 128x ---- */
    vrgather.vx   v6, v5, a4          /* 8  V: broadcast hw-row k of v5    */
    vmul.vv       v6, v4, v6          /* 9  H: A[r][c]*B[c][k] (low byte)  */
    vredsum.vs    v24, v6, v28        /* 10 H: v24[0] = sum_c (mod 256)    */
    vslideup.vi   v8, v7, 1           /* 11 H: shift result row up by 1    */
    vmerge.vvm    v8, v8, v24, v0     /* 12 H: lane0 <- C[r][k]            */
    vmv.v.v       v7, v8              /* 13 H: commit shifted+inserted row */

    /* ---- postamble [14..15) ---- */
    vse8.v        v7, (a2)            /* 14 H: store C row -> dst          */
