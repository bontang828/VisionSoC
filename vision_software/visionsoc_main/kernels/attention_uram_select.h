/*
 * attention_uram_select.h -- URAM-resident variant of the attention kernel,
 * for DDR-vs-URAM comparison against attention_select.h.
 *
 * Same kernel words (attention_uram[] == attention[]); the difference is data
 * placement:
 *   - Q/O use main.c's normal URAM round-trip (frame DMA'd DDR->URAM_HALF_A,
 *     O DMA'd URAM_HALF_B->DDR) -- i.e. NO ACTIVE_KERNEL_DDR_IO here.
 *   - the four operands (Kt/V/exp/seed LUTs) are staged ONCE into the URAM
 *     scratchpad **by DMA** (matmul's proven pattern), NOT via
 *     t1_scratchpad_alloc CPU-mmap -- the CPU-mmap populate path wedges the
 *     fabric for this kernel (verified: libt1/test/attention_uram_probe.c
 *     all-URAM PASS with DMA staging; the scratchpad-mmap select-probe hung).
 *
 * Switch: ./sync_kernel.sh attention_uram   (URAM)
 *         ./sync_kernel.sh attention        (DDR)
 *
 * See fyp_doc/attention_kernel_status.md.
 */
#pragma once

#include "attention_uram.h"     /* generated words: attention_uram[] */
#include "attention_weights.h"  /* builders + constants */
#include "attention_issue.h"    /* attention_issue(), struct attn_pa */
#include "libt1.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#define ACTIVE_KERNEL            attention_uram
#define ACTIVE_KERNEL_NEUTRAL_UV 1
#define ACTIVE_KERNEL_FLOW_COLOR 0
/* NOTE: deliberately NOT ACTIVE_KERNEL_DDR_IO -- use the URAM round-trip. */

#define ATTN_BUF_BYTES (128u * 128u)     /* 16 KB per operand */
#define ATTN_URAM_KT   (T1_SCRATCHPAD_PA + 0x08000u)   /* above HALF_A/+0, HALF_B/+0x4000 */
#define ATTN_URAM_V    (T1_SCRATCHPAD_PA + 0x0C000u)
#define ATTN_URAM_EXP  (T1_SCRATCHPAD_PA + 0x10000u)
#define ATTN_URAM_SEED (T1_SCRATCHPAD_PA + 0x14000u)

/* DDR -> URAM via the async-pair + wait loopback (same as main.c's weights). */
static int attn_dma_ddr_to_uram(struct t1_buf *ddr, uint32_t uram_pa)
{
    if (t1_buf_sync_for_device(ddr) < 0) return -1;
    if (t1_dma_s2mm_async(0, uram_pa, ATTN_BUF_BYTES) < 0) return -1;
    if (t1_dma_mm2s_async(ddr->pa, 0, ATTN_BUF_BYTES) < 0) return -1;
    return t1_dma_wait();
}

static int attn_stage_operands_uram(struct attn_pa *pa_out)
{
    static int staged = 0;
    static struct attn_pa pa;

    if (!staged) {
        struct t1_buf tmp = {0};   /* one DDR staging buffer, reused + freed */
        if (t1_buf_alloc(&tmp, ATTN_BUF_BYTES) < 0) return -1;
        attn_build_Kt((uint8_t *)tmp.va);       if (attn_dma_ddr_to_uram(&tmp, ATTN_URAM_KT)   < 0) return -1;
        attn_build_V((uint8_t *)tmp.va);        if (attn_dma_ddr_to_uram(&tmp, ATTN_URAM_V)    < 0) return -1;
        attn_build_exp_lut((uint8_t *)tmp.va);  if (attn_dma_ddr_to_uram(&tmp, ATTN_URAM_EXP)  < 0) return -1;
        attn_build_seed_lut((uint8_t *)tmp.va); if (attn_dma_ddr_to_uram(&tmp, ATTN_URAM_SEED) < 0) return -1;
        t1_buf_free(&tmp);
        pa.kt = ATTN_URAM_KT; pa.v = ATTN_URAM_V;
        pa.exp_lut = ATTN_URAM_EXP; pa.seed_lut = ATTN_URAM_SEED;
        staged = 1;
        fprintf(stderr,
                "attention(URAM): DMA-staged Kt/V/expLUT/seedLUT -> URAM "
                "(kt=0x%08x v=0x%08x exp=0x%08x seed=0x%08x)\n",
                pa.kt, pa.v, pa.exp_lut, pa.seed_lut);
    }
    *pa_out = pa;
    return 0;
}

static inline int issue_active_kernel(uint32_t src_pa, uint32_t dst_pa)
{
    struct attn_pa pa;
    if (attn_stage_operands_uram(&pa) < 0) {
        return -1;
    }
    pa.q   = src_pa;   /* Q = URAM_HALF_A (camera frame DMA'd by main.c) */
    pa.out = dst_pa;   /* O = URAM_HALF_B (DMA'd out by main.c)          */
    return attention_issue(attention_uram, &pa, 0);
}
