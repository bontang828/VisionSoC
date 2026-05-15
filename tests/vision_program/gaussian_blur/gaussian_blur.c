/*
 * gaussian_blur - Separable 3x3 Gaussian blur on the 2D RVV fabric.
 *
 * Filter weights:
 *
 *     [1 2 1] / 4   horizontal pass
 *     [1                                vertical pass
 *      2 ] / 4
 *      [1]
 *
 * The full 2D effect after both passes is the outer product:
 *
 *     1 2 1
 *     2 4 2 / 16
 *     1 2 1
 *
 * Architecture lever: H+V mode flip in compute. The horizontal pass uses
 * a horizontal-mode 3-tap weighted sum with vslideup/vslidedown; the
 * vertical pass uses the same arithmetic but under CSR 0x7c0 = 1, so
 * the slide moves between hardware (= image) rows.
 *
 * Single LSU round-trip, two compute passes back-to-back in the VRF.
 *
 * Register budget (LMUL=4 groups):
 *   v8  = image (input);    later reused as below-row buffer in V pass
 *   v12 = right neighbour;  later reused as above-row buffer in V pass
 *   v16 = left neighbour;   final output
 *   v20 = horizontal-pass result (h_blur)
 *
 * Verification reproduces the same arithmetic with int8 wrap.
 */

#include <stdio.h>
#include <stdint.h>
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
            // A noisy sinusoidal pattern + small high-freq tile so the
            // blur effect is visible.
            int base = ((r * 7) ^ (c * 11)) & 0x3f;        // 0..63
            int hi   = ((r ^ c) & 1) ? 32 : -32;            // checker pattern
            int v    = base + hi - 32;                       // ~ -32..63
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

/* a0 = src, a1 = dst, a2 = vl, a3 = perf_reg */
__attribute__((naked, noinline))
void k_gauss(int8_t *src, int8_t *dst, size_t vl, uintptr_t perf_reg) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"

        "li      t0, 2                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a3)                  \n\t"  // perf START tag=2

        // ----- Horizontal pass: v20 = (left + 2*centre + right) >> 2 -----
        "vmv.v.i v12, 0                     \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "vslidedown.vi v12, v8, 1           \n\t"  // c+1: right
        "vslideup.vi   v16, v8, 1           \n\t"  // c-1: left
        "vadd.vv v20, v8, v8                \n\t"  // 2*centre
        "vadd.vv v20, v20, v12              \n\t"  // + right
        "vadd.vv v20, v20, v16              \n\t"  // + left
        "vsra.vi v20, v20, 2                \n\t"  // >> 2

        // ----- Vertical pass: v16 = (above + 2*v20 + below) >> 2 -----
        "li      t3, 1                      \n\t"
        "csrw    0x7c0, t3                  \n\t"
        "vmv.v.i v8,  0                     \n\t"
        "vmv.v.i v12, 0                     \n\t"
        "vslidedown.vi v8,  v20, 1          \n\t"  // r+1: below
        "vslideup.vi   v12, v20, 1          \n\t"  // r-1: above
        "vadd.vv v16, v20, v20              \n\t"  // 2*centre
        "vadd.vv v16, v16, v8               \n\t"  // + below
        "vadd.vv v16, v16, v12              \n\t"  // + above
        "vsra.vi v16, v16, 2                \n\t"  // >> 2
        "csrw    0x7c0, zero                \n\t"

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

    // Horizontal-pass intermediate (kept in scalar memory only for the
    // verifier; the kernel keeps it in VRF).
    static int8_t h_blur[ROWS][COLS];

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int left  = (c == 0)        ? 0 : (int)grid_in[r][c - 1];
            int right = (c == COLS - 1) ? 0 : (int)grid_in[r][c + 1];
            int sum   = left + 2 * (int)grid_in[r][c] + right;
            // vsra.vi semantic: arithmetic shift right of int8 = signed
            // shift. C does the same on signed int when value fits.
            int shifted = sum >> 2;
            h_blur[r][c] = (int8_t)shifted;   // i8 wrap matches kernel
        }
    }

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int above = (r == 0)        ? 0 : (int)h_blur[r - 1][c];
            int below = (r == ROWS - 1) ? 0 : (int)h_blur[r + 1][c];
            int sum   = above + 2 * (int)h_blur[r][c] + below;
            int shifted = sum >> 2;
            int8_t exp = (int8_t)shifted;

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
        printf("[CHECK] PASS gaussian_blur: all %d cells correct\n", ROWS * COLS);
    } else {
        printf("[CHECK] FAIL gaussian_blur: %d / %d errors; first at [%d][%d] got %d exp %d\n",
               errors, ROWS * COLS, br, bc, (int)bgot, (int)bexp);
    }
}

void test(void) {
    printf("=== gaussian_blur: separable 3x3 [1 2 1]/4 H + V ===\n");
    init_grids();

    printf("Input sample:\n");
    print_grid(3, &grid_in[0][0]);

    dump_grid("grid_in", &grid_in[0][0]);

    k_gauss(&grid_in[0][0], &grid_out[0][0], (size_t)COLS,
            (uintptr_t)PERF_REG_ADDR);

    printf("Output sample:\n");
    print_grid(3, &grid_out[0][0]);

    dump_grid("grid_out", &grid_out[0][0]);

    verify();
    printf("=== gaussian_blur done ===\n");
}
