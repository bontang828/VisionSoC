#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

/*
 * Primitive vertical-LSU tests for CSR 0x7c0.
 *
 * Hardware assumptions follow fyp_doc/LSU_vertical_mode_handoff.md:
 *   - SEW=8, LMUL=4, vl=128.
 *   - LSU addresses remain row-major for both modes.
 *   - CSR 0x7c0 only changes the VRF-side layout of vle8.v/vse8.v.
 *
 * Expected effects:
 *   - V-load + H-store: memory receives M^T.
 *   - H-load + V-store: memory receives M^T.
 *   - V-load, CSR flips while load drains, H-store: memory still receives M^T.
 *   - V-load + V-store: transpose twice, memory receives M.
 */

#define ROWS 128
#define COLS 128

uint8_t grid_in[ROWS][COLS];
uint8_t grid_out[ROWS][COLS];

__attribute__((noinline))
static void init_grids(void) {
    volatile uint8_t *p_in = (volatile uint8_t *)&grid_in[0][0];
    volatile uint8_t *p_out = (volatile uint8_t *)&grid_out[0][0];

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            p_in[r * COLS + c] = (uint8_t)(((r * 3) + (c * 5) + (r ^ c)) & 0xff);
            p_out[r * COLS + c] = 0;
        }
    }
}

__attribute__((noinline))
static void clear_grid_out(void) {
    volatile uint8_t *p_out = (volatile uint8_t *)&grid_out[0][0];

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            p_out[r * COLS + c] = 0;
        }
    }
}

static inline uint8_t expected_value(int r, int c, int transpose) {
    return transpose ? grid_in[c][r] : grid_in[r][c];
}

__attribute__((noinline))
static void print_grid_sample(const char *name, uint8_t *grid) {
    printf("%s first 4 rows:\n", name);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < COLS; c++) {
            printf("%u ", (unsigned)grid[r * COLS + c]);
        }
        printf("\n");
    }
}

__attribute__((noinline))
static void check_grid(const char *name, int transpose) {
    int errors = 0;
    int br = -1;
    int bc = -1;
    uint8_t bgot = 0;
    uint8_t bexp = 0;

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            uint8_t got = grid_out[r][c];
            uint8_t exp = expected_value(r, c, transpose);
            if (got != exp) {
                if (errors == 0) {
                    br = r;
                    bc = c;
                    bgot = got;
                    bexp = exp;
                }
                errors++;
            }
        }
    }

    if (errors == 0) {
        printf("[CHECK] PASS %s\n", name);
    } else {
        printf("[CHECK] FAIL %s: %d / %d errors; first at [%d][%d] got %u exp %u\n",
               name, errors, ROWS * COLS, br, bc, (unsigned)bgot, (unsigned)bexp);
    }
}

/* args: src, dst, vl */
__attribute__((naked, noinline))
void k_vload_vert_store_h(uint8_t *src, uint8_t *dst, size_t vl) {
    __asm__ volatile (
        "li      t3, 1                      \n\t"
        "csrw    0x7c0, t3                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v8, (a1)                   \n\t"
        "ret                                \n\t"
    );
}

/* args: src, dst, vl */
__attribute__((naked, noinline))
void k_vload_h_store_vert(uint8_t *src, uint8_t *dst, size_t vl) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "li      t3, 1                      \n\t"
        "csrw    0x7c0, t3                  \n\t"
        "vse8.v  v8, (a1)                   \n\t"
        "csrw    0x7c0, zero                \n\t"
        "ret                                \n\t"
    );
}

/* args: src, dst, vl */
__attribute__((naked, noinline))
void k_vload_vert_flip_store_h(uint8_t *src, uint8_t *dst, size_t vl) {
    __asm__ volatile (
        "li      t3, 1                      \n\t"
        "csrw    0x7c0, t3                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "csrw    0x7c0, zero                \n\t"
        "csrw    0x7c0, t3                  \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v8, (a1)                   \n\t"
        "ret                                \n\t"
    );
}

/* args: src, dst, vl */
__attribute__((naked, noinline))
void k_vload_vert_store_vert(uint8_t *src, uint8_t *dst, size_t vl) {
    __asm__ volatile (
        "li      t3, 1                      \n\t"
        "csrw    0x7c0, t3                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vse8.v  v8, (a1)                   \n\t"
        "csrw    0x7c0, zero                \n\t"
        "ret                                \n\t"
    );
}

void test(void) {
    init_grids();

    printf("=== vertical LSU primitive test ===\n");
    printf("[INIT] grid_in[r][c] = ((r*3) + (c*5) + (r^c)) & 255\n");

    printf("\n--- TEST 1: V-load + H-store => transpose ---\n");
    clear_grid_out();
    k_vload_vert_store_h(&grid_in[0][0], &grid_out[0][0], COLS);
    print_grid_sample("grid_out", &grid_out[0][0]);
    check_grid("V-load + H-store transpose", 1);

    printf("\n--- TEST 2: H-load + V-store => transpose ---\n");
    clear_grid_out();
    k_vload_h_store_vert(&grid_in[0][0], &grid_out[0][0], COLS);
    print_grid_sample("grid_out", &grid_out[0][0]);
    check_grid("H-load + V-store transpose", 1);

    printf("\n--- TEST 3: V-load snapshot survives CSR flips => transpose ---\n");
    clear_grid_out();
    k_vload_vert_flip_store_h(&grid_in[0][0], &grid_out[0][0], COLS);
    print_grid_sample("grid_out", &grid_out[0][0]);
    check_grid("V-load CSR-flip snapshot transpose", 1);

    printf("\n--- TEST 4: V-load + V-store => original ---\n");
    clear_grid_out();
    k_vload_vert_store_vert(&grid_in[0][0], &grid_out[0][0], COLS);
    print_grid_sample("grid_out", &grid_out[0][0]);
    check_grid("V-load + V-store round-trip", 0);

    printf("\n=== vertical LSU primitive test done ===\n");
}
