/*
 * attention_patch_im2col.S -- on-fabric 8x8 patchify (im2col) via three
 * vrgather passes (H -> V -> H). A 128x128 frame in URAM (hw-row = image row,
 * lane = image col) becomes a 128x64 patch-token tile per block (token = patch,
 * lane = the 8x8 pixels). SEW=8 throughout (pure byte rearrangement); NO SEW>8
 * vertical -- the only vmode=1 word is the vertical vrgather.vv row-router.
 *
 * Bare instruction stream: libt1 supplies vtype/vl/vertical_mode/rs1 PER ISSUE
 * (kernels/attention_patch_im2col_issue.h). The index tiles and the 3-pass
 * decomposition (verified bit-exact in C) live in attention_patch_weights.h:
 *   S[P][Q] = image[blockbase+P][Q]
 *   M1[P][L1] = S[P][idxH1[P][L1]]           (H : per-row lane gather)
 *   M2[t][L1] = M1[idxV[t][L1]][L1]          (V : per-element row gather)
 *   T[t][f]   = M2[t][idxH2[t][f]]           (H)
 *
 * rs1 nominations (issue.h injects the PA per word): index/src/out addresses.
 *
 * Vector map (SEW=8 LMUL=1, vLen=1024 = 128 lanes):
 *   v4 idxH1   v6 idxV   v8 idxH2          (index tiles, loaded once)
 *   v10 S      v12 M1    v14 M2   v16 T    (per-block working set)
 *
 * Region map (keep in lockstep with the issue header):
 *   PRE  [0,3)  load idxH1, idxV, idxH2
 *   BODY [3,8)  vle8 S ; vrgather H1 ; vrgather V ; vrgather H2 ; vse8 T
 *   CKPT  m1=8  m2=9    (probe-only checkpoint stores of M1, M2)
 */
.text
.global attention_patch_im2col
attention_patch_im2col:
    /* ===== PRE [0,3): index tiles (issued once for both blocks) ===== */
    vle8.v        v4,  (a0)            /* 0  H : idxH1 -> v4              */
    vle8.v        v6,  (a0)            /* 1  H : idxV  -> v6             */
    vle8.v        v8,  (a0)            /* 2  H : idxH2 -> v8            */

    /* ===== BODY [3,8): one token-block (a0 = src region / out tile) ===== */
    vle8.v        v10, (a0)            /* 3  H : S = src region (image rows) */
    vrgather.vv   v12, v10, v4         /* 4  H : M1 = S[idxH1]               */
    vrgather.vv   v14, v12, v6         /* 5  V : M2 = M1[idxV] (vertical)    */
    vrgather.vv   v16, v14, v8         /* 6  H : T  = M2[idxH2]              */
    vse8.v        v16, (a0)            /* 7  H : store token tile           */

    /* ===== CKPT [8,10): probe-only checkpoints ===== */
    vse8.v        v12, (a0)            /* 8  H : store M1 (checkpoint)       */
    vse8.v        v14, (a0)            /* 9  H : store M2 (checkpoint)       */
