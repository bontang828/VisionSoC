/*
 * matmul_8bitraw_short_perf -- standalone T1 perf harness for the short
 * raw-8 matmul kernel.
 *
 * The .S has 903 logical words per run:
 *   6 preamble words + (7 body words * 128 columns) + 1 postamble word.
 * Of those, 258 are scalar CSR-mode markers tracked by this harness, so
 * each run issues 645 T1 vector ops.
 *
 * The CSV deliberately aggregates the repeated body words into seven logical
 * rows with "x128" mnemonics. Each individual T1 issue is still bracketed by
 * t1_perf_start/stop and t1_wait_ready(); body CSR-marker wall time is also
 * accumulated. This keeps the plot readable while preserving total per-kernel
 * cycles for issued T1 ops.
 */
#include "kernels/matmul_8bitraw_short.h"
#include "kernels/matmul_weights.h"
#include "libt1.h"
#include "libt1_regs.h"

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define URAM_BASE_PA   0xA0080000u
#define URAM_HALF_A    (URAM_BASE_PA + 0x00000u)
#define URAM_HALF_B    (URAM_BASE_PA + 0x04000u)
#define MMK_WEIGHT_PA  (URAM_BASE_PA + 0x08000u)

#define FRAME_BYTES    (128u * 128u)
#define DEFAULT_ITERS  100u
#define MAX_ITERS      1024u

#define MMK_PRE_BASE    0u
#define MMK_PRE_COUNT   6u
#define MMK_BODY_BASE   6u
#define MMK_BODY_COUNT  7u
#define MMK_POST_BASE   13u
#define MMK_LOGICAL_COUNT 14u
#define MMK_ITERS       128u
#define MMK_ISSUED_T1_OPS_PER_RUN 645u

#define MMK_REG_X0   0u
#define MMK_REG_A0  10u
#define MMK_REG_A1  11u
#define MMK_REG_A2  12u

#define MMK_OPCODE_SYSTEM    0x73u
#define MMK_FUNCT3_CSRRWI    0x5u
#define MMK_FUNCT3_CSRRW     0x1u
#define MMK_CSR_VERTMODE     0x7C0u
#define MMK_OPCODE_LOAD_FP   0x07u
#define MMK_OPCODE_STORE_FP  0x27u

struct body_step {
    uint8_t inject_k;
};

struct sample {
    uint8_t  is_csr_flip;
    uint64_t t1_cycles;
    double   wall_us;
};

static const struct body_step body_steps[MMK_BODY_COUNT] = {
    { 0u },  /* vslideup.vi           */
    { 0u },  /* vmv.v.v               */
    { 0u },  /* csrwi 0x7c0, 1 marker */
    { 1u },  /* vrgather.vx           */
    { 0u },  /* csrwi 0x7c0, 0 marker */
    { 0u },  /* vmul.vv               */
    { 0u },  /* vredsum.vs            */
};

static struct sample samples[MAX_ITERS][MMK_LOGICAL_COUNT];

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

static const char *logical_mnemonic(uint32_t idx)
{
    switch (idx) {
        case 0u:  return "vle8.v A";
        case 1u:  return "csrwi(vmode=1)";
        case 2u:  return "vle8.v B^T";
        case 3u:  return "csrwi(vmode=0)";
        case 4u:  return "vmv.v.i v7";
        case 5u:  return "vmv.v.i v28";
        case 6u:  return "vslideup.vi x128";
        case 7u:  return "vmv.v.v x128";
        case 8u:  return "csrwi V=1 x128";
        case 9u:  return "vrgather.vx x128";
        case 10u: return "csrwi V=0 x128";
        case 11u: return "vmul.vv x128";
        case 12u: return "vredsum.vs x128";
        case 13u: return "vse8.v C";
        default:  return "??";
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [--iters N] [--out path]\n"
        "  --iters N   number of kernel runs (default %u, env "
        "VISIONSOC_MATMUL_8BITRAW_SHORT_ITERS)\n"
        "  --out path  CSV destination (default stdout)\n",
        argv0, DEFAULT_ITERS);
}

static uint32_t route_lsu_rs1(uint32_t rs1, uint32_t src_pa, uint32_t dst_pa)
{
    switch (rs1) {
        case MMK_REG_A0: return src_pa;
        case MMK_REG_A1: return MMK_WEIGHT_PA;
        case MMK_REG_A2: return dst_pa;
        default:         return 0u;
    }
}

static int handle_csr_flip(uint32_t iter, uint32_t idx, uint32_t w, int *vmode)
{
    uint32_t opcode = bits(w, 6, 0);
    uint32_t funct3 = bits(w, 14, 12);
    uint32_t rs1    = bits(w, 19, 15);
    uint32_t csr    = bits(w, 31, 20);

    if (opcode != MMK_OPCODE_SYSTEM || csr != MMK_CSR_VERTMODE) {
        return 0;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    __asm__ __volatile__("" ::: "memory");
    if (funct3 == MMK_FUNCT3_CSRRWI) {
        *vmode = rs1 ? 1 : 0;
    } else if (funct3 == MMK_FUNCT3_CSRRW && rs1 == MMK_REG_X0) {
        *vmode = 0;
    } else {
        fprintf(stderr, "unsupported csrw form at logical word %u\n", idx);
        exit(1);
    }
    __asm__ __volatile__("" ::: "memory");
    clock_gettime(CLOCK_MONOTONIC, &t1);

    samples[iter][idx].is_csr_flip = 1u;
    samples[iter][idx].wall_us += us_between(&t0, &t1);
    return 1;
}

static void time_issue(uint32_t iter, uint32_t idx, struct t1_op *op)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    __asm__ __volatile__("" ::: "memory");
    volatile uint32_t start = t1_perf_start((uint8_t)((idx & 0x7Fu) + 1u));
    int rc = t1_issue(op);
    if (rc >= 0) rc = t1_wait_ready();
    volatile uint32_t cycles = t1_perf_stop();
    __asm__ __volatile__("" ::: "memory");
    clock_gettime(CLOCK_MONOTONIC, &t1);

    if (rc < 0) {
        fprintf(stderr,
                "t1_issue/wait_ready failed at iter %u logical word %u: %s\n",
                iter, idx, strerror(errno));
        exit(1);
    }

    samples[iter][idx].is_csr_flip = 0u;
    samples[iter][idx].t1_cycles += cycles;
    samples[iter][idx].wall_us += us_between(&t0, &t1);
    (void)start;
}

int main(int argc, char **argv)
{
    unsigned iters = DEFAULT_ITERS;
    const char *out_path = NULL;
    const char *env_iters = getenv("VISIONSOC_MATMUL_8BITRAW_SHORT_ITERS");
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
    if (iters == 0 || iters > MAX_ITERS) {
        fprintf(stderr, "iters must be in 1..%u\n", MAX_ITERS);
        return 2;
    }
    if (matmul_8bitraw_short_count != MMK_LOGICAL_COUNT) {
        fprintf(stderr, "kernel has %u words, expected %u\n",
                matmul_8bitraw_short_count, MMK_LOGICAL_COUNT);
        return 2;
    }

    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) die("fopen out");
    }

    if (t1_init() < 0) die("t1_init");

    struct t1_buf in = {0};
    struct t1_buf weights = {0};
    struct t1_buf out_buf = {0};
    if (t1_buf_alloc(&in, FRAME_BYTES) < 0) die("t1_buf_alloc in");
    if (t1_buf_alloc(&weights, FRAME_BYTES) < 0) die("t1_buf_alloc weights");
    if (t1_buf_alloc(&out_buf, FRAME_BYTES) < 0) die("t1_buf_alloc out");

    uint8_t *pi = in.va;
    for (unsigned r = 0; r < 128u; r++) {
        for (unsigned col = 0; col < 128u; col++) {
            pi[r * 128u + col] = (uint8_t)((17u * r + col) & 0xFFu);
        }
    }
    mmk_build_weights((uint8_t *)weights.va, MMK_B_SHIFT);

    if (t1_buf_sync_for_device(&in) < 0) die("sync_for_device in");
    if (t1_buf_sync_for_device(&weights) < 0) die("sync_for_device weights");

    if (t1_dma_s2mm_async(0, URAM_HALF_A, FRAME_BYTES) < 0)
        die("dma s2mm arm uram_a");
    if (t1_dma_mm2s_async(in.pa, 0, FRAME_BYTES) < 0)
        die("dma mm2s in");
    if (t1_dma_wait() < 0) die("dma wait in->uram_a");

    if (t1_dma_s2mm_async(0, MMK_WEIGHT_PA, FRAME_BYTES) < 0)
        die("dma s2mm arm weights");
    if (t1_dma_mm2s_async(weights.pa, 0, FRAME_BYTES) < 0)
        die("dma mm2s weights");
    if (t1_dma_wait() < 0) die("dma wait weights->uram");

    fprintf(stderr,
            "matmul_8bitraw_short_perf: iters=%u, logical_rows=%u, "
            "logical_words_per_iter=%u, issued_t1_ops_per_iter=%u\n",
            iters, MMK_LOGICAL_COUNT,
            MMK_PRE_COUNT + MMK_BODY_COUNT * MMK_ITERS + 1u,
            MMK_ISSUED_T1_OPS_PER_RUN);

    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M1_TA_MA,
        .vl = 128u,
    };

    for (uint32_t iter = 0; iter < iters; iter++) {
        int vmode = 0;

        for (uint32_t i = MMK_PRE_BASE; i < MMK_PRE_BASE + MMK_PRE_COUNT; i++) {
            uint32_t w = matmul_8bitraw_short[i];
            if (handle_csr_flip(iter, i, w, &vmode)) {
                continue;
            }

            uint32_t opcode = bits(w, 6, 0);
            uint32_t rs1    = bits(w, 19, 15);
            op.instruction = w;
            op.vtype = T1_VTYPE_E8_M1_TA_MA;
            op.vl = 128u;
            op.rs1 = (opcode == MMK_OPCODE_LOAD_FP ||
                      opcode == MMK_OPCODE_STORE_FP)
                         ? route_lsu_rs1(rs1, URAM_HALF_A, URAM_HALF_B)
                         : 0u;
            op.vertical_mode = (uint8_t)vmode;
            time_issue(iter, i, &op);
        }

        for (int32_t k = (int32_t)MMK_ITERS - 1; k >= 0; k--) {
            for (uint32_t p = 0; p < MMK_BODY_COUNT; p++) {
                uint32_t idx = MMK_BODY_BASE + p;
                uint32_t w = matmul_8bitraw_short[idx];
                if (handle_csr_flip(iter, idx, w, &vmode)) {
                    continue;
                }

                op.instruction = w;
                op.vtype = T1_VTYPE_E8_M1_TA_MA;
                op.vl = 128u;
                op.rs1 = body_steps[p].inject_k ? (uint32_t)k : 0u;
                op.vertical_mode = (uint8_t)vmode;
                time_issue(iter, idx, &op);
            }
        }

        {
            uint32_t w = matmul_8bitraw_short[MMK_POST_BASE];
            uint32_t rs1 = bits(w, 19, 15);
            op.instruction = w;
            op.vtype = T1_VTYPE_E8_M1_TA_MA;
            op.vl = 128u;
            op.rs1 = route_lsu_rs1(rs1, URAM_HALF_A, URAM_HALF_B);
            op.vertical_mode = 0u;
            time_issue(iter, MMK_POST_BASE, &op);
        }
    }

    if (t1_dma_s2mm_async(0, out_buf.pa, FRAME_BYTES) < 0)
        die("dma s2mm arm out");
    if (t1_dma_mm2s_async(URAM_HALF_B, 0, FRAME_BYTES) < 0)
        die("dma mm2s uram_b");
    if (t1_dma_wait() < 0) die("dma wait uram_b->out");
    if (t1_buf_sync_for_cpu(&out_buf) < 0) die("sync_for_cpu out");

    const uint8_t *po = out_buf.va;
    unsigned mismatches = 0;
    for (unsigned r = 0; r < 128u; r++) {
        for (unsigned col = 0; col < 128u; col++) {
            uint8_t expected = (col + MMK_B_SHIFT_D < 128u)
                ? pi[r * 128u + col + MMK_B_SHIFT_D]
                : 0u;
            if (po[r * 128u + col] != expected) {
                mismatches++;
            }
        }
    }

    fprintf(stderr,
            "matmul_8bitraw_short_perf: sanity for MMK_B_SHIFT=%u "
            "(expected C[r][k]=A[r][k+%u], tail zero): mismatches=%u\n",
            MMK_B_SHIFT_D, MMK_B_SHIFT_D, mismatches);
    fprintf(stderr,
            "  out[ 1][ 1]=%u  exp=%u   out[64][64]=%u  exp=%u   "
            "out[64][121]=%u  exp=0\n",
            po[1 * 128u + 1u], pi[1 * 128u + 1u + MMK_B_SHIFT_D],
            po[64 * 128u + 64u], pi[64 * 128u + 64u + MMK_B_SHIFT_D],
            po[64 * 128u + 121u]);

    fprintf(out, "iter,instr_idx,mnemonic,is_csr_flip,t1_cycles,wall_us\n");
    for (unsigned iter = 0; iter < iters; iter++) {
        for (uint32_t i = 0; i < MMK_LOGICAL_COUNT; i++) {
            fprintf(out, "%u,%u,%s,%u,%llu,%.3f\n",
                    iter, i, logical_mnemonic(i),
                    samples[iter][i].is_csr_flip,
                    (unsigned long long)samples[iter][i].t1_cycles,
                    samples[iter][i].wall_us);
        }
    }
    if (out != stdout) fclose(out);

    t1_buf_free(&in);
    t1_buf_free(&weights);
    t1_buf_free(&out_buf);
    t1_close();
    return 0;
}
