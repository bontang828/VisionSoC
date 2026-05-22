/*
 * main_perf -- stage-instrumented twin of main.c.
 *
 * Runs the full camera->display pipeline exactly like main.c but
 * captures clock_gettime(CLOCK_MONOTONIC) at every stage boundary and
 * emits one CSV row per frame. The T1 kernel stage is also bracketed
 * with t1_perf_start/stop so we get both wall-microseconds and T1
 * hardware cycles; their delta is the libt1/Linux-overhead component.
 *
 * Exits after VISIONSOC_PERF_FRAMES frames (env, default 100) or
 * --frames N. CSV goes to stdout by default; --out path overrides.
 *
 * Production behaviour is otherwise unchanged from main.c -- same
 * camera path, same URAM/DMA flow, same display pack/conversion, same
 * displayqbuf cadence. Do not run this binary in production; it
 * has no marker support and exits after N frames.
 */
#include "camera.h"
#include "display.h"
#include "kernels/active_kernel.h"
#include "libt1.h"
#include "libt1_regs.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef ACTIVE_KERNEL_FLOW_COLOR
#define ACTIVE_KERNEL_FLOW_COLOR 0
#endif

/* Same URAM layout as main.c (main.c:24-26). */
#define URAM_BASE_PA 0xA0080000u
#define URAM_HALF_A  (URAM_BASE_PA + 0x00000u)
#define URAM_HALF_B  (URAM_BASE_PA + 0x04000u)

#define FRAME_BYTES  (128u * 128u)
#define NV12_BYTES   (FRAME_BYTES + FRAME_BYTES / 2u)
#define DEFAULT_FRAMES 100u

static volatile sig_atomic_t g_should_exit;

struct byte_stats {
    uint8_t min;
    uint8_t max;
    double avg;
};

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

static double us_between(const struct timespec *a, const struct timespec *b)
{
    double sec = (double)(b->tv_sec - a->tv_sec);
    double nsec = (double)(b->tv_nsec - a->tv_nsec);
    return sec * 1e6 + nsec / 1e3;
}

static struct byte_stats calc_stats(const void *data, size_t length)
{
    const uint8_t *p = data;
    struct byte_stats s = {.min = 255, .max = 0, .avg = 0.0};
    uint64_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        if (p[i] < s.min) s.min = p[i];
        if (p[i] > s.max) s.max = p[i];
        sum += p[i];
    }
    if (length > 0) s.avg = (double)sum / (double)length;
    return s;
}

static void ps_edit(struct t1_buf *out_buf)
{
    (void)out_buf;
}

/* Verbatim copy of main.c::copy_nv12_to_display so RGB-convert timing
 * matches production. */
static void copy_nv12_to_display(void *dst, uint32_t dst_pitch,
                                 uint32_t dst_width, uint32_t dst_height,
                                 const void *src)
{
    const uint8_t *src_y_plane  = (const uint8_t *)src;
    const uint8_t *src_uv_plane = (const uint8_t *)src + FRAME_BYTES;
    uint8_t *dst_base = dst;

    uint32_t square = dst_width < dst_height ? dst_width : dst_height;
    if (square == 0) return;
    uint32_t x0 = (dst_width  - square) / 2u;
    uint32_t y0 = (dst_height - square) / 2u;
    uint32_t inv = (128u << 16) / square;

    for (uint32_t y = 0; y < dst_height; y++) {
        uint16_t *dst_line = (uint16_t *)(void *)(dst_base + (size_t)y * dst_pitch);
        memset(dst_line, 0, (size_t)dst_width * sizeof(*dst_line));
        if (y < y0 || y >= y0 + square) continue;

        uint32_t src_yi = ((y - y0) * inv) >> 16;
        if (src_yi > 127u) src_yi = 127u;
        const uint8_t *y_line  = src_y_plane  + (size_t)src_yi * 128u;
        const uint8_t *uv_line = src_uv_plane + (size_t)(src_yi >> 1) * 128u;

        for (uint32_t x = x0; x < x0 + square; x++) {
            uint32_t src_xi = ((x - x0) * inv) >> 16;
            if (src_xi > 127u) src_xi = 127u;
            int Y = y_line[src_xi];
            uint32_t uv_off = src_xi & ~1u;
            int u = (int)uv_line[uv_off]     - 128;
            int v = (int)uv_line[uv_off + 1] - 128;
            int r = Y + ((359 * v) >> 8);
            int g = Y - ((88  * u + 183 * v) >> 8);
            int b = Y + ((454 * u) >> 8);
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            dst_line[x] = (uint16_t)(((r >> 3) << 11) |
                                     ((g >> 2) << 5)  |
                                      (b >> 3));
        }
    }
}

static void copy_gray_to_display(void *dst, uint32_t dst_pitch,
                                 uint32_t dst_width, uint32_t dst_height,
                                 const void *src)
{
    const uint8_t *src_y_plane = (const uint8_t *)src;
    uint8_t *dst_base = dst;

    uint32_t square = dst_width < dst_height ? dst_width : dst_height;
    if (square == 0) return;
    uint32_t x0 = (dst_width  - square) / 2u;
    uint32_t y0 = (dst_height - square) / 2u;
    uint32_t inv = (128u << 16) / square;

    for (uint32_t y = 0; y < dst_height; y++) {
        uint16_t *dst_line = (uint16_t *)(void *)(dst_base + (size_t)y * dst_pitch);
        memset(dst_line, 0, (size_t)dst_width * sizeof(*dst_line));
        if (y < y0 || y >= y0 + square) continue;

        uint32_t src_yi = ((y - y0) * inv) >> 16;
        if (src_yi > 127u) src_yi = 127u;
        const uint8_t *y_line = src_y_plane + (size_t)src_yi * 128u;

        for (uint32_t x = x0; x < x0 + square; x++) {
            uint32_t src_xi = ((x - x0) * inv) >> 16;
            if (src_xi > 127u) src_xi = 127u;
            uint8_t y8 = y_line[src_xi];
            dst_line[x] = (uint16_t)(((uint16_t)(y8 >> 3) << 11) |
                                     ((uint16_t)(y8 >> 2) << 5)  |
                                      (uint16_t)(y8 >> 3));
        }
    }
}

static uint16_t flow_color_rgb565_ps(uint8_t code)
{
    switch (code) {
        case 0:   return 0x0000u;
        case 50:  return 0xf800u;
        case 100: return 0x07ffu;
        case 150: return 0x07e0u;
        case 200: return 0xf81fu;
        default:
            return (uint16_t)(((uint16_t)(code >> 3) << 11) |
                              ((uint16_t)(code >> 2) << 5)  |
                               (uint16_t)(code >> 3));
    }
}

static void copy_flow_to_display(void *dst, uint32_t dst_pitch,
                                 uint32_t dst_width, uint32_t dst_height,
                                 const void *src)
{
    const uint8_t *src_y_plane = (const uint8_t *)src;
    uint8_t *dst_base = dst;

    uint32_t square = dst_width < dst_height ? dst_width : dst_height;
    if (square == 0) return;
    uint32_t x0 = (dst_width  - square) / 2u;
    uint32_t y0 = (dst_height - square) / 2u;
    uint32_t inv = (128u << 16) / square;

    for (uint32_t y = 0; y < dst_height; y++) {
        uint16_t *dst_line = (uint16_t *)(void *)(dst_base + (size_t)y * dst_pitch);
        memset(dst_line, 0, (size_t)dst_width * sizeof(*dst_line));
        if (y < y0 || y >= y0 + square) continue;

        uint32_t src_yi = ((y - y0) * inv) >> 16;
        if (src_yi > 127u) src_yi = 127u;
        const uint8_t *y_line = src_y_plane + (size_t)src_yi * 128u;

        for (uint32_t x = x0; x < x0 + square; x++) {
            uint32_t src_xi = ((x - x0) * inv) >> 16;
            if (src_xi > 127u) src_xi = 127u;
            dst_line[x] = flow_color_rgb565_ps(y_line[src_xi]);
        }
    }
}

static void copy_frame_to_display(void *dst, uint32_t dst_pitch,
                                  uint32_t dst_width, uint32_t dst_height,
                                  const void *src)
{
    if (ACTIVE_KERNEL_FLOW_COLOR) {
        copy_flow_to_display(dst, dst_pitch, dst_width, dst_height, src);
    } else if (ACTIVE_KERNEL_NEUTRAL_UV) {
        copy_gray_to_display(dst, dst_pitch, dst_width, dst_height, src);
    } else {
        copy_nv12_to_display(dst, dst_pitch, dst_width, dst_height, src);
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [--frames N] [--out path] [--device /dev/videoN] [--no-display]\n"
        "  --frames N     measure N frames then exit (default %u, env VISIONSOC_PERF_FRAMES)\n"
        "  --out path     CSV destination (default stdout)\n"
        "  --device path  V4L2 device (default /dev/video0)\n"
        "  --no-display   skip display_open/dq/qb + RGB convert (lets the\n"
        "                 production visionsoc_main keep driving the HDMI;\n"
        "                 disp_dq_us/rgb_conv_us/disp_qb_us land as 0 in CSV)\n",
        argv0, DEFAULT_FRAMES);
}

int main(int argc, char **argv)
{
    unsigned frames = DEFAULT_FRAMES;
    const char *out_path = NULL;
    const char *camera_dev = "/dev/video0";
    int no_display = 0;
    const char *env_frames = getenv("VISIONSOC_PERF_FRAMES");
    if (env_frames && *env_frames) {
        frames = (unsigned)strtoul(env_frames, NULL, 0);
    }

    static const struct option opts[] = {
        {"frames",     required_argument, NULL, 'f'},
        {"out",        required_argument, NULL, 'o'},
        {"device",     required_argument, NULL, 'd'},
        {"no-display", no_argument,       NULL, 'n'},
        {"help",       no_argument,       NULL, 'h'},
        {0, 0, 0, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "f:o:d:nh", opts, NULL)) != -1) {
        switch (c) {
            case 'f': frames = (unsigned)strtoul(optarg, NULL, 0); break;
            case 'o': out_path = optarg; break;
            case 'd': camera_dev = optarg; break;
            case 'n': no_display = 1; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 2;
        }
    }
    if (frames == 0) {
        fprintf(stderr, "frames must be > 0\n");
        return 2;
    }

    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) die("fopen out");
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (t1_init() < 0) die("t1_init");

    struct display disp;
    if (!no_display) {
        if (display_open(&disp) < 0) die("display_open");
    }

    struct t1_buf in_buf = {0};
    if (t1_buf_alloc(&in_buf, FRAME_BYTES) < 0) die("t1_buf_alloc in");

    struct t1_buf out_buf = {0};
    if (t1_buf_alloc(&out_buf, NV12_BYTES) < 0) die("t1_buf_alloc out");

    struct camera cam;
    if (camera_open(&cam, camera_dev, 128, 128) < 0) die("camera_open");
    camera_set_cancel_flag(&cam, &g_should_exit);

#ifdef ACTIVE_KERNEL_NEEDS_WEIGHTS
    /* matmul kernels read a fixed weight matrix B from URAM; stage it once
     * (mirrors main.c). Without this the kernel reads stale URAM. */
    {
        struct t1_buf wbuf = {0};
        if (t1_buf_alloc(&wbuf, FRAME_BYTES) < 0) die("t1_buf_alloc(weights)");
        mmk_build_weights((uint8_t *)wbuf.va, ACTIVE_KERNEL_WEIGHT_PATTERN);
        if (t1_buf_sync_for_device(&wbuf) < 0) die("t1_buf_sync_for_device(weights)");
        if (t1_dma_s2mm_async(0, ACTIVE_KERNEL_WEIGHT_PA, FRAME_BYTES) < 0)
            die("t1_dma_s2mm_async(arm weights)");
        if (t1_dma_mm2s_async(wbuf.pa, 0, FRAME_BYTES) < 0)
            die("t1_dma_mm2s_async(weights)");
        if (t1_dma_wait() < 0) die("t1_dma_wait(weights -> uram)");
        t1_buf_free(&wbuf);
    }
#endif

    fprintf(stderr, "main_perf: capturing %u frames\n", frames);
    fprintf(out,
        "frame,cam_dq_us,stats_in_us,prep_in_us,dma_in_us,t1_kernel_us,"
        "t1_kernel_cycles,dma_out_us,sync_out_us,stats_out_us,uv_fill_us,"
        "disp_dq_us,rgb_conv_us,disp_qb_us,cam_qb_us,frame_total_us\n");

    unsigned frame_count = 0;
    while (!g_should_exit && frame_count < frames) {
        struct timespec t_frame_start, t_a, t_b;
        struct camera_buf cb;

        /* Stage: cam_dq -- blocking V4L2 DQBUF. */
        clock_gettime(CLOCK_MONOTONIC, &t_frame_start);
        t_a = t_frame_start;
        if (camera_dqbuf(&cam, &cb) < 0) {
            if (g_should_exit && errno == ECANCELED) break;
            die("camera_dqbuf");
        }
        clock_gettime(CLOCK_MONOTONIC, &t_b);
        double cam_dq_us = us_between(&t_a, &t_b);

        if (cb.length < FRAME_BYTES ||
            (!cb.uv_va && cb.length < NV12_BYTES) ||
            (cb.uv_va && cb.uv_length < FRAME_BYTES / 2u)) {
            errno = EINVAL;
            die("camera frame too small");
        }

        /* Stage: stats_in -- min/max/avg scan over the input Y plane. */
        t_a = t_b;
        struct byte_stats cam_y_stats = calc_stats(cb.va, FRAME_BYTES);
        (void)cam_y_stats;
        clock_gettime(CLOCK_MONOTONIC, &t_b);
        double stats_in_us = us_between(&t_a, &t_b);

        /* Stage: prep_in -- memcpy into the udmabuf + sync_for_device. */
        t_a = t_b;
        memcpy(in_buf.va, cb.va, FRAME_BYTES);
        if (t1_buf_sync_for_device(&in_buf) < 0) die("sync_for_device in");
        clock_gettime(CLOCK_MONOTONIC, &t_b);
        double prep_in_us = us_between(&t_a, &t_b);

        /* Stage: dma_in -- DDR -> URAM_HALF_A (async-pair pattern). */
        t_a = t_b;
        if (t1_dma_s2mm_async(0, URAM_HALF_A, FRAME_BYTES) < 0) die("dma s2mm arm uram_a");
        if (t1_dma_mm2s_async(in_buf.pa, 0, FRAME_BYTES) < 0)   die("dma mm2s in");
        if (t1_dma_wait() < 0) die("dma wait in->uram_a");
        clock_gettime(CLOCK_MONOTONIC, &t_b);
        double dma_in_us = us_between(&t_a, &t_b);

        /* Stage: t1_kernel -- bracketed with BOTH clock_gettime and the
         * T1 hardware cycle counter so we can subtract to get the
         * libt1/Linux overhead. */
        t_a = t_b;
        __asm__ __volatile__("" ::: "memory");
        volatile uint32_t perf_start_lo = t1_perf_start(1);
        if (issue_active_kernel(URAM_HALF_A, URAM_HALF_B) < 0) die("issue_active_kernel");
        volatile uint32_t t1_kernel_cycles = t1_perf_stop();
        __asm__ __volatile__("" ::: "memory");
        clock_gettime(CLOCK_MONOTONIC, &t_b);
        double t1_kernel_us = us_between(&t_a, &t_b);
        (void)perf_start_lo;

        /* Stage: dma_out -- URAM_HALF_B -> DDR. */
        t_a = t_b;
        if (t1_dma_s2mm_async(0, out_buf.pa, FRAME_BYTES) < 0) die("dma s2mm arm out");
        if (t1_dma_mm2s_async(URAM_HALF_B, 0, FRAME_BYTES) < 0) die("dma mm2s uram_b");
        if (t1_dma_wait() < 0) die("dma wait uram_b->out");
        clock_gettime(CLOCK_MONOTONIC, &t_b);
        double dma_out_us = us_between(&t_a, &t_b);

        /* Stage: sync_out -- invalidate output cache for CPU read. */
        t_a = t_b;
        if (t1_buf_sync_for_cpu(&out_buf) < 0) die("sync_for_cpu out");
        clock_gettime(CLOCK_MONOTONIC, &t_b);
        double sync_out_us = us_between(&t_a, &t_b);

        /* Stage: stats_out -- min/max/avg scan over output Y plane. */
        t_a = t_b;
        struct byte_stats out_y_stats = calc_stats(out_buf.va, FRAME_BYTES);
        (void)out_y_stats;
        clock_gettime(CLOCK_MONOTONIC, &t_b);
        double stats_out_us = us_between(&t_a, &t_b);

        /* Stage: uv_fill -- write UV plane + ps_edit hook. */
        t_a = t_b;
        if (ACTIVE_KERNEL_NEUTRAL_UV) {
            memset((uint8_t *)out_buf.va + FRAME_BYTES, 0x80, FRAME_BYTES / 2u);
        } else {
            const uint8_t *uv = cb.uv_va ?
                                (const uint8_t *)cb.uv_va :
                                (const uint8_t *)cb.va + FRAME_BYTES;
            memcpy((uint8_t *)out_buf.va + FRAME_BYTES, uv, FRAME_BYTES / 2u);
        }
        ps_edit(&out_buf);
        clock_gettime(CLOCK_MONOTONIC, &t_b);
        double uv_fill_us = us_between(&t_a, &t_b);

        /* Stage: disp_dq -- DRM-side wait for a free backbuffer.
         * Skipped under --no-display so we don't fight gdm/Xorg for
         * DRM master. The CSV cells are zeroed in that case. */
        double disp_dq_us = 0.0;
        double rgb_conv_us = 0.0;
        double disp_qb_us = 0.0;
        if (!no_display) {
            t_a = t_b;
            struct display_buf db;
            if (display_dq_for_filling(&disp, &db) < 0) die("display_dq_for_filling");
            clock_gettime(CLOCK_MONOTONIC, &t_b);
            disp_dq_us = us_between(&t_a, &t_b);

            if (db.pitch < db.width * 2u) {
                errno = EINVAL;
                die("display pitch < width*2");
            }

            /* Stage: rgb_conv -- PS RGB565 false-colour/scale pack. */
            t_a = t_b;
            copy_frame_to_display(db.va, db.pitch, db.width, db.height,
                                  out_buf.va);
            clock_gettime(CLOCK_MONOTONIC, &t_b);
            rgb_conv_us = us_between(&t_a, &t_b);

            /* Stage: disp_qb -- page flip. */
            t_a = t_b;
            if (display_qbuf(&disp, &db) < 0) die("display_qbuf");
            clock_gettime(CLOCK_MONOTONIC, &t_b);
            disp_qb_us = us_between(&t_a, &t_b);
        }

        /* Stage: cam_qb -- return the camera buffer. */
        t_a = t_b;
        if (camera_qbuf(&cam, &cb) < 0) die("camera_qbuf");
        clock_gettime(CLOCK_MONOTONIC, &t_b);
        double cam_qb_us = us_between(&t_a, &t_b);

        double frame_total_us = us_between(&t_frame_start, &t_b);

        fprintf(out,
            "%u,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
            frame_count,
            cam_dq_us, stats_in_us, prep_in_us, dma_in_us,
            t1_kernel_us, t1_kernel_cycles,
            dma_out_us, sync_out_us, stats_out_us, uv_fill_us,
            disp_dq_us, rgb_conv_us, disp_qb_us, cam_qb_us,
            frame_total_us);
        if ((frame_count & 0x1Fu) == 0) {
            fprintf(stderr, "  frame %u/%u  total=%.0fus  t1=%.0fus (%u cyc)\n",
                    frame_count, frames, frame_total_us, t1_kernel_us,
                    t1_kernel_cycles);
        }

        frame_count++;
    }

    if (out != stdout) fclose(out);
    fprintf(stderr, "main_perf: captured %u frames\n", frame_count);

    if (!no_display) display_close(&disp);
    camera_close(&cam);
    t1_buf_free(&in_buf);
    t1_buf_free(&out_buf);
    t1_close();
    return 0;
}
