/*
 * attention_issue.h -- shared issue sequencer for kernels/attention.S.
 *
 * libt1 issues one word at a time and supplies vtype/vl/vertical_mode/rs1 per
 * issue; this header walks attention[] in order with the correct per-word
 * schedule. Used by BOTH the board probe (libt1/test/attention_probe.c, with
 * dbg=1 to emit per-phase checkpoint stores) and the production select.h
 * (dbg=0, only the final O store).
 *
 * INVARIANT: vmode=1 (vertical) is set ONLY for the SEW=8 vle8 V-loads and the
 * SEW=8 vrgather.vx row-broadcasts. Every widening / SEW>8 word is vmode=0.
 * See the project_vertical_mode_sew8_only memory.
 *
 * Region offsets MUST match the index map in attention.S.
 */
#pragma once

#include "libt1.h"
#include <stdint.h>

/* vtype encodings (vma=1,vta=1): 0xC0 | (vsew<<3) | vlmul */
#define ATTN_E8_M1  0xC0u
#define ATTN_E16_M1 0xC8u
#define ATTN_E16_M2 0xC9u
#define ATTN_E32_M1 0xD0u

/* Newton / saturation scalar constants injected as rs1 on .vx words. */
#define ATTN_C_255   255u
#define ATTN_C_127   127u
#define ATTN_C_1       1u
#define ATTN_C_2       2u
#define ATTN_C_2P17  131072u   /* 1 << 17 */

#define ATTN_N_TOKENS 128

/* Physical addresses of the staged buffers + optional checkpoint debug PAs. */
struct attn_pa {
    uint32_t q;          /* Q  [N][D]                */
    uint32_t kt;         /* Kt [D][N] = K^T          */
    uint32_t v;          /* V  [N][D]                */
    uint32_t exp_lut;    /* replicated expLUT  [N][128] u8  */
    uint32_t seed_lut;   /* replicated seedLUT [N][128] u8 (R0>>1) */
    uint32_t out;        /* O  [N][D]                */
    /* checkpoint targets (only read when dbg != 0) */
    uint32_t dbg_s8;
    uint32_t dbg_e;
    uint32_t dbg_z;      /* u32, one per row at byte row*128 (vl=1 store) */
    uint32_t dbg_r;      /* u32, one per row at byte row*128 (vl=1 store) */
    uint32_t dbg_pq;
};

/* Issue word K[idx] with an explicit schedule entry. */
static inline int attn_iss(const uint32_t *K, uint32_t idx, uint32_t vtype,
                           uint32_t vl, uint8_t vmode, uint32_t rs1)
{
    struct t1_op op = {
        .instruction   = K[idx],
        .rs1           = rs1,
        .vtype         = vtype,
        .vl            = vl,
        .vertical_mode = vmode,
    };
    return t1_issue(&op);
}

/* Issue one matmul body (QK or PV): 9 words at `base`, gather index `k`.
 *   base=6  -> QK (a5=key j),  base=47 -> PV (a5=value v).
 * Narrow shift is encoded in the .S word, so QK(>>S_SHIFT) and PV(>>O_SHIFT)
 * differ only by their stored immediate -- same schedule here. */
static inline int attn_body(const uint32_t *K, uint32_t base, uint32_t k)
{
    if (attn_iss(K, base + 0, ATTN_E8_M1,  128, 1, k)         < 0) return -1; /* vrgather.vx (V) */
    if (attn_iss(K, base + 1, ATTN_E8_M1,  128, 0, 0)         < 0) return -1; /* vwmulu.vv      */
    if (attn_iss(K, base + 2, ATTN_E16_M2, 128, 0, 0)         < 0) return -1; /* vwredsumu.vs   */
    if (attn_iss(K, base + 3, ATTN_E16_M1,   1, 0, 0)         < 0) return -1; /* vnsrl.wi >>sh  */
    if (attn_iss(K, base + 4, ATTN_E16_M1,   1, 0, ATTN_C_255)< 0) return -1; /* vminu.vx 255   */
    if (attn_iss(K, base + 5, ATTN_E8_M1,    1, 0, 0)         < 0) return -1; /* vnsrl.wi ->u8  */
    if (attn_iss(K, base + 6, ATTN_E8_M1,  128, 0, 0)         < 0) return -1; /* vslideup.vi    */
    if (attn_iss(K, base + 7, ATTN_E8_M1,  128, 0, 0)         < 0) return -1; /* vmerge.vvm     */
    if (attn_iss(K, base + 8, ATTN_E8_M1,  128, 0, 0)         < 0) return -1; /* vmv.v.v        */
    return 0;
}

/*
 * Issue the whole attention kernel.
 *   K   = the assembled attention[] words.
 *   pa  = staged buffer PAs (+ checkpoint PAs if dbg).
 *   dbg = 0 production (only final O store); 1 probe (emit all checkpoints).
 */
static inline int attention_issue(const uint32_t *K, const struct attn_pa *pa, int dbg)
{
    /* ----- PRE [0,6) ----- */
    if (attn_iss(K, 0, ATTN_E8_M1, 128, 0, pa->q)  < 0) return -1; /* vle8 Q (H)        */
    if (attn_iss(K, 1, ATTN_E8_M1, 128, 1, pa->kt) < 0) return -1; /* vle8 Kt (V-load)  */
    if (attn_iss(K, 2, ATTN_E8_M1, 128, 0, 0)      < 0) return -1; /* vid.v             */
    if (attn_iss(K, 3, ATTN_E8_M1, 128, 0, 0)      < 0) return -1; /* vmseq.vi          */
    if (attn_iss(K, 4, ATTN_E8_M1, 128, 0, 0)      < 0) return -1; /* vmv.v.i v6,0      */
    if (attn_iss(K, 5, ATTN_E8_M1, 128, 0, 0)      < 0) return -1; /* vmv.v.i v28,0     */

    /* ----- QK body x128: key columns j = 127..0 ----- */
    for (int j = ATTN_N_TOKENS - 1; j >= 0; j--)
        if (attn_body(K, 6, (uint32_t)j) < 0) return -1;
    if (dbg && attn_iss(K, 56, ATTN_E8_M1, 128, 0, pa->dbg_s8) < 0) return -1; /* store S8 */

    /* ----- SM_EXP [15,21) ----- */
    if (attn_iss(K, 15, ATTN_E8_M1, 128, 0, 0)            < 0) return -1; /* vredmax.vs      */
    if (attn_iss(K, 16, ATTN_E8_M1, 128, 0, 0)            < 0) return -1; /* vrgather.vx x0  */
    if (attn_iss(K, 17, ATTN_E8_M1, 128, 0, 0)            < 0) return -1; /* vssubu.vv       */
    if (attn_iss(K, 18, ATTN_E8_M1, 128, 0, ATTN_C_127)   < 0) return -1; /* vminu.vx 127    */
    if (attn_iss(K, 19, ATTN_E8_M1, 128, 0, pa->exp_lut)  < 0) return -1; /* vle8 expLUT     */
    if (attn_iss(K, 20, ATTN_E8_M1, 128, 0, 0)            < 0) return -1; /* vrgather.vv e   */
    if (dbg && attn_iss(K, 57, ATTN_E8_M1, 128, 0, pa->dbg_e) < 0) return -1; /* store e */

    /* ----- SM_Z [21,23) ----- */
    if (attn_iss(K, 21, ATTN_E8_M1,  128, 0, 0)        < 0) return -1; /* vwredsumu.vs Z16 (e8->16) */
    if (attn_iss(K, 22, ATTN_E16_M1,   1, 0, ATTN_C_1) < 0) return -1; /* vwmulu.vx Z32 (16->32)    */
    if (dbg && attn_iss(K, 58, ATTN_E32_M1, 1, 0, pa->dbg_z) < 0) return -1; /* store Z (vl=1) */

    /* ----- SM_NEWTON [23,40): seed + 3 iters (SEW=32) ----- */
    if (attn_iss(K, 23, ATTN_E8_M1,  1, 0, 0)            < 0) return -1; /* vnsrl.wi idx8=Z16>>8  */
    if (attn_iss(K, 24, ATTN_E8_M1, 128, 0, pa->seed_lut)< 0) return -1; /* vle8 seedLUT          */
    if (attn_iss(K, 25, ATTN_E8_M1, 128, 0, 0)           < 0) return -1; /* vrgather.vv seed8     */
    if (attn_iss(K, 26, ATTN_E8_M1,  1, 0, ATTN_C_2)     < 0) return -1; /* vwmulu.vx seed16=seed*2 */
    if (attn_iss(K, 27, ATTN_E16_M1, 1, 0, ATTN_C_1)     < 0) return -1; /* vwmulu.vx R0_32       */
    for (int it = 0; it < 3; it++) {
        uint32_t b = 28 + (uint32_t)it * 4;
        if (attn_iss(K, b + 0, ATTN_E32_M1, 1, 0, 0)         < 0) return -1; /* vmul t=Z*R    */
        if (attn_iss(K, b + 1, ATTN_E32_M1, 1, 0, ATTN_C_2P17)< 0) return -1; /* vrsub 2^17-t */
        if (attn_iss(K, b + 2, ATTN_E32_M1, 1, 0, 0)         < 0) return -1; /* vmul R=R*t    */
        if (attn_iss(K, b + 3, ATTN_E32_M1, 1, 0, 0)         < 0) return -1; /* vsrl >>16     */
    }
    if (dbg && attn_iss(K, 59, ATTN_E32_M1, 1, 0, pa->dbg_r) < 0) return -1; /* store R (vl=1) */

    /* ----- SM_NORM [40,45): pq = (e*R)>>8 ----- */
    if (attn_iss(K, 40, ATTN_E16_M1,   1, 0, 0)        < 0) return -1; /* vnsrl.wi R16          */
    if (attn_iss(K, 41, ATTN_E16_M2, 128, 0, 0)        < 0) return -1; /* vrgather.vx Rb16 (x0) */
    if (attn_iss(K, 42, ATTN_E8_M1,  128, 0, ATTN_C_1) < 0) return -1; /* vwmulu.vx e16         */
    if (attn_iss(K, 43, ATTN_E16_M2, 128, 0, 0)        < 0) return -1; /* vmul.vv P16           */
    if (attn_iss(K, 44, ATTN_E8_M1,  128, 0, 0)        < 0) return -1; /* vnsrl.wi pq           */
    if (dbg && attn_iss(K, 60, ATTN_E8_M1, 128, 0, pa->dbg_pq) < 0) return -1; /* store pq */

    /* ----- PV_PRE [45,47) ----- */
    if (attn_iss(K, 45, ATTN_E8_M1, 128, 1, pa->v) < 0) return -1; /* vle8 V (V-load) */
    if (attn_iss(K, 46, ATTN_E8_M1, 128, 0, 0)     < 0) return -1; /* vmv.v.i v22,0   */

    /* ----- PV body x128: value columns v = 127..0 ----- */
    for (int v = ATTN_N_TOKENS - 1; v >= 0; v--)
        if (attn_body(K, 47, (uint32_t)v) < 0) return -1;

    /* ----- final O store ----- */
    if (attn_iss(K, 61, ATTN_E8_M1, 128, 0, pa->out) < 0) return -1; /* vse8 O -> dst */
    return 0;
}
