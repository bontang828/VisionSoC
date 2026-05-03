#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

/*****************************************************************************
 * VisionSoC instruction benchmark suite
 *
 * Measures back-to-back execution cost of a fixed set of RVV instructions
 * under both horizontal and vertical compute modes (CSR 0x7c0). For each
 * (instruction, mode) pair we run ITERS=100 invocations inside a single
 * (place_counter START, ..., place_counter STOP) span tagged with a unique
 * counter id. The simulator emits the start/stop cycle pair to run.log;
 * plot_bench_cycles.py parses those lines and renders a grouped H-vs-V
 * bar chart.
 *
 * Coding restrictions follow benchmark_vadd.c (R1..R8). Briefly:
 *   - Init/check code uses scalar volatile-pointer access so -O2 does not
 *     auto-vectorise it (R1).
 *   - Each measured kernel is __attribute__((naked, noinline)) and contains
 *     a single inline-asm block; it never calls another C function. The
 *     place_counter MMIO write is encoded as `sw` inside the asm so the
 *     compiler cannot insert a vector-register spill across the call
 *     boundary (R2/R3).
 *   - LMUL=4 with disjoint v8 / v12 / v16 / v20 register groups (R4/R5).
 *   - SEW=8, vl=128. One image row per LMUL=4 register group; 128 hw-rows
 *     handle the 128 image-rows in parallel.
 *
 * Vertical mode strategy (see fyp_doc/2d_fabric_handoff.md § 2 and § 4.3)
 * ----------------------------------------------------------------------
 * Each kernel takes a runtime `vert` flag (0 = horizontal, 1 = vertical).
 * The kernel writes that flag to CSR 0x7c0 before the timed loop and
 * resets to 0 afterwards. The mode is latched per-instruction, so all
 * compute ops in the loop see the requested mode. Memory ops (vle8.v /
 * vse8.v) are gated to horizontal regardless of the CSR per § 4.3 - the
 * V-mode LSU tests still run in horizontal-LSU today; they exist so the
 * benchmark works unchanged once vertical-LSU support lands. Until then,
 * V-LSU cycles are expected to match H-LSU cycles exactly and the data
 * checker is the same.
 *
 * Result correctness across modes
 * -------------------------------
 * grid_a[r][c] = (r+c) & 127 is symmetric in r,c. Many of the V-mode
 * results consequently coincide with the H-mode results in row-major
 * memory:
 *   - vadd.vv / vmul.vv / vmv.v.v       same memory layout (element-wise
 *                                        ops applied to symmetric inputs;
 *                                        the V-mode scatter cancels on
 *                                        round-trip with H-mode LSU; § R8).
 *   - vrgather.vx with diagonal v0.t    diagonal element on the diagonal
 *                                        (r==c) coincides under both modes
 *                                        because grid_a[r][rs1] equals
 *                                        grid_a[rs1][c] when r==c.
 *   - vmv.s.x / vredsum.vs              both touch element 0 of the
 *                                        "first" lane (hw-row 0 in H,
 *                                        vert-lane 0 in V), which is
 *                                        the same physical byte
 *                                        grid_c[0][0]; symmetric grid_a
 *                                        also makes sum(row 0) == sum(col 0).
 * The instructions whose V-mode result genuinely differs from H-mode in
 * row-major memory are vrgather.vx (broadcast direction flips), vrgather.vv
 * (gather happens within a vertical lane = column in H view), and the
 * vslide{up,down} family (slide direction flips column<->row). Those have
 * dedicated V-mode checkers below.
 *
 * Things that are not "stock RVV" here (refer to fyp_doc/2d_fabric_handoff.md)
 *   - vredsum.vs and vmv.s.x are scalar-output ops; per § 4.1, only one
 *     of the 128 hw-rows actually receives the write (hw-row 0 in H mode,
 *     vert-lane 0 in V mode = the same memory cell). The corresponding
 *     check functions only inspect grid_c[0][0].
 *   - vrgather.vx with v0.t mask: v0 is read through the same VRF scatter
 *     as any operand (§ 4.2 - mask is mode-dependent). We construct v0
 *     under the same mode in which it is consumed by loading a pre-baked
 *     diag_buf[r][c] = (r==c)?1:0 byte array via vle8.v (always horizontal
 *     LSU, so the bytes land in v20 row-major), then converting with
 *     vmsne.vi v0, v20, 0 in the active mode. The diagonal is symmetric
 *     under transpose so the resulting v0 picks the same cells in either
 *     mode.
 *
 * Counter tag map (must match INSTRUCTION_ORDER in plot_bench_cycles.py)
 * Tags are assigned positionally - slot i within a mode block is tag
 * i + (mode-base), where mode-base = 0 for H and N_PER_MODE for V; the
 * compute block precedes the LSU block within each mode. With the
 * default N_COMPUTE=14 / N_LSU=4 that lands at:
 *   1..14  H compute (vadd, vmul, vand, vor, vrgather.vx, vrgather.vx mask,
 *          vrgather.vv, vmv.s.x, vmv.v.v, vredsum.vs, vslideup.vx 1,
 *          vslideup.vx 100, vslidedown.vx 1, vslidedown.vx 100)
 *  15..18  H LSU (vle.full, vle.masked, vse.full, vse.masked)
 *  19..32  V compute (same order)
 *  33..36  V LSU (same order)
 * Adding an instruction is documented in the N_COMPUTE block below.
 *****************************************************************************/

#define ROWS 128
#define COLS 128
#define ITERS 5
#define PERF_REG_ADDR 0x10000014

#define VRGATHER_VX_INDEX  5    /* scalar source-element index for vrgather.vx */
#define VMV_S_X_VALUE     42    /* scalar value moved to vd[0] for vmv.s.x */
#define VSLIDE_SMALL       1
#define VSLIDE_BIG       100

/* Counter-tag layout. Each instruction has a fixed slot index; H tags are
 * 1..N_PER_MODE, V tags are N_PER_MODE+1..2*N_PER_MODE. The compute block
 * occupies the first N_COMPUTE slots, the LSU block the remaining N_LSU.
 *
 * Adding an instruction:
 *   1. Bump N_COMPUTE (or N_LSU) below by 1.
 *   2. Insert the new test in run_compute_block() / run_lsu_block() at the
 *      slot index you want (1-indexed within the block).
 *   3. Add the matching label at the same position in INSTRUCTION_ORDER
 *      in plot_bench_cycles.py.
 * The Python side then picks up the new bar automatically.
 */
#define N_COMPUTE   14
#define N_LSU        4
#define N_PER_MODE  (N_COMPUTE + N_LSU)

int8_t grid_a[ROWS][COLS];
int8_t grid_b[ROWS][COLS];
int8_t grid_c[ROWS][COLS];
int8_t idx_grid[ROWS][COLS];     /* indices for vrgather.vv */
int8_t diag_buf[ROWS][COLS];     /* (r == c) ? 1 : 0; mask source for TEST 4/20 */

__attribute__((noinline))
static void init_grids(void) {
    volatile int8_t *pa = (volatile int8_t *)&grid_a[0][0];
    volatile int8_t *pb = (volatile int8_t *)&grid_b[0][0];
    volatile int8_t *pc = (volatile int8_t *)&grid_c[0][0];
    volatile int8_t *pi = (volatile int8_t *)&idx_grid[0][0];
    volatile int8_t *pd = (volatile int8_t *)&diag_buf[0][0];
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            pa[i * COLS + j] = (int8_t)((i + j) & (COLS - 1));
            pb[i * COLS + j] = (int8_t)((i * 2 + j) & (COLS - 1));
            pc[i * COLS + j] = 0;
            pi[i * COLS + j] = (int8_t)((i + j + 1) & (COLS - 1));
            pd[i * COLS + j] = (i == j) ? (int8_t)1 : (int8_t)0;
        }
    }
}

__attribute__((noinline))
static void clear_grid_c(void) {
    volatile int8_t *pc = (volatile int8_t *)&grid_c[0][0];
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            pc[i * COLS + j] = 0;
        }
    }
}

static void print_grid(int n, int8_t *grid) {
    printf("Printing first %d rows of the grid:\n", n);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < COLS; j++) {
            printf("%d ", grid[i * COLS + j]);
        }
        printf("\n");
    }
    if (n < ROWS) printf("...\n");
}

/*============================================================================
 * Naked kernels. Each one takes a runtime `vert` flag (0 = H, 1 = V) and a
 * `tag` value used as the place_counter MMIO start tag. Common shape:
 *
 *   csrw 0x7c0, <vert>
 *   vsetvli zero, <vl>, e8, m4, ta, <ma|mu>
 *   <pre-loop setup>
 *   sw <tag>, 0(<perf>)               # counter START
 * 1:
 *   <single instruction under test>
 *   addi <iter_ctr>, -1
 *   bnez <iter_ctr>, 1b
 *   sw zero, 0(<perf>)                # counter STOP
 *   csrw 0x7c0, zero                  # restore H mode for next test
 *   <vse for verification, if any>
 *   ret
 *==========================================================================*/

/* TEST {1,17} - vadd.vv
 * args: a, b, c, vl, iters, perf, vert, tag
 *       a0 a1 a2 a3   a4    a5    a6    a7
 */
__attribute__((naked, noinline))
void k_vadd(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
            uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a6                  \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vadd.vv v16, v8, v12               \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a5)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {2,N_PER_MODE+2} - vmul.vv */
__attribute__((naked, noinline))
void k_vmul(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
            uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a6                  \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vmul.vv v16, v8, v12               \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a5)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {3,N_PER_MODE+3} - vand.vv (bitwise AND, element-wise) */
__attribute__((naked, noinline))
void k_vand(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
            uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a6                  \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vand.vv v16, v8, v12               \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a5)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {4,N_PER_MODE+4} - vor.vv (bitwise OR, element-wise) */
__attribute__((naked, noinline))
void k_vor(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
           uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a6                  \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vor.vv  v16, v8, v12               \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a5)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {3,19} - vrgather.vx (broadcast vs2[rs1] to all elements)
 * args: a, c, vl, iters, perf, vert, tag
 *       a0 a1 a2 a3    a4    a5    a6
 */
__attribute__((naked, noinline))
void k_vrgather_vx(int8_t *a, int8_t *c, size_t vl, int iters,
                   uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "li      t3, 5                      \n\t"  /* VRGATHER_VX_INDEX */
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vrgather.vx v16, v8, t3            \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {4,20} - vrgather.vx with diagonal v0.t mask (mu policy)
 * args: a, diag, c, vl, iters, perf, vert, tag
 *       a0    a1 a2 a3   a4    a5    a6    a7
 *
 * Diagonal mask: vle8.v v20, (diag_buf) lands diag_buf[r][c] in v20[c] of
 * hw-row r. vmsne.vi v0, v20, 0 then yields v0[c] in hw-row r = (r==c).
 * vmv.v.v v16, v8 sets the dst baseline to grid_a so mu keeps off-diag
 * cells equal to the input, while the timed vrgather.vx writes the
 * scalar source v8[rs1] onto the diagonal of every iteration.
 */
__attribute__((naked, noinline))
void k_vrgather_vx_mask(int8_t *a, int8_t *diag, int8_t *c, size_t vl,
                        int iters, uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a6                  \n\t"
        "vsetvli zero, a3, e8, m4, ta, mu   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v20, (a1)                  \n\t"
        "vmsne.vi v0, v20, 0                \n\t"
        "vmv.v.v v16, v8                    \n\t"
        "li      t3, 5                      \n\t"  /* VRGATHER_VX_INDEX */
        "mv      t0, a7                     \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vrgather.vx v16, v8, t3, v0.t      \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a5)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {5,21} - vrgather.vv */
__attribute__((naked, noinline))
void k_vrgather_vv(int8_t *a, int8_t *idx, int8_t *c, size_t vl,
                   int iters, uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a6                  \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vrgather.vv v16, v8, v12           \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a5)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {6,22} - vmv.s.x (vd[0] = rs1; only the first lane in the 2D fabric)
 * args: c, vl, iters, perf, vert, tag
 *       a0 a1 a2     a3    a4    a5
 */
__attribute__((naked, noinline))
void k_vmv_s_x(int8_t *c, size_t vl, int iters, uintptr_t perf,
               int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a4                  \n\t"
        "vsetvli zero, a1, e8, m4, ta, ma   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "li      t3, 42                     \n\t"  /* VMV_S_X_VALUE */
        "mv      t0, a5                     \n\t"
        "mv      t2, a2                     \n\t"
        "sw      t0, 0(a3)                  \n\t"
    "1:                                     \n\t"
        "vmv.s.x v16, t3                    \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a3)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a0)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {7,23} - vmv.v.v
 * args: a, c, vl, iters, perf, vert, tag
 */
__attribute__((naked, noinline))
void k_vmv_v_v(int8_t *a, int8_t *c, size_t vl, int iters,
               uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vmv.v.v v16, v8                    \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {8,24} - vredsum.vs (only first lane writes; § 4.1)
 * Use a separate seed vs1 = v20 = 0, kept across iters so vd[0] is a
 * pure function of vs2 and not an accumulator.
 */
__attribute__((naked, noinline))
void k_vredsum(int8_t *a, int8_t *c, size_t vl, int iters,
               uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "vmv.v.i v20, 0                     \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vredsum.vs v16, v8, v20            \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {9,10,25,26} - vslideup.vx
 * args: a, c, vl, iters, perf, offset, vert, tag
 *       a0 a1 a2 a3    a4    a5      a6    a7
 */
__attribute__((naked, noinline))
void k_vslideup(int8_t *a, int8_t *c, size_t vl, int iters,
                uintptr_t perf, int offset, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a6                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vslideup.vx v16, v8, a5            \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {11,12,27,28} - vslidedown.vx */
__attribute__((naked, noinline))
void k_vslidedown(int8_t *a, int8_t *c, size_t vl, int iters,
                  uintptr_t perf, int offset, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a6                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vslidedown.vx v16, v8, a5          \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {13,29} - vle8.v full (no mask)
 * Repeats the load 100x into v16, then stores once for verification.
 * args: a, c, vl, iters, perf, vert, tag
 */
__attribute__((naked, noinline))
void k_vle_full(int8_t *a, int8_t *c, size_t vl, int iters,
                uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vle8.v  v16, (a0)                  \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {14,30} - vle8.v with mask (only element index 0 active per row)
 * Mask construction in horizontal mode: vid.v + vmseq.vi v0, _, 0 gives
 * v0[c] = (c == 0) for every hw-row, so the masked load only touches
 * column 0 of every hardware row. mu policy keeps the pre-filled zero
 * in masked-off cells. CSR is then switched to the test mode for the
 * timed loop; LSU is gated horizontal regardless (§ 4.3) so the
 * data movement is currently identical between H and V calls.
 */
__attribute__((naked, noinline))
void k_vle_masked(int8_t *a, int8_t *c, size_t vl, int iters,
                  uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, mu   \n\t"
        "vid.v   v12                        \n\t"
        "vmseq.vi v0, v12, 0                \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "csrw    0x7c0, a5                  \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vle8.v  v16, (a0), v0.t            \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {15,31} - vse8.v full (no mask)
 * Loads grid_a once into v8, then issues 100 stores of v8 to grid_c.
 * No verification vse needed at the end - the loop already wrote grid_c.
 */
__attribute__((naked, noinline))
void k_vse_full(int8_t *a, int8_t *c, size_t vl, int iters,
                uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vse8.v  v8, (a1)                   \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "ret                                \n\t"
    );
}

/* TEST {16,32} - vse8.v with mask (only element index 0 active per row)
 * grid_c is pre-cleared to 0 by the C-side wrapper. Each iteration
 * overwrites only column 0 of grid_c with column 0 of grid_a; other
 * columns stay 0.
 */
__attribute__((naked, noinline))
void k_vse_masked(int8_t *a, int8_t *c, size_t vl, int iters,
                  uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vid.v   v12                        \n\t"
        "vmseq.vi v0, v12, 0                \n\t"
        "csrw    0x7c0, a5                  \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vse8.v  v8, (a1), v0.t             \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "ret                                \n\t"
    );
}

/*============================================================================
 * Result checkers (all scalar, no vector liveness across calls).
 *==========================================================================*/

static inline int8_t a_val(int r, int c) {
    return (int8_t)((r + c) & (COLS - 1));
}
static inline int8_t b_val(int r, int c) {
    return (int8_t)((r * 2 + c) & (COLS - 1));
}

static const char *mode_str(int vert) { return vert ? "V" : "H"; }

/* Generic two-counter accumulator used by all per-cell checkers. */
typedef struct {
    int errors;
    int br, bc;
    int8_t bgot, bexp;
} CheckState;

static inline void cs_init(CheckState *s) {
    s->errors = 0; s->br = -1; s->bc = -1; s->bgot = 0; s->bexp = 0;
}
static inline void cs_record(CheckState *s, int r, int c, int8_t got, int8_t exp) {
    if (got != exp) {
        if (s->errors == 0) {
            s->br = r; s->bc = c; s->bgot = got; s->bexp = exp;
        }
        s->errors++;
    }
}
static inline void cs_report(const CheckState *s, const char *name, const char *mode,
                             const char *desc) {
    if (s->errors == 0) {
        printf("[CHECK] PASS %s (%s): %s\n", name, mode, desc);
    } else {
        printf("[CHECK] FAIL %s (%s): %d errors; first at [%d][%d] got %d exp %d\n",
               name, mode, s->errors, s->br, s->bc, s->bgot, s->bexp);
    }
}

__attribute__((noinline))
static void check_vadd(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      (int8_t)(a_val(r, c) + b_val(r, c)));
    cs_report(&s, "vadd.vv", mode_str(vert), "grid_c == grid_a + grid_b (i8 wrap)");
}

__attribute__((noinline))
static void check_vmul(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      (int8_t)((int)a_val(r, c) * (int)b_val(r, c)));
    cs_report(&s, "vmul.vv", mode_str(vert), "grid_c == grid_a * grid_b (i8 wrap)");
}

__attribute__((noinline))
static void check_vand(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      (int8_t)((uint8_t)a_val(r, c) & (uint8_t)b_val(r, c)));
    cs_report(&s, "vand.vv", mode_str(vert), "grid_c == grid_a & grid_b");
}

__attribute__((noinline))
static void check_vor(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      (int8_t)((uint8_t)a_val(r, c) | (uint8_t)b_val(r, c)));
    cs_report(&s, "vor.vv", mode_str(vert), "grid_c == grid_a | grid_b");
}

/* Horizontal vrgather.vx: every column of row r holds grid_a[r][rs1]. */
__attribute__((noinline))
static void check_vrgather_vx_h(void) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++) {
        int8_t exp = a_val(r, VRGATHER_VX_INDEX);
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c], exp);
    }
    cs_report(&s, "vrgather.vx", "H", "all cells == grid_a[r][rs1=5]");
}

/* Vertical vrgather.vx: every row of column c holds grid_a[rs1][c]. */
__attribute__((noinline))
static void check_vrgather_vx_v(void) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      a_val(VRGATHER_VX_INDEX, c));
    cs_report(&s, "vrgather.vx", "V", "all cells == grid_a[rs1=5][c]");
}

/* vrgather.vx with diagonal v0.t mask: H and V results coincide for the
 * symmetric grid_a (see header comment).
 */
__attribute__((noinline))
static void check_vrgather_vx_mask(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int8_t exp = (r == c) ? a_val(r, VRGATHER_VX_INDEX) : a_val(r, c);
            cs_record(&s, r, c, grid_c[r][c], exp);
        }
    }
    cs_report(&s, "vrgather.vx (diag mask)", mode_str(vert),
              "diag = grid_a[r][rs1], off-diag = grid_a[r][c]");
}

/* H-mode vrgather.vv: idx_grid[r][c] = (r+c+1)&127 -> result (2r+c+1)&127. */
__attribute__((noinline))
static void check_vrgather_vv_h(void) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      (int8_t)((2 * r + c + 1) & (COLS - 1)));
    cs_report(&s, "vrgather.vv", "H", "grid_c == (2r+c+1)&127");
}

/* V-mode vrgather.vv: gather happens within a vert-lane (= column in H view).
 * For each H-view column c, vert_idx[c][r] = (r+c+1)&127, vert_src[c] = column
 * c of grid_a, so vert_dst[c][r] = grid_a[(r+c+1)&127][c]. In H view that is
 * v16[r][c] = grid_a[(r+c+1)&127][c] = ((r+c+1) + c) & 127 = (r+2c+1)&127.
 */
__attribute__((noinline))
static void check_vrgather_vv_v(void) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      (int8_t)((r + 2 * c + 1) & (COLS - 1)));
    cs_report(&s, "vrgather.vv", "V", "grid_c == (r+2c+1)&127");
}

__attribute__((noinline))
static void check_vmv_s_x(int vert) {
    int8_t got = grid_c[0][0];
    if (got == (int8_t)VMV_S_X_VALUE) {
        printf("[CHECK] PASS vmv.s.x (%s): grid_c[0][0] = %d\n",
               mode_str(vert), VMV_S_X_VALUE);
    } else {
        printf("[CHECK] FAIL vmv.s.x (%s): grid_c[0][0] = %d, expected %d\n",
               mode_str(vert), got, VMV_S_X_VALUE);
    }
}

__attribute__((noinline))
static void check_vmv_v_v(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c], a_val(r, c));
    cs_report(&s, "vmv.v.v", mode_str(vert), "grid_c == grid_a");
}

/* For the symmetric grid, sum of row 0 == sum of col 0. Both modes land
 * the result in grid_c[0][0]. */
__attribute__((noinline))
static void check_vredsum(int vert) {
    int8_t ref = 0;
    for (int c = 0; c < COLS; c++) ref = (int8_t)(ref + a_val(0, c));
    int8_t got = grid_c[0][0];
    if (got == ref) {
        printf("[CHECK] PASS vredsum.vs (%s): grid_c[0][0] sum = %d\n",
               mode_str(vert), ref);
    } else {
        printf("[CHECK] FAIL vredsum.vs (%s): grid_c[0][0] = %d, expected %d\n",
               mode_str(vert), got, ref);
    }
}

/* H slideup: shift columns within a row by `off`. */
__attribute__((noinline))
static void check_vslideup_h(int off) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int8_t exp = (c < off) ? (int8_t)0 : a_val(r, c - off);
            cs_record(&s, r, c, grid_c[r][c], exp);
        }
    char d[40]; (void)d;
    if (s.errors == 0) {
        printf("[CHECK] PASS vslideup.vx by %d (H): col c<%d zero, "
               "rest = grid_a[r][c-%d]\n", off, off, off);
    } else {
        printf("[CHECK] FAIL vslideup.vx by %d (H): %d errors; "
               "first at [%d][%d] got %d exp %d\n",
               off, s.errors, s.br, s.bc, s.bgot, s.bexp);
    }
}

/* V slideup: shift rows within a column by `off` (image moves down `off`). */
__attribute__((noinline))
static void check_vslideup_v(int off) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int8_t exp = (r < off) ? (int8_t)0 : a_val(r - off, c);
            cs_record(&s, r, c, grid_c[r][c], exp);
        }
    if (s.errors == 0) {
        printf("[CHECK] PASS vslideup.vx by %d (V): row r<%d zero, "
               "rest = grid_a[r-%d][c]\n", off, off, off);
    } else {
        printf("[CHECK] FAIL vslideup.vx by %d (V): %d errors; "
               "first at [%d][%d] got %d exp %d\n",
               off, s.errors, s.br, s.bc, s.bgot, s.bexp);
    }
}

/* H slidedown: shift columns within a row by `off` toward lower index. */
__attribute__((noinline))
static void check_vslidedown_h(int off) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int8_t exp = (c + off >= COLS) ? (int8_t)0 : a_val(r, c + off);
            cs_record(&s, r, c, grid_c[r][c], exp);
        }
    if (s.errors == 0) {
        printf("[CHECK] PASS vslidedown.vx by %d (H): col c>=%d zero, "
               "rest = grid_a[r][c+%d]\n",
               off, COLS - off, off);
    } else {
        printf("[CHECK] FAIL vslidedown.vx by %d (H): %d errors; "
               "first at [%d][%d] got %d exp %d\n",
               off, s.errors, s.br, s.bc, s.bgot, s.bexp);
    }
}

/* V slidedown: shift rows within a column by `off` (image moves up `off`). */
__attribute__((noinline))
static void check_vslidedown_v(int off) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int8_t exp = (r + off >= ROWS) ? (int8_t)0 : a_val(r + off, c);
            cs_record(&s, r, c, grid_c[r][c], exp);
        }
    if (s.errors == 0) {
        printf("[CHECK] PASS vslidedown.vx by %d (V): row r>=%d zero, "
               "rest = grid_a[r+%d][c]\n",
               off, ROWS - off, off);
    } else {
        printf("[CHECK] FAIL vslidedown.vx by %d (V): %d errors; "
               "first at [%d][%d] got %d exp %d\n",
               off, s.errors, s.br, s.bc, s.bgot, s.bexp);
    }
}

__attribute__((noinline))
static void check_vle_full(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c], a_val(r, c));
    cs_report(&s, "vle8.v full", mode_str(vert), "grid_c == grid_a");
}

__attribute__((noinline))
static void check_vle_masked(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++) {
        cs_record(&s, r, 0, grid_c[r][0], a_val(r, 0));
        for (int c = 1; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c], 0);
    }
    cs_report(&s, "vle8.v masked", mode_str(vert),
              "col 0 == grid_a[r][0], rest == 0");
}

__attribute__((noinline))
static void check_vse_full(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c], a_val(r, c));
    cs_report(&s, "vse8.v full", mode_str(vert), "grid_c == grid_a");
}

__attribute__((noinline))
static void check_vse_masked(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++) {
        cs_record(&s, r, 0, grid_c[r][0], a_val(r, 0));
        for (int c = 1; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c], 0);
    }
    cs_report(&s, "vse8.v masked", mode_str(vert),
              "col 0 == grid_a[r][0], rest == 0 (preserved)");
}

/*============================================================================
 * Test driver
 *==========================================================================*/

#define PERF ((uintptr_t)PERF_REG_ADDR)
#define VL   ((size_t)COLS)

static void run_compute_block(int vert) {
    /* H compute occupies slots 1..N_COMPUTE; V compute occupies slots
     * N_PER_MODE+1..N_PER_MODE+N_COMPUTE. */
    const int base = vert ? N_PER_MODE : 0;
    const char *mode = mode_str(vert);

    printf("\n--- TEST %d: vadd.vv (%s, tag=%d) ---\n", 1 + base, mode, 1 + base);
    clear_grid_c();
    k_vadd(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
           VL, ITERS, PERF, vert, 1 + base);
    print_grid(2, &grid_c[0][0]);
    check_vadd(vert);

    printf("\n--- TEST %d: vmul.vv (%s, tag=%d) ---\n", 2 + base, mode, 2 + base);
    clear_grid_c();
    k_vmul(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
           VL, ITERS, PERF, vert, 2 + base);
    print_grid(2, &grid_c[0][0]);
    check_vmul(vert);

    printf("\n--- TEST %d: vand.vv (%s, tag=%d) ---\n", 3 + base, mode, 3 + base);
    clear_grid_c();
    k_vand(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
           VL, ITERS, PERF, vert, 3 + base);
    print_grid(2, &grid_c[0][0]);
    check_vand(vert);

    printf("\n--- TEST %d: vor.vv (%s, tag=%d) ---\n", 4 + base, mode, 4 + base);
    clear_grid_c();
    k_vor(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
          VL, ITERS, PERF, vert, 4 + base);
    print_grid(2, &grid_c[0][0]);
    check_vor(vert);

    printf("\n--- TEST %d: vrgather.vx idx=%d (%s, tag=%d) ---\n",
           5 + base, VRGATHER_VX_INDEX, mode, 5 + base);
    clear_grid_c();
    k_vrgather_vx(&grid_a[0][0], &grid_c[0][0],
                  VL, ITERS, PERF, vert, 5 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vrgather_vx_v();
    else      check_vrgather_vx_h();

    printf("\n--- TEST %d: vrgather.vx + diag v0.t mask (%s, tag=%d) ---\n",
           6 + base, mode, 6 + base);
    clear_grid_c();
    k_vrgather_vx_mask(&grid_a[0][0], &diag_buf[0][0], &grid_c[0][0],
                       VL, ITERS, PERF, vert, 6 + base);
    print_grid(2, &grid_c[0][0]);
    check_vrgather_vx_mask(vert);

    printf("\n--- TEST %d: vrgather.vv (%s, tag=%d) ---\n", 7 + base, mode, 7 + base);
    clear_grid_c();
    k_vrgather_vv(&grid_a[0][0], &idx_grid[0][0], &grid_c[0][0],
                  VL, ITERS, PERF, vert, 7 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vrgather_vv_v();
    else      check_vrgather_vv_h();

    printf("\n--- TEST %d: vmv.s.x val=%d (%s, tag=%d) ---\n",
           8 + base, VMV_S_X_VALUE, mode, 8 + base);
    clear_grid_c();
    k_vmv_s_x(&grid_c[0][0], VL, ITERS, PERF, vert, 8 + base);
    print_grid(2, &grid_c[0][0]);
    check_vmv_s_x(vert);

    printf("\n--- TEST %d: vmv.v.v (%s, tag=%d) ---\n", 9 + base, mode, 9 + base);
    clear_grid_c();
    k_vmv_v_v(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF, vert, 9 + base);
    print_grid(2, &grid_c[0][0]);
    check_vmv_v_v(vert);

    printf("\n--- TEST %d: vredsum.vs (%s, tag=%d) ---\n", 10 + base, mode, 10 + base);
    clear_grid_c();
    k_vredsum(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF, vert, 10 + base);
    print_grid(3, &grid_c[0][0]);
    check_vredsum(vert);

    printf("\n--- TEST %d: vslideup.vx by %d (%s, tag=%d) ---\n",
           11 + base, VSLIDE_SMALL, mode, 11 + base);
    clear_grid_c();
    k_vslideup(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF,
               VSLIDE_SMALL, vert, 11 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vslideup_v(VSLIDE_SMALL);
    else      check_vslideup_h(VSLIDE_SMALL);

    printf("\n--- TEST %d: vslideup.vx by %d (%s, tag=%d) ---\n",
           12 + base, VSLIDE_BIG, mode, 12 + base);
    clear_grid_c();
    k_vslideup(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF,
               VSLIDE_BIG, vert, 12 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vslideup_v(VSLIDE_BIG);
    else      check_vslideup_h(VSLIDE_BIG);

    printf("\n--- TEST %d: vslidedown.vx by %d (%s, tag=%d) ---\n",
           13 + base, VSLIDE_SMALL, mode, 13 + base);
    clear_grid_c();
    k_vslidedown(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF,
                 VSLIDE_SMALL, vert, 13 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vslidedown_v(VSLIDE_SMALL);
    else      check_vslidedown_h(VSLIDE_SMALL);

    printf("\n--- TEST %d: vslidedown.vx by %d (%s, tag=%d) ---\n",
           14 + base, VSLIDE_BIG, mode, 14 + base);
    clear_grid_c();
    k_vslidedown(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF,
                 VSLIDE_BIG, vert, 14 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vslidedown_v(VSLIDE_BIG);
    else      check_vslidedown_h(VSLIDE_BIG);
}

static void run_lsu_block(int vert) {
    /* LSU follows compute within each mode: slots N_COMPUTE+1..N_PER_MODE
     * for H, and the same range shifted by N_PER_MODE for V. */
    const int base = vert ? (N_PER_MODE + N_COMPUTE) : N_COMPUTE;
    const char *mode = mode_str(vert);

    printf("\n--- TEST %d: vle8.v full (%s, tag=%d) ---\n", 1 + base, mode, 1 + base);
    clear_grid_c();
    k_vle_full(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF, vert, 1 + base);
    print_grid(2, &grid_c[0][0]);
    check_vle_full(vert);

    printf("\n--- TEST %d: vle8.v masked (%s, tag=%d) ---\n", 2 + base, mode, 2 + base);
    clear_grid_c();
    k_vle_masked(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF, vert, 2 + base);
    print_grid(2, &grid_c[0][0]);
    check_vle_masked(vert);

    printf("\n--- TEST %d: vse8.v full (%s, tag=%d) ---\n", 3 + base, mode, 3 + base);
    clear_grid_c();
    k_vse_full(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF, vert, 3 + base);
    print_grid(2, &grid_c[0][0]);
    check_vse_full(vert);

    printf("\n--- TEST %d: vse8.v masked (%s, tag=%d) ---\n", 4 + base, mode, 4 + base);
    clear_grid_c();
    k_vse_masked(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF, vert, 4 + base);
    print_grid(2, &grid_c[0][0]);
    check_vse_masked(vert);
}

void test(void) {
    init_grids();

    printf("=== benchmark_instructions: %d back-to-back iters per (instruction, mode) ===\n",
           ITERS);
    printf("[INIT] grid_a[r][c] = (r+c)&127; grid_b[r][c] = (r*2+c)&127\n");
    printf("[INIT] idx_grid[r][c] = (r+c+1)&127; diag_buf[r][c] = (r==c)?1:0\n");

    printf("\n############### HORIZONTAL MODE ###############\n");
    run_compute_block(/*vert=*/0);
    run_lsu_block    (/*vert=*/0);

    printf("\n############### VERTICAL MODE ###############\n");
    run_compute_block(/*vert=*/1);
    run_lsu_block    (/*vert=*/1);

    printf("\n=== benchmark_instructions done ===\n");
}
