/*
 * matvec_fc_relu - Single fully-connected NN layer:
 *
 *     y[r] = ReLU( sum_{c=0..127} A[r][c] * x[c] + b[r] )    for r = 0..127
 *
 * 128-element input x, 128x128 weight matrix A, 128-element bias b,
 * 128-element output y. All int8.
 *
 * Architecture lever: per-row vredsum.vs with scalar output. With
 * LMUL=4 / SEW=8 / vl=128, one register group is one image row, and
 * vredsum.vs writes the row's dot product into vd[0] of each hardware
 * row. A subsequent vl=1 store places y[r] at &grid[r][0] using the
 * LSU's fixed 128-element row pitch.
 *
 * Memory layout follows TEST 9 of benchmark_vadd.c:
 *   - x_replicated[r][:] = x[:]   so vle8 loads x into every hw-row.
 *   - b_replicated[r][:] = b[r]   so vle8 loads b[r] into v24[0] of hw-row r.
 *
 * Register budget (LMUL=4 groups):
 *   v8  = A row r
 *   v12 = x replicated
 *   v16 = A * x_replicated (elementwise product, per-row)
 *   v20 = vredsum result, bias-added, ReLU'd
 *   v24 = b replicated
 *   v28 = zero constant for ReLU
 *
 * Verification reproduces y[r] = ReLU(int8_wrap(A.x) + b[r]) in scalar C.
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
int8_t x_replicated[ROWS][COLS];

__attribute__((aligned(128)))
int8_t b_replicated[ROWS][COLS];

__attribute__((aligned(128)))
int8_t y_out[ROWS][COLS];           /* only column 0 used */

int8_t x_vec[COLS];
int8_t b_vec[ROWS];

__attribute__((noinline))
static void init_inputs(void) {
    volatile int8_t *pA = (volatile int8_t *)&A[0][0];
    volatile int8_t *pX = (volatile int8_t *)&x_vec[0];
    volatile int8_t *pB = (volatile int8_t *)&b_vec[0];
    volatile int8_t *pY = (volatile int8_t *)&y_out[0][0];

    /* A: a small banded structure so the matvec is non-trivial */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int diag = (r - c);
            int v = (diag >= -2 && diag <= 2) ? (3 - (diag < 0 ? -diag : diag))
                                              : 0;
            /* +/- alternate to avoid every row summing to a constant */
            if ((r ^ c) & 1) v = -v;
            pA[r * COLS + c] = (int8_t)v;
        }
    }

    /* x: small ramp (-32..31) so x.A can stay near i8 range */
    for (int c = 0; c < COLS; c++) {
        pX[c] = (int8_t)((c & 0x3f) - 32);
    }

    /* b: another small pattern */
    for (int r = 0; r < ROWS; r++) {
        pB[r] = (int8_t)(((r * 5) & 0x1f) - 16);
    }

    /* clear output */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            pY[r * COLS + c] = 0;
        }
    }
}

__attribute__((noinline))
static void replicate_x(void) {
    /* x_replicated[r][:] = x_vec[:] for all r */
    volatile int8_t *p = (volatile int8_t *)&x_replicated[0][0];
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            p[r * COLS + c] = x_vec[c];
        }
    }
}

__attribute__((noinline))
static void replicate_b(void) {
    /* b_replicated[r][c] = b_vec[r] for all c */
    volatile int8_t *p = (volatile int8_t *)&b_replicated[0][0];
    for (int r = 0; r < ROWS; r++) {
        int8_t br = b_vec[r];
        for (int c = 0; c < COLS; c++) {
            p[r * COLS + c] = br;
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
 * a0 = &A[0][0]
 * a1 = &x_replicated[0][0]
 * a2 = &b_replicated[0][0]
 * a3 = cols (= 128)
 * a4 = &y_out[0][0]            (rows write [r][0] only via vl=1 store)
 * a5 = perf_reg
 *
 * Note: b_replicated only matters at element [0] of the bias-add register,
 * because the final vse8 uses vl=1, m1 to write only element [0] per hw-row.
 */
__attribute__((naked, noinline))
void k_matvec_relu(int8_t *a, int8_t *xrep, int8_t *brep,
                   size_t cols, int8_t *y, uintptr_t perf_reg) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"  /* A[r][:] in hw-row r */
        "vle8.v  v12, (a1)                  \n\t"  /* x[:] in every hw-row */
        "vle8.v  v24, (a2)                  \n\t"  /* b[r] in every elem of hw-row r */

        "li      t0, 3                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a5)                  \n\t"  /* perf START tag=3 */

        "vmul.vv v16, v8,  v12              \n\t"  /* A[r][c] * x[c]; i8 wrap */
        "vmv.v.i v20, 0                     \n\t"
        "vredsum.vs v20, v16, v20           \n\t"  /* per-row sum -> v20[0] */

        "vadd.vv v20, v20, v24              \n\t"  /* bias-add; only [0] matters */

        "vmv.v.i v28, 0                     \n\t"
        "vmax.vv v20, v20, v28              \n\t"  /* ReLU = max(., 0) */

        "sw      t1, 0(a5)                  \n\t"  /* perf STOP */

        "li      t2, 1                      \n\t"
        "vsetvli zero, t2, e8, m1, ta, ma   \n\t"  /* vl=1 store */
        "vse8.v  v20, (a4)                  \n\t"  /* y_out[r][0] = result */
        "ret                                \n\t"
    );
}

__attribute__((noinline))
static void verify(void) {
    int errors = 0;
    int br_idx = -1;
    int8_t bgot = 0, bexp = 0;

    for (int r = 0; r < ROWS; r++) {
        /* Reproduce the kernel's int8-wrap arithmetic exactly */
        int8_t acc = 0;
        for (int c = 0; c < COLS; c++) {
            int8_t prod = (int8_t)((int)A[r][c] * (int)x_vec[c]);  /* i8 wrap */
            acc = (int8_t)((int)acc + (int)prod);                   /* i8 wrap */
        }
        int8_t with_bias = (int8_t)((int)acc + (int)b_vec[r]);     /* i8 wrap */
        int8_t exp = (with_bias > 0) ? with_bias : 0;              /* ReLU */

        int8_t got = y_out[r][0];
        if (got != exp) {
            if (errors == 0) {
                br_idx = r;
                bgot = got;
                bexp = exp;
            }
            errors++;
        }
    }

    if (errors == 0) {
        printf("[CHECK] PASS matvec_fc_relu: all %d rows correct\n", ROWS);
    } else {
        printf("[CHECK] FAIL matvec_fc_relu: %d / %d errors; first at row %d got %d exp %d\n",
               errors, ROWS, br_idx, (int)bgot, (int)bexp);
    }
}

void test(void) {
    printf("=== matvec_fc_relu: y = ReLU(A.x + b), 128x128 weights ===\n");
    init_inputs();
    replicate_x();
    replicate_b();

    printf("A sample:\n");
    print_grid(3, &A[0][0]);

    printf("x: ");
    for (int c = 0; c < 16; c++) printf("%d ", x_vec[c]);
    printf("...\n");

    printf("b: ");
    for (int r = 0; r < 16; r++) printf("%d ", b_vec[r]);
    printf("...\n");

    /* Dump A, x_replicated, b_replicated as 'inputs'; y_out (col 0 only)
     * is what the visualiser focuses on for the output. */
    dump_grid("grid_A", &A[0][0]);
    dump_grid("grid_x_replicated", &x_replicated[0][0]);
    dump_grid("grid_b_replicated", &b_replicated[0][0]);

    k_matvec_relu(&A[0][0], &x_replicated[0][0], &b_replicated[0][0],
                  (size_t)COLS, &y_out[0][0], (uintptr_t)PERF_REG_ADDR);

    printf("y[0..15]: ");
    for (int r = 0; r < 16; r++) printf("%d ", y_out[r][0]);
    printf("\n");

    /* Output dump: full y_out grid (only column 0 holds the result) */
    dump_grid("grid_y_out", &y_out[0][0]);

    verify();
    printf("=== matvec_fc_relu done ===\n");
}
