#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

/*****************************************************************************
 * benchmark_instructions_lmul1
 *
 * 1:1 LMUL=1 port of sibling `benchmark_instructions`. The ONLY hardware-
 * visible change vs the LMUL=4 reference is `m4` -> `m1` in every kernel's
 * vsetvli. Same 29 compute + 6 LSU kernels, same H+V test driver, same
 * scalar checkers. Targets the wide-VRF "big" configs
 * (mudkip2d128big1bram1chain2lanescale*) where vLen=1024 makes one LMUL=1
 * register hold vl=128 elements at SEW=8 = one full image row.
 *
 * Why this is a useful regression on top of the LMUL=4 backcompat:
 *   - LMUL=4 backcompat only exercises 8 register groups (v0/4/8/12/16/20/
 *     24/28); LMUL=1 exercises any vN with no group-base alignment.
 *   - vrgather/mask/vmv-family read v0 through the SharedVRF scatter just
 *     like any operand; at LMUL=1, vrfOffsetBits widens from 1 -> 3 and
 *     cVsOffBits collapses from 2 -> 0. This test stresses both.
 *   - Verifies Spike difftest agrees byte-for-byte at vLen=1024 with
 *     LMUL=1 vsetvli.
 *
 * Programmer rules R1..R8 from benchmark_vadd.c apply verbatim. Kernels
 * are naked + noinline + no internal C calls; place_counter is encoded as
 * `sw <tag>, 0(<perf>)` inside the asm block.
 *
 * Counter tag map (must match INSTRUCTION_ORDER in plot_bench_cycles.py if
 * you want the grouped H-vs-V bar chart):
 *   1..29  H compute (slot 1..29)
 *  30..35  H LSU     (slot 1..6 within LSU block)
 *  36..64  V compute
 *  65..70  V LSU
 *****************************************************************************/

#define ROWS 128
#define COLS 128
#define ITERS 2
#define PERF_REG_ADDR 0x10000014

#define VRGATHER_VX_INDEX  5
#define VMV_S_X_VALUE     42
#define VMV_X_S_INDEX      5
#define VSLIDE_SMALL       1
#define VSLIDE_BIG       100
#define VSLIDE_IMM_BIG    31
#define VMSGT_THRESHOLD   64
#define STRIDE2_PIXELS    64
#define STRIDE2_BYTES      2

#define N_COMPUTE   29
#define N_LSU        6
#define N_PER_MODE  (N_COMPUTE + N_LSU)

int8_t grid_a[ROWS][COLS];
int8_t grid_b[ROWS][COLS];
int8_t grid_c[ROWS][COLS];
int8_t idx_grid[ROWS][COLS];     /* indices for vrgather.vv */
int8_t diag_buf[ROWS][COLS];     /* (r == c) ? 1 : 0; mask source */

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
 * Naked kernels - LMUL=1 (`m1`) versions of every benchmark_instructions
 * kernel. The kernel-by-kernel arg signatures match the reference exactly
 * so the same C-side driver can call them.
 *
 * NOTE on register choices: at LMUL=1 every vN is its own register group.
 * The original LMUL=4 kernels use v8 (input1), v12 (input2/idx), v16
 * (dst), v20 (intermediate) - all aligned LMUL=4 group bases. We keep
 * the SAME register IDs here for diff-clarity; at LMUL=1 they are simply
 * single-register groups with no alignment constraint.
 *==========================================================================*/

/* TEST {1,36} - vadd.vv */
__attribute__((naked, noinline))
void k_vadd(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
            uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
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

/* TEST {2,37} - vmul.vv */
__attribute__((naked, noinline))
void k_vmul(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
            uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
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

/* TEST {3,38} - vmacc.vv: vd += vs1 * vs2 */
__attribute__((naked, noinline))
void k_vmacc(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
             uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
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

/* TEST {4,39} - vmadd.vv: vd = vs1 * vd + vs2 */
__attribute__((naked, noinline))
void k_vmadd(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
             uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
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

/* TEST {5,40} - vand.vv */
__attribute__((naked, noinline))
void k_vand(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
            uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
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

/* TEST {6,41} - vor.vv */
__attribute__((naked, noinline))
void k_vor(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
           uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
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

/* TEST {7,42} - vsll.vi */
__attribute__((naked, noinline))
void k_vsll(int8_t *a, int8_t *c, size_t vl, int iters,
            uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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

/* TEST {8,43} - vsra.vi */
__attribute__((naked, noinline))
void k_vsra(int8_t *a, int8_t *c, size_t vl, int iters,
            uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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

/* TEST {9,44} - vmseq.vv; stored as 0/1 bytes for checking. */
__attribute__((naked, noinline))
void k_vmseq(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
             uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
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

/* TEST {10,45} - vmsle.vv */
__attribute__((naked, noinline))
void k_vmsle(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
             uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
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

/* TEST {11,46} - vmsgt.vx */
__attribute__((naked, noinline))
void k_vmsgt(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
             uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a6                  \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
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

/* TEST {12,47} - vmslt.vv */
__attribute__((naked, noinline))
void k_vmslt(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
             uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
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

/* TEST {13,48} - vmand.mm */
__attribute__((naked, noinline))
void k_vmand(int8_t *a, int8_t *b, int8_t *c, size_t vl, int iters,
             uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a6                  \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
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

/* TEST {14,49} - vrgather.vx */
__attribute__((naked, noinline))
void k_vrgather_vx(int8_t *a, int8_t *c, size_t vl, int iters,
                   uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "li      t3, 5                      \n\t"
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

/* TEST {15,50} - vrgather.vx + diag v0.t mask (mu policy) */
__attribute__((naked, noinline))
void k_vrgather_vx_mask(int8_t *a, int8_t *diag, int8_t *c, size_t vl,
                        int iters, uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a6                  \n\t"
        "vsetvli zero, a3, e8, m1, ta, mu   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v20, (a1)                  \n\t"
        "vmsne.vi v0, v20, 0                \n\t"
        "vmv.v.v v16, v8                    \n\t"
        "li      t3, 5                      \n\t"
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

/* TEST {16,51} - vrgather.vv */
__attribute__((naked, noinline))
void k_vrgather_vv(int8_t *a, int8_t *idx, int8_t *c, size_t vl,
                   int iters, uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a6                  \n\t"
        "vsetvli zero, a3, e8, m1, ta, ma   \n\t"
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

/* TEST {17,52} - vmv.s.x */
__attribute__((naked, noinline))
void k_vmv_s_x(int8_t *c, size_t vl, int iters, uintptr_t perf,
               int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a4                  \n\t"
        "vsetvli zero, a1, e8, m1, ta, ma   \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "li      t3, 42                     \n\t"
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

/* TEST {18,53} - vmv.x.s after vslidedown.vi setup
 * NOTE: Same known limitation as the LMUL=4 reference - the C-side
 * verifier expects "10" but the hardware extracts the byte that vmv.x.s
 * actually produces. Spike difftest will agree with DUT; the [CHECK] FAIL
 * is a verifier-formula issue, not a hardware regression. */
__attribute__((naked, noinline))
void k_vmv_x_s(int8_t *b, int8_t *c, size_t vl, int iters,
               uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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
        "vle8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* TEST {19,54} - vmv.v.v */
__attribute__((naked, noinline))
void k_vmv_v_v(int8_t *a, int8_t *c, size_t vl, int iters,
               uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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

/* TEST {20,55} - vredsum.vs */
__attribute__((naked, noinline))
void k_vredsum(int8_t *a, int8_t *c, size_t vl, int iters,
               uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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

/* TEST {21,56} - vredmax.vs */
__attribute__((naked, noinline))
void k_vredmax(int8_t *a, int8_t *c, size_t vl, int iters,
               uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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

/* TEST {22..25, 57..60} - vslideup.vx / vslidedown.vx
 * args: a, c, vl, iters, perf, offset, vert, tag */
__attribute__((naked, noinline))
void k_vslideup(int8_t *a, int8_t *c, size_t vl, int iters,
                uintptr_t perf, int offset, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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

__attribute__((naked, noinline))
void k_vslidedown(int8_t *a, int8_t *c, size_t vl, int iters,
                  uintptr_t perf, int offset, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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

/* TEST {26,27,28,29, 61,62,63,64} - vslideup.vi / vslidedown.vi */
__attribute__((naked, noinline))
void k_vslideup_vi1(int8_t *a, int8_t *c, size_t vl, int iters,
                    uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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

__attribute__((naked, noinline))
void k_vslidedown_vi1(int8_t *a, int8_t *c, size_t vl, int iters,
                      uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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

/* LSU block (slots 30..35 H, 65..70 V) */

/* vle8.v full */
__attribute__((naked, noinline))
void k_vle_full(int8_t *a, int8_t *c, size_t vl, int iters,
                uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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

/* vle8.v masked - mask built under the active CSR mode (no H-mode op
 * between mask build and the timed masked vle). */
__attribute__((naked, noinline))
void k_vle_masked(int8_t *a, int8_t *c, size_t vl, int iters,
                  uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m1, ta, mu   \n\t"
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

/* vse8.v full */
__attribute__((naked, noinline))
void k_vse_full(int8_t *a, int8_t *c, size_t vl, int iters,
                uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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

/* vse8.v masked - mask built in H, store in active CSR mode */
__attribute__((naked, noinline))
void k_vse_masked(int8_t *a, int8_t *c, size_t vl, int iters,
                  uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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

/* vlse8.v stride2 */
__attribute__((naked, noinline))
void k_vlse_stride2(int8_t *a, int8_t *c, size_t vl, int iters,
                    uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/* vsse8.v stride2 */
__attribute__((naked, noinline))
void k_vsse_stride2(int8_t *a, int8_t *c, size_t vl, int iters,
                    uintptr_t perf, int vert, int tag) {
    __asm__ volatile (
        "csrw    0x7c0, a5                  \n\t"
        "vsetvli zero, a2, e8, m1, ta, ma   \n\t"
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
 * Result checkers (verbatim from sibling benchmark_instructions.c, since the
 * memory-side semantics are LMUL-independent at vl=128 / SEW=8).
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

__attribute__((noinline))
static void check_vrgather_vx_v(void) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      a_val(VRGATHER_VX_INDEX, c));
    cs_report(&s, "vrgather.vx", "V", "all cells == grid_a[rs1=5][c]");
}

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

__attribute__((noinline))
static void check_vrgather_vv_h(void) {
    CheckState s; cs_init(&s);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            cs_record(&s, r, c, grid_c[r][c],
                      (int8_t)((2 * r + c + 1) & (COLS - 1)));
    cs_report(&s, "vrgather.vv", "H", "grid_c == (2r+c+1)&127");
}

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
        printf("[CHECK] PASS %s by %d (H): col c<%d zero, rest = grid_%c[r][c-%d]\n",
               op, off, off, use_b ? 'b' : 'a', off);
    } else {
        printf("[CHECK] FAIL %s by %d (H): %d errors; first at [%d][%d] got %d exp %d\n",
               op, off, s.errors, s.br, s.bc, s.bgot, s.bexp);
    }
}

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
        printf("[CHECK] PASS %s by %d (V): row r<%d zero, rest = grid_%c[r-%d][c]\n",
               op, off, off, use_b ? 'b' : 'a', off);
    } else {
        printf("[CHECK] FAIL %s by %d (V): %d errors; first at [%d][%d] got %d exp %d\n",
               op, off, s.errors, s.br, s.bc, s.bgot, s.bexp);
    }
}

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
        printf("[CHECK] PASS %s by %d (H): col c>=%d zero, rest = grid_%c[r][c+%d]\n",
               op, off, COLS - off, use_b ? 'b' : 'a', off);
    } else {
        printf("[CHECK] FAIL %s by %d (H): %d errors; first at [%d][%d] got %d exp %d\n",
               op, off, s.errors, s.br, s.bc, s.bgot, s.bexp);
    }
}

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
        printf("[CHECK] PASS %s by %d (V): row r>=%d zero, rest = grid_%c[r+%d][c]\n",
               op, off, ROWS - off, use_b ? 'b' : 'a', off);
    } else {
        printf("[CHECK] FAIL %s by %d (V): %d errors; first at [%d][%d] got %d exp %d\n",
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
 * Test driver - same orchestration as benchmark_instructions.c
 *==========================================================================*/

#define PERF ((uintptr_t)PERF_REG_ADDR)
#define VL   ((size_t)COLS)

static void run_compute_block(int vert) {
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
    k_vmv_x_s(&grid_b[0][0], &grid_c[0][0], VL, ITERS, PERF, vert, 18 + base);
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

    printf("=== benchmark_instructions_lmul1: %d back-to-back iters per (instruction, mode) ===\n",
           ITERS);
    printf("[CONFIG] SEW=8 / LMUL=1 / vl=%d, vLen=1024 (zvl1024b)\n", COLS);
    printf("[INIT] grid_a[r][c] = (r+c)&127; grid_b[r][c] = (r*2+c)&127\n");
    printf("[INIT] idx_grid[r][c] = (r+c+1)&127; diag_buf[r][c] = (r==c)?1:0\n");

    /* Match the reference's H-disabled / V-only orchestration. Uncomment
     * both run_*_block(0) lines if you want H-mode coverage too. */
    printf("\n############### HORIZONTAL MODE ###############\n");
    run_compute_block(/*vert=*/0);
    run_lsu_block    (/*vert=*/0);

    printf("\n############### VERTICAL MODE ###############\n");
    run_compute_block(/*vert=*/1);
    run_lsu_block    (/*vert=*/1);

    printf("\n=== benchmark_instructions_lmul1 summary: %s (%d/%d checks passed, %d failed) ===\n",
           g_check_failed ? "FAIL" : "PASS",
           g_check_total - g_check_failed, g_check_total, g_check_failed);
    printf("\n=== benchmark_instructions_lmul1 done ===\n");
}
