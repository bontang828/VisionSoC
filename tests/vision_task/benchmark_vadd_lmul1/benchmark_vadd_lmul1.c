#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

/******************************************************************************
 * benchmark_vadd_lmul1
 *
 * LMUL=1 port of the canonical benchmark_vadd suite (in the same directory's
 * sibling). Targets the "big" config where vLen=1024, baseLMUL=1, and one
 * LMUL=1 register at SEW=8 holds vl_max=128 elements = one 128-pixel image
 * row. The fabric still time-multiplexes 128 hw-rows; only the per-register
 * storage and the visible register-group count changed.
 *
 * Coverage:
 *   TEST 1 - per-iteration vadd cost                (counter tag 1)
 *   TEST 2 - amortised vadd cost                    (counter tag 2)
 *   TEST 3 - saturating brightness vsadd.vx         (counter tag 3)
 *   TEST 4 - 3-tap horizontal blur via vslide       (counter tag 4)
 *   TEST 5 - 3-tap vertical blur (CSR 0x7c0=1)      (counter tag 5)
 *   TEST 9 - 8-way pixel accumulate (LMUL=1-only)   (counter tag 9)
 *
 * The TEST 6/7/8 reduction kernels from the m4 reference are intentionally
 * omitted -- they hit the open per-row vredsum and full-grid sum issues
 * that the 2d_fabric handoff calls out, and they are not what this port is
 * trying to prove.
 *
 * TEST 9 is the new one. At LMUL=4 the legal register-group bases are
 *   v0, v4, v8, v12, v16, v20, v24, v28   (8 groups -> 5 sources + dest max)
 * so an 8-source accumulate doesn't fit. At LMUL=1 every register is its
 * own group, so we can use v8..v15 as 8 distinct image-row buffers and v16
 * as the running sum. This is the kernel that demonstrates the 4x register-
 * group headroom unlocked by the wider VRF.
 *
 * Programmer rules R1-R8 from benchmark_vadd.c carry over verbatim. The
 * inline asm blocks are still naked + noinline + no internal C calls;
 * place_counter() is encoded as a `sw <tag>, 0(<perf_reg>)` inside the
 * asm block, exactly as in the m4 reference.
 *****************************************************************************/

#define ROWS  128
#define COLS  128
#define ITERS 5

#define PERF_REG_ADDR 0x10000014

int8_t grid_a[ROWS][COLS];
int8_t grid_b[ROWS][COLS];
int8_t grid_c[ROWS][COLS];

/* TEST 9: 8 distinct source grids, all summed into one output. */
__attribute__((aligned(128))) int8_t src8[8][ROWS][COLS];

__attribute__((noinline))
static void init_grids(void) {
    volatile int8_t *pa = (volatile int8_t *)&grid_a[0][0];
    volatile int8_t *pb = (volatile int8_t *)&grid_b[0][0];
    volatile int8_t *pc = (volatile int8_t *)&grid_c[0][0];
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            pa[i * COLS + j] = (int8_t)((i + j) & (COLS - 1));
            pb[i * COLS + j] = (int8_t)((i * 2) & (COLS - 1));
            pc[i * COLS + j] = 0;
        }
    }
}

__attribute__((noinline))
static void init_src8(void) {
    // Each plane filled with a distinct constant so the 8-way sum has a
    // predictable expected value at every pixel.
    for (int k = 0; k < 8; k++) {
        volatile int8_t *p = (volatile int8_t *)&src8[k][0][0];
        int8_t v = (int8_t)(k + 1);            // 1, 2, 3, 4, 5, 6, 7, 8
        for (int i = 0; i < ROWS; i++) {
            for (int j = 0; j < COLS; j++) {
                p[i * COLS + j] = v;
            }
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

/*--- vector kernels (naked, no spill, no calls; all m1) ---*/

/* TEST 1: per-iteration counter wrap (matches m4 reference's TEST 1).
 *   a0 = a, a1 = b, a2 = c, a3 = vl, a4 = ITERS, a5 = PERF_REG_ADDR. */
__attribute__((naked, noinline))
static void grid_vadd_per_iter_m1(int8_t *a, int8_t *b, int8_t *c,
                                  size_t vl, int iters, uintptr_t perf_reg) {
    __asm__ volatile (
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v9,  (a1)                  \n\t"
        "li      t0, 1                      \n\t"
        "li      t1, 0                      \n\t"
        "mv      t2, a4                     \n\t"
    "1:                                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
        "vadd.vv v10, v8, v9                \n\t"
        "sw      t1, 0(a5)                  \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "vse8.v  v10, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST 2: single counter wraps the burst. */
__attribute__((naked, noinline))
static void grid_vadd_back2back_m1(int8_t *a, int8_t *b, int8_t *c,
                                   size_t vl, int iters, uintptr_t perf_reg) {
    __asm__ volatile (
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v9,  (a1)                  \n\t"
        "li      t0, 2                      \n\t"
        "li      t1, 0                      \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vadd.vv v10, v8, v9                \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      t1, 0(a5)                  \n\t"
        "vse8.v  v10, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST 3: saturating vsadd.vx brightness boost.
 *   a0 = a, a1 = c, a2 = vl, a3 = boost, a4 = PERF_REG_ADDR. */
__attribute__((naked, noinline))
static void grid_brightness_sat_m1(int8_t *a, int8_t *c, size_t vl,
                                   int boost, uintptr_t perf_reg) {
    __asm__ volatile (
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "li      t0, 3                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a4)                  \n\t"
        "vsadd.vx v9, v8, a3                \n\t"
        "sw      t1, 0(a4)                  \n\t"
        "vse8.v  v9, (a1)                   \n\t"
        "ret                                \n\t"
    );
}

/* TEST 4: 3-tap horizontal box blur.
 * out = (a_left + 2*a + a_right) >> 2, 0-fill at edges. */
__attribute__((naked, noinline))
static void grid_blur3_horiz_m1(int8_t *a, int8_t *c, size_t vl,
                                uintptr_t perf_reg) {
    __asm__ volatile (
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "li      t0, 4                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a3)                  \n\t"
        "vmv.v.i v9,  0                     \n\t"
        "vmv.v.i v10, 0                     \n\t"
        "vslidedown.vi v9,  v8, 1           \n\t"
        "vslideup.vi   v10, v8, 1           \n\t"
        "vadd.vv v11, v8,  v9               \n\t"
        "vadd.vv v11, v11, v10              \n\t"
        "vadd.vv v11, v11, v8               \n\t"
        "vsra.vi v11, v11, 2                \n\t"
        "sw      t1, 0(a3)                  \n\t"
        "vse8.v  v11, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST 5: 3-tap vertical box blur via CSR 0x7c0 = 1.
 * Restores CSR to 0 before ret so downstream kernels see horizontal mode. */
__attribute__((naked, noinline))
static void grid_blur3_vert_m1(int8_t *a, int8_t *c, size_t vl,
                               uintptr_t perf_reg) {
    __asm__ volatile (
        "li      t3, 1                      \n\t"
        "csrw    0x7c0, t3                  \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "li      t0, 5                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a3)                  \n\t"
        "vmv.v.i v9,  0                     \n\t"
        "vmv.v.i v10, 0                     \n\t"
        "vslidedown.vi v9,  v8, 1           \n\t"
        "vslideup.vi   v10, v8, 1           \n\t"
        "vadd.vv v11, v8,  v9               \n\t"
        "vadd.vv v11, v11, v10              \n\t"
        "vadd.vv v11, v11, v8               \n\t"
        "vsra.vi v11, v11, 2                \n\t"
        "sw      t1, 0(a3)                  \n\t"
        "vse8.v  v11, (a1)                  \n\t"
        "csrw    0x7c0, zero                \n\t"
        "ret                                \n\t"
    );
}

/*
 * TEST 9 (LMUL=1-only): 8-way pixel accumulate.
 *
 *   for each pixel (r, c):
 *     grid_c[r][c] = sat_i8(sum_{k=0..7} src8[k][r][c])
 *
 * Uses v8..v15 as 8 source planes and v16 as the accumulator. Cannot be
 * expressed at LMUL=4 in a single naked block because only 8 register
 * groups exist (v0/v4/v8/v12/v16/v20/v24/v28) and one is reserved as the
 * destination; 8 sources + 1 dest = 9 groups needed. At LMUL=1 we have 32
 * groups to work with.
 *
 *   a0..a7 = src8[0..7] base pointers (a-regs 0-7 are the RISC-V ABI's
 *            first 8 integer args, which is exactly what we need to pass
 *            eight pointers from C)
 *   tail args via stack are dodged by using register-only signatures.
 *
 *   We have 8 source pointers but only 8 caller-saved a-regs total, and
 *   a0..a7 ARE those eight. So a8 doesn't exist; we have to load grid_c
 *   pointer and perf_reg + vl from the stack OR use static globals.
 *   Simpler: pass dest/vl/perf via globals.
 */
/* External linkage required: the inline asm's `la <sym>` in grid_acc8_m1
 * references these by symbol name, which means they cannot be `static` (the
 * static qualifier gives internal linkage and the linker drops the symbol).*/
int8_t *g_dest_ptr;
size_t  g_vl;
uintptr_t g_perf_reg;

__attribute__((naked, noinline))
static void grid_acc8_m1(int8_t *s0, int8_t *s1, int8_t *s2, int8_t *s3,
                         int8_t *s4, int8_t *s5, int8_t *s6, int8_t *s7) {
    __asm__ volatile (
        // Load globals via la / lw - all scalar, no vector spill risk.
        "la      t3, g_vl                   \n\t"
        "lw      t3, 0(t3)                  \n\t"
        "vsetvli zero, t3, e8, m1, ta, ma   \n\t"

        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v9,  (a1)                  \n\t"
        "vle8.v  v10, (a2)                  \n\t"
        "vle8.v  v11, (a3)                  \n\t"
        "vle8.v  v12, (a4)                  \n\t"
        "vle8.v  v13, (a5)                  \n\t"
        "vle8.v  v14, (a6)                  \n\t"
        "vle8.v  v15, (a7)                  \n\t"

        "la      t4, g_perf_reg             \n\t"
        "lw      t4, 0(t4)                  \n\t"
        "li      t0, 9                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(t4)                  \n\t"

        // saturating accumulate so 8 planes of i8 don't silently wrap.
        "vsadd.vv v16, v8,  v9              \n\t"
        "vsadd.vv v16, v16, v10             \n\t"
        "vsadd.vv v16, v16, v11             \n\t"
        "vsadd.vv v16, v16, v12             \n\t"
        "vsadd.vv v16, v16, v13             \n\t"
        "vsadd.vv v16, v16, v14             \n\t"
        "vsadd.vv v16, v16, v15             \n\t"

        "sw      t1, 0(t4)                  \n\t"

        "la      t5, g_dest_ptr             \n\t"
        "lw      t5, 0(t5)                  \n\t"
        "vse8.v  v16, (t5)                  \n\t"
        "ret                                \n\t"
    );
}

/*--- C-side test wrappers ---*/

__attribute__((noinline))
static void test1_per_iter(void) {
    printf("\n=== TEST 1 (LMUL=1): per-iter vadd (tag=1) ===\n");
    clear_grid_c();
    grid_vadd_per_iter_m1(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
                          (size_t)COLS, ITERS, (uintptr_t)PERF_REG_ADDR);
    print_first_rows(2, &grid_c[0][0]);
}

__attribute__((noinline))
static void test2_back2back(void) {
    printf("\n=== TEST 2 (LMUL=1): back-to-back vadd (tag=2) ===\n");
    clear_grid_c();
    grid_vadd_back2back_m1(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
                           (size_t)COLS, ITERS, (uintptr_t)PERF_REG_ADDR);
    print_first_rows(2, &grid_c[0][0]);
}

__attribute__((noinline))
static void test3_brightness(int boost) {
    printf("\n=== TEST 3 (LMUL=1): vsadd.vx +%d (tag=3) ===\n", boost);
    clear_grid_c();
    grid_brightness_sat_m1(&grid_a[0][0], &grid_c[0][0],
                           (size_t)COLS, boost, (uintptr_t)PERF_REG_ADDR);
    print_first_rows(2, &grid_c[0][0]);
}

__attribute__((noinline))
static void test4_blur_horiz(void) {
    printf("\n=== TEST 4 (LMUL=1): 3-tap H blur (tag=4) ===\n");
    clear_grid_c();
    grid_blur3_horiz_m1(&grid_a[0][0], &grid_c[0][0],
                        (size_t)COLS, (uintptr_t)PERF_REG_ADDR);
    print_first_rows(2, &grid_c[0][0]);
}

__attribute__((noinline))
static void test5_blur_vert(void) {
    printf("\n=== TEST 5 (LMUL=1): 3-tap V blur, CSR 0x7c0=1 (tag=5) ===\n");
    clear_grid_c();
    grid_blur3_vert_m1(&grid_a[0][0], &grid_c[0][0],
                       (size_t)COLS, (uintptr_t)PERF_REG_ADDR);
    print_first_rows(2, &grid_c[0][0]);
}

__attribute__((noinline))
static void test9_acc8(void) {
    printf("\n=== TEST 9 (LMUL=1 ONLY): 8-way pixel accumulate (tag=9) ===\n");
    init_src8();
    clear_grid_c();

    g_dest_ptr = &grid_c[0][0];
    g_vl       = (size_t)COLS;
    g_perf_reg = (uintptr_t)PERF_REG_ADDR;

    grid_acc8_m1(&src8[0][0][0], &src8[1][0][0], &src8[2][0][0], &src8[3][0][0],
                 &src8[4][0][0], &src8[5][0][0], &src8[6][0][0], &src8[7][0][0]);

    /* Expected: every pixel = 1+2+3+4+5+6+7+8 = 36 (well below i8 sat) */
    int errors = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (grid_c[r][c] != 36) {
                if (errors < 8) {
                    printf("  MISMATCH [%d][%d]: got %d, expected 36\n",
                           r, c, grid_c[r][c]);
                }
                errors++;
            }
        }
    }
    if (errors == 0) {
        printf("[TEST 9] PASS: all %d cells = 36 (8-way sum)\n", ROWS * COLS);
    } else {
        printf("[TEST 9] FAIL: %d mismatches (LMUL=1 register-group headroom not working)\n",
               errors);
    }
    print_first_rows(2, &grid_c[0][0]);
}

void test(void) {
    init_grids();
    printf("benchmark_vadd_lmul1: SEW=8 / vLen=1024 / LMUL=1 / vl=%d\n", COLS);

    test1_per_iter();
    test2_back2back();
    test3_brightness(50);
    test4_blur_horiz();
    test5_blur_vert();
    test9_acc8();

    printf("\nbenchmark_vadd_lmul1 DONE.\n");
}
