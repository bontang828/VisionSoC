/*
 * attention_patch.S -- blocked ViT 8x8-patch attention math on patch-token tiles.
 *   O = softmax(Q*K^T / sqrt(d)) * V    (256 tokens x 64 features, u8)
 * computed as 2 query-blocks x 2 key/value-blocks (this stream = ONE query-block
 * over both key/value-blocks; the host calls it twice). Reuses the row-kernel
 * matmul/softmax/Newton idiom; the new parts are the two-register (Sa,Sb) softmax
 * over all 256 keys, and pqa/pqb + two-V-block PV. Patchify is a separate kernel
 * (attention_patch_im2col.S) that produced the Q tile.
 *
 * Bare stream: libt1 supplies vtype/vl/vertical_mode/rs1 per issue
 * (kernels/attention_patch_issue.h). Fixed-point + C reference:
 * kernels/attention_patch_weights.h. See fyp_doc/attention_kernel_status.md.
 *
 * VERTICAL (vmode=1) ONLY for SEW=8 vle8 V-loads (Kt/V) and SEW=8 vrgather.vx
 * broadcasts. All widening / SEW>8 / Newton is horizontal. Feature-dimension
 * ops (QK broadcast/mul/reduce) run at vl=64; key-dimension at vl=128.
 *
 * Scalar nominations (issue.h injects per word):
 *   a0 = load/store PA   a5 = gather index (key j in QK, value d in PV)
 *   a6 = 255  a7 = 127   t0 = 1  t1 = 2  t2 = 131072 (2^17)   x0 = 0
 *
 * Vector map (SEW=8 LMUL=1, vLen=1024 = 128 lanes):
 *   v0  lane-0 mask     v2  Q tile          v3  Sa->ea->pqa
 *   v4  Kt blk / Va     v6  score->Sb->eb->pqb       v30 Vb
 *   v8  bcast K[j]/V[:,d]   v10 slide scratch    v12:13 u16 prod / Rb16
 *   v14:15 Z32          v16  raw u32 / Z16 / P16     v18 narrow / Newton t / R16
 *   v20 u8 / seed / R32 v22  max-acc -> O accum      v24 expLUT / seedLUT
 *   v26 max broadcast   v28 zero
 *
 * Region map (keep in lockstep with attention_patch_issue.h):
 *   PRE[0,4) KTLOAD=4 SCZERO=5 QK_BODY[6,15)x128
 *   SOFTMAX[16,29) NEWTON[29,46) NORM[46,54)
 *   PV_PRE[54,57) PV_BODY[57,69)x64
 *   O_STORE=69  CKPT: vse8 v3 =70  vse8 v6 =71  vse32 v14 =72  vse32 v20 =73
 */
.set AP_S_SHIFT, 13
.set AP_PQ_SHIFT, 8
.set AP_O_SHIFT, 8
.set AP_F_SHIFT, 16
.set AP_SEED_SHIFT, 9

.text
.global attention_patch
attention_patch:
    /* ===== PRE [0,4) ===== */
    vle8.v        v2,  (a0)              /* 0  H : Q tile -> v2                       */
    vid.v         v10                    /* 1  H : v10[c]=c                           */
    vmseq.vi      v0,  v10, 0            /* 2  H : v0 = (c==0) lane-0 mask            */
    vmv.v.i       v28, 0                 /* 3  H : zero (reduction scalar source)     */

    /* ===== KTLOAD=4 / SCZERO=5 (per key-block) ===== */
    vle8.v        v4,  (a0)              /* 4  V : Kt block -> v4 (V-load; H-view=K)  */
    vmv.v.i       v6,  0                 /* 5  H : score row = 0                      */

    /* ===== QK_BODY [6,15): one key column j (a5=j); feat ops vl=64 ===== */
    vrgather.vx   v8,  v4,  a5           /* 6  V : v8 = K[j][0..63] broadcast         */
    vwmulu.vv     v12, v2,  v8           /* 7  H : Q[r][f]*K[j][f] -> v12:13 (vl=64)  */
    vwredsumu.vs  v16, v12, v28          /* 8  H : raw_S = sum_f -> v16[0] (vl=64)    */
    vnsrl.wi      v18, v16, AP_S_SHIFT   /* 9  H1: raw_S >> S_SHIFT                   */
    vminu.vx      v18, v18, a6           /* 10 H1: min(.,255)                         */
    vnsrl.wi      v20, v18, 0            /* 11 H1: -> u8 S8 in v20[0]                 */
    vslideup.vi   v10, v6,  1            /* 12 H : shift score row                    */
    vmerge.vvm    v10, v10, v20, v0      /* 13 H : insert S8 at lane 0                */
    vmv.v.v       v6,  v10               /* 14 H : commit score row v6                */

    /* ===== COPYSA=15 : stash Sa = v6 -> v3 (after key-block a) ===== */
    vmv.v.v       v3,  v6                /* 15 H : Sa -> v3                           */

    /* ===== SOFTMAX [16,29): max/exp over 256 keys (Sa=v3, Sb=v6) ===== */
    vredmaxu.vs   v22, v3,  v28          /* 16 H : max(Sa)                            */
    vredmaxu.vs   v22, v6,  v22          /* 17 H : max(Sa,Sb) = m in v22[0]           */
    vrgather.vx   v26, v22, x0           /* 18 H : broadcast m across row             */
    vle8.v        v24, (a0)              /* 19 H : expLUT replicated -> v24           */
    vssubu.vv     v20, v26, v3           /* 20 H : m - Sa (saturating)                */
    vminu.vx      v20, v20, a7           /* 21 H : min(.,127)                         */
    vrgather.vv   v3,  v24, v20          /* 22 H : ea = expLUT[.] -> v3               */
    vssubu.vv     v20, v26, v6           /* 23 H : m - Sb                             */
    vminu.vx      v20, v20, a7           /* 24 H : min(.,127)                         */
    vrgather.vv   v6,  v24, v20          /* 25 H : eb = expLUT[.] -> v6               */
    vwredsumu.vs  v16, v3,  v28          /* 26 H : Z16 = sum ea (e8->16)              */
    vwredsumu.vs  v16, v6,  v16          /* 27 H : Z16 += sum eb (chain) -> v16[0]    */
    vwmulu.vx     v14, v16, t0           /* 28 H1: Z32 = Z16*1 (t0=1) -> v14[0]       */

    /* ===== NEWTON [29,46): R ~= 2^16/Z, seed (shift 9) + 3 iters (SEW=32) ===== */
    vnsrl.wi      v18, v16, AP_SEED_SHIFT/* 29 H1: idx8 = Z16>>9 (seed bucket)        */
    vle8.v        v24, (a0)              /* 30 H : seedLUT8 replicated -> v24         */
    vrgather.vv   v20, v24, v18          /* 31 H : seed8 = seedLUT8[idx] -> v20[0]    */
    vwmulu.vx     v12, v20, t1           /* 32 H1: seed16 = seed8*2 (t1=2)            */
    vwmulu.vx     v20, v12, t0           /* 33 H1: R0(u32) = seed16*1 -> v20[0]       */
    vmul.vv       v18, v14, v20          /* 34 H1: t = Z*R          (iter 1)          */
    vrsub.vx      v18, v18, t2           /* 35 H1: t = 2^17 - Z*R   (t2=131072)       */
    vmul.vv       v20, v20, v18          /* 36 H1: R = R*t                            */
    vsrl.vi       v20, v20, AP_F_SHIFT   /* 37 H1: R >>= 16                           */
    vmul.vv       v18, v14, v20          /* 38 H1: iter 2                             */
    vrsub.vx      v18, v18, t2           /* 39 H1                                     */
    vmul.vv       v20, v20, v18          /* 40 H1                                     */
    vsrl.vi       v20, v20, AP_F_SHIFT   /* 41 H1                                     */
    vmul.vv       v18, v14, v20          /* 42 H1: iter 3                             */
    vrsub.vx      v18, v18, t2           /* 43 H1                                     */
    vmul.vv       v20, v20, v18          /* 44 H1                                     */
    vsrl.vi       v20, v20, AP_F_SHIFT   /* 45 H1: R in v20[0]                        */

    /* ===== NORM [46,54): pqa,pqb = (e*R)>>8 ===== */
    vnsrl.wi      v18, v20, 0            /* 46 H1: R16 = R -> v18[0]                  */
    vrgather.vx   v12, v18, x0           /* 47 H : Rb16 broadcast across row          */
    vwmulu.vx     v10, v3,  t0           /* 48 H : e16a = ea*1                        */
    vmul.vv       v16, v10, v12          /* 49 H : P16a = e16a * Rb16                 */
    vnsrl.wi      v3,  v16, AP_PQ_SHIFT  /* 50 H : pqa = P16a >> 8 -> v3              */
    vwmulu.vx     v10, v6,  t0           /* 51 H : e16b = eb*1                        */
    vmul.vv       v16, v10, v12          /* 52 H : P16b = e16b * Rb16                 */
    vnsrl.wi      v6,  v16, AP_PQ_SHIFT  /* 53 H : pqb = P16b >> 8 -> v6              */

    /* ===== PV_PRE [54,57): V-load Va,Vb; clear O accumulator ===== */
    vle8.v        v4,  (a0)              /* 54 V : Va -> v4                           */
    vle8.v        v30, (a0)              /* 55 V : Vb -> v30                          */
    vmv.v.i       v22, 0                 /* 56 H : O accumulator = 0                  */

    /* ===== PV_BODY [57,69): one value column d (a5=d); reduce over 256 keys ===== */
    vrgather.vx   v8,  v4,  a5           /* 57 V : v8 = Va[:][d] broadcast            */
    vwmulu.vv     v12, v3,  v8           /* 58 H : pqa[r][k]*Va[k][d] (vl=128)        */
    vwredsumu.vs  v16, v12, v28          /* 59 H : accA = sum_k -> v16[0]             */
    vrgather.vx   v8,  v30, a5           /* 60 V : v8 = Vb[:][d] broadcast            */
    vwmulu.vv     v12, v6,  v8           /* 61 H : pqb[r][k]*Vb[k][d]                 */
    vwredsumu.vs  v16, v12, v16          /* 62 H : acc = accA + sum_k (chain)         */
    vnsrl.wi      v18, v16, AP_O_SHIFT   /* 63 H1: acc >> 8                           */
    vminu.vx      v18, v18, a6           /* 64 H1: min(.,255)                         */
    vnsrl.wi      v20, v18, 0            /* 65 H1: -> u8 O in v20[0]                  */
    vslideup.vi   v10, v22, 1            /* 66 H : shift O row (vl=64)                */
    vmerge.vvm    v10, v10, v20, v0      /* 67 H : insert O at lane 0                 */
    vmv.v.v       v22, v10               /* 68 H : commit O row v22                   */

    /* ===== O_STORE=69 + checkpoints [70,74) ===== */
    vse8.v        v22, (a0)              /* 69 H : store O (vl=64)  -> dst / O ckpt   */
    vse8.v        v3,  (a0)              /* 70 H : store v3 (Sa/ea/pqa)  checkpoint   */
    vse8.v        v6,  (a0)              /* 71 H : store v6 (Sb/eb/pqb)  checkpoint   */
    vse32.v       v14, (a0)              /* 72 H1: store Z32 (vl=1)      checkpoint   */
    vse32.v       v20, (a0)              /* 73 H1: store R   (vl=1)      checkpoint   */
