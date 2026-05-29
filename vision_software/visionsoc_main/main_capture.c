/*
 * main_capture -- screenshot-capturing twin of main.c.
 *
 * Runs the full camera->display pipeline like main.c, and every
 * --every frames writes four PPM files to --out-dir capturing the
 * visible algorithm state at native 128x128 plus the final HDMI-output
 * framebuffer:
 *
 *   capture_NNNNNN_camera_y.ppm
 *     128x128 raw camera Y plane as grayscale RGB888 -- exactly what
 *     T1 reads as input (greyscale view of the scene).
 *
 *   capture_NNNNNN_camera_color.ppm
 *     128x128 camera NV12 (Y + UV) rendered as YUV->RGB888 -- the
 *     pre-T1 colour view of the scene.
 *
 *   capture_NNNNNN_t1_output.ppm
 *     128x128 T1 output rendered with the same colour logic as
 *     copy_frame_to_display, so the algorithm result is visible at
 *     native resolution without the display-side letterboxing.
 *
 *   capture_NNNNNN_display.ppm
 *     Full HDMI framebuffer (RGB565 unpacked to RGB888) -- exactly
 *     what is rendered on the monitor, including the letterboxed
 *     128x128 source in the centered square.
 *
 * Exits after --frames N (default 100); captures frame 0, --every,
 * 2*--every, ... (default --every 10). Output dir via --out-dir
 * (default /tmp/capture).
 *
 * Behaviour outside the capture sites is identical to main.c so the
 * pipeline observed on the monitor exactly matches the captured frames.
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
#include <sys/stat.h>
#include <time.h>

#ifndef ACTIVE_KERNEL_FLOW_COLOR
#define ACTIVE_KERNEL_FLOW_COLOR 0
#endif

#define URAM_BASE_PA 0xA0080000u
#define URAM_HALF_A  (URAM_BASE_PA + 0x00000u)
#define URAM_HALF_B  (URAM_BASE_PA + 0x04000u)

#define FRAME_BYTES  (128u * 128u)
#define NV12_BYTES   (FRAME_BYTES + FRAME_BYTES / 2u)
#define DEFAULT_FRAMES 300u
#define DEFAULT_EVERY  30u
#define DEFAULT_OUTDIR "/tmp/capture"

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

static double elapsed_seconds(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) +
           (double)(b->tv_nsec - a->tv_nsec) / 1000000000.0;
}

static void ps_edit(struct t1_buf *out_buf)
{
    (void)out_buf;
}

/*
 * Verbatim copies of main.c's display-path renderers, so the HDMI
 * output during a capture run is byte-identical to production main.c.
 */
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

/*
 * PPM writers. P6 (binary RGB888) is the simplest "no dependencies"
 * format and converts cleanly to PNG on the host via Pillow.
 */
static int write_ppm(const char *path, const uint8_t *rgb888,
                     uint32_t width, uint32_t height)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fprintf(f, "P6\n%u %u\n255\n", width, height) < 0) {
        fclose(f);
        return -1;
    }
    size_t want = (size_t)width * (size_t)height * 3u;
    if (fwrite(rgb888, 1, want, f) != want) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) return -1;
    return 0;
}

/* 128x128 Y plane -> 128x128 grayscale RGB888 (one byte triplicated). */
static void render_y_to_rgb888(uint8_t *dst, const uint8_t *src_y)
{
    for (uint32_t i = 0; i < FRAME_BYTES; i++) {
        uint8_t y = src_y[i];
        dst[3*i + 0] = y;
        dst[3*i + 1] = y;
        dst[3*i + 2] = y;
    }
}

/*
 * 128x128 NV12 (Y plane + half-res interleaved UV plane) -> 128x128
 * RGB888. Same JFIF BT.601 math as copy_nv12_to_display, just at
 * native resolution with no letterboxing/scaling.
 */
static void render_nv12_to_rgb888(uint8_t *dst,
                                  const uint8_t *src_y,
                                  const uint8_t *src_uv)
{
    for (uint32_t row = 0; row < 128u; row++) {
        const uint8_t *y_line  = src_y  + (size_t)row * 128u;
        const uint8_t *uv_line = src_uv + (size_t)(row >> 1) * 128u;
        uint8_t *dst_line = dst + (size_t)row * 128u * 3u;
        for (uint32_t col = 0; col < 128u; col++) {
            int Y = y_line[col];
            uint32_t uv_off = col & ~1u;
            int u = (int)uv_line[uv_off]     - 128;
            int v = (int)uv_line[uv_off + 1] - 128;
            int r = Y + ((359 * v) >> 8);
            int g = Y - ((88  * u + 183 * v) >> 8);
            int b = Y + ((454 * u) >> 8);
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            dst_line[3*col + 0] = (uint8_t)r;
            dst_line[3*col + 1] = (uint8_t)g;
            dst_line[3*col + 2] = (uint8_t)b;
        }
    }
}

/*
 * 128x128 T1 output bytes -> 128x128 RGB888, rendered with the same
 * colour decision as copy_frame_to_display so the saved image matches
 * how the algorithm result appears on the monitor (but at native res).
 *
 *   flow-colour kernels  : false-colour LUT (matches copy_flow_to_display)
 *   neutral-UV kernels   : grayscale (matches copy_gray_to_display)
 *   full-colour kernels  : T1 Y + camera UV via YUV->RGB
 *                          (matches copy_nv12_to_display)
 */
static void render_t1_output_to_rgb888(uint8_t *dst,
                                       const uint8_t *t1_y,
                                       const uint8_t *camera_uv)
{
    if (ACTIVE_KERNEL_FLOW_COLOR) {
        for (uint32_t i = 0; i < FRAME_BYTES; i++) {
            uint16_t rgb565 = flow_color_rgb565_ps(t1_y[i]);
            uint8_t r5 = (uint8_t)((rgb565 >> 11) & 0x1Fu);
            uint8_t g6 = (uint8_t)((rgb565 >> 5)  & 0x3Fu);
            uint8_t b5 = (uint8_t)( rgb565        & 0x1Fu);
            dst[3*i + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
            dst[3*i + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
            dst[3*i + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
        }
    } else if (ACTIVE_KERNEL_NEUTRAL_UV) {
        render_y_to_rgb888(dst, t1_y);
    } else {
        render_nv12_to_rgb888(dst, t1_y, camera_uv);
    }
}

/*
 * Full HDMI framebuffer (RGB565, pitch >= width*2) -> RGB888.
 * Allocated by the caller so we don't malloc per capture frame.
 */
static void render_rgb565_to_rgb888(uint8_t *dst, const void *src,
                                    uint32_t width, uint32_t height,
                                    uint32_t pitch)
{
    const uint8_t *src_base = src;
    for (uint32_t y = 0; y < height; y++) {
        const uint16_t *src_line = (const uint16_t *)(const void *)
                                       (src_base + (size_t)y * pitch);
        uint8_t *dst_line = dst + (size_t)y * width * 3u;
        for (uint32_t x = 0; x < width; x++) {
            uint16_t pixel = src_line[x];
            uint8_t r5 = (uint8_t)((pixel >> 11) & 0x1Fu);
            uint8_t g6 = (uint8_t)((pixel >> 5)  & 0x3Fu);
            uint8_t b5 = (uint8_t)( pixel        & 0x1Fu);
            dst_line[3*x + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
            dst_line[3*x + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
            dst_line[3*x + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
        }
    }
}

static int mkdir_p(const char *path)
{
    if (mkdir(path, 0755) == 0) return 0;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    }
    return -1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [--frames N] [--every M] [--out-dir PATH] [--device /dev/videoN]\n"
        "  --frames N      run N frames then exit (default %u)\n"
        "  --every M       capture every Mth frame, including frame 0 (default %u)\n"
        "  --out-dir PATH  destination dir for PPM files (default %s)\n"
        "  --device PATH   V4L2 device (default /dev/video0)\n",
        argv0, DEFAULT_FRAMES, DEFAULT_EVERY, DEFAULT_OUTDIR);
}

int main(int argc, char **argv)
{
    unsigned frames = DEFAULT_FRAMES;
    unsigned every = DEFAULT_EVERY;
    const char *out_dir = DEFAULT_OUTDIR;
    const char *camera_dev = "/dev/video0";

    static const struct option opts[] = {
        {"frames",  required_argument, NULL, 'f'},
        {"every",   required_argument, NULL, 'e'},
        {"out-dir", required_argument, NULL, 'o'},
        {"device",  required_argument, NULL, 'd'},
        {"help",    no_argument,       NULL, 'h'},
        {0, 0, 0, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "f:e:o:d:h", opts, NULL)) != -1) {
        switch (c) {
            case 'f': frames = (unsigned)strtoul(optarg, NULL, 0); break;
            case 'e': every  = (unsigned)strtoul(optarg, NULL, 0); break;
            case 'o': out_dir = optarg; break;
            case 'd': camera_dev = optarg; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 2;
        }
    }
    if (frames == 0) {
        fprintf(stderr, "--frames must be > 0\n"); return 2;
    }
    if (every == 0) {
        fprintf(stderr, "--every must be > 0\n"); return 2;
    }
    if (mkdir_p(out_dir) < 0) {
        die("mkdir_p out-dir");
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (t1_init() < 0) die("t1_init");

    struct display disp;
    if (display_open(&disp) < 0) die("display_open");

    struct t1_buf in_buf = {0};
    if (t1_buf_alloc(&in_buf, FRAME_BYTES) < 0) die("t1_buf_alloc in");
    struct t1_buf out_buf = {0};
    if (t1_buf_alloc(&out_buf, NV12_BYTES) < 0) die("t1_buf_alloc out");

    struct camera cam;
    if (camera_open(&cam, camera_dev, 128, 128) < 0) die("camera_open");
    camera_set_cancel_flag(&cam, &g_should_exit);

#ifdef ACTIVE_KERNEL_NEEDS_WEIGHTS
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

    /*
     * Persistent RGB888 scratch -- sized for the worst case (full
     * display framebuffer). The 128x128 native PPMs reuse the head
     * of the same buffer, so we malloc once.
     */
    uint8_t *rgb_native = malloc((size_t)128 * 128 * 3u);
    if (!rgb_native) die("malloc rgb_native");
    uint8_t *rgb_display = NULL;
    uint32_t rgb_display_w = 0, rgb_display_h = 0;

    fprintf(stderr, "main_capture: frames=%u every=%u out-dir=%s\n",
            frames, every, out_dir);

    unsigned frame_count = 0;
    unsigned next_capture = 0;
    unsigned capture_count = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (!g_should_exit && frame_count < frames) {
        struct camera_buf cb;
        if (camera_dqbuf(&cam, &cb) < 0) {
            if (g_should_exit && errno == ECANCELED) break;
            die("camera_dqbuf");
        }
        if (cb.length < FRAME_BYTES ||
            (!cb.uv_va && cb.length < NV12_BYTES) ||
            (cb.uv_va && cb.uv_length < FRAME_BYTES / 2u)) {
            errno = EINVAL;
            die("camera frame too small");
        }

        memcpy(in_buf.va, cb.va, FRAME_BYTES);
        if (t1_buf_sync_for_device(&in_buf) < 0) die("t1_buf_sync_for_device(in)");

        /* DDR -> URAM_HALF_A. */
        if (t1_dma_s2mm_async(0, URAM_HALF_A, FRAME_BYTES) < 0)
            die("t1_dma_s2mm_async(arm uram_a)");
        if (t1_dma_mm2s_async(in_buf.pa, 0, FRAME_BYTES) < 0)
            die("t1_dma_mm2s_async(in)");
        if (t1_dma_wait() < 0) die("t1_dma_wait(in -> uram_a)");

        /* URAM_HALF_A -> T1 -> URAM_HALF_B. */
        if (issue_active_kernel(URAM_HALF_A, URAM_HALF_B) < 0)
            die("issue_active_kernel");

        /* URAM_HALF_B -> DDR. */
        if (t1_dma_s2mm_async(0, out_buf.pa, FRAME_BYTES) < 0)
            die("t1_dma_s2mm_async(arm out)");
        if (t1_dma_mm2s_async(URAM_HALF_B, 0, FRAME_BYTES) < 0)
            die("t1_dma_mm2s_async(uram_b)");
        if (t1_dma_wait() < 0) die("t1_dma_wait(uram_b -> out)");
        if (t1_buf_sync_for_cpu(&out_buf) < 0) die("t1_buf_sync_for_cpu(out)");

        const uint8_t *camera_uv = cb.uv_va ?
                                   (const uint8_t *)cb.uv_va :
                                   (const uint8_t *)cb.va + FRAME_BYTES;
        if (ACTIVE_KERNEL_NEUTRAL_UV) {
            memset((uint8_t *)out_buf.va + FRAME_BYTES, 0x80, FRAME_BYTES / 2u);
        } else {
            memcpy((uint8_t *)out_buf.va + FRAME_BYTES, camera_uv, FRAME_BYTES / 2u);
        }
        ps_edit(&out_buf);

        struct display_buf db;
        if (display_dq_for_filling(&disp, &db) < 0) die("display_dq_for_filling");
        if (db.pitch < db.width * 2u) {
            errno = EINVAL;
            die("display pitch < width*2");
        }
        copy_frame_to_display(db.va, db.pitch, db.width, db.height, out_buf.va);

        /*
         * Capture sites. Snap AFTER the display compose so db.va holds
         * the rendered frame, but BEFORE display_qbuf so we are still
         * the owner of the buffer.
         */
        if (frame_count == next_capture) {
            char path[512];

            snprintf(path, sizeof(path),
                     "%s/capture_%06u_camera_y.ppm", out_dir, frame_count);
            render_y_to_rgb888(rgb_native, cb.va);
            if (write_ppm(path, rgb_native, 128, 128) < 0)
                die("write_ppm camera_y");

            snprintf(path, sizeof(path),
                     "%s/capture_%06u_camera_color.ppm", out_dir, frame_count);
            render_nv12_to_rgb888(rgb_native, cb.va, camera_uv);
            if (write_ppm(path, rgb_native, 128, 128) < 0)
                die("write_ppm camera_color");

            snprintf(path, sizeof(path),
                     "%s/capture_%06u_t1_output.ppm", out_dir, frame_count);
            render_t1_output_to_rgb888(rgb_native, out_buf.va, camera_uv);
            if (write_ppm(path, rgb_native, 128, 128) < 0)
                die("write_ppm t1_output");

            if (!rgb_display ||
                rgb_display_w != db.width || rgb_display_h != db.height) {
                free(rgb_display);
                size_t need = (size_t)db.width * db.height * 3u;
                rgb_display = malloc(need);
                if (!rgb_display) die("malloc rgb_display");
                rgb_display_w = db.width;
                rgb_display_h = db.height;
            }
            snprintf(path, sizeof(path),
                     "%s/capture_%06u_display.ppm", out_dir, frame_count);
            render_rgb565_to_rgb888(rgb_display, db.va,
                                    db.width, db.height, db.pitch);
            if (write_ppm(path, rgb_display, db.width, db.height) < 0)
                die("write_ppm display");

            capture_count++;
            fprintf(stderr,
                    "  capture %u: frame %u/%u -> 4 PPMs (display %ux%u)\n",
                    capture_count, frame_count, frames,
                    db.width, db.height);
            next_capture += every;
        }

        if (display_qbuf(&disp, &db) < 0) die("display_qbuf");
        if (camera_qbuf(&cam, &cb) < 0) die("camera_qbuf");

        frame_count++;
        if ((frame_count & 0x1Fu) == 0) {
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double seconds = elapsed_seconds(&t0, &t1);
            double fps = seconds > 0.0 ? 32.0 / seconds : 0.0;
            fprintf(stderr, "  frame %u/%u, %.1f fps\n",
                    frame_count, frames, fps);
            t0 = t1;
        }
    }

    fprintf(stderr, "main_capture: ran %u frames, captured %u snapshots to %s\n",
            frame_count, capture_count, out_dir);

    free(rgb_native);
    free(rgb_display);
    display_close(&disp);
    camera_close(&cam);
    t1_buf_free(&in_buf);
    t1_buf_free(&out_buf);
    t1_close();
    return 0;
}
