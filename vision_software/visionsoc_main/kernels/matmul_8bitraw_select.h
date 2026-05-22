/*
 * matmul_8bitraw_select.h -- activate the rawest (SEW=8, mod-256) matmul.
 *
 * Drives kernels/matmul_8bitraw.S. Unlike the generic dispatcher or even
 * optical_flow_select.h, this kernel is a LOOP: the body block is re-issued
 * 128 times, once per output column k = 127..0, with the vrgather index k
 * injected as op.rs1. The .S is laid out as preamble | body | postamble
 * (see matmul_8bitraw.S); the index ranges below MUST match it.
 *
 * Buffer routing:
 *   a0 -> src_pa        : A = current camera frame (staged into URAM by main.c)
 *   a1 -> MMK_WEIGHT_PA : B = 128x128 weight matrix (an internal URAM region;
 *                             main.c must DMA the demo weights there ONCE at
 *                             startup -- mirrors optical_flow's OFK_PREV_PA)
 *   a2 -> dst_pa        : C = result, displayed
 *   a4                  : vrgather column index; injected per iteration
 *
 * Everything is SEW=8 / vl=128: products and the 128-wide sum wrap mod 256,
 * so this is only sensible for a very sparse/small B. For a real weighted
 * matmul use matmul_select.h (widening + scale).
 */
#pragma once

#include "matmul_8bitraw.h"
#include "matmul_weights.h"
#include "libt1.h"
#include "libt1_regs.h"

#include <errno.h>
#include <stdint.h>

#define ACTIVE_KERNEL            matmul_8bitraw
#define ACTIVE_KERNEL_NEUTRAL_UV 1   /* result is luma-only */
#define ACTIVE_KERNEL_FLOW_COLOR 0

/* Internal URAM region holding the weight matrix B, at +32 KB (disjoint
 * from URAM_HALF_A / URAM_HALF_B used by main.c for A and C). */
#define MMK_URAM_BASE_PA   0xA0080000u
#define MMK_WEIGHT_PA      (MMK_URAM_BASE_PA + 0x08000u)

/* main.c hooks: stage matrix B into URAM once at startup. The raw SEW=8
 * kernel wraps mod-256, so it pairs with IDENTITY (exact passthrough --
 * outY should equal camY, a clean first-light sanity). Switch to MMK_B_SHIFT
 * for a visible horizontal slide; do NOT use MMK_B_BLUR here (it overflows). */
#define ACTIVE_KERNEL_NEEDS_WEIGHTS  1
#define ACTIVE_KERNEL_WEIGHT_PA      MMK_WEIGHT_PA
#define ACTIVE_KERNEL_WEIGHT_PATTERN MMK_B_IDENTITY

/* .S region layout -- keep in lockstep with matmul_8bitraw.S. */
#define MMK_PRE_BASE    0u
#define MMK_PRE_COUNT   8u
#define MMK_BODY_BASE   8u
#define MMK_BODY_COUNT  6u
#define MMK_POST_BASE   14u
#define MMK_ITERS       128u   /* output columns k = 127..0 */
#define MMK_GATHER_POS  0u     /* body position whose rs1 = k */

/* a-register numbers. */
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

/* Per-body-position issue metadata. The raw kernel is uniform (e8/m1,
 * vl=128); only the gather runs in vertical mode with an injected index. */
struct mmk_body_step {
    uint32_t vtype;
    uint32_t vl;
    uint8_t  vmode;
    uint8_t  inject_k;   /* 1 = set op.rs1 = current column index k */
};

static const struct mmk_body_step mmk_body[MMK_BODY_COUNT] = {
    { T1_VTYPE_E8_M1_TA_MA, 128u, 1u, 1u },  /* vrgather.vx (V), rs1=k */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u, 0u },  /* vmul.vv                */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u, 0u },  /* vredsum.vs             */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u, 0u },  /* vslideup.vi            */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u, 0u },  /* vmerge.vvm             */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u, 0u },  /* vmv.v.v                */
};

static inline int issue_active_kernel(uint32_t src_pa, uint32_t dst_pa)
{
    struct t1_op op = { .vtype = T1_VTYPE_E8_M1_TA_MA, .vl = 128u };
    int vmode = 0;

    /* ---- preamble: walk words [PRE_BASE, PRE_BASE+PRE_COUNT) ---- */
    for (uint32_t i = MMK_PRE_BASE; i < MMK_PRE_BASE + MMK_PRE_COUNT; i++) {
        uint32_t w      = matmul_8bitraw[i];
        uint32_t opcode = w & 0x7Fu;
        uint32_t funct3 = (w >> 12) & 0x7u;
        uint32_t rs1    = (w >> 15) & 0x1Fu;
        uint32_t csr    = (w >> 20) & 0xFFFu;

        if (opcode == MMK_OPCODE_SYSTEM && csr == MMK_CSR_VERTMODE) {
            if (funct3 == MMK_FUNCT3_CSRRWI) { vmode = rs1 ? 1 : 0; continue; }
            if (funct3 == MMK_FUNCT3_CSRRW && rs1 == MMK_REG_X0) { vmode = 0; continue; }
            errno = ENOTSUP;
            return -1;
        }

        op.vtype = T1_VTYPE_E8_M1_TA_MA;
        op.vl = 128u;
        if (opcode == MMK_OPCODE_LOAD_FP || opcode == MMK_OPCODE_STORE_FP) {
            switch (rs1) {
                case MMK_REG_A0: op.rs1 = src_pa;       break;  /* A    */
                case MMK_REG_A1: op.rs1 = MMK_WEIGHT_PA; break; /* B    */
                case MMK_REG_A2: op.rs1 = dst_pa;       break;  /* C    */
                default:         op.rs1 = 0u;           break;
            }
        } else {
            op.rs1 = 0u;
        }
        op.instruction = w;
        op.vertical_mode = (uint8_t)vmode;
        if (t1_issue(&op) < 0) return -1;
    }

    /* ---- body: 128 columns, k = 127..0 ---- */
    for (int32_t k = (int32_t)MMK_ITERS - 1; k >= 0; k--) {
        for (uint32_t p = 0; p < MMK_BODY_COUNT; p++) {
            const struct mmk_body_step *s = &mmk_body[p];
            op.instruction   = matmul_8bitraw[MMK_BODY_BASE + p];
            op.vtype         = s->vtype;
            op.vl            = s->vl;
            op.vertical_mode = s->vmode;
            op.rs1           = s->inject_k ? (uint32_t)k : 0u;
            if (t1_issue(&op) < 0) return -1;
        }
    }

    /* ---- postamble: store C row ---- */
    {
        uint32_t w   = matmul_8bitraw[MMK_POST_BASE];
        uint32_t rs1 = (w >> 15) & 0x1Fu;
        op.vtype = T1_VTYPE_E8_M1_TA_MA;
        op.vl = 128u;
        op.rs1 = (rs1 == MMK_REG_A2) ? dst_pa : 0u;
        op.instruction = w;
        op.vertical_mode = 0u;
        if (t1_issue(&op) < 0) return -1;
    }
    return 0;
}
