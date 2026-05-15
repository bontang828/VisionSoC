/*
 * Non-LSU retire probe (2026-05-09).
 *
 * Background: with build 5h the AXI4-Lite read-zero bug is fixed and
 * triage_t1 passes, but ddr_roundtrip / port_grid_vadd / vert_lsu all
 * fail because MEM_COUNT never increments after vle8/vse8 issues - i.e.
 * T1's retire_mem_valid signal isn't reaching the wrapper. See
 * fyp_doc/camera_bringup_status.md § 6.4.
 *
 * This probe asks: does T1's NON-LSU retire path work? If
 *   vmv.s.x v8, t0   (scalar -> vector lane 0; OPMVX, no LSU)
 *   vmv.x.s t0, v8   (vector lane 0 -> scalar; OPMVV, retires to
 *                     wrapper rd_fifo via retire_rd_valid)
 * causes RD_FIFO_STS to show non-empty (bit 4 = !rd_fifo_empty) then
 * T1's general retire path is fine and the LSU is the only broken
 * channel. If RD_FIFO_STS stays at 0, the issue is wider - either
 * T1's slot is stuck or some retire wiring is wrong.
 *
 * Encoding:
 *   vmv.s.x v8, t0    funct6=010000, vm=1, vs2=00000, rs1=t0(5),
 *                     funct3=110 (OPMVX), vd=v8, opcode=0x57
 *                     = 0x4202E457
 *   vmv.x.s t0, v8    funct6=010000, vm=1, vs2=v8(8), vs1=00000,
 *                     funct3=010 (OPMVV), rd=t0(5), opcode=0x57
 *                     = 0x428022D7
 *
 * Verified bit-by-bit against the RVV 1.0 spec.
 *
 * Self-contained: no libt1 dependency, mmap T1 UIO directly.
 */

#include "libt1_regs.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAP_SIZE 4096u

#define INSTR_VMV_S_X_V8_T0  0x4202E457u
#define INSTR_VMV_X_S_T0_V8  0x428022D7u

static volatile uint32_t *g_regs;

static uint32_t rd(uint32_t off) { return g_regs[off / 4u]; }
static void     wr(uint32_t off, uint32_t v) { g_regs[off / 4u] = v; }

/* Single-instruction issue: write all op fields, write CTRL.W1S, poll
 * CTRL until issue_busy clears (== T1 accepted the issue).
 * Returns 0 on success, -1 on timeout. */
static int issue_one(uint32_t instr, uint32_t rs1, uint32_t rs2,
                     uint32_t vtype, uint32_t vl, uint8_t vert)
{
    /* Wait for issue_ready */
    for (int i = 0; i < 1000; i++) {
        if (rd(T1_REG_CTRL) & T1_CTRL_ISSUE_READY) break;
        usleep(100);
    }
    if (!(rd(T1_REG_CTRL) & T1_CTRL_ISSUE_READY)) {
        fprintf(stderr, "issue_one: not ready before issue (CTRL=0x%x)\n",
                rd(T1_REG_CTRL));
        return -1;
    }

    wr(T1_REG_INSTRUCTION,    instr);
    wr(T1_REG_RS1_DATA,       rs1);
    wr(T1_REG_RS2_DATA,       rs2);
    wr(T1_REG_VTYPE,          vtype);
    wr(T1_REG_VL,             vl);
    wr(T1_REG_VSTART,         0);
    wr(T1_REG_VCSR,           0);
    wr(T1_REG_VERTICAL_MODE,  vert ? 1u : 0u);
    wr(T1_REG_CTRL,           T1_CTRL_ISSUE_START);

    /* Wait for issue_busy clear */
    for (int i = 0; i < 1000; i++) {
        if (!(rd(T1_REG_CTRL) & T1_CTRL_ISSUE_BUSY)) {
            return 0;
        }
        usleep(100);
    }
    fprintf(stderr, "issue_one: still busy after 100ms (CTRL=0x%x)\n",
            rd(T1_REG_CTRL));
    return -1;
}

int main(void)
{
    int fd = open("/dev/uio4", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/uio4"); return 1; }

    void *map = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    g_regs = (volatile uint32_t *)map;

    printf("=== scalar_retire_probe ===\n");
    printf("\nInitial state:\n");
    printf("  CTRL          = 0x%08x\n", rd(T1_REG_CTRL));
    printf("  RD_FIFO_STS   = 0x%08x\n", rd(T1_REG_RD_FIFO_STS));
    printf("  CSR_FIFO_STS  = 0x%08x\n", rd(T1_REG_CSR_FIFO_STS));
    printf("  IRQ_STATUS    = 0x%08x\n", rd(T1_REG_IRQ_STATUS));
    printf("  MEM_COUNT     = 0x%08x\n", rd(T1_REG_MEM_COUNT));

    /* Move 0xDEADBEEF -> v8[0] via vmv.s.x */
    printf("\n[1] Issue vmv.s.x v8, t0   (rs1 = 0xDEADBEEF)\n");
    if (issue_one(INSTR_VMV_S_X_V8_T0, 0xDEADBEEFu, 0,
                  T1_VTYPE_E8_M4_TA_MA, 1, 0) < 0) {
        fprintf(stderr, "FAIL: vmv.s.x didn't accept\n");
        return 2;
    }
    printf("    issued OK; CTRL=0x%08x\n", rd(T1_REG_CTRL));

    /* Move v8[0] -> t0 via vmv.x.s; this should retire scalar value to
     * the wrapper's rd_fifo. */
    printf("\n[2] Issue vmv.x.s t0, v8   (should retire to rd_fifo)\n");
    if (issue_one(INSTR_VMV_X_S_T0_V8, 0, 0,
                  T1_VTYPE_E8_M4_TA_MA, 1, 0) < 0) {
        fprintf(stderr, "FAIL: vmv.x.s didn't accept\n");
        return 3;
    }
    printf("    issued OK; CTRL=0x%08x\n", rd(T1_REG_CTRL));

    /* Poll for retire - 200 ms max */
    printf("\n[3] Polling RD_FIFO_STS / IRQ_STATUS for retire_rd_valid:\n");
    for (int i = 0; i < 200; i++) {
        uint32_t fs = rd(T1_REG_RD_FIFO_STS);
        uint32_t st = rd(T1_REG_IRQ_STATUS);
        if (i < 5 || (i % 50) == 0 || (fs & T1_FIFO_NONEMPTY)) {
            printf("    poll[%d]: RD_FIFO_STS=0x%08x  IRQ_STATUS=0x%08x\n",
                   i, fs, st);
        }
        if (fs & T1_FIFO_NONEMPTY) {
            printf("    -> RD FIFO non-empty after %d polls\n", i);
            uint32_t pop_data = rd(T1_REG_RD_POP_DATA);
            uint32_t pop_meta = rd(T1_REG_RD_POP_META);
            printf("    -> POP_DATA=0x%08x  POP_META=0x%08x\n",
                   pop_data, pop_meta);
            printf("\n=== T1 NON-LSU RETIRE PATH WORKS ===\n");
            printf("This isolates the bug to LSU-specific retire (§ 6.4).\n");
            munmap(map, MAP_SIZE); close(fd);
            return 0;
        }
        usleep(1000);
    }
    fprintf(stderr,
            "\n=== T1 NON-LSU RETIRE PATH BROKEN TOO ===\n"
            "RD FIFO never went non-empty after 200 ms. Either T1's\n"
            "general retire path is dead, or vmv.s.x / vmv.x.s have a\n"
            "decode issue, or the retire wiring at t1_fpga_top is\n"
            "broken (different from just LSU). Wider debug needed.\n");
    munmap(map, MAP_SIZE); close(fd);
    return 4;
}
