# vrgather.vx debug handoff

**Status:** resolved for the current `simple_instruction_gather_scalar`
coverage. The old row-0, per-row v0, and V-mode failures described below
are historical debug notes; the latest checked run
`test_output/mudkip2d128small1bram1chain2lanescale/vision_task.simple_instruction_gather_scalar-20260504-140644/`
passes all five checks.

## TL;DR

`vrgather.vx vd, vs2, t3` (rs1=5) is implemented in this fabric as **"vmv.v.x with rs1 = vs2[rs1] read by the MaskUnit"**: T1.scala:1128-1134 routes `maskUnit.io.gatherData.bits` into the lane request as `readFromScalar`. Since each hw-row's vs2 differs, the MaskUnit must issue **one read per hw-row** during the time-multiplex replay.

The bugs found in this read pipeline were:

1. **(FIXED)** Read for hw-row R+1 fires immediately after row R's `gatherData.fire`, while `rowCounter` is still pinned to R. Result: each hw-row consumed the previous hw-row's vs2[rs1]. Patch: park the MaskUnit gather FSM in a new `sWaitNextRow` state until `replayFSM.rowDone` pulses, AND gate `replayFSM.executionReady` on `gatherDataReadyForRow`. Confirmed in 2026-05-02 re-run: rows 1..127 are correct.

2. **(FIXED)** The very first `gatherRequestFire` used to pulse the moment
   `gatherRead` went high — i.e. one cycle after `io.issue.fire` for the
   gather. If the previous instruction's `replayFSM` was still running, this
   fired the row-0 gather read at the wrong `rowCounter` and returned
   stale/zero data. Current RTL gates the initial read with
   `gatherReplayBusy`/`gatherInstActive`; see §5.

3. **(FIXED)** Masked H-mode used a single shared v0 shadow, so the last
   replayed row's diagonal mask overwrote all prior rows. Current `MaskUnit`
   uses a per-hw-row `v0Vec` indexed by `gatherRowCounter`; see §6.

4. **(FIXED for this test)** V-mode unmasked and masked `vrgather.vx` now
   pass the scalar-C checker in `simple_instruction_gather_scalar`; see §7.

## 1. Test artifact

- Test: `tests/vision_task/simple_instruction_gather_scalar/simple_instruction_gather_scalar.c` — H/V × unmasked/diag-masked with `rs1=5`, plus an H-mode back-to-back unmasked regression (`rs1=5` then `rs1=9`).
- Latest passing run:
  `test_output/mudkip2d128small1bram1chain2lanescale/vision_task.simple_instruction_gather_scalar-20260504-140644/`
  - TEST 1: PASS `vrgather.vx (H) unmasked`
  - TEST 2: PASS `vrgather.vx (V) unmasked`
  - TEST 3: PASS `vrgather.vx (H) masked`
  - TEST 4: PASS `vrgather.vx (V) masked`
  - TEST 5: PASS `vrgather.vx (H) back-to-back`
  - `total_cycles`: 145606, `SIMULATION PASSED`
- Historical failing run used for the waveform notes below:
  `test_output/mudkip2d128small1bram1chain2lanescale/vision_task.simple_instruction_gather_scalar-20260502-103013/`
  - `run.log` showed TEST 1 (H, unmasked): row 0 = all 0, rows 1..127 correct. 128/16384 errors.
  - Trace: `wave.fst`, 210547 timesteps (1 cycle = 2 timesteps).

## 2. Architecture map

- vrgather.vx is in `isScheduler` (decoder/attribute/isScheduler.scala:191) and `isGather` (isGather.scala:22). It does **not** flow through `maskStage` — confirmed by waveform (every `maskStage.gather*` signal stays 0 the whole sim).
- T1.scala:891 asserts `gatherNeedRead := requestRegDequeue.valid && Decoder.gather && !Decoder.vtype` for the entire instruction lifetime in requestReg.
- T1.scala:1128-1134 selects `source1Select2D(row) := Mux(Decoder.gather, maskUnit2D(row).io.gatherData.bits, ...)` and feeds this to `request.bits.readFromScalar` (line 1184). The lane writes vd as if vmv.v.x.
- MaskUnit gather FSM (MaskUnit.scala:391-413) — pre-fix had 4 states {idle, sRead, wRead, sResponse}. Post-fix has a 5th `sWaitNextRow`.
- T1.scala:1413 wires `maskUnit.io.gatherData.ready := replayFSM.rowFire` — every rowFire pulse consumes the broadcast.

## 3. Fix already applied (Bug 1)

Current implementation:
- `t1/src/mask/MaskUnit.scala`: new IO `gatherRowDone: Input(Bool())`; new state `sWaitNextRow`; transition `sResponse → sWaitNextRow` on `gatherData.fire`; `sWaitNextRow → idle` on `gatherRowDone`.
- `t1/src/T1.scala`: `maskUnit.io.gatherRowDone := replayRowDone`; new wire `gatherDataReadyForRow := !gatherNeedRead || maskUnitGatherDataValid`; `executionReady` AND-ed with it.

Effect: the read for row R+1 cannot start until `rowCounter` has advanced to R+1, and `rowFire` for row R+1 cannot pulse until that read has returned. Rows 1..127 of TEST 1 verified correct after rebuild.

## 4. Bug 2 — row 0 initial gather read — FIXED

**Current status:** fixed. The failure description in this section is kept as
the original waveform diagnosis. Latest passing evidence:
`vision_task.simple_instruction_gather_scalar-20260504-140644/run.log`
reports `PASS vrgather.vx (H) unmasked`, and TEST 5 also passes the
back-to-back gather regression intended to catch stale `gatherInstActive`.

### 4.1 Symptom (run 20260502-103013, TEST 1)

```
Row 0: 0 0 0 ... 0
Row 1: 6 6 6 ... 6   (= grid_in[1][5], correct)
Row 2: 7 7 7 ... 7
Row 3: 8 8 8 ... 8
```

### 4.2 Wave evidence (1 cycle = 2 timesteps)

| Timestamp | Signal | Value | Meaning |
|---|---|---|---|
| t=10426 | `replayFSM.lastRowFire` | 0→1 | vle8.v starts its **last** row (rowCounter=127) |
| t=10426 | `requestReg.valid` | falls | vle8.v released from requestReg |
| t=10428 | `io.issue.fire` (inferred) | pulse | vrgather.vx accepted into T1 |
| t=10430 | `requestReg_bits_decodeResult_gather` | 0→1 | vrgather.vx visible |
| t=10430 | `requestReg_bits_issue_rs1Data` | →5 | rs1 captured |
| t=10432 | `maskUnit2D_0.gatherReadState` | 0→1 (sRead) | **first gatherRequestFire fired** |
| t=10432 | `maskUnit2D_0.gatherDatOffset` | 0→5 | dataOffset captured correctly |
| t=10434 | `gatherReadState` | 1→2 (wRead) | |
| t=10436 | `sharedVRF2D_0.lanePorts_0_readRequests_4_valid` | 0→1 | SRAM read fires, vs=8, lane port 4 (= maskUnit's port) |
| t=10436 | `lanePorts_0_readRequests_4_ready` | 1 | accepted |
| t=10444 | `gatherReadState` | 2→3 (sResponse) | response |
| t=10444 | `readVS1Reg.dataValid` | 0→1 | latches |
| t=10444 | `readVS1Reg.data` | 0 (no transition) | **read returned 0** |
| t=10474 (approx) | `replayRowDone` (vle8.v's last) | rises | |
| t=10476 | `replayFSM.busy` | 1→0 | vle8.v fully done; rowCounter resets to 0 |
| t=10482 | row 0's rowFire (vrgather) | pulse | lane consumes `gatherData.bits=0` |
| t=10484 | `replayFSM.busy` | 0→1 | vrgather replay starts |
| t=10694 | row 1's read fires | | rs2[5]=6 read |
| t=10698 | `lanePorts_0_readResults_4` | 0→`0x0807060504030201` | row 1's correct data |
| t=10700 | `readVs1AckFire_0` | rises | |
| t=10702 | `readVS1Reg.data` | 0→`0x80706` (after >>40 shift) | row 1's data captured |

### 4.3 Root cause (mechanical)

The **first** `gatherRequestFire` is governed only by `gatherReadState===idle && gatherRead && !instVlValid` (MaskUnit.scala:413). At t=10432 all three are true:
- maskUnit's state is idle (initial / never used).
- `gatherRead` just rose (vrgather.vx in requestReg).
- `instVlValid` is 0 (vrgather.vx is not a `Decoder.maskUnit` instruction; `instReq.valid` never pulses for it; the RegEnable that could set instVlValid never fires).

So the read fires at t=10432–10436. **At that time, `replayFSM.busy=1` and `rowCounter=127`** (vle8.v's last row, just started at t=10426 and not done until t=10476). The SRAM read therefore queries v8 at logicalAddr (vs=8, offset=0, lane=0) **with `rowCounter=127`**, not 0. v8 hw-row 127 hasn't been written yet by vle8.v at this exact cycle (vle8.v writes hw-row 127 last, around t=10450-10462), so the SRAM bank returns the still-zero default.

Two cycles later (vrfReadLatency=2) the response is captured into `readVS1Reg.data` as 0; gatherData.valid goes high; my `gatherDataReadyForRow` gate is satisfied; `requestRegDequeue.fire` pulses; `replayFSM.firstRowFire` for vrgather pulses at t=10482; the lane consumes 0 and writes vd=0 to all of hw-row 0.

Subsequent rows are unaffected because the new `sWaitNextRow` park forces them to wait until **vrgather's own** `rowDone` pulses. The first one slipped through because `sWaitNextRow` is entered only on `gatherData.fire`, not at instruction boundaries.

### 4.4 Why the H-mode pattern in `benchmark_instructions.c` looked different

That benchmark wrapped vrgather.vx in a 100-iter loop and printed row 0 = all 4. Reason: each loop iteration is a **new** vrgather.vx issue, so iter 2's row-0 gather fires while iter 1's row 127 is being processed. Iter 1's row 127 wrote v16's hw-row 127 (with stale data from the off-by-one bug). Hence iter 2's row 0 gathers `vs2[rs1=5]` from hw-row 127 = `(127+5)&127 = 4`. The single-iter test in this handoff has nothing populated in v16 hw-row 127 either, but the bug surface is different — the read returns 0 because v8 hw-row 127 hasn't been LOAD-written yet at the moment of the speculative read. Both are facets of the same root cause: the first gather read fires before the instruction is actually being replayed.

## 5. Implemented fix for Bug 2

**Goal:** the first `gatherRequestFire` of an instruction must wait until the *previous* instruction has fully released `replayFSM` (so `rowCounter` is the settled 0). Subsequent `gatherRequestFire`s within this instruction's replay must continue to use the existing `sWaitNextRow → idle on rowDone` path.

This fix is present in the current RTL:
- `MaskUnit.scala`: `gatherReplayBusy` IO, `gatherInstActive` register,
  and the initial-read gate.
- `T1.scala`: `maskUnit.io.gatherReplayBusy := replayFSM.busy`.

Track "have I fired at least once for this instruction" with a 1-bit register, then gate the initial firing on `!replayFSM.busy`:

```scala
// New IO on MaskUnit
val gatherReplayBusy: Bool = Input(Bool())   // <- replayFSM.busy

// New regs in body
val gatherInstActive: Bool = RegInit(false.B)
when(gatherRequestFire) { gatherInstActive := true.B }
when(!gatherRead)       { gatherInstActive := false.B }

// Modified gate
gatherRequestFire := gatherReadState === idle &&
                     gatherRead &&
                     !instVlValid &&
                     (gatherInstActive || !gatherReplayBusy)
```

Wiring in T1.scala beside existing `gatherRowDone`:
```scala
maskUnit.io.gatherReplayBusy := replayFSM.busy
```

Behavior:
- **Row 0 of a new instruction**: `gatherInstActive=0`, must wait for `!replayFSM.busy`. Fires only after previous instruction's lastRowDone has cleared `replayFSM` — at that moment `rowCounter=0`. ✓
- **Row R+1 within the same instruction**: `gatherInstActive=1`, fires whenever state returns to idle (i.e. just after `gatherRowDone` per the existing fix). `replayFSM.busy` is irrelevant. ✓
- **Across instruction boundary**: `!gatherRead` clears `gatherInstActive` between instructions, resetting the gate.

No deadlock: `replayFSM.busy` going low doesn't depend on `gatherData.valid` (it depends on the previous instruction's lane work finishing).

## 6. Bug 3 — shared v0 shadow lost per-row diagonals — FIXED

**Current status:** fixed. The old failure was: after the Bug 2 fix, TEST 1
passed but TEST 3 (H, diag mask) still failed because every hw-row wrote its
masked value at **col 127** instead of col r. Current RTL uses a per-hw-row
v0 shadow (`v0Vec`) indexed by `gatherRowCounter`, and the latest run reports
`PASS vrgather.vx (H) masked`.

### 6.1 Root cause

`MaskUnit.scala:178-181` declares a single v0 shadow register:

```scala
/** duplicate v0 for mask */
val v0: Vec[UInt] = RegInit(
  VecInit(Seq.fill(parameter.vLen / parameter.datapathWidth)(0.U(parameter.datapathWidth.W)))
)
```

This is one flat register — there is **no per-hw-row v0**. The kernel does `vmsne.vi v0, v20, 0` with `v20 = diag_buf` (where `diag_buf[r][c] = (r==c)`). Replayed across 128 hw-rows:

- Hw-row 0 writes: v0 has bit 0 set (rest 0).
- Hw-row 1 writes: v0 has bit 1 set (rest 0).
- ...
- Hw-row 127 writes: v0 has bit 127 set (rest 0).

Each row's writes overwrite v0 entirely (vmsne writes all elements 0..vl-1). So when the gather reads v0, it sees row 127's mask. Every row's gather hits col 127.

This was correctly spotted by codex; the wave run.log corroborates it (every row has the masked write at col 127).

### 6.2 Why H/V/masked/unmasked land where they do

| Test | Config | Historical status before fix | Current status |
|---|---|---|---|
| 1 | H, no mask | PASS | PASS |
| 2 | V, no mask | FAIL | PASS |
| 3 | H, diag mask | FAIL | PASS |
| 4 | V, diag mask | FAIL | PASS |

### 6.3 Implemented fix (Bug 3) — approach (a)

**Goal:** Replace MaskUnit's single shared `v0` shadow with a per-hw-row shadow indexed by `rowCounter`. All v0 reads and writes in MaskUnit go through that index. Cost in flops: `timeMultiplexBatch × vLen` = `128 × 256 = 32768` bits = 4 KiB per MaskUnit (one MaskUnit per row of the 2D fabric). Acceptable for current configs.

#### 6.3.1 Files to change

- `t1/src/mask/MaskUnit.scala` — main change.
- `t1/src/T1.scala` — wire `replayFSM.rowCounter` into the new MaskUnit IO.
- *No* changes needed in lanes, SharedVRF, V0Update bundle, or the lane→maskUnit `v0UpdateVec` connections. The lane writes target the *current* row implicitly because `rowCounter` doesn't advance until `rowDone`, which only fires after every lane has reported the row's writes complete (T1.scala:1058 `replayRowDone := slots.map(... endTag.andR ...)`). So a v0Update arriving from the lane is always for the row whose `rowCounter` is currently asserted.

#### 6.3.2 New IO on MaskUnit

```scala
// MaskUnitInterface, beside `gatherReplayBusy`:
/** Bon2D: replayFSM.rowCounter, used to index the per-hw-row v0 shadow array.
  * Stable within a row's processing window and only advances at rowDone, so
  * both v0 writes (from lane v0UpdateVec) and v0 reads (mask consumers below)
  * agree on the currently-active row. */
val gatherRowCounter: UInt = Input(UInt(parameter.rowCounterBits.W))
```

Wire-up in T1.scala (in the `for (row <- 0 until parameter.numRows)` block alongside `gatherRowDone` / `gatherReplayBusy`):

```scala
maskUnit.io.gatherRowCounter := replayFSM.rowCounter
```

#### 6.3.3 Storage change in MaskUnit

Replace the existing single shadow at MaskUnit.scala:178-181:

```scala
val v0: Vec[UInt] = RegInit(
  VecInit(Seq.fill(parameter.vLen / parameter.datapathWidth)(0.U(parameter.datapathWidth.W)))
)
```

with a 2D shadow array, plus a single-row "view" wire that all existing readers can latch onto unchanged:

```scala
// Per-hw-row v0 shadow: outer index = rowCounter, inner = vLen/datapathWidth chunks.
val v0Vec: Vec[Vec[UInt]] = RegInit(
  VecInit(Seq.fill(parameter.timeMultiplexBatch)(
    VecInit(Seq.fill(parameter.vLen / parameter.datapathWidth)(0.U(parameter.datapathWidth.W)))
  ))
)
// View of the current row's v0. All existing v0 consumers below read from
// this wire so call-sites change minimally (just `v0` → `v0`).
val v0: Vec[UInt] = v0Vec(io.gatherRowCounter)
```

**Note:** `v0` is now a `Vec[UInt]` *Wire* (combinationally selected from `v0Vec`), not a `Reg`. All existing reads of `v0.asUInt`, `v0(0)(0)`, `cutUInt(v0.asUInt, ...)` etc. continue to compile and read the active row's mask.

`parameter.timeMultiplexBatch` is already exposed (T1.scala:317; passed into LaneIFParameter and visible in laneParam). Verify the field is reachable from `MaskUnitInterface(parameter: T1Parameter)` — if not, plumb `parameter.timeMultiplexBatch` down via the existing parameter chain.

#### 6.3.4 Write path update (MaskUnit.scala:312-324)

Existing code writes the single `v0` register from `v0UpdateVec`:

```scala
v0.zipWithIndex.foreach { case (data, index) =>
  val laneIndex: Int = index % parameter.laneNumber
  val v0Write = v0UpdateVec(laneIndex)
  val offset: Int = index / parameter.laneNumber
  val maskExt = FillInterleaved(8, v0Write.bits.mask)
  when(v0Write.valid && v0Write.bits.offset === offset.U) {
    data := (data & (~maskExt).asUInt) | (maskExt & v0Write.bits.data)
  }
}
```

Replace with a write that targets `v0Vec(io.gatherRowCounter)`:

```scala
// Writes target the row identified by the current rowCounter. rowCounter is
// stable within a row (advances only on rowDone, which is gated on every
// lane's endTag, including this v0 write), so the lane's v0Update for the
// row currently being executed always lands in v0Vec(rowCounter).
v0Vec(io.gatherRowCounter).zipWithIndex.foreach { case (data, index) =>
  val laneIndex: Int = index % parameter.laneNumber
  val v0Write = v0UpdateVec(laneIndex)
  val offset: Int = index / parameter.laneNumber
  val maskExt = FillInterleaved(8, v0Write.bits.mask)
  when(v0Write.valid && v0Write.bits.offset === offset.U) {
    data := (data & (~maskExt).asUInt) | (maskExt & v0Write.bits.data)
  }
}
```

That single change wires the per-element-mask write-merge into the row-indexed slot. All other 127 entries of `v0Vec` are unaffected this cycle, preserving the pattern each row built up.

#### 6.3.5 Read sites — should be source-compatible

All current readers of `v0` continue to work because `v0` is now a wire pointing at `v0Vec(io.gatherRowCounter)`. The relevant sites already grepped:

- L200: `slideUpV0 := changeUIntSize((v0.asUInt >> slideSize).asUInt, parameter.vLen)`
- L201: `slideDownV0Shift = (v0.asUInt << shifterUpSize).asUInt`
- L212: `baseV0 := Mux(instReq.bits.maskType, v0.asUInt, -1.S(parameter.vLen.W).asUInt)`
- L310: `io.maskE0 := v0(0)(0)`
- L329-337: `regroupV0 := … cutUInt(v0.asUInt, groupSize) …`
- L703, 713, 732: `selectReadStageMask`, `maskSelect`, `maskForDestination` all `cutUInt(v0.asUInt, …)`.

No source changes at any of these sites. Verify with a `grep -n 'v0\\.asUInt\\|v0(' MaskUnit.scala` after the edit — every match should still be valid.

#### 6.3.6 Reset and idle behaviour

`RegInit(VecInit(Seq.fill(timeMultiplexBatch)(...)))` sets all rows' v0 to 0 at hardware reset. That matches the prior behaviour for the shared shadow.

When a non-mask instruction runs, `v0UpdateVec` valid stays low, so v0Vec keeps whatever rows were last written. That's fine — a later masked instruction will overwrite per-row before reading.

#### 6.3.7 Slide/regroup paths

`slideV0` (the slide-shifted view at MaskUnit.scala:201-209) and `regroupV0` are pure functions of `v0.asUInt` (now the active row's mask). They become per-row implicitly. No additional changes.

If a future test exercises a slide that *carries v0 across rows* (uncommon — slides don't typically read v0 of a different hw-row), this might need rethinking. For TEST 3 / vrgather.vx-with-mask, slides are not on the path; this is out of scope.

#### 6.3.8 Test plan

1. Rebuild the simulator.
2. Run `bash run-test.sh vision_task.simple_instruction_gather_scalar -c mudkip2d128small1bram1chain2lanescale -e verilator-emu` (no trace first; faster).
3. Current observed result in `vision_task.simple_instruction_gather_scalar-20260504-140644/run.log`:
   - TEST 1 (H, unmasked): PASS
   - TEST 2 (V, unmasked): PASS
   - TEST 3 (H, diag mask): PASS
   - TEST 4 (V, diag mask): PASS
   - TEST 5 (H, back-to-back unmasked): PASS
4. If TEST 3 regresses: dump `v0Vec` for a few rows (e.g. rows 0, 5, 127) at the cycle of `vrgather.vx, v0.t` and confirm each row's mask has only its diagonal bit set. If the writes are landing in the wrong row, suspect rowCounter timing — a v0Update arriving 1 cycle after rowDone would target row R+1 instead of R.

#### 6.3.9 Risk and mitigations

- **Flop cost.** 4 KiB of state per MaskUnit. With `numRows=1` (current configs), a single MaskUnit is fine. For larger fabrics, consider migrating `v0Vec` to a small SRAM (read latency 1, write port for v0Update); MaskUnit already operates in cycles, so a 1-cycle latency on `v0` reads should be tolerable, but every consumer would need to be re-pipelined. Defer until the flop cost actually bites.
- **rowCounter race.** Edge case: if a lane v0Update arrives in the same cycle as rowCounter advances, the write targets the wrong row. This shouldn't happen because `replayRowDone` requires every lane's `endTag` bit to be set, and that bit only goes high after the lane has finished issuing its last write (including v0Update) for the current row. Verify in waveform after the fix lands by checking that `v0UpdateVec(_).valid` never asserts in the same cycle as `rowDone`.
- **Initial state.** All v0Vec rows are 0 at reset; the first masked instruction must explicitly build v0 (via vmsne/vmseq/etc.) before reading it. The test does this correctly.

## 7. Bug 4 — V-mode (TEST 2/4) — FIXED / SUPERSEDED

After the Bug 3 fix landed, run
`test_output/.../vision_task.simple_instruction_gather_scalar-20260503-094850/run.log`
still had TEST 2 + TEST 4 failures. That diagnosis is now superseded:
`vision_task.simple_instruction_gather_scalar-20260504-140644/run.log`
passes both V-mode cases:

- TEST 2: PASS `vrgather.vx (V) unmasked`
- TEST 4: PASS `vrgather.vx (V) masked`

The historical analysis below is kept only as debug context for future
regressions.

### 7.1 Diagnosis (codex's hypothesis confirmed by data)

**TEST 2 (V, unmasked)** — 16129 / 16384 errors. Result pattern:
- Row r, cols 0..126 = `(r+5)&127` (= H-mode-style scalar broadcast = `grid_in[r][5]`).
- Every row's col 127 = 4 (= `(5+127)&127` = `grid_in[5][127]`).

The 255 correct cells = 128 diagonal cells (where `grid_in[r][5]` coincides with V-expected `grid_in[5][c]` only when r=c, by symmetry of `grid_in`) + 128 col-127 cells (which happen to land on the V-expected value 4) − 1 overlap at `[127,127]`. Arithmetic matches: 128+128−1 = 255 = 16384−16129. ✓

**Conclusion**: V-mode `vrgather.vx` is doing **H-mode-style per-row scalar broadcast**. The MaskUnit's pre-read of `vs2[rs1]` is using horizontal addressing in both modes; the V-mode bit on `requestRegCSR.verticalMode` reaches the SharedVRF (`useVerticalRead`/`useVerticalWrite` are gated by it elsewhere) but the **MaskUnit's `gatherRequestFire` path issues a normal lane-port read (`lanePorts_X.readRequests_4`) that does not opt into the narrow-vertical interpretation**. So V-mode `vrgather.vx` ends up identical to H-mode at the value level except for whatever the V-mode write-scatter incidentally does to col 127.

**TEST 4 (V, diag mask)** — 128 / 16384 errors, all on the diagonal. After Bug 3, the per-row v0 is correct, so the mask hits the right cell `(r, r)`. But the *value* written there is `r` (= `grid_in[0][r]` = `(0+r)&127`), not the expected `(r+5)&127`. So:

| r | got (r,r) | expected | what got is |
|---|---|---|---|
| 0 | 0 | 5 | `grid_in[0][0]` |
| 1 | 1 | 6 | `grid_in[0][1]` |
| 2 | 2 | 7 | `grid_in[0][2]` |
| 3 | 3 | 8 | `grid_in[0][3]` |

The masked path's pre-read **does** seem to enter some V-mode interpretation (the value pattern is `grid_in[0][r]`, not `grid_in[r][5]` like TEST 2), but with `rs1` collapsed to 0 — i.e., it reads element 0 of vert-lane R (= `grid_in[0][R]`) instead of element 5 of vert-lane R (= `grid_in[5][R]` = `grid_in[R][5]` by symmetry, which would match the expected diagonal value).

The TEST 2 vs TEST 4 divergence — H-style addressing in TEST 2 vs partial V-style addressing in TEST 4 — is the key clue: the gather pipeline behaves differently for masked vs unmasked V-mode, suggesting either the `narrowVertical` flag or the `dataOffset` capture is conditional on something that flips between the two cases.

### 7.2 Architectural framing (per design intent)

The MaskUnit is meant to be **mode-agnostic**: it issues a normal lane VRF read for `vs2`, and the SharedVRF returns mode-correct data automatically. SharedVRF.scala already has both H-mode (wide-horizontal) and V-mode (wide-vertical wide-fold gather across 8 banks) read paths — `useVerticalRead := verticalMode && !firstReadFromLSU && !anyNarrowReq` (SharedVRF.scala:422). When V-mode is engaged, the wide-fold returns "transposed" bytes such that the gathered word at `rowCounter=R` contains **byte cByte=R[2:0] of hw-rows 0..7's data at the request's logical address**. After the MaskUnit's `>>40` shift to align to dataOffset=5, the low byte ends up as `grid_in[5][cByte]` — which by symmetry of `grid_in` happens to equal `grid_in[r][5]` only when `cByte == r mod 8 == r mod 8`. (Note: the **narrow-vertical** read path mentioned in earlier drafts of this doc is a deprecated dead-code branch — `narrowEnable` was held at false during Phase 3c work; do not use it as a fix surface.)

So:
- MaskUnit doesn't need changing.
- The fix must come from either (i) verifying V-mode wide-vert is actually firing for the MaskUnit's read, (ii) confirming the wide-vert address mapping is correct, or (iii) accepting that the natural V-mode semantics differs from stock RVV V-mode (transposed) and updating the test expectation.

### 7.3 What the empirical data tells us

For TEST 2 (V-mode unmasked), broadcast value per row r = `(r+5)&127` for cols 0..126.

Two competing predictions:
- If V-mode wide-vert is **firing**: gather word at `rowCounter=R` returns `(grid_in[0..7][cByte=R[2:0]])`. After `>>40`, low byte = `grid_in[5][R[2:0]]`. For r in 0..7 this equals `(r+5)&127`. For r ≥ 8, it would equal `grid_in[5][r mod 8]` — **not** `(r+5)&127`. So if V-mode is firing, rows ≥ 8 should diverge.
- If V-mode wide-vert is **NOT firing** (H-mode wide-horizontal taken): `lanePorts_*.readResults_*` returns `grid_in[R][0..7]` (8 bytes from hw-row R, lane 0 offset 0). After `>>40`, low byte = `grid_in[R][5] = (R+5)&127`. Matches **all** R uniformly.

The TEST 2 result (rows 0..3 = (r+5)) is consistent with **either** path for r<8. We can't distinguish without seeing rows ≥ 8 (only 4 rows are printed). However, the per-row v0-shadow Bug 3 confirmed v0 was being overwritten 128 times in V-mode replays, proving replay does happen in V mode. Then the question becomes whether **`useVerticalRead` is engaging at the cycle the MaskUnit's read fires**.

Given that the previous run's H-mode trace showed `lanePorts_0_readResults_4` returning `0x0807060504030201` (= unmistakably **horizontal** 8-byte hw-row read of grid_in[1]) at row-1 of TEST 1, and that TEST 2 produces exactly the same per-row scalar pattern as H-mode for the first 4 rows, the hypothesis is: **`useVerticalRead` is 0 at the cycle the MaskUnit's gather read fires in TEST 2**, so SharedVRF takes the H-path and returns hw-row R's data uniformly.

There are only three ways that can happen given the gating:
- `verticalMode` (the SharedVRF input, line 128) is 0 — i.e. the per-instruction CSR latch isn't reaching the SharedVRF for `vrgather.vx`.
- `firstReadFromLSU` is 1 — `isLSUInst(firstReadReq.instructionIndex)` is incorrectly returning true for the MaskUnit's gather read (could be a chaining-record stale entry, since vrgather.vx isn't an LSU op).
- `anyNarrowReq` is 1 — some other lane port has `narrowVertical=1`. This is the deprecated path; in current code, `narrowEnable` should be 0 unless something legacy is still asserting it.

For TEST 4 (V-mode masked) we observe a **different** pattern: diagonal cells got `grid_in[0][r]`, not `grid_in[r][5]`. That means in the masked V-mode path, V-mode addressing **does** partly engage (the value comes from row 0's vert-slice, not row r's H-slice), but with `rs1=5` collapsed to 0. So the masked and unmasked V-mode reads are taking **different code paths** in SharedVRF, and only one of them is partially engaging V-mode.

### 7.4 Diagnostic plan for codex (do this first, before any code change)

Build the test with trace enabled (`-e verilator-emu-trace`) and probe these signals in the FST around the cycle of the maskUnit's first gather read in TEST 2 (look for the V-mode CSR write pulse, then the next `lanePorts_0_readRequests_4_valid` rising edge):

| Signal | Expected if V-mode read engages correctly |
|---|---|
| `TestBench.dut.sharedVRF2D_0.verticalMode` | 1 throughout TEST 2/4 (= the per-instruction CSR latch) |
| `TestBench.dut.sharedVRF2D_0.useVerticalRead` (intermediate wire) | 1 at the read-fire cycle |
| `TestBench.dut.sharedVRF2D_0.firstReadFromLSU` | 0 (vrgather.vx is not LSU) |
| `TestBench.dut.sharedVRF2D_0.anyNarrowReq` | 0 (narrow path is dead) |
| `TestBench.dut.sharedVRF2D_0.lanePorts_0_readRequests_4_bits_narrowVertical` | 0 (MaskUnit doesn't set this) |
| `TestBench.dut.sharedVRF2D_0.lanePorts_0_readResults_4` (post-latency) | gathered word: byte_i of hw-rows 0..7 (transposed), NOT hw-row R's bytes 0..7 |

Compare these against TEST 1 (H-mode, where they should all be 0/horizontal) and TEST 4 (V-mode masked, where one of them likely diverges to explain the different failure pattern).

### 7.5 Fix paths (depend on which signal is wrong)

Whichever of the three gating signals is misbehaving, the fix is in **SharedVRF.scala** or in the **CSR plumbing**, not in MaskUnit. Concretely:

**Case A: `verticalMode` input is 0 at SharedVRF during TEST 2.**
The CSR latch isn't reaching SharedVRF for `vrgather.vx`. Trace `requestRegCSR.verticalMode` (T1.scala:572) → `sharedVRF2D.foreach(_.verticalMode := verticalModeReg)` (T1.scala:791). If `verticalModeReg` deasserts mid-replay (e.g., on `io.issue.fire` of the next instruction), and the replay+gather extend past that, the read fires under verticalMode=0. Fix: gate `verticalModeReg` deassert on instruction completion (`replayFSM.lastRowFire+done`), not on `io.issue.fire` of the next.

**Case B: `firstReadFromLSU` is incorrectly 1.**
Some chainingRecord entry tagged as `ls`/`st` has the same `instructionIndex` as the gather. Most likely the previous `vle8.v`'s record hasn't been released yet. Fix: either ensure the record is released by the time vrgather.vx issues (would also resolve Bug 2's earlier symptoms), or relax `isLSUInst` to disambiguate by additional bits in the request.

**Case C: `anyNarrowReq` is 1 spuriously.**
Some legacy code is still driving `narrowVertical` high on a port. Search for any `narrowVertical := true.B` or `:= verticalMode` assignment that isn't gated correctly. Fix: force the dead-code path off by setting `narrowEnable := false.B` at MaskExchangeUnit.scala:926 (the comment there already says "Held at false until correct" — restore that intent).

**Case D: All three look correct, but `readResults_4` still returns horizontal data.**
Then the wide-vert path itself has an addressing bug (`vsStore`, `cGroup`, `cLane`, `rcBase` not matching the H-mode write layout). Trace the actual SRAM bank reads at the cycle of fire and compare to the expected gathered word. Fix is in `SharedVRF.scala:425-451` (the `vsStore`/`rcBase`/`vBankOHPerI`/`vAddrPerI` derivation).

### 7.6 Current V-mode contract in this test

The speculation in older notes that TEST 2 should change to a "natural
fabric" expected formula is superseded. The current test still checks the
V-mode scalar-broadcast contract as:

```
grid_out[r][c] = grid_in[rs1][c]
```

and `vision_task.simple_instruction_gather_scalar-20260504-140644/run.log`
passes that checker. Do **not** update `expected()` from `(RS1+c)&127` to
`(r+RS1)&127` unless the intended ISA contract is deliberately changed.

Future hardening could use a non-symmetric input pattern to make H-mode
`grid_in[r][rs1]` and V-mode `grid_in[rs1][c]` impossible to confuse.

### 7.7 Recommendation

No action required while `simple_instruction_gather_scalar` continues to pass.
If V-mode regresses again, start from the §7.4 waveform diagnostic rather than
assuming the old Case A–D root cause still applies.

## 8. Relevant files in current implementation

- `t1/src/mask/MaskUnit.scala` — `sWaitNextRow` state,
  `gatherRowDone` IO, `gatherReplayBusy` IO, `gatherInstActive` register,
  and per-hw-row `v0Vec` indexed by `gatherRowCounter`.
- `t1/src/T1.scala` — `gatherRowDone`/`gatherReplayBusy`/`gatherRowCounter`
  wiring and `executionReady` gate.
- `t1/src/vrf/SharedVRF.scala` — horizontal/vertical VRF read/write paths
  used by the MaskUnit gather read and LSU snapshots.
- `tests/vision_task/simple_instruction_gather_scalar/simple_instruction_gather_scalar.c` —
  regression test with five checks: H/V unmasked, H/V masked, and H
  back-to-back unmasked.

## 9. Next concrete step

No vrgather-specific fix is pending. Keep
`vision_task.simple_instruction_gather_scalar` in the regression set; use
TEST 5 to catch `gatherInstActive` leaking across back-to-back gathers.

## 10. How to run the test

From repo root (`/home/cbt22/code/code_fyp/VisionSoC`):

```bash
# Trace mode, useful when debugging a regression.
bash run-test.sh vision_task.simple_instruction_gather_scalar \
    -c mudkip2d128small1bram1chain2lanescale \
    -i t1emu \
    -e verilator-emu-trace
```

Useful variants:

```bash
# Without waveform (faster; use this once you trust the result):
T1_MIRROR_RTL_WRITES=1 bash run-test.sh vision_task.simple_instruction_gather_scalar \
    -c mudkip2d128small1bram1chain2lanescale \
    -i t1emu \
    -e verilator-emu

# Cap cycles so a hang aborts cleanly (full-grid kernels need ~5e7 — see handoff §5.4):
bash run-test.sh vision_task.simple_instruction_gather_scalar \
    -c mudkip2d128small1bram1chain2lanescale \
    -i t1emu \
    -e verilator-emu-trace --max-cycles 50000000

# Force vertical mode globally via plusarg (skip per-kernel CSR toggling):
bash run-test.sh vision_task.simple_instruction_gather_scalar \
    -c mudkip2d128small1bram1chain2lanescale \
    -i t1emu \
    -e verilator-emu-trace --vertical
```

After the run, results land in:
```
test_output/mudkip2d128small1bram1chain2lanescale/vision_task.simple_instruction_gather_scalar-<YYYYMMDD-HHMMSS>/
  ├── run.log                                         # full simulator stdout (PASS/FAIL lines + grid prints)
  ├── wave.fst                                        # waveform (only when -e verilator-emu-trace)
  ├── rtl_event.jsonl                                 # DPI event stream (Issue/LsuEnq/etc.)
  ├── sim_result.json                                 # total cycles, success flag
  └── vision_task.simple_instruction_gather_scalar.s  # llvm-objdump of the test ELF
```

To re-check the five cases at a glance:
```bash
grep -E "TEST [0-9]+:|\[CHECK\]" \
  test_output/mudkip2d128small1bram1chain2lanescale/vision_task.simple_instruction_gather_scalar-*/run.log
```

To open the waveform in GTKWave:
```bash
gtkwave test_output/.../wave.fst &
```

The benchmark counterpart (100 iters per case, used for timing) lives in `tests/vision_task/benchmark_instructions/`:
```bash
bash run-test.sh vision_task.benchmark_instructions \
    -c mudkip2d128small1bram1chain2lanescale \
    -e verilator-emu --max-cycles 50000000
```
