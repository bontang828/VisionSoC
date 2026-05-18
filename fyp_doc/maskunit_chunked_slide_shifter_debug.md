# MaskUnitFpga — Chunked Slide Barrel Shifter (Opt 7 v2) — codex brief

**Branch:** `fpga_driver`
**Repo:** `/home/cbt22/code/code_fyp/VisionSoC`
**Date this brief written:** 2026-05-18 BST
**Owner of last touch:** Claude (Opus 4.7)

## 0. TL;DR for codex

The current MaskUnitFpga uses a UNIFIED 1024-bit barrel shifter (via the
reverse-trick: `Reverse((Reverse(v0) >> S))` for slide-down, plain `v0 >> S`
for slide-up) — this is "Opt 7 v1" and currently costs ~10K LUTs of the
24K-LUT MaskUnit body.

The next-tier optimization "Opt 7 v2" — and the biggest remaining single
LUT lever — is to replace that single combinational 1024-bit shifter with
a **128-bit-wide barrel shifter reused 8 cycles** for the 8 chunks of the
slide result. Estimated savings: **~8K LUTs** (10K → ~2K combinational
shifter), at the cost of ~8 extra cycles of slide-setup latency that the
existing multi-thousand-cycle slide execution absorbs trivially.

Goal: bring system_top_wrapper LUT from **117,104 → ~109K** (still 4K over
the user's 105K route-safe target, but a major step toward it).

## 1. Kickoff prompt for codex

```
Read fyp_doc/maskunit_chunked_slide_shifter_debug.md end-to-end, then:

1. Read t1/src/mask/MaskUnitFpga.scala lines ~260-365 (the "Opt 7"
   block — unified reverse-trick barrel shifter producing slideV0Reg
   and slideV0OverReg combinationally on `io.maskPipeReq.valid && slide`).
2. Implement Opt 7 v2: a chunked FSM that builds slideV0Reg over 8
   cycles using a single 128-bit barrel shifter. Detailed design in
   section 4 of this brief.
3. Verify with the t1emu regression:
     T1_MIRROR_RTL_WRITES=1 bash run-test.sh vision_task.benchmark_instructions \
       -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt \
       --max-cycles 50000000
   Acceptance: 68/70 PASS (the 2 fails are pre-existing vmv.x.s H+V
   C-verifier issues — confirm by comparing to the wide-shifter
   baseline; both should report the same 2 fails and the same passing
   set).
4. Also run the slide-heavy individual tests to confirm cycle-count
   delta is small (slides will gain ~8 cycles setup; per-instruction
   cycle deltas should be <1%):
     T1_MIRROR_RTL_WRITES=1 bash run-test.sh vision_task.simple_instruction_asm \
       -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt --max-cycles 50000000
5. Synth-only check to measure LUT savings:
     bash fpga/system/build_fpga.sh -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt -s -a
   Target: MaskUnitFpga body LUT drops from 24,240 to ~16K. System
   total LUT drops from 117,104 to ~109K.

Document the chosen approach + final synth numbers in
fyp_doc/maskunit_fpga_handoff.md § 0.13.
```

## 2. Current state — what's in tree now

After Phases 3b/3c/3d/3e + Phase 4 + codex's narrow-BRAM v3 fix:

**Synth (Vivado 2025.2 on KV260) for
`mudkip2d128big1bram1chain2lanescale_fpga_maskopt`:**

| Hierarchy | LUT | FF | BRAM36 |
|---|---:|---:|---:|
| `system_top_wrapper` | 117,104 | 106,447 | 143 (+3 BRAM18 +16 URAM) |
| `t1_top/u_t1` | 95,502 | 77,594 | 132 |
| `maskUnit2D_0` (MaskUnitFpga) | 33,360 | 14,051 | 4 |
| `maskUnit2D_0/(body)` | 24,240 | 10,674 | 0 |
| `maskUnit2D_0/v0BramInst` | 271 | 0 | 4 |

The **24,240 LUT in the MaskUnitFpga body** breaks down roughly as
(estimates based on the optimisations applied so far):

* **Slide barrel shifter (Opt 7 v1, unified reverse-trick):** ~10K LUT.
  This is the single biggest contributor and the target of this brief.
* writeMaskForMaskPipe wide trees (`vlCorrection`, `upCorrection`): ~6K
  LUT (untouched).
* Per-lane regroup/slicers for laneMaskInput (Opt 1): ~3K LUT.
* Per-lane writeBitMask slicers (Opt 2): ~3K LUT.
* Gather state machine + compress/extend control: ~2K LUT.

User-stated target: system LUT ≤ 105K for route-safe placement. We are
12K LUT over that target. The chunked slide shifter is the largest
single lever inside MaskUnit that has not been pulled yet.

## 3. Reference: Opt 7 v1 (currently in tree) — what we're replacing

`t1/src/mask/MaskUnitFpga.scala` lines ~263-365 (the "Opt 7: Unified
v0 slide barrel shifter via the reverse trick" block):

```scala
val isSlideDown:  Bool = !slideUp
val shifterInput: UInt = Mux(isSlideDown, Reverse(v0.asUInt), v0.asUInt)
val shiftAmt:     UInt = Mux(isSlideDown, shifterUpSize, slideSize)
val shiftedRaw:   UInt = changeUIntSize((shifterInput >> shiftAmt).asUInt,
                                         parameter.vLen)
val slideV0Enq:   UInt = Mux(isSlideDown, Reverse(shiftedRaw), shiftedRaw)
val slideV0Reg:   UInt = RegEnable(slideV0Enq, 0.U.asTypeOf(slideV0Enq),
                                    io.maskPipeReq.valid && slide)

// Overlap (top shifterUpSize bits of v0 for slide-down)
val v0TopDByte:      UInt = v0.asUInt(parameter.vLen - 1, parameter.vLen - dByte)
val overlapShiftAmt: UInt = (dByte.U - shifterUpSize)
val slideV0Overlap:  UInt = changeUIntSize((v0TopDByte >> overlapShiftAmt).asUInt, dByte)
val slideV0OverReg:  UInt = RegEnable(slideV0Overlap, 0.U(dByte.W),
                                       io.maskPipeReq.valid && slide)

val slideV0: Vec[UInt] = cutUInt(slideV0Reg, parameter.datapathWidth)
```

This is one **vLen=1024-bit-wide barrel shifter** (the `>>` on
`shifterInput`). Its cost dominates the body LUT count.

## 4. Proposed Opt 7 v2 — chunked FSM design

### 4.1 Concept

Instead of a single combinational `shifter[vLen]` evaluated once, use a
single combinational `shifter[chunkWidth]` evaluated 8 times by a small
FSM. Each evaluation computes one 128-bit chunk of slideV0Reg.

Storage:
* `slideV0RegBanks`: `Vec[UInt(chunkWidth.W)]` of `numChunks = 8` banks,
  combined via `.asUInt` for downstream consumption.
* `slideV0OverReg`: unchanged (still computed combinationally from
  `v0TopDByte`, which is a small `dByte=64`-bit slice; cost is
  negligible).

FSM:
* `slideShifterActive`: Bool register; high while chunking is in
  progress.
* `slideChunkCounter`: 4-bit register, counts 0..numChunks-1.
* `slideShiftAmtReg`: register-held shift amount captured at start.
* `slideShiftDirReg`: register-held direction (slideUp vs slideDown)
  captured at start.
* `slideShiftV0`: register-held input (= `v0.asUInt` or
  `Reverse(v0.asUInt)` depending on direction) captured at start. This
  is a 1024-bit register; 1024 FFs but no LUTs.

### 4.2 Per-cycle compute

For chunk `c` (0..7), the output 128 bits of slideV0Reg correspond to
specific positions of the shifted source. After the reverse-trick the
shift is always a **right shift**. So:

```
shiftedFull = slideShiftV0 >> slideShiftAmtReg   // (conceptually wide)
chunkData   = shiftedFull[c*128 +: 128]          // 128-bit window
```

But we don't want to compute `shiftedFull` as a wide combinational
shifter (defeats the point). Instead, combine the per-cycle shift +
window into a single 128-bit-output barrel shifter:

```scala
val srcStart: UInt = slideShiftAmtReg + (slideChunkCounter << log2Ceil(chunkWidth))
val chunkData: UInt = (slideShiftV0 >> srcStart)(chunkWidth - 1, 0)
```

This single expression compiles to a 128-bit-wide barrel shifter on a
1024-bit input with a (log2(vLen) + log2(chunkWidth))-bit dynamic
offset = ~11-bit dynamic offset. Vivado synth cost: ~128 × log2(1024)
≈ 1280 LUTs combinational. Reused for c=0..7 (the shifter logic is
shared across cycles).

### 4.3 Store ordering (reverse trick for slideDown)

For slideUp: chunk c output stored at `slideV0RegBanks(c)`. Direct.

For slideDown: the OLD reverse-trick formula is `Reverse((Reverse(v0)
>> S))`. With chunked output:
* The shifter computes 128-bit windows of `Reverse(v0) >> S`.
* Each window, when reversed, becomes a chunk of the slideDown output.
* The window at offset (c*128 + S) of `Reverse(v0) >> S` corresponds
  (after reverse) to slideDownV0 chunk (numChunks - 1 - c) — the order
  reverses.

Two implementation options:

(a) **Compute slideDown chunks in reverse order:** for slideDown, walk
counter from numChunks-1 down to 0. Compute the same `srcStart`, but
the stored index is `slideChunkCounter` (which now counts down). Apply
`Reverse` to the 128-bit chunk before storing.

(b) **Compute slideDown chunks in forward order with adjusted offsets
and reverse-on-output:** for chunk c of slideDown, the input window
position is `(numChunks - 1 - c) * 128 + S` of `Reverse(v0)`, and the
chunk is `Reverse`-d before storing at `slideV0RegBanks(c)`.

Option (b) keeps the counter monotonic forward and the storeIdx as
`slideChunkCounter`; only the offset and the per-chunk reverse differ.

### 4.4 FSM pseudo-code

```scala
val slideChunkWidth: Int = 128
val slideNumChunks:  Int = parameter.vLen / slideChunkWidth  // 8

val slideShifterActive: Bool = RegInit(false.B)
val slideChunkCounter:  UInt = RegInit(0.U(log2Ceil(slideNumChunks + 1).W))
val slideShiftAmtReg:   UInt = RegInit(0.U(slideSize.getWidth.W))
val slideShiftDirReg:   Bool = RegInit(false.B)  // true = slideDown
val slideShiftV0:       UInt = RegInit(0.U(parameter.vLen.W))
val slideV0RegBanks:    Vec[UInt] =
  RegInit(VecInit(Seq.fill(slideNumChunks)(0.U(slideChunkWidth.W))))

when(io.maskPipeReq.valid && slide) {
  slideShifterActive := true.B
  slideChunkCounter  := 0.U
  slideShiftDirReg   := isSlideDown
  slideShiftAmtReg   := Mux(isSlideDown, shifterUpSize, slideSize)
  slideShiftV0       := Mux(isSlideDown, Reverse(v0.asUInt), v0.asUInt)
}

when(slideShifterActive) {
  // For slideUp: stride forward, no reverse
  // For slideDown: walk reversed offset, apply Reverse to output chunk
  val effectiveC: UInt = Mux(slideShiftDirReg,
                             (slideNumChunks - 1).U - slideChunkCounter,
                             slideChunkCounter)
  val srcStart: UInt   = slideShiftAmtReg +
                         (effectiveC << log2Ceil(slideChunkWidth))
  val rawChunk: UInt   = (slideShiftV0 >> srcStart)(slideChunkWidth - 1, 0)
  val finalChunk: UInt = Mux(slideShiftDirReg, Reverse(rawChunk), rawChunk)

  slideV0RegBanks(slideChunkCounter) := finalChunk
  slideChunkCounter := slideChunkCounter + 1.U
  when(slideChunkCounter === (slideNumChunks - 1).U) {
    slideShifterActive := false.B
  }
}

val slideV0Reg: UInt = slideV0RegBanks.asUInt
val slideV0: Vec[UInt] = cutUInt(slideV0Reg, parameter.datapathWidth)
```

### 4.5 Timing analysis — when do consumers read slideV0Reg?

`slideV0Reg` is consumed by `laneMaskInput` (per-lane mask delivery
to the lanes). Looking at the lane-side mask consumption:

```scala
input := Mux(askMaskVec(index).slide, slideResult, nonSlideResult)
```

`askMaskVec(index).slide` is high only for SLIDE instructions. For
non-slide ops, slideV0Reg is not consumed (the Mux picks
`nonSlideResult`). So Opt 7 v2's 8-cycle latency only matters for
slide instructions.

For slide instructions: looking at `benchmark_instructions`, a single
`vslideup.vx`/`.vi` takes 12,933 - 13,957 cycles (per the existing
cycle counts in the per-test report). The 8-cycle setup is < 0.07%
overhead. Hidden trivially.

## 5. Correctness equivalence — what to verify

For each direction × shift amount, the per-bit output of the OLD
unified shifter MUST match the NEW chunked one.

### 5.1 slideUp by S

OLD: `slideV0Enq[i] = (v0 >> S)[i] = v0[i + S]` for `i + S < vLen`,
else 0.

NEW: `slideV0RegBanks(c)[k] = (v0 >> (S + c*128))[k] = v0[S + c*128 + k]`
for `S + c*128 + k < vLen`, else 0.
`slideV0Reg[c*128 + k] = slideV0RegBanks(c)[k] = v0[S + c*128 + k]`.
Substituting `i = c*128 + k`: `slideV0Reg[i] = v0[S + i]` for `S + i <
vLen`, else 0. ✓ MATCHES.

### 5.2 slideDown by S (the reverse-trick equivalence)

OLD: `slideDownV0[i] = v0[i - S]` for `i >= S`, else 0.

OLD implementation: `slideV0Enq = Reverse((Reverse(v0) >> S)[vLen-1:0])`.
At bit i: `slideV0Enq[i] = (Reverse(v0) >> S)[vLen-1-i] = Reverse(v0)
[vLen-1-i+S] = v0[vLen-1-(vLen-1-i+S)] = v0[i-S]` for `vLen-1-i+S < vLen`,
i.e., `i >= S`. ✓ matches the semantic.

NEW: with `effectiveC = numChunks - 1 - c` for slideDown:
* For c=0: effectiveC=7. srcStart = S + 7*128 = S + 896.
* rawChunk = `(Reverse(v0) >> (S + 896))[127:0] = Reverse(v0)[S+896 +: 128]`
* Reverse(rawChunk)[k] = rawChunk[127-k] = Reverse(v0)[S+896 + (127-k)]
  = v0[vLen-1 - (S+896+127-k)] = v0[vLen-1-S-1023+k] = v0[k - S] (since
  vLen=1024)
* Store at `slideV0RegBanks(0)`. So slideV0Reg[k] = v0[k - S] for the
  first chunk's bits.
* For chunk c general: slideV0Reg[c*128 + k] = v0[c*128 + k - S].
* Matches slideDownV0[i] = v0[i - S]. ✓

### 5.3 slideV0OverReg

Unchanged from Opt 7 v1 — already computed from a small dByte=64-bit
slice. Cost negligible.

## 6. Reproduction recipe

```sh
cd /home/cbt22/code/code_fyp/VisionSoC

# Build RTL after edits to MaskUnitFpga.scala:
bash build_rtl.sh -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt

# Run mask regression (~7 min wall):
T1_MIRROR_RTL_WRITES=1 bash run-test.sh \
  vision_task.benchmark_instructions \
  -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt \
  --max-cycles 50000000

# Smoke test for fast iteration:
T1_MIRROR_RTL_WRITES=1 bash run-test.sh \
  vision_task.simple_instruction_asm \
  -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt \
  --max-cycles 50000000

# FPGA synth-only to measure LUT delta:
bash fpga/system/build_fpga.sh \
  -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt -s -a
# (~25-30 min wall)
```

## 7. Acceptance criteria

* `benchmark_instructions`: **68/70 PASS** (same passing set as the wide
  shifter baseline; the 2 `vmv.x.s` H+V fails are pre-existing C-verifier
  issues unrelated to this work).
* `simple_instruction_asm`: Spike difftest `"success": true`,
  `total_cycles` within ±5% of the wide-shifter baseline (the 127
  MISMATCH lines printed are from a known-buggy C verifier formula —
  ignore those).
* Slide-heavy tests in `benchmark_instructions` (vslideup.vx,
  vslidedown.vi, etc.) — cycle counts within ±1% of baseline.
* Synth report: MaskUnitFpga body LUT drops from 24,240 to ~16K. System
  `system_top_wrapper` LUT drops from 117,104 to ~109K. The extra ~1024
  FFs from `slideShiftV0` register are fine (FF count is well under
  cap).

## 8. Out of scope / non-goals

* Don't touch `t1/src/mask/MaskUnit.scala` — canonical simulator path.
* Don't change the `MaskUnitInterface` IO bundle.
* Don't change the OLD overlap formula — `slideV0OverReg` cost is tiny
  and the equivalence already holds.
* The narrow BRAM (codex's v3 design) is good; this work is in a
  completely separate block of MaskUnitFpga.

## 9. Risk callouts

1. **8-cycle latency assumption.** If a future config (smaller vLen,
   different replay schedule) can fire `askMaskVec(i).slide=true` on the
   lane within < 8 cycles of `maskPipeReq.valid`, slideV0Reg would be
   partial. The fallback is to add a "slide ready" signal back to the
   lane or to gate `askMaskVec(i)` consumption on `!slideShifterActive`.
   For the `mudkip2d128big1bram1chain2lanescale_fpga_maskopt` target,
   slide execution takes >12K cycles, so 8 cycles is comfortably
   within the natural setup window.
2. **Dynamic bit-select cost on slideShiftV0.** `(slideShiftV0 >> srcStart)
   (127, 0)` is a 1024-bit-input × 128-bit-output barrel shifter. Vivado
   should infer this as a multi-stage mux; expect ~1.3K LUTs. If synth
   shows a worse number, restructure as an explicit 8-stage shifter
   (each stage shifts by a power of 2).
3. **Two slide instructions back-to-back.** If a second
   `maskPipeReq.valid && slide` arrives while `slideShifterActive=true`,
   the current pseudo-code would lose the second instruction's setup.
   Acceptable for `_fpga_maskopt` (slides are far apart), but if you want
   robustness, gate `slideShifterActive`-busy back into `maskPipeReq`
   handshake. For the immediate goal, prefer simplicity.

## 10. Related references

* `fyp_doc/maskunit_fpga_handoff.md` — overall MaskUnit area-reduction
  plan and current state.
* `fyp_doc/maskunit_narrow_bram_debug.md` — the previous codex brief
  (BRAM narrow); the chunked-FSM pattern there is structurally
  similar.
* `~/.claude/projects/-home-cbt22-code-code-fyp-VisionSoC/memory/` —
  relevant entries: `feedback_handoff_before_launch.md`,
  `feedback_no_shell_timeout_on_run_test.md`,
  `project_xpm_memory_sdpram_ports.md` (XPM gotchas for any BRAM you
  might add).
