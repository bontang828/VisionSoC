/*
 * attention_uram_probe.c -- verify the ALL-URAM attention path (the natural
 * "URAM version" for a DDR-vs-URAM comparison).
 *
 * Mirrors main.c's URAM round-trip exactly, the way matmul works:
 *   - Q DMA'd DDR -> URAM_HALF_A
 *   - operands Kt/V/expLUT/seedLUT built in DDR, DMA'd into URAM
 *   - attention_issue(URAM_HALF_A, URAM_HALF_B)  (all operands+I/O in URAM)
 *   - O DMA'd URAM_HALF_B -> DDR, checked vs the C reference
 * URAM is populated by DMA (NOT t1_scratchpad_alloc CPU-mmap, which wedges the
 * fabric for this kernel). Flushed [uram] checkpoints localise any hang.
 *
 *   build: cd ~/vision_software/libt1 && make test/attention_uram_probe
 *   run:   sudo ./test/attention_uram_probe
 */
#include "libt1.h"
#include "libt1_regs.h"
#include "kernels/attention_weights.h"
#include "kernels/attention.h"
#include "kernels/attention_issue.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define N 128
#define GRID (N * N)

#define URAM_HALF_A (T1_SCRATCHPAD_PA + 0x00000u)   /* Q  */
#define URAM_HALF_B (T1_SCRATCHPAD_PA + 0x04000u)   /* O  */
#define URAM_KT     (T1_SCRATCHPAD_PA + 0x08000u)
#define URAM_V      (T1_SCRATCHPAD_PA + 0x0C000u)
#define URAM_EXP    (T1_SCRATCHPAD_PA + 0x10000u)
#define URAM_SEED   (T1_SCRATCHPAD_PA + 0x14000u)

static void p(const char *m) { fprintf(stderr, "[uram] %s\n", m); fflush(stderr); }

/* DDR -> URAM (async-pair + wait; DMA fabric is a loopback, like main.c). */
static int dma_ddr_to_uram(struct t1_buf *ddr, uint32_t uram_pa)
{
    if (t1_buf_sync_for_device(ddr) < 0) return -1;
    if (t1_dma_s2mm_async(0, uram_pa, GRID) < 0) return -1;
    if (t1_dma_mm2s_async(ddr->pa, 0, GRID) < 0) return -1;
    return t1_dma_wait();
}
/* URAM -> DDR. */
static int dma_uram_to_ddr(uint32_t uram_pa, struct t1_buf *ddr)
{
    if (t1_dma_s2mm_async(0, ddr->pa, GRID) < 0) return -1;
    if (t1_dma_mm2s_async(uram_pa, 0, GRID) < 0) return -1;
    if (t1_dma_wait() < 0) return -1;
    return t1_buf_sync_for_cpu(ddr);
}

int main(void)
{
    if (t1_init() < 0) { perror("t1_init"); return 1; }
    p("t1_init ok");

    struct t1_buf q = {0}, o = {0}, tmp = {0};
    if (t1_buf_alloc(&q, GRID) < 0 || t1_buf_alloc(&o, GRID) < 0 ||
        t1_buf_alloc(&tmp, GRID) < 0) { perror("t1_buf_alloc"); return 1; }
    p("DDR staging bufs ok");

    attn_build_Q((uint8_t *)q.va);
    if (dma_ddr_to_uram(&q, URAM_HALF_A) < 0) { perror("dma Q"); return 1; }
    p("Q -> URAM_HALF_A");

    attn_build_Kt((uint8_t *)tmp.va);       if (dma_ddr_to_uram(&tmp, URAM_KT)   < 0) { perror("dma Kt");   return 1; } p("Kt -> URAM");
    attn_build_V((uint8_t *)tmp.va);        if (dma_ddr_to_uram(&tmp, URAM_V)    < 0) { perror("dma V");    return 1; } p("V -> URAM");
    attn_build_exp_lut((uint8_t *)tmp.va);  if (dma_ddr_to_uram(&tmp, URAM_EXP)  < 0) { perror("dma exp");  return 1; } p("exp -> URAM");
    attn_build_seed_lut((uint8_t *)tmp.va); if (dma_ddr_to_uram(&tmp, URAM_SEED) < 0) { perror("dma seed"); return 1; } p("seed -> URAM");

    struct attn_pa pa = {
        .q = URAM_HALF_A, .out = URAM_HALF_B,
        .kt = URAM_KT, .v = URAM_V, .exp_lut = URAM_EXP, .seed_lut = URAM_SEED,
    };
    p("calling attention_issue (ALL URAM)...");
    if (attention_issue(attention, &pa, 0) < 0) { perror("attention_issue"); return 1; }  /* warm-up */
    (void)t1_perf_start(1);
    if (attention_issue(attention, &pa, 0) < 0) { perror("attention_issue(2)"); return 1; }
    uint32_t cyc = t1_perf_stop();
    fprintf(stderr, "[uram] attention_issue returned (%u kernel cycles, URAM operands)\n", cyc);

    if (dma_uram_to_ddr(URAM_HALF_B, &o) < 0) { perror("dma O out"); return 1; }
    p("O <- URAM_HALF_B");

    static uint8_t Kt[GRID], V[GRID], rO[GRID];
    attn_build_Kt(Kt); attn_build_V(V);
    uint8_t et[128]; attn_exp_table(et);
    uint8_t st[128]; attn_seed_table8(st);
    attn_reference((const uint8_t *)q.va, Kt, V, et, st, NULL,NULL,NULL,NULL,NULL, rO);

    const uint8_t *hw = (const uint8_t *)o.va;
    int errs = 0, fi = -1, fg = 0, fe = 0;
    for (int i = 0; i < GRID; i++) {
        int d = hw[i] - rO[i]; if (d < 0) d = -d;
        if (d > 2) { if (!errs) { fi = i; fg = hw[i]; fe = rO[i]; } errs++; }
    }
    if (errs == 0) printf("[PASS] all-URAM O matches reference (%d cells, tol 2)\n", GRID);
    else printf("[FAIL] all-URAM O: %d/%d; first idx %d got %d exp %d\n", errs, GRID, fi, fg, fe);
    printf("\n==== attention all-URAM probe: %s ====\n", errs ? "FAIL" : "PASS");
    t1_close();
    return errs ? 1 : 0;
}
