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

static void prefault_mapping(void *va, size_t length)
{
    volatile uint8_t *p = va;
    long page_size = sysconf(_SC_PAGESIZE);
    size_t step = page_size > 0 ? (size_t)page_size : 4096u;

    for (size_t off = 0; off < length; off += step) {
        (void)p[off];
    }
    if (length > 0) {
        (void)p[length - 1u];
    }
}

static void camera_unmap_buffers(struct camera *cam)
{
    for (unsigned i = 0; i < cam->nbufs; i++) {
        if (cam->bufs[i].va && cam->bufs[i].length) {
            munmap(cam->bufs[i].va, cam->bufs[i].length);
        }
        if (cam->bufs[i].uv_va && cam->bufs[i].uv_length) {
            munmap(cam->bufs[i].uv_va, cam->bufs[i].uv_length);
        }
        cam->bufs[i].va = NULL;
        cam->bufs[i].uv_va = NULL;
        cam->bufs[i].pa = 0;
        cam->bufs[i].length = 0;
        cam->bufs[i].uv_length = 0;
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
    /*
     * NV12 to match the deployed 5q bitstream / dts (xlnx,vid-formats =
     * "nv12"). NV12 is single-buffer (contiguous Y plane then half-res
     * interleaved UV), so the single-plane mmap path below is unchanged;
     * the driver reports sizeimage = width*height*3/2 (24576 at 128x128).
     */
    cam->pixfmt = V4L2_PIX_FMT_NV12;

    cam->fd = open(dev, O_RDWR | O_CLOEXEC);
    if (cam->fd < 0) {
        return -1;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(cam->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "camera: VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
        goto fail;
    }

    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ?
                    cap.device_caps : cap.capabilities;
    if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        cam->buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    } else if (caps & V4L2_CAP_VIDEO_CAPTURE) {
        cam->buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    } else {
        errno = ENODEV;
        goto fail;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = cam->buf_type;
    if (cam->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        fmt.fmt.pix_mp.width = (uint32_t)width;
        fmt.fmt.pix_mp.height = (uint32_t)height;
        fmt.fmt.pix_mp.pixelformat = cam->pixfmt;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    } else {
        fmt.fmt.pix.width = (uint32_t)width;
        fmt.fmt.pix.height = (uint32_t)height;
        fmt.fmt.pix.pixelformat = cam->pixfmt;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
    }

    if (xioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "camera: VIDIOC_S_FMT NV12 %dx%d failed: %s\n",
                width, height, strerror(errno));
        goto fail;
    }

    if (cam->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        cam->width = (int)fmt.fmt.pix_mp.width;
        cam->height = (int)fmt.fmt.pix_mp.height;
        cam->pixfmt = fmt.fmt.pix_mp.pixelformat;
        cam->nplanes = fmt.fmt.pix_mp.num_planes;
        if (cam->nplanes == 0) {
            cam->nplanes = VIDEO_MAX_PLANES;
        }
        if (cam->nplanes > VIDEO_MAX_PLANES) {
            errno = ENOTSUP;
            goto fail;
        }
    } else {
        cam->width = (int)fmt.fmt.pix.width;
        cam->height = (int)fmt.fmt.pix.height;
        cam->pixfmt = fmt.fmt.pix.pixelformat;
        cam->nplanes = 0;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = cam->buf_type;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "camera: VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        goto fail;
    }
    if (req.count < 2 || req.count > CAMERA_MAX_BUFS) {
        errno = EINVAL;
        goto fail;
    }

    cam->nbufs = req.count;
    for (unsigned i = 0; i < cam->nbufs; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = cam->buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (cam->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes;
            buf.length = cam->nplanes;
        }

        if (xioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "camera: VIDIOC_QUERYBUF[%u] failed: %s\n",
                    i, strerror(errno));
            goto fail;
        }

        unsigned nplanes = 1;
        if (cam->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            nplanes = buf.length;
            if (nplanes == 0) {
                if (planes[0].length && planes[1].length) {
                    nplanes = 2;
                } else if (planes[0].length) {
                    nplanes = 1;
                }
            }
            if (nplanes < 1 || nplanes > 2) {
                errno = ENOTSUP;
                goto fail;
            }
            if (i == 0) {
                cam->nplanes = nplanes;
            } else if (cam->nplanes != nplanes) {
                errno = EIO;
                goto fail;
            }
            if (i == 0) {
                fprintf(stderr,
                        "camera: mplane query nplanes=%u p0.len=%u p0.off=%u "
                        "p1.len=%u p1.off=%u\n",
                        nplanes, planes[0].length, planes[0].m.mem_offset,
                        planes[1].length, planes[1].m.mem_offset);
            }
        }

        size_t length;
        size_t uv_length = 0;
        off_t offset;
        off_t uv_offset = 0;
        if (cam->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            length = planes[0].length;
            offset = (off_t)planes[0].m.mem_offset;
            if (nplanes == 2) {
                uv_length = planes[1].length;
                uv_offset = (off_t)planes[1].m.mem_offset;
            }
        } else {
            length = buf.length;
            offset = (off_t)buf.m.offset;
        }

        void *va = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
                        cam->fd, offset);
        if (va == MAP_FAILED) {
            goto fail;
        }
        prefault_mapping(va, length);

        void *uv_va = NULL;
        if (uv_length > 0) {
            uv_va = mmap(NULL, uv_length, PROT_READ | PROT_WRITE, MAP_SHARED,
                         cam->fd, uv_offset);
            if (uv_va == MAP_FAILED) {
                uv_va = NULL;
                goto fail;
            }
            prefault_mapping(uv_va, uv_length);
        }

        cam->bufs[i].va = va;
        cam->bufs[i].uv_va = uv_va;
        cam->bufs[i].length = length;
        cam->bufs[i].uv_length = uv_length;
        cam->bufs[i].pa = 0;
    }

    for (unsigned i = 0; i < cam->nbufs; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = cam->buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (cam->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes;
            buf.length = cam->nplanes;
        }

        if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "camera: VIDIOC_QBUF[%u] failed: %s\n",
                    i, strerror(errno));
            goto fail;
        }
    }

    enum v4l2_buf_type type = (enum v4l2_buf_type)cam->buf_type;
    if (xioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "camera: VIDIOC_STREAMON failed: %s\n", strerror(errno));
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
        enum v4l2_buf_type type = (enum v4l2_buf_type)cam->buf_type;
        (void)xioctl(cam->fd, VIDIOC_STREAMOFF, &type);
        cam->streaming = 0;
    }
    camera_unmap_buffers(cam);
    if (cam->fd >= 0) {
        close(cam->fd);
        cam->fd = -1;
    }
}

void camera_set_cancel_flag(struct camera *cam, volatile sig_atomic_t *cancel)
{
    if (cam) {
        cam->cancel = cancel;
    }
}

int camera_dqbuf(struct camera *cam, struct camera_buf *cb)
{
    if (!cam || !cb || cam->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    for (;;) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = cam->buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        if (cam->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes;
            buf.length = cam->nplanes;
        }

        if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) == 0) {
            if (buf.index >= cam->nbufs) {
                errno = EIO;
                return -1;
            }
            cb->index = buf.index;
            cb->va = cam->bufs[buf.index].va;
            cb->uv_va = cam->bufs[buf.index].uv_va;
            cb->pa = cam->bufs[buf.index].pa;
            cb->length = cam->bufs[buf.index].length;
            cb->uv_length = cam->bufs[buf.index].uv_length;
            cb->bytesused = cam->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ?
                            planes[0].bytesused : buf.bytesused;
            cb->uv_bytesused = (cam->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
                                cam->nplanes == 2) ? planes[1].bytesused : 0;
            return 0;
        }
        if (errno != EAGAIN) {
            if (errno == EINTR) {
                if (cam->cancel && *cam->cancel) {
                    errno = ECANCELED;
                    return -1;
                }
                continue;
            }
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
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = cam->buf_type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = cb->index;
    if (cam->buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.m.planes = planes;
        buf.length = cam->nplanes;
    }

    return xioctl(cam->fd, VIDIOC_QBUF, &buf);
}
