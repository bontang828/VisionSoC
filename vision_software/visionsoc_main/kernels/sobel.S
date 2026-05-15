/*
 * sobel_kernel -- separable 3x3 Sobel via H/V mode flip on the 2D fabric.
 *
 * Algorithm verified on the simulator by
 * tests/vision_program/sobel_edge/sobel_edge.c. Output: sat8(|Gx| + |Gy|)
 * per pixel, single LSU round-trip.
 *
 * libt1 conventions handled by active_kernel_dispatcher.h:
 *   (a0)             = source PA  (camera Y plane)
 *   (a1)             = destination PA
 *   csrwi 0x7c0, 1   = enter vertical compute mode (cross-row slides)
 *   csrwi 0x7c0, 0   = back to horizontal mode
 * The dispatcher detects the csrwi words at issue time and skips them
 * (the T1 wrapper has no scalar core to execute a CSR write); it sets
 * op.vertical_mode = current mode on every following vector issue.
 */
.text
.global sobel_kernel
sobel_kernel:
    /* Horizontal pass: Gx = right - left within each image row. */
    vle8.v        v8,  (a0)
    vmv.v.i       v12, 0                  /* zero slide tail */
    vmv.v.i       v16, 0                  /* zero slide prologue */
    vslidedown.vi v12, v8,  1             /* v12[c] = img[c+1] (right) */
    vslideup.vi   v16, v8,  1             /* v16[c] = img[c-1] (left)  */
    vsub.vv       v20, v12, v16           /* Gx = right - left          */

    /* Vertical pass: Gy = below - above, slides now move between rows. */
    csrwi         0x7c0, 1
    vmv.v.i       v12, 0
    vmv.v.i       v16, 0
    vslidedown.vi v12, v8,  1             /* image row r+1 (below) */
    vslideup.vi   v16, v8,  1             /* image row r-1 (above) */
    vsub.vv       v24, v12, v16           /* Gy_V; H-view of v24 is Gy in image-space */
    csrwi         0x7c0, 0

    /* |Gx|, |Gy|, then saturated sum. Element-wise ops; mode-agnostic. */
    vrsub.vi      v12, v20, 0             /* -Gx */
    vmax.vv       v20, v20, v12           /* |Gx| = max(Gx, -Gx) */
    vrsub.vi      v16, v24, 0             /* -Gy (reads v24 in H view) */
    vmax.vv       v24, v24, v16           /* |Gy| */
    vsadd.vv      v16, v20, v24           /* sat8(|Gx| + |Gy|) */

    vse8.v        v16, (a1)
