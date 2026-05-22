/*
 * optical_flow - 5-direction integer-pixel block-matching optical flow on
 * the 2D RVV fabric.
 *
 * Per pixel: compute SAD against 5 candidate displacements of `prev`
 * (static, +1 col / -1 col / +1 row / -1 row). The argmin selects the
 * winning direction class in {0, 1, 2, 3, 4}. A small static deadband forces
 * argmin back to 0 when SAD_static <= 6. Output = argmin * 50 -> a
 * 5-level grayscale motion-direction map.
 *
 * Architecture levers:
 *   - H/V mode flip (CSR 0x7c0): horizontal-axis vs vertical-axis slides
 *     on `prev`. Same trick Sobel uses for Gy.
 *   - vrsub + vmax for |x| (no vabs in base RVV).
 *   - vmsltu.vv + vminu.vv + vmerge.vim: unsigned argmin chain across
 *     candidates.
 *   - vmul.vx with rs1 = 50 for the visualisation scaling.
 *
 * Test image: `curr[r][c] = (16*r + c) & 127`.
 * `prev[r][c] = (16*(r+1) + c) & 127`,
 * i.e. curr shifted UP by 1 row in image space. So shifted-down(prev) ==
 * curr at every interior pixel -> argmin = 3 ("down") and output = 150
 * at all interior cells. Boundaries differ because vslide fills the
 * shifted-in slot with 0.
 *
 * Register budget for the big config (LMUL=1, vLen=1024). v0 mask; v4 prev;
 * v8 curr; v12 work; v16 abs-neg; v20 min_sad; v24 argmin.
 *
 * See fyp_doc/vision_program_demos.md and fyp_doc/2d_fabric_handoff.md.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "emurt.h"

#define ROWS 128
#define COLS 128
#define PERF_REG_ADDR 0x10000014

#define DIR_STATIC 0
#define DIR_RIGHT  1
#define DIR_LEFT   2
#define DIR_DOWN   3
#define DIR_UP     4
#define DIR_SCALE  50
#define NOISE_THRESHOLD 6

int8_t grid_curr[ROWS][COLS];
int8_t grid_prev[ROWS][COLS];
int8_t grid_prev_ref[ROWS][COLS];
int8_t grid_out[ROWS][COLS];

__attribute__((noinline))
static void init_grids(void) {
    /* Volatile-pointer pattern defeats -O2 auto-vectorisation per R3.1 of
     * 2d_fabric_handoff. The compiler must NOT turn this into a vle/vse
     * pair across hw rows. */
    volatile int8_t *p_curr = (volatile int8_t *)&grid_curr[0][0];
    volatile int8_t *p_prev = (volatile int8_t *)&grid_prev[0][0];
    volatile int8_t *p_prev_ref = (volatile int8_t *)&grid_prev_ref[0][0];
    volatile int8_t *p_out  = (volatile int8_t *)&grid_out[0][0];

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            /* curr[r][c] = (16*r + c) & 127. The 16*r weight breaks the H/V
             * symmetry that (r+c)&127 has (where right-shifted prev and
             * down-shifted prev both give SAD=0; the kernel's first-wins
             * tiebreak then picks "right"). With (16*r+c), only the "down"
             * candidate hits SAD=0 at interior pixels, and SAD_static is
             * comfortably above the noise threshold. */
            p_curr[r * COLS + c] = (int8_t)((16 * r + c) & 127);
            /* prev[r][c] = curr shifted UP by 1 row in image space, so
             * prev_shifted_down (= prev[r-1][c]) matches curr exactly:
             *   prev_shifted_down[r][c] = prev[r-1][c]
             *                           = (16*((r-1)+1) + c) & 127
             *                           = (16*r + c) & 127
             *                           = curr[r][c]
             * Expected interior: argmin = 3 (down), output = 150.
             */
            int8_t prev = (int8_t)((16 * (r + 1) + c) & 127);
            p_prev[r * COLS + c] = prev;
            p_prev_ref[r * COLS + c] = prev;
            p_out [r * COLS + c] = 0;
        }
    }
}

static void print_grid(int n, int8_t *g) {
    printf("First %d rows (showing 16 cols):\n", n);
    for (int r = 0; r < n; r++) {
        for (int c = 0; c < 16; c++) {
            printf("%d ", g[r * COLS + c]);
        }
        printf("...\n");
    }
    if (n < ROWS) printf("...\n");
}

static void dump_grid(const char *name, const int8_t *g) {
    printf("[GRID_DUMP_BEGIN] %s\n", name);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            printf("%d ", g[r * COLS + c]);
        }
        printf("\n");
    }
    printf("[GRID_DUMP_END] %s\n", name);
}

/* a0 = curr, a1 = prev, a2 = dst, a3 = vl, a4 = perf_reg.
 * The kernel needs an extra scalar register holding the constant 50 for
 * vmul.vx; load it into t0 inside the naked block (since we're naked
 * we don't disturb the C ABI's argument registers). */
__attribute__((naked, noinline))
void k_optical_flow(int8_t *curr, int8_t *prev, int8_t *dst,
                    size_t vl, uintptr_t perf_reg) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"   /* LMUL=1, big config */
        "li      t0, 50                     \n\t"   /* vmul.vx scaling */
        "li      t1, 1                      \n\t"   /* perf START tag */
        "li      t2, 0                      \n\t"   /* perf STOP tag */
        "sw      t1, 0(a4)                  \n\t"   /* perf START */

        /* ---- Load curr and prev ---- */
        "vle8.v        v8,  (a0)            \n\t"
        "vle8.v        v4,  (a1)            \n\t"

        /* ---- SAD_static = |curr - prev| -> v20; argmin = 0 ---- */
        "vsub.vv       v12, v8,  v4         \n\t"
        "vrsub.vi      v16, v12, 0          \n\t"
        "vmax.vv       v20, v12, v16        \n\t"
        "vmv.v.i       v24, 0               \n\t"

        /* ---- Candidate right (object moved right) ----
         * Must zero v12 before vslideup: the prefix lane v12[0] is
         * UNDISTURBED per the RVV spec; without zeroing it would retain
         * the previous candidate's SAD value and pollute the boundary. */
        "vmv.v.i       v12, 0               \n\t"
        "vslideup.vi   v12, v4,  1          \n\t"   /* prev[c-1] */
        "vsub.vv       v12, v8,  v12        \n\t"
        "vrsub.vi      v16, v12, 0          \n\t"
        "vmax.vv       v12, v12, v16        \n\t"
        "vmsltu.vv     v0,  v12, v20        \n\t"
        "vminu.vv      v20, v12, v20        \n\t"
        "vmerge.vim    v24, v24, 1, v0      \n\t"

        /* ---- Candidate left (object moved left) ---- */
        "vmv.v.i       v12, 0               \n\t"
        "vslidedown.vi v12, v4,  1          \n\t"   /* prev[c+1] */
        "vsub.vv       v12, v8,  v12        \n\t"
        "vrsub.vi      v16, v12, 0          \n\t"
        "vmax.vv       v12, v12, v16        \n\t"
        "vmsltu.vv     v0,  v12, v20        \n\t"
        "vminu.vv      v20, v12, v20        \n\t"
        "vmerge.vim    v24, v24, 2, v0      \n\t"

        /* ---- Candidate down (object moved down) ----
         * Sobel's V-mode pass idiom: zero-init + slide + sub + abs ALL
         * in V mode (so v12's V-write is consumed by V-reads, not
         * H-reads that would transpose the data). Switch back to H mode
         * only for the compare/merge against the H-mode accumulators. */
        "li      t3, 1                      \n\t"
        "csrw    0x7c0, t3                  \n\t"   /* V mode */
        "vmv.v.i       v12, 0               \n\t"   /* V-mode zero */
        "vslideup.vi   v12, v4,  1          \n\t"   /* V-mode: prev[r-1] */
        "vsub.vv       v12, v8,  v12        \n\t"   /* V-mode diff_down */
        "vrsub.vi      v16, v12, 0          \n\t"   /* V-mode -diff */
        "vmax.vv       v12, v12, v16        \n\t"   /* V-mode SAD_down */
        "csrw    0x7c0, zero                \n\t"   /* back to H mode */
        "vmsltu.vv     v0,  v12, v20        \n\t"
        "vminu.vv      v20, v12, v20        \n\t"
        "vmerge.vim    v24, v24, 3, v0      \n\t"

        /* ---- Candidate up (object moved up) ---- */
        "csrw    0x7c0, t3                  \n\t"   /* V mode */
        "vmv.v.i       v12, 0               \n\t"   /* V-mode zero */
        "vslidedown.vi v12, v4,  1          \n\t"   /* V-mode: prev[r+1] */
        "vsub.vv       v12, v8,  v12        \n\t"   /* V-mode diff_up */
        "vrsub.vi      v16, v12, 0          \n\t"   /* V-mode -diff */
        "vmax.vv       v12, v12, v16        \n\t"   /* V-mode SAD_up */
        "csrw    0x7c0, zero                \n\t"   /* back to H mode */
        "vmsltu.vv     v0,  v12, v20        \n\t"
        "vminu.vv      v20, v12, v20        \n\t"
        "vmerge.vim    v24, v24, 4, v0      \n\t"

        /* ---- Noise deadband ----
         * If curr is already close to prev, force static. This suppresses
         * small camera-noise wins in static parts of the image. */
        "vsub.vv       v28, v8,  v4         \n\t"   /* recompute diff_static */
        "vrsub.vi      v16, v28, 0          \n\t"
        "vmax.vv       v28, v28, v16        \n\t"   /* SAD_static */
        "vmsleu.vi     v0,  v28, 6          \n\t"   /* SAD_static <= threshold */
        "vmerge.vim    v24, v24, 0, v0      \n\t"   /* force static */

        /* ---- Scale argmin x 50 for visualisation ---- */
        "vmul.vx       v24, v24, t0         \n\t"

        "sw      t2, 0(a4)                  \n\t"   /* perf STOP */

        /* ---- Stores: motion map -> (a2); refresh prev <- curr ---- */
        "vse8.v        v24, (a2)            \n\t"
        "vse8.v        v8,  (a1)            \n\t"
        "ret                                \n\t"
    );
}

static inline int sat_u8_for_motion(int v) {
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return v;
}

/* Unsigned absolute difference of two int8 values, as the kernel computes
 * it via vsub + vrsub + vmax. For the inputs we use here, |diff| fits in
 * [0..127] except for the rare INT8_MIN corner -- avoid that with
 * unsigned semantics. */
static inline uint8_t abs_diff(int8_t a, int8_t b) {
    int diff = (int)(int8_t)((int)a - (int)b);   /* match i8-wrap subtract */
    if (diff < 0) diff = -diff;
    return (uint8_t)diff;
}

/* Boundary-aware shifted lookup: vslide fills the shifted-in lane with
 * 0 (the TA tail policy is mask-agnostic and zero-fill is the standard
 * slide behaviour for these instructions). */
static inline int8_t shifted_prev(int dr, int dc, int r, int c) {
    int rr = r + dr;
    int cc = c + dc;
    if (rr < 0 || rr >= ROWS || cc < 0 || cc >= COLS) {
        return 0;
    }
    return grid_prev_ref[rr][cc];
}

__attribute__((noinline))
static void verify(void) {
    int errors = 0;
    int br = -1, bc = -1;
    uint8_t bgot = 0, bexp = 0;

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int8_t cur_val = grid_curr[r][c];

            /* Replay the kernel: SAD_static is the initial min_sad and
             * argmin = 0 (static). For each directional candidate, the
             * kernel uses vmsltu (strict less-than): a candidate must be
             * STRICTLY less than the current min_sad to win. Ties go to
             * the earlier candidate (static beats right beats left beats
             * down beats up). */
            uint8_t sad[5];
            sad[DIR_STATIC] = abs_diff(cur_val, grid_prev_ref[r][c]);
            /* "moved right" -> compare to prev[r][c-1] (vslideup.vi H mode) */
            sad[DIR_RIGHT]  = abs_diff(cur_val, shifted_prev(0, -1, r, c));
            /* "moved left"  -> compare to prev[r][c+1] (vslidedown.vi H) */
            sad[DIR_LEFT]   = abs_diff(cur_val, shifted_prev(0, +1, r, c));
            /* "moved down"  -> compare to prev[r-1][c] (V-mode vslideup) */
            sad[DIR_DOWN]   = abs_diff(cur_val, shifted_prev(-1, 0, r, c));
            /* "moved up"    -> compare to prev[r+1][c] (V-mode vslidedown) */
            sad[DIR_UP]     = abs_diff(cur_val, shifted_prev(+1, 0, r, c));

            int argmin = DIR_STATIC;
            uint8_t mn = sad[DIR_STATIC];
            if (sad[DIR_RIGHT] < mn) { mn = sad[DIR_RIGHT]; argmin = DIR_RIGHT; }
            if (sad[DIR_LEFT]  < mn) { mn = sad[DIR_LEFT];  argmin = DIR_LEFT;  }
            if (sad[DIR_DOWN]  < mn) { mn = sad[DIR_DOWN];  argmin = DIR_DOWN;  }
            if (sad[DIR_UP]    < mn) { mn = sad[DIR_UP];    argmin = DIR_UP;    }
            if (sad[DIR_STATIC] <= NOISE_THRESHOLD) {
                argmin = DIR_STATIC;
            }

            /* vmul.vx is signed multiply with i8 wrap. argmin in [0..4],
             * scale 50 -> result in [0, 50, 100, 150, 200] all in i8
             * range. No wrap. */
            int expected = argmin * DIR_SCALE;
            uint8_t exp_u = (uint8_t)expected;
            uint8_t got_u = (uint8_t)grid_out[r][c];

            if (got_u != exp_u) {
                if (errors == 0) {
                    br = r; bc = c; bgot = got_u; bexp = exp_u;
                }
                errors++;
            }
        }
    }

    if (errors == 0) {
        printf("[CHECK] PASS optical_flow: all %d cells correct\n",
               ROWS * COLS);
    } else {
        printf("[CHECK] FAIL optical_flow: %d / %d errors; "
               "first at [%d][%d] got %u exp %u\n",
               errors, ROWS * COLS, br, bc, bgot, bexp);
    }
}

void test(void) {
    printf("=== optical_flow: 5-direction integer-pixel block matching ===\n");
    init_grids();

    printf("Input curr (sample):\n");
    print_grid(3, &grid_curr[0][0]);
    printf("Input prev (sample):\n");
    print_grid(3, &grid_prev[0][0]);

    dump_grid("grid_curr", &grid_curr[0][0]);
    dump_grid("grid_prev", &grid_prev[0][0]);

    k_optical_flow(&grid_curr[0][0], &grid_prev[0][0], &grid_out[0][0],
                   (size_t)COLS, (uintptr_t)PERF_REG_ADDR);

    printf("Output (motion-direction map, sample):\n");
    print_grid(3, &grid_out[0][0]);

    dump_grid("grid_out", &grid_out[0][0]);

    verify();
    printf("=== optical_flow done ===\n");
}
