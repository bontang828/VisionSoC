# FPGA Build Status — pickup doc for next agent

**Owner of last touch:** Claude (Opus 4.7), 2026-05-05 17:24
**Branch:** `vertical_narrow_rw`
**Config:** `mudkip2d128small1bram1chain2lanescale`
**Vivado:** 2025.2 at `~/Xilinx/2025.2/Vivado/bin/vivado` (auto-found
by `build_fpga.sh`)

This file tracks an in-flight RTL + FPGA bitstream build for the
KV260 deployment (Task A in `implementation_tasks_index.md`). Update it
in place — keep "Current state" at the top truthful so a fresh agent
can resume without rereading the whole conversation.

---

## 0. TL;DR for the next agent

  * Config in use: **`mudkip2d128small1bram1chain2lanescale`** (the
    standard 2D-fabric SEW=8 / dLen=128 / laneScale=2 / chainingSize=1
    setup that all prior FPGA builds have used).
  * Wrapper extensions per `fpga_implementation_handoff.md` § 3 + § 4
    have been **applied this session** — VERTICAL_MODE @ 0x44, perf
    counters @ 0x48–0x54, ADDR_WIDTH 7→8, `issue_bits_verticalMode`
    wired between wrapper and T1 in `gen_wrapper.sh`. See § 4 of
    this doc for the exact diff.
  * What's running: see § 2 "Current state".
  * Streaming-pipeline IPs (CSI-2, vpss, frmbuf, HDMI TX, IIC, BRAM
    scratchpad) per § 5 of the handoff doc are **not** yet added to
    the block design — that's a follow-up after this build closes.

---

## 1. Why this config

  * `mudkip2d128small1bram1chain2lanescale` is the only FPGA-targeted
    2D config the previous builds have all used (see
    `fpga/build/mudkip2d128small1bram1chain2lanescale-*`).
  * `--dLen 128 --laneScale 2 --chainingSize 1 --vrfBankSize 1
    --vrfRamType p0rw --vfuInstantiateParameter minimal --rowNumber 1`
    (params copied from the May 4 RTL build log).
  * `rowNumber=1` means a single horizontal lane row — minimal area
    for FPGA. The "2d128" in the name refers to dLen=128 with the
    2D fabric topology, not multiple physical rows.

If you need a different config (e.g. wider, multi-row), update both
this doc's `Config` field and rerun both build steps. The fpga build
script keys off `--dLen`, `rowNumber`, etc. inferred from the chosen
RTL output dir.

---

## 2. Current state

| Step | Script | Status | Output dir |
|------|--------|--------|------------|
| 1. RTL build (nix) | `build_rtl.sh -c mudkip2d128small1bram1chain2lanescale` | **done 17:14 — 1m 14s** | `test_output/mudkip2d128small1bram1chain2lanescale/rtl-20260505-171326/` |
| 2. Wrapper § 3 + gen_wrapper § 4 edits | (manual edits; see § 4 below) | **done 17:24** | `fpga/wrapper/t1_axi_lite_wrapper.sv`, `fpga/system/gen_wrapper.sh` |
| 3. Block design + synth + bitstream | `fpga/system/build_fpga.sh -c mudkip2d128small1bram1chain2lanescale -b` | **running, started 17:24** | `fpga/build/mudkip2d128small1bram1chain2lanescale-20260505-172438/` |

Update the Status column with one of:

  * `pending` — not started
  * `running (pid <N>, started <HH:MM>)` — in progress
  * `done <HH:MM> — <build dir>` — finished successfully
  * `failed <HH:MM> — see <log>` — failed; log path appended

Live log tails (use these to watch progress without reading the whole
log):

```sh
BUILD=fpga/build/mudkip2d128small1bram1chain2lanescale-20260505-172438

# Aggregate build log (script-level)
tail -f $BUILD/build.log

# Vivado BD-creation detail
tail -f $BUILD/vivado.log

# Vivado synth detail (after BD creation)
tail -f $BUILD/vivado_synth.log

# Vivado impl + bitstream detail (after synth)
tail -f $BUILD/vivado_impl.log
```

Background-task ID for this run (Claude Code internal): `b2yr4elmt`.
If you've taken over from a fresh session, that ID won't help you
— use `pgrep -af vivado` to find live runs.

---

## 3. Commands used

```sh
cd /home/cbt22/code/code_fyp/VisionSoC

# Step 1 — RTL build (nix; uses store cache so usually a few minutes)
bash build_rtl.sh -c mudkip2d128small1bram1chain2lanescale

# Step 2 — Vivado block design + full synth + impl + bitstream
#  -b means: synthesis + implementation + write bitstream.
#  Expect ~1-2 hours wall time on a typical workstation.
bash fpga/system/build_fpga.sh -c mudkip2d128small1bram1chain2lanescale -b
```

The fpga script auto-picks the newest `rtl-*` directory under
`test_output/<config>/`. If you want a specific RTL build, pass
`-r <rtl_result_dir>`.

---

## 4. How to verify after step 2 finishes

The build dir is `fpga/build/<config>-<timestamp>/`. Expect:

  * `system_top_wrapper.bit` — the bitstream itself (copied in by the
    build script after `write_bitstream` succeeds).
  * `timing_impl.rpt` — check the WNS line is positive. Reference
    point: the May 4 build closed at WNS = 0.003 ns at 80 MHz pl_clk0
    (passing but tight).
  * `utilization_impl.rpt` — sanity-check LUT/FF/BRAM use against the
    May 4 baseline.
  * `vivado_impl.log` ending in `write_bitstream Complete!`.

Quick checks:

```sh
BUILD=fpga/build/mudkip2d128small1bram1chain2lanescale-<TIMESTAMP>
ls -la "$BUILD/system_top_wrapper.bit"
grep -E "WNS|TNS" "$BUILD/timing_impl.rpt" | head -5
grep -E "Slice LUTs|Slice Registers|Block RAM Tile" "$BUILD/utilization_impl.rpt" | head -5
tail -20 "$BUILD/vivado_impl.log"
```

If `system_top_wrapper.bit` is missing but the synth log says
"Synthesis completed successfully", the failure is in implementation
— inspect `vivado_impl.log` and `timing_impl.rpt` for negative WNS.
Per `fpga_implementation_handoff.md` § 8.4.5, the fix is to drop
`PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ` from 80 → 50 in
`fpga/system/system_top.tcl`.

---

## 5. Hand-off — what to do next

After the bitstream is verified (§ 4 above):

  1. **Author the device-tree overlay** per
     `fpga_implementation_handoff.md` § 7. The wrapper now has 256
     bytes of register space (offsets 0x00–0x54), but `range = 64K`
     in the existing UIO node template still works.
  2. **Add the camera/HDMI streaming pipeline** per
     `fpga_implementation_handoff.md` § 5 — extends `system_top.tcl`
     with MIPI CSI-2, `v_proc_ss` ×2, `v_frmbuf_wr/rd`, `v_hdmi_tx_ss`,
     `axi_iic`, `axi_bram_ctrl` + `blk_mem_gen`. After these
     additions, rerun this build pipeline (RTL doesn't need
     re-generating; just rerun `build_fpga.sh -c <config> -b`).
  3. **Deploy on the Kria** per `fpga_implementation_handoff.md` § 8 —
     `fpgautil`, `/dev/uioN`, `devmem2` smoke tests, then camera
     bringup.

`fyp_doc/implementation_tasks_index.md` § 5.1 has the canonical fresh-
agent prompt for "do all of Task A".

---

## 4. Wrapper / gen_wrapper edits applied this session (verbatim)

These are the changes done before kicking off the in-flight build.
They follow `fpga_implementation_handoff.md` § 3 + § 4. If the build
fails and you need to re-derive these, the original (pre-edit) wrapper
is the file as it stood on commit `f36f834a` — see `git diff` against
that for the full delta.

### 4.1 `fpga/wrapper/t1_axi_lite_wrapper.sv`

  * **Header doc block:** added entries for 0x44 / 0x48 / 0x4C / 0x50 /
    0x54 to mirror `fpga_implementation_handoff.md` § 1.
  * **Parameter:** `ADDR_WIDTH 7 → 8`. The existing offsets 0x00–0x40
    only need 7 bits, but the new 0x44–0x54 block pushes case
    constants to 6 bits, so we widen the address bus to keep things
    consistent.
  * **Port list:** added `output logic issue_bits_verticalMode` (drives
    T1's new issue-bundle field).
  * **Internal regs:** added `logic reg_vertical_mode;` and
    `assign issue_bits_verticalMode = reg_vertical_mode;`.
  * **Write path:** `wr_addr` widened `[4:0] → [5:0]`, sliced from
    `aw_addr_reg[7:2]`. All `5'hXX` constants in the case become
    `6'hXX` (same numeric values). Added `6'h11: reg_vertical_mode <=
    w_data_reg[0]`. Reset block now resets `reg_vertical_mode <= 0`.
    `wr_addr == 5'h00` (CTRL) and `5'h0E` (MEM_COUNT W1C) similarly
    bumped to `6'h00` and `6'h0E`.
  * **Free-running cycle counter:** added a 64-bit `perf_cycles`
    counter clocked on `aclk`, reset on `aresetn`. Source for both
    `PERF_CYCLES_LO/HI` reads and the perf-delta start latch.
  * **Tag-driven perf delta:** added combinational
    `assign perf_tag_w = w_data_reg[7:0]` and
    `assign perf_tag_we = wr_en && (wr_addr == 6'h12)`. Registered
    `perf_tag` (latched), `perf_start` (snapshot of perf_cycles on
    0→nonzero edge), `perf_delta` (latched on nonzero→0 edge). This
    differs from the doc's literal example (which puts `perf_tag_w`
    in the case block as a register) — combinational gives a 1-cycle
    saving and avoids a stale-compare race. Functionally identical
    from the PS perspective: write-nonzero starts, write-zero stops,
    read 0x4C reads the delta. See § 6 below for rationale.
  * **Read path:** `rd_addr` widened `[4:0] → [5:0]`, sliced from
    `s_axi_araddr[7:2]`. All `5'hXX` constants become `6'hXX`. Added
    new cases:
    - `6'h11`: `{31'b0, reg_vertical_mode}` (0x44)
    - `6'h13`: `perf_delta`                   (0x4C)
    - `6'h14`: `perf_cycles[31:0]`            (0x50)
    - `6'h15`: `perf_cycles[63:32]`           (0x54)
  * **FIFO pop assigns:** `5'h09 → 6'h09` and `5'h0C → 6'h0C`.

### 4.2 `fpga/system/gen_wrapper.sh`

  * **t1_fpga_top ctrl ports:** `s_axi_ctrl_awaddr` and
    `s_axi_ctrl_araddr` widened `[6:0] → [7:0]` so the wrapper's
    8-bit `ADDR_WIDTH` connects without a width-mismatch warning.
    The block design's address segment is `64K` which Vivado
    auto-truncates to whatever the slave port declares.
  * **Internal wire:** added `wire issue_bits_verticalMode;` alongside
    the other issue wires.
  * **Wrapper instance:** added
    `.issue_bits_verticalMode (issue_bits_verticalMode),`.
  * **T1 instance:** added the same
    `.issue_bits_verticalMode (issue_bits_verticalMode),`.

No edits to `fpga/system/system_top.tcl` were needed for the wrapper
extensions — the BD only sees the AXI-Lite interface, which Vivado
re-infers from the new `t1_fpga_top.v` automatically.

---

## 5. (renumbered above)

(Section 5 hand-off content is now § 5 above — kept for posterity in
case the doc grows.)

---

## 6. Why the perf-counter divergence from the doc

The handoff doc's example for § 3.7 shows `perf_tag_w` being assigned
inside the main `case (wr_addr)` block (`6'h12: perf_tag_w <=
w_data_reg[7:0]`), then used inside the perf-delta `always_ff` block
on the same write cycle. Because both are non-blocking, `perf_tag_w`
in the perf-delta block reads the *previous* value, not the value
being written this cycle. The 0→nonzero / nonzero→0 edge detection
then never fires correctly.

To fix without altering the PS-visible behaviour, we made
`perf_tag_w` a combinational alias of `w_data_reg[7:0]`:

  ```sv
  assign perf_tag_w  = w_data_reg[7:0];
  assign perf_tag_we = wr_en && (wr_addr == 6'h12);
  ```

Now `perf_tag` (latched) compared against `perf_tag_w` (current
write data) detects edges in one cycle. The PS sees identical
semantics: write nonzero = START, write 0 = STOP, read 0x4C = delta.

If a future agent reverts to the doc's literal form, write a small
sim or `devmem2` sequence first to confirm the edge detection still
works.

---

## 7. Change log

  * 2026-05-05 17:14 — RTL built (`rtl-20260505-171326`).
  * 2026-05-05 17:24 — wrapper + gen_wrapper edits applied per
    handoff doc § 3 + § 4. FPGA build kicked off in background.
  * (next agent): record bitstream completion / failure here.
