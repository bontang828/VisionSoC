# LUT Optimisation: Stub Divider + DSP-mapped Multiplier (+ Audit)

## 0. Status

Investigation only. No RTL changed yet. Author of next session should
treat § 4 / § 5 as the implementation handoff for two new Scala
modules + one TOML preset, run the comparison test in § 6, and only
*then* attempt another bitstream. § 7 lists further FPGA-resource
optimisations the audit turned up; those are **not** part of the
immediate scope but are documented so we do not have to re-derive
them next iteration.

* Date written: 2026-05-06
* Baseline build: `fpga/build/mudkip2d128small1bram1chain2lanescale-20260506-055305/`
* Target device: KV260 (`xck26-sfvc784-2LV-c`)

> **Required background reading before touching any of this RTL:**
> [`fyp_doc/2d_fabric_handoff.md`](2d_fabric_handoff.md) — explains
> the 2D-fabric programming model, why the spike difftest checker is
> *not* a source of truth for this design, and what the
> programmer-side rules (R1–R8) are. Without § 1 (the big idea) and
> § 4.1 (reductions / `vl=1` stores) of that doc, the test-pass /
> test-fail signal in § 6.2 below will look mysterious. Also worth
> a skim before editing LaneMul / LaneDiv: § 5.1 (Hardware
> components) lists where in the Scala tree each block sits and how
> the time-multiplex row-FSM frames every instruction.

---

## 1. Problem

```
LUTs : 119 868 / 117 120  (102.4 %)
FFs  : 144 543 / 234 240  ( 61.7 %)
DSPs :       7 / 1 248    (  0.6 %)  ← almost untouched
BRAM :   53/3  / 144      ( ~37 %)
URAM :       1 / 64
```

The design will not place. We need ~3 k LUTs to fit and ideally
~10 k LUTs of headroom. The worst-case setup path is also *inside
the multiplier*, so fixing the LUT bloat there pays twice.

User-specified constraints:

* Do not change `laneScale` or any lane-count knob.
* Do not delete the existing `LaneMul` / `LaneDiv` Scala. Adding
  new files is fine, but the original code stays intact and bit-
  identical so we can A/B-test against it.
* Two switches required:
  * `run-test.sh -c <config>` must be able to choose the original
    or the new module set, so we can compare simulation results.
  * `build_rtl.sh -c <config>` must do the same so the FPGA flow
    can pick which RTL it emits.
* DSP / URAM are essentially free here; spend them.

---

## 2. Baseline measurements

### 2.1 Per-lane LUT breakdown (one of two lanes)

| Sub-module                               | LUTs   | FFs   | Notes                                           |
|------------------------------------------|--------|-------|-------------------------------------------------|
| `multiplier` (`LaneMul`)                 |  6 456 |   351 | **Hot spot, no DSPs**                            |
| `maskStage` (`MaskExchangeUnit`)         |  3 372 | 5 859 | Cross-lane slide/gather glue                     |
| `stage1` (VRF read pipes + xRead)        |  2 477 | 5 774 |                                                 |
| `executionUnit` (`LaneExecutionBridge`)  |  2 273 | 2 867 |                                                 |
| `divider` (`LaneDiv`)                    |  1 207 |   320 | **96 % is `SRTWrapper`/`SRT16` (1167)**          |
| `adder` (`LaneAdder`)                    |    917 |   228 |                                                 |
| `other` (`OtherUnit`)                    |    578 |   191 |                                                 |
| Distributors / arbiters / shifter        |  ~1.4 k|       |                                                 |
| (lane shell)                             |    507 | 1 034 |                                                 |
| **Lane total**                           | 20 234 | 20 124|                                                 |

### 2.2 Cross-lane shared blocks within `T1`

| Module                | LUTs   | FFs    | DSPs | Notes                                  |
|-----------------------|--------|--------|------|----------------------------------------|
| `maskUnit2D_0`        | 24 380 | 41 280 |    0 | 86 % of FFs come from one Reg-array (§ 7.1) |
| `lsu2D_0`             | 21 798 | 24 521 |    6 | All current DSPs (address calc)         |
| `sharedVRF2D_0`       |  9 734 |    891 |    0 | 8 banks, 32 RAMB36 — odd asymmetry (§ 7.2) |
| `replayFSM`           |  1 376 |     14 |    0 |                                        |
| `(T1 shell)`          |    242 |  6 600 |    0 |                                        |

### 2.3 Critical path (synth, slow corner)

```
Source       : laneVec2D_0_0/multiplier/requestReg_opcode_reg[1]/C
Destination  : laneVec2D_0_0/multiplier/request_pipeResponse_pipe_b_data_reg[16]/S
Slack (MET)  :  +5.700 ns  (target 16.666 ns @ 60 MHz)
Data delay   : 10.551 ns   (logic 3.94 ns + route 6.61 ns)
Logic levels : 34   (CARRY8=2 LUT2=2 LUT3=2 LUT4=5 LUT5=5 LUT6=18)
```

Path: `opcode_reg → mul1InputSelect → fusionMultiplier/ax00_mul16 →
ax01_layerOut_csa42 → result16_layer1_csa42 → adder64/tree8 →
tree16 → saturation → roundResultForSew16`. Every node is a soft
LUT.

---

## 3. Why Vivado did not auto-infer DSPs

Vivado XST infers DSP48E2 from `*` operator expressions. The T1
multiplier never uses `*`. `t1/src/vfu/VectorMultiplier32Unsigned.scala`
builds partial products by hand:

```scala
def make16BitsPartialProduct(a: Tuple2[Bool, Int], in: UInt): UInt = {
  val exist   = Mux(a._1, in, 0.U(16.W))
  val doShift = a._2 match {
    case 0 => exist
    case c => Cat(exist, 0.U(c.W))
  }
  doShift
}
```

— per-bit `AND` + shift, fed into a custom `CSA42` Wallace tree
that emits *redundant* `(sum, carry)` not finalised until the
surrounding `LaneMul` runs them through addend-correction CSAs and
a 64-bit `VectorAdder64`. Vivado has nothing to recognise.

Synth log confirms 7 DSPs total (HLS frmbuf, MIPI line buffer, LSU
addr-calc); zero in T1 lanes. No "DSP threshold" warnings — the
inferer never even saw a candidate.

---

## 4. Step 1 — Stub the divider via a NEW Scala file

### 4.1 Goal

Reclaim ~2.4 k LUTs (`SRTWrapper` + `SRT16` + `Abs` + leading-zero
counter). Division ops still **decode and complete** (no trap), but
return zero. Re-enabling later is a one-line preset switch + RTL
re-emit. No edits to `t1/src/LaneDiv.scala`.

### 4.2 Why simply removing `LaneDiv` from `genVec` deadlocks

`SlotExecuteRequest` (`t1/src/VectorFunctionUnit.scala:624`) builds
the per-slot Decoupled bundle by name from
`genVec.filter(_._2.contains(slotIndex))`. If we drop `LaneDiv`,
the slot loses its `"divider"` element. `instantiateVFU` in
`package.scala:352–365` then folds every present element's `fire`
into `executeEnqueueFire(slotIndex)`. With no divider element, a
slot whose decoded `Decoder.divider` bit is high raises
`requestValid(slotIndex)` but never asserts `requestFire` — the
slot deadlocks for the lifetime of the instruction.

So we must keep a module that publishes
`decodeField = Decoder.divider` and honours the request/response
handshake; only the *body* may be stubbed.

### 4.3 New file: `t1/src/LaneDivStub.scala`

```scala
package org.chipsalliance.t1.rtl

import chisel3._
import chisel3.experimental.hierarchy.{instantiable, Instance, Instantiate}
import chisel3.experimental.{SerializableModule, SerializableModuleParameter}
import org.chipsalliance.stdlib.GeneralOM
import org.chipsalliance.t1.rtl.decoder.{BoolField, Decoder}

object LaneDivStubParam {
  implicit def rw: upickle.default.ReadWriter[LaneDivStubParam] =
    upickle.default.macroRW
}
case class LaneDivStubParam(datapathWidth: Int, latency: Int)
    extends VFUParameter with SerializableModuleParameter {
  val decodeField: BoolField = Decoder.divider          // SAME field -> slot routing unchanged
  val inputBundle  = new LaneDivRequest(datapathWidth)  // reuse the bundles from LaneDiv.scala
  val outputBundle = new LaneDivResponse(datapathWidth)
  override val NeedSplit:   Boolean = true              // mirror LaneDivParam contract
  override val singleCycle: Boolean = false
}

class LaneDivStubOM(parameter: LaneDivStubParam)
  extends GeneralOM[LaneDivStubParam, LaneDivStub](parameter)

@instantiable
class LaneDivStub(val parameter: LaneDivStubParam)
    extends VFUModule with SerializableModule[LaneDivStubParam] {
  val omInstance: Instance[LaneDivStubOM] = Instantiate(new LaneDivStubOM(parameter))
  val response:      LaneDivResponse = Wire(new LaneDivResponse(parameter.datapathWidth))
  val responseValid: Bool            = Wire(Bool())
  val request:       LaneDivRequest  = connectIO(response, responseValid).asTypeOf(parameter.inputBundle)

  // Honour the multi-cycle handshake: always ready, complete in one cycle, return 0.
  vfuRequestReady.foreach(_ := true.B)
  responseValid          := requestRegValid
  response.executeIndex  := RegEnable(request.executeIndex, 0.U, vfuRequestFire)
  response.busy          := false.B
  response.data          := 0.U
}
```

Note: `LaneDivRequest` and `LaneDivResponse` are already declared
in `LaneDiv.scala`; the stub re-uses the bundles to keep the wire-
level interface bit-identical to the real divider.

### 4.4 Wire it into a new VFU preset

Add to `t1/src/VectorFunctionUnit.scala` next to `minimalInt`:

```scala
def minimalIntFpga(vLen: Int, dLen: Int, requestSourceSize: Int, laneScale: Int) =
  minimalInt(vLen, dLen, requestSourceSize, laneScale).copy(
    divModuleParameters = Seq(
      ( SerializableModuleGenerator(
          classOf[LaneDivStub],
          LaneDivStubParam(32, 1)),
        Seq.tabulate(requestSourceSize)(i => i)
      )
    )
    // mulModuleParameters: see § 5.4 — replaced in the same preset
  )
```

And extend the dispatch:

```scala
def parse(...) = preset match {
  case "minimal"     => ...
  case "minimalFpga" => (fp, zvbb) match {
                          case (false, false) => minimalIntFpga(...)
                          case _              => /* fall back or throw */
                        }
  case "small"       => ...
  ...
}
```

Also extend the `genVec` type union in
`VFUInstantiateParameter`:

```scala
divModuleParameters: Seq[
  ( SerializableModuleGenerator[_ <: VFUModule, _ <: VFUParameter],
    Seq[Int]
  )
],
```

— or keep `LaneDiv` typed and add a second list `divStubModuleParameters`,
folded into `genVec` the same way. Either shape works; the union
shape is simpler and only touches one type signature.

### 4.5 Expected savings

```
LaneDiv (current): 1 207 LUTs, 320 FFs / lane (1 167 LUTs in SRT)
LaneDivStub      :   ~5 LUTs, ~few FFs   / lane
```

Across two lanes: **~2 400 LUTs reclaimed**, ~620 FFs reclaimed.

### 4.6 Risks

* Any kernel that issues `vdiv*` / `vrem*` will silently get 0.
  Acceptable for the bringup path (smoke kernels do not divide);
  **must** be flagged in the driver. Suggest exposing a CSR bit
  ("divider stubbed") so `libt1` can refuse to load kernels that
  need it.
* `Decoder.divider` is also referenced in
  `Bundles.scala:84` (`ExecutionUnitType.divider`). The stub keeps
  that bit set and routes through the same dispatch path, so the
  T1-level scoreboard logic in `package.scala` lines 334–449 needs
  no changes. Verified by reading.

---

## 5. Step 2 — DSP-mapped multiplier via a NEW Scala file

### 5.1 Goal

Replace the per-SEW partial-product tree with simple `*` expressions
that Vivado will infer into DSP48E2 slices. Preserve the
`(multiplierSum, multiplierCarry, z)` interface so `LaneMul.scala`
needs zero edits — we just point its `Instantiate(...)` call at the
new module via a new VFU preset.

### 5.2 DSP budget per multiplier instance

| SEW | Multiplies | DSP48E2 used                                  |
|-----|------------|------------------------------------------------|
| 8   | 4 × (8 b × 8 b → 16 b)              | 4 (each DSP runs 8×8 — heavy underuse, but free) |
| 16  | 2 × (16 b × 16 b → 32 b)            | 2 (native fit, AB ports ample)                  |
| 32  | 1 × (32 b × 32 b → 64 b)            | 4 via 2×2 expansion (al·bl, al·bh, ah·bl, ah·bh)|

Worst case: 4 + 2 + 4 = **10 DSPs / `VectorMultiplier32Unsigned`
instance**. With `laneScale = 2` (so 2 instances per lane) and 2
lanes, that is **40 DSPs** for the whole T1 multiplier path. KV260
has 1248 DSP48E2; we are spending ~3.2 % of the pool to lift ~10–
12 k LUTs.

### 5.3 New file: `t1/src/vfu/VectorMultiplier32UnsignedDSP.scala`

```scala
package org.chipsalliance.t1.rtl.vfu

import chisel3._
import chisel3.experimental.hierarchy.{instantiable, public}
import chisel3.util._

@instantiable
class VectorMultiplier32UnsignedDSP extends Module {
  @public val a   = IO(Input(UInt(32.W)))
  @public val b   = IO(Input(UInt(32.W)))
  @public val sew = IO(Input(UInt(3.W)))
  @public val z               = IO(Output(UInt(64.W)))
  @public val multiplierSum   = IO(Output(UInt(64.W)))
  @public val multiplierCarry = IO(Output(UInt(64.W)))

  // Three independent multiplier shapes — Vivado infers a separate
  // DSP for each `*`. They all run in parallel; sew picks the
  // result. Total: 4 + 2 + 4 = 10 DSPs.
  val p8: Vec[UInt] = VecInit(Seq.tabulate(4) { i =>
    (a(8*i+7, 8*i) * b(8*i+7, 8*i)).asUInt    // 8×8 → 16
  })
  // SEW=8 result lays four 16-bit products end-to-end.
  val out8: UInt =
    Cat(p8(3)(15, 8), p8(3)(7, 0),
        p8(2)(15, 8), p8(2)(7, 0),
        p8(1)(15, 8), p8(1)(7, 0),
        p8(0)(15, 8), p8(0)(7, 0))

  val p16: Vec[UInt] = VecInit(Seq.tabulate(2) { i =>
    (a(16*i+15, 16*i) * b(16*i+15, 16*i)).asUInt   // 16×16 → 32
  })
  val out16: UInt = Cat(p16(1), p16(0))

  val out32: UInt = (a * b).asUInt   // 32×32 → 64; XST turns this into 4 DSPs

  val z32: UInt = Mux1H(sew(2,0), Seq(out8, out16, out32))

  // LaneMul wants the result in CSA-redundant form.
  // Degenerating `(z, 0)` keeps the downstream addend-correction
  // CSA + VectorAdder64 semantics intact.
  multiplierSum   := z32
  multiplierCarry := 0.U
  z               := z32
}
```

If Vivado is conservative we can bolt
`(* use_dsp = "yes" *)` onto the module via a Verilog wrapper
emitted by FIRRTL (the project already uses similar attributes in
the `mark_debug` pattern); cheap test is to try without first.

### 5.4 Wire into the new preset

Extend the `minimalIntFpga` preset from § 4.4 to also swap the
multiplier. Easiest shape: a thin `LaneMulDSP` wrapper that
inherits from `LaneMul` but instantiates the DSP-mapped inner
multiplier instead. Since LaneMul itself already calls
`Instantiate(new VectorMultiplier32Unsigned)` (line 94), the cleanest
new file is:

`t1/src/LaneMulDSP.scala`

```scala
package org.chipsalliance.t1.rtl

import chisel3._
import chisel3.experimental.hierarchy.{instantiable, Instance, Instantiate}
import chisel3.experimental.{SerializableModule, SerializableModuleParameter}
import org.chipsalliance.t1.rtl.vfu.VectorMultiplier32UnsignedDSP

object LaneMulDSPParam {
  implicit def rw: upickle.default.ReadWriter[LaneMulDSPParam] =
    upickle.default.macroRW
}
case class LaneMulDSPParam(eLen: Int, latency: Int, laneScale: Int)
    extends VFUParameter with SerializableModuleParameter {
  /* reuse all of LaneMulParam's fields verbatim */
  val datapathWidth: Int = eLen * laneScale
  val respWidth:     Int = datapathWidth
  val sourceWidth:   Int = datapathWidth
  val decodeField        = org.chipsalliance.t1.rtl.decoder.Decoder.multiplier
  val inputBundle  = new LaneMulReq(datapathWidth)
  val outputBundle = new LaneMulResponse(this.asInstanceOf[LaneMulParam])
}
```

Then either:
* (a) Subclass `LaneMul` and override the single `Instantiate(new
  VectorMultiplier32Unsigned)` line, or
* (b) Copy `LaneMul.scala` body verbatim into a new `LaneMulDSP`
  class and change just that one line.

(b) is uglier but avoids any subclassing surprise from the
hierarchy macros. Either way the **only** real-code difference is
*one line* — line 94 of LaneMul.scala. Everything else (sign
handling, addend correction, rounding, saturation) is reused.

The CSA-correction in lines 138–159 of `LaneMul.scala` does
`csa32(sumSelect, carrySelect, csaAddInput)` which collapses
correctly when `carry = 0` to a normal 2-input add, so the
multiply-add path stays semantically identical.

### 5.5 Expected savings

* Per `VectorMultiplier32Unsigned`: ~5 500 LUTs → ~50 LUTs of muxing.
* 2 instances per lane × 2 lanes = **~22 k LUTs lifted** on paper,
  conservatively **~10–12 k LUTs after surrounding mux/correction
  logic stays.**
* DSPs: 7 → ~47.
* Worst-case path no longer terminates inside `multiplier/`. New
  bottleneck likely shifts to `maskUnit2D_0` or
  `LaneExecutionBridge`.

### 5.6 Risks

* **Sign handling.** `LaneMul` does its own absolute-value
  conversion (`Abs32`) and re-applies the sign at the CSA stage
  via `negativeBlock`. The DSP receives unsigned inputs, so the
  sign-restore path stays correct. Verified by reading
  `LaneMul.scala` lines 75–119.
* **Saturation/round.** `roundResultForSew*` reads `adder64.z`
  (the resolved product). With `(sum=z, carry=0)` propagated
  through `VectorAdder64`, `z` is unchanged. Verified at lines
  199–251.
* **`vwmacc` widening.** Each lane slot gets its own
  `LaneMulRequest`, so widening MAC is not affected. Confirmed
  against `Bundles.scala:843`.
* **Latency.** `LaneMulParam(32, 2, laneScale=2)` → latency=2.
  `Pipe(...)` in `VFUModule.connectIO` adds two cycles outside the
  multiplier, so the DSP can run combinational from
  `requestReg.bits` to `multiplierSum`. At 60 MHz that is fine
  (DSP48E2 is rated > 700 MHz). If we ever push the clock past
  ~250 MHz we will need to register inside the multiplier and
  shrink LaneMul's external pipe by one stage.

---

## 6. Switch plumbing — `run-test.sh` and `build_rtl.sh`

Both scripts already accept `-c <config>` and resolve it via
`designs/org.chipsalliance.t1.elaborator.t1.T1.toml`. We do **not**
need to edit either bash script — just add a second TOML entry.

### 6.1 New TOML entries

Append to `designs/org.chipsalliance.t1.elaborator.t1.T1.toml`:

```toml
# Original — keep unchanged for A/B comparison
[mudkip2d128small1bram1chain2lanescale]
cmdopt = "--dLen 128 --extensions zvl256b --extensions zve32x \
          --laneScale 2 --chainingSize 1 --vrfBankSize 1 \
          --vrfRamType p0rw --vfuInstantiateParameter minimal \
          --rowNumber 1"

# NEW — same shape, FPGA-friendly multiplier + stubbed divider
[mudkip2d128small1bram1chain2lanescale_fpga]
cmdopt = "--dLen 128 --extensions zvl256b --extensions zve32x \
          --laneScale 2 --chainingSize 1 --vrfBankSize 1 \
          --vrfRamType p0rw --vfuInstantiateParameter minimalFpga \
          --rowNumber 1"
```

The `_fpga` suffix is the only difference; both presets accept
exactly the same instructions, so test ELFs are interchangeable
(provided the kernel doesn't divide).

### 6.2 Side-by-side test

> **No `--check` flag.** This is a 2D-fabric design — the
> architectural spike model used by the offline checker has no
> notion of vertical-mode `vse` / time-multiplexed reductions /
> dual-axis LSU, so the difftest log will always diverge even on
> a correct run. Pass / fail must come from the **online check
> only**: the C kernel itself prints `[== SUCCESS ==]` or `[==
> FAILURE ==]` (or asserts and traps) inside the simulator, and
> `run-test.sh` mirrors that to the run.log. Treat
> `sim_result.json` `"success": true` plus the kernel's own
> success print as the source of truth. See
> `fyp_doc/2d_fabric_handoff.md` § 1 for the modal-CSR explanation
> and § 4.1 for the specific spike divergence on reductions.

```sh
# Run against the original config, online check only:
./run-test.sh vision_task.benchmark_vadd \
    -c mudkip2d128small1bram1chain2lanescale \
    -e verilator-emu

# Run against the FPGA config; the in-kernel print should match:
./run-test.sh vision_task.benchmark_vadd \
    -c mudkip2d128small1bram1chain2lanescale_fpga \
    -e verilator-emu
```

Compare the two `run.log` files (or just the kernel-emitted
`[PERF]` / pass-fail lines) — they should be identical for any
test that does not divide. If they diverge, the new module set is
breaking semantics; bisect by reverting one of the two new files
at a time.

A useful smoke matrix to run before declaring the FPGA preset
green (each entry uses an in-kernel pass/fail print that the
run-log already captures):

| Kernel                                    | What it stresses                       |
|-------------------------------------------|-----------------------------------------|
| `vision_task.simple_instruction_asm`      | `vmul.vv` + macc — main DSP path        |
| `vision_task.benchmark_vadd`              | adder + LSU                             |
| `vision_task.benchmark_instructions`      | all per-instruction microbenchmarks     |
| `vision_task.simple_instruction_vert_lsu` | vertical-mode LSU                       |
| `vision_task.simple_vadd`                 | tightest sanity check                   |

Skip any test that issues `vdiv*` / `vrem*` while the divider is
stubbed.

### 6.3 RTL build switch

```sh
./build_rtl.sh -c mudkip2d128small1bram1chain2lanescale       # original
./build_rtl.sh -c mudkip2d128small1bram1chain2lanescale_fpga  # DSP + stub
```

Output goes to
`test_output/<config-name>/rtl-<timestamp>/result/`.
`fpga/system/build_fpga.sh` then takes that as input via `-r` (or
auto-finds the latest); existing flow already routes by config
name so no change is needed beyond pointing it at the new dir.

---

## 7. Audit of remaining FPGA-resource opportunities

User asked for a sweep of *anything else* the current design
leaves on the table while keeping the config and `laneScale`
unchanged. Findings ordered by payoff.

### 7.1 `MaskUnit.v0Vec` — **biggest single win after § 4 / § 5**

`t1/src/mask/MaskUnit.scala:194–198`:

```scala
val v0Vec: Vec[Vec[UInt]] = RegInit(
  VecInit(Seq.fill(parameter.timeMultiplexBatch)(
    VecInit(Seq.fill(parameter.vLen / parameter.datapathWidth)(0.U(parameter.datapathWidth.W)))
  ))
)
```

For our preset (`vLen=256`, `datapathWidth=64`, `timeMultiplexBatch
= targetElementNum / numRows = 128 / 1 = 128`), this is
**128 × 4 × 64 = 32 768 flip-flops**, or 86 % of the MaskUnit
shell's 37 970 FFs. The associated read mux (`v0Vec(gatherRowCounter)`,
line 199) is a 128:1 mux of 256-bit data — that is an explicit
chunk of the ~6 700-LUT shell logic.

**Optimisation**: convert `v0Vec` to a `SyncReadMem(128,
UInt(256.W))`. Vivado infers a single BRAM18 (128 × 256 bits =
4 KB; one BRAM36 supports 64 × 512-bit, comfortably enough). Cost
of the change:

* The current read at line 199 is *combinational*; `SyncReadMem`
  is one-cycle. Either (a) add a one-cycle pipeline register to
  the consumers of `v0`, or (b) keep `v0Vec` as a register array
  but add `(* ram_style = "block" *)` (Vivado's inferer will not
  promote a `RegInit(...)` array, so this almost certainly does
  not work — option (a) is the real path).
* Need a write-forward bypass for the case "the mask written this
  cycle is consumed next cycle" — read `MaskUnit.scala`
  line ~263 forward to confirm; the comment at line 192 says "row
  127's mask visible to the following instruction" which suggests
  inter-instruction visibility, not same-cycle bypass, so this is
  probably free.

Expected savings: ~32 k FFs (reduces to ~5 k after surrounding
register and bypass overhead) and several thousand LUTs of mux
logic. **Plausibly ~3–5 k LUTs.** This is the single biggest
remaining lever the user has not ruled out.

This is a non-trivial structural change (write side, read latency,
forwarding). Suggest tackling **after** § 4 and § 5 land and the
bitstream is closing on resources — only if we still need
headroom.

### 7.2 `LSU.writeQueueVec` — **second-biggest remaining lever**

`t1/src/lsu/LSU.scala:349–351` declares one `writeQueueVec` per
lane:

```scala
val writeQueueVec: Seq[QueueIO[LSUWriteQueueBundle]] = Seq.fill(param.laneNumber)(
  Queue.io(new LSUWriteQueueBundle(param), param.toVRFWriteQueueSize, flow = true)
)
```

`LSUWriteQueueBundle` (`Bundles.scala:437`) wraps a
`VRFWriteRequest` (vd 5 b + offset ~2 b + mask 8 b + data 64 b +
last 1 b + instructionIndex ~3 b + narrowVertical 1 b +
rowOverride 7 b ≈ 91 b) plus `targetLane` (laneNumber=2 b), so
**~93 bits per entry**.

For our preset:

* `laneNumber = 2`
* `toVRFWriteQueueSize = 96` — **hardcoded** in
  `t1/src/T1.scala:385` with the comment `// TODO: make it
  configurable for each lane`.

Total: **2 × 96 × 93 ≈ 17 856 flip-flops**. That alone explains
the lsu shell's 16 693 FFs (the rest of the shell registers cover
the difference). It also costs ~3 k LUTs of fanout / mux around
the queue (the lsu shell shows 4 988 LUTs, several of those go
here).

Why 96 is wildly oversized for our system:

* `lsuMSHRSize = 3` (`t1/src/T1.scala:268`) — at most 3
  outstanding TileLink/AXI cache-line transfers.
* `lsuTransposeSize = 16 B` (`T1.scala:274`), so 3 in-flight
  cache lines × 16 B / (datapath = 8 B) = **6 worst-case
  pending VRF writes per lane**, not 96.
* `sourceQueueSize = min(32, vLen×8 / lsuTransposeSize×8) = 16`
  (`LSU.scala:71`) — the source-tag tracker is already 16. So the
  rest of the LSU is sized for ≤16 in-flight, but this one queue
  is at 96.

Two ways to claw back FFs (do whichever first; they compose):

1. **Trim the depth.** Drop `toVRFWriteQueueSize` from 96 to 16
   (or 32 for a safety margin). Single-line edit at
   `T1.scala:385`. Expected savings: 2 × (96 − 16) × 93 = **~14
   880 FFs**, plus some mux LUTs. No correctness risk so long as
   the new depth ≥ `lsuMSHRSize × ceil(lsuTransposeSize ×
   8 / datapathWidth) + sourceQueueSize` ≈ 22; pick 32 to be
   safe.
2. **Back the queue with BRAM/SRL.** A Chisel `Queue(..., flow =
   true)` synthesises as a register array — no BRAM inference.
   For deep queues, swap to a `SyncReadMem`-based FIFO (XPM
   FIFO style) or convert the underlying `Reg(Vec(...))` with
   an `(* shreg_extract = "yes" *)` SRL annotation to put the
   storage in LUTRAM (SRL16E packs 16 deep × 1 bit per LUT). At
   16-deep × 93 b × 2 lanes, SRL inference would cost 2 × 93 ÷
   1 (1 SRL16E per bit) = ~186 LUTRAMs and zero FFs for storage.

The pragmatic order is **(1) first** because it is a one-line
edit with a measurable resource delta and no architectural risk;
do (2) only if we end up needing the queue deep again later.

Expected combined savings: **~14 k FFs and ~1–2 k LUTs**, just
from making `toVRFWriteQueueSize` honest. Trivial change; should
go in the same patch as § 4 / § 5.

### 7.3 `SharedVRF` per-bank LUT asymmetry

The 8 SRAM bank instances in `sharedVRF2D_0` use 1 426 / 116 /
1 778 / 117 / 2 037 / 119 / 3 449 / 123 LUTs respectively. Even-
indexed banks pay an order of magnitude more LUTs than odd-indexed
ones, despite all eight using the same `sram_0R_0W_2RW_8M_2048x64`
macro and the same 4 RAMB36 slots. The most likely cause is
asymmetric write-side conflict resolution / bank-correct logic in
`t1/src/vrf/SharedVRF.scala` (line 285+ shows hoisted
`anyNarrowReq` glue that only applies to the wide-vertical path).
Total per-VRF wrapper overhead: **~9.6 k LUTs**.

This is harder to optimise without reshaping bank topology and is
on the chaining hazard / verticalMode critical path. Suggest
parking — flag for a deeper review only if § 4 / § 5 / § 7.1 do
not yield enough room.

### 7.4 SRL inference for cross-lane / read-pipe shifters

The synth report shows 671 SRLs total in the design but **0**
inside any T1 lane / mask / lsu module. Likely candidates:

* `MaskExchangeUnit` per lane: 5 859 FFs / 3 372 LUTs. Used for
  the cross-lane slide / gather pipelines.
* `LaneStage1` shell per lane: 4 189 FFs / 1 514 LUTs. Cross-read
  (`crossReadUnitOp`) and read-pipe pacing.
* Total per-lane shifter-style FFs: ~10 k FFs × 2 lanes ≈ 20 k
  FFs that are *fixed-latency RegEnable chains*, exactly the
  pattern an SRL16E absorbs.

Two cheap experiments:

* Convert the longer `RegEnable` chains to
  `Queue(..., useShiftReg = true)` (the chisel-stdlib Queue
  honours that hint), OR
* Add `(* srl_style = "srl" *)` annotations through a FIRRTL
  attribute pass on the underlying `Reg(Vec(...))`.

Even at 50 % conversion this could move ~5–10 k FFs and ~200–
500 LUTs into LUTRAM SRL16E — modest but cumulative with the
above.

Defer until we know whether we still need it after § 4 / § 5
/ § 7.1 / § 7.2.

### 7.5 LSU multiplications already use DSPs

Confirmed by reading `t1/src/lsu/SimpleAccessUnit.scala`:

* Line 577: `(groupIndex ## nextElementForMemoryRequestIndex) * lsuRequestReg.rs2Data`
* Line 586: `offsetForUnitStride * dataWidthForSegmentLoadStore`

These are the 4 DSPs in `otherUnit`. Adding an explicit
`(* use_dsp = "yes" *)` will not unlock more.

`LoadUnit.scala:190` has a small `accessPtr * segmentInstructionIndexInterval`
— operands are only a few bits, so DSP would be wasteful. Skip.

### 7.6 `MaskCompress` prefix-sum

`t1/src/mask/MaskCompress.scala:103–108` runs a 16-deep prefix-
sum of mask bits, ~8 k LUTs, no DSPs. DSP48E2 SIMD ALU mode (4 ×
12-bit add) could absorb this at the cost of a hand-tuned SIMD
adder tree. Not a quick win — skip unless we are out of options.

### 7.7 Vendor `smartconnect_hb` (4 254 LUTs / 6 334 FFs / 812 LUTRAMs)

The high-bandwidth AXI smartconnect (T1 hb master + DMA mm2s +
DMA s2mm → PS HPC0) is the heaviest non-T1 IP. We do not need
all of its inferred infrastructure:

* `STRATEGY = "MINIMUM_AREA"` on the smartconnect IP often saves
  10–25 % LUTs by disabling speculative pipelining.
* `NUM_SI = 3` is correct (T1 hb + DMA mm2s + DMA s2mm), but the
  internal data-width converter chain has 812 LUTRAMs of FIFO
  buffer for ID renaming + cross-clock — most of that is xpm
  defaults, not strictly needed at our 60 MHz single-clock
  topology.
* All three slaves and the master are 128-bit at the same clock,
  so the smartconnect should ideally collapse to a 3:1 arbiter
  with no width conversion.

Set
`set_property STRATEGY {MINIMUM_AREA} [get_bd_cells smartconnect_hb]`
in `fpga/system/system_top.tcl` and re-run synth. Likely
savings: **~500–1 000 LUTs**, no functional change. Same trick
applies to `smartconnect_ctrl` (1 075 LUTs) and
`smartconnect_idx`.

### 7.8 Hold violations

`Hold : 53884 Failing Endpoints, Worst Slack -0.093 ns,
Total Violation -1224.086 ns`. This is a synth-stage report
artefact; place-and-route fixes hold by buffer insertion. No
action needed. Document so we do not chase it.

### 7.9 URAM (only 1/64 used)

URAM48 is 4096 × 72 bits = 36 KB per slice. Could the `SharedVRF`
swap its 32 RAMB36s for ~10 URAMs? **No** — URAM48 is single-
ported (1R + 1W simultaneous), and the VRF is `0R_0W_2RW` (two
read-write ports). The macro contract is incompatible. The line-
buffer in MIPI already uses 1 URAM (the one we see).

URAM is only useful here if we bring in a new large memory (e.g.
DDR-side scratchpad cache). Out of scope for this task.

---

## 8. Order of operations for the next session

1. **Snapshot baseline.** Don't delete
   `mudkip2d128small1bram1chain2lanescale-20260506-055305/` — per
   project standing rule on FPGA build artefacts.
2. **Add the two new Scala files** (`LaneDivStub.scala`,
   `VectorMultiplier32UnsignedDSP.scala`, plus the optional
   `LaneMulDSP.scala`).
3. **Add the `minimalIntFpga` preset** in
   `t1/src/VectorFunctionUnit.scala`.
4. **Add the `_fpga` TOML entry**.
5. **Re-emit RTL** for both presets:
   ```sh
   ./build_rtl.sh -c mudkip2d128small1bram1chain2lanescale
   ./build_rtl.sh -c mudkip2d128small1bram1chain2lanescale_fpga
   ```
6. **Verify simulation parity.** Run the matrix in § 6.2 against
   both configs (no `--check` — see the warning in § 6.2 about why
   the offline difftest is meaningless on a 2D-fabric design).
   Compare the two `run.log` files. Anything that fails on `_fpga`
   but passes on the original means the new modules diverge in
   semantics — bisect by reverting one new file at a time.
7. **Synth-only on `_fpga`**:
   ```sh
   ./fpga/system/build_fpga.sh -c mudkip2d128small1bram1chain2lanescale_fpga -s -a
   ```
   Read `utilization_synth.rpt` — expect total LUTs ~106 k (~91 %),
   DSPs ~47, multiplier removed from the worst-path list.
8. If LUTs are still over 100 %, walk § 7 in the order shown
   below — the cheap wins first, the structural rewrites last.
   Do **not** change `laneScale`.
   * **§ 7.2 first** (one-line edit, `toVRFWriteQueueSize 96 → 32`):
     ~14 k FFs and ~1–2 k LUTs for free.
   * **§ 7.7 next** (`STRATEGY = MINIMUM_AREA` on the three
     smartconnects, TCL-only): ~500–1 000 LUTs.
   * **§ 7.4** (SRL annotation on the long RegEnable chains in
     `MaskExchangeUnit` / `LaneStage1`): a few hundred FFs and
     LUTs. Quick FIRRTL-attribute pass.
   * **§ 7.1** (`v0Vec` → SyncReadMem) only if § 7.2 + § 7.7 +
     § 7.4 still leave us short. This is the only structural
     rewrite in the audit list and changes mask read latency.
9. Once synth fits, do a full `-b` run for the bitstream.

---

## 9. Pointers to source files

| Concern               | File                                                                 |
|-----------------------|----------------------------------------------------------------------|
| 2D-fabric programming model (REQUIRED reading) | `fyp_doc/2d_fabric_handoff.md`                  |
| Existing divider      | `t1/src/LaneDiv.scala` (LaneDiv + SRTWrapper + Abs)                   |
| Existing multiplier   | `t1/src/vfu/VectorMultiplier32Unsigned.scala`, `Multiplier16.scala`  |
| LaneMul integration   | `t1/src/LaneMul.scala`                                               |
| VFU preset selection  | `t1/src/VectorFunctionUnit.scala`                                    |
| Slot-side dispatch    | `t1/src/package.scala` lines 334–449 (`instantiateVFU`)              |
| Per-slot request bndl | `t1/src/VectorFunctionUnit.scala` lines 624–637 (`SlotExecuteRequest`)|
| Decode field def      | `t1/src/decoder/Decoder.scala` lines 58–64 (multiplier / divider)     |
| Elaborator CLI        | `elaborator/src/t1/T1.scala`                                         |
| TOML preset map       | `designs/org.chipsalliance.t1.elaborator.t1.T1.toml`                 |
| RTL build script      | `build_rtl.sh`  (auto-routes via `-c`)                                |
| Test runner           | `run-test.sh`  (auto-routes via `-c`)                                 |
| FPGA build orchestrator| `fpga/system/build_fpga.sh`                                          |
| Wrapper RTL gen       | `fpga/system/gen_wrapper.sh`                                         |
| `MaskUnit.v0Vec`      | `t1/src/mask/MaskUnit.scala:194` (audit § 7.1)                       |
| LSU `writeQueueVec`   | `t1/src/lsu/LSU.scala:349` + `t1/src/T1.scala:385` (audit § 7.2)     |
| SharedVRF asymmetry   | `t1/src/vrf/SharedVRF.scala:285+` (audit § 7.3)                      |
| Smartconnect IPs      | `fpga/system/system_top.tcl:137,146,169` (audit § 7.7)               |
| Baseline reports      | `fpga/build/mudkip2d128small1bram1chain2lanescale-20260506-055305/`  |

---

## 10. Implementation status

Updated 2026-05-06.

1. **Snapshot baseline** — Done. Existing FPGA build artefacts were left untouched.
2. **Add new Scala modules** — Done.
   * Added `t1/src/LaneDivStub.scala`.
   * Added `t1/src/vfu/VectorMultiplier32UnsignedDSP.scala`.
   * Added `t1/src/LaneMulDSP.scala`.
   * Existing `LaneDiv.scala`, `LaneMul.scala`, and
     `VectorMultiplier32Unsigned.scala` were not modified.
3. **Add `minimalIntFpga` preset** — Done.
   * `minimalFpga` selects `LaneDivStub` and `LaneMulDSP` together.
   * Direct elaborator switch: `--vfuInstantiateParameter minimalFpga`.
4. **Add `_fpga` TOML entries** — Done.
   * Use `mudkip2d128small1bram1chain2lanescale_fpga` for the combined
     divider-stub + DSP-multiplier build.
   * Added entries for `t1`, `t1emu`, and `t1rocketemu` so
     `build_rtl.sh -c ..._fpga` and `run-test.sh -c ..._fpga` route
     through the same preset.
5. **Re-emit RTL** — Done.
   * Original preset: `build_rtl.sh -c mudkip2d128small1bram1chain2lanescale`
     completed successfully in
     `test_output/mudkip2d128small1bram1chain2lanescale/rtl-20260506-211857/`.
   * FPGA preset: `build_rtl.sh -c mudkip2d128small1bram1chain2lanescale_fpga`
     completed successfully in
     `test_output/mudkip2d128small1bram1chain2lanescale_fpga/rtl-20260506-212101/`.
6. **Verify simulation parity** — Done for the § 6.2 non-divider matrix on
   `t1emu`, using `run-test.sh` with no `--check`.
   * `vision_task.simple_instruction_asm`: original and `_fpga` passed,
     both `total_cycles = 15367`.
   * `vision_task.benchmark_vadd`: original and `_fpga` passed, both
     `total_cycles = 543440`.
   * `vision_task.benchmark_instructions`: original and `_fpga` passed,
     both `total_cycles = 3650894`.
   * `vision_task.simple_instruction_vert_lsu`: original and `_fpga`
     passed, both `total_cycles = 25626`.
   * `vision_task.simple_vadd`: original and `_fpga` passed, both
     `total_cycles = 50633`.
   * `rg` found no `vdiv*` / `vrem*` instructions in the remaining matrix
     entries before running them against the divider-stub config.
7. **Synth-only `_fpga`** — Done.
   * `fpga/system/build_fpga.sh -c mudkip2d128small1bram1chain2lanescale_fpga -s -a`
     completed successfully in
     `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260506-212313/`.
   * `utilization_synth.rpt` top-level result: 107771 total LUTs,
     105052 logic LUTs, 143402 FFs, 53 RAMB36, 3 RAMB18, 1 URAM,
     31 DSP blocks.
   * T1 hierarchy result: 86405 total LUTs, 113064 FFs, 32 RAMB36,
     30 DSP blocks.
   * `LaneMulDSP` is present in both lanes and the report shows
     `VectorMultiplier32UnsignedDSP` instances using DSP blocks.
   * `timing_synth.rpt` reports setup clean at synthesis; hold/PW still
     show synthesis-stage violations in generated platform/IP paths.
8. **Apply cheap audit wins (§ 7.2 + § 7.4 + § 7.7)** — Done.

   These three were applied as a single follow-up pass on the
   `mudkip2d128small1bram1chain2lanescale_fpga-20260506-212313/`
   baseline (107 771 LUTs / 92 % — placement still tight).

   * **§ 7.2 — `toVRFWriteQueueSize` 96 → 32** at
     `t1/src/T1.scala:385`. Original `96` retained as a comment
     with the rationale and a note on when to restore. Expected
     savings: laneNumber × (96 − 32) × ~93 b ≈ **~14 k FFs** plus
     the surrounding mux LUTs.
   * **§ 7.4 — Queue `ram_style = "distributed"` annotation** at
     `stdlib/src/Queue.scala`. Imports `chisel3.util.addAttribute`
     and tags the multi-entry `ram` with the SystemVerilog
     attribute Vivado uses to map register arrays to LUTRAM
     instead of dedicated FFs. The annotation is emitted as
     `(* ram_style = "distributed" *)` in the firtool-generated
     SystemVerilog (see § 11 below for why this is reliable).
   * **§ 7.7 — `STRATEGY = MINIMUM_AREA` on every SmartConnect**
     at `fpga/system/system_top.tcl` (after the existing extend
     block, around line 460). Single `foreach` loop sets
     `CONFIG.STRATEGY {1}` on all five (`_hb`, `_idx`, `_ctrl`,
     `_lpd`, `_video`). Expected savings: ~500–1 000 LUTs across
     the smartconnect IPs.

   Combined expected delta over the 92 % baseline: **~3–5 k LUTs
   and ~14 k FFs**, mostly through smaller LSU shell and lighter
   smartconnect IPs.

   **Re-run required**: `build_rtl.sh -c
   mudkip2d128small1bram1chain2lanescale_fpga` and then
   `build_fpga.sh -c mudkip2d128small1bram1chain2lanescale_fpga
   -s -a`. Compare new `utilization_synth.rpt` against the
   `_fpga-20260506-212313/` baseline. Quick sim parity re-check
   (run-test against the `_fpga` config) is also warranted because
   the LSU queue depth changed — a kernel that was relying on >32
   in-flight LSU writes per lane could now stall.

9. **Escalate to `v0Vec` BRAM (§ 7.1) only if step 8 still leaves
   placement tight** — Pending; structural change to MaskUnit, do
   not start without measuring step 8 first.

10. **Full bitstream** — Pending; not run.

---

## 11. How `(* srl_style *)` / `(* ram_style *)` annotations actually reach Vivado

A reasonable question came up: *does FIRRTL get lost before
Vivado synth, so attribute annotations can't survive?* Short
answer: **no, they do survive**. The toolchain is:

```
Chisel (Scala)  ──>  FIRRTL (text)  ──>  firtool (CIRCT)  ──>  SystemVerilog  ──>  Vivado synth
```

Annotations are first-class citizens at every hop *up to* the
SystemVerilog emission. Specifically:

1. **Chisel side** — `chisel3.util.addAttribute(target, "...")`
   (see `core/src/main/scala/chisel3/util/AttributeAnnotation.scala`
   in the Chisel sources) calls `chisel3.experimental.annotate`
   with a `firrtl.AttributeAnnotation`. This attaches a
   `Named`-targeted annotation to the chisel3 elaboration result.
2. **FIRRTL side** — the annotation is serialised into the
   `*.json` annotation file alongside the `*.fir` text emitted by
   `chisel3` (or the bundled `.anno.json` block when emitted
   through CIRCT).
3. **firtool side (CIRCT)** — when `firtool` lowers FIRRTL to
   SystemVerilog, it walks each `AttributeAnnotation`, finds the
   target signal in the lowered netlist, and emits the SV
   `(* attr = "..." *)` syntax directly in front of the
   declaration. Crucially, firtool does *not* drop unknown
   attributes; it passes any string through verbatim, so vendor-
   specific hints like `ram_style`, `shreg_extract`, or
   `keep` reach the Vivado synth-source view unchanged.
4. **Vivado side** — Vivado's `synth_design` reads the
   SystemVerilog and treats `(* ram_style = "distributed" *)` as
   a directive on the immediately-following declaration, exactly
   as if a human had written it.

So a single line `addAttribute(ram, "ram_style = \"distributed\"")`
in `stdlib/src/Queue.scala` survives the full chain and lands as
`(* ram_style = "distributed" *)` in the netlist Vivado actually
synthesises.

### What the alternatives mean

Three closely related patterns get conflated; they are different
tools for different jobs:

| Pattern                                             | Storage primitive   | Access pattern needed             | When to use                                                |
|-----------------------------------------------------|---------------------|-----------------------------------|------------------------------------------------------------|
| `(* shreg_extract = "yes" *)`                       | SRL16E / SRL32      | strict shift-register (sequential) | Fixed-latency pipelines whose data is never re-addressed.  |
| `(* ram_style = "distributed" *)`                   | LUTRAM (RAM32M / RAM64M) | random access by index           | Small queues / regfiles addressed by head/tail pointer.    |
| `(* ram_style = "block" *)` or `SyncReadMem(...)`   | BRAM18 / BRAM36     | random access, ≥ 1-cycle read latency | Larger storage (hundreds of bytes upward), tolerant of pipeline read latency. |
| `chisel3.util.ShiftRegister(in, depth, en)`         | SRL16E / SRL32 (auto-inferred) | structural — Chisel emits a sequential chain | When you control the source; cleaner than annotation. |

The Chisel `Queue` in `dwbb.stdlib.queue` uses
`Reg(Vec(entries, gen))` with head/tail pointers (random access),
so SRL is **not** the right target for it — LUTRAM is. That is
why § 7.4 lands on `ram_style = "distributed"` and not
`shreg_extract`. The two are not interchangeable.

Where a bona-fide shift register exists (e.g. fixed-latency
cross-read pipes in `LaneStage1` or pipeline registers in
`MaskExchangeUnit`), the right move is to swap explicit
`RegEnable` chains for `chisel3.util.ShiftRegister(data, depth,
enable)`. firtool emits a sequential RegEnable chain that Vivado
auto-infers as SRL16E without any annotation. We have not done
this yet — flag for a follow-up if step 8 needs more headroom.

### Why not just use a Vivado XDC `set_property`?

Two reasons we kept the annotation Chisel-side rather than
pushing it into a `system_top.tcl`/XDC constraint:

1. **Naming.** firtool mangles signal names through several
   passes. A TCL constraint targeting `*ram_reg*` may match
   nothing, or worse, match the wrong array, after a re-emit.
   The Chisel-side annotation tracks the original signal through
   firtool's renaming and lands on the right cell.
2. **Reproducibility.** The hint becomes part of the RTL the
   `build_rtl.sh -c ..._fpga` flow emits. It survives a clean
   nix rebuild and is the same set of files the
   simulation flow uses (although Verilator ignores SV
   attributes, so simulation behaviour is unchanged).
