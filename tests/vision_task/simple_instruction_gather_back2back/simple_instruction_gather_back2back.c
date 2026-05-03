#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

// Diagnostic test for vrgather.vv: rearrange vector elements per a per-element
// index vector. The kernel loads one image row of grid_in into v8, the matching
// row of indices_grid into v16, gathers v12[c] = v8[v16[c]], and stores v12.
//
// init writes grid_in[i][j] = (i + j) & 127 and
// indices_grid[i][j] = (i + j + 1) & 127. Both vary in both dimensions so the
// gather actually rearranges in either time-multiplex orientation:
//   horizontal at row r: v8=[r,r+1,...], v16=[r+1,r+2,...]
//     grid_out[r][c] = grid_in[r][(r+c+1)&127] = (2r + c + 1) & 127
//     row 0 prints "1 2 3 ... 127 0"; row 1 prints "3 4 5 ... 1 2"; ...
//   vertical at column c: v8=[c,c+1,...], v16=[c+1,c+2,...]
//     grid_out[e][c] = grid_in[(e+c+1)&127][c] = (e + 2c + 1) & 127
//     row 0 prints "1 3 5 ... 127 1 ..."; row 1 prints "2 4 6 ... 0 2 ..."; ...


#define ROWS 128
#define COLS 128

int8_t grid_in[ROWS][COLS];
int8_t grid_out[ROWS][COLS];
int8_t grid_out2[ROWS][COLS];
int8_t grid_out3[ROWS][COLS];
int8_t indices_grid[ROWS][COLS];

// volatile pointer casts force the compiler to emit scalar byte stores;
// any auto-vectorisation here triggered by compiler optimisations can cause the test to fail 
// as vector hardware does not support the way the compiler is intending to use it for generating the inital grid. 
__attribute__((noinline))
static void init_grid(void) {
    volatile int8_t *p_in  = (volatile int8_t *)&grid_in[0][0];
    volatile int8_t *p_out = (volatile int8_t *)&grid_out[0][0];
    volatile int8_t *p_out2 = (volatile int8_t *)&grid_out2[0][0];
    volatile int8_t *p_out3 = (volatile int8_t *)&grid_out3[0][0];
    volatile int8_t *p_idx = (volatile int8_t *)&indices_grid[0][0];
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            p_in[i * COLS + j]  = (int8_t)((i + j) & (COLS - 1));
            p_out[i * COLS + j] = 0;
            p_out2[i * COLS + j] = 0;
            p_idx[i * COLS + j] = (int8_t)((i + j + 1) & (COLS - 1));
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

// vrgather.vv: load grid_in into v8, load indices into v16, gather into v12,
// store v12 to grid_out. v8/v12/v16 occupy disjoint LMUL=4 register groups
// (vrgather forbids dest/src overlap).
// a0 = src base, a1 = idx base, a2 = dst base, a3 = element count.
__attribute__((naked, noinline))
void grid_gather(int8_t *src, int8_t *idx, int8_t *dst, size_t n) {
    __asm__ volatile (
        "vsetvli zero, a3, e8, m4, ta, ma\n\t"
        "vle8.v  v8,  (a0)\n\t"
        "vle8.v  v16, (a1)\n\t"
        "vrgather.vv v12, v8, v16\n\t"
        "vse8.v  v12, (a2)\n\t"
        "ret\n\t"
    );
}

static inline void set_vertical_mode(int enable) {
    // Custom CSR 0x7c0 is the T1 verticalMode control on t1rocketemu.
    unsigned long v = (unsigned long)enable;
    __asm__ volatile("csrw 0x7c0, %0" :: "r"(v));
}

// Compare grid_out against the closed-form expectation for both modes and report which if any matched.
__attribute__((noinline))
static void check_result(void) {
    int h_mismatch = 0, v_mismatch = 0;
    int h_first_bad_r = -1, h_first_bad_c = -1;
    int v_first_bad_r = -1, v_first_bad_c = -1;
    int8_t h_got = 0, h_exp = 0, v_got = 0, v_exp = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int8_t got = grid_out[r][c];
            int8_t exp_h = (int8_t)((2 * r + c + 1) & (COLS - 1));
            int8_t exp_v = (int8_t)((r + 2 * c + 1) & (COLS - 1));
            if (got != exp_h) {
                if (h_mismatch == 0) {
                    h_first_bad_r = r; h_first_bad_c = c;
                    h_got = got;       h_exp = exp_h;
                }
                h_mismatch++;
            }
            if (got != exp_v) {
                if (v_mismatch == 0) {
                    v_first_bad_r = r; v_first_bad_c = c;
                    v_got = got;       v_exp = exp_v;
                }
                v_mismatch++;
            }
        }
    }
    if (h_mismatch == 0) {
        printf("[CHECK] PASS horizontal: grid_out[r][c] == (2r+c+1)&127 for all %d cells\n",
               ROWS * COLS);
    } else if (v_mismatch == 0) {
        printf("[CHECK] PASS vertical:   grid_out[r][c] == (r+2c+1)&127 for all %d cells\n",
               ROWS * COLS);
    } else {
        printf("[CHECK] FAIL: %d cells mismatch horizontal, %d cells mismatch vertical\n",
               h_mismatch, v_mismatch);
        printf("[CHECK] first H mismatch at [%d][%d]: got %d, expected %d\n",
               h_first_bad_r, h_first_bad_c, h_got, h_exp);
        printf("[CHECK] first V mismatch at [%d][%d]: got %d, expected %d\n",
               v_first_bad_r, v_first_bad_c, v_got, v_exp);
    }
}

// Compare grid_out against the closed-form expectation for both modes and report which if any matched.
__attribute__((noinline))
static void check_result2(void) {
    int h_mismatch = 0, v_mismatch = 0;
    int h_first_bad_r = -1, h_first_bad_c = -1;
    int v_first_bad_r = -1, v_first_bad_c = -1;
    int8_t h_got = 0, h_exp = 0, v_got = 0, v_exp = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int8_t got = grid_out2[r][c];
            int8_t exp_h = (int8_t)((2 * r + c + 1) & (COLS - 1));
            int8_t exp_v = (int8_t)((r + 2 * c + 1) & (COLS - 1));
            if (got != exp_h) {
                if (h_mismatch == 0) {
                    h_first_bad_r = r; h_first_bad_c = c;
                    h_got = got;       h_exp = exp_h;
                }
                h_mismatch++;
            }
            if (got != exp_v) {
                if (v_mismatch == 0) {
                    v_first_bad_r = r; v_first_bad_c = c;
                    v_got = got;       v_exp = exp_v;
                }
                v_mismatch++;
            }
        }
    }
    if (h_mismatch == 0) {
        printf("[CHECK] PASS horizontal: grid_out[r][c] == (2r+c+1)&127 for all %d cells\n",
               ROWS * COLS);
    } else if (v_mismatch == 0) {
        printf("[CHECK] PASS vertical:   grid_out[r][c] == (r+2c+1)&127 for all %d cells\n",
               ROWS * COLS);
    } else {
        printf("[CHECK] FAIL: %d cells mismatch horizontal, %d cells mismatch vertical\n",
               h_mismatch, v_mismatch);
        printf("[CHECK] first H mismatch at [%d][%d]: got %d, expected %d\n",
               h_first_bad_r, h_first_bad_c, h_got, h_exp);
        printf("[CHECK] first V mismatch at [%d][%d]: got %d, expected %d\n",
               v_first_bad_r, v_first_bad_c, v_got, v_exp);
    }
}

__attribute__((noinline))
static void check_grid_result(const int8_t (*grid)[COLS]) {
    int h_mismatch = 0, v_mismatch = 0;
    int h_first_bad_r = -1, h_first_bad_c = -1;
    int v_first_bad_r = -1, v_first_bad_c = -1;
    int8_t h_got = 0, h_exp = 0, v_got = 0, v_exp = 0;

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int8_t got = grid[r][c];
            int8_t exp_h = (int8_t)((2 * r + c + 1) & (COLS - 1));
            int8_t exp_v = (int8_t)((r + 2 * c + 1) & (COLS - 1));
            if (got != exp_h) {
                if (h_mismatch == 0) {
                    h_first_bad_r = r; h_first_bad_c = c;
                    h_got = got;       h_exp = exp_h;
                }
                h_mismatch++;
            }
            if (got != exp_v) {
                if (v_mismatch == 0) {
                    v_first_bad_r = r; v_first_bad_c = c;
                    v_got = got;       v_exp = exp_v;
                }
                v_mismatch++;
            }
        }
    }

    if (h_mismatch == 0) {
        printf("[CHECK] PASS horizontal: grid[r][c] == (2r+c+1)&127 for all %d cells\n",
               ROWS * COLS);
    } else if (v_mismatch == 0) {
        printf("[CHECK] PASS vertical:   grid[r][c] == (r+2c+1)&127 for all %d cells\n",
               ROWS * COLS);
    } else {
        printf("[CHECK] FAIL: %d cells mismatch horizontal, %d cells mismatch vertical\n",
               h_mismatch, v_mismatch);
        printf("[CHECK] first H mismatch at [%d][%d]: got %d, expected %d\n",
               h_first_bad_r, h_first_bad_c, h_got, h_exp);
        printf("[CHECK] first V mismatch at [%d][%d]: got %d, expected %d\n",
               v_first_bad_r, v_first_bad_c, v_got, v_exp);
    }
}

void test(void) {
    init_grid();

    printf("[BEFORE] Print Grid In");
    print_grid(2, &grid_in[0][0]);

    printf("[BEFORE] Print Indices Grid");
    print_grid(2, &indices_grid[0][0]);

    printf("[BEFORE] Print Grid Out");
    print_grid(2, &grid_out[0][0]);    
    
    printf("[BEFORE] Print Grid Out2");
    print_grid(2, &grid_out[0][0]);

    printf("[BEFORE] Print Grid Out3");
    print_grid(2, &grid_out3[0][0]);

    set_vertical_mode(0);
    grid_gather(&grid_in[0][0], &indices_grid[0][0], &grid_out[0][0], COLS);
    set_vertical_mode(1);
    grid_gather(&grid_in[0][0], &indices_grid[0][0], &grid_out2[0][0], COLS);
    set_vertical_mode(0);
    grid_gather(&grid_in[0][0], &indices_grid[0][0], &grid_out3[0][0], COLS);

    printf("[AFTER] Print Grid Out");
    print_grid(4, &grid_out[0][0]);

    printf("[AFTER] Print Grid Out2");
    print_grid(4, &grid_out2[0][0]);

    printf("[AFTER] Print Grid Out3");
    print_grid(4, &grid_out3[0][0]);

    // check_result();
    // check_result2();
    // check_result3();
    check_grid_result(grid_out);
    check_grid_result(grid_out2);
    check_grid_result(grid_out3);
}
