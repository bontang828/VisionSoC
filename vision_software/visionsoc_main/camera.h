#pragma once

#include <stddef.h>
#include <stdint.h>

#define CAMERA_MAX_BUFS 8

struct camera_mmap_buf {
    void *va;
    uint32_t pa;
    size_t length;
};

struct camera {
    int fd;
    int width;
    int height;
    uint32_t pixfmt;
    unsigned nbufs;
    int streaming;
    struct camera_mmap_buf bufs[CAMERA_MAX_BUFS];
};

struct camera_buf {
    unsigned index;
    void *va;
    uint32_t pa;
    size_t length;
    size_t bytesused;
};

int camera_open(struct camera *cam, const char *dev, int width, int height);
void camera_close(struct camera *cam);
int camera_dqbuf(struct camera *cam, struct camera_buf *cb);
int camera_qbuf(struct camera *cam, const struct camera_buf *cb);
