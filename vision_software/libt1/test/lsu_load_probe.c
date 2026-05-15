/*
 * LSU read-vs-write narrowing probe (2026-05-09).
 *
 * scalar_retire_probe.c proved T1's general retire path works (issue ->
 * non-LSU op -> rd_fifo populated). ddr_roundtrip.c showed mem_count
 * never increments after vle/vse, so LSU retire is broken - but doesn't
 * tell us if the LOAD path (vle8 reading DRAM via m_axi_hb AR/R) is
 * broken or only the STORE path (vse8 writing DRAM via m_axi_hb AW/W/B).
 *
 * Strategy: use the working non-LSU retire path to inspect what vle8
 * actually loaded into v8, without touching the broken LSU-store path.
 *   1. Write a known pattern to udmabuf via /dev/mem (CPU-side write).
 *   2. Issue vle8.v v8, (a0) with a0 = udmabuf.pa, vl=128.
 *      If LSU READ works: T1 actually fetches udmabuf bytes into v8.
 *      If LSU READ broken: v8 stays unchanged (or zero / undefined).
 *   3. Issue vmv.x.s t0, v8 (we know this retires to rd_fifo).
 *   4. Read POP_DATA. The lowest byte is v8[0] sign-extended.
 *
 *   If POP_DATA's low byte matches udmabuf[0], LSU READ works -> bug
 *   is purely on the WRITE/STORE path (vse8 via m_axi_hb AW/W/B).
 *   If POP_DATA doesn't match -> LSU READ is also broken -> wider bug.
 *
 * This is a zero-rebuild diagnostic. Run after triage_t1 and
 * scalar_retire_probe both pass.
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

#define INSTR_VLE8_V8_A0     0x02050407u
#define INSTR_VMV_X_S_T0_V8  0x428022D7u

/* udmabuf0 phys + 4 MiB region */
#define UDMABUF_PHYS         0x37900000u
#define UDMABUF_SIZE         0x00400000u

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
    wr(T1_REG_INSTRUCTION,    instr);
    wr(T1_REG_RS1_DATA,       rs1);
    wr(T1_REG_RS2_DATA,       0);
    wr(T1_REG_VTYPE,          vtype);
    wr(T1_REG_VL,             vl);
    wr(T1_REG_VSTART,         0);
    wr(T1_REG_VCSR,           0);
    wr(T1_REG_VERTICAL_MODE,  0);
    wr(T1_REG_CTRL,           T1_CTRL_ISSUE_START);
    for (int i = 0; i < 1000; i++) {
        if (!(rd(T1_REG_CTRL) & T1_CTRL_ISSUE_BUSY)) return 0;
        usleep(100);
    }
    return -1;
}

int main(void)
{
    /* /dev/mem is locked down on this kernel; use /dev/udmabuf0
     * directly. Same physical region, just routed via the
     * udmabuf char device which the kernel's whitelist allows. */
    int memfd = open("/dev/udmabuf0", O_RDWR | O_SYNC);
    if (memfd < 0) { perror("open /dev/udmabuf0"); return 1; }
    void *udmap = mmap(NULL, UDMABUF_SIZE, PROT_READ | PROT_WRITE,
                       MAP_SHARED, memfd, 0);
    if (udmap == MAP_FAILED) { perror("mmap udmabuf"); close(memfd); return 1; }

    /* Write known pattern to udmabuf. Don't be subtle - make
     * byte 0 a distinctive value that won't collide with stale state. */
    volatile uint8_t *udma = (volatile uint8_t *)udmap;
    for (size_t i = 0; i < 256; i++) {
        udma[i] = (uint8_t)(0xA0u + (i & 0x0Fu));
    }
    /* Memory barrier to ensure CPU writes are visible before T1 reads */
    __asm__ __volatile__("dsb sy" ::: "memory");

    int t1fd = open("/dev/uio4", O_RDWR | O_SYNC);
    if (t1fd < 0) { perror("open /dev/uio4"); return 1; }
    void *map = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, t1fd, 0);
    if (map == MAP_FAILED) { perror("mmap uio4"); close(t1fd); return 1; }
    g_regs = (volatile uint32_t *)map;

    printf("=== lsu_load_probe ===\n");
    printf("\nudmabuf @ phys 0x%08x, byte[0..15] written:\n  ", UDMABUF_PHYS);
    for (int i = 0; i < 16; i++) printf("%02x ", udma[i]);
    printf("\n");

    printf("\nInitial T1 state: CTRL=0x%08x  RD_FIFO_STS=0x%08x\n",
           rd(T1_REG_CTRL), rd(T1_REG_RD_FIFO_STS));

    /* vle8.v v8, (a0)  with a0 = udmabuf phys; vl=128, sew=8, lmul=4 */
    printf("\n[1] Issue vle8.v v8, (a0)   rs1=0x%08x vl=128\n", UDMABUF_PHYS);
    if (issue_one(INSTR_VLE8_V8_A0, UDMABUF_PHYS,
                  T1_VTYPE_E8_M4_TA_MA, 128) < 0) {
        fprintf(stderr, "FAIL: vle8 not accepted\n"); return 2;
    }
    printf("    issued OK; CTRL=0x%08x\n", rd(T1_REG_CTRL));

    /* Give the LSU a moment in case the AR/R round-trip to HPC0/DDR
     * needs cycles. We can't gate on retire_mem_valid (that's broken),
     * so just sleep - the load will either complete or it won't. */
    usleep(10000);

    /* vmv.x.s t0, v8 - pulls v8[0] back via the working retire path. */
    printf("\n[2] Issue vmv.x.s t0, v8   (read v8[0] back via rd_fifo)\n");
    if (issue_one(INSTR_VMV_X_S_T0_V8, 0,
                  T1_VTYPE_E8_M4_TA_MA, 1) < 0) {
        fprintf(stderr, "FAIL: vmv.x.s not accepted\n"); return 3;
    }
    printf("    issued OK; CTRL=0x%08x\n", rd(T1_REG_CTRL));

    /* Wait for retire to rd_fifo */
    int popped = 0;
    for (int i = 0; i < 200; i++) {
        if (rd(T1_REG_RD_FIFO_STS) & T1_FIFO_NONEMPTY) { popped = 1; break; }
        usleep(1000);
    }
    if (!popped) {
        fprintf(stderr, "FAIL: rd_fifo never went nonempty\n");
        return 4;
    }

    uint32_t pop_data = rd(T1_REG_RD_POP_DATA);
    uint32_t pop_meta = rd(T1_REG_RD_POP_META);
    printf("\n[3] POP_DATA=0x%08x  POP_META=0x%08x\n", pop_data, pop_meta);

    uint8_t expected = udma[0]; /* what we wrote at byte 0 */
    uint8_t got_low  = pop_data & 0xFFu;
    if (got_low == expected) {
        printf("\n=== LSU LOAD PATH WORKS ===\n");
        printf("v8[0] = 0x%02x matches udmabuf[0] = 0x%02x - vle8 is\n",
               got_low, expected);
        printf("reading DRAM via m_axi_hb correctly. Bug is purely on\n");
        printf("the LSU STORE path (vse8 / m_axi_hb AW/W/B channels).\n");
        munmap(map, MAP_SIZE); close(t1fd);
        munmap(udmap, UDMABUF_SIZE); close(memfd);
        return 0;
    }
    fprintf(stderr,
            "\n=== LSU LOAD PATH ALSO BROKEN ===\n"
            "v8[0] = 0x%02x  expected 0x%02x. vle8 didn't actually\n"
            "fetch from DRAM. Bug spans both LSU read and write -\n"
            "wider scope than store-only.\n", got_low, expected);
    munmap(map, MAP_SIZE); close(t1fd);
    munmap(udmap, UDMABUF_SIZE); close(memfd);
    return 5;
}
