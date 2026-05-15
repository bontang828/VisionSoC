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
    /* Flush CPU writes to DRAM so T1's vle8 reads our pattern, not
     * stale cached data. msync() on udmabuf is unreliable; the
     * udmabuf sysfs sync_for_device interface is the supported path. */
    if (t1_buf_sync_for_device(&in) < 0)  perror("sync_for_device(in)");
    if (t1_buf_sync_for_device(&out) < 0) perror("sync_for_device(out)");

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

    /* Invalidate CPU cache for out before reading, so we see what T1
     * wrote to DRAM rather than the cached zeros from memset. */
    if (t1_buf_sync_for_cpu(&out) < 0) perror("sync_for_cpu(out)");

    /* The single vle8/vse8 with vl=128 only touches the first 128 bytes
     * (vlmax for lmul=4 sew=8 vlen=256). Compare just that range. */
    if (memcmp(in.va, out.va, 128) != 0) {
        fprintf(stderr, "FAIL: DDR roundtrip mismatch in first 128 bytes\n");
        for (size_t i = 0; i < 16; i++) {
            fprintf(stderr, "  [%2zu] in=0x%02x  out=0x%02x\n",
                    i, ((uint8_t*)in.va)[i], ((uint8_t*)out.va)[i]);
        }
        goto out_free_out;
    }

    printf("PASS: DDR roundtrip (128-byte vlmax slice)\n");
    rc = 0;

out_free_out:
    t1_buf_free(&out);
out_free_in:
    t1_buf_free(&in);
out_close:
    t1_close();
    return rc;
}
