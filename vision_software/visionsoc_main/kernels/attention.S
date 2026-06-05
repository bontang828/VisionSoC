/*
 * attention.S -- scaled-dot-product attention on the 2D fabric, SEW=8 inputs
 * with horizontal widening, divide-free softmax (exp LUT + Newton reciprocal).
 *
 *   O = softmax(Q*K^T / sqrt(d)) * V        (128 tokens x 128 features, u8)
 *
 * This is a BARE vector instruction stream (no vsetvli): libt1 issues one word
 * at a time and supplies vtype / vl / vertical_mode / rs1 PER ISSUE over MMIO.
 * kernels/attention_issue.h is the schedule that drives it; the C reference and
 * the fixed-point spec live in kernels/attention_weights.h. See
 * fyp_doc/attention_kernel_status.md.
 *
 * VERTICAL MODE IS SEW=8 ONLY (byte-granular scatter). The only vmode=1 words
 * are the SEW=8 vle8 V-loads (Kt, V) and the SEW=8 vrgather.vx row-broadcasts.
 * Every widening / SEW>8 word is vmode=0 (horizontal). See the
 * project_vertical_mode_sew8_only memory.
 *
 * Scalar register-field nominations (issue.h injects the value per word):
 *   a0 = Q PA        a1 = B PA (Kt in QK, V in PV)   a2 = expLUT PA
 *   a3 = seedLUT PA  a4 = output / checkpoint PA      a5 = gather index (j or v)
 *   a6 = 255         a7 = 127        t0 = 1     t1 = 2     t2 = 131072 (2^17)
 *   x0 = 0           (gather index 0 for the max / reciprocal broadcasts)
 *
 * Vector register map (vLen=1024, baseLMUL=1; SEW=8 LMUL=1 = one 128-lane row):
 *   v0  lane-0 mask          v2  Q (QK)              v4  B = Kt(QK)/V(PV)
 *   v6  score row -> pq      v8  bcast K[j]/V[:,v] ; e (softmax)
 *   v10 slide scratch ; e16  v12:13 u16 products ; Rb16    v14:15 Z32
 *   v16 raw u32 ; Z16 ; P16  v18 narrow scratch ; Newton t ; R16
 *   v20 u8 narrow ; seed ; R32                     v22 O accumulator (PV)
 *   v24 expLUT / seedLUT     v26 max broadcast      v28 zero (reduction src)
 *
 * Region index map (keep in lockstep with attention_issue.h ATTN_* offsets):
 *   PRE     [0  ,6 )    QK_BODY [6  ,15)   (x128, inject a5=j)
 *   SM_EXP  [15 ,21)    SM_Z    [21 ,23)   SM_NEWTON [23 ,40)   SM_NORM [40,45)
 *   PV_PRE  [45 ,47)    PV_BODY [47 ,56)   (x128, inject a5=v)
 *   STORES  s8=56 e=57 z=58 r=59 pq=60 o=61
 */

.set ATTN_S_SHIFT, 11      /* score narrow: folds 1/sqrt(128); matches attention_weights.h */
.set ATTN_PQ_SHIFT, 8      /* (e*R) >> 8  -> Q0.8 weight */
.set ATTN_O_SHIFT, 8       /* (sum pq*V) >> 8 -> u8 */
.set ATTN_F_SHIFT, 16      /* Newton: R*(2^17 - Z*R) >> 16 */

.text
.global attention
attention:
    /* ===== PRE [0,6) ===== */
    vle8.v        v2,  (a0)              /* 0  H : Q -> v2                          */
    vle8.v        v4,  (a1)              /* 1  V : Kt -> v4 (V-load; H-view = K)    */
    vid.v         v10                    /* 2  H : v10[c] = c                       */
    vmseq.vi      v0,  v10, 0            /* 3  H : v0 = (c==0) lane-0 mask          */
    vmv.v.i       v6,  0                 /* 4  H : score row = 0                    */
    vmv.v.i       v28, 0                 /* 5  H : zero (reduction scalar source)   */

    /* ===== QK_BODY [6,15): one key column j, issued x128 (a5=j) ===== */
    vrgather.vx   v8,  v4,  a5           /* 6  V : v8 = K[j][:] (broadcast hw-row j)*/
    vwmulu.vv     v12, v2,  v8           /* 7  H : Q[r][c]*K[j][c] -> v12:13 (u16)  */
    vwredsumu.vs  v16, v12, v28          /* 8  H : raw_S = sum_c -> v16[0] (u32)    */
    vnsrl.wi      v18, v16, ATTN_S_SHIFT /* 9  H1: raw_S >> S_SHIFT (u32->u16)      */
    vminu.vx      v18, v18, a6           /* 10 H1: min(.,255) saturate (a6=255)     */
    vnsrl.wi      v20, v18, 0            /* 11 H1: -> u8 S8 in v20[0]               */
    vslideup.vi   v10, v6,  1            /* 12 H : shift score row up -> scratch    */
    vmerge.vvm    v10, v10, v20, v0      /* 13 H : v10[0] = S8 (lane-0 mask)        */
    vmv.v.v       v6,  v10               /* 14 H : commit -> score row v6           */

    /* ===== SM_EXP [15,21): row-max, subtract, exp LUT ===== */
    vredmaxu.vs   v22, v6,  v28          /* 15 H : v22[0] = max_j S8 (UNSIGNED:     */
                                         /*        S8 can exceed 127)               */
    vrgather.vx   v26, v22, x0           /* 16 H : broadcast max across row (idx 0) */
    vssubu.vv     v20, v26, v6           /* 17 H : max - S8 (saturating, >=0)       */
    vminu.vx      v20, v20, a7           /* 18 H : min(.,127) clamp (a7=127)        */
    vle8.v        v24, (a2)              /* 19 H : expLUT replicated -> v24         */
    vrgather.vv   v8,  v24, v20          /* 20 H : e = expLUT[idx] -> v8            */

    /* ===== SM_Z [21,23): Z = sum_j e ===== */
    vwredsumu.vs  v16, v8,  v28          /* 21 H : Z16 = sum e (u8->u16) -> v16[0]  */
    vwmulu.vx     v14, v16, t0           /* 22 H1: Z32 = Z16*1 (u16->u32) (t0=1)    */

    /* ===== SM_NEWTON [23,40): R ~= 2^16 / Z, seed + 3 iters (SEW=32) ===== */
    vnsrl.wi      v18, v16, 8            /* 23 H1: idx8 = Z16>>8 (seed bucket)      */
    vle8.v        v24, (a3)              /* 24 H : seedLUT8 replicated -> v24       */
    vrgather.vv   v20, v24, v18          /* 25 H : seed8 = seedLUT8[idx] -> v20[0]  */
    vwmulu.vx     v12, v20, t1           /* 26 H1: seed16 = seed8*2 (recover R0)    */
    vwmulu.vx     v20, v12, t0           /* 27 H1: R0(u32) = seed16*1 -> v20[0]     */
    vmul.vv       v18, v14, v20          /* 28 H1: t = Z*R           (iter 1)       */
    vrsub.vx      v18, v18, t2           /* 29 H1: t = 2^17 - Z*R   (t2=131072)     */
    vmul.vv       v20, v20, v18          /* 30 H1: R = R*t                          */
    vsrl.vi       v20, v20, ATTN_F_SHIFT /* 31 H1: R >>= 16                         */
    vmul.vv       v18, v14, v20          /* 32 H1: t = Z*R           (iter 2)       */
    vrsub.vx      v18, v18, t2           /* 33 H1                                   */
    vmul.vv       v20, v20, v18          /* 34 H1                                   */
    vsrl.vi       v20, v20, ATTN_F_SHIFT /* 35 H1                                   */
    vmul.vv       v18, v14, v20          /* 36 H1: t = Z*R           (iter 3)       */
    vrsub.vx      v18, v18, t2           /* 37 H1                                   */
    vmul.vv       v20, v20, v18          /* 38 H1                                   */
    vsrl.vi       v20, v20, ATTN_F_SHIFT /* 39 H1: R in v20[0]                      */

    /* ===== SM_NORM [40,45): pq = (e*R) >> 8 ===== */
    vnsrl.wi      v18, v20, 0            /* 40 H1: R16 = R (u32->u16) in v18[0]     */
    vrgather.vx   v12, v18, x0           /* 41 H : Rb16 = broadcast R16 across row  */
    vwmulu.vx     v10, v8,  t0           /* 42 H : e16 = e*1 (u8->u16)              */
    vmul.vv       v16, v10, v12          /* 43 H : P16 = e16 * Rb16 (u16)           */
    vnsrl.wi      v6,  v16, ATTN_PQ_SHIFT/* 44 H : pq = P16 >> 8 -> v6 (u8)         */

    /* ===== PV_PRE [45,47): V-load V, clear O accumulator ===== */
    vle8.v        v4,  (a1)              /* 45 V : V -> v4 (V-load; a1 = V PA)      */
    vmv.v.i       v22, 0                 /* 46 H : O accumulator = 0                */

    /* ===== PV_BODY [47,56): one value column v, issued x128 (a5=v) ===== */
    vrgather.vx   v8,  v4,  a5           /* 47 V : v8 = V[:][v] (broadcast)         */
    vwmulu.vv     v12, v6,  v8           /* 48 H : pq[r][j]*V[j][v] -> v12:13 (u16) */
    vwredsumu.vs  v16, v12, v28          /* 49 H : U = sum_j -> v16[0] (u32)        */
    vnsrl.wi      v18, v16, ATTN_O_SHIFT /* 50 H1: U >> 8 (u32->u16)                */
    vminu.vx      v18, v18, a6           /* 51 H1: min(.,255) saturate              */
    vnsrl.wi      v20, v18, 0            /* 52 H1: -> u8 O in v20[0]                */
    vslideup.vi   v10, v22, 1            /* 53 H : shift O row up -> scratch        */
    vmerge.vvm    v10, v10, v20, v0      /* 54 H : v10[0] = O                       */
    vmv.v.v       v22, v10               /* 55 H : commit -> O row v22              */

    /* ===== STORES [56,62): checkpoints (probe) + final O ===== */
    vse8.v        v6,  (a4)              /* 56 H  : store S8 score  (checkpoint)    */
    vse8.v        v8,  (a4)              /* 57 H  : store e         (checkpoint)    */
    vse32.v       v14, (a4)              /* 58 H1 : store Z (vl=1)  (checkpoint)    */
    vse32.v       v20, (a4)              /* 59 H1 : store R (vl=1)  (checkpoint)    */
    vse8.v        v6,  (a4)              /* 60 H  : store pq        (checkpoint)    */
    vse8.v        v22, (a4)              /* 61 H  : store O         (final / dst)   */
