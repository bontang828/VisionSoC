/*
 * matmul_weights.h -- PS-side builder for the matmul weight matrix B.
 *
 * The matmul kernels compute C = A * B with
 *     C[r][k] = sum_c A[r][c] * B[c][k]
 * so column k of B is the weight vector that combines the INPUT image
 * columns into OUTPUT column k -- i.e. B mixes pixels horizontally.
 *
 * B is staged into URAM once at startup by main.c (see ACTIVE_KERNEL_*
 * macros the matmul *_select.h export). Stored row-major to match the
 * kernel's vertical (transposing) load:  b[c*128 + k] = B[c][k].
 *
 * Patterns are kept small/sparse on purpose:
 *   IDENTITY / SHIFT : single tap per column -> exact, never overflows,
 *                      safe for the raw SEW=8 kernel (matmul_8bitraw).
 *   BLUR             : 3-tap [1,2,1] (weight sum 4) -> needs the widening
 *                      kernel (matmul) with a >>2 display scale
 *                      (MM_SH1+MM_SH2 = 2 in matmul.S). Overflows the raw
 *                      kernel, so don't pair BLUR with matmul_8bitraw.
 */
#pragma once

#include <stdint.h>

enum mmk_weight_pattern {
    MMK_B_IDENTITY = 0, /* C = A (passthrough; exact)                    */
    MMK_B_SHIFT    = 1, /* C[r][k] = A[r][k + MMK_B_SHIFT_D] (slide left) */
    MMK_B_BLUR     = 2, /* horizontal [1,2,1] blur (sum 4, widening only) */
};

#define MMK_B_SHIFT_D 8   /* columns to slide for MMK_B_SHIFT */

/* Build a 128x128 u8 weight matrix into `b` (must hold >= 128*128 bytes). */
static inline void mmk_build_weights(uint8_t *b, int pattern)
{
    for (int c = 0; c < 128; c++) {
        for (int k = 0; k < 128; k++) {
            int v = 0;
            switch (pattern) {
                case MMK_B_SHIFT:
                    v = (c == k + MMK_B_SHIFT_D) ? 1 : 0;
                    break;
                case MMK_B_BLUR:
                    /* [1,2,1] centred on column k; out-of-range c never
                     * matches, so edges just drop a tap (slightly darker). */
                    if (c == k)                       v = 2;
                    else if (c == k - 1 || c == k + 1) v = 1;
                    else                               v = 0;
                    break;
                case MMK_B_IDENTITY:
                default:
                    v = (c == k) ? 1 : 0;
                    break;
            }
            b[c * 128 + k] = (uint8_t)v;
        }
    }
}
