/*
 * sp_4issue_probe - minimal 4-issue version of port_grid_vadd_scratchpad.
 *
 * Identical T1 issue sequence to the full test (vle DDR, vse sp, vle sp,
 * vse DDR), but with NO PS-side verification (no memcmp, no sync_for_cpu)
 * and minimal allocation. If THIS hangs the kernel where sp_3issue_probe
 * passes, the bug is in issue #4 (vse from a vreg that was loaded from
 * BRAM, written to DDR via HPC0).
 *
 * If this passes but the full port_grid_vadd_scratchpad hangs, the bug
 * is in the test scaffolding (sync_for_cpu, memcmp, or some interaction
 * between scratchpad mmap and PS-side udmabuf access).
 */

#include "libt1.h"
#include "libt1_regs.h"

#include <stdint.h>
#include <stdio.h>

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
    if (t1_buf_alloc(&src, 4096u) < 0) {
        perror("t1_buf_alloc(src)");
        goto out_close;
    }
    if (t1_buf_alloc(&dst, 4096u) < 0) {
        perror("t1_buf_alloc(dst)");
        goto out_free_src;
    }
    if (t1_scratchpad_alloc(&sp, 0u, 4096u) < 0) {
        perror("t1_scratchpad_alloc(sp)");
        goto out_free_dst;
    }

    uint8_t *src_va = (uint8_t *)src.va;
    for (size_t i = 0; i < 128; i++) {
        src_va[i] = (uint8_t)((i * 31u + 5u) & 0xFFu);
    }
    if (t1_buf_sync_for_device(&src) < 0) perror("sync_for_device(src)");
    if (t1_buf_sync_for_device(&dst) < 0) perror("sync_for_device(dst)");

    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M4_TA_MA,
        .vl = 128,
    };

    op.instruction = 0x02050407u;  /* vle8.v v8, (a0) */
    op.rs1 = src.pa;
    if (t1_issue(&op) < 0) { perror("issue 1: vle DDR"); goto out_free_sp; }
    fprintf(stderr, "after issue 1 (vle DDR)\n");

    op.instruction = 0x02050427u;  /* vse8.v v8, (a0) */
    op.rs1 = sp.pa;
    if (t1_issue(&op) < 0) { perror("issue 2: vse sp"); goto out_free_sp; }
    fprintf(stderr, "after issue 2 (vse sp)\n");

    op.instruction = 0x02058607u;  /* vle8.v v12, (a1) */
    op.rs1 = sp.pa;
    if (t1_issue(&op) < 0) { perror("issue 3: vle sp"); goto out_free_sp; }
    fprintf(stderr, "after issue 3 (vle sp)\n");

    op.instruction = 0x02058427u;  /* vse8.v v12, (a1) */
    op.rs1 = dst.pa;
    if (t1_issue(&op) < 0) { perror("issue 4: vse DDR"); goto out_free_sp; }
    fprintf(stderr, "after issue 4 (vse DDR)\n");

    printf("PASS: sp_4issue_probe (4 issues completed without hang)\n");
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
