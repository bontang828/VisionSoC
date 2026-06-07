/*
 * attention_self16.S -- self-attention with a 16-BIT score path so the texture
 * covariance survives the softmax (the 8-bit path narrows the score before the
 * max-subtract, crushing the small texture signal under the brightness DC).
 *
 *   S16[i][j] = (Q_i . K_j) >> 6            (16-bit, holds DC + texture)
 *   m16       = max_j S16                    (16-bit)
 *   idx       = min((m16 - S16) >> DIFF, 127)  (DC cancels in m-S; texture survives)
 *   e         = expLUT[idx]                  (8-bit; softmax/Newton/PV as usual)
 *
 * Q,K are PARTIAL-centered patches (attention_self_centered.S); V = raw patches.
 * One query-block over both key/value-blocks; host calls it twice. The score row
 * is SEW=16, LMUL=2 (128 keys); everything from e onward is 8-bit like
 * attention_self.S. Bare stream; libt1 supplies vtype/vl/vmode/rs1 per issue.
 *
 * Scalars: a5 gather idx · a6=255 · a7=127 · t0=1 t1=2 t2=131072 · x0=0
 * .set AP_S16_NARROW=6  AP_S16_DIFF=8  (match attention_patch_weights.h)
 *
 * Vector map:
 *   v0 mask   v2 Q   v4 Kt/Va   v30 Vb   v28 zero   v24 expLUT/seedLUT
 *   v6:7 Sb16(build target)  v8:9 Sa16(held)  v10:11 slide scratch
 *   v12:13 u16 prod / Rb16   v14:15 Z32   v16:17 u32 dot / P16
 *   v18:19 S16 / d16 narrow  v20 idx/seed/R/u8   v22 m16 / O accum
 *   v26:27 m16 broadcast     v3 ea->pqa   v5 eb->pqb
 *
 * Region map (keep in lockstep with attention_self16_issue.h):
 *   PRE[0,4) KTLOAD=4 SCZERO=5 QK_BODY[6,13)x128 COPYSA=13
 *   SOFTMAX[14,26) Z[26,29) NEWTON[29,46) NORM[46,54)
 *   PV_PRE[54,57) PV_BODY[57,69)x64 O_STORE=69
 *   CKPT: vse16 v8=70  vse16 v6=71  vse8 v3=72  vse8 v5=73  vse32 v14=74  vse32 v20=75
 */
.set AP_S16_NARROW, 6
.set AP_S16_DIFF, 8
.set AP_PQ_SHIFT, 8
.set AP_O_SHIFT, 8
.set AP_F_SHIFT, 16
.set AP_SEED_SHIFT, 9

.text
.global attention_self16
attention_self16:
    /* ===== PRE [0,4) ===== */
    vle8.v        v2,  (a0)              /* 0  H : Q tile (centered) -> v2             */
    vid.v         v10                    /* 1  H : v10[c]=c                            */
    vmseq.vi      v0,  v10, 0            /* 2  H : v0 = (c==0) lane-0 mask             */
    vmv.v.i       v28, 0                 /* 3  H : zero (reduction scalar source)      */

    /* ===== KTLOAD=4 / SCZERO=5 (per key-block) ===== */
    vle8.v        v4,  (a0)              /* 4  V : Kt block -> v4 (V-load; H-view=K)   */
    vmv.v.i       v6,  0                 /* 5  H16: Sb16 build target = 0 (SEW16)      */

    /* ===== QK_BODY [6,13): one key column j (a5=j); 16-bit score ===== */
    vrgather.vx   v20, v4,  a5           /* 6  V : v20 = K[j][0..63] broadcast         */
    vwmulu.vv     v12, v2,  v20          /* 7  H : Q[r][f]*K[j][f] -> v12:13 (vl=64)   */
    vwredsumu.vs  v16, v12, v28          /* 8  H : dot = sum_f -> v16[0] (u32, vl=64)  */
    vnsrl.wi      v18, v16, AP_S16_NARROW/* 9  H1: S16 = dot >> 6 (u32->u16) in v18[0] */
    vslideup.vi   v10, v6,  1            /* 10 H16: shift score row up                 */
    vmerge.vvm    v10, v10, v18, v0      /* 11 H16: insert S16 at elem 0               */
    vmv.v.v       v6,  v10               /* 12 H16: commit score row v6:7              */

    /* ===== COPYSA=13 : Sa16 = v6:7 -> v8:9 (after key-block a) ===== */
    vmv.v.v       v8,  v6                /* 13 H16: Sa16 -> v8:9                        */

    /* ===== SOFTMAX [14,26): 16-bit max/sub, narrow diff -> 8-bit exp ===== */
    vredmaxu.vs   v22, v8,  v28          /* 14 H16: max(Sa16) -> v22[0]                */
    vredmaxu.vs   v22, v6,  v22          /* 15 H16: m16 = max(Sa16,Sb16)               */
    vrgather.vx   v26, v22, x0           /* 16 H16: broadcast m16 -> v26:27            */
    vle8.v        v24, (a0)              /* 17 H : expLUT replicated -> v24            */
    vssubu.vv     v18, v26, v8           /* 18 H16: d16 = m16 - Sa16 (>=0)             */
    vnsrl.wi      v20, v18, AP_S16_DIFF  /* 19 H1: idx_a = d16 >> 8 (u16->u8)          */
    vminu.vx      v20, v20, a7           /* 20 H : min(idx,127)                        */
    vrgather.vv   v3,  v24, v20          /* 21 H : ea = expLUT[idx_a] -> v3 (8-bit)    */
    vssubu.vv     v18, v26, v6           /* 22 H16: d16 = m16 - Sb16                   */
    vnsrl.wi      v20, v18, AP_S16_DIFF  /* 23 H1: idx_b = d16 >> 8                    */
    vminu.vx      v20, v20, a7           /* 24 H : min(idx,127)                        */
    vrgather.vv   v5,  v24, v20          /* 25 H : eb = expLUT[idx_b] -> v5 (8-bit)    */

    /* ===== Z [26,29): Z = sum_j (ea+eb) ===== */
    vwredsumu.vs  v16, v3,  v28          /* 26 H : Z16 = sum ea (u8->u16)              */
    vwredsumu.vs  v16, v5,  v16          /* 27 H : Z16 += sum eb (chain)               */
    vwmulu.vx     v14, v16, t0           /* 28 H1: Z32 = Z16*1 (t0=1) -> v14[0]        */

    /* ===== NEWTON [29,46): R ~= 2^16/Z (seed shift 9) ===== */
    vnsrl.wi      v18, v16, AP_SEED_SHIFT/* 29 H1: idx8 = Z16>>9                        */
    vle8.v        v24, (a0)              /* 30 H : seedLUT8 replicated -> v24          */
    vrgather.vv   v20, v24, v18          /* 31 H : seed8 -> v20[0]                      */
    vwmulu.vx     v12, v20, t1           /* 32 H1: seed16 = seed8*2                     */
    vwmulu.vx     v20, v12, t0           /* 33 H1: R0(u32) = seed16*1                   */
    vmul.vv       v18, v14, v20          /* 34 H1: t = Z*R   (iter 1)                   */
    vrsub.vx      v18, v18, t2           /* 35 H1: t = 2^17 - Z*R                       */
    vmul.vv       v20, v20, v18          /* 36 H1: R = R*t                             */
    vsrl.vi       v20, v20, AP_F_SHIFT   /* 37 H1: R >>= 16                            */
    vmul.vv       v18, v14, v20          /* 38 H1: iter 2                              */
    vrsub.vx      v18, v18, t2           /* 39 H1                                       */
    vmul.vv       v20, v20, v18          /* 40 H1                                       */
    vsrl.vi       v20, v20, AP_F_SHIFT   /* 41 H1                                       */
    vmul.vv       v18, v14, v20          /* 42 H1: iter 3                              */
    vrsub.vx      v18, v18, t2           /* 43 H1                                       */
    vmul.vv       v20, v20, v18          /* 44 H1                                       */
    vsrl.vi       v20, v20, AP_F_SHIFT   /* 45 H1: R in v20[0]                         */

    /* ===== NORM [46,54): pqa,pqb = (e*R)>>8 ===== */
    vnsrl.wi      v18, v20, 0            /* 46 H1: R16 = R -> v18[0]                    */
    vrgather.vx   v12, v18, x0           /* 47 H16: Rb16 broadcast                      */
    vwmulu.vx     v10, v3,  t0           /* 48 H : e16a = ea*1                          */
    vmul.vv       v16, v10, v12          /* 49 H16: P16a = e16a * Rb16                  */
    vnsrl.wi      v3,  v16, AP_PQ_SHIFT  /* 50 H : pqa = P16a >> 8 -> v3                */
    vwmulu.vx     v10, v5,  t0           /* 51 H : e16b = eb*1                          */
    vmul.vv       v16, v10, v12          /* 52 H16: P16b                               */
    vnsrl.wi      v5,  v16, AP_PQ_SHIFT  /* 53 H : pqb -> v5                            */

    /* ===== PV_PRE [54,57) ===== */
    vle8.v        v4,  (a0)              /* 54 V : Va -> v4                            */
    vle8.v        v30, (a0)              /* 55 V : Vb -> v30                           */
    vmv.v.i       v22, 0                 /* 56 H : O accumulator = 0                   */

    /* ===== PV_BODY [57,69): one value column d (a5=d); reduce 256 keys ===== */
    vrgather.vx   v20, v4,  a5           /* 57 V : Va[:][d] broadcast                  */
    vwmulu.vv     v12, v3,  v20          /* 58 H : pqa*Va (vl=128)                     */
    vwredsumu.vs  v16, v12, v28          /* 59 H : accA = sum_k                        */
    vrgather.vx   v20, v30, a5           /* 60 V : Vb[:][d] broadcast                  */
    vwmulu.vv     v12, v5,  v20          /* 61 H : pqb*Vb                             */
    vwredsumu.vs  v16, v12, v16          /* 62 H : acc = accA + sum_k (chain)         */
    vnsrl.wi      v18, v16, AP_O_SHIFT   /* 63 H1: acc >> 8                            */
    vminu.vx      v18, v18, a6           /* 64 H1: min(.,255)                         */
    vnsrl.wi      v20, v18, 0            /* 65 H1: -> u8 O                             */
    vslideup.vi   v10, v22, 1            /* 66 H : shift O row (vl=64)                 */
    vmerge.vvm    v10, v10, v20, v0      /* 67 H : insert O at lane 0                  */
    vmv.v.v       v22, v10               /* 68 H : commit O row v22                    */

    /* ===== O_STORE=69 + checkpoints [70,76) ===== */
    vse8.v        v22, (a0)              /* 69 H : store O (vl=64) -> dst              */
    vse16.v       v8,  (a0)              /* 70 H16: store Sa16 (checkpoint)            */
    vse16.v       v6,  (a0)              /* 71 H16: store Sb16 (checkpoint)            */
    vse8.v        v3,  (a0)              /* 72 H : store v3 (ea/pqa) checkpoint        */
    vse8.v        v5,  (a0)              /* 73 H : store v5 (eb/pqb) checkpoint        */
    vse32.v       v14, (a0)              /* 74 H1: store Z32 (vl=1) checkpoint         */
    vse32.v       v20, (a0)              /* 75 H1: store R (vl=1) checkpoint           */
