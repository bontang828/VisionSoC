#include "libt1.h"
#include "libt1_regs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define FRAME_BYTES (128u * 128u)

#define INSTR_VLE8_V8_A0 0x02050407u
#define INSTR_VSE8_V8_A0 0x02050427u

static void init_pattern(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)((i * 17u + i / 7u) & 0xFFu);
    }
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

    init_pattern((uint8_t *)in.va, in.size);
    memset(out.va, 0, out.size);
    (void)msync(in.va, in.size, MS_SYNC);
    (void)msync(out.va, out.size, MS_SYNC);

    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M4_TA_MA,
        .vl = 128,
    };

    op.instruction = INSTR_VLE8_V8_A0;
    op.rs1 = in.pa;
    if (t1_issue(&op) < 0) {
        perror("t1_issue(vle8)");
        goto out_free_out;
    }

    op.instruction = INSTR_VSE8_V8_A0;
    op.rs1 = out.pa;
    if (t1_issue(&op) < 0) {
        perror("t1_issue(vse8)");
        goto out_free_out;
    }

    (void)msync(out.va, out.size, MS_INVALIDATE);
    if (memcmp(in.va, out.va, FRAME_BYTES) != 0) {
        fprintf(stderr, "FAIL: DDR roundtrip mismatch\n");
        goto out_free_out;
    }

    printf("PASS: DDR roundtrip\n");
    rc = 0;

out_free_out:
    t1_buf_free(&out);
out_free_in:
    t1_buf_free(&in);
out_close:
    t1_close();
    return rc;
}
