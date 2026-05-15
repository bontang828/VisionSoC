#pragma once

#include <stddef.h>
#include <stdint.h>
#include <xf86drmMode.h>

#define DISPLAY_NUM_BUFS 2

struct display_fb {
    uint32_t handle;
    uint32_t fb_id;
    uint32_t pitch;
    uint64_t size;
    void *va;
    uint32_t pa;   /* unused in the PS-memcpy flow; kept for a future DMA path */
};

struct display {
    int drm_fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t video_plane_id;        /* overlay/video plane that accepts NV12 */
    drmModeModeInfo mode;
    int scale_to_fit;               /* kept for future scaling experiments */
    int front_index;
    struct display_fb primary_fb;   /* mode-sized RGB565, drives the CRTC/primary plane */
    struct display_fb fbs[DISPLAY_NUM_BUFS]; /* mode-sized NV12 video-plane buffers */
};

struct display_buf {
    int index;
    void *va;
    uint32_t pa;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
};

int display_open(struct display *disp);
void display_close(struct display *disp);
int display_dq_for_filling(struct display *disp, struct display_buf *db);
int display_qbuf(struct display *disp, const struct display_buf *db);
