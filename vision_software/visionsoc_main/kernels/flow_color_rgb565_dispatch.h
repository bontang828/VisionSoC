#pragma once

#include "flow_color_rgb565.h"
#include "libt1.h"
#include "libt1_regs.h"

#include <errno.h>
#include <stdint.h>

#define FLOW_RGB_REG_A0 10u
#define FLOW_RGB_REG_A1 11u
#define FLOW_RGB_REG_A2 12u
#define FLOW_RGB_REG_A3 13u
#define FLOW_RGB_REG_A4 14u
#define FLOW_RGB_REG_A5 15u
#define FLOW_RGB_REG_A6 16u

#define FLOW_RGB_OPCODE_LOAD_FP  0x07u
#define FLOW_RGB_OPCODE_STORE_FP 0x27u
#define FLOW_RGB_OPCODE_OP_V     0x57u
#define FLOW_RGB_FUNCT3_OPIVX    0x4u
#define FLOW_RGB_FUNCT3_OPMVX    0x6u

static inline int issue_flow_color_rgb565(uint32_t src_pa,
                                          uint32_t lo_pa,
                                          uint32_t hi_pa)
{
    const uint32_t count = (uint32_t)
        (sizeof(flow_color_rgb565) / sizeof(flow_color_rgb565[0]));
    if (count == 0u) {
        errno = EINVAL;
        return -1;
    }

    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M1_TA_MA,
        .vl = 128,
    };

    for (uint32_t i = 0; i < count; i++) {
        uint32_t w = flow_color_rgb565[i];
        uint32_t opcode = w & 0x7Fu;
        uint32_t funct3 = (w >> 12) & 0x7u;
        uint32_t rs1 = (w >> 15) & 0x1Fu;

        if (opcode == FLOW_RGB_OPCODE_LOAD_FP ||
            opcode == FLOW_RGB_OPCODE_STORE_FP) {
            switch (rs1) {
                case FLOW_RGB_REG_A0: op.rs1 = src_pa; break;
                case FLOW_RGB_REG_A1: op.rs1 = lo_pa;  break;
                case FLOW_RGB_REG_A2: op.rs1 = hi_pa;  break;
                default:              op.rs1 = 0u;     break;
            }
        } else if (opcode == FLOW_RGB_OPCODE_OP_V &&
                   (funct3 == FLOW_RGB_FUNCT3_OPIVX ||
                    funct3 == FLOW_RGB_FUNCT3_OPMVX)) {
            switch (rs1) {
                case FLOW_RGB_REG_A3: op.rs1 = 50u;  break;
                case FLOW_RGB_REG_A4: op.rs1 = 100u; break;
                case FLOW_RGB_REG_A5: op.rs1 = 150u; break;
                case FLOW_RGB_REG_A6: op.rs1 = 200u; break;
                default:              op.rs1 = 0u;   break;
            }
        } else {
            op.rs1 = 0u;
        }

        op.instruction = w;
        op.vertical_mode = 0u;
        if (t1_issue(&op) < 0) {
            return -1;
        }
    }
    return 0;
}
