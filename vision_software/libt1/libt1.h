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
    int _udmabuf_idx;  /* used by t1_buf_sync_for_{cpu,device} */
};

int t1_init(void);
void t1_close(void);

int t1_issue(const struct t1_op *op);
int t1_issue_kernel(const uint32_t *kernel, size_t n_words,
                    const struct t1_op *op_template);

int t1_drain_rd(uint32_t *data, uint8_t *rd_addr, bool *is_fp);
int t1_drain_csr(uint32_t *vxsat, uint32_t *fflag);
int t1_wait_rd(uint32_t *data, uint8_t *rd_addr, bool *is_fp, int timeout_ms);
int t1_wait_csr(uint32_t *vxsat, uint32_t *fflag, int timeout_ms);
int t1_wait_mem(unsigned n_events);

/*
 * Block until the T1 core re-asserts ISSUE_READY, meaning the most
 * recently issued instruction has fully drained ALL its writeback paths.
 *
 * This is distinct from t1_issue()'s internal completion check: for a
 * compute op t1_issue() returns at *acceptance* (ISSUE_BUSY clears at
 * issue.fire), while the op keeps draining in the background for
 * thousands of cycles. Per-instruction timing harnesses must call
 * t1_wait_ready() inside their perf bracket so the measured cycle count
 * reflects this op's own latency rather than leaking into the next
 * issue's wait. Returns 0 once ready, -1 with errno=ETIMEDOUT on stall.
 */
int t1_wait_ready(void);

uint32_t t1_perf_start(uint8_t tag);
uint32_t t1_perf_stop(void);
uint64_t t1_cycles(void);

int t1_dma_mm2s_async(uint32_t src_pa, uint32_t dst_pa, uint32_t len);
int t1_dma_s2mm_async(uint32_t src_pa, uint32_t dst_pa, uint32_t len);
int t1_dma_mm2s_sync(uint32_t src_pa, uint32_t dst_pa, uint32_t len);
int t1_dma_s2mm_sync(uint32_t src_pa, uint32_t dst_pa, uint32_t len);
int t1_dma_wait(void);

/*
 * Allocate a udmabuf-backed buffer.
 *
 * Returns 0 on success, -1 with errno set on failure.
 *
 * IMPORTANT: udmabuf is mmap'd CACHED. If you share this buffer with
 * T1's m_axi_hb master (i.e. you intend to issue any vle*.v / vse*.v
 * with rs1 = buf->pa), you MUST bracket the LSU sequence with the
 * sync helpers below - otherwise CPU writes stay in cache (T1 reads
 * stale DRAM) and CPU reads after T1 writes return cached values
 * (you see memset zeros). Linux's msync() is NOT a substitute on
 * udmabuf. See `fyp_doc/driver_implementation_status.md` § "Fix D".
 *
 * Required usage:
 *
 *   t1_buf_alloc(&in,  N);
 *   t1_buf_alloc(&out, N);
 *   write_input(in.va);
 *   t1_buf_sync_for_device(&in);    // must, before T1 reads
 *   t1_buf_sync_for_device(&out);   // must, if T1 will read out too
 *   t1_issue(...);                  // your vle/vse / kernel
 *   t1_buf_sync_for_cpu(&out);      // must, before CPU reads
 *   read_output(out.va);
 */
int t1_buf_alloc(struct t1_buf *buf, size_t size);
void t1_buf_free(struct t1_buf *buf);

/*
 * Cache management for udmabuf-backed t1_buf regions.
 *
 * udmabuf maps cached by default. CPU writes land in L1/L2 cache and
 * don't reach DRAM until explicitly flushed; CPU reads return cached
 * data even after T1 has written fresh values to DRAM via m_axi_hb.
 * Linux's msync() on udmabuf does NOT reliably perform the cache
 * operation we need; udmabuf provides a sysfs interface for this:
 *
 *   /sys/class/u-dma-buf/udmabufN/sync_for_device  <- write before T1 reads
 *   /sys/class/u-dma-buf/udmabufN/sync_for_cpu     <- write after T1 writes
 *
 * The expected pattern around an LSU roundtrip is:
 *   t1_buf_sync_for_device(&in);   // flush CPU writes to DRAM
 *   t1_issue(vle8 from in.pa);
 *   t1_issue(vse8 to out.pa);
 *   t1_buf_sync_for_cpu(&out);     // invalidate CPU cache for out
 *   memcmp(in.va, out.va, ...);
 *
 * Returns 0 on success, -1 with errno set on sysfs write failure.
 */
int t1_buf_sync_for_cpu(struct t1_buf *buf);
int t1_buf_sync_for_device(struct t1_buf *buf);

/*
 * URAM scratchpad on the 5r / 5t bitstreams: 512 KB at PA 0xA0080000
 * (16/64 URAM288, backed by fpga/wrapper/uram_scratchpad.v), reachable
 * from T1 m_axi_hb (via smartconnect_hb/M01) and from both axi_dma
 * masters. UIO-exposed as "bram" (the bram_scratch DT node, reg size
 * 0x80000). Earlier F4/5l bitstreams used a 32 KB BRAM at 0xB0000000.
 *
 * t1_scratchpad_alloc returns a t1_buf whose .va points into the
 * libt1-owned URAM mmap and whose .pa is exactly T1_SCRATCHPAD_PA
 * (0xA0080000) + offset.
 * Use this PA as the rs1 to vle/vse instructions when you want T1 to
 * read or write scratchpad instead of DDR. The DMA kernel APIs accept
 * the same PA - descriptor src/dst can mix DDR and scratchpad freely.
 *
 * Caller responsibilities:
 *   * `offset` and `size` must satisfy offset + size <= 512 KB
 *     (T1_SCRATCHPAD_SIZE). Failure returns -1 with errno=ERANGE.
 *   * Caller must pick non-overlapping [offset, offset+size) ranges
 *     for concurrent allocations. There is no internal allocator.
 *
 * Cache coherency: scratchpad is URAM, non-coherent at the bram_ctrl
 * level but PS-side access goes through UIO mmio (uncached by the UIO
 * kernel driver), so t1_buf_sync_for_{cpu,device} are no-ops on
 * scratchpad bufs (they return 0 without touching any sysfs file).
 *
 * t1_buf_free on a scratchpad buf is also a no-op (zero-cost): the
 * underlying mmap is owned by libt1 and freed in t1_close().
 */
/*
 * 5m: scratchpad relocated 0xB0000000 -> 0xA0080000 because the B aperture
 * is HPM1_FPD which is disabled (PSU__USE__M_AXI_GP1=0); A aperture is
 * HPM0_FPD which is enabled and routes through smartconnect_ctrl. F6
 * additionally added a PS-side path via dual-port bram_ctrl S_AXI_CTRL,
 * so PS direct mmap of /dev/uio6 now works.
 */
#define T1_SCRATCHPAD_PA   0xA0080000u
#define T1_SCRATCHPAD_SIZE 0x80000u  /* 512 KB (URAM, 16/64 URAM288) */

int t1_scratchpad_alloc(struct t1_buf *buf, size_t offset, size_t size);

uint32_t t1_va_to_pa(const void *va);

/*
 * Like t1_va_to_pa, but verifies that the entire [va, va+size) range is
 * physically contiguous. Returns the starting PA on success, or 0 with
 * errno set (EXDEV if non-contiguous, EFAULT if any page is not present,
 * EOVERFLOW if the PA exceeds 32 bits, EINVAL on bad input).
 *
 * Use this instead of t1_va_to_pa() for any buffer larger than one page
 * (i.e. anything that DMA will read or write in bulk). V4L2 MMAP buffers
 * and DRM dumb buffers are NOT guaranteed contiguous on systems without
 * CMA-backed allocators, and feeding a non-contiguous buffer to AXI DMA
 * silently corrupts pages 2..N. This helper turns that into a loud error
 * at open() time.
 */
uint32_t t1_va_to_pa_range(const void *va, size_t size);

void t1_set_vertical_mode_raw(int v);
int t1_get_vertical_mode_raw(void);

#ifdef __cplusplus
}
#endif
