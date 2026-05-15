/*
 * matmul_via_vt - 128x128 matrix multiply C = A * B using the
 * vertical-mode LSU transpose primitive plus a per-column matvec.
 *
 * Architecture levers:
 *
 *   1. Vertical-mode LSU transpose (csrw 0x7c0=1; vle8; csrw 0; vse8).
 *      One single-instruction-pair primitive turns B into B^T = BT in
 *      memory. This is the canonical pattern proved by
 *      tests/vision_task/simple_instruction_vert_lsu/...
 *
 *   2. Per-row vredsum.vs with vl=1 column-strided store. Same
 *      mechanism as Demo 3 (matvec_fc_relu) without bias / ReLU.
 *      With LMUL=4 / SEW=8 / vl=128 each hardware row independently
 *      computes one row's dot product into vd[0]; a vl=1 store with
 *      base &C[0][j] writes hw-row r's result to &C[r][j] using the
 *      LSU's fixed 128-element row pitch.
 *
 * Algorithm:
 *
 *   transpose_kernel(B, BT)             // single LSU round-trip
 *   for j = 0..127:
 *      x_replicated[r][:] = BT[j][:]    // scalar broadcast (volatile)
 *      matvec_col_kernel(A, x_replicated, &C[0][j])
 *
 * The scalar broadcast is the only non-vector work in the inner loop;
 * BT[j][:] is the column j of B. Each hardware row r then sees the
 * same x = column j of B, which is what the matvec needs to give
 * C[r][j].
 *
 * Register budget per kernel:
 *   transpose_kernel:    v8                 (1 group)
 *   matvec_col_kernel:   v8 / v12 / v16 / v20    (4 groups)
 *
 * Verification reproduces the matmul in scalar C using int8-wrap
 * semantics matching the kernel.
 */

#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

#define ROWS 128
#define COLS 128
#define PERF_REG_ADDR 0x10000014

__attribute__((aligned(128)))
int8_t A[ROWS][COLS];

__attribute__((aligned(128)))
int8_t B[ROWS][COLS];

__attribute__((aligned(128)))
int8_t BT[ROWS][COLS];               /* B transposed via vertical-LSU */

__attribute__((aligned(128)))
int8_t x_replicated[ROWS][COLS];     /* rebuilt per outer-loop iteration */

__attribute__((aligned(128)))
int8_t C[ROWS][COLS];                /* output: full 128x128 matrix */

__attribute__((noinline))
static void init_inputs(void) {
    volatile int8_t *pA = (volatile int8_t *)&A[0][0];
    volatile int8_t *pB = (volatile int8_t *)&B[0][0];
    volatile int8_t *pC = (volatile int8_t *)&C[0][0];
    volatile int8_t *pBT = (volatile int8_t *)&BT[0][0];
    volatile int8_t *pXR = (volatile int8_t *)&x_replicated[0][0];

    /* A: small banded values so per-row sum stays in i8 range */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int v = ((r + c) & 7) - 4;       /* -4..3 */
            pA[r * COLS + c] = (int8_t)v;
        }
    }
    /* B: a different small pattern */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int v = ((r * 3 + c * 5) & 7) - 4;  /* -4..3 */
            pB[r * COLS + c] = (int8_t)v;
        }
    }
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            pC[r * COLS + c]  = 0;
            pBT[r * COLS + c] = 0;
            pXR[r * COLS + c] = 0;
        }
    }
}

__attribute__((noinline))
static void replicate_row_to_grid(const int8_t *row128, int8_t *grid_dst) {
    /* grid_dst[r][:] = row128[:] for all r */
    volatile int8_t *p = (volatile int8_t *)grid_dst;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            p[r * COLS + c] = row128[c];
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

/*
 * Transpose kernel: V-load + H-store.
 *
 *   a0 = src (B), a1 = dst (BT), a2 = vl
 */
__attribute__((naked, noinline))
void k_transpose(int8_t *src, int8_t *dst, size_t vl) {
    __asm__ volatile (
        "li      t3, 1                      \n\t"
        "csrw    0x7c0, t3                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v8, (a1)                   \n\t"
        "ret                                \n\t"
    );
}

/*
 * Per-column matvec kernel (no bias, no ReLU).
 *
 *   a0 = &A[0][0]                  A[r][:] in hw-row r
 *   a1 = &x_replicated[0][0]       x[:] in every hw-row (= BT[j][:])
 *   a2 = cols (= 128)
 *   a3 = &C[0][j]                  vl=1 store base; row r writes &C[r][j]
 *   a4 = perf_reg
 *
 * Result: C[r][j] = sum_k A[r][k] * BT[j][k] = sum_k A[r][k] * B[k][j].
 */
__attribute__((naked, noinline))
void k_matvec_col(int8_t *a, int8_t *xrep, size_t cols,
                  int8_t *c_col, uintptr_t perf_reg) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"  /* A[r][:] */
        "vle8.v  v12, (a1)                  \n\t"  /* x[:] (= BT[j][:]) */

        "li      t0, 4                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a4)                  \n\t"  /* perf START tag=4 */

        "vmul.vv v16, v8, v12               \n\t"  /* per-row product, i8 wrap */
        "vmv.v.i v20, 0                     \n\t"
        "vredsum.vs v20, v16, v20           \n\t"  /* per-row sum -> v20[0] */

        "sw      t1, 0(a4)                  \n\t"  /* perf STOP */

        "li      t2, 1                      \n\t"
        "vsetvli zero, t2, e8, m1, ta, ma   \n\t"  /* vl=1 store */
        "vse8.v  v20, (a3)                  \n\t"  /* C[r][j] = result */
        "ret                                \n\t"
    );
}

__attribute__((noinline))
static void check_transpose(void) {
    int errors = 0;
    int br = -1, bc = -1;
    int8_t bgot = 0, bexp = 0;

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int8_t exp = B[c][r];           /* B^T[r][c] = B[c][r] */
            int8_t got = BT[r][c];
            if (got != exp) {
                if (errors == 0) {
                    br = r; bc = c; bgot = got; bexp = exp;
                }
                errors++;
            }
        }
    }
    if (errors == 0) {
        printf("[CHECK] PASS transpose: BT == B^T (%d cells)\n", ROWS * COLS);
    } else {
        printf("[CHECK] FAIL transpose: %d / %d errors; first [%d][%d] got %d exp %d\n",
               errors, ROWS * COLS, br, bc, (int)bgot, (int)bexp);
    }
}

__attribute__((noinline))
static void verify_matmul_sample(void) {
    /* Full-grid scalar verify is 128^3 = 2M multiplies, which hits
     * the t1emu --max-cycles cap at any reasonable budget. Verify a
     * fixed sample of strategic cells instead: 4 corners, the centre,
     * the 4 mid-edges, and 7 PRNG-picked cells = 16 cells. Each
     * verify costs 128 mul-adds (i8 wrap) + 1 compare = trivial.
     */
    static const struct { int r; int c; } sample_cells[] = {
        { 0,   0   }, { 0,   127 }, { 127, 0   }, { 127, 127 },
        { 64,  64  },
        { 0,   64  }, { 127, 64  }, { 64,  0   }, { 64,  127 },
        { 17,  93  }, { 42,  11  }, { 88,  77  }, { 13,  102 },
        { 100, 25  }, { 55,  88  }, { 31,  31  },
    };
    const int N = (int)(sizeof(sample_cells) / sizeof(sample_cells[0]));

    int errors = 0;
    int br = -1, bc = -1;
    int8_t bgot = 0, bexp = 0;

    for (int i = 0; i < N; i++) {
        int r = sample_cells[i].r;
        int c = sample_cells[i].c;
        int8_t acc = 0;
        for (int k = 0; k < COLS; k++) {
            int8_t prod = (int8_t)((int)A[r][k] * (int)B[k][c]);
            acc = (int8_t)((int)acc + (int)prod);
        }
        int8_t got = C[r][c];
        if (got != acc) {
            if (errors == 0) {
                br = r; bc = c; bgot = got; bexp = acc;
            }
            errors++;
        }
    }
    if (errors == 0) {
        printf("[CHECK] PASS matmul_via_vt: all %d sampled cells correct\n", N);
    } else {
        printf("[CHECK] FAIL matmul_via_vt: %d / %d errors; first [%d][%d] got %d exp %d\n",
               errors, N, br, bc, (int)bgot, (int)bexp);
    }
}

void test(void) {
    printf("=== matmul_via_vt: 128x128 C = A * B via V-LSU transpose ===\n");
    init_inputs();

    printf("A sample:\n"); print_grid(2, &A[0][0]);
    printf("B sample:\n"); print_grid(2, &B[0][0]);

    dump_grid("grid_A", &A[0][0]);
    dump_grid("grid_B", &B[0][0]);

    /* Step 1: transpose B via vertical-LSU primitive. */
    printf("--- Step 1: transpose B -> BT (vert-LSU) ---\n");
    k_transpose(&B[0][0], &BT[0][0], (size_t)COLS);
    check_transpose();
    dump_grid("grid_BT", &BT[0][0]);

    /* Step 2: per-column matvec for j = 0..127. */
    printf("--- Step 2: 128 per-column matvecs ---\n");
    for (int j = 0; j < COLS; j++) {
        replicate_row_to_grid(&BT[j][0], &x_replicated[0][0]);
        k_matvec_col(&A[0][0], &x_replicated[0][0],
                     (size_t)COLS, &C[0][j],
                     (uintptr_t)PERF_REG_ADDR);
    }

    printf("C sample:\n"); print_grid(2, &C[0][0]);

    dump_grid("grid_C", &C[0][0]);

    verify_matmul_sample();
    printf("=== matmul_via_vt done ===\n");
}
