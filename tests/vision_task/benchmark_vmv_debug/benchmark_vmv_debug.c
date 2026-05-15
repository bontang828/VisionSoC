#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

#define ROWS 128
#define COLS 128
#define ITERS 2
#define PERF_REG_ADDR 0x10000014

#define VL ((size_t)COLS)
#define PERF ((uintptr_t)PERF_REG_ADDR)
#define VMV_X_S_INDEX 5
#define VMV_IMM_VALUE 7

static int8_t grid_b[ROWS][COLS];
static int8_t grid_c[ROWS][COLS];
static int8_t grid_d[ROWS][COLS];

static int g_check_total;
static int g_check_failed;

static inline const char *mode_str(int vert) {
    return vert ? "V" : "H";
}

static inline int8_t b_val(int r, int c) {
    return (int8_t)((r * 2 + c) & (COLS - 1));
}

__attribute__((noinline))
static void record_check_result(int pass) {
    g_check_total++;
    if (!pass) g_check_failed++;
}

__attribute__((noinline))
static void init_grids(void) {
    volatile int8_t *pb = (volatile int8_t *)&grid_b[0][0];
    volatile int8_t *pc = (volatile int8_t *)&grid_c[0][0];
    volatile int8_t *pd = (volatile int8_t *)&grid_d[0][0];
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            pb[r * COLS + c] = b_val(r, c);
            pc[r * COLS + c] = 0;
            pd[r * COLS + c] = 0;
        }
    }
}

__attribute__((noinline))
static void clear_grid_c(void) {
    volatile int8_t *pc = (volatile int8_t *)&grid_c[0][0];
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            pc[r * COLS + c] = 0;
}

__attribute__((noinline))
static void clear_grid_d(void) {
    volatile int8_t *pc = (volatile int8_t *)&grid_d[0][0];
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            pc[r * COLS + c] = 0;
}

static void print_grid(int n, int8_t *grid) {
    printf("Printing first %d rows of the grid:\n", n);
    for (int r = 0; r < n; r++) {
        for (int c = 0; c < COLS; c++) {
            printf("%d ", grid[r * COLS + c]);
        }
        printf("\n");
    }
    if (n < ROWS) printf("...\n");
}

typedef struct {
    int errors;
    int br;
    int bc;
    int8_t bgot;
    int8_t bexp;
} CheckState;

static void cs_init(CheckState *s) {
    s->errors = 0;
    s->br = -1;
    s->bc = -1;
    s->bgot = 0;
    s->bexp = 0;
}

static void cs_record(CheckState *s, int r, int c, int8_t got, int8_t exp) {
    if (got != exp) {
        if (s->errors == 0) {
            s->br = r;
            s->bc = c;
            s->bgot = got;
            s->bexp = exp;
        }
        s->errors++;
    }
}

static void cs_report(CheckState *s, const char *name, const char *mode,
                      const char *detail) {
    record_check_result(s->errors == 0);
    if (s->errors == 0) {
        printf("[CHECK] PASS %s (%s): %s\n", name, mode, detail);
    } else {
        printf("[CHECK] FAIL %s (%s): %d errors; first at [%d][%d] got %d exp %d\n",
               name, mode, s->errors, s->br, s->bc, s->bgot, s->bexp);
    }
}

__attribute__((naked, noinline))
void k_vmv_x_s_after_vmv_vi(int8_t *c, size_t vl, int iters,
                            uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a4                  \n\t"
        "vsetvli zero, a1, e8, m4, ta, ma   \n\t"
        "vmv.v.i v16, 7                     \n\t"
        "mv      t0, a5                     \n\t"
        "mv      t2, a2                     \n\t"
        "sw      t0, 0(a3)                  \n\t"
    "1:                                     \n\t"
        "vmv.x.s t4, v16                    \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a3)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "sb      t4, 0(a0)                  \n\t"
        "ret                                \n\t"
    );
}

__attribute__((naked, noinline))
void k_vslidedown_vi5_store(int8_t *b, int8_t *c, size_t vl, int iters,
                            uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "csrw    0x7c0, a5                  \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vslidedown.vi v16, v8, 5           \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

__attribute__((naked, noinline))
void k_vmv_x_s_after_vslidedown_vi5(int8_t *b, int8_t *c, int8_t *d, size_t vl, int iters,
                                    uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "csrw    0x7c0, a6                  \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vslidedown.vi v16, v8, 5           \n\t"
        "vmv.x.s t4, v16                    \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a5)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "sb      t4, 0(a1)                  \n\t"
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

__attribute__((noinline))
static void check_scalar_only(const char *name, int vert, int8_t ref) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int8_t exp = (r == 0 && c == 0) ? ref : (int8_t)0;
            cs_record(&s, r, c, grid_c[r][c], exp);
        }
    cs_report(&s, name, mode_str(vert), "only grid_c[0][0] receives scalar rd");
}

__attribute__((noinline))
static void check_vslidedown_vi5(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int8_t exp;
            if (vert)
                exp = (r + VMV_X_S_INDEX >= ROWS) ? (int8_t)0 : b_val(r + VMV_X_S_INDEX, c);
            else
                exp = (c + VMV_X_S_INDEX >= COLS) ? (int8_t)0 : b_val(r, c + VMV_X_S_INDEX);
            cs_record(&s, r, c, grid_c[r][c], exp);
        }
    cs_report(&s, "vslidedown.vi 5 setup", mode_str(vert),
              vert ? "grid_c[r][c] == grid_b[r+5][c]" :
                     "grid_c[r][c] == grid_b[r][c+5]");
}

static void run_mode(int vert) {
    int base = vert ? 200 : 100;
    const char *mode = mode_str(vert);

    printf("\n############### %s MODE ###############\n", vert ? "VERTICAL" : "HORIZONTAL");

    // printf("\n--- TEST %d: vmv.x.s after vmv.v.i setup (%s, tag=%d) ---\n",
    //        base + 1, mode, base + 1);
    // clear_grid_c();
    // print_grid(8, &grid_b[0][0]);
    // print_grid(8, &grid_c[0][0]);
    // k_vmv_x_s_after_vmv_vi(&grid_c[0][0], VL, ITERS, PERF, vert, base + 1);
    // print_grid(8, &grid_b[0][0]);
    // print_grid(8, &grid_c[0][0]);
    // check_scalar_only("vmv.x.s after vmv.v.i", vert, (int8_t)VMV_IMM_VALUE);

    // printf("\n--- TEST %d: vslidedown.vi 5 setup store (%s, tag=%d) ---\n",
    //        base + 2, mode, base + 2);
    // clear_grid_c();
    // print_grid(8, &grid_b[0][0]);
    // print_grid(8, &grid_c[0][0]);
    // k_vslidedown_vi5_store(&grid_b[0][0], &grid_c[0][0], VL, ITERS,
    //                        PERF, vert, base + 2);
    // print_grid(8, &grid_b[0][0]);
    // print_grid(8, &grid_c[0][0]);
    // check_vslidedown_vi5(vert);

    printf("\n--- TEST %d: vmv.x.s after vslidedown.vi 5 setup (%s, tag=%d) ---\n",
           base + 3, mode, base + 3);
    clear_grid_c();
    clear_grid_d();
    print_grid(8, &grid_b[0][0]);
    print_grid(8, &grid_c[0][0]);
    print_grid(8, &grid_d[0][0]);
    k_vmv_x_s_after_vslidedown_vi5(&grid_b[0][0], &grid_c[0][0], &grid_d[0][0], VL, ITERS,
                                   PERF, vert, base + 3);
    print_grid(8, &grid_b[0][0]);
    print_grid(8, &grid_c[0][0]);
    print_grid(8, &grid_d[0][0]);
    check_scalar_only("vmv.x.s after vslidedown.vi 5",
                      vert,
                      vert ? b_val(VMV_X_S_INDEX, 0) : b_val(0, VMV_X_S_INDEX));
}

void test(void) {
    printf("=== benchmark_vmv_debug: %d back-to-back iters per test ===\n", ITERS);
    printf("[INIT] grid_b[r][c] = (r*2+c)&127\n");
    init_grids();

    // run_mode(0);
    run_mode(1);

    int passed = g_check_total - g_check_failed;
    printf("\n=== benchmark_vmv_debug summary: %s (%d/%d checks passed, %d failed) ===\n",
           g_check_failed ? "FAIL" : "PASS",
           passed, g_check_total, g_check_failed);
    printf("\n=== benchmark_vmv_debug done ===\n");
}
