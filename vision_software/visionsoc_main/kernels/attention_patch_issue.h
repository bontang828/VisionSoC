/*
 * attention_patch_issue.h -- issue sequencer for attention_patch.S (blocked ViT
 * 8x8-patch attention). One call processes ONE query-block over both key/value-
 * blocks; main.c / the probe calls it twice (Qa, Qb). libt1 supplies
 * vtype/vl/vertical_mode/rs1 per issue.
 *
 * INVARIANT: vmode=1 only for the SEW=8 vle8 V-loads (Kt/V) and SEW=8
 * vrgather.vx broadcasts. Feature-dimension QK ops run vl=64; everything on the
 * key dimension runs vl=128. See attention_patch.S region map.
 */
#pragma once

#include "libt1.h"
#include <stdint.h>

#define AP_E8_M1   0xC0u
#define AP_E16_M1  0xC8u
#define AP_E16_M2  0xC9u
#define AP_E32_M1  0xD0u

#define AP_C_255  255u
#define AP_C_127  127u
#define AP_C_1      1u
#define AP_C_2      2u
#define AP_C_2P17 131072u

#define AP_NKEYS   128   /* keys per block (= hw-rows) */
#define AP_NFEAT    64   /* features per token (8x8)   */

struct ap_pa {
    uint32_t q;          /* Q token tile [128][128] (lanes 0..63 = feats)  */
    uint32_t kt_a, kt_b; /* Kt blocks [feat][key] (V-loaded)               */
    uint32_t va, vb;     /* V blocks  [key][feat] (V-loaded)               */
    uint32_t exp_lut;    /* replicated expLUT  [128][128] u8               */
    uint32_t seed_lut;   /* replicated seedLUT [128][128] u8 (R0>>1)       */
    uint32_t out;        /* O tile [128][128] (lanes 0..63 = feats)        */
    /* checkpoint targets (read only when dbg != 0) */
    uint32_t dbg_sa, dbg_sb, dbg_ea, dbg_eb, dbg_z, dbg_r, dbg_pqa, dbg_pqb;
};

static inline int ap_iss(const uint32_t *K, uint32_t idx, uint32_t vtype,
                         uint32_t vl, uint8_t vmode, uint32_t rs1)
{
    struct t1_op op = {
        .instruction = K[idx], .rs1 = rs1,
        .vtype = vtype, .vl = vl, .vertical_mode = vmode,
    };
    return t1_issue(&op);
}

/* QK body (words 6..14), gather index k=key j. The vrgather.vx broadcast is a
 * VERTICAL op, where vl counts HW-ROWS -> must be 128 (all query tokens); only
 * the horizontal feature ops (vwmulu/vwredsumu) use vl=64 (64 patch features). */
static inline int ap_qk_body(const uint32_t *K, uint32_t k)
{
    if (ap_iss(K, 6,  AP_E8_M1,  AP_NKEYS, 1, k)        < 0) return -1; /* vrgather.vx K[j] (V: vl=hw-rows=128) */
    if (ap_iss(K, 7,  AP_E8_M1,  AP_NFEAT, 0, 0)        < 0) return -1; /* vwmulu.vv  (H: vl=feat=64)           */
    if (ap_iss(K, 8,  AP_E16_M2, AP_NFEAT, 0, 0)        < 0) return -1; /* vwredsumu.vs (H: vl=feat=64)         */
    if (ap_iss(K, 9,  AP_E16_M1, 1, 0, 0)               < 0) return -1; /* vnsrl >>S_SHIFT  */
    if (ap_iss(K, 10, AP_E16_M1, 1, 0, AP_C_255)        < 0) return -1; /* vminu 255        */
    if (ap_iss(K, 11, AP_E8_M1,  1, 0, 0)               < 0) return -1; /* vnsrl -> u8      */
    if (ap_iss(K, 12, AP_E8_M1,  AP_NKEYS, 0, 0)        < 0) return -1; /* vslideup         */
    if (ap_iss(K, 13, AP_E8_M1,  AP_NKEYS, 0, 0)        < 0) return -1; /* vmerge           */
    if (ap_iss(K, 14, AP_E8_M1,  AP_NKEYS, 0, 0)        < 0) return -1; /* vmv.v.v          */
    return 0;
}

/* PV body (words 57..68), gather index d=value feature; reduce over 128 keys. */
static inline int ap_pv_body(const uint32_t *K, uint32_t d)
{
    if (ap_iss(K, 57, AP_E8_M1,  AP_NKEYS, 1, d)        < 0) return -1; /* vrgather Va[:,d]  */
    if (ap_iss(K, 58, AP_E8_M1,  AP_NKEYS, 0, 0)        < 0) return -1; /* vwmulu pqa*Va     */
    if (ap_iss(K, 59, AP_E16_M2, AP_NKEYS, 0, 0)        < 0) return -1; /* vwredsumu accA    */
    if (ap_iss(K, 60, AP_E8_M1,  AP_NKEYS, 1, d)        < 0) return -1; /* vrgather Vb[:,d]  */
    if (ap_iss(K, 61, AP_E8_M1,  AP_NKEYS, 0, 0)        < 0) return -1; /* vwmulu pqb*Vb     */
    if (ap_iss(K, 62, AP_E16_M2, AP_NKEYS, 0, 0)        < 0) return -1; /* vwredsumu chain   */
    if (ap_iss(K, 63, AP_E16_M1, 1, 0, 0)               < 0) return -1; /* vnsrl >>O_SHIFT   */
    if (ap_iss(K, 64, AP_E16_M1, 1, 0, AP_C_255)        < 0) return -1; /* vminu 255         */
    if (ap_iss(K, 65, AP_E8_M1,  1, 0, 0)               < 0) return -1; /* vnsrl -> u8       */
    if (ap_iss(K, 66, AP_E8_M1,  AP_NFEAT, 0, 0)        < 0) return -1; /* vslideup (vl=64)  */
    if (ap_iss(K, 67, AP_E8_M1,  AP_NFEAT, 0, 0)        < 0) return -1; /* vmerge            */
    if (ap_iss(K, 68, AP_E8_M1,  AP_NFEAT, 0, 0)        < 0) return -1; /* vmv.v.v           */
    return 0;
}

/* Issue one query-block: Q=pa->q over key/value-blocks (a,b) -> pa->out. */
static inline int attention_patch_issue(const uint32_t *K, const struct ap_pa *pa, int dbg)
{
    /* PRE */
    if (ap_iss(K, 0, AP_E8_M1, AP_NKEYS, 0, pa->q) < 0) return -1; /* vle8 Q   */
    if (ap_iss(K, 1, AP_E8_M1, AP_NKEYS, 0, 0)     < 0) return -1; /* vid      */
    if (ap_iss(K, 2, AP_E8_M1, AP_NKEYS, 0, 0)     < 0) return -1; /* vmseq    */
    if (ap_iss(K, 3, AP_E8_M1, AP_NKEYS, 0, 0)     < 0) return -1; /* vmv zero */

    /* ---- key-block a: build Sa in v6, stash to v3 ---- */
    if (ap_iss(K, 4, AP_E8_M1, AP_NKEYS, 1, pa->kt_a) < 0) return -1; /* vle8 Kt_a (V) */
    if (ap_iss(K, 5, AP_E8_M1, AP_NKEYS, 0, 0)        < 0) return -1; /* vmv v6,0      */
    for (int j = AP_NKEYS - 1; j >= 0; j--)
        if (ap_qk_body(K, (uint32_t)j) < 0) return -1;
    if (ap_iss(K, 15, AP_E8_M1, AP_NKEYS, 0, 0) < 0) return -1;       /* Sa -> v3 */
    if (dbg && ap_iss(K, 70, AP_E8_M1, AP_NKEYS, 0, pa->dbg_sa) < 0) return -1;

    /* ---- key-block b: build Sb in v6 ---- */
    if (ap_iss(K, 4, AP_E8_M1, AP_NKEYS, 1, pa->kt_b) < 0) return -1; /* vle8 Kt_b (V) */
    if (ap_iss(K, 5, AP_E8_M1, AP_NKEYS, 0, 0)        < 0) return -1; /* vmv v6,0      */
    for (int j = AP_NKEYS - 1; j >= 0; j--)
        if (ap_qk_body(K, (uint32_t)j) < 0) return -1;
    if (dbg && ap_iss(K, 71, AP_E8_M1, AP_NKEYS, 0, pa->dbg_sb) < 0) return -1;

    /* ---- softmax over 256 keys (Sa=v3, Sb=v6) ---- */
    if (ap_iss(K, 16, AP_E8_M1, AP_NKEYS, 0, 0)           < 0) return -1; /* vredmaxu Sa  */
    if (ap_iss(K, 17, AP_E8_M1, AP_NKEYS, 0, 0)           < 0) return -1; /* vredmaxu both*/
    if (ap_iss(K, 18, AP_E8_M1, AP_NKEYS, 0, 0)           < 0) return -1; /* vrgather m   */
    if (ap_iss(K, 19, AP_E8_M1, AP_NKEYS, 0, pa->exp_lut) < 0) return -1; /* vle8 expLUT  */
    if (ap_iss(K, 20, AP_E8_M1, AP_NKEYS, 0, 0)           < 0) return -1; /* m - Sa       */
    if (ap_iss(K, 21, AP_E8_M1, AP_NKEYS, 0, AP_C_127)    < 0) return -1; /* min 127      */
    if (ap_iss(K, 22, AP_E8_M1, AP_NKEYS, 0, 0)           < 0) return -1; /* ea -> v3     */
    if (dbg && ap_iss(K, 70, AP_E8_M1, AP_NKEYS, 0, pa->dbg_ea) < 0) return -1;
    if (ap_iss(K, 23, AP_E8_M1, AP_NKEYS, 0, 0)           < 0) return -1; /* m - Sb       */
    if (ap_iss(K, 24, AP_E8_M1, AP_NKEYS, 0, AP_C_127)    < 0) return -1; /* min 127      */
    if (ap_iss(K, 25, AP_E8_M1, AP_NKEYS, 0, 0)           < 0) return -1; /* eb -> v6     */
    if (dbg && ap_iss(K, 71, AP_E8_M1, AP_NKEYS, 0, pa->dbg_eb) < 0) return -1;
    if (ap_iss(K, 26, AP_E8_M1, AP_NKEYS, 0, 0)           < 0) return -1; /* Z16 = sum ea */
    if (ap_iss(K, 27, AP_E8_M1, AP_NKEYS, 0, 0)           < 0) return -1; /* Z16 += sum eb*/
    if (ap_iss(K, 28, AP_E16_M1, 1, 0, AP_C_1)            < 0) return -1; /* Z32          */
    if (dbg && ap_iss(K, 72, AP_E32_M1, 1, 0, pa->dbg_z) < 0) return -1;

    /* ---- Newton R ~= 2^16/Z (seed shift 9) ---- */
    if (ap_iss(K, 29, AP_E8_M1,  1, 0, 0)            < 0) return -1; /* idx = Z16>>9     */
    if (ap_iss(K, 30, AP_E8_M1, AP_NKEYS, 0, pa->seed_lut) < 0) return -1; /* vle8 seed   */
    if (ap_iss(K, 31, AP_E8_M1, AP_NKEYS, 0, 0)      < 0) return -1; /* seed8            */
    if (ap_iss(K, 32, AP_E8_M1,  1, 0, AP_C_2)       < 0) return -1; /* seed16 = seed*2  */
    if (ap_iss(K, 33, AP_E16_M1, 1, 0, AP_C_1)       < 0) return -1; /* R0_32            */
    for (int it = 0; it < 3; it++) {
        uint32_t b = 34 + (uint32_t)it * 4;
        if (ap_iss(K, b + 0, AP_E32_M1, 1, 0, 0)         < 0) return -1; /* t = Z*R   */
        if (ap_iss(K, b + 1, AP_E32_M1, 1, 0, AP_C_2P17) < 0) return -1; /* 2^17 - t  */
        if (ap_iss(K, b + 2, AP_E32_M1, 1, 0, 0)         < 0) return -1; /* R = R*t   */
        if (ap_iss(K, b + 3, AP_E32_M1, 1, 0, 0)         < 0) return -1; /* R >>= 16  */
    }
    if (dbg && ap_iss(K, 73, AP_E32_M1, 1, 0, pa->dbg_r) < 0) return -1;

    /* ---- normalize pqa, pqb = (e*R)>>8 ---- */
    if (ap_iss(K, 46, AP_E16_M1, 1, 0, 0)            < 0) return -1; /* R16             */
    if (ap_iss(K, 47, AP_E16_M2, AP_NKEYS, 0, 0)     < 0) return -1; /* Rb16 broadcast  */
    if (ap_iss(K, 48, AP_E8_M1,  AP_NKEYS, 0, AP_C_1)< 0) return -1; /* e16a            */
    if (ap_iss(K, 49, AP_E16_M2, AP_NKEYS, 0, 0)     < 0) return -1; /* P16a            */
    if (ap_iss(K, 50, AP_E8_M1,  AP_NKEYS, 0, 0)     < 0) return -1; /* pqa -> v3       */
    if (dbg && ap_iss(K, 70, AP_E8_M1, AP_NKEYS, 0, pa->dbg_pqa) < 0) return -1;
    if (ap_iss(K, 51, AP_E8_M1,  AP_NKEYS, 0, AP_C_1)< 0) return -1; /* e16b            */
    if (ap_iss(K, 52, AP_E16_M2, AP_NKEYS, 0, 0)     < 0) return -1; /* P16b            */
    if (ap_iss(K, 53, AP_E8_M1,  AP_NKEYS, 0, 0)     < 0) return -1; /* pqb -> v6       */
    if (dbg && ap_iss(K, 71, AP_E8_M1, AP_NKEYS, 0, pa->dbg_pqb) < 0) return -1;

    /* ---- PV over both value-blocks ---- */
    if (ap_iss(K, 54, AP_E8_M1, AP_NKEYS, 1, pa->va) < 0) return -1; /* vle8 Va (V) */
    if (ap_iss(K, 55, AP_E8_M1, AP_NKEYS, 1, pa->vb) < 0) return -1; /* vle8 Vb (V) */
    if (ap_iss(K, 56, AP_E8_M1, AP_NKEYS, 0, 0)      < 0) return -1; /* O accum = 0 */
    for (int d = AP_NFEAT - 1; d >= 0; d--)
        if (ap_pv_body(K, (uint32_t)d) < 0) return -1;

    /* ---- final O store (vl=64) ---- */
    if (ap_iss(K, 69, AP_E8_M1, AP_NFEAT, 0, pa->out) < 0) return -1;
    return 0;
}
