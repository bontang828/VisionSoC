#pragma once

#include <stddef.h>
#include <stdint.h>

struct t1_buf;

#ifdef __cplusplus
extern "C" {
#endif

#define CNN2D_ROWS 128u
#define CNN2D_COLS 128u
#define CNN2D_MAP_BYTES (CNN2D_ROWS * CNN2D_COLS)

/*
 * vtype = e32, m4, ta, ma. Used by the tagged-logit argmax helper.
 * e8/m4 uses T1_VTYPE_E8_M4_TA_MA from libt1_regs.h.
 */
#define CNN2D_VTYPE_E32_M4_TA_MA 0x000000D2u

enum cnn2d_prim {
    CNN2D_PRIM_EDGE_LOAD_INPUT       = 0,
    CNN2D_PRIM_EDGE_LOAD_LUT         = 1,
    CNN2D_PRIM_DEBUG_ZERO_POS        = 2,
    CNN2D_PRIM_DEBUG_ZERO_NEG        = 3,
    CNN2D_PRIM_EDGE_STORE            = 22,

    CNN2D_PRIM_LOAD_FEATURE          = 23,
    CNN2D_PRIM_ACC_ZERO              = 24,
    CNN2D_PRIM_ACC_SEED_FEATURE      = 25,
    CNN2D_PRIM_SHIFT_POS             = 26,
    CNN2D_PRIM_SHIFT_NEG             = 27,
    CNN2D_PRIM_ACC_ADD_CENTER        = 28,
    CNN2D_PRIM_ACC_ADD_POS           = 29,
    CNN2D_PRIM_ACC_ADD_NEG           = 30,
    CNN2D_PRIM_ACC_SUB_CENTER        = 31,
    CNN2D_PRIM_ACC_SUB_POS           = 32,
    CNN2D_PRIM_ACC_SUB_NEG           = 33,
    CNN2D_PRIM_ACC_ADD_BIAS          = 34,
    CNN2D_PRIM_ACC_SUB_BIAS          = 35,
    CNN2D_PRIM_MASK_GT_THRESHOLD     = 36,
    CNN2D_PRIM_BINARY_ZERO           = 37,
    CNN2D_PRIM_BINARY_FROM_MASK      = 38,
    CNN2D_PRIM_ACC_RELU_WITH_ZERO    = 39,
    CNN2D_PRIM_STORE_ACC             = 40,
    CNN2D_PRIM_STORE_BINARY          = 41,
    CNN2D_PRIM_DIAG_POS_FROM_POS     = 42,
    CNN2D_PRIM_DIAG_NEG_FROM_POS     = 43,
    CNN2D_PRIM_DIAG_POS_FROM_NEG     = 44,
    CNN2D_PRIM_DIAG_NEG_FROM_NEG     = 45,

    CNN2D_PRIM_REDUCE_ZERO           = 46,
    CNN2D_PRIM_ROW_MAX               = 47,
    CNN2D_PRIM_ROW_SUM               = 48,
    CNN2D_PRIM_EXTRACT_V20           = 49,
    CNN2D_PRIM_LOAD_TAGGED_LOGITS    = 50,
    CNN2D_PRIM_ARGMAX_ZERO           = 51,
    CNN2D_PRIM_ARGMAX_TAGGED         = 52,
    CNN2D_PRIM_STORE_POS             = 53,
    CNN2D_PRIM_STORE_NEG             = 54,
    CNN2D_PRIM_SHIFT_POS_I1          = 55,
    CNN2D_PRIM_SHIFT_NEG_I1          = 56,
    CNN2D_PRIM_SHIFT_POS_I2          = 57,
    CNN2D_PRIM_SHIFT_NEG_I2          = 58,
    CNN2D_PRIM_SHIFT_POS_I4          = 59,
    CNN2D_PRIM_SHIFT_NEG_I4          = 60,
    CNN2D_PRIM_SHIFT_POS_I8          = 61,
    CNN2D_PRIM_SHIFT_NEG_I8          = 62,
    CNN2D_PRIM_SHIFT_POS_I16         = 63,
    CNN2D_PRIM_SHIFT_NEG_I16         = 64,
    CNN2D_PRIM_SHIFT_POS_I31         = 65,
    CNN2D_PRIM_SHIFT_NEG_I31         = 66,
    CNN2D_PRIM_STORE_TAGGED_WINNER   = 67,
};

struct cnn2d_tap {
    uint8_t in_ch;
    uint8_t out_ch;
    int8_t dy;
    int8_t dx;
    int8_t sign;       /* -1 or +1; omit zero weights from the schedule */
};

struct cnn2d_buf_view {
    void *va;
    uint32_t pa;
    size_t size;
};

struct cnn2d_arena {
    struct t1_buf *backing;
    size_t offset;
};

void cnn2d_arena_init(struct cnn2d_arena *arena, struct t1_buf *backing);
int cnn2d_arena_alloc(struct cnn2d_arena *arena, size_t size, size_t align,
                      struct cnn2d_buf_view *out);

int cnn2d_issue_primitive(enum cnn2d_prim prim, uint32_t rs1,
                          uint8_t vertical_mode);
int cnn2d_issue_edge_lut(uint32_t input_pa, uint32_t lut_pa,
                         uint32_t edges_pa);
int cnn2d_issue_threshold_store(int8_t threshold, uint32_t dst_pa);
int cnn2d_issue_row_pool(uint32_t src_pa, uint32_t row_scores_pa,
                         int use_max);
int cnn2d_issue_tagged_argmax(uint32_t tagged_logits_pa, uint32_t *tagged_out);
int cnn2d_issue_tagged_argmax_store(uint32_t tagged_logits_pa,
                                    uint32_t tagged_out_pa);

#ifdef __cplusplus
}
#endif
