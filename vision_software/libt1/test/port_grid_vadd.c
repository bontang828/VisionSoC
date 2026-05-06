#include "libt1.h"
#include "libt1_regs.h"
#include "../../visionsoc_main/kernels/grid_vadd.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define FRAME_BYTES (128u * 128u)

static void init_inputs(int8_t *a, int8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        a[i] = (int8_t)(20 + (int)(i & 0x0F));
        b[i] = (int8_t)(3 + (int)((i / 13u) & 0x07));
    }
}

static int issue_grid_vadd(uint32_t a_pa, uint32_t b_pa, uint32_t out_pa)
{
    if (grid_vadd_count != 4) {
        fprintf(stderr, "FAIL: unexpected grid_vadd_count=%u\n", grid_vadd_count);
        return -1;
    }

    struct t1_op op = {
        .vtype = T1_VTYPE_E8_M4_TA_MA,
        .vl = 128,
    };

    op.instruction = grid_vadd[0];
    op.rs1 = a_pa;
    if (t1_issue(&op) < 0) {
        perror("t1_issue(vle a)");
        return -1;
    }

    op.instruction = grid_vadd[1];
    op.rs1 = b_pa;
    if (t1_issue(&op) < 0) {
        perror("t1_issue(vle b)");
        return -1;
    }

    op.instruction = grid_vadd[2];
    op.rs1 = 0;
    if (t1_issue(&op) < 0) {
        perror("t1_issue(vsub)");
        return -1;
    }

    op.instruction = grid_vadd[3];
    op.rs1 = out_pa;
    if (t1_issue(&op) < 0) {
        perror("t1_issue(vse out)");
        return -1;
    }

    return 0;
}

int main(void)
{
    struct t1_buf a = {0};
    struct t1_buf b = {0};
    struct t1_buf out = {0};
    int rc = 1;

    if (t1_init() < 0) {
        perror("t1_init");
        return 1;
    }
    if (t1_buf_alloc(&a, FRAME_BYTES) < 0) {
        perror("t1_buf_alloc(a)");
        goto out_close;
    }
    if (t1_buf_alloc(&b, FRAME_BYTES) < 0) {
        perror("t1_buf_alloc(b)");
        goto out_free_a;
    }
    if (t1_buf_alloc(&out, FRAME_BYTES) < 0) {
        perror("t1_buf_alloc(out)");
        goto out_free_b;
    }

    init_inputs((int8_t *)a.va, (int8_t *)b.va, FRAME_BYTES);
    memset(out.va, 0, out.size);
    (void)msync(a.va, a.size, MS_SYNC);
    (void)msync(b.va, b.size, MS_SYNC);
    (void)msync(out.va, out.size, MS_SYNC);

    if (issue_grid_vadd(a.pa, b.pa, out.pa) < 0) {
        goto out_free_out;
    }

    (void)msync(out.va, out.size, MS_INVALIDATE);

    const int8_t *pa = (const int8_t *)a.va;
    const int8_t *pb = (const int8_t *)b.va;
    const int8_t *po = (const int8_t *)out.va;
    for (size_t i = 0; i < FRAME_BYTES; i++) {
        int8_t expected = (int8_t)((int)pa[i] - (int)pb[i]);
        if (po[i] != expected) {
            fprintf(stderr, "FAIL: mismatch at byte %zu got %d expected %d\n",
                    i, (int)po[i], (int)expected);
            goto out_free_out;
        }
    }

    printf("PASS: port_grid_vadd\n");
    rc = 0;

out_free_out:
    t1_buf_free(&out);
out_free_b:
    t1_buf_free(&b);
out_free_a:
    t1_buf_free(&a);
out_close:
    t1_close();
    return rc;
}
