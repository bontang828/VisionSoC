/*
 * attention_patch_im2col_issue.h -- issue sequencer for attention_patch_im2col.S.
 *
 * libt1 issues one word at a time supplying vtype/vl/vertical_mode/rs1. This
 * walks the 3-pass patchify (H -> V -> H) for ONE token-block; call it twice
 * (block 0 = image rows 0..63, block 1 = rows 64..127). The 3 index tiles are
 * (re)loaded each call (cheap vle8) so each call is self-contained.
 *
 * INVARIANT: the ONLY vmode=1 word is the vertical vrgather.vv (word 5). All
 * loads/stores and the two horizontal vrgathers are vmode=0, SEW=8.
 *
 * AP_IM2COL_IDXV_VMODE selects how the vertical-gather index tile is loaded
 * (0 = horizontal, 1 = vertical). Start 0; flip to 1 if the M2 checkpoint shows
 * the vertical vrgather reads its index in transposed (vertical) layout.
 */
#pragma once

#include "libt1.h"
#include <stdint.h>

#define AP_E8_M1            0xC0u
#define AP_IM2COL_VL         128u
#ifndef AP_IM2COL_IDXV_VMODE
#define AP_IM2COL_IDXV_VMODE   0u   /* index-tile load mode for the V pass */
#endif

/* Physical addresses for the patchify passes (+ optional checkpoint PAs). */
struct ap_im2col_pa {
    uint32_t idxh1;     /* idxH1 tile [128][128] */
    uint32_t idxv;      /* idxV  tile [128][128] */
    uint32_t idxh2;     /* idxH2 tile [128][128] */
    uint32_t src;       /* source region (image rows blockbase..+63) */
    uint32_t out;       /* token tile [128][128] (lanes 0..63 = features) */
    /* checkpoint targets (read only when dbg != 0) */
    uint32_t dbg_m1;
    uint32_t dbg_m2;
};

static inline int ap_im2_iss(const uint32_t *K, uint32_t idx, uint8_t vmode, uint32_t rs1)
{
    struct t1_op op = {
        .instruction   = K[idx],
        .rs1           = rs1,
        .vtype         = AP_E8_M1,
        .vl            = AP_IM2COL_VL,
        .vertical_mode = vmode,
    };
    return t1_issue(&op);
}

/* Run the 3-pass patchify for one token-block: pa->src -> pa->out.
 * dbg=1 also stores the M1 and M2 intermediates to pa->dbg_m1 / pa->dbg_m2.   */
static inline int attention_patch_im2col_issue(const uint32_t *K,
                                               const struct ap_im2col_pa *pa, int dbg)
{
    /* PRE: load the three index tiles */
    if (ap_im2_iss(K, 0, 0,                     pa->idxh1) < 0) return -1; /* idxH1 (H) */
    if (ap_im2_iss(K, 1, AP_IM2COL_IDXV_VMODE,  pa->idxv)  < 0) return -1; /* idxV      */
    if (ap_im2_iss(K, 2, 0,                     pa->idxh2) < 0) return -1; /* idxH2 (H) */

    /* BODY */
    if (ap_im2_iss(K, 3, 0, pa->src) < 0) return -1; /* vle8 S            */
    if (ap_im2_iss(K, 4, 0, 0)       < 0) return -1; /* vrgather H1 -> M1 */
    if (ap_im2_iss(K, 5, 1, 0)       < 0) return -1; /* vrgather V  -> M2 */
    if (ap_im2_iss(K, 6, 0, 0)       < 0) return -1; /* vrgather H2 -> T  */

    if (dbg) {
        if (ap_im2_iss(K, 8, 0, pa->dbg_m1) < 0) return -1; /* store M1 */
        if (ap_im2_iss(K, 9, 0, pa->dbg_m2) < 0) return -1; /* store M2 */
    }
    if (ap_im2_iss(K, 7, 0, pa->out) < 0) return -1;        /* store T   */
    return 0;
}
