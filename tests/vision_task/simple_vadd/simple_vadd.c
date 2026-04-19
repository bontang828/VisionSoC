#include <stdio.h>
#include <riscv_vector.h>
#include "emurt.h"


#define ROWS 8
#define COLS 8
#define GRID_NUM 4

int8_t grids[GRID_NUM][ROWS][COLS];
int8_t output[ROWS][COLS];

void initialise_grids() {
    for (int g = 0; g < GRID_NUM; g++) {
        for (int i = 0; i < ROWS; i++) {
            for (int j = 0; j < COLS; j++) {
                grids[g][i][j] = g*ROWS*COLS + i*COLS + j;
            }
        }
    }
}

void grid_vadd(int8_t *a, int8_t *b, int8_t *c, size_t vl) {
    for (int i = 0; i < COLS; i += vl) {
        place_counter(1);
        vint8m1_t va = __riscv_vle8_v_i8m1(a + i, vl);

        place_counter(2);
        vint8m1_t vb = __riscv_vle8_v_i8m1(b + i, vl);

        place_counter(3);
        vint8m1_t vc = __riscv_vadd_vv_i8m1(va, vb, vl);

        place_counter(4);
        __riscv_vse8_v_i8m1(c + i, vc, vl);
        
        place_counter(0);
    }
}


void print_grid(int8_t *grid) {
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            printf("%d ", grid[i*COLS + j]);
        }
        printf("\n");
    }
}

void test() {
    //initialise the grids with some values
    initialise_grids();

    size_t vl = __riscv_vsetvl_e8m1(COLS);

    grid_vadd(&grids[0][0][0], &grids[1][0][0], &output[0][0], vl);

    printf("RVV vl:\n");
    printf("  vl = %zu\n", vl);

    printf("Grid A:\n");
    print_grid(&grids[0][0][0]);
    printf("Grid B:\n");
    print_grid(&grids[1][0][0]);
    printf("vadd result:\n");
    print_grid(&output[0][0]);

}




