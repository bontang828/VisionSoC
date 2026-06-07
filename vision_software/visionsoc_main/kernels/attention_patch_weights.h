/*
 * attention_patch_weights.h -- PS-side builders + bit-accurate C reference for
 * the on-fabric 8x8-patch ViT attention kernel.
 *
 * SINGLE SOURCE OF TRUTH for the patch attention fixed-point spec and the
 * canonical patchify map. A 128x128 frame becomes 16x16 = 256 patch-tokens of
 * 64 features (the 8x8 pixels); full 256x256 self-attention against fixed
 * pseudo-random K/V (a content-addressed transform of the scene).
 *
 * The on-fabric kernel (kernels/attention_patch*.S) computes the IDENTICAL
 * integer ops as ap_reference() below, so the board probe's comparison is exact
 * (bit-for-bit). The on-fabric tokenisation must reproduce ap_patchify()'s
 * layout; ap_unpatchify() is its inverse for display.
 *
 * Canonical token / feature order (ViT, row-major):
 *   token t  = Py*16 + Px          (Py = t/16, Px = t%16; Py,Px in 0..15)
 *   feat  f  = ry*8  + rx          (ry = f/8, rx = f%8;  ry,rx in 0..7)
 *   tokens[t][f] = frame[(Py*8+ry)*128 + (Px*8+rx)]
 * Token-block split: block A = tokens 0..127 (Py 0..7, top half of frame),
 *                    block B = tokens 128..255 (Py 8..15, bottom half).
 *
 * NOTE: token order is permutation-free for attention. If the on-fabric
 * rearrange is cheaper in a different order, change ap_patchify/ap_unpatchify
 * AND the K/V indexing together and the reference stays exact.
 *
 * See fyp_doc/attention_kernel_status.md and the row-kernel attention_weights.h.
 */
#pragma once

#include <stdint.h>

/* ----- dimensions ----- */
#define AP_IMG     128u   /* frame side (pixels)                 */
#define AP_PSZ       8u   /* patch side (pixels)                 */
#define AP_PGRID    16u   /* patches per side (AP_IMG/AP_PSZ)    */
#define AP_TOKENS  256u   /* AP_PGRID * AP_PGRID                 */
#define AP_FEAT     64u   /* AP_PSZ * AP_PSZ                     */
#define AP_BLK     128u   /* tokens per block (= hw-rows)        */
#define AP_NBLK      2u   /* AP_TOKENS / AP_BLK                  */
#define AP_LANES   128u   /* tile lane width (feat 0..63 + zero pad) */

/* ----- tunable fixed-point knobs (probe check is exact regardless) ----- */
/* score narrow: 64-feature u8.u8 dot >>S_SHIFT into u8, folds 1/sqrt(d). Tuned
 * (host sweep): S_SHIFT=13 -> 0% saturation, Z in [422,1013], R in [13,150]
 * (well-conditioned, no degenerate R=0), attention genuinely peaked.           */
#define AP_S_SHIFT        13
/* exp LUT decay in Q8: tab[k] = (tab[k-1]*DECAY)>>8, tab[0]=255. 200 -> tau~5.7
 * (sharper than the row kernel's 240) keeps Z bounded under the wider key set. */
#define AP_EXP_DECAY_Q8  200u
/* Newton reciprocal R ~= 2^F / Z, F=16, 3 iters. F=16 never overflows u32 over
 * the full Z range (256 keys -> Z in [255, 65280]; R*(2^17-Z*R) <= ~1.7e7).   */
#define AP_F              16
#define AP_NEWTON_ITERS    3
/* seed bucket: Z up to 256*255=65280, so bucket = min(Z>>9,127) (was >>8 for
 * 128 keys); seed table centres at b*512+256. ONLY Newton change vs row kernel.*/
#define AP_SEED_SHIFT      9
/* weight-normalise / output narrow shifts (both >>8 = undo Q0.8). */
#define AP_PQ_SHIFT        8
#define AP_O_SHIFT         8

/* ----- deterministic test frame + fixed K/V -------------------------------
 * The probe needs a synthetic 128x128 frame (the "camera" input) whose patches
 * give a non-trivial, peaked score distribution. Q = patchify(frame). K and V
 * are fixed pseudo-random per (token,feat) -- a learned-style dictionary.       */

/* a deterministic 128x128 frame: smooth 2D structure so neighbouring patches
 * resemble each other (peaked attention) yet every patch differs.              */
static inline uint8_t ap_frame_pixel(int r, int c) {
    /* low-frequency bumps + a per-patch offset so patches are distinguishable */
    int Py = r / 8, Px = c / 8;
    int ry = r % 8, rx = c % 8;
    int base = 64 + ((Py * 9 + Px * 5) & 63);            /* per-patch DC 64..127 */
    int wave = ((r * 3) ^ (c * 7)) & 31;                 /* fine texture 0..31   */
    int rad  = (ry - 4) * (ry - 4) + (rx - 4) * (rx - 4);/* radial bump 0..32    */
    int v = base + wave + (32 - rad);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}
static inline void ap_build_frame(uint8_t *frame /* [128*128] */) {
    for (int r = 0; r < (int)AP_IMG; r++)
        for (int c = 0; c < (int)AP_IMG; c++)
            frame[r * (int)AP_IMG + c] = ap_frame_pixel(r, c);
}

/* fixed pseudo-random K/V: key/value token j, feature f (f in 0..63). All
 * arithmetic unsigned (defined wraparound; no signed-overflow UB).             */
static inline uint8_t ap_K(int j, int f) {
    uint32_t h = ((uint32_t)j * 73856093u ^ (uint32_t)f * 19349663u) + 0x9E37u;
    h ^= h >> 13; h *= 0x85EBCA6Bu; h ^= h >> 16;
    return (uint8_t)h;
}
static inline uint8_t ap_V(int j, int f) {
    uint32_t h = ((uint32_t)j * 83492791u ^ (uint32_t)f * 49979687u) + 0x2545u;
    h ^= h >> 15; h *= 0xC2B2AE35u; h ^= h >> 13;
    return (uint8_t)h;
}

/* ----- patchify / un-patchify (the canonical map the on-fabric must match) - */
/* frame[128*128] -> tokens[256][64], row-major token tile (t*64 + f). */
static inline void ap_patchify(const uint8_t *frame, uint8_t *tokens) {
    for (int t = 0; t < (int)AP_TOKENS; t++) {
        int Py = t / (int)AP_PGRID, Px = t % (int)AP_PGRID;
        for (int f = 0; f < (int)AP_FEAT; f++) {
            int ry = f / (int)AP_PSZ, rx = f % (int)AP_PSZ;
            int r = Py * (int)AP_PSZ + ry, c = Px * (int)AP_PSZ + rx;
            tokens[t * (int)AP_FEAT + f] = frame[r * (int)AP_IMG + c];
        }
    }
}
/* tokens[256][64] -> frame[128*128] (inverse, for display of O). */
static inline void ap_unpatchify(const uint8_t *tokens, uint8_t *frame) {
    for (int t = 0; t < (int)AP_TOKENS; t++) {
        int Py = t / (int)AP_PGRID, Px = t % (int)AP_PGRID;
        for (int f = 0; f < (int)AP_FEAT; f++) {
            int ry = f / (int)AP_PSZ, rx = f % (int)AP_PSZ;
            int r = Py * (int)AP_PSZ + ry, c = Px * (int)AP_PSZ + rx;
            frame[r * (int)AP_IMG + c] = tokens[t * (int)AP_FEAT + f];
        }
    }
}

/* ----- tile builders for the fabric (128x128 byte tiles, feat 64..127 = 0) -
 * Q tile (per block): hw-row = token, lane = feat (H-loaded).
 * Kt block:           V-loaded so H-view = K block (hw-row=key, lane=feat);
 *                     stored Kt[feat][key] row-major, feat-rows 64..127 zero.
 * V block:            V-loaded so H-view = V^T (hw-row=feat-col=output dim);
 *                     stored V[key][feat] row-major, feat-cols 64..127 zero.    */
static inline void ap_build_Q_block(const uint8_t *frame, int blk, uint8_t *q /*[128*128]*/) {
    /* q[hwrow=token_in_block][lane] = patch feature; lanes 64..127 = 0 */
    for (int tb = 0; tb < (int)AP_BLK; tb++) {
        int t = blk * (int)AP_BLK + tb;
        int Py = t / (int)AP_PGRID, Px = t % (int)AP_PGRID;
        for (int lane = 0; lane < (int)AP_LANES; lane++) {
            uint8_t v = 0;
            if (lane < (int)AP_FEAT) {
                int ry = lane / (int)AP_PSZ, rx = lane % (int)AP_PSZ;
                int r = Py * (int)AP_PSZ + ry, c = Px * (int)AP_PSZ + rx;
                v = frame[r * (int)AP_IMG + c];
            }
            q[tb * (int)AP_LANES + lane] = v;
        }
    }
}
static inline void ap_build_Kt_block(int blk, uint8_t *kt /*[128*128]*/) {
    /* kt[feat][key] = K[blk*128+key][feat]; feat-rows 64..127 = 0 */
    for (int d = 0; d < (int)AP_LANES; d++)
        for (int k = 0; k < (int)AP_BLK; k++)
            kt[d * (int)AP_BLK + k] =
                (d < (int)AP_FEAT) ? ap_K(blk * (int)AP_BLK + k, d) : 0;
}
static inline void ap_build_V_block(int blk, uint8_t *v /*[128*128]*/) {
    /* v[key][feat] = V[blk*128+key][feat]; feat-cols 64..127 = 0 */
    for (int k = 0; k < (int)AP_BLK; k++)
        for (int f = 0; f < (int)AP_LANES; f++)
            v[k * (int)AP_LANES + f] =
                (f < (int)AP_FEAT) ? ap_V(blk * (int)AP_BLK + k, f) : 0;
}

/* ----- on-fabric patchify: 3-pass gather (H -> V -> H) index tiles ----------
 * The image->token reshape swaps the ry<->Px index groups. On the fabric the
 * LSU row pitch is hardwired to 128, so this needs cross-hw-row movement. It is
 * realised as three vrgather passes, using the per-element vertical vrgather.vv
 * (each dest element pulls from an arbitrary source hw-row, lane preserved):
 *
 *   source S[P][Q]   = image[blockbase + P][Q]   (P=Py*8+ry in 0..63, Q=Px*8+rx)
 *   H1: M1[P][L1] = S[P][idxH1[P][L1]]           (horizontal, per-row lane gather)
 *   V : M2[t][L1] = M1[idxV[t][L1]][L1]          (vertical, per-element row gather)
 *   H2: T[t][f]   = M2[t][idxH2[t][f]]           (horizontal)
 * with intermediate lane L1 = rx*16 + ((Px+ry) mod 16).  Result T[t][f] equals
 * ap_build_Q_block (token t = Py*16+Px, feat f = ry*8+rx). The SAME three tiles
 * serve both token-blocks; only the source region (image rows 0..63 vs 64..127)
 * differs. Index values are bytes (rows/lanes 0..127). Replicated checkpoints:
 * idxH1/idxH2 are indexed by hw-row (P or t) and dest lane; idxV by (t, L1).
 *
 * Verify in pure C with ap_im2col_check() below BEFORE touching the board.       */
static inline void ap_build_im2col_idxH1(uint8_t *idx /*[128*128]*/) {
    /* idxH1[P][L1] = source lane Q;  ry=P%8, rx=L1/16, Px=(L1%16 - ry) mod 16 */
    for (int P = 0; P < 128; P++) {
        int ry = P & 7;
        for (int L1 = 0; L1 < 128; L1++) {
            int rx = L1 >> 4;
            int Px = ((L1 & 15) - ry + 16) & 15;
            idx[P * 128 + L1] = (uint8_t)(Px * 8 + rx);
        }
    }
}
static inline void ap_build_im2col_idxV(uint8_t *idx /*[128*128]*/) {
    /* idxV[t][L1] = source row P = Py*8+ry; Py=t/16, ry=(L1%16 - t%16) mod 16 */
    for (int t = 0; t < 128; t++) {
        int Py = t >> 4, Px = t & 15;
        for (int L1 = 0; L1 < 128; L1++) {
            int ry = ((L1 & 15) - Px + 16) & 15;
            int P = (ry < 8) ? (Py * 8 + ry) : 0;   /* unused L1 -> harmless 0 */
            idx[t * 128 + L1] = (uint8_t)P;
        }
    }
}
static inline void ap_build_im2col_idxH2(uint8_t *idx /*[128*128]*/) {
    /* idxH2[t][f] = intermediate lane L1 = (f%8)*16 + ((t%16 + f/8) mod 16) */
    for (int t = 0; t < 128; t++) {
        int Px = t & 15;
        for (int f = 0; f < 128; f++) {
            int rx = f & 7, ry = f >> 3;             /* f<64 meaningful; pad 0 */
            int L1 = (f < (int)AP_FEAT) ? (rx * 16 + ((Px + ry) & 15)) : 0;
            idx[t * 128 + f] = (uint8_t)L1;
        }
    }
}

/* Host simulation of the 3 passes on one block's 64x128 source region (rows
 * 0..63 = image rows blockbase..blockbase+63). Writes the 128x64 token tile
 * (zero-padded to 128 lanes). Returns 0 if it matches ap_build_Q_block.        */
static inline void ap_im2col_simulate(const uint8_t *frame, int blk, uint8_t *tile /*[128*128]*/) {
    static uint8_t S[128][128], M1[128][128], M2[128][128];
    static uint8_t idxH1[128 * 128], idxV[128 * 128], idxH2[128 * 128];
    ap_build_im2col_idxH1(idxH1);
    ap_build_im2col_idxV(idxV);
    ap_build_im2col_idxH2(idxH2);
    int base = blk * 64;                              /* image row offset */
    for (int P = 0; P < 128; P++)
        for (int Q = 0; Q < 128; Q++)
            S[P][Q] = (P < 64) ? frame[(base + P) * (int)AP_IMG + Q] : 0;
    for (int P = 0; P < 128; P++)
        for (int L1 = 0; L1 < 128; L1++)
            M1[P][L1] = S[P][idxH1[P * 128 + L1]];
    for (int t = 0; t < 128; t++)
        for (int L1 = 0; L1 < 128; L1++)
            M2[t][L1] = M1[idxV[t * 128 + L1]][L1];
    for (int t = 0; t < 128; t++)
        for (int f = 0; f < 128; f++)
            tile[t * 128 + f] = (f < (int)AP_FEAT) ? M2[t][idxH2[t * 128 + f]] : 0;
}

/* ----- exp / seed LUTs (replicated to all 128 hw-rows) ----- */
static inline void ap_exp_table(uint8_t tab[128]) {
    int acc = 255;
    for (int k = 0; k < 128; k++) { tab[k] = (uint8_t)acc; acc = (acc * (int)AP_EXP_DECAY_Q8) >> 8; }
}
static inline void ap_build_exp_lut(uint8_t *lut /*[128*128]*/) {
    uint8_t tab[128]; ap_exp_table(tab);
    for (int r = 0; r < 128; r++) for (int k = 0; k < 128; k++) lut[r * 128 + k] = tab[k];
}
/* seed8[b] = R0>>1 (kernel recovers R0 = seed8*2), R0 = round(2^16 / bucket-centre),
 * bucket centre = b*512 + 256 (matches AP_SEED_SHIFT = 9).                       */
static inline void ap_seed_table8(uint8_t tab[128]) {
    for (int b = 0; b < 128; b++) {
        uint32_t r0 = (1u << 16) / ((uint32_t)b * 512u + 256u);  /* 1..256 */
        uint32_t h  = r0 >> 1;
        tab[b] = (uint8_t)(h > 255u ? 255u : h);
    }
}
static inline void ap_build_seed_lut(uint8_t *lut /*[128*128]*/) {
    uint8_t tab[128]; ap_seed_table8(tab);
    for (int r = 0; r < 128; r++) for (int b = 0; b < 128; b++) lut[r * 128 + b] = tab[b];
}

/* ----- bit-accurate C reference: full 256-token blocked-softmax attention ----
 * Mirrors attention_patch.S exactly (saturating narrows, truncating multiplies,
 * uint32 Newton with seed shift 9). Each *_out is optional (NULL to skip).
 * Outputs are per-token (256) where noted. expTab/seedTab are the 128-entry
 * tables (NOT the replicated buffers). Q = patchify(frame).                     */
static inline uint8_t ap_sat_u8(uint32_t x) { return (uint8_t)(x > 255u ? 255u : x); }

static inline void ap_reference(
    const uint8_t *frame,        /* [128*128] camera/synthetic frame */
    const uint8_t  expTab[128],
    const uint8_t  seedTab[128], /* R0>>1 table (ap_seed_table8) */
    uint8_t  *S8_out,            /* [256][256] optional */
    uint8_t  *e_out,             /* [256][256] optional */
    uint32_t *Z_out,             /* [256]      optional */
    uint32_t *R_out,             /* [256]      optional */
    uint8_t  *pq_out,            /* [256][256] optional */
    uint8_t  *O_out)             /* [256][64]  optional (token tile, t*64+f) */
{
    static uint8_t Qtok[AP_TOKENS][AP_FEAT];
    ap_patchify(frame, &Qtok[0][0]);

    for (int i = 0; i < (int)AP_TOKENS; i++) {
        uint8_t  S8[AP_TOKENS];
        uint8_t  e[AP_TOKENS];
        uint8_t  pq[AP_TOKENS];

        /* phase 1: scores S8[i][j] over all 256 keys (64-feature dot) */
        uint8_t m = 0;
        for (int j = 0; j < (int)AP_TOKENS; j++) {
            uint32_t acc = 0;
            for (int f = 0; f < (int)AP_FEAT; f++)
                acc += (uint32_t)Qtok[i][f] * (uint32_t)ap_K(j, f);
            uint8_t s = ap_sat_u8(acc >> AP_S_SHIFT);
            S8[j] = s;
            if (s > m) m = s;
            if (S8_out) S8_out[i * (int)AP_TOKENS + j] = s;
        }

        /* phase 2: softmax over j -> e, Z */
        uint32_t Z = 0;
        for (int j = 0; j < (int)AP_TOKENS; j++) {
            int diff = (int)m - (int)S8[j];
            if (diff > 127) diff = 127;
            uint8_t ev = expTab[diff];
            e[j] = ev; Z += ev;
            if (e_out) e_out[i * (int)AP_TOKENS + j] = ev;
        }
        if (Z_out) Z_out[i] = Z;

        /* phase 2.5: Newton R ~= 2^16 / Z (uint32, seed shift 9) */
        uint32_t b = Z >> AP_SEED_SHIFT; if (b > 127u) b = 127u;
        uint32_t R = (uint32_t)seedTab[b] * 2u;
        for (int it = 0; it < AP_NEWTON_ITERS; it++)
            R = (R * ((1u << 17) - Z * R)) >> 16;
        if (R_out) R_out[i] = R;

        /* phase 2.6: pq = (e*R)>>8 */
        for (int j = 0; j < (int)AP_TOKENS; j++) {
            uint32_t p = ((uint32_t)e[j] * R) >> AP_PQ_SHIFT;
            pq[j] = (uint8_t)p;
            if (pq_out) pq_out[i * (int)AP_TOKENS + j] = pq[j];
        }

        /* phase 3: O[i][f] = sat((sum_j pq*V)>>8), f in 0..63 */
        if (O_out) {
            for (int f = 0; f < (int)AP_FEAT; f++) {
                uint32_t acc = 0;
                for (int j = 0; j < (int)AP_TOKENS; j++)
                    acc += (uint32_t)pq[j] * (uint32_t)ap_V(j, f);
                O_out[i * (int)AP_FEAT + f] = ap_sat_u8(acc >> AP_O_SHIFT);
            }
        }
    }
}

/* ===== SELF-ATTENTION variant: K = V = the live patch tokens (not a dictionary) =====
 * Here Q = K = V = patchify(frame), so each output patch is a similarity-weighted blend of
 * ALL the frame's own patches -> true global context (move one patch, every patch that
 * attends to it shifts). Self scores are Q.Q; the diagonal Q_i.Q_i = patch energy <=
 * 64*255^2 = 4.16e6, and by Cauchy-Schwarz every off-diagonal <= that too, so
 * AP_SELF_S_SHIFT=14 (>>14 of 4.16e6 = 254) GUARANTEES no saturation on ANY frame. Sharper
 * decay (180) for peakedness (host-swept; the smooth synthetic frame stays flat, real
 * textured frames are sharper). seedLUT is shared (decay-independent); only expLUT differs. */
#define AP_SELF_S_SHIFT       14
/* decay 110 (sharper than the cross kernel) -> ~2x more peaked self-attention so
 * ordinary scene content (not just a very close object) breaks the global-average
 * collapse; host-swept, Newton stays conditioned (R in [13,48], no R<2). Lower =
 * sharper (toward identity/blocky copy); raise toward 180 = flatter (toward the
 * uniform global-average field). */
#define AP_SELF_EXP_DECAY_Q8 110u

static inline void ap_self_exp_table(uint8_t tab[128]) {
    int acc = 255;
    for (int k = 0; k < 128; k++) { tab[k] = (uint8_t)acc; acc = (acc * (int)AP_SELF_EXP_DECAY_Q8) >> 8; }
}
static inline void ap_build_self_exp_lut(uint8_t *lut /*[128*128]*/) {
    uint8_t tab[128]; ap_self_exp_table(tab);
    for (int r = 0; r < 128; r++) for (int k = 0; k < 128; k++) lut[r * 128 + k] = tab[k];
}

/* Transpose a 128x128 tile on the PS (for probe-side expected Kt = Q^T). */
static inline void ap_transpose_tile(const uint8_t *src, uint8_t *dst) {
    for (int r = 0; r < 128; r++)
        for (int c = 0; c < 128; c++)
            dst[c * 128 + r] = src[r * 128 + c];
}

/* bit-accurate self-attention reference: K = V = patchify(frame). */
static inline void ap_self_reference(const uint8_t *frame, const uint8_t expTab[128],
                                     const uint8_t seedTab[128], uint8_t *O_out /*[256][64]*/) {
    static uint8_t Q[AP_TOKENS][AP_FEAT];
    ap_patchify(frame, &Q[0][0]);
    for (int i = 0; i < (int)AP_TOKENS; i++) {
        uint8_t S8[AP_TOKENS], e[AP_TOKENS], pq[AP_TOKENS];
        uint8_t m = 0;
        for (int j = 0; j < (int)AP_TOKENS; j++) {
            uint32_t a = 0;
            for (int f = 0; f < (int)AP_FEAT; f++) a += (uint32_t)Q[i][f] * (uint32_t)Q[j][f];
            uint8_t s = ap_sat_u8(a >> AP_SELF_S_SHIFT);
            S8[j] = s; if (s > m) m = s;
        }
        uint32_t Z = 0;
        for (int j = 0; j < (int)AP_TOKENS; j++) {
            int d = (int)m - (int)S8[j]; if (d > 127) d = 127;
            e[j] = expTab[d]; Z += e[j];
        }
        uint32_t b = Z >> AP_SEED_SHIFT; if (b > 127u) b = 127u;
        uint32_t R = (uint32_t)seedTab[b] * 2u;
        for (int it = 0; it < AP_NEWTON_ITERS; it++) R = (R * ((1u << 17) - Z * R)) >> 16;
        for (int j = 0; j < (int)AP_TOKENS; j++) pq[j] = (uint8_t)(((uint32_t)e[j] * R) >> AP_PQ_SHIFT);
        for (int f = 0; f < (int)AP_FEAT; f++) {
            uint32_t a = 0;
            for (int j = 0; j < (int)AP_TOKENS; j++) a += (uint32_t)pq[j] * (uint32_t)Q[j][f];
            O_out[i * (int)AP_FEAT + f] = ap_sat_u8(a >> AP_O_SHIFT);
        }
    }
}

/* ===== PARTIAL-CENTERED self-attention (alpha=0.5): half the LayerNorm DC step =====
 * c = clamp(raw - mean/2 + 64, 0, 255). Subtracting HALF the patch mean halves the
 * brightness bias of raw-pixel attention (dark stays dark) while keeping enough DC
 * floor that flat/low-texture patches don't degenerate (full centering -> flat
 * patches get a uniform softmax that underflows to 0 = black on 8-bit fixed point).
 * The partial-centered tiles feed the EXISTING attention_self.S (unsigned, >>14)
 * verbatim as Q and (transposed) K; V = the RAW patches (output = real pixels).
 * Verified (host): brightness bias 0.64 -> 0.24, zero degenerate tokens.            */
#define AP_SELFC_EXP_DECAY_Q8 110u   /* host-swept (clustered frame): bias 0.24, no degeneracy */

/* a textured synthetic frame: per-patch distinct stripe freq/direction + brightness
 * -> rich covariance structure (the smooth ap_build_frame has ~0 covariance, which
 * makes centered self-attention degenerate; use this to exercise/tune the path). */
static inline uint8_t ap_frame_textured_pixel(int r, int c) {
    int Py = r / 8, Px = c / 8, ry = r % 8, rx = c % 8;
    int freq = 1 + ((Py * 2 + Px) % 5);
    int dir = (Py + Px) & 1, coord = dir ? ry : rx;
    int stripe = ((coord * freq) & 3) * 64;
    int dc = 30 + ((Py * 13 + Px * 7) % 6) * 35;
    int v = dc / 2 + stripe / 2;
    return (uint8_t)(v > 255 ? 255 : v);
}
static inline void ap_build_frame_textured(uint8_t *frame) {
    for (int r = 0; r < (int)AP_IMG; r++)
        for (int c = 0; c < (int)AP_IMG; c++)
            frame[r * (int)AP_IMG + c] = ap_frame_textured_pixel(r, c);
}

static inline void ap_selfc_exp_table(uint8_t tab[128]) {
    int acc = 255;
    for (int k = 0; k < 128; k++) { tab[k] = (uint8_t)acc; acc = (acc * (int)AP_SELFC_EXP_DECAY_Q8) >> 8; }
}
static inline void ap_build_selfc_exp_lut(uint8_t *lut /*[128*128]*/) {
    uint8_t t[128]; ap_selfc_exp_table(t);
    for (int r = 0; r < 128; r++) for (int k = 0; k < 128; k++) lut[r * 128 + k] = t[k];
}

/* partial-center one token's 64 features (matches attention_self_centered.S):
 * c = clamp(raw - mean/2 + 128, 0, 255), with mean/2 = sum>>7. The +128 keeps the
 * value >=1 so the kernel clamps with a single unsigned vminu (no signed clamp).  */
static inline void ap_center_token(const uint8_t *q, uint8_t *c) {
    uint32_t s = 0;
    for (int d = 0; d < (int)AP_FEAT; d++) s += q[d];
    int hm = (int)(s >> 7);                        /* mean/2 */
    for (int d = 0; d < (int)AP_FEAT; d++) {
        int v = (int)q[d] - hm + 128;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        c[d] = (uint8_t)v;
    }
}
/* partial-center a [128][128] tile (per row, over the 64 real feature columns). */
static inline void ap_center_tile(const uint8_t *raw, uint8_t *c) {
    for (int r = 0; r < 128; r++) {
        uint32_t s = 0;
        for (int d = 0; d < (int)AP_FEAT; d++) s += raw[r * 128 + d];
        int hm = (int)(s >> 7);
        for (int d = 0; d < (int)AP_FEAT; d++) {
            int v = (int)raw[r * 128 + d] - hm + 128;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            c[r * 128 + d] = (uint8_t)v;
        }
        for (int d = (int)AP_FEAT; d < 128; d++) c[r * 128 + d] = 0;
    }
}

/* bit-accurate centered self-attention reference: Q=K=center(patches), V=raw patches. */
static inline void ap_self_centered_reference(const uint8_t *frame, const uint8_t expTab[128],
                                              const uint8_t seedTab[128], uint8_t *O_out) {
    static uint8_t Q[AP_TOKENS][AP_FEAT], C[AP_TOKENS][AP_FEAT];
    ap_patchify(frame, &Q[0][0]);
    for (int i = 0; i < (int)AP_TOKENS; i++) ap_center_token(Q[i], C[i]);
    for (int i = 0; i < (int)AP_TOKENS; i++) {
        uint8_t S8[AP_TOKENS], e[AP_TOKENS], pq[AP_TOKENS];
        uint8_t m = 0;
        for (int j = 0; j < (int)AP_TOKENS; j++) {
            uint32_t a = 0;
            for (int d = 0; d < (int)AP_FEAT; d++) a += (uint32_t)C[i][d] * (uint32_t)C[j][d];
            uint8_t s = ap_sat_u8(a >> AP_SELF_S_SHIFT);   /* partial-centered dot, same >>14 */
            S8[j] = s; if (s > m) m = s;
        }
        uint32_t Z = 0;
        for (int j = 0; j < (int)AP_TOKENS; j++) {
            int d = (int)m - (int)S8[j]; if (d > 127) d = 127;
            e[j] = expTab[d]; Z += e[j];
        }
        uint32_t b = Z >> AP_SEED_SHIFT; if (b > 127u) b = 127u;
        uint32_t R = (uint32_t)seedTab[b] * 2u;
        for (int it = 0; it < AP_NEWTON_ITERS; it++) R = (R * ((1u << 17) - Z * R)) >> 16;
        for (int j = 0; j < (int)AP_TOKENS; j++) pq[j] = (uint8_t)(((uint32_t)e[j] * R) >> AP_PQ_SHIFT);
        for (int d = 0; d < (int)AP_FEAT; d++) {
            uint32_t a = 0;
            for (int j = 0; j < (int)AP_TOKENS; j++) a += (uint32_t)pq[j] * (uint32_t)Q[j][d];  /* V = RAW */
            O_out[i * (int)AP_FEAT + d] = ap_sat_u8(a >> AP_O_SHIFT);
        }
    }
}

/* ===== 16-bit-SCORE self-attention: keep the score wide through the max-subtract =====
 * The 8-bit score path narrows the dot to u8 BEFORE the softmax, which crushes the small
 * texture covariance (the brightness term eats the 8 bits). Here the score is held at
 * 16-bit: S16 = dot >> AP_S16_NARROW; the max-subtract (m16 - S16) is done at 16-bit (the
 * brightness DC cancels there); only the *difference* is narrowed to the 8-bit exp-LUT
 * index via >> AP_S16_DIFF. So the texture survives into the attention weights. Q,K are
 * partial-centered (reuse ap_center_token); V = raw patches. AP_S16_DIFF (the texture
 * shift) and the decay are tuned live on the camera. */
#define AP_S16_NARROW   6        /* dot(u32) >> 6 -> S16 (u16); 4.16e6>>6 = 65025 fits */
#define AP_S16_DIFF     8        /* (m16 - S16) >> 8 -> 0..127 exp-LUT index (tunable)  */
#define AP_S16_EXP_DECAY_Q8 150u /* tunable live */

static inline void ap_s16_exp_table(uint8_t tab[128]) {
    int acc = 255;
    for (int k = 0; k < 128; k++) { tab[k] = (uint8_t)acc; acc = (acc * (int)AP_S16_EXP_DECAY_Q8) >> 8; }
}
static inline void ap_build_s16_exp_lut(uint8_t *lut /*[128*128]*/) {
    uint8_t t[128]; ap_s16_exp_table(t);
    for (int r = 0; r < 128; r++) for (int k = 0; k < 128; k++) lut[r * 128 + k] = t[k];
}

/* bit-accurate 16-bit-score self-attention reference (Q=K=partial-centered, V=raw). */
static inline void ap_self16_reference(const uint8_t *frame, const uint8_t expTab[128],
                                       const uint8_t seedTab[128], uint8_t *O_out) {
    static uint8_t Q[AP_TOKENS][AP_FEAT], C[AP_TOKENS][AP_FEAT];
    ap_patchify(frame, &Q[0][0]);
    for (int i = 0; i < (int)AP_TOKENS; i++) ap_center_token(Q[i], C[i]);
    for (int i = 0; i < (int)AP_TOKENS; i++) {
        uint16_t S16[AP_TOKENS]; uint8_t e[AP_TOKENS], pq[AP_TOKENS];
        uint16_t m = 0;
        for (int j = 0; j < (int)AP_TOKENS; j++) {
            uint32_t a = 0;
            for (int d = 0; d < (int)AP_FEAT; d++) a += (uint32_t)C[i][d] * (uint32_t)C[j][d];
            uint32_t s = a >> AP_S16_NARROW; if (s > 65535u) s = 65535u;
            S16[j] = (uint16_t)s; if (S16[j] > m) m = S16[j];
        }
        uint32_t Z = 0;
        for (int j = 0; j < (int)AP_TOKENS; j++) {
            int d = ((int)m - (int)S16[j]) >> AP_S16_DIFF;   /* m>=S16 -> >=0 */
            if (d > 127) d = 127;
            e[j] = expTab[d]; Z += e[j];
        }
        uint32_t b = Z >> AP_SEED_SHIFT; if (b > 127u) b = 127u;
        uint32_t R = (uint32_t)seedTab[b] * 2u;
        for (int it = 0; it < AP_NEWTON_ITERS; it++) R = (R * ((1u << 17) - Z * R)) >> 16;
        for (int j = 0; j < (int)AP_TOKENS; j++) pq[j] = (uint8_t)(((uint32_t)e[j] * R) >> AP_PQ_SHIFT);
        for (int d = 0; d < (int)AP_FEAT; d++) {
            uint32_t a = 0;
            for (int j = 0; j < (int)AP_TOKENS; j++) a += (uint32_t)pq[j] * (uint32_t)Q[j][d];  /* V=RAW */
            O_out[i * (int)AP_FEAT + d] = ap_sat_u8(a >> AP_O_SHIFT);
        }
    }
}
