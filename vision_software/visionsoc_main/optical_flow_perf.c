/*
 * optical_flow_perf -- standalone T1 instruction-level perf harness for the
 * 5-direction block-matching optical-flow kernel.
 *
 * Mirrors sobel_perf.c. Feeds a fixed synthetic curr+prev frame pair
 * through the same DDR -> URAM(_HALF_A / _PREV) -> T1(optical_flow) ->
 * URAM_HALF_B -> DDR path as main.c would, but opens the dispatcher loop
 * so each issued instruction is bracketed by t1_perf_start/stop (T1
 * hardware cycles) plus clock_gettime (PS wall-clock microseconds). One
 * CSV row is emitted per (iteration, instruction); the plot_perf.py
 * aggregator reads the same column schema as sobel_perf.
 *
 * The kernel rewrites OFK_PREV_PA on every iter (the `vse8.v v8, (a1)` at
 * index 40), so we re-DMA the synthetic prev frame into OFK_PREV_PA at
 * the START of every iter -- OUTSIDE the per-instruction timing bracket
 * -- to keep |It| consistent across iters. Without this, from iter 1
 * onwards prev == curr -> SAD_static = 0 wins everywhere -> output = 0
 * (the steady-state sanity check would degenerate).
 *
 * Compiler-optimisation notes match sobel_perf:
 *   - t1_perf_start/stop, t1_issue, clock_gettime all touch volatile
 *     MMIO or syscalls. No reordering across them.
 *   - The per-instr DMA arming happens OUTSIDE the bracket.
 *   - Return values are stored into `volatile` locals.
 */
#include "kernels/optical_flow.h"
#include "libt1.h"
#include "libt1_regs.h"

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Match optical_flow_select.h / main.c (URAM_BASE_PA = 0xA0080000). */
#define URAM_BASE_PA   0xA0080000u
#define URAM_HALF_A    (URAM_BASE_PA + 0x00000u)  /* curr   */
#define URAM_HALF_B    (URAM_BASE_PA + 0x04000u)  /* output */
#define OFK_PREV_PA    (URAM_BASE_PA + 0x08000u)  /* prev   */

#define FRAME_BYTES    (128u * 128u)
#define DEFAULT_ITERS  100u
#define MAX_INSTR      64u   /* optical_flow_count is 41; small headroom */

/* AKD decode constants (mirror optical_flow_select.h). */
#define AKD_REG_X0   0u
#define AKD_REG_A0  10u
#define AKD_REG_A1  11u
#define AKD_REG_A2  12u
#define AKD_REG_A3  13u

#define AKD_DIR_SCALE 50u   /* vmul.vx scaling factor */

#define AKD_OPCODE_SYSTEM    0x73u
#define AKD_FUNCT3_CSRRWI    0x5u
#define AKD_FUNCT3_CSRRW     0x1u
#define AKD_CSR_VERTMODE     0x7C0u
#define AKD_OPCODE_LOAD_FP   0x07u
#define AKD_OPCODE_STORE_FP  0x27u
#define AKD_OPCODE_OP_V      0x57u
#define AKD_FUNCT3_OPIVV     0x0u
#define AKD_FUNCT3_OPIVI     0x3u
#define AKD_FUNCT3_OPIVX     0x4u
#define AKD_FUNCT3_OPMVX     0x6u

struct sample {
    uint8_t  is_csr_flip;
    uint32_t t1_cycles;
    double   wall_us;
};

static struct sample samples[1024][MAX_INSTR];

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

static double us_between(const struct timespec *a, const struct timespec *b)
{
    double sec  = (double)(b->tv_sec  - a->tv_sec);
    double nsec = (double)(b->tv_nsec - a->tv_nsec);
    return sec * 1e6 + nsec / 1e3;
}

static uint32_t bits(uint32_t w, unsigned hi, unsigned lo)
{
    uint32_t mask = (hi - lo == 31u) ? 0xFFFFFFFFu
                                     : ((1u << (hi - lo + 1u)) - 1u);
    return (w >> lo) & mask;
}

/* Mnemonic decode for the instructions actually appearing in
 * optical_flow.S. Bits used: opcode[6:0], funct3[14:12], funct6[31:26],
 * vm[25]. Anything else returns "??" so a kernel swap shows up clearly
 * in the CSV. */
static const char *mnemonic(uint32_t w)
{
    uint32_t opcode = bits(w, 6, 0);
    uint32_t funct3 = bits(w, 14, 12);
    uint32_t funct6 = bits(w, 31, 26);
    uint32_t vm     = bits(w, 25, 25);
    uint32_t csr    = bits(w, 31, 20);

    if (opcode == AKD_OPCODE_LOAD_FP)  return "vle8.v";
    if (opcode == AKD_OPCODE_STORE_FP) return "vse8.v";
    if (opcode == AKD_OPCODE_SYSTEM && csr == AKD_CSR_VERTMODE) {
        if (funct3 == AKD_FUNCT3_CSRRWI) {
            uint32_t imm = bits(w, 19, 15);
            return imm ? "csrwi(vmode=1)" : "csrwi(vmode=0)";
        }
        if (funct3 == AKD_FUNCT3_CSRRW) return "csrw(vmode<-x0)";
        return "csrw(vmode<-?)";
    }
    if (opcode == AKD_OPCODE_OP_V) {
        if (funct3 == AKD_FUNCT3_OPIVI) {       /* OPIVI */
            switch (funct6) {
                case 0x17u:
                    /* vmv.v.i (vm=1, unmasked) vs vmerge.vim (vm=0,
                     * masked from v0). They share funct6. */
                    return vm ? "vmv.v.i" : "vmerge.vim";
                case 0x0Fu: return "vslidedown.vi";
                case 0x0Eu: return "vslideup.vi";
                case 0x03u: return "vrsub.vi";
                case 0x1Cu: return "vmsleu.vi";
                default:    return "OPIVI?";
            }
        }
        if (funct3 == AKD_FUNCT3_OPIVV) {       /* OPIVV */
            switch (funct6) {
                case 0x02u: return "vsub.vv";
                case 0x07u: return "vmax.vv";
                case 0x21u: return "vsadd.vv";
                case 0x04u: return "vminu.vv";
                case 0x1Au: return "vmsltu.vv";
                default:    return "OPIVV?";
            }
        }
        if (funct3 == AKD_FUNCT3_OPMVX) {       /* OPMVX */
            switch (funct6) {
                case 0x25u: return "vmul.vx";
                default:    return "OPMVX?";
            }
        }
        if (funct3 == AKD_FUNCT3_OPIVX) {       /* OPIVX */
            return "OPIVX?";
        }
        return "OP-V?";
    }
    return "??";
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [--iters N] [--out path]\n"
        "  --iters N   number of kernel iterations "
        "(default %u, env VISIONSOC_OPTICAL_FLOW_ITERS)\n"
        "  --out path  CSV destination (default stdout)\n",
        argv0, DEFAULT_ITERS);
}

int main(int argc, char **argv)
{
    unsigned iters = DEFAULT_ITERS;
    const char *out_path = NULL;
    const char *env_iters = getenv("VISIONSOC_OPTICAL_FLOW_ITERS");
    if (env_iters && *env_iters) {
        iters = (unsigned)strtoul(env_iters, NULL, 0);
    }

    static const struct option opts[] = {
        {"iters", required_argument, NULL, 'i'},
        {"out",   required_argument, NULL, 'o'},
        {"help",  no_argument,       NULL, 'h'},
        {0, 0, 0, 0},
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
    if (iters == 0 || iters > (sizeof(samples) / sizeof(samples[0]))) {
        fprintf(stderr, "iters must be in 1..%zu\n",
                sizeof(samples) / sizeof(samples[0]));
        return 2;
    }
    if (optical_flow_count > MAX_INSTR) {
        fprintf(stderr,
                "kernel has %u words > MAX_INSTR=%u; bump MAX_INSTR\n",
                optical_flow_count, MAX_INSTR);
        return 2;
    }

    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) die("fopen out");
    }

    if (t1_init() < 0) die("t1_init");

    struct t1_buf in_buf      = {0};
    struct t1_buf prev_buf    = {0};
    struct t1_buf out_buf     = {0};
    if (t1_buf_alloc(&in_buf,   FRAME_BYTES) < 0) die("t1_buf_alloc in");
    if (t1_buf_alloc(&prev_buf, FRAME_BYTES) < 0) die("t1_buf_alloc prev");
    if (t1_buf_alloc(&out_buf,  FRAME_BYTES) < 0) die("t1_buf_alloc out");

    /* Synthetic curr: curr[r][c] = (16*r + c) & 127. The 16*r weight breaks
     * the H/V symmetry that (r+c)&127 has (where right- and down- shifted
     * prev give identical SAD because the gradient is the same in r and c
     * directions). The row weight also keeps SAD_static above the optical
     * flow kernel's <=6 luma noise threshold.
     *
     * Synthetic prev: shifted UP by 1 row in image space, so when the
     * kernel compares curr to "prev shifted down" it gets SAD = 0 at
     * every interior pixel:
     *
     *   prev_shifted_down[r][c] = prev[r-1][c]
     *                           = (16*((r-1)+1) + c) & 127
     *                           = (16*r + c) & 127
     *                           = curr[r][c]
     *
     * Other candidates give non-zero SAD:
     *   SAD_static is usually 16, above the threshold
     *   SAD_down   (curr vs prev[r-1][c])  = 0   <-- winner
     *
     * Boundary rows/cols have nowhere to compare to (vslide zero-fill),
     * so they may pick different argmin or stay at static = 0. */
    uint8_t *pc = in_buf.va;
    uint8_t *pp = prev_buf.va;
    for (unsigned r = 0; r < 128u; r++) {
        for (unsigned cc = 0; cc < 128u; cc++) {
            pc[r * 128u + cc] = (uint8_t)((16u * r + cc) & 127u);
            pp[r * 128u + cc] = (uint8_t)((16u * (r + 1u) + cc) & 127u);
        }
    }
    if (t1_buf_sync_for_device(&in_buf)   < 0) die("sync_for_device in");
    if (t1_buf_sync_for_device(&prev_buf) < 0) die("sync_for_device prev");

    /* DMA DDR -> URAM_HALF_A once (curr is constant across iters). */
    if (t1_dma_s2mm_async(0, URAM_HALF_A, FRAME_BYTES) < 0)
        die("dma s2mm arm uram_a");
    if (t1_dma_mm2s_async(in_buf.pa, 0, FRAME_BYTES) < 0)
        die("dma mm2s in");
    if (t1_dma_wait() < 0) die("dma wait in->uram_a");

    fprintf(stderr, "optical_flow_perf: iters=%u, instructions=%u\n",
            iters, optical_flow_count);

    /* LMUL=1 for the "big" config (vLen=1024): one register = 128 e8
     * elements = one image row. Matches optical_flow_select.h. */
    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M1_TA_MA,
        .vl = 128,
    };

    for (unsigned iter = 0; iter < iters; iter++) {
        /* Re-DMA prev -> OFK_PREV_PA BEFORE the per-instruction timing
         * window. The kernel itself overwrites OFK_PREV_PA on every iter
         * via the index-40 vse8.v v8, (a1), so without this re-arm every
         * iter from #1 onwards would see prev == curr and produce 0 after
         * the static deadband. */
        if (t1_dma_s2mm_async(0, OFK_PREV_PA, FRAME_BYTES) < 0)
            die("dma s2mm arm prev");
        if (t1_dma_mm2s_async(prev_buf.pa, 0, FRAME_BYTES) < 0)
            die("dma mm2s prev");
        if (t1_dma_wait() < 0) die("dma wait prev->uram_prev");

        int vmode = 0;
        for (uint32_t i = 0; i < optical_flow_count; i++) {
            uint32_t w      = optical_flow[i];
            uint32_t opcode = bits(w, 6, 0);
            uint32_t funct3 = bits(w, 14, 12);
            uint32_t rs1    = bits(w, 19, 15);
            uint32_t csr    = bits(w, 31, 20);

            /* CSR 0x7c0 mode toggle? Tracked but NOT issued (mirrors
             * optical_flow_select.h). */
            if (opcode == AKD_OPCODE_SYSTEM && csr == AKD_CSR_VERTMODE) {
                struct timespec t0, t1;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                __asm__ __volatile__("" ::: "memory");
                if (funct3 == AKD_FUNCT3_CSRRWI) {
                    vmode = (rs1 ? 1 : 0);
                } else if (funct3 == AKD_FUNCT3_CSRRW &&
                           rs1 == AKD_REG_X0) {
                    vmode = 0;
                } else {
                    fprintf(stderr,
                            "unsupported csrw form at word %u\n", i);
                    exit(1);
                }
                __asm__ __volatile__("" ::: "memory");
                clock_gettime(CLOCK_MONOTONIC, &t1);
                samples[iter][i].is_csr_flip = 1;
                samples[iter][i].t1_cycles = 0;
                samples[iter][i].wall_us = us_between(&t0, &t1);
                continue;
            }

            /* LSU and OPIVX rs1 routing (mirrors optical_flow_select.h). */
            if (opcode == AKD_OPCODE_LOAD_FP ||
                opcode == AKD_OPCODE_STORE_FP) {
                if      (rs1 == AKD_REG_A0) op.rs1 = URAM_HALF_A;
                else if (rs1 == AKD_REG_A1) op.rs1 = OFK_PREV_PA;
                else if (rs1 == AKD_REG_A2) op.rs1 = URAM_HALF_B;
                else                        op.rs1 = 0u;
            } else if (opcode == AKD_OPCODE_OP_V &&
                       (funct3 == AKD_FUNCT3_OPIVX ||
                        funct3 == AKD_FUNCT3_OPMVX)) {
                if (rs1 == AKD_REG_A3) op.rs1 = AKD_DIR_SCALE;
                else                   op.rs1 = 0u;
            } else {
                op.rs1 = 0u;
            }
            op.instruction = w;
            op.vertical_mode = (uint8_t)vmode;

            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            __asm__ __volatile__("" ::: "memory");
            volatile uint32_t start = t1_perf_start((uint8_t)((i & 0x7Fu) + 1u));
            int rc = t1_issue(&op);
            /* Wait for THIS instruction's full writeback drain (ISSUE_READY
             * high again) INSIDE the perf bracket. t1_issue() returns at
             * acceptance for compute ops, so without this the op's drain
             * leaks into the next issue's wait and the CSV is shifted by one
             * instruction. See perf/perf_doc/perf_status.md. */
            if (rc >= 0) rc = t1_wait_ready();
            volatile uint32_t cycles = t1_perf_stop();
            __asm__ __volatile__("" ::: "memory");
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (rc < 0) {
                fprintf(stderr,
                        "t1_issue/wait_ready failed at iter %u word %u: %s\n",
                        iter, i, strerror(errno));
                exit(1);
            }
            samples[iter][i].is_csr_flip = 0;
            samples[iter][i].t1_cycles   = cycles;
            samples[iter][i].wall_us     = us_between(&t0, &t1);
            (void)start;
        }
    }

    /* DMA URAM_HALF_B -> DDR once so we can verify the kernel produced
     * a real motion-direction map. */
    if (t1_dma_s2mm_async(0, out_buf.pa, FRAME_BYTES) < 0)
        die("dma s2mm arm out");
    if (t1_dma_mm2s_async(URAM_HALF_B, 0, FRAME_BYTES) < 0)
        die("dma mm2s uram_b");
    if (t1_dma_wait() < 0) die("dma wait uram_b->out");
    if (t1_buf_sync_for_cpu(&out_buf) < 0) die("sync_for_cpu out");

    /* Sanity: with prev = curr shifted up by 1 row, the "down" candidate
     * (which compares curr against prev shifted down) hits SAD=0 at
     * every interior pixel. Argmin = 3 -> output = 150. Boundary rows
     * may pick a different argmin because of the vslide zero-fill. */
    const uint8_t *po = out_buf.va;
    fprintf(stderr,
            "optical_flow_perf: sanity (interior pixels should be 150 "
            "= argmin 'down' x50; boundaries may differ):\n");
    fprintf(stderr,
            "  out[ 1][ 1]=%u  out[64][64]=%u  out[64][65]=%u  "
            "out[100][100]=%u\n",
            po[1 * 128 + 1], po[64 * 128 + 64], po[64 * 128 + 65],
            po[100 * 128 + 100]);
    fprintf(stderr,
            "  out[  0][  0]=%u  out[127][127]=%u\n",
            po[0], po[127 * 128 + 127]);

    /* CSV emit. One row per (iter, instr). */
    fprintf(out, "iter,instr_idx,mnemonic,is_csr_flip,t1_cycles,wall_us\n");
    for (unsigned iter = 0; iter < iters; iter++) {
        for (uint32_t i = 0; i < optical_flow_count; i++) {
            fprintf(out, "%u,%u,%s,%u,%u,%.3f\n",
                    iter, i, mnemonic(optical_flow[i]),
                    samples[iter][i].is_csr_flip,
                    samples[iter][i].t1_cycles,
                    samples[iter][i].wall_us);
        }
    }
    if (out != stdout) fclose(out);

    t1_buf_free(&in_buf);
    t1_buf_free(&prev_buf);
    t1_buf_free(&out_buf);
    t1_close();
    return 0;
}
