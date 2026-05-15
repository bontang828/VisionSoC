/*
 * sp_ddr_to_sp_probe - vle from DDR, vse to scratchpad. Two issues only.
 *
 * After alloc/load/store probes all pass in isolation but the full
 * port_grid_vadd_scratchpad test hangs the kernel, this isolates the
 * minimal "DDR-then-scratchpad" transition.
 *
 * Sequence:
 *   1. vle8.v v8, (src_ddr_pa)   - same as ddr_roundtrip's first half
 *   2. vse8.v v8, (sp_pa)         - write what we just loaded into BRAM
 *
 * If this hangs, the bug is in the smartconnect_hb arbiter's handling
 * of consecutive transactions that switch between M00 (HPC0) and M01
 * (BRAM). If it passes, the bug must involve a third or fourth issue
 * (or read-after-write on the same scratchpad address).
 */

#include "libt1.h"
#include "libt1_regs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct t1_buf src = {0};
    struct t1_buf sp = {0};
    int rc = 1;

    if (t1_init() < 0) {
        perror("t1_init");
        return 1;
    }
    if (t1_buf_alloc(&src, 4096u) < 0) {
        perror("t1_buf_alloc(src)");
        goto out_close;
    }
    if (t1_scratchpad_alloc(&sp, 0u, 4096u) < 0) {
        perror("t1_scratchpad_alloc(sp)");
        goto out_free_src;
    }

    uint8_t *src_va = (uint8_t *)src.va;
    for (size_t i = 0; i < 128; i++) {
        src_va[i] = (uint8_t)((i * 31u + 5u) & 0xFFu);
    }
    if (t1_buf_sync_for_device(&src) < 0) perror("sync_for_device(src)");

    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M4_TA_MA,
        .vl = 128,
    };

    /* vle8.v v8, (a0) */
    op.instruction = 0x02050407u;
    op.rs1 = src.pa;
    if (t1_issue(&op) < 0) {
        perror("t1_issue(vle8 from DDR)");
        goto out_free_sp;
    }

    /* vse8.v v8, (a0) */
    op.instruction = 0x02050427u;
    op.rs1 = sp.pa;
    if (t1_issue(&op) < 0) {
        perror("t1_issue(vse8 to sp)");
        goto out_free_sp;
    }

    printf("PASS: sp_ddr_to_sp_probe (vle from 0x%08x, vse to 0x%08x)\n",
           src.pa, sp.pa);
    rc = 0;

out_free_sp:
    t1_buf_free(&sp);
out_free_src:
    t1_buf_free(&src);
out_close:
    t1_close();
    return rc;
}
