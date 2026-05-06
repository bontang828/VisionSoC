#include "libt1.h"

#include "libt1_regs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define T1_UIO_PATH "/dev/uio0"
#define DMA_UIO_PATH "/dev/uio1"
#define UIO_MAP_SIZE 4096u
#define T1_POLL_TIMEOUT_MS 1000
#define T1_DMA_TIMEOUT_MS 1000
#define T1_READY_TIMEOUT_MS 1000
#define T1_BUSY_TIMEOUT_MS 1000
#define T1_MAX_UDMABUF 32

static int g_uio_t1_fd = -1;
static int g_uio_dma_fd = -1;
static volatile uint32_t *g_t1_regs;
static volatile uint32_t *g_dma_regs;
static int g_initialised;
static int g_next_udmabuf_idx;

static inline void mmio_fence(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static inline uint32_t mmio_rd(volatile uint32_t *base, uint32_t off)
{
    mmio_fence();
    uint32_t v = base[off / sizeof(uint32_t)];
    mmio_fence();
    return v;
}

static inline void mmio_wr(volatile uint32_t *base, uint32_t off, uint32_t v)
{
    mmio_fence();
    base[off / sizeof(uint32_t)] = v;
    mmio_fence();
}

static int require_init(void)
{
    if (!g_initialised) {
        errno = ENODEV;
        return -1;
    }
    return 0;
}

static int uio_rearm(int fd)
{
    uint32_t one = 1;
    ssize_t n;

    do {
        n = write(fd, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);

    return n == (ssize_t)sizeof(one) ? 0 : -1;
}

static int wait_uio_irq(int fd, int timeout_ms)
{
    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN,
    };
    uint32_t irq_count;

    for (;;) {
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        ssize_t n;
        do {
            n = read(fd, &irq_count, sizeof(irq_count));
        } while (n < 0 && errno == EINTR);

        if (n == (ssize_t)sizeof(irq_count)) {
            return 0;
        }
        if (n >= 0) {
            errno = EIO;
        }
        return -1;
    }
}

static int elapsed_ms_since(const struct timespec *start, int timeout_ms)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long sec = now.tv_sec - start->tv_sec;
    long nsec = now.tv_nsec - start->tv_nsec;
    long elapsed = sec * 1000 + nsec / 1000000;
    return elapsed >= timeout_ms;
}

static int wait_ctrl_ready(void)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (!(mmio_rd(g_t1_regs, T1_REG_CTRL) & T1_CTRL_ISSUE_READY)) {
        if (elapsed_ms_since(&start, T1_READY_TIMEOUT_MS)) {
            errno = ETIMEDOUT;
            return -1;
        }
        sched_yield();
    }

    return 0;
}

static int wait_ctrl_not_busy(void)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (mmio_rd(g_t1_regs, T1_REG_CTRL) & T1_CTRL_ISSUE_BUSY) {
        if (elapsed_ms_since(&start, T1_BUSY_TIMEOUT_MS)) {
            errno = ETIMEDOUT;
            return -1;
        }
        sched_yield();
    }

    return 0;
}

static void consume_mem_events(unsigned n_events)
{
    for (unsigned i = 0; i < n_events; i++) {
        mmio_wr(g_t1_regs, T1_REG_MEM_COUNT, 1);
    }
}

static unsigned mem_count(void)
{
    return mmio_rd(g_t1_regs, T1_REG_MEM_COUNT) & 0xFFu;
}

static int wait_mem_events(unsigned n_events)
{
    if (n_events == 0) {
        return 0;
    }

    for (;;) {
        unsigned count = mem_count();
        if (count >= n_events) {
            consume_mem_events(n_events);
            return 0;
        }

        if (wait_uio_irq(g_uio_t1_fd, T1_POLL_TIMEOUT_MS) < 0) {
            return -1;
        }

        uint32_t status = mmio_rd(g_t1_regs, T1_REG_IRQ_STATUS);
        if ((status & T1_IRQ_MEM) == 0) {
            if (uio_rearm(g_uio_t1_fd) < 0) {
                return -1;
            }
            continue;
        }

        count = mem_count();
        if (count >= n_events) {
            consume_mem_events(n_events);
            if (uio_rearm(g_uio_t1_fd) < 0) {
                return -1;
            }
            return 0;
        }

        if (uio_rearm(g_uio_t1_fd) < 0) {
            return -1;
        }
    }
}

static void clear_pending_mem_events(void)
{
    unsigned count = mem_count();
    consume_mem_events(count);
}

static bool is_lsu_instruction(uint32_t instr)
{
    uint32_t opcode = instr & 0x7Fu;
    return opcode == 0x07u || opcode == 0x27u;
}

static int open_and_map(const char *path, int *out_fd, volatile uint32_t **out_regs)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    void *map = mmap(NULL, UIO_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    *out_fd = fd;
    *out_regs = (volatile uint32_t *)map;
    return 0;
}

static void unmap_and_close(int *fd, volatile uint32_t **regs)
{
    if (*regs) {
        munmap((void *)*regs, UIO_MAP_SIZE);
        *regs = NULL;
    }
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static int dma_reset(void)
{
    mmio_wr(g_dma_regs, DMA_REG_MM2S_CR, DMA_CR_RESET);
    mmio_wr(g_dma_regs, DMA_REG_S2MM_CR, DMA_CR_RESET);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        uint32_t mm2s = mmio_rd(g_dma_regs, DMA_REG_MM2S_CR);
        uint32_t s2mm = mmio_rd(g_dma_regs, DMA_REG_S2MM_CR);
        if (((mm2s | s2mm) & DMA_CR_RESET) == 0) {
            mmio_wr(g_dma_regs, DMA_REG_MM2S_SR, DMA_SR_IRQ_MASK | DMA_SR_ERR_MASK);
            mmio_wr(g_dma_regs, DMA_REG_S2MM_SR, DMA_SR_IRQ_MASK | DMA_SR_ERR_MASK);
            return 0;
        }
        if (elapsed_ms_since(&start, T1_DMA_TIMEOUT_MS)) {
            errno = ETIMEDOUT;
            return -1;
        }
        sched_yield();
    }
}

int t1_init(void)
{
    if (g_initialised) {
        return 0;
    }

    if (open_and_map(T1_UIO_PATH, &g_uio_t1_fd, &g_t1_regs) < 0) {
        goto fail;
    }
    if (open_and_map(DMA_UIO_PATH, &g_uio_dma_fd, &g_dma_regs) < 0) {
        goto fail;
    }

    mmio_wr(g_t1_regs, T1_REG_IRQ_EN, T1_IRQ_MEM);
    clear_pending_mem_events();

    if (dma_reset() < 0) {
        goto fail;
    }

    if (uio_rearm(g_uio_t1_fd) < 0) {
        goto fail;
    }
    if (uio_rearm(g_uio_dma_fd) < 0) {
        goto fail;
    }

    g_initialised = 1;
    return 0;

fail:
    {
        int saved = errno;
        t1_close();
        errno = saved;
        return -1;
    }
}

void t1_close(void)
{
    unmap_and_close(&g_uio_t1_fd, &g_t1_regs);
    unmap_and_close(&g_uio_dma_fd, &g_dma_regs);
    g_initialised = 0;
    g_next_udmabuf_idx = 0;
}

int t1_drain_rd(uint32_t *data, uint8_t *rd_addr, bool *is_fp)
{
    if (require_init() < 0) {
        return -1;
    }

    uint32_t status = mmio_rd(g_t1_regs, T1_REG_RD_FIFO_STS);
    if ((status & T1_FIFO_COUNT_MASK) == 0) {
        return 0;
    }

    uint32_t pop_data = mmio_rd(g_t1_regs, T1_REG_RD_POP_DATA);
    uint32_t meta = mmio_rd(g_t1_regs, T1_REG_RD_POP_META);

    if (data) {
        *data = pop_data;
    }
    if (rd_addr) {
        *rd_addr = (uint8_t)(meta & 0x1Fu);
    }
    if (is_fp) {
        *is_fp = (meta & (1u << 5)) != 0;
    }

    return 1;
}

int t1_drain_csr(uint32_t *vxsat, uint32_t *fflag)
{
    if (require_init() < 0) {
        return -1;
    }

    uint32_t status = mmio_rd(g_t1_regs, T1_REG_CSR_FIFO_STS);
    if ((status & T1_FIFO_COUNT_MASK) == 0) {
        return 0;
    }

    uint32_t pop = mmio_rd(g_t1_regs, T1_REG_CSR_POP);
    uint32_t f = mmio_rd(g_t1_regs, T1_REG_CSR_FFLAG);

    if (vxsat) {
        *vxsat = pop;
    }
    if (fflag) {
        *fflag = f;
    }

    return 1;
}

int t1_wait_mem(unsigned n_events)
{
    if (require_init() < 0) {
        return -1;
    }
    return wait_mem_events(n_events);
}

int t1_issue(const struct t1_op *op)
{
    if (require_init() < 0) {
        return -1;
    }
    if (!op) {
        errno = EINVAL;
        return -1;
    }

    bool lsu = is_lsu_instruction(op->instruction);
    if (lsu) {
        clear_pending_mem_events();
    }

    if (wait_ctrl_ready() < 0) {
        return -1;
    }

    mmio_wr(g_t1_regs, T1_REG_INSTRUCTION, op->instruction);
    mmio_wr(g_t1_regs, T1_REG_RS1_DATA, op->rs1);
    mmio_wr(g_t1_regs, T1_REG_RS2_DATA, op->rs2);
    mmio_wr(g_t1_regs, T1_REG_VTYPE, op->vtype);
    mmio_wr(g_t1_regs, T1_REG_VL, op->vl);
    mmio_wr(g_t1_regs, T1_REG_VSTART, op->vstart);
    mmio_wr(g_t1_regs, T1_REG_VCSR, op->vcsr);
    mmio_wr(g_t1_regs, T1_REG_VERTICAL_MODE, op->vertical_mode ? 1u : 0u);
    mmio_wr(g_t1_regs, T1_REG_CTRL, T1_CTRL_ISSUE_START);

    if (lsu) {
        return wait_mem_events(1);
    }

    return wait_ctrl_not_busy();
}

int t1_issue_kernel(const uint32_t *kernel, size_t n_words,
                    const struct t1_op *op_template)
{
    if (!kernel && n_words != 0) {
        errno = EINVAL;
        return -1;
    }

    struct t1_op op;
    if (op_template) {
        op = *op_template;
    } else {
        memset(&op, 0, sizeof(op));
    }

    for (size_t i = 0; i < n_words; i++) {
        op.instruction = kernel[i];
        if (t1_issue(&op) < 0) {
            return -1;
        }
    }

    return 0;
}

uint32_t t1_perf_start(uint8_t tag)
{
    if (require_init() < 0) {
        return 0;
    }
    mmio_wr(g_t1_regs, T1_REG_PERF_TAG, tag);
    return mmio_rd(g_t1_regs, T1_REG_PERF_CYCLES_LO);
}

uint32_t t1_perf_stop(void)
{
    if (require_init() < 0) {
        return 0;
    }
    mmio_wr(g_t1_regs, T1_REG_PERF_TAG, 0);
    return mmio_rd(g_t1_regs, T1_REG_PERF_DELTA);
}

uint64_t t1_cycles(void)
{
    if (require_init() < 0) {
        return 0;
    }

    uint32_t hi1;
    uint32_t lo;
    uint32_t hi2;

    do {
        hi1 = mmio_rd(g_t1_regs, T1_REG_PERF_CYCLES_HI);
        lo = mmio_rd(g_t1_regs, T1_REG_PERF_CYCLES_LO);
        hi2 = mmio_rd(g_t1_regs, T1_REG_PERF_CYCLES_HI);
    } while (hi1 != hi2);

    return ((uint64_t)hi2 << 32) | lo;
}

int t1_dma_mm2s_async(uint32_t src_pa, uint32_t dst_pa, uint32_t len)
{
    (void)dst_pa;

    if (require_init() < 0) {
        return -1;
    }
    if (len == 0) {
        errno = EINVAL;
        return -1;
    }

    mmio_wr(g_dma_regs, DMA_REG_MM2S_SR, DMA_SR_IRQ_MASK | DMA_SR_ERR_MASK);
    mmio_wr(g_dma_regs, DMA_REG_MM2S_CR,
            DMA_CR_RUN | DMA_CR_IOC_IRQ_EN | DMA_CR_ERR_IRQ_EN);
    mmio_wr(g_dma_regs, DMA_REG_MM2S_SRC_ADDR, src_pa);
    mmio_wr(g_dma_regs, DMA_REG_MM2S_LENGTH, len);
    return 0;
}

int t1_dma_s2mm_async(uint32_t src_pa, uint32_t dst_pa, uint32_t len)
{
    (void)src_pa;

    if (require_init() < 0) {
        return -1;
    }
    if (len == 0) {
        errno = EINVAL;
        return -1;
    }

    mmio_wr(g_dma_regs, DMA_REG_S2MM_SR, DMA_SR_IRQ_MASK | DMA_SR_ERR_MASK);
    mmio_wr(g_dma_regs, DMA_REG_S2MM_CR,
            DMA_CR_RUN | DMA_CR_IOC_IRQ_EN | DMA_CR_ERR_IRQ_EN);
    mmio_wr(g_dma_regs, DMA_REG_S2MM_DST_ADDR, dst_pa);
    mmio_wr(g_dma_regs, DMA_REG_S2MM_LENGTH, len);
    return 0;
}

int t1_dma_wait(void)
{
    if (require_init() < 0) {
        return -1;
    }

    if (wait_uio_irq(g_uio_dma_fd, T1_DMA_TIMEOUT_MS) < 0) {
        return -1;
    }

    uint32_t mm2s = mmio_rd(g_dma_regs, DMA_REG_MM2S_SR);
    uint32_t s2mm = mmio_rd(g_dma_regs, DMA_REG_S2MM_SR);
    uint32_t clear_mm2s = mm2s & (DMA_SR_IRQ_MASK | DMA_SR_ERR_MASK);
    uint32_t clear_s2mm = s2mm & (DMA_SR_IRQ_MASK | DMA_SR_ERR_MASK);

    if (clear_mm2s) {
        mmio_wr(g_dma_regs, DMA_REG_MM2S_SR, clear_mm2s);
    }
    if (clear_s2mm) {
        mmio_wr(g_dma_regs, DMA_REG_S2MM_SR, clear_s2mm);
    }

    if (uio_rearm(g_uio_dma_fd) < 0) {
        return -1;
    }

    if ((mm2s | s2mm) & DMA_SR_ERR_MASK) {
        errno = EIO;
        return -1;
    }

    return 0;
}

int t1_dma_mm2s_sync(uint32_t src_pa, uint32_t dst_pa, uint32_t len)
{
    if (t1_dma_mm2s_async(src_pa, dst_pa, len) < 0) {
        return -1;
    }
    return t1_dma_wait();
}

int t1_dma_s2mm_sync(uint32_t src_pa, uint32_t dst_pa, uint32_t len)
{
    if (t1_dma_s2mm_async(src_pa, dst_pa, len) < 0) {
        return -1;
    }
    return t1_dma_wait();
}

int t1_buf_alloc(struct t1_buf *buf, size_t size)
{
    if (!buf || size == 0) {
        errno = EINVAL;
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    buf->_udmabuf_fd = -1;

    if (g_next_udmabuf_idx >= T1_MAX_UDMABUF) {
        errno = ENOSPC;
        return -1;
    }

    int idx = g_next_udmabuf_idx;
    char path[64];
    snprintf(path, sizeof(path), "/dev/udmabuf%d", idx);

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    void *va = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (va == MAP_FAILED) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    char sysfs[128];
    snprintf(sysfs, sizeof(sysfs), "/sys/class/u-dma-buf/udmabuf%d/phys_addr", idx);
    FILE *f = fopen(sysfs, "r");
    if (!f) {
        int saved = errno;
        munmap(va, size);
        close(fd);
        errno = saved;
        return -1;
    }

    unsigned long long pa = 0;
    if (fscanf(f, "%llx", &pa) != 1) {
        fclose(f);
        munmap(va, size);
        close(fd);
        errno = EINVAL;
        return -1;
    }
    fclose(f);

    if (pa > UINT32_MAX) {
        munmap(va, size);
        close(fd);
        errno = EOVERFLOW;
        return -1;
    }

    buf->va = va;
    buf->pa = (uint32_t)pa;
    buf->size = size;
    buf->_udmabuf_fd = fd;
    g_next_udmabuf_idx++;
    return 0;
}

void t1_buf_free(struct t1_buf *buf)
{
    if (!buf) {
        return;
    }
    if (buf->va && buf->size) {
        munmap(buf->va, buf->size);
    }
    if (buf->_udmabuf_fd >= 0) {
        close(buf->_udmabuf_fd);
    }
    memset(buf, 0, sizeof(*buf));
    buf->_udmabuf_fd = -1;
}

uint32_t t1_va_to_pa(const void *va)
{
    if (!va) {
        errno = EINVAL;
        return 0;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return 0;
    }

    uint64_t vaddr = (uint64_t)(uintptr_t)va;
    uint64_t vpn = vaddr / (uint64_t)page_size;
    uint64_t offset = vpn * sizeof(uint64_t);

    int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }

    uint64_t entry = 0;
    ssize_t n = pread(fd, &entry, sizeof(entry), (off_t)offset);
    int saved = errno;
    close(fd);

    if (n != (ssize_t)sizeof(entry)) {
        errno = n < 0 ? saved : EIO;
        return 0;
    }

    if ((entry & (1ull << 63)) == 0) {
        errno = EFAULT;
        return 0;
    }

    uint64_t pfn = entry & ((1ull << 55) - 1ull);
    uint64_t pa = pfn * (uint64_t)page_size + (vaddr & ((uint64_t)page_size - 1ull));
    if (pa > UINT32_MAX) {
        errno = EOVERFLOW;
        return 0;
    }

    return (uint32_t)pa;
}

void t1_set_vertical_mode_raw(int v)
{
    if (require_init() < 0) {
        return;
    }
    mmio_wr(g_t1_regs, T1_REG_VERTICAL_MODE, v ? 1u : 0u);
}

int t1_get_vertical_mode_raw(void)
{
    if (require_init() < 0) {
        return -1;
    }
    return (int)(mmio_rd(g_t1_regs, T1_REG_VERTICAL_MODE) & 1u);
}
