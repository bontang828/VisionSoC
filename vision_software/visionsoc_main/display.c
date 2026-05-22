#include "display.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

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

static int wait_vblank_enabled(void)
{
    const char *value = getenv("VISIONSOC_WAIT_VBLANK");
    return value && strcmp(value, "0") != 0 && strcmp(value, "false") != 0;
}

/* Mode-sized RGB565 scanout buffer. We use this as the CRTC framebuffer
 * directly, avoiding the zynqmp overlay ordering/visibility ambiguity that
 * made the full-screen black primary hide the NV12 video plane. */
static int create_video_fb(struct display *disp, struct display_fb *fb,
                           uint32_t width, uint32_t height)
{
    struct drm_mode_create_dumb create;
    memset(&create, 0, sizeof(create));
    create.width = width;
    create.height = height;
    create.bpp = 16;

    if (drmIoctl(disp->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        fprintf(stderr, "display: CREATE_DUMB RGB565 %ux%u failed: %s\n",
                width, height, strerror(errno));
        return -1;
    }

    fb->handle = create.handle;
    fb->pitch = create.pitch;
    fb->size = create.size;

    uint32_t handles[4] = {fb->handle, 0, 0, 0};
    uint32_t pitches[4] = {fb->pitch, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    if (drmModeAddFB2(disp->drm_fd, width, height, DRM_FORMAT_RGB565,
                      handles, pitches, offsets, &fb->fb_id, 0) < 0) {
        fprintf(stderr, "display: AddFB2 RGB565 %ux%u pitch %u failed: %s\n",
                width, height, fb->pitch, strerror(errno));
        return -1;
    }

    struct drm_mode_map_dumb map;
    memset(&map, 0, sizeof(map));
    map.handle = fb->handle;
    if (drmIoctl(disp->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        fprintf(stderr, "display: MAP_DUMB RGB565 failed: %s\n", strerror(errno));
        return -1;
    }

    fb->va = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  disp->drm_fd, (off_t)map.offset);
    if (fb->va == MAP_FAILED) {
        fb->va = NULL;
        return -1;
    }

    memset(fb->va, 0, fb->size);
    fb->pa = 0;
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

    /* Expose every plane (primary + overlay) to drmModeGetPlaneResources. */
    drmSetClientCap(disp->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

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

    for (int i = 0; i < DISPLAY_NUM_BUFS; i++) {
        if (create_video_fb(disp, &disp->fbs[i],
                            disp->mode.hdisplay, disp->mode.vdisplay) < 0) {
            goto fail;
        }
    }

    if (drmModeSetCrtc(disp->drm_fd, disp->crtc_id, disp->fbs[0].fb_id,
                       0, 0, &disp->connector_id, 1, &disp->mode) < 0) {
        fprintf(stderr, "display: SetCrtc crtc=%u conn=%u fb=%u mode=%ux%u failed: %s\n",
                disp->crtc_id, disp->connector_id, disp->fbs[0].fb_id,
                disp->mode.hdisplay, disp->mode.vdisplay, strerror(errno));
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
    db->width = disp->mode.hdisplay;
    db->height = disp->mode.vdisplay;
    return 0;
}

int display_qbuf(struct display *disp, const struct display_buf *db)
{
    if (!disp || !db || disp->drm_fd < 0 ||
        db->index < 0 || db->index >= DISPLAY_NUM_BUFS) {
        errno = EINVAL;
        return -1;
    }

    if (drmModeSetCrtc(disp->drm_fd, disp->crtc_id,
                       disp->fbs[db->index].fb_id, 0, 0,
                       &disp->connector_id, 1, &disp->mode) < 0) {
        return -1;
    }

    if (wait_vblank_enabled()) {
        /*
         * Optional pacing to the next vblank. The default path skips this
         * wait because it quantizes the live pipeline into 2- or 3-vblank
         * buckets, which is what made optical_flow look capped at 20 fps.
         */
        drmVBlank vbl;
        memset(&vbl, 0, sizeof(vbl));
        vbl.request.type = DRM_VBLANK_RELATIVE;
        vbl.request.sequence = 1;
        (void)drmWaitVBlank(disp->drm_fd, &vbl);
    }

    disp->front_index = db->index;
    return 0;
}
