#include "cnn2d_decoder.h"
#include "libt1.h"
#include "libt1_regs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ROWS 128u
#define COLS 128u
#define MAP_BYTES (ROWS * COLS)
#define ARENA_BYTES (4u * 1024u * 1024u)
#define TAGGED_WORDS_PER_ROW 32u
#define TAGGED_BYTES (ROWS * TAGGED_WORDS_PER_ROW * sizeof(int32_t))

static int fail_at(const char *name, unsigned r, unsigned c, int got, int exp)
{
    fprintf(stderr, "FAIL: %s mismatch at [%u][%u]: got %d expected %d\n",
            name, r, c, got, exp);
    return -1;
}

static int8_t i8(int v)
{
    return (int8_t)v;
}

static int8_t abs_i8_like_rvv(int8_t x)
{
    int8_t neg = i8(0 - (int)x);
    return x > neg ? x : neg;
}

static int8_t sat_add_i8(int a, int b)
{
    int v = a + b;
    if (v > 127) {
        return 127;
    }
    if (v < -128) {
        return -128;
    }
    return (int8_t)v;
}

static void init_input(int8_t *p)
{
    for (unsigned r = 0; r < ROWS; r++) {
        for (unsigned c = 0; c < COLS; c++) {
            int v = 0;
            if (r >= 24 && r < 104 && c >= 40 && c < 88) {
                v = 70;
            }
            if (r >= 58 && r < 70) {
                v = 95;
            }
            p[r * COLS + c] = (int8_t)v;
        }
    }
}

static void init_lut(int8_t *p)
{
    for (unsigned r = 0; r < ROWS; r++) {
        for (unsigned c = 0; c < COLS; c++) {
            p[r * COLS + c] = c >= 24 ? 1 : 0;
        }
    }
}

static void init_feature(int8_t *p)
{
    for (unsigned r = 0; r < ROWS; r++) {
        for (unsigned c = 0; c < COLS; c++) {
            p[r * COLS + c] = (int8_t)(((r * 3u + c * 5u) & 0x7fu) - 32);
        }
    }
}

static int8_t expected_edge(const int8_t *input, const int8_t *lut,
                            unsigned r, unsigned c)
{
    int left = c == 0 ? 0 : input[r * COLS + c - 1u];
    int right = c + 1u >= COLS ? 0 : input[r * COLS + c + 1u];
    int above = r == 0 ? 0 : input[(r - 1u) * COLS + c];
    int below = r + 1u >= ROWS ? 0 : input[(r + 1u) * COLS + c];

    int8_t gx = i8(right - left);
    int8_t gy = i8(below - above);
    int8_t mag = sat_add_i8(abs_i8_like_rvv(gx), abs_i8_like_rvv(gy));
    if (mag < 0) {
        mag = 0;
    }

    unsigned idx = (unsigned)(uint8_t)mag;
    return idx < COLS ? lut[r * COLS + idx] : 0;
}

static int check_edge_and_threshold(const int8_t *input, const int8_t *lut,
                                    const int8_t *edges,
                                    const int8_t *thresholded)
{
    for (unsigned r = 0; r < ROWS; r++) {
        for (unsigned c = 0; c < COLS; c++) {
            int8_t exp_edge = expected_edge(input, lut, r, c);
            if (edges[r * COLS + c] != exp_edge) {
                return fail_at("edge_lut", r, c, edges[r * COLS + c], exp_edge);
            }
            int8_t exp_thresh = exp_edge > 0 ? 1 : 0;
            if (thresholded[r * COLS + c] != exp_thresh) {
                return fail_at("threshold", r, c,
                               thresholded[r * COLS + c], exp_thresh);
            }
        }
    }
    printf("PASS: edge/LUT stem and threshold store\n");
    return 0;
}

static int check_shift_h(const int8_t *src, const int8_t *got, unsigned off)
{
    for (unsigned r = 0; r < ROWS; r++) {
        for (unsigned c = 0; c < COLS; c++) {
            int8_t exp = c < off ? 0 : src[r * COLS + c - off];
            if (got[r * COLS + c] != exp) {
                return fail_at("horizontal shift", r, c, got[r * COLS + c], exp);
            }
        }
    }
    printf("PASS: horizontal shifted tap\n");
    return 0;
}

static int check_shift_v(const int8_t *src, const int8_t *got, unsigned off)
{
    for (unsigned r = 0; r < ROWS; r++) {
        for (unsigned c = 0; c < COLS; c++) {
            int8_t exp = r < off ? 0 : src[(r - off) * COLS + c];
            if (got[r * COLS + c] != exp) {
                return fail_at("vertical shift", r, c, got[r * COLS + c], exp);
            }
        }
    }
    printf("PASS: vertical shifted tap\n");
    return 0;
}

static int check_row_max(const int8_t *src, const int8_t *scores)
{
    for (unsigned r = 0; r < ROWS; r++) {
        int8_t exp = src[r * COLS];
        for (unsigned c = 1; c < COLS; c++) {
            int8_t v = src[r * COLS + c];
            if (v > exp) {
                exp = v;
            }
        }
        if (scores[r * COLS] != exp) {
            return fail_at("row max", r, 0, scores[r * COLS], exp);
        }
    }
    printf("PASS: row max pooling\n");
    return 0;
}

static void init_tagged_logits(int32_t *p)
{
    static const int32_t logits[10] = {12, 3, 44, 9, 7, 91, 5, 18, 33, 29};

    for (unsigned r = 0; r < ROWS; r++) {
        for (unsigned i = 0; i < TAGGED_WORDS_PER_ROW; i++) {
            p[r * TAGGED_WORDS_PER_ROW + i] =
                i < 10u ? ((logits[i] << 4) | (int32_t)i) : 0;
        }
    }
}

static int issue_shift_acc(uint32_t src_pa, uint32_t dst_pa, unsigned off,
                           uint8_t vertical)
{
    if (cnn2d_issue_primitive(CNN2D_PRIM_LOAD_FEATURE, src_pa, 0) < 0) {
        perror("load feature");
        return -1;
    }
    if (cnn2d_issue_primitive(CNN2D_PRIM_ACC_ZERO, 0, 0) < 0) {
        perror("zero acc");
        return -1;
    }
    if (cnn2d_issue_primitive(CNN2D_PRIM_SHIFT_POS, off, vertical) < 0) {
        perror("shift pos");
        return -1;
    }
    if (cnn2d_issue_primitive(CNN2D_PRIM_ACC_ADD_POS, 0, vertical) < 0) {
        perror("acc add pos");
        return -1;
    }
    if (cnn2d_issue_primitive(CNN2D_PRIM_STORE_ACC, dst_pa, 0) < 0) {
        perror("store acc");
        return -1;
    }
    return 0;
}

static int issue_shift_direct(uint32_t src_pa, uint32_t dst_pa, unsigned off,
                              uint8_t vertical)
{
    if (cnn2d_issue_primitive(CNN2D_PRIM_LOAD_FEATURE, src_pa, 0) < 0) {
        perror("load feature");
        return -1;
    }
    if (cnn2d_issue_primitive(CNN2D_PRIM_DEBUG_ZERO_POS, 0, 0) < 0) {
        perror("zero pos tap");
        return -1;
    }
    if (cnn2d_issue_primitive(CNN2D_PRIM_SHIFT_POS, off, vertical) < 0) {
        perror("shift pos");
        return -1;
    }
    if (cnn2d_issue_primitive(CNN2D_PRIM_STORE_POS, dst_pa, 0) < 0) {
        perror("store pos");
        return -1;
    }
    return 0;
}

int main(void)
{
    struct t1_buf arena_buf = {0};
    struct cnn2d_arena arena = {0};
    struct cnn2d_buf_view input = {0};
    struct cnn2d_buf_view aux = {0};
    struct cnn2d_buf_view out_buf = {0};
    struct cnn2d_buf_view h_out = {0};
    struct cnn2d_buf_view v_out = {0};
    int rc = 1;

    if (t1_init() < 0) {
        perror("t1_init");
        return 1;
    }
    if (t1_buf_alloc(&arena_buf, ARENA_BYTES) < 0) {
        perror("t1_buf_alloc(arena)");
        goto out;
    }

    cnn2d_arena_init(&arena, &arena_buf);
    if (cnn2d_arena_alloc(&arena, MAP_BYTES, COLS, &input) < 0 ||
        cnn2d_arena_alloc(&arena, MAP_BYTES, COLS, &aux) < 0 ||
        cnn2d_arena_alloc(&arena, MAP_BYTES, COLS, &out_buf) < 0 ||
        cnn2d_arena_alloc(&arena, MAP_BYTES, COLS, &h_out) < 0 ||
        cnn2d_arena_alloc(&arena, MAP_BYTES, COLS, &v_out) < 0) {
        perror("cnn2d_arena_alloc");
        goto out;
    }

    init_input((int8_t *)input.va);
    init_lut((int8_t *)aux.va);
    memset(out_buf.va, 0, out_buf.size);

    if (t1_buf_sync_for_device(&arena_buf) < 0) {
        perror("sync_for_device");
        goto out;
    }

    if (cnn2d_issue_edge_lut(input.pa, aux.pa, out_buf.pa) < 0) {
        perror("cnn2d_issue_edge_lut");
        goto out;
    }
    if (cnn2d_issue_threshold_store(0, out_buf.pa) < 0) {
        perror("cnn2d_issue_threshold_store");
        goto out;
    }
    if (t1_buf_sync_for_cpu(&arena_buf) < 0) {
        perror("sync_for_cpu(edge outputs)");
        goto out;
    }
    if (check_edge_and_threshold((const int8_t *)input.va,
                                 (const int8_t *)aux.va,
                                 (const int8_t *)out_buf.va,
                                 (const int8_t *)out_buf.va) < 0) {
        goto out;
    }

    memset(input.va, 0, input.size);
    init_feature((int8_t *)input.va);
    memset(h_out.va, 0, h_out.size);
    if (t1_buf_sync_for_device(&arena_buf) < 0) {
        perror("sync_for_device(shift h)");
        goto out;
    }
    if (issue_shift_acc(input.pa, h_out.pa, 3u, 0) < 0) {
        goto out;
    }
    if (t1_buf_sync_for_cpu(&arena_buf) < 0) {
        perror("sync_for_cpu(shift h)");
        goto out;
    }
    if (check_shift_h((const int8_t *)input.va, (const int8_t *)h_out.va, 3u) < 0) {
        goto out;
    }

    memset(v_out.va, 0, v_out.size);
    if (t1_buf_sync_for_device(&arena_buf) < 0) {
        perror("sync_for_device(shift v)");
        goto out;
    }
    if (issue_shift_direct(input.pa, v_out.pa, 2u, 1) < 0) {
        goto out;
    }
    if (t1_buf_sync_for_cpu(&arena_buf) < 0) {
        perror("sync_for_cpu(shift v)");
        goto out;
    }
    if (check_shift_v((const int8_t *)input.va, (const int8_t *)v_out.va, 2u) < 0) {
        goto out;
    }

    memset(out_buf.va, 0, out_buf.size);
    if (t1_buf_sync_for_device(&arena_buf) < 0) {
        perror("sync_for_device(row_scores)");
        goto out;
    }
    if (cnn2d_issue_row_pool(input.pa, out_buf.pa, 1) < 0) {
        perror("cnn2d_issue_row_pool");
        goto out;
    }
    if (t1_buf_sync_for_cpu(&arena_buf) < 0) {
        perror("sync_for_cpu(row_scores)");
        goto out;
    }
    if (check_row_max((const int8_t *)input.va, (const int8_t *)out_buf.va) < 0) {
        goto out;
    }

    init_tagged_logits((int32_t *)aux.va);
    if (t1_buf_sync_for_device(&arena_buf) < 0) {
        perror("sync_for_device(tagged)");
        goto out;
    }
    uint32_t tagged_winner = 0;
    if (cnn2d_issue_tagged_argmax(aux.pa, &tagged_winner) < 0) {
        perror("cnn2d_issue_tagged_argmax");
        goto out;
    }
    if ((tagged_winner & 0xFu) != 5u) {
        fprintf(stderr, "FAIL: tagged argmax got 0x%08x expected class 5\n",
                tagged_winner);
        goto out;
    }
    printf("PASS: tagged argmax class=%u tagged=0x%08x\n",
           tagged_winner & 0xFu, tagged_winner);

    printf("PASS: cnn2d_decoder_probe\n");
    rc = 0;

out:
    t1_buf_free(&arena_buf);
    t1_close();
    return rc;
}
