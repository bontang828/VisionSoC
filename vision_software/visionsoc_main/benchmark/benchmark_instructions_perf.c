/*
 * benchmark_instructions_perf -- FPGA (KV260) port of the t1emu test
 * tests/vision_task/benchmark_instructions_lmul1.c.
 *
 * Runs the same 35 instruction micro-kernels (29 compute + 6 LSU), in both
 * Horizontal and Vertical CSR-0x7c0 modes, on the real T1 in the PL and
 * measures per-instruction hardware cycles with the libt1 perf counter.
 *
 * Differences vs the t1emu test (all intentional):
 *   - No camera frame: operands are dummy data written straight into the
 *     URAM scratchpad.
 *   - Load/store go to the URAM scratchpad (T1_SCRATCHPAD_PA), not DDR:
 *     setup vle / timed vle / vse / vlse / vsse all use a scratchpad PA as
 *     rs1, exactly as the user asked.
 *   - Each op is issued one-at-a-time via t1_issue() and bracketed by
 *     t1_perf_start()/t1_wait_ready()/t1_perf_stop(); per-iter cycles are
 *     summed over --iters runs. (On hardware the cycle count includes the
 *     AXI-Lite issue + writeback-drain handshake, so it is NOT directly
 *     comparable to the tight t1emu replay loop -- it is the real per-issue
 *     FPGA cost.)
 *   - No correctness checking: this is a timing harness (dummy data).
 *
 * Output: a `[PERF] counter <tag> START/STOP` pair per (instruction, mode),
 * with the SAME tag layout as plot_bench_cycles.py
 *   1..35   H-mode instructions (INSTRUCTION_ORDER slot)
 *   36..70  V-mode instructions
 * so both plot_bench_cycles.py and plot_bench_cycles_fpga.py parse it. The
 * STOP cycle is the SUM over --iters issues; divide by --iters for cyc/iter
 * (the plot scripts do this with --iters N). A CSV twin is written to --out.
 *
 * Kernel words come from kernels/bench_instructions.h, generated on the board
 * by ../../libt1/build_kernel.sh from kernels/bench_instructions.S. The BI_*
 * enum below MUST stay in lockstep with the op ORDER in that .S.
 */
#include "kernels/bench_instructions.h"
#include "libt1.h"
#include "libt1_regs.h"

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS         128u
#define COLS         128u
#define FRAME_BYTES  (ROWS * COLS)        /* 16384 */
#define DEFAULT_ITERS 100u
#define MAX_ITERS     100000u

/* Scratchpad layout (offsets within the 512 KB URAM at T1_SCRATCHPAD_PA).
 * T1 vle/vse use SP_A/SP_B/SP_C as rs1; the host never touches the URAM with
 * the CPU (see the DMA staging in main -- byte stores to the bram_ctrl MMIO
 * fault with an SError). */
#define SP_OFF_A   0x00000u   /* operand A / load source  */
#define SP_OFF_B   0x04000u   /* operand B                */
#define SP_OFF_C   0x08000u   /* store destination        */
#define SP_A  (T1_SCRATCHPAD_PA + SP_OFF_A)
#define SP_B  (T1_SCRATCHPAD_PA + SP_OFF_B)
#define SP_C  (T1_SCRATCHPAD_PA + SP_OFF_C)

/* Scalar immediates the t1emu test uses (passed via op.rs1). */
#define SC_VMSGT      64u
#define SC_VRGATHER    5u
#define SC_VMV_S_X    42u
#define SC_SLIDE_SMALL 1u
#define SC_SLIDE_BIG 100u
#define STRIDE2        2u
#define STRIDE2_VL    64u

/* Index into bench_instructions[] -- MUST match kernels/bench_instructions.S. */
enum {
    BI_SET_LDA = 0, BI_SET_LDB, BI_SET_Z16, BI_SET_Z20, BI_SET_M0, BI_SET_M1,
    BI_VADD, BI_VMUL, BI_VMACC, BI_VMADD, BI_VAND, BI_VOR, BI_VSLL, BI_VSRA,
    BI_VMSEQ, BI_VMSLE, BI_VMSGT, BI_VMSLT, BI_VMAND, BI_VRG_VX, BI_VRG_VX_M,
    BI_VRG_VV, BI_VMV_SX, BI_VMV_XS, BI_VMV_VV, BI_VREDSUM, BI_VREDMAX,
    BI_VSLUP_VX, BI_VSLDN_VX, BI_VSLUP_VI1, BI_VSLDN_VI1, BI_VSLUP_VI31,
    BI_VSLDN_VI31, BI_VLE, BI_VLE_M, BI_VSE, BI_VSE_M, BI_VLSE, BI_VSSE,
    BI_COUNT
};

/* How a timed op gets its operand/address. */
enum bi_kind {
    K_VV,         /* vector-vector: rs1 = 0                         */
    K_SCALAR,     /* vx / .s.x / slide.vx: rs1 = scalar immediate   */
    K_LD,         /* vle full:   rs1 = scratchpad A                 */
    K_LD_MASK,    /* vle masked: rs1 = scratchpad A, mask v0        */
    K_ST,         /* vse full:   rs1 = scratchpad C                 */
    K_ST_MASK,    /* vse masked: rs1 = scratchpad C, mask v0        */
    K_LD_STRIDE,  /* vlse:       rs1 = scratchpad A, rs2 = stride   */
    K_ST_STRIDE   /* vsse:       rs1 = scratchpad C, rs2 = stride   */
};

struct test {
    const char  *name;    /* MUST match plot_bench_cycles.py INSTRUCTION_ORDER */
    int          idx;     /* bench_instructions[] slot for the timed op        */
    uint32_t     scalar;  /* op.rs1 for K_SCALAR (ignored otherwise)           */
    uint32_t     vl;      /* op.vl                                             */
    enum bi_kind kind;
};

/* The 35 tests, in the exact order of plot_bench_cycles.py INSTRUCTION_ORDER.
 * H tags are 1..35, V tags 36..70 (index i -> tag i+1 / i+1+35). */
static const struct test TESTS[] = {
    { "vadd.vv",                 BI_VADD,       0,            COLS,      K_VV },
    { "vmul.vv",                 BI_VMUL,       0,            COLS,      K_VV },
    { "vmacc.vv",                BI_VMACC,      0,            COLS,      K_VV },
    { "vmadd.vv",                BI_VMADD,      0,            COLS,      K_VV },
    { "vand.vv",                 BI_VAND,       0,            COLS,      K_VV },
    { "vor.vv",                  BI_VOR,        0,            COLS,      K_VV },
    { "vsll.vi",                 BI_VSLL,       0,            COLS,      K_VV },
    { "vsra.vi",                 BI_VSRA,       0,            COLS,      K_VV },
    { "vmseq.vv",                BI_VMSEQ,      0,            COLS,      K_VV },
    { "vmsle.vv",                BI_VMSLE,      0,            COLS,      K_VV },
    { "vmsgt.vx",                BI_VMSGT,      SC_VMSGT,     COLS,      K_SCALAR },
    { "vmslt.vv",                BI_VMSLT,      0,            COLS,      K_VV },
    { "vmand.mm",                BI_VMAND,      0,            COLS,      K_VV },
    { "vrgather.vx",             BI_VRG_VX,     SC_VRGATHER,  COLS,      K_SCALAR },
    { "vrgather.vx (diag mask)", BI_VRG_VX_M,   SC_VRGATHER,  COLS,      K_SCALAR },
    { "vrgather.vv",             BI_VRG_VV,     0,            COLS,      K_VV },
    { "vmv.s.x",                 BI_VMV_SX,     SC_VMV_S_X,   COLS,      K_SCALAR },
    { "vmv.x.s",                 BI_VMV_XS,     0,            COLS,      K_VV },
    { "vmv.v.v",                 BI_VMV_VV,     0,            COLS,      K_VV },
    { "vredsum.vs",              BI_VREDSUM,    0,            COLS,      K_VV },
    { "vredmax.vs",              BI_VREDMAX,    0,            COLS,      K_VV },
    { "vslideup.vx 1",           BI_VSLUP_VX,   SC_SLIDE_SMALL, COLS,    K_SCALAR },
    { "vslideup.vx 100",         BI_VSLUP_VX,   SC_SLIDE_BIG,   COLS,    K_SCALAR },
    { "vslidedown.vx 1",         BI_VSLDN_VX,   SC_SLIDE_SMALL, COLS,    K_SCALAR },
    { "vslidedown.vx 100",       BI_VSLDN_VX,   SC_SLIDE_BIG,   COLS,    K_SCALAR },
    { "vslideup.vi 1",           BI_VSLUP_VI1,  0,            COLS,      K_VV },
    { "vslidedown.vi 1",         BI_VSLDN_VI1,  0,            COLS,      K_VV },
    { "vslideup.vi 31",          BI_VSLUP_VI31, 0,            COLS,      K_VV },
    { "vslidedown.vi 31",        BI_VSLDN_VI31, 0,            COLS,      K_VV },
    { "vle8.v full",             BI_VLE,        0,            COLS,      K_LD },
    { "vle8.v masked",           BI_VLE_M,      0,            COLS,      K_LD_MASK },
    { "vse8.v full",             BI_VSE,        0,            COLS,      K_ST },
    { "vse8.v masked",           BI_VSE_M,      0,            COLS,      K_ST_MASK },
    { "vlse8.v stride2",         BI_VLSE,       0,            STRIDE2_VL, K_LD_STRIDE },
    { "vsse8.v stride2",         BI_VSSE,       0,            STRIDE2_VL, K_ST_STRIDE },
};
#define N_TESTS ((int)(sizeof(TESTS) / sizeof(TESTS[0])))

static uint32_t g_sp_a, g_sp_b, g_sp_c;   /* scratchpad PAs, set in main() */

static void die(const char *msg) { perror(msg); exit(1); }

/* Issue one op (no timing), wait for it to fully drain. Aborts on error. */
static void issue_blocking(uint32_t instruction, uint32_t rs1, uint32_t rs2,
                           uint32_t vl, uint8_t vmode, const char *what)
{
    struct t1_op op = {0};
    op.instruction   = instruction;
    op.rs1           = rs1;
    op.rs2           = rs2;
    op.vtype         = T1_VTYPE_E8_M1_TA_MA;
    op.vl            = vl;
    op.vertical_mode = vmode;
    int rc = t1_issue(&op);
    if (rc >= 0) rc = t1_wait_ready();
    if (rc < 0) {
        fprintf(stderr, "issue failed (%s): %s\n", what, strerror(errno));
        exit(1);
    }
}

/* Load operands + build masks once for a given mode. Mirrors the per-kernel
 * preamble in the t1emu test, hoisted out so the timed loop measures only the
 * op under test. */
static void run_setup(uint8_t vmode)
{
    issue_blocking(bench_instructions[BI_SET_LDA], g_sp_a, 0, COLS, vmode, "setup vle A");
    issue_blocking(bench_instructions[BI_SET_LDB], g_sp_b, 0, COLS, vmode, "setup vle B");
    issue_blocking(bench_instructions[BI_SET_Z16], 0,      0, COLS, vmode, "setup vmv v16");
    issue_blocking(bench_instructions[BI_SET_Z20], 0,      0, COLS, vmode, "setup vmv v20");
    issue_blocking(bench_instructions[BI_SET_M0],  0,      0, COLS, vmode, "setup mask v0");
    issue_blocking(bench_instructions[BI_SET_M1],  0,      0, COLS, vmode, "setup mask v1");
}

/* Time one test over `iters` issues; returns summed hardware cycles. */
static uint64_t time_test(const struct test *t, uint8_t vmode, uint8_t tag,
                          unsigned iters)
{
    struct t1_op op = {0};
    op.instruction   = bench_instructions[t->idx];
    op.vtype         = T1_VTYPE_E8_M1_TA_MA;
    op.vl            = t->vl;
    op.vertical_mode = vmode;
    switch (t->kind) {
        case K_VV:         op.rs1 = 0;       op.rs2 = 0;       break;
        case K_SCALAR:     op.rs1 = t->scalar; op.rs2 = 0;     break;
        case K_LD:
        case K_LD_MASK:    op.rs1 = g_sp_a;  op.rs2 = 0;       break;
        case K_ST:
        case K_ST_MASK:    op.rs1 = g_sp_c;  op.rs2 = 0;       break;
        case K_LD_STRIDE:  op.rs1 = g_sp_a;  op.rs2 = STRIDE2; break;
        case K_ST_STRIDE:  op.rs1 = g_sp_c;  op.rs2 = STRIDE2; break;
    }

    uint64_t total = 0;
    for (unsigned i = 0; i < iters; i++) {
        (void)t1_perf_start(tag);
        int rc = t1_issue(&op);
        if (rc >= 0) rc = t1_wait_ready();
        uint32_t cyc = t1_perf_stop();
        if (rc < 0) {
            fprintf(stderr, "t1_issue/wait failed (%s, %s): %s\n",
                    t->name, vmode ? "V" : "H", strerror(errno));
            exit(1);
        }
        total += cyc;
    }
    return total;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [--iters N] [--out path]\n"
        "  --iters N   issues per (instruction, mode) to average (default %u,\n"
        "              env VISIONSOC_BENCH_ITERS). STOP cycle = sum over N.\n"
        "  --out path  CSV destination (default: no CSV, [PERF] lines on stdout)\n",
        argv0, DEFAULT_ITERS);
}

int main(int argc, char **argv)
{
    /* Unbuffered stdout so progress is visible live when piped through tee
     * (otherwise the per-test lines sit in the 4 KB buffer and the run looks
     * hung even while it is happily issuing ops). */
    setvbuf(stdout, NULL, _IONBF, 0);

    unsigned iters = DEFAULT_ITERS;
    const char *out_path = NULL;
    const char *env_iters = getenv("VISIONSOC_BENCH_ITERS");
    if (env_iters && *env_iters) iters = (unsigned)strtoul(env_iters, NULL, 0);

    static const struct option opts[] = {
        { "iters", required_argument, NULL, 'i' },
        { "out",   required_argument, NULL, 'o' },
        { "help",  no_argument,       NULL, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:o:h", opts, NULL)) != -1) {
        switch (c) {
            case 'i': iters = (unsigned)strtoul(optarg, NULL, 0); break;
            case 'o': out_path = optarg; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 2;
        }
    }
    if (iters == 0 || iters > MAX_ITERS) {
        fprintf(stderr, "iters must be in 1..%u\n", MAX_ITERS);
        return 2;
    }
    if (bench_instructions_count != BI_COUNT) {
        fprintf(stderr, "kernel has %u words, expected %u -- "
                "bench_instructions.S out of sync with the BI_* enum\n",
                bench_instructions_count, (unsigned)BI_COUNT);
        return 2;
    }

    FILE *csv = NULL;
    if (out_path) {
        csv = fopen(out_path, "w");
        if (!csv) die("fopen out");
        fprintf(csv, "tag,mode,instruction,iters,total_cycles,cyc_per_iter\n");
    }

    if (t1_init() < 0) die("t1_init");

    g_sp_a = SP_A; g_sp_b = SP_B; g_sp_c = SP_C;

    /* Operands live in the URAM scratchpad (T1 vle/vse use SP_A/SP_B/SP_C as
     * rs1). We do NOT write the URAM with the CPU -- it is reached PS-side via
     * axi_bram_ctrl MMIO, which faults on byte/sub-word stores (SError ->
     * kernel panic). Instead stage the dummy data in DDR (cached, byte-
     * writable) and DMA it into the scratchpad: the exact dma_to_scratchpad
     * pattern matmul_8bitraw_short_perf.c uses (s2mm_async + mm2s_async + wait). */
    struct t1_buf in_a = {0}, in_b = {0};
    if (t1_buf_alloc(&in_a, FRAME_BYTES) < 0) die("t1_buf_alloc A");
    if (t1_buf_alloc(&in_b, FRAME_BYTES) < 0) die("t1_buf_alloc B");

    uint8_t *pa = (uint8_t *)in_a.va, *pb = (uint8_t *)in_b.va;
    for (unsigned r = 0; r < ROWS; r++) {
        for (unsigned col = 0; col < COLS; col++) {
            pa[r * COLS + col] = (uint8_t)((r + col) & (COLS - 1));
            pb[r * COLS + col] = (uint8_t)((r * 2 + col) & (COLS - 1));
        }
    }
    if (t1_buf_sync_for_device(&in_a) < 0) die("sync A");
    if (t1_buf_sync_for_device(&in_b) < 0) die("sync B");

    /* DDR -> scratchpad: arm S2MM (dst = scratchpad), kick MM2S (src = DDR), wait. */
    if (t1_dma_s2mm_async(0, SP_A, FRAME_BYTES) < 0)    die("dma s2mm A");
    if (t1_dma_mm2s_async(in_a.pa, 0, FRAME_BYTES) < 0) die("dma mm2s A");
    if (t1_dma_wait() < 0)                              die("dma wait A");
    if (t1_dma_s2mm_async(0, SP_B, FRAME_BYTES) < 0)    die("dma s2mm B");
    if (t1_dma_mm2s_async(in_b.pa, 0, FRAME_BYTES) < 0) die("dma mm2s B");
    if (t1_dma_wait() < 0)                              die("dma wait B");

    printf("=== benchmark_instructions_perf (FPGA): %u issues per (instr, mode) ===\n",
           iters);
    printf("[CONFIG] SEW=8 / LMUL=1 / vl=%u, operands in URAM scratchpad "
           "(A=0x%08x B=0x%08x C=0x%08x)\n", COLS, g_sp_a, g_sp_b, g_sp_c);
    printf("[CONFIG] tags: H=1..%d, V=%d..%d; STOP cycle = sum over %u issues "
           "(plot with --iters %u)\n",
           N_TESTS, N_TESTS + 1, 2 * N_TESTS, iters, iters);

    for (int mode = 0; mode <= 1; mode++) {
        printf("\n############### %s MODE ###############\n", mode ? "VERTICAL" : "HORIZONTAL");
        run_setup((uint8_t)mode);
        for (int i = 0; i < N_TESTS; i++) {
            const struct test *t = &TESTS[i];
            int tag = mode ? (i + 1 + N_TESTS) : (i + 1);
            uint64_t total = time_test(t, (uint8_t)mode, (uint8_t)(tag & 0x7f), iters);

            /* plot_bench_cycles.py / plot_bench_cycles_fpga.py format. */
            printf("[PERF] counter %d START at cycle 0\n", tag);
            printf("[PERF] counter STOP at cycle %llu\n", (unsigned long long)total);
            printf("    %-22s %s  total=%llu  cyc/iter=%.1f\n",
                   t->name, mode ? "V" : "H", (unsigned long long)total,
                   (double)total / (double)iters);
            if (csv)
                fprintf(csv, "%d,%s,%s,%u,%llu,%.3f\n",
                        tag, mode ? "V" : "H", t->name, iters,
                        (unsigned long long)total, (double)total / (double)iters);
        }
    }

    printf("\n=== benchmark_instructions_perf done ===\n");

    if (csv) fclose(csv);
    t1_buf_free(&in_a); t1_buf_free(&in_b);
    t1_close();
    return 0;
}
