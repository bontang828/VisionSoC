/*
 * optical_flow_kernel -- 5-direction integer-pixel block-matching optical
 * flow on the 2D RVV fabric.
 *
 * Per pixel, the kernel evaluates the Sum of Absolute Differences (SAD) of
 * `curr` against 5 candidate displacements of `prev`:
 *   - static (no motion)
 *   - +1 column  (object moved right between prev and curr)
 *   - -1 column  (object moved left)
 *   - +1 row     (object moved down)
 *   - -1 row     (object moved up)
 *
 * The horizontal candidates (right/left) use H-mode slides on `prev`. The
 * vertical candidates (down/up) use V-mode slides on `prev` -- which on the
 * 2D fabric shift across hw-rows, i.e. across image rows. The H/V CSR flip
 * is the same trick Sobel uses for Gy.
 *
 * Output per pixel: argmin(SAD) * 50 in {0, 50, 100, 150, 200} which renders
 * as 5 distinct grayscale levels on a luma display. A small temporal
 * deadband suppresses noisy motion votes: if SAD_static is <= 6 luma
 * levels, the output is forced back to static/no-motion.
 *   0   black   - static / no confident motion
 *   50  dark    - moved right
 *   100 gray    - moved left
 *   150 lighter - moved down
 *   200 bright  - moved up
 *
 * The kernel also rewrites `(a1)` with the current frame at the end so the
 * next iteration's `prev` is this iteration's `curr` -- no PS-side state.
 *
 * libt1 conventions handled by optical_flow_select.h (NOT the generic
 * active_kernel_dispatcher.h because we need 3 buffers + an injected scalar):
 *   (a0)             = source PA (curr from camera staging)
 *   (a1)             = prev PA (internal URAM region)
 *   (a2)             = destination PA (motion direction map)
 *   a3               = sentinel: dispatcher injects op.rs1 = 50 for any
 *                      vmul.vx using a3 (the visualisation scaling factor)
 *   csrwi 0x7c0, 1   = enter vertical compute mode (cross-row slides)
 *   csrwi 0x7c0, 0   = back to horizontal mode
 *
 * Register groups (LMUL=1 on the big vLen=1024 config, vl=128, SEW=8).
 * Disjoint per R3.4:
 *   v0  = predicate (built by vmsltu.vv, consumed by vmerge.vim)
 *   v4  = prev      (loaded once; live until the final vse)
 *   v8  = curr      (loaded once; live until the final two vse)
 *   v12 = per-candidate work register: shifted-prev -> diff -> SAD_d
 *   v16 = negation buffer for the abs idiom
 *   v20 = running min_sad accumulator
 *   v24 = running argmin (direction class 0..4) accumulator
 *   v28 = threshold work register: recomputed SAD_static
 */
.text
.global optical_flow_kernel
optical_flow_kernel:
    /* ---- Load curr and prev ----
     * v4 (prev) must be loaded BEFORE the final vse v8, (a1) overwrites
     * the (a1) backing store with curr. */
    vle8.v        v8,  (a0)              /*  0  curr -> v8 */
    vle8.v        v4,  (a1)              /*  1  prev -> v4 */

    /* ---- SAD_static = |curr - prev| -> v20 (initial min_sad);
     *      argmin = 0 (static class) ---- */
    vsub.vv       v12, v8,  v4           /*  2  diff_static (signed i8 wrap) */
    vrsub.vi      v16, v12, 0            /*  3  -diff */
    vmax.vv       v20, v12, v16          /*  4  |diff| = SAD_static -> min_sad */
    vmv.v.i       v24, 0                 /*  5  argmin = 0 (no motion) */

    /* ---- Candidate: object moved right
     *      prev_shifted_right[c] = prev[c-1] (vslideup.vi in H mode)
     *
     * IMPORTANT: zero v12 first. vslideup leaves the "prefix" (lanes
     * 0..OFFSET-1) UNDISTURBED per the RVV spec. Without this zero-init,
     * v12[0] would retain its prior value (the previous candidate's
     * SAD), polluting the boundary SAD. Sobel does the same with
     * vmv.v.i before its slides. */
    vmv.v.i       v12, 0                 /*  6  zero slide prologue */
    vslideup.vi   v12, v4,  1            /*  7  v12[c] = v4[c-1] (v12[0]=0) */
    vsub.vv       v12, v8,  v12          /*  8  diff_right */
    vrsub.vi      v16, v12, 0            /*  9  -diff */
    vmax.vv       v12, v12, v16          /* 10  SAD_right */
    vmsltu.vv     v0,  v12, v20          /* 11  v0 = (SAD_right < min_sad) */
    vminu.vv      v20, v12, v20          /* 12  min_sad = min(SAD_right, min_sad) */
    vmerge.vim    v24, v24, 1, v0        /* 13  argmin = 1 where right wins */

    /* ---- Candidate: object moved left
     *      prev_shifted_left[c] = prev[c+1] (vslidedown.vi in H mode)
     *
     * vslidedown leaves the "tail" (lane vl-1) undisturbed; zero v12
     * first to make SAD at the right boundary correct. */
    vmv.v.i       v12, 0                 /* 14  zero slide tail */
    vslidedown.vi v12, v4,  1            /* 15  v12[c] = v4[c+1] (v12[127]=0) */
    vsub.vv       v12, v8,  v12          /* 16  diff_left */
    vrsub.vi      v16, v12, 0            /* 17  -diff */
    vmax.vv       v12, v12, v16          /* 18  SAD_left */
    vmsltu.vv     v0,  v12, v20          /* 19  v0 = (SAD_left < min_sad) */
    vminu.vv      v20, v12, v20          /* 20  min_sad = min(SAD_left, min_sad) */
    vmerge.vim    v24, v24, 2, v0        /* 21  argmin = 2 where left wins */

    /* ---- Candidate: object moved down
     *      prev_shifted_down[r] = prev[r-1] (V-mode vslideup.vi)
     *
     * Sobel's V-mode pass pattern: do ALL of zero-init, slide, vsub,
     * vrsub, vmax in V mode so v12's V-write is consumed by V-reads.
     * Mixing V-write-then-H-read on v12 transposes the data (per
     * 2d_fabric_handoff: "same final data layout under either mode,
     * as long as operands and destination are read/written through
     * the same mode"). Only the comparison ops (vmsltu/vminu/vmerge)
     * read v12 in H mode -- they're element-wise reductions so the
     * H-view of V-written SAD is fine to use against v20 (the H-mode
     * accumulator). Same idiom as sobel.S's vrsub+vmax after the V
     * pass. */
    csrwi         0x7c0, 1               /* 22  enter V mode */
    vmv.v.i       v12, 0                 /* 23  V-mode zero slide prologue */
    vslideup.vi   v12, v4,  1            /* 24  V-mode slideup: image shifted down */
    vsub.vv       v12, v8,  v12          /* 25  V-mode diff_down */
    vrsub.vi      v16, v12, 0            /* 26  V-mode -diff */
    vmax.vv       v12, v12, v16          /* 27  V-mode SAD_down */
    csrwi         0x7c0, 0               /* 28  back to H mode */
    vmsltu.vv     v0,  v12, v20          /* 29  v0 = (SAD_down < min_sad) */
    vminu.vv      v20, v12, v20          /* 30  min_sad = min(SAD_down, min_sad) */
    vmerge.vim    v24, v24, 3, v0        /* 31  argmin = 3 where down wins */

    /* ---- Candidate: object moved up
     *      prev_shifted_up[r] = prev[r+1] (V-mode vslidedown.vi)
     *
     * Same pattern: V-mode for the slide+sub+abs, H-mode for the
     * compare/merge. */
    csrwi         0x7c0, 1               /* 32  enter V mode */
    vmv.v.i       v12, 0                 /* 33  V-mode zero slide tail */
    vslidedown.vi v12, v4,  1            /* 34  V-mode slidedown: image shifted up */
    vsub.vv       v12, v8,  v12          /* 35  V-mode diff_up */
    vrsub.vi      v16, v12, 0            /* 36  V-mode -diff */
    vmax.vv       v12, v12, v16          /* 37  V-mode SAD_up */
    csrwi         0x7c0, 0               /* 38  back to H mode */
    vmsltu.vv     v0,  v12, v20          /* 39  v0 = (SAD_up < min_sad) */
    vminu.vv      v20, v12, v20          /* 40  min_sad = min(SAD_up, min_sad) */
    vmerge.vim    v24, v24, 4, v0        /* 41  argmin = 4 where up wins */

    /* ---- Noise deadband ----
     * If curr is already close to prev, force static. Otherwise small
     * camera-noise differences can let right/left/up/down win in static
     * parts of the scene. */
    vsub.vv       v28, v8,  v4           /* 42  recompute diff_static */
    vrsub.vi      v16, v28, 0            /* 43  -diff_static */
    vmax.vv       v28, v28, v16          /* 44  v28 = SAD_static */
    vmsleu.vi     v0,  v28, 6            /* 45  static SAD <= noise threshold */
    vmerge.vim    v24, v24, 0, v0        /* 46  noisy pixels become static */

    /* ---- Scale argmin x 50 for grayscale visualization ---- */
    vmul.vx       v24, v24, a3           /* 47  dispatcher injects op.rs1 = 50 */

    /* ---- Stores: motion-direction map -> (a2); refresh prev <- curr ---- */
    vse8.v        v24, (a2)              /* 48  motion map -> dst */
    vse8.v        v8,  (a1)              /* 49  save curr -> prev for next iter */
