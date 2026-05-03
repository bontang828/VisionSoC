#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

/*****************************************************************************
 * VisionSoC vadd benchmark suite
 *
 * Hardware model
 * --------------
 *  This SoC uses the RISC-V V extension in a non-standard way: every vector
 *  instruction is broadcast to a 128-row time-multiplexed grid, and each of
 *  those rows independently re-executes the same instruction with a per-row
 *  base-address offset for memory ops (row r reads/writes from
 *  base + r * COLS). One vle8.v / vse8.v therefore touches a full
 *  ROWS x COLS image, NOT just a single row.
 *
 *  Recommended vsetvli for full-grid kernels:
 *    vsetvli zero, COLS(=128), e8, m4, ta, ma
 *  This makes vl = 128 = COLS, so v8..v11 (LMUL=4) holds exactly one
 *  image row in each of the 128 rows of the grid.
 *
 *  Vertical mode is toggled via custom CSR 0x7c0 (T1's verticalMode input).
 *  When 1, vle8/vse8 transposes the access pattern (each row reads a column
 *  of the source image), and vslideup/vslidedown shifts in the vertical
 *  direction. On t1emu Spike just stores the CSR value (it is registered as
 *  a basic CSR in spike_rs/runner.rs); the DPI driver mirrors it to the DUT.
 *
 *  Perf counters are MMIO at 0x10000014. A non-zero write starts a counter
 *  with that tag, a zero write stops it. The simulator prints a line per
 *  edge: "[PERF] counter <N> START at cycle <c>" or "[PERF] counter STOP".
 *  Subtract START from the next STOP to get the cycle count for that span.
 *  Use T1_MIRROR_RTL_WRITES=1 in the env to make printf reach the terminal.
 *
 * Programmer rules (DO NOT IGNORE)
 * --------------------------------
 *  R1. NO AUTO-VECTORISATION OF NON-KERNEL CODE.
 *      The compiler at -O2 happily turns scalar init loops into LMUL=8
 *      vector code (e.g. vid.v + vse8.v). Such code does not have the
 *      same semantics on the 2D grid (each row would generate the same
 *      vid.v sequence and write it ROWS times to the same destination).
 *      Defeat auto-vec by:
 *        - dereferencing through `volatile int8_t *` in init code, or
 *        - using inline asm for any data setup that the compiler might
 *          notice as vectorisable.
 *      Do NOT rely on `-fno-tree-vectorize` from the build system - the
 *      Nix builder enforces -O2 and we have no per-file flag override.
 *
 *  R2. NO VECTOR REGISTER SPILLS IN VECTOR KERNELS.
 *      The hardware fans out vs1r.v / vl1r.v (whole-register save/restore
 *      that the compiler emits at function-call boundaries) across all
 *      ROWS rows of the grid. A single vs1r.v therefore writes
 *      vlenb * ROWS bytes (NOT vlenb bytes), so the stack frame the
 *      compiler reserves (it sized using a single `csrr vlenb`) is
 *      ROWS-times too small. The result is a saved-RA overwrite and an
 *      illegal-instruction trap on return - exactly what simple_vadd.c
 *      hit before this benchmark was written. Symptoms in the run log:
 *        [PERF] counter STOP at cycle <N>
 *        Error: spike trapped with trap_illegal_instruction (tval=0...)
 *
 *      To avoid spills entirely:
 *        - put every vector instruction sequence in an
 *          `__attribute__((naked, noinline))` function written in pure
 *          inline asm,
 *        - never call another C function from inside that kernel
 *          (place_counter/printf/anything) - the call boundary is what
 *          forces the compiler to spill live vector temporaries,
 *        - if you must instrument inside the kernel, encode the
 *          place_counter MMIO write directly as a `sw` in the same
 *          inline asm block (an example is grid_vadd_per_iter below).
 *
 *  R3. CALL-BOUNDARY SPILLS - a clearer fix than enlarging the stack.
 *      Even if you make __stacktop / SP huge, the compiler-emitted spill
 *      DESTINATIONS overlap (v8 at sp+0, v9 at sp+vlenb), and each spill
 *      writes vlenb*ROWS bytes, so v9's region is clobbered by v8's
 *      fan-out. Enlarging the stack by ROWS-times only delays the
 *      collision; it does not fix data corruption between live registers.
 *      The only non-LLVM-invasive fixes are:
 *        a) eliminate the spill (R2) - preferred,
 *        b) save/restore explicitly through vse8.v / vle8.v to per-vreg
 *           buffers in .vdata, each sized ROWS*COLS bytes per LMUL=1
 *           register (see vsave_v8 below for the pattern). vse8/vle8 are
 *           2D-fabric-aware and write the right number of bytes.
 *      Other ideas that were considered and rejected:
 *        - "shadow vector stack" via `gp` and explicit vse8/vle8: works
 *          but requires every spill site to be hand-coded; only useful
 *          if you have many vector live ranges across calls. Not used
 *          here because (R2) avoids the situation.
 *        - asking LLVM to treat vlenb as ROWS*real_vlenb: would need a
 *          custom backend; not practical for this project.
 *
 *  R4. LMUL CHOICE.
 *      All kernels in this file use SEW=8, LMUL=4, vl=128. That gives
 *      one register group = one image row. Larger LMUL exists in the
 *      ISA but the SharedVRF banking in this design assumes LMUL <= 4
 *      for the 2D path. Reaching for LMUL=8 silently produces wrong
 *      behaviour (the compiler does this in init - see R1).
 *
 *  R5. REGISTER GROUP DISJOINT-NESS.
 *      vrgather.vv forbids overlap between source and destination
 *      groups. With LMUL=4 the legal groups are v0/v4/v8/v12/v16/v20/
 *      v24/v28; pick three different ones for src, idx, dst (this file
 *      uses v8, v12, v16 for vadd-style kernels, and v8/v16/v12 for
 *      gather, matching simple_instruction_gather).
 *
 * What the benchmarks measure
 * ---------------------------
 *  TEST 1 - Per-iteration vadd cost
 *    One vle8 of grid_a + one vle8 of grid_b are issued ONCE outside the
 *    measured region. Then ITERS=100 (place_counter, vadd.vv, place_counter)
 *    triples are issued back-to-back. The simulator emits 100 START/STOP
 *    pairs. Steady-state per-vadd cycles = STOP[i] - START[i].
 *    The store happens once after the loop. Counter tag = 1.
 *
 *  TEST 2 - Amortised vadd cost (load/store overhead removed)
 *    Same kernel, but only ONE counter span wraps the whole 100-vadd
 *    burst (single START tag=2, single STOP). Average per vadd =
 *    (STOP - START) / ITERS. Should match TEST 1 once the per-iteration
 *    counter MMIO writes are subtracted.
 *
 *  TEST 3 - CV: full-frame brightness boost (vsadd.vx, saturating)
 *    grid_c = sat_i8(grid_a + boost). Demonstrates a vector-scalar op,
 *    which is the per-pixel form most CV pipelines use. Counter tag = 3.
 *
 *  TEST 4 - CV: 3-tap horizontal box blur using vslide
 *    grid_c[r][c] = (grid_a[r][c-1] + grid_a[r][c] + grid_a[r][c+1]) >> 2
 *    Implemented with vslideup / vslidedown to access neighbours within a
 *    row, then vadd + vsra. Edges decay toward zero (slide fills with 0).
 *    Counter tag = 4.
 *
 *  TEST 5 - CV: 3-tap vertical box blur (vertical mode via CSR 0x7c0)
 *    Same kernel as TEST 4, but vertical mode is enabled, so the slides
 *    move data between vertically-adjacent pixels (different rows of the
 *    image). Counter tag = 5.
 *
 *  TEST 6 - CV: per-row reduction (matvec sketch)
 *    grid_c[r][0] = sum over c of grid_a[r][c], using vredsum.vs.
 *    This is the building block of a matvec (each row computes a dot
 *    product into element 0 of its own register). Counter tag = 6.
 *
 * Mode rules (LSU vs compute) - read this BEFORE designing kernels
 * ----------------------------------------------------------------
 *  R6. LOAD/STORE IS ALWAYS HORIZONTAL.
 *      vle8.v / vse8.v always fan out to row-stride access regardless of
 *      CSR 0x7c0. This is enforced inside SharedVRF.scala, where the
 *      vertical permutation is gated by `!isLSUInst(...)` - LSU traffic
 *      stays on the horizontal bank path so a vle followed by a vse
 *      can round-trip data unchanged. CSR 0x7c0 only re-routes
 *      lane-side compute (slides, arithmetic, reductions, vrgather, etc.).
 *      Consequence: there is no "vertical-mode load" / "vertical-mode
 *      store" - any algorithm that wanted to use them (e.g. transpose
 *      via H-load + V-store, or a column-stride load) must be
 *      re-expressed as a compute-only mode switch. The four
 *      RESERVED tests at the bottom of this file are the H/V LSU
 *      flips that are NOT supported today; keep them around as a
 *      sketch of what would unlock once vertical LSU lands.
 *
 *  R7. v0 MASK IS ALSO MODE-DEPENDENT.
 *      The mask register v0 is read through the same VRF path as any
 *      other vector source, so a vector instruction issued under
 *      vertical mode reads v0 in the vertical view (element index
 *      indexes image rows, lane index indexes image columns - the
 *      "transpose" of horizontal). A mask designed to look like
 *      "every other column" in horizontal mode therefore presents
 *      as "every other row" in vertical mode. If you build v0 with
 *      vmseq/vmslt etc. under one mode and then consume it under the
 *      other, expect the mask to change shape. Build masks with the
 *      mode you intend to use them under, or build the same logical
 *      pattern twice (once per mode) and switch at use time.
 *
 *  R8. INTRA-INSTRUCTION CONSISTENCY.
 *      Every operand of a single instruction is read through the same
 *      mode-aware path on the cycle the instruction issues, so mixing
 *      registers that were written under different modes inside one
 *      vadd/vmul is fine - the read transposes them into the active
 *      view consistently. The dangerous case is reading a value back
 *      via vse8 (always horizontal): the bytes you see in memory are
 *      the bytes physically stored after the previous write went
 *      through the active mode's scatter, so a value computed in
 *      vertical mode and then stored sees the inverse permutation -
 *      the result in memory matches "what you would have got from
 *      the same compute in horizontal mode applied to the
 *      transposed image". simple_instruction_vert_hori is the
 *      canonical example: vle8 (H) -> vslideup.vi (V compute) -> vse8 (H)
 *      lands a vertical-direction shift in row-major memory.
 *
 * Mode-aware tests (interleaved H + V compute, no LSU mode flip)
 * --------------------------------------------------------------
 *  TEST 7 - Full grid scalar reduction, all-vector finalisation
 *    Pass 1 (horizontal compute): vredsum.vs reduces each image row into
 *      element 0 of v12 in the corresponding hardware row.
 *      v12 layout: v12[0] of hw-row r = row_sum[r];
 *                  v12[1..127] of hw-row r = vredsum tail (undefined).
 *    Pass 2 (horizontal compute): mask-clean v12 so the garbage tail
 *      doesn't poison the next reduction. vid.v + vmseq.vi builds
 *      v0 = (i == 0), then vmerge.vvm with a zero source produces
 *      v24 = [rs[r], 0, 0, ..., 0] in each hw-row. (We clear instead
 *      of masking the next reduction so we don't have to reason about
 *      v0 changing meaning when read in vertical mode - see R7.)
 *    Pass 3 (vertical compute): vredsum.vs in vertical mode reduces v24
 *      across hardware rows. Vertical lane 0 sees [rs0, rs1, ..., rs127]
 *      (the meaningful column of v24, exposed by the vertical view's
 *      transpose), so its reduction = grid total. Vertical lanes 1..127
 *      see all-zero columns, so their reductions = 0.
 *    Pass 4 (extract): vmv.x.s reads element 0 of vd back to the scalar
 *      core. In the 2D fabric the natural origin is hw-row 0 element 0,
 *      which holds vertical-lane-0's result, so this returns the grid
 *      total directly. The kernel writes it as a 32-bit word to
 *      *result so the C wrapper can printf it without re-loading any
 *      strided byte buffer. Counter tag = 7.
 *
 *  TEST 8 - 3x3 separable box blur, single LSU round-trip
 *    Pure compute mode flip. v8 holds the image (one H load), then:
 *      (a) horizontal compute: vslideup/vslidedown within each row to
 *          form left/right neighbours; vadd to get the 3-tap horizontal
 *          sum h_sum in v20.
 *      (b) vertical compute: csrw 0x7c0, 1; vslideup/vslidedown on v20
 *          now move data between vertically-adjacent rows of the image
 *          (via the VRF scatter); vadd folds in the above/below
 *          neighbours of h_sum to give the full 9-tap box sum.
 *      (c) horizontal store: csrw 0x7c0, 0; vse8 writes the result
 *          row-major. Because LSU is horizontal-only, the vertical
 *          slide's effect ends up correctly placed in row-major memory
 *          (see R8). Boundaries see 0-fill from the slides.
 *    Counter tag = 8.
 *
 *  TEST 9 - Matrix-vector multiply (matvec) y = A * x
 *    100% horizontal-mode kernel. Every hardware row r naturally holds
 *    A[r][:] after a horizontal vle8, and x is pre-replicated into
 *    x_replicated[ROWS][COLS] so a second vle8 puts x into every row.
 *    vmul.vv + vredsum.vs gives y[r] in element 0 of each hardware row;
 *    vse8 with vl=1 writes y[r] to result[r*COLS]. The strided result
 *    is finalised in C (a 128-byte read with stride 128). Note: i8*i8
 *    -> i8 saturates with realistic values; switch to vwmul.vv +
 *    vwredsum.vs if you need full precision. Counter tag = 9.
 *
 *  TEST 10 - Mode-aware mask: column-stripe vs row-stripe via v0
 *    Demonstrates R7. We build v0 under horizontal compute as the mask
 *    "even element indices = 1" (every other COLUMN of the image).
 *    Applied with vadd.vv,v0.t in horizontal mode, only even-column
 *    pixels are updated - a column-stripe pattern.
 *    We then re-build v0 under vertical compute with the same
 *    "even element indices = 1" pattern, but vertical's element index
 *    is the image-row, so this masks every other ROW of the image.
 *    Applied under vertical compute the result is a row-stripe pattern.
 *    Same vmseq.vi recipe, two different selectivity patterns - the
 *    test prints both outputs for visual confirmation. Counter tag = 10.
 *
 * Open investigations (observed during a run, NOT settled hardware rules)
 * -----------------------------------------------------------------------
 *  P1. vredsum.vs apparently writes only hw-row 0's vd[0].
 *      In TEST 6, after `vmv.v.i v12, 0; vredsum.vs v12, v8, v12`, only
 *      grid_c[0][0] held a real row sum (-64); rows 1..7 stayed at 0
 *      (the cleared default). TEST 7's vector-only grid total returned
 *      -64 instead of the expected 0 (= grid total mod 256), and TEST 9
 *      matvec produced a correct y[0]=28 with y[1..7]=0. The pattern is
 *      consistent with "vredsum.vs is a single-scalar op and the 2D
 *      fabric materialises only one of the 128 candidate results, that
 *      of hw-row 0".
 *      To investigate:
 *        - confirm by reading t1/src/laneStage/MaskExchangeUnit.scala
 *          (reduceResultOut is 1 scalar wire; check whether the fabric
 *          fans out the result to all hw-rows' vd[0] or just one);
 *        - try an explicit vmv.v.x broadcast of the scalar back into
 *          v12 to see whether all hw-rows get populated.
 *      To work around (not yet implemented in this file):
 *        - per-row reduction: 7 steps of vslidedown.vi/.vx + vadd
 *          within the register, leaving each hw-row's row sum in
 *          element 0;
 *        - grid total: 7 steps of vertical-mode vslidedown.vi/.vx +
 *          vadd to collapse all hw-rows into hw-row 0, then a single
 *          horizontal vredsum.vs gives the grid sum.
 *      See fyp_doc/2d_fabric_handoff.md for the broader context.
 *
 *  P2. Mask v0 built under vertical mode behaves unexpectedly.
 *      In TEST 10, the second pass (mask built and applied entirely in
 *      vertical compute) produced an output identical to the input - no
 *      pixels were boosted. The first pass (horizontal compute) produced
 *      the expected column-stripe. The vertical mask seemingly evaluated
 *      to all-zero from the vadd's perspective (or all-one with the .vt
 *      polarity inverted - either way nothing got the +50). This is the
 *      same family of issue R7 warns about, but the *exact* layout of
 *      v0 in vertical view is what needs to be characterised before we
 *      can build a working vertical-row-stripe mask.
 *      To investigate:
 *        - dump the v0 bytes after each construction step (vid.v ->
 *          vand.vi -> vmseq.vi) using vse8 of v0 alongside;
 *        - read t1/src/mask/MaskUnit.scala for how v0 is consumed under
 *          verticalMode (look for `verticalMode` references in the mask
 *          read path).
 *
 * RESERVED tests (require vertical-mode LSU support; not yet implemented)
 * ----------------------------------------------------------------------
 *  R-TEST 7 - Full grid sum via H-reduce store + V-load + V-reduce.
 *  R-TEST 8 - Image transpose via H-load + V-store.
 *  R-TEST 10 - 3x3 box blur with H-load, H-store of intermediate, then
 *              V-load + V-store of result.
 *  See the bottom of the file for the kernel sketches. They are
 *  retained as design notes for when the LSU gains a vertical mode.
 *
 * For incoming Claude Code sessions: read fyp_doc/2d_fabric_handoff.md
 * BEFORE writing any new vector kernel. This is not stock RVV - many
 * intuitions from textbook RVV programming (per-lane independence,
 * predictable mask semantics, scalar-output reductions producing
 * one scalar period) bend or break here.
 *****************************************************************************/

#define ROWS  128
#define COLS  128
#define ITERS 100

#define PERF_REG_ADDR 0x10000014

int8_t grid_a[ROWS][COLS];
int8_t grid_b[ROWS][COLS];
int8_t grid_c[ROWS][COLS];

// TEST 9 (matvec): pre-replicated input vector. Each row is a copy of the
// same 128-element x[], so a horizontal vle8 puts x into every hardware row.
__attribute__((aligned(128)))
int8_t x_replicated[ROWS][COLS];

// TEST 10 (mask demo): two-grid output buffer. First grid (rows 0..127) holds
// the horizontal-mask result, second grid (rows 128..255) holds the
// vertical-mask result.
__attribute__((aligned(128)))
int8_t mask_out[ROWS * 2][COLS];

// TEST 7 (full grid sum): scalar landing slot for the kernel's vmv.x.s + sw.
__attribute__((aligned(4)))
int grid_total_word;

/*--- init / debug helpers (scalar; volatile defeats auto-vectorisation) ---*/

__attribute__((noinline))
static void init_grids(void) {
    volatile int8_t *pa = (volatile int8_t *)&grid_a[0][0];
    volatile int8_t *pb = (volatile int8_t *)&grid_b[0][0];
    volatile int8_t *pc = (volatile int8_t *)&grid_c[0][0];
    for (int i = 0; i < ROWS; i++) {
        for (int j = 0; j < COLS; j++) {
            pa[i * COLS + j] = (int8_t)((i + j) & (COLS - 1));
            pb[i * COLS + j] = (int8_t)((i * 2) & (COLS - 1));
            pc[i * COLS + j] = 0;
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

static void print_grid(int num_rows, int8_t *grid) {
    printf("Printing first %d rows of the grid:\n", num_rows);
    for (int i = 0; i < num_rows; i++) {
        for (int j = 0; j < COLS; j++) {
            printf("%d ", grid[i * COLS + j]);
        }
        printf("\n");
    }
    if (num_rows < ROWS) {
        printf("...\n");
    }
}

/*--- vector kernels (all naked, no internal calls, no spills) ---*/

/*
 * TEST 1 kernel: per-iteration counter wrap.
 *
 *   a0 = grid_a base, a1 = grid_b base, a2 = grid_c base, a3 = vl,
 *   a4 = ITERS, a5 = PERF_REG_ADDR.
 *
 *   vsetvli once
 *   vle8 v8 = a; vle8 v12 = b
 *   loop ITERS times:
 *     sw 1, 0(a5)             // place_counter(1) START
 *     vadd.vv v16, v8, v12
 *     sw 0, 0(a5)             // place_counter(0) STOP
 *   vse8 v16 -> c
 *
 * place_counter is inlined as a `sw` so we never leave the asm block,
 * which means no compiler-driven vector-register spilling around a call.
 */
__attribute__((naked, noinline))
void grid_vadd_per_iter(int8_t *a, int8_t *b, int8_t *c,
                        size_t vl, int iters, uintptr_t perf_reg) {
    __asm__ volatile (
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "li      t0, 1                      \n\t"
        "li      t1, 0                      \n\t"
        "mv      t2, a4                     \n\t"  // iter counter
    "1:                                     \n\t"
        "sw      t0, 0(a5)                  \n\t"  // counter START tag=1
        "vadd.vv v16, v8, v12               \n\t"
        "sw      t1, 0(a5)                  \n\t"  // counter STOP
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/*
 * TEST 2 kernel: single counter wraps the whole burst.
 *
 *   a0 = grid_a, a1 = grid_b, a2 = grid_c, a3 = vl,
 *   a4 = ITERS, a5 = PERF_REG_ADDR.
 *
 *   vsetvli once
 *   vle8 v8, vle8 v12
 *   sw 2, 0(a5)              // counter START tag=2
 *   loop ITERS times: vadd.vv v16, v8, v12
 *   sw 0, 0(a5)              // counter STOP
 *   vse8 v16 -> c
 *
 * Each iteration overwrites v16 - we only care about steady-state
 * vadd throughput here, not per-iteration result.
 */
__attribute__((naked, noinline))
void grid_vadd_back2back(int8_t *a, int8_t *b, int8_t *c,
                         size_t vl, int iters, uintptr_t perf_reg) {
    __asm__ volatile (
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "vle8.v  v12, (a1)                  \n\t"
        "li      t0, 2                      \n\t"
        "li      t1, 0                      \n\t"
        "mv      t2, a4                     \n\t"
        "sw      t0, 0(a5)                  \n\t"  // counter START tag=2
    "1:                                     \n\t"
        "vadd.vv v16, v8, v12               \n\t"
        "addi    t2, t2, -1                 \n\t"
        "bnez    t2, 1b                     \n\t"
        "sw      t1, 0(a5)                  \n\t"  // counter STOP
        "vse8.v  v16, (a2)                  \n\t"
        "ret                                \n\t"
    );
}

/*
 * TEST 3 kernel: saturating brightness boost.
 *   grid_c = vsadd.vx(grid_a, boost)
 *   a0 = a, a1 = c, a2 = vl, a3 = boost (sign-extended in scalar reg),
 *   a4 = PERF_REG_ADDR.
 */
__attribute__((naked, noinline))
void grid_brightness_sat(int8_t *a, int8_t *c, size_t vl,
                         int boost, uintptr_t perf_reg) {
    __asm__ volatile (
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"
        "li      t0, 3                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a4)                  \n\t"  // counter START tag=3
        "vsadd.vx v12, v8, a3               \n\t"  // signed saturating add
        "sw      t1, 0(a4)                  \n\t"
        "vse8.v  v12, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/*
 * TEST 4 kernel: 3-tap horizontal box blur.
 *   For each pixel (r, c):
 *     out[r][c] = (a[r][c-1] + 2*a[r][c] + a[r][c+1]) >> 2
 *   Edges shifted in from vslide are 0.
 *
 *   a0 = a, a1 = c, a2 = vl, a3 = PERF_REG_ADDR.
 *
 *   v8  = a
 *   v12 = vslidedown(v8, 1)   // a[r][c+1]
 *   v16 = vslideup(v8, 1)     // a[r][c-1]
 *   v20 = v8 + v12            // a + a_right
 *   v20 = v20 + v16           // + a_left
 *   v20 = v20 + v8            // + a (gives a_left + 2*a + a_right)
 *   v20 = vsra(v20, 2)        // >> 2
 *
 * All slide ops use 0-fill at boundaries.
 */
__attribute__((naked, noinline))
void grid_blur3_horiz(int8_t *a, int8_t *c, size_t vl, uintptr_t perf_reg) {
    __asm__ volatile (
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "li      t0, 4                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a3)                  \n\t"  // counter START tag=4
        "vmv.v.i v12, 0                     \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "vslidedown.vi v12, v8, 1           \n\t"  // c+1
        "vslideup.vi   v16, v8, 1           \n\t"  // c-1
        "vadd.vv v20, v8,  v12              \n\t"
        "vadd.vv v20, v20, v16              \n\t"
        "vadd.vv v20, v20, v8               \n\t"  // 2*a + a_left + a_right
        "vsra.vi v20, v20, 2                \n\t"  // >> 2
        "sw      t1, 0(a3)                  \n\t"  // counter STOP
        "vse8.v  v20, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/*
 * TEST 5 kernel: 3-tap vertical box blur via CSR 0x7c0 = 1.
 * Same arithmetic as TEST 4 but the slide direction is rotated 90 degrees
 * so neighbours come from the row above / row below. CSR is restored to 0
 * at the end of the kernel so subsequent tests see horizontal mode.
 *
 *   a0 = a, a1 = c, a2 = vl, a3 = PERF_REG_ADDR.
 */
__attribute__((naked, noinline))
void grid_blur3_vert(int8_t *a, int8_t *c, size_t vl, uintptr_t perf_reg) {
    __asm__ volatile (
        "li      t3, 1                      \n\t"
        "csrw    0x7c0, t3                  \n\t"  // vertical mode ON
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "li      t0, 5                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a3)                  \n\t"  // counter START tag=5
        "vmv.v.i v12, 0                     \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "vslidedown.vi v12, v8, 1           \n\t"  // row+1 (vertical)
        "vslideup.vi   v16, v8, 1           \n\t"  // row-1 (vertical)
        "vadd.vv v20, v8,  v12              \n\t"
        "vadd.vv v20, v20, v16              \n\t"
        "vadd.vv v20, v20, v8               \n\t"
        "vsra.vi v20, v20, 2                \n\t"
        "sw      t1, 0(a3)                  \n\t"  // counter STOP
        "vse8.v  v20, (a1)                  \n\t"
        "csrw    0x7c0, zero                \n\t"  // restore horizontal mode
        "ret                                \n\t"
    );
}

/*
 * TEST 6 kernel: per-row sum (vredsum sketch for matvec).
 *   For each row r:
 *     grid_c[r][0] = sum over c of grid_a[r][c]
 *
 *   vredsum.vs reduces v8..v11 (LMUL=4, all 128 elements of one row) into
 *   element 0 of v12 - and because the 2D fabric runs the same op per row,
 *   each of the 128 hardware rows produces its own row-sum in element 0 of
 *   its v12. We then store with vl=1 so only element 0 of each row is
 *   written: row r writes into &grid_c[r][0].
 *
 *   a0 = a, a1 = c, a2 = COLS, a3 = PERF_REG_ADDR.
 */
__attribute__((naked, noinline))
void grid_row_sum(int8_t *a, int8_t *c, size_t cols, uintptr_t perf_reg) {
    __asm__ volatile (
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"
        "li      t0, 6                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a3)                  \n\t"  // counter START tag=6
        "vmv.v.i v12, 0                     \n\t"
        "vredsum.vs v12, v8, v12            \n\t"
        "sw      t1, 0(a3)                  \n\t"  // counter STOP
        "li      t2, 1                      \n\t"
        "vsetvli zero, t2, e8, m1, ta, ma   \n\t"  // vl=1: write only e[0]
        "vse8.v  v12, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/*
 * TEST 7 kernel: full grid scalar reduction via interleaved H + V vredsum.
 *
 *   a0 = grid_a base, a1 = pointer to int32_t result_word,
 *   a2 = COLS (= vl), a3 = PERF_REG_ADDR.
 *
 *   Phase 1 (H): per-row vredsum, row sums in v12[0] of each hw-row.
 *   Phase 2 (H): mask + vmerge clean v12 to [rs[r], 0, 0, ..., 0]
 *                so the vertical-lanes 1..127 see all-zero columns.
 *   Phase 3 (V): cross-row vredsum on the cleaned register; vertical
 *                lane 0 reduces the 128 row sums into the grid total.
 *   Phase 4   : vmv.x.s extracts element 0 (= grid total) to a scalar
 *                register; sw writes it to *result_word.
 */
__attribute__((naked, noinline))
void grid_full_sum(int8_t *a, int *result_word, size_t cols,
                   uintptr_t perf_reg) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"  // start in horizontal mode
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"

        "li      t0, 7                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a3)                  \n\t"  // counter START tag=7

        // Phase 1: per-row reduction (horizontal compute)
        "vmv.v.i v12, 0                     \n\t"
        "vredsum.vs v12, v8, v12            \n\t"  // v12[0] in hw-row r = rs[r]

        // Phase 2: clean v12 - keep element 0, zero elements 1..127.
        // Mask construction must happen in horizontal compute mode so the
        // bit pattern (bit 0 set, rest clear) corresponds to "element 0
        // only" in the same view we're using for vmerge below.
        "vid.v   v16                        \n\t"
        "vmseq.vi v0, v16, 0                \n\t"  // v0[i] = (i == 0)
        "vmv.v.i v20, 0                     \n\t"
        "vmerge.vvm v24, v20, v12, v0       \n\t"  // v24[0]=rs[r], v24[1..]=0

        // Phase 3: cross-row reduction (vertical compute)
        "li      t3, 1                      \n\t"
        "csrw    0x7c0, t3                  \n\t"
        "vmv.v.i v28, 0                     \n\t"
        "vredsum.vs v28, v24, v28           \n\t"  // vert-lane-0 -> grid total
        "csrw    0x7c0, zero                \n\t"  // back to horizontal

        "sw      t1, 0(a3)                  \n\t"  // counter STOP

        // Phase 4: extract scalar back to scalar core
        "vmv.x.s a4, v28                    \n\t"  // a4 = element 0 of v28
        "sw      a4, 0(a1)                  \n\t"  // *result_word = grid total
        "ret                                \n\t"
    );
}

/*
 * TEST 8 kernel: 3x3 separable box blur, single horizontal LSU round-trip.
 *
 *   a0 = input image, a1 = output image, a2 = COLS, a3 = PERF_REG_ADDR.
 *
 *   v8  = image (loaded once, horizontal LSU)
 *   v12 = a_right (h slide)            -> a[r][c+1]
 *   v16 = a_left  (h slide)            -> a[r][c-1]
 *   v20 = a_left + a + a_right         -> 3-tap horizontal sum h_sum
 *   csrw 0x7c0, 1                      -> compute mode flips to vertical
 *   v8  = above-h_sum (v slide)        -> h_sum[r-1][c]   (reused)
 *   v12 = below-h_sum (v slide)        -> h_sum[r+1][c]   (reused)
 *   v16 = h_sum + above + below        -> full 3x3 box sum
 *   v16 = v16 >> 3                     -> /8 (close enough to /9)
 *   csrw 0x7c0, 0                      -> back to horizontal
 *   vse8 v16                           -> output image
 *
 *   Boundary pixels see 0 from the slide fill on each side, so a 1-pixel
 *   border of darker values is expected on the output.
 */
__attribute__((naked, noinline))
void grid_blur3x3(int8_t *a, int8_t *c, size_t cols, uintptr_t perf_reg) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8, (a0)                   \n\t"

        "li      t0, 8                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a3)                  \n\t"  // counter START tag=8

        // Horizontal compute: 1x3 horizontal sum
        "vmv.v.i v12, 0                     \n\t"
        "vmv.v.i v16, 0                     \n\t"
        "vslidedown.vi v12, v8, 1           \n\t"  // c+1: right
        "vslideup.vi   v16, v8, 1           \n\t"  // c-1: left
        "vadd.vv v20, v8,  v12              \n\t"
        "vadd.vv v20, v20, v16              \n\t"  // v20 = a_left + a + a_right

        // Vertical compute: 1x3 vertical sum on v20 (h_sum)
        "li      t3, 1                      \n\t"
        "csrw    0x7c0, t3                  \n\t"
        "vmv.v.i v8,  0                     \n\t"
        "vmv.v.i v12, 0                     \n\t"
        "vslidedown.vi v8,  v20, 1          \n\t"  // r+1: below in image
        "vslideup.vi   v12, v20, 1          \n\t"  // r-1: above in image
        "vadd.vv v16, v20, v8               \n\t"
        "vadd.vv v16, v16, v12              \n\t"  // v16 = box-3x3 sum
        "vsra.vi v16, v16, 3                \n\t"  // /8
        "csrw    0x7c0, zero                \n\t"

        "sw      t1, 0(a3)                  \n\t"  // counter STOP
        "vse8.v  v16, (a1)                  \n\t"
        "ret                                \n\t"
    );
}

/*
 * TEST 9 kernel: matrix-vector multiply y = A * x.
 *   A is 128x128 in grid_a, x must be pre-replicated to x_replicated[r][:]
 *   = x[:] for every r so that a horizontal vle8 puts x into every hw-row.
 *
 *   a0 = A base, a1 = X_REPL base, a2 = result_strided base
 *        (result[r*COLS] = y[r]),
 *   a3 = COLS, a4 = PERF_REG_ADDR.
 *
 *   Pure horizontal mode - no CSR flip needed because the result lands
 *   strided in a way that horizontal-mode vse8(vl=1) writes correctly.
 *
 *   Note: i8*i8 may saturate. For full precision use vwmul.vv (i8 -> i16)
 *   and vwredsum.vs (i16 -> i32); skipped here for kernel brevity.
 */
__attribute__((naked, noinline))
void grid_matvec(int8_t *A, int8_t *X_REPL, int8_t *result_strided,
                 size_t cols, uintptr_t perf_reg) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a3, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"  // A[r][:] in hw-row r
        "vle8.v  v12, (a1)                  \n\t"  // x[:] in every hw-row

        "li      t0, 9                      \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a4)                  \n\t"  // counter START tag=9

        "vmul.vv v16, v8, v12               \n\t"  // i8*i8 -> i8 (may saturate)
        "vmv.v.i v20, 0                     \n\t"
        "vredsum.vs v20, v16, v20           \n\t"  // y[r] in v20[0] of hw-row r

        "sw      t1, 0(a4)                  \n\t"  // counter STOP

        "li      t2, 1                      \n\t"
        "vsetvli zero, t2, e8, m1, ta, ma   \n\t"
        "vse8.v  v20, (a2)                  \n\t"  // result[r*COLS] = y[r]
        "ret                                \n\t"
    );
}

/*
 * TEST 10 kernel: mode-aware mask demo - same recipe, two patterns.
 *
 *   We build v0 with the SAME instructions under each mode and apply it
 *   to the same vector add, but the resulting masked stripe runs in
 *   different directions: horizontal-built mask -> column stripes;
 *   vertical-built mask -> row stripes. R7 in the header explains why.
 *
 *   a0 = grid_a, a1 = output buffer, a2 = COLS,
 *   a3 = PERF_REG_ADDR. Two writes to the output:
 *       *(a1 + 0)             -> horizontal-mask result (column stripe)
 *       *(a1 + ROWS*COLS)     -> vertical-mask  result (row stripe)
 *
 *   The "selectivity" is "even element index". In horizontal-compute view
 *   element index = image-column index, so even columns get the increment.
 *   In vertical-compute view element index = image-row index, so even
 *   rows get the increment instead.
 */
__attribute__((naked, noinline))
void grid_mask_modes(int8_t *a, int8_t *out, size_t cols,
                     uintptr_t perf_reg) {
    __asm__ volatile (
        "csrw    0x7c0, zero                \n\t"
        "vsetvli zero, a2, e8, m4, ta, ma   \n\t"
        "vle8.v  v8,  (a0)                  \n\t"

        "li      t0, 10                     \n\t"
        "li      t1, 0                      \n\t"
        "sw      t0, 0(a3)                  \n\t"  // counter START tag=10

        // ---- Horizontal compute pass: mask = even element indices ----
        "vid.v   v12                        \n\t"
        "vand.vi v12, v12, 1                \n\t"  // 0/1 toggle
        "vmseq.vi v0, v12, 0                \n\t"  // mask: i % 2 == 0
        "vmv.v.v v16, v8                    \n\t"  // copy image to output reg
        "li      t4, 50                     \n\t"
        "vadd.vx v16, v16, t4, v0.t         \n\t"  // +50 only on masked
        "vse8.v  v16, (a1)                  \n\t"  // first output buffer

        // ---- Vertical compute pass: same mask recipe -> different effect
        "li      t3, 1                      \n\t"
        "csrw    0x7c0, t3                  \n\t"
        "vid.v   v12                        \n\t"  // same recipe under V mode
        "vand.vi v12, v12, 1                \n\t"
        "vmseq.vi v0, v12, 0                \n\t"
        "vmv.v.v v16, v8                    \n\t"
        "vadd.vx v16, v16, t4, v0.t         \n\t"  // V-mode masked vadd
        "csrw    0x7c0, zero                \n\t"  // store under horizontal LSU

        "sw      t1, 0(a3)                  \n\t"  // counter STOP

        // Second output is one full grid above the first
        "li      t5, 16384                  \n\t"  // ROWS * COLS = 128*128
        "add     t6, a1, t5                 \n\t"
        "vse8.v  v16, (t6)                  \n\t"
        "ret                                \n\t"
    );
}

/*--- test wrappers ---*/

__attribute__((noinline))
static void test1_individual(void) {
    printf("\n=== TEST 1: per-iteration vadd (counter tag=1, %d iters) ===\n",
           ITERS);
    clear_grid_c();
    grid_vadd_per_iter(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
                       (size_t)COLS, ITERS, (uintptr_t)PERF_REG_ADDR);
    printf("[TEST 1] grid_c first 2 rows after %d-iter run:\n", ITERS);
    print_grid(2, &grid_c[0][0]);
}

__attribute__((noinline))
static void test2_back2back(void) {
    printf("\n=== TEST 2: back-to-back vadd (counter tag=2, %d iters) ===\n",
           ITERS);
    clear_grid_c();
    grid_vadd_back2back(&grid_a[0][0], &grid_b[0][0], &grid_c[0][0],
                        (size_t)COLS, ITERS, (uintptr_t)PERF_REG_ADDR);
    printf("[TEST 2] grid_c first 2 rows after %d-iter run:\n", ITERS);
    print_grid(2, &grid_c[0][0]);
}

__attribute__((noinline))
static void test3_brightness(int boost) {
    printf("\n=== TEST 3: brightness boost vsadd.vx %d (counter tag=3) ===\n",
           boost);
    clear_grid_c();
    grid_brightness_sat(&grid_a[0][0], &grid_c[0][0],
                        (size_t)COLS, boost, (uintptr_t)PERF_REG_ADDR);
    printf("[TEST 3] grid_c first 2 rows:\n");
    print_grid(2, &grid_c[0][0]);
}

__attribute__((noinline))
static void test4_blur_horiz(void) {
    printf("\n=== TEST 4: 3-tap horizontal box blur (counter tag=4) ===\n");
    clear_grid_c();
    grid_blur3_horiz(&grid_a[0][0], &grid_c[0][0],
                     (size_t)COLS, (uintptr_t)PERF_REG_ADDR);
    printf("[TEST 4] grid_c first 2 rows:\n");
    print_grid(2, &grid_c[0][0]);
}

__attribute__((noinline))
static void test5_blur_vert(void) {
    printf("\n=== TEST 5: 3-tap vertical box blur, CSR 0x7c0=1 (counter tag=5) ===\n");
    clear_grid_c();
    grid_blur3_vert(&grid_a[0][0], &grid_c[0][0],
                    (size_t)COLS, (uintptr_t)PERF_REG_ADDR);
    printf("[TEST 5] grid_c first 2 rows:\n");
    print_grid(2, &grid_c[0][0]);
}

__attribute__((noinline))
static void test6_row_sum(void) {
    printf("\n=== TEST 6: per-row reduction (vredsum, counter tag=6) ===\n");
    clear_grid_c();
    grid_row_sum(&grid_a[0][0], &grid_c[0][0],
                 (size_t)COLS, (uintptr_t)PERF_REG_ADDR);
    printf("[TEST 6] grid_c[r][0] for r=0..7 (per-row sum of grid_a):\n");
    for (int r = 0; r < 8; r++) {
        printf("  row %d sum = %d\n", r, grid_c[r][0]);
    }
}

__attribute__((noinline))
static void test7_full_sum(void) {
    printf("\n=== TEST 7: full-grid sum, H+V vredsum (counter tag=7) ===\n");
    grid_total_word = 0;
    grid_full_sum(&grid_a[0][0], &grid_total_word,
                  (size_t)COLS, (uintptr_t)PERF_REG_ADDR);
    // i8 overflow is expected to wrap each per-row sum and the final
    // sum independently; this is the value the hardware actually
    // produces given the i8 datapath. Print as int8 to make it match
    // the 8-bit datapath; also print as int32 to see the raw extracted
    // word.
    int total_word = grid_total_word;
    int8_t total_i8 = (int8_t)(total_word & 0xff);
    printf("[TEST 7] grid total (i8 wrap)  = %d\n", (int)total_i8);
    printf("[TEST 7] grid total (raw word) = %d (0x%x)\n",
           total_word, total_word);

    // Independent scalar reference: sum every cell of grid_a as i32 and
    // also as i8-wrapping accumulation; compare against the vector
    // result so the test can self-check.
    int ref_i32 = 0;
    int8_t ref_i8 = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            ref_i32 += grid_a[r][c];
            ref_i8  = (int8_t)(ref_i8 + grid_a[r][c]);
        }
    }
    printf("[TEST 7] reference (i32) = %d, reference (i8 wrap) = %d\n",
           ref_i32, (int)ref_i8);
    if (total_i8 == ref_i8) {
        printf("[TEST 7] PASS: vector i8 total matches scalar i8 reference\n");
    } else {
        printf("[TEST 7] CHECK: vector i8 (%d) vs scalar i8 (%d)\n",
               (int)total_i8, (int)ref_i8);
    }
}

__attribute__((noinline))
static void test8_blur3x3(void) {
    printf("\n=== TEST 8: 3x3 box blur, H+V slides (counter tag=8) ===\n");
    clear_grid_c();
    grid_blur3x3(&grid_a[0][0], &grid_c[0][0],
                 (size_t)COLS, (uintptr_t)PERF_REG_ADDR);
    printf("[TEST 8] grid_c first 4 rows of blurred image:\n");
    print_grid(4, &grid_c[0][0]);
}

__attribute__((noinline))
static void replicate_x(const int8_t *x) {
    volatile int8_t *p = (volatile int8_t *)&x_replicated[0][0];
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            p[r * COLS + c] = x[c];
        }
    }
}

__attribute__((noinline))
static void test9_matvec(void) {
    printf("\n=== TEST 9: matrix-vector multiply (counter tag=9) ===\n");
    // Use a unit-ish vector to keep the i8 product in range. Sum of
    // A[r][k] * x[k] for k=0..7 only (rest of x is 0). With grid_a[r][c]
    // = (r+c)&127, expected y[r] = sum_{k=0..7} (r+k)&127 = small.
    int8_t x[COLS];
    volatile int8_t *p = (volatile int8_t *)x;
    for (int i = 0; i < COLS; i++) p[i] = (i < 8) ? 1 : 0;
    replicate_x(x);
    clear_grid_c();
    grid_matvec(&grid_a[0][0], &x_replicated[0][0], &grid_c[0][0],
                (size_t)COLS, (uintptr_t)PERF_REG_ADDR);
    printf("[TEST 9] y[r] (= grid_c[r][0]) for r=0..7:\n");
    for (int r = 0; r < 8; r++) {
        // expected y[r] = sum over k=0..7 of (r+k)&127
        int exp = 0;
        for (int k = 0; k < 8; k++) exp += ((r + k) & (COLS - 1));
        printf("  y[%d] = %d (expected i32 = %d, i8 wrap = %d)\n",
               r, grid_c[r][0], exp, (int)(int8_t)exp);
    }
}

__attribute__((noinline))
static void test10_mask_modes(void) {
    printf("\n=== TEST 10: mode-aware mask demo (counter tag=10) ===\n");
    // Zero the dual output buffer before the kernel writes both halves.
    volatile int8_t *po = (volatile int8_t *)&mask_out[0][0];
    for (int r = 0; r < ROWS * 2; r++) {
        for (int c = 0; c < COLS; c++) {
            po[r * COLS + c] = 0;
        }
    }
    grid_mask_modes(&grid_a[0][0], &mask_out[0][0],
                    (size_t)COLS, (uintptr_t)PERF_REG_ADDR);

    // First half: built under horizontal compute -> mask selects every
    // other COLUMN. Print 3 rows: even columns should be image+50, odd
    // columns should be image (unchanged).
    printf("[TEST 10] horizontal mask (column-stripe), first 3 rows:\n");
    print_grid(3, &mask_out[0][0]);
    // Second half: built under vertical compute -> mask selects every
    // other ROW. Print 4 rows so the row alternation is visible: rows
    // 0/2 should be image+50, rows 1/3 should be image.
    printf("[TEST 10] vertical mask (row-stripe), first 4 rows:\n");
    print_grid(4, &mask_out[ROWS][0]);
}

__attribute__((noinline))
static void check_vadd_result(void) {
    int errors = 0;
    int first_bad_r = -1, first_bad_c = -1;
    int8_t first_got = 0, first_exp = 0;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int8_t a_v = (int8_t)((r + c) & (COLS - 1));
            int8_t b_v = (int8_t)((r * 2) & (COLS - 1));
            int8_t exp = (int8_t)(a_v + b_v);
            int8_t got = grid_c[r][c];
            if (got != exp) {
                if (errors == 0) {
                    first_bad_r = r;
                    first_bad_c = c;
                    first_got = got;
                    first_exp = exp;
                }
                errors++;
            }
        }
    }
    if (errors == 0) {
        printf("[CHECK] PASS: grid_c == grid_a + grid_b for all %d cells\n",
               ROWS * COLS);
    } else {
        printf("[CHECK] FAIL: %d mismatches; first at [%d][%d] got %d expected %d\n",
               errors, first_bad_r, first_bad_c, first_got, first_exp);
    }
}

void test(void) {
    init_grids();

    printf("[INIT] grid_a first 2 rows (a[i][j] = (i+j) & 127):\n");
    print_grid(2, &grid_a[0][0]);
    printf("[INIT] grid_b first 2 rows (b[i][j] = (i*2) & 127):\n");
    print_grid(2, &grid_b[0][0]);

    test1_individual();
    check_vadd_result();
    test2_back2back();
    check_vadd_result();
    test3_brightness(20);
    test4_blur_horiz();
    test5_blur_vert();
    test6_row_sum();
    test7_full_sum();
    test8_blur3x3();
    test9_matvec();
    test10_mask_modes();

    printf("\n=== benchmark_vadd done ===\n");
}
