/*
 * matmul_select.h -- activate the full-precision (widening) matmul.
 *
 * Drives kernels/matmul.S. Same loop structure as matmul_8bitraw_select.h
 * (preamble | body x128 | postamble, gather index injected per iteration),
 * but the body changes SEW/vl mid-iteration to keep the dot product exact:
 *
 *   pos 1 vwmulu.vv    : SEW=8  source, vl=128   (u8*u8 -> u16, LMUL=2 dst)
 *   pos 2 vwredsumu.vs : SEW=16 source, vl=128   (sum u16 -> u32 at lane 0)
 *   pos 3 vnsrl.wi     : SEW=16 dst,    vl=1      (u32 -> u16, lane 0 only)
 *   pos 4 vnsrl.wi     : SEW=8  dst,    vl=1      (u16 -> u8,  lane 0 only)
 * everything else is SEW=8 / vl=128. Each issued op carries its own vtype
 * and vl (the wrapper latches them at issue, like a per-op vsetvli).
 *
 * Buffer routing is identical to the raw kernel:
 *   a0 -> src_pa        : A = camera frame
 *   a1 -> MMK_WEIGHT_PA : B = 128x128 weights (main.c stages this ONCE)
 *   a2 -> dst_pa        : C = result
 *   a4                  : vrgather column index k, injected per iteration
 *
 * !!! The widening/narrowing ops and mid-kernel SEW/vl changes are
 * unverified on this 2D fabric -- validate against t1emu before trusting
 * hardware output (see matmul.S header). !!!
 */
#pragma once

#include "matmul.h"
#include "matmul_weights.h"
#include "libt1.h"
#include "libt1_regs.h"

#include <errno.h>
#include <stdint.h>

#define ACTIVE_KERNEL            matmul
#define ACTIVE_KERNEL_NEUTRAL_UV 1
#define ACTIVE_KERNEL_FLOW_COLOR 0

#define MMK_URAM_BASE_PA   0xA0080000u
#define MMK_WEIGHT_PA      (MMK_URAM_BASE_PA + 0x08000u)

/* main.c hooks: stage matrix B into URAM once at startup. The widening
 * kernel pairs with the [1,2,1] blur (display scale >>2 set in matmul.S). */
#define ACTIVE_KERNEL_NEEDS_WEIGHTS  1
#define ACTIVE_KERNEL_WEIGHT_PA      MMK_WEIGHT_PA
#define ACTIVE_KERNEL_WEIGHT_PATTERN MMK_B_BLUR

/* .S region layout -- keep in lockstep with matmul.S. */
#define MMK_PRE_BASE    0u
#define MMK_PRE_COUNT   8u
#define MMK_BODY_BASE   8u
#define MMK_BODY_COUNT  8u
#define MMK_POST_BASE   16u
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

/* vtype encodings (vma=1,vta=1): bits [7]=vma [6]=vta [5:3]=vsew [2:0]=vlmul.
 *   e8/m1  -> T1_VTYPE_E8_M1_TA_MA (0xC0)
 *   e16/m1 -> 0xC8   e16/m2 -> 0xC9 */
#define MMK_VTYPE_E16_M1  0xC8u
#define MMK_VTYPE_E16_M2  0xC9u

struct mmk_body_step {
    uint32_t vtype;
    uint32_t vl;
    uint8_t  vmode;
    uint8_t  inject_k;
};

static const struct mmk_body_step mmk_body[MMK_BODY_COUNT] = {
    { T1_VTYPE_E8_M1_TA_MA, 128u, 1u, 1u },  /* vrgather.vx (V), rs1=k    */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u, 0u },  /* vwmulu.vv (u8*u8->u16)    */
    { MMK_VTYPE_E16_M2,     128u, 0u, 0u },  /* vwredsumu.vs (u16->u32)   */
    { MMK_VTYPE_E16_M1,       1u, 0u, 0u },  /* vnsrl.wi u32->u16 (lane0) */
    { T1_VTYPE_E8_M1_TA_MA,   1u, 0u, 0u },  /* vnsrl.wi u16->u8  (lane0) */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u, 0u },  /* vslideup.vi               */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u, 0u },  /* vmerge.vvm                */
    { T1_VTYPE_E8_M1_TA_MA, 128u, 0u, 0u },  /* vmv.v.v                   */
};

static inline int issue_active_kernel(uint32_t src_pa, uint32_t dst_pa)
{
    struct t1_op op = { .vtype = T1_VTYPE_E8_M1_TA_MA, .vl = 128u };
    int vmode = 0;

    /* ---- preamble ---- */
    for (uint32_t i = MMK_PRE_BASE; i < MMK_PRE_BASE + MMK_PRE_COUNT; i++) {
        uint32_t w      = matmul[i];
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
                case MMK_REG_A0: op.rs1 = src_pa;        break;  /* A */
                case MMK_REG_A1: op.rs1 = MMK_WEIGHT_PA; break;  /* B */
                case MMK_REG_A2: op.rs1 = dst_pa;        break;  /* C */
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
            const struct mmk_body_step *s = &mmk_body[p];
            op.instruction   = matmul[MMK_BODY_BASE + p];
            op.vtype         = s->vtype;
            op.vl            = s->vl;
            op.vertical_mode = s->vmode;
            op.rs1           = s->inject_k ? (uint32_t)k : 0u;
            if (t1_issue(&op) < 0) return -1;
        }
    }

    /* ---- postamble: store C row ---- */
    {
        uint32_t w   = matmul[MMK_POST_BASE];
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
