/*
 * Option B: direct frmbuf capture, bypass V4L2.
 * Tests if camera hardware path works without kernel driver involvement.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

#define FRMBUF_BASE 0x80010000
#define CSISS_BASE  0x80000000
#define MAP_SIZE    0x10000

#define FB_CTRL  0x00
#define FB_GIE   0x04
#define FB_IE    0x08
#define FB_ISR   0x0c
#define FB_WIDTH 0x10
#define FB_HEIGHT 0x18
#define FB_STRIDE 0x20
#define FB_FMT   0x28
#define FB_ADDR  0x30
#define FB_ADDR2 0x3c

#define CS_CCR   0x00
#define CS_PCR   0x04
#define CS_CSR   0x10
#define CS_GIER  0x20
#define CS_ISR   0x24
#define CS_IER   0x28

#define AP1302_REG_SYS_START 0x601a
#define AP1302_REG_PREVIEW_OUT_FMT 0x2012
#define AP1302_REG_PREVIEW_WIDTH 0x2000
#define AP1302_REG_PREVIEW_HEIGHT 0x2002
#define AP1302_REG_PREVIEW_HINF 0x2030

static uint32_t r32(volatile uint8_t *base, unsigned off) {
    return *(volatile uint32_t*)(base + off);
}
static void w32(volatile uint8_t *base, unsigned off, uint32_t v) {
    *(volatile uint32_t*)(base + off) = v;
}

static int ap_write16(int fd, uint16_t reg, uint16_t val) {
    uint8_t buf[4] = { reg >> 8, reg & 0xff, val >> 8, val & 0xff };
    if (write(fd, buf, 4) != 4) { perror("ap write"); return -1; }
    return 0;
}
static int ap_read16(int fd, uint16_t reg, uint16_t *val) {
    uint8_t reg_buf[2] = { reg >> 8, reg & 0xff };
    uint8_t val_buf[2];
    struct i2c_msg msgs[2] = {
        { .addr = 0x3c, .flags = 0, .len = 2, .buf = reg_buf },
        { .addr = 0x3c, .flags = I2C_M_RD, .len = 2, .buf = val_buf },
    };
    struct i2c_rdwr_ioctl_data x = { .msgs = msgs, .nmsgs = 2 };
    if (ioctl(fd, I2C_RDWR, &x) < 0) { perror("ap read"); return -1; }
    *val = (val_buf[0] << 8) | val_buf[1];
    return 0;
}

int main(void) {
    int fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_mem < 0) { perror("/dev/mem"); return 1; }

    volatile uint8_t *fb = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd_mem, FRMBUF_BASE);
    volatile uint8_t *cs = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd_mem, CSISS_BASE);
    if (fb == MAP_FAILED || cs == MAP_FAILED) { perror("mmap CSR"); return 1; }

    FILE *fp = fopen("/sys/class/u-dma-buf/udmabuf2/phys_addr", "r");
    if (!fp) { perror("phys_addr"); return 1; }
    uint64_t buf_pa = 0;
    fscanf(fp, "%lx", &buf_pa);
    fclose(fp);
    printf("udmabuf2 PA = 0x%lx\n", buf_pa);

    int fd_buf = open("/dev/udmabuf2", O_RDWR);
    if (fd_buf < 0) { perror("/dev/udmabuf2"); return 1; }
    void *buf = mmap(NULL, 32768, PROT_READ|PROT_WRITE, MAP_SHARED, fd_buf, 0);
    if (buf == MAP_FAILED) { perror("mmap buf"); return 1; }

    /* fill buffer with sentinel pattern so we can tell if camera wrote it */
    memset(buf, 0xAB, 32768);

    int fd_i2c = open("/dev/i2c-4", O_RDWR);
    if (fd_i2c < 0) { perror("/dev/i2c-4"); return 1; }
    if (ioctl(fd_i2c, I2C_SLAVE_FORCE, 0x3c) < 0) { perror("i2c slave"); return 1; }

    /* Stall AP1302 hard */
    printf("Stalling AP1302...\n");
    ap_write16(fd_i2c, AP1302_REG_SYS_START, 0x8040);
    usleep(200000);
    ap_write16(fd_i2c, AP1302_REG_SYS_START, 0x8140);
    usleep(200000);
    uint16_t sys = 0;
    ap_read16(fd_i2c, AP1302_REG_SYS_START, &sys);
    printf("  SYS_START after stall = 0x%04x\n", sys);

    /* Configure AP1302 for NV12 128x128 */
    printf("Configuring AP1302 for NV12 128x128...\n");
    ap_write16(fd_i2c, AP1302_REG_PREVIEW_HINF, 0x0014);  /* SPOOF | MIPI_LANES(4) */
    ap_write16(fd_i2c, AP1302_REG_PREVIEW_WIDTH, 128);
    ap_write16(fd_i2c, AP1302_REG_PREVIEW_HEIGHT, 128);
    ap_write16(fd_i2c, AP1302_REG_PREVIEW_OUT_FMT, 0x0031);  /* FT_YUV_JFIF(3<<4)|FST_YUV_420(1) */

    /* Reset csiss */
    printf("Resetting csiss...\n");
    w32(cs, CS_CCR, 0x2);
    usleep(2000);
    w32(cs, CS_CCR, 0x0);
    usleep(2000);
    w32(cs, CS_GIER, 0x1);
    w32(cs, CS_IER, 0xffffffff);
    w32(cs, CS_CCR, 0x1);  /* enable */
    printf("  csiss CCR=%x CSR=%x PCR=%x\n", r32(cs, CS_CCR), r32(cs, CS_CSR), r32(cs, CS_PCR));

    /* Configure frmbuf for NV12 128x128 */
    printf("Configuring frmbuf...\n");
    /* Make sure it's not running */
    w32(fb, FB_CTRL, 0x0);
    w32(fb, FB_GIE, 0x0);
    w32(fb, FB_IE, 0x0);

    w32(fb, FB_WIDTH, 128);
    w32(fb, FB_HEIGHT, 128);
    w32(fb, FB_STRIDE, 128);
    w32(fb, FB_FMT, 19);  /* Y_UV8_420 = NV12 */
    w32(fb, FB_ADDR, buf_pa);
    w32(fb, FB_ADDR2, buf_pa + 16384);

    /* arm interrupts (we'll poll, but enable so ISR works) */
    w32(fb, FB_GIE, 0x1);
    w32(fb, FB_IE, 0x1);
    /* clear any stale ISR */
    w32(fb, FB_ISR, 0xffffffff);
    printf("  frmbuf width=%u height=%u stride=%u fmt=%u addr=0x%x addr2=0x%x\n",
           r32(fb, FB_WIDTH), r32(fb, FB_HEIGHT), r32(fb, FB_STRIDE),
           r32(fb, FB_FMT), r32(fb, FB_ADDR), r32(fb, FB_ADDR2));

    /* Kick frmbuf - one-shot, no auto_restart */
    printf("Starting frmbuf (AP_START)...\n");
    w32(fb, FB_CTRL, 0x1);

    /* Un-stall AP1302 - it should start emitting */
    printf("Un-stalling AP1302...\n");
    ap_write16(fd_i2c, AP1302_REG_SYS_START, 0x8340);
    ap_read16(fd_i2c, AP1302_REG_SYS_START, &sys);
    printf("  SYS_START after unstall = 0x%04x\n", sys);

    /* Poll for ap_done */
    printf("Polling frmbuf for ap_done...\n");
    for (int i = 0; i < 1000; i++) {
        uint32_t ctrl = r32(fb, FB_CTRL);
        uint32_t isr = r32(fb, FB_ISR);
        uint32_t csr = r32(cs, CS_CSR);
        if (i % 100 == 0)
            printf("  iter=%d CTRL=%x ISR=%x csi.CSR=%x (PKTCNT=%u)\n",
                   i, ctrl, isr, csr, (csr >> 16) & 0xffff);
        if (ctrl & 0x2) {  /* AP_DONE */
            printf("  AP_DONE asserted at iter %d!\n", i);
            break;
        }
        if (isr & 0x1) {
            printf("  ISR.ap_done_irq at iter %d\n", i);
        }
        usleep(10000);  /* 10ms */
    }

    uint32_t final_ctrl = r32(fb, FB_CTRL);
    uint32_t final_isr = r32(fb, FB_ISR);
    uint32_t final_csr = r32(cs, CS_CSR);
    printf("Final state: CTRL=%x ISR=%x csi.CSR=%x (PKTCNT=%u)\n",
           final_ctrl, final_isr, final_csr, (final_csr >> 16) & 0xffff);

    /* Stall AP1302 again, halt frmbuf */
    ap_write16(fd_i2c, AP1302_REG_SYS_START, 0x8140);
    w32(fb, FB_CTRL, 0x0);
    w32(cs, CS_CCR, 0x0);

    /* Dump first 64 bytes of buffer */
    uint8_t *p = (uint8_t*)buf;
    printf("First 64 bytes of buffer:\n");
    int nz = 0;
    for (int i = 0; i < 64; i++) {
        if (i % 16 == 0) printf("  ");
        printf("%02x ", p[i]);
        if (p[i] != 0xAB) nz++;
        if (i % 16 == 15) printf("\n");
    }
    printf("Non-sentinel bytes in first 64: %d/%d\n", nz, 64);

    /* Save full frame */
    FILE *out = fopen("/tmp/optb_frame.nv12", "wb");
    if (out) {
        fwrite(buf, 1, 24576, out);  /* 16384 Y + 8192 UV */
        fclose(out);
        printf("Saved 24576 bytes to /tmp/optb_frame.nv12\n");
    }

    /* Count non-sentinel bytes in whole frame */
    int total_nz = 0;
    for (int i = 0; i < 24576; i++) if (p[i] != 0xAB) total_nz++;
    printf("Non-sentinel bytes total: %d/%d\n", total_nz, 24576);

    return final_ctrl & 0x2 ? 0 : 2;
}
