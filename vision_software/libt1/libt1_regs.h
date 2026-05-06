#pragma once

#include <stdint.h>

/*
 * T1 AXI-Lite wrapper register offsets.
 *
 * These values mirror fpga/wrapper/t1_axi_lite_wrapper.sv. Keep this
 * header and the wrapper comment block in lockstep; offset drift silently
 * corrupts issue payloads.
 */
#define T1_REG_CTRL              0x00u
#define T1_REG_INSTRUCTION       0x04u
#define T1_REG_RS1_DATA          0x08u
#define T1_REG_RS2_DATA          0x0Cu
#define T1_REG_VTYPE             0x10u
#define T1_REG_VL                0x14u
#define T1_REG_VSTART            0x18u
#define T1_REG_VCSR              0x1Cu
#define T1_REG_RD_FIFO_STS       0x20u
#define T1_REG_RD_POP_DATA       0x24u
#define T1_REG_RD_POP_META       0x28u
#define T1_REG_CSR_FIFO_STS      0x2Cu
#define T1_REG_CSR_POP           0x30u
#define T1_REG_CSR_FFLAG         0x34u
#define T1_REG_MEM_COUNT         0x38u
#define T1_REG_IRQ_EN            0x3Cu
#define T1_REG_IRQ_STATUS        0x40u
#define T1_REG_VERTICAL_MODE     0x44u
#define T1_REG_PERF_TAG          0x48u
#define T1_REG_PERF_DELTA        0x4Cu
#define T1_REG_PERF_CYCLES_LO    0x50u
#define T1_REG_PERF_CYCLES_HI    0x54u

#define T1_CTRL_ISSUE_START      (1u << 0)  /* W1S */
#define T1_CTRL_ISSUE_READY      (1u << 1)  /* RO */
#define T1_CTRL_ISSUE_BUSY       (1u << 2)  /* RO */

#define T1_IRQ_RD                (1u << 0)
#define T1_IRQ_CSR               (1u << 1)
#define T1_IRQ_MEM               (1u << 2)

#define T1_FIFO_COUNT_MASK       0x0Fu
#define T1_FIFO_NONEMPTY         (1u << 4)

/*
 * Standard RVV vtype value for e8, m4, ta, ma.
 * vma=1, vta=1, vsew=0, vlmul=2.
 */
#define T1_VTYPE_E8_M4_TA_MA     0x000000C2u

/*
 * Xilinx AXI DMA simple-mode register subset, relative to the DMA
 * AXI-Lite BAR at 0xA0010000.
 */
#define DMA_REG_MM2S_CR          0x00u
#define DMA_REG_MM2S_SR          0x04u
#define DMA_REG_MM2S_SRC_ADDR    0x18u
#define DMA_REG_MM2S_LENGTH      0x28u
#define DMA_REG_S2MM_CR          0x30u
#define DMA_REG_S2MM_SR          0x34u
#define DMA_REG_S2MM_DST_ADDR    0x48u
#define DMA_REG_S2MM_LENGTH      0x58u

#define DMA_CR_RUN               (1u << 0)
#define DMA_CR_RESET             (1u << 2)
#define DMA_CR_IOC_IRQ_EN        (1u << 12)
#define DMA_CR_DLY_IRQ_EN        (1u << 13)
#define DMA_CR_ERR_IRQ_EN        (1u << 14)

#define DMA_SR_HALTED            (1u << 0)
#define DMA_SR_IDLE              (1u << 1)
#define DMA_SR_IOC_IRQ           (1u << 12)
#define DMA_SR_DLY_IRQ           (1u << 13)
#define DMA_SR_ERR_IRQ           (1u << 14)
#define DMA_SR_INT_ERR           (1u << 4)
#define DMA_SR_SLV_ERR           (1u << 5)
#define DMA_SR_DEC_ERR           (1u << 6)

#define DMA_SR_IRQ_MASK          (DMA_SR_IOC_IRQ | DMA_SR_DLY_IRQ | DMA_SR_ERR_IRQ)
#define DMA_SR_ERR_MASK          (DMA_SR_ERR_IRQ | DMA_SR_INT_ERR | DMA_SR_SLV_ERR | DMA_SR_DEC_ERR)
