#include "camera.h"

#include "libt1.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long req, void *arg)
{
    int rc;
    do {
        rc = ioctl(fd, req, arg);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

static void camera_unmap_buffers(struct camera *cam)
{
    for (unsigned i = 0; i < cam->nbufs; i++) {
        if (cam->bufs[i].va && cam->bufs[i].length) {
            munmap(cam->bufs[i].va, cam->bufs[i].length);
        }
        cam->bufs[i].va = NULL;
        cam->bufs[i].pa = 0;
        cam->bufs[i].length = 0;
    }
    cam->nbufs = 0;
}

int camera_open(struct camera *cam, const char *dev, int width, int height)
{
    if (!cam || !dev || width <= 0 || height <= 0) {
        errno = EINVAL;
        return -1;
    }

    memset(cam, 0, sizeof(*cam));
    cam->fd = -1;
    cam->width = width;
    cam->height = height;
    cam->pixfmt = V4L2_PIX_FMT_YUYV;

    cam->fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (cam->fd < 0) {
        return -1;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = (uint32_t)width;
    fmt.fmt.pix.height = (uint32_t)height;
    fmt.fmt.pix.pixelformat = cam->pixfmt;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
        goto fail;
    }

    cam->width = (int)fmt.fmt.pix.width;
    cam->height = (int)fmt.fmt.pix.height;
    cam->pixfmt = fmt.fmt.pix.pixelformat;

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0) {
        goto fail;
    }
    if (req.count < 2 || req.count > CAMERA_MAX_BUFS) {
        errno = EINVAL;
        goto fail;
    }

    cam->nbufs = req.count;
    for (unsigned i = 0; i < cam->nbufs; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            goto fail;
        }

        void *va = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                        cam->fd, (off_t)buf.m.offset);
        if (va == MAP_FAILED) {
            goto fail;
        }

        cam->bufs[i].va = va;
        cam->bufs[i].length = buf.length;
        cam->bufs[i].pa = t1_va_to_pa(va);
        if (cam->bufs[i].pa == 0) {
            goto fail;
        }
    }

    for (unsigned i = 0; i < cam->nbufs; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            goto fail;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        goto fail;
    }
    cam->streaming = 1;
    return 0;

fail:
    {
        int saved = errno;
        camera_close(cam);
        errno = saved;
        return -1;
    }
}

void camera_close(struct camera *cam)
{
    if (!cam) {
        return;
    }
    if (cam->fd >= 0 && cam->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)xioctl(cam->fd, VIDIOC_STREAMOFF, &type);
        cam->streaming = 0;
    }
    camera_unmap_buffers(cam);
    if (cam->fd >= 0) {
        close(cam->fd);
        cam->fd = -1;
    }
}

int camera_dqbuf(struct camera *cam, struct camera_buf *cb)
{
    if (!cam || !cb || cam->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    struct pollfd pfd = {
        .fd = cam->fd,
        .events = POLLIN,
    };

    for (;;) {
        int prc = poll(&pfd, 1, 1000);
        if (prc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (prc == 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) == 0) {
            if (buf.index >= cam->nbufs) {
                errno = EIO;
                return -1;
            }
            cb->index = buf.index;
            cb->va = cam->bufs[buf.index].va;
            cb->pa = cam->bufs[buf.index].pa;
            cb->length = cam->bufs[buf.index].length;
            cb->bytesused = buf.bytesused;
            return 0;
        }
        if (errno != EAGAIN) {
            return -1;
        }
    }
}

int camera_qbuf(struct camera *cam, const struct camera_buf *cb)
{
    if (!cam || !cb || cam->fd < 0 || cb->index >= cam->nbufs) {
        errno = EINVAL;
        return -1;
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = cb->index;

    return xioctl(cam->fd, VIDIOC_QBUF, &buf);
}
