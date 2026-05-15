#pragma once

#include <stddef.h>
#include <stdint.h>
#include <signal.h>

#define CAMERA_MAX_BUFS 8

struct camera_mmap_buf {
    void *va;
    void *uv_va;
    uint32_t pa;
    size_t length;
    size_t uv_length;
};

struct camera {
    int fd;
    int width;
    int height;
    uint32_t pixfmt;
    uint32_t buf_type;
    unsigned nplanes;
    unsigned nbufs;
    int streaming;
    volatile sig_atomic_t *cancel;
    struct camera_mmap_buf bufs[CAMERA_MAX_BUFS];
};

struct camera_buf {
    unsigned index;
    void *va;
    void *uv_va;
    uint32_t pa;
    size_t length;
    size_t uv_length;
    size_t bytesused;
    size_t uv_bytesused;
};

int camera_open(struct camera *cam, const char *dev, int width, int height);
void camera_set_cancel_flag(struct camera *cam, volatile sig_atomic_t *cancel);
void camera_close(struct camera *cam);
int camera_dqbuf(struct camera *cam, struct camera_buf *cb);
int camera_qbuf(struct camera *cam, const struct camera_buf *cb);
