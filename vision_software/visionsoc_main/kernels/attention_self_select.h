/*
 * attention_self_select.h -- on-fabric ViT 8x8-patch SELF-attention.
 *
 * Same shape as attention_patch_select.h, but K and V are the LIVE frame's own
 * patches (not a fixed dictionary), so each output patch is a similarity-weighted
 * blend of all the other patches in the scene -> true global context. Per frame:
 *
 *   frame --im2col--> Qa,Qb (patch tiles)
 *          --transpose--> Kt_a=Qa^T, Kt_b=Qb^T      (K = the patches; V = the patches)
 *          --attention(Q=Qa/Qb, K=Kt_a/Kt_b, V=Qa/Qb)--> Oa,Ob
 *          --unpatchify--> out
 *
 * Reuses the im2col kernel, the attention issue sequencer, and the attention math
 * (here `attention_self[]`, identical to attention_patch[] but score shift >>14 so
 * the Q.Q self-scores never saturate). The only new on-fabric step is the
 * transpose (attention_self_transpose[]). main.c calls attention_patch_run()
 * under ACTIVE_KERNEL_PATCH_IO (same entry name as the cross-attention variant;
 * active_kernel.h includes exactly one of the two select headers).
 */
#pragma once

#include "attention_patch_im2col.h"
#include "attention_patch_im2col_issue.h"
#include "attention_self.h"              /* generated: attention_self[]  (math, >>14) */
#include "attention_self_transpose.h"    /* generated: attention_self_transpose[]     */
#include "attention_patch_issue.h"       /* reused: attention_patch_issue, struct ap_pa */
#include "attention_patch_weights.h"
#include "libt1.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ACTIVE_KERNEL            attention_self
#define ACTIVE_KERNEL_NEUTRAL_UV 1
#define ACTIVE_KERNEL_FLOW_COLOR 0
#define ACTIVE_KERNEL_PATCH_IO   1

#define SS_TILE 16384u
#define SS_FRAMEPAD (192u * 128u)

#define SS_FRAME 0u
#define SS_IDXH1 (SS_FRAME + SS_FRAMEPAD)
#define SS_IDXV  (SS_IDXH1 + SS_TILE)
#define SS_IDXH2 (SS_IDXV  + SS_TILE)
#define SS_EXP   (SS_IDXH2 + SS_TILE)
#define SS_SEED  (SS_EXP + SS_TILE)
#define SS_QA    (SS_SEED + SS_TILE)
#define SS_QB    (SS_QA + SS_TILE)
#define SS_KTA   (SS_QB + SS_TILE)
#define SS_KTB   (SS_KTA + SS_TILE)
#define SS_OA    (SS_KTB + SS_TILE)
#define SS_OB    (SS_OA + SS_TILE)
#define SS_BYTES (SS_OB + SS_TILE)

static struct t1_buf g_self_stage;
static int g_self_staged;

static int attn_self_stage(void)
{
    if (g_self_staged) return 0;
    if (t1_buf_alloc(&g_self_stage, SS_BYTES) < 0) return -1;
    uint8_t *b = (uint8_t *)g_self_stage.va;
    memset(b, 0, SS_BYTES);
    ap_build_im2col_idxH1(b + SS_IDXH1);
    ap_build_im2col_idxV (b + SS_IDXV);
    ap_build_im2col_idxH2(b + SS_IDXH2);
    ap_build_self_exp_lut(b + SS_EXP);     /* self decay (180) */
    ap_build_seed_lut    (b + SS_SEED);    /* shared seed table */
    if (t1_buf_sync_for_device(&g_self_stage) < 0) return -1;
    g_self_staged = 1;
    fprintf(stderr, "attention_self: staged index tiles + self-expLUT + seedLUT (%u KB)\n",
            (unsigned)(SS_BYTES / 1024u));
    return 0;
}

/* on-fabric transpose tile: dst = src^T (V-load + H-store). */
static inline int attn_self_transpose(uint32_t src, uint32_t dst)
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
    if (attn_self_stage() < 0) return -1;
    uint8_t  *b  = (uint8_t *)g_self_stage.va;
    uint32_t  pa = g_self_stage.pa;

    memcpy(b + SS_FRAME, in_y, (size_t)AP_IMG * AP_IMG);
    if (t1_buf_sync_for_device(&g_self_stage) < 0) return -1;

    /* patchify: frame -> Qa, Qb */
    for (int blk = 0; blk < 2; blk++) {
        struct ap_im2col_pa ip = {
            .idxh1 = pa + SS_IDXH1, .idxv = pa + SS_IDXV, .idxh2 = pa + SS_IDXH2,
            .src   = pa + SS_FRAME + (uint32_t)blk * 64u * 128u,
            .out   = pa + (blk ? SS_QB : SS_QA),
        };
        if (attention_patch_im2col_issue(attention_patch_im2col, &ip, 0) < 0) return -1;
    }

    /* K = patches^T: Kt_a = Qa^T, Kt_b = Qb^T (V = Qa, Qb directly) */
    if (attn_self_transpose(pa + SS_QA, pa + SS_KTA) < 0) return -1;
    if (attn_self_transpose(pa + SS_QB, pa + SS_KTB) < 0) return -1;

    /* self-attention: every query-block attends to all 256 keys */
    for (int qb = 0; qb < 2; qb++) {
        struct ap_pa ap = {
            .q    = pa + (qb ? SS_QB : SS_QA),
            .kt_a = pa + SS_KTA, .kt_b = pa + SS_KTB,
            .va   = pa + SS_QA,  .vb   = pa + SS_QB,
            .exp_lut = pa + SS_EXP, .seed_lut = pa + SS_SEED,
            .out  = pa + (qb ? SS_OB : SS_OA),
        };
        if (attention_patch_issue(attention_self, &ap, 0) < 0) return -1;
    }

    if (t1_buf_sync_for_cpu(&g_self_stage) < 0) return -1;

    for (int qb = 0; qb < 2; qb++) {
        const uint8_t *o = b + (qb ? SS_OB : SS_OA);
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
