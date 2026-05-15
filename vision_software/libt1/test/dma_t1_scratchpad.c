/*
 * dma_t1_scratchpad - full F4 bandwidth-amplifier flow:
 *   PS writes src DDR
 *   DMA: src DDR -> sp_in (scratchpad)
 *   T1 : vle from sp_in, vse to sp_out (T1 hb -> BRAM both directions)
 *   DMA: sp_out (scratchpad) -> dst DDR
 *   PS verifies dst DDR matches src DDR
 *
 * This is the closest thing to an end-to-end exercise of the F4 design
 * intent: DMA loads scratchpad, T1 reads/writes scratchpad at high
 * bandwidth, DMA exfiltrates results. Validates F4 + F5 + the T1
 * hb->BRAM round-trip in one test.
 *
 * Buffer layout in scratchpad (32 KB total, 16 KB used):
 *   sp_in  at offset 0x0000 (4 KB) - DMA writes, T1 reads
 *   sp_out at offset 0x1000 (4 KB) - T1 writes, DMA reads
 *
 * Note: T1 only touches the first 128 bytes per vreg (vl=128 SEW=8).
 * The 4 KB buffer size is just for page alignment + headroom.
 *
 * This test does NOT require F6 (PS->BRAM direct access) - PS only
 * touches DDR-backed udmabuf throughout.
 */

#include "libt1.h"
#include "libt1_regs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DDR_BYTES 4096u
#define SP_BUF_BYTES 4096u

int main(void)
{
    struct t1_buf src = {0};
    struct t1_buf dst = {0};
    struct t1_buf sp_in = {0};
    struct t1_buf sp_out = {0};
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
    if (t1_scratchpad_alloc(&sp_in, 0u, SP_BUF_BYTES) < 0) {
        perror("t1_scratchpad_alloc(sp_in)");
        goto out_free_dst;
    }
    if (t1_scratchpad_alloc(&sp_out, SP_BUF_BYTES, SP_BUF_BYTES) < 0) {
        perror("t1_scratchpad_alloc(sp_out)");
        goto out_free_sp_in;
    }

    uint8_t *src_va = (uint8_t *)src.va;
    uint8_t *dst_va = (uint8_t *)dst.va;
    for (size_t i = 0; i < 128; i++) {
        src_va[i] = (uint8_t)((i * 31u + 5u) & 0xFFu);
        dst_va[i] = 0;
    }
    if (t1_buf_sync_for_device(&src) < 0) perror("sync_for_device(src)");
    if (t1_buf_sync_for_device(&dst) < 0) perror("sync_for_device(dst)");

    /* Stage 1: DMA src DDR -> sp_in. Only the first 128 bytes need to
     * be valid (T1 reads vl=128 SEW=8), but DMA the whole 4 KB chunk
     * for simplicity. */
    if (t1_dma_s2mm_async(0, sp_in.pa, SP_BUF_BYTES) < 0) {
        perror("dma_s2mm src->sp_in"); goto out_free_sp_out;
    }
    if (t1_dma_mm2s_async(src.pa, 0, SP_BUF_BYTES) < 0) {
        perror("dma_mm2s src->sp_in"); goto out_free_sp_out;
    }
    if (t1_dma_wait() < 0) { perror("dma_wait src->sp_in"); goto out_free_sp_out; }

    /* Stage 2: T1 reads sp_in into v8, writes v8 to sp_out. */
    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M4_TA_MA,
        .vl = 128,
    };
    op.instruction = 0x02050407u;  /* vle8.v v8, (a0) */
    op.rs1 = sp_in.pa;
    if (t1_issue(&op) < 0) { perror("vle from sp_in"); goto out_free_sp_out; }

    op.instruction = 0x02050427u;  /* vse8.v v8, (a0) */
    op.rs1 = sp_out.pa;
    if (t1_issue(&op) < 0) { perror("vse to sp_out"); goto out_free_sp_out; }

    /* Stage 3: DMA sp_out -> dst DDR. */
    if (t1_dma_s2mm_async(0, dst.pa, SP_BUF_BYTES) < 0) {
        perror("dma_s2mm sp_out->dst"); goto out_free_sp_out;
    }
    if (t1_dma_mm2s_async(sp_out.pa, 0, SP_BUF_BYTES) < 0) {
        perror("dma_mm2s sp_out->dst"); goto out_free_sp_out;
    }
    if (t1_dma_wait() < 0) { perror("dma_wait sp_out->dst"); goto out_free_sp_out; }

    if (t1_buf_sync_for_cpu(&dst) < 0) perror("sync_for_cpu(dst)");

    /* T1 only touched the first 128 bytes of sp_in/sp_out. Bytes 128+
     * of dst contain whatever DMA copied from sp_out's uninitialised
     * region - we only verify the T1-touched range. */
    for (size_t i = 0; i < 128; i++) {
        if (src_va[i] != dst_va[i]) {
            fprintf(stderr,
                    "FAIL: byte %zu: src=0x%02x dst=0x%02x (sp_in=0x%08x sp_out=0x%08x)\n",
                    i, src_va[i], dst_va[i], sp_in.pa, sp_out.pa);
            goto out_free_sp_out;
        }
    }

    printf("PASS: dma_t1_scratchpad (sp_in=0x%08x sp_out=0x%08x)\n",
           sp_in.pa, sp_out.pa);
    rc = 0;

out_free_sp_out:
    t1_buf_free(&sp_out);
out_free_sp_in:
    t1_buf_free(&sp_in);
out_free_dst:
    t1_buf_free(&dst);
out_free_src:
    t1_buf_free(&src);
out_close:
    t1_close();
    return rc;
}
