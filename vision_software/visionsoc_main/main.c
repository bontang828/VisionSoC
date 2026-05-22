#include "camera.h"
#include "display.h"
#include "kernels/active_kernel.h"
#include "libt1.h"
#include "libt1_regs.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef ACTIVE_KERNEL_FLOW_COLOR
#define ACTIVE_KERNEL_FLOW_COLOR 0
#endif

/*
 * 5r scratchpad path: T1 reads its input from URAM half A and writes
 * its output to URAM half B. The PS uses DMA mm2s to stage the camera Y
 * plane from a DDR udmabuf into half A before issuing the kernel, then
 * DMA s2mm to drain half B back to a DDR udmabuf for ps_edit / display.
 * Arena is 512 KB at PA 0xA0080000 (was 32 KB on 5q-r3 BRAM); each half
 * is still 16 KB = one FRAME_BYTES Y-plane, so the upper 480 KB of the
 * arena is available for future multi-frame staging without re-flashing.
 */
#define URAM_BASE_PA 0xA0080000u
#define URAM_HALF_A  (URAM_BASE_PA + 0x00000u)
#define URAM_HALF_B  (URAM_BASE_PA + 0x04000u)

#define FRAME_BYTES  (128u * 128u)                    /* Y plane: one 128x128 greyscale image */
#define NV12_BYTES   (FRAME_BYTES + FRAME_BYTES / 2u) /* full NV12 frame: Y + half-res UV (24576) */

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

static void usage(const char *argv0)
{
    fprintf(stderr, "Usage: %s [camera-device]\n", argv0);
}

static double elapsed_seconds(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) +
           (double)(b->tv_nsec - a->tv_nsec) / 1000000000.0;
}

static int env_enabled(const char *name)
{
    const char *value = getenv(name);
    return value && strcmp(value, "0") != 0 && strcmp(value, "false") != 0;
}

static struct byte_stats calc_stats(const void *data, size_t length)
{
    const uint8_t *p = data;
    struct byte_stats s = {.min = 255, .max = 0, .avg = 0.0};
    uint64_t sum = 0;

    for (size_t i = 0; i < length; i++) {
        if (p[i] < s.min) {
            s.min = p[i];
        }
        if (p[i] > s.max) {
            s.max = p[i];
        }
        sum += p[i];
    }

    if (length > 0) {
        s.avg = (double)sum / (double)length;
    }
    return s;
}

/*
 * PS-side edit hook. `out` holds a complete, CPU-coherent NV12 frame when
 * this is called. No-op for now -- this is where future PS-side processing
 * (annotation, format tweaks, etc.) belongs before the frame is displayed.
 */
static void ps_edit(struct t1_buf *out)
{
    (void)out;
}

/*
 * Convert and scale the 128x128 NV12 frame in `src` into the active DRM
 * mode-sized RGB565 buffer at `dst`. The 128-pixel image is letterboxed
 * into the largest centered square that fits, with black bars filling
 * the rest of the row.
 *
 * Colour: AP1302 emits JFIF full-range BT.601 on this carrier (see
 * fpga_build_status.md s 0.6.6), so we use the JFIF (not limited-range)
 * coefficients. Math is in 8.8 fixed point; the inner loop replaces the
 * scaling division with a precomputed 16.16 inverse + multiply + shift.
 */
static void copy_nv12_to_display(void *dst, uint32_t dst_pitch,
                                 uint32_t dst_width, uint32_t dst_height,
                                 const void *src)
{
    const uint8_t *src_y_plane  = (const uint8_t *)src;
    const uint8_t *src_uv_plane = (const uint8_t *)src + FRAME_BYTES;
    uint8_t *dst_base = dst;

    uint32_t square = dst_width < dst_height ? dst_width : dst_height;
    if (square == 0) {
        return;
    }
    uint32_t x0 = (dst_width  - square) / 2u;
    uint32_t y0 = (dst_height - square) / 2u;

    /* 16.16 inverse of `square` so the inner loop replaces a divide with
     * one multiply + shift: src_idx = ((dst_idx - origin) * inv) >> 16. */
    uint32_t inv = (128u << 16) / square;

    for (uint32_t y = 0; y < dst_height; y++) {
        uint16_t *dst_line = (uint16_t *)(void *)(dst_base + (size_t)y * dst_pitch);
        memset(dst_line, 0, (size_t)dst_width * sizeof(*dst_line));

        if (y < y0 || y >= y0 + square) {
            continue;
        }

        uint32_t src_yi = ((y - y0) * inv) >> 16;
        if (src_yi > 127u) {
            src_yi = 127u;
        }
        const uint8_t *y_line  = src_y_plane  + (size_t)src_yi * 128u;
        /* NV12 chroma: half-resolution interleaved Cb/Cr, row stride 128. */
        const uint8_t *uv_line = src_uv_plane + (size_t)(src_yi >> 1) * 128u;

        for (uint32_t x = x0; x < x0 + square; x++) {
            uint32_t src_xi = ((x - x0) * inv) >> 16;
            if (src_xi > 127u) {
                src_xi = 127u;
            }

            int Y = y_line[src_xi];
            /* One CbCr pair covers two luma columns. */
            uint32_t uv_off = src_xi & ~1u;
            int u = (int)uv_line[uv_off]     - 128;
            int v = (int)uv_line[uv_off + 1] - 128;

            /* JFIF full-range BT.601, 8.8 fixed point (coef * 256). */
            int r = Y + ((359 * v) >> 8);
            int g = Y - ((88  * u + 183 * v) >> 8);
            int b = Y + ((454 * u) >> 8);

            if (r < 0) {
                r = 0;
            } else if (r > 255) {
                r = 255;
            }
            if (g < 0) {
                g = 0;
            } else if (g > 255) {
                g = 255;
            }
            if (b < 0) {
                b = 0;
            } else if (b > 255) {
                b = 255;
            }

            dst_line[x] = (uint16_t)(((r >> 3) << 11) |
                                     ((g >> 2) << 5)  |
                                      (b >> 3));
        }
    }
}

/*
 * Faster display path for luma-only kernels (sobel/optical_flow). The DRM
 * buffer is still RGB565, so we still pack pixels, but neutral UV means the
 * expensive YUV chroma math is unnecessary.
 */
static void copy_gray_to_display(void *dst, uint32_t dst_pitch,
                                 uint32_t dst_width, uint32_t dst_height,
                                 const void *src)
{
    const uint8_t *src_y_plane = (const uint8_t *)src;
    uint8_t *dst_base = dst;

    uint32_t square = dst_width < dst_height ? dst_width : dst_height;
    if (square == 0) {
        return;
    }
    uint32_t x0 = (dst_width  - square) / 2u;
    uint32_t y0 = (dst_height - square) / 2u;
    uint32_t inv = (128u << 16) / square;

    for (uint32_t y = 0; y < dst_height; y++) {
        uint16_t *dst_line = (uint16_t *)(void *)(dst_base + (size_t)y * dst_pitch);
        memset(dst_line, 0, (size_t)dst_width * sizeof(*dst_line));

        if (y < y0 || y >= y0 + square) {
            continue;
        }

        uint32_t src_yi = ((y - y0) * inv) >> 16;
        if (src_yi > 127u) {
            src_yi = 127u;
        }
        const uint8_t *y_line = src_y_plane + (size_t)src_yi * 128u;

        for (uint32_t x = x0; x < x0 + square; x++) {
            uint32_t src_xi = ((x - x0) * inv) >> 16;
            if (src_xi > 127u) {
                src_xi = 127u;
            }

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
        case 0:   return 0x0000u;  /* static */
        case 50:  return 0xf800u;  /* right: red */
        case 100: return 0x07ffu;  /* left: cyan */
        case 150: return 0x07e0u;  /* down: green */
        case 200: return 0xf81fu;  /* up: magenta */
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
    if (square == 0) {
        return;
    }
    uint32_t x0 = (dst_width  - square) / 2u;
    uint32_t y0 = (dst_height - square) / 2u;
    uint32_t inv = (128u << 16) / square;

    for (uint32_t y = 0; y < dst_height; y++) {
        uint16_t *dst_line = (uint16_t *)(void *)(dst_base + (size_t)y * dst_pitch);
        memset(dst_line, 0, (size_t)dst_width * sizeof(*dst_line));

        if (y < y0 || y >= y0 + square) {
            continue;
        }

        uint32_t src_yi = ((y - y0) * inv) >> 16;
        if (src_yi > 127u) {
            src_yi = 127u;
        }
        const uint8_t *y_line = src_y_plane + (size_t)src_yi * 128u;

        for (uint32_t x = x0; x < x0 + square; x++) {
            uint32_t src_xi = ((x - x0) * inv) >> 16;
            if (src_xi > 127u) {
                src_xi = 127u;
            }

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

static void draw_debug_marker(void *dst, uint32_t pitch,
                              uint32_t width, uint32_t height)
{
    uint8_t *base = dst;
    uint32_t border = height >= 720 ? 24u : 8u;
    uint32_t mid_x = width / 2u;
    uint32_t mid_y = height / 2u;

    for (uint32_t y = 0; y < height; y++) {
        uint16_t *line = (uint16_t *)(void *)(base + (size_t)y * pitch);
        for (uint32_t x = 0; x < width; x++) {
            int on_border = x < border || y < border ||
                            x >= width - border || y >= height - border;
            int on_cross = (x >= mid_x && x < mid_x + border) ||
                           (y >= mid_y && y < mid_y + border);
            if (on_border || on_cross) {
                line[x] = 0xffff;
            }
        }
    }
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

    int bypass_t1 = env_enabled("VISIONSOC_BYPASS_T1");
    int marker = env_enabled("VISIONSOC_MARKER");
    if (bypass_t1) {
        fprintf(stderr, "debug: VISIONSOC_BYPASS_T1=1, PS copies Y plane instead of T1\n");
    }
    if (marker) {
        fprintf(stderr, "debug: VISIONSOC_MARKER=1, drawing white display marker\n");
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (t1_init() < 0) {
        die("t1_init");
    }

    struct display disp;
    if (display_open(&disp) < 0) {
        die("display_open");
    }

    /*
     * V4L2 MMAP buffers do not expose a reliable physical address to
     * userspace on this kernel, so the PS stages the camera Y plane into
     * a known udmabuf before T1 reads it. T1 still reads DDR and writes
     * DDR; only the unsafe V4L2-PA shortcut is avoided.
     */
    struct t1_buf in = {0};
    if (t1_buf_alloc(&in, FRAME_BYTES) < 0) {
        die("t1_buf_alloc(in)");
    }

    struct t1_buf out = {0};
    if (t1_buf_alloc(&out, NV12_BYTES) < 0) {
        die("t1_buf_alloc(out)");
    }

    struct camera cam;
    if (camera_open(&cam, camera_dev, 128, 128) < 0) {
        die("camera_open");
    }
    camera_set_cancel_flag(&cam, &g_should_exit);

#ifdef ACTIVE_KERNEL_NEEDS_WEIGHTS
    /*
     * matmul kernels read a fixed 128x128 weight matrix B from an internal
     * URAM region (the kernel's (a1) -> ACTIVE_KERNEL_WEIGHT_PA). B is
     * read-only to the kernel, so build it once on the PS, DMA it into URAM
     * via the same async-pair pattern used for the camera frame, then free
     * the DDR staging buffer -- the URAM copy persists across every frame.
     */
    {
        struct t1_buf wbuf = {0};
        if (t1_buf_alloc(&wbuf, FRAME_BYTES) < 0) {
            die("t1_buf_alloc(weights)");
        }
        mmk_build_weights((uint8_t *)wbuf.va, ACTIVE_KERNEL_WEIGHT_PATTERN);
        if (t1_buf_sync_for_device(&wbuf) < 0) {
            die("t1_buf_sync_for_device(weights)");
        }
        if (t1_dma_s2mm_async(0, ACTIVE_KERNEL_WEIGHT_PA, FRAME_BYTES) < 0) {
            die("t1_dma_s2mm_async(arm weights)");
        }
        if (t1_dma_mm2s_async(wbuf.pa, 0, FRAME_BYTES) < 0) {
            die("t1_dma_mm2s_async(weights)");
        }
        if (t1_dma_wait() < 0) {
            die("t1_dma_wait(weights -> uram)");
        }
        t1_buf_free(&wbuf);
        fprintf(stderr,
                "matmul: staged 128x128 weight matrix B (pattern %d) -> 0x%08x\n",
                (int)ACTIVE_KERNEL_WEIGHT_PATTERN,
                (unsigned)ACTIVE_KERNEL_WEIGHT_PA);
    }
#endif

    unsigned frame_count = 0;
    uint32_t last_kernel_cycles = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (!g_should_exit) {
        struct camera_buf cb;
        if (camera_dqbuf(&cam, &cb) < 0) {
            if (g_should_exit && errno == ECANCELED) {
                break;
            }
            die("camera_dqbuf");
        }
        if (cb.length < FRAME_BYTES ||
            (!cb.uv_va && cb.length < NV12_BYTES) ||
            (cb.uv_va && cb.uv_length < FRAME_BYTES / 2u)) {
            errno = EINVAL;
            die("camera frame smaller than one NV12 frame");
        }

        struct byte_stats cam_y_stats = calc_stats(cb.va, FRAME_BYTES);

        memcpy(in.va, cb.va, FRAME_BYTES);
        if (t1_buf_sync_for_device(&in) < 0) {
            die("t1_buf_sync_for_device(in)");
        }

        if (bypass_t1) {
            /* Debug bypass: PS copies the Y plane into out and skips
             * the URAM/T1 round trip entirely. */
            memcpy(out.va, in.va, FRAME_BYTES);
            last_kernel_cycles = 0;
        } else {
            /*
             * Stage 1 (DDR -> URAM half A): the visionsoc DMA fabric is a
             * loopback (MM2S M_AXIS feeds back into S2MM S_AXIS via
             * axis_reg_slice_dma), so a DDR->URAM transfer needs BOTH
             * channels armed: s2mm with the URAM destination, mm2s with
             * the DDR source. t1_dma_mm2s/s2mm_sync each program only one
             * channel (the *_async helpers ignore the unused address), so
             * we use the async-pair-plus-wait pattern from
             * libt1/test/dma_to_scratchpad.c.
             */
            if (t1_dma_s2mm_async(0, URAM_HALF_A, FRAME_BYTES) < 0) {
                die("t1_dma_s2mm_async(arm uram_a)");
            }
            if (t1_dma_mm2s_async(in.pa, 0, FRAME_BYTES) < 0) {
                die("t1_dma_mm2s_async(in)");
            }
            if (t1_dma_wait() < 0) {
                die("t1_dma_wait(in -> uram_a)");
            }

            /*
             * Stage 2 (URAM half A -> T1 -> URAM half B): the kernel reads
             * (a0) from URAM_HALF_A and writes (a1) to URAM_HALF_B over
             * T1's m_axi_hb, which is wired through smartconnect_hb/M01
             * directly to the URAM-backed bram_ctrl - no DDR round trip.
             */
            (void)t1_perf_start(1);
            if (issue_active_kernel(URAM_HALF_A, URAM_HALF_B) < 0) {
                die("issue_active_kernel(uram_a -> uram_b)");
            }
            last_kernel_cycles = t1_perf_stop();

            /*
             * Stage 3 (URAM half B -> DDR): same async-pair pattern, this
             * time s2mm writes the DDR out buffer and mm2s reads the URAM
             * source. Invalidate the udmabuf before the CPU reads so it
             * sees DMA's writes, not stale cache.
             */
            if (t1_dma_s2mm_async(0, out.pa, FRAME_BYTES) < 0) {
                die("t1_dma_s2mm_async(arm out)");
            }
            if (t1_dma_mm2s_async(URAM_HALF_B, 0, FRAME_BYTES) < 0) {
                die("t1_dma_mm2s_async(uram_b)");
            }
            if (t1_dma_wait() < 0) {
                die("t1_dma_wait(uram_b -> out)");
            }
            if (t1_buf_sync_for_cpu(&out) < 0) {
                die("t1_buf_sync_for_cpu(out)");
            }
        }
        struct byte_stats out_y_stats = calc_stats(out.va, FRAME_BYTES);

        /*
         * The T1 kernel only writes the Y plane. Fill the NV12 UV plane on
         * the PS side so `out` is a complete NV12 frame for the display.
         * The active kernel decides whether to keep the camera's chroma
         * (full-colour passthrough) or use neutral 0x80 (grayscale).
         */
        if (ACTIVE_KERNEL_NEUTRAL_UV) {
            memset((uint8_t *)out.va + FRAME_BYTES, 0x80, FRAME_BYTES / 2u);
        } else {
            const uint8_t *uv = cb.uv_va ?
                                (const uint8_t *)cb.uv_va :
                                (const uint8_t *)cb.va + FRAME_BYTES;
            memcpy((uint8_t *)out.va + FRAME_BYTES, uv, FRAME_BYTES / 2u);
        }

        /* PS-side edit hook -- no-op for now. */
        ps_edit(&out);

        struct display_buf db;
        if (display_dq_for_filling(&disp, &db) < 0) {
            die("display_dq_for_filling");
        }
        if (db.pitch < db.width * 2u) {
            errno = EINVAL;
            die("display buffer pitch smaller than display width");
        }
        /*
         * Display path stays on the PS. Optical flow uses a false-colour
         * RGB565 map, other luma-only kernels use grayscale, and full-colour
         * kernels scale and colour-convert NV12. We do not DMA URAM -> DRM
         * directly because the display buffer requires letterboxing + RGB565
         * packing/conversion the DMA can't do.
         */
        /* T1 colour-pack helper kept in kernels/flow_color_rgb565.S, but the
         * active path is PS false-colour because it is faster in the measured
         * pipeline. */
        copy_frame_to_display(db.va, db.pitch, db.width, db.height, out.va);
        if (marker) {
            draw_debug_marker(db.va, db.pitch, db.width, db.height);
        }
        if (display_qbuf(&disp, &db) < 0) {
            die("display_qbuf");
        }
        if (camera_qbuf(&cam, &cb) < 0) {
            die("camera_qbuf");
        }

        frame_count++;
        if ((frame_count & 0x1Fu) == 0) {
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double seconds = elapsed_seconds(&t0, &t1);
            double fps = seconds > 0.0 ? 32.0 / seconds : 0.0;
            printf("frame %u, last kernel %u cycles, %.1f fps, "
                   "camY %u/%u/%.1f, outY %u/%u/%.1f\n",
                   frame_count, last_kernel_cycles, fps,
                   cam_y_stats.min, cam_y_stats.max, cam_y_stats.avg,
                   out_y_stats.min, out_y_stats.max, out_y_stats.avg);
            t0 = t1;
        }
    }

    display_close(&disp);
    camera_close(&cam);
    t1_buf_free(&in);
    t1_buf_free(&out);
    t1_close();
    return 0;
}
