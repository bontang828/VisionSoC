#include <stdio.h>
#include <stdint.h>
#include "emurt.h"


#define ROWS 128
#define COLS 128

int8_t grid_a[ROWS][COLS];
int8_t grid_b[ROWS][COLS];
int8_t output[ROWS][COLS];

void initialise_grids() {
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            // grid_a[i][j] = (int8_t)(1 + i + j);
            // grid_b[i][j] = (int8_t)(1 + i * 2);
            grid_a[i][j] = (int8_t)6;
            grid_b[i][j] = (int8_t)4;

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



// assembly version of grid_vadd, this makes sures there are not stack spill/fill instructions that the compiler makes in the backgroud, 
// as the stack allocation does not have the correct assumption of how big a grid is
// a0 = grid_a base
// a1 = grid_b base
// a2 = grid_c base
// a3 = n element counts
__attribute__((naked, noinline))
void grid_vadd(int8_t *a, int8_t *b, int8_t *c, size_t n) {
    __asm__ volatile (
        "vsetvli zero, a3, e8, m4, ta, ma\n\t"
        "vle8.v  v8,  (a0)\n\t"
        "vle8.v  v12, (a1)\n\t"
        "vsub.vv v8,  v8, v12\n\t"
        "vse8.v  v8,  (a2)\n\t"
        "ret\n\t"
    );
}

void test() {
    initialise_grids();

    printf("Row 0:  ");
    for (int j = 0; j < 8; j++) printf("%d ", output[0][j]);
    printf("...\n");

    printf("Row 1:  ");
    for (int j = 0; j < 8; j++) printf("%d ", output[1][j]);
    printf("...\n");

    printf("Row 2:  ");
    for (int j = 0; j < 8; j++) printf("%d ", output[2][j]);
    printf("...\n");

    printf("grid a");
    print_grid(5, &grid_a[0][0]);

    printf("grid b");
    print_grid(5, &grid_b[0][0]);

    printf("output");
    print_grid(5, &output[0][0]);


    grid_vadd(&grid_a[0][0], &grid_b[0][0], &output[0][0], COLS);

    //verify: with grid_a[i][j] = 1+i+j and grid_b[i][j] = 1+i*2,
    //expected[i][j] = (int8_t)(2 + 3*i + j). grid_vadd only fills row 0.
    int errors = 0;
    for (int j = 0; j < COLS; j++) {
        int8_t expected = (int8_t)(2 + j);
        if (output[0][j] != expected) {
            if (errors < 16) {
                printf("MISMATCH [0][%d]: got %d, expected %d\n",
                       j, output[0][j], expected);
            }
            errors++;
        }
    }

    if (errors == 0) {
        printf("PASS: row 0 correct (%d elements)\n", COLS);
    } else {
        printf("FAIL: %d mismatches in row 0\n", errors);
    }

    printf("Row 0:  ");
    for (int j = 0; j < 8; j++) printf("%d ", output[0][j]);
    printf("...\n");

    printf("Row 1:  ");
    for (int j = 0; j < 8; j++) printf("%d ", output[1][j]);
    printf("...\n");

    printf("Row 2:  ");
    for (int j = 0; j < 8; j++) printf("%d ", output[2][j]);
    printf("...\n");

    printf("grid a");
    print_grid(5, &grid_a[0][0]);

    printf("grid b");
    print_grid(5, &grid_b[0][0]);

    printf("output");
    print_grid(5, &output[0][0]);

    printf("output address");
    print_grid_address_in_hex(5, &output[0][0]);

    print_address_range_in_hex(&output[0][0], &output[2][COLS-1]);


    print_address_range_in_hex((int8_t *)(uintptr_t)0x200172a0, (int8_t *)(uintptr_t)0x20017410); //debug
}