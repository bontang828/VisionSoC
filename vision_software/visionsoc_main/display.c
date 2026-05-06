#include "display.h"

#include "libt1.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
                              unsigned int usec, void *data)
{
    (void)fd;
    (void)frame;
    (void)sec;
    (void)usec;
    int *waiting = (int *)data;
    *waiting = 0;
}

static drmModeConnector *find_connector(int fd, drmModeRes *res)
{
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) {
            continue;
        }
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            return conn;
        }
        drmModeFreeConnector(conn);
    }
    return NULL;
}

static drmModeEncoder *find_encoder(int fd, drmModeRes *res, drmModeConnector *conn)
{
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
        if (enc) {
            return enc;
        }
    }

    for (int i = 0; i < conn->count_encoders; i++) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[i]);
        if (!enc) {
            continue;
        }
        for (int c = 0; c < res->count_crtcs; c++) {
            if (enc->possible_crtcs & (1 << c)) {
                return enc;
            }
        }
        drmModeFreeEncoder(enc);
    }

    errno = ENODEV;
    return NULL;
}

static drmModeModeInfo pick_mode(const drmModeConnector *conn)
{
    for (int i = 0; i < conn->count_modes; i++) {
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            return conn->modes[i];
        }
    }
    return conn->modes[0];
}

static int create_fb(struct display *disp, struct display_fb *fb,
                     uint32_t width, uint32_t height)
{
    struct drm_mode_create_dumb create;
    memset(&create, 0, sizeof(create));
    create.width = width;
    create.height = height;
    create.bpp = 32;

    if (drmIoctl(disp->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        return -1;
    }

    fb->handle = create.handle;
    fb->pitch = create.pitch;
    fb->size = create.size;

    uint32_t handles[4] = {fb->handle, 0, 0, 0};
    uint32_t pitches[4] = {fb->pitch, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    if (drmModeAddFB2(disp->drm_fd, width, height, DRM_FORMAT_XRGB8888,
                      handles, pitches, offsets, &fb->fb_id, 0) < 0) {
        return -1;
    }

    struct drm_mode_map_dumb map;
    memset(&map, 0, sizeof(map));
    map.handle = fb->handle;
    if (drmIoctl(disp->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        return -1;
    }

    fb->va = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  disp->drm_fd, (off_t)map.offset);
    if (fb->va == MAP_FAILED) {
        fb->va = NULL;
        return -1;
    }
    memset(fb->va, 0, fb->size);

    fb->pa = t1_va_to_pa(fb->va);
    if (fb->pa == 0) {
        return -1;
    }

    return 0;
}

static void destroy_fb(struct display *disp, struct display_fb *fb)
{
    if (fb->va && fb->size) {
        munmap(fb->va, (size_t)fb->size);
    }
    if (fb->fb_id) {
        drmModeRmFB(disp->drm_fd, fb->fb_id);
    }
    if (fb->handle) {
        struct drm_mode_destroy_dumb destroy;
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = fb->handle;
        (void)drmIoctl(disp->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    memset(fb, 0, sizeof(*fb));
}

int display_open(struct display *disp)
{
    if (!disp) {
        errno = EINVAL;
        return -1;
    }

    memset(disp, 0, sizeof(*disp));
    disp->drm_fd = -1;
    disp->front_index = 0;

    disp->drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (disp->drm_fd < 0) {
        return -1;
    }

    drmModeRes *res = drmModeGetResources(disp->drm_fd);
    if (!res) {
        goto fail;
    }

    drmModeConnector *conn = find_connector(disp->drm_fd, res);
    if (!conn) {
        drmModeFreeResources(res);
        errno = ENODEV;
        goto fail;
    }

    drmModeEncoder *enc = find_encoder(disp->drm_fd, res, conn);
    if (!enc) {
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        goto fail;
    }

    disp->connector_id = conn->connector_id;
    disp->crtc_id = enc->crtc_id ? enc->crtc_id : res->crtcs[0];
    disp->mode = pick_mode(conn);

    drmModeFreeEncoder(enc);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    uint32_t width = (uint32_t)disp->mode.hdisplay;
    uint32_t height = (uint32_t)disp->mode.vdisplay;
    for (int i = 0; i < DISPLAY_NUM_BUFS; i++) {
        if (create_fb(disp, &disp->fbs[i], width, height) < 0) {
            goto fail;
        }
    }

    if (drmModeSetCrtc(disp->drm_fd, disp->crtc_id, disp->fbs[0].fb_id,
                       0, 0, &disp->connector_id, 1, &disp->mode) < 0) {
        goto fail;
    }

    return 0;

fail:
    {
        int saved = errno;
        display_close(disp);
        errno = saved;
        return -1;
    }
}

void display_close(struct display *disp)
{
    if (!disp) {
        return;
    }
    if (disp->drm_fd >= 0) {
        for (int i = 0; i < DISPLAY_NUM_BUFS; i++) {
            destroy_fb(disp, &disp->fbs[i]);
        }
        close(disp->drm_fd);
        disp->drm_fd = -1;
    }
}

int display_dq_for_filling(struct display *disp, struct display_buf *db)
{
    if (!disp || !db || disp->drm_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    int idx = disp->front_index == 0 ? 1 : 0;
    struct display_fb *fb = &disp->fbs[idx];
    db->index = idx;
    db->va = fb->va;
    db->pa = fb->pa;
    db->pitch = fb->pitch;
    db->width = (uint32_t)disp->mode.hdisplay;
    db->height = (uint32_t)disp->mode.vdisplay;
    return 0;
}

int display_qbuf(struct display *disp, const struct display_buf *db)
{
    if (!disp || !db || disp->drm_fd < 0 ||
        db->index < 0 || db->index >= DISPLAY_NUM_BUFS) {
        errno = EINVAL;
        return -1;
    }

    int waiting = 1;
    if (drmModePageFlip(disp->drm_fd, disp->crtc_id,
                        disp->fbs[db->index].fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT, &waiting) < 0) {
        return -1;
    }

    drmEventContext ev;
    memset(&ev, 0, sizeof(ev));
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = page_flip_handler;

    while (waiting) {
        struct pollfd pfd = {
            .fd = disp->drm_fd,
            .events = POLLIN,
        };
        int rc = poll(&pfd, 1, 1000);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (drmHandleEvent(disp->drm_fd, &ev) < 0) {
            return -1;
        }
    }

    disp->front_index = db->index;
    return 0;
}
