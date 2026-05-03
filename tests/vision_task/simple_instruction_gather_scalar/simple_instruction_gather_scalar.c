#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

// Test for vrgather.vx (scalar broadcast index): vd[i] = vs2[rs1] for all i.
// Runs four cases: {horizontal, vertical} x {unmasked, diag-masked}, with rs1=5.
//
// Expected results (grid_in[r][c] = (r+c)&127):
//   H unmasked: grid_out[r][c] = grid_in[r][rs1] = (r+rs1)&127  (depends on r only)
//   V unmasked: grid_out[r][c] = grid_in[rs1][c] = (rs1+c)&127  (depends on c only)
//   H masked  : (r==c) -> grid_in[r][rs1];  else baseline = grid_in[r][c]
//   V masked  : (r==c) -> grid_in[rs1][c];  else baseline = grid_in[r][c]
//                The diagonal cell value coincides under both modes for symmetric
//                grid_in, since grid_in[r][rs1] == grid_in[rs1][r] when r==c.
//
// Conventions follow fyp_doc/2d_fabric_handoff.md:
//   R3.1  init via volatile *p (no auto-vector)
//   R3.2  naked kernel, no C calls inside
//   R3.3  LMUL=4, SEW=8, vl=128
//   R3.4  disjoint LMUL=4 register groups (v8 / v12 / v16 / v20)
//   R3.5  CSR 0x7c0 toggles vertical compute mode (1 = vertical), reset to 0 before vse
//   §4.3  vle/vse always run in horizontal mode regardless of CSR
//   §4.2  diag mask is symmetric so it lands on the same cells in either mode

#define ROWS 128
#define COLS 128
#define RS1  5

int8_t grid_in[ROWS][COLS];
int8_t grid_out[ROWS][COLS];
int8_t diag_buf[ROWS][COLS];

__attribute__((noinline))
static void init_grids(void) {
    volatile int8_t *p_in   = (volatile int8_t *)&grid_in[0][0];
    volatile int8_t *p_out  = (volatile int8_t *)&grid_out[0][0];
    volatile int8_t *p_diag = (volatile int8_t *)&diag_buf[0][0];
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            p_in  [i * COLS + j] = (int8_t)((i + j) & (COLS - 1));
            p_out [i * COLS + j] = 0;
            p_diag[i * COLS + j] = (i == j) ? (int8_t)1 : (int8_t)0;
        }
    }
}

__attribute__((noinline))
static void clear_grid_out(void) {
    volatile int8_t *p_out = (volatile int8_t *)&grid_out[0][0];
    for (int i = 0; i < ROWS; i++)
        for (int j = 0; j < COLS; j++)
            p_out[i * COLS + j] = 0;
}

static void print_grid(int n, int8_t *grid) {
    printf("Printing first %d rows of the grid:\n", n);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < COLS; j++) {
            printf("%d ", grid[i * COLS + j]);
        }
        printf("\n");
    }
    if (n < ROWS) printf("...\n");
}

/* Unmasked vrgather.vx
 * args: src, dst, vl, vert
 *       a0   a1   a2  a3
 */
__attribute__((naked, noinline))
void k_vrgather_vx(int8_t *src, int8_t *dst, size_t vl, int vert) {
    __asm__ volatile (
        "csrw    0x7c0, a3                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "li      t3, 5                      \n\t"  /* RS1 */
        "vrgather.vx v16, v8, t3            \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* Masked vrgather.vx with diag v0.t mask (mu policy preserves baseline off-diag).
 * Build v0 under the same CSR mode in which the gather runs (§4.2). The diag
 * source diag_buf[r][c] = (r==c)?1:0 is symmetric so vmsne.vi gives the same
 * v0 footprint either way.
 *
 * args: src, diag, dst, vl, vert
 *       a0   a1    a2   a3  a4
 */
__attribute__((naked, noinline))
void k_vrgather_vx_mask(int8_t *src, int8_t *diag, int8_t *dst, size_t vl, int vert) {
    __asm__ volatile (
        "csrw    0x7c0, a4                  \n\t"
        "vsetvli zero, a3, e8, m4, ta, mu   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v20, (a1)                  \n\t"
        "vmsne.vi v0, v20, 0                \n\t"
        "vmv.v.v v16, v8                    \n\t"
        "li      t3, 5                      \n\t"  /* RS1 */
        "vrgather.vx v16, v8, t3, v0.t      \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

static const char *mode_str(int vert) { return vert ? "V" : "H"; }

__attribute__((noinline))
static void check_unmasked(int vert) {
    int errors = 0;
    int br = -1, bc = -1;
    int8_t bgot = 0, bexp = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int8_t got = grid_out[r][c];
            int8_t exp = vert
                ? (int8_t)((RS1 + c) & (COLS - 1))   /* V: grid_in[rs1][c] = (rs1+c)&127 */
                : (int8_t)((r + RS1) & (COLS - 1));  /* H: grid_in[r][rs1] = (r+rs1)&127 */
            if (got != exp) {
                if (errors == 0) {
                    br = r; bc = c; bgot = got; bexp = exp;
                }
                errors++;
            }
        }
    }
    if (errors == 0) {
        printf("[CHECK] PASS vrgather.vx (%s) unmasked: grid_out[r][c] == %s\n",
               mode_str(vert),
               vert ? "grid_in[5][c] = (5+c)&127" : "grid_in[r][5] = (r+5)&127");
    } else {
        printf("[CHECK] FAIL vrgather.vx (%s) unmasked: %d / %d errors; "
               "first at [%d][%d] got %d exp %d\n",
               mode_str(vert), errors, ROWS * COLS, br, bc, bgot, bexp);
    }
}

__attribute__((noinline))
static void check_masked(int vert) {
    int errors = 0;
    int br = -1, bc = -1;
    int8_t bgot = 0, bexp = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int8_t got = grid_out[r][c];
            /* Diagonal cell r==c gets vs2[rs1] of that hw-row (H) or vert-lane (V).
             * H: grid_in[r][rs1] = (r+rs1)&127.
             * V: grid_in[rs1][c] = (rs1+c)&127.
             * For r==c these are equal because grid_in is symmetric.
             * Off-diagonal cells keep the vmv.v.v baseline = grid_in[r][c]. */
            int8_t exp = (r == c)
                ? (int8_t)((r + RS1) & (COLS - 1))
                : (int8_t)((r + c)   & (COLS - 1));
            if (got != exp) {
                if (errors == 0) {
                    br = r; bc = c; bgot = got; bexp = exp;
                }
                errors++;
            }
        }
    }
    if (errors == 0) {
        printf("[CHECK] PASS vrgather.vx (%s) masked: diag = grid_in[r][5], "
               "off-diag = grid_in[r][c]\n", mode_str(vert));
    } else {
        printf("[CHECK] FAIL vrgather.vx (%s) masked: %d / %d errors; "
               "first at [%d][%d] got %d exp %d\n",
               mode_str(vert), errors, ROWS * COLS, br, bc, bgot, bexp);
    }
}

void test(void) {
    init_grids();

    printf("=== vrgather.vx test (rs1=%d) ===\n", RS1);
    printf("[INIT] grid_in[r][c] = (r+c)&127\n");
    printf("[INIT] diag_buf[r][c] = (r==c)?1:0\n");

    printf("\n--- TEST 1: vrgather.vx (H, unmasked) ---\n");
    clear_grid_out();
    k_vrgather_vx(&grid_in[0][0], &grid_out[0][0], COLS, /*vert=*/0);
    print_grid(2, &grid_out[0][0]);
    check_unmasked(0);

    printf("\n--- TEST 2: vrgather.vx (V, unmasked) ---\n");
    clear_grid_out();
    k_vrgather_vx(&grid_in[0][0], &grid_out[0][0], COLS, /*vert=*/1);
    print_grid(2, &grid_out[0][0]);
    check_unmasked(1);

    printf("\n--- TEST 3: vrgather.vx (H, diag mask) ---\n");
    clear_grid_out();
    k_vrgather_vx_mask(&grid_in[0][0], &diag_buf[0][0], &grid_out[0][0], COLS, /*vert=*/0);
    print_grid(2, &grid_out[0][0]);
    check_masked(0);

    printf("\n--- TEST 4: vrgather.vx (V, diag mask) ---\n");
    clear_grid_out();
    k_vrgather_vx_mask(&grid_in[0][0], &diag_buf[0][0], &grid_out[0][0], COLS, /*vert=*/1);
    print_grid(2, &grid_out[0][0]);
    check_masked(1);

    printf("\n=== vrgather.vx test done ===\n");
}
