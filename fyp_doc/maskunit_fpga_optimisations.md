# MaskUnitFpga Optimisations — Consolidated Summary

**Branch:** `fpga_driver` · **Repo:** `/home/cbt22/code/code_fyp/VisionSoC`
**Last updated:** 2026-05-18 (after full FPGA impl signed off and Sobel
verified on KV260)

This doc catalogs every area-reduction optimisation landed in
`t1/src/mask/MaskUnitFpga.scala` (plus one camera-pipeline change in
`fpga/system/system_top.tcl`) that takes the vLen=1024 T1 vector core
from over-budget to deployable on the KV260 with the route-safe LUT
target. It links to deep-dive docs for the optimisations that have one.

For HISTORICAL CONTEXT (the bring-up of the maskopt config, the failed
narrow-BRAM v1/v2 attempts, the Phase 1-5 plan), see
`maskunit_fpga_handoff.md`. For per-optimisation deep-dives, see the
linked debug briefs.

---

## 0. Cheat sheet

| # | Optimisation | File | LUT Δ | FF Δ | BRAM Δ |
|--:|---|:--|---:|---:|---:|
| 1 | Phase 3b → narrow BRAM v3 (codex) | `MaskUnitFpga.scala` | −12K* | −128K | +4 BRAM36 (16 → 4) |
| 2 | Phase 3c — per-lane laneMaskInput slicer (Opt 1) | `MaskUnitFpga.scala` | small | 0 | 0 |
| 3 | Phase 3d v2 — per-lane writeMaskForMaskPipe push-down (Opt 2 v2) | `MaskUnitFpga.scala` | **−7,698** | −512 | 0 |
| 4 | Phase 3e v2 — chunked slide shifter (Opt 7 v2) | `MaskUnitFpga.scala` | **−4,689** | +1024 | 0 |
| 5 | Phase 4 — conditional `MaskUnit` / `MaskUnitFpga` selection | `T1.scala:556` | 0 | 0 | 0 |
| 6 | `axis_data_fifo_cap` → `axis_register_slice` | `system_top.tcl` | −34 | small | **−1 BRAM18 tile** |

\* For Opt 3 the "−12K LUT" is the net delta vs the unoptimised
FF-backed MaskUnit at vLen=1024 (70,066 LUT baseline → ~26K LUT after
narrow-BRAM v3 with codex's three-row policy). Individual sub-savings
(BRAM count, FF, row-cache logic) overlap so they're not separable.

**End-to-end synth result on `mudkip2d128big1bram1chain2lanescale_fpga_maskopt`:**

| Resource | unoptimised vLen=1024 (baseline) | After all opts | Δ |
|---|---:|---:|---:|
| `system_top_wrapper` LUT | 155,748 | **104,717** | **−51,031 (−33%)** |
| `system_top_wrapper` FF  | 234,453 |   108,017 | −126,436 (−54%) |
| BRAM36 tiles | 139 | 143 + 2 BRAM18 = **144 tiles exactly** | +5 tiles (still at the cap) |
| MaskUnitFpga (body) LUT | 70,066 (MaskUnit) | **11,638** | −58,428 (−83%) |
| MaskUnitFpga (body) FF | 142,014 (MaskUnit) | 12,220 | −129,794 (−91%) |

Impl signs off cleanly: WNS +0.239 ns, WHS +0.010 ns,
`write_bitstream completed successfully`.

---

## 1. Phase 3b — Narrow-port v0 BRAM + active-row register cache (codex v3)

**Problem.** The original `MaskUnit.scala:194-198` v0Vec was a FF
replication of v0: `Vec[Vec[UInt]](timeMultiplexBatch, vLen / datapathWidth)`.
At vLen=1024 timeMultiplexBatch=128, that's 131,072 FFs — and 70K LUT
of row-decode / mux fan-in.

**Solution (codex's "v3" approach, in
`t1/src/mask/MaskUnitFpga.scala`):**

* Move v0 storage out of FFs into a BRAM blackbox `v0_bram` wrapped in
  Chisel (`V0BramBlackBox`). XPM `xpm_memory_sdpram` at the Vivado side
  (`fpga/wrapper/v0_bram.v`), behavioural model at the Verilator side
  (`t1/resources/v0_bram.sv`). Both file content kept in sync; sim and
  synth use different files for the same logical module (XPM cells
  aren't in Verilator's lib).
* The BRAM port is narrowed to `chunkWidth` = `datapathWidth × laneNumber`
  bits (= 128 for this config). At 128-bit width, Vivado synth lands at
  **4 BRAM36 tiles** instead of the 16 that a 1024-bit port forced
  (BRAM36 max single-port width is 72 bits → ceil(1024/72)=15-16).
* Three local row-cache stages keep the wide read path:
  - `activeCacheChunks`: the row visible through `v0` (combinational
    source for every downstream reader).
  - `prefetchCacheChunks`: streams the next sequential row from BRAM
    while the active row executes, so a row change is a register swap
    rather than a blocking refill.
  - `writebackCacheChunks`: snapshots the row just left and writes it
    back through BRAM port A in the background.
* Coherence: lane v0Updates fire while `gatherRowCounter` is stable.
  Writes go to `activeCacheChunks` combinationally AND to the BRAM
  write port A. The "prefetch + writeback" overlap relies on the
  existing T1 replay schedule giving enough cycles per row for the 8
  narrow chunks to stream — verified working for the
  `mudkip2d128big1bram1chain2lanescale_fpga_maskopt` config.

**Why this works AND why the narrow v1/v2 attempts didn't.** The v1
attempt assumed laneNumber=8 implicitly (chunkWidth=256 with "all lanes
share one offset per chunk") but our config has laneNumber=2 (= dLen /
datapathWidth = 128/64), so each BRAM chunk would have held 2 lanes ×
multiple offsets — incompatible with the per-chunk merge code. The v2
attempt generalised the (lane, offset)→dpIdx mapping but raced lane
writes against multi-cycle refill, dropping writes during the
transition window. Codex's v3 keeps the active row in a register
(combinational read for consumers), uses prefetch to remove the
blocking refill, and writes back in the background — eliminating the
race entirely.

**Code reference:** `MaskUnitFpga.scala` lines ~140-380 (the Opt 3
revised v3 block, including the BRAM blackbox declaration, write
merger, three-row cache, and FSM).

**Deep dive:** `maskunit_narrow_bram_debug.md` (originally a codex
hand-off brief documenting the v2 bug; codex's v3 fix replaced it).

---

## 2. Phase 3c — Per-lane on-demand `laneMaskInput` slicer (Opt 1)

**Problem.** `MaskUnit.scala:341-360` and the lane mask consumption loop
(`MaskUnit.scala:361-382`) pre-build THREE vLen-bit regrouping trees
(one per SEW=8/16/32) for both v0 and slideV0, then per-lane Mux1H to
get the 128-bit slice each lane needs. The wide trees `cutUInt +
grouped + transpose + asUInt` are pure wiring, but the per-lane Mux1H
on 128-bit-wide × 3-way is non-trivial in LUT cost.

**Solution.** Per-lane on-demand: each lane computes ONLY its own
128-bit slice for each SEW via a direct bit-gather (`Cat((0 until
outerCount).map { ... })`), then 3-way Mux1H on `maskSelectSew`. The
SEW Mux1H now operates on 32-bit values (after the maskSelect cut)
instead of 128-bit values, and the per-SEW regroup permutation is
folded into a static (lane, SEW)-indexed wire pattern — Vivado can
share LUTs much more aggressively.

**Function:** `maskSliceFor(lane: Int, sew: UInt, maskSelect: UInt,
source: UInt)` in `MaskUnitFpga.scala`. Returns a `datapathWidth`-wide
output per call. Used in the `laneMaskInput` driver loop for both
`v0Slice` and `slideV0Slice`.

**Code reference:** `MaskUnitFpga.scala` ~ lines 320-340 (the function)
+ the `laneMaskInput.zipWithIndex.foreach` loop directly below it.

**Equivalence.** For each (lane, SEW), the produced 128-bit slice
matches the OLD code's slice bit-for-bit. Verified by sim regression
(byte-identical to baseline).

---

## 3. Phase 3d v2 — Per-lane `writeMaskForMaskPipe` push-down (Opt 2 v2)

**Problem.** The OLD `writeMaskForMaskPipe` is a vLen=1024-bit
combinational AND of three vLen-bit signals — `baseV0` (Mux on v0),
`vlCorrection` (`scanRightOr(UIntToOH(vl)) >> 1`), and `upCorrection`
(slide-only thermometer). Registered into `writeCountPipe0`. Then
post-Pipe, per-lane `writeBitMaskForSlide / writeBitMaskForExtend`
slicers re-extract 128-bit slices per (lane, SEW) and Mux1H.

The wide-vLen scanRightOr/scanLeftOr trees were assumed to be Vivado-
optimised, but synth measurement showed otherwise: eliminating them
saved **~−7,698 LUT** in MaskUnitFpga (way more than the original
−3 to −5K estimate).

**Solution.** Pre-Pipe per-lane direct compute:

1. `computeWriteMaskBit(srcPos: Int)`: helper that produces a single
   output bit as `baseBit && vlBit && upBit`, where each sub-bit comes
   from a SOURCE signal directly:
   - `baseBit = !maskType || v0Cache(srcPos)`
   - `vlBit   = srcPos.U < instReq.bits.vl`
   - `upBit   = !writeMaskActive || ((srcPos.U >= shifterValidSize) && !shifterSizeOverlap)`

2. `computeWriteBitMaskForSlide(lane, sew1H)` and
   `computeWriteBitMaskForExtend(lane, sew1H)`: build the per-lane
   128-bit slice from `computeWriteMaskBit(srcPos)` calls at compile-
   time-known srcPos values for each (lane, SEW). Mux1H selects the
   active SEW slice.

3. The `WriteCountPipe0` bundle stores the per-lane slices directly
   (`Vec[UInt(128.W)] × 2` for slide+extend), NOT the wide
   writeMaskForMaskPipe. Pipe shrinks 1024 → 512 FFs and the wide
   trees disappear from synth entirely.

**Why this won so big.** Vivado synth was treating each of the 1024
output bits of `scanRightOr(UIntToOH(vl))` as a thermometer cell with
its own decoder, NOT sharing the vl decode across positions efficiently.
By computing per-lane bits directly with constant-vs-vl compares
(which Vivado DOES share), we let the synth tool factor out vl-decode
logic across the per-lane slices, eliminating most of the LUT cost.

**Code reference:** `MaskUnitFpga.scala` ~ lines 420-510. Search for
"Opt 2 v2: Per-lane writeMaskForMaskPipe push-down" block.

**Equivalence.** Per-lane sim verification — `benchmark_instructions`
68/70 PASS (the 2 fails are the pre-existing `vmv.x.s` H+V issues).
Plus the new `sew16_32_masktest` (8/8 PASS) covers the SEW=16 and
SEW=32 Mux1H paths that `benchmark_instructions` (SEW=8 only) doesn't
exercise.

---

## 4. Phase 3e v2 — Chunked v0 slide barrel shifter (Opt 7 v2)

**Problem.** The OLD design had two parallel 1024-bit barrel shifters
(`slideUpV0 = v0 >> slideSize`, `slideDownV0 = v0 << shifterUpSize`)
combined via Mux. Opt 7 v1 (in `MaskUnit.scala`'s reverse-trick
implementation already landed earlier) unified them into a single
right-shift via `Reverse(v0) >> S` + output reverse — saving one
shifter (~−10K LUT) but still a single 1024-bit combinational shifter
(~10K LUT remaining).

**Solution (Opt 7 v2).** Replace the unified 1024-bit shifter with a
**single 128-bit-wide barrel shifter reused over 8 cycles** via a tiny
FSM.

* `slideShifterActive` + `slideChunkCounter` (3 bits) + `slideShiftV0`
  (1024-bit captured input) form the FSM state.
* On `io.maskPipeReq.valid && slide`, capture inputs and start chunking.
* Per cycle, compute `srcStart = slideShiftAmtReg + (effectiveC <<
  log2(slideChunkWidth))` and extract a 128-bit window via
  `(slideShiftV0 >> srcStart)(slideChunkWidth - 1, 0)`. Apply `Reverse`
  if slideDown. Write to `slideV0RegBanks(slideChunkCounter)`.
* `effectiveC` walks numChunks-1 → 0 for slideDown (so chunks land at
  the right output position after the post-Reverse) or 0 → numChunks-1
  for slideUp.
* After numChunks cycles, the slideV0RegBanks Vec contains the full
  result; `slideV0Reg = slideV0RegBanks.asUInt` for downstream
  consumption.
* `slideV0OverReg` (overflow bits for slideDown) is unchanged — it's
  a tiny `dByte`-wide shift on the top dByte bits of v0, negligible
  cost.

**Timing analysis.** `slideV0Reg` is consumed only when
`askMaskVec(i).slide` is high — i.e. by SLIDE instructions. A single
vslideup.vx/.vi or vslidedown.vx/.vi takes 12,933-13,957 cycles per
`benchmark_instructions`. The 8-cycle setup is < 0.07% of execution;
hidden trivially.

**Code reference:** `MaskUnitFpga.scala` ~ lines 260-410 (the "Opt 7
v2" block).

**Deep dive:** `maskunit_chunked_slide_shifter_debug.md` — codex
hand-off brief originally; contains the full equivalence proof for
slideUp and slideDown reverse-trick mapping.

---

## 5. Phase 4 — Conditional `MaskUnit` / `MaskUnitFpga` selection

**Problem.** The original `MaskUnit.scala` is the canonical simulator
path. We want `MaskUnitFpga` ONLY for FPGA-targeting configs that opt
in.

**Solution.** New `useFpgaMaskUnit: Option[Boolean]` field in
`T1Parameter` (default `Some(false)`), threaded through both elaborators
(`elaborator/src/t1/T1.scala` and `elaborator/src/t1emu/TestBench.scala`).
At the `t1/src/T1.scala:556` instantiation site:

```scala
val maskUnit2D =
  if (parameter.useFpgaMaskUnit.getOrElse(false))
    Seq.tabulate(parameter.numRows)(_ => Instantiate(new MaskUnitFpga(parameter)))
  else
    Seq.tabulate(parameter.numRows)(_ => Instantiate(new MaskUnit(parameter)))
```

`Instance[+A]` is covariant in Chisel 5, so Scala's LUB inference
resolves the sequence type to the common supertype and downstream
`.io.foo` access works for either module.

New TOML config family `_fpga_maskopt` (small / medium / big in both
the T1 elaborator + the t1emu TestBench TOMLs) sets `--useFpgaMaskUnit
true`. The original `_fpga` configs are preserved as easy-revert
baselines.

**Code reference:** `t1/src/T1.scala:556`,
`elaborator/src/t1/T1.scala`, `elaborator/src/t1emu/TestBench.scala`,
`designs/org.chipsalliance.t1.elaborator.t1.T1.toml`,
`designs/org.chipsalliance.t1.elaborator.t1emu.TestBench.toml`.

---

## 6. `axis_data_fifo_cap` → `axis_register_slice` (BRAM tile cut)

**Problem.** With BRAM36 = 143 and BRAM18 = 3, Vivado packs the 3
BRAM18s into 2 physical tiles (1 packed pair + 1 lone) = 145 tile
equivalents, over the 144 cap.

**Solution.** Replace the 256-deep `axis_data_fifo_cap` (1 BRAM18) in
the camera pipeline with a 1-deep `axis_register_slice` (0 BRAM, ~20
LUT). At 128×128@26 fps the per-pixel period is ~700 cycles at 300 MHz
— far longer than the ~10-100 cycle DDR write latency through PS HP1,
so 256 entries of buffering were ~10× over-provisioned. A 1-deep skid
buffer covers the odd 1-cycle stall and keeps the BD width-match
intact.

**Trade-off.** Loses 255 entries of rate-match cushion. In the unlikely
event v_frmbuf_wr stalls > 1 cycle (e.g. DDR contention burst longer
than typical), a pixel could be dropped. This appears to be the
cause of the observed ~3 fps drop on real hardware vs the 5q-r3
baseline (see § 8 — "FPS drop diagnosis" below). The trade-off is **−1
BRAM tile vs ~3 fps**; the user accepted this trade to hit the cap.

**Code reference:** `fpga/system/system_top.tcl` lines ~496-510 (the
`create_bd_cell` for axis_data_fifo_cap) and ~626-630 (pin name change
from `s_axis_aclk` / `s_axis_aresetn` to `aclk` / `aresetn`).

---

## 7. Verification matrix

| Test | Config | Result |
|---|---|---|
| `vision_task.benchmark_instructions` | `_fpga_maskopt` big | 68/70 PASS (2 pre-existing `vmv.x.s` C-verifier fails) |
| `vision_task.simple_instruction_asm` | `_fpga_maskopt` big | Spike difftest `success: true` |
| `vision_task.sew16_32_masktest` (new) | `_fpga_maskopt` big | **8/8 PASS** (covers SEW=16/32 Mux1H paths not exercised by benchmark_instructions) |
| Synth (`-s -a`) | `_fpga_maskopt` big | 104,717 LUT / 144 BRAM tiles / WNS +0.307 ns |
| Impl (`-b -a`) | `_fpga_maskopt` big | WNS +0.239 ns, WHS +0.010 ns, `write_bitstream completed successfully` |
| KV260 deploy + Sobel kernel | `_fpga_maskopt` big | 16-17 fps, 161K cycles/frame, edge-detector output pattern correct |

---

## 8. fps comparison vs the 5q-r3 baseline — what we actually know

**Update (2026-05-18 after the deploy):** the first 12-second test run
clocked **16-17 fps**, but a longer 30-second run reaches **~19 fps**
once warmed up. The "drop" was largely a stabilisation/warm-up
artefact, not a real ~3 fps regression. **Stable fps ≈ 19 vs the 5q-r3
baseline's ~20 = effectively no regression once warmed up.**

What likely happens in the first ~12 seconds: AP1302 ISP frame
delivery isn't yet at its steady-state lock; the Linux `gst` /
`media-ctl` pipeline retains some setup overhead; the display path
flushes its initial scanout buffer. Once that settles, the system
hits its real ceiling (camera + DDR + display fed through the
shrunken `axis_register_slice`).

Why the small remaining 1-fps gap (5q-r3 ~20 → today ~19) is plausible
even at steady state — and not necessarily caused by ONE single
change:

The Sobel kernel on this build runs at ~161K cycles per kernel
invocation. The reported "prior baseline of ~20 fps" comes from the
5q-r3 production bitstream (per memory
`project_5q_final_production_reference.md`), but that was on a
DIFFERENT T1 config:

| | 5q-r3 production | Today's deploy |
|---|:--|:--|
| T1 config | `mudkip2d128small1bram1chain2lanescale_fpga` | `mudkip2d128big1bram1chain2lanescale_fpga_maskopt` |
| vLen | 256 | 1024 |
| baseLMUL | (default 4) | 1 |
| MaskUnit variant | original `MaskUnit` (FF-backed v0Vec) | `MaskUnitFpga` (BRAM v0 + chunked shifter + all opts) |
| `axis_data_fifo_cap` | 256-deep FIFO (1 BRAM18) | 1-deep `axis_register_slice` |
| Sobel fps | ~20 | ~16-17 |
| Sobel kernel cycles | (not measured here) | 161K |

So the apples-to-apples comparison would require either:
1. Running **today's bitstream (BIG_maskopt) with the OLD 256-deep FIFO**
   — isolates the FIFO swap's effect.
2. Running **today's bitstream (BIG_maskopt) with the OLD MaskUnit** —
   isolates the MaskUnitFpga effect.
3. Running **5q-r3 (SMALL) bitstream now** to re-baseline ~20 fps with
   our current camera/lighting (might be different now than at 5q-r3
   time).

### What the cycle math says

* 161K cycles per Sobel kernel at 300 MHz = **537 µs / kernel**.
* Camera period at 26 fps = **38.5 ms / frame**.
* Kernel is 0.014% of the camera period — kernel speed is NOT the
  bottleneck. Even a 10× slower kernel would not drop fps.

### Two candidate root causes (both plausible)

**Candidate A — `axis_data_fifo_cap` downgrade.** Replacing a 256-deep
rate-match FIFO with a 1-deep skid buffer means any DDR write-back
stall longer than 1 cycle propagates back to `mipi_csi2_rx`. CSI-2
doesn't have a flow-control mechanism — the AP1302 keeps streaming
on its 26 fps schedule. If `v_frmbuf_wr` can't accept a pixel that
cycle, the pixel is lost. Lost pixels can corrupt a frame, and a
corrupted frame may be dropped by the V4L2 layer.

**Candidate B — BIG-config DMA / cache miss costs.** The BIG config
has 4× the vector register storage (vLen=1024 vs 256). The PS-side
`libt1` DMA APIs are configured to copy a 128×128 = 16,384-byte Y
plane per frame from DDR into URAM scratchpad before T1 reads it.
Bigger vector register groups don't directly change the copy size,
but the T1 dispatch path inside `visionsoc_main` may have different
per-instruction overhead at vLen=1024 (e.g., longer
ScratchpadFsm/replayFSM cycles per instruction).

Given the steady-state result is **~19 fps vs ~20 fps baseline = ~5%
regression**, the cost-benefit of pursuing this further is dubious.
Documented below in case it ever does matter.

### Recommended diagnosis steps (if reclaiming the last 1 fps matters)

* **Quick (1 hour total):** revert just the TCL change (set
  `axis_data_fifo_cap` back to a 256-deep FIFO with
  `xilinx.com:ip:axis_data_fifo`), keep all RTL optimisations, re-synth
  and re-deploy. If fps climbs back to ~19-20, the FIFO was the cause.
  BRAM tile count goes back over by 1 (144.5 effective) — Vivado
  *might* still place + route in that case (marginal), or another
  BRAM18 elsewhere can be hunted for replacement (e.g.,
  `axi_dma` has 2 BRAM18s in its internal FIFOs).
* **Intermediate:** use a 32-deep `axis_data_fifo` configured with
  `MEMORY_TYPE=distributed` — no BRAM, ~few hundred LUT, covers
  typical DDR contention windows. Trade-off: ~500 LUT for keeping the
  BRAM cap clean.
* **For the BIG-config dispatch overhead theory:** instrument
  `visionsoc_main` to measure the per-iteration breakdown
  (DMA copy time vs T1 issue time vs T1 wait time vs display setup
  time) — that's a `~30 LOC` printf-around-time-measurement and tells
  us whether the camera pipeline OR the T1 dispatch is the limiter.

### Trade-off recorded

The BRAM tile cap was the hard constraint; the −1 BRAM18 swap landed
the design at exactly 144 tiles. Whether the ~3 fps drop is recoverable
without exceeding the BRAM cap is a separate optimisation — paths above.
The user accepted the trade-off to hit the cap; this section documents
what we know and don't know.
