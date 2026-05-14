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
 * default N_COMPUTE=29 / N_LSU=6 that lands at:
 *   1..29  H compute (arithmetic, bitwise/shift, mask, gather/move,
 *          reduction, slide)
 *  30..35  H LSU (unit-stride/masked/strided load-store)
 *  36..64  V compute (same order)
 *  65..70  V LSU (same order)
 * Adding an instruction is documented in the N_COMPUTE block below.
 *****************************************************************************/

#define ROWS 128
#define COLS 128
#define ITERS 2
#define PERF_REG_ADDR 0x10000014

#define VRGATHER_VX_INDEX  5    /* scalar source-element index for vrgather.vx */
#define VMV_S_X_VALUE     42    /* scalar value moved to vd[0] for vmv.s.x */
#define VMV_X_S_INDEX      5    /* setup gather index before scalar extraction */
#define VSLIDE_SMALL       1
#define VSLIDE_BIG       100
#define VSLIDE_IMM_BIG    31
#define VMSGT_THRESHOLD   64
#define STRIDE2_PIXELS    64
#define STRIDE2_BYTES      2

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
#define N_COMPUTE   29
#define N_LSU        6
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
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "csrw    0x7c0, a6                  \n\t"
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
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "csrw    0x7c0, a6                  \n\t"
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

/* TEST {3,N_PER_MODE+3} - vmacc.vv: vd += vs1 * vs2 */
__attribute__((naked, noinline))
void k_vmacc(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
             uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "vmv.v.i v16, 1                     \n\t"
        "csrw    0x7c0, a6                  \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vmacc.vv v16, v8, v12              \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a5)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {4,N_PER_MODE+4} - vmadd.vv: vd = vs1 * vd + vs2 */
__attribute__((naked, noinline))
void k_vmadd(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
             uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "vmv.v.i v16, 1                     \n\t"
        "csrw    0x7c0, a6                  \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vmadd.vv v16, v8, v12              \n\t"
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
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "csrw    0x7c0, a6                  \n\t"
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
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "csrw    0x7c0, a6                  \n\t"
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

/* TEST {7,N_PER_MODE+7} - vsll.vi */
__attribute__((naked, noinline))
void k_vsll(int8_t *a, int8_t *c, size_t vl, int iters,
            uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vsll.vi v16, v8, 1                 \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {8,N_PER_MODE+8} - vsra.vi */
__attribute__((naked, noinline))
void k_vsra(int8_t *a, int8_t *c, size_t vl, int iters,
            uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vsra.vi v16, v8, 1                 \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {9,N_PER_MODE+9} - vmseq.vv; stored as 0/1 bytes for checking. */
__attribute__((naked, noinline))
void k_vmseq(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
             uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "csrw    0x7c0, a6                  \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vmseq.vv v0, v8, v12               \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a5)                \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "vmerge.vim v16, v16, 1, v0         \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {10,N_PER_MODE+10} - vmsle.vv; stored as 0/1 bytes for checking. */
__attribute__((naked, noinline))
void k_vmsle(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
             uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "csrw    0x7c0, a6                  \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vmsle.vv v0, v8, v12               \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a5)                \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "vmerge.vim v16, v16, 1, v0         \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {11,N_PER_MODE+11} - vmsgt.vx; stored as 0/1 bytes for checking. */
__attribute__((naked, noinline))
void k_vmsgt(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
             uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a6                  \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "li      t3, 64                     \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vmsgt.vx v0, v8, t3                \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a5)                \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "vmerge.vim v16, v16, 1, v0         \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {12,N_PER_MODE+12} - vmslt.vv; stored as 0/1 bytes for checking. */
__attribute__((naked, noinline))
void k_vmslt(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
             uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "csrw    0x7c0, a6                  \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vmslt.vv v0, v8, v12               \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a5)                \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "vmerge.vim v16, v16, 1, v0         \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {13,N_PER_MODE+13} - vmand.mm; stored as 0/1 bytes for checking. */
__attribute__((naked, noinline))
void k_vmand(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
             uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a6                  \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "vmseq.vi v0, v8, 0                 \n\t"
        "vmseq.vi v1, v12, 0                \n\t"
        "mv      t0, a7                     \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"
    "1:                                     \n\t"
        "vmand.mm v0, v0, v1                \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a5)                \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "vmerge.vim v16, v16, 1, v0         \n\t"
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

/* TEST {19,N_PER_MODE+19} - vredmax.vs */
__attribute__((naked, noinline))
void k_vredmax(int8_t *a, int8_t *c, size_t vl, int iters,
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
        "vredmax.vs v16, v8, v20            \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {21,22,N_PER_MODE+21,N_PER_MODE+22} - vslideup.vx
 * args: a, c, vl, iters, perf, offset, vert, tag
 *       a0 a1 a2 a3    a4    a5      a6    a7
 *
 * Slides intentionally use horizontal load + selected-mode compute +
 * horizontal store. That makes V mode observable as a row shift in memory;
 * a V-load/V-store pair would hide the compute orientation by transposing
 * both sides of the operation.
 */
__attribute__((naked, noinline))
void k_vslideup(int8_t *a, int8_t *c, size_t vl, int iters,
                uintptr_t perf, int offset, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "csrw    0x7c0, a6                  \n\t"
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

/* TEST {23,24,N_PER_MODE+23,N_PER_MODE+24} - vslidedown.vx */
__attribute__((naked, noinline))
void k_vslidedown(int8_t *a, int8_t *c, size_t vl, int iters,
                  uintptr_t perf, int offset, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "csrw    0x7c0, a6                  \n\t"
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

/* TEST {18,N_PER_MODE+18} - vmv.x.s
 * Setup uses a mode-specific vslidedown.vi on asymmetric grid_b, then the
 * timed instruction extracts element 0 under the same mode:
 *   H expected = grid_b[0][VMV_X_S_INDEX]
 *   V expected = grid_b[VMV_X_S_INDEX][0]
 *
 * Do not use vrgather as the setup here: that makes this test depend on
 * vertical gather behavior and hides whether vmv.x.s itself is correct.
 */
// __attribute__((naked, noinline))
// void k_vmv_x_s(int8_t *b, int8_t *c, size_t vl, int iters,
//                uintptr_t perf, int vert, int tag) {
//     __asm__ volatile (
//         "csrw    0x7c0, zero                \n\t"
//         "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
//         "vle8.v  v8, (a0)                   \n\t"
//         "vmv.v.i v16, 0                     \n\t"
//         "csrw    0x7c0, a5                  \n\t"
//         "vslidedown.vi v16, v8, 5           \n\t"
//         "mv      t0, a6                     \n\t"
//         "mv      t2, a3                     \n\t"
//         "sw      t0, 0(a4)                  \n\t"
//     "1:                                     \n\t"
//         "vmv.x.s t4, v16                    \n\t"
//         "addi    t2, t2, -1                 \n\t"
//         "bnez    t2, 1b                     \n\t"
//         "sw      zero, 0(a4)                \n\t"
//         "csrw    0x7c0, zero                \n\t"
//         "sb      t4, 0(a1)                  \n\t"
//         "ret                                \n\t"
//     );
// }
__attribute__((naked, noinline))
void k_vmv_x_s(int8_t *b, int8_t *c, size_t vl, int iters,
               uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "csrw    0x7c0, a5                  \n\t"
        "vslidedown.vi v16, v8, 5           \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vmv.x.s t4, v16                    \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        // "sb      t4, 0(a1)                  \n\t"
        "vle8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {26,28,N_PER_MODE+26,N_PER_MODE+28} - vslideup.vi */
__attribute__((naked, noinline))
void k_vslideup_vi1(int8_t *a, int8_t *c, size_t vl, int iters,
                    uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "csrw    0x7c0, a5                  \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vslideup.vi v16, v8, 1             \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

__attribute__((naked, noinline))
void k_vslideup_vi31(int8_t *a, int8_t *c, size_t vl, int iters,
                     uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "csrw    0x7c0, a5                  \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vslideup.vi v16, v8, 31            \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {27,29,N_PER_MODE+27,N_PER_MODE+29} - vslidedown.vi */
__attribute__((naked, noinline))
void k_vslidedown_vi1(int8_t *a, int8_t *c, size_t vl, int iters,
                      uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "csrw    0x7c0, a5                  \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vslidedown.vi v16, v8, 1           \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

__attribute__((naked, noinline))
void k_vslidedown_vi31(int8_t *a, int8_t *c, size_t vl, int iters,
                       uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "csrw    0x7c0, a5                  \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vslidedown.vi v16, v8, 31          \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {N_COMPUTE+1,2*N_COMPUTE+N_LSU+1} - vle8.v full (no mask)
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
        /* Build the mask under the same CSR mode the masked vle will consume
         * it in, with no H-mode op touching v0 in between (handoff §4.2).
         * For vert=0 this is identical to the old H-only flow; for vert=1
         * v0 is built in V mode, kept untouched, and the masked V-load
         * reads it through the V scatter consistently. */
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, mu   \n\t"
        "vid.v   v12                        \n\t"
        "vmseq.vi v0, v12, 0                \n\t"
        "vmv.v.i v16, 0                     \n\t"
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

/* TEST {28,57} - vlse8.v with stride 2, vl=64 */
__attribute__((naked, noinline))
void k_vlse_stride2(int8_t *a, int8_t *c, size_t vl, int iters,
                    uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "li      t4, 2                      \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vlse8.v v16, (a0), t4              \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      zero, 0(a4)                \n\t"
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {29,58} - vsse8.v with stride 2, vl=64 */
__attribute__((naked, noinline))
void k_vsse_stride2(int8_t *a, int8_t *c, size_t vl, int iters,
                    uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "li      t4, 2                      \n\t"
        "mv      t0, a6                     \n\t"
        "mv      t2, a3                     \n\t"
        "sw      t0, 0(a4)                  \n\t"
    "1:                                     \n\t"
        "vsse8.v v8, (a1), t4               \n\t"
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
static inline int8_t bool_byte(int p) {
    return p ? (int8_t)1 : (int8_t)0;
}

static const char *mode_str(int vert) { return vert ? "V" : "H"; }

static int g_check_total;
static int g_check_failed;

static inline void record_check_result(int pass) {
    g_check_total++;
    if (!pass) g_check_failed++;
}

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
    record_check_result(s->errors == 0);
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
static void check_vmacc(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      (int8_t)(1 + ITERS * (int)a_val(r, c) * (int)b_val(r, c)));
    cs_report(&s, "vmacc.vv", mode_str(vert),
              "grid_c == 1 + ITERS * grid_a * grid_b (i8 wrap)");
}

__attribute__((noinline))
static void check_vmadd(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int x = 1;
            int a = a_val(r, c);
            int b = b_val(r, c);
            for (int i = 0; i < ITERS; i++) x = (int8_t)(a * x + b);
            cs_record(&s, r, c, grid_c[r][c], (int8_t)x);
        }
    }
    cs_report(&s, "vmadd.vv", mode_str(vert),
              "grid_c == repeated(grid_a * vd + grid_b) from vd=1");
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

__attribute__((noinline))
static void check_vsll(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      (int8_t)((uint8_t)a_val(r, c) << 1));
    cs_report(&s, "vsll.vi", mode_str(vert), "grid_c == grid_a << 1 (i8 wrap)");
}

__attribute__((noinline))
static void check_vsra(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c], (int8_t)(a_val(r, c) >> 1));
    cs_report(&s, "vsra.vi", mode_str(vert), "grid_c == grid_a >> 1 (signed)");
}

__attribute__((noinline))
static void check_vmseq(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      bool_byte(a_val(r, c) == b_val(r, c)));
    cs_report(&s, "vmseq.vv", mode_str(vert), "grid_c == (grid_a == grid_b)");
}

__attribute__((noinline))
static void check_vmsle(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      bool_byte(a_val(r, c) <= b_val(r, c)));
    cs_report(&s, "vmsle.vv", mode_str(vert), "grid_c == (grid_a <= grid_b)");
}

__attribute__((noinline))
static void check_vmsgt(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      bool_byte(a_val(r, c) > VMSGT_THRESHOLD));
    cs_report(&s, "vmsgt.vx", mode_str(vert), "grid_c == (grid_a > 64)");
}

__attribute__((noinline))
static void check_vmslt(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      bool_byte(a_val(r, c) < b_val(r, c)));
    cs_report(&s, "vmslt.vv", mode_str(vert), "grid_c == (grid_a < grid_b)");
}

__attribute__((noinline))
static void check_vmand(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      bool_byte(a_val(r, c) == 0 && b_val(r, c) == 0));
    cs_report(&s, "vmand.mm", mode_str(vert),
              "grid_c == (grid_a == 0) & (grid_b == 0)");
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
    record_check_result(got == (int8_t)VMV_S_X_VALUE);
    if (got == (int8_t)VMV_S_X_VALUE) {
        printf("[CHECK] PASS vmv.s.x (%s): grid_c[0][0] = %d\n",
               mode_str(vert), VMV_S_X_VALUE);
    } else {
        printf("[CHECK] FAIL vmv.s.x (%s): grid_c[0][0] = %d, expected %d\n",
               mode_str(vert), got, VMV_S_X_VALUE);
    }
}

__attribute__((noinline))
static void check_vmv_x_s(int vert) {
    CheckState s; cs_init(&s);
    int8_t ref = vert ? b_val(VMV_X_S_INDEX, 0) : b_val(0, VMV_X_S_INDEX);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int8_t exp = (r == 0 && c == 0) ? ref : (int8_t)0;
            cs_record(&s, r, c, grid_c[r][c], exp);
        }
    cs_report(&s, "vmv.x.s", mode_str(vert),
              vert
                ? "extract after V vslidedown.vi setup: grid_c[0][0] == grid_b[5][0]"
                : "extract after H vslidedown.vi setup: grid_c[0][0] == grid_b[0][5]");
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
    record_check_result(got == ref);
    if (got == ref) {
        printf("[CHECK] PASS vredsum.vs (%s): grid_c[0][0] sum = %d\n",
               mode_str(vert), ref);
    } else {
        printf("[CHECK] FAIL vredsum.vs (%s): grid_c[0][0] = %d, expected %d\n",
               mode_str(vert), got, ref);
    }
}

__attribute__((noinline))
static void check_vredmax(int vert) {
    CheckState s; cs_init(&s);
    if (!vert) {
        for (int r = 0; r < ROWS; r++) {
            int8_t ref = a_val(r, 0);
            for (int c = 1; c < COLS; c++)
                if (a_val(r, c) > ref) ref = a_val(r, c);
            cs_record(&s, r, 0, grid_c[r][0], ref);
        }
        cs_report(&s, "vredmax.vs", "H", "first column == max of each row");
    } else {
        for (int c = 0; c < COLS; c++) {
            int8_t ref = a_val(0, c);
            for (int r = 1; r < ROWS; r++)
                if (a_val(r, c) > ref) ref = a_val(r, c);
            cs_record(&s, 0, c, grid_c[0][c], ref);
        }
        cs_report(&s, "vredmax.vs", "V", "first row == max of each column");
    }
}

static inline int8_t slide_src_val(int use_b, int r, int c) {
    return use_b ? b_val(r, c) : a_val(r, c);
}

/* H slideup: shift columns within a row by `off`. */
__attribute__((noinline))
static void check_vslideup_h(const char *op, int off, int use_b) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int8_t exp = (c < off) ? (int8_t)0 : slide_src_val(use_b, r, c - off);
            cs_record(&s, r, c, grid_c[r][c], exp);
        }
    record_check_result(s.errors == 0);
    if (s.errors == 0) {
        printf("[CHECK] PASS %s by %d (H): col c<%d zero, "
               "rest = grid_%c[r][c-%d]\n", op, off, off,
               use_b ? 'b' : 'a', off);
    } else {
        printf("[CHECK] FAIL %s by %d (H): %d errors; "
               "first at [%d][%d] got %d exp %d\n",
               op, off, s.errors, s.br, s.bc, s.bgot, s.bexp);
    }
}

/* V slideup: shift rows within a column by `off` (image moves down `off`). */
__attribute__((noinline))
static void check_vslideup_v(const char *op, int off, int use_b) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int8_t exp = (r < off) ? (int8_t)0 : slide_src_val(use_b, r - off, c);
            cs_record(&s, r, c, grid_c[r][c], exp);
        }
    record_check_result(s.errors == 0);
    if (s.errors == 0) {
        printf("[CHECK] PASS %s by %d (V): row r<%d zero, "
               "rest = grid_%c[r-%d][c]\n", op, off, off,
               use_b ? 'b' : 'a', off);
    } else {
        printf("[CHECK] FAIL %s by %d (V): %d errors; "
               "first at [%d][%d] got %d exp %d\n",
               op, off, s.errors, s.br, s.bc, s.bgot, s.bexp);
    }
}

/* H slidedown: shift columns within a row by `off` toward lower index. */
__attribute__((noinline))
static void check_vslidedown_h(const char *op, int off, int use_b) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int8_t exp = (c + off >= COLS) ? (int8_t)0 : slide_src_val(use_b, r, c + off);
            cs_record(&s, r, c, grid_c[r][c], exp);
        }
    record_check_result(s.errors == 0);
    if (s.errors == 0) {
        printf("[CHECK] PASS %s by %d (H): col c>=%d zero, "
               "rest = grid_%c[r][c+%d]\n",
               op, off, COLS - off, use_b ? 'b' : 'a', off);
    } else {
        printf("[CHECK] FAIL %s by %d (H): %d errors; "
               "first at [%d][%d] got %d exp %d\n",
               op, off, s.errors, s.br, s.bc, s.bgot, s.bexp);
    }
}

/* V slidedown: shift rows within a column by `off` (image moves up `off`). */
__attribute__((noinline))
static void check_vslidedown_v(const char *op, int off, int use_b) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int8_t exp = (r + off >= ROWS) ? (int8_t)0 : slide_src_val(use_b, r + off, c);
            cs_record(&s, r, c, grid_c[r][c], exp);
        }
    record_check_result(s.errors == 0);
    if (s.errors == 0) {
        printf("[CHECK] PASS %s by %d (V): row r>=%d zero, "
               "rest = grid_%c[r+%d][c]\n",
               op, off, ROWS - off, use_b ? 'b' : 'a', off);
    } else {
        printf("[CHECK] FAIL %s by %d (V): %d errors; "
               "first at [%d][%d] got %d exp %d\n",
               op, off, s.errors, s.br, s.bc, s.bgot, s.bexp);
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
    if (vert) {
        /* V-mode masked vle + H-store. Mask v0 is built in V mode as
         *   physical_v0[i][j] = (i == 0)   (only physical row 0 is set)
         * which the LSU reads through V scatter as
         *   lane_mask[r][c] = (c == 0)     (active lane = c==0 of each hw-row).
         * The active lanes load grid_a[r][0]; V-write transposes those
         * lanes into v16 such that physical_v16[0][j] = grid_a[j][0] and
         * physical_v16[i>0][:] = 0. The H-store then surfaces row 0 of
         * memory as the loaded column. So the expected transposed pattern is
         *   grid_c[0][c] = grid_a[c][0] = a_val(c, 0)
         *   grid_c[r>0][c] = 0 */
        for (int c = 0; c < COLS; c++)
            cs_record(&s, 0, c, grid_c[0][c], a_val(c, 0));
        for (int r = 1; r < ROWS; r++)
            for (int c = 0; c < COLS; c++)
                cs_record(&s, r, c, grid_c[r][c], 0);
        cs_report(&s, "vle8.v masked", mode_str(vert),
                  "row 0 == grid_a[c][0] (transpose), rest == 0");
    } else {
        for (int r = 0; r < ROWS; r++) {
            cs_record(&s, r, 0, grid_c[r][0], a_val(r, 0));
            for (int c = 1; c < COLS; c++)
                cs_record(&s, r, c, grid_c[r][c], 0);
        }
        cs_report(&s, "vle8.v masked", mode_str(vert),
                  "col 0 == grid_a[r][0], rest == 0");
    }
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

/* LSU row pitch is fixed at the logical image-row width (128 e8) regardless
 * of the current `vl`. With vl=STRIDE2_PIXELS (64) and stride 2, every hw-row
 * touches exactly 64 of its own 128-byte slot.
 *
 * H mode (kernel = vlse-H + vse-H, both with vl=64):
 *   vlse8.v v16, (a0), 2     reads grid_a[r][0,2,...,126] into v16 lanes 0..63
 *                            for every hw-row r; lanes 64..127 stay 0 from
 *                            the `vmv.v.i v16, 0` init.
 *   vse8.v v16, (a1)         writes the first 64 lanes -> grid_c[r][0..63];
 *                            cols 64..127 stay 0 (clear_grid_c).
 *   Result: grid_c[r][c<64] = grid_a[r][2c]; rest = 0.
 *
 * V mode (kernel = vlse-V + vse-H): the V-LSU strided load lands the data in
 * VRF transposed, so after the H-store the memory image is the transpose of
 * the H-mode result, with the same vl=64 windowing applied along both axes:
 *   physical_v16_V[r][c] = physical_v16_H[c][r]
 *   so grid_c[r][c] = grid_a[c][2r] when r<64 and c<64; otherwise 0
 *      (rows >=64 not written by the vse, cols >=64 outside the H-row vl
 *       window have zero in v16 from the init). */
__attribute__((noinline))
static void check_vlse_stride2(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int8_t exp;
            if (vert) {
                exp = (r < STRIDE2_PIXELS && c < STRIDE2_PIXELS)
                      ? a_val(c, r * STRIDE2_BYTES)
                      : (int8_t)0;
            } else {
                exp = (c < STRIDE2_PIXELS)
                      ? a_val(r, c * STRIDE2_BYTES)
                      : (int8_t)0;
            }
            cs_record(&s, r, c, grid_c[r][c], exp);
        }
    }
    cs_report(&s, "vlse8.v stride2", mode_str(vert),
              vert
                ? "stride-2 load (V-load + H-store): transposed window grid_a[c][2r]"
                : "stride-2 load (H): cols [0,vl)=grid_a[r][2c], cols [vl,128)=0");
}

/* Mirror of the load test:
 *   vle8.v  v8, (a0)          (vl=64) loads grid_a[r][0..63] into v8.
 *   vsse8.v v8, (a1), 2       (vl=64, stride 2) writes those 64 bytes back
 *     to grid_c[r][0,2,4,...,126]; odd columns and rows the kernel did not
 *     write stay 0 from clear_grid_c(). */
__attribute__((noinline))
static void check_vsse_stride2(int vert) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int8_t exp = ((c & 1) == 0)
                         ? a_val(r, c / STRIDE2_BYTES)
                         : (int8_t)0;
            cs_record(&s, r, c, grid_c[r][c], exp);
        }
    cs_report(&s, "vsse8.v stride2", mode_str(vert),
              "stride-2 store: even cols=grid_a[r][c/2], odd cols=0");
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

    printf("\n--- TEST %d: vmacc.vv (%s, tag=%d) ---\n", 3 + base, mode, 3 + base);
    clear_grid_c();
    k_vmacc(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
            VL, ITERS, PERF, vert, 3 + base);
    print_grid(2, &grid_c[0][0]);
    check_vmacc(vert);

    printf("\n--- TEST %d: vmadd.vv (%s, tag=%d) ---\n", 4 + base, mode, 4 + base);
    clear_grid_c();
    k_vmadd(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
            VL, ITERS, PERF, vert, 4 + base);
    print_grid(2, &grid_c[0][0]);
    check_vmadd(vert);

    printf("\n--- TEST %d: vand.vv (%s, tag=%d) ---\n", 5 + base, mode, 5 + base);
    clear_grid_c();
    k_vand(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
           VL, ITERS, PERF, vert, 5 + base);
    print_grid(2, &grid_c[0][0]);
    check_vand(vert);

    printf("\n--- TEST %d: vor.vv (%s, tag=%d) ---\n", 6 + base, mode, 6 + base);
    clear_grid_c();
    k_vor(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
          VL, ITERS, PERF, vert, 6 + base);
    print_grid(2, &grid_c[0][0]);
    check_vor(vert);

    printf("\n--- TEST %d: vsll.vi (%s, tag=%d) ---\n", 7 + base, mode, 7 + base);
    clear_grid_c();
    k_vsll(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF, vert, 7 + base);
    print_grid(2, &grid_c[0][0]);
    check_vsll(vert);

    printf("\n--- TEST %d: vsra.vi (%s, tag=%d) ---\n", 8 + base, mode, 8 + base);
    clear_grid_c();
    k_vsra(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF, vert, 8 + base);
    print_grid(2, &grid_c[0][0]);
    check_vsra(vert);

    printf("\n--- TEST %d: vmseq.vv (%s, tag=%d) ---\n", 9 + base, mode, 9 + base);
    clear_grid_c();
    k_vmseq(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
            VL, ITERS, PERF, vert, 9 + base);
    print_grid(2, &grid_c[0][0]);
    check_vmseq(vert);

    printf("\n--- TEST %d: vmsle.vv (%s, tag=%d) ---\n", 10 + base, mode, 10 + base);
    clear_grid_c();
    k_vmsle(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
            VL, ITERS, PERF, vert, 10 + base);
    print_grid(2, &grid_c[0][0]);
    check_vmsle(vert);

    printf("\n--- TEST %d: vmsgt.vx (%s, tag=%d) ---\n", 11 + base, mode, 11 + base);
    clear_grid_c();
    k_vmsgt(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
            VL, ITERS, PERF, vert, 11 + base);
    print_grid(2, &grid_c[0][0]);
    check_vmsgt(vert);

    printf("\n--- TEST %d: vmslt.vv (%s, tag=%d) ---\n", 12 + base, mode, 12 + base);
    clear_grid_c();
    k_vmslt(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
            VL, ITERS, PERF, vert, 12 + base);
    print_grid(2, &grid_c[0][0]);
    check_vmslt(vert);

    printf("\n--- TEST %d: vmand.mm (%s, tag=%d) ---\n", 13 + base, mode, 13 + base);
    clear_grid_c();
    k_vmand(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
            VL, ITERS, PERF, vert, 13 + base);
    print_grid(2, &grid_c[0][0]);
    check_vmand(vert);

    printf("\n--- TEST %d: vrgather.vx idx=%d (%s, tag=%d) ---\n",
           14 + base, VRGATHER_VX_INDEX, mode, 14 + base);
    clear_grid_c();
    k_vrgather_vx(&grid_a[0][0], &grid_c[0][0],
                  VL, ITERS, PERF, vert, 14 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vrgather_vx_v();
    else      check_vrgather_vx_h();

    printf("\n--- TEST %d: vrgather.vx + diag v0.t mask (%s, tag=%d) ---\n",
           15 + base, mode, 15 + base);
    clear_grid_c();
    k_vrgather_vx_mask(&grid_a[0][0], &diag_buf[0][0], &grid_c[0][0],
                       VL, ITERS, PERF, vert, 15 + base);
    print_grid(2, &grid_c[0][0]);
    check_vrgather_vx_mask(vert);

    printf("\n--- TEST %d: vrgather.vv (%s, tag=%d) ---\n", 16 + base, mode, 16 + base);
    clear_grid_c();
    k_vrgather_vv(&grid_a[0][0], &idx_grid[0][0], &grid_c[0][0],
                  VL, ITERS, PERF, vert, 16 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vrgather_vv_v();
    else      check_vrgather_vv_h();

    printf("\n--- TEST %d: vmv.s.x val=%d (%s, tag=%d) ---\n",
           17 + base, VMV_S_X_VALUE, mode, 17 + base);
    clear_grid_c();
    k_vmv_s_x(&grid_c[0][0], VL, ITERS, PERF, vert, 17 + base);
    print_grid(2, &grid_c[0][0]);
    check_vmv_s_x(vert);

    printf("\n--- TEST %d: vmv.x.s after vslidedown.vi setup (%s, tag=%d) ---\n",
           18 + base, mode, 18 + base);
    clear_grid_c();
    print_grid(10, &grid_b[0][0]);
    print_grid(10, &grid_c[0][0]);
    k_vmv_x_s(&grid_b[0][0], &grid_c[0][0], VL, ITERS, PERF, vert, 18 + base);
    // print_grid(2, &grid_c[0][0]);
    print_grid(10, &grid_b[0][0]);
    print_grid(10, &grid_c[0][0]);

    check_vmv_x_s(vert);

    printf("\n--- TEST %d: vmv.v.v (%s, tag=%d) ---\n", 19 + base, mode, 19 + base);
    clear_grid_c();
    k_vmv_v_v(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF, vert, 19 + base);
    print_grid(2, &grid_c[0][0]);
    check_vmv_v_v(vert);

    printf("\n--- TEST %d: vredsum.vs (%s, tag=%d) ---\n", 20 + base, mode, 20 + base);
    clear_grid_c();
    k_vredsum(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF, vert, 20 + base);
    print_grid(3, &grid_c[0][0]);
    check_vredsum(vert);

    printf("\n--- TEST %d: vredmax.vs (%s, tag=%d) ---\n", 21 + base, mode, 21 + base);
    clear_grid_c();
    k_vredmax(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF, vert, 21 + base);
    print_grid(3, &grid_c[0][0]);
    check_vredmax(vert);

    printf("\n--- TEST %d: vslideup.vx by %d (%s, tag=%d) ---\n",
           22 + base, VSLIDE_SMALL, mode, 22 + base);
    clear_grid_c();
    k_vslideup(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF,
               VSLIDE_SMALL, vert, 22 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vslideup_v("vslideup.vx", VSLIDE_SMALL, 0);
    else      check_vslideup_h("vslideup.vx", VSLIDE_SMALL, 0);

    printf("\n--- TEST %d: vslideup.vx by %d (%s, tag=%d) ---\n",
           23 + base, VSLIDE_BIG, mode, 23 + base);
    clear_grid_c();
    k_vslideup(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF,
               VSLIDE_BIG, vert, 23 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vslideup_v("vslideup.vx", VSLIDE_BIG, 0);
    else      check_vslideup_h("vslideup.vx", VSLIDE_BIG, 0);

    printf("\n--- TEST %d: vslidedown.vx by %d (%s, tag=%d) ---\n",
           24 + base, VSLIDE_SMALL, mode, 24 + base);
    clear_grid_c();
    k_vslidedown(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF,
                 VSLIDE_SMALL, vert, 24 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vslidedown_v("vslidedown.vx", VSLIDE_SMALL, 0);
    else      check_vslidedown_h("vslidedown.vx", VSLIDE_SMALL, 0);

    printf("\n--- TEST %d: vslidedown.vx by %d (%s, tag=%d) ---\n",
           25 + base, VSLIDE_BIG, mode, 25 + base);
    clear_grid_c();
    k_vslidedown(&grid_a[0][0], &grid_c[0][0], VL, ITERS, PERF,
                 VSLIDE_BIG, vert, 25 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vslidedown_v("vslidedown.vx", VSLIDE_BIG, 0);
    else      check_vslidedown_h("vslidedown.vx", VSLIDE_BIG, 0);

    printf("\n--- TEST %d: vslideup.vi by %d (%s, tag=%d) ---\n",
           26 + base, VSLIDE_SMALL, mode, 26 + base);
    clear_grid_c();
    k_vslideup_vi1(&grid_b[0][0], &grid_c[0][0], VL, ITERS, PERF,
                   vert, 26 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vslideup_v("vslideup.vi", VSLIDE_SMALL, 1);
    else      check_vslideup_h("vslideup.vi", VSLIDE_SMALL, 1);

    printf("\n--- TEST %d: vslidedown.vi by %d (%s, tag=%d) ---\n",
           27 + base, VSLIDE_SMALL, mode, 27 + base);
    clear_grid_c();
    k_vslidedown_vi1(&grid_b[0][0], &grid_c[0][0], VL, ITERS, PERF,
                     vert, 27 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vslidedown_v("vslidedown.vi", VSLIDE_SMALL, 1);
    else      check_vslidedown_h("vslidedown.vi", VSLIDE_SMALL, 1);

    printf("\n--- TEST %d: vslideup.vi by %d (%s, tag=%d) ---\n",
           28 + base, VSLIDE_IMM_BIG, mode, 28 + base);
    clear_grid_c();
    k_vslideup_vi31(&grid_b[0][0], &grid_c[0][0], VL, ITERS, PERF,
                    vert, 28 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vslideup_v("vslideup.vi", VSLIDE_IMM_BIG, 1);
    else      check_vslideup_h("vslideup.vi", VSLIDE_IMM_BIG, 1);

    printf("\n--- TEST %d: vslidedown.vi by %d (%s, tag=%d) ---\n",
           29 + base, VSLIDE_IMM_BIG, mode, 29 + base);
    clear_grid_c();
    k_vslidedown_vi31(&grid_b[0][0], &grid_c[0][0], VL, ITERS, PERF,
                      vert, 29 + base);
    print_grid(2, &grid_c[0][0]);
    if (vert) check_vslidedown_v("vslidedown.vi", VSLIDE_IMM_BIG, 1);
    else      check_vslidedown_h("vslidedown.vi", VSLIDE_IMM_BIG, 1);
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

    printf("\n--- TEST %d: vlse8.v stride2 (%s, tag=%d) ---\n", 5 + base, mode, 5 + base);
    clear_grid_c();
    k_vlse_stride2(&grid_a[0][0], &grid_c[0][0],
                   STRIDE2_PIXELS, ITERS, PERF, vert, 5 + base);
    print_grid(2, &grid_c[0][0]);
    check_vlse_stride2(vert);

    printf("\n--- TEST %d: vsse8.v stride2 (%s, tag=%d) ---\n", 6 + base, mode, 6 + base);
    clear_grid_c();
    k_vsse_stride2(&grid_a[0][0], &grid_c[0][0],
                   STRIDE2_PIXELS, ITERS, PERF, vert, 6 + base);
    print_grid(2, &grid_c[0][0]);
    check_vsse_stride2(vert);
}

void test(void) {
    init_grids();

    printf("=== benchmark_instructions: %d back-to-back iters per (instruction, mode) ===\n",
           ITERS);
    printf("[INIT] grid_a[r][c] = (r+c)&127; grid_b[r][c] = (r*2+c)&127\n");
    printf("[INIT] idx_grid[r][c] = (r+c+1)&127; diag_buf[r][c] = (r==c)?1:0\n");

    printf("\n############### HORIZONTAL MODE ###############\n");
    // run_compute_block(/*vert=*/0);
    // run_lsu_block    (/*vert=*/0);

    printf("\n############### VERTICAL MODE ###############\n");
    run_compute_block(/*vert=*/1);
    run_lsu_block    (/*vert=*/1);

    printf("\n=== benchmark_instructions summary: %s (%d/%d checks passed, %d failed) ===\n",
           g_check_failed ? "FAIL" : "PASS",
           g_check_total - g_check_failed, g_check_total, g_check_failed);
    printf("\n=== benchmark_instructions done ===\n");
}
