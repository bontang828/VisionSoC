/*
 * attention_self_centered_select.h -- on-fabric ViT self-attention with PARTIAL
 * per-patch mean-centering (alpha=0.5) to halve the brightness bias while
 * avoiding the flat-patch degeneracy of full centering.
 *
 *   frame --im2col--> Qa,Qb (raw)
 *          --partial-center--> Ca,Cb            (c = clamp(raw - mean/2 + 64,0,255))
 *          --transpose--> Kt_a=Ca^T, Kt_b=Cb^T  (K = partial-centered patches)
 *          --attention(Q=Ca/Cb, K=Kt_a/Kt_b, V=Qa/Qb RAW)--> Oa,Ob
 *          --unpatchify--> out
 *
 * Reuses im2col, the transpose, the attention math (attention_self[], >>14), and
 * the attention issue sequencer unchanged; the only new step is the partial
 * centering (attention_self_centered[]). V is the RAW patches so the output is
 * real pixels. main.c calls attention_patch_run() under ACTIVE_KERNEL_PATCH_IO.
 * The cross-attention (attention_patch) and row (attention) kernels are untouched.
 */
#pragma once

#include "attention_patch_im2col.h"
#include "attention_patch_im2col_issue.h"
#include "attention_self.h"               /* attention math (>>14)            */
#include "attention_self_transpose.h"
#include "attention_self_centered.h"      /* partial-centering kernel          */
#include "attention_patch_issue.h"        /* attention_patch_issue, struct ap_pa, ap_iss */
#include "attention_patch_weights.h"
#include "libt1.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ACTIVE_KERNEL            attention_self
#define ACTIVE_KERNEL_NEUTRAL_UV 1
#define ACTIVE_KERNEL_FLOW_COLOR 0
#define ACTIVE_KERNEL_PATCH_IO   1

#define SC_TILE 16384u
#define SC_FRAMEPAD (192u * 128u)

#define SC_FRAME 0u
#define SC_IDXH1 (SC_FRAME + SC_FRAMEPAD)
#define SC_IDXV  (SC_IDXH1 + SC_TILE)
#define SC_IDXH2 (SC_IDXV  + SC_TILE)
#define SC_EXP   (SC_IDXH2 + SC_TILE)
#define SC_SEED  (SC_EXP + SC_TILE)
#define SC_QA    (SC_SEED + SC_TILE)
#define SC_QB    (SC_QA + SC_TILE)
#define SC_CA    (SC_QB + SC_TILE)
#define SC_CB    (SC_CA + SC_TILE)
#define SC_KTA   (SC_CB + SC_TILE)
#define SC_KTB   (SC_KTA + SC_TILE)
#define SC_OA    (SC_KTB + SC_TILE)
#define SC_OB    (SC_OA + SC_TILE)
#define SC_BYTES (SC_OB + SC_TILE)

static struct t1_buf g_sc_stage;
static int g_sc_staged;

static int attn_sc_stage(void)
{
    if (g_sc_staged) return 0;
    if (t1_buf_alloc(&g_sc_stage, SC_BYTES) < 0) return -1;
    uint8_t *b = (uint8_t *)g_sc_stage.va;
    memset(b, 0, SC_BYTES);
    ap_build_im2col_idxH1(b + SC_IDXH1);
    ap_build_im2col_idxV (b + SC_IDXV);
    ap_build_im2col_idxH2(b + SC_IDXH2);
    ap_build_selfc_exp_lut(b + SC_EXP);    /* partial-centered decay (110) */
    ap_build_seed_lut     (b + SC_SEED);
    if (t1_buf_sync_for_device(&g_sc_stage) < 0) return -1;
    g_sc_staged = 1;
    fprintf(stderr, "attention_self_centered: staged idx + selfc-expLUT + seedLUT (%u KB)\n",
            (unsigned)(SC_BYTES / 1024u));
    return 0;
}

/* partial-center one tile (src -> dst): attention_self_centered[] 13-word stream. */
static inline int attn_sc_center(uint32_t src, uint32_t dst)
{
    const uint32_t *K = attention_self_centered;
    if (ap_iss(K, 0,  AP_E8_M1,  64, 0, src) < 0) return -1; /* vle8 raw            */
    if (ap_iss(K, 1,  AP_E8_M1,  64, 0, 0)   < 0) return -1; /* vmv.v.i 0           */
    if (ap_iss(K, 2,  AP_E8_M1,  64, 0, 0)   < 0) return -1; /* vwredsumu sum       */
    if (ap_iss(K, 3,  AP_E8_M1,   1, 0, 0)   < 0) return -1; /* vnsrl >>7 half-mean */
    if (ap_iss(K, 4,  AP_E8_M1,  64, 0, 0)   < 0) return -1; /* vrgather.vx bcast   */
    if (ap_iss(K, 5,  AP_E8_M1,  64, 0, 0)   < 0) return -1; /* vwaddu raw->u16     */
    if (ap_iss(K, 6,  AP_E8_M1,  64, 0, 0)   < 0) return -1; /* vwaddu hmean->u16   */
    if (ap_iss(K, 7,  AP_E16_M2, 64, 0, 0)   < 0) return -1; /* vsub t=raw-mean/2   */
    if (ap_iss(K, 8,  AP_E16_M2, 64, 0, 128) < 0) return -1; /* vadd +128           */
    if (ap_iss(K, 9,  AP_E16_M2, 64, 0, 255) < 0) return -1; /* vminu 255           */
    if (ap_iss(K, 10, AP_E8_M1,  64, 0, 0)   < 0) return -1; /* vnsrl -> u8         */
    if (ap_iss(K, 11, AP_E8_M1,  64, 0, dst) < 0) return -1; /* vse8 centered       */
    return 0;
}

/* on-fabric transpose (V-load + H-store): dst = src^T. */
static inline int attn_sc_transpose(uint32_t src, uint32_t dst)
{
    struct t1_op v = { .instruction = attention_self_transpose[0], .rs1 = src,
                       .vtype = AP_E8_M1, .vl = 128, .vertical_mode = 1 };
    if (t1_issue(&v) < 0) return -1;
    struct t1_op h = { .instruction = attention_self_transpose[1], .rs1 = dst,
                       .vtype = AP_E8_M1, .vl = 128, .vertical_mode = 0 };
    return t1_issue(&h);
}

static inline int attention_patch_run(const uint8_t *in_y, uint8_t *out_y)
{
    if (attn_sc_stage() < 0) return -1;
    uint8_t  *b  = (uint8_t *)g_sc_stage.va;
    uint32_t  pa = g_sc_stage.pa;

    memcpy(b + SC_FRAME, in_y, (size_t)AP_IMG * AP_IMG);
    if (t1_buf_sync_for_device(&g_sc_stage) < 0) return -1;

    /* patchify -> raw tiles Qa, Qb */
    for (int blk = 0; blk < 2; blk++) {
        struct ap_im2col_pa ip = {
            .idxh1 = pa + SC_IDXH1, .idxv = pa + SC_IDXV, .idxh2 = pa + SC_IDXH2,
            .src   = pa + SC_FRAME + (uint32_t)blk * 64u * 128u,
            .out   = pa + (blk ? SC_QB : SC_QA),
        };
        if (attention_patch_im2col_issue(attention_patch_im2col, &ip, 0) < 0) return -1;
    }

    /* partial-center -> Ca, Cb; transpose -> Kt_a, Kt_b */
    if (attn_sc_center(pa + SC_QA, pa + SC_CA) < 0) return -1;
    if (attn_sc_center(pa + SC_QB, pa + SC_CB) < 0) return -1;
    if (attn_sc_transpose(pa + SC_CA, pa + SC_KTA) < 0) return -1;
    if (attn_sc_transpose(pa + SC_CB, pa + SC_KTB) < 0) return -1;

    /* self-attention: Q=K=centered, V=raw patches */
    for (int qb = 0; qb < 2; qb++) {
        struct ap_pa ap = {
            .q    = pa + (qb ? SC_CB : SC_CA),
            .kt_a = pa + SC_KTA, .kt_b = pa + SC_KTB,
            .va   = pa + SC_QA,  .vb   = pa + SC_QB,      /* RAW patches */
            .exp_lut = pa + SC_EXP, .seed_lut = pa + SC_SEED,
            .out  = pa + (qb ? SC_OB : SC_OA),
        };
        if (attention_patch_issue(attention_self, &ap, 0) < 0) return -1;
    }

    if (t1_buf_sync_for_cpu(&g_sc_stage) < 0) return -1;

    for (int qb = 0; qb < 2; qb++) {
        const uint8_t *o = b + (qb ? SC_OB : SC_OA);
        for (int tb = 0; tb < 128; tb++) {
            int t = qb * 128 + tb, Py = t / 16, Px = t % 16;
            for (int f = 0; f < (int)AP_FEAT; f++) {
                int ry = f / 8, rx = f % 8;
                out_y[(Py * 8 + ry) * (int)AP_IMG + (Px * 8 + rx)] = o[tb * 128 + f];
            }
        }
    }
    return 0;
}
