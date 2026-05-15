/*
 * sp_load_probe - issue one vle8 from scratchpad and exit.
 *
 * Builds on sp_alloc_probe by additionally issuing a single
 * vle8.v v8, (sp.pa). If alloc-probe is OK but this crashes the kernel,
 * T1's m_axi_hb -> smartconnect_hb/M01 -> bram_ctrl read path is
 * propagating a fault back to the PS somehow.
 *
 * Note: scratchpad is uninitialised on first run after a bitstream
 * load, so v8 will hold whatever BRAM defaults to (typically zero).
 * We don't read v8 contents - we only care that the issue completes.
 *
 * Expected behaviour on a healthy bitstream:
 *   prints "PASS: sp_load_probe ..." and exits 0.
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

    /* vle8.v v8, (a0) - same encoding port_grid_vadd uses. */
    struct t1_op op = {
        .instruction = 0x02050407u,
        .rs1 = sp.pa,
        .vtype = T1_VTYPE_E8_M4_TA_MA,
        .vl = 128,
    };

    if (t1_issue(&op) < 0) {
        perror("t1_issue(vle8 from sp)");
        t1_buf_free(&sp);
        t1_close();
        return 1;
    }

    printf("PASS: sp_load_probe (issued vle8 v8, (0x%08x))\n", sp.pa);
    t1_buf_free(&sp);
    t1_close();
    return 0;
}
