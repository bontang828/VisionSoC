/*
 * matmul_8bitraw_short_select.h -- activate the short raw matmul loop.
 *
 * Drives kernels/matmul_8bitraw_short.S. This variant keeps A and B live in
 * vector registers across the whole kernel and stores C once at the end, but
 * its per-column body is shorter than matmul_8bitraw: no vid/vmseq mask, no
 * vmerge, and vredsum writes directly into v7 after v7 has been shifted.
 * The .S keeps explicit csrwi markers around vrgather to document the
 * intended H/V-mode sequence; this selector tracks and skips those scalar
 * CSR words just like the preamble markers.
 *
 * Buffer routing:
 *   a0 -> src_pa        : A = current camera frame
 *   a1 -> MMK_WEIGHT_PA : B = 128x128 weight matrix staged once by main.c
 *   a2 -> dst_pa        : C = result, displayed
 *   a4                  : vrgather column index; injected per iteration
 */
#pragma once

#include "matmul_8bitraw_short.h"
#include "matmul_weights.h"
#include "libt1.h"
#include "libt1_regs.h"

#include <errno.h>
#include <stdint.h>

#define ACTIVE_KERNEL            matmul_8bitraw_short
#define ACTIVE_KERNEL_NEUTRAL_UV 1
#define ACTIVE_KERNEL_FLOW_COLOR 0

/* Internal URAM region holding the weight matrix B, at +32 KB. */
#define MMK_URAM_BASE_PA   0xA0080000u
#define MMK_WEIGHT_PA      (MMK_URAM_BASE_PA + 0x08000u)

/* Single-tap shift gives a visible demo while staying exact in raw SEW=8. */
#define ACTIVE_KERNEL_NEEDS_WEIGHTS  1
#define ACTIVE_KERNEL_WEIGHT_PA      MMK_WEIGHT_PA
#define ACTIVE_KERNEL_WEIGHT_PATTERN MMK_B_SHIFT

/* .S region layout -- keep in lockstep with matmul_8bitraw_short.S. */
#define MMK_PRE_BASE    0u
#define MMK_PRE_COUNT   6u
#define MMK_BODY_BASE   6u
#define MMK_BODY_COUNT  7u
#define MMK_POST_BASE   13u
#define MMK_ITERS       128u

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

struct mmk_short_body_step {
    uint32_t vtype;
    uint32_t vl;
    uint8_t  inject_k;
};

static const struct mmk_short_body_step mmk_short_body[MMK_BODY_COUNT] = {
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u },  /* vslideup.vi             */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u },  /* vmv.v.v                 */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u },  /* csrwi 0x7c0, 1 marker   */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 1u },  /* vrgather.vx (V), rs1=k */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u },  /* csrwi 0x7c0, 0 marker   */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u },  /* vmul.vv                 */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u },  /* vredsum.vs              */
};

static inline int issue_active_kernel(uint32_t src_pa, uint32_t dst_pa)
{
    struct t1_op op = { .vtype = T1_VTYPE_E8_M1_TA_MA, .vl = 128u };
    int vmode = 0;

    /* ---- preamble ---- */
    for (uint32_t i = MMK_PRE_BASE; i < MMK_PRE_BASE + MMK_PRE_COUNT; i++) {
        uint32_t w      = matmul_8bitraw_short[i];
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
                case MMK_REG_A0: op.rs1 = src_pa;        break;
                case MMK_REG_A1: op.rs1 = MMK_WEIGHT_PA; break;
                case MMK_REG_A2: op.rs1 = dst_pa;        break;
                default:         op.rs1 = 0u;            break;
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
            const struct mmk_short_body_step *s = &mmk_short_body[p];
            uint32_t w      = matmul_8bitraw_short[MMK_BODY_BASE + p];
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

            op.instruction   = w;
            op.vtype         = s->vtype;
            op.vl            = s->vl;
            op.vertical_mode = (uint8_t)vmode;
            op.rs1           = s->inject_k ? (uint32_t)k : 0u;
            if (t1_issue(&op) < 0) return -1;
        }
    }

    /* ---- postamble ---- */
    {
        uint32_t w   = matmul_8bitraw_short[MMK_POST_BASE];
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
