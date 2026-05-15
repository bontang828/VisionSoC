/*
 * Option B (hybrid): take over frmbuf mid-stream.
 * Run yavta first to STREAMON state (kernel inits AP1302 fully).
 * Then this program halts frmbuf, redirects to udmabuf, kicks AP_START.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

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

static uint32_t r32(volatile uint8_t *b, unsigned o) { return *(volatile uint32_t*)(b+o); }
static void w32(volatile uint8_t *b, unsigned o, uint32_t v) { *(volatile uint32_t*)(b+o) = v; }

int main(void) {
    int fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_mem < 0) { perror("/dev/mem"); return 1; }
    volatile uint8_t *fb = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd_mem, FRMBUF_BASE);
    volatile uint8_t *cs = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd_mem, CSISS_BASE);

    FILE *fp = fopen("/sys/class/u-dma-buf/udmabuf2/phys_addr", "r");
    uint64_t buf_pa = 0;
    fscanf(fp, "%lx", &buf_pa);
    fclose(fp);

    int fd_buf = open("/dev/udmabuf2", O_RDWR);
    void *buf = mmap(NULL, 32768, PROT_READ|PROT_WRITE, MAP_SHARED, fd_buf, 0);
    memset(buf, 0xAB, 32768);

    printf("udmabuf2 PA = 0x%lx\n", buf_pa);
    printf("Initial frmbuf state:\n");
    printf("  CTRL=%x WIDTH=%u HEIGHT=%u STRIDE=%u FMT=%u ADDR=0x%x ADDR2=0x%x\n",
           r32(fb, FB_CTRL), r32(fb, FB_WIDTH), r32(fb, FB_HEIGHT),
           r32(fb, FB_STRIDE), r32(fb, FB_FMT),
           r32(fb, FB_ADDR), r32(fb, FB_ADDR2));
    printf("  csi CCR=%x CSR=%x (PKTCNT=%u)\n",
           r32(cs, 0x00), r32(cs, 0x10), (r32(cs, 0x10) >> 16) & 0xffff);

    if (r32(cs, 0x10) >> 16 == 0) {
        fprintf(stderr, "WARNING: csi PKTCNT=0, AP1302 not emitting. yavta needs to be running first.\n");
    }

    /* Wait for any in-flight transfer to complete */
    printf("Halting frmbuf...\n");
    w32(fb, FB_CTRL, 0x0);  /* clear AP_START */
    usleep(50000);

    printf("Redirecting frmbuf to udmabuf2...\n");
    /* Override addresses but keep dimensions if already set */
    if (r32(fb, FB_WIDTH) != 128) w32(fb, FB_WIDTH, 128);
    if (r32(fb, FB_HEIGHT) != 128) w32(fb, FB_HEIGHT, 128);
    if (r32(fb, FB_STRIDE) != 128) w32(fb, FB_STRIDE, 128);
    if (r32(fb, FB_FMT) != 19) w32(fb, FB_FMT, 19);
    w32(fb, FB_ADDR, buf_pa);
    w32(fb, FB_ADDR2, buf_pa + 16384);

    w32(fb, FB_GIE, 0x0);  /* disable IRQ so kernel doesn't see frame-done */
    w32(fb, FB_IE, 0x0);
    w32(fb, FB_ISR, 0xff);  /* clear ISR */

    printf("After override:\n");
    printf("  WIDTH=%u HEIGHT=%u STRIDE=%u FMT=%u ADDR=0x%x ADDR2=0x%x\n",
           r32(fb, FB_WIDTH), r32(fb, FB_HEIGHT),
           r32(fb, FB_STRIDE), r32(fb, FB_FMT),
           r32(fb, FB_ADDR), r32(fb, FB_ADDR2));

    /* Kick frmbuf - one shot */
    printf("Kicking frmbuf AP_START...\n");
    w32(fb, FB_CTRL, 0x1);

    /* Poll for ap_done */
    for (int i = 0; i < 1000; i++) {
        uint32_t ctrl = r32(fb, FB_CTRL);
        uint32_t isr = r32(fb, FB_ISR);
        uint32_t csr = r32(cs, 0x10);
        if (i % 50 == 0)
            printf("  iter=%d CTRL=%x ISR=%x PKTCNT=%u\n",
                   i, ctrl, isr, (csr >> 16) & 0xffff);
        if (ctrl & 0x2) { printf("  AP_DONE at iter %d\n", i); break; }
        if (isr & 0x1) { printf("  ISR.ap_done_irq at iter %d\n", i); }
        usleep(10000);
    }

    printf("Final: CTRL=%x ISR=%x PKTCNT=%u\n",
           r32(fb, FB_CTRL), r32(fb, FB_ISR), (r32(cs, 0x10) >> 16) & 0xffff);

    w32(fb, FB_CTRL, 0x0);

    uint8_t *p = (uint8_t*)buf;
    int nz = 0;
    for (int i = 0; i < 24576; i++) if (p[i] != 0xAB) nz++;
    printf("Non-sentinel bytes total: %d/%d\n", nz, 24576);

    printf("First 64 bytes of Y plane:\n");
    for (int i = 0; i < 64; i++) {
        if (i % 16 == 0) printf("  ");
        printf("%02x ", p[i]);
        if (i % 16 == 15) printf("\n");
    }

    FILE *out = fopen("/tmp/optb2_frame.nv12", "wb");
    if (out) {
        fwrite(buf, 1, 24576, out);
        fclose(out);
        printf("Saved /tmp/optb2_frame.nv12\n");
    }
    return 0;
}
