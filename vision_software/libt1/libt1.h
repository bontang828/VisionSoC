#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * libt1 is a single-threaded userspace UIO driver for the VisionSoC T1
 * wrapper. Callers must serialize all t1_* calls themselves.
 */

struct t1_op {
    uint32_t instruction;
    uint32_t rs1;
    uint32_t rs2;
    uint32_t vtype;
    uint32_t vl;
    uint32_t vstart;
    uint32_t vcsr;
    uint8_t vertical_mode;
};

struct t1_buf {
    void *va;
    uint32_t pa;
    size_t size;
    int _udmabuf_fd;
};

int t1_init(void);
void t1_close(void);

int t1_issue(const struct t1_op *op);
int t1_issue_kernel(const uint32_t *kernel, size_t n_words,
                    const struct t1_op *op_template);

int t1_drain_rd(uint32_t *data, uint8_t *rd_addr, bool *is_fp);
int t1_drain_csr(uint32_t *vxsat, uint32_t *fflag);
int t1_wait_mem(unsigned n_events);

uint32_t t1_perf_start(uint8_t tag);
uint32_t t1_perf_stop(void);
uint64_t t1_cycles(void);

int t1_dma_mm2s_async(uint32_t src_pa, uint32_t dst_pa, uint32_t len);
int t1_dma_s2mm_async(uint32_t src_pa, uint32_t dst_pa, uint32_t len);
int t1_dma_mm2s_sync(uint32_t src_pa, uint32_t dst_pa, uint32_t len);
int t1_dma_s2mm_sync(uint32_t src_pa, uint32_t dst_pa, uint32_t len);
int t1_dma_wait(void);

int t1_buf_alloc(struct t1_buf *buf, size_t size);
void t1_buf_free(struct t1_buf *buf);

uint32_t t1_va_to_pa(const void *va);

void t1_set_vertical_mode_raw(int v);
int t1_get_vertical_mode_raw(void);

#ifdef __cplusplus
}
#endif
