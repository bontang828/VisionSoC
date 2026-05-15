# LMUL=2 / VLEN=512 plan — double the register-group count without busting KV260 BRAM

**Status:** planned, not yet implemented
**Drafted:** 2026-05-07
**Target config:** `mudkip2d128small1bram1chain2lanescale_fpga` on KV260
**Reference build (baseline numbers in this doc):** `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260506-234437/`

---

## 0. TL;DR

The current setup is VLEN=256 with LMUL=4 to cover one 128-element image row. That
gives the programmer only **8 register-group bases** (v0, v4, v8, ..., v28),
which is tight for kernels with several live LMUL groups.

Plan: bump VLEN to 512 and run kernels at LMUL=2. Same vl=128 covers one image
row, but the programmer now has **16 register-group bases** (any even vN). It
needs only one RTL constant change and a config edit; the SharedVRF code is
already parametric.

| | Now (VLEN=256, LMUL=4) | Plan (VLEN=512, LMUL=2) | LMUL=1 alternative (VLEN=1024) |
|---|---|---|---|
| SharedVRF RAMB36 | 32 | **64** | 128 |
| Top-level RAMB36 | 53 / 144 | **~85 / 144** | ~149 / 144 (over) |
| Util | 37% | **59%** | 103% |
| URAM rework? | no | **no** | yes |
| Peripheral BRAM cuts? | no | **no** | yes |
| `vrfReadLatency` bump? | no | **no** | yes (URAM) |
| Register-group bases | 8 | **16** | 32 |

---

## 1. Why LMUL=2 instead of LMUL=1

LMUL=1 (VLEN=1024) was considered first — it gives the programmer the full 32
register namespace and removes the LMUL-fusion rule entirely. But the SharedVRF
storage scales linearly with VLEN (banks 4× deeper), and the math works out to
**128 RAMB36 for SharedVRF alone** plus ~21 for peripherals → ~149 total,
~5 over the KV260's 144-RAMB36 budget. Mitigations are either:

  * Remap SharedVRF to URAM (16 URAMs out of 64 — fits easily, but URAM has
    higher minimum read latency than BRAM, so `vrfReadLatency` would have to
    bump from 2 to 3 and every `Pipe(true.B, ..., vrfReadLatency)` site in
    `SharedVRF.scala` retimes, with knock-on regression risk on the chaining
    record's "ready" cadence); or
  * Cut peripherals (axi_dma DataMover FIFOs, v_frmbuf_wr line buffers, MIPI
    YUV bridge) — see `fpga_build_status.md` § 9 for the menu.

LMUL=2 (VLEN=512) sidesteps all of that:

  * BRAM grows 2× not 4×, fits in 59% of the budget with no peripheral cuts.
  * Stays on RAMB36 (no URAM, no `vrfReadLatency` change).
  * Still doubles the programmer's register-group budget over LMUL=4.

The trade-off is "only" 16 groups instead of 32. For the kernels we have today
(see `tests/vision_task/`), 16 is plenty.

---

## 2. BRAM math

From `utilization_synth.rpt:681-690`, the current SharedVRF maps as 8 banks ×
4 RAMB36 = 32 RAMB36, with each bank a 2K × 64-bit TDP byte-masked SRAM
(reported name: `sram_0R_0W_2RW_8M_2048x64`).

SharedVRF param formulas (`t1/src/vrf/SharedVRF.scala:39-46`) at VLEN=512,
dpw=64, laneNumber=2, timeMultiplexBatch=128:

  * `groupsPerRegister = 512/64 = 8` (×2 from 4)
  * `entriesPerRow = 32 × 8 = 256` (×2 from 128)
  * `bankDepth = 128 × 256 / 8 = 4096` (×2 from 2048)

A 4K × 72 TDP byte-masked SRAM packs into **8 RAMB36 per bank** in either of
the standard mappings:

  * 1Kx36 mode: 2 in width (2×36 = 72b) × 4 in depth (4×1K = 4K) = 8 RAMB36
  * 2Kx18 mode: 4 in width (4×18 = 72b) × 2 in depth (2×2K = 4K) = 8 RAMB36

8 banks × 8 = **64 RAMB36 total** for SharedVRF. Net top-level:
53 − 32 + 64 = ~85 RAMB36 (59% of KV260's 144).

---

## 3. SharedVRF parametric verification

The vertical-mode decomposition `[cVsOff | cGroup | cLane | cByte]` lives in
`SharedVRF.scala:52-67`. At VLEN=512:

  * `cByteBits = log2(64/8) = 3`
  * `cLaneBits = log2(2) = 1`
  * `cGroupBits = vrfOffsetBits = log2Ceil(8/2) = 2`
  * `rowCounterBits = log2Ceil(128) = 7`
  * **`cVsOffBits = 7 − 3 − 1 − 2 = 1`** (positive — comfortably in the
    parametric zone)

The `require(cVsOffBits >= 0)` at line 67 passes. Compare LMUL=1 (VLEN=1024)
which would land on `cVsOffBits = 0` — still legal but at the edge.

The `vsLowField`, `offsetField`, and `vsStoreUpper` helpers
(`SharedVRF.scala:280-283, 343-345, 428-431`) all guard the zero-width case
already, so even the LMUL=1 path elaborates — but at LMUL=2 we never exercise
those branches.

**No SharedVRF code edits are needed.** Only the comment at lines 396-406
("Assumes SEW=8, LMUL=4") wants updating.

---

## 4. Required changes

### 4.1 `t1/src/T1.scala:318` — drop the baked-in LMUL=4

Current:

```scala
val targetElementNum: Int = vLen * 4 / 8 // 4 means LMUL=4 and 8 means 8 bit per element
```

The literal `4` is the LMUL=4 assumption. Left as-is at VLEN=512, this gives
`targetElementNum = 256`, so `timeMultiplexBatch = 256 / numRows = 256` instead
of 128, and `bankDepth` doubles unnecessarily (BRAM blows up to 128 RAMB36
again — back to the LMUL=1 problem).

Fix options, ordered by cleanness:

  * **Quick:** change `4` → `2`.
  * **Better:** introduce a `targetLmul` config knob and use it here.
  * **Cleanest:** hardcode `targetElementNum: Int = 128` directly — it's an
    invariant (one image-row's worth of elements at SEW=8), not a derived
    quantity. Then the formula doesn't lie about its assumptions.

### 4.2 Elaborator config

`designs/org.chipsalliance.t1.elaborator.t1.T1.toml`, FPGA config row:

```
[mudkip2d128small1bram1chain2lanescale_fpga]
cmdopt = "--dLen 128 --extensions zvl512b --extensions zve32x --laneScale 2 --chainingSize 1 --vrfBankSize 1 --vrfRamType p0rw --vfuInstantiateParameter minimalFpga --rowNumber 1"
```

(Was: `--extensions zvl256b`.)

### 4.3 Kernel code in `tests/vision_task/`

All `vsetvli` macros change from `m4` to `m2`. Touch points:

  * `simple_instruction_*.c` naked-asm blocks
  * `benchmark_vadd.c` perf-counter kernels
  * Any future kernel built off the canonical pattern

```asm
vsetvli zero, x_cols, e8, m2, ta, ma   ;# was: ..., e8, m4, ta, ma
```

### 4.4 Update programmer-rule doc

`fyp_doc/2d_fabric_handoff.md`:

  * § 3.3 says "LMUL stays at 4" — update to "LMUL stays at 2".
  * § 3.4 says "with LMUL=4 the legal group-bases are v0, v4, v8, v12, v16,
    v20, v24, v28" — update to LMUL=2 (16 bases: any even vN, v0 through v30).

### 4.5 Update SharedVRF vertical-mode comment

`t1/src/vrf/SharedVRF.scala:396-406` — change "LMUL=4" → "LMUL=2".

---

## 5. Caveats

### 5.1 LUT impact (small but real, **must verify**)

Current top utilisation from `utilization_synth.rpt:25`:

```
LUTs: 104,998 / 117,120 = ~90%
FFs:  133,268 / 234,240 = 57%
```

Doubling VLEN grows T1's combinational logic somewhat:

  * Wider register-offset decode (cGroupBits 1 → 2)
  * Wider VRF read multiplexers in the per-port narrow/horizontal/vertical
    paths
  * Wider mask routing in `MaskUnit.scala`

Expected delta: **+1 to +3% LUT** (so ~106-108K LUTs, still under 117K). Should
fit, but the headroom is thin — **run synth-only first** before committing the
config. If it overflows, the fallback levers from `fpga_build_status.md` § 9
still apply (drop laneScale, drop chainingSize), or the BRAM-only peripheral
cuts (axi_dma burst-size 16→4, v_frmbuf_wr MAX_COLS 1920→512) free LUTs as a
side-effect.

### 5.2 Disjointness contract change

§ 3.4's `vrgather.vv` register-group disjointness rule loosens at LMUL=2:

  * **LMUL=4** (now): need 4-aligned register groups. `vrgather.vv v16, v8, v12`
    is legal (v8-v11, v12-v15, v16-v19 are disjoint).
  * **LMUL=2** (plan): need 2-aligned register groups.
    `vrgather.vv v8, v4, v6` is now legal (v4-v5, v6-v7, v8-v9 are disjoint),
    but would have overlapped illegally under LMUL=4.

Programmer freedom goes up, but the legality predicate has changed — port
existing kernels deliberately rather than auto-substituting `m4 → m2` and
hoping.

### 5.3 What this does NOT change

  * Time-multiplex grid: still 128 hardware rows × 128 image rows.
  * Hardware row count, dLen, laneScale, chainingSize, numRows: all unchanged.
  * Camera capture pipeline / DMA / peripherals: untouched.
  * `vrfReadLatency`: stays at 2.
  * `cVsOffBits >= 0` invariant: holds with margin.

In particular, **leave `chainingSize=1`**. The LMUL change does not depend on it,
and bumping `chainingSize` is a separate decision with its own LUT cost (the
hazard-check matrices in `Lane.scala` and `SharedVRF.scala` scale roughly with
`chainingSize × (chainingSize + 1)`, and the VRF read-port count grows as
`chainingSize × 3 + 2` — see the chaining-record breakdown in
`SharedVRF.scala:212-232` and `Lane.scala:487-507`). If you want more
instruction-level overlap later, raise `chainingSize` independently; don't
couple it to the LMUL switch.

---

## 6. Verification plan

In order, smallest-first:

1. **Elaborate RTL only:**
   ```sh
   bash build_rtl.sh -c mudkip2d128small1bram1chain2lanescale_fpga
   ```
   Confirms the SharedVRF parameter checks pass and the elaboration produces
   the expected bank geometry. Watch for any new `require` failures or width
   mismatches.

2. **Functional regression** on the canonical tests, after updating
   `vsetvli` macros to `m2` in each:
   ```sh
   bash run-test.sh ... simple_instruction_gather_scalar      # CSR snapshot path
   bash run-test.sh ... simple_instruction_vert_lsu           # vert-LSU path
   bash run-test.sh ... benchmark_vadd                        # functional baseline (tests 1-5, 8)
   ```
   Use `--max-cycles 50000000` per `fyp_doc/2d_fabric_handoff.md` § 5.4.

3. **Synth-only build** (no impl, no bitstream) to verify resource fit:
   ```sh
   bash fpga/system/build_fpga.sh -c mudkip2d128small1bram1chain2lanescale_fpga
   ```
   Inspect `utilization_synth.rpt` from the new build dir:
   * RAMB36 ≈ 85 (≤144)
   * LUT < 117,120 (target: ≤114K to leave routing headroom)

4. **Full bitstream:**
   ```sh
   bash fpga/system/build_fpga.sh -c mudkip2d128small1bram1chain2lanescale_fpga -b
   ```
   Expected WNS positive at 60 MHz; reference: prior build closed at WNS=+5.99
   ns at synth (`fpga_build_status.md` § 2). Check `timing_impl.rpt`.

If step 3 overflows LUTs:
  * Drop `axi_dma` burst-size 16→4 in `system_top.tcl:163-164` (BRAM and LUT
    saving simultaneously — see `fpga_build_status.md` § 9 menu).
  * If still over, drop `laneScale` 2 → 1 in the toml — that's the heavy hammer
    but reliably halves T1's lane-side LUT count.

---

## 7. Open questions / future work

  * Do we want a `targetLmul` config parameter (clean) vs. hardcoding `4 → 2`
    (quick)? Cleaner is better if there's a chance LMUL=1 comes back later.
  * The `cVsOffBits = 0` path in `SharedVRF.scala` is currently dead code at
    LMUL=2. Worth keeping for LMUL=1 portability, or worth simplifying away?
    Default: keep — the parametric guard is harmless and protects against
    regressions if LMUL gets bumped further.
  * Are there other "LMUL=4" assumptions baked in elsewhere? Confirmed:
    * `t1/src/T1.scala:318` — yes, the only one (verified by grep on
      `LMUL=4`, `vLen.*4`, `4.*lmul`).
    * `t1/src/vrf/SharedVRF.scala:399` — comment only, code is parametric.
    * `t1/src/T1.scala:1671` — comment-only coverage block, not active code.
