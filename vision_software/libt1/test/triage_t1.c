/*
 * Cheap on-Kria triage for the wrapper-register bus-error symptom
 * documented in fyp_doc/camera_bringup_status.md § 6.1.
 *
 * Background: with the current bitstream, 0xa0000000 (CTRL),
 * 0xa0000048 (PERF_TAG, W) and 0xa0000050 (PERF_CYCLES_LO) all R/W
 * cleanly, while 0xa0000044 (VERTICAL_MODE) and 0xa000004c
 * (PERF_DELTA) throw SIGBUS on both R and W. The post-synth netlist
 * has the right flops and decode LUTs, so the wrapper itself is fine
 * - the bug is upstream (suspect: smartconnect_ctrl with NUM_CLKS=2
 * + STRATEGY=LOW_AREA stripping arbitration on the read path).
 *
 * This probe runs three tests against the existing bitstream so we
 * can decide whether to launch a multi-hour Vivado rebuild:
 *
 *   1. CTRL.W1S smoke. Write 0x1 to CTRL, read back. Expect bit[2]
 *      (issue_pending) set if writes work. If it still reads 0x2,
 *      writes to low offsets are also dropped - extends the failure
 *      beyond the "added in this patch" set.
 *
 *   2. PERF_TAG round-trip via PERF_DELTA. Write 0x42 to PERF_TAG
 *      (=START), busy-wait, write 0x00 (=STOP), read PERF_DELTA. If
 *      non-zero, writes to 0x48 work AND reads of 0x4C work - narrows
 *      the bug to a specific subset.
 *
 *   3. IRQ_EN write/readback. Write T1_IRQ_MEM to IRQ_EN, read back.
 *      Readback already known to be 0 from prior probing; the full
 *      "did the write stick?" half of this test requires issuing a
 *      real LSU and watching for the IRQ - that's already exercised
 *      by test/vert_lsu, which should be re-run after this probe.
 *
 * Self-contained: opens the T1 UIO (default /dev/uio4, override via
 * argv[1]) and mmaps a 4 KiB BAR. No libt1 dependency, so a libt1
 * regression cannot mask a bus-fabric symptom.
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

static volatile uint32_t *g_regs;

static uint32_t rd(uint32_t off)
{
    return g_regs[off / 4u];
}

static void wr(uint32_t off, uint32_t v)
{
    g_regs[off / 4u] = v;
}

static const struct {
    uint32_t off;
    const char *name;
} REG_NAMES[] = {
    {T1_REG_CTRL,           "CTRL"},
    {T1_REG_INSTRUCTION,    "INSTRUCTION"},
    {T1_REG_RS1_DATA,       "RS1_DATA"},
    {T1_REG_RS2_DATA,       "RS2_DATA"},
    {T1_REG_VTYPE,          "VTYPE"},
    {T1_REG_VL,             "VL"},
    {T1_REG_VSTART,         "VSTART"},
    {T1_REG_VCSR,           "VCSR"},
    {T1_REG_RD_FIFO_STS,    "RD_FIFO_STS"},
    {T1_REG_RD_POP_DATA,    "RD_POP_DATA"},
    {T1_REG_RD_POP_META,    "RD_POP_META"},
    {T1_REG_CSR_FIFO_STS,   "CSR_FIFO_STS"},
    {T1_REG_CSR_POP,        "CSR_POP"},
    {T1_REG_CSR_FFLAG,      "CSR_FFLAG"},
    {T1_REG_MEM_COUNT,      "MEM_COUNT"},
    {T1_REG_IRQ_EN,         "IRQ_EN"},
    {T1_REG_IRQ_STATUS,     "IRQ_STATUS"},
    {T1_REG_VERTICAL_MODE,  "VERTICAL_MODE"},
    {T1_REG_PERF_TAG,       "PERF_TAG"},
    {T1_REG_PERF_DELTA,     "PERF_DELTA"},
    {T1_REG_PERF_CYCLES_LO, "PERF_CYCLES_LO"},
    {T1_REG_PERF_CYCLES_HI, "PERF_CYCLES_HI"},
};

static void dump_all_regs(const char *label)
{
    printf("--- %s ---\n", label);
    for (size_t i = 0; i < sizeof(REG_NAMES)/sizeof(REG_NAMES[0]); i++) {
        printf("    [0x%02x] %-15s = 0x%08x\n",
               REG_NAMES[i].off, REG_NAMES[i].name,
               rd(REG_NAMES[i].off));
    }
}

static int test_perf_cycles_advance(void)
{
    uint32_t lo0 = rd(T1_REG_PERF_CYCLES_LO);
    uint32_t hi0 = rd(T1_REG_PERF_CYCLES_HI);
    for (volatile unsigned i = 0; i < 1000000u; i++) {
        __asm__ __volatile__("");
    }
    uint32_t lo1 = rd(T1_REG_PERF_CYCLES_LO);
    uint32_t hi1 = rd(T1_REG_PERF_CYCLES_HI);

    printf("[0] PERF_CYCLES advance check (clock-alive smoke)\n");
    printf("    t0 PERF_CYCLES_HI:LO = 0x%08x:%08x\n", hi0, lo0);
    printf("    t1 PERF_CYCLES_HI:LO = 0x%08x:%08x\n", hi1, lo1);
    if (lo1 != lo0 || hi1 != hi0) {
        printf("    PASS: cycles advance -> T1 wrapper is clocked\n");
        return 0;
    }
    printf("    FAIL: cycles stuck -> wrapper either not clocked, OR "
           "PERF_CYCLES read mux is also broken (unlikely - 0x50 was "
           "in the 'works' set per § 6.1)\n");
    return -1;
}

static int test_ctrl_w1s(void)
{
    uint32_t before = rd(T1_REG_CTRL);
    wr(T1_REG_CTRL, T1_CTRL_ISSUE_START);
    uint32_t after = rd(T1_REG_CTRL);
    int issue_pending_set = (after & (1u << 2)) != 0;

    printf("[1] CTRL.W1S smoke\n");
    printf("    CTRL before write : 0x%08x\n", before);
    printf("    CTRL after  write : 0x%08x\n", after);
    if (issue_pending_set) {
        printf("    PASS: bit[2]=1 -> write reached the issue FSM\n");
        return 0;
    }
    if (after == before) {
        printf("    FAIL: CTRL unchanged -> low-offset writes dropped\n");
        return -1;
    }
    printf("    PARTIAL: CTRL changed but bit[2] not set "
           "(issue_pending may have already self-cleared)\n");
    return 0;
}

static int test_perf_tag_roundtrip(void)
{
    /* Re-arm: an in-flight perf measurement would skew the result. */
    wr(T1_REG_PERF_TAG, 0u);

    /* Bus-error guard: try a read of 0x4C first to surface SIGBUS now,
     * before we've started a perf window. The signal handler is at the
     * caller's discretion (`devmem2`-style probes don't install one);
     * if this binary aborts on this line, the kernel will print
     * "Bus error" and exit - that itself is the diagnostic for read of
     * 0x4C being broken. */
    uint32_t pre_delta = rd(T1_REG_PERF_DELTA);

    wr(T1_REG_PERF_TAG, 0x42u);

    /* ~1 ms-ish busy wait. PERF_CYCLES advances at the T1 clock rate
     * (~60 MHz); a few hundred microseconds gives a comfortably
     * non-zero delta. */
    for (volatile unsigned i = 0; i < 100000u; i++) {
        __asm__ __volatile__("");
    }

    wr(T1_REG_PERF_TAG, 0x00u);
    uint32_t delta = rd(T1_REG_PERF_DELTA);

    printf("[2] PERF_TAG round-trip via PERF_DELTA\n");
    printf("    PERF_DELTA pre-arm : 0x%08x\n", pre_delta);
    printf("    PERF_DELTA after   : 0x%08x\n", delta);
    if (delta != 0u) {
        printf("    PASS: write 0x48 + read 0x4C both work - bug is "
               "narrower than 'whole upper half'\n");
        return 0;
    }
    printf("    FAIL: PERF_DELTA stuck at 0 -> either write to 0x48 "
           "dropped, or read of 0x4C broken\n");
    return -1;
}

static int test_vertical_mode_roundtrip(void)
{
    /* The original § 6.1 observation was that 0x44 (VERTICAL_MODE)
     * SIGBUSes on R and W. If this read survives without bus error
     * we already know that claim is at least partially stale.
     * Then write 1, read; write 0, read. */
    uint32_t pre = rd(T1_REG_VERTICAL_MODE);

    wr(T1_REG_VERTICAL_MODE, 1u);
    uint32_t after_one = rd(T1_REG_VERTICAL_MODE);

    wr(T1_REG_VERTICAL_MODE, 0u);
    uint32_t after_zero = rd(T1_REG_VERTICAL_MODE);

    printf("[2b] VERTICAL_MODE round-trip\n");
    printf("    VERTICAL_MODE pre        : 0x%08x\n", pre);
    printf("    VERTICAL_MODE after wr 1 : 0x%08x\n", after_one);
    printf("    VERTICAL_MODE after wr 0 : 0x%08x\n", after_zero);
    if (after_one == 1u && after_zero == 0u) {
        printf("    PASS: 0x44 round-trips cleanly - § 6.1 SIGBUS "
               "claim for this address is stale\n");
        return 0;
    }
    if (after_one == 0u && after_zero == 0u) {
        printf("    FAIL: writes to 0x44 dropped (or read mux returns "
               "constant 0 - same as IRQ_EN symptom)\n");
        return -1;
    }
    printf("    UNEXPECTED: partial round-trip\n");
    return -1;
}

static int test_lower_half_roundtrip(void)
{
    /* Map the broken-read set: write known patterns to 0x04..0x1C and
     * read them back. These are all in the case statement and all
     * conditionally written (only on wr_en && wr_addr matching), same
     * structural class as the broken set. If they round-trip cleanly
     * the broken set is narrowly { 0x38, 0x3C, 0x44, 0x4C }. If not,
     * the broken set is wider and the synth bug is more global. */
    static const struct { uint32_t off; const char *name; } LOWER[] = {
        {T1_REG_INSTRUCTION, "INSTRUCTION"},
        {T1_REG_RS1_DATA,    "RS1_DATA"},
        {T1_REG_RS2_DATA,    "RS2_DATA"},
        {T1_REG_VTYPE,       "VTYPE"},
        {T1_REG_VL,          "VL"},
        {T1_REG_VSTART,      "VSTART"},
        {T1_REG_VCSR,        "VCSR"},
    };
    static const uint32_t PATTERNS[] = {
        0xDEADBEEFu, 0xCAFEBABEu, 0x5A5A5A5Au, 0xA5A5A5A5u,
        0x12345678u, 0x87654321u, 0xFFFFFFFFu,
    };

    printf("[5] Lower-half register round-trip (map broken-read set)\n");
    int failed = 0;
    for (size_t i = 0; i < sizeof(LOWER)/sizeof(LOWER[0]); i++) {
        wr(LOWER[i].off, PATTERNS[i]);
        uint32_t got = rd(LOWER[i].off);
        const char *verdict = (got == PATTERNS[i]) ? "PASS"
                            : (got == 0u)         ? "FAIL (stuck 0)"
                            :                       "FAIL (other)";
        printf("    [0x%02x] %-12s wrote 0x%08x  read 0x%08x  %s\n",
               LOWER[i].off, LOWER[i].name, PATTERNS[i], got, verdict);
        if (got != PATTERNS[i]) failed++;
    }
    if (failed == 0) {
        printf("    PASS: all lower-half regs round-trip -> broken-read "
               "set is narrowly {0x38,0x3C,0x44,0x4C}\n");
        return 0;
    }
    printf("    %d/%zu lower-half regs failed -> broken-read set is "
           "wider than expected\n", failed,
           sizeof(LOWER)/sizeof(LOWER[0]));
    return -1;
}

static int test_mem_count_dec_via_irq_status(void)
{
    /* This is the clean differentiator between "writes dropped" and
     * "reads broken" for the upper-half register block.
     *
     * irq_pending in the wrapper is { mem_count != 0, !csr_fifo_empty,
     * !rd_fifo_empty }. IRQ_STATUS at 0x40 is a known-good read (it
     * was returning 0x4 in the initial snapshot, contradicting the
     * 0x38 MEM_COUNT readback of 0 - so the underlying register is
     * non-zero, but its readback is stuck).
     *
     * Writing 1 to MEM_COUNT (0x38) decrements mem_count by 1. If
     * writes to 0x38 actually stick, mem_count will eventually drop
     * to 0 and IRQ_STATUS bit[2] will clear. That proves writes work
     * and isolates the bug to the read mux for a specific address
     * subset. If IRQ_STATUS bit[2] never clears no matter how many
     * decrements we do, writes are also dropped.
     *
     * Bound the loop at 256 iterations; mem_count is only 8 bits in
     * the wrapper. */
    uint32_t status_before = rd(T1_REG_IRQ_STATUS);
    int mem_pending_before = (status_before & 0x4u) != 0;

    printf("[4] MEM_COUNT decrement via IRQ_STATUS (write-stickiness)\n");
    printf("    IRQ_STATUS before : 0x%08x  (bit[2]=mem_count!=0 -> %d)\n",
           status_before, mem_pending_before);

    if (!mem_pending_before) {
        printf("    SKIP: mem_count is already 0; can't decrement to "
               "test write stickiness\n");
        return 0;
    }

    int decrements = 0;
    for (int i = 0; i < 256; i++) {
        wr(T1_REG_MEM_COUNT, 1u);
        decrements++;
        uint32_t status = rd(T1_REG_IRQ_STATUS);
        if ((status & 0x4u) == 0) {
            printf("    PASS: IRQ_STATUS bit[2] cleared after %d "
                   "decrement writes -> writes to 0x38 stick "
                   "(IRQ_STATUS now 0x%08x)\n", decrements, status);
            printf("    => Bug is in the READ MUX for 0x38/0x3C/0x44/"
                   "0x4C, not in the AXI write path. § 6.1.2's "
                   "smartconnect rebuild hypothesis is wrong; fix is "
                   "in t1_axi_lite_wrapper.sv read FSM.\n");
            return 0;
        }
    }
    uint32_t status_after = rd(T1_REG_IRQ_STATUS);
    printf("    FAIL: IRQ_STATUS bit[2] still set after 256 "
           "decrement writes (now 0x%08x) -> writes to 0x38 are "
           "dropped\n", status_after);
    printf("    => Writes to upper-half are also broken; § 6.1.2's "
           "smartconnect hypothesis stays alive.\n");
    return -1;
}

static int test_irq_en_writeback(void)
{
    wr(T1_REG_IRQ_EN, 0u);
    uint32_t cleared = rd(T1_REG_IRQ_EN);
    wr(T1_REG_IRQ_EN, T1_IRQ_MEM);
    uint32_t enabled = rd(T1_REG_IRQ_EN);

    printf("[3] IRQ_EN write/readback\n");
    printf("    IRQ_EN after write 0x0          : 0x%08x\n", cleared);
    printf("    IRQ_EN after write T1_IRQ_MEM   : 0x%08x\n", enabled);
    if (enabled == T1_IRQ_MEM) {
        printf("    PASS: readback matches written value\n");
        return 0;
    }
    if (enabled == 0u) {
        printf("    EXPECTED: readback 0 - matches the documented "
               "symptom (read mux for 0x3C broken)\n");
        printf("    To complete this test, run test/vert_lsu next: if "
               "the LSU IRQ fires, the WRITE stuck despite readback=0; "
               "if it hangs, IRQ_EN really is gated.\n");
        return 0;
    }
    printf("    UNEXPECTED: readback 0x%08x is neither 0 nor "
           "T1_IRQ_MEM\n", enabled);
    return -1;
}

/* Probe a Xilinx AXI DMA (also on smartconnect_ctrl/M01) to see if
 * the same araddr[3:2] read pattern shows up there. If yes, the bug
 * is in smartconnect_ctrl (affects all M-side reads). If no, the bug
 * is wrapper-side (specific to t1_axi_lite_wrapper.sv). */
static int test_dma_address_pattern(const char *dma_uio_path)
{
    int fd = open(dma_uio_path, O_RDWR | O_SYNC);
    if (fd < 0) {
        printf("[6] DMA cross-check at %s: open failed: %s - skipping\n",
               dma_uio_path, strerror(errno));
        return 0;
    }
    void *map = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        printf("[6] DMA cross-check: mmap failed: %s - skipping\n",
               strerror(errno));
        close(fd);
        return 0;
    }
    volatile uint32_t *dma = (volatile uint32_t *)map;

    /* DMA_REG_MM2S_CR @ 0x00 - araddr[3:2]=00. After reset its default
     *   bit pattern is non-zero (ALL transfers reset -> IDLE-ish).
     * DMA_REG_MM2S_SR @ 0x04 - araddr[3:2]=01. After reset it has
     *   HALTED + IDLE bits set (= 0x00010002 typical).
     * DMA_REG_S2MM_CR @ 0x30 - araddr[3:2]=00.
     * DMA_REG_S2MM_SR @ 0x34 - araddr[3:2]=01.
     *
     * We write a soft reset (DMA_CR_RESET=4) to MM2S_CR to ensure the
     * DMA is in a known state, then sample SR. SR is a status reg that
     * the DMA itself populates - reading 0 means either DMA hung or
     * the read is broken. After reset, MM2S_SR HALTED (bit 0) is
     * always set, so a non-zero MM2S_SR is the proof. */
    dma[DMA_REG_MM2S_CR / 4] = DMA_CR_RESET;
    /* Wait for reset to deassert (CR.RESET bit 2 self-clears). */
    for (int i = 0; i < 1000 && (dma[DMA_REG_MM2S_CR / 4] & DMA_CR_RESET); i++) {
        for (volatile unsigned j = 0; j < 1000u; j++) {}
    }
    uint32_t mm2s_cr = dma[DMA_REG_MM2S_CR / 4];   /* offset 0x00 - [3:2]=00 */
    uint32_t mm2s_sr = dma[DMA_REG_MM2S_SR / 4];   /* offset 0x04 - [3:2]=01 */
    uint32_t s2mm_cr = dma[DMA_REG_S2MM_CR / 4];   /* offset 0x30 - [3:2]=00 */
    uint32_t s2mm_sr = dma[DMA_REG_S2MM_SR / 4];   /* offset 0x34 - [3:2]=01 */

    printf("[6] DMA cross-check at %s\n", dma_uio_path);
    printf("    DMA MM2S_CR (0x00, [3:2]=00) = 0x%08x\n", mm2s_cr);
    printf("    DMA MM2S_SR (0x04, [3:2]=01) = 0x%08x\n", mm2s_sr);
    printf("    DMA S2MM_CR (0x30, [3:2]=00) = 0x%08x\n", s2mm_cr);
    printf("    DMA S2MM_SR (0x34, [3:2]=01) = 0x%08x\n", s2mm_sr);

    int sr_zero = (mm2s_sr == 0u) && (s2mm_sr == 0u);
    int sr_works = (mm2s_sr != 0u) || (s2mm_sr != 0u);

    if (sr_zero) {
        printf("    DIAG: both SR offsets read 0 - same araddr[3:2]!=00 "
               "symptom as on the T1 wrapper. Bug likely lives in "
               "smartconnect_ctrl, not the wrapper.\n");
        printf("    => Suggested fix: rebuild with smartconnect_ctrl "
               "NUM_CLKS=1 + STRATEGY=AUTOMATIC (§ 6.1.2 BD edit, but "
               "for the right reason this time).\n");
    } else if (sr_works) {
        printf("    DIAG: at least one SR offset returns non-zero - DMA "
               "reads work for araddr[3:2]!=00. Bug is wrapper-specific, "
               "not smartconnect-wide. Fix is in "
               "t1_axi_lite_wrapper.sv read FSM.\n");
    }

    munmap(map, MAP_SIZE);
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    const char *uio_path = (argc >= 2) ? argv[1] : "/dev/uio4";
    const char *dma_uio_path = (argc >= 3) ? argv[2] : "/dev/uio5";

    int fd = open(uio_path, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "open(%s): %s\n", uio_path, strerror(errno));
        return 1;
    }

    void *map = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap %s: %s\n", uio_path, strerror(errno));
        close(fd);
        return 1;
    }
    g_regs = (volatile uint32_t *)map;

    printf("triage_t1: probing T1 wrapper at %s\n\n", uio_path);

    dump_all_regs("initial register snapshot");
    printf("\n");
    int rc0 = test_perf_cycles_advance();
    printf("\n");
    int rc1 = test_ctrl_w1s();
    printf("\n");
    int rc2 = test_perf_tag_roundtrip();
    printf("\n");
    int rc2b = test_vertical_mode_roundtrip();
    printf("\n");
    int rc4 = test_mem_count_dec_via_irq_status();
    printf("\n");
    int rc5 = test_lower_half_roundtrip();
    printf("\n");
    int rc3 = test_irq_en_writeback();
    printf("\n");

    dump_all_regs("final register snapshot");
    printf("\n");

    munmap(map, MAP_SIZE);
    close(fd);

    test_dma_address_pattern(dma_uio_path);
    printf("\n");

    int fails = (rc0 < 0) + (rc1 < 0) + (rc2 < 0) + (rc2b < 0)
                + (rc4 < 0) + (rc5 < 0) + (rc3 < 0);
    if (fails == 0) {
        printf("triage_t1: all probes returned PASS/EXPECTED\n");
        return 0;
    }
    printf("triage_t1: %d probe(s) failed\n", fails);
    return 1;
}
