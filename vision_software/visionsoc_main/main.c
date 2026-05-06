#include "camera.h"
#include "display.h"
#include "kernels/grid_vadd.h"
#include "libt1.h"
#include "libt1_regs.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BRAM_BASE_PA 0xB0000000u
#define BRAM_HALF_A  (BRAM_BASE_PA + 0x0000u)
#define BRAM_HALF_B  (BRAM_BASE_PA + 0x4000u)
#define FRAME_BYTES  (128u * 128u)

static volatile sig_atomic_t g_should_exit;

static void on_signal(int signo)
{
    (void)signo;
    g_should_exit = 1;
}

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

static void usage(const char *argv0)
{
    fprintf(stderr, "Usage: %s [camera-device]\n", argv0);
}

static double elapsed_seconds(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) +
           (double)(b->tv_nsec - a->tv_nsec) / 1000000000.0;
}

static int issue_grid_vadd_in_place(uint32_t bram_pa)
{
    if (grid_vadd_count != 4) {
        errno = EINVAL;
        return -1;
    }

    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M4_TA_MA,
        .vl = 128,
    };

    op.instruction = grid_vadd[0];
    op.rs1 = bram_pa;
    if (t1_issue(&op) < 0) {
        return -1;
    }

    op.instruction = grid_vadd[1];
    op.rs1 = bram_pa;
    if (t1_issue(&op) < 0) {
        return -1;
    }

    op.instruction = grid_vadd[2];
    op.rs1 = 0;
    if (t1_issue(&op) < 0) {
        return -1;
    }

    op.instruction = grid_vadd[3];
    op.rs1 = bram_pa;
    if (t1_issue(&op) < 0) {
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    const char *camera_dev = "/dev/video0";
    if (argc > 2) {
        usage(argv[0]);
        return 2;
    }
    if (argc == 2) {
        camera_dev = argv[1];
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (t1_init() < 0) {
        die("t1_init");
    }

    struct camera cam;
    struct display disp;
    if (camera_open(&cam, camera_dev, 128, 128) < 0) {
        die("camera_open");
    }
    if (display_open(&disp) < 0) {
        die("display_open");
    }

    int half = 0;
    unsigned frame_count = 0;
    uint32_t last_kernel_cycles = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (!g_should_exit) {
        struct camera_buf cb;
        if (camera_dqbuf(&cam, &cb) < 0) {
            die("camera_dqbuf");
        }

        uint32_t bram_pa = half == 0 ? BRAM_HALF_A : BRAM_HALF_B;
        if (t1_dma_mm2s_sync(cb.pa, bram_pa, FRAME_BYTES) < 0) {
            die("t1_dma_mm2s_sync");
        }

        (void)t1_perf_start(1);
        if (issue_grid_vadd_in_place(bram_pa) < 0) {
            die("issue_grid_vadd_in_place");
        }
        last_kernel_cycles = t1_perf_stop();

        struct display_buf db;
        if (display_dq_for_filling(&disp, &db) < 0) {
            die("display_dq_for_filling");
        }
        if (t1_dma_s2mm_sync(bram_pa, db.pa, FRAME_BYTES) < 0) {
            die("t1_dma_s2mm_sync");
        }
        if (display_qbuf(&disp, &db) < 0) {
            die("display_qbuf");
        }
        if (camera_qbuf(&cam, &cb) < 0) {
            die("camera_qbuf");
        }

        half = !half;
        frame_count++;
        if ((frame_count & 0x1Fu) == 0) {
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double seconds = elapsed_seconds(&t0, &t1);
            double fps = seconds > 0.0 ? 32.0 / seconds : 0.0;
            printf("frame %u, last kernel %u cycles, %.1f fps\n",
                   frame_count, last_kernel_cycles, fps);
            t0 = t1;
        }
    }

    display_close(&disp);
    camera_close(&cam);
    t1_close();
    return 0;
}
