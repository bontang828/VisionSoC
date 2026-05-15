#include "libt1.h"
#include "libt1_regs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define ROWS 128u
#define COLS 128u
#define FRAME_BYTES (ROWS * COLS)

#define INSTR_VLE8_V8_A0 0x02050407u
#define INSTR_VSE8_V8_A0 0x02050427u

static uint8_t pattern(unsigned r, unsigned c)
{
    return (uint8_t)(((r * 3u) + (c * 5u) + (r ^ c)) & 0xFFu);
}

static void init_input(uint8_t *p)
{
    for (unsigned r = 0; r < ROWS; r++) {
        for (unsigned c = 0; c < COLS; c++) {
            p[r * COLS + c] = pattern(r, c);
        }
    }
}

static int check_transpose(const uint8_t *out)
{
    for (unsigned r = 0; r < ROWS; r++) {
        for (unsigned c = 0; c < COLS; c++) {
            uint8_t expected = pattern(c, r);
            uint8_t got = out[r * COLS + c];
            if (got != expected) {
                fprintf(stderr,
                        "FAIL: transpose mismatch at [%u][%u] got %u expected %u\n",
                        r, c, (unsigned)got, (unsigned)expected);
                return -1;
            }
        }
    }
    return 0;
}

int main(void)
{
    struct t1_buf in = {0};
    struct t1_buf out = {0};
    int rc = 1;

    if (t1_init() < 0) {
        perror("t1_init");
        return 1;
    }
    if (t1_buf_alloc(&in, FRAME_BYTES) < 0) {
        perror("t1_buf_alloc(in)");
        goto out_close;
    }
    if (t1_buf_alloc(&out, FRAME_BYTES) < 0) {
        perror("t1_buf_alloc(out)");
        goto out_free_in;
    }

    init_input((uint8_t *)in.va);
    memset(out.va, 0, out.size);
    if (t1_buf_sync_for_device(&in) < 0)  perror("sync_for_device(in)");
    if (t1_buf_sync_for_device(&out) < 0) perror("sync_for_device(out)");

    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M4_TA_MA,
        .vl = 128,
    };

    op.instruction = INSTR_VLE8_V8_A0;
    op.rs1 = in.pa;
    op.vertical_mode = 1;
    if (t1_issue(&op) < 0) {
        perror("t1_issue(vertical vle8)");
        goto out_free_out;
    }

    op.instruction = INSTR_VSE8_V8_A0;
    op.rs1 = out.pa;
    op.vertical_mode = 0;
    if (t1_issue(&op) < 0) {
        perror("t1_issue(horizontal vse8)");
        goto out_free_out;
    }

    if (t1_buf_sync_for_cpu(&out) < 0) perror("sync_for_cpu(out)");
    if (check_transpose((const uint8_t *)out.va) < 0) {
        goto out_free_out;
    }

    printf("PASS: vertical LSU transpose\n");
    rc = 0;

out_free_out:
    t1_buf_free(&out);
out_free_in:
    t1_buf_free(&in);
out_close:
    t1_close();
    return rc;
}
