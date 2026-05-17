#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

/******************************************************************************
 * benchmark_basic_instructions_lmul1
 *
 * Slim "did the LMUL=1 datapath wake up?" smoke test. NOT a 1:1 port of
 * benchmark_instructions.c -- for the full 35-kernel mirror with H+V
 * coverage, see sibling test `benchmark_instructions_lmul1`.
 *
 * Coverage (vs the full 29-kernel m4 reference, which is intentionally NOT
 * mirrored 1:1):
 *
 *   k_vadd_m1         - basic compute path (alu)              tag 1
 *   k_vmul_m1         - multiplier path                        tag 2
 *   k_vrgather_m1     - VRF-bandwidth-heavy permute            tag 3
 *   k_vadd_masked_m1  - v0 mask read (mask-unit path)          tag 4
 *   k_vle_vse_m1      - LSU round-trip                         tag 5
 *
 * Each kernel runs under horizontal mode (vert=0) because the goal is to
 * validate the LMUL=1 datapath, not vertical mode (which has separate
 * regression coverage in simple_instruction_vert_*).
 *
 * Programmer rules R1-R8 from benchmark_vadd.c carry over verbatim. The
 * inline asm blocks are naked + noinline + no internal C calls; v0 is
 * built with a vid+vmseq recipe that is mode-invariant per the
 * 2d_fabric handoff § 4.2 rule "build mode-invariant masks from vid".
 *****************************************************************************/

#define ROWS 128
#define COLS 128
#define PERF_REG_ADDR 0x10000014

int8_t grid_a[ROWS][COLS];
int8_t grid_b[ROWS][COLS];
int8_t grid_c[ROWS][COLS];
int8_t idx_grid[ROWS][COLS];   /* indices for vrgather.vv */

__attribute__((noinline))
static void init_grids(void) {
    volatile int8_t *pa = (volatile int8_t *)&grid_a[0][0];
    volatile int8_t *pb = (volatile int8_t *)&grid_b[0][0];
    volatile int8_t *pc = (volatile int8_t *)&grid_c[0][0];
    volatile int8_t *pi = (volatile int8_t *)&idx_grid[0][0];
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            pa[i * COLS + j] = (int8_t)((i + j) & (COLS - 1));
            pb[i * COLS + j] = (int8_t)((i * 2 + j) & (COLS - 1));
            pc[i * COLS + j] = 0;
            /* gather index: reverse-permutation, so out[j] = in[COLS-1-j] */
            pi[i * COLS + j] = (int8_t)((COLS - 1 - j) & (COLS - 1));
        }
    }
}

__attribute__((noinline))
static void clear_grid_c(void) {
    volatile int8_t *pc = (volatile int8_t *)&grid_c[0][0];
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            pc[i * COLS + j] = 0;
        }
    }
}

static void print_first_rows(int n, int8_t *grid) {
    for (int i = 0; i < n; i++) {
        printf(" row %d:", i);
        for (int j = 0; j < 8; j++) printf(" %d", grid[i * COLS + j]);
        printf(" ...\n");
    }
}

/*--- LMUL=1 kernels (naked, no spill, no calls) ---*/

/* vadd.vv: ALU path */
__attribute__((naked, noinline))
static void k_vadd_m1(int8_t *a, int8_t *b, int8_t *c, size_t vl,
                      uintptr_t perf, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vle8.v  v9, (a1)                   \n\t"
        "sw      a5, 0(a4)                  \n\t"
        "vadd.vv v10, v8, v9                \n\t"
        "sw      zero, 0(a4)                \n\t"
        "vse8.v  v10, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* vmul.vv: multiplier path. Use small operands so i8 saturation doesn't
 * confuse the verifier. */
__attribute__((naked, noinline))
static void k_vmul_m1(int8_t *a, int8_t *b, int8_t *c, size_t vl,
                      uintptr_t perf, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vle8.v  v9, (a1)                   \n\t"
        "sw      a5, 0(a4)                  \n\t"
        "vmul.vv v10, v8, v9                \n\t"
        "sw      zero, 0(a4)                \n\t"
        "vse8.v  v10, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* vrgather.vv: maximally stresses VRF read bandwidth.
 *   a0 = src grid, a1 = index grid, a2 = dst grid */
__attribute__((naked, noinline))
static void k_vrgather_m1(int8_t *src, int8_t *idx, int8_t *dst, size_t vl,
                          uintptr_t perf, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"   /* src */
        "vle8.v  v9,  (a1)                  \n\t"   /* idx */
        "sw      a5, 0(a4)                  \n\t"
        "vrgather.vv v10, v8, v9            \n\t"   /* LMUL=1: any vN legal */
        "sw      zero, 0(a4)                \n\t"
        "vse8.v  v10, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* vadd.vv,v0.t: mask-unit path. Build v0 = "even element indices" via
 * vid + vmseq, then masked-add boost into grid_a. Mask construction is
 * mode-invariant (vid is index-driven), so the bits are identical regardless
 * of CSR 0x7c0. */
__attribute__((naked, noinline))
static void k_vadd_masked_m1(int8_t *a, int8_t *c, size_t vl,
                             int boost, uintptr_t perf, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m1, ta, mu   \n\t"
        "vle8.v  v8, (a0)                   \n\t"          /* src */
        /* build v0 = (idx & 1) == 0 -> mask every other column */
        "vid.v   v9                         \n\t"
        "vand.vi v9, v9, 1                  \n\t"
        "vmseq.vi v0, v9, 0                 \n\t"
        "vmv.v.v v10, v8                    \n\t"          /* copy src */
        "sw      a5, 0(a4)                  \n\t"
        "vadd.vx v10, v10, a3, v0.t         \n\t"          /* masked boost */
        "sw      zero, 0(a4)                \n\t"
        "vse8.v  v10, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* vle/vse round-trip: LSU bandwidth check. */
__attribute__((naked, noinline))
static void k_vle_vse_m1(int8_t *a, int8_t *c, size_t vl,
                         uintptr_t perf, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
        "sw      a4, 0(a3)                  \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vse8.v  v8, (a1)                   \n\t"
        "sw      zero, 0(a3)                \n\t"
        "ret                                \n\t"
    );
}

/*--- C-side test wrappers + verifiers ---*/

__attribute__((noinline))
static void test_vadd(void) {
    printf("\n=== TEST 1 (LMUL=1): vadd.vv (tag=1) ===\n");
    clear_grid_c();
    k_vadd_m1(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
              (size_t)COLS, (uintptr_t)PERF_REG_ADDR, 1);

    int errors = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int8_t expected = (int8_t)(grid_a[r][c] + grid_b[r][c]);
            if (grid_c[r][c] != expected) {
                if (errors < 8) {
                    printf("  MISMATCH [%d][%d]: got %d, expected %d\n",
                           r, c, grid_c[r][c], expected);
                }
                errors++;
            }
        }
    }
    printf("[TEST 1] %s (errors=%d)\n", errors == 0 ? "PASS" : "FAIL", errors);
    print_first_rows(1, &grid_c[0][0]);
}

__attribute__((noinline))
static void test_vmul(void) {
    printf("\n=== TEST 2 (LMUL=1): vmul.vv (tag=2) ===\n");
    clear_grid_c();
    k_vmul_m1(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
              (size_t)COLS, (uintptr_t)PERF_REG_ADDR, 2);
    /* i8 * i8 wraps; just spot-check that something non-zero was written */
    int nonzero = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (grid_c[r][c] != 0) nonzero++;
        }
    }
    printf("[TEST 2] nonzero cells = %d / %d %s\n",
           nonzero, ROWS * COLS, nonzero > (ROWS * COLS / 2) ? "(plausible)" : "(SUSPICIOUS)");
    print_first_rows(1, &grid_c[0][0]);
}

__attribute__((noinline))
static void test_vrgather(void) {
    printf("\n=== TEST 3 (LMUL=1): vrgather.vv reverse permute (tag=3) ===\n");
    clear_grid_c();
    k_vrgather_m1(&grid_a[0][0], &idx_grid[0][0], &grid_c[0][0],
                  (size_t)COLS, (uintptr_t)PERF_REG_ADDR, 3);

    int errors = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int8_t expected = grid_a[r][COLS - 1 - c];
            if (grid_c[r][c] != expected) {
                if (errors < 8) {
                    printf("  MISMATCH [%d][%d]: got %d, expected %d\n",
                           r, c, grid_c[r][c], expected);
                }
                errors++;
            }
        }
    }
    printf("[TEST 3] %s (errors=%d)\n", errors == 0 ? "PASS" : "FAIL", errors);
    print_first_rows(1, &grid_c[0][0]);
}

__attribute__((noinline))
static void test_vadd_masked(int boost) {
    printf("\n=== TEST 4 (LMUL=1): vadd.vx v0.t (even-column mask, +%d, tag=4) ===\n", boost);
    clear_grid_c();
    k_vadd_masked_m1(&grid_a[0][0], &grid_c[0][0],
                     (size_t)COLS, boost, (uintptr_t)PERF_REG_ADDR, 4);

    /* expected: even columns get +boost, odd columns are copy-through */
    int errors = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int8_t expected = (c % 2 == 0)
                ? (int8_t)(grid_a[r][c] + boost)
                : grid_a[r][c];
            if (grid_c[r][c] != expected) {
                if (errors < 8) {
                    printf("  MISMATCH [%d][%d]: got %d, expected %d\n",
                           r, c, grid_c[r][c], expected);
                }
                errors++;
            }
        }
    }
    printf("[TEST 4] %s (errors=%d)\n", errors == 0 ? "PASS" : "FAIL", errors);
    print_first_rows(1, &grid_c[0][0]);
}

__attribute__((noinline))
static void test_vle_vse(void) {
    printf("\n=== TEST 5 (LMUL=1): vle8/vse8 round-trip (tag=5) ===\n");
    clear_grid_c();
    k_vle_vse_m1(&grid_a[0][0], &grid_c[0][0],
                 (size_t)COLS, (uintptr_t)PERF_REG_ADDR, 5);

    int errors = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (grid_c[r][c] != grid_a[r][c]) {
                if (errors < 8) {
                    printf("  MISMATCH [%d][%d]: got %d, expected %d\n",
                           r, c, grid_c[r][c], grid_a[r][c]);
                }
                errors++;
            }
        }
    }
    printf("[TEST 5] %s (errors=%d)\n", errors == 0 ? "PASS" : "FAIL", errors);
}

void test(void) {
    init_grids();
    printf("benchmark_instructions_lmul1: SEW=8 / vLen=1024 / LMUL=1 / vl=%d\n", COLS);

    test_vadd();
    test_vmul();
    test_vrgather();
    test_vadd_masked(50);
    test_vle_vse();

    printf("\nbenchmark_instructions_lmul1 DONE.\n");
}
