/*
 * cnn_digit - End-to-end int8 CNN digit-classification inference on
 * the 2D RVV fabric.
 *
 * Pipeline (per test input):
 *
 *   Stage 1  k_conv_relu_lut   3x3 separable Sobel-like edge filter
 *                              (H Gx + V Gy via mode-flip), |Gx|+|Gy|
 *                              with i8 saturation, ReLU, and a
 *                              vrgather.vv-based 128-byte LUT that
 *                              binarises edges (>= threshold -> 1).
 *
 *   Stage 2  k_score_class     for each class c in 0..9:
 *                              vmul.vv (edges, W_c), per-row vredsum.vs,
 *                              vl=1 store of 128 i8 row-sums to a
 *                              16 KB scratch buffer (LSU row-pitch = 128).
 *                              Scalar tail sums those into i32 logit_c.
 *
 *   Stage 3  k_argmax_vec      load 10 tagged i32 logits at SEW=32
 *                              LMUL=4 vl=10, vredmax.vs, vmv.x.s,
 *                              andi 0xF -> predicted class.
 *
 * Architectural levers exercised:
 *   - vrgather.vv as a 128-byte LUT activation (binariser).
 *   - Two orthogonal conv filters from one image load (H+V mode flip).
 *   - Multiple feature maps live in distinct register groups (Gx in v20,
 *     Gy in v24) so we never spill conv intermediates to memory.
 *   - Per-row vredsum.vs as the FC primitive.
 *   - Vector argmax via index-tagged vredmax (heavy on RVV, scalar tail
 *     is a 4-instruction extract-and-mask).
 *
 * Softmax bypass: argmax (monotonic property) - see fyp_doc/vision_program_demos.md.
 *
 * R-rules respected:
 *   R1 volatile init    R2 naked, no spills, no C calls inside kernels
 *   R4 LMUL=4           R5 disjoint groups for vrgather (v4=src, v20=idx,
 *                       v28=dst - three different LMUL=4 register bases)
 *   R7 mask-mode-deps   no v0 use across modes
 */

#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

#define ROWS 128
#define COLS 128
#define NUM_CLASSES 10
#define LUT_THRESH 24                 /* edge >= this -> 1 in LUT */
#define PERF_REG_ADDR 0x10000014

/* ============================ data ============================ */

__attribute__((aligned(128)))
int8_t input_grid[ROWS][COLS];

__attribute__((aligned(128)))
int8_t lut_grid[ROWS][COLS];          /* lut_grid[r][:] = 128-byte LUT */

__attribute__((aligned(128)))
int8_t edges_grid[ROWS][COLS];

__attribute__((aligned(128)))
int8_t W[NUM_CLASSES][ROWS][COLS];    /* 10 binary class templates */

__attribute__((aligned(128)))
int8_t row_sums_buf[ROWS][COLS];      /* vl=1 store target; only [r][0] meaningful */

__attribute__((aligned(128)))
int32_t tagged_replicated[ROWS * 32]; /* SEW=32 LMUL=4: 32 i32 per hw-row, 128B/row */

int32_t logits[NUM_CLASSES];
int32_t predicted_class;

/* ============================ scalar setup ============================ */

__attribute__((noinline))
static void init_lut_grid(void) {
    /* lut_grid[r][i] = 1 if i >= LUT_THRESH, else 0, for all r.
     * The vrgather instruction will use edges as index into this LUT
     * and produce a binary edge map.
     */
    volatile int8_t *p = (volatile int8_t *)&lut_grid[0][0];
    for (int r = 0; r < ROWS; r++) {
        for (int i = 0; i < COLS; i++) {
            p[r * COLS + i] = (i >= LUT_THRESH) ? (int8_t)1 : (int8_t)0;
        }
    }
}

__attribute__((noinline))
static void clear_grid(int8_t *g) {
    volatile int8_t *p = (volatile int8_t *)g;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            p[r * COLS + c] = 0;
        }
    }
}

/* ============================ digit renderers ============================ */

/* Each renderer fills input_grid with a stylized 7-segment-display-like
 * pattern for a digit. Bright = 80, dark = 0. The Sobel-like conv in
 * Stage 1 will fire at the bright/dark transitions.
 *
 * Coordinate convention: row 0 is top, row 127 is bottom. Column 0 is
 * left, column 127 is right.
 *
 * Drawing primitives use volatile pointers (R1).
 */

#define BRIGHT 80
#define DARK   0

#define DIGIT_TOP    16
#define DIGIT_BOTTOM 112
#define DIGIT_LEFT   16
#define DIGIT_RIGHT  112
#define DIGIT_MID    64
#define BAR_THICK    10

__attribute__((noinline))
static void fill_rect(volatile int8_t *p, int r0, int r1, int c0, int c1, int v) {
    if (r0 < 0) r0 = 0;
    if (c0 < 0) c0 = 0;
    if (r1 > ROWS) r1 = ROWS;
    if (c1 > COLS) c1 = COLS;
    for (int r = r0; r < r1; r++) {
        for (int c = c0; c < c1; c++) {
            p[r * COLS + c] = (int8_t)v;
        }
    }
}

__attribute__((noinline))
static void draw_oval_ring(volatile int8_t *p, int rcenter, int ccenter,
                           int a_outer, int b_outer, int a_inner, int b_inner) {
    /* Cells where (dx/a_outer)^2 + (dy/b_outer)^2 <= 1 AND
     *             (dx/a_inner)^2 + (dy/b_inner)^2 >= 1
     * are bright. */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int dy = r - rcenter;
            int dx = c - ccenter;
            int outer = dx * dx * b_outer * b_outer + dy * dy * a_outer * a_outer;
            int outer_lim = a_outer * a_outer * b_outer * b_outer;
            int inner = dx * dx * b_inner * b_inner + dy * dy * a_inner * a_inner;
            int inner_lim = a_inner * a_inner * b_inner * b_inner;
            if (outer <= outer_lim && inner >= inner_lim) {
                p[r * COLS + c] = (int8_t)BRIGHT;
            }
        }
    }
}

__attribute__((noinline))
static void render_digit(int d) {
    volatile int8_t *p = (volatile int8_t *)&input_grid[0][0];
    /* Clear */
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            p[r * COLS + c] = (int8_t)DARK;

    int top    = DIGIT_TOP;
    int bot    = DIGIT_BOTTOM;
    int left   = DIGIT_LEFT;
    int right  = DIGIT_RIGHT;
    int mid    = DIGIT_MID;
    int bt     = BAR_THICK;

    switch (d) {
    case 0:
        /* Hollow oval */
        draw_oval_ring(p, mid, mid, 48, 40, 36, 30);
        break;
    case 1:
        /* Vertical bar in column band */
        fill_rect(p, top, bot, mid - bt/2, mid + bt/2, BRIGHT);
        break;
    case 2:
        /* Top bar + diagonal + bottom bar */
        fill_rect(p, top,        top + bt,    left, right, BRIGHT);
        fill_rect(p, bot - bt,   bot,         left, right, BRIGHT);
        for (int r = top + bt; r < bot - bt; r++) {
            int c = right - bt - ((r - (top + bt)) * (right - left - bt))
                                  / (bot - bt - top - bt);
            fill_rect(p, r, r + 1, c, c + bt, BRIGHT);
        }
        break;
    case 3:
        /* Three horizontal bars + right edge */
        fill_rect(p, top,         top + bt,    left, right, BRIGHT);
        fill_rect(p, mid - bt/2,  mid + bt/2,  left, right, BRIGHT);
        fill_rect(p, bot - bt,    bot,         left, right, BRIGHT);
        fill_rect(p, top,         bot,         right - bt, right, BRIGHT);
        break;
    case 4:
        /* Left vertical (top half) + middle horizontal + right vertical (full) */
        fill_rect(p, top,         mid + bt/2,  left, left + bt, BRIGHT);
        fill_rect(p, mid - bt/2,  mid + bt/2,  left, right, BRIGHT);
        fill_rect(p, top,         bot,         right - bt, right, BRIGHT);
        break;
    case 5:
        /* Top + left-top + middle + right-bot + bottom */
        fill_rect(p, top,         top + bt,    left, right, BRIGHT);
        fill_rect(p, top,         mid + bt/2,  left, left + bt, BRIGHT);
        fill_rect(p, mid - bt/2,  mid + bt/2,  left, right, BRIGHT);
        fill_rect(p, mid - bt/2,  bot,         right - bt, right, BRIGHT);
        fill_rect(p, bot - bt,    bot,         left, right, BRIGHT);
        break;
    case 6:
        /* Same as 5 plus left-bottom */
        fill_rect(p, top,         top + bt,    left, right, BRIGHT);
        fill_rect(p, top,         bot,         left, left + bt, BRIGHT);
        fill_rect(p, mid - bt/2,  mid + bt/2,  left, right, BRIGHT);
        fill_rect(p, mid - bt/2,  bot,         right - bt, right, BRIGHT);
        fill_rect(p, bot - bt,    bot,         left, right, BRIGHT);
        break;
    case 7:
        /* Top bar + diagonal */
        fill_rect(p, top,         top + bt,    left, right, BRIGHT);
        for (int r = top + bt; r < bot; r++) {
            int c = right - bt - ((r - (top + bt)) * (right - left - bt))
                                  / (bot - top - bt);
            fill_rect(p, r, r + 1, c, c + bt, BRIGHT);
        }
        break;
    case 8:
        /* Hollow oval + middle bar */
        draw_oval_ring(p, mid, mid, 48, 40, 36, 30);
        fill_rect(p, mid - bt/2, mid + bt/2, left + 6, right - 6, BRIGHT);
        break;
    case 9:
        /* Top loop + right vertical (full) + bottom bar */
        draw_oval_ring(p, top + 32, mid, 36, 32, 24, 20);
        fill_rect(p, top + 32,    bot,         right - bt, right, BRIGHT);
        fill_rect(p, bot - bt,    bot,         left, right, BRIGHT);
        break;
    default:
        break;
    }
}

/* ============================ vector kernels ============================ */

/*
 * Stage 1 - k_conv_relu_lut.
 *
 *   a0 = input
 *   a1 = lut_grid     (128 hw-rows of identical 128-byte LUT)
 *   a2 = edges output
 *   a3 = vl  (= 128)
 *   a4 = perf_reg
 *
 * Pipeline:
 *   csrw 0; vle8 input -> v8; vle8 lut -> v4
 *   H mode: Gx via vslidedown(right) - vslideup(left)        -> v20
 *   V mode: Gy via vslidedown(below) - vslideup(above)       -> v24
 *   |Gx| via vrsub(0,Gx)+vmax                                -> v20
 *   |Gy| via vrsub(0,Gy)+vmax                                -> v24
 *   sat: v20 = vsadd v20+v24                                 (i8 sat)
 *   ReLU: v20 = vmax(v20, 0)                                  (in case of -128 wrap)
 *   LUT: v28 = vrgather.vv v4, v20                           (binary edge map)
 *   vse8 v28 -> edges
 */
__attribute__((naked, noinline))
void k_conv_relu_lut(int8_t *input, int8_t *lut, int8_t *edges,
                     size_t vl, uintptr_t perf_reg) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"   /* input */
        "vle8.v  v4,  (a1)                  \n\t"   /* lut */

        "li      t0, 1                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a4)                  \n\t"   /* perf START tag=1 */

        /* H gradient: v20 = right - left */
        "vmv.v.i v12, 0                     \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "vslidedown.vi v12, v8, 1           \n\t"
        "vslideup.vi   v16, v8, 1           \n\t"
        "vsub.vv v20, v12, v16              \n\t"

        /* V gradient: v24 = below - above (mode flip) */
        "li      t3, 1                      \n\t"
        "csrw    0x7c0, t3                  \n\t"
        "vmv.v.i v12, 0                     \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "vslidedown.vi v12, v8, 1           \n\t"
        "vslideup.vi   v16, v8, 1           \n\t"
        "vsub.vv v24, v12, v16              \n\t"
        "csrw    0x7c0, zero                \n\t"

        /* |Gx| via vrsub + vmax */
        "vrsub.vi v12, v20, 0               \n\t"
        "vmax.vv  v20, v20, v12             \n\t"

        /* |Gy| via vrsub + vmax */
        "vrsub.vi v16, v24, 0               \n\t"
        "vmax.vv  v24, v24, v16             \n\t"

        /* magnitude = |Gx| + |Gy|  (i8 saturating add) */
        "vsadd.vv v20, v20, v24             \n\t"

        /* ReLU clamp to [0, 127] (handles the |x|=-128 corner case
         * where vrsub+vmax leaves a negative value through). */
        "vmv.v.i v12, 0                     \n\t"
        "vmax.vv v20, v20, v12              \n\t"

        /* LUT activation: v28[r][c] = v4[r][v20[r][c]] (binary). */
        "vrgather.vv v28, v4, v20           \n\t"

        "sw      t1, 0(a4)                  \n\t"   /* perf STOP */

        "vse8.v  v28, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/*
 * Stage 2 - k_score_class.
 *
 *   a0 = edges base
 *   a1 = W_c base
 *   a2 = row_sums_buf base  (16 KB; only [r*128] meaningful)
 *   a3 = vl (= 128)
 *   a4 = perf_reg
 *
 * Pipeline:
 *   vle8 edges -> v8; vle8 W_c -> v12
 *   vmul.vv v16 = v8 * v12  (i8 wrap; safe because edges in {0,1} and W in {0,1})
 *   vmv.v.i v20, 0
 *   vredsum.vs v20, v16, v20    (v20[0] in hw-row r = row sum)
 *   vsetvli vl=1 m1
 *   vse8 v20 -> row_sums_buf    (one byte per hw-row, strided 128)
 */
__attribute__((naked, noinline))
void k_score_class(int8_t *edges, int8_t *W_c, int8_t *row_sums,
                   size_t vl, uintptr_t perf_reg) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"

        "li      t0, 2                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a4)                  \n\t"   /* perf START tag=2 */

        "vmul.vv v16, v8, v12               \n\t"
        "vmv.v.i v20, 0                     \n\t"
        "vredsum.vs v20, v16, v20           \n\t"

        "sw      t1, 0(a4)                  \n\t"   /* perf STOP */

        "li      t2, 1                      \n\t"
        "vsetvli zero, t2, e8, m1, ta, ma   \n\t"
        "vse8.v  v20, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/*
 * Stage 3 - k_argmax_vec.
 *
 *   a0 = tagged_replicated base (128 hw-rows of 32 i32 each, first 10 valid)
 *   a1 = predicted_class output (i32 store)
 *   a2 = perf_reg
 *
 * Pipeline:
 *   vsetvli vl=10, e32, m4, ta, ma
 *   vle32.v v4, (a0)            (loads 10 tagged i32 logits per hw-row)
 *   vmv.v.i v8, 0               (initial value for vredmax)
 *   vredmax.vs v8, v4, v8       (v8[0] = max-tagged)
 *   vmv.x.s t0, v8              (extract winner)
 *   andi t0, t0, 0xF            (low 4 bits = predicted class)
 *   sw t0, 0(a1)
 */
__attribute__((naked, noinline))
void k_argmax_vec(int32_t *tagged_repl, int32_t *predicted_out,
                  uintptr_t perf_reg) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "li      t3, 10                     \n\t"
        "vsetvli zero, t3, e32, m4, ta, ma  \n\t"
        "vle32.v v4, (a0)                   \n\t"

        "li      t0, 3                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a2)                  \n\t"   /* perf START tag=3 */

        "vmv.v.i v8, 0                      \n\t"
        "vredmax.vs v8, v4, v8              \n\t"

        "sw      t1, 0(a2)                  \n\t"   /* perf STOP */

        "vmv.x.s t0, v8                     \n\t"
        "andi    t0, t0, 0xF                \n\t"
        "sw      t0, 0(a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* ============================ scalar tail ============================ */

__attribute__((noinline))
static int32_t aggregate_logit(void) {
    /* Sum the 128 row-sum bytes into an i32 logit.
     * row_sums_buf[r][0] holds the byte for hw-row r; others are scratch.
     */
    int32_t total = 0;
    for (int r = 0; r < ROWS; r++) {
        total += (int32_t)row_sums_buf[r][0];
    }
    return total;
}

__attribute__((noinline))
static void build_tagged_replicated(void) {
    /* tagged[c] = (logit[c] << 4) | c   - index in low 4 bits.
     * Then replicate the same 10-element pattern to every hw-row's 32-element slot.
     */
    int32_t tagged[NUM_CLASSES];
    for (int c = 0; c < NUM_CLASSES; c++) {
        tagged[c] = (logits[c] << 4) | c;
    }
    /* Pad to 32 i32 per hw-row by zeroing the rest.
     * Zero is < any positive tagged value, so vredmax will ignore them. */
    for (int r = 0; r < ROWS; r++) {
        for (int i = 0; i < 32; i++) {
            tagged_replicated[r * 32 + i] = (i < NUM_CLASSES) ? tagged[i] : 0;
        }
    }
}

__attribute__((noinline))
static void binarize_template(int d) {
    /* W[d][r][c] = 1 if edges_grid[r][c] != 0, else 0 */
    volatile int8_t *p = (volatile int8_t *)&W[d][0][0];
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            p[r * COLS + c] = (edges_grid[r][c] != 0) ? (int8_t)1 : (int8_t)0;
        }
    }
}

__attribute__((noinline))
static int count_nonzero_template(int d) {
    int n = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (W[d][r][c]) n++;
        }
    }
    return n;
}

/* ============================ debug helpers ============================ */

static void print_grid_sample(const char *name, const int8_t *g, int n) {
    printf("%s first %d rows:\n", name, n);
    for (int r = 0; r < n; r++) {
        for (int c = 0; c < COLS; c++) {
            printf("%d ", g[r * COLS + c]);
        }
        printf("\n");
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

/* ============================ test entry ============================ */

void test(void) {
    printf("=== cnn_digit: int8 CNN digit classifier on 2D RVV fabric ===\n");

    /* ----- One-time setup: LUT + 10 weight templates ----- */
    init_lut_grid();

    printf("--- Setup: build 10 binary weight templates from rendered digits ---\n");
    for (int d = 0; d < NUM_CLASSES; d++) {
        clear_grid(&edges_grid[0][0]);
        render_digit(d);
        k_conv_relu_lut(&input_grid[0][0], &lut_grid[0][0], &edges_grid[0][0],
                        (size_t)COLS, (uintptr_t)PERF_REG_ADDR);
        binarize_template(d);
        printf("  template %d: nonzero count = %d\n", d, count_nonzero_template(d));
    }

    /* Skip LUT and template dumps in default builds - they were
     * blowing the simulator's UART log buffer (~22 grids × 16 K chars)
     * and shadowing the per-input PASS/FAIL printouts. Define
     * CNN_DUMP_SETUP_GRIDS at compile time to re-enable. */
#ifdef CNN_DUMP_SETUP_GRIDS
    dump_grid("grid_lut", &lut_grid[0][0]);
    for (int d = 0; d < NUM_CLASSES; d++) {
        printf("[GRID_DUMP_BEGIN] grid_W_%d\n", d);
        for (int r = 0; r < ROWS; r++) {
            for (int c = 0; c < COLS; c++) {
                printf("%d ", W[d][r][c]);
            }
            printf("\n");
        }
        printf("[GRID_DUMP_END] grid_W_%d\n", d);
    }
#endif

    /* ----- Inference: 10 test inputs, one per class ----- */
    int correct = 0;
    for (int d = 0; d < NUM_CLASSES; d++) {
        printf("\n--- INPUT %d: render digit '%d' and infer ---\n", d, d);

        clear_grid(&edges_grid[0][0]);
        render_digit(d);

        /* Stage 1 */
        k_conv_relu_lut(&input_grid[0][0], &lut_grid[0][0], &edges_grid[0][0],
                        (size_t)COLS, (uintptr_t)PERF_REG_ADDR);

        /* Stage 2: per-class scoring */
        for (int c = 0; c < NUM_CLASSES; c++) {
            k_score_class(&edges_grid[0][0], &W[c][0][0], &row_sums_buf[0][0],
                          (size_t)COLS, (uintptr_t)PERF_REG_ADDR);
            logits[c] = aggregate_logit();
        }

        /* Print logits for visibility */
        printf("logits: ");
        for (int c = 0; c < NUM_CLASSES; c++) printf("%d ", (int)logits[c]);
        printf("\n");

        /* Stage 3: vector argmax */
        build_tagged_replicated();
        predicted_class = -1;
        k_argmax_vec(&tagged_replicated[0], &predicted_class,
                     (uintptr_t)PERF_REG_ADDR);

        printf("[CHECK] %s cnn_digit input %d: predicted=%d (expected %d)\n",
               (predicted_class == d) ? "PASS" : "FAIL",
               d, (int)predicted_class, d);
        if (predicted_class == d) correct++;

        /* Per-input grid dumps gated by CNN_DUMP_PER_INPUT - disabled
         * by default to keep the UART log under the simulator buffer
         * limit (~300 K chars). The first 1-2 inputs can still be
         * dumped via CNN_DUMP_FIRST_INPUTS. */
#ifdef CNN_DUMP_PER_INPUT
        printf("[GRID_DUMP_BEGIN] grid_input_%d\n", d);
        for (int r = 0; r < ROWS; r++) {
            for (int c = 0; c < COLS; c++) printf("%d ", input_grid[r][c]);
            printf("\n");
        }
        printf("[GRID_DUMP_END] grid_input_%d\n", d);
        printf("[GRID_DUMP_BEGIN] grid_edges_%d\n", d);
        for (int r = 0; r < ROWS; r++) {
            for (int c = 0; c < COLS; c++) printf("%d ", edges_grid[r][c]);
            printf("\n");
        }
        printf("[GRID_DUMP_END] grid_edges_%d\n", d);
#else
        if (d == 0) {
            /* Always dump just input 0's input + edges so the
             * visualiser has at least one example pipeline frame. */
            printf("[GRID_DUMP_BEGIN] grid_input_%d\n", d);
            for (int r = 0; r < ROWS; r++) {
                for (int c = 0; c < COLS; c++) printf("%d ", input_grid[r][c]);
                printf("\n");
            }
            printf("[GRID_DUMP_END] grid_input_%d\n", d);
            printf("[GRID_DUMP_BEGIN] grid_edges_%d\n", d);
            for (int r = 0; r < ROWS; r++) {
                for (int c = 0; c < COLS; c++) printf("%d ", edges_grid[r][c]);
                printf("\n");
            }
            printf("[GRID_DUMP_END] grid_edges_%d\n", d);
        }
#endif
    }

    if (correct == NUM_CLASSES) {
        printf("\n[CHECK] PASS cnn_digit: %d/%d inputs classified correctly\n",
               correct, NUM_CLASSES);
    } else {
        printf("\n[CHECK] FAIL cnn_digit: %d/%d inputs classified correctly\n",
               correct, NUM_CLASSES);
    }
    printf("=== cnn_digit done ===\n");
    /* spot-check sample */
    print_grid_sample("input_grid", &input_grid[0][0], 2);
    print_grid_sample("edges_grid", &edges_grid[0][0], 2);
}
