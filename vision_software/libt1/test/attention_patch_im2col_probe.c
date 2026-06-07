/*
 * attention_patch_im2col_probe.c -- verify the on-fabric 8x8 patchify (im2col)
 * 3-pass gather (H -> V -> H) on hardware, standalone (no attention, no camera).
 *
 * For each token-block it stages a synthetic frame + the three index tiles in
 * DDR, issues attention_patch_im2col.S with dbg=1 (checkpoints M1, M2), and
 * compares M1 / M2 / T to a probe-side C replica of the exact 3 passes, plus T
 * against the canonical ap_build_Q_block. fprintf checkpoints localise hangs;
 * t1_perf reports the per-block patchify cost.
 *
 * Only 3 /dev/udmabuf nodes exist (4 MB each), so everything is packed into two
 * udmabuf "arenas" via byte offsets.
 *
 *   build: cd ~/vision_software/libt1 && make test/attention_patch_im2col_probe
 *   run:   sudo ./test/attention_patch_im2col_probe
 */
#include "libt1.h"
#include "kernels/attention_patch_weights.h"
#include "kernels/attention_patch_im2col.h"      /* generated: attention_patch_im2col[] */
#include "kernels/attention_patch_im2col_issue.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TILE  (128 * 128)
#define FRAMEROWS 192                 /* 128 real + 64 pad so block-1 over-read is defined */
#define FRAMEB (FRAMEROWS * 128)

/* in-arena offsets */
#define OFF_FRAME 0u
#define OFF_IDXH1 (OFF_FRAME + FRAMEB)
#define OFF_IDXV  (OFF_IDXH1 + TILE)
#define OFF_IDXH2 (OFF_IDXV  + TILE)
#define IN_BYTES  (OFF_IDXH2 + TILE)
/* out-arena offsets */
#define OFF_OUT   0u
#define OFF_M1    (OFF_OUT + TILE)
#define OFF_M2    (OFF_M1  + TILE)
#define OUT_BYTES (OFF_M2  + TILE)

static void p(const char *m) { fprintf(stderr, "[im2col] %s\n", m); fflush(stderr); }

/* probe-side replica of the 3 passes on the exact 128x128 source the kernel
 * loads for this block (src rows = frame[blk*64 .. blk*64+127]). */
static void expect_passes(const uint8_t *frame /*[FRAMEB]*/, int blk,
                          const uint8_t *idxH1, const uint8_t *idxV, const uint8_t *idxH2,
                          uint8_t *M1, uint8_t *M2, uint8_t *T)
{
    static uint8_t S[TILE];
    int base = blk * 64;
    for (int P = 0; P < 128; P++)
        for (int Q = 0; Q < 128; Q++)
            S[P * 128 + Q] = frame[(base + P) * 128 + Q];
    for (int P = 0; P < 128; P++)
        for (int L = 0; L < 128; L++)
            M1[P * 128 + L] = S[P * 128 + idxH1[P * 128 + L]];
    for (int t = 0; t < 128; t++)
        for (int L = 0; L < 128; L++)
            M2[t * 128 + L] = M1[idxV[t * 128 + L] * 128 + L];
    for (int t = 0; t < 128; t++)
        for (int f = 0; f < 128; f++)
            T[t * 128 + f] = M2[t * 128 + idxH2[t * 128 + f]];
}

static int cmp_tile(const char *name, const uint8_t *got, const uint8_t *exp,
                    int rows, int lanes)
{
    int errs = 0, fr = -1, fc = -1, fg = 0, fe = 0;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < lanes; c++) {
            int g = got[r * 128 + c], e = exp[r * 128 + c];
            if (g != e) { if (!errs) { fr = r; fc = c; fg = g; fe = e; } errs++; }
        }
    if (errs == 0) printf("  [PASS] %s (%dx%d)\n", name, rows, lanes);
    else printf("  [FAIL] %s: %d/%d; first [%d][%d] got %d exp %d\n",
               name, errs, rows * lanes, fr, fc, fg, fe);
    return errs;
}

int main(void)
{
    if (t1_init() < 0) { perror("t1_init"); return 1; }
    p("t1_init ok");

    struct t1_buf in = {0}, out = {0};
    if (t1_buf_alloc(&in, IN_BYTES) < 0 || t1_buf_alloc(&out, OUT_BYTES) < 0) {
        perror("t1_buf_alloc"); return 1;
    }
    p("arenas allocated");

    uint8_t *inb = (uint8_t *)in.va;
    memset(inb, 0, IN_BYTES);
    ap_build_frame(inb + OFF_FRAME);                 /* rows 0..127; 128..191 = 0 */
    ap_build_im2col_idxH1(inb + OFF_IDXH1);
    ap_build_im2col_idxV (inb + OFF_IDXV);
    ap_build_im2col_idxH2(inb + OFF_IDXH2);
    if (t1_buf_sync_for_device(&in) < 0) { perror("sync in"); return 1; }
    p("frame + index tiles staged");

    int total = 0;
    for (int blk = 0; blk < (int)AP_NBLK; blk++) {
        memset(out.va, 0, OUT_BYTES);
        if (t1_buf_sync_for_device(&out) < 0) { perror("sync out"); return 1; }

        struct ap_im2col_pa pa = {
            .idxh1 = in.pa + OFF_IDXH1, .idxv = in.pa + OFF_IDXV, .idxh2 = in.pa + OFF_IDXH2,
            .src   = in.pa + OFF_FRAME + (uint32_t)blk * 64u * 128u,
            .out   = out.pa + OFF_OUT,
            .dbg_m1 = out.pa + OFF_M1, .dbg_m2 = out.pa + OFF_M2,
        };

        fprintf(stderr, "[im2col] block %d: issuing (src=0x%08x)...\n", blk, pa.src);
        if (attention_patch_im2col_issue(attention_patch_im2col, &pa, 0) < 0) {  /* warm-up */
            perror("issue"); return 1;
        }
        (void)t1_perf_start(1);
        if (attention_patch_im2col_issue(attention_patch_im2col, &pa, 1) < 0) {  /* measured + dbg */
            perror("issue(2)"); return 1;
        }
        uint32_t cyc = t1_perf_stop();
        if (t1_buf_sync_for_cpu(&out) < 0) { perror("sync_for_cpu"); return 1; }
        fprintf(stderr, "[im2col] block %d: %u kernel cycles\n", blk, cyc);

        /* expected M1/M2/T from the exact source bytes */
        static uint8_t eM1[TILE], eM2[TILE], eT[TILE], qb[TILE];
        expect_passes(inb + OFF_FRAME, blk,
                      inb + OFF_IDXH1, inb + OFF_IDXV, inb + OFF_IDXH2, eM1, eM2, eT);
        ap_build_Q_block(inb + OFF_FRAME, blk, qb);   /* canonical patchify */

        const uint8_t *gM1 = (uint8_t *)out.va + OFF_M1;
        const uint8_t *gM2 = (uint8_t *)out.va + OFF_M2;
        const uint8_t *gT  = (uint8_t *)out.va + OFF_OUT;

        printf("block %d checks:\n", blk);
        total += cmp_tile("M1 (H pass, rows 0..63)", gM1, eM1, 64, 128);
        total += cmp_tile("M2 (V pass)",             gM2, eM2, 128, 128);
        total += cmp_tile("T  vs 3-pass replica",    gT,  eT,  128, AP_FEAT);
        total += cmp_tile("T  vs ap_build_Q_block",  gT,  qb,  128, AP_FEAT);
    }

    printf("\n==== attention_patch im2col probe: %s ====\n", total ? "FAIL" : "PASS");
    t1_close();
    return total ? 1 : 0;
}
