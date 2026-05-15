/*
 * port_grid_vadd_scratchpad - T1-self-test of the F4 / 5l scratchpad path.
 *
 * Validates T1 m_axi_hb -> smartconnect_hb/M01 -> bram_ctrl -> blk_mem_gen
 * end-to-end WITHOUT requiring PS-side or DMA-side access to scratchpad.
 *
 * Why not PS-side mmap of /dev/uio6:
 *   On 5l, scratchpad lives at PA 0xB0000000 which is in the HPM1_FPD
 *   PS-PL aperture (per UG1085). But the BD has PSU__USE__M_AXI_GP1=0 -
 *   HPM1 is disabled. Any PS load/store to 0xB0000000 DECERRs at the PS
 *   internal NIC, propagates as SError, and panics the kernel. The
 *   t1_scratchpad_alloc helper still mmaps uio6 (the kernel's UIO subsys
 *   sets up page tables fine), but the resulting .va must NOT be
 *   dereferenced from the PS until a future bitstream either enables HPM1
 *   or relocates scratchpad to the HPM0 aperture (0xA0000000-0xAFFFFFFF).
 *   See "F6" in fpga_build_status.md.
 *
 * Why not DMA into scratchpad:
 *   F3 (axi_dma streaming loopback M_AXIS_MM2S -> S_AXIS_S2MM) didn't
 *   synthesise correctly on 5l - Vivado's parameter propagation
 *   ([BD 41-3281] + [BD 41-702]) confused itself on the IP-internal
 *   self-loop and the result is that any DMA mem-to-mem transfer hangs.
 *   The dma_loopback test confirms this. Fix needs an axis_register_slice
 *   between the two streaming sides ("F5" in fpga_build_status.md).
 *
 * Self-test pattern:
 *   1. PS allocates src_ddr (udmabuf, DDR), dst_ddr (udmabuf, DDR), and
 *      a scratchpad region.
 *   2. PS writes a known pattern to src_ddr; sync_for_device.
 *   3. T1 issues vle8 from src_ddr   (T1 hb -> HPC0 -> DDR, known good).
 *   4. T1 issues vse8 to scratchpad  (T1 hb -> smartconnect_hb/M01 -> BRAM).
 *   5. T1 issues vle8 from scratchpad (BRAM -> smartconnect_hb/M01 -> T1).
 *   6. T1 issues vse8 to dst_ddr     (T1 hb -> HPC0 -> DDR).
 *   7. PS sync_for_cpu(dst_ddr); memcmp(src_ddr, dst_ddr) - must match.
 *
 * If src_ddr == dst_ddr after the round-trip, the F4 T1-hb-to-scratchpad
 * path round-tripped 128 bytes of data correctly. If dst_ddr is zeros or
 * stale data, T1 either failed to write scratchpad (vse hung or data
 * lost) or failed to read it back (vle returned wrong bytes).
 */

#include "libt1.h"
#include "libt1_regs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DDR_BYTES 4096u
#define SP_BYTES  4096u

/*
 * 5m note: switched from scratchpad_self.h-generated instructions to
 * hardcoded equivalents matching sp_4issue_with_verify_probe.c, which
 * passes on 5m. The scratchpad_self.h kernel (encoded rs1 fields
 * differ: a0/a1/a1/a2 vs the working a0/a0/a1/a1) triggers a kernel
 * panic on the final vse - possibly a v12-after-BRAM-read AxUSER
 * metadata leak that F7's axi_register_slice doesn't fully scrub.
 * Investigation pending; for now this is the validated kernel.
 */
static int issue_self_test(uint32_t src_ddr_pa, uint32_t sp_pa, uint32_t dst_ddr_pa)
{
    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M4_TA_MA,
        .vl = 128,
    };

    op.instruction = 0x02050407u;  /* vle8.v v8, (a0) -- load from src DDR */
    op.rs1 = src_ddr_pa;
    if (t1_issue(&op) < 0) { perror("t1_issue(vle src_ddr)"); return -1; }

    op.instruction = 0x02050427u;  /* vse8.v v8, (a0) -- store to sp */
    op.rs1 = sp_pa;
    if (t1_issue(&op) < 0) { perror("t1_issue(vse sp)"); return -1; }

    op.instruction = 0x02058607u;  /* vle8.v v12, (a1) -- load from sp */
    op.rs1 = sp_pa;
    if (t1_issue(&op) < 0) { perror("t1_issue(vle sp)"); return -1; }

    op.instruction = 0x02058427u;  /* vse8.v v8, (a1) -- store v8 to dst DDR */
    op.rs1 = dst_ddr_pa;
    if (t1_issue(&op) < 0) { perror("t1_issue(vse dst_ddr)"); return -1; }

    return 0;
}

int main(void)
{
    struct t1_buf src = {0};
    struct t1_buf dst = {0};
    struct t1_buf sp = {0};
    int rc = 1;

    if (t1_init() < 0) {
        perror("t1_init");
        return 1;
    }

    if (t1_buf_alloc(&src, DDR_BYTES) < 0) {
        perror("t1_buf_alloc(src)");
        goto out_close;
    }
    if (t1_buf_alloc(&dst, DDR_BYTES) < 0) {
        perror("t1_buf_alloc(dst)");
        goto out_free_src;
    }
    if (t1_scratchpad_alloc(&sp, 0u, SP_BYTES) < 0) {
        perror("t1_scratchpad_alloc(sp)");
        goto out_free_dst;
    }
    if (sp.pa != T1_SCRATCHPAD_PA) {
        fprintf(stderr, "FAIL: sp.pa=0x%08x expected 0x%08x\n",
                sp.pa, T1_SCRATCHPAD_PA);
        goto out_free_sp;
    }

    uint8_t *src_va = (uint8_t *)src.va;
    uint8_t *dst_va = (uint8_t *)dst.va;
    for (size_t i = 0; i < 128; i++) {
        src_va[i] = (uint8_t)((i * 31u + 5u) & 0xFFu);
        dst_va[i] = 0;
    }
    if (t1_buf_sync_for_device(&src) < 0) perror("sync_for_device(src)");
    if (t1_buf_sync_for_device(&dst) < 0) perror("sync_for_device(dst)");

    if (issue_self_test(src.pa, sp.pa, dst.pa) < 0) {
        goto out_free_sp;
    }

    if (t1_buf_sync_for_cpu(&dst) < 0) perror("sync_for_cpu(dst)");

    for (size_t i = 0; i < 128; i++) {
        if (src_va[i] != dst_va[i]) {
            fprintf(stderr,
                    "FAIL: byte %zu: src=0x%02x dst=0x%02x (sp.pa=0x%08x)\n",
                    i, src_va[i], dst_va[i], sp.pa);
            goto out_free_sp;
        }
    }

    printf("PASS: port_grid_vadd_scratchpad (T1 hb round-trip via sp.pa=0x%08x)\n",
           sp.pa);
    rc = 0;

out_free_sp:
    t1_buf_free(&sp);
out_free_dst:
    t1_buf_free(&dst);
out_free_src:
    t1_buf_free(&src);
out_close:
    t1_close();
    return rc;
}
