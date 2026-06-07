/*
 * attention_self16_issue.h -- issue sequencer for attention_self16.S (16-bit
 * score path). One query-block over both key/value-blocks; call twice. Reuses
 * struct ap_pa, ap_iss, and the AP_E* vtype constants from attention_patch_issue.h.
 *
 * Score row is SEW=16 LMUL=2 (E16_M2): build, max, sub all at 16-bit; the diff is
 * narrowed to the 8-bit exp index. Feature-dim QK ops are vl=64; key-dim vl=128.
 */
#pragma once

#include "attention_patch_issue.h"   /* struct ap_pa, ap_iss, AP_E8_M1/E16_M1/E16_M2/E32_M1, AP_C_* */
#include "libt1.h"
#include <stdint.h>

#define S16_NKEYS 128
#define S16_NFEAT 64

/* QK body (words 6..12): builds the SEW=16 score row v6:7. */
static inline int ap16_qk_body(const uint32_t *K, uint32_t k)
{
    if (ap_iss(K, 6,  AP_E8_M1,  S16_NKEYS, 1, k) < 0) return -1; /* vrgather.vx K[j] (V: vl=hw-rows=128) */
    if (ap_iss(K, 7,  AP_E8_M1,  S16_NFEAT, 0, 0) < 0) return -1; /* vwmulu.vv (H: vl=feat=64)            */
    if (ap_iss(K, 8,  AP_E16_M2, S16_NFEAT, 0, 0) < 0) return -1; /* vwredsumu -> u32     */
    if (ap_iss(K, 9,  AP_E16_M1, 1, 0, 0)         < 0) return -1; /* vnsrl >>6 -> S16     */
    if (ap_iss(K, 10, AP_E16_M2, S16_NKEYS, 0, 0) < 0) return -1; /* vslideup (SEW16)     */
    if (ap_iss(K, 11, AP_E16_M2, S16_NKEYS, 0, 0) < 0) return -1; /* vmerge (SEW16)       */
    if (ap_iss(K, 12, AP_E16_M2, S16_NKEYS, 0, 0) < 0) return -1; /* vmv.v.v (SEW16)      */
    return 0;
}

/* PV body (words 57..68): 8-bit, reduce over 128 keys x 2 V-blocks. */
static inline int ap16_pv_body(const uint32_t *K, uint32_t d)
{
    if (ap_iss(K, 57, AP_E8_M1,  S16_NKEYS, 1, d) < 0) return -1; /* vrgather Va[:,d]  */
    if (ap_iss(K, 58, AP_E8_M1,  S16_NKEYS, 0, 0) < 0) return -1; /* vwmulu pqa*Va     */
    if (ap_iss(K, 59, AP_E16_M2, S16_NKEYS, 0, 0) < 0) return -1; /* vwredsumu accA    */
    if (ap_iss(K, 60, AP_E8_M1,  S16_NKEYS, 1, d) < 0) return -1; /* vrgather Vb[:,d]  */
    if (ap_iss(K, 61, AP_E8_M1,  S16_NKEYS, 0, 0) < 0) return -1; /* vwmulu pqb*Vb     */
    if (ap_iss(K, 62, AP_E16_M2, S16_NKEYS, 0, 0) < 0) return -1; /* vwredsumu chain   */
    if (ap_iss(K, 63, AP_E16_M1, 1, 0, 0)         < 0) return -1; /* vnsrl >>O_SHIFT   */
    if (ap_iss(K, 64, AP_E16_M1, 1, 0, AP_C_255)  < 0) return -1; /* vminu 255         */
    if (ap_iss(K, 65, AP_E8_M1,  1, 0, 0)         < 0) return -1; /* vnsrl -> u8       */
    if (ap_iss(K, 66, AP_E8_M1,  S16_NFEAT, 0, 0) < 0) return -1; /* vslideup (vl=64)  */
    if (ap_iss(K, 67, AP_E8_M1,  S16_NFEAT, 0, 0) < 0) return -1; /* vmerge            */
    if (ap_iss(K, 68, AP_E8_M1,  S16_NFEAT, 0, 0) < 0) return -1; /* vmv.v.v           */
    return 0;
}

static inline int attention_self16_issue(const uint32_t *K, const struct ap_pa *pa, int dbg)
{
    /* PRE */
    if (ap_iss(K, 0, AP_E8_M1, S16_NKEYS, 0, pa->q) < 0) return -1; /* vle8 Q   */
    if (ap_iss(K, 1, AP_E8_M1, S16_NKEYS, 0, 0)     < 0) return -1; /* vid      */
    if (ap_iss(K, 2, AP_E8_M1, S16_NKEYS, 0, 0)     < 0) return -1; /* vmseq    */
    if (ap_iss(K, 3, AP_E8_M1, S16_NKEYS, 0, 0)     < 0) return -1; /* vmv zero */

    /* key-block a: build Sb16 (v6) then COPYSA -> Sa16 (v8) */
    if (ap_iss(K, 4, AP_E8_M1,  S16_NKEYS, 1, pa->kt_a) < 0) return -1; /* vle8 Kt_a (V) */
    if (ap_iss(K, 5, AP_E16_M2, S16_NKEYS, 0, 0)        < 0) return -1; /* vmv.v.i v6,0  */
    for (int j = S16_NKEYS - 1; j >= 0; j--)
        if (ap16_qk_body(K, (uint32_t)j) < 0) return -1;
    if (ap_iss(K, 13, AP_E16_M2, S16_NKEYS, 0, 0) < 0) return -1;       /* Sa16 -> v8    */
    if (dbg && ap_iss(K, 70, AP_E16_M2, S16_NKEYS, 0, pa->dbg_sa) < 0) return -1;

    /* key-block b: build Sb16 (v6) */
    if (ap_iss(K, 4, AP_E8_M1,  S16_NKEYS, 1, pa->kt_b) < 0) return -1; /* vle8 Kt_b (V) */
    if (ap_iss(K, 5, AP_E16_M2, S16_NKEYS, 0, 0)        < 0) return -1; /* vmv.v.i v6,0  */
    for (int j = S16_NKEYS - 1; j >= 0; j--)
        if (ap16_qk_body(K, (uint32_t)j) < 0) return -1;
    if (dbg && ap_iss(K, 71, AP_E16_M2, S16_NKEYS, 0, pa->dbg_sb) < 0) return -1;

    /* softmax: 16-bit max/sub, narrow diff -> 8-bit exp */
    if (ap_iss(K, 14, AP_E16_M2, S16_NKEYS, 0, 0)           < 0) return -1; /* vredmaxu Sa */
    if (ap_iss(K, 15, AP_E16_M2, S16_NKEYS, 0, 0)           < 0) return -1; /* vredmaxu m16*/
    if (ap_iss(K, 16, AP_E16_M2, S16_NKEYS, 0, 0)           < 0) return -1; /* vrgather m16*/
    if (ap_iss(K, 17, AP_E8_M1,  S16_NKEYS, 0, pa->exp_lut) < 0) return -1; /* vle8 expLUT */
    if (ap_iss(K, 18, AP_E16_M2, S16_NKEYS, 0, 0)           < 0) return -1; /* d16 = m-Sa  */
    if (ap_iss(K, 19, AP_E8_M1,  S16_NKEYS, 0, 0)           < 0) return -1; /* idx_a >>8   */
    if (ap_iss(K, 20, AP_E8_M1,  S16_NKEYS, 0, AP_C_127)    < 0) return -1; /* min 127     */
    if (ap_iss(K, 21, AP_E8_M1,  S16_NKEYS, 0, 0)           < 0) return -1; /* ea -> v3    */
    if (dbg && ap_iss(K, 72, AP_E8_M1, S16_NKEYS, 0, pa->dbg_ea) < 0) return -1;
    if (ap_iss(K, 22, AP_E16_M2, S16_NKEYS, 0, 0)           < 0) return -1; /* d16 = m-Sb  */
    if (ap_iss(K, 23, AP_E8_M1,  S16_NKEYS, 0, 0)           < 0) return -1; /* idx_b >>8   */
    if (ap_iss(K, 24, AP_E8_M1,  S16_NKEYS, 0, AP_C_127)    < 0) return -1; /* min 127     */
    if (ap_iss(K, 25, AP_E8_M1,  S16_NKEYS, 0, 0)           < 0) return -1; /* eb -> v5    */
    if (dbg && ap_iss(K, 73, AP_E8_M1, S16_NKEYS, 0, pa->dbg_eb) < 0) return -1;

    /* Z */
    if (ap_iss(K, 26, AP_E8_M1,  S16_NKEYS, 0, 0)        < 0) return -1; /* Z16 = sum ea */
    if (ap_iss(K, 27, AP_E8_M1,  S16_NKEYS, 0, 0)        < 0) return -1; /* Z16 += sum eb*/
    if (ap_iss(K, 28, AP_E16_M1, 1, 0, AP_C_1)           < 0) return -1; /* Z32          */
    if (dbg && ap_iss(K, 74, AP_E32_M1, 1, 0, pa->dbg_z) < 0) return -1;

    /* Newton (seed shift 9) */
    if (ap_iss(K, 29, AP_E8_M1,  1, 0, 0)            < 0) return -1; /* idx8 = Z16>>9   */
    if (ap_iss(K, 30, AP_E8_M1, S16_NKEYS, 0, pa->seed_lut) < 0) return -1; /* vle8 seed */
    if (ap_iss(K, 31, AP_E8_M1, S16_NKEYS, 0, 0)     < 0) return -1; /* seed8           */
    if (ap_iss(K, 32, AP_E8_M1,  1, 0, AP_C_2)       < 0) return -1; /* seed16          */
    if (ap_iss(K, 33, AP_E16_M1, 1, 0, AP_C_1)       < 0) return -1; /* R0_32           */
    for (int it = 0; it < 3; it++) {
        uint32_t b = 34 + (uint32_t)it * 4;
        if (ap_iss(K, b + 0, AP_E32_M1, 1, 0, 0)         < 0) return -1;
        if (ap_iss(K, b + 1, AP_E32_M1, 1, 0, AP_C_2P17) < 0) return -1;
        if (ap_iss(K, b + 2, AP_E32_M1, 1, 0, 0)         < 0) return -1;
        if (ap_iss(K, b + 3, AP_E32_M1, 1, 0, 0)         < 0) return -1;
    }
    if (dbg && ap_iss(K, 75, AP_E32_M1, 1, 0, pa->dbg_r) < 0) return -1;

    /* normalize pqa, pqb */
    if (ap_iss(K, 46, AP_E16_M1, 1, 0, 0)            < 0) return -1; /* R16            */
    if (ap_iss(K, 47, AP_E16_M2, S16_NKEYS, 0, 0)    < 0) return -1; /* Rb16 broadcast */
    if (ap_iss(K, 48, AP_E8_M1,  S16_NKEYS, 0, AP_C_1)< 0) return -1; /* e16a          */
    if (ap_iss(K, 49, AP_E16_M2, S16_NKEYS, 0, 0)    < 0) return -1; /* P16a          */
    if (ap_iss(K, 50, AP_E8_M1,  S16_NKEYS, 0, 0)    < 0) return -1; /* pqa -> v3     */
    if (dbg && ap_iss(K, 72, AP_E8_M1, S16_NKEYS, 0, pa->dbg_pqa) < 0) return -1;
    if (ap_iss(K, 51, AP_E8_M1,  S16_NKEYS, 0, AP_C_1)< 0) return -1; /* e16b          */
    if (ap_iss(K, 52, AP_E16_M2, S16_NKEYS, 0, 0)    < 0) return -1; /* P16b          */
    if (ap_iss(K, 53, AP_E8_M1,  S16_NKEYS, 0, 0)    < 0) return -1; /* pqb -> v5     */
    if (dbg && ap_iss(K, 73, AP_E8_M1, S16_NKEYS, 0, pa->dbg_pqb) < 0) return -1;

    /* PV */
    if (ap_iss(K, 54, AP_E8_M1, S16_NKEYS, 1, pa->va) < 0) return -1; /* vle8 Va (V) */
    if (ap_iss(K, 55, AP_E8_M1, S16_NKEYS, 1, pa->vb) < 0) return -1; /* vle8 Vb (V) */
    if (ap_iss(K, 56, AP_E8_M1, S16_NKEYS, 0, 0)      < 0) return -1; /* O accum = 0 */
    for (int d = S16_NFEAT - 1; d >= 0; d--)
        if (ap16_pv_body(K, (uint32_t)d) < 0) return -1;

    /* final O store (vl=64) */
    if (ap_iss(K, 69, AP_E8_M1, S16_NFEAT, 0, pa->out) < 0) return -1;
    return 0;
}
