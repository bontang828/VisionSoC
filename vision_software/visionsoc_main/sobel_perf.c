/*
 * sobel_perf -- standalone T1 instruction-level perf harness.
 *
 * Feeds a fixed synthetic gradient frame through the same
 * DDR -> URAM_A -> T1(sobel) -> URAM_B -> DDR path as main.c, but
 * opens the dispatcher loop so each issued instruction is bracketed
 * with t1_perf_start/stop (T1 hardware cycles) and clock_gettime
 * (PS wall-clock microseconds). One CSV row is emitted per
 * (iteration, instruction); a Python plotter aggregates.
 *
 * Compiler-optimisation notes:
 *   - t1_perf_start/stop, t1_issue, clock_gettime are external function
 *     calls that all touch volatile MMIO or syscalls. The compiler may
 *     not reorder them.
 *   - Return values are stored into `volatile` locals before being
 *     copied into the per-instruction array. No DCE path.
 *   - `__asm__ __volatile__("" ::: "memory")` barriers between
 *     clock_gettime and t1_perf_start (both ends) forbid any speculative
 *     register motion across the timing window.
 *
 * See plan file please-can-yo-uread-pure-chipmunk.md for the full
 * design rationale.
 */
#include "kernels/sobel.h"
#include "libt1.h"
#include "libt1_regs.h"

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Same URAM layout as main.c (main.c:24-26). Kept literal here so
 * sobel_perf doesn't depend on main.c. */
#define URAM_BASE_PA 0xA0080000u
#define URAM_HALF_A  (URAM_BASE_PA + 0x00000u)
#define URAM_HALF_B  (URAM_BASE_PA + 0x04000u)

#define FRAME_BYTES  (128u * 128u)
#define DEFAULT_ITERS 100u
#define MAX_INSTR 64u  /* sobel_count is 19; leave headroom for kernel swaps */

/* AKD decode constants (mirror active_kernel_dispatcher.h:54-66). */
#define AKD_REG_X0  0u
#define AKD_REG_A0 10u
#define AKD_REG_A1 11u
#define AKD_OPCODE_SYSTEM   0x73u
#define AKD_FUNCT3_CSRRWI   0x5u
#define AKD_FUNCT3_CSRRW    0x1u
#define AKD_CSR_VERTMODE    0x7C0u
#define AKD_OPCODE_LOAD_FP  0x07u
#define AKD_OPCODE_STORE_FP 0x27u
#define AKD_OPCODE_OP_V     0x57u

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
    double sec = (double)(b->tv_sec - a->tv_sec);
    double nsec = (double)(b->tv_nsec - a->tv_nsec);
    return sec * 1e6 + nsec / 1e3;
}

/* Decoder helpers. funct6 = bits[31:26]; rs1 = bits[19:15]; opcode bits[6:0];
 * funct3 bits[14:12]; uimm-5 for csrrwi is also bits[19:15]. */
static uint32_t bits(uint32_t w, unsigned hi, unsigned lo)
{
    uint32_t mask = (hi - lo == 31u) ? 0xFFFFFFFFu : ((1u << (hi - lo + 1u)) - 1u);
    return (w >> lo) & mask;
}

/* Returns a static string per (opcode, funct6, funct3) combination used
 * inside sobel.S. Anything unrecognised returns "??" so a kernel swap
 * shows up clearly in the CSV. */
static const char *mnemonic(uint32_t w)
{
    uint32_t opcode = bits(w, 6, 0);
    uint32_t funct3 = bits(w, 14, 12);
    uint32_t funct6 = bits(w, 31, 26);
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
        if (funct3 == 0x3u) {   /* OPIVI */
            switch (funct6) {
                case 0x17u: return "vmv.v.i";
                case 0x0Fu: return "vslidedown.vi";
                case 0x0Eu: return "vslideup.vi";
                case 0x03u: return "vrsub.vi";
                default:    return "OPIVI?";
            }
        }
        if (funct3 == 0x0u) {   /* OPIVV */
            switch (funct6) {
                case 0x02u: return "vsub.vv";
                case 0x07u: return "vmax.vv";
                case 0x21u: return "vsadd.vv";
                default:    return "OPIVV?";
            }
        }
        return "OP-V?";
    }
    return "??";
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [--iters N] [--out path]\n"
        "  --iters N   number of kernel iterations (default %u, env VISIONSOC_SOBEL_ITERS)\n"
        "  --out path  CSV destination (default stdout)\n",
        argv0, DEFAULT_ITERS);
}

int main(int argc, char **argv)
{
    unsigned iters = DEFAULT_ITERS;
    const char *out_path = NULL;
    const char *env_iters = getenv("VISIONSOC_SOBEL_ITERS");
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
    if (sobel_count > MAX_INSTR) {
        fprintf(stderr, "kernel has %u words > MAX_INSTR=%u; bump MAX_INSTR\n",
                sobel_count, MAX_INSTR);
        return 2;
    }

    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) die("fopen out");
    }

    if (t1_init() < 0) die("t1_init");

    struct t1_buf in = {0};
    struct t1_buf out_buf = {0};
    if (t1_buf_alloc(&in, FRAME_BYTES) < 0) die("t1_buf_alloc in");
    if (t1_buf_alloc(&out_buf, FRAME_BYTES) < 0) die("t1_buf_alloc out");

    /* Synthetic gradient: pix(r,c) = (r+c) & 127. Horizontal gradient is
     * +1 except at the wrap; vertical gradient is +1 except at the wrap;
     * |Gx| + |Gy| ~ 2 everywhere away from the wrap row/column. */
    uint8_t *p = in.va;
    for (unsigned r = 0; r < 128u; r++) {
        for (unsigned cc = 0; cc < 128u; cc++) {
            p[r * 128u + cc] = (uint8_t)((r + cc) & 127u);
        }
    }
    if (t1_buf_sync_for_device(&in) < 0) die("sync_for_device in");

    /* DMA DDR -> URAM_HALF_A once (input is constant across iters). */
    if (t1_dma_s2mm_async(0, URAM_HALF_A, FRAME_BYTES) < 0) die("dma s2mm arm uram_a");
    if (t1_dma_mm2s_async(in.pa, 0, FRAME_BYTES) < 0)       die("dma mm2s in");
    if (t1_dma_wait() < 0) die("dma wait in->uram_a");

    fprintf(stderr, "sobel_perf: iters=%u, instructions=%u\n",
            iters, sobel_count);

    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M4_TA_MA,
        .vl = 128,
    };

    for (unsigned iter = 0; iter < iters; iter++) {
        int vmode = 0;
        for (uint32_t i = 0; i < sobel_count; i++) {
            uint32_t w = sobel[i];
            uint32_t opcode = bits(w, 6, 0);
            uint32_t funct3 = bits(w, 14, 12);
            uint32_t rs1    = bits(w, 19, 15);
            uint32_t csr    = bits(w, 31, 20);

            /* CSR 0x7c0 mode toggle? Track but do NOT issue. Matches
             * active_kernel_dispatcher.h:94-108. */
            if (opcode == AKD_OPCODE_SYSTEM && csr == AKD_CSR_VERTMODE) {
                struct timespec t0, t1;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                __asm__ __volatile__("" ::: "memory");
                if (funct3 == AKD_FUNCT3_CSRRWI) {
                    vmode = (rs1 ? 1 : 0);
                } else if (funct3 == AKD_FUNCT3_CSRRW && rs1 == AKD_REG_X0) {
                    vmode = 0;
                } else {
                    fprintf(stderr, "unsupported csrw form at word %u\n", i);
                    exit(1);
                }
                __asm__ __volatile__("" ::: "memory");
                clock_gettime(CLOCK_MONOTONIC, &t1);
                samples[iter][i].is_csr_flip = 1;
                samples[iter][i].t1_cycles = 0;
                samples[iter][i].wall_us = us_between(&t0, &t1);
                continue;
            }

            /* LSU: route op.rs1 by which a-register the kernel named.
             * Same convention as active_kernel_dispatcher.h:110-122. */
            if (opcode == AKD_OPCODE_LOAD_FP || opcode == AKD_OPCODE_STORE_FP) {
                if (rs1 == AKD_REG_A0)      op.rs1 = URAM_HALF_A;
                else if (rs1 == AKD_REG_A1) op.rs1 = URAM_HALF_B;
                else                        op.rs1 = 0u;
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
                fprintf(stderr, "t1_issue/wait_ready failed at iter %u word %u: %s\n",
                        iter, i, strerror(errno));
                exit(1);
            }
            samples[iter][i].is_csr_flip = 0;
            samples[iter][i].t1_cycles = cycles;
            samples[iter][i].wall_us = us_between(&t0, &t1);
            (void)start;
        }
    }

    /* DMA URAM_HALF_B -> DDR once so we can verify the kernel actually
     * produced sobel output, not zeros or passthrough. */
    if (t1_dma_s2mm_async(0, out_buf.pa, FRAME_BYTES) < 0) die("dma s2mm arm out");
    if (t1_dma_mm2s_async(URAM_HALF_B, 0, FRAME_BYTES) < 0) die("dma mm2s uram_b");
    if (t1_dma_wait() < 0) die("dma wait uram_b->out");
    if (t1_buf_sync_for_cpu(&out_buf) < 0) die("sync_for_cpu out");

    /* Sanity print: input is (r+c)&127, sobel of that has |Gx|+|Gy|
     * close to 2 in the interior (away from the wrap row/column).
     * Print a few sample pixels so the user can see we're not all-zero
     * (bypass) or input-identical (passthrough). */
    const uint8_t *po = out_buf.va;
    fprintf(stderr, "sobel_perf: sanity (interior pixels should be ~2, "
                    "wrap-row/col may saturate):\n");
    fprintf(stderr, "  out[ 1][ 1]=%u  out[64][64]=%u  out[64][65]=%u  "
                    "out[100][100]=%u\n",
            po[1 * 128 + 1], po[64 * 128 + 64], po[64 * 128 + 65],
            po[100 * 128 + 100]);
    fprintf(stderr, "  out[  0][  0]=%u  out[127][127]=%u\n",
            po[0], po[127 * 128 + 127]);

    /* CSV emit. One row per (iter, instr). */
    fprintf(out, "iter,instr_idx,mnemonic,is_csr_flip,t1_cycles,wall_us\n");
    for (unsigned iter = 0; iter < iters; iter++) {
        for (uint32_t i = 0; i < sobel_count; i++) {
            fprintf(out, "%u,%u,%s,%u,%u,%.3f\n",
                    iter, i, mnemonic(sobel[i]),
                    samples[iter][i].is_csr_flip,
                    samples[iter][i].t1_cycles,
                    samples[iter][i].wall_us);
        }
    }
    if (out != stdout) fclose(out);

    t1_buf_free(&in);
    t1_buf_free(&out_buf);
    t1_close();
    return 0;
}
