#include "libt1.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define DEFAULT_DMA_BYTES (16u * 1024u)

static void init_pattern(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)((i * 31u + 5u) & 0xFFu);
    }
}

int main(int argc, char **argv)
{
    struct t1_buf src = {0};
    struct t1_buf dst = {0};
    int rc = 1;
    size_t dma_bytes = DEFAULT_DMA_BYTES;
    if (argc >= 2) {
        char *end = NULL;
        unsigned long v = strtoul(argv[1], &end, 0);
        if (end == argv[1] || v == 0u) {
            fprintf(stderr, "usage: %s [bytes]\n", argv[0]);
            return 1;
        }
        dma_bytes = (size_t)v;
    }
    printf("dma_loopback: bytes=%zu\n", dma_bytes);

    if (t1_init() < 0) {
        perror("t1_init");
        return 1;
    }
    if (t1_buf_alloc(&src, dma_bytes) < 0) {
        perror("t1_buf_alloc(src)");
        goto out_close;
    }
    if (t1_buf_alloc(&dst, dma_bytes) < 0) {
        perror("t1_buf_alloc(dst)");
        goto out_free_src;
    }

    init_pattern((uint8_t *)src.va, src.size);
    memset(dst.va, 0, dst.size);
    if (t1_buf_sync_for_device(&src) < 0) perror("sync_for_device(src)");
    if (t1_buf_sync_for_device(&dst) < 0) perror("sync_for_device(dst)");

    /*
     * Start S2MM first so the stream sink is ready before MM2S produces.
     * This assumes the FPGA block design wires MM2S stream to S2MM stream
     * for loopback or through a compatible stream path.
     */
    if (t1_dma_s2mm_async(0, dst.pa, dma_bytes) < 0) {
        perror("t1_dma_s2mm_async");
        goto out_free_dst;
    }
    if (t1_dma_mm2s_async(src.pa, 0, dma_bytes) < 0) {
        perror("t1_dma_mm2s_async");
        goto out_free_dst;
    }
    if (t1_dma_wait() < 0) {
        perror("t1_dma_wait");
        goto out_free_dst;
    }

    if (t1_buf_sync_for_cpu(&dst) < 0) perror("sync_for_cpu(dst)");
    if (memcmp(src.va, dst.va, dma_bytes) != 0) {
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
