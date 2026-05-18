# MaskUnitFpga narrow-BRAM v2 — debug brief for codex

**Branch:** `fpga_driver`
**Repo:** `/home/cbt22/code/code_fyp/VisionSoC`
**Date this brief written:** 2026-05-18 BST
**Owner of last touch:** Claude (Opus 4.7)

## 0. TL;DR for codex

A "narrow-port + chunked refill" rewrite of the v0 mask BRAM in
`t1/src/mask/MaskUnitFpga.scala` is functional in elaboration but
breaks t1emu sim on mask-producing kernels (vmseq.vv, vmslt.vv,
vmsle.vv, vmsgt.vx, vmand.mm, vrgather.vx-with-diag-mask).

The Spike difftest still reports `success: true` (because Spike's
shadow state matches the DUT's broken state) but the per-cell C-side
verifier in `benchmark_instructions` reports row-1+ data corruption.
Reverting to a wide-port BRAM (Phase 3b style, in the same file's git
history) restores 68/70 PASS (the 2 fails are pre-existing `vmv.x.s`
known issues).

Goal: keep the narrow port (4-tile BRAM footprint) AND make the FSM
correct under the actual workload timing.

## 1. Kickoff prompt for codex

```
Read fyp_doc/maskunit_narrow_bram_debug.md end-to-end, then:

1. Read t1/src/mask/MaskUnitFpga.scala lines ~140-380 (Opt 3 revised v2
   block).
2. Read t1/src/mask/MaskUnit.scala lines 194-336 (the original FF-backed
   v0Vec write/read code that this BRAM design replaces).
3. Run the failing regression:
     T1_MIRROR_RTL_WRITES=1 bash run-test.sh vision_task.benchmark_instructions \
       -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt \
       --max-cycles 50000000
   Confirm you see "FAIL (57/70 checks passed, 13 failed)" (or similar
   number — exact count varies by which writes get lost).
4. Identify the timing / correctness bug. The known hypotheses are
   listed in section 5 below; consider them but don't take them as
   gospel — measure with waveforms or RTL prints if needed.
5. Land a fix. Acceptance criteria:
   - 68/70 PASS on benchmark_instructions (i.e. only the 2 pre-existing
     vmv.x.s H+V fails remain).
   - vision_task.simple_instruction_asm passes Spike difftest (it
     emits 127 MISMATCH lines from a known-buggy C verifier formula —
     ignore those; `"success": true` is what matters).
   - Synth on mudkip2d128big1bram1chain2lanescale_fpga_maskopt shows
     v0_bram dropping from 16 BRAM36 (wide) to ~4 BRAM36 (narrow).

If the FSM-based narrow approach fundamentally races, propose an
alternative design (e.g. lane-side back-pressure, or accept wide BRAM
but compensate BRAM tiles elsewhere). Document the chosen approach in
fyp_doc/maskunit_fpga_handoff.md.
```

## 2. Background — what the design is doing

### 2.1 T1 + the time-multiplex grid

The T1 vector core elaborated for `mudkip2d128big1bram1chain2lanescale_fpga_maskopt`
has these parameters (verify in `designs/org.chipsalliance.t1.elaborator.t1.T1.toml`):

| Param | Value | Source |
|---|---:|---|
| vLen | 1024 bits | --extensions zvl1024b |
| dLen | 128 bits | --dLen 128 |
| eLen | 32 bits | --extensions zve32x |
| laneScale | 2 | --laneScale 2 |
| **datapathWidth** | **64 bits** | = laneScale × eLen |
| **laneNumber** | **2** | = dLen / datapathWidth = 128 / 64 |
| vrfOffsetBits | 3 bits | = log2(vLen / datapathWidth / laneNumber) = log2(8) |
| baseLMUL | 1 | --baseLMUL 1 |
| **timeMultiplexBatch** | **128** | = vLen × baseLMUL / 8 / numRows = 1024/8/1 |
| numRows | 1 | --rowNumber 1 |

**Important: laneNumber is 2, not 8.** Earlier narrow-BRAM attempts
(v1) implicitly assumed laneNumber=8 (chunkWidth=256 ≈ 8 lanes × 32 bit
datapath). The v2 attempt generalises but still fails.

### 2.2 v0 mask register layout

`v0` is a vLen=1024-bit mask register. The HW maintains
`timeMultiplexBatch` = 128 copies of v0, one per image-row batch
(indexed by `gatherRowCounter`). The original storage is FF-backed:

```scala
// MaskUnit.scala:194-198
val v0Vec: Vec[Vec[UInt]] = RegInit(
  VecInit(Seq.fill(parameter.timeMultiplexBatch)(
    VecInit(Seq.fill(parameter.vLen / parameter.datapathWidth)(0.U(parameter.datapathWidth.W)))
  ))
)
val v0: Vec[UInt] = v0Vec(gatherRowCounter)
```

= 128 rows × 16 datapath chunks × 64 bits = 131,072 FFs at vLen=1024.
Plus a 128:1 row mux. Total ~70k LUTs / 142k FFs at synth (the whole
reason Phase 3 exists).

Each row's v0 is divided into `chunksPerRow = vLen / datapathWidth =
16` datapath chunks of `datapathWidth = 64` bits each. The mapping
from (lane, offset) to datapath-chunk index is:

```scala
// MaskUnit.scala:325-336
v0Vec(gatherRowCounter).zipWithIndex.foreach { case (data, index) =>
  val laneIndex: Int = index % parameter.laneNumber   // 0..1
  val offset:    Int = index / parameter.laneNumber   // 0..7
  // index = lane + offset * laneNumber
  // data covers v0 bits [index*64 : index*64 + 63]
  val v0Write = v0UpdateVec(laneIndex)
  val maskExt = FillInterleaved(8, v0Write.bits.mask)
  when(v0Write.valid && v0Write.bits.offset === offset.U) {
    data := (data & (~maskExt).asUInt) | (maskExt & v0Write.bits.data)
  }
}
```

So lane 0 at offset 3 writes datapath chunk 6 = v0 bits [384:447].
Lane 1 at offset 3 writes datapath chunk 7 = v0 bits [448:511].

### 2.3 The narrow-BRAM v2 design (current in-tree)

`t1/src/mask/MaskUnitFpga.scala` Opt 3 revised v2 section
(`val chunkWidth: Int = ...` through the sRefill `when` block):

* `chunkWidth = max(datapathWidth × laneNumber, 64) = max(128, 64) = 128`
* `numChunks = vLen / chunkWidth = 8`
* `dpChunksPerBramChunk = chunkWidth / datapathWidth = 2`

Each BRAM chunk holds 2 datapath chunks = ALL laneNumber lanes at ONE
offset (with this chunkWidth choice). Mapping is:

| BRAM chunk c | dpIdx range | (lane, offset) |
|---:|:---|:---|
| 0 | 0..1 | (0,0), (1,0) |
| 1 | 2..3 | (0,1), (1,1) |
| ... | ... | ... |
| 7 | 14..15 | (0,7), (1,7) |

So BRAM chunk c = (all lanes, offset = c). Clean 1:1 between BRAM
chunks and v0Update offsets.

**Storage:** BRAM_DEPTH = 128 rows × 8 chunks per row = 1024 entries
of 128 bits each. Capacity = 128 Kib, Vivado-inferred tile count is
2-4 BRAM36 by capacity+width (vs 16 BRAM36 in the wide design).

**FSM:** 3 states (sIdle, sWriteback, sRefill). On
`gatherRowCounter` change:

1. sWriteback: 8 cycles to drain `v0CacheChunks` to BRAM, one chunk per
   cycle.
2. sRefill: 9 cycles (8 issues + 1 capture-only tail) to load NEW row
   from BRAM into `v0CacheChunks`. BRAM has READ_LATENCY_B=1.

**Pending-write buffer:** `pendingChunk{Data,Strb}` accumulates lane
v0Updates that arrive while `!cacheRowMatch` (cache holds OLD row but
gatherRowCounter has advanced to NEW row). These are merged into
`v0CacheChunks` at refill-capture time + a final-cycle drain for chunks
captured earlier in the same refill.

## 3. Reproduction

### 3.1 Build + run

```sh
cd /home/cbt22/code/code_fyp/VisionSoC

# 1. Build RTL for the maskopt config:
bash build_rtl.sh -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt

# 2. Run the failing regression:
T1_MIRROR_RTL_WRITES=1 bash run-test.sh \
  vision_task.benchmark_instructions \
  -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt \
  --max-cycles 50000000
```

Expected output (current broken state):

```
=== benchmark_instructions summary: FAIL (57/70 checks passed, 13 failed) ===
[CHECK] FAIL vmseq.vv (H): 256 errors; first at [0][0] got 0 exp 1
[CHECK] FAIL vmsle.vv (H): 506 errors; first at [0][0] got 0 exp 1
[CHECK] FAIL vmsgt.vx (H): 256 errors; first at [0][0] got 1 exp 0
[CHECK] FAIL vmslt.vv (H): 506 errors; first at [0][1] got 1 exp 0
[CHECK] FAIL vmand.mm (H): 2 errors; first at [0][0] got 0 exp 1
[CHECK] FAIL vrgather.vx (diag mask) (H): 252 errors; first at [1][0] got 6 exp 1
[CHECK] FAIL vmv.x.s (H): 1 errors; first at [0][0] got 0 exp 5
[CHECK] FAIL vmsle.vv (V): 254 errors; first at [1][126] got 1 exp 0
[CHECK] FAIL vmsgt.vx (V): 256 errors; first at [0][0] got 1 exp 0
[CHECK] FAIL vmslt.vv (V): 254 errors; first at [1][126] got 1 exp 0
[CHECK] FAIL vmand.mm (V): 2 errors; first at [0][0] got 0 exp 1
[CHECK] FAIL vrgather.vx (diag mask) (V): 252 errors; first at [0][1] got 6 exp 1
[CHECK] FAIL vmv.x.s (V): 1 errors; first at [0][0] got 0 exp 10
```

Expected baseline (wide BRAM revert): 68/70 PASS, only the 2
`vmv.x.s` fails remain (those are a pre-existing C-verifier formula
bug in the test, not an HW regression).

### 3.2 Test ignores Spike difftest's `success: true`

`run-test.sh` reports `[SUCCESS] SIMULATION PASSED` based on Spike
difftest. That's true here — Spike and DUT agree on (broken) state.
The per-cell C-side check in `tests/vision_task/benchmark_instructions
/benchmark_instructions.c` is the real correctness oracle, and that
reports the FAIL summary line above.

### 3.3 Bisect points already done

| Variant | Result |
|---|---|
| Wide BRAM (Phase 3b orig) + Phase 3c | 68/70 PASS |
| Wide BRAM + 3c + 3d + 3e | 68/70 PASS |
| Narrow BRAM v1 (chunkWidth=256, assumed laneNumber=8) | 11 fails |
| Narrow BRAM v2 (chunkWidth=128, generalized mapping) | 13 fails |
| Wide BRAM + 3c + 3d (3e reverted) | same fails — proves 3d/3e are NOT the bug |
| Narrow BRAM v1 + cacheRowMatch-gated direct write | same fails — gating alone insufficient |

So the bug is unique to the narrow-BRAM chunked-FSM block. Phases
3c (per-lane mask slicer) and 3d (per-lane writeBitMask slicer) and 3e
(unified slide barrel shifter via reverse trick) are all fine.

## 4. Symptom pattern — what the data tells us

* All failing tests are **mask-producing** instructions (vmseq, vmslt,
  vmsle, vmsgt, vmand). These write v0 via the `v0Update` interface.
* All failures are at **row 1+ columns** (the very first row often
  has correct data — startup refill from zero-initialised BRAM happens
  to be benign because v0 starts at zero anyway).
* `vrgather.vx (diag mask)` failing is a strong hint: that test reads
  v0 as input mask. If v0 reads return stale OLD-row data during a row
  transition window, the gather slice is wrong.
* The H/V mode pattern: e.g. `vmseq.vv (H)` fails but `vmseq.vv (V)`
  does NOT fail. Vertical mode may avoid row transitions or feed v0
  reads via the `verticalMode` IO path (the MaskUnit IO has a
  `verticalMode: Bool` input that controls some access paths).

## 5. Hypotheses for the bug

Listed in order of "Claude's current best guess":

### H1 — Lane writes during the 9-cycle transition can target multiple rows in sequence

The replayFSM's inter-row gap may be SHORTER than 9 cycles, OR
gatherRowCounter may advance MORE THAN ONCE while a transition is in
flight. In that case:

* sWriteback captures `refillTargetRow := gatherRowCounter` at trigger
  cycle. If gatherRowCounter changes during the 17-cycle
  (writeback+refill) window, my FSM only tracks ONE target row.
* Subsequent lane writes for the SECOND new row pile into the same
  pending buffer that was supposed to merge with the FIRST new row's
  refill data.

The 1-cycle Phase 3b refill is short enough that this can't happen;
the 17-cycle v2 transition is long enough to expose it.

### H2 — v0Cache reads during transition return stale OLD-row data

Consumers of `v0` (= `v0CacheChunks.asUInt`) read combinationally. While
the FSM is in sWriteback/sRefill, v0Cache holds OLD row data until
each chunk is overwritten in sRefill's capture cycles. If a downstream
consumer (laneMaskInput, slide shifter, writeMaskForMaskPipe, etc.)
reads v0 during this window, it sees a mix of OLD and partially-loaded
NEW row bytes.

The MaskUnit IO has no flow-control on the v0 read path
(`askMaskVec`/`laneMaskInput` are plain Input/Output, not Decoupled).
So we can't back-pressure consumers from MaskUnit alone.

### H3 — Pending-buffer accumulation has a same-cycle race with refill capture

The pending writes for chunk c are accumulated each cycle via
`pdata := applyByteWrite(pdata, ...)` AND
`pstrb := pstrb | ...`. At the refill-capture cycle for chunk c, the
code reads `pendingChunkStrb(c)` (pre-update value) for the merge and
ALSO clears it via `pendingChunkStrb(c) := 0.U`.

Chisel last-connect-wins should make this OK (clear wins, no
double-apply), but a lane write firing AT the capture cycle goes into
pending data via the accumulator AND into the capture merge via the
`curStrb/curData` path. The merge is correct, but the pendingChunkData
register update at next cycle holds the cycle-T write data with strb=0
— harmless because strb gates the merge.

This is the LEAST likely culprit since the math checks out, but worth
re-verifying with a quick waveform.

### H4 — BRAM read latency mishandled

The BRAM (`fpga/wrapper/v0_bram.v` + `t1/resources/v0_bram.sv`) is
configured with `READ_LATENCY_B = 1`. The narrow v2 FSM issues a read
at cycle k, captures at cycle k+1. Verify the behavioural model in
`t1/resources/v0_bram.sv` actually matches the 1-cycle latency
(Verilator simulation uses this; Vivado synth uses the XPM version
which definitely is 1-cycle).

If the behavioural model is broken or has a different latency, NEW
rows refill garbage data and any subsequent v0 read sees garbage.

## 6. Files involved

| File | What's there |
|---|---|
| `t1/src/mask/MaskUnitFpga.scala` | The narrow BRAM v2 implementation (Opt 3 revised v2 block, lines ~140-380). Also has Phase 3c per-lane laneMaskInput slicer, Phase 3d per-lane writeBitMask slicer, Phase 3e unified slide barrel shifter via reverse trick — all known good. |
| `t1/src/mask/MaskUnit.scala` | The ORIGINAL FF-backed v0Vec design. Unmodified. Use as semantic reference. |
| `t1/resources/v0_bram.sv` | Verilator behavioural model of v0_bram. 1-cycle read latency, byte-write port A. |
| `fpga/wrapper/v0_bram.v` | Vivado XPM xpm_memory_sdpram wrapper. Same external behaviour as the .sv model. |
| `t1/src/T1.scala:556` | Conditional instantiation: `if (parameter.useFpgaMaskUnit.getOrElse(false)) MaskUnitFpga else MaskUnit`. |
| `t1/src/Bundles.scala:296-302` | `V0Update` bundle: `data: UInt(datapathWidth.W)`, `offset: UInt(vrfOffsetBits.W)`, `mask: UInt((datapathWidth/8).W)`. |
| `t1/src/Bundles.scala:706-718` | `MaskUnitInstReq`, `maskPipeRequest` bundles fed into MaskUnit. |
| `fyp_doc/maskunit_fpga_handoff.md` | Higher-level Phase 3 plan (§ 4 covers Opt 3 etc.). |
| `fyp_doc/fpga_build_status.md § 0.13` | Build status running log with synth numbers. |
| `~/.claude/projects/-home-cbt22-code-code-fyp-VisionSoC/memory/` | Claude memory entries (relevant: `project_chisel_blackbox_xpm_split.md`, `project_xpm_memory_sdpram_ports.md`). |

## 7. Configs + commands cheat-sheet

| Need | Command |
|---|---|
| Build RTL | `bash build_rtl.sh -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt` |
| Run sim (need T1_MIRROR_RTL_WRITES=1 for vert-mode kernels) | `T1_MIRROR_RTL_WRITES=1 bash run-test.sh <test> -c <config> --max-cycles 50000000` |
| Quick smoke test | `vision_task.simple_instruction_asm` (single instruction, fast) |
| Mask regression | `vision_task.benchmark_instructions` (70 sub-tests, ~7 min wall) |
| Vert-LSU regression | `vision_task.simple_instruction_vert_lsu` |
| FPGA synth-only | `bash fpga/system/build_fpga.sh -c <config> -s -a` (~25-30 min wall) |
| Inspect synth utilization | `fpga/build/<dir>/utilization_synth.rpt` |
| Inspect a test's run log | `test_output/<config>/<test>-<ts>/run.log` |

The maskopt config is `mudkip2d128big1bram1chain2lanescale_fpga_maskopt`
(uses `--useFpgaMaskUnit true`). The non-maskopt `_fpga` config picks
the original MaskUnit (no narrow BRAM).

## 8. Out of scope / non-goals

* Don't modify `t1/src/mask/MaskUnit.scala` — it's the canonical
  simulator path and must stay byte-identical.
* Don't change the `MaskUnitInterface` IO bundle — downstream T1
  wiring depends on it.
* Don't pull in big architectural refactors (e.g. moving v0 into
  SharedVRF). The scope here is the v0_bram + cache + FSM block.
* If a fix requires lane-side back-pressure, propose it but don't
  silently land it — the Lane.scala / MaskExchangeUnit.scala paths
  are higher-impact and need user review.

## 9. What success looks like

* `benchmark_instructions`: 68/70 PASS (the two `vmv.x.s` fails are
  pre-existing).
* `simple_instruction_asm`: Spike difftest `success: true`,
  `total_cycles=21514` (the 127 MISMATCH lines from the buggy C
  verifier are expected and not a failure of the narrow-BRAM code).
* `simple_instruction_vert_lsu`: PASS (this exercises vertical-mode v0
  access, which the H/V failure pattern suggests is the safer path).
* Synth on `mudkip2d128big1bram1chain2lanescale_fpga_maskopt -s -a`:
  v0_bram BRAM36 tile count drops from 16 (wide) to ≤ 8 (narrow).
* Document in `fyp_doc/maskunit_fpga_handoff.md` what fix landed and
  why.
