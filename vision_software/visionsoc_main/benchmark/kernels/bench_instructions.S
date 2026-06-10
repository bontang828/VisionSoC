/*
 * bench_instructions.S -- bare T1 vector ops for benchmark_instructions_perf.
 *
 * Built by ../../libt1/build_kernel.sh into bench_instructions.h:
 *     static const uint32_t bench_instructions[] = { <one 32-bit word per op> };
 *     static const uint32_t bench_instructions_count = ...;
 *
 * The harness (benchmark_instructions_perf.c) supplies vtype / vl /
 * vertical_mode / rs1 / rs2 per issue, so this file contains ONLY the raw
 * vector ops -- no vsetvli, no csrw (mode + vl are host-side fields of
 * struct t1_op). The ORDER below is load-bearing: the harness indexes
 * bench_instructions[] positionally via its BI_* enum, which MUST match
 * this order exactly. If you reorder/insert here, update the enum there.
 *
 * Register convention (LMUL=1: every vN is its own 1-register group):
 *     v8  = operand A          v12 = operand B / gather index
 *     v16 = destination        v20 = zero (reduction identity)
 *     v0  = mask               v1  = mask2 (vmand.mm)
 * Scalar operands (vmsgt.vx / vrgather.vx / vmv.s.x / vslide*.vx) take their
 * value from op.rs1 at issue time; the x-register named here is cosmetic.
 * vle/vse base addresses also come from op.rs1 (a URAM scratchpad PA).
 */
    .text
    .global bench_instructions_kernel
bench_instructions_kernel:

    /* ===== setup block (issued once per H/V run) : indices 0..5 ===== */
    vle8.v   v8,  (a0)                 /* 0  BI_SET_LDA  : v8  = grid_a   */
    vle8.v   v12, (a1)                 /* 1  BI_SET_LDB  : v12 = grid_b   */
    vmv.v.i  v16, 0                    /* 2  BI_SET_Z16  : v16 = 0        */
    vmv.v.i  v20, 0                    /* 3  BI_SET_Z20  : v20 = 0        */
    vmseq.vi v0,  v8, 0               /* 4  BI_SET_M0   : v0  = (v8==0)  */
    vmseq.vi v1,  v12, 0              /* 5  BI_SET_M1   : v1  = (v12==0) */

    /* ===== timed compute ops : indices 6..32 (27 distinct) ===== */
    vadd.vv      v16, v8,  v12         /* 6   vadd.vv               */
    vmul.vv      v16, v8,  v12         /* 7   vmul.vv               */
    vmacc.vv     v16, v8,  v12         /* 8   vmacc.vv              */
    vmadd.vv     v16, v8,  v12         /* 9   vmadd.vv              */
    vand.vv      v16, v8,  v12         /* 10  vand.vv               */
    vor.vv       v16, v8,  v12         /* 11  vor.vv                */
    vsll.vi      v16, v8,  1           /* 12  vsll.vi               */
    vsra.vi      v16, v8,  1           /* 13  vsra.vi               */
    vmseq.vv     v0,  v8,  v12         /* 14  vmseq.vv              */
    vmsle.vv     v0,  v8,  v12         /* 15  vmsle.vv              */
    vmsgt.vx     v0,  v8,  t3          /* 16  vmsgt.vx  (rs1=64)    */
    vmslt.vv     v0,  v8,  v12         /* 17  vmslt.vv              */
    vmand.mm     v0,  v0,  v1          /* 18  vmand.mm              */
    vrgather.vx  v16, v8,  t3          /* 19  vrgather.vx (rs1=5)   */
    vrgather.vx  v16, v8,  t3, v0.t    /* 20  vrgather.vx diag mask */
    vrgather.vv  v16, v8,  v12         /* 21  vrgather.vv           */
    vmv.s.x      v16, t3               /* 22  vmv.s.x   (rs1=42)    */
    vmv.x.s      t4,  v16              /* 23  vmv.x.s               */
    vmv.v.v      v16, v8               /* 24  vmv.v.v               */
    vredsum.vs   v16, v8,  v20         /* 25  vredsum.vs            */
    vredmax.vs   v16, v8,  v20         /* 26  vredmax.vs            */
    vslideup.vx  v16, v8,  a5          /* 27  vslideup.vx  (rs1=off)*/
    vslidedown.vx v16, v8, a5          /* 28  vslidedown.vx(rs1=off)*/
    vslideup.vi  v16, v8,  1           /* 29  vslideup.vi 1         */
    vslidedown.vi v16, v8, 1           /* 30  vslidedown.vi 1       */
    vslideup.vi  v16, v8,  31          /* 31  vslideup.vi 31        */
    vslidedown.vi v16, v8, 31          /* 32  vslidedown.vi 31      */

    /* ===== timed LSU ops : indices 33..38 ===== */
    vle8.v   v16, (a0)                 /* 33  vle8.v full           */
    vle8.v   v16, (a0), v0.t           /* 34  vle8.v masked         */
    vse8.v   v8,  (a1)                 /* 35  vse8.v full           */
    vse8.v   v8,  (a1), v0.t           /* 36  vse8.v masked         */
    vlse8.v  v16, (a0), t4             /* 37  vlse8.v stride2 (rs2=2)*/
    vsse8.v  v8,  (a1), t4             /* 38  vsse8.v stride2 (rs2=2)*/
