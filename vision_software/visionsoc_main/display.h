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
    uint32_t pa;
};

struct display {
    int drm_fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    drmModeModeInfo mode;
    int front_index;
    struct display_fb fbs[DISPLAY_NUM_BUFS];
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
