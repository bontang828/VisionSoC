/*
 * sp_v12_compute_probe - F7-extension diagnostic for the v12-from-BRAM panic.
 *
 * Background (5m results, see fpga_build_status.md § 0):
 *   sp_4issue_with_verify_probe PASSES on 5m: it does vle v8 from DDR,
 *   vse v8 to BRAM, vle v12 from BRAM, then vse v8 to DDR (stores v8,
 *   the DDR-loaded vreg).
 *   port_grid_vadd_scratchpad with the original scratchpad_self.h kernel
 *   PANICS on 5m: vle v8 from DDR, vse v8 to BRAM, vle v12 from BRAM,
 *   then vse v12 to DDR (stores v12, the BRAM-loaded vreg).
 *
 *   So F7's axi_register_slice scrubs metadata on the AXI master interface
 *   itself, but doesn't help when T1 forwards a freshly BRAM-loaded vreg
 *   directly to a DDR store with no intermediate compute.
 *
 * Hypothesis being tested:
 *   Real kernels never do "load-then-store v12 verbatim" - they always
 *   compute something between the load and store. If pushing v12 through
 *   the ALU resets its forward/rename metadata, then F4 + F7 is already
 *   good enough for production kernels even without F7-extension RTL work.
 *
 * Sequence (5 issues):
 *   1. vle8.v  v8,  (a0)         - load v8 from src DDR
 *   2. vse8.v  v8,  (a0)         - store v8 to BRAM (seed scratchpad)
 *   3. vle8.v  v12, (a1)         - load v12 from BRAM (this is the "taint" load)
 *   4. vadd.vv v12, v12, v8      - compute on v12 (= v12 + v8 = 2*src mod 256)
 *   5. vse8.v  v12, (a1)         - store v12 to dst DDR
 *
 * Expected: dst[i] == (2 * src[i]) & 0xFF for all i in [0,128).
 *
 * Result interpretation:
 *   PASS  -> bug only affects no-compute load-then-store patterns. Real
 *            kernels are fine on 5m. F4 is usable; F7-extension is a low
 *            priority. Next bitstream should focus on F5 / F2.
 *   PANIC -> bug is structural. v12 carries BRAM-tainted metadata through
 *            the ALU and out the store. F4 needs F7-extension (RTL work
 *            in T1's vreg writeback path, or stronger AXI scrubbing) to
 *            be production-ready. Plan a third bitstream.
 *   FAIL (mismatched bytes, no panic) -> the compute path is corrupting
 *            data, NOT a metadata bug. Compare against vsub.vv variant.
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

    op.instruction = 0x02050407u;  op.rs1 = src.pa;  op.rs2 = 0;
    if (t1_issue(&op) < 0) { perror("issue 1 (vle v8 from src DDR)"); goto out_free_sp; }
    fprintf(stderr, "after issue 1 (vle v8 <- src DDR)\n");

    op.instruction = 0x02050427u;  op.rs1 = sp.pa;  op.rs2 = 0;
    if (t1_issue(&op) < 0) { perror("issue 2 (vse v8 to sp)"); goto out_free_sp; }
    fprintf(stderr, "after issue 2 (vse v8 -> sp BRAM)\n");

    op.instruction = 0x02058607u;  op.rs1 = sp.pa;  op.rs2 = 0;
    if (t1_issue(&op) < 0) { perror("issue 3 (vle v12 from sp)"); goto out_free_sp; }
    fprintf(stderr, "after issue 3 (vle v12 <- sp BRAM)\n");

    op.instruction = 0x02C40657u;  op.rs1 = 0;  op.rs2 = 0;
    if (t1_issue(&op) < 0) { perror("issue 4 (vadd.vv v12,v12,v8)"); goto out_free_sp; }
    fprintf(stderr, "after issue 4 (vadd.vv v12 = v12 + v8)\n");

    op.instruction = 0x02058627u;  op.rs1 = dst.pa;  op.rs2 = 0;
    if (t1_issue(&op) < 0) { perror("issue 5 (vse v12 to dst DDR)"); goto out_free_sp; }
    fprintf(stderr, "after issue 5 (vse v12 -> dst DDR)\n");

    fprintf(stderr, "calling sync_for_cpu(dst)...\n");
    if (t1_buf_sync_for_cpu(&dst) < 0) perror("sync_for_cpu(dst)");
    fprintf(stderr, "sync_for_cpu(dst) returned\n");

    int mismatch = 0;
    for (size_t i = 0; i < 128; i++) {
        uint8_t expected = (uint8_t)((src_va[i] * 2u) & 0xFFu);
        if (dst_va[i] != expected) {
            if (!mismatch) {
                fprintf(stderr,
                        "first mismatch at byte %zu: src=0x%02x expected=0x%02x dst=0x%02x\n",
                        i, src_va[i], expected, dst_va[i]);
            }
            mismatch++;
        }
    }
    fprintf(stderr, "memcmp loop done, mismatches=%d\n", mismatch);

    if (mismatch == 0) {
        printf("PASS: sp_v12_compute_probe (v12 from BRAM + vadd + store to DDR, sp.pa=0x%08x)\n",
               sp.pa);
        rc = 0;
    } else {
        fprintf(stderr,
                "FAIL: %d mismatched bytes - vadd.vv v12,v12,v8 path corrupted\n",
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
