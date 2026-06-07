/*
 * attention_self16_select.h -- on-fabric self-attention with a 16-BIT score path
 * (texture survives the softmax) + partial centering. Pipeline:
 *
 *   frame --im2col--> Qa,Qb (raw)
 *          --partial-center--> Ca,Cb
 *          --transpose--> Kt_a=Ca^T, Kt_b=Cb^T
 *          --attention_self16(Q=Ca/Cb, K=Kt, V=Qa/Qb raw)--> Oa,Ob
 *          --unpatchify--> out
 *
 * Reuses im2col, the partial-centering kernel, the transpose, and the ap_pa
 * issue struct; the math is attention_self16[] (16-bit score row). main.c calls
 * attention_patch_run() under ACTIVE_KERNEL_PATCH_IO.
 */
#pragma once

#include "attention_patch_im2col.h"
#include "attention_patch_im2col_issue.h"
#include "attention_self_centered.h"      /* partial-centering kernel */
#include "attention_self16.h"             /* 16-bit-score attention math */
#include "attention_self_transpose.h"
#include "attention_self16_issue.h"       /* attention_self16_issue, struct ap_pa, ap_iss */
#include "attention_patch_weights.h"
#include "libt1.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ACTIVE_KERNEL            attention_self16
#define ACTIVE_KERNEL_NEUTRAL_UV 1
#define ACTIVE_KERNEL_FLOW_COLOR 0
#define ACTIVE_KERNEL_PATCH_IO   1

#define S6_TILE 16384u
#define S6_FRAMEPAD (192u * 128u)

#define S6_FRAME 0u
#define S6_IDXH1 (S6_FRAME + S6_FRAMEPAD)
#define S6_IDXV  (S6_IDXH1 + S6_TILE)
#define S6_IDXH2 (S6_IDXV  + S6_TILE)
#define S6_EXP   (S6_IDXH2 + S6_TILE)
#define S6_SEED  (S6_EXP + S6_TILE)
#define S6_QA    (S6_SEED + S6_TILE)
#define S6_QB    (S6_QA + S6_TILE)
#define S6_CA    (S6_QB + S6_TILE)
#define S6_CB    (S6_CA + S6_TILE)
#define S6_KTA   (S6_CB + S6_TILE)
#define S6_KTB   (S6_KTA + S6_TILE)
#define S6_OA    (S6_KTB + S6_TILE)
#define S6_OB    (S6_OA + S6_TILE)
#define S6_BYTES (S6_OB + S6_TILE)

static struct t1_buf g_s6_stage;
static int g_s6_staged;

static int attn_s16_stage(void)
{
    if (g_s6_staged) return 0;
    if (t1_buf_alloc(&g_s6_stage, S6_BYTES) < 0) return -1;
    uint8_t *b = (uint8_t *)g_s6_stage.va;
    memset(b, 0, S6_BYTES);
    ap_build_im2col_idxH1(b + S6_IDXH1);
    ap_build_im2col_idxV (b + S6_IDXV);
    ap_build_im2col_idxH2(b + S6_IDXH2);
    ap_build_s16_exp_lut(b + S6_EXP);      /* 16-bit-path decay */
    ap_build_seed_lut    (b + S6_SEED);
    if (t1_buf_sync_for_device(&g_s6_stage) < 0) return -1;
    g_s6_staged = 1;
    fprintf(stderr, "attention_self16: staged idx + s16-expLUT + seedLUT (%u KB)\n",
            (unsigned)(S6_BYTES / 1024u));
    return 0;
}

/* partial-center one tile (src -> dst): attention_self_centered[] 12-word stream. */
static inline int attn_s16_center(uint32_t src, uint32_t dst)
{
    const uint32_t *K = attention_self_centered;
    if (ap_iss(K, 0,  AP_E8_M1,  64, 0, src) < 0) return -1;
    if (ap_iss(K, 1,  AP_E8_M1,  64, 0, 0)   < 0) return -1;
    if (ap_iss(K, 2,  AP_E8_M1,  64, 0, 0)   < 0) return -1;
    if (ap_iss(K, 3,  AP_E8_M1,   1, 0, 0)   < 0) return -1;
    if (ap_iss(K, 4,  AP_E8_M1,  64, 0, 0)   < 0) return -1;
    if (ap_iss(K, 5,  AP_E8_M1,  64, 0, 0)   < 0) return -1;
    if (ap_iss(K, 6,  AP_E8_M1,  64, 0, 0)   < 0) return -1;
    if (ap_iss(K, 7,  AP_E16_M2, 64, 0, 0)   < 0) return -1;
    if (ap_iss(K, 8,  AP_E16_M2, 64, 0, 128) < 0) return -1;
    if (ap_iss(K, 9,  AP_E16_M2, 64, 0, 255) < 0) return -1;
    if (ap_iss(K, 10, AP_E8_M1,  64, 0, 0)   < 0) return -1;
    if (ap_iss(K, 11, AP_E8_M1,  64, 0, dst) < 0) return -1;
    return 0;
}

static inline int attn_s16_transpose(uint32_t src, uint32_t dst)
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
    if (attn_s16_stage() < 0) return -1;
    uint8_t  *b  = (uint8_t *)g_s6_stage.va;
    uint32_t  pa = g_s6_stage.pa;

    memcpy(b + S6_FRAME, in_y, (size_t)AP_IMG * AP_IMG);
    if (t1_buf_sync_for_device(&g_s6_stage) < 0) return -1;

    for (int blk = 0; blk < 2; blk++) {
        struct ap_im2col_pa ip = {
            .idxh1 = pa + S6_IDXH1, .idxv = pa + S6_IDXV, .idxh2 = pa + S6_IDXH2,
            .src   = pa + S6_FRAME + (uint32_t)blk * 64u * 128u,
            .out   = pa + (blk ? S6_QB : S6_QA),
        };
        if (attention_patch_im2col_issue(attention_patch_im2col, &ip, 0) < 0) return -1;
    }
    if (attn_s16_center(pa + S6_QA, pa + S6_CA) < 0) return -1;
    if (attn_s16_center(pa + S6_QB, pa + S6_CB) < 0) return -1;
    if (attn_s16_transpose(pa + S6_CA, pa + S6_KTA) < 0) return -1;
    if (attn_s16_transpose(pa + S6_CB, pa + S6_KTB) < 0) return -1;

    for (int qb = 0; qb < 2; qb++) {
        struct ap_pa ap = {
            .q    = pa + (qb ? S6_CB : S6_CA),
            .kt_a = pa + S6_KTA, .kt_b = pa + S6_KTB,
            .va   = pa + S6_QA,  .vb   = pa + S6_QB,   /* RAW patches */
            .exp_lut = pa + S6_EXP, .seed_lut = pa + S6_SEED,
            .out  = pa + (qb ? S6_OB : S6_OA),
        };
        if (attention_self16_issue(attention_self16, &ap, 0) < 0) return -1;
    }

    if (t1_buf_sync_for_cpu(&g_s6_stage) < 0) return -1;

    for (int qb = 0; qb < 2; qb++) {
        const uint8_t *o = b + (qb ? S6_OB : S6_OA);
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
