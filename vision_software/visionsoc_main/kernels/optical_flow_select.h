/*
 * optical_flow_select.h -- activate the 5-direction block-matching
 * optical-flow kernel.
 *
 * Cannot use the generic active_kernel_dispatcher.h because:
 *   1) The kernel needs THREE buffers (curr, prev, dst), not two. The
 *      generic dispatcher only routes a0 -> src_pa and a1 -> dst_pa.
 *   2) The kernel's vmul.vx needs op.rs1 = 50 (the
 *      visualisation scaling factor). The generic dispatcher sets
 *      op.rs1 = 0 for all non-LSU ops.
 *
 * This file hand-rolls the issue loop, mirroring
 * active_kernel_dispatcher.h's structure but extending the rs1 routing
 * table:
 *
 *   vle/vse:
 *     a0 -> src_pa            (curr from URAM_HALF_A, staged by main.c)
 *     a1 -> OFK_PREV_PA       (internal URAM region; updated by kernel)
 *     a2 -> dst_pa            (motion-direction map -> URAM_HALF_B)
 *
 *   non-LSU OPIVX/OPMVX vector-scalar ops (vmul.vx, etc.):
 *     a3 -> OFK_DIR_SCALE = 50
 *
 *   csrwi 0x7c0, IMM and csrw 0x7c0, x0:
 *     dispatcher-only; tracks the H/V mode for op.vertical_mode on
 *     subsequent issued vector ops. Same handling as the generic
 *     dispatcher.
 *
 * Frame-to-frame state lives entirely in URAM at OFK_PREV_PA, refreshed
 * by the kernel's own `vse8.v v8, (a1)` at the end of
 * optical_flow.S). No PS-side ping-pong is required.
 *
 * On first invocation OFK_PREV_PA is whatever URAM held after overlay
 * reload, typically all-zero. The first frame's argmin is 0 = static
 * everywhere (large SAD_static masks all directional candidates) so the
 * first output frame is black; from frame 2 onwards the kernel is in
 * steady state.
 */
#pragma once

#include "optical_flow.h"
#include "libt1.h"
#include "libt1_regs.h"

#include <errno.h>
#include <stdint.h>

#define ACTIVE_KERNEL            optical_flow
#define ACTIVE_KERNEL_NEUTRAL_UV 1   /* motion-direction map is luma-only */
#define ACTIVE_KERNEL_FLOW_COLOR 1   /* false-colour direction map in main.c */

/* Internal URAM region for prev frame, at offset 32 KB (16 KB after
 * URAM_HALF_B). Disjoint from URAM_HALF_A / URAM_HALF_B in main.c. */
#define OFK_URAM_BASE_PA  0xA0080000u
#define OFK_PREV_PA       (OFK_URAM_BASE_PA + 0x08000u)

#define OFK_DIR_SCALE     50u   /* visualisation: argmin (0..4) * 50 */

/* Register numbers (RVV: rs1 / rs2 / rd / vs1 fields all use 5-bit x-reg
 * indices except where the slot encodes vd/vs1/vs2/imm). */
#define OFK_REG_X0   0u
#define OFK_REG_A0  10u
#define OFK_REG_A1  11u
#define OFK_REG_A2  12u
#define OFK_REG_A3  13u

/* Opcodes / funct3 for the instruction families we route. */
#define OFK_OPCODE_SYSTEM    0x73u
#define OFK_FUNCT3_CSRRWI    0x5u
#define OFK_FUNCT3_CSRRW     0x1u
#define OFK_CSR_VERTMODE     0x7C0u

#define OFK_OPCODE_LOAD_FP   0x07u   /* vle*.v */
#define OFK_OPCODE_STORE_FP  0x27u   /* vse*.v */
#define OFK_OPCODE_OP_V      0x57u   /* vector arith family */
#define OFK_FUNCT3_OPIVX     0x4u    /* OPIVX vector-scalar integer ops */
#define OFK_FUNCT3_OPMVX     0x6u    /* OPMVX vector-scalar integer ops, incl. vmul.vx */

static inline int issue_active_kernel(uint32_t src_pa, uint32_t dst_pa)
{
    const uint32_t count = (uint32_t)
        (sizeof(ACTIVE_KERNEL) / sizeof((ACTIVE_KERNEL)[0]));
    if (count == 0u) {
        errno = EINVAL;
        return -1;
    }

    /* LMUL=1 for the "big" config (vLen=1024): a single register already
     * holds 128 e8 elements = one image row. LMUL=4 would just consume
     * 4x more register space for no benefit. */
    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M1_TA_MA,
        .vl = 128,
    };

    int vmode = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t w      = ACTIVE_KERNEL[i];
        uint32_t opcode = w & 0x7Fu;
        uint32_t funct3 = (w >> 12) & 0x7u;
        uint32_t rs1    = (w >> 15) & 0x1Fu;
        uint32_t csr    = (w >> 20) & 0xFFFu;

        /* CSR 0x7c0 mode toggle? Dispatcher-only (NOT issued to T1). */
        if (opcode == OFK_OPCODE_SYSTEM && csr == OFK_CSR_VERTMODE) {
            if (funct3 == OFK_FUNCT3_CSRRWI) {
                vmode = rs1 ? 1 : 0;
                continue;
            }
            if (funct3 == OFK_FUNCT3_CSRRW && rs1 == OFK_REG_X0) {
                vmode = 0;
                continue;
            }
            /* csrw 0x7c0, <non-x0 register> -- can't resolve without a
             * scalar regfile. Use csrwi instead. */
            errno = ENOTSUP;
            return -1;
        }

        if (opcode == OFK_OPCODE_LOAD_FP || opcode == OFK_OPCODE_STORE_FP) {
            switch (rs1) {
                case OFK_REG_A0: op.rs1 = src_pa;       break;  /* curr */
                case OFK_REG_A1: op.rs1 = OFK_PREV_PA;  break;  /* prev */
                case OFK_REG_A2: op.rs1 = dst_pa;       break;  /* dst  */
                default:         op.rs1 = 0u;           break;
            }
        } else if (opcode == OFK_OPCODE_OP_V &&
                   (funct3 == OFK_FUNCT3_OPIVX ||
                    funct3 == OFK_FUNCT3_OPMVX)) {
            /* Vector-scalar OPIVX/OPMVX: inject the scalar value that the
             * kernel's a-register name codes for. vmul.vx is OPMVX
             * (funct3=0x6); missing that route makes the final scale use
             * zero and blacks out the direction map. */
            switch (rs1) {
                case OFK_REG_A3: op.rs1 = OFK_DIR_SCALE; break;  /* 50 */
                default:         op.rs1 = 0u;            break;
            }
        } else {
            /* Pure vector ops: bits[19:15] is vs1, not a scalar register. */
            op.rs1 = 0u;
        }

        op.instruction = w;
        op.vertical_mode = (uint8_t)vmode;
        if (t1_issue(&op) < 0) {
            return -1;
        }
    }
    return 0;
}
