#include "display.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void fill_nv12(struct display_buf *db, unsigned frame)
{
    uint8_t *y = db->va;
    uint8_t *uv = y + (size_t)db->pitch * db->height;

    for (uint32_t row = 0; row < db->height; row++) {
        uint8_t *line = y + (size_t)row * db->pitch;
        for (uint32_t col = 0; col < db->width; col++) {
            line[col] = (uint8_t)((row + col + frame * 3u) & 0xffu);
        }
    }

    for (uint32_t row = 0; row < db->height / 2u; row++) {
        memset(uv + (size_t)row * db->pitch, 0x80, db->width);
    }
}

int main(void)
{
    struct display disp;
    if (display_open(&disp) < 0) {
        perror("display_open");
        return 1;
    }

    fprintf(stderr, "display_smoke: mode %ux%u, scale=%d\n",
            disp.mode.hdisplay, disp.mode.vdisplay, disp.scale_to_fit);

    for (unsigned frame = 0; frame < 300u; frame++) {
        struct display_buf db;
        if (display_dq_for_filling(&disp, &db) < 0) {
            perror("display_dq_for_filling");
            display_close(&disp);
            return 1;
        }
        if (db.pitch < db.width) {
            errno = EINVAL;
            perror("display pitch");
            display_close(&disp);
            return 1;
        }
        fill_nv12(&db, frame);
        if (display_qbuf(&disp, &db) < 0) {
            perror("display_qbuf");
            display_close(&disp);
            return 1;
        }
        usleep(16666);
    }

    display_close(&disp);
    return 0;
}
