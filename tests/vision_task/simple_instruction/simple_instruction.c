#include <stdio.h>
#include <stdint.h>
#include <riscv_vector.h>
#include "emurt.h"


#define ROWS 128
#define COLS 128

int8_t grid_a[ROWS][COLS];
int8_t grid_b[ROWS][COLS];
int8_t output[ROWS][COLS];

void initialise_grids() {
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            grid_a[i][j] = (int8_t)(1 + i + j);
            grid_b[i][j] = (int8_t)(1 + i * 2);
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

void print_grid_address_in_hex(int num_row_to_print, int8_t *grid) {
    printf("Printing addresses of first %d rows of the grid:\n", num_row_to_print);
    for (int i = 0; i < num_row_to_print; i++) {
        for (int j = 0; j < COLS; j++) {
            printf("0x%016lx ", (unsigned long)(uintptr_t)&grid[i * COLS + j]);
        }
        printf("\n");
    }
    if (num_row_to_print < ROWS) {
        printf("...\n");
    }
}

void print_address_range_in_hex(int8_t *start, int8_t *end) {
    if (start == NULL || end == NULL) {
        printf("Address range: null pointer\n");
        return;
    }

    if (start > end) {
        int8_t *tmp = start;
        start = end;
        end = tmp;
    }

    printf("Address range: 0x%016lx to 0x%016lx\n",
           (unsigned long)(uintptr_t)start, (unsigned long)(uintptr_t)end);

    for (int8_t *ptr = start; ptr <= end; ptr++) {
        if (((uintptr_t)(ptr - start) % 16u) == 0u) {
            printf("0x%016lx: ", (unsigned long)(uintptr_t)ptr);
        }
        printf("%02x ", (unsigned int)(uint8_t)(*ptr));
        if (((uintptr_t)(ptr - start) % 16u) == 15u || ptr == end) {
            printf("\n");
        }
    }
}

void grid_vadd(int8_t *a, int8_t *b, int8_t *c, size_t n) {
    size_t vl = __riscv_vsetvl_e8m4(n);
    vint8m4_t va = __riscv_vle8_v_i8m4(a, vl);
    vint8m4_t vb = __riscv_vle8_v_i8m4(b, vl);
    vint8m4_t vc = __riscv_vadd_vv_i8m4(va, vb, vl);
    __riscv_vse8_v_i8m4(c, vc, vl);
    // __riscv_vse8_v_i8m4(c, va, vl);
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

    // printf("Row 0:   ");
    // for (int j = 0; j < 8; j++) printf("%d ", output[0][j]);
    // printf("...\n");

    // printf("Row 64:  ");
    // for (int j = 0; j < 8; j++) printf("%d ", output[64][j]);
    // printf("...\n");

    // printf("Row 127: ");
    // for (int j = 0; j < 8; j++) printf("%d ", output[127][j]);
    // printf("...\n");

    printf("Row 0:   ");
    for (int j = 0; j < 8; j++) printf("%d ", output[0][j]);
    printf("...\n");

    printf("Row 1:  ");
    for (int j = 0; j < 8; j++) printf("%d ", output[1][j]);
    printf("...\n");

    printf("Row 2: ");
    for (int j = 0; j < 8; j++) printf("%d ", output[2][j]);
    printf("...\n");

    printf("Row 3: ");
    for (int j = 0; j < 8; j++) printf("%d ", output[3][j]);
    printf("...\n");

    printf("grid a");
    print_grid(5, &grid_a[0][0]);

    printf("grid b");
    print_grid(5, &grid_b[0][0]);

    printf("output");
    print_grid(5, &output[0][0]);

    printf("output address");
    print_grid_address_in_hex(3, &output[0][0]);

    print_address_range_in_hex(&output[0][0], &output[3][COLS-1]);

    print_address_range_in_hex((int8_t *)(uintptr_t)0x200172a0, (int8_t *)(uintptr_t)0x20017410);
}
