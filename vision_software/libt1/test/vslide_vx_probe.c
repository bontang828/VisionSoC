#include "cnn2d_decoder.h"
#include "libt1.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ROWS CNN2D_ROWS
#define COLS CNN2D_COLS
#define MAP_BYTES CNN2D_MAP_BYTES
#define ARENA_BYTES (7u * MAP_BYTES)

enum slide_dir {
    SLIDE_UP,
    SLIDE_DOWN,
};

enum slide_kind {
    SLIDE_VX,
    SLIDE_VI,
};

static int8_t src_at(const int8_t *src, unsigned r, unsigned c)
{
    return src[r * COLS + c];
}

static void init_feature(int8_t *p)
{
    for (unsigned r = 0; r < ROWS; r++) {
        for (unsigned c = 0; c < COLS; c++) {
            p[r * COLS + c] = (int8_t)(((r * 17u + c * 3u) & 0x7fu) - 64);
        }
    }
}

static int8_t expected_h(const int8_t *src, enum slide_dir dir,
                         unsigned r, unsigned c, unsigned off)
{
    if (dir == SLIDE_UP) {
        return c < off ? 0 : src_at(src, r, c - off);
    }
    return c + off >= COLS ? 0 : src_at(src, r, c + off);
}

static int8_t expected_v(const int8_t *src, enum slide_dir dir,
                         unsigned r, unsigned c, unsigned off)
{
    if (dir == SLIDE_UP) {
        return r < off ? 0 : src_at(src, r - off, c);
    }
    return r + off >= ROWS ? 0 : src_at(src, r + off, c);
}

static void classify_result(const char *name, const int8_t *src,
                            const int8_t *got, enum slide_dir dir,
                            unsigned off)
{
    unsigned v_errors = 0;
    unsigned h_errors = 0;
    unsigned first_r = 0;
    unsigned first_c = 0;
    int8_t first_got = 0;
    int8_t first_v = 0;
    int8_t first_h = 0;

    for (unsigned r = 0; r < ROWS; r++) {
        for (unsigned c = 0; c < COLS; c++) {
            int8_t exp_v = expected_v(src, dir, r, c, off);
            int8_t exp_h = expected_h(src, dir, r, c, off);
            int8_t val = got[r * COLS + c];

            if (val != exp_v) {
                if (v_errors == 0) {
                    first_r = r;
                    first_c = c;
                    first_got = val;
                    first_v = exp_v;
                    first_h = exp_h;
                }
                v_errors++;
            }
            if (val != exp_h) {
                h_errors++;
            }
        }
    }

    if (v_errors == 0) {
        printf("PASS: %s matches vertical expectation\n", name);
        return;
    }

    printf("FAIL: %s mismatches vertical expectation: %u cells", name, v_errors);
    if (h_errors == 0) {
        printf(" and exactly matches horizontal expectation");
    } else {
        printf(" and also mismatches horizontal expectation: %u cells", h_errors);
    }
    printf("\n");
    printf("      first mismatch [%u][%u]: got %d, vertical exp %d, horizontal exp %d\n",
           first_r, first_c, first_got, first_v, first_h);
}

static enum cnn2d_prim slide_prim(enum slide_dir dir, enum slide_kind kind)
{
    if (kind == SLIDE_VX) {
        return dir == SLIDE_UP ? CNN2D_PRIM_SHIFT_POS : CNN2D_PRIM_SHIFT_NEG;
    }
    return dir == SLIDE_UP ? CNN2D_PRIM_SHIFT_POS_I2 : CNN2D_PRIM_SHIFT_NEG_I2;
}

static enum cnn2d_prim zero_prim(enum slide_dir dir)
{
    return dir == SLIDE_UP ? CNN2D_PRIM_DEBUG_ZERO_POS : CNN2D_PRIM_DEBUG_ZERO_NEG;
}

static enum cnn2d_prim store_prim(enum slide_dir dir)
{
    return dir == SLIDE_UP ? CNN2D_PRIM_STORE_POS : CNN2D_PRIM_STORE_NEG;
}

static int issue_case(struct t1_buf *arena_buf, uint32_t src_pa, uint32_t dst_pa,
                      enum slide_dir dir, enum slide_kind kind, unsigned off)
{
    if (t1_buf_sync_for_device(arena_buf) < 0) {
        perror("sync_for_device");
        return -1;
    }
    if (cnn2d_issue_primitive(CNN2D_PRIM_LOAD_FEATURE, src_pa, 0) < 0) {
        perror("load feature");
        return -1;
    }
    if (cnn2d_issue_primitive(zero_prim(dir), 0, 0) < 0) {
        perror("zero slide dest");
        return -1;
    }
    if (cnn2d_issue_primitive(slide_prim(dir, kind),
                              kind == SLIDE_VX ? off : 0, 1) < 0) {
        perror("vertical slide");
        return -1;
    }
    if (cnn2d_issue_primitive(store_prim(dir), dst_pa, 0) < 0) {
        perror("store slide dest");
        return -1;
    }
    if (t1_buf_sync_for_cpu(arena_buf) < 0) {
        perror("sync_for_cpu");
        return -1;
    }
    return 0;
}

static int issue_acc_case(struct t1_buf *arena_buf, uint32_t src_pa, uint32_t dst_pa,
                          enum slide_dir dir, enum slide_kind kind, unsigned off)
{
    if (t1_buf_sync_for_device(arena_buf) < 0) {
        perror("sync_for_device");
        return -1;
    }
    if (cnn2d_issue_primitive(CNN2D_PRIM_LOAD_FEATURE, src_pa, 0) < 0) {
        perror("load feature");
        return -1;
    }
    if (cnn2d_issue_primitive(CNN2D_PRIM_ACC_ZERO, 0, 0) < 0) {
        perror("zero acc");
        return -1;
    }
    if (cnn2d_issue_primitive(slide_prim(dir, kind),
                              kind == SLIDE_VX ? off : 0, 1) < 0) {
        perror("vertical slide");
        return -1;
    }
    if (cnn2d_issue_primitive(dir == SLIDE_UP ? CNN2D_PRIM_ACC_ADD_POS
                                              : CNN2D_PRIM_ACC_ADD_NEG,
                              0, 1) < 0) {
        perror("vertical acc add");
        return -1;
    }
    if (cnn2d_issue_primitive(CNN2D_PRIM_STORE_ACC, dst_pa, 0) < 0) {
        perror("store acc");
        return -1;
    }
    if (t1_buf_sync_for_cpu(arena_buf) < 0) {
        perror("sync_for_cpu");
        return -1;
    }
    return 0;
}

int main(void)
{
    struct t1_buf arena_buf = {0};
    struct cnn2d_arena arena = {0};
    struct cnn2d_buf_view src = {0};
    struct cnn2d_buf_view up_vx = {0};
    struct cnn2d_buf_view up_vi = {0};
    struct cnn2d_buf_view down_vx = {0};
    struct cnn2d_buf_view down_vi = {0};
    struct cnn2d_buf_view up_vx_acc = {0};
    struct cnn2d_buf_view down_vx_acc = {0};
    const unsigned off = 2;
    int rc = 1;

    if (t1_init() < 0) {
        perror("t1_init");
        return 1;
    }
    if (t1_buf_alloc(&arena_buf, ARENA_BYTES) < 0) {
        perror("t1_buf_alloc");
        goto out;
    }

    cnn2d_arena_init(&arena, &arena_buf);
    if (cnn2d_arena_alloc(&arena, MAP_BYTES, COLS, &src) < 0 ||
        cnn2d_arena_alloc(&arena, MAP_BYTES, COLS, &up_vx) < 0 ||
        cnn2d_arena_alloc(&arena, MAP_BYTES, COLS, &up_vi) < 0 ||
        cnn2d_arena_alloc(&arena, MAP_BYTES, COLS, &down_vx) < 0 ||
        cnn2d_arena_alloc(&arena, MAP_BYTES, COLS, &down_vi) < 0 ||
        cnn2d_arena_alloc(&arena, MAP_BYTES, COLS, &up_vx_acc) < 0 ||
        cnn2d_arena_alloc(&arena, MAP_BYTES, COLS, &down_vx_acc) < 0) {
        perror("cnn2d_arena_alloc");
        goto out;
    }

    init_feature((int8_t *)src.va);
    memset(up_vx.va, 0, up_vx.size);
    memset(up_vi.va, 0, up_vi.size);
    memset(down_vx.va, 0, down_vx.size);
    memset(down_vi.va, 0, down_vi.size);
    memset(up_vx_acc.va, 0, up_vx_acc.size);
    memset(down_vx_acc.va, 0, down_vx_acc.size);

    printf("=== vslide_vx_probe: horizontal load, vertical compute, horizontal store, offset %u ===\n",
           off);

    if (issue_case(&arena_buf, src.pa, up_vx.pa, SLIDE_UP, SLIDE_VX, off) < 0 ||
        issue_case(&arena_buf, src.pa, up_vi.pa, SLIDE_UP, SLIDE_VI, off) < 0 ||
        issue_case(&arena_buf, src.pa, down_vx.pa, SLIDE_DOWN, SLIDE_VX, off) < 0 ||
        issue_case(&arena_buf, src.pa, down_vi.pa, SLIDE_DOWN, SLIDE_VI, off) < 0 ||
        issue_acc_case(&arena_buf, src.pa, up_vx_acc.pa, SLIDE_UP, SLIDE_VX, off) < 0 ||
        issue_acc_case(&arena_buf, src.pa, down_vx_acc.pa, SLIDE_DOWN, SLIDE_VX, off) < 0) {
        goto out;
    }

    classify_result("vslideup.vx vertical", (const int8_t *)src.va,
                    (const int8_t *)up_vx.va, SLIDE_UP, off);
    classify_result("vslideup.vi vertical", (const int8_t *)src.va,
                    (const int8_t *)up_vi.va, SLIDE_UP, off);
    classify_result("vslidedown.vx vertical", (const int8_t *)src.va,
                    (const int8_t *)down_vx.va, SLIDE_DOWN, off);
    classify_result("vslidedown.vi vertical", (const int8_t *)src.va,
                    (const int8_t *)down_vi.va, SLIDE_DOWN, off);
    classify_result("vslideup.vx vertical via acc", (const int8_t *)src.va,
                    (const int8_t *)up_vx_acc.va, SLIDE_UP, off);
    classify_result("vslidedown.vx vertical via acc", (const int8_t *)src.va,
                    (const int8_t *)down_vx_acc.va, SLIDE_DOWN, off);

    rc = 0;

out:
    t1_buf_free(&arena_buf);
    t1_close();
    return rc;
}
