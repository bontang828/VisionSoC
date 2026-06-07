/*
 * attention_patch_select.h -- activate the on-fabric ViT 8x8-patch attention in
 * the camera pipeline. The ENTIRE tokenisation + attention runs on the vector
 * fabric (near-sensor): a 128x128 Y frame is patchified on-fabric (im2col, 3
 * vrgather passes), blocked 256-token attention runs against a fixed
 * pseudo-random K/V dictionary, and the output is un-patchified for display.
 *
 *   per frame:  frame --im2col--> Qa,Qb --attention--> Oa,Ob --unpatchify--> out
 *
 * main.c calls attention_patch_run(in_y, out_y) per frame (ACTIVE_KERNEL_PATCH_IO).
 * All buffers are packed into one staging udmabuf (main.c's in/out use the other
 * two of the three nodes). Operands + im2col index tiles are built once. The
 * fixed-point spec, builders and C reference live in attention_patch_weights.h;
 * see fyp_doc/attention_kernel_status.md.
 */
#pragma once

#include "attention_patch_im2col.h"        /* generated: attention_patch_im2col[] */
#include "attention_patch_im2col_issue.h"
#include "attention_patch.h"               /* generated: attention_patch[]        */
#include "attention_patch_issue.h"
#include "attention_patch_weights.h"
#include "libt1.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ACTIVE_KERNEL            attention_patch
#define ACTIVE_KERNEL_NEUTRAL_UV 1
#define ACTIVE_KERNEL_FLOW_COLOR 0
#define ACTIVE_KERNEL_PATCH_IO   1

#define AP_TILE 16384u
#define AP_FRAMEPAD (192u * 128u)          /* 128 real rows + 64 pad for block-1 src */

/* staging-udmabuf offsets */
#define S_FRAME 0u
#define S_IDXH1 (S_FRAME + AP_FRAMEPAD)
#define S_IDXV  (S_IDXH1 + AP_TILE)
#define S_IDXH2 (S_IDXV  + AP_TILE)
#define S_KTA   (S_IDXH2 + AP_TILE)
#define S_KTB   (S_KTA + AP_TILE)
#define S_VA    (S_KTB + AP_TILE)
#define S_VB    (S_VA + AP_TILE)
#define S_EXP   (S_VB + AP_TILE)
#define S_SEED  (S_EXP + AP_TILE)
#define S_QA    (S_SEED + AP_TILE)
#define S_QB    (S_QA + AP_TILE)
#define S_OA    (S_QB + AP_TILE)
#define S_OB    (S_OA + AP_TILE)
#define S_BYTES (S_OB + AP_TILE)

static struct t1_buf g_ap_stage;
static int g_ap_staged;

static int attn_patch_stage(void)
{
    if (g_ap_staged) return 0;
    if (t1_buf_alloc(&g_ap_stage, S_BYTES) < 0) return -1;
    uint8_t *b = (uint8_t *)g_ap_stage.va;
    memset(b, 0, S_BYTES);
    ap_build_im2col_idxH1(b + S_IDXH1);
    ap_build_im2col_idxV (b + S_IDXV);
    ap_build_im2col_idxH2(b + S_IDXH2);
    ap_build_Kt_block(0, b + S_KTA);
    ap_build_Kt_block(1, b + S_KTB);
    ap_build_V_block(0, b + S_VA);
    ap_build_V_block(1, b + S_VB);
    ap_build_exp_lut(b + S_EXP);
    ap_build_seed_lut(b + S_SEED);
    if (t1_buf_sync_for_device(&g_ap_stage) < 0) return -1;
    g_ap_staged = 1;
    fprintf(stderr, "attention_patch: staged index tiles + K/V + LUTs (%u KB)\n",
            (unsigned)(S_BYTES / 1024u));
    return 0;
}

/* On-fabric patch attention on a 128x128 Y frame -> 128x128 Y display frame. */
static inline int attention_patch_run(const uint8_t *in_y, uint8_t *out_y)
{
    if (attn_patch_stage() < 0) return -1;
    uint8_t  *b  = (uint8_t *)g_ap_stage.va;
    uint32_t  pa = g_ap_stage.pa;

    /* frame -> padded staging region (rows 128..191 stay 0 for block-1 src) */
    memcpy(b + S_FRAME, in_y, (size_t)AP_IMG * AP_IMG);
    if (t1_buf_sync_for_device(&g_ap_stage) < 0) return -1;

    /* on-fabric patchify: frame -> Qa, Qb */
    for (int blk = 0; blk < 2; blk++) {
        struct ap_im2col_pa ip = {
            .idxh1 = pa + S_IDXH1, .idxv = pa + S_IDXV, .idxh2 = pa + S_IDXH2,
            .src   = pa + S_FRAME + (uint32_t)blk * 64u * 128u,
            .out   = pa + (blk ? S_QB : S_QA),
        };
        if (attention_patch_im2col_issue(attention_patch_im2col, &ip, 0) < 0) return -1;
    }

    /* blocked attention: Qa->Oa, Qb->Ob */
    for (int qb = 0; qb < 2; qb++) {
        struct ap_pa ap = {
            .q    = pa + (qb ? S_QB : S_QA),
            .kt_a = pa + S_KTA, .kt_b = pa + S_KTB,
            .va   = pa + S_VA,  .vb   = pa + S_VB,
            .exp_lut = pa + S_EXP, .seed_lut = pa + S_SEED,
            .out  = pa + (qb ? S_OB : S_OA),
        };
        if (attention_patch_issue(attention_patch, &ap, 0) < 0) return -1;
    }

    if (t1_buf_sync_for_cpu(&g_ap_stage) < 0) return -1;

    /* un-patchify O tiles (pitch 128, feat 0..63) -> display frame */
    for (int qb = 0; qb < 2; qb++) {
        const uint8_t *o = b + (qb ? S_OB : S_OA);
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
