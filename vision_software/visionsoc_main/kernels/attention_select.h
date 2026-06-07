/*
 * attention_select.h -- activate the attention kernel in the camera pipeline.
 *
 * Drives kernels/attention.S via kernels/attention_issue.h. main.c calls
 * issue_active_kernel(src_pa, dst_pa) per frame with src = the camera Y frame
 * (the QUERY) and dst = the attention output O (displayed).
 *
 * DATA PLACEMENT -- all DDR. The attention kernel was hardware-verified all-DDR
 * (libt1/test/attention_probe.c PASS), but HANGS the fabric when its operands
 * are read from URAM (verified: the all-DDR main probe passes, the all-URAM
 * select probe wedges the board). So:
 *   - ACTIVE_KERNEL_DDR_IO tells main.c to run the kernel directly on the DDR
 *     `in` -> `out` udmabufs (skip the URAM_HALF_A/B round trip), and
 *   - the three constant operands (Kt=K^T, V, exp+seed LUTs) are staged ONCE
 *     into a single 64 KB DDR udmabuf (udmabuf pool: in, out, this = 3, the max).
 * Attention does only a handful of one-shot LSU loads, so DDR latency is
 * amortised and the all-DDR placement costs nothing measurable.
 *
 * K and V are fixed pseudo-random (no pretrained weights): cross-attention of
 * the live frame against a learned-style dictionary. The output is a
 * content-addressed transform that changes with the scene. See
 * fyp_doc/attention_kernel_status.md.
 */
#pragma once

#include "attention.h"          /* generated words: attention[], attention_count */
#include "attention_weights.h"  /* builders + constants */
#include "attention_issue.h"    /* attention_issue(), struct attn_pa */
#include "libt1.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#define ACTIVE_KERNEL            attention
#define ACTIVE_KERNEL_NEUTRAL_UV 1   /* O is luma-only; show grayscale */
#define ACTIVE_KERNEL_FLOW_COLOR 0
#define ACTIVE_KERNEL_DDR_IO     1   /* main.c: run on DDR in->out (no URAM) */

#define ATTN_BUF_BYTES (128u * 128u)         /* 16 KB per operand */
#define ATTN_OPS_BYTES (4u * ATTN_BUF_BYTES) /* Kt | V | expLUT | seedLUT = 64 KB */

/* Stage Kt/V/expLUT/seedLUT once into one DDR udmabuf; fill pa with their PAs. */
static int attn_stage_operands(struct attn_pa *pa_out)
{
    static int staged = 0;
    static struct attn_pa pa;
    static struct t1_buf ops = {0};

    if (!staged) {
        if (t1_buf_alloc(&ops, ATTN_OPS_BYTES) < 0) {
            return -1;
        }
        uint8_t *b = (uint8_t *)ops.va;
        attn_build_Kt     (b + 0u * ATTN_BUF_BYTES);
        attn_build_V      (b + 1u * ATTN_BUF_BYTES);
        attn_build_exp_lut(b + 2u * ATTN_BUF_BYTES);
        attn_build_seed_lut(b + 3u * ATTN_BUF_BYTES);
        if (t1_buf_sync_for_device(&ops) < 0) {
            return -1;
        }
        pa.kt       = ops.pa + 0u * ATTN_BUF_BYTES;
        pa.v        = ops.pa + 1u * ATTN_BUF_BYTES;
        pa.exp_lut  = ops.pa + 2u * ATTN_BUF_BYTES;
        pa.seed_lut = ops.pa + 3u * ATTN_BUF_BYTES;
        staged = 1;
        fprintf(stderr,
                "attention: staged Kt/V/expLUT/seedLUT in DDR "
                "(kt=0x%08x v=0x%08x exp=0x%08x seed=0x%08x)\n",
                pa.kt, pa.v, pa.exp_lut, pa.seed_lut);
    }
    *pa_out = pa;
    return 0;
}

static inline int issue_active_kernel(uint32_t src_pa, uint32_t dst_pa)
{
    struct attn_pa pa;
    if (attn_stage_operands(&pa) < 0) {
        return -1;
    }
    pa.q   = src_pa;   /* Q = camera frame (DDR `in`)  */
    pa.out = dst_pa;   /* O = attention output (DDR `out`) */
    return attention_issue(attention, &pa, 0);   /* dbg=0: no checkpoints */
}
