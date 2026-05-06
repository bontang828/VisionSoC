#include "libt1.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

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

    init_pattern((uint8_t *)src.va, src.size);
    memset(dst.va, 0, dst.size);
    (void)msync(src.va, src.size, MS_SYNC);
    (void)msync(dst.va, dst.size, MS_SYNC);

    /*
     * Start S2MM first so the stream sink is ready before MM2S produces.
     * This assumes the FPGA block design wires MM2S stream to S2MM stream
     * for loopback or through a compatible stream path.
     */
    if (t1_dma_s2mm_async(0, dst.pa, DMA_BYTES) < 0) {
        perror("t1_dma_s2mm_async");
        goto out_free_dst;
    }
    if (t1_dma_mm2s_async(src.pa, 0, DMA_BYTES) < 0) {
        perror("t1_dma_mm2s_async");
        goto out_free_dst;
    }
    if (t1_dma_wait() < 0) {
        perror("t1_dma_wait");
        goto out_free_dst;
    }

    (void)msync(dst.va, dst.size, MS_INVALIDATE);
    if (memcmp(src.va, dst.va, DMA_BYTES) != 0) {
        fprintf(stderr, "FAIL: DMA loopback mismatch\n");
        goto out_free_dst;
    }

    printf("PASS: DMA loopback\n");
    rc = 0;

out_free_dst:
    t1_buf_free(&dst);
out_free_src:
    t1_buf_free(&src);
out_close:
    t1_close();
    return rc;
}
