#include "libt1.h"

#include "libt1_regs.h"

#include <ctype.h>
#include <dirent.h>
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
#define BRAM_UIO_PATH "/dev/uio6"
#define T1_UIO_NAME "t1"
#define DMA_UIO_NAME "dma"
#define BRAM_UIO_NAME "bram"
#define UIO_MAP_SIZE 4096u
#define T1_POLL_TIMEOUT_MS 1000
#define T1_DMA_TIMEOUT_MS 1000
#define T1_READY_TIMEOUT_MS 1000
#define T1_BUSY_TIMEOUT_MS 1000
#define T1_MAX_UDMABUF 32

static int g_uio_t1_fd = -1;
static int g_uio_dma_fd = -1;
static int g_uio_bram_fd = -1;
static volatile uint32_t *g_t1_regs;
static volatile uint32_t *g_dma_regs;
static void *g_bram_base;  /* lazy-mapped on first t1_scratchpad_alloc */
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

/*
 * Find /dev/uioN where /sys/class/uio/uioN/name == target_name.
 * Used because k26-starter-kits (and the always-on PS axi-pmon nodes)
 * push T1/DMA off uio0/uio1 - the binding name is the only stable handle.
 */
static int find_uio_by_name(const char *target_name, char *path_out, size_t path_sz)
{
    DIR *d = opendir("/sys/class/uio");
    if (!d) {
        return -1;
    }

    int found = -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "uio", 3) != 0 ||
            !isdigit((unsigned char)de->d_name[3])) {
            continue;
        }

        /* Big enough for "/sys/class/uio/" + NAME_MAX + "/name". */
        char name_path[64 + 256];
        snprintf(name_path, sizeof(name_path),
                 "/sys/class/uio/%s/name", de->d_name);

        FILE *f = fopen(name_path, "r");
        if (!f) {
            continue;
        }
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), f)) {
            char *nl = strchr(buf, '\n');
            if (nl) {
                *nl = '\0';
            }
            if (strcmp(buf, target_name) == 0) {
                /* d_name is "uioN" in practice; cap conservatively for snprintf-trunc warning. */
                snprintf(path_out, path_sz, "/dev/%.32s", de->d_name);
                found = 0;
            }
        }
        fclose(f);
        if (found == 0) {
            break;
        }
    }
    closedir(d);
    if (found != 0) {
        errno = ENODEV;
    }
    return found;
}

static const char *resolve_uio_path(const char *env_var,
                                    const char *target_name,
                                    const char *fallback,
                                    char *buf, size_t buf_sz)
{
    const char *p = getenv(env_var);
    if (p && *p) {
        return p;
    }
    if (find_uio_by_name(target_name, buf, buf_sz) == 0) {
        return buf;
    }
    return fallback;
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

static int remaining_ms_since(const struct timespec *start, int timeout_ms)
{
    if (timeout_ms < 0) {
        return -1;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long sec = now.tv_sec - start->tv_sec;
    long nsec = now.tv_nsec - start->tv_nsec;
    long elapsed = sec * 1000 + nsec / 1000000;
    if (elapsed >= timeout_ms) {
        return 0;
    }
    return timeout_ms - (int)elapsed;
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

static int t1_wait_irq_status(uint32_t enable_mask, uint32_t wanted_mask,
                              uint32_t *status_out, int timeout_ms)
{
    if (wanted_mask == 0) {
        errno = EINVAL;
        return -1;
    }

    uint32_t old_irq_en = mmio_rd(g_t1_regs, T1_REG_IRQ_EN);
    mmio_wr(g_t1_regs, T1_REG_IRQ_EN, old_irq_en | enable_mask);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        int wait_ms = remaining_ms_since(&start, timeout_ms);
        if (wait_ms == 0) {
            mmio_wr(g_t1_regs, T1_REG_IRQ_EN, old_irq_en);
            errno = ETIMEDOUT;
            return 0;
        }

        if (wait_uio_irq(g_uio_t1_fd, wait_ms) < 0) {
            int saved = errno;
            mmio_wr(g_t1_regs, T1_REG_IRQ_EN, old_irq_en);
            errno = saved;
            return saved == ETIMEDOUT ? 0 : -1;
        }

        uint32_t status = mmio_rd(g_t1_regs, T1_REG_IRQ_STATUS);
        int rearm_rc = uio_rearm(g_uio_t1_fd);
        if (status_out) {
            *status_out = status;
        }
        if (rearm_rc < 0) {
            int saved = errno;
            mmio_wr(g_t1_regs, T1_REG_IRQ_EN, old_irq_en);
            errno = saved;
            return -1;
        }
        if (status & wanted_mask) {
            mmio_wr(g_t1_regs, T1_REG_IRQ_EN, old_irq_en);
            return 1;
        }
    }
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

        int rc = t1_wait_irq_status(T1_IRQ_MEM, T1_IRQ_MEM, NULL,
                                    T1_POLL_TIMEOUT_MS);
        if (rc <= 0) {
            if (rc == 0) {
                errno = ETIMEDOUT;
            }
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

    char t1_buf[64], dma_buf[64];
    const char *t1_path = resolve_uio_path("T1_UIO_PATH", T1_UIO_NAME,
                                           T1_UIO_PATH, t1_buf, sizeof(t1_buf));
    const char *dma_path = resolve_uio_path("DMA_UIO_PATH", DMA_UIO_NAME,
                                            DMA_UIO_PATH, dma_buf, sizeof(dma_buf));

    if (open_and_map(t1_path, &g_uio_t1_fd, &g_t1_regs) < 0) {
        goto fail;
    }
    if (open_and_map(dma_path, &g_uio_dma_fd, &g_dma_regs) < 0) {
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
    if (g_bram_base) {
        munmap(g_bram_base, T1_SCRATCHPAD_SIZE);
        g_bram_base = NULL;
    }
    if (g_uio_bram_fd >= 0) {
        close(g_uio_bram_fd);
        g_uio_bram_fd = -1;
    }
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

int t1_wait_rd(uint32_t *data, uint8_t *rd_addr, bool *is_fp, int timeout_ms)
{
    if (require_init() < 0) {
        return -1;
    }

    int rc = t1_drain_rd(data, rd_addr, is_fp);
    if (rc != 0) {
        return rc;
    }

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        int wait_ms = remaining_ms_since(&start, timeout_ms);
        if (wait_ms == 0) {
            errno = ETIMEDOUT;
            return 0;
        }

        rc = t1_wait_irq_status(T1_IRQ_RD, T1_IRQ_RD, NULL, wait_ms);
        if (rc <= 0) {
            return rc;
        }

        rc = t1_drain_rd(data, rd_addr, is_fp);
        if (rc != 0) {
            return rc;
        }
    }
}

int t1_wait_csr(uint32_t *vxsat, uint32_t *fflag, int timeout_ms)
{
    if (require_init() < 0) {
        return -1;
    }

    int rc = t1_drain_csr(vxsat, fflag);
    if (rc != 0) {
        return rc;
    }

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        int wait_ms = remaining_ms_since(&start, timeout_ms);
        if (wait_ms == 0) {
            errno = ETIMEDOUT;
            return 0;
        }

        rc = t1_wait_irq_status(T1_IRQ_CSR, T1_IRQ_CSR, NULL, wait_ms);
        if (rc <= 0) {
            return rc;
        }

        rc = t1_drain_csr(vxsat, fflag);
        if (rc != 0) {
            return rc;
        }
    }
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

    /*
     * Poll DMA SR.IDLE on both channels rather than block on the UIO IRQ.
     *
     * The BD wires axi_dma/mm2s_introut and axi_dma/s2mm_introut to separate
     * GIC SPIs through irq_concat (system_top.tcl § "Concat DMA interrupts").
     * generic-uio only registers IRQ index 0 (mm2s) on /dev/uio_dma, so
     * s2mm completion is invisible at the UIO read interface - a blocking
     * wait_uio_irq() would hang for the full timeout on s2mm-only ops.
     *
     * SR.IDLE is the authoritative completion signal for both channels and
     * also closes the dma_loopback race where one channel's IRQ has landed
     * at wake time but the other is still transferring. A channel that was
     * never kicked since reset is already idle, so this is safe for the
     * mm2s-only / s2mm-only sync paths too.
     */
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        uint32_t mm2s = mmio_rd(g_dma_regs, DMA_REG_MM2S_SR);
        uint32_t s2mm = mmio_rd(g_dma_regs, DMA_REG_S2MM_SR);

        bool error = ((mm2s | s2mm) & DMA_SR_ERR_MASK) != 0;
        bool both_idle = ((mm2s & DMA_SR_IDLE) != 0) && ((s2mm & DMA_SR_IDLE) != 0);

        if (error || both_idle) {
            uint32_t mc = mm2s & (DMA_SR_IRQ_MASK | DMA_SR_ERR_MASK);
            uint32_t sc = s2mm & (DMA_SR_IRQ_MASK | DMA_SR_ERR_MASK);
            if (mc) {
                mmio_wr(g_dma_regs, DMA_REG_MM2S_SR, mc);
            }
            if (sc) {
                mmio_wr(g_dma_regs, DMA_REG_S2MM_SR, sc);
            }
            (void)uio_rearm(g_uio_dma_fd);

            if (error) {
                errno = EIO;
                return -1;
            }
            return 0;
        }

        if (elapsed_ms_since(&start, T1_DMA_TIMEOUT_MS)) {
            errno = ETIMEDOUT;
            return -1;
        }
        sched_yield();
    }
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
    buf->_udmabuf_idx = idx;
    g_next_udmabuf_idx++;
    return 0;
}

static int udmabuf_sysfs_write(int idx, const char *attr, const char *value)
{
    char path[160];
    snprintf(path, sizeof(path),
             "/sys/class/u-dma-buf/udmabuf%d/%s", idx, attr);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, value, strlen(value));
    int saved = errno;
    close(fd);
    if (n < 0) { errno = saved; return -1; }
    return 0;
}

int t1_buf_sync_for_cpu(struct t1_buf *buf)
{
    if (!buf) {
        errno = EINVAL;
        return -1;
    }
    /* Scratchpad is BRAM mmio'd via UIO (uncached) - no flush needed. */
    if (buf->_udmabuf_idx < 0) {
        return 0;
    }
    if (buf->_udmabuf_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    return udmabuf_sysfs_write(buf->_udmabuf_idx, "sync_for_cpu", "1");
}

int t1_buf_sync_for_device(struct t1_buf *buf)
{
    if (!buf) {
        errno = EINVAL;
        return -1;
    }
    if (buf->_udmabuf_idx < 0) {
        return 0;
    }
    if (buf->_udmabuf_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    return udmabuf_sysfs_write(buf->_udmabuf_idx, "sync_for_device", "1");
}

static int ensure_bram_mapped(void)
{
    if (g_bram_base) {
        return 0;
    }

    char path_buf[64];
    const char *path = resolve_uio_path("BRAM_UIO_PATH", BRAM_UIO_NAME,
                                         BRAM_UIO_PATH, path_buf, sizeof(path_buf));

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    void *base = mmap(NULL, T1_SCRATCHPAD_SIZE, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    g_uio_bram_fd = fd;
    g_bram_base = base;
    return 0;
}

int t1_scratchpad_alloc(struct t1_buf *buf, size_t offset, size_t size)
{
    if (!buf || size == 0) {
        errno = EINVAL;
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    buf->_udmabuf_fd = -1;
    buf->_udmabuf_idx = -1;  /* sentinel: scratchpad-backed, not udmabuf */

    if (size > T1_SCRATCHPAD_SIZE || offset > T1_SCRATCHPAD_SIZE - size) {
        errno = ERANGE;
        return -1;
    }

    if (ensure_bram_mapped() < 0) {
        return -1;
    }

    buf->va = (char *)g_bram_base + offset;
    buf->pa = T1_SCRATCHPAD_PA + (uint32_t)offset;
    buf->size = size;
    return 0;
}

void t1_buf_free(struct t1_buf *buf)
{
    if (!buf) {
        return;
    }
    /* Scratchpad bufs share the libt1-owned BRAM mmap, freed in t1_close().
     * Only udmabuf-backed bufs own their fd / mmap. */
    if (buf->_udmabuf_idx >= 0) {
        if (buf->va && buf->size) {
            munmap(buf->va, buf->size);
        }
        if (buf->_udmabuf_fd >= 0) {
            close(buf->_udmabuf_fd);
        }
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

uint32_t t1_va_to_pa_range(const void *va, size_t size)
{
    if (!va || size == 0) {
        errno = EINVAL;
        return 0;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return 0;
    }

    int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }

    uint64_t vaddr = (uint64_t)(uintptr_t)va;
    uint64_t page_mask = (uint64_t)page_size - 1ull;
    uint64_t first_page = vaddr & ~page_mask;
    uint64_t last_page = (vaddr + (uint64_t)size - 1ull) & ~page_mask;

    uint64_t first_pfn = 0;
    uint64_t expected_pfn = 0;
    bool first = true;
    int saved = 0;

    for (uint64_t page_va = first_page; page_va <= last_page; page_va += (uint64_t)page_size) {
        uint64_t off = (page_va / (uint64_t)page_size) * sizeof(uint64_t);
        uint64_t entry = 0;
        ssize_t n = pread(fd, &entry, sizeof(entry), (off_t)off);
        if (n != (ssize_t)sizeof(entry)) {
            saved = (n < 0) ? errno : EIO;
            close(fd);
            errno = saved;
            return 0;
        }
        if ((entry & (1ull << 63)) == 0) {
            close(fd);
            errno = EFAULT;
            return 0;
        }

        uint64_t pfn = entry & ((1ull << 55) - 1ull);
        if (first) {
            first_pfn = pfn;
            expected_pfn = pfn + 1ull;
            first = false;
        } else {
            if (pfn != expected_pfn) {
                close(fd);
                errno = EXDEV;
                return 0;
            }
            expected_pfn++;
        }
    }

    close(fd);

    uint64_t pa = first_pfn * (uint64_t)page_size + (vaddr & page_mask);
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
