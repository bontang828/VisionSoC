/*
 * active_kernel_dispatcher.h -- generic issue_active_kernel() implementation.
 *
 * Each <name>_select.h #defines ACTIVE_KERNEL (the words array) and
 * ACTIVE_KERNEL_NEUTRAL_UV (0 or 1), then #includes this header. The
 * expanded issue_active_kernel iterates the kernel words and handles two
 * libt1-specific conventions automatically -- the kernel .S file is the
 * single source of truth, no parallel per-word schedule needed:
 *
 *   1) `csrwi 0x7c0, IMM` (or `csrw 0x7c0, x0`) inside the kernel acts as an
 *      inline horizontal/vertical mode toggle. The dispatcher detects the
 *      instruction word at runtime, sets a running `vmode` state, and SKIPS
 *      the T1 issue. The T1 wrapper never sees the csrwi; it sees the next
 *      vector instruction with op.vertical_mode = vmode (libt1.c writes that
 *      into the MMIO VERTICAL_MODE register before forwarding).
 *
 *      ONLY the immediate forms work -- `csrwi 0x7c0, 1`, `csrwi 0x7c0, 0`,
 *      `csrw 0x7c0, zero`. The simulator-style `li t3, 1 ; csrw 0x7c0, t3`
 *      is NOT supported because libt1 has no scalar register file at this
 *      layer -- there is no way to know what value `t3` would have held.
 *      The dispatcher errors out (ENOTSUP) on a csrw 0x7c0 whose rs1 is a
 *      non-x0 register.
 *
 *   2) rs1 routing on LSU instructions (vle/vse opcodes 0x07/0x27): the
 *      dispatcher inspects the instruction's rs1 register field and assigns
 *      op.rs1 = src_pa when rs1 = x10 (a0), dst_pa when rs1 = x11 (a1), and
 *      0 otherwise. So `vle8.v v8, (a0)` reads the source buffer and
 *      `vse8.v v16, (a1)` writes the destination buffer -- no per-word
 *      index plumbing in <name>_select.h.
 *
 *      Strided/indexed LSU and multi-buffer kernels (e.g. grid_vadd's
 *      a0/a1/a2 pattern) need a custom issue function -- skip this
 *      dispatcher and write the issue loop by hand in your <name>_select.h.
 *
 * For non-LSU vector ops (opcode 0x57 = OP-V) bits[19:15] is vs1, not rs1, so
 * op.rs1 stays 0 there.
 */
#pragma once

#ifndef ACTIVE_KERNEL
#  error "include active_kernel_dispatcher.h after #defining ACTIVE_KERNEL"
#endif
#ifndef ACTIVE_KERNEL_NEUTRAL_UV
#  error "missing #define ACTIVE_KERNEL_NEUTRAL_UV (0 or 1)"
#endif

#include "libt1.h"
#include "libt1_regs.h"

#include <errno.h>
#include <stdint.h>

/* Reg numbers for the libt1 PA-routing convention. */
#define AKD_REG_X0  0u
#define AKD_REG_A0 10u
#define AKD_REG_A1 11u

/* CSR ops to 0x7c0. CSRRWI opcode 0x73, funct3 0b101; CSRRW funct3 0b001. */
#define AKD_OPCODE_SYSTEM 0x73u
#define AKD_FUNCT3_CSRRWI 0x5u
#define AKD_FUNCT3_CSRRW  0x1u
#define AKD_CSR_VERTMODE  0x7C0u

/* LSU opcodes. */
#define AKD_OPCODE_LOAD_FP  0x07u
#define AKD_OPCODE_STORE_FP 0x27u

static inline int issue_active_kernel(uint32_t src_pa, uint32_t dst_pa)
{
    /* ACTIVE_KERNEL resolves to a concrete static array, not a pointer, so
     * sizeof gives the compile-time element count -- no separate _count
     * macro to keep in sync. */
    const uint32_t count = (uint32_t)
        (sizeof(ACTIVE_KERNEL) / sizeof((ACTIVE_KERNEL)[0]));
    if (count == 0u) {
        errno = EINVAL;
        return -1;
    }

    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M4_TA_MA,
        .vl = 128,
    };

    int vmode = 0;  /* CSR 0x7c0 state mirrored on the host */

    for (uint32_t i = 0; i < count; i++) {
        uint32_t w      = ACTIVE_KERNEL[i];
        uint32_t opcode = w & 0x7Fu;
        uint32_t funct3 = (w >> 12) & 0x7u;
        uint32_t rs1    = (w >> 15) & 0x1Fu;

        /* CSR 0x7c0 mode toggle? */
        if (opcode == AKD_OPCODE_SYSTEM &&
            ((w >> 20) & 0xFFFu) == AKD_CSR_VERTMODE) {
            if (funct3 == AKD_FUNCT3_CSRRWI) {
                vmode = ((w >> 15) & 0x1Fu) ? 1 : 0;
                continue;
            }
            if (funct3 == AKD_FUNCT3_CSRRW && rs1 == AKD_REG_X0) {
                vmode = 0;  /* csrw 0x7c0, zero */
                continue;
            }
            /* csrw 0x7c0, <non-x0 register> -- can't resolve without a
             * scalar regfile. Use csrwi instead. */
            errno = ENOTSUP;
            return -1;
        }

        /* LSU? Route op.rs1 by the scalar register the .S nominated. */
        if (opcode == AKD_OPCODE_LOAD_FP || opcode == AKD_OPCODE_STORE_FP) {
            if (rs1 == AKD_REG_A0) {
                op.rs1 = src_pa;
            } else if (rs1 == AKD_REG_A1) {
                op.rs1 = dst_pa;
            } else {
                op.rs1 = 0u;
            }
        } else {
            /* Non-LSU vector op: bits[19:15] is vs1, not a scalar register. */
            op.rs1 = 0u;
        }

        op.instruction = w;
        op.vertical_mode = vmode;
        if (t1_issue(&op) < 0) {
            return -1;
        }
    }

    return 0;
}
