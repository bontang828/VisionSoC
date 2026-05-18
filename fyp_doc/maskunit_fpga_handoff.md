# MaskUnitFpga — Handoff for a Fresh Session

**Owner of last touch:** Claude (Opus 4.7), 2026-05-17.
**Branch:** `fpga_driver`.
**Repo:** `/home/cbt22/code/code_fyp/VisionSoC`.

This doc lets a fresh Claude session continue the MaskUnit area-reduction
work without re-deriving any context. It captures the full plan, current
state, references, validation steps, and a ready-to-use kickoff prompt.

---

## 0bis. Latest update (2026-05-18 later, Claude)

**Two further LUT cuts landed in `t1/src/mask/MaskUnitFpga.scala` +
`fpga/system/system_top.tcl` after codex's narrow-BRAM v3.** Combined
result: design hits the route-safe budget AND is at the BRAM cap
exactly.

### Changes since codex's v3:

1. **Opt 7 v2 (chunked v0 slide barrel shifter)** in `MaskUnitFpga.scala`.
   The unified reverse-trick shifter (Opt 7 v1, ~10K LUT for the
   1024-bit `>>`) is replaced by a single 128-bit-wide barrel shifter
   reused over 8 cycles via a small FSM (slideShifterActive +
   slideChunkCounter + slideShiftV0). Output banks (slideV0RegBanks)
   concatenate into slideV0Reg for downstream consumption. Latency: 8
   cycles, hidden trivially within slide instructions' 12K-66K cycle
   execution windows. **Equivalence proof in
   fyp_doc/maskunit_chunked_slide_shifter_debug.md § 5.** **Δ: −4,689 LUT.**

2. **`axis_data_fifo_cap` → `axis_register_slice`** in `system_top.tcl`.
   The camera-pipeline rate-match FIFO (256-deep, 1 BRAM18) was over-
   provisioned by ~10× given the 700-cycle inter-pixel period at
   128×128@26fps. Replaced with a 1-deep skid buffer. **Δ: −1 BRAM18
   tile (= the difference between 144.5 effective tiles and 144 cap),
   −34 LUT.**

3. **Per-lane writeMaskForMaskPipe push-down** in `MaskUnitFpga.scala`.
   The wide-vLen writeMaskForMaskPipe register + scanRightOr/scanLeftOr
   trees are eliminated entirely. Per-lane writeBitMaskForSlide and
   writeBitMaskForExtend slices are computed DIRECTLY from sources
   (v0Cache bit-select, srcPos.U < vl comparisons, etc.) pre-Pipe
   instead of slicing a wide intermediate post-Pipe. The Pipe register
   shrinks from 1024 bits (writeMaskForMaskPipe) to 2 × 128 bits per
   lane (writeBitMaskForSlide + writeBitMaskForExtend pre-computed).
   **Δ: −7,698 LUT** (massively more than the −3 to −5K estimate; Vivado
   was apparently NOT optimizing the wide scanRightOr trees as well as
   I assumed — eliminating them entirely was a much bigger win than
   expected).

### New test added:

* `tests/vision_task/sew16_32_masktest/sew16_32_masktest.c` — exercises
  MaskUnitFpga's SEW=16 and SEW=32 Mux1H paths (vslideup.vi, vslidedown.vi,
  vrgather.vx, vmseq.vx + vmerge.vim at LMUL=1). The existing
  benchmark_instructions only runs SEW=8 so these paths were elaborated
  but not previously exercised at runtime. 8/8 kernels PASS.

### Verification

| Command | Result |
|---|---|
| `T1_MIRROR_RTL_WRITES=1 bash run-test.sh vision_task.benchmark_instructions -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt --max-cycles 50000000` | 68/70 PASS (2 pre-existing vmv.x.s H+V fails — same as baseline) |
| `T1_MIRROR_RTL_WRITES=1 bash run-test.sh vision_task.sew16_32_masktest -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt --max-cycles 50000000` | 8/8 PASS (new SEW=16/32 coverage) |
| Synth: `bash fpga/system/build_fpga.sh -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt -s -a` (report dir below) | 104,717 LUT / 143 BRAM36 + 2 BRAM18 = 144 tiles / WNS positive |

### Synth report 2026-05-18

`fpga/build/mudkip2d128big1bram1chain2lanescale_fpga_maskopt-20260518-052355/`

| Hierarchy | LUT | FF | BRAM36 | BRAM18 | URAM |
|---|---:|---:|---:|---:|---:|
| `system_top_wrapper` | **104,717** | 108,017 | 143 | 2 | 16 |
| `maskUnit2D_0` (`MaskUnitFpga`) | 21,034 | 15,603 | 4 | 0 | 0 |
| `maskUnit2D_0/(body)` | **11,638** | 12,220 | 0 | 0 | 0 |
| `maskUnit2D_0/v0BramInst` | 274 | 0 | 4 | 0 | 0 |

System LUT delta from codex's v3 baseline (117,104): **−12,387 LUT**.
We are **under the user-stated 105K route-safe target** and **at the
144 BRAM tile cap exactly** (143 BRAM36 + 2 BRAM18 packed as 144 tiles).

### Full FPGA impl launching 2026-05-18

Command: `bash fpga/system/build_fpga.sh -c
mudkip2d128big1bram1chain2lanescale_fpga_maskopt -b -a` (~6-7h wall).

Acceptance:
* `write_bitstream Complete!` in vivado_impl.log
* `timing_impl.rpt`: WNS ≥ 0, 0 failing endpoints (was setup-clean at
  WNS=0.307 ns in synth)
* Hold violations are not P&R-blocking; will be addressed in a follow-up
  if they remain after impl.

---

## 0. Latest update (2026-05-18, Codex)

**Narrow-BRAM v3 functional fix is landed in `t1/src/mask/MaskUnitFpga.scala`
only.** The failed revised-v2 design tried to service a row change by draining
lane writes, writing the old row, and refilling the new row on the critical
transition path. That races the existing T1 replay/row schedule: `gatherRowCounter`
can advance while lane `v0Update` traffic for the row being left is still
arriving, and MaskUnit has no local ready/valid back-pressure on that path.
Writes could therefore be dropped or applied to the wrong cached row.

Chosen approach: keep the fix local to `MaskUnitFpga.scala` by using a
three-row local policy:

* `activeCacheChunks` is the only row visible through `v0`.
* `prefetchCacheChunks` streams the next sequential row from the narrow BRAM
  while the active row executes, so a row change is a register swap rather
  than a blocking refill.
* `writebackCacheChunks` snapshots the row just left and writes it back through
  the BRAM write port in the background.

The design intentionally does not change `Lane.scala`, `MaskExchangeUnit.scala`,
or T1 replay control. It relies on the existing replay schedule giving enough
cycles per row for the 8 narrow chunks to prefetch/write back; the target
`mudkip2d128big1bram1chain2lanescale_fpga_maskopt` regression satisfies this.
If a future config can switch rows faster than that, the robust alternative is
lane-side row-transition back-pressure or a two-entry writeback queue.

Verification so far:

| Command | Result |
|---|---|
| `T1_MIRROR_RTL_WRITES=1 bash run-test.sh vision_task.benchmark_instructions -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt --max-cycles 50000000` before the fix | Reproduced `FAIL (57/70 checks passed, 13 failed)` |
| Same benchmark after the fix | `FAIL (68/70 checks passed, 2 failed)`; only the pre-existing `vmv.x.s` H+V C-checks remain |
| `T1_MIRROR_RTL_WRITES=1 bash run-test.sh vision_task.simple_instruction_asm -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt --max-cycles 50000000` | Spike difftest `"success": true`; ignore the known 127 C-verifier mismatch lines |
| FPGA synth resource check | PASS for narrow BRAM: `v0BramInst` is 4 BRAM36 instead of the previous 16 |

Synth command and report:

```sh
bash fpga/system/build_fpga.sh \
  -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt \
  -r test_output/mudkip2d128big1bram1chain2lanescale_fpga_maskopt/rtl-20260518-023232/result \
  -s -a
```

Report directory:
`fpga/build/mudkip2d128big1bram1chain2lanescale_fpga_maskopt-20260518-024351/`.

| Hierarchy | LUT | FF | BRAM36 | BRAM18 | URAM |
|---|---:|---:|---:|---:|---:|
| `system_top_wrapper` | 117,104 | 106,447 | 143 | 3 | 16 |
| `t1_top/u_t1` | 95,502 | 77,594 | 132 | 0 | 0 |
| `maskUnit2D_0` (`MaskUnitFpga`) | 33,360 | 14,051 | 4 | 0 | 0 |
| `maskUnit2D_0/(body)` | 24,240 | 10,674 | 0 | 0 | 0 |
| `maskUnit2D_0/v0BramInst` (`v0_bram`) | 271 | 0 | 4 | 0 | 0 |

Synth timing is setup-clean (`WNS=0.307 ns`, 0 setup failing endpoints).
The synth timing report still shows hold/pulse-width violations, which are
not a route/timing-closure signoff result.

---

## 1. Kickoff prompt (paste into new session)

```
Read fyp_doc/maskunit_fpga_handoff.md end-to-end. It is the canonical
handoff for a multi-phase task that reduces MaskUnit area on the FPGA
build of the VisionSoC T1 vector core. Phases 1-2 are done and verified;
Phases 3-5 remain.

The big config (vLen=1024) currently overflows the KV260 LUT budget
because MaskUnit alone uses 70K LUTs / 142K FFs. The plan adds a
parallel `t1/src/mask/MaskUnitFpga.scala` (drop-in replacement, same IO
bundle as MaskUnit) that stacks four FPGA-only optimisations to bring
MaskUnit down to roughly 20K LUTs / 15K FFs, fitting the design within
the 117K LUT cap.

Phase 1 (config flag plumbing) and Phase 2 (BRAM wrapper at
fpga/wrapper/v0_bram.v) are landed and the t1emu sim regression on the
new `_fpga_maskopt` configs passes byte-identical to the `_fpga`
baseline (flag is currently a no-op since Phase 4 wiring is pending).

Pick up at Phase 3a: read t1/src/mask/MaskUnit.scala end-to-end, then
write t1/src/mask/MaskUnitFpga.scala by copying the IO + skeleton and
applying the four optimisations described in § 4 of the handoff doc.
Use the existing approved plan file at
~/.claude/plans/please-read-home-cbt22-code-code-fyp-vis-validated-spark.md
for the detailed step-by-step inside MaskUnitFpga. Verify per the
matrix in § 7 of the handoff doc.

Do not modify t1/src/mask/MaskUnit.scala — it must stay untouched as
the simulator path. All edits go into the new MaskUnitFpga.scala plus
the conditional instantiation at t1/src/T1.scala:550 (Phase 4).
```

---

## 2. TL;DR

**Problem.** Big config (vLen=1024) FPGA synth: 155,748 LUTs / 234,453
FFs / 139 BRAM. KV260 cap is 117,120 LUTs — **33% over**. MaskUnit alone
accounts for 70K LUTs and 142K FFs of that, dominated by a
128-row × vLen-bit FF-replicated shadow of the v0 mask register.

**Solution.** A new `t1/src/mask/MaskUnitFpga.scala` with identical IO
to the original `MaskUnit.scala`, selected at the T1 instantiation site
via a new `useFpgaMaskUnit` config flag (mirrors the existing
`vfuInstantiateParameter == "minimalFpga"` FPGA-variant pattern).
Internally stacks four optimisations to land MaskUnit at ~20K LUTs /
~15K FFs (matching the small config).

**Expected outcome.** System total drops 155K → ~110K LUTs (fits 117K
cap). All SEW=8/16/32 paths preserved. <5% performance hit on masked
instructions. Original `MaskUnit.scala` and simulator builds unchanged.

**Status (2026-05-17):** Phases 1-2 done and verified. Phases 3-5 remain.

---

## 3. Background reference docs (in priority order)

1. **This handoff** — `fyp_doc/maskunit_fpga_handoff.md`
2. **The approved plan** — `~/.claude/plans/please-read-home-cbt22-code-code-fyp-vis-validated-spark.md`
   (most detailed implementation plan; contains pseudo-code for each
   optimisation)
3. **FPGA build status** — `fyp_doc/fpga_build_status.md`. Read § 0.13
   (this work's section), § 0.12 (the vLen=1024 expansion that
   prompted this), § 0.11 (5r URAM scratchpad — sets the XPM wrapper
   precedent for `fpga/wrapper/v0_bram.v`), § 0.2-0.3 (deployment
   rules), § 0.5+ (config naming + the `_fpga` suffix convention).
4. **2D fabric handoff** — `fyp_doc/2d_fabric_handoff.md` § 4.2 for the
   v0 mask semantics (it's a packed bit map indexed by element index,
   mode-agnostic on bit interpretation, mode-sensitive on data
   orientation) and § 5.1 for where MaskUnit lives in the T1 datapath.
5. **LSU vertical-mode handoff** — `fyp_doc/LSU_vertical_mode_handoff.md`
   if any V-mode regression breaks.

Memory entries that apply:
* `feedback_handoff_before_launch.md` — update status doc BEFORE
  launching multi-hour FPGA builds, never after.
* `feedback_preserve_past_builds.md` — never delete `fpga/build/*` or
  `test_output/*` dirs.
* `feedback_no_shell_timeout_on_run_test.md` — use `--max-cycles
  50000000`, never `timeout`.

---

## 4. The architectural plan — four optimisations stacked

All four live inside the new `t1/src/mask/MaskUnitFpga.scala`. The
external IO bundle is identical to `MaskUnit.scala` so the rest of T1
(Lane.scala, MaskExchangeUnit.scala, SharedVRF.scala, every mask
consumer) is unaffected.

### Opt 3 — BRAM-back v0Vec + active-row register cache

**Target:** −15K LUTs, −131K FFs.

**Today (`MaskUnit.scala:194-199`):**
```scala
val v0Vec: Vec[Vec[UInt]] = RegInit(
  VecInit(Seq.fill(parameter.timeMultiplexBatch)(
    VecInit(Seq.fill(parameter.vLen / parameter.datapathWidth)(0.U(parameter.datapathWidth.W)))
  ))
)
val v0: Vec[UInt] = v0Vec(gatherRowCounter)
```
At `timeMultiplexBatch=128`, `vLen=1024` → 131,072 FFs in `v0Vec` and
a 128:1 row-select mux (~15K LUTs).

**Replace with:**
- A `BlackBox` wrapping `fpga/wrapper/v0_bram.v` (Phase 2 — already
  drafted).
- A `v0Cache: UInt = RegInit(0.U(vLen.W))` register that mirrors
  `v0Vec(gatherRowCounter)`.
- An `enum` FSM that refills `v0Cache` from BRAM when
  `gatherRowCounter` changes (1-cycle BRAM read latency with the
  current wrapper).
- Write-through: when a lane v0Update fires, update `v0Cache`
  (if the target row matches the cached row) AND write to BRAM
  via the BRAM's port-A byte-write strobe.

**Cache-validity gate** on `askMaskVec(i).ready` so a stale cache
back-pressures the mask request rather than feeding garbage. Pre-fetch
row 0 whenever the unit is idle so the first hw-row of every
instruction is always hot.

### Opt 1 — On-demand per-lane mask slicer

**Target:** −12K LUTs.

**Today (`MaskUnit.scala:341-360`, plus the corresponding `maskSplit`
loop at lines 727-747):** three SEW-specific `cutUInt + grouped +
transpose + Mux1H` chains computed in parallel every cycle:
```scala
val regroupV0: Seq[UInt] = Seq(4, 2, 1).map { singleSize =>
  val groupSize = singleSize * (parameter.datapathWidth / parameter.eLen)
  VecInit(cutUInt(v0.asUInt, groupSize).grouped(parameter.laneNumber).toSeq.transpose
    .map(seq => VecInit(seq).asUInt)).asUInt
}
```
That's three independent O(vLen) trees that Vivado synth doesn't share
gates between.

**Replace with** a single runtime-parameterised slicer that computes
ONLY the requested lane's requested-SEW slice per cycle:
```scala
def maskSliceFor(lane: Int, sew: UInt, group: UInt, baseV0: UInt, baseSlideV0: UInt): UInt = {
  // small runtime mux on sew (3-way) producing the appropriate
  // 64-bit slice from v0Cache + slideV0Reg
}
```
All three SEWs still supported via the runtime `sew` mux, but only
one slice gets computed per request instead of all-SEWs × all-lanes
pre-computed.

### Opt 2 — Pipelined slide write-mask trees

**Target:** −10K LUTs.

**Today (`MaskUnit.scala:231-289`):** `writeMaskForMaskPipe` combines
`scanRightOr(UIntToOH(vl))` and `scanLeftOr(UIntToOH(shifterValidSize))`
over vLen bits as wide combinational trees. Then
`writeBitMaskForSlide` / `writeBitMaskForExtend` slice it per SEW.

**Replace with** a segment-walked FSM that processes 128 bits per
cycle into a register, accumulating 8 cycles to build the full 1024-bit
`writeMaskForMaskPipe`. Hidden behind the slide instruction's normal
multi-cycle execution (slides cost 26-65K cycles per the
`benchmark_instructions` data).

### Opt 7 — Pipelined v0 slide barrel shifters

**Target:** −15K LUTs.

**Today (`MaskUnit.scala:218-220`):** two 1024-bit barrel shifters in
parallel:
```scala
val slideUpV0:       UInt = changeUIntSize((v0.asUInt >> slideSize).asUInt, parameter.vLen)
val slideDownV0Shift =                       (v0.asUInt << shifterUpSize).asUInt
val slideDownV0:     UInt = changeUIntSize(slideDownV0Shift, parameter.vLen)
```
Each lowers to a 10-stage mux tree where each stage is 1024 wide — ~10K
LUTs per shifter. The two together account for ~20K LUTs.

**Replace with** a single 128-bit-wide pipelined shifter reused over
8 cycles for each direction. Output written 128 bits/cycle into
`slideV0Reg`. **Behavior-preserving**: every slide instruction
(`vslideup.vi/.vx`, `vslidedown.vi/.vx`, `vslide1up`, `vslide1down`)
produces the same `slideUpV0`/`slideDownV0` bit pattern; only the
latency from "valid" to "available" lengthens from 1 cycle to ~8
cycles, fully absorbed by the slide-setup pipeline. The
`slideV0OverReg` overlap-bit register and downstream `slideV0` Vec
remain untouched.

**Combined savings (all four):** ~−52K LUTs, ~−131K FFs.
**Resulting MaskUnitFpga:** ~20K LUTs / ~15K FFs (matching small).
**System total:** 155K → ~105-115K LUTs (under 117K KV260 cap).

---

## 5b. Synth measurement update (2026-05-18, early morning BST)

**Synth on `mudkip2d128big1bram1chain2lanescale_fpga_maskopt` (Phase 3b
wide-BRAM + 3c per-lane laneMaskInput slicer + 3d per-lane writeBitMask
slicer + 3e unified slide barrel shifter via reverse trick):**

| Resource | Value | Target | Status |
|---|---:|---:|:--|
| `system_top_wrapper` Total LUT | **119,363** | ≤ 105,000 (route-safe) | OVER by 14,363 |
| `system_top_wrapper` FF | 105,549 | — | fine |
| `system_top_wrapper` BRAM36 | **155** + 3 BRAM18 | ≤ 144 tiles | OVER by ~12 tiles |
| MaskUnitFpga LUT | 35,700 | — | (down from 70,066 baseline) |
| MaskUnitFpga body LUT | 24,562 | — | (down from 70,066) |
| v0_bram wrapper LUT | 2,270 | — | XPM `xpm_memory_sdpram` 1024-bit |
| MaskUnitFpga FF | 13,138 | — | (down from 142,014 baseline) |
| MaskUnitFpga BRAM36 | 16 | — | width-driven (1024-bit / 72) |

**Compared to `_fpga` baseline (155,748 LUT / 234,453 FF / 139 BRAM):**
- LUT: **−36,385** (−23%)
- FF: **−128,904** (−55%)
- BRAM36: **+16** (v0_bram new)

**Narrow-BRAM attempts (Phase 3b-rev v1 + v2): both BROKEN.**
- v1 (chunkWidth=256 with "all lanes at offset c" assumption): failed
  because the config has laneNumber=2 (=dLen/datapathWidth = 128/64),
  so each BRAM chunk holds 2 lanes × 2 offsets, not 1 offset.
- v2 (generalized (lane,offset)→dpIdx mapping with chunkWidth=128, 8
  chunks, 9-cycle transition with pending buffer + final-cycle drain):
  also failed. Suspected race during multi-cycle transition when lane
  v0Updates fire faster than refill can drain pending. Needs deeper
  redesign or different approach (e.g. back-pressure lane writes).
- Reverted to wide-BRAM (Phase 3b style) which passes regression
  byte-identical to baseline.

**Remaining gaps:**
1. **LUT over by 14K.** Possible levers: MaskCompress (5,994 LUT,
   untouched), BitLevelMaskWrite (2,811 LUT, untouched), more
   per-lane on-demand inside MaskUnitFpga body, or attack non-MaskUnit
   modules.
2. **BRAM over by 12 tiles.** Requires narrow-BRAM redesign that
   handles arbitrary laneNumber correctly AND tolerates the latency
   between lane v0Updates and BRAM refill. Likely needs lane-side
   back-pressure (touches `Lane.scala` or `MaskExchangeUnit.scala`).

## 5. Current state (2026-05-17, later in the day)

### Done

**Phase 3b (Opt 3) — BRAM v0 + active-row register cache.** Landed.
* `t1/src/mask/MaskUnitFpga.scala` is the new module (parallel to
  MaskUnit.scala), in the same `org.chipsalliance.t1.rtl` package, same
  `MaskUnitInterface` IO. v0Vec replaced by:
  * `V0BramBlackBox extends BlackBox with HasBlackBoxResource` wrapping
    `v0_bram.sv` (behavioural model, see note below).
  * `v0Cache: UInt(vLen.W)` register mirroring `mem[gatherRowCounter]`.
  * Refill FSM: when `gatherRowCounter =/= v0CacheRow`, drive BRAM port-B
    read with rd_addr=gatherRowCounter, latch into v0Cache on next cycle.
  * Merged-write path: per-cycle lane v0Updates are merged into a single
    vLen-bit (data, byte-strobe) write to BRAM port A; if the write
    targets the cached row and no refill is in flight, cache also updated
    combinationally. A `pendingWrite{Data,Strb}` shadow covers the race
    where a write fires DURING the refill issue cycle for the same row
    (BRAM port B doesn't observe port A in same cycle with WRITE_MODE_B
    = "no_change").

**Phase 3c (Opt 1) — On-demand per-lane mask slicer.** Landed.
* `maskSliceFor(lane, sew, maskSelect, source)` returns the
  datapath-width slice this lane needs. Three SEW-specific permutations
  are built per lane and Mux1H'd at 32-bit width (instead of the OLD
  vLen-bit-wide regroupV0/regroupSlideV0). Net effect: 1024-bit-wide
  reorder logic per SEW × 3 SEWs collapsed to 32-bit-wide SEW mux per
  lane × 8 lanes.

**Phase 4 — Conditional MaskUnit/MaskUnitFpga selection at T1.scala:556.**
Landed.
```scala
val maskUnit2D =
  if (parameter.useFpgaMaskUnit.getOrElse(false))
    Seq.tabulate(parameter.numRows)(_ => Instantiate(new MaskUnitFpga(parameter)))
  else
    Seq.tabulate(parameter.numRows)(_ => Instantiate(new MaskUnit(parameter)))
```
LUB inference works because `Instance[+A]` is covariant and both modules
share `FixedIORawModule[MaskUnitInterface] with SerializableModule[T1Parameter]
with ImplicitClock with ImplicitReset` as their parent.

**Phase 3 supporting infrastructure**
* `fpga/wrapper/v0_bram.v` — Vivado-synthesis copy, XPM `xpm_memory_sdpram`.
* `t1/resources/v0_bram.sv` — Verilator-friendly behavioural model
  (XPM cells don't exist in Verilator's lib so a parallel `.sv` copy is
  required, see `[[chisel-blackbox-xpm-split]]` memory entry). `.sv`
  extension is mandatory because nix `mlirbc-to-sv.nix` builds
  `filelist.f` from `find -name "*.sv"` and ignores `.v` blackbox
  resources.
* `fpga/system/system_top.tcl` — added `add_files ... v0_bram.v` next to
  the existing `uram_scratchpad.v` add.
* Both new files (`t1/src/mask/MaskUnitFpga.scala`, `t1/resources/v0_bram.sv`,
  `fpga/wrapper/v0_bram.v`) are `git add`-ed because nix
  `lib.fileset.toSource` only includes git-tracked files.

**Verification — t1emu sim regression on `mudkip2d128big1bram1chain2lanescale_fpga_maskopt`:**

| Test | Result |
|---|---|
| `vision_task.simple_instruction_asm` | `success: true`, `total_cycles=21514` (bit-identical to `_fpga` baseline) |
| `vision_task.benchmark_instructions` | 68/70 PASS (the 2 `vmv.x.s` H+V fails are pre-existing per § 8 risk #8) |

### Not done

**Phase 3d (Opt 2) — Pipelined slide write-mask trees.** Deferred.
* The plan's "segment-walked FSM that processes 128 bits per cycle into
  a register, accumulating 8 cycles to build the full 1024-bit
  writeMaskForMaskPipe" requires deferring the writeCountPipe0 firing
  by 8 cycles for slide instructions while keeping the non-slide
  fast path. Downstream `writeCountVec` consumers in the lane assume
  fixed 3-cycle latency from `maskPipeReq.valid`; lengthening this for
  slide-only requires either a conditional Pipe or splitting the slide
  path entirely. Non-trivial control-flow surgery.
* Per-cycle 128-bit chunk computation still requires a 128-bit-wide
  barrel shifter on a vLen-bit source (~1280 LUTs per cycle), so the
  plan's -10K LUT target depends on Vivado's ability to recognize the
  reuse — would need to measure.

**Phase 3e (Opt 7) — Pipelined v0 slide barrel shifters.** Deferred.
* Same fundamental issue: each per-cycle 128-bit window extraction
  from `v0Cache` is a 128-bit barrel shifter on a vLen-bit input
  (~1280 LUTs per cycle). The savings claimed by the plan rely on
  Vivado SHARING the per-cycle shifter across 8 cycles (one physical
  shifter, 8 invocations). To get that, we'd need to build a Vec[8]
  of bank registers + an FSM that writes one bank per cycle. Doable
  but adds a 2-bit-wide state and an 8-bank Vec, plus reasoning about
  the bidirectional shift (slideUp vs slideDown).
* RECOMMENDED next: synth measure Phases 3b+3c savings first; if
  they hit the 117K LUT cap, defer 3d/3e indefinitely. Synth pre-flight
  is launched as the next step.

**Phase 5 — FPGA synth pre-flight is the immediate next gate.**

### Phase 1 — Config flag plumbing

(unchanged from earlier; still done.) `useFpgaMaskUnit: Option[Boolean]
= Some(false)` is now part of:
* `t1/src/T1.scala:141-146` — `T1Parameter` case class field.
* `elaborator/src/t1/T1.scala:48` — `@arg(name = "useFpgaMaskUnit")`
  added to `T1ParameterMain`; threaded into the `T1Parameter(...)`
  constructor on line 76.
* `elaborator/src/t1emu/TestBench.scala:49` — same edit in the t1emu
  elaborator's separate `T1ParameterMain` definition.
* `designs/org.chipsalliance.t1.elaborator.t1.T1.toml` — added three
  `_fpga_maskopt` configs (small/medium/big) alongside the existing
  `_fpga` ones, each with `--useFpgaMaskUnit true`.
* `designs/org.chipsalliance.t1.elaborator.t1emu.TestBench.toml` —
  same six configs added.

**Phase 2 — BRAM wrapper drafted.**
* `fpga/wrapper/v0_bram.v` — XPM `xpm_memory_sdpram`, 128 × 1024-bit,
  byte-write enable, simple dual-port (write A, read B), 1-cycle read
  latency. Follows the `fpga/wrapper/uram_scratchpad.v` precedent.
* **Not yet added to `fpga/system/system_top.tcl`** —do this when
  doing Phase 4 / the FPGA build, NOT before.
* **TBD:** verify Vivado infers ≤4 BRAM36 tiles. The 1024-bit-wide
  port may force more parallel BRAMs than the capacity-based 4 tiles
  (BRAM36 max single-port width is 72 bits → 1024/72 ≈ 15 BRAMs).
  Two possible fixes if synth shows >5 BRAM36:
  1. Split the wrapper into 8 parallel narrower XPM instances each
     fitting in 1 BRAM18 (8 BRAM18 = 4 BRAM36 by capacity).
  2. Narrow the wrapper to 256-bit width and let MaskUnitFpga
     time-multiplex the refill over 4 cycles.

**Phase 1 verification.** Ran `vision_task.simple_instruction_asm` on
`mudkip2d128big1bram1chain2lanescale_fpga_maskopt` under t1emu:
`success: true`, `meta_vlen=1024`, `total_cycles=21514` — bit-identical
to the existing `_fpga` baseline. Confirms the elaborator accepts the
new flag and nothing breaks. (The flag is a no-op until Phase 4 wires
the conditional MaskUnit/MaskUnitFpga selection at `T1.scala:550`.)

### Not done

**Phase 3a** — Read `t1/src/mask/MaskUnit.scala` end-to-end (~1000
lines). The previous exploration agent provided a summary in the
approved plan file, but writing MaskUnitFpga requires close reading of
the IO bundle, gather state machine, compress/viota machinery, and the
read/write port contracts.

**Phase 3b** — Create `t1/src/mask/MaskUnitFpga.scala` skeleton with
Opt 3 only (BRAM swap + active-row cache + refill FSM). Verify it
elaborates and behaves identically to MaskUnit under the
`_fpga_maskopt` regression (still TBD when Phase 4 wires the
selection).

**Phase 3c** — Add Opt 1 (on-demand per-lane slicer) inside
MaskUnitFpga.

**Phase 3d** — Add Opt 2 (pipelined slide write-mask trees) inside
MaskUnitFpga.

**Phase 3e** — Add Opt 7 (pipelined v0 slide barrel shifters) inside
MaskUnitFpga.

**Phase 4** — Wire the conditional instantiation at `t1/src/T1.scala:550`:
```scala
val maskUnit2D = Seq.tabulate(parameter.numRows) { _ =>
  if (parameter.useFpgaMaskUnit.getOrElse(false))
    Instantiate(new MaskUnitFpga(parameter))
  else
    Instantiate(new MaskUnit(parameter))
}
```

**Phase 5** — Full validation per § 7 + 8 below. FPGA synth pre-flight
is the ultimate deliverable.

---

## 6. Files inventory

### Already touched (Phase 1-2)

| File | Change |
|---|---|
| `t1/src/T1.scala` (line 141-146) | Added `useFpgaMaskUnit` field to T1Parameter |
| `elaborator/src/t1/T1.scala` (line 48, 76) | Added CLI arg + threaded into T1Parameter |
| `elaborator/src/t1emu/TestBench.scala` (line 49, similar) | Same edits |
| `designs/org.chipsalliance.t1.elaborator.t1.T1.toml` | Added 3 `_fpga_maskopt` configs |
| `designs/org.chipsalliance.t1.elaborator.t1emu.TestBench.toml` | Same 3 configs |
| `fpga/wrapper/v0_bram.v` (new) | XPM BRAM wrapper |
| `fyp_doc/fpga_build_status.md` § 0.13 | Status doc |
| `fyp_doc/maskunit_fpga_handoff.md` (this file) | Handoff |

### To be touched (Phase 3-5)

| File | Change |
|---|---|
| `t1/src/mask/MaskUnitFpga.scala` (NEW) | Full new module, ~600-700 LOC. Copy IO from MaskUnit, apply the four optimisations. |
| `t1/src/T1.scala` (line ~550) | Conditional `if (parameter.useFpgaMaskUnit.getOrElse(false))` selecting between MaskUnit and MaskUnitFpga |
| `fpga/system/system_top.tcl` | `add_files` for `v0_bram.v` (place near the existing `uram_scratchpad.v` add) |
| `fyp_doc/fpga_build_status.md` | Update § 0.13 status as phases complete; write a new sub-section BEFORE launching the multi-hour FPGA build per `feedback_handoff_before_launch.md` |

### Explicitly NOT touched

* `t1/src/mask/MaskUnit.scala` — preserved as the simulator path.
* `t1/src/laneStage/MaskExchangeUnit.scala` — no v0 storage; unaffected.
* `t1/src/Lane.scala` — v0Update interface preserved.
* `t1/src/vrf/SharedVRF.scala` — unaffected.
* `Bundles.scala` — no Bundle widths change.

---

## 7. Verification matrix + passing criteria

### Per-phase gates

| Phase | Test | Pass criterion |
|---|---|---|
| **3b** (BRAM swap only) | `build_rtl.sh -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt` | nix build success; no new chisel warnings beyond today's 5 baseline |
| **3b** | `run-test.sh vision_task.simple_instruction_asm -c <_fpga_maskopt> --max-cycles 50000000` | `success: true` (Spike difftest); cycle count within 5% of `_fpga` baseline (21,514 cyc) |
| **3c** (add on-demand slicer) | Same elaborate + sim regression | Same PASS; cycles unchanged |
| **3d** (add slide write-mask pipelining) | Slide-heavy regression: `vision_task.benchmark_instructions -c <_fpga_maskopt>` | All 34/35 V-mode checks PASS (vmv.x.s known fail); cycles within 5% of baseline |
| **3e** (add slide barrel shifter pipelining) | Same regression as 3d | Same PASS criteria |
| **4** (wire selection) | All non-`_fpga` configs run unchanged | bit-identical to today's PASS set (Phase 4 must not affect simulator builds) |

### Full regression (post-Phase 4)

Run under `mudkip2d128big1bram1chain2lanescale_fpga_maskopt`:

| Test | Pass criterion |
|---|---|
| `vision_task.simple_instruction_lmul1` | 16,384 cells correct, Spike `success: true` |
| `vision_task.benchmark_vadd_lmul1` | 6/6 PASS including TEST 9 8-way accumulate |
| `vision_task.benchmark_instructions_lmul1` | 34/35 PASS (vmv.x.s known fail) |
| `vision_task.benchmark_instructions` (LMUL=4 backcompat) | 34/35 PASS (same) |
| `vision_task.simple_instruction_vert_lsu` | PASS (critical: stresses v0 read under V mode + LSU) |
| `vision_task.simple_instruction_asm` | PASS, cycles within 5% |

Cycle-count check: per-kernel cycles ≤ +5% delta vs `_fpga` baseline.

### FPGA synth pre-flight (the actual deliverable)

```sh
bash fpga/system/build_fpga.sh -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt -s -a
```

Read `fpga/build/<dir>/utilization_synth.rpt`. Targets:

| Resource | Today (big_fpga, no maskopt) | Target (with maskopt) | KV260 cap |
|---|---:|---:|---:|
| MaskUnit LUT | 70,066 | **≤ 25,000** | — |
| MaskUnit FF | 142,014 | **≤ 20,000** | — |
| **System total LUT** | 155,748 | **≤ 115,000** | 117,120 |
| System FF | 234,453 | ~105,000 | 234,240 |
| BRAM36 | 139 | **≤ 144** (was 139; +4 for v0_bram → 143) | 144 |

**Acceptance:** system LUT ≤ 115K AND BRAM ≤ 144 AND all sim
regressions PASS.

### FPGA full impl (only after synth pre-flight passes)

```sh
bash fpga/system/build_fpga.sh -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt -b -a
```

Expect ~6-7 h wall. Acceptance:
* `timing_impl.rpt`: WNS ≥ 0, 0 failing endpoints
* `write_bitstream Complete!` in `vivado_impl.log`

---

## 8. Risks / things that might bite

1. **BRAM tile count.** `xpm_memory_sdpram` with a 1024-bit-wide port
   may infer more BRAMs than the 4-tile capacity-based minimum. If
   synth shows >5 BRAM36, refactor `v0_bram.v` to 8 parallel narrower
   XPM instances (each fitting in 1 BRAM18 = 4 BRAM36 total).
2. **First-cycle cold cache.** First hw-row of every instruction needs
   the active-row cache valid. Pre-fetch row 0 whenever idle to avoid
   per-instruction startup stalls.
3. **Mask-write hazard during refill.** If a v0 write arrives while
   refill is in flight for the same row, the cache could stale. Chosen
   handling: write-through to BRAM always; if write hits the in-flight
   refill row, also forward to the accumulator.
4. **Code duplication.** ~500 LOC copied from MaskUnit into
   MaskUnitFpga. Two modules to keep in sync. Mitigation deferred to a
   follow-up refactor; document the duplication clearly in
   MaskUnitFpga's header comment.
5. **Slide latency leakage.** Opts 2 + 7 add ~8 cycles of internal
   latency to slide-setup. The cycle-count regression in § 7 catches
   any kernel that's bottlenecked on this.
6. **Verilator + BlackBox.** The `V0BramBlackBox` Chisel BlackBox must
   wrap a Verilog file Verilator can simulate. `v0_bram.v` itself is
   synthesizable Verilog (XPM cells expand to RTL), so Verilator can
   simulate it directly — no separate sim model needed.
7. **`_fpga_maskopt` configs already exist in t1emu TOML.** Re-running
   the sim regressions after each phase doesn't need new config
   plumbing — just rerun `run-test.sh -c <…_fpga_maskopt> …`.
8. **vmv.x.s known fail at V-mode** is pre-existing (commit `ce8c3762
   [DEBUG] debug test for vmv.x.s`). Spike difftest agrees with DUT,
   so it's a C-side verifier formula bug, not a hardware regression.
   Continues to FAIL the C-checker in `benchmark_instructions`; this is
   expected and not a regression.

---

## 9. Pointers to runtime hooks

| Need | Command / location |
|---|---|
| Build RTL for a config | `bash build_rtl.sh -c <config_name>` |
| Run a sim test | `T1_MIRROR_RTL_WRITES=1 bash run-test.sh <test> -c <config> --max-cycles 50000000` |
| Run FPGA synth only | `bash fpga/system/build_fpga.sh -c <config> -s -a` (~25-30 min wall) |
| Run full FPGA impl | `bash fpga/system/build_fpga.sh -c <config> -b -a` (~6-7 h wall) |
| Inspect synth report | `fpga/build/<dir>/utilization_synth.rpt` |
| Inspect timing | `fpga/build/<dir>/timing_impl.rpt` |
| Inspect sim cycle results | `test_output/<config>/<test>-<ts>/run.log` |

Default config = `_fpga_maskopt` siblings of the existing `_fpga`
configs. Don't touch the original `_fpga` entries — they are the
easy-revert baseline.

---

## 10. End-of-task acceptance summary

The work is done when ALL of the following hold simultaneously:

1. `MaskUnitFpga.scala` exists, ~600-700 LOC, with the four
   optimisations applied.
2. `T1.scala` conditionally instantiates MaskUnitFpga when
   `useFpgaMaskUnit` is true.
3. Original `MaskUnit.scala` is byte-identical to today's HEAD.
4. All non-`_fpga` t1emu sim regressions pass bit-identically (verify
   `success: true` AND `total_cycles` unchanged on at least
   `simple_instruction_asm`, `benchmark_instructions`, and
   `simple_instruction_vert_lsu`).
5. All `_fpga_maskopt` t1emu sim regressions pass (`success: true`;
   cycle delta ≤ 5% from the `_fpga` baseline).
6. FPGA synth on `mudkip2d128big1bram1chain2lanescale_fpga_maskopt`
   reports MaskUnit ≤ 25K LUTs, system total ≤ 115K LUTs, BRAM ≤ 144.
7. `fyp_doc/fpga_build_status.md § 0.13` is updated with the final
   numbers AND written-before-launch of any multi-hour FPGA impl run.

After this, optional follow-up work for a separate session:
* Factor shared helpers between MaskUnit and MaskUnitFpga to eliminate
  duplication.
* Full FPGA impl + deployment on KV260 with the new bitstream.
