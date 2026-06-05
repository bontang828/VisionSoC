/*
 * attention_weights.h -- PS-side builders + bit-accurate C reference for the
 * 2D-fabric attention kernel (kernels/attention.S).
 *
 * This header is the SINGLE SOURCE OF TRUTH for the attention fixed-point
 * spec. The board probe (libt1/test/attention_probe.c) stages the buffers
 * built here, issues attention.S over MMIO, and compares the hardware result
 * to attn_reference() below -- which performs the IDENTICAL integer ops, so
 * the comparison is exact (bit-for-bit) on any input.
 *
 * Layout (128 tokens x 128 features, all u8; hw-row = token, element = feat):
 *   Q  [i][d]  = attn_Q(i,d)        row-major i*128+d   (H-loaded)
 *   Kt [d][j]  = K[j][d]            row-major d*128+j   (V-loaded -> H-view = K)
 *   V  [j][v]  = attn_V(j,v)        row-major j*128+v   (V-loaded)
 *   expLUT     = tab[k] replicated  row-major r*128+k   (H-loaded, every row same)
 *   seedLUT    = tab[b] replicated  row-major r*128+b   (u32, every row same)
 *
 * See fyp_doc/attention_kernel_status.md for the full strategy.
 */
#pragma once

#include <stdint.h>

/* ----- dimensions ----- */
#define ATTN_N 128u   /* tokens (hw-rows) */
#define ATTN_D 128u   /* features = keys = value-dims (lanes) */

/* ----- tunable fixed-point knobs (probe check is exact regardless) ----- */
/* Score narrow: folds 1/sqrt(d_k)=1/sqrt(128) plus brings the 128-wide u8.u8
 * dot product (peak ~3.3e5) into u8 range. 3.3e5>>11 = 163 < 256 (no clip). */
#define ATTN_S_SHIFT   11
/* exp LUT decay in Q8: tab[k] = (tab[k-1]*DECAY)>>8, tab[0]=255. 240/256 ~=
 * exp(-1/15.5), i.e. temperature tau ~= 15.5 score-units. Smaller = sharper. */
#define ATTN_EXP_DECAY_Q8  240u
/* Newton reciprocal: R ~= 2^F / Z, F=16. 3 iters give full u16 precision. */
#define ATTN_F            16
#define ATTN_NEWTON_ITERS  3
/* weight normalize / output narrow shifts (both >>8 = /256 = Q0.8 undo). */
#define ATTN_PQ_SHIFT      8
#define ATTN_O_SHIFT       8

/* ----- deterministic, peaked test inputs ----------------------------------
 * Triangular "bump" key/query: token t has a bump centred at feature t, so the
 * score dot(Q_i,K_j) is the bump autocorrelation -- peaked at j=i, decaying
 * with circular |i-j|. The per-query width is varied so Z spreads across tokens
 * (exercises Newton across reciprocal buckets). V is pseudo-random.            */
#define ATTN_WK        40            /* key bump width factor (fixed) */
#define ATTN_WQ_BASE   24            /* query bump width factor, per-token... */
#define ATTN_WQ_SPAN   40            /* ...= BASE + (i % SPAN), range 24..63 */

static inline int attn_circ_dist(int off) {
    int d = off & (int)(ATTN_N - 1u);      /* off mod 128, 0..127 */
    if (d > (int)(ATTN_N / 2u)) d = (int)ATTN_N - d;   /* circular: 0..64 */
    return d;
}
static inline uint8_t attn_bump(int center_off, int w) {
    int v = 255 - w * attn_circ_dist(center_off);
    return (uint8_t)(v < 0 ? 0 : v);
}
static inline int attn_wq(int i) { return ATTN_WQ_BASE + (i % ATTN_WQ_SPAN); }

static inline uint8_t attn_Q(int i, int d) { return attn_bump(d - i, attn_wq(i)); }
static inline uint8_t attn_K(int j, int d) { return attn_bump(d - j, ATTN_WK); }
static inline uint8_t attn_V(int j, int v) {
    uint32_t h = (uint32_t)(j * 131 + v * 17) + 0x9E37u;
    h ^= h >> 13; h *= 0x9E3779B1u; h ^= h >> 15;
    return (uint8_t)h;
}

/* ----- buffer builders (each target must hold >= 128*128 elements) ----- */
static inline void attn_build_Q(uint8_t *q) {
    for (int i = 0; i < (int)ATTN_N; i++)
        for (int d = 0; d < (int)ATTN_D; d++)
            q[i * (int)ATTN_D + d] = attn_Q(i, d);
}
static inline void attn_build_Kt(uint8_t *kt) {       /* Kt[d][j] = K[j][d] */
    for (int d = 0; d < (int)ATTN_D; d++)
        for (int j = 0; j < (int)ATTN_N; j++)
            kt[d * (int)ATTN_N + j] = attn_K(j, d);
}
static inline void attn_build_V(uint8_t *v) {
    for (int j = 0; j < (int)ATTN_N; j++)
        for (int vv = 0; vv < (int)ATTN_D; vv++)
            v[j * (int)ATTN_D + vv] = attn_V(j, vv);
}
/* exp table, replicated into all 128 rows (each hw-row gathers its own copy). */
static inline void attn_exp_table(uint8_t tab[128]) {
    int acc = 255;
    for (int k = 0; k < 128; k++) {
        tab[k] = (uint8_t)acc;
        acc = (acc * (int)ATTN_EXP_DECAY_Q8) >> 8;
    }
}
static inline void attn_build_exp_lut(uint8_t *lut) {
    uint8_t tab[128];
    attn_exp_table(tab);
    for (int r = 0; r < (int)ATTN_N; r++)
        for (int k = 0; k < 128; k++)
            lut[r * 128 + k] = tab[k];
}
/* Newton seed table, stored as u8 so a 128-entry replicated table fits the
 * 128-byte LSU row pitch (a SEW=32 table cannot be loaded -- only 32 e32/row).
 * Stores R0>>1 (clamped to 255); the kernel recovers R0 = seed8*2 in SEW=32.
 * R0 = bucket-centre reciprocal round(2^16/(b*256+128)), range 2..512.        */
static inline void attn_seed_table8(uint8_t tab[128]) {
    for (int b = 0; b < 128; b++) {
        uint32_t r0 = (1u << 16) / ((uint32_t)b * 256u + 128u);  /* 2..512 */
        uint32_t h  = r0 >> 1;                                   /* 1..256 */
        tab[b] = (uint8_t)(h > 255u ? 255u : h);
    }
}
static inline void attn_build_seed_lut(uint8_t *lut) {
    uint8_t tab[128];
    attn_seed_table8(tab);
    for (int r = 0; r < (int)ATTN_N; r++)
        for (int b = 0; b < 128; b++)
            lut[r * 128 + b] = tab[b];
}

/* ----- bit-accurate C reference -------------------------------------------
 * Mirrors attention.S exactly (saturating narrows, truncating multiplies,
 * uint32 Newton). Each *_out buffer is optional (pass NULL to skip).
 * expTab/seedTab are the 128-entry tables (NOT the replicated buffers).        */
static inline uint8_t attn_sat_u8(uint32_t x) { return (uint8_t)(x > 255u ? 255u : x); }

static inline void attn_reference(
    const uint8_t  *Q,        /* [N][D]  Q[i][d]        */
    const uint8_t  *Kt,       /* [D][N]  Kt[d][j]       */
    const uint8_t  *V,        /* [N][D]  V[j][v]        */
    const uint8_t   expTab[128],
    const uint8_t   seedTab[128],   /* R0>>1 table (see attn_seed_table8) */
    uint8_t  *S8_out,         /* [N][N]  optional */
    uint8_t  *e_out,          /* [N][N]  optional */
    uint32_t *Z_out,          /* [N]     optional */
    uint32_t *R_out,          /* [N]     optional */
    uint8_t  *pq_out,         /* [N][N]  optional */
    uint8_t  *O_out)          /* [N][D]  optional */
{
    for (int i = 0; i < (int)ATTN_N; i++) {
        uint8_t  S8[ATTN_N];
        uint8_t  e[ATTN_N];
        uint8_t  pq[ATTN_N];

        /* phase 1: scores S8[i][j] = sat(raw_S >> S_SHIFT) */
        uint8_t m = 0;
        for (int j = 0; j < (int)ATTN_N; j++) {
            uint32_t acc = 0;
            for (int d = 0; d < (int)ATTN_D; d++)
                acc += (uint32_t)Q[i * (int)ATTN_D + d] * (uint32_t)Kt[d * (int)ATTN_N + j];
            uint8_t s = attn_sat_u8(acc >> ATTN_S_SHIFT);
            S8[j] = s;
            if (s > m) m = s;
            if (S8_out) S8_out[i * (int)ATTN_N + j] = s;
        }

        /* phase 2: softmax over j -> e, Z */
        uint32_t Z = 0;
        for (int j = 0; j < (int)ATTN_N; j++) {
            int diff = (int)m - (int)S8[j];      /* >= 0 */
            if (diff > 127) diff = 127;
            uint8_t ev = expTab[diff];
            e[j] = ev;
            Z += ev;
            if (e_out) e_out[i * (int)ATTN_N + j] = ev;
        }
        if (Z_out) Z_out[i] = Z;

        /* phase 2.5: Newton reciprocal R ~= 2^16/Z (uint32, matches SEW=32).
         * Seed: idx=Z>>8, seed8=seedTab[idx], R0=seed8*2 (kernel recovers the
         * dropped LSB the same way). 3 iters, all uint32 to match the datapath. */
        uint32_t b = Z >> 8; if (b > 127u) b = 127u;
        uint32_t R = (uint32_t)seedTab[b] * 2u;
        for (int it = 0; it < ATTN_NEWTON_ITERS; it++)
            R = (R * ((1u << 17) - Z * R)) >> 16;
        if (R_out) R_out[i] = R;

        /* phase 2.6: normalize pq = (e*R)>>8 (truncating narrow) */
        for (int j = 0; j < (int)ATTN_N; j++) {
            uint32_t p = ((uint32_t)e[j] * R) >> ATTN_PQ_SHIFT;
            pq[j] = (uint8_t)p;                  /* low byte = vnsrl narrow */
            if (pq_out) pq_out[i * (int)ATTN_N + j] = pq[j];
        }

        /* phase 3: O[i][v] = sat((sum_j pq*V)>>8) */
        if (O_out) {
            for (int v = 0; v < (int)ATTN_D; v++) {
                uint32_t acc = 0;
                for (int j = 0; j < (int)ATTN_N; j++)
                    acc += (uint32_t)pq[j] * (uint32_t)V[j * (int)ATTN_D + v];
                O_out[i * (int)ATTN_D + v] = attn_sat_u8(acc >> ATTN_O_SHIFT);
            }
        }
    }
}
