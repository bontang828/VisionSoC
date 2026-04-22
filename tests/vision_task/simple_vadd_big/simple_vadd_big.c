#include <stdio.h>
#include <riscv_vector.h>
#include "emurt.h"

//128 rows x 128 cols = full time-multiplex grid at LMUL=4, SEW=8, vLen=256
//Hardware time-multiplexes across 128 virtual rows automatically
#define ROWS 128
#define COLS 128

int8_t grid_a[ROWS][COLS];
int8_t grid_b[ROWS][COLS];
int8_t output[ROWS][COLS];

void initialise_grids() {
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            grid_a[i][j] = (int8_t)(i + j);
            grid_b[i][j] = (int8_t)(i * 2);
        }
    }
}


void grid_vadd(int8_t *a, int8_t *b, int8_t *c, size_t n) {
    size_t vl = __riscv_vsetvl_e8m4(n);
    vint8m4_t va = __riscv_vle8_v_i8m4(a, vl);
    vint8m4_t vb = __riscv_vle8_v_i8m4(b, vl);
    vint8m4_t vc = __riscv_vadd_vv_i8m4(va, vb, vl);
    __riscv_vse8_v_i8m4(c, vc, vl);
}

void test() {
    initialise_grids();

    grid_vadd(&grid_a[0][0], &grid_b[0][0], &output[0][0], COLS);

    //verify: output[i][j] should be (i+j) + (i*2) = 3*i + j
    int errors = 0;
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            int8_t expected = (int8_t)(3 * i + j);
            if (output[i][j] != expected) {
                if (errors < 16) {
                    printf("MISMATCH [%d][%d]: got %d, expected %d\n",
                           i, j, output[i][j], expected);
                }
                errors++;
            }
        }
    }

    if (errors == 0) {
        printf("PASS: all %d elements correct\n", ROWS * COLS);
    } else {
        printf("FAIL: %d mismatches out of %d\n", errors, ROWS * COLS);
    }

    printf("Row 0:   ");
    for (int j = 0; j < 8; j++) printf("%d ", output[0][j]);
    printf("...\n");

    printf("Row 64:  ");
    for (int j = 0; j < 8; j++) printf("%d ", output[64][j]);
    printf("...\n");

    printf("Row 127: ");
    for (int j = 0; j < 8; j++) printf("%d ", output[127][j]);
    printf("...\n");

    printf("Row 0:   ");
    for (int j = 0; j < 8; j++) printf("%d ", output[0][j]);
    printf("...\n");

    printf("Row 1:  ");
    for (int j = 0; j < 8; j++) printf("%d ", output[1][j]);
    printf("...\n");

    printf("Row 2: ");
    for (int j = 0; j < 8; j++) printf("%d ", output[2][j]);
    printf("...\n");

}
