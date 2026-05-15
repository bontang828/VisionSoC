/*
 * sp_4issue_with_verify_probe - sp_4issue_probe + sync_for_cpu + memcmp.
 *
 * Bisects the gap between sp_4issue_probe (PASS) and the full
 * port_grid_vadd_scratchpad test (CRASH). Identical 4-issue T1 chain.
 * Adds: t1_buf_sync_for_cpu(&dst) and a memcmp comparing src and dst.
 *
 * If THIS hangs, the bug is in either:
 *   - sync_for_cpu (udmabuf sysfs write -> dma_sync_single_for_cpu),
 *     which somehow misbehaves when the buffer was last written by T1
 *     after T1 had also accessed BRAM, or
 *   - memcmp / PS read of dst.va after T1 wrote it.
 *
 * If this passes, the bug is in something specific to port_grid_vadd_
 * scratchpad's structure not present here (e.g. the buffer initialisation
 * loop where src is written before any T1 issue).
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
    uint8_t *dst_va = (uint8_t *)dst.va;
    for (size_t i = 0; i < 128; i++) {
        src_va[i] = (uint8_t)((i * 31u + 5u) & 0xFFu);
        dst_va[i] = 0;
    }
    if (t1_buf_sync_for_device(&src) < 0) perror("sync_for_device(src)");
    if (t1_buf_sync_for_device(&dst) < 0) perror("sync_for_device(dst)");

    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M4_TA_MA,
        .vl = 128,
    };

    op.instruction = 0x02050407u;  op.rs1 = src.pa;
    if (t1_issue(&op) < 0) { perror("issue 1"); goto out_free_sp; }
    fprintf(stderr, "after issue 1\n");

    op.instruction = 0x02050427u;  op.rs1 = sp.pa;
    if (t1_issue(&op) < 0) { perror("issue 2"); goto out_free_sp; }
    fprintf(stderr, "after issue 2\n");

    op.instruction = 0x02058607u;  op.rs1 = sp.pa;
    if (t1_issue(&op) < 0) { perror("issue 3"); goto out_free_sp; }
    fprintf(stderr, "after issue 3\n");

    op.instruction = 0x02058427u;  op.rs1 = dst.pa;
    if (t1_issue(&op) < 0) { perror("issue 4"); goto out_free_sp; }
    fprintf(stderr, "after issue 4\n");

    fprintf(stderr, "calling sync_for_cpu(dst)...\n");
    if (t1_buf_sync_for_cpu(&dst) < 0) perror("sync_for_cpu(dst)");
    fprintf(stderr, "sync_for_cpu(dst) returned\n");

    fprintf(stderr, "starting memcmp loop...\n");
    int mismatch = 0;
    for (size_t i = 0; i < 128; i++) {
        if (src_va[i] != dst_va[i]) {
            if (!mismatch) {
                fprintf(stderr, "first mismatch at byte %zu: src=0x%02x dst=0x%02x\n",
                        i, src_va[i], dst_va[i]);
            }
            mismatch++;
        }
    }
    fprintf(stderr, "memcmp loop done, mismatches=%d\n", mismatch);

    if (mismatch == 0) {
        printf("PASS: sp_4issue_with_verify_probe (T1 round-trip via sp.pa=0x%08x)\n",
               sp.pa);
        rc = 0;
    } else {
        fprintf(stderr, "FAIL: %d mismatched bytes (T1 hb -> sp -> DDR readback corrupted)\n",
                mismatch);
    }

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
