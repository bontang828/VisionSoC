/*
 * attention_self_perf -- standalone T1 perf harness for the self-attention patch
 * pipeline (attention_self), emitting a PER-INSTRUCTION breakdown in the same
 * style as sobel_perf / optical_flow_perf / matmul_8bitraw_short_perf: every
 * logical kernel word is one CSV row, and the repeated loop-body words are
 * aggregated (QK body x512 = 128 keys x 2 key-blocks x 2 query-blocks; PV body
 * x128 = 64 features x 2 query-blocks). Each individual T1 issue is bracketed
 * with t1_perf_start/stop + t1_wait_ready and accumulated into its logical row.
 *
 * No camera: a deterministic synthetic frame is staged, then the exact
 * attention_self pipeline (im2col -> transpose -> attention) is replayed with
 * per-issue timing. CSV schema is universal:
 *   iter,instr_idx,mnemonic,is_csr_flip,t1_cycles,wall_us
 *
 *   build: (in visionsoc_main) make attention_self_perf
 *   run:   sudo ./attention_self_perf [--iters N] [--out path]
 */
#include "kernels/attention_patch_weights.h"
#include "kernels/attention_patch_im2col.h"
#include "kernels/attention_self.h"
#include "kernels/attention_self_transpose.h"
#include "kernels/attention_patch_issue.h"   /* AP_E8_M1/E16_M1/E16_M2/E32_M1, AP_C_* */
#include "libt1.h"

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TILE 16384u
#define FRAMEPAD (192u * 128u)
#define MAX_ITERS 256u

#define O_FRAME 0u
#define O_IDXH1 (O_FRAME + FRAMEPAD)
#define O_IDXV  (O_IDXH1 + TILE)
#define O_IDXH2 (O_IDXV  + TILE)
#define O_EXP   (O_IDXH2 + TILE)
#define O_SEED  (O_EXP + TILE)
#define O_QA    (O_SEED + TILE)
#define O_QB    (O_QA + TILE)
#define O_KTA   (O_QB + TILE)
#define O_KTB   (O_KTA + TILE)
#define O_OA    (O_KTB + TILE)
#define O_OB    (O_OA + TILE)
#define STAGE_BYTES (O_OB + TILE)

#define NLOG 80
/* logical row mnemonics (im2col 0-7, transpose 8-9, attention word w -> 10+w) */
static const char *mnem[NLOG] = {
    /* 0 */ "im2col vle8 idxH1 x2",  "im2col vle8 idxV x2",  "im2col vle8 idxH2 x2",
    /* 3 */ "im2col vle8 src x2",    "im2col vrgather H1 x2","im2col vrgather V x2",
    /* 6 */ "im2col vrgather H2 x2", "im2col vse8 T x2",
    /* 8 */ "transpose vle8(V) x2",  "transpose vse8(H) x2",
    /* 10 attn PRE */ "attn vle8 Q x2", "attn vid x2", "attn vmseq x2", "attn vmv0 x2",
    /* 14 */ "attn vle8 Kt x4", "attn vmv sczero x4",
    /* 16 QK body x512 */ "QK vrgather.vx K[j] x512", "QK vwmulu.vv x512",
    /* 18 */ "QK vwredsumu.vs x512", "QK vnsrl>>S x512", "QK vminu255 x512",
    /* 21 */ "QK vnsrl->u8 x512", "QK vslideup x512", "QK vmerge x512", "QK vmv x512",
    /* 25 */ "attn vmv COPYSA x2",
    /* 26 softmax x2 */ "sm vredmaxu Sa x2", "sm vredmaxu both x2", "sm vrgather m x2",
    /* 29 */ "sm vle8 expLUT x2", "sm vssubu m-Sa x2", "sm vminu127 x2", "sm vrgather ea x2",
    /* 33 */ "sm vssubu m-Sb x2", "sm vminu127 x2", "sm vrgather eb x2",
    /* 36 */ "sm vwredsumu Z(a) x2", "sm vwredsumu Z(b) x2", "sm vwmulu Z32 x2",
    /* 39 newton x2 */ "nw vnsrl idx8 x2", "nw vle8 seedLUT x2", "nw vrgather seed8 x2",
    /* 42 */ "nw vwmulu seed16 x2", "nw vwmulu R0_32 x2",
    /* 44 */ "nw vmul i1 x2", "nw vrsub i1 x2", "nw vmul i1 x2", "nw vsrl i1 x2",
    /* 48 */ "nw vmul i2 x2", "nw vrsub i2 x2", "nw vmul i2 x2", "nw vsrl i2 x2",
    /* 52 */ "nw vmul i3 x2", "nw vrsub i3 x2", "nw vmul i3 x2", "nw vsrl i3 x2",
    /* 56 norm x2 */ "norm vnsrl R16 x2", "norm vrgather Rb16 x2", "norm vwmulu e16a x2",
    /* 59 */ "norm vmul P16a x2", "norm vnsrl pqa x2", "norm vwmulu e16b x2",
    /* 62 */ "norm vmul P16b x2", "norm vnsrl pqb x2",
    /* 64 PV_PRE x2 */ "pv vle8 Va x2", "pv vle8 Vb x2", "pv vmv Oacc0 x2",
    /* 67 PV body x128 */ "PV vrgather Va col-d x128", "PV vwmulu pqa*Va x128",
    /* 69 */ "PV vwredsumu accA x128", "PV vrgather Vb col-d x128", "PV vwmulu pqb*Vb x128",
    /* 72 */ "PV vwredsumu chain x128", "PV vnsrl>>O x128", "PV vminu255 x128",
    /* 75 */ "PV vnsrl->u8 x128", "PV vslideup x128", "PV vmerge x128", "PV vmv x128",
    /* 79 */ "attn vse8 O x2",
};

struct sample { uint64_t t1_cycles; double wall_us; };
static struct sample samp[MAX_ITERS][NLOG];

static void die(const char *m) { perror(m); exit(1); }
static double us_between(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) * 1e6 + (double)(b->tv_nsec - a->tv_nsec) / 1e3;
}

/* time one T1 issue and accumulate into logical row lidx (matmul_perf style). */
static void ti(uint32_t iter, int lidx, uint32_t w, uint32_t vtype, uint32_t vl,
               uint8_t vmode, uint32_t rs1) {
    struct t1_op op = { .instruction = w, .rs1 = rs1, .vtype = vtype, .vl = vl,
                        .vertical_mode = vmode };
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    __asm__ __volatile__("" ::: "memory");
    volatile uint32_t s = t1_perf_start(1);
    int rc = t1_issue(&op);
    if (rc >= 0) rc = t1_wait_ready();
    volatile uint32_t c = t1_perf_stop();
    __asm__ __volatile__("" ::: "memory");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (rc < 0) { fprintf(stderr, "issue failed iter %u lidx %d: %s\n", iter, lidx, strerror(errno)); exit(1); }
    samp[iter][lidx].t1_cycles += c;
    samp[iter][lidx].wall_us  += us_between(&t0, &t1);
    (void)s;
}

/* one im2col block (logical 0-7); src/out PAs injected. */
static void im2col_block(uint32_t iter, uint32_t pa, uint32_t src, uint32_t out) {
    const uint32_t *K = attention_patch_im2col;
    ti(iter, 0, K[0], AP_E8_M1, 128, 0, pa + O_IDXH1);
    ti(iter, 1, K[1], AP_E8_M1, 128, 0, pa + O_IDXV);
    ti(iter, 2, K[2], AP_E8_M1, 128, 0, pa + O_IDXH2);
    ti(iter, 3, K[3], AP_E8_M1, 128, 0, src);
    ti(iter, 4, K[4], AP_E8_M1, 128, 0, 0);
    ti(iter, 5, K[5], AP_E8_M1, 128, 1, 0);   /* vertical vrgather.vv */
    ti(iter, 6, K[6], AP_E8_M1, 128, 0, 0);
    ti(iter, 7, K[7], AP_E8_M1, 128, 0, out);
}
static void transpose_t(uint32_t iter, uint32_t src, uint32_t dst) {
    ti(iter, 8, attention_self_transpose[0], AP_E8_M1, 128, 1, src);
    ti(iter, 9, attention_self_transpose[1], AP_E8_M1, 128, 0, dst);
}

/* one query-block of attention_self, per-instruction (logical = 10 + word). */
static void attn_qblock(uint32_t iter, uint32_t pa, uint32_t q, uint32_t kta,
                        uint32_t ktb, uint32_t va, uint32_t vb, uint32_t out) {
    const uint32_t *K = attention_self;
    #define A(w) (K[w])
    ti(iter, 10, A(0), AP_E8_M1, 128, 0, q);
    ti(iter, 11, A(1), AP_E8_M1, 128, 0, 0);
    ti(iter, 12, A(2), AP_E8_M1, 128, 0, 0);
    ti(iter, 13, A(3), AP_E8_M1, 128, 0, 0);
    for (int blk = 0; blk < 2; blk++) {
        ti(iter, 14, A(4), AP_E8_M1, 128, 1, blk ? ktb : kta);
        ti(iter, 15, A(5), AP_E8_M1, 128, 0, 0);
        for (int j = 127; j >= 0; j--) {
            ti(iter, 16, A(6),  AP_E8_M1,  128, 1, (uint32_t)j);
            ti(iter, 17, A(7),  AP_E8_M1,  64,  0, 0);
            ti(iter, 18, A(8),  AP_E16_M2, 64,  0, 0);
            ti(iter, 19, A(9),  AP_E16_M1, 1,   0, 0);
            ti(iter, 20, A(10), AP_E16_M1, 1,   0, AP_C_255);
            ti(iter, 21, A(11), AP_E8_M1,  1,   0, 0);
            ti(iter, 22, A(12), AP_E8_M1,  128, 0, 0);
            ti(iter, 23, A(13), AP_E8_M1,  128, 0, 0);
            ti(iter, 24, A(14), AP_E8_M1,  128, 0, 0);
        }
        if (blk == 0) ti(iter, 25, A(15), AP_E8_M1, 128, 0, 0);   /* COPYSA */
    }
    /* softmax 16-28 -> logical 26-38 */
    ti(iter, 26, A(16), AP_E8_M1,  128, 0, 0);
    ti(iter, 27, A(17), AP_E8_M1,  128, 0, 0);
    ti(iter, 28, A(18), AP_E8_M1,  128, 0, 0);
    ti(iter, 29, A(19), AP_E8_M1,  128, 0, pa + O_EXP);
    ti(iter, 30, A(20), AP_E8_M1,  128, 0, 0);
    ti(iter, 31, A(21), AP_E8_M1,  128, 0, AP_C_127);
    ti(iter, 32, A(22), AP_E8_M1,  128, 0, 0);
    ti(iter, 33, A(23), AP_E8_M1,  128, 0, 0);
    ti(iter, 34, A(24), AP_E8_M1,  128, 0, AP_C_127);
    ti(iter, 35, A(25), AP_E8_M1,  128, 0, 0);
    ti(iter, 36, A(26), AP_E8_M1,  128, 0, 0);
    ti(iter, 37, A(27), AP_E8_M1,  128, 0, 0);
    ti(iter, 38, A(28), AP_E16_M1, 1,   0, AP_C_1);
    /* newton 29-45 -> 39-55 */
    ti(iter, 39, A(29), AP_E8_M1,  1,   0, 0);
    ti(iter, 40, A(30), AP_E8_M1,  128, 0, pa + O_SEED);
    ti(iter, 41, A(31), AP_E8_M1,  128, 0, 0);
    ti(iter, 42, A(32), AP_E8_M1,  1,   0, AP_C_2);
    ti(iter, 43, A(33), AP_E16_M1, 1,   0, AP_C_1);
    for (int it = 0; it < 3; it++) {
        uint32_t w = 34 + (uint32_t)it * 4; int l = 44 + it * 4;
        ti(iter, l + 0, A(w + 0), AP_E32_M1, 1, 0, 0);
        ti(iter, l + 1, A(w + 1), AP_E32_M1, 1, 0, AP_C_2P17);
        ti(iter, l + 2, A(w + 2), AP_E32_M1, 1, 0, 0);
        ti(iter, l + 3, A(w + 3), AP_E32_M1, 1, 0, 0);
    }
    /* norm 46-53 -> 56-63 */
    ti(iter, 56, A(46), AP_E16_M1, 1,   0, 0);
    ti(iter, 57, A(47), AP_E16_M2, 128, 0, 0);
    ti(iter, 58, A(48), AP_E8_M1,  128, 0, AP_C_1);
    ti(iter, 59, A(49), AP_E16_M2, 128, 0, 0);
    ti(iter, 60, A(50), AP_E8_M1,  128, 0, 0);
    ti(iter, 61, A(51), AP_E8_M1,  128, 0, AP_C_1);
    ti(iter, 62, A(52), AP_E16_M2, 128, 0, 0);
    ti(iter, 63, A(53), AP_E8_M1,  128, 0, 0);
    /* PV_PRE 54-56 -> 64-66 */
    ti(iter, 64, A(54), AP_E8_M1, 128, 1, va);
    ti(iter, 65, A(55), AP_E8_M1, 128, 1, vb);
    ti(iter, 66, A(56), AP_E8_M1, 128, 0, 0);
    /* PV body 57-68 -> 67-78 x64 */
    for (int d = 63; d >= 0; d--) {
        ti(iter, 67, A(57), AP_E8_M1,  128, 1, (uint32_t)d);
        ti(iter, 68, A(58), AP_E8_M1,  128, 0, 0);
        ti(iter, 69, A(59), AP_E16_M2, 128, 0, 0);
        ti(iter, 70, A(60), AP_E8_M1,  128, 1, (uint32_t)d);
        ti(iter, 71, A(61), AP_E8_M1,  128, 0, 0);
        ti(iter, 72, A(62), AP_E16_M2, 128, 0, 0);
        ti(iter, 73, A(63), AP_E16_M1, 1,   0, 0);
        ti(iter, 74, A(64), AP_E16_M1, 1,   0, AP_C_255);
        ti(iter, 75, A(65), AP_E8_M1,  1,   0, 0);
        ti(iter, 76, A(66), AP_E8_M1,  64,  0, 0);
        ti(iter, 77, A(67), AP_E8_M1,  64,  0, 0);
        ti(iter, 78, A(68), AP_E8_M1,  64,  0, 0);
    }
    ti(iter, 79, A(69), AP_E8_M1, 64, 0, out);
    #undef A
}

static void usage(const char *a0) { fprintf(stderr, "Usage: %s [--iters N] [--out path]\n", a0); }

int main(int argc, char **argv) {
    unsigned iters = 20;
    const char *out_path = NULL;
    static const struct option opts[] = {
        {"iters", required_argument, NULL, 'i'},
        {"out",   required_argument, NULL, 'o'},
        {"help",  no_argument,       NULL, 'h'}, {0,0,0,0}};
    int c;
    while ((c = getopt_long(argc, argv, "i:o:h", opts, NULL)) != -1) {
        switch (c) {
            case 'i': iters = (unsigned)strtoul(optarg, NULL, 0); break;
            case 'o': out_path = optarg; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 2;
        }
    }
    if (iters == 0 || iters > MAX_ITERS) { fprintf(stderr, "iters 1..%u\n", MAX_ITERS); return 2; }

    if (t1_init() < 0) die("t1_init");
    struct t1_buf stg = {0};
    if (t1_buf_alloc(&stg, STAGE_BYTES) < 0) die("t1_buf_alloc");
    uint8_t *b = (uint8_t *)stg.va;
    uint32_t pa = stg.pa;
    memset(b, 0, STAGE_BYTES);
    ap_build_frame_textured(b + O_FRAME);
    ap_build_im2col_idxH1(b + O_IDXH1);
    ap_build_im2col_idxV (b + O_IDXV);
    ap_build_im2col_idxH2(b + O_IDXH2);
    ap_build_self_exp_lut(b + O_EXP);
    ap_build_seed_lut    (b + O_SEED);
    if (t1_buf_sync_for_device(&stg) < 0) die("sync");
    fprintf(stderr, "attention_self_perf: iters=%u, %u logical rows, staging=%uKB\n",
            iters, NLOG, STAGE_BYTES / 1024u);

    for (uint32_t iter = 0; iter < iters; iter++) {
        im2col_block(iter, pa, pa + O_FRAME, pa + O_QA);
        im2col_block(iter, pa, pa + O_FRAME + 64u * 128u, pa + O_QB);
        transpose_t(iter, pa + O_QA, pa + O_KTA);
        transpose_t(iter, pa + O_QB, pa + O_KTB);
        attn_qblock(iter, pa, pa + O_QA, pa + O_KTA, pa + O_KTB, pa + O_QA, pa + O_QB, pa + O_OA);
        attn_qblock(iter, pa, pa + O_QB, pa + O_KTA, pa + O_KTB, pa + O_QA, pa + O_QB, pa + O_OB);
    }

    FILE *out = out_path ? fopen(out_path, "w") : stdout;
    if (!out) die("fopen out");
    fprintf(out, "iter,instr_idx,mnemonic,is_csr_flip,t1_cycles,wall_us\n");
    for (unsigned i = 0; i < iters; i++)
        for (int l = 0; l < NLOG; l++)
            fprintf(out, "%u,%d,%s,0,%llu,%.3f\n", i, l, mnem[l],
                    (unsigned long long)samp[i][l].t1_cycles, samp[i][l].wall_us);
    if (out != stdout) fclose(out);

    /* quick stderr summary (top rows by cycles) */
    uint64_t tot[NLOG] = {0}; uint64_t grand = 0;
    for (unsigned i = 0; i < iters; i++) for (int l = 0; l < NLOG; l++) { tot[l] += samp[i][l].t1_cycles; grand += samp[i][l].t1_cycles; }
    fprintf(stderr, "\n=== attention_self per-instruction (avg/frame), top 10 by cycles ===\n");
    for (int rank = 0; rank < 10; rank++) {
        int best = -1; for (int l = 0; l < NLOG; l++) if (tot[l] && (best < 0 || tot[l] > tot[best])) best = l;
        if (best < 0) break;
        fprintf(stderr, "  %-26s %12.0f cyc  %5.1f%%\n", mnem[best],
                (double)tot[best] / iters, grand ? 100.0 * (double)tot[best] / (double)grand : 0.0);
        tot[best] = 0;
    }
    t1_buf_free(&stg);
    t1_close();
    return 0;
}
