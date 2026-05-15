/*
 * LSU store-path narrow probe (2026-05-09).
 *
 * lsu_load_probe.c proved:
 *   - T1 m_axi_hb AR/R (load) path works (vle8 fetches udmabuf bytes
 *     into v8 correctly, observed via vmv.x.s -> rd_fifo)
 *   - retire_mem_valid fires for vle8 (MEM_COUNT incremented)
 *
 * This probe asks: does vse8 fire retire_mem_valid? Does T1 actually
 * emit AW/W on m_axi_hb? Does the store reach DRAM?
 *
 * Plan:
 *   1. Pre-fill v8 by vle8 from a known source udmabuf.
 *   2. Read MEM_COUNT_a (after vle8).
 *   3. Issue vse8.v v8, (a0) targeting a different udmabuf.
 *   4. Read MEM_COUNT_b (after vse8).
 *   5. Compare destination udmabuf bytes to expected pattern.
 *
 * Outcomes:
 *   * MEM_COUNT_b > MEM_COUNT_a AND dest bytes match: vse8 worked,
 *     ddr_roundtrip's failure was something else.
 *   * MEM_COUNT_b == MEM_COUNT_a (no retire): T1 isn't completing the
 *     store. Either AW/W never asserts, or HPC0 holds bvalid forever,
 *     or T1 doesn't fire retire_mem_valid for stores in this RTL build.
 *     Need ILA to disambiguate.
 *   * MEM_COUNT_b > MEM_COUNT_a BUT dest bytes don't match: T1 retires
 *     the store but the AXI write never lands in DRAM. Problem is
 *     downstream of T1's commit (smartconnect_hb / HPC0_FPD).
 */

#include "libt1_regs.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAP_SIZE 4096u
#define INSTR_VLE8_V8_A0 0x02050407u
#define INSTR_VSE8_V8_A0 0x02050427u

#define UDMABUF0_PHYS  0x37900000u   /* source */
#define UDMABUF1_PHYS  0x37D00000u   /* destination */
#define UDMABUF_SIZE   0x00400000u

static volatile uint32_t *g_regs;
static uint32_t rd(uint32_t off) { return g_regs[off / 4u]; }
static void     wr(uint32_t off, uint32_t v) { g_regs[off / 4u] = v; }

static int issue_one(uint32_t instr, uint32_t rs1, uint32_t vtype, uint32_t vl)
{
    for (int i = 0; i < 1000; i++) {
        if (rd(T1_REG_CTRL) & T1_CTRL_ISSUE_READY) break;
        usleep(100);
    }
    if (!(rd(T1_REG_CTRL) & T1_CTRL_ISSUE_READY)) return -1;
    wr(T1_REG_INSTRUCTION, instr);
    wr(T1_REG_RS1_DATA, rs1);
    wr(T1_REG_RS2_DATA, 0);
    wr(T1_REG_VTYPE, vtype);
    wr(T1_REG_VL, vl);
    wr(T1_REG_VSTART, 0);
    wr(T1_REG_VCSR, 0);
    wr(T1_REG_VERTICAL_MODE, 0);
    wr(T1_REG_CTRL, T1_CTRL_ISSUE_START);
    for (int i = 0; i < 1000; i++) {
        if (!(rd(T1_REG_CTRL) & T1_CTRL_ISSUE_BUSY)) return 0;
        usleep(100);
    }
    return -1;
}

int main(void)
{
    int fdSrc = open("/dev/udmabuf0", O_RDWR | O_SYNC);
    int fdDst = open("/dev/udmabuf1", O_RDWR | O_SYNC);
    if (fdSrc < 0 || fdDst < 0) { perror("open udmabufs"); return 1; }
    volatile uint8_t *src = mmap(NULL, UDMABUF_SIZE, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fdSrc, 0);
    volatile uint8_t *dst = mmap(NULL, UDMABUF_SIZE, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fdDst, 0);
    if (src == MAP_FAILED || dst == MAP_FAILED) { perror("mmap"); return 1; }

    /* Distinct, non-zero source pattern */
    for (size_t i = 0; i < 256; i++) src[i] = (uint8_t)(0xC0u + (i & 0x0Fu));
    /* Zero destination */
    for (size_t i = 0; i < 256; i++) dst[i] = 0u;
    __asm__ __volatile__("dsb sy" ::: "memory");

    int t1fd = open("/dev/uio4", O_RDWR | O_SYNC);
    void *map = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, t1fd, 0);
    g_regs = (volatile uint32_t *)map;

    printf("=== lsu_store_probe ===\n");
    printf("\nsrc[0..7]  = ");
    for (int i = 0; i < 8; i++) printf("%02x ", src[i]);
    printf("\ndst[0..7]  = ");
    for (int i = 0; i < 8; i++) printf("%02x ", dst[i]);
    printf("\n");

    /* Drain any pending mem_count from prior tests */
    while (rd(T1_REG_MEM_COUNT) > 0) wr(T1_REG_MEM_COUNT, 1);
    printf("\nDrained MEM_COUNT to 0\n");

    /* Step 1: vle8 from src */
    printf("\n[1] Issue vle8.v v8, (a0)   rs1=0x%08x\n", UDMABUF0_PHYS);
    if (issue_one(INSTR_VLE8_V8_A0, UDMABUF0_PHYS,
                  T1_VTYPE_E8_M4_TA_MA, 128) < 0) {
        fprintf(stderr, "FAIL: vle8 not accepted\n"); return 2;
    }
    /* Wait for retire_mem_valid */
    int wait_loads = 0;
    for (; wait_loads < 200; wait_loads++) {
        if (rd(T1_REG_MEM_COUNT) >= 1) break;
        usleep(1000);
    }
    uint32_t mc_after_load = rd(T1_REG_MEM_COUNT);
    printf("    after vle8: MEM_COUNT=%u (waited %d ms)\n",
           mc_after_load, wait_loads);

    /* Step 2: vse8 to dst */
    printf("\n[2] Issue vse8.v v8, (a0)   rs1=0x%08x\n", UDMABUF1_PHYS);
    if (issue_one(INSTR_VSE8_V8_A0, UDMABUF1_PHYS,
                  T1_VTYPE_E8_M4_TA_MA, 128) < 0) {
        fprintf(stderr, "FAIL: vse8 not accepted\n"); return 3;
    }
    /* Wait for retire_mem_valid for store */
    int wait_stores = 0;
    for (; wait_stores < 200; wait_stores++) {
        uint32_t mc = rd(T1_REG_MEM_COUNT);
        if (mc > mc_after_load) break;
        usleep(1000);
    }
    uint32_t mc_after_store = rd(T1_REG_MEM_COUNT);
    printf("    after vse8: MEM_COUNT=%u (waited %d ms)\n",
           mc_after_store, wait_stores);

    /* Memory barrier + cache invalidate so CPU sees what T1 wrote */
    __asm__ __volatile__("dsb sy\nisb" ::: "memory");
    msync((void *)dst, UDMABUF_SIZE, MS_INVALIDATE);

    printf("\n[3] dst[0..7]  = ");
    for (int i = 0; i < 8; i++) printf("%02x ", dst[i]);
    printf("\n    src[0..7]  = ");
    for (int i = 0; i < 8; i++) printf("%02x ", src[i]);
    printf("\n");

    int matches = 1;
    for (size_t i = 0; i < 128; i++) {
        if (dst[i] != src[i]) { matches = 0; break; }
    }

    printf("\n=== Verdict ===\n");
    if (mc_after_store > mc_after_load) {
        printf("vse8 retired (mem_count: %u -> %u)\n",
               mc_after_load, mc_after_store);
        if (matches) {
            printf("Destination matches source: STORE PATH WORKS.\n"
                   "ddr_roundtrip's failure was something else (test\n"
                   "design or timing). vse8 + DRAM works.\n");
            return 0;
        } else {
            printf("Destination DOES NOT match source: T1 retired but\n"
                   "the AXI write didn't land in DRAM. Bug is\n"
                   "downstream of T1 commit (smartconnect_hb /\n"
                   "HPC0_FPD).\n");
            return 5;
        }
    } else {
        printf("vse8 NEVER retired (mem_count stayed %u). T1 didn't\n"
               "complete the store. Either AW/W never asserts, the\n"
               "store hangs waiting for bvalid, or retire_mem_valid\n"
               "doesn't fire for stores in this RTL build.\n",
               mc_after_load);
        return 6;
    }
}
