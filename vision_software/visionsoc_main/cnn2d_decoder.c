#include "cnn2d_decoder.h"

#include "kernels/cnn2d_decoder_primitives.h"
#include "libt1.h"
#include "libt1_regs.h"

#include <errno.h>
#include <stdint.h>
#include <stddef.h>

static int align_up_size(size_t value, size_t align, size_t *out)
{
    if (!out || align == 0 || (align & (align - 1u)) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (value > SIZE_MAX - (align - 1u)) {
        errno = EOVERFLOW;
        return -1;
    }
    *out = (value + align - 1u) & ~(align - 1u);
    return 0;
}

void cnn2d_arena_init(struct cnn2d_arena *arena, struct t1_buf *backing)
{
    if (!arena) {
        return;
    }
    arena->backing = backing;
    arena->offset = 0;
}

int cnn2d_arena_alloc(struct cnn2d_arena *arena, size_t size, size_t align,
                      struct cnn2d_buf_view *out)
{
    if (!arena || !arena->backing || !arena->backing->va || !out ||
        size == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t aligned = 0;
    if (align_up_size(arena->offset, align, &aligned) < 0) {
        return -1;
    }
    if (size > arena->backing->size || aligned > arena->backing->size - size) {
        errno = ERANGE;
        return -1;
    }
    if (aligned > UINT32_MAX || size > UINT32_MAX - aligned) {
        errno = EOVERFLOW;
        return -1;
    }

    out->va = (uint8_t *)arena->backing->va + aligned;
    out->pa = arena->backing->pa + (uint32_t)aligned;
    out->size = size;
    arena->offset = aligned + size;
    return 0;
}

static int issue_with_vtype(enum cnn2d_prim prim, uint32_t rs1,
                            uint8_t vertical_mode, uint32_t vtype,
                            uint32_t vl)
{
    if ((size_t)prim >= cnn2d_decoder_primitives_count) {
        errno = ERANGE;
        return -1;
    }

    struct t1_op op = {
        .instruction = cnn2d_decoder_primitives[prim],
        .rs1 = rs1,
        .vtype = vtype,
        .vl = vl,
        .vertical_mode = vertical_mode ? 1u : 0u,
    };
    return t1_issue(&op);
}

int cnn2d_issue_primitive(enum cnn2d_prim prim, uint32_t rs1,
                          uint8_t vertical_mode)
{
    return issue_with_vtype(prim, rs1, vertical_mode,
                            T1_VTYPE_E8_M4_TA_MA, CNN2D_COLS);
}

int cnn2d_issue_edge_lut(uint32_t input_pa, uint32_t lut_pa,
                         uint32_t edges_pa)
{
    for (unsigned i = 0; i <= CNN2D_PRIM_EDGE_STORE; i++) {
        uint32_t rs1 = 0;
        uint8_t vertical = 0;

        if (i == CNN2D_PRIM_EDGE_LOAD_INPUT) {
            rs1 = input_pa;
        } else if (i == CNN2D_PRIM_EDGE_LOAD_LUT) {
            rs1 = lut_pa;
        } else if (i == CNN2D_PRIM_EDGE_STORE) {
            rs1 = edges_pa;
        }

        if (i >= 7u && i <= 11u) {
            vertical = 1;
        }

        if (cnn2d_issue_primitive((enum cnn2d_prim)i, rs1, vertical) < 0) {
            return -1;
        }
    }
    return 0;
}

int cnn2d_issue_threshold_store(int8_t threshold, uint32_t dst_pa)
{
    uint32_t scalar_threshold = (uint32_t)(int32_t)threshold;

    if (cnn2d_issue_primitive(CNN2D_PRIM_MASK_GT_THRESHOLD,
                              scalar_threshold, 0) < 0) {
        return -1;
    }
    if (cnn2d_issue_primitive(CNN2D_PRIM_BINARY_ZERO, 0, 0) < 0) {
        return -1;
    }
    if (cnn2d_issue_primitive(CNN2D_PRIM_BINARY_FROM_MASK, 0, 0) < 0) {
        return -1;
    }
    return cnn2d_issue_primitive(CNN2D_PRIM_STORE_BINARY, dst_pa, 0);
}

int cnn2d_issue_row_pool(uint32_t src_pa, uint32_t row_scores_pa,
                         int use_max)
{
    if (cnn2d_issue_primitive(CNN2D_PRIM_LOAD_FEATURE, src_pa, 0) < 0) {
        return -1;
    }
    if (cnn2d_issue_primitive(CNN2D_PRIM_REDUCE_ZERO, 0, 0) < 0) {
        return -1;
    }
    if (cnn2d_issue_primitive(use_max ? CNN2D_PRIM_ROW_MAX
                                     : CNN2D_PRIM_ROW_SUM,
                              0, 0) < 0) {
        return -1;
    }

    /*
     * Store one score byte per hardware row at row_scores[row][0].
     * The fixed LSU row pitch keeps rows separated even though vl=1.
     */
    return issue_with_vtype(CNN2D_PRIM_STORE_ACC, row_scores_pa, 0,
                            T1_VTYPE_E8_M4_TA_MA, 1u);
}

int cnn2d_issue_tagged_argmax(uint32_t tagged_logits_pa, uint32_t *tagged_out)
{
    if (!tagged_out) {
        errno = EINVAL;
        return -1;
    }

    if (issue_with_vtype(CNN2D_PRIM_LOAD_TAGGED_LOGITS, tagged_logits_pa, 0,
                         CNN2D_VTYPE_E32_M4_TA_MA, 10u) < 0) {
        return -1;
    }
    if (issue_with_vtype(CNN2D_PRIM_ARGMAX_ZERO, 0, 0,
                         CNN2D_VTYPE_E32_M4_TA_MA, 10u) < 0) {
        return -1;
    }
    if (issue_with_vtype(CNN2D_PRIM_ARGMAX_TAGGED, 0, 0,
                         CNN2D_VTYPE_E32_M4_TA_MA, 10u) < 0) {
        return -1;
    }
    if (issue_with_vtype(CNN2D_PRIM_EXTRACT_V20, 0, 0,
                         CNN2D_VTYPE_E32_M4_TA_MA, 10u) < 0) {
        return -1;
    }

    uint8_t rd_addr = 0;
    bool is_fp = false;
    int rc = t1_wait_rd(tagged_out, &rd_addr, &is_fp, 1000);
    if (rc < 0) {
        return -1;
    }
    if (rc == 0) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

int cnn2d_issue_tagged_argmax_store(uint32_t tagged_logits_pa,
                                    uint32_t tagged_out_pa)
{
    if (issue_with_vtype(CNN2D_PRIM_LOAD_TAGGED_LOGITS, tagged_logits_pa, 0,
                         CNN2D_VTYPE_E32_M4_TA_MA, 10u) < 0) {
        return -1;
    }
    if (issue_with_vtype(CNN2D_PRIM_ARGMAX_ZERO, 0, 0,
                         CNN2D_VTYPE_E32_M4_TA_MA, 10u) < 0) {
        return -1;
    }
    if (issue_with_vtype(CNN2D_PRIM_ARGMAX_TAGGED, 0, 0,
                         CNN2D_VTYPE_E32_M4_TA_MA, 10u) < 0) {
        return -1;
    }
    return issue_with_vtype(CNN2D_PRIM_STORE_TAGGED_WINNER, tagged_out_pa, 0,
                            CNN2D_VTYPE_E32_M4_TA_MA, 1u);
}
