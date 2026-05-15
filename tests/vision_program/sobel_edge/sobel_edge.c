/*
 * sobel_edge - 3x3 separable Sobel edge detector on the 2D RVV fabric.
 *
 * Architecture lever: H+V mode flip in compute. The horizontal gradient
 * Gx is computed as (right - left) within each image row using a
 * horizontal-mode 3-tap slide; the vertical gradient Gy is the same
 * formula on the same loaded image but with CSR 0x7c0 = 1, so the slide
 * moves between hardware rows, i.e. between image rows. Output is
 * |Gx| + |Gy| with int8 saturation.
 *
 * Single LSU round-trip: load once, do H+V compute in registers, store once.
 *
 * Register budget (LMUL=4 groups):
 *   v8  = image (input)
 *   v12 = slide aux (right neighbour, then below)
 *   v16 = slide aux (left neighbour, then above)  reused for output later
 *   v20 = Gx
 *   v24 = Gy
 *
 * Per-row scalar verify reproduces the same arithmetic in C.
 *
 * See fyp_doc/vision_program_demos.md and fyp_doc/2d_fabric_handoff.md
 * for the architectural background.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "emurt.h"

#define ROWS 128
#define COLS 128
#define PERF_REG_ADDR 0x10000014

int8_t grid_in[ROWS][COLS];
int8_t grid_out[ROWS][COLS];

__attribute__((noinline))
static void init_grids(void) {
    volatile int8_t *p_in  = (volatile int8_t *)&grid_in[0][0];
    volatile int8_t *p_out = (volatile int8_t *)&grid_out[0][0];
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            // A test image with a diagonal step + a vertical bar so both
            // gradient directions produce visible structure.
            int v;
            if (c >= 60 && c < 70) {
                v = 80;             // bright vertical bar (Gx fires at edges)
            } else if (r > c) {
                v = 30;             // lower triangle (slow gradient)
            } else {
                v = -30;            // upper triangle
            }
            p_in[r * COLS + c]  = (int8_t)v;
            p_out[r * COLS + c] = 0;
        }
    }
}

static void print_grid(int n, int8_t *g) {
    printf("First %d rows:\n", n);
    for (int r = 0; r < n; r++) {
        for (int c = 0; c < COLS; c++) {
            printf("%d ", g[r * COLS + c]);
        }
        printf("\n");
    }
    if (n < ROWS) printf("...\n");
}

static void dump_grid(const char *name, const int8_t *g) {
    printf("[GRID_DUMP_BEGIN] %s\n", name);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            printf("%d ", g[r * COLS + c]);
        }
        printf("\n");
    }
    printf("[GRID_DUMP_END] %s\n", name);
}

static inline int8_t sat8(int v) {
    if (v >  127) return  127;
    if (v < -128) return -128;
    return (int8_t)v;
}

/* a0 = src, a1 = dst, a2 = vl, a3 = perf_reg */
__attribute__((naked, noinline))
void k_sobel(int8_t *src, int8_t *dst, size_t vl, uintptr_t perf_reg) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"

        "li      t0, 1                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a3)                  \n\t"  // perf START tag=1

        // ---- Horizontal gradient Gx = right - left ----
        "vmv.v.i v12, 0                     \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "vslidedown.vi v12, v8, 1           \n\t"  // c+1: right
        "vslideup.vi   v16, v8, 1           \n\t"  // c-1: left
        "vsub.vv v20, v12, v16              \n\t"  // Gx = right - left

        // ---- Vertical gradient Gy = below - above (mode flip) ----
        "li      t3, 1                      \n\t"
        "csrw    0x7c0, t3                  \n\t"
        "vmv.v.i v12, 0                     \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "vslidedown.vi v12, v8, 1           \n\t"  // r+1: below
        "vslideup.vi   v16, v8, 1           \n\t"  // r-1: above
        "vsub.vv v24, v12, v16              \n\t"  // Gy = below - above
        "csrw    0x7c0, zero                \n\t"

        // ---- |Gx| via vneg + vmax (no vabs in base RVV) ----
        "vrsub.vi v12, v20, 0               \n\t"  // v12 = -Gx
        "vmax.vv  v20, v20, v12             \n\t"  // v20 = max(Gx, -Gx) = |Gx|

        // ---- |Gy| via vneg + vmax ----
        "vrsub.vi v16, v24, 0               \n\t"  // v16 = -Gy
        "vmax.vv  v24, v24, v16             \n\t"  // v24 = |Gy|

        // ---- |Gx| + |Gy| with i8 saturation ----
        "vsadd.vv v16, v20, v24             \n\t"

        "sw      t1, 0(a3)                  \n\t"  // perf STOP

        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

__attribute__((noinline))
static void verify(void) {
    int errors = 0;
    int br = -1, bc = -1;
    int8_t bgot = 0, bexp = 0;

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            // Boundary: slides shift in 0 from outside the loaded register.
            // For Gx: column 0 has no left neighbour, column 127 no right.
            // For Gy: row 0 has no above, row 127 no below.
            // The kernel uses vslideup/vslidedown which fill with 0 at the
            // shifted-in side, so the "missing neighbour" is treated as 0.
            int left  = (c == 0)        ? 0 : (int)grid_in[r][c - 1];
            int right = (c == COLS - 1) ? 0 : (int)grid_in[r][c + 1];
            int above = (r == 0)        ? 0 : (int)grid_in[r - 1][c];
            int below = (r == ROWS - 1) ? 0 : (int)grid_in[r + 1][c];

            // Inside slide-window: subtract uses signed int8 wrap, but
            // the post-vrsub vmax gives |x|. Reproduce same path in C:
            int8_t gx = (int8_t)(right - left);  // i8 wrap
            int8_t gy = (int8_t)(below - above); // i8 wrap

            // |x| in i8 with x = -128 stays -128 because -(-128) = -128.
            // vmax(x, -x) handles this correctly: max(-128, -128) = -128.
            int8_t agx = (gx == -128) ? -128 : (int8_t)abs(gx);
            int8_t agy = (gy == -128) ? -128 : (int8_t)abs(gy);

            // vsadd.vv saturates to int8 range
            int sum = (int)agx + (int)agy;
            int8_t exp = sat8(sum);

            int8_t got = grid_out[r][c];
            if (got != exp) {
                if (errors == 0) {
                    br = r; bc = c; bgot = got; bexp = exp;
                }
                errors++;
            }
        }
    }

    if (errors == 0) {
        printf("[CHECK] PASS sobel_edge: all %d cells correct\n", ROWS * COLS);
    } else {
        printf("[CHECK] FAIL sobel_edge: %d / %d errors; first at [%d][%d] got %d exp %d\n",
               errors, ROWS * COLS, br, bc, (int)bgot, (int)bexp);
    }
}

void test(void) {
    printf("=== sobel_edge: 3x3 separable Sobel via H+V mode flip ===\n");
    init_grids();

    printf("Input sample:\n");
    print_grid(3, &grid_in[0][0]);

    dump_grid("grid_in", &grid_in[0][0]);

    k_sobel(&grid_in[0][0], &grid_out[0][0], (size_t)COLS,
            (uintptr_t)PERF_REG_ADDR);

    printf("Output sample:\n");
    print_grid(3, &grid_out[0][0]);

    dump_grid("grid_out", &grid_out[0][0]);

    verify();
    printf("=== sobel_edge done ===\n");
}
