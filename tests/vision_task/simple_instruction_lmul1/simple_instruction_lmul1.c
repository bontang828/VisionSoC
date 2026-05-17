#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

/******************************************************************************
 * simple_instruction_lmul1
 *
 * Counterpart of simple_instruction_asm but targeting the LMUL=1 baseline on
 * the "big" config (mudkip2d128big1bram1chain2lanescale*). With vLen=1024
 * and SEW=8, vsetvli ..., e8, m1 gives vl_max = 1024*1/8 = 128 = one full
 * 128-pixel image row in a single LMUL=1 register.
 *
 * Key contract differences from the LMUL=4 reference kernels:
 *   - vsetvli uses `m1` instead of `m4`.
 *   - Register groups are 1 register each: any vN is a legal "base" (no
 *     v0/v4/v8/v12/... alignment constraint that LMUL=4 imposes).
 *   - There are 32 visible groups (one per architectural register) instead
 *     of 8 - the headroom this test demonstrates is exercised more
 *     aggressively by benchmark_vadd_lmul1's 8-way accumulate.
 *
 * The kernel itself is a per-row vsub (matches simple_instruction_asm)
 * so a passing run proves: (a) the basic LMUL=1 issue path works through
 * vsetvli/vle8/vsub/vse8, (b) memory layout is preserved (LSU still uses
 * logicalRowElements=128), (c) Spike difftest agrees at vLen=1024.
 *****************************************************************************/

#define ROWS 128
#define COLS 128

int8_t grid_a[ROWS][COLS];
int8_t grid_b[ROWS][COLS];
int8_t output[ROWS][COLS];

static void initialise_grids(void) {
    // Defeat -O2 auto-vectorisation: write through volatile so the compiler
    // doesn't turn this into LMUL=8 vid.v + vse8.v (which would scribble
    // ROWS copies of 0..vl-1 instead of the values we want).
    volatile int8_t *pa = (volatile int8_t *)&grid_a[0][0];
    volatile int8_t *pb = (volatile int8_t *)&grid_b[0][0];
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            pa[i * COLS + j] = (int8_t)6;
            pb[i * COLS + j] = (int8_t)4;
        }
    }
}

// a0 = grid_a base, a1 = grid_b base, a2 = grid_c base, a3 = element count.
// LMUL=1 / SEW=8 / vl=COLS=128: one register holds one full image row, and
// the time-multiplex grid replays this asm across 128 hw-rows.
__attribute__((naked, noinline))
static void grid_vsub_lmul1(int8_t *a, int8_t *b, int8_t *c, size_t n) {
    __asm__ volatile (
        "csrw   0x7c0, zero\n\t"                 // horizontal mode (R8)
        "vsetvli zero, a3, e8, m1, ta, ma\n\t"   // <-- m1 vs the m4 reference
        "vle8.v  v8,  (a0)\n\t"
        "vle8.v  v9,  (a1)\n\t"                  // any vN is legal at LMUL=1
        "vsub.vv v10, v8, v9\n\t"
        "vse8.v  v10, (a2)\n\t"
        "ret\n\t"
    );
}

void test(void) {
    initialise_grids();

    grid_vsub_lmul1(&grid_a[0][0], &grid_b[0][0], &output[0][0], COLS);

    // Expect output[r][c] = 6 - 4 = 2 for every cell in the grid.
    int errors = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int8_t expected = (int8_t)2;
            if (output[r][c] != expected) {
                if (errors < 16) {
                    printf("MISMATCH [%d][%d]: got %d, expected %d\n",
                           r, c, output[r][c], expected);
                }
                errors++;
            }
        }
    }
    if (errors == 0) {
        printf("PASS: lmul1 grid_vsub correct (%d x %d cells)\n", ROWS, COLS);
    } else {
        printf("FAIL: %d mismatches\n", errors);
    }

    // Sample first row for human eyeballing.
    printf("output row 0:");
    for (int c = 0; c < 8; c++) printf(" %d", output[0][c]);
    printf(" ...\n");
}
