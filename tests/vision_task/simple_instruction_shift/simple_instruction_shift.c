#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

// Expected MemoryWrite events (use rtl_event.jsonl as truth, printf needs
// T1_MIRROR_RTL_WRITES=1 to reflect RTL writes back to scalar memory):
//   horizontal (default):
//     time-multiplex fans the slideup across all 128 rows independently. Each
//     row gets shifted in element-space (a row): col 0 = 0 (from v12 init),
//     cols 1..127 = grid_in[r][0..126] = r+1. So grid_out row r prints:
//       "0 (r+1) (r+1) (r+1) ... (r+1)"
//     First 4 rows: "0 1 1 ... 1", "0 2 2 ... 2", "0 3 3 ... 3", "0 4 4 ... 4".
//   vertical (--vertical):
//     each column gets shifted in row-space (a column): row 0 = 0, rows 1..127
//     of column c = grid_in[0..126][c] = 1..127. So grid_out row r prints
//     constant value r (row 0 = all 0s, row 1 = all 1s, row 2 = all 2s, ...).
//     First 4 rows: "0 0 0 ... 0", "1 1 1 ... 1", "2 2 2 ... 2", "3 3 3 ... 3".

#define ROWS 128
#define COLS 128

int8_t grid_in[ROWS][COLS];
int8_t grid_out[ROWS][COLS];

// volatile pointer casts force the compiler to emit scalar byte stores;
// any auto-vectorisation here triggers the time-multiplex fanout and burns
// the whole cycle budget before the kernel runs.
__attribute__((noinline))
static void init_grid(void) {
    volatile int8_t *p_in  = (volatile int8_t *)&grid_in[0][0];
    volatile int8_t *p_out = (volatile int8_t *)&grid_out[0][0];
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            p_in[i * COLS + j]  = (int8_t)(i + 1);
            p_out[i * COLS + j] = 0;
        }
    }
}

void print_grid(int num_row_to_print, int8_t *grid) {
    printf("Printing first %d rows of the grid:\n", num_row_to_print);
    for (int i = 0; i < num_row_to_print; i++) {
        for (int j = 0; j < COLS; j++) {
            printf("%d ", grid[i * COLS + j]);
        }
        printf("\n");
    }
    if (num_row_to_print < ROWS) {
        printf("...\n");
    }
}

// Slideup-by-1: zero v12 first so the unchanged element[0] reads as 0, load
// grid_in into v8, slide v8 up by 1 into v12, store v12 to grid_out. 
// a0 = src base, a1 = dst base, a2 = element count
__attribute__((naked, noinline))
void grid_shift(int8_t *src, int8_t *dst, size_t n) {
    __asm__ volatile (
        "vsetvli zero, a2, e8, m4, ta, ma\n\t"
        "vmv.v.i v12, 0\n\t"
        "vle8.v v8, (a0)\n\t"
        "vslideup.vi v12, v8, 1\n\t"
        "vse8.v v12, (a1)\n\t"
        "ret\n\t"
    );
}

void test(void) {
    init_grid();

    printf("[BEFORE] Print Grid In");
    print_grid(4, &grid_in[0][0]);

    printf("[BEFORE] Print Grid Out");
    print_grid(4, &grid_out[0][0]);

    grid_shift(&grid_in[0][0], &grid_out[0][0], COLS);

    printf("[AFTER] Print Grid In");
    print_grid(4, &grid_in[0][0]);

    printf("[AFTER] Print Grid Out");
    print_grid(4, &grid_out[0][0]);
}
