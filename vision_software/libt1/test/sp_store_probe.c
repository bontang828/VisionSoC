/*
 * sp_store_probe - issue one vse8 to scratchpad and exit.
 *
 * After sp_alloc_probe + sp_load_probe both pass, this isolates the
 * T1-hb -> smartconnect_hb/M01 -> bram_ctrl WRITE path. If this crashes
 * the kernel (where load + alloc don't), the write path is the
 * bug - likely an AXI-side issue (BRESP propagation, write-channel
 * arbitration) rather than read-channel routing.
 *
 * The vse stores whatever happens to be in v8 - we don't care about
 * the data, only whether the issue completes.
 *
 * Expected behaviour on a healthy bitstream:
 *   prints "PASS: sp_store_probe ..." and exits 0.
 */

#include "libt1.h"
#include "libt1_regs.h"

#include <stdio.h>

int main(void)
{
    struct t1_buf sp = {0};

    if (t1_init() < 0) {
        perror("t1_init");
        return 1;
    }

    if (t1_scratchpad_alloc(&sp, 0u, 4096u) < 0) {
        perror("t1_scratchpad_alloc");
        t1_close();
        return 1;
    }

    /* vse8.v v8, (a0) - same encoding port_grid_vadd uses. */
    struct t1_op op = {
        .instruction = 0x02050427u,
        .rs1 = sp.pa,
        .vtype = T1_VTYPE_E8_M4_TA_MA,
        .vl = 128,
    };

    if (t1_issue(&op) < 0) {
        perror("t1_issue(vse8 to sp)");
        t1_buf_free(&sp);
        t1_close();
        return 1;
    }

    printf("PASS: sp_store_probe (issued vse8 v8, (0x%08x))\n", sp.pa);
    t1_buf_free(&sp);
    t1_close();
    return 0;
}
