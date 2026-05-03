#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

// Minimal vertical-mode round-trip test: vle8 followed by vse8.
// On t1rocketemu, vertical mode is toggled per-instruction via the Rocket CSR
// at 0x7c0 (plumbed to T1's verticalMode input). On t1emu there is no DUT-side
// scalar CSR file, so the csrw below is executed by Spike and does NOT touch
// the RTL - use `./run-test.sh ... --vertical` (plusarg path) instead for that target.

#define ROWS 128
#define COLS 128

int8_t grid_in[ROWS][COLS];
int8_t grid_vert[ROWS][COLS];
int8_t grid_hori[ROWS][COLS];
int8_t grid_hori2[ROWS][COLS];


static void init_grid(void) {
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            grid_in[i][j]  = (int8_t)5;
            grid_vert[i][j] = 0;
            grid_hori[i][j] = 0;
            grid_hori2[i][j] = 0;
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

// Single vle8.v / vse8.v pair. SEW=8, LMUL=4, vl=128 matches the 128-wide
// grid row (and, under vertical mode, a full column's worth of bytes spread
// across v8..v11 by the scatter fabric).
// a0 = src base, a1 = dst base, a2 = element count
// __attribute__((naked, noinline))
// void vcopy(int8_t *src, int8_t *dst, size_t n) {
//     __asm__ volatile(
//         "vsetvli zero, a2, e8, m4, ta, ma\n\t"
//         "vle8.v v8, (a0)\n\t"
//         "vse8.v v8, (a1)\n\t"
//         "ret\n\t"
//     );
// }

__attribute__((naked, noinline))
void grid_shift(int8_t *src, int8_t *dst,  size_t n) {
    __asm__ volatile (
        "vsetvli zero, a2, e8, m4, ta, ma\n\t"
        "vle8.v  v8,  (a0)\n\t"
        "vslideup.vi v12, v8, 1\n\t"
        "vse8.v  v12,  (a1)\n\t"
        "ret\n\t"
    );
}

// __attribute__((naked, noinline))
// void grid_vadd(int8_t *a, int8_t *b, int8_t *c, size_t n) {
//     __asm__ volatile (
//         "vsetvli zero, a3, e8, m4, ta, ma\n\t"
//         "vle8.v  v8,  (a0)\n\t"
//         "vle8.v  v12, (a1)\n\t"
//         "vsub.vv v8,  v8, v12\n\t"
//         "vse8.v  v8,  (a2)\n\t"
//         "ret\n\t"
//     );
// }

// __attribute__((naked, noinline))
// void grid_shift(int8_t *src, int8_t *dst,  size_t n) {
//     __asm__ volatile (
//         "vsetvli zero, a2, e8, m4, ta, ma\n\t"
//         "vle8.v  v8,  (a0)\n\t"
//         "vsub.vv v8,  v8, v8\n\t"
//         "vse8.v  v8,  (a1)\n\t"
//         "ret\n\t"
//     );
// }


static inline void set_vertical_mode(int enable) {
    // Custom CSR 0x7c0 is the T1 verticalMode control on t1rocketemu.
    unsigned long v = (unsigned long)enable;
    __asm__ volatile("csrw 0x7c0, %0" :: "r"(v) : "memory");
}

void test(void) {
    init_grid();

    printf("[BEFORE] Print Grid In");
    print_grid(3, &grid_in[0][0]);

    printf("[BEFORE] Print Grid Vert");
    print_grid(3, &grid_vert[0][0]);

    printf("[BEFORE] Print Grid Hori");
    print_grid(3, &grid_hori[0][0]);

    printf("[BEFORE] Print Grid Hori2");
    print_grid(3, &grid_hori2[0][0]);

    

    printf("=== Running horizontal-mode shift ===\n");
    set_vertical_mode(0);
    grid_shift(&grid_in[0][0], &grid_hori[0][0], COLS);

    printf("=== Running vertical-mode shift ===\n");
    set_vertical_mode(1);
    grid_shift(&grid_in[0][0], &grid_vert[0][0], COLS);

    printf("=== Running horizontal-mode shift ===\n");
    set_vertical_mode(0);
    grid_shift(&grid_in[0][0], &grid_hori2[0][0], COLS);



    printf("[AFTER] Print Grid In");
    print_grid(3, &grid_in[0][0]);

    printf("[AFTER] Print Grid Hori");
    print_grid(3, &grid_hori[0][0]);

    printf("[AFTER] Print Grid Vert");
    print_grid(3, &grid_vert[0][0]);

    printf("[AFTER] Print Grid Hori2");
    print_grid(3, &grid_hori2[0][0]);

}
