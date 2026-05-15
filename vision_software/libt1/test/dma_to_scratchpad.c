/*
 * dma_to_scratchpad - DMA-courier round-trip DDR -> scratchpad -> DDR.
 *
 * Validates F4 (DMA can reach scratchpad via smartconnect_hb/M01) +
 * F5 (axis_register_slice fixes the streaming loopback) without ever
 * requiring PS-side direct access to scratchpad. PS only ever touches
 * udmabuf-backed DDR buffers; scratchpad is the courier in the middle.
 *
 * Path under test:
 *   1. PS writes pattern to src DDR (udmabuf), sync_for_device
 *   2. DMA: src.pa (DDR) -> sp.pa (scratchpad)
 *      MM2S reads DDR -> M_AXIS_MM2S -> axis_reg_slice -> S_AXIS_S2MM ->
 *      M_AXI_S2MM writes BRAM
 *   3. DMA: sp.pa (scratchpad) -> dst.pa (DDR)
 *      MM2S reads BRAM -> stream loopback -> S2MM writes DDR
 *   4. sync_for_cpu(dst), memcmp(src, dst) - must match
 *
 * If src == dst after the round-trip, both DMA-to-BRAM and DMA-from-BRAM
 * paths work end-to-end, which means F4 and F5 are both functional.
 *
 * F6 (PS direct mmap of scratchpad) is NOT needed here - every PS access
 * is to udmabuf-backed DDR, which has worked since 5h.
 */

#include "libt1.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DMA_BYTES (16u * 1024u)

static void init_pattern(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)((i * 31u + 5u) & 0xFFu);
    }
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
    if (t1_buf_alloc(&src, DMA_BYTES) < 0) {
        perror("t1_buf_alloc(src)");
        goto out_close;
    }
    if (t1_buf_alloc(&dst, DMA_BYTES) < 0) {
        perror("t1_buf_alloc(dst)");
        goto out_free_src;
    }
    if (t1_scratchpad_alloc(&sp, 0u, DMA_BYTES) < 0) {
        perror("t1_scratchpad_alloc(sp)");
        goto out_free_dst;
    }
    if (sp.pa != T1_SCRATCHPAD_PA) {
        fprintf(stderr, "FAIL: sp.pa=0x%08x expected 0x%08x\n",
                sp.pa, T1_SCRATCHPAD_PA);
        goto out_free_sp;
    }

    init_pattern((uint8_t *)src.va, src.size);
    memset(dst.va, 0, dst.size);
    if (t1_buf_sync_for_device(&src) < 0) perror("sync_for_device(src)");
    if (t1_buf_sync_for_device(&dst) < 0) perror("sync_for_device(dst)");

    /* Round-trip leg 1: DDR -> scratchpad. */
    if (t1_dma_s2mm_async(0, sp.pa, DMA_BYTES) < 0) {
        perror("dma_s2mm DDR->sp");
        goto out_free_sp;
    }
    if (t1_dma_mm2s_async(src.pa, 0, DMA_BYTES) < 0) {
        perror("dma_mm2s DDR->sp");
        goto out_free_sp;
    }
    if (t1_dma_wait() < 0) {
        perror("dma_wait DDR->sp");
        goto out_free_sp;
    }

    /* Round-trip leg 2: scratchpad -> DDR. */
    if (t1_dma_s2mm_async(0, dst.pa, DMA_BYTES) < 0) {
        perror("dma_s2mm sp->DDR");
        goto out_free_sp;
    }
    if (t1_dma_mm2s_async(sp.pa, 0, DMA_BYTES) < 0) {
        perror("dma_mm2s sp->DDR");
        goto out_free_sp;
    }
    if (t1_dma_wait() < 0) {
        perror("dma_wait sp->DDR");
        goto out_free_sp;
    }

    if (t1_buf_sync_for_cpu(&dst) < 0) perror("sync_for_cpu(dst)");

    if (memcmp(src.va, dst.va, DMA_BYTES) != 0) {
        size_t mismatch_first = (size_t)-1;
        size_t mismatch_count = 0;
        const uint8_t *s = (const uint8_t *)src.va;
        const uint8_t *d = (const uint8_t *)dst.va;
        for (size_t i = 0; i < DMA_BYTES; i++) {
            if (s[i] != d[i]) {
                if (mismatch_first == (size_t)-1) mismatch_first = i;
                mismatch_count++;
            }
        }
        fprintf(stderr,
                "FAIL: DMA round-trip mismatch at byte %zu (count=%zu/%u, src=0x%02x dst=0x%02x sp.pa=0x%08x)\n",
                mismatch_first, mismatch_count, DMA_BYTES,
                s[mismatch_first], d[mismatch_first], sp.pa);
        goto out_free_sp;
    }

    printf("PASS: dma_to_scratchpad round-trip (sp.pa=0x%08x len=%u)\n",
           sp.pa, DMA_BYTES);
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
