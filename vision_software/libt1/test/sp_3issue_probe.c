/*
 * sp_3issue_probe - vle DDR, vse sp, vle sp. Three issues, read-after-write
 * to scratchpad.
 *
 * After 1- and 2-issue probes pass but the full 4-issue test hangs the
 * kernel, this isolates issue #3 (vle from sp after vse to same sp).
 * Read-after-write to BRAM should be safe (axi_bram_ctrl handles
 * ordering), but if T1 issues them faster than BRAM commits, the
 * read might race the write.
 */

#include "libt1.h"
#include "libt1_regs.h"

#include <stdint.h>
#include <stdio.h>

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

    op.instruction = 0x02050407u;  /* vle8.v v8, (a0) */
    op.rs1 = src.pa;
    if (t1_issue(&op) < 0) { perror("vle DDR"); goto out_free_sp; }

    op.instruction = 0x02050427u;  /* vse8.v v8, (a0) */
    op.rs1 = sp.pa;
    if (t1_issue(&op) < 0) { perror("vse sp"); goto out_free_sp; }

    op.instruction = 0x02058607u;  /* vle8.v v12, (a1) */
    op.rs1 = sp.pa;
    if (t1_issue(&op) < 0) { perror("vle sp"); goto out_free_sp; }

    printf("PASS: sp_3issue_probe (vle DDR, vse sp, vle sp)\n");
    rc = 0;

out_free_sp:
    t1_buf_free(&sp);
out_free_src:
    t1_buf_free(&src);
out_close:
    t1_close();
    return rc;
}
