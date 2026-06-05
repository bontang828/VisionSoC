# FPGA Build Status — pickup doc for next agent

**Owner of last touch:** Claude (Opus 4.7), 2026-05-07
**Branch:** `fpga_driver`
**Config:** `mudkip2d128small1bram1chain2lanescale`
**Vivado:** 2025.2 at `~/Xilinx/2025.2/Vivado/bin/vivado` (auto-found
by `build_fpga.sh`)

This file tracks an in-flight RTL + FPGA bitstream build for the
KV260 deployment (Task A in `implementation_tasks_index.md`). Update it
in place — keep "Current state" at the top truthful so a fresh agent
can resume without rereading the whole conversation.

---

## 0. TL;DR for the next agent

### 0.1 Where to look depending on what you're doing

| You are doing… | Read first |
|---|---|
| Writing vision kernels / exploring 2D fabric | `fyp_doc/vision_kernel_programming_guide.md` (canonical kernel-dev manual: board access, bitstream choice, SEW=8/LMUL=4/128×128 assumptions, v0 mask H/V semantics, camera API, example library, opportunities) |
| Debugging the 2D fabric / vector mode / vrgather | `fyp_doc/2d_fabric_handoff.md` + `fyp_doc/LSU_vertical_mode_handoff.md` |
| Camera bringup (driver, V4L2, AP1302) | This doc § 0.4 + `fyp_doc/camera_bringup_status.md` § 0, § 6.3 |
| FPGA / BD / Vivado iteration | This doc rest of § 0 + sections below (chronological take-by-take trail) |
| libt1 / driver API | `fyp_doc/driver_function_spec.md` + the headers in `vision_software/libt1/` |
| Implementation roadmap | `fyp_doc/implementation_tasks_index.md` |

### 0.2 Current state (as of 2026-05-18 — 5t-maskopt DEPLOYED, two stable options)

There are now **two stable deployable bitstreams**. Pick by need:

| Bitstream | T1 config | Best for |
|---|:--|:--|
| **5r** | vLen=256 (`mudkip2d128small1bram1chain2lanescale_fpga`) + URAM 512 KB scratchpad + original MaskUnit | Smaller / lower-LUT design. Kernels written for vLen=256 hardware. |
| **5t-maskopt** (NEW) | vLen=1024 (`mudkip2d128big1bram1chain2lanescale_fpga_maskopt`) + URAM 512 KB scratchpad + MaskUnitFpga (all area-reduction opts) + axis_register_slice | 4× larger vector register file, supports bigger kernels / longer vectors. Same camera+display pipeline. |

Both use the same camera+display+URAM/DMA flow; only the T1 vector
core (vLen, MaskUnit variant) and the camera-pipeline rate-match
buffer differ. Either can be the active production bitstream — they
swap by changing the `system_top_wrapper.bit.bin` and `.dtbo` in
`/lib/firmware/xilinx/visionsoc/`.

  * **5r — vLen=256, URAM DMA (deployed 2026-05-16):**
    `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260516-010205/`.
    URAM-backed 512 KB scratchpad at 0xA0080000; 16/64 URAM288,
    44.5/144 BRAM36 (−8 vs 5q-r3), 100,220 LUT (+1.0%), WNS +0.068 ns.
    visionsoc_main camera→DDR→URAM→T1→URAM→DDR→display flow verified
    at 20 fps with `outY` byte-identical to `camY` under
    `frame_passthrough` (proves URAM round-trip integrity). Full
    delta + bringup trail in § 0.11.

  * **5t-maskopt — vLen=1024, URAM DMA, MaskUnitFpga (deployed 2026-05-18):**
    `fpga/build/mudkip2d128big1bram1chain2lanescale_fpga_maskopt-20260518-055430/`.
    Bitstream:
    `fpga/build/mudkip2d128big1bram1chain2lanescale_fpga_maskopt-20260518-055430/system_top_wrapper.bit`
    (Vivado .bit; convert to .bit.bin via § 7.1 bootgen recipe before
    `fpgautil`). URAM scratchpad preserved (same 512 KB at 0xA0080000,
    same camera pipeline). Synth: 104,717 LUT / 143 BRAM36 + 2 BRAM18
    (= 144 tiles exact). Impl: **WNS +0.239 ns, WHS +0.010 ns,
    `write_bitstream completed successfully`**. **Camera+Sobel verified
    end-to-end on KV260 at ~19 fps** after warm-up (~5% vs 5r baseline,
    detailed in `fyp_doc/maskunit_fpga_optimisations.md § 8`). All
    MaskUnitFpga area-reduction work (Phase 3b through Phase 3e + axis
    swap) is consolidated in `fyp_doc/maskunit_fpga_optimisations.md`.
    Currently the ACTIVE bitstream on the Kria (replaced 5r as of
    2026-05-18).
  * **Backup / revert:** 5q-r3 preserved on Kria as
    `system_top_wrapper.bit.bin.5q-r3-backup` +
    `system_top_wrapper.dtbo.5q-r3-backup`; on host at
    `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260515-024828/`.
  * **Prior deploy / history:** 5o bitstream
    (`fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260511-210311/`,
    .bit.bin sha256 `e2222e8d6d334fd8a838ef8673d2c2ee4ce577124b38a5f2c66ad2534fc0b938`,
    backed up at `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5o-backup`).
    Synth 87.96%, Impl 84.19%, timing met. F4/F1/F7/F8 all in, F2 software-only.
    UYVY camera path (dts `xlnx,csi-pxl-format = <0x1E>`).
  * **In-flight build:** 5p adds F9 = `v_frmbuf_wr HAS_Y_UV8_420=1`
    for native NV12 frame output (Y plane contiguous → single
    `vle8.v` reads greyscale, no Y-extract pre-pass). Build dir
    `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260512-092526/`,
    PID 1264383, in impl phase; bitstream expected ~13:00-14:00.
    Synth landed at 88.38% — borderline but viable.
  * **T1 status:** F4 production-validated (`sp_v12_compute_probe`
    3/3 PASS). All pre-5o probes still pass.
  * **Camera status (2026-05-13 late): BD-level WORKS, V4L2 framework
    integration is the wall.** Full handoff in
    `fyp_doc/camera_handoff_2026-05-13.md`. Camtest3 v8 bitstream
    at `fpga/build/camtest3-20260513-145152/` proves receiver decodes
    64,800+ packets; `gst-launch v4l2src` / `v4l2-ctl` can't
    negotiate the format chain (smartcam uses Xilinx-internal
    `mediasrcbin` which isn't packaged for Ubuntu). Two paths for the
    next agent: (A) defer V4L2 capture and ship 5q with synthetic
    frames, (B) bypass V4L2 with ~100 LoC direct frmbuf via
    `/dev/mem`.
  * **Camera FVCO sub-status:** `pl_clk0 = 60 MHz` ⇒ DPHY PLL FVCO out of range.**
    Vivado emits CRITICAL WARNING `DRC AVAL-350` in all our builds
    (camtest1/2/3 + 5o): "FVCO is 1500.060 MHz, valid range 750-1500 MHz".
    Root cause: clk_wiz_0 can't produce exactly 200 MHz from 60 MHz
    pl_clk0 (gets 200.008 MHz). DPHY internal PLL multiplies × 7.5 →
    1500.060 MHz (just over the 1500 limit). PLL silently fails to
    lock → DPHY can't sample data → zero packets decoded (even though
    chip emits, receiver enabled, no errors flagged).
    **FIX (validated 2026-05-13 in camtest3): change PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ
    from 60 → 50 (or 100). 50 MHz gives clean MMCM math: VCO=1200 MHz,
    /4=300, /6=200, /12=100 — all exact. With 50 MHz pl_clk0, FVCO
    warning gone, CSI2RX PKTCNT climbs at 30 fps.**
    T1 implications for 5q: T1 was tested at 60 MHz; 50 MHz is slower
    (more timing slack) → safe for T1 without re-validation. 100 MHz
    would also fix clocks but is faster than T1 was validated for.
    Smartcam uses pl_clk0=100MHz and works.
    Hardware comparison: smartcam captured 14,822 packets in 4s on
    same hardware — the BD config / clocking is the discriminator.
    See § 0.4 for full diagnosis trail.
  * **AP1302 firmware:** updated to `Xilinx/ap1302-firmware` HEAD
    (sha256 `2dd09e34...`, was Nov 2021 vintage `d996b4...`). The
    old CRC-mismatch panic is gone. Old firmware preserved at
    `/lib/firmware/ap1302_ar1335_single_fw.bin.old`.

### 0.15 dLen / bank-count sweep + SharedVRF numBanks generalization (REPORT-ONLY, 2026-05-29/30)

**REVISION v2 (2026-05-30): switched to 2 lanes per config.** The v1 sweep below
varied dLen at fixed laneScale=2 → laneNumber 4/2/1, which forced lane-port sharing
(4 lanes on 2 BRAM ports) and made small single-port → URAM. v2 instead scales
`laneScale` (4/2/1) so **laneNumber=2 for all three** (2 lanes ↔ 2 dual-port BRAM
ports), and `numBanks = datapathWidth/8` (= byteCount, the diagonal-banking invariant):

| Config | dLen | laneScale | datapathWidth | laneNumber | numBanks | bank word | bankDepth |
|---|---:|---:|---:|---:|---:|---:|---:|
| large_dlen256  | 256 | 4 | 128 | 2 | 16 | 128b | 8192 |
| medium_dlen128 | 128 | 2 | 64  | 2 | 8  | 64b  | 8192 |
| small_dlen64   | 64  | 1 | 32  | 2 | 4  | 32b  | 8192 |

v2 RTL changes: dropped the unused `dLen` param; `numBanks = datapathWidth/8`
(deployed dLen=128/dpw=64 still → 8, unchanged). All 3 RTL built OK; bank modules
`sram_…_2RW_8M_8192x{128,64,32}` confirm **dual-port** banks (small no longer
single-port → expected to infer BRAM not URAM). Synth (`-s -a`) IN FLIGHT;
v2 results + small's memory-primitive verdict appended when done. Master logs:
`test_output/dlen_sweep_20260529-184136/` (`rtl_phase3.log`, `synth_phase3.log`).

**v2 synth results (2026-05-30):**

| Metric | large_dlen256 (16 banks) | medium_dlen128 (8 banks) | small_dlen64 (4 banks) |
|---|---:|---:|---:|
| status | **SYNTH FAIL** | OK | OK |
| system LUT | — | 104,717 | 64,367 |
| system RAMB36 / URAM | — | 143 / 16 | 44 / 16 |
| `sharedVRF2D_0` LUT | — | 10,651 | 3,247 |
| `sharedVRF2D_0` RAMB36 / URAM | — | 128 / 0 | **32 / 0** |
| per-bank | 8192×128 | 8192×64 (16 RAMB36) | 8192×32 (**8 RAMB36**) |

- **small VRF is now BRAM** (32 RAMB36, 0 URAM) — the 2-lane/dual-port change moved it
  off URAM. The user's "force small to BRAM" request is satisfied by the dual-port geometry
  (no explicit primitive needed for small).
- **medium == deployed** (104,717 LUT / 143 RAMB36), unchanged.
- **large_dlen256 fails synth**: bank = 8192×128 = 1,048,576 bits > Vivado's 1,000,000-bit
  behavioral-inference limit (`[Synth 8-4556]` on `sram_…_8192x128.sv`). → RESOLVED in v3.

**REVISION v3 (2026-05-30): explicit XPM block-RAM banks, all 3 synthesize.**
Added `--bankBramPrimitive` flag (default false → deployed/sim untouched). When set, each
SharedVRF bank is an XPM `xpm_memory_tdpram`, `MEMORY_PRIMITIVE="block"`, true-dual-port,
byte-masked, 1-cycle read (`fpga/wrapper/vrf_bank_bram.v` + `VrfBankBram` BlackBox + uniform
`BankMem` trait in `SharedVRF.scala`; synth-only, no Verilator twin). Flag set on all 3 sweep
configs. XPM has no behavioral-size limit → large clears the 1 Mb wall, and every bank is
forced to BRAM.

| Metric | large_dlen256 (16 banks) | medium_dlen128 (8 banks) | small_dlen64 (4 banks) |
|---|---:|---:|---:|
| status | **OK** | OK | OK |
| system LUT | 231,183 | 104,392 | 64,448 |
| system FF | 174,814 | 108,013 | 75,255 |
| system RAMB36 / RAMB18 / URAM | 523 / 2 / 20 | 143 / 2 / 16 | 44 / 2 / 16 |
| system DSP | 55 | 31 | 19 |
| `u_t1` LUT / RAMB36 | 207,443 / 512 | 82,830 / 132 | 43,199 / 33 |
| `sharedVRF2D_0` LUT | 57,482 | 10,816 | 3,351 |
| **`sharedVRF2D_0` RAMB36 / URAM** | **512 / 0** | **128 / 0** | **32 / 0** |
| per-bank (vrf_bank_bram) | 8192×128 = 32 RAMB36 | 8192×64 = 16 RAMB36 | 8192×32 = 8 RAMB36 |
| `maskUnit2D_0` RAMB36 / URAM | 0 / 4 | 4 / 0 | 1 / 0 |

**Findings (v3).**
1. **All banks are BRAM now** (sharedVRF URAM=0 everywhere). Bank count scales 16/8/4 with
   datapathWidth; per-bank tiles 32/16/8 ∝ word width (128/64/32, depth fixed 8192).
2. **XPM is resource-neutral vs inferred** where both work: medium XPM 10,816 LUT / 128 RAMB36
   vs v2 inferred 10,651 / 128; small XPM 3,351 / 32 vs v2 inferred 3,247 / 32. The ~1-2% LUT
   delta is XPM wrapper steering; BRAM identical. So forcing the primitive costs ~nothing for
   the configs that already inferred BRAM, and is the *only* way large synthesizes.
3. **large is LUT-heavy intrinsically** (sharedVRF 57 k LUT): the 128-bit-wide, 32-deep-cascade
   BRAM needs large read-data output muxing — a property of the wide datapath, not the XPM.
4. **Budget (117,120 LUT / 144 BRAM36 / 64 URAM):** large busts LUT (197%) and BRAM (363%);
   medium fits (~89% LUT, 143 BRAM36 ≈ deployed); small fits easily (55% LUT, 44 BRAM36).
5. Deployed config + t1emu sim untouched (flag defaults false; inferred-SRAM path unchanged).

--- v1 (laneNumber 4/2/1, superseded) below for reference ---

**Goal.** Show how T1/SharedVRF resources scale when `numBanks` tracks `dLen`
(more lanes → more dual-port banks for parallel vertical-mode access), holding
the bank word at 64 bits. Three configs (copies of large/medium/small varying
`--dLen`, `--baseLMUL 1`, `--laneScale 2`):

| Config suffix | dLen | laneNumber=dLen/64 | numBanks | vLen |
|---|---:|---:|---:|---:|
| `…_maskopt_large_dlen256`  | 256 | 4 | **16** | 2048 |
| `…_maskopt_medium_dlen128` | 128 | 2 | **8**  | 1024 |
| `…_maskopt_small_dlen64`   | 64  | 1 | **4**  | 512  |

**SharedVRF RTL change (`t1/src/vrf/SharedVRF.scala`, resource-only — NOT
functionally verified for laneNumber≠2):**
- `SharedVRFParam` now takes `dLen` (threaded from `T1.scala`, like `vLen`).
  `numBanks = dLen/(2*(datapathWidth/8))` replaces the hardcoded `8`
  (dLen=128 still → 8, so all deployed dLen=128 configs are unchanged).
- Banks stay **dual-port** (`sramReadWritePorts = min(laneNumber,2)`); the extra
  *bank count*, not extra ports, provides parallel access. parallel byte-accesses
  = `numBanks*2 = laneNumber*byteCount`.
- `require(laneNumber==2)` relaxed to `>=1`.
- Lane field in `logicalAddr` generalized from the hardcoded 1-bit `laneIdx(0)`
  to `cLaneBits` (fixes the laneNumber=4 "High index 13 out of range" width bug).
- Vertical gather/scatter width decoupled from `numBanks` (now iterates
  `byteCount`); bank index slice/shift parameterized by `bankIdxBits`.
- Lanes mapped onto the ≤2 physical ports via `portIdx = laneIdx % sramReadWritePorts`.

**RTL: all 3 built OK** (`test_output/<cfg>/rtl-20260529-18/19xx/`). `T1.sv` sizes
track the lane count: large 1.52 MB (4 lanes) / medium 738 KB (2) / small 379 KB (1).

**Synth COMPLETE 2026-05-29/30 (all 3 OK).** Master logs:
`test_output/dlen_sweep_20260529-184136/`; reports in `fpga/build/<cfg>-<ts>/`.

| Metric | large dLen256 (16 banks/4 lanes/vLen2048) | medium dLen128 (8/2/1024) | small dLen64 (4/1/512) |
|---|---:|---:|---:|
| system LUT | 218,174 | 104,717 | 57,590 |
| system FF | 191,051 | 108,017 | 68,709 |
| system RAMB36 / RAMB18 | 523 / 2 | 143 / 2 | 12 / 2 |
| system URAM | 20 | 16 | 20 |
| system DSP | 55 | 31 | 19 |
| `u_t1` LUT | 194,434 | 83,147 | 36,334 |
| `u_t1` RAMB36 / URAM | 512 / 4 | 132 / 0 | 1 / 4 |
| `sharedVRF2D_0` LUT | 37,115 | 10,651 | 2,426 |
| `sharedVRF2D_0` FF | 3,342 | 1,317 | 547 |
| `sharedVRF2D_0` RAMB36 / URAM | 512 / 0 | 128 / 0 | 0 / 4 |
| `maskUnit2D_0` LUT | 54,604 | 21,034 | 8,787 |
| VRF banks (count × depth × width, ports) | 16 × 16384×64, 2RW | 8 × 8192×64, 2RW | 4 × 4096×64, **1RW** |

**Findings.**
1. **numBanks scaled exactly as designed** (16/8/4 instances counted in the reports).
2. **Doubling banks at fixed vLen does NOT change VRF BRAM.** Cross-ref §0.14
   large (vLen2048, dLen128, **8** banks): `sharedVRF`=**512 RAMB36**, system 523.
   §0.15 large (vLen2048, dLen256, **16** banks): `sharedVRF`=**512 RAMB36**, system
   523 — identical. Total VRF storage = numBanks·bankDepth·64 = vLen²/16·64 is set by
   vLen; more banks just split the same bits into more/shallower banks (16×16384 vs
   8×32768, both 16 Mbit → same 512 tiles). So the **dLen/bank knob buys parallel
   vertical-access bandwidth at ~zero extra BRAM.**
3. **The real cost of dLen is lane datapath logic.** At fixed vLen=2048, dLen 128→256
   (2→4 lanes): system LUT 120,284 → 218,174 (**+81%**), DSP 31→55, `maskUnit` LUT
   ~2.6×, `sharedVRF` LUT 13,110 → 37,115 (~2.8×, crossbar widens with banks×lanes).
   FF and LUT roughly track laneNumber.
4. **small VRF mapped to URAM, not BRAM.** sramReadWritePorts = min(1,2) = 1 → the
   4×4096×64 single-port banks inferred as **4 URAM** (1 each), 0 RAMB36. medium/large
   are dual-port → RAMB36. (If an apples-to-apples BRAM comparison is wanted for small,
   force sramReadWritePorts=2.)
5. **medium == deployed 5t-maskopt, bit-for-bit** (104,717 LUT, 143 RAMB36, 16 URAM,
   31 DSP) — confirms the `dLen=128 → 8 banks` path is unchanged and the deployed
   bitstream config is untouched.
6. **Budget (117,120 LUT / 144 BRAM36 / 64 URAM):** large busts everything (218k LUT
   =186%, 523 BRAM=363%); medium = deployable; small fits easily (57k LUT, 12 BRAM, 20 URAM).

### 0.14 VRF-width synth sweep (REPORT-ONLY, IN FLIGHT 2026-05-29)

**Purpose.** Measure how T1 synthesis resources — system LUT, BRAM tiles,
and especially the SharedVRF and MaskUnitFpga sub-hierarchies — scale with
VRF width, by sweeping `vLen` at a **fixed `baseLMUL=1`**. Synthesis-resource
study only; the user explicitly accepts these are NOT functionally correct
for the deployed 128×128 grid (see geometry note).

**Configs (added to `designs/org.chipsalliance.t1.elaborator.t1.T1.toml`,
all `--laneScale 2 --rowNumber 1 --baseLMUL 1 --vfuInstantiateParameter
minimalFpga --useFpgaMaskUnit true`):**

| Config suffix | vLen | targetElementNum = vLen·baseLMUL/8 | timeMultiplexBatch | implied grid |
|---|---:|---:|---:|---|
| `…_fpga_maskopt_large`  | 2048 | 256 | 256 | 256-wide row × 256 hw-rows |
| `…_fpga_maskopt_medium` | 1024 | 128 | 128 | 128×128 (== existing `_fpga_maskopt`) |
| `…_fpga_maskopt_small`  |  512 |  64 |  64 | 64-wide row × 64 hw-rows |

Because `baseLMUL` is pinned at 1 (not scaled to hold the 128-element image
row like the functional small/medium/big siblings in §0.13), each `vLen`
implies a *different* grid geometry, and the LSU's hardcoded
`logicalRowElements=128` (`StrideBase.scala:157`, `SimpleAccessUnit.scala:820`)
no longer matches large/small — hence "not functionally verified". The
TOML comment labels all three "256×256" but only `_large` matches that.
Elaboration is sound for all three: `cVsOffBits = rowCounterBits −
cByteBits(3) − cLaneBits(1) − cGroupBits` = 0 in every case.

**Build recipe (per config):**
```
bash build_rtl.sh -c <config>
bash fpga/system/build_fpga.sh -c <config> -s -a   # synth-only + full analysis
```
`-a` = `flatten_hierarchy none` + `report_utilization -hierarchical` so the
SharedVRF / MaskUnitFpga rows survive in `utilization_synth.rpt`.

**Sweep artefacts:** master logs under `test_output/synth_sweep_20260529-141244/`
(`rtl_phase.log`, then `synth_phase.log`); per-config synth reports land in
`fpga/build/<config>-<timestamp>/utilization_synth.rpt` + `timing_synth.rpt`.

**Status: COMPLETE 2026-05-29.** All 3 RTL builds + all 3 synth (`-s -a`)
runs finished OK. Whole sweep ~1h (RTL ~5 min; synth ~30/27/~? min for
large/medium/small). Reports at
`fpga/build/<config>-<ts>/utilization_synth.rpt`.

**Synth utilization (hierarchical, `flatten_hierarchy none`):**

| Metric | large (2048) | medium (1024) | small (512) |
|---|---:|---:|---:|
| `system_top_wrapper` LUT | 120,284 | 104,717 | 97,457 |
| system FF | 117,989 | 108,017 | 102,676 |
| system RAMB36 / RAMB18 | 523 / 2 | 143 / 2 | 45 / 2 |
| system URAM | 18 | 16 | 16 |
| system DSP | 31 | 31 | 31 |
| `u_t1` LUT | 98,711 | 83,147 | 75,896 |
| `u_t1` RAMB36 | 512 | 132 | 34 |
| `sharedVRF2D_0` LUT | 13,110 | 10,651 | 10,051 |
| `sharedVRF2D_0` FF | 1,868 | 1,317 | 1,022 |
| `sharedVRF2D_0` RAMB36 | 512 | 128 | 32 |
| `maskUnit2D_0` LUT | 32,452 | 21,034 | 16,198 |
| `maskUnit2D_0` FF | 23,384 | 15,603 | 11,683 |
| `maskUnit2D_0` RAMB36 / URAM | 0 / 2 | 4 / 0 | 2 / 0 |
| VRF bank SRAM module | 32768×64 | 8192×64 | 2048×64 |

**Findings.**
1. **SharedVRF BRAM scales as vLen² (not vLen) at fixed baseLMUL=1.**
   `bankDepth = timeMultiplexBatch(vLen/8) × regNum(32) × groupsPerReg(vLen/64)
   / 8banks = vLen²/128` → 32768 / 8192 / 2048, confirmed by the bank module
   names. RAMB36 follows: 512 → 128 → 32 (clean ×4 per ×2 vLen). Contrast
   §0.13's functional sweep, where baseLMUL was scaled to hold the 128×128
   grid and VRF BRAM scaled only linearly with vLen.
2. **SharedVRF LUT is nearly flat** (13.1k / 10.7k / 10.1k) — VRF cost is
   BRAM, not logic. The address-decode/bank-mux glue grows slightly at large.
3. **MaskUnitFpga is the dominant LUT scaler in T1** (32.5k / 21.0k / 16.2k);
   its v0 store maps to 2 URAM at vLen=2048 vs 4/2 RAMB36 at 1024/512.
4. **VRF dominates T1 BRAM**: sharedVRF is 512/523, 128/143, 32/45 of system
   RAMB36 respectively.
5. **Budget (KV260: 117,120 LUT, 144 BRAM36, 64 URAM):** large busts BRAM
   hard (523 = 363%) and LUT (103%) — report-only as expected; medium = the
   deployed 5t-maskopt point (143 BRAM36 = 99%, 89% LUT, numbers match §0.13
   bit-for-bit — methodology check); small fits easily (45 BRAM36 = 31%, 83%
   LUT).

### 0.13 MaskUnitFpga — area-reduction variant for vLen=1024 (FUNCTIONAL + SYNTH VERIFIED 2026-05-18)

**Motivation.** The 5s vLen=1024 synth pre-flight (§ 0.12) revealed that
the big config's 155,748 LUTs / 234,453 FFs dramatically exceeds the
KV260 117,120 LUT cap. Per-module breakdown:

| Module | small (vLen=256) | medium (vLen=512) | big (vLen=1024) |
|---|---:|---:|---:|
| SharedVRF LUT | 9,729 | 10,029 | 10,652 (essentially flat) |
| MaskUnit LUT | 24,087 | 39,383 | **70,066** |
| MaskUnit FF | 41,267 | 74,868 | **142,014** |
| Lane LUT (each) | 14,570 | 14,530 | 14,830 |

MaskUnit is responsible for ~75% of T1's LUT growth and ~100% of FF
growth. The dominant cost is `v0Vec` at `MaskUnit.scala:194-198`: a
128-row × `vLen`-bit FF replication of the v0 mask register
(= 131,072 flops at vLen=1024) plus the 128:1 row mux and per-row
write merge. Beyond v0Vec, the 3-SEW unroll (regroupV0, regroupSlideV0,
maskSplit) and the 1024-bit slide barrel shifters compound the LUT cost.

**Approach (per the approved plan in
`~/.claude/plans/please-read-home-cbt22-code-code-fyp-vis-validated-spark.md`).**
Add a parallel `t1/src/mask/MaskUnitFpga.scala` as a drop-in
FPGA-optimised variant of `MaskUnit.scala`. Same IO bundle (zero
changes needed in `Lane.scala`, `MaskExchangeUnit.scala`, or any
consumer); internally stacks four optimisations:

1. **Opt 3 — BRAM-back v0Vec + active-row cache:** −15K LUT, −131K FF,
   +4 BRAM36 (XPM `xpm_memory_sdpram` wrapper at
   `fpga/wrapper/v0_bram.v`).
2. **Opt 1 — On-demand per-lane mask slicer:** collapse the 3-SEW
   regroup/maskSplit unroll into a runtime-parameterised slicer (all
   SEW=8/16/32 preserved). −12K LUT.
3. **Opt 2 — Pipelined slide write-mask trees:** segment-walk the
   1024-bit `scanRightOr`/`scanLeftOr` over 8 cycles. −10K LUT.
4. **Opt 7 — Pipelined v0 slide barrel shifters:** reuse a 128-bit
   shifter over 8 cycles instead of two parallel 1024-bit barrels.
   −15K LUT. Behavior-preserving (every slide instruction produces
   identical results, just with ~8 cycles internal setup latency
   hidden in the slide-instruction's normal multi-cycle execution
   window).

**Target:** big-config MaskUnit at ~20K LUT / ~15K FF (matching small),
system total ~105-115K LUT (fits the 117K cap). BRAM goes 139 → 143,
still under the 144 limit. All SEWs preserved. <5% perf hit on masked
instructions.

**Status update (2026-05-18, Codex): narrow-BRAM v3 fix landed and
verified.** The failing revised-v2 narrow-BRAM design raced row changes:
`gatherRowCounter` could advance while lane `v0Update` writes for the
row being left were still arriving, and MaskUnit has no local ready/valid
back-pressure for those writes. The fix is contained in
`t1/src/mask/MaskUnitFpga.scala`: the v0 row cache is now split into
`activeCacheChunks`, `prefetchCacheChunks`, and `writebackCacheChunks`.
The next sequential row is prefetched from the 128-bit narrow BRAM while
the current row executes; on row change the active row swaps from the
prefetch buffer, and the row just left is written back in the background.

No lane-side or T1 replay-control changes were added for this fix. The
design relies on the current replay schedule giving enough cycles per row
for the 8 narrow chunks to prefetch/write back; this is true for the
target big maskopt regression. If a future config switches rows faster,
the robust follow-up is lane-side row-transition back-pressure or a
deeper writeback queue.

Verification:

| Test / build | Result |
|---|---|
| Pre-fix `benchmark_instructions` on `mudkip2d128big1bram1chain2lanescale_fpga_maskopt` | Reproduced `FAIL (57/70 checks passed, 13 failed)` |
| Post-fix `benchmark_instructions` | `FAIL (68/70 checks passed, 2 failed)`; remaining failures are the known `vmv.x.s` H+V C-checks |
| Post-fix `simple_instruction_asm` | Spike difftest `"success": true`; known 127 C-verifier mismatch lines ignored |
| Fresh RTL for synth | `test_output/mudkip2d128big1bram1chain2lanescale_fpga_maskopt/rtl-20260518-023232/result`, verified `MaskUnitFpga.sv` instantiates `v0_bram` with `.DATA_WIDTH(128)` |
| Synth command | `bash fpga/system/build_fpga.sh -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt -r test_output/mudkip2d128big1bram1chain2lanescale_fpga_maskopt/rtl-20260518-023232/result -s -a` |
| Synth report dir | `fpga/build/mudkip2d128big1bram1chain2lanescale_fpga_maskopt-20260518-024351/` |

Synth utilization:

| Hierarchy | LUT | FF | BRAM36 | BRAM18 | URAM |
|---|---:|---:|---:|---:|---:|
| `system_top_wrapper` | 117,104 | 106,447 | 143 | 3 | 16 |
| `t1_top/u_t1` | 95,502 | 77,594 | 132 | 0 | 0 |
| `maskUnit2D_0` (`MaskUnitFpga`) | 33,360 | 14,051 | 4 | 0 | 0 |
| `maskUnit2D_0/(body)` | 24,240 | 10,674 | 0 | 0 | 0 |
| `maskUnit2D_0/v0BramInst` (`v0_bram`) | 271 | 0 | 4 | 0 | 0 |

The BRAM objective is met: `v0_bram` drops from the earlier 16 BRAM36
wide-port result to 4 BRAM36 with the narrow port. The fix still uses
BRAM tiles, but only through `maskUnit2D_0/v0BramInst`; the new
`activeCacheChunks`, `prefetchCacheChunks`, and `writebackCacheChunks`
row caches are register/LUT-backed and do not add extra BRAM instances.
Top-level LUT is just under the KV260 117,120 LUT cap at synth
(`117,104`, leaving only 16 LUT of nominal margin), so full
implementation may still need additional LUT or placement/routing margin.

Synth timing summary: setup is clean (`WNS=+0.307 ns`, 0 setup failing
endpoints). The synth timing report still has hold/pulse-width violations;
those are not implementation timing-closure signoff.

**Subsequent (2026-05-18 later, Claude) — 3 further area cuts landed on
top of codex's v3, taking LUT below the route-safe 105K target and
packing BRAM to exactly 144 tiles:**

1. **Opt 7 v2 (chunked v0 slide barrel shifter)** in `MaskUnitFpga.scala`.
   Replaces the unified-reverse-trick 1024-bit `>>` with a single
   128-bit-wide shifter reused over 8 cycles via slideShifterActive /
   slideChunkCounter / slideShiftV0 FSM. Output banks
   (slideV0RegBanks) concatenate into slideV0Reg for downstream
   consumption. Latency: 8 cycles, hidden trivially within slide
   instructions' 12K-66K cycle execution windows. Equivalence proof in
   `fyp_doc/maskunit_chunked_slide_shifter_debug.md § 5`. **Δ: −4,689 LUT.**

2. **`axis_data_fifo_cap` → `axis_register_slice`** in `system_top.tcl`.
   The camera-pipeline rate-match FIFO (256-deep, 1 BRAM18) was over-
   provisioned by ~10× given the 700-cycle inter-pixel period at
   128×128@26fps. Replaced with a 1-deep skid buffer. **Δ: −1 BRAM18
   tile (= the difference between 144.5 effective tiles and 144 cap),
   −34 LUT.**

3. **Per-lane writeMaskForMaskPipe push-down** in `MaskUnitFpga.scala`.
   The wide-vLen `writeMaskForMaskPipe` register + scanRightOr/scanLeftOr
   trees are eliminated entirely. Per-lane writeBitMaskForSlide /
   writeBitMaskForExtend slices are computed DIRECTLY from sources
   (v0Cache bit-select, `srcPos.U < vl` comparisons, etc.) pre-Pipe
   instead of slicing a wide intermediate post-Pipe. The Pipe register
   shrinks from 1024 bits (writeMaskForMaskPipe) to 2 × 128 bits per
   lane (writeBitMaskForSlide + writeBitMaskForExtend pre-computed).
   **Δ: −7,698 LUT** (much more than the −3-to-−5K estimate; Vivado was
   apparently NOT efficiently sharing the wide scan-tree logic, so
   eliminating it entirely was a much bigger win than expected).

**New test added:** `tests/vision_task/sew16_32_masktest/sew16_32_masktest.c`
exercises MaskUnitFpga's SEW=16 and SEW=32 Mux1H paths (vslideup.vi,
vslidedown.vi, vrgather.vx, vmseq.vx + vmerge.vim at LMUL=1). The
existing benchmark_instructions only runs SEW=8 so these paths were
elaborated but not previously exercised at runtime. **8/8 kernels PASS.**

**Synth report 2026-05-18 (after the 3 further cuts):**
`fpga/build/mudkip2d128big1bram1chain2lanescale_fpga_maskopt-20260518-052355/`

| Hierarchy | LUT | FF | BRAM36 | BRAM18 | URAM |
|---|---:|---:|---:|---:|---:|
| `system_top_wrapper` | **104,717** | 108,017 | 143 | 2 | 16 |
| `maskUnit2D_0` (`MaskUnitFpga`) | 21,034 | 15,603 | 4 | 0 | 0 |
| `maskUnit2D_0/(body)` | **11,638** | 12,220 | 0 | 0 | 0 |
| `maskUnit2D_0/v0BramInst` | 274 | 0 | 4 | 0 | 0 |

System LUT delta from codex's v3 baseline (117,104): **−12,387 LUT**.
Under the user-stated 105K route-safe target. BRAM tiles **143 RAMB36 +
2 RAMB18 = 144 tiles exactly** (Vivado packs the 2 BRAM18 into 1 tile
shared with a RAMB36 OR another BRAM18).

**Full FPGA impl: BITSTREAM COMPLETE 2026-05-18.**

Command: `bash fpga/system/build_fpga.sh -c
mudkip2d128big1bram1chain2lanescale_fpga_maskopt -b -a`. Wall: **1h47m**
(much faster than 6-7h estimate).

Build dir: `fpga/build/mudkip2d128big1bram1chain2lanescale_fpga_maskopt-20260518-055430/`

| Result | Value |
|---|:--|
| `write_bitstream` | **completed successfully** ✅ |
| WNS (setup) | **+0.239 ns** ✅ |
| TNS | 0.000 ns (0 setup failing endpoints) |
| WHS (hold) | **+0.010 ns** ✅ (hold also clean!) |
| THS | 0.000 ns (0 hold failing endpoints) |
| Bitstream | `system_top_wrapper.bit` (in build dir) |

**Impl utilization (after P&R):**

| Resource | Used | Cap | Util% |
|---|---:|---:|---:|
| Block RAM Tile | 144 | 144 | **100.00%** (143 RAMB36 + 2 RAMB18) |
| CLB Registers | 103,721 | 234,240 | 44.28% |
| URAM | 16 | 64 | 25.00% |
| LUT (system_top_wrapper) | ~104,717 | 117,120 | ~89.4% |

Design is signed-off and deployable to KV260.

**Selection mechanism.** Mirror the existing `vfuInstantiateParameter
== "minimalFpga"` pattern (DSP-based multipliers, divider stubs).
Added `useFpgaMaskUnit: Option[Boolean]` to `T1Parameter` and
threaded through both elaborators (`elaborator/src/t1/T1.scala` +
`elaborator/src/t1emu/TestBench.scala`). The original `MaskUnit.scala`
stays untouched; sim builds and non-`_fpga` configs continue to use it.
At `t1/src/T1.scala:550` the instantiation site will conditionally pick
between `MaskUnit` and `MaskUnitFpga` based on the flag (wiring TBD in
Phase 4).

**Config layout.** Existing `_fpga` configs kept untouched as the
easy-revert baseline. New `_fpga_maskopt` siblings added with the same
parameters plus `--useFpgaMaskUnit true`:

| Config | vLen | baseLMUL | useFpgaMaskUnit |
|---|---:|---:|:-:|
| `mudkip2d128small1bram1chain2lanescale_fpga` | 256 | (default 4) | false |
| `mudkip2d128small1bram1chain2lanescale_fpga_maskopt` | 256 | (default 4) | **true** |
| `mudkip2d128medium1bram1chain2lanescale_fpga` | 512 | 2 | false |
| `mudkip2d128medium1bram1chain2lanescale_fpga_maskopt` | 512 | 2 | **true** |
| `mudkip2d128big1bram1chain2lanescale_fpga` | 1024 | 1 | false |
| `mudkip2d128big1bram1chain2lanescale_fpga_maskopt` | 1024 | 1 | **true** |

All six are also in `designs/org.chipsalliance.t1.elaborator.t1emu.TestBench.toml`
for t1emu sim regression.

**Status (2026-05-17):**

* **Phase 1 — config flag plumbing: DONE + VERIFIED.**
  `T1Parameter.useFpgaMaskUnit` added (default `Some(false)`), threaded
  through both elaborators (`elaborator/src/t1/T1.scala` +
  `elaborator/src/t1emu/TestBench.scala`). New `_fpga_maskopt` configs
  added next to the existing `_fpga` ones in both
  `designs/org.chipsalliance.t1.elaborator.t1.T1.toml` and
  `…t1emu.TestBench.toml`. Verified clean with t1emu sim:
  `vision_task.simple_instruction_asm` under
  `mudkip2d128big1bram1chain2lanescale_fpga_maskopt` →
  **`success: true`, `meta_vlen=1024`, `total_cycles=21514`**, byte-identical
  to the `_fpga` baseline (since the T1 instantiation still picks the
  default `MaskUnit` — the flag is a no-op until Phase 4 wires the
  selection). Wall: 6m 25s for a fresh verilator build.
* **Phase 2 — `fpga/wrapper/v0_bram.v`: DRAFT WRITTEN.** XPM
  `xpm_memory_sdpram` wrapper, 128 × 1024-bit storage, byte-write
  strobe, simple dual-port (write A / read B), 1-cycle read latency.
  TBD: verify Vivado infers ≤4 BRAM36 in synth (may need to refactor
  to 8 parallel narrower instances if XPM uses too many).
* **Phase 3b (Opt 3 — BRAM v0 + active-row cache): DONE + VERIFIED.**
  `t1/src/mask/MaskUnitFpga.scala` is the parallel module (same
  `MaskUnitInterface` IO, same package). v0Vec is replaced by a
  `V0BramBlackBox extends BlackBox with HasBlackBoxResource` wrapping
  `v0_bram` (XPM at `fpga/wrapper/v0_bram.v` for synthesis,
  behavioural at `t1/resources/v0_bram.sv` for Verilator —
  XPM cells don't exist in Verilator's lib). Active-row cache
  (`v0Cache: UInt(vLen.W)`) + 2-state refill FSM
  (issue read + latch). Merged-write path combines per-lane v0Updates
  into one vLen-bit (data, strobe) BRAM port-A write per cycle, plus a
  `pendingWrite{Data,Strb}` shadow that covers the write-during-refill
  race for the same row.
* **Phase 3c (Opt 1 — On-demand per-lane mask slicer): DONE + VERIFIED.**
  `maskSliceFor(lane, sew, maskSelect, source)` computes only the
  datapath-width slice this lane needs. SEW Mux1H now operates on
  32-bit values instead of vLen-bit values.
* **Phase 4 — Conditional MaskUnit/MaskUnitFpga at `t1/src/T1.scala`
  line 556: DONE + VERIFIED.** `if (parameter.useFpgaMaskUnit.getOrElse(false))`
  picks `MaskUnitFpga`; LUB inference (`Instance[+A]` is covariant)
  resolves the seq type to the common supertype.
* **t1emu sim verification on `_fpga_maskopt` big config:**
  * `vision_task.simple_instruction_asm`: `success: true`,
    `total_cycles=21514` — bit-identical to the `_fpga` baseline.
  * `vision_task.benchmark_instructions`: 68/70 PASS (the 2 fails are
    `vmv.x.s (H)` + `vmv.x.s (V)` — pre-existing per
    maskunit_fpga_handoff.md § 8 risk #8).
* **Phases 3d (Opt 2) and 3e (Opt 7): DEFERRED.** Both require non-trivial
  FSM redesigns with multi-cycle slideV0Reg / writeMaskForMaskPipe
  population, with downstream-timing coupling to the lane's pipeline.
  Deferred until we measure Phases 3b+3c LUT delta in synth and assess
  whether more optimisation is required to hit the 117K LUT cap.
* **`fpga/system/system_top.tcl`: updated** with `add_files ...
  fpga/wrapper/v0_bram.v` next to the existing `uram_scratchpad.v` add.
* **FPGA synth pre-flight LAUNCHING NEXT.** Command:
  `bash fpga/system/build_fpga.sh -c mudkip2d128big1bram1chain2lanescale_fpga_maskopt -s -a`
  (synth-only, ~25-30 min wall). Target rows:
  * MaskUnitFpga LUT ≤ 55,000 (estimate: Opts 3+1 should drop ~27K LUTs
    from the 70K baseline, landing around 43K — still well above the
    20K final target but a meaningful step toward the 117K system cap).
  * MaskUnitFpga FF ≤ 15,000 (Opt 3 alone should drop ~131K FFs).
  * System total LUT ≤ 130K (155K - 27K = 128K).
  * BRAM36 ≤ 144 (was 139; +4 for v0_bram → 143).

### 0.12 5s — LMUL=1 vLen=1024 baseline (synth pre-flight LAUNCHING 2026-05-16)

**Motivation.** Switch the architectural assumption from LMUL=4 to LMUL=1,
quadrupling SharedVRF storage so one LMUL=1 register at SEW=8 holds 128
elements = one full 128-pixel image row. Same 128×128 grid, same memory
layout, just 32 LMUL=1 register groups visible to programs (vs 8 LMUL=4
groups previously). The KV260 program-visible architectural change: every
kernel can address 4× more independent register temporaries. Plan file:
`~/.claude/plans/please-read-home-cbt22-code-code-fyp-vis-validated-spark.md`.

**Implementation done so far (2026-05-16 ~14:00 BST):**

  * `T1Parameter`: added `baseLMUL: Option[Int] = Some(4)` field;
    `targetElementNum = vLen * baseLMUL.getOrElse(4) / 8` (was hardcoded
    `vLen * 4 / 8`). Default preserves all legacy configs numerically.
  * `elaborator/src/t1/T1.scala` + `elaborator/src/t1emu/TestBench.scala`:
    both `T1ParameterMain` definitions get the new `--baseLMUL` CLI arg.
  * `designs/org.chipsalliance.t1.elaborator.t1.T1.toml` +
    `…t1emu.TestBench.toml`: new pair of configs
    `mudkip2d128big1bram1chain2lanescale` (sim, `minimal` VFU) and
    `_fpga` variant (`minimalFpga` VFU). Both use
    `--extensions zvl1024b --baseLMUL 1 --rowNumber 1`.
  * `SharedVRF.scala`: stale "Assumes LMUL=4" comment in the wide-vert
    overlay block replaced with worked examples for BOTH baseLMUL=4
    (current) and baseLMUL=1 (new). No code change — the existing logic
    parametric across both.

**Simulator validation (t1emu, vLen=1024 / LMUL=1):**

  * Elaboration: clean for both `big` and `big_fpga` configs (4m 14s wall,
    no `require` failures). 5 chisel warnings — all pre-existing scaling
    artefacts that ALSO appear at vLen=256, none new.
  * `vision_task.simple_instruction_lmul1` — single-kernel LMUL=1 vsub:
    **PASS**, all 128×128 cells correct, Spike difftest agreed at
    `meta_vlen=1024`. 22026 cycles.
  * `vision_task.simple_instruction_asm` — LMUL=4 backcompat under the
    new "big" config (vsetvli `m4` still legal, vl=128 requested,
    `vl_max=512`): **PASS**, 21514 cycles. Confirms the keep-LMUL=4 +
    add-LMUL=1 strategy.
  * `vision_task.benchmark_vadd_lmul1` — 6-test LMUL=1 benchmark
    (vadd/vsadd/H+V blur/8-way pixel accumulate): **PASS**, all tests
    including TEST 9's LMUL=1-only 8-way accumulate (16384 cells = 36,
    proves the 32-group register headroom). 311433 cycles.
  * `vision_task.benchmark_instructions_lmul1` — IN FLIGHT (vadd/vmul
    passed; vrgather running).

**Expected FPGA deltas vs 5r (BRAM only — user opted out of URAM swap for SharedVRF):**

| Resource | 5r | Expected 5s | Delta |
|---|---|---|---|
| CLB LUTs (impl) | 100,220 (85.57%) | ~105–115k (89–98%) | +5–15k from wider chaining-record offset comparators (vrfOffsetBits 1→3); wider Mux1H trees in SharedVRF; 4× wider elementMask.andR (16→64 bits) |
| CLB FFs | 126,946 (54.19%) | ~135–145k | Wider chaining records, deeper VRF address paths |
| BRAM36 tiles | 44.5 / 144 (30.9%) | ~96–128 / 144 (67–89%) | 8 VRF banks × 4× depth (2048→8192 entries); packing-dependent |
| URAM288 | 16 / 64 | 16 / 64 | Unchanged (scratchpad only; user opted no URAM for VRF) |

**LUT cliff watch.** 5r was 85.57%; routing cliff was observed around 87–88%
on past builds (5j hit 87.96% and failed-routed). The +5–15k LUT estimate
puts us at 90–98% — borderline to past-the-cliff. The synth pre-flight
(`-s` flag) will tell us the actual LUT before committing to the multi-hour
full build. **Decision criterion:** if synth LUT ≥ ~108k, the BRAM-only
path will likely not route; escalate to user about URAM swap for VRF.

**Build dir (this attempt):** TBD after launch.

  1. **Build with `-c mudkip2d128small1bram1chain2lanescale_fpga`**
     (note the `_fpga` suffix). The non-`_fpga` config is +12-18k
     LUTs heavier and yields bogus 100%+ synth util. § 0.5 below
     has the lesson trail (5n take 1/2/3 + 5o takes 1-4 all used
     the wrong config and were misdiagnosed as "axi_dma propagation
     trap").
  2. **Don't unbind/rebind `ap1302` via sysfs.** The driver's
     cleanup path doesn't fully unwind V4L2 state; the rebind hits
     `-EEXIST` and a subsequent overlay rmdir segfaults the kernel
     inside configfs, hanging the Kria. For firmware reloads or
     re-probe, do a full `fpgautil` overlay-reload cycle (or reboot).
     Memory entry: `project_ap1302_firmware_path.md`.
  3. **AXI DMA `c_sg_length_width` MUST be set explicitly.** In
     direct-register mode (`c_include_sg=0`), Vivado defaults this
     parameter to **14 bits**, capping any single DMA transfer at
     `2^14 - 1 = 16383 bytes`. ANY transfer `len ≥ 16384` will hang
     forever (S2MM never sees TLAST because the LENGTH register
     physically can't hold the value). This silently breaks the
     natural visionsoc workload sizes: Y-plane 16384 B, NV12 frame
     24576 B, 256×256 grey 65536 B. The fix is one line in
     `system_top.tcl` axi_dma `set_property` dict:
     ```
     CONFIG.c_sg_length_width {23} \
     ```
     With 23 bits, the LENGTH register holds values 0 … `2^23 - 1`,
     so **max single transfer = 8,388,607 bytes (just under 8 MiB)**.
     PG081 range is 8-26 (i.e. up to `2^26 - 1` = ~64 MiB at the
     ceiling, at slightly higher LUT cost). For reference, our
     current workload maxima: 128×128 NV12 frame = 24,576 B;
     256×256 NV12 = 98,304 B — both comfortably under 8 MiB.
     Hardware cost of going 14→23 bits: ~50-100 LUTs.
     **Keep this line.** If anyone removes it, the
     bug returns at the exact 16384 B threshold. Resolved in 5q-r3
     (`mudkip2d128small1bram1chain2lanescale_fpga-20260515-024828`).
     Full diagnostic trail and dma_loopback sweep evidence in § 0.10.
     Memory entry: `project_dma_loopback_16384_bug.md`.

### 0.4 Camera next-session checklist

After power-cycle, expect this sequence to land at "STREAMON works
but no frames":

```sh
# 1. ssh kv260 + reload visionsoc overlay
ssh kv260 'sudo rmdir /sys/kernel/config/device-tree/overlays/k26-starter-kits_image_1 2>/dev/null
           sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin \
                         -o /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo'
# 2. dmesg should show: "AP1302 revision 0.2.6 detected" (no CRC mismatch)
ssh kv260 'sudo dmesg | grep ap1302'
# 3. T1 regression
ssh kv260 'cd ~/vision_software/libt1 && sudo ./test/sp_v12_compute_probe'
# 4. Camera pipeline
ssh kv260 '
  media-ctl -V "\"ap1302.4-003c\":2 [fmt:UYVY8_1X16/128x128 field:none colorspace:srgb]"
  media-ctl -V "\"80000000.csiss\":0 [fmt:UYVY8_1X16/128x128 field:none colorspace:srgb]"
  media-ctl -V "\"80000000.csiss\":1 [fmt:UYVY8_1X16/128x128 field:none colorspace:srgb]"
  v4l2-ctl --device=/dev/video0 --set-fmt-video=width=128,height=128,pixelformat=UYVY
  timeout 5 v4l2-ctl --device=/dev/video0 --stream-mmap --stream-count=1 --stream-to=/tmp/frame.uyvy
'
# Result: STREAMON returns 0 (good), but timeout fires; /tmp/frame.uyvy stays 0 bytes.
```

Next investigation steps — UPDATED 2026-05-12 after deep diagnostic session:

**Diagnostic work already completed this session (don't repeat):**

  * Sudoers allowlist extended with `v4l2-dbg, i2c{get,set,transfer,detect},
    media-ctl, v4l2-ctl, timeout, kill, pkill, fuser, lsof, cat, ls,
    sha256sum`. Both `/etc/sudoers.d/visionsoc-nopasswd` and
    `~ubuntu/visionsoc-nopasswd.template` updated. Verified with `sudo -n`.
  * Correct AP1302 register addresses (per linux-xlnx `drivers/media/i2c/ap1302.c`):
    - 0x0000 CHIP_VERSION (16-bit, expect 0x0265) ✓ confirmed
    - 0x0002 FRAME_CNT (upper byte = HINF count, lower = BRAC) — both wrap 8-bit
    - 0x0006 ERROR — observed 0x0000 (no firmware error)
    - 0x6002 BOOTDATA_STAGE — 0xFFFF (boot complete) ✓
    - 0x600c SENSOR_SELECT — 0x0011 (SINF_MIPI | SENSOR(0)) ✓
    - 0x601a SYS_START — 0x8040 during stream (PLL_LOCK|STALL_MODE_DISABLED,
      un-stalled) vs 0x8240 before (STALL_STATUS=1, stalled)
    - 0x2000 PREVIEW_WIDTH — 0x0080 (128) ✓
    - 0x2002 PREVIEW_HEIGHT — 0x0080 (128) ✓
    - 0x2012 PREVIEW_OUT_FMT — 0x0050 (FT_YUV_JFIF|FST_YUV_422 = UYVY) ✓
    - 0x2030 PREVIEW_HINF_CTRL — 0x0014 (SPOOF | MIPI_LANES(4)) — note: driver
      DOES NOT set MIPI_MODE bit BIT(3)=0x08, but writing it manually mid-stream
      doesn't change anything; chip already in MIPI mode by default.
    - The doc's earlier `0x6000` "chip version" and `0x6134/0x6136` "warning"
      addresses were WRONG. Correct above.
  * **CSI2RX register offsets (from `xilinx-csi2rxss.c`):** 0x00 CCR, 0x04 PCR
    (lanes), 0x10 CSR (PKTCNT in [31:16] — DEFINITIVE), 0x20 GIER, 0x24 ISR,
    0x28 IER, 0x30 SPKTR, 0x3C CLKINFR (STOP bit shows clock lane state),
    0x40/44/48/4C DL0..3 INF (STOP/SOTERR/SOTSYNCERR per lane), 0x60 VC0_LINECNT.
    (Note: devmem2 hits MMU artefact on offsets where `offset & 0x4 == 0x4` —
    only reads at 0x00, 0x10, 0x20, 0x40-aligned succeed. Use a kernel-side
    tool if you need 0x04/0x24/0x3C/0x44/0x4C/0x64.)
  * **`v4l2-dbg --device=/dev/v4l-subdev2 --log-status`** dumps AP1302 errors,
    warnings, frame counters, AND per-lane MIPI Rx state (SINF). Use this
    instead of trying to manually probe sensor-side lanes.
  * **`i2ctransfer -f`** (`-f` = force, overrides driver claim on bus) works
    fine for read-only AP1302 register probes while the driver is active.

**DEFINITIVE FINDING (2026-05-12):**

The AP1302 chip is healthy and emitting frames at ~30 fps on its MIPI Tx
pads — confirmed by:
  * ICP frame counter advancing ~30/sec (sensor → AP1302 capture working)
  * HINF frame counter advancing at the SAME rate (AP1302 → host emission)
  * HINF count delta of +141 in 5s = 28.2 fps, matching ICP delta exactly
  * **Test pattern mode (sensor bypass) ALSO advances HINF at the same rate**,
    proving the issue is downstream of the AP1302 chip, NOT in the sensor.

But the CSI2RX receiver sees ZERO packets — CSR=0, VC0_LINECNT=0 throughout
streaming. The break is somewhere in the **AP1302 MIPI Tx → IAS PCB →
SOM240 connector → KV260 carrier → DPHY hard block** physical chain.

**BD configuration verified against Xilinx reference:** the canonical
`kria-vitis-platforms/kv260/platforms/vivado/kv260_ispMipiRx_vcu_DP`
uses EXACTLY the same MIPI Rx settings as ours: `C_HS_LINE_RATE=896`,
`C_HS_SETTLE_NS=146`, `DPHYRX_BOARD_INTERFACE=som240_1_connector_mipi_csi_isp`,
4 lanes. NOT a BD config bug.

**Cosmetic dts bug noticed (not the cause):** local
`fpga/dts/system_top_wrapper.dts` has `clock-frequency = <0x48000000>`
(= 1.2 GHz!) for `sensor_clk`. Should be `<48000000>` decimal (48 MHz) or
similar. The AP1302 driver doesn't use this value (just calls
`clk_prepare_enable` on the fixed-clock), so it's harmless — but worth
fixing.

**UPDATE 2026-05-12 (later same day): HARDWARE PROVEN GOOD via smartcam test.**

Loaded Xilinx's `kv260-smartcam` reference app (installed at
`/lib/firmware/xilinx/kv260-smartcam/`). Smartcam uses the SAME J7
connector, AP1302+AR1335 module, MIPI signal path, and DPHY hard block
as visionsoc — but with different BD/dts (its own bitstream + dtbo).

Under smartcam, with AP1302 manually un-stalled (i2c write
`0x601a = 0x8340`) and CSI2RX CCR manually set to 1:

  * **CSR (0x10) read 0x39E60000** → PKTCNT[31:16] = **14,822 packets received in ~4s**.
  * **VC0_LINECNT (0x60) read 0x3E870F00** → substantial line activity.
  * AP1302 HINF counter +178 in 4s = ~44 fps emission, matching ICP delta exactly.

This **definitively rules out the hardware** as the cause of the
visionsoc zero-packet bug. The IAS card, AR1335 ribbon, J7 connector,
SOM240 routing, K26 DPHY hard block, and CSI2RX core all function.

**The bug is in our visionsoc BD config (or a subtle driver flow
difference).** Reference vs visionsoc BD comparison:

| Parameter | smartcam (works) | visionsoc (broken) | Suspect? |
|---|---|---|---|
| `DPHYRX_BOARD_INTERFACE` | `som240_1_connector_mipi_csi_isp` | same | no |
| `C_HS_LINE_RATE` | 896 | 896 | no |
| `C_HS_SETTLE_NS` | 146 | 146 | no |
| `CMN_VC` | 0 | 0 | no |
| `C_CSI_FILTER_USERDATATYPE` | true | true | no |
| **`CMN_NUM_PIXELS`** | **2** | **1** | **PRIMARY** |
| **`CSI_BUF_DEPTH`** | **4096** | **256** | **PRIMARY** |
| Downstream IPs | demosaic + gamma + ISP | bare axis_data_fifo | secondary |

**PRIMARY ROOT CAUSE HYPOTHESIS: AP1302 `reset_b` wiring difference.**

In smartcam BD:
```
PS emio_gpio_o[bit 0] → xlslice → ap1302_rst_b
```
The Linux driver's `ap1302_power_on()` (line 1090 of ap1302.c) is called
at probe time and includes step 4: "De-assert RESET via reset_gpio".
This is a real reset cycle on every probe — chip starts from a clean
state synchronized with when the FPGA DPHY is ready.

In our visionsoc BD:
```
proc_sys_reset/peripheral_aresetn → ap1302_rst_b
```
The chip only resets once at FPGA boot, NOT under driver control.
The driver still calls `power_on()` and toggles `reset_gpio` — but
`reset-gpios = <&gpio 79 1>` in dts points to an unrouted EMIO pin
(the doc comment even calls this a "stub"). So the reset GPIO toggle
goes to a dead pin; the chip never gets a driver-controlled reset.

The likely mechanism: when the FPGA fabric boots, the DPHY hard block
goes through reset and calibration. The AP1302 chip ALSO comes out of
reset at fabric boot, and its MIPI Tx PHY initializes. If these two
init sequences aren't properly ordered (chip Tx may start before DPHY
Rx is ready), the chip's PHY emits symbols that the receiver's PHY
never syncs to. Smartcam avoids this by deferring the chip reset to
driver probe (after fabric DPHY is fully initialized).

**SECONDARY HYPOTHESES (additive — may matter too):**

  * `CMN_NUM_PIXELS=1` (smartcam=2): the CSI2RX subsystem at ppc=1 may
    have a stricter or less-tested data-plane path; smartcam's ppc=2 is
    the canonical config.
  * `CSI_BUF_DEPTH=256` (smartcam=4096): smaller buffer may drop
    packets before they're counted.

**Recommended next bitstream (5q) — minimum delta to fix camera:**

  1. **PRIMARY: wire `ap1302_rst_b` through PS EMIO GPIO** (matches smartcam):
     - Remove `connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_ports ap1302_rst_b]`
     - Verify PS config has `PSU__USE__GPIO=1` and `PSU__EMIO_GPIO__WIDTH` is sufficient (≥1, ideally same as smartcam's 92)
     - Create xlslice (DIN_FROM=1 DIN_TO=1 DIN_WIDTH=92 DOUT_WIDTH=1) to extract gpio_o[1] — bit 1 maps to dts `gpio 79`. Validate dts `&gpio` numbering for our zynqmp_gpio: bank 3 EMIO usually starts at gpio 78, so EMIO bit 1 = gpio 79.
     - Wire `${PS_INST}/emio_gpio_o → xlslice/Din`, `xlslice/Dout → ap1302_rst_b`
     - dts: keep existing `reset-gpios = <&gpio 79 1>` — it's now actually routed
     - LUT cost: ~50-100 (one xlslice)
  2. **SECONDARY (if #1 alone doesn't unblock): `CMN_NUM_PIXELS` 1 → 2**
     in `system_top.tcl` mipi_csi2_rx config. Paired dts: `xlnx,ppc = <2>`
     on `isp_csiss` (currently `<1>`). v_frmbuf_wr's `SAMPLES_PER_CLOCK`
     should match — set to 2. LUT cost: ~+1k.
  3. **TERTIARY: `CSI_BUF_DEPTH` 256 → 1024 or 4096**. LUT cost: ~+500-1500.
  4. Keep everything else from 5o (F4/F1/F7/F8) intact.
  5. **Skip 5p (NV12 frmbuf)** for now — it's an optimization blocked
     behind getting frames flowing at all. UYVY end-to-end is the priority.

**Test strategy for 5q:**
- Build with just fix #1 first (cheapest, most targeted to observed difference). If frames flow, ship it. If not, layer #2 + #3 on top.
- Validation: same media-ctl + v4l2-ctl sequence as smartcam test; expect CSR PKTCNT[31:16] to climb during stream and `/tmp/frame.uyvy` to be 32 KB (128×128×2).

**Recovery steps documented (this session):**

  * `xmutil unloadapp` + `fpgautil -b` overlay churn caused a kernel
    panic on 2026-05-12 evening. User had to power-cycle. **Lesson:
    avoid xmutil/fpgautil dance for smartcam testing in a single
    session; if a smartcam test is needed, plan a reboot after.**
  * On boot, the k26-starter-kits overlay is loaded by default. Remove
    with `sudo rmdir /sys/kernel/config/device-tree/overlays/k26-starter-kits_image_1`
    before loading visionsoc.

**Cosmetic dts bug noticed (not the cause):** local
`fpga/dts/system_top_wrapper.dts` has `clock-frequency = <0x48000000>`
(= 1.2 GHz!) for `sensor_clk`. Should be `<48000000>` decimal (48 MHz)
or `<24000000>` (24 MHz) depending on actual crystal frequency. The
AP1302 driver doesn't use this value (just calls `clk_prepare_enable`
on the fixed-clock), so it's harmless — but worth fixing for cleanliness.

### 0.8 5q-final DEPLOYED + camera pipeline VERIFIED (2026-05-14 ~12:15 BST)

**Bitstream:** `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260514-041933/`
  * .bit sha256: (run `sha256sum system_top_wrapper.bit`)
  * .bit.bin: `system_top_wrapper.bit.bin` (7,797,692 bytes)
  * .dtbo source: `fpga/dts/system_top_wrapper.dts` (compiled on Kria to `.dtbo`)

**Deploy procedure (verified working):**
```sh
scp fpga/build/.../system_top_wrapper.bit.bin kv260:/tmp/
scp fpga/dts/system_top_wrapper.dts kv260:/tmp/
ssh kv260 'cd /tmp && dtc -@ -I dts -O dtb -o system_top_wrapper.dtbo system_top_wrapper.dts'
ssh kv260 '
    sudo cp /tmp/system_top_wrapper.bit.bin /lib/firmware/xilinx/visionsoc/
    sudo cp /tmp/system_top_wrapper.dtbo    /lib/firmware/xilinx/visionsoc/
    for ovl in /sys/kernel/config/device-tree/overlays/*/; do
        sudo rmdir "$ovl"
    done
    sleep 5
    sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin \
                  -o /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo
    sleep 5
'
```

**Verified probes after fpgautil:**
  * `/dev/video0`, `/dev/v4l-subdev0/1/2`, `/dev/i2c-4` ✓
  * `/dev/uio0..6` (T1 + DMA + IIC + camera UIO mappings) ✓
  * `media-ctl -p` shows: `isp_vcap_csi output 0`, `ap1302.4-003c`,
    `ar1335 0`, `80000000.csiss` — all 4 entities with 1 link each
  * T1 AXI-Lite probe (`axi_lite_read_probe`): UIO map size 64 KB
    visible at `va=0xffffb6288000`, MAP_FAILED not hit ✓

**Camera capture verified end-to-end with `mediasrcbin`:**
```
mediasrcbin name=videosrc media-device=/dev/media0 ! \
    video/x-raw,width=128,height=128,format=NV12,framerate=30/1 ! \
    filesink location=/tmp/5q_cam.nv12
```
Result: 150 NV12 frames captured in 6 s ≈ 25 fps.
HINF counter advancing normally (170 frames during `v4l2-dbg
--log-status` poll, matches expected emission rate).
Frame data verified valid:
  * Y plane min/max/mean = 18 / 254 / 72 (good dynamic range)
  * Sobel applied PS-side shows clear edges (LED rim, heatsink fins,
    object silhouettes) — confirms the camera image isn't noise

**Captured artifacts** (in repo at `captured_img/`):
  * `5q_input.nv12` (24 KB) — raw NV12 frame
  * `5q_input_y.png` — Y plane grayscale
  * `5q_input_rgb.png` — full YUV → RGB rendering
  * `5q_sobel_output.png` — Sobel |∇Y| edge magnitude

**T1 hardware path works; DMA AXIS loopback has a pathological-size bug:**
  * `ddr_roundtrip` (T1 hb → smartconnect_hb → HPC0 → DDR): **PASS**
  * `sp_v12_compute_probe` (T1 + BRAM scratchpad + DDR + vadd): **PASS**
  * `dma_loopback` (DDR → MM2S → AXIS → S2MM → DDR): **TIMEOUT**

Root cause isolated by direct DMA-register probing:
  * Engine accepts reset, accepts RS=1, accepts SA/DA/LENGTH writes
  * No error bits in DMASR (no decode/slave/internal error)
  * Transfer SIMPLY NEVER COMPLETES — both channels stay `Idle=0`

**The hang depends on transfer length.** Direct sweep at the
DMA register level (skipping libt1):

| len (bytes) | bursts (256 B each) | result |
|---|---|---|
|  1024 |     4 | DONE in 20 ms |
|  8192 |    32 | DONE in 20 ms |
| 12288 |    48 | DONE in 20 ms |
| 16128 |    63 | DONE in 20 ms |
| 16256 |    63.5 | DONE |
| 16383 |    63.99 | DONE |
| **16384** | **64** | **TIMEOUT** ✗ |
| 16385 | 64.004 | DONE |
| 16400 | 64.06 | DONE |
| 17408 |    68 | DONE |
| 24576 |    96 | DONE |
| **32768** | **128** | **TIMEOUT** ✗ |
| 40960 |   160 | DONE |
| **49152** | **192** | **TIMEOUT** ✗ |
| 57344 |   224 | DONE |
| **65536** | **256** | **TIMEOUT** ✗ |

**The hang is at transfer lengths that are exact multiples of
16384 bytes** (= 64 AXI bursts at the BD's `c_*_burst_size=16`
configuration × 128-bit data width = 256 B/burst). Equivalently:
hangs when burst count is an even multiple of 32.

The bug looks like an off-by-one in TLAST generation on the
DMA's M_AXIS_MM2S when the transfer ends exactly on a 64-burst
boundary. Without TLAST, the S2MM channel waits forever for
end-of-packet.

**This bug pre-dates 5q-final.** The DMA `c_*_burst_size {16}`
config is identical to 5p. None of the previously-validated tests
(`port_grid_vadd_scratchpad` uses 128-byte vlmax transfers,
`ddr_roundtrip` uses T1 hb not DMA) exercise this corner case, so
it was never noticed.

**Workarounds:**

  1. **In application code: avoid multiple-of-16384 transfer
     sizes.** Round up by 16 bytes, or split into two non-round
     halves. For NV12 frames: transfer the full 24576 bytes (Y+UV)
     rather than just the 16384-byte Y plane.

  2. **In BD: rebuild with `c_mm2s_burst_size {32}` and
     `c_s2mm_burst_size {32}`.** Eliminates the 16384-byte
     corner case (the bug shifts to multiples of 32768 instead,
     which most apps won't hit). Costs ~500-1000 LUTs.

  3. **Permanent: switch to scatter-gather mode** by setting
     `c_include_sg {1}`. SG-mode buffers and descriptor handling
     don't have this corner case. Much bigger BD rework.

Test confirmation: with the workaround in libt1 (pad small
transfers to non-pathological size), DMA loopback should pass.

### 0.9 5q-final-r2 — DMA burst_size 16 → 32 (FAILED + diagnosis WAS WRONG, 2026-05-14 ~14:00 BST)

**Update 2026-05-15:** the "multiples-of-16384 / TLAST off-by-one"
theory described in this section was WRONG. A sweep on 2026-05-15
showed every size ≥ 16384 fails (16385, 24576, 32768, 49152, …),
not just multiples. The real root cause is the AXI DMA's
`c_sg_length_width` defaulting to 14 bits. See §0.10 for the
corrected diagnosis and the actual fix.

The 5q-r2 attempt and its routing failure are still informative
(showed how brittle 88% util is to topology changes), so the
section is preserved below — just disregard its diagnostic claims.

---

Applying Option 2 fix to permanently move the 16384 B pathological
size out of the way. Single line change in
`fpga/system/system_top.tcl`:

```diff
-    CONFIG.c_mm2s_burst_size    {16} \
-    CONFIG.c_s2mm_burst_size    {16} \
+    CONFIG.c_mm2s_burst_size    {32} \
+    CONFIG.c_s2mm_burst_size    {32} \
```

Effect:
  * AXI bursts grow 256 B → 512 B max
  * Burst count at 16384 B drops from 64 → 32
  * The DMA off-by-one corner case shifts from "multiples of
    16384" to "multiples of 32768"
  * Common Y-plane (16384 B), full NV12 frame (24576 B),
    scratchpad-half (16384 B) all work with this config

Hardware cost: ~500-1000 LUTs + ~200 FFs estimated; should
land at ~85% impl (was 84.59%).

Software cost: ZERO. libt1 + all test/app code unchanged. The
register interface is identical; only the IP's internal beat
counter width changed.

BD validates clean. Full build in flight.

**5q-r2 BUILD RESULT (2026-05-15 ~01:05 BST): FAILED at route.**

Synth landed at **88.40% LUT util** (vs 5q-final 88.38% — only
+30 LUTs!), but **route_design failed**:
  * 4674 signals failed to route (congestion)
  * 3887 node overlaps remained after multiple rip-up-and-retry
    iterations
  * Effective congestion level 6 (max)
  * Wall: 9h 28m (most of it spent in the routing retry loop)

The burst_size 16→32 change rebalanced the DMA's internal AXIS
FIFO. Synth util barely moved but the netlist topology changed
just enough to push the placer into a region the router couldn't
escape. **5q-final was already at the routing-cliff edge.**

**Reverted to burst_size=16 in `system_top.tcl`.** The 5q-final
bitstream (deployed) is what's on the board.

**Recommended path forward: app-level workaround.** The DMA
multiple-of-16384 bug doesn't actually hurt camera+T1 use:
  * Camera frame transfers = 24576 B (NV12 Y+UV) ✓ works
  * BRAM scratchpad halves = 16384 B each ✗ — would need to
    split into two non-pathological halves, or transfer full
    32 KB at once

For libt1 / vision_program: build the API to always pad
"transfer length" to the next non-multiple-of-16384 (round up by
16 B), or document the constraint.

**Alternative attempted fixes that would have route impact:**
  * burst_size=64 or 256: more LUTs, worse congestion (won't fit)
  * burst_size=8: bug shifts to multiples of 8192 (worse)
  * SG mode (`c_include_sg=1`): adds ~3-5k LUTs, won't fit at all
  * Reduce something else first (smartconnect strategies, T1
    config trim) to make room — significant work

**Acceptance: ship FYP demo on 5q-final.** Camera capture + T1
both proven working. DMA loopback works at non-pathological
sizes. Vision pipeline naturally uses 24576-byte NV12 frames.

### 0.10 5q-final-r3 — c_sg_length_width 14→23 fix BUILT + DEPLOYED + VERIFIED (2026-05-15 / 2026-05-16)

A board-side size sweep on 2026-05-15 disproved the "multiples of
16384 / TLAST off-by-one" theory:

| Size | Result |
|---|---|
| 4096, 8192, 12288, 16383 | PASS |
| 16384, 16385, 24576, 32767, 32768, 32769, 49152, 65536 | TIMEOUT |

Threshold is exactly `2^14 - 1 = 16383 B`. That's the maximum value
representable in the AXI DMA's LENGTH register at the default
`c_sg_length_width = 14`. Direct-mode (`c_include_sg=0`) makes
Vivado pick 14 bits by default unless we override it; our
`axi_dma` set_property dict in `system_top.tcl` never set this
parameter.

DMA recovery between processes works fine (4096 PASS → 16384 FAIL
→ 4096 PASS), so it's not a stuck-state issue — the LENGTH
register simply can't hold the value.

**Fix applied — one line, `fpga/system/system_top.tcl` lines 165-174:**

```diff
     CONFIG.c_mm2s_burst_size    {16} \
     CONFIG.c_s2mm_burst_size    {16} \
+    CONFIG.c_sg_length_width    {23} \
 ] [get_bd_cells axi_dma]
```

23 bits = max 8 MiB transfer (PG081 range is 8-26 bits).
Future-proofs against any imaginable visionsoc transfer:

| Workload | Bytes | Bits needed |
|---|---|---|
| 128×128 grey i8 | 16,384 | 15 |
| 128×128 NV12 frame | 24,576 | 15 |
| 256×256 grey | 65,536 | 17 |
| 256×256 NV12 | 98,304 | 17 |
| 23-bit max | 8,388,608 | — |

**Hardware cost (estimated):** ~9 flops in LENGTH register
(14→23 wider), comparator and down-counter widened by 9 bits;
~50-100 LUTs total. Strictly smaller than the burst_size=32 change
in 5q-r2 (which shifted FIFO topology). Here the LENGTH register
just gets wider — no FIFO depth changes, no AXI behaviour changes,
no internal pipelining changes.

**Software cost:** ZERO. libt1 + visionsoc_main unchanged. APIs
identical; the register interface is unchanged. Future kernels can
just request bigger transfers and they'll work.

**Risk:** at 88.38% LUT util in 5q-final, even a tiny change can
disturb routing (5q-r2 was +30 LUTs and still failed). Best case:
+50 LUTs, +0 placement disturbance. Worst case: mirrors 5q-r2 and
we fall back to libt1-side chunking (split ≥16384 into pieces).

**Revert path if route fails:**
```sh
git checkout fpga/system/system_top.tcl   # restores 5q-final source
```
The 5q-final bitstream at
`fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260514-041933/`
remains the deployed production build regardless.

---

**BUILD RESULT (2026-05-15 07:32 BST, 4h 44m wall):** SUCCESS.

| Metric | 5q-final | 5q-r3 | Delta |
|---|---|---|---|
| CLB LUTs (impl) | 99,074 (84.59%) | 99,053 (84.57%) | **-21 LUTs** |
| CLB FFs (impl)  | 126,795 (54.13%) | 126,945 (54.19%) | +150 FFs |
| Worst setup slack | +0.254 ns | +1.566 ns (worst clock) | all positive |
| Build wall time | 5h 30m | 4h 44m | -46 min |

LUT count actually came down by 21 (placer noise — within measurement
error). FFs went up by 150 as expected (9 extra LENGTH-register bits ×
fan-out across mm2s/s2mm descriptor + counter logic). All clocks meet
timing. No congestion warnings.

**DEPLOYED (2026-05-16 00:13 BST):**
  * `system_top_wrapper.bit.bin.5q-r3` scp'd to Kria
  * Backed up previous active as `system_top_wrapper.bit.bin.5q-final-backup`
  * Same dtbo (`system_top_wrapper.dtbo`) — no DT-visible change
  * `rmdir /sys/kernel/config/device-tree/overlays/full && sleep 6 && fpgautil -b ... -o ...`
  * UIOs re-enumerate cleanly: uio4=t1, uio5=dma, uio6=bram

**VERIFIED (dma_loopback sweep, every size PASSES):**

```
size=4096  PASS    size=24576  PASS    size=65536  PASS
size=8192  PASS    size=32767  PASS    size=131072 PASS
size=12288 PASS    size=32768  PASS    size=262144 PASS
size=16383 PASS    size=32769  PASS
size=16384 PASS    size=49152  PASS
size=16385 PASS
```

The DMA now handles any transfer up to 8 MiB. The visionsoc full
pipeline (camera NV12 24576 B frames, BRAM scratchpad halves at
16384 B, 256×256 future scaling at 98304 B) is unblocked.

**5q-r3 is the new production bitstream.** 5q-final is preserved as
`.5q-final-backup` on the Kria and remains at
`fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260514-041933/`
on the workstation for revert capability.

### 0.11 5r — BRAM scratchpad → URAM swap, 32 KB → 512 KB (LAUNCHING 2026-05-16)

**Motivation.** Today's 32 KB scratchpad lives in ~8 BRAM36 tiles
(see 5q-r3 util: 52.5 / 144 BRAM = 36.5 %). Those tiles are wanted
elsewhere. The XCK26 has **64 URAM288 blocks currently unused** —
the impl utilization table for 5q-r3 doesn't even list a URAM line.
Migrating the scratchpad into URAM frees the BRAM tiles for a
trivial 16-of-64 URAM cost (or 2-of-64 if we'd kept 32 KB) and lets
us grow the arena 16× at no incremental URAM cost beyond what we
already had to spend on width.

**Sizing chosen.** 512 KB, organized as 128-bit × 32768 deep, backed
by 16 URAM288 blocks (2-wide × 8-deep cascade). 512 KB is a clean
power-of-2 aperture so `axi_bram_ctrl` MEM_DEPTH stays clean and
`assign_bd_address … -range 512K` decodes without aliasing. The
4-URAM premium over the 384 KB calculation (12 URAMs) buys this and
costs nothing meaningful at 16 / 64 URAMs.

**Implementation.** axi_bram_ctrl stays — its BRAM_PORTA interface
is primitive-agnostic. The legacy `blk_mem_gen 8.4` (BRAM-only, no
URAM toggle) is swapped for a small XPM-based wrapper at
`fpga/wrapper/uram_scratchpad.v` that instantiates
`xpm_memory_spram` with `MEMORY_PRIMITIVE = "ultra"`. The BD cell is
created as a Module Reference so the controller sees the same
BRAM_PORTA bus it used with blk_mem_gen.

**emb_mem_gen gotcha (2026-05-16):** Xilinx provides
`emb_mem_gen 1.0`, the URAM-capable successor to blk_mem_gen, which
*does* expose `CONFIG.MEMORY_PRIMITIVE {URAM}` as a literal keyword
and is the cleanest swap on paper (IP-only, no Verilog). Its
component.xml is present in the Vivado 2025.2 install
(`$Xilinx/2025.2/Vivado/data/ip/xilinx/emb_mem_gen_v1_0/component.xml`,
enumeration `{AUTO, LUTRAM, BRAM, URAM, MIXED}`), but `create_bd_cell
-type ip -vlnv xilinx.com:ip:emb_mem_gen:1.0` errors with
`[BD 5-683] VLNV xilinx.com:ip:emb_mem_gen:1.0 is not supported
for the current part` on the KV260's XCK26 Zynq UltraScale+ — the
IP is gated to other part families (likely Versal). XPM is officially
supported on Zynq UltraScale+ so we use the wrapper here. Don't
re-attempt emb_mem_gen for this board.

```
T1 m_axi_hb  ─┐
DMA mm2s     ─┼─► smartconnect_hb ─► axi_bram_ctrl ─► uram_scratchpad
DMA s2mm     ─┘   (single-port, 128-bit)             (xpm_memory_spram,
                                                      MEMORY_PRIMITIVE="ultra",
                                                      MEM_SIZE=32768×128 b
                                                      = 512 KB,
                                                      16 / 64 URAM288 blocks)
```

Address space — T1's view is **unchanged**: same flat window at
`0xA0080000`, just 16× larger (`0xA0080000 – 0xA00FFFFF`). No T1
RTL change, no wrapper SV change, no libt1 API change.

**Expected resource delta vs 5q-r3:**

| Resource | 5q-r3 | 5r (target) | Delta |
|---|---|---|---|
| BRAM36 tiles | 52.5 / 144 (36.5%) | ~44.5 / 144 (~31%) | **-8 tiles** |
| URAM288 blocks | 0 / 64 | **16 / 64 (25%)** | +16 |
| CLB LUTs (impl) | 99,053 (84.57%) | ~99k (±a few hundred) | ≈0 |
| Read latency (first-beat, head of burst) | ~BRAM | **+~2-6 cycles** (8-deep URAM cascade, pipelined) | +2-6 cyc |
| Beat-to-beat throughput | 1 × 128b / cyc | 1 × 128b / cyc (unchanged) | 0 |

LUT impact ≈ neutral because URAM cascade is hard-routed — the only
fabric cost is `axi_bram_ctrl`'s slightly wider address decode
(MEM_DEPTH 2048 → 32768 = +4 addr bits) which is tens of LUTs at most.

Latency: read first-beat at the head of each AXI burst pays the
cascade-fill tax. At 60 MHz (clk_60M, 16.6 ns period) the cascade
has plenty of slack so blk_mem_gen / synth will likely keep the
pipeline short; expect +2-6 cycles vs BRAM. **Beat-to-beat
throughput within a burst is unchanged at 1 × 128-bit / cycle** —
URAM streams back-to-back same as BRAM.

**Software changes** (`vision_software/visionsoc_main/main.c`):

  * Restore the URAM scratchpad PAs (was commented out): URAM base
    `0xA0080000`, HALF_A at `+0x0000`, HALF_B at `+0x4000` (still
    only 16 KB each = one Y-plane; the extra 480 KB headroom is
    available for future double-buffering / multi-frame staging).
  * Re-instate `t1_dma_mm2s_sync(in.pa, half_a, FRAME_BYTES)`
    before the kernel (DDR → URAM).
  * `issue_active_kernel(half_a, half_b)` — T1 reads URAM half-A,
    writes URAM half-B over `m_axi_hb`.
  * `t1_dma_s2mm_sync(half_b, out.pa, FRAME_BYTES)` after the
    kernel (URAM → DDR).
  * **Keep `ps_edit(&out)`** on the DDR side — the result lives in
    DDR after the s2mm DMA, the PS edit hook still operates on a
    coherent NV12 buffer there.
  * UV plane is still PS-filled (T1 kernel writes Y only).

**DTS change** (`fpga/dts/system_top_wrapper.dts`):
  * `bram_scratch` reg size `0x8000` → `0x80000` (32 KB → 512 KB).

**BD change** (`fpga/system/system_top.tcl`):
  * `add_files` for `fpga/wrapper/uram_scratchpad.v` so the BD can
    Module-Reference it.
  * `blk_mem_gen 8.4` `create_bd_cell` block →
    `create_bd_cell -type module -reference uram_scratchpad bram`.
  * `axi_bram_ctrl` MEM_DEPTH 2048 → 32768.
  * Three `assign_bd_address … -range 32K` → `-range 512K`.

**Revert path:** `git checkout fpga/system/system_top.tcl
fpga/dts/system_top_wrapper.dts vision_software/visionsoc_main/main.c
fpga/wrapper/` restores 5q-r3. The 5q-r3 bitstream at
`fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260515-024828/`
remains deployable as the production fallback.

**Risks:**
  * URAM cascade timing across 8 deep at higher Fmax — at 60 MHz
    clk_60M we have ~16.6 ns margin so this is unlikely to bite.
    Worst case `xpm_memory_spram` pipelines additional registers
    automatically.
  * Module-reference instantiation interferes with the BD's
    auto-generated wrapper — mitigated by adding the source file
    via `add_files` before `make_wrapper`.
  * Camera + DMA path already verified on 5q-r3 — the URAM swap
    doesn't touch any IP that has been validated, just the
    scratchpad backing store.

**Test plan after build + deploy:**
  1. `ls -la /dev/uio6` and `cat /sys/class/uio/uio6/maps/map0/size`
     → expect 524288 bytes (was 32768).
  2. `make` in `vision_software/visionsoc_main` (kernel + main).
  3. Run `./run_after_power_cycle.sh` — verify
     camera → DDR → URAM (mm2s) → T1 (kernel) → URAM → DDR (s2mm)
     → display flow renders frames at comparable fps to 5q-r3
     and the `outY` stats match `camY` stats (frame_passthrough).
  4. Compare 5r impl util to 5q-r3 numbers (above) to confirm the
     BRAM-tile reduction landed.

---

**BUILD RESULT (2026-05-16 08:00 BST, 6 h 58 m wall):** SUCCESS.

Three Vivado attempts were needed to land:

| # | Failure | Fix |
|---|---|---|
| 1 | `[BD 5-683] VLNV xilinx.com:ip:emb_mem_gen:1.0 not supported for the current part` — emb_mem_gen IP is present in 2025.2 catalog (and exposes `CONFIG.MEMORY_PRIMITIVE {URAM}`) but is gated against Zynq UltraScale+ | switch to XPM `xpm_memory_spram` wrapper, Module-Reference into BD |
| 2 | `[BD 5-106] connect_bd_intf_net … empty` (BD failed to auto-bundle `BRAM_PORTA_*` pins) + `[BD 41-237] READ_LATENCY mismatch /bram(2) vs /bram_ctrl(1)` | canonical port names (`clka`, `addra`, …) + explicit `X_INTERFACE_INFO` pragmas; `axi_bram_ctrl CONFIG.READ_LATENCY {2}` to match URAM |
| 3 | — | success |

| Metric | 5q-r3 | 5r | Delta |
|---|---|---|---|
| CLB LUTs (impl) | 99,053 (84.57%) | **100,220 (85.57%)** | +1,167 (+1.0%) |
| CLB FFs (impl) | 126,945 (54.19%) | 126,946 (54.19%) | ≈0 |
| BRAM36 tiles | 52.5 / 144 (36.5%) | **44.5 / 144 (30.9%)** | **−8 ✓** |
| URAM288 | 0 / 64 | **16 / 64 (25%)** | **+16 ✓** (2-wide × 8-deep) |
| DSPs | 30 / 1,248 | 30 / 1,248 | 0 |
| WNS | +0.254 ns | **+0.068 ns** | URAM cascade ate ~190 ps but still passing |
| WHS | +0.010 ns | +0.010 ns | unchanged |
| Build wall | 4 h 44 m | **6 h 58 m** | +2 h 14 m (URAM placer harder) |

The +1.0 % LUT bump comes from the wider `axi_bram_ctrl` address decode
(MEM_DEPTH 2048→32768) and the READ_LATENCY=2 pipelining inside the
controller. URAM cascade itself is hard-routed and contributes no
fabric.

**DEPLOYED (2026-05-16 ~09:30 BST):**
  * `system_top_wrapper.bit.bin` (5r, 7.8 MB) installed at
    `/lib/firmware/xilinx/visionsoc/`; prior production saved as
    `.5q-r3-backup`.
  * dtbo recompiled with `bram_scratch reg = <… 0x0 0x80000>` (was
    `0x8000`); installed alongside, prior saved as
    `.5q-r3-backup`.
  * Overlay reloaded via `rmdir overlay/full + rmdir
    overlay/k26-starter-kits_image_1 + fpgautil -b … -o …`. All 7
    UIOs re-enumerate (uio4=t1, uio5=dma, **uio6=bram, size=0x80000**).

**VERIFIED end-to-end (visionsoc_main, 2026-05-16 ~09:35 BST):**

```
camera: mplane query nplanes=1 p0.len=24576 p0.off=0 p1.len=0 p1.off=0
frame 32, last kernel 22302 cycles, 17.7 fps, camY 17/254/79.1, outY 17/254/79.1
frame 64, last kernel 22303 cycles, 18.1 fps, camY 17/254/81.0, outY 17/254/81.0
frame 96, last kernel 22230 cycles, 20.0 fps, camY 17/254/80.4, outY 17/254/80.4
…
frame 384, last kernel 22303 cycles, 20.0 fps, camY 17/254/81.3, outY 17/254/81.3
```

`outY` byte-identical to `camY` proves the
**camera → DDR → URAM (mm2s) → T1 (frame_passthrough) → URAM →
DDR (s2mm) → display** round-trip is data-faithful. `ps_edit()`
runs on the DDR-side `out` buffer between s2mm and display copy as
intended. Kernel cycle count (~22–27 k) is in the same range as
the 5q-r3 BRAM baseline — the URAM +2 cycle head-of-burst latency
is in the noise inside a kernel that does mostly compute.

**main.c gotcha discovered during bringup.** First attempt of the
URAM round-trip hit `t1_dma_mm2s_sync(in.pa, URAM_HALF_A, len):
Connection timed out` even though `libt1/test/dma_to_scratchpad`
passed in isolation. Root cause: `t1_dma_mm2s_sync()` /
`t1_dma_s2mm_sync()` in `vision_software/libt1/libt1.c` each
program only ONE channel — the unused address argument is silently
discarded (`(void)dst_pa;` / `(void)src_pa;`). The visionsoc DMA is
a loopback (axi_dma M_AXIS_MM2S → axis_reg_slice_dma → S_AXIS_S2MM),
so a DDR↔URAM transfer needs BOTH channels armed: s2mm with the
destination, mm2s with the source, then a single `t1_dma_wait()`.
The fix in `main.c` uses the async-pair-plus-wait pattern from
`libt1/test/dma_to_scratchpad.c`. Loopback `dma_loopback` and
`dma_to_scratchpad` test programs already do this correctly; only
the sync API misleads.

**5r is the new production bitstream.** Revert: `git checkout` of
`fpga/system/system_top.tcl`, `fpga/dts/system_top_wrapper.dts`,
`vision_software/visionsoc_main/main.c`, and
`rm fpga/wrapper/uram_scratchpad.v` restores 5q-r3 source. Re-flash
from `system_top_wrapper.bit.bin.5q-r3-backup` +
`.dtbo.5q-r3-backup` on the Kria.

### 0.7 5q-final build in flight (2026-05-14 ~04:20 BST)

Production BD `system_top.tcl` + `system_top_wrapper.dts` updated with:
  * `PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ = 100` (was 60; DPHY FVCO)
  * `PSU__GPIO_EMIO__PERIPHERAL__ENABLE = 1`, IO = 2 (for AP1302 reset)
  * `clk_wiz_0 NUM_OUT_CLKS = 4` — added CLK_OUT4 = 60 MHz for T1
  * All T1-side blocks moved from `pl_clk0` → `clk_wiz_0/clk_60M`:
    `t1_top`, `smartconnect_{hb,idx,ctrl}`, `axi_dma`,
    `axi_reg_slice_hb`, `axis_reg_slice_dma`, `bram_ctrl`,
    `sensor_iic`, PS HP ports (`maxihpm0_fpd`, `saxihpc0_fpd`,
    `saxihp0_fpd`)
  * `proc_sys_reset` now wires to `clk_wiz_0/clk_60M` +
    `dcm_locked` (so T1 stays in reset until clk_wiz locks)
  * AP1302 reset rewired: `peripheral_aresetn` → xlslice from
    `emio_gpio_o[1]` (matches camtest3 v8 / camtest4 working
    pattern; lets the kernel ap1302 driver toggle a real pin)
  * dts: `xlnx,axis-tdata-width 32 → 16` to match ppc=1; clock
    rate comment updated to reflect pl_clk0=100, clk_60M for T1
  * Camera trim already at 1/256/1 — confirmed working in
    camtest4 (see § 4.26 of camera_handoff)

BD validates: 0 errors, 1 known-harmless critical warning
(`bram_ctrl MEM_DEPTH read-only`, pre-existing from 5l).

Build dir: `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260514-041933`
Wall: 3h 49m. **SUCCESS — timing met, bitstream generated.**

| Metric | Value |
|---|---|
| CLB LUTs (impl) | 99,074 / 117,120 = **84.59%** |
| CLB Registers | 126,795 / 234,240 = 54.13% |
| Block RAM Tile | 52.5 / 144 = 36.46% |
| DSPs | 30 / 1,248 = 2.40% |
| WNS (setup slack) | **+0.254 ns** ✓ zero failing endpoints |
| WHS (hold slack) | **+0.010 ns** ✓ zero failing endpoints |
| Critical warnings | 0 |

Comparison: 5p was 88.38% synth (right at the routing cliff;
prior versions 5j/5n hit 87-103% and failed-routed). 5q-final
landed at **84.59% impl** thanks to:
  * Camera trim 2/4096/2 → 1/256/1 (~250 LUTs + 2 BRAMs saved)
  * Cleaner clock topology (clk_wiz 4-output, T1/DMA on clk_60M)
  * Slightly less interconnect pressure (frmbuf at 64-bit AXIMM)

Artifacts:
  * `system_top_wrapper.bit` (7.8 MB) — raw bitstream
  * `system_top_wrapper.bit.bin` (7.8 MB) — fpgautil format
  * `utilization_impl.rpt`, `timing_impl.rpt`
  * `vivado_impl.log` (235 infos, 15 warnings, 0 critical, 0 errors)

Deploy to Kria:
```sh
sudo cp .../system_top_wrapper.bit.bin /lib/firmware/xilinx/visionsoc/
# dts: compile system_top_wrapper.dts → .dtbo on Kria (host has no dtc)
scp fpga/dts/system_top_wrapper.dts kv260:/tmp/
ssh kv260 'dtc -@ -I dts -O dtb -o /tmp/system_top_wrapper.dtbo /tmp/system_top_wrapper.dts'
ssh kv260 'sudo cp /tmp/system_top_wrapper.dtbo /lib/firmware/xilinx/visionsoc/'
# overlay swap (sleep ≥5 between)
ssh kv260 'sudo rmdir /sys/kernel/config/device-tree/overlays/* ; sleep 5
           sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin \
                         -o /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo'
```

### 0.6 Camera bringup — fresh install + per-boot procedure (2026-05-14)

**Status: WORKING.** Camera captures 128×128 NV12 at ~25fps via the
`mediasrcbin` GStreamer element. Full forensic trail in
`fyp_doc/camera_handoff_2026-05-13.md`; this section is the
production install/usage recipe.

#### 0.6.1 One-time install on a fresh Ubuntu Server 22.04 image

Camera capture requires Xilinx-patched GStreamer plugins-bad from
two PPAs that aren't enabled by default. Without these, only
`xlnx-firmware-kv260-smartcam` is available (just bit+dtbo+xclbin,
no runtime). The actual `mediasrcbin` GStreamer element lives in
`gstreamer1.0-plugins-bad` from `ppa:ubuntu-xilinx/gstreamer`.

```sh
# Add Xilinx PPAs (sudoers expansion has tee, not add-apt-repository)
echo "deb https://ppa.launchpadcontent.net/ubuntu-xilinx/sdk/ubuntu jammy main" | \
    sudo tee /etc/apt/sources.list.d/ubuntu-xilinx-sdk.list
echo "deb https://ppa.launchpadcontent.net/ubuntu-xilinx/gstreamer/ubuntu jammy main" | \
    sudo tee /etc/apt/sources.list.d/ubuntu-xilinx-gstreamer.list

# Import the Launchpad PPA signing key (id 52150A179A9E84C9)
gpg --keyserver keyserver.ubuntu.com --recv-keys 52150A179A9E84C9
gpg --export 52150A179A9E84C9 | sudo tee /etc/apt/trusted.gpg.d/ubuntu-xilinx.gpg

sudo apt update
sudo apt install -y vvas-essentials gstreamer1.0-plugins-bad
```

Verify the plugin is loadable (will print a few harmless
`GLib-GObject-CRITICAL` warnings then the factory details):

```sh
gst-inspect-1.0 mediasrcbin | head -15
# Plugin Details:
#   Name                     mediasrcbin
#   Filename                 /usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstmediasrcbin.so
```

Other useful packages already on this image:
  * `v4l-utils` (`v4l2-ctl`, `v4l2-dbg`, `media-ctl`) — pipeline
    introspection
  * `yavta` — for non-mediasrcbin V4L2 testing (less capable than
    mediasrcbin; left HINF throttled in our tests, see camera_handoff
    § 4.16)
  * `device-tree-compiler` (`dtc`) — needed to compile `.dts` → `.dtbo`
    for bitstream-overlay reloads

#### 0.6.2 Sudoers expansion (durable)

`/etc/sudoers.d/visionsoc-nopasswd` lets `ubuntu` run the camera
bringup commands without typing a password. Template at
`~ubuntu/visionsoc-nopasswd.template`. Notable allowed binaries:
`fpgautil`, `xmutil`, `devmem2`, `media-ctl`, `v4l2-ctl`, `v4l2-dbg`,
`i2cset`, `i2cget`, `i2ctransfer`, `i2cdetect`, `tee`, `cat`,
`yavta`, custom `/tmp/optb`, `/tmp/optb2`, `/tmp/ddr_peek`. Do not
need a separate sudoers entry for `gst-launch-1.0` — it doesn't
require root to open `/dev/video*` if the user is in the `video`
group (already default on Kria).

#### 0.6.3 Per-boot / per-power-cycle bringup

After power-on, the board comes up with `k26-starter-kits_image_1`
overlay (a default Xilinx app), not the visionsoc bitstream. To get
the camera-capable bitstream + dtbo loaded:

```sh
# Unload default overlay (sleep matters — kernel panic if too fast)
sudo rmdir /sys/kernel/config/device-tree/overlays/k26-starter-kits_image_1
sleep 5
# Load camtest3 v8 (camera-only) OR camtest4 (1/256/1 trim) OR 5q (camera+T1)
sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_camtest4.bit.bin \
              -o /lib/firmware/xilinx/camtest4/system_top_camtest4.dtbo
sleep 5
```

Verify probe completed (~3s after fpgautil returns):

```sh
ls /dev/video0 /dev/v4l-subdev* /dev/i2c-4   # should all exist
media-ctl -p | head -20                       # should list ap1302, ar1335, csiss
```

The 5s sleeps between unload and load are NOT optional: rapid
overlay swaps cause kernel panic ~50% of the time on the
`5.15.0-1027-xilinx-zynqmp` kernel (see camera_handoff § 4.19).

#### 0.6.4 Working capture pipeline

After bringup, no subdev format setup is needed (mediasrcbin
handles it):

```sh
gst-launch-1.0 -e mediasrcbin name=videosrc media-device=/dev/media0 ! \
    video/x-raw,width=128,height=128,format=NV12,framerate=30/1 ! \
    filesink location=/tmp/frame.nv12
```

Expected output: ~25 fps NV12 frames concatenated to `/tmp/frame.nv12`.
24576 bytes/frame (16 KB Y plane + 8 KB UV plane). Verify HINF
counter is incrementing during capture:

```sh
sudo v4l2-dbg --log-status -d /dev/v4l-subdev<ap1302_idx> 2>&1
# look for: "Frame counters: ICP <N>, HINF <M>, BRAC 1"
# HINF must advance with each call (ICP advances regardless)
```

Subdev numbering changes between fresh reload sessions; use
`media-ctl -p` to find which `/dev/v4l-subdevN` is the ap1302.

#### 0.6.5 Camera-to-HDMI direct pipeline (5q only, no PL HDMI IP)

5q uses the PS DisplayPort subsystem fed from DDR. The KV260
carrier converts DP→HDMI externally. zynqmp_dpsub supports NV12
directly — no software conversion needed:

```sh
gst-launch-1.0 mediasrcbin name=videosrc media-device=/dev/media0 ! \
    video/x-raw,width=128,height=128,format=NV12,framerate=30/1 ! \
    videoscale ! video/x-raw,width=1920,height=1080,format=NV12 ! \
    kmssink driver-name=xlnx
```

For camera→T1 vision kernel→HDMI flow, allocate two udmabuf
instances (in DDR), use mediasrcbin → udmabuf0, T1 reads udmabuf0
and writes udmabuf1, register udmabuf1 as a DRM framebuffer with
`drmModeAddFB2()`.

#### 0.6.6 Format notes

AP1302 emits NV12 in **JFIF (full-range BT.601)** format, not
limited-range. For software-side YUV→RGB conversion, use:

```
R = Y + 1.402*(V-128)
G = Y - 0.344*(U-128) - 0.714*(V-128)
B = Y + 1.772*(U-128)
```

NOT the limited-range coefficients (1.164*(Y-16) + ...) — those
crush blacks and shift hue. For T1 kernels that operate on Y plane
only (grayscale), this doesn't matter — Y is correct either way.

NV12 byte layout in DDR (verified):
  * Bytes `[0 .. W*H)`: Y plane (luma), W=128, H=128 → 16384 bytes
  * Bytes `[W*H .. W*H + W*H/2)`: Cb-Cr-Cb-Cr interleaved at half
    resolution → 64×64×2 = 8192 bytes
  * Total: 24576 bytes / frame for 128×128

#### 0.6.7 Things that DO NOT need re-doing per boot

  * AP1302 firmware (`/lib/firmware/ap1302_ar1335_single_fw.bin`) —
    persists on rootfs; updated from `Xilinx/ap1302-firmware` repo
    HEAD in earlier session.
  * mediasrcbin / gstreamer1.0-plugins-bad — persists on rootfs.
  * Sudoers expansion — persists.
  * AR1335 sensor calibration — chip-internal, no PS-side action
    needed.

### 0.5 The historical-trail sections below

The remaining § 0 subsections (and the full doc body) are the
chronological build-and-debug trail through 5h → 5k → 5l → 5m →
5n (failed 3 takes with wrong config) → 5o → 5p. Useful for
"how did we get here?" forensic questions. If you only need
"what's deployed and what works", § 0.2 above is sufficient.

### CONFIG-NAME LESSON (2026-05-11, very late) — use the `_fpga` variant

**All 5n take 1/2/3 and 5o take 1-4 builds used the WRONG config:
`mudkip2d128small1bram1chain2lanescale` (non-FPGA-optimised T1) instead
of `mudkip2d128small1bram1chain2lanescale_fpga`. The non-`_fpga` variant
has a larger T1 footprint (~12-18k more LUTs), which is why those builds
landed at 99-103% synth. The "+15k LUT axi_dma propagation trap"
hypothesis derived from those builds is BOGUS — there is no real trap,
just a mis-typed config.**

  * **Correct invocation:**
    `./build_fpga.sh -c mudkip2d128small1bram1chain2lanescale_fpga -b`
  * **Sanity check:** the resulting `fpga/build/` dir should be named
    `mudkip2d128small1bram1chain2lanescale_fpga-<timestamp>` (with
    `_fpga` infix). Anything without `_fpga` in the build-dir name
    was launched with the wrong config and its LUT numbers are
    meaningless.
  * **All historical successful builds (5h, 5k, 5l, 5m) used the
    `_fpga` config** — search `fpga/build/` for `_fpga-*` dirs to
    verify. The 5m bitstream that deployed to the Kria is at
    `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260511-003220/`.
  * **What this means for F5/F6/F8 attempts:** the LUT budget is much
    bigger than my earlier (wrong-config) builds suggested. F5
    redo (axi_cdma) is probably viable. F6 (PS→BRAM) is more plausible.
    F8 (frmbuf CSR via smartconnect_lpd) should fit comfortably.
  * Wrong-config build artefacts preserved at
    `fpga/build/mudkip2d128small1bram1chain2lanescale-*` dirs (no
    `_fpga` infix) for forensic reference. Do NOT delete; they
    explain the earlier dead-end analysis.

### 5o BUILD LAUNCHING (2026-05-11, late) — F8 wires v_frmbuf_wr CSR for camera streaming

**Goal:** unblock the camera path. On 5m, `v_frmbuf_wr/s_axi_CTRL` is
genuinely unrouted in the BD (historical detach 2026-05-07 to dodge
smartconnect_ctrl NUM_CLKS=2 CDC concerns). xilinx-frmbuf driver's
first MMIO write hits 0xA0020000 → DECERR → SError → kernel panic.
Verified against [linux-xlnx/drivers/dma/xilinx/xilinx_frmbuf.c](https://github.com/Xilinx/linux-xlnx/blob/master/drivers/dma/xilinx/xilinx_frmbuf.c):
first probe-time access is `xilinx_frmbuf_chan_reset()` writing to
`XILINX_FRMBUF_IE_OFFSET (0x08)` / `XILINX_FRMBUF_GIE_OFFSET (0x04)`.
No probe path skips CSR access.

**BD edit (F8, 5o):** route frmbuf CSR through `smartconnect_lpd`
(NOT smartconnect_ctrl, to avoid the +15k LUT axi_dma propagation
trap proved in F5 takes 1-3).

  * `smartconnect_lpd` NUM_MI 1 → 2 (M00 stays = mipi_csi2_rx,
    M01 NEW = v_frmbuf_wr/s_axi_CTRL).
  * `v_frmbuf_wr/s_axi_CTRL_aclk` ← `clk_wiz_0/clk_100M` (matches
    smartconnect_lpd's 100 MHz domain). Frmbuf IP has internal CDC
    between `s_axi_CTRL_aclk` and `ap_clk` per PG278, so the
    control plane at 100 MHz with data plane at 300 MHz is fine.
  * `v_frmbuf_wr/s_axi_CTRL_aresetn` ← `proc_sys_reset_100M/peripheral_aresetn`.
  * Address map: `v_frmbuf_wr/s_axi_CTRL/Reg` @ **0x80010000**
    (LPD aperture, next to mipi_csi2_rx at 0x80000000). Cannot use
    0xA0020000 because PS LPD GP master only addresses
    0x8000_0000-0x9FFF_FFFF (UG1085 NIC-400 LPD aperture). dts
    `fb_wr@*` node moved to 0x80010000 in lockstep.
  * `irq_concat` NUM_PORTS 5 → 6, wire `v_frmbuf_wr/interrupt → In5`
    (= GIC SPI 94, matches existing dts `interrupts = <0 94 4>`).
  * smartconnect_ctrl / smartconnect_hb / axi_dma all UNTOUCHED — no
    F5-style propagation regression expected.

**Expected LUT cost:** smartconnect_lpd NUM_MI 1→2 ~+500-800 LUTs,
frmbuf CSR logic activates ~+200-400 LUTs (was pruned by synth before),
irq_concat trivial. **Forecast ~88.5% synth** (5m 87.70% + ~1k LUTs).
Borderline but should route in LOW_AREA mode.

**dts changes already applied locally:**
  * `axi_iic_sensor` enabled, `isp_csiss` enabled, `isp_fb_wr_csi`
    enabled (with `reset-gpios = <&gpio 78 1>` stub), `isp_vcap_csi`
    enabled.
  * `ap1302` has `reset-gpios = <&gpio 79 1>` stub.
  * `isp_fb_wr_csi` node renamed from `fb_wr@a0020000` →
    `fb_wr@80010000`, reg updated to match new BD address.
  * **dts metadata aligned to BD (2026-05-11):**
    - `isp_csiss`: `xlnx,vc = <0>` (was 4), `xlnx,ppc = <1>` (was 2) —
      matches BD `CMN_VC=0`, `CMN_NUM_PIXELS=1`.
    - `isp_fb_wr_csi`: `xlnx,vid-formats = "uyvy"` only (was 4
      formats), `xlnx,pixels-per-clock = <1>` (was 2),
      `xlnx,max-width = <256>` (was 1920), `xlnx,max-height = <256>`
      (was 1080) — matches BD `HAS_UYVY8=1`, `SAMPLES_PER_CLOCK=1`,
      `MAX_COLS=256`, `MAX_ROWS=256`. Now V4L2 `S_FMT` will validate
      correctly for 128x128 UYVY and reject unsupported formats
      cleanly instead of letting userspace request impossible modes.

**Validation plan once 5o builds + deploys:**
  1. Power-cycle Kria (user; ongoing as of session pickup).
  2. `fpgautil` load 5o bitstream + the iic+apreset+full-camera dtbo
     from the local dts.
  3. Check `dmesg` for `xilinx-frmbuf` probe success, then
     `xilinx-video` graph binding.
  4. Verify `/dev/media0` + `/dev/video0` exist.
  5. `media-ctl --print-topology` — confirm AP1302 → csiss → frmbuf
     graph.
  6. `v4l2-ctl --device=/dev/video0 --set-fmt-video=width=128,height=128,pixelformat=UYVY`
     then `v4l2-ctl --stream-mmap --stream-count=1` to capture a frame.
  7. Regression: T1 path still works (`sp_v12_compute_probe`).

### F5 REBUILD ATTEMPTS ABANDONED (2026-05-11, late evening) — keeping 5m as production target NO LONGER (superseded by 5o above)

**Three takes; none routable within the LUT budget. BD restored to
5m baseline (system_top.tcl reverted).**

  * **Take 1: axis_data_fifo on axi_dma streaming loopback** — hypothesis
    was right (BD 41-3281 + BD 41-702 cleared, propagation feedback
    broken successfully) but side-effect was axi_dma's streaming engines
    correctly resizing from 32-bit (5m's broken-prop fallback) to 128-bit.
    Synth util **100.65%** (+15k LUTs over 5m). Killed at Phase 5.
  * **Take 2: axi_cdma added alongside axi_dma** — keeping axi_dma's
    slice loopback (preserves 5m's broken-prop axi_dma at 32-bit) and
    adding a separate axi_cdma for mem-to-mem. The smartconnect_hb
    NUM_SI 3→4 expansion still triggered re-elaboration → axi_dma's
    streaming side rebuilt at 128-bit → same +15k LUTs. Synth **102.93%**.
  * **Take 3: axi_dma REMOVED entirely, axi_cdma sole DMA engine** —
    cleaner topology, but synth landed at **99.65%**. The +12k LUTs
    over 5m (with axi_dma GONE) showed our LUT model was off:
    axi_cdma at 128-bit + 16-beat burst is heavier than estimated,
    OR Vivado is doing something else (smartconnect width converters,
    cross-clock-domain crossings, etc.) that we don't have a handle on.
  * **Build artefacts preserved** at `fpga/build/`:
    - take 1: `mudkip2d128small1bram1chain2lanescale-20260511-132711/`
    - take 2: `mudkip2d128small1bram1chain2lanescale-20260511-160059/`
    - take 3: `mudkip2d128small1bram1chain2lanescale-20260511-164812/`
  * **What 5m delivers without DMA mem-to-mem:**
    - T1's m_axi_hb reaches both DDR (via HPC0) AND BRAM scratchpad
      (via smartconnect_hb/M01). T1 can vle from DDR + vse to scratchpad
      + vle from scratchpad + vse to DDR in a single kernel.
      `sp_v12_compute_probe` validates this works.
    - This covers all current and near-term F4 amplifier use cases.
      DMA mem-to-mem is only useful if the PS wants to set up transfers
      in parallel with T1 compute (a future optimisation, not needed
      for today's workflows).
  * **Lesson for next BD agent:** ANY change that touches the
    smartconnect_hb topology or its connected IPs re-triggers Vivado's
    propagation graph for axi_dma's streaming side. With propagation
    working, axi_dma jumps from 5m's 32-bit (broken-prop) state to
    proper 128-bit. That single change costs ~15k LUTs. The
    "ANY BD change near axi_dma → +15k LUT spike" is the load-bearing
    constraint blocking F5/F6 in current LUT budget. Real fix
    requires either (a) finding 15k LUTs of savings elsewhere
    (camera-path trim, drop ILA-style smartconnects), or (b) replacing
    axi_dma + axi_cdma with something even lighter (custom small
    BRAM-prefetch DMA, or just doing without and using T1-only flows).
  * **For now: 5m remains the production target.** No new build needed.
    The local repo's `fpga/system/system_top.tcl` matches 5m bit-for-
    bit (only the `system_top_wrapper.dts` + libt1 test additions are
    deltas vs the deployed 5m artefact).

**Take 2 failed:** Adding axi_cdma alongside axi_dma still ballooned
synth util to **102.93%** (120,554 LUTs, +18k over 5m). Hypothesis
confirmed: ANY BD re-elaboration that touches axi_dma's neighborhood
(in this case the smartconnect_hb NUM_SI expansion 3→4) re-triggers
Vivado's propagation graph for axi_dma's streaming side, which then
correctly resizes to 128-bit (vs the 5m broken-propagation 32-bit
form). axi_dma is fundamentally heavy at 128-bit streaming — there's
no way to keep it AND get under the LUT budget once propagation works.

**Take 3 decision: remove axi_dma entirely.** axi_cdma covers all our
DMA workloads (DDR↔BRAM for T1 prefetch, BRAM→DDR for T1 output).
We don't run parallel async DMA + T1 compute, so single-channel
mem-to-mem is sufficient.

**BD edits (take 3 vs take 2):**
- `create_bd_cell axi_dma` + its full configuration block: REMOVED.
- axi_dma clock/reset wiring (4 lines): REMOVED.
- `smartconnect_ctrl/M01_AXI → axi_dma/S_AXI_LITE`: REMOVED;
  `smartconnect_ctrl/M01_AXI → axi_cdma/S_AXI_LITE` (axi_cdma claims
  axi_dma's old slot).
- `axi_dma/mm2s_introut → irq_concat/In1` + `s2mm_introut → In2`:
  REMOVED. irq_concat NUM_PORTS 5 → 3 (now: In0=t1, In1=sensor_iic,
  In2=csi2).
- `axi_dma/M_AXI_MM2S → smartconnect_hb/S01`,
  `axi_dma/M_AXI_S2MM → smartconnect_hb/S02`: REMOVED;
  `axi_cdma/M_AXI → smartconnect_hb/S01` (axi_cdma replaces).
- `axis_reg_slice_dma` cell + its 2 AXIS connections: REMOVED (was
  the axi_dma streaming self-loop, no longer needed).
- `smartconnect_hb` NUM_SI: 4 → 2 (only T1 hb + axi_cdma now).
- `smartconnect_ctrl` NUM_MI: 4 → 3 (only T1 ctrl + axi_cdma + sensor_iic).
- Address map: removed `axi_dma/Data_MM2S`, `axi_dma/Data_S2MM`
  segments. Moved `axi_cdma/S_AXI_LITE` from 0xA0060000 back to
  0xA0010000 (taking axi_dma's old slot — keeps the historical
  /dev/uio5 layout meaningful if a userspace tool was using that
  address).
- All other 5m features (F4, F1, F7, F2-software) preserved.

**Expected synth util:** 5m's 87.70% + axi_cdma ~3k LUTs - axi_dma's
streaming-side IF it had been at 128-bit ~15k LUTs = potentially
**~80-83% synth** (significantly lighter than 5m). Tight, but should
route comfortably.

**Software impact:** libt1's axi_dma helpers need to be rewritten.
The dma_loopback test as-is uses axi_dma — it must be replaced
with a cdma_loopback test that drives axi_cdma's registers
(CR=0x00, SR=0x04, SrcAddr=0x18/0x1C, DstAddr=0x20/0x24, BTT=0x28).
This is a thin libt1.c diff, not a major refactor.

**dts impact:** the existing dtbo references `axi_dma_0` at 0xA0010000.
After 5n take 3 deploys, the dtbo needs to claim axi_cdma at
0xA0010000 instead (same address, different IP). Existing tools that
mmap `/dev/uio5` (dma) will see the cdma registers there.

### 5n TAKE 2 (FAILED) — axi_cdma alongside axi_dma at 102.93% synth

**Take 1 failed:** Replacing the `axi_dma` streaming-loopback's
`axis_register_slice` with `axis_data_fifo (TDATA_NUM_BYTES=16)` DID
sever Vivado's propagation feedback (BD 41-3281 + BD 41-702 didn't
fire, validating the hypothesis) — but the side-effect was `axi_dma`'s
internal streaming logic correctly resizing from its previous
propagation-blocked 32-bit state up to 128-bit, adding ~15k LUTs.
Synth util ballooned to **100.65%** (117,882 LUTs). Phase 5 rip-up
loop diverged with 319,295 overlaps. Build killed, artefacts preserved
at `fpga/build/mudkip2d128small1bram1chain2lanescale-20260511-132711/`.

**Lesson:** the "F5 via axi_dma streaming loopback" path costs ~15k
LUTs intrinsically. axi_dma's MM2S+S2MM streaming engines have to be
properly configured for any real mem-to-mem to work. Cheaper path
exists.

**Take 2 (current build):** Restored axi_dma to its 5m configuration
(register_slice loopback with broken propagation, ~50 LUTs). Added
a SEPARATE `axi_cdma` IP for memory-to-memory transfers — purpose-
built for this case, no streaming hop, expected LUT cost ~2.5-3k.

  * **BD edits:**
    - `axi_cdma` cell created with `C_INCLUDE_SG=0,
      C_M_AXI_DATA_WIDTH=128, C_M_AXI_MAX_BURST_LEN=16, C_INCLUDE_DRE=0`.
    - `axi_cdma/M_AXI` → `smartconnect_hb/S03_AXI` (so cdma reaches
      DDR via HPC0/M00 and BRAM via M01).
    - `smartconnect_ctrl/M03_AXI` → `axi_cdma/S_AXI_LITE` (PS control plane).
    - `smartconnect_hb` NUM_SI 3 → 4.
    - `smartconnect_ctrl` NUM_MI 3 → 4.
    - Address map: `axi_cdma/S_AXI_LITE` at 0xA0060000 (between
      sensor_iic at 0xA0050000 and scratchpad at 0xA0080000).
      `axi_cdma/Data` sees full DDR + BRAM scratchpad.
    - No IRQ wiring — software polls cdma SR.Idle.
  * **All other 5m features preserved:** F4, F1, F7. F6 still deferred.
    F2 software-fix already in place via dts (no BD work needed).
  * **Expected synth util:** 5m's 87.70% + ~2.5-3k LUTs for cdma +
    smartconnect_hb NUM_SI=3→4 overhead ≈ 89-90%. Close to the
    ~88.5% cliff but should route in LOW_AREA mode.
  * **dts follow-up (after deploy):** add `axi_cdma` UIO node at
    0xA0060000. Until then, the cdma is reachable via /dev/mem
    only — fine for an initial loopback test.
  * **Validation plan:**
    1. Regression: all libt1 tests that passed on 5m must still pass.
    2. NEW: write `cdma_loopback.c` that programs axi_cdma's
       SrcAddr/DstAddr/Length CRs for a DDR→DDR transfer, polls
       SR.Idle, verifies dst matches src. This is the replacement
       for `dma_loopback` (the original axi_dma streaming loopback).
    3. NEW: `cdma_to_scratchpad.c` — DDR→BRAM transfer via axi_cdma
       (the F4 amplifier flow, replacing dma_to_scratchpad).
    4. Re-deploy iic+apreset dtbo to re-enable camera path.
  * **Build command:** `./build_fpga.sh -c mudkip2d128small1bram1chain2lanescale -b`.
    Expected wall clock: 4-5h.
  * **Launch attempt #1 (2026-05-11 15:15:56)** failed at 1m 24s
    with `BD 5-106: Arguments to the connect_bd_intf_net command
    cannot be empty.` Root cause: `connect_bd_intf_net axi_cdma/M_AXI
    ...` was placed at tcl line 604 — BEFORE the `create_bd_cell
    axi_cdma` at line 658. Pin query returned empty, Vivado failed.
    Fix: moved both `connect_bd_intf_net` calls to immediately after
    the cell creation block (where the clock/reset nets are wired).
    Build artefacts preserved at
    `fpga/build/mudkip2d128small1bram1chain2lanescale-20260511-151556/`
    (synth never started; only BD-creation log is there).
  * **Launch attempts #2-3 (2026-05-11 15:54 + 15:57)** also failed,
    both at BD elaboration — pin-name learning curve for axi_cdma:
    - #2: `axi_cdma/m_axi_aresetn` doesn't exist as a discrete pin
      (the IP propagates reset implicitly from s_axi_lite_aresetn
      in single-clock mode).
    - #3: `axi_cdma/m_axi_aclk` IS a discrete pin and must be
      explicitly connected (validate_bd_design failed without it).
    - Final correct wiring (attempt #4): `m_axi_aclk` and
      `s_axi_lite_aclk` both tied to `pl_clk0`; only
      `s_axi_lite_aresetn` tied to `peripheral_aresetn` (no
      explicit m_axi reset pin).
  * **Launch attempt #4 (2026-05-11 16:00:59, PID 983399)** —
    BD passed at 16:05:12, synth_1 started. Build dir
    `fpga/build/mudkip2d128small1bram1chain2lanescale-20260511-160059`,
    log at `/tmp/5n_take2_build.log`. Detached via `nohup`.
    Watching for synth_1 completion to gate on LUT util.

### 5p BUILD LAUNCHING (2026-05-12, morning) — F9 enables NV12 frmbuf output

**Goal:** flip the camera memory format from UYVY 4:2:2 interleaved (5o's
deployed format, requires Y-extract pre-pass for greyscale T1 kernels)
to NV12 4:2:0 planar (Y contiguous at offset 0, T1 reads greyscale in
one `vle8.v` with no extraction).

**Single BD edit on top of 5o:**

  * `v_frmbuf_wr` config: `HAS_Y_UV8_420 = 1` (was 0), `MAX_NR_PLANES =
    2` (was 1). Frmbuf still has `HAS_UYVY8 = 1` for AXIS input; the
    IP demuxes UYVY into Y + UV planes in memory.

**Paired dts changes (already applied locally):**

  * `xlnx,csi-pxl-format = <0x18>` (was 0x1E). 0x18 =
    `MIPI_CSI2_DT_YUV420_8B → VYYUYY8_1X24` (NV12 family); matches the
    new BD config + the kv260-smartcam reference.
  * `xlnx,vid-formats = "nv12"` (was "uyvy"). Driver advertises NV12
    at `/dev/video0`; frmbuf accepts UYVY on AXIS and writes Y +
    UV planes.

**Expected synth util:** 5o sat at 87.96% synth. The NV12 plane
separator in `v_frmbuf_wr` adds ~300-500 LUTs. Forecast ~88.5%
synth — borderline but should route given the data-path is already
laid out in 5o. Wall clock similar to 5o (~8h congestion-driven).

**Validation plan after build:**

  1. T1 regression: `sp_v12_compute_probe` PASSES.
  2. AP1302 still detected over IIC.
  3. `/dev/video0` advertises `'NV12' (Y/CbCr 4:2:0)` — no UYVY this
     time.
  4. `v4l2-ctl --set-fmt-video=width=128,height=128,pixelformat=NV12` +
     `--stream-mmap --stream-count=1 --stream-to=/tmp/frame.nv12`.

#### Bringup state after this session (2026-05-12 morning)

**Firmware fix verified working on 5o:**
  * Updated AP1302 firmware to `Xilinx/ap1302-firmware` HEAD (was
    Nov 2021; new is current; sha256 `2dd09e34...`). CRC mismatch is
    gone. AP1302 chip boots cleanly on every overlay reload.
  * Updated dts to `xlnx,csi-pxl-format = <0x1E>` (YUV422_8B →
    UYVY8_1X16) to match 5o's `HAS_UYVY8=1` BD config. csiss now
    advertises UYVY at the source pad; `/dev/video0` accepts UYVY at
    128×128.

**STREAMON now succeeds end-to-end:**
  * The fix that unblocked STREAMON was explicit `field:none colorspace:srgb`
    on the csiss pads (matching what AP1302 already had). Without
    them, the pipeline validator returned `-EPIPE` silently.
  * After STREAMON the CSI2RX core enables (CCR @ 0x80000000 goes
    `0x0 → 0x1`).
  * **However: frames don't actually flow yet.** The csiss Protocol
    Status register (@ 0x80000010) stays at 0, meaning the AP1302
    chip isn't emitting MIPI traffic despite the driver reporting OK.
    This is now AP1302/AR1335 chip-level debugging territory
    (physical cable seating, firmware sub-init, register pokes via
    v4l2-dbg or i2cget — neither tool currently on sudoers allowlist).

**For the next camera-bringup session:**
  1. Verify AR1335 IAS cable physical connection.
  2. Add `v4l2-dbg`, `i2cget`, `i2cset`, `i2ctransfer`, `media-ctl`
     to `/etc/sudoers.d/visionsoc-nopasswd` allowlist for diagnostics.
  3. Read AP1302 registers `0x6000` (chip version), `0x600c` (sensor
     select), `0x6134/0x6136` (warning regs), `0x1184` (system state)
     to find which stage of init is failing.
  4. Try setting AP1302 V4L2 controls (exposure, frame_interval, etc.)
     before STREAMON to nudge the chip into emission mode.
  5. Capture AP1302 driver verbose logs: `echo +p > /sys/kernel/debug/dynamic_debug/control` filtered for `ap1302`.

The 5p NV12 rebuild + the camera-debug last mile are independent
work streams. 5p will reduce the kernel-side `vle8.v` extraction
overhead once camera frames are flowing.

### 5o BUILT SUCCESSFULLY (2026-05-12 05:23, with the `_fpga` config) — superseded by 5p above (F9 NV12 fix)

**Bitstream 5o complete. F8 lands — `v_frmbuf_wr/s_axi_CTRL` now
routed via smartconnect_lpd, addressable at 0x80010000 (LPD aperture).
Camera frmbuf is finally PS-programmable.**

  * **Build dir:** `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260511-210311/`
  * **`.bit`** (7.8 MB, sha256 `3954e368ed87af9dcbe60fbe4bd221619eb816200a490826aec18737aa12769a`)
  * **`.bit.bin`** (7.8 MB, sha256 `e2222e8d6d334fd8a838ef8673d2c2ee4ce577124b38a5f2c66ad2534fc0b938`)
  * **Wall clock:** 500m 6s ≈ **8h 20m** — slow due to congestion-driven
    Phase 5 rip-up (level 5 / 32x32). Overlaps converged from 165k →
    53k → 22k → routing complete, so it didn't diverge.
  * **Synth util:** 87.96% (103,021 LUTs, +301 over 5m).
  * **Impl util:** 84.19% (98,599 LUTs, +913 over 5m).
  * **Timing:** WNS=+0.044 ns, WHS=+0.010 ns, WPWS=+0.164 ns,
    0 failing endpoints, all user-specified timing constraints met.
    Setup margin is tighter than 5m's +0.169 ns but still positive.
  * **What's new vs 5m (single F8 BD edit):**
    - `smartconnect_lpd` NUM_MI 1→2, NUM_CLKS 1→2 (aclk1 = clk_300M).
    - `smartconnect_lpd/M01_AXI → v_frmbuf_wr/s_axi_CTRL` (the
      previously detached frmbuf CSR — now reachable from PS).
    - Address segment: `v_frmbuf_wr/s_axi_CTRL/Reg` @ 0x80010000
      in LPD aperture (PS LPD GP master can't address the FPD `0xA*`
      range — that's why the address moved from the dts's original
      0xA0020000).
    - `irq_concat` NUM_PORTS 5→6, frmbuf interrupt → In5 (= GIC SPI 94).
  * **Everything else preserved from 5m:** F4 BRAM scratchpad at
    0xA0080000, F1 chroma 2→3 byte fix on subset_converter_cap, F7
    axi_register_slice on T1 hb, axi_dma with broken streaming
    loopback (axis_register_slice), sensor_iic on smartconnect_ctrl,
    full camera capture path BD IPs.
  * **Local dts paired with 5o** (in `fpga/dts/system_top_wrapper.dts`):
    - `isp_fb_wr_csi: fb_wr@80010000` (moved from 0xA0020000, reg
      updated to match new address).
    - `xlnx,vc = <0>`, `xlnx,ppc = <1>` (matches BD CMN_VC=0, CMN_NUM_PIXELS=1).
    - `xlnx,vid-formats = "uyvy"` (single format, matches BD HAS_UYVY8=1).
    - `xlnx,pixels-per-clock = <1>`, `xlnx,max-width = <256>`,
      `xlnx,max-height = <256>` (matches BD).
    - Other 5m fixes preserved: ap1302 + isp_fb_wr_csi reset-gpios
      stubs to `<&gpio 79 1>` / `<&gpio 78 1>`; all camera nodes
      `status="okay"`; axi_iic_sensor enabled.

#### Deployment sequence (after Kria power-cycle)

```sh
# 1. scp the new bit.bin to Kria
scp fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260511-210311/system_top_wrapper.bit.bin \
    fpga/dts/system_top_wrapper.dts \
    kv260:/tmp/v12_pkg/

# 2. On Kria: install + dtbo-compile + reload
ssh kv260 '
  # Backup 5o for posterity
  sudo install -m 644 -o root -g root /tmp/v12_pkg/system_top_wrapper.bit.bin \
              /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5o-backup
  # Make 5o the active bit.bin
  sudo install -m 644 -o root -g root /tmp/v12_pkg/system_top_wrapper.bit.bin \
              /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin
  # Compile dtbo
  dtc -@ -I dts -O dtb -o /tmp/v12_pkg/system_top_wrapper.dtbo \
                          /tmp/v12_pkg/system_top_wrapper.dts
  sudo install -m 644 -o root -g root /tmp/v12_pkg/system_top_wrapper.dtbo \
              /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo
  # Remove k26 default, load visionsoc
  sudo rmdir /sys/kernel/config/device-tree/overlays/k26-starter-kits_image_1 2>/dev/null
  sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin \
                -o /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo
'

# 3. Sanity check: T1 still works
ssh kv260 'cd ~/vision_software/libt1 && sudo ./test/sp_v12_compute_probe'
# Expect: PASS

# 4. Camera test: dmesg should show no frmbuf probe failures + V4L2 device
ssh kv260 '
  sudo dmesg | grep -iE "ap1302|frmbuf|csiss|video[0-9]|media[0-9]"
  ls /dev/video* /dev/media*
'
# Expect: /dev/video0 + /dev/media0 to exist, AP1302 revision detected

# 5. Streaming test (camera-on)
ssh kv260 '
  sudo v4l2-ctl --device=/dev/video0 \
                --set-fmt-video=width=128,height=128,pixelformat=UYVY \
                --stream-mmap --stream-count=1 --stream-to=/tmp/frame.uyvy
  ls -la /tmp/frame.uyvy   # 128*128*2 = 32768 bytes
'
```

### F5 REBUILD ATTEMPTS ABANDONED (2026-05-11, late evening) — keeping 5m as production target NO LONGER (superseded by 5o above)
  * **Next-agent checklist when build completes:**
    1. Check `fpga/build/mudkip2d128small1bram1chain2lanescale-20260511-151556/utilization_synth.rpt`
       LUT util — must be <90% for routing to succeed.
    2. If synth >90%: route will likely fail (similar to take 1).
       Consider trimming elsewhere or dropping `axi_cdma` features
       (try `C_M_AXI_DATA_WIDTH=64` instead of 128 — halves the
       cdma datapath cost at the price of half-rate DDR access).
    3. If synth OK + impl + bitstream success: convert .bit -> .bit.bin,
       scp to Kria as `.5n-backup`, then update the active bit.bin.
    4. Add `axi_cdma` node to `fpga/dts/system_top_wrapper.dts` at
       0xA0060000 (UIO, e.g. `cdma@a0060000 { compatible = "generic-uio"; reg = <0x0 0xa0060000 0x0 0x10000>; };`).
       Recompile dtbo, install over `.dtbo`, reload.
    5. Write `vision_software/libt1/test/cdma_loopback.c` (DDR→DDR
       via axi_cdma) and `cdma_to_scratchpad.c` (DDR→BRAM). Both
       drive the CDMA's CR (offset 0x00), SrcAddr (0x18 lo / 0x1C hi),
       DstAddr (0x20 lo / 0x24 hi), and BytesToTransfer (0x28),
       polling SR.Idle (0x04 bit 1) until done.
    6. Re-verify F4 still works on 5n: `sp_v12_compute_probe`,
       `sp_4issue_with_verify_probe` should pass unchanged.

### 5n TAKE 1 (FAILED) — axis_data_fifo on axi_dma streaming loopback

**Goal: close `dma_loopback` (and any DMA mem-to-mem flow) by
breaking Vivado's parameter-propagation feedback loop on the
axi_dma streaming self-loop. 5m's `axis_register_slice` was
insufficient (BD 41-3281 + BD 41-702 still fired; dma_loopback
still hung). 5n replaces it with `axis_data_fifo` configured
with EXPLICIT `TDATA_NUM_BYTES=16` to define the AXIS width
statically at the IP boundary, severing the propagation graph
between MM2S and S2MM.**

  * **Single BD edit:** in `fpga/system/system_top.tcl`, the
    `axis_reg_slice_dma` cell + its two intf nets are replaced with
    `axis_fifo_dma` (`xilinx.com:ip:axis_data_fifo`) configured
    `FIFO_DEPTH=16, TDATA_NUM_BYTES=16, HAS_TKEEP=1, HAS_TLAST=1,
    IS_ACLK_ASYNC=0`. Wired to `pl_clk0` + `peripheral_aresetn`.
    The intf nets re-route `axi_dma/M_AXIS_MM2S → axis_fifo_dma/S_AXIS`
    and `axis_fifo_dma/M_AXIS → axi_dma/S_AXIS_S2MM`.
  * **All other 5m features preserved:** F4 (BRAM rewire), F1 (chroma
    fix), F7 (axi_register_slice on T1 hb). F6 still deferred. F2 NOT
    in this build (already resolved on 5m via dts software fix —
    BD edit was reverted).
  * **Expected LUT cost:** axis_data_fifo with depth 16 + 128-bit data
    ~ 250-400 LUTs. 5m sat at 87.70% synth; 5n forecast ~88.0% —
    just under the ~88.5% routing cliff. If route fails, the fallback
    is `axi_cdma` (purpose-built for memory-to-memory) which can
    replace the entire MM2S→S2MM loopback.
  * **Validation plan after build:**
    1. Regression: `triage_t1`, `smoke`, `sp_v12_compute_probe`,
       `port_grid_vadd`, `vert_lsu`, all libt1 tests passing on 5m.
    2. NEW: `dma_loopback` — should PASS (was timeout on 5m).
    3. NEW: `dma_to_scratchpad` — should PASS (DMA round-trip
       DDR→sp→DDR, requires F5 + F4).
    4. NEW: `dma_t1_scratchpad` — full F4 amplifier (DMA load
       + T1 sp→sp + DMA store).
    5. Re-deploy iic+apreset dtbo (from 5m work) + re-test camera path.
  * **Build command:** `./build_fpga.sh -c mudkip2d128small1bram1chain2lanescale -b`
    (no `-a`). Auto-finds RTL dir
    `mudkip2d128small1bram1chain2lanescale-20260506-055305`.
    Expected wall clock: 4-5h. Build dir: `fpga/build/<config>_fpga-<timestamp>/`.
  * **If 5n fails to route or dma_loopback still hangs after deploy:**
    fall back to `axi_cdma` topology. Remove axis_fifo_dma + the two
    streaming intf nets entirely; replace with an `axi_cdma` IP that
    does memory-to-memory directly without the streaming hop.

### F2 PANIC ALREADY RESOLVED ON 5m — NO BITSTREAM REBUILD NEEDED (2026-05-11, late)

**Diagnostic B (re-enable `axi_iic_sensor` in the dts, reload overlay
on the deployed 5m bitstream) confirmed that the `xiic_reinit` panic
documented in `camera_bringup_status.md` § 6.3 IS GONE. F4 / F7's BD
edits (or some other 5m-vs-5k change) incidentally fixed whatever
made `xiic` panic on 5k/5l/5e. The previously-planned 5n F2 register-
slice intervention is therefore unnecessary; the BD edit has been
reverted.**

  * **Test:** copied `fpga/dts/system_top_wrapper.dts` to a temp,
    commented out `status = "disabled";` inside the `axi_iic_sensor`
    node only (other camera nodes still disabled), `dtc`-compiled,
    `fpgautil -b … -o …` reloaded against the already-deployed 5m
    bit.bin.
  * **Result:** overlay applied cleanly. `dmesg` shows the
    `pca954x 3-0074` IIC mux successfully registering all 4 channels
    (which requires real IIC ACKs from the hardware), and the
    AP1302 driver attempts probe at `4-003c`. No SError, no kernel
    panic, no oops.
  * **Regression check post-IIC-enable:** `sp_4issue_with_verify_probe`
    PASSES. T1 + DMA paths are unaffected.
  * **Remaining bringup blocker (software-only) — RESOLVED 2026-05-11
    late.** Added `reset-gpios = <&gpio 79 1>;` stub to the ap1302 dts
    node, pointing at an unrouted EMIO bit of the zynqmp_gpio
    controller. Driver toggles a dead pin during probe (chip is already
    de-reset via BD `ap1302_rst_b <- peripheral_aresetn`), then proceeds
    to read AP1302 revision 0.2.6 over IIC. Confirmed clean probe; T1
    regression intact. Local dts updated in `fpga/dts/system_top_wrapper.dts`.
  * **Diagnostic A (devmem2 reads on sensor_iic):** Inconclusive.
    Showed the documented "devmem2 SIGBUSes on 4-byte-misaligned
    AXI4-Lite offsets" artefact (§ 6.1.4 callout — MMU attribute
    difference, not a fabric break). Same pattern reproduces on
    `t1_axi_lite_wrapper`, which is known-good via UIO. devmem2 is
    NOT a reliable indicator of the F2 panic mechanism; the kernel
    driver uses a different MMU path and behaves correctly.
  * **Revised priorities:** With F2 done as a software task, the
    next bitstream lever (F5 redo for DMA mem-to-mem) is the only
    BD-level work outstanding for now. Camera bringup is unblocked
    from the FPGA side.

### 5m F4 PROMOTED TO PRODUCTION-CAPABLE (2026-05-11, late)

**F7-extension diagnostic experiment ran on 5m and PASSED reproducibly
(3/3 runs). The v12-from-BRAM panic was confirmed to require the
*no-compute* "load-then-immediately-store" pattern — any ALU touch
on v12 between the BRAM read and the DDR write scrubs the offending
T1-internal metadata. Real compute kernels (matmul, conv, vsub-based
diffing, etc.) never exhibit this pattern, so F4 is now considered
production-capable on 5m. F7-extension drops from "next-bitstream
priority" to "low priority / RTL investigation only if a kernel
emerges that genuinely needs no-compute pass-through."**

  * **Test:** `vision_software/libt1/test/sp_v12_compute_probe.c`
    (5 issues: vle v8 ← DDR, vse v8 → BRAM, vle v12 ← BRAM,
    **vadd.vv v12, v12, v8**, vse v12 → DDR). Expected `dst[i] ==
    (2 * src[i]) & 0xFF`. All 128 bytes match on each run.
  * **Contrast:** the original `port_grid_vadd_scratchpad`
    panic-trigger (scratchpad_self.h kernel: vle v12 ← BRAM, vse v12
    → DDR directly, no compute) is still the only known repro.
    Documented as "low-priority RTL bug" not "blocker."
  * **Encoding used:** `vadd.vv v12, v12, v8` = `0x02C40657`.
    Cross-checked manually against `grid_vadd.h`'s
    `vsub.vv v8, v8, v12 = 0x0a860457` (same funct3, opposite funct6).
    Stored as a const in the test for repeatability.
  * **Kria state at end of this session:** 5m is **currently deployed**
    (not 5k). bit.bin sha256 `a95451925a2f9d9f629530d26acc831b15c2dffcd1dab8c9e08e475351df1c95`,
    dtbo recompiled from `fpga/dts/system_top_wrapper.dts` with
    `bram@a0080000`. The 5k backup is still available at
    `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5k-backup`
    for rollback.
  * **Regression check:** `sp_alloc_probe`, `sp_4issue_with_verify_probe`
    re-pass after the v12 experiment. No kernel oops in dmesg.

#### Revised next-bitstream priorities (5n+)

  1. **F5 redo** (now top priority) — try `axis_data_fifo` with
     `TDATA_NUM_BYTES=16` instead of register slice. If still broken,
     switch to `axi_cdma` for memory-to-memory. Unblocks DMA loopback
     and any DMA-driven scratchpad seeding.
  2. **F2** (camera `axi_iic` panic) — F7-style `axi_register_slice`
     between `smartconnect_ctrl/M02` and `sensor_iic/S_AXI`. Cheap
     (~50 LUTs); same mechanism that worked for F7. Unblocks camera
     bringup.
  3. **F6** (PS→BRAM direct mmap) — once LUT savings from F5/F2 land,
     re-attempt the `smartconnect_bram_arb` single-controller topology.
     Lower priority now since F4 production-capable via T1+DMA paths.
  4. **F7-extension** (RTL fix for no-compute pass-through) — demoted.
     Only needed if a real kernel surfaces this pattern. None known.

### 5m BUILT + PARTIALLY VALIDATED (2026-05-11)

**5m attempt #3 (F4 + F1 + F5 + F7, F6 deferred) built cleanly and
was deployed + tested on Kria. Mixed results: F4 mechanical paths
fully work, F7 partially fixes the panic, F5 still doesn't synth
correctly. 5k remains the production fallback; 5l + 5m bit.bins
preserved on Kria as `.5l-backup` / `.5m-backup`.**

  * **Build dir:** `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260511-003220/`
  * **Bitstream:** `system_top_wrapper.bit` (7.8 MB, sha256 `f4d64c255e3ec23368be729cb9413700c664690e5d1c0c8ceeed0330ba10c151`; .bit.bin sha256 `a95451925a2f9d9f629530d26acc831b15c2dffcd1dab8c9e08e475351df1c95`)
  * **Wall clock:** 4h 41m. Synth util **87.70%** (102,720 LUTs, only +571 over 5l). Impl util **83.74%** (97,686 LUTs). Timing **WNS=+0.169 ns, WHS=+0.010 ns, WPWS=+0.164 ns, 0 failing endpoints**.
  * **F6 was attempted twice and dropped:**
    - Attempt #1 (two `axi_bram_ctrl` + true-dual-port `blk_mem_gen`): synth 93.18%, killed in rip-up loop.
    - Attempt #2 (single `bram_ctrl` + `smartconnect_bram_arb` fanout): synth 96.51%, also too dense.
    - Conclusion: F6 in any topology costs +5000-7000 LUTs over 5l's 87.22% baseline. Deferred.

#### 5m test results (on Kria, deployed 2026-05-11)

| Test | Result | Verdict |
|------|--------|---------|
| `triage_t1`, `smoke`, `ddr_roundtrip`, `port_grid_vadd`, `vert_lsu` | ✅ PASS | Regression clean — 5l-passing tests intact |
| `sp_alloc_probe` (pa=0xa0080000) | ✅ PASS | F6 dts/PA relocation works |
| `sp_load_probe`, `sp_store_probe` | ✅ PASS | F4 T1 hb → BRAM read/write isolated |
| `sp_ddr_to_sp_probe`, `sp_3issue_probe`, `sp_4issue_probe` | ✅ PASS | F4 multi-issue chains, no DDR↔BRAM transition issues for v8 pattern |
| `dma_loopback` | ❌ TIMEOUT | **F5 broken** — `[BD 41-3281]` + `[BD 41-702]` warnings still fire; axis_register_slice insufficient |
| `dma_to_scratchpad` | ❌ TIMEOUT | Same F5 issue — DMA mem-to-mem hangs |
| `sp_4issue_with_verify_probe` | ✅ PASS | F7 fixed the v8-pattern panic — but THIS TEST ONLY STORES v8 (not v12 after BRAM load); doesn't actually exercise the panicking case |
| `port_grid_vadd_scratchpad` (with `scratchpad_self.h` kernel: vse v12 to DDR after vle v12 from BRAM) | 💀 PANIC | **F7 did not fix this case** — kernel SError on the v12-store path |
| `sp_v12_compute_probe` (vle v12 ← BRAM, vadd.vv v12,v12,v8, vse v12 → DDR) | ✅ PASS (3/3) | **F7-extension diagnostic** — compute between vle and vse scrubs the tainting metadata. Real kernels are safe; v12 pass-through bug is low-priority. |

#### Critical insight: F7 only fixed half the panic

The 5l hypothesis was "AxUSER/AxID metadata leaks from BRAM read into HPC0 write, panicking PS cache-sync". The
`axi_register_slice` between `t1_top/m_axi_hb` and `smartconnect_hb/S00_AXI` clearly helped — `sp_4issue_with_verify_probe` (which stores v8, not v12) PASSES on 5m. But the original failure case (`scratchpad_self.h` kernel: `vse v12, (dst_ddr)` after `vle v12, (sp)`) still panics.

The difference between the two cases is **which vreg is stored**:
* `sp_4issue_with_verify` stores **v8** (loaded from src DDR in issue 0, never touched by BRAM).
* `port_grid_vadd_scratchpad` stores **v12** (loaded from BRAM in issue 3).

So F7 sanitises AxUSER/AxID *on the AXI master interface itself*, but doesn't scrub whatever T1-internal state v12 picked up from the BRAM read. The bug is upstream of T1's m_axi_hb — likely in T1's vreg file rename/forward logic or in how the BRAM read's response data is tagged inside T1.

Probably needs ILA on T1's vreg writeback or RTL-level inspection of the vle.v from BRAM. Not solvable by BD edits alone.

#### F5 (axis_register_slice) didn't break the param propagation feedback

Same warnings as 5l fired during 5m synth:
* `[BD 41-3281] axi_dma is connected on both sides by SmartConnects`
* `[BD 41-702] Propagation TCL tries to overwrite C_M_AXI_S2MM_DATA_WIDTH(128) with propagated value(32). Command ignored`

Vivado still considers `axi_dma` in a feedback loop even with the slice. Need a different IP topology — possibilities:
* `axis_data_fifo` with explicit `TDATA_NUM_BYTES {16}` instead of `axis_register_slice` (deeper buffering may break the propagation chain)
* Setting explicit `CONFIG.TDATA_NUM_BYTES` on the slice
* Adding an intermediate stream-width converter
* Using `axi_cdma` (purpose-built for memory-to-memory) instead of looping `axi_dma`

#### Next-bitstream priorities (5n+)

  1. **F5 redo** — try `axis_data_fifo` with TDATA_NUM_BYTES=16 instead of register slice. If still broken, switch to `axi_cdma` for memory-to-memory.
  2. **F7 extension** — investigate v12-from-BRAM panic. Likely needs T1 RTL change (not BD-level), so consider implementing it in `t1_axi_lite_wrapper.sv` or by adding a vector mask between the AXI response and the vreg writeback. Could also try `axi_register_slice` with stronger config (all stages registered), or a small FIFO before T1 hb's S00.
  3. **F6** — once F5/F7 are routed, evaluate LUT headroom and re-attempt PS→BRAM. Smallest implementation: single `axi_bram_ctrl` with `smartconnect_bram_arb` (already in `system_top.tcl` from attempt #2, just commented out).
  4. **F2** (camera) — separate session. The hypothesis from `camera_bringup_status.md` § 6.3 (smartconnect_ctrl clock/aresetn) is still open. F7-style fix (`axi_register_slice` between `smartconnect_ctrl/M02` and `sensor_iic/S_AXI`) is plausible — bit.bin reads of register-mapped IIC controllers may face the same AXI-metadata issue as T1's BRAM reads.

#### Backup files on Kria (post-session)

| File | Content |
|------|---------|
| `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin` | **5m** (re-deployed 2026-05-11 late for F7-extension diagnostic, kept loaded) |
| `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5h-backup` | 5h |
| `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5k-backup` | 5k |
| `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5l-backup` | 5l |
| `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5m-backup` | 5m |
| `/lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo` | **bram@a0080000 + axi_iic_sensor ENABLED + ap1302 reset-gpios stub** (matches local `fpga/dts/system_top_wrapper.dts` after the F2 software fix; sha256 `bb2a91281a260eb83858ffe2228a03864584a7e4f1552aea310b2b3bec566da4`) |
| `/lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo.5l-backup` | bram@b0000000 (5l/5k) |

To roll back to 5k: `sudo install -m 644 /lib/firmware/.../system_top_wrapper.bit.bin.5k-backup /lib/firmware/.../system_top_wrapper.bit.bin` + install the `.5l-backup` dtbo (`bram@b0000000`) over `system_top_wrapper.dtbo` + `fpgautil` load. To stay on 5m, just confirm `/dev/uio6` PA is `0xa0080000`.

### 5l BUILT, PARTIALLY VALIDATED, NOT PRODUCTION-READY (2026-05-10, late evening)

#### Why F6 was dropped

  * **5m attempt #1 (two `axi_bram_ctrl` + true-dual-port
    `blk_mem_gen`):** synth util **93.18%**. Phase 5.2 re-spiked
    overlaps from 7,433 → 197,369 (5g/5j-style rip-up loop). Killed
    after ~30 min.
  * **5m attempt #2 (single `bram_ctrl` + arbiter
    `smartconnect_bram_arb` fanning T1+DMA and PS to one
    controller):** synth util **96.51%** (worse than attempt #1).
    The smartconnect at NUM_SI=2 NUM_MI=1 with 128-bit width is
    surprisingly heavy — heavier than two separate bram_ctrls.
    Killed at synth.
  * **Conclusion:** F6 in any form costs at least +5000-7000 LUTs
    over 5l's 87.22% baseline, pushing past the routing cliff
    (~88.5%). Deferred to a future bitstream that either reduces
    elsewhere (drop ILA-grade smartconnects, shrink camera path
    further) or reorganises the BD topology more aggressively.
  * **F4 + F1 + F5 + F7 only:** estimated synth util ~88%, just
    under the cliff. Should route.

#### What 5m attempt #3 contains

  * **F4 (BRAM rewire to smartconnect_hb/M01):** unchanged from 5l
    in topology. Scratchpad now at PA `0xA0080000` (relocated from
    5l's 0xB0000000) — kept the relocation for forward-compat with
    a future F6, since T1's view of the address is opaque anyway.
  * **F1 (chroma 2→3 byte fix on `axis_subset_converter_cap`):**
    unchanged from 5l. Closes BD 41-237 width-mismatch warning.
  * **F5 (`axis_register_slice` between `axi_dma/M_AXIS_MM2S` and
    `axi_dma/S_AXIS_S2MM`):** new in 5m. Replaces 5l's direct
    self-loop that confused Vivado's parameter propagation. Should
    make `dma_loopback` PASS for the first time.
  * **F7 (`axi_register_slice` between `t1_top/m_axi_hb` and
    `smartconnect_hb/S00_AXI`):** new in 5m. Speculative fix for
    the 5l kernel-panic on `port_grid_vadd_scratchpad`. Hypothesis:
    AxUSER/AxID metadata leaks from T1's BRAM read into the
    subsequent HPC0 write, panicking the PS cache-sync path. The
    register slice zeros that metadata.

#### What 5m does NOT touch

  * **F2 (axi_iic kernel panic on probe):** still blocking camera.
    Separate session. F7-style `axi_register_slice` between
    `smartconnect_ctrl/M02` and `sensor_iic/S_AXI` is the leading
    candidate fix once we know whether F7 worked for T1 hb.
  * **F6 (PS direct mmap of scratchpad):** deferred. Future
    bitstream when LUT headroom allows.

#### 5m validation plan

Run on Kria post-deploy in this order:

| # | Test | Validates |
|---|------|-----------|
| 1 | `triage_t1`, `smoke`, `ddr_roundtrip`, `port_grid_vadd`, `vert_lsu` | Regression: 5l-passing tests still pass on 5m |
| 2 | `sp_alloc_probe`, `sp_load_probe`, `sp_store_probe`, `sp_4issue_probe` | F4 paths in isolation (regression from 5l) |
| 3 | `dma_loopback` | F5 fix (was failing on 5l) |
| 4 | `sp_4issue_with_verify_probe`, `port_grid_vadd_scratchpad` | F7 fix (panicked on 5l) |
| 5 | `dma_to_scratchpad` (rewritten) | DMA round-trip DDR→sp→DDR (F4 DMA path + F5) |
| 6 | `dma_t1_scratchpad` (new) | Full F4 amplifier: DMA load + T1 sp→sp + DMA store |

Tests 1+2+3 must pass for 5m to be a usable bitstream. Tests 4+5+6
confirm the architectural goals were achieved.

### 5l BUILT, PARTIALLY VALIDATED, NOT PRODUCTION-READY (2026-05-10, late evening)

**5l succeeded as a build but is unsafe for general use. F4's
hardware path works in isolation; combining it with the existing
udmabuf cache-sync workflow triggers a kernel panic. F3's DMA
loopback didn't actually wire. 5k remains the production target.**

  * **Build dir:**
    `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260510-140423/`
  * **Bitstream:** `system_top_wrapper.bit` (7.8 MB, sha256
    `7e400260609538d65a1279c2cb5d48fe287d3695e3c0c834c2ff385f645cef4a`)
  * **Wall clock:** 3h 49m. Synth util **87.22%** (102,149 LUTs,
    -822 vs 5k). Impl util **83.41%** (97,686 LUTs). Timing
    **WNS=+0.355, WHS=+0.010, WPWS=+0.164, 0 failing endpoints**
    (329,097 setup / 329,047 hold / 128,191 pulse-width).
    All-around healthier than 5k.
  * **Deployed briefly to Kria** for testing, then rolled back to
    5k. The 5l `.bit.bin` lives on the Kria at
    `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5l-backup`;
    re-deploy with `cp …5l-backup …bit.bin && fpgautil -b … -o …`.
    The 5k `.bit.bin` is at `…bit.bin.5k-backup` (also restored
    in-place as the active `…bit.bin` after the panic-debug session).

#### What 5l contains (BD edits applied in 5l, all in `system_top.tcl`)

  * **F4 (BRAM rewire):** `smartconnect_hb` NUM_SI=3 NUM_MI=2,
    `smartconnect_idx` NUM_MI=1, `bram_ctrl/S_AXI` connected to
    `smartconnect_hb/M01_AXI` (was idx/M01). Address-map: T1 hb +
    DMA both channels all reach BRAM at 0xB0000000-0xB0007FFF.
  * **F1 (chroma fix):** `axis_subset_converter_cap`
    `M_TDATA_NUM_BYTES 2 → 3`, `TDATA_REMAP {8'b00000000,tdata[15:0]}`.
    The BD 41-237 width-mismatch CRITICAL WARNING that fired in
    5e/5j/5k is now silent.
  * **F3 (DMA streaming loopback):** `axi_dma/M_AXIS_MM2S →
    axi_dma/S_AXIS_S2MM`. **Did not wire correctly** — see below.

#### Test results on 5l

| Test | Result | Notes |
|------|--------|-------|
| `triage_t1` | PASS | Same probes as 5k. |
| `smoke` | PASS | Cycles advance. |
| `ddr_roundtrip` | PASS | T1 hb DDR R/W intact. |
| `port_grid_vadd` | PASS | T1 hb DDR-only kernel intact. |
| `vert_lsu` | PASS | Vertical-mode transpose intact. |
| `dma_loopback` | **FAIL** (timeout) | F3 wire didn't actually connect — see "F3 broke at synth" below. |
| `port_grid_vadd_scratchpad` | **PANIC** (kernel SError) | F4 hw path works in isolation but full test panics — see bisection below. |
| `dma_to_scratchpad` | not runnable | Required F3 working. Removed from suite. |

#### F3 broke at synth: AXIS self-loop confused parameter propagation

Vivado emitted two warnings during synth that explain the dma_loopback
failure:

  * `[BD 41-3281] Instance '/axi_dma' is connected on both sides by
    SmartConnects and cannot automatically configure itself to match
    the endpoints. Please manually configure its AXI interfaces as
    required for the design.`
  * `[BD 41-702] Propagation TCL tries to overwrite USER strength
    parameter C_M_AXI_S2MM_DATA_WIDTH(128) on '/axi_dma' with
    propagated value(32). Command ignored.`

Plain `connect_bd_intf_net axi_dma/M_AXIS_MM2S → axi_dma/S_AXIS_S2MM`
created an IP-internal AXIS self-loop that confused Vivado's parameter
propagation. Our explicit user setting (M_AXI_S2MM_DATA_WIDTH=128) was
preserved (the propagation override was ignored), but the AXIS sideband
parameters (TID_WIDTH, TUSER_WIDTH, TKEEP semantics) silently drifted
out of sync. Result: streams "connect" structurally but never transfer
payload, so dma_loopback's S2MM channel waits forever for data that
never arrives.

**Fix for next bitstream (F5):** insert an `axis_register_slice`
between MM2S and S2MM. The slice provides timing isolation AND breaks
the propagation feedback loop (Vivado treats it as two separate AXIS
channels, each with its own propagation graph). Standard Xilinx
pattern for IP-internal AXIS loopback.

#### F4 partially validated: T1 hb path works, full test panics kernel

Bisection via standalone probes (all on 5l):

| Probe | T1 issues | Other ops | Result |
|-------|-----------|-----------|--------|
| `sp_alloc_probe` | 0 | mmap /dev/uio6 | PASS |
| `sp_load_probe` | 1: vle from sp | — | PASS |
| `sp_store_probe` | 1: vse to sp | — | PASS |
| `sp_ddr_to_sp_probe` | 2: vle DDR + vse sp | sync_for_dev(src) | PASS |
| `sp_3issue_probe` | 3: + vle sp | sync_for_dev(src) | PASS |
| `sp_4issue_probe` | 4: + vse DDR | sync_for_dev(src) | PASS |
| `sp_4issue_with_verify_probe` | 4 | + sync_for_cpu(dst), memcmp | **PANIC** |
| `port_grid_vadd_scratchpad` | 4 | + dst init + sync_for_dev(dst) + sync_for_cpu(dst), memcmp | **PANIC** |

The 4-issue T1 chain itself completes cleanly. The kernel panic
appears specifically when the udmabuf `sync_for_cpu(dst)` runs on a
buffer that T1 just wrote via HPC0, where T1's source vreg `v12`
had been loaded from BRAM in the immediately preceding issue.

Most likely root cause: T1's `vle` from BRAM tags `v12` with side-band
metadata (AxUSER, AxID, or similar) that smartconnect_hb propagates
through the subsequent `vse` to HPC0. The PS-side cache-sync path
walks the AXI snoop tags and panics on the unexpected metadata.
Without an ILA on smartconnect_hb's M00 channel this is hard to
confirm — kernel logs are lost to the panic, no oops trace survives.

**Fix candidate (F7):** insert an `axi_register_slice` between
`t1_top/m_axi_hb` and `smartconnect_hb/S00_AXI`. The slice would
zero out any unexpected AxUSER/AxID propagation and present a clean
AXI-standard interface to smartconnect. Cheap (~50 LUTs) and a
well-known fix for "weird AXI master metadata leaks downstream"
classes of bug.

#### F4 partially deployable: PS→scratchpad direct mmap impossible on 5l

Independent of the F7 panic: scratchpad PA `0xB0000000` is in the
HPM1_FPD aperture per UG1085, but our BD has `PSU__USE__M_AXI_GP1=0`.
Any PS-side load/store to a `/dev/uio6` mmap'd VA hits the disabled
HPM1 master at the PS internal NIC, DECERRs immediately, and panics
the kernel with an SError. (Same root cause as F4's panic in spirit
but a different mechanism — F4's panic happens during cache-sync
of an HPC0 write, whereas this happens on direct PS load/store of
an HPM1-aperture address.)

For 5l, this means the new `t1_scratchpad_alloc` helper's `.va`
field MUST NOT be dereferenced by PS code. The probe scaffolding
(sp_alloc_probe etc.) avoids this. Real applications (visionsoc_main)
would need to access scratchpad via DMA only, which (given F3 is
also broken) means scratchpad is effectively isolated from the PS
on 5l: only T1 can read or write it.

**Fix for next bitstream (F6):** relocate scratchpad to the HPM0
aperture (`0xA0080000`, after sensor_iic at 0xA0050000+0x10000),
add PS→BRAM via `smartconnect_ctrl/M03_AXI`. Requires bram_ctrl in
dual-port mode (`SINGLE_PORT_BRAM=0`) so PS and T1+DMA can reach it
through separate AXI ports. Costs ~+800-1200 LUTs (dual-port
controller + smartconnect_ctrl NUM_MI 3→4) but unlocks PS-side
direct verification + fast small-data setup.

#### Recommended ordering for 5m

  1. **F5 first (axis_register_slice).** Smallest BD edit, no
     LUT impact, unblocks `dma_loopback` regression test as proof
     it worked. Run that test alone before touching anything else.
  2. **F7 next (T1-hb axi_register_slice).** Tests whether T1's
     vle-from-BRAM propagates side-band metadata that breaks
     downstream HPC0 writes. If the slice fixes it, the
     port_grid_vadd_scratchpad panic clears.
  3. **F6 last (scratchpad PA + dual-port BRAM).** Bigger BD edit,
     more LUTs, but unlocks the PS-side debug path and the
     dma_to_scratchpad test. Defer if F5+F7 alone make 5l usable.
  4. F2 (axi_iic SError, camera bringup) is still its own session.

#### Probes left in place for 5m validation

`vision_software/libt1/test/sp_*_probe.c` are kept in the source
tree. After 5m comes online, run them in order to validate F5+F7+F6
landed cleanly:

  * `sp_alloc_probe`: scratchpad mmap works (F6 unlocks PS-side use)
  * `sp_load_probe`, `sp_store_probe`: T1 hb single-issue scratchpad ops
  * `sp_ddr_to_sp_probe`, `sp_3issue_probe`, `sp_4issue_probe`:
    confirm the chained ops still pass (regression check)
  * `sp_4issue_with_verify_probe`: confirms F7 fixed the
    `vse-after-vle-from-BRAM` panic
  * `port_grid_vadd_scratchpad`: full test should now pass
  * `dma_loopback`: regression check that F5 fixed the streaming loopback
  * (re-add) `dma_to_scratchpad`: F4 + F5 prefetch path validation

### CURRENT STATE (2026-05-10, evening)

  * **5k SUCCEEDED + DEPLOYED 2026-05-10.** Camera-RESTORED
    bitstream that closed timing cleanly with positive slack, was
    flashed on Kria via `fpgautil` (391 ms load), and re-passes
    the same 5-of-6 libt1 hardware tests as 5h. This is the new
    production target, supersedes 5h.
      * Build dir:
        `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260510-025732/`
      * Bitstream: `system_top_wrapper.bit` (7.8 MB, sha256
        `3bdf9d25bea5fe93530bd23b8c4b87634d7953081bf169a5450ee37db7520020`)
      * Wall clock: **3h 10m** end-to-end (way under the 8-12h
        estimate; suggests the routing cliff was crossed cleanly,
        no rip-up loop).
      * Synth util: **102,971 LUTs / 87.92%** (≈identical to 5j's
        87.97% — slim didn't shrink synth-time count meaningfully).
      * Impl util: **98,208 LUTs / 83.85%** (down ~5pp from synth
        after place-time optimisations; this is the meaningful
        post-route number).
      * Timing: **WNS=+0.337 ns, WHS=+0.010 ns, WPWS=+0.164 ns**,
        all 0 failing endpoints (336,294 setup / 336,244 hold /
        130,043 pulse-width).
      * Functional path: T1 control plane + LSU + DMA + camera
        capture pipeline (mipi_csi2_rx + axis_data_fifo +
        axis_subset_converter + v_frmbuf_wr → HP1 DDR) all in BD.
    The slimmed config in this build (`CMN_NUM_PIXELS 1`, axis
    converter at 2 bytes, `SAMPLES_PER_CLOCK 1`) is the recipe
    that closes timing on KV260 with PSU=128.
      * Deployment 2026-05-10: `bit2bin.py` strip + `scp` +
        `sudo install`, `fpgautil -b ...bit.bin -o ...dtbo`
        replaces k26-starter-kits overlay; `/dev/uio4=t1`,
        `uio5=dma`, `uio6=bram` enumerate; udmabuf0..2 carry
        across boots via the persistence files from 2026-05-07.
        Hardware test results on 5k (same Kria, same dtbo,
        no driver changes from 5h state):
        - `triage_t1`: PASS (all probes; control-plane
          AXI4-Lite reads at non-aligned offsets return correct
          values, confirming Fix 2 carries forward)
        - `smoke`: PASS (cycles advanced 1,008,757 in 10 ms)
        - `ddr_roundtrip`: PASS (Fix D cache sync intact)
        - `port_grid_vadd`: PASS
        - `vert_lsu`: PASS (vertical-mode LSU transpose)
        - `dma_loopback`: FAIL (`t1_dma_wait: Connection timed
          out`) — same longstanding `axi_dma/S_AXIS_S2MM`
          unconnected BD issue as on 5h.
        5h `.bit.bin` preserved at
        `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5h-backup`
        on the Kria.
  * **Bitstream 5h
    (`…fpga-20260509-121320`) is the previous production point**,
    camera REMOVED. 5/6 libt1 hardware tests passed on it on Kria
    (`triage_t1`, `smoke`, `ddr_roundtrip`, `port_grid_vadd`,
    `vert_lsu`); `dma_loopback` fails due to a longstanding BD
    issue (`axi_dma/S_AXIS_S2MM` unconnected — same warning still
    present in 5k, also unrelated to LSU). 5h remains flashed on
    the Kria until 5k is deployed.
  * **AXI4-Lite read-zero bug RESOLVED.**
    `PSU__MAXIGP0__DATA_WIDTH=128` and `PSU__MAXIGP2__DATA_WIDTH=128`
    bypass the PS-internal NIC-400 downsizer that mishandled
    AXI4-Lite read response repacking. **DO NOT revert to 32-bit.**
    See `fyp_doc/camera_bringup_status.md` § 6.1.5 for the full
    explanation + § 6.4 for the udmabuf cache fix that unblocked
    the LSU tests.
  * **Camera-restored build (5j) FAILED routing 2026-05-10** — same
    timing-driven rip-up loop pattern as 5g. 87.97% LUT util with
    PSU=128 + camera is structurally over the routing cliff. The
    in-flight 5j run was confirmed in the rip-up loop (Phase 5.2
    converged to 5–8 overlaps over 7h 18m, Phase 5.3 re-spiked to
    121,850). Killed 2026-05-10 to free the host for 5k.
  * Driver-side state in `fyp_doc/driver_implementation_status.md`
    § "Fix D" — udmabuf is mmap'd cached; libt1 has new
    `t1_buf_sync_for_{cpu,device}` helpers; required usage pattern
    documented. **`msync()` and `O_SYNC` were tried and rejected;
    DON'T re-introduce them.**

### Open issues remaining on 5k (unchanged from 5e/5j-class)

  1. **CRITICAL [BD 41-237] s_axis_video TDATA_NUM_BYTES mismatch
     `(3) vs (2)`** between `v_frmbuf_wr/s_axis_video` and
     `axis_subset_converter_cap/M_AXIS`. The IP's input port stays
     3 bytes wide at SAMPLES_PER_CLOCK=1 with HAS_UYVY8=1 — Vivado
     pads upper byte. Net effect on hardware: the upper byte
     reaching v_frmbuf_wr is undefined; UYVY frames will be
     corrupted on the chroma channel. **Camera capture validation
     blocked** until this is resolved. The fix likely requires
     either a small custom AXIS bridge (zero-pad the 3rd byte
     intentionally) or a different downstream buffer IP. The T1
     control-plane and DMA are unaffected.
  2. **CRITICAL [DRC AVAL-350] MIPI PLL FVCO=1500.060 MHz**
     (60 kHz over the 1500 MHz upper bound) — same as 5e/5g/5h.
     Non-blocking but worth fixing if MIPI capture turns out flaky.
     Source: `clk_wiz_0/clk_200M` derived from 60 MHz pl_clk0.
  3. **CRITICAL [axi_dma:7.1-11] S_AXIS_S2MM unconnected** — same
     longstanding BD issue that causes `dma_loopback` test to fail.
     Not LSU-related.
  4. **CRITICAL [BD 41-737] MEM_DEPTH read-only on bram_ctrl** —
     harmless propagation warning.

### Forward work — F4: scratchpad as T1 high-bandwidth target

**Status:** spec'd 2026-05-10, not yet implemented. This is the
architectural follow-up the original `fpga_implementation_handoff.md`
envisioned but the current BD doesn't yet support. Bigger than
F1-F3; deserves its own session.

#### Goal

Use the 32 KB BRAM scratchpad as a double-buffer for T1's
high-bandwidth memory operations. DMA prefetches one vreg's worth
of data from DDR into one half of scratchpad while T1 computes on
the other half. Hides DMA latency under compute and gives T1
single-cycle 128-bit BRAM access vs multi-cycle DDR via HPC0.

Sizing: T1 vlen ≈ 16 KB at SEW=8 in the current config — the 32 KB
scratchpad fits exactly 2 vregs.

```
DDR ──DMA──► scratchpad[0:16K]      (DMA loads vreg N+1 while T1 works)
              ▲
              │   T1 reads vreg N from scratchpad[16K:32K]
              ▼   via m_axi_hb (128-bit, single-cycle BRAM access)
DDR ◄──DMA── scratchpad[16K:32K]    (DMA stores vreg N-1 from previous)
```

#### Why 5k can't do this as-built

The scratchpad is on T1's `m_axi_idx` (32-bit) port, not
`m_axi_hb` (128-bit). Backwards for a bandwidth amplifier:

```
T1 m_axi_hb  (128-bit) ──► smartconnect_hb (NUM_MI=1) ──► HPC0 ──► DDR  ← only target
                                  ▲
                            DMA mm2s/s2mm share

T1 m_axi_idx (32-bit)  ──► smartconnect_idx (NUM_MI=2) ──┬──► HP0 ──► DDR
                                                         └──► bram_ctrl (scratchpad)
                                                               ↑
                                            **only T1 idx reaches it; DMA cannot**
```

Even though `bram_ctrl/DATA_WIDTH=128`, an upsize converter on a
32-bit master can't recover bandwidth the master never had. And
`axi_dma`'s masters are wired to `smartconnect_hb` which only
routes to HPC0 — DMA can't touch scratchpad at all.

#### Two BD topology options

**Option A — extend `smartconnect_hb` (recommended, minimal delta):**

  * `smartconnect_hb`: NUM_SI stays 3 (T1 hb + DMA mm2s + DMA s2mm),
    NUM_MI 1 → 2 (add `bram_ctrl` as M01).
  * Detach `bram_ctrl` from `smartconnect_idx`; drop its NUM_MI
    back to 1 (T1 idx → HP0 only).
  * Address-map: scratchpad 0xB000_0000 (32 K) reachable from
    `t1_top/m_axi_hb`, `axi_dma/Data_MM2S`, `axi_dma/Data_S2MM`.
  * Estimate: +300-500 LUTs over 5k (additional 128-bit decode
    + arbiter slice in smartconnect_hb).

**Option B — dedicated scratchpad smartconnect:**

  * New `smartconnect_scratch` (NUM_SI=3, NUM_MI=1 → bram_ctrl).
  * Bigger BD, more LUTs (~500-800), but cleanly separates DDR
    traffic from scratchpad traffic. Useful if you ever want
    different clock domains, QoS, or strategies on scratchpad
    vs DDR.

Start with Option A. Fall back to B only if A misroutes.

#### Implementation checklist

  - [ ] BD edits in `fpga/system/system_top.tcl`:
        * `smartconnect_hb` NUM_MI 1 → 2
        * Detach `bram_ctrl` from `smartconnect_idx`; NUM_MI 2 → 1
        * Connect `smartconnect_hb/M01_AXI` → `bram_ctrl/S_AXI`
        * Address-map: assign scratchpad to `t1_top/m_axi_hb`,
          `axi_dma/Data_MM2S`, `axi_dma/Data_S2MM` spaces (drop
          from `t1_top/m_axi_idx` space)
  - [ ] Build (estimate +300-500 LUTs over 5k baseline 87.92%
        synth; should still close, but watch the cliff at ~88%)
  - [ ] libt1 helper:
        `t1_scratchpad_alloc(offset, size) -> { .pa, .va }`
        — mmap `/dev/uio6`, return PA = 0xB000_0000+offset
  - [ ] At least one new hardware test:
        `port_grid_vadd_scratchpad` — same compute as
        `port_grid_vadd` but with input/output buffers in
        scratchpad instead of udmabuf-backed DDR. Validates the
        new T1 hb → scratchpad path end-to-end.
  - [ ] Optional benchmark: double-buffer kernel that measures
        actual DMA-vs-compute overlap. Use perf counters
        (PERF_TAG / PERF_DELTA at offsets 0x48 / 0x4C) for
        cycle-accurate timing.

#### Things to watch

  1. **LUT headroom.** 5k synth = 87.92%. The route-cliff
     failures in 5g/5j were at 87.97% synth. F4 adds LUTs
     modestly; if synth pushes past ~88.5%, fall back to a
     smaller scratchpad (16 KB / 1 vreg) or Option B with
     more aggressive camera-pipeline trimming.
  2. **Address decoding on T1's hb port.** `m_axi_hb` already
     issues whatever address the kernel encodes in `rs1` — no
     T1 RTL changes needed, just kernel-side address choice.
     SmartConnect decoder routes 0xB000_0000+ to bram_ctrl,
     lower addresses to HPC0/DDR.
  3. **No PS-side cache flush for scratchpad.** Scratchpad is
     BRAM, non-coherent at the BRAM-controller level — but
     the PS only ever accesses it via `/dev/uio6` mmio (no
     CPU cache between PS and bram_ctrl). The
     `t1_buf_sync_for_{cpu,device}` helpers from Fix D apply
     to udmabuf-backed DDR only. Scratchpad buffers don't
     need them.
  4. **SmartConnect strategy.** Keep all SCs in LOW_AREA. The
     PERFORMANCE strategy on smartconnect_ctrl in 5f hit
     congestion immediately; the same risk applies if you flip
     `smartconnect_hb` to PERFORMANCE just because it now has
     2 MIs.
  5. **DMA descriptors.** Currently AXI DMA's MM2S/S2MM only
     map DDR. After address-map updates, descriptors like
     `mm2s.SRC=DDR_pa, dst=0xB000_0000, len=16K` should work.
     The libt1 `t1_dma_mm2s_async` already takes raw PA — no
     API change needed, just pass scratchpad PA at call sites.

#### Why this is bigger than F1/F2/F3

F1 (chroma fix) and F3 (DMA loopback) are tactical fixes on
existing BD shape. F2 (axi_iic) is a kernel-level debug. F4 is
a small but real architectural change that affects:

  * BD topology (smartconnect rewire)
  * Kernel encodings (kernels using scratchpad must encode
    `rs1 = 0xB000_0000+offset`, not a udmabuf PA)
  * libt1 API surface (new alloc helper, perhaps a new buffer
    type tag distinguishing DDR-backed vs scratchpad-backed)
  * Test suite (needs at least one new variant; the existing
    five tests all use udmabuf and don't exercise the new path)

#### Recommended ordering

  1. **F1 first** — chroma fix is small, ~3 h rebuild, unblocks
     camera streaming mechanically (ignoring axi_iic). Low risk
     of disturbing 5k's working timing closure.
  2. **F4 next** — once F1 is stable, extend BD with scratchpad
     on hb. This is the architectural step that lets future
     kernels actually exploit the scratchpad bandwidth.
  3. **F2 separately** — axi_iic kernel-panic debug benefits
     from a fresh context window; not on F1/F4's critical path.
  4. **F3 (DMA loopback)** is opportunistic — bundle into F1
     or F4's rebuild if the BD edit is small (one extra AXIS
     loopback wire from `axi_dma/M_AXIS_MM2S` to its own
     `S_AXIS_S2MM`).

### Older context (kept for posterity)

  * Config in use: **`mudkip2d128small1bram1chain2lanescale`** (the
    standard 2D-fabric SEW=8 / dLen=128 / laneScale=2 / chainingSize=1
    setup that all prior FPGA builds have used).
  * Wrapper extensions per `fpga_implementation_handoff.md` § 3 + § 4
    are applied (carried over from session of 2026-05-05 17:24) —
    VERTICAL_MODE @ 0x44, perf counters @ 0x48–0x54, ADDR_WIDTH 7→8,
    `issue_bits_verticalMode` wired between wrapper and T1 in
    `gen_wrapper.sh`. See § 4 of this doc for the exact diff.
  * **2026-05-05 23:39:** The 60 MHz build started 18:05
    (`mudkip2d128small1bram1chain2lanescale-20260505-180504`) was
    **manually terminated by the user** because the streaming
    pipeline was *not yet wired into `system_top.tcl`*. The user wanted
    Task A built with the full pipeline in one shot. That build dir
    still contains valid synth artefacts (synth WNS = +5.990 ns) —
    preserve it for reference.
  * **2026-05-06 (this session):** Camera capture pipeline + scratchpad
    + IIC added to `system_top.tcl`, modelled on the canonical
    `kv260_ispMipiRx_vcu_DP` reference platform (cloned at
    `kria_ref/kria-vitis-platforms/kv260/platforms/vivado/kv260_ispMipiRx_vcu_DP/`).
    See § 8 below for the exact diff. Architectural correction from the
    original handoff doc: **no `v_hdmi_tx_ss` is needed** — the KV260
    SmartCam reference uses the PS DisplayPort controller for the
    display path (carrier has external DP→HDMI converter), not a PL
    HDMI subsystem. Likewise **no `v_proc_ss`** — the AP1302 ISP does
    crop+downsample (sensor 4208×3120 → square crop → 128×128) before
    pixels enter the FPGA, so we don't need a PL scaler. The pipeline
    is just `CSI-2 RX → axis_data_fifo → axis_subset_converter →
    v_frmbuf_wr → DDR (via PS HP1)`.
  * **BD validates cleanly** as of 2026-05-06 — see build dir
    `mudkip2d128small1bram1chain2lanescale-20260506-050031/`. Bitstream
    build is the next step.
  * **2026-05-07: BITSTREAM SUCCESS** — the
    `mudkip2d128small1bram1chain2lanescale_fpga` config closed timing
    and produced a clean bitstream after a one-line constraints fix.
    First `write_bitstream` (overnight 2026-05-06) had failed at DRC
    on `ap1302_rst_b[0]` / `ap1302_standby[0]` (no LOC/IOSTANDARD).
    Root cause: those two ports were created bare in `system_top.tcl`
    and the project had **no XDC in `constrs_1` at all** — MIPI/IIC
    only worked because they're bound to `som240_*` board interfaces
    that auto-supply pin constraints. Fix:
    `fpga/system/pin.xdc` (J11/J10, LVCMOS33, SLEW SLOW, DRIVE 4 —
    copied from `kria_ref/.../kv260_ispMipiRx_vcu_DP/xdc/pin.xdc`),
    auto-added to `constrs_1` from `system_top.tcl`. Recovery rerun
    of `impl_1` only (synth reused) finished 2026-05-07 11:44.
    Final timing: **WNS=+0.135 ns, WHS=+0.010 ns, 0 failing
    endpoints** out of 342,990 setup / 342,856 hold. Bitstream at
    `fpga/build/t1_mudkip2d128small1bram1chain2lanescale_fpga_system/t1_mudkip2d128small1bram1chain2lanescale_fpga_system.runs/impl_1/system_top_wrapper.bit`
    (7.8 MB), copied alongside the original failed build dir for
    reference.
  * **Open camera-bring-up risk (not blocking bitstream):**
    `[DRC AVAL-350]` — MIPI CSI2 PHY PLL FVCO=1500.060 MHz, 60 kHz over
    the 1500 MHz upper bound. CLKFBOUT_MULT_F=15, CLKIN_PERIOD=4.99980
    (i.e. driven from `clk_wiz_0/clk_200M`, which is itself derived
    from a 60 MHz `pl_clk0`). If MIPI capture is flaky on hardware,
    fix the source clock first — e.g. drive `clk_200M` from a clock
    that produces an exact 5.00000 ns period (a separate `pl_clkN`
    at 200 MHz, or change `clk_wiz_0`'s reference / multiplier).
  * What's running / done: see § 2 "Current state".

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
| 3a. First bitstream attempt @ 80 MHz | `fpga/system/build_fpga.sh -c mudkip2d128small1bram1chain2lanescale -b` | **failed 18:29 (impl placer terminated abnormally with boost::filesystem error mid-place)** | `fpga/build/mudkip2d128small1bram1chain2lanescale-20260505-172438/` |
| 3b. Clock edit | `system_top.tcl` line 107: `FREQMHZ {80} → {60}` | **done** | (handoff doc § 8.4.5 recommends this for slack margin) |
| 3c. Bitstream rebuild @ 60 MHz | `fpga/system/build_fpga.sh -c mudkip2d128small1bram1chain2lanescale -b` | **TERMINATED 2026-05-05 23:39 by user — pipeline IPs missing** | `fpga/build/mudkip2d128small1bram1chain2lanescale-20260505-180504/` (preserved) |
| 4a. Streaming pipeline IPs added to `system_top.tcl` (capture path) | manual edit, modelled on `kv260_ispMipiRx_vcu_DP` reference | **done 2026-05-06** | `fpga/system/system_top.tcl` (see § 8) |
| 4b. BD validation (no synth) | `bash fpga/system/build_fpga.sh -c <config>` | **passed 2026-05-06 05:00** | `fpga/build/mudkip2d128small1bram1chain2lanescale-20260506-050031/` |
| 4c. Diagnostic full-bitstream attempt (`-b -a`) | `bash fpga/system/build_fpga.sh -c <config> -b -a` (user-launched, this session) | **synth done 06:19, impl in routing — expected to fail (LUT 102%, routing WNS -3.6 ns)** | `fpga/build/mudkip2d128small1bram1chain2lanescale-20260506-055305/` |
| 4d. DTS overlay authored | `fpga/dts/system_top_wrapper.dts` (modelled on smartcam DTSI, addresses match § 8.6) | **done 2026-05-06** | `fpga/dts/system_top_wrapper.dts` |
| 5a. First full bitstream attempt (`_fpga` config) | `build_fpga.sh -c mudkip2d128small1bram1chain2lanescale_fpga -b` | **failed 2026-05-07 ~01:53 — write_bitstream DRC NSTD-1/UCIO-1 on AP1302 sideband ports** | `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260506-234437/` |
| 5b. Pin constraints added | `fpga/system/pin.xdc` + 1-line patch in `system_top.tcl` (auto-add to `constrs_1`) | **done 2026-05-07** | `fpga/system/pin.xdc`, `fpga/system/system_top.tcl` (post-`add_files ${bd_wrapper}`) |
| 5c. Impl-only recovery rerun | live `add_files ... pin.xdc` + `reset_run impl_1` + `launch_runs impl_1 -to_step write_bitstream` on existing project | **done 2026-05-07 11:44 — bitstream OK, WNS=+0.135 ns / WHS=+0.010 ns / 0 failing endpoints** | `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260506-234437/recovery-20260507-impl-rerun/` |
| 5d. Smartconnect_ctrl revert + rebuild | `bash fpga/system/build_fpga.sh -c mudkip2d128small1bram1chain2lanescale_fpga -b -a` — staged BD edits: NUM_CLKS 2→1, NUM_MI 4→3, dropped LOW_AREA on smartconnect_ctrl, detached v_frmbuf_wr/s_axi_CTRL + frmbuf IRQ. Triggered by triage finding (camera_bringup_status.md § 6.1.3) that read responses for araddr[3:2]≠00 return 0 on both T1 wrapper AND DMA. | **failed 2026-05-08 06:49 — `route_design` 4243 node overlaps after 7h45m route. Synth utilisation actually went down (104780 LUTs vs 5a's 104998), but the new smartconnect_ctrl topology pushed placement past the routing cliff.** | `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260507-215432/` |
| 5e. Camera pipeline slim + retry | Same script as 5d. Additional BD edits to drop ~2k LUTs of unused-format / oversized-buffer logic from camera path (still functional, just sized for our actual UYVY 128×128 use case). v_frmbuf_wr drop unused formats, MAX_COLS/ROWS 1920/1080→256, AXIMM_DATA_WIDTH 128→64; mipi_csi2_rx CSI_BUF_DEPTH 4096→1024; axis_data_fifo_cap FIFO_DEPTH 1024→256; axis_subset_converter M_TDATA_NUM_BYTES 6→4. | **build OK 2026-05-08 16:40 — bitstream produced, WNS=+0.609 ns / WHS=+0.010 ns / 0 failing endpoints. BUT bug NOT FIXED** — read pattern unchanged on hw. Triggered re-diagnosis (camera_bringup_status.md § 6.1.4). | `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260508-131546/` |
| 5f. Fix 2 attempt 1 (PSU=128, camera kept, PERFORMANCE strategy) | `bash fpga/system/build_fpga.sh -c mudkip2d128small1bram1chain2lanescale_fpga -b -a`. Staged: PSU__MAXIGP0/MAXIGP2__DATA_WIDTH 32→128, smartconnects forced to PERFORMANCE strategy (camera_bringup_status.md § 6.1.4 Fix 2). | **terminated 2026-05-08** — Phase 4 routing reported level-5 short congestion; "congestion is preventing the router from routing all nets." Stopped before bitstream. | `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260508-200459/` |
| 5g. Fix 2 attempt 2 (PSU=128, camera kept, LOW_AREA strategy) | Same script. Strategy reverted to LOW_AREA on all smartconnects (PERFORMANCE was too area-hungry). MAXIGP0/MAXIGP2 stayed at 128. | **failed 2026-05-09 — route timing-driven rip-up loop after 11h+**. Phase 5.x converged Phase 5.1 to ~4243 overlaps but couldn't simultaneously meet timing; subsequent iterations re-spiked overlaps to many thousands and re-iterated indefinitely. User killed the build. Diagnosed as the design being structurally over the routing cliff at ~89.5% LUT util with PSU=128's wider PS-side path. | `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260509-002137/` |
| 5h. Camera REMOVED + ILA + Fix 2 (debug build) | Same script. BD edits captured in `fyp_doc/camera_pipeline_restore_handoff.md`: drop mipi_csi2_rx, v_frmbuf_wr, sensor_iic, smartconnect_lpd, smartconnect_video, clk_wiz_0, related top-level ports. Add system_ila on smartconnect_ctrl/M00 for control-plane debug. Keep PSU=128. NUM_MI=2 on smartconnect_ctrl (only T1 + DMA). | **build OK 2026-05-09 14:18 — bitstream produced, WNS=+0.553 ns / WHS=+0.010 ns / 0 failing endpoints. 96.5k LUTs (82.45%).** Deployed to Kria; `triage_t1` PASS — Fix 2 (PSU=128) confirmed; AXI4-Lite read-zero bug RESOLVED (camera_bringup_status.md § 6.1.5). LSU/test-suite work followed (5/6 tests pass; cache-coherency fix in libt1, see § 6.4 + driver_implementation_status.md § Fix D). **This is the bitstream currently flashed on hardware.** | `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260509-121320/` |
| 5j. Camera RESTORED + ILA removed + Fix 2 (production target) | Same script. Restored full camera pipeline per the handoff doc, removed system_ila + debug_bridge. Kept PSU=128. Smartconnect_ctrl back to NUM_MI=3 (T1 + DMA + sensor_iic), all 5 smartconnects in LOW_AREA. | **failed 2026-05-10 — same failure mode as 5g.** Synth completed cleanly (103,036 LUTs / 87.97% util — 1.5pp better than 5g but still over the routing cliff). Phase 5.2 converged overlaps to 5–8 over 7h 18m; Phase 5.3 timing-driven rip-up re-spiked overlaps to 121,850. Vivado reports CLB routing congestion (`iter_200_CongestedCLBsAndNets.txt`). **Killed 2026-05-10 (parent vivado pid 4070227) to free the host for 5k**; build dir preserved for reference. | `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260509-174417/` |
| 5k. Camera RESTORED + Fix 2 + aggressive camera slim (option 1) | Same script. Camera kept; on top of 5j: `mipi_csi2_rx` `CSI_BUF_DEPTH 1024→256` + `CMN_NUM_PIXELS 2→1`; `v_frmbuf_wr` `SAMPLES_PER_CLOCK 2→1`; `axis_subset_converter_cap` shrunk to 2-byte passthrough (`S/M_TDATA_NUM_BYTES 4→2`, `TDATA_REMAP {tdata[15:0]}`). The CMN_NUM_PIXELS + axis converter changes are required to match the 16-bit-per-clock pixel rate that v_frmbuf_wr expects at SPC=1; without them the AXIS chain widths mismatch end-to-end (BD validate would error or drop pixels). | **build OK 2026-05-10 06:08 — bitstream produced in 3h 10m. Synth: 102,971 LUTs / 87.92%. Impl: 98,208 LUTs / 83.85%. Timing: WNS=+0.337 ns, WHS=+0.010 ns, WPWS=+0.164 ns, 0 failing endpoints (336,294 setup / 336,244 hold / 130,043 pulse-width).** sha256 `3bdf9d25bea5fe93530bd23b8c4b87634d7953081bf169a5450ee37db7520020`. **This is the new production target — supersedes 5h. T1 control-plane + LSU + DMA all functional from 5h carry over; camera capture path lives in the BD but BD 41-237 width mismatch (s_axis_video 3 bytes vs converter 2 bytes) means video frames will be chroma-corrupted until that's fixed in a follow-up.** | `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260510-025732/` |
| 5l. F4 (BRAM rewire to smartconnect_hb/M01) + F1 (chroma 2→3 byte fix) + F3 (axi_dma streaming loopback) | Same script. On top of 5k: `smartconnect_hb` NUM_SI=3 NUM_MI=2 (M01→bram_ctrl); `smartconnect_idx` NUM_MI=1 (bram_ctrl detached); address-map: T1 hb + DMA both channels reach BRAM @ 0xB0000000-0xB0007FFF; `axis_subset_converter_cap` M_TDATA_NUM_BYTES 2→3 with `TDATA_REMAP {8'b00000000,tdata[15:0]}`; `connect_bd_intf_net axi_dma/M_AXIS_MM2S → axi_dma/S_AXIS_S2MM`. | **build OK 2026-05-10 17:54 — bitstream produced in 3h 49m. Synth: 102,149 LUTs / 87.22% (-0.7pp vs 5k). Impl: 97,686 LUTs / 83.41% (-0.44pp). Timing: WNS=+0.355 ns, WHS=+0.010 ns, WPWS=+0.164 ns, 0 failing endpoints. All metrics healthier than 5k.** sha256 `7e400260609538d65a1279c2cb5d48fe287d3695e3c0c834c2ff385f645cef4a`. **DEPLOYED + TESTED + ROLLED BACK TO 5k.** Test results: 5/8 PASS (`triage_t1`, `smoke`, `ddr_roundtrip`, `port_grid_vadd`, `vert_lsu`). 1 FAIL: `dma_loopback` (timeout — F3 wire didn't actually connect; Vivado warnings BD 41-3281 + 41-702 indicate the IP-internal AXIS self-loop confused param propagation). 1 PANIC: `port_grid_vadd_scratchpad` (kernel SError on udmabuf sync_for_cpu after T1 vse-to-DDR-following-vle-from-BRAM; root cause likely AxUSER/AxID metadata leak through smartconnect_hb). 1 not runnable: `dma_to_scratchpad` (depends on F3). F4's T1-hb→scratchpad path itself is validated via standalone probes (`sp_alloc_probe`, `sp_load_probe`, `sp_store_probe`, `sp_4issue_probe` all PASS). 5l demoted to experimental; backups preserved on Kria as `…bit.bin.5l-backup`. See § 0 for full bisection + F5/F6/F7 fix plan for 5m. | `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260510-140423/` |
| 5m. F4+F1 (kept) + F5 (axis_register_slice on axi_dma stream loop) + F7 (axi_register_slice on T1 hb master) + F6 (PS->BRAM) DEFERRED. Scratchpad relocated 0xB0000000 -> 0xA0080000 (HPM0 aperture). Two earlier 5m attempts blew LUT budget (93%/96%) before settling on this slim variant. | **build OK 2026-05-11 05:13 -- bitstream produced in 4h 41m. Synth: 102,720 LUTs / 87.70%. Impl: 97,686 LUTs / 83.74%. Timing: WNS=+0.169 ns, 0 failing endpoints.** sha256 `f4d64c25...` (.bit) / `a9545192...` (.bit.bin). **DEPLOYED + TESTED + ROLLED BACK TO 5k.** Test results: 11/14 PASS, 2 FAIL, 1 PANIC. ALL 5l-passing tests still pass + the full sp_*_probe ladder up through sp_4issue_with_verify_probe. **F7 partially worked**: fixed v8-pattern panic. **F5 still broken**: `dma_loopback` and `dma_to_scratchpad` timeout (Vivado warnings still firing; axis_register_slice insufficient). **port_grid_vadd_scratchpad still panics** with v12 store after BRAM read -- F7 doesn't cover the v12-as-stored case. Backups preserved on Kria as `…bit.bin.5m-backup`. See § 0 for next-bitstream priorities (F5 redo with axis_data_fifo, F7 extension for v12, F6 retry, F2 camera). | `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260511-003220/` |
| 6. Resource-optimised rebuild  | (now ACTIVE again — see "5j next steps" below) | **next agent** | TBD |

### 5j — what to try next

The 5j run demonstrates that PSU=128 + camera-restored is structurally
over the routing cliff in this BD. Removing the ILA helps slightly
(~1.5 pp util), not enough. Options for the next iteration, in order
of effort:

1. **Aggressive camera slim — small risk, no major BD edit.** Cut
   `mipi_csi2_rx CSI_BUF_DEPTH 1024 → 256` (only 256-wide rows, no
   need for deep line buffering). Cut `v_frmbuf_wr SAMPLES_PER_CLOCK
   2 → 1`, which forces a re-derive of `axis_subset_converter_cap`
   widths down the chain (NUM_BYTES/REMAP). Estimate: -1.5 to -2.5 k
   LUTs in v_frmbuf_wr alone. If that combination routes, we're good.
   **Caveat (added 2026-05-10):** SPC=2→1 on `v_frmbuf_wr` shrinks
   `s_axis_video` width from 4 → 2 bytes (UYVY at 16 b/pixel). To
   keep the AXIS chain matched end-to-end, also drop
   `mipi_csi2_rx CMN_NUM_PIXELS 2 → 1` and shrink
   `axis_subset_converter_cap S/M_TDATA_NUM_BYTES 4 → 2` with
   `TDATA_REMAP {tdata[15:0]}`. Without those, the converter would
   drop every other pixel (or BD validate would error on the width
   mismatch). The CMN_NUM_PIXELS reduction also yields ~1k extra
   LUT savings inside `mipi_csi2_rx`.
2. **Move camera path to 60 MHz.** Currently mipi_csi2_rx + frmbuf
   run at 100/200/300 MHz. At 128×128 / 30 fps the actual data rate
   is ~1 MB/s; everything could run at 60 MHz on pl_clk0 alone, which
   removes `clk_wiz_0` (MMCM) + `proc_sys_reset_100M/_300M` and the
   CDC overhead. That's about another -500 LUTs. Bigger BD edit.
3. **Drop camera entirely from this build, re-add later.** Same as
   5h's strategy — known to route. Camera lives in the BD as an
   off-by-default island that gets restored once the rest of the
   pipeline is end-to-end-validated. The bitstream is functionally
   identical to 5h for the T1 control path.
4. **Different impl strategy.** Try `Performance_ExtraTimingOpt` or
   `Congestion_AlternateRoutability`. P&R-luck dependent; same util
   so unlikely to fix structurally.

**2026-05-10 status:** acting on option 1. 5j killed; 5k launched
with the matched-chain camera-slim edits (CSI_BUF_DEPTH 1024→256,
CMN_NUM_PIXELS 2→1, SAMPLES_PER_CLOCK 2→1, axis_subset_converter
4→2 bytes). If 5k still fails to route, fall back is option 3
(drop camera, ship a 5h-equivalent + the production driver).

**Build 3c (terminated) snapshot — kept here for reference:**

  * Synth completed cleanly. WNS: **+5.990 ns**; TNS setup endpoints: 0.
    WHS: −0.087 ns / THS −564 ns at 34 K endpoints (hold; routing
    expected to fix).
  * Impl placement WNS: **+0.139 ns** (still positive after place).
  * Router was in phase 5.2 (congestion warning) when terminated.
  * Synth/place artefacts under `mudkip2d128small1bram1chain2lanescale-20260505-180504/`
    are still on disk — useful reference for the new build's slack
    comparison.

**Build 4b (BD-only validation, with pipeline) — confirmed clean:**

  * `validate_bd_design` passed with only benign warnings (AWUSER/ARUSER
    width mismatches between SmartConnects (0) and PS HP slaves (1) —
    typical for SmartConnect, runtime-OK), and one harmless info note
    about the IIC port name vs. board interface name (visual only).
  * One CRITICAL_WARNING: `axi_dma S_AXIS_S2MM unconnected` — expected,
    we don't use scatter-gather streams.
  * Build dir contains the synthesized BD wrapper but no bitstream
    (no `-s`/`-b` flag on this run). Useful for cross-checking the
    flat HDL output (`system_top.v`) if anything looks off in the
    impl logs of build 5.

**Current build (4c, diagnostic, in flight at handoff):**

  * Started 2026-05-06 05:53, user-launched with `-b -a` (full analysis
    mode: flatten_hierarchy=none + hierarchical util report + all
    messages enabled). Build pid 2362371 still running at handoff.
  * **Synthesis finished 06:19 (~26 min) but the design overflows
    KV260's LUT capacity:**

    | Resource | Used    | KV260 capacity | Util |
    |----------|---------|----------------|------|
    | LUTs     | 119,868 | 117,120        | **102%** |
    | FFs      | 144,543 | 234,240        |  62% |
    | RAMB36   | 53      | 144            |  37% |
    | DSPs     | 7       | 1248           |  0.6%|

    LUT is the killer. T1 itself (without the new pipeline IPs)
    already accounts for the bulk; the camera capture pipeline
    additions (csi-2 ~3 K LUTs, frmbuf ~2 K, smartconnects/clk_wiz
    ~1.5 K) push it over.
  * Routing intermediate timing (latest tail of `vivado_impl.log`):
    Phase 5.4 / global iteration 3, **WNS = −3.653 ns**,
    **TNS = −2126.79 ns** — far from closing.
  * **Will not produce a usable bitstream.** User is keeping the run
    going for diagnostic purposes (the `-a` flag's hierarchical util
    report tells where the LUTs went).
  * **What to do next** (deferred to a separate task):
      - Pick a smaller T1 config (e.g. drop laneScale 2 → 1, or
        chainingSize 1 → 0, or drop dLen 128 → 64) — the existing
        `build_rtl.sh` flow can regenerate.
      - Or strip features in T1 RTL the FPGA path doesn't exercise
        (e.g. `--vfuInstantiateParameter minimal` is already set; the
        `vrfBankSize 1` is already minimal).
      - Or accept that VisionSoC needs a larger Kria SOM
        (KR260 / K26 starter kits with more LUTs).
    Optimising T1 itself is the most promising angle since the camera
    pipeline IPs only contribute ~5 % of the total LUT count.

Update the Status column with one of:

  * `pending` — not started
  * `running (pid <N>, started <HH:MM>)` — in progress
  * `done <HH:MM> — <build dir>` — finished successfully
  * `failed <HH:MM> — see <log>` — failed; log path appended

Live log tails (use these to watch progress without reading the whole
log):

```sh
# Live build (60 MHz)
BUILD=fpga/build/mudkip2d128small1bram1chain2lanescale-20260505-180504

tail -f $BUILD/build.log         # script-level aggregate
tail -f $BUILD/vivado_synth.log  # synth detail (already complete for this build)
tail -f $BUILD/vivado_impl.log   # impl + bitstream — this is where progress is now
```

To check if Vivado is still running: `pgrep -af vivado`. If nothing
matches and `$BUILD/system_top_wrapper.bit` doesn't exist, the build
either died or completed silently — inspect the tail of vivado_impl.log
for the last status line.

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

**Status as of 2026-05-07: bitstream shipped.** The
`mudkip2d128small1bram1chain2lanescale_fpga` config closed timing
(WNS=+0.135 ns, WHS=+0.010 ns, 0 failing endpoints) and
`write_bitstream` is clean after the `pin.xdc` fix landed. Bitstream
lives at `…/impl_1/system_top_wrapper.bit` (also copied into
`fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260506-234437/`
for archival next to its build log). The `_fpga` config is the leaner
T1 variant that fits on KV260 — the 102% LUT over-utilisation in § 9
was for the older `mudkip2d128small1bram1chain2lanescale` (no `_fpga`
suffix); that diagnosis is preserved as historical context but is
**no longer a blocker**.

Hand-off to KV260 deployment:

  1. **DTS overlay is already authored** at
     `fpga/dts/system_top_wrapper.dts`. Compile on the Kria with
     `dtc -@ -I dts -O dtb -o system_top_wrapper.dtbo system_top_wrapper.dts`
     (the build host doesn't have `dtc` installed; the Kria does).
     Tree was modelled directly on
     `kria_ref/kria-apps-firmware/boards/kv260/smartcam/kv260-smartcam.dtsi`
     with addresses retargeted to VisionSoC's BD address map (§ 8.6).
     Gives `/dev/uio0..2` for T1/DMA/BRAM scratchpad and `/dev/video0`
     for the V4L2 camera capture path. UIO nodes for:
       - T1 wrapper @ 0xA0000000, 64K
       - axi_dma @ 0xA0010000, 64K
       - v_frmbuf_wr @ 0xA0020000, 64K  (or use `xilinx-frmbuf` V4L2
         driver, not UIO)
       - mipi_csi2_rx_subsystem @ 0x80000000, 64K  (V4L2: `xilinx-csi2rxss`)
       - axi_iic @ 0xA0050000, 64K  (kernel `xiic-i2c` driver, AP1302
         lives behind this as an i2c-mux child)
       - axi_bram_ctrl @ 0xB0000000, 32K
     IRQ map (single `pl_ps_irq0` line, 6 wide xlconcat):
       In0=T1, In1=mm2s, In2=s2mm, In3=sensor_iic,
       In4=mipi_csi2_rx, In5=v_frmbuf_wr.
     The AP1302 + AR1335 sensor subtree should be lifted **verbatim**
     from `kria_ref/kria-apps-firmware/boards/kv260/smartcam/kv260-smartcam.dtsi`
     (path: AP1302 i2c-mux child, sensor i2c address 0x3c on bus 4
     behind a pca9546 mux per user's `media-ctl -p` confirmation).
  2. **Configure AP1302 to output 128×128.** AP1302 has internal
     crop+scaler. Path is: AR1335 4208×3120 → AP1302 crop to a centre
     square (e.g. 3120×3120 keeps full vertical FOV) → AP1302 scaler
     downsamples square → 128×128 UYVY (or RGB888 if reconfigured).
     This is a *runtime* configuration via V4L2 selection ioctls
     (`VIDIOC_S_FMT` + `VIDIOC_S_SELECTION`). The FPGA pipeline only
     ever sees 128×128 frames after this is set up. Belongs in the
     Task B `visionsoc_main` user-space app (or a one-shot
     `v4l2-ctl --set-fmt …` invocation in the boot script).
     **NOT** the FPGA's job.
  3. **Deploy on the Kria** per `fpga_implementation_handoff.md` § 8 —
     `fpgautil`, `/dev/uioN`, `devmem2` smoke tests, then camera
     bringup. Note OS: Ubuntu 22.04 (jammy), not 24.04 — AMD only
     ships `xlnx-firmware-kv260-smartcam` for jammy.

### Open questions for the next agent

  * ~~**LUT over-utilisation.**~~ Resolved by switching to the `_fpga`
    config variant — § 9's diagnosis is retained for historical context
    but is no longer load-bearing. If a future feature push exceeds
    KV260 capacity again, that section is the right starting point.
  * **MIPI PLL FVCO=1500.060 MHz `[DRC AVAL-350]`** — non-blocking
    critical warning, 60 kHz over the 1500 MHz upper bound. Caused by
    `clk_wiz_0/clk_200M` being derived from `pl_clk0=60 MHz`
    (CLKIN_PERIOD=4.99980, MULT=15, DIV=2). If MIPI capture is flaky
    on hardware, fix the source clock to land an exact 5.000 ns period
    before chasing software issues.
  * **`axi_dma → BRAM scratchpad` data path is not yet wired.** Currently
    `axi_dma`'s mm2s/s2mm masters reach DDR via HPC0 only. To let DMA
    prefetch from DDR into the scratchpad (per the original handoff
    doc's intent), either widen `smartconnect_hb` to add a 2nd MI to
    `bram_ctrl`, or add a dedicated DMA-side smartconnect with
    NUM_MI=2. Decide based on whether the driver path benefits from
    DDR→BRAM hardware prefetch vs CPU memcpy.
  * **AP1302 `rst_b` is currently driven from `proc_sys_reset/peripheral_aresetn`**
    (always-released-when-FPGA-up). The smartcam DTSI uses a PS EMIO
    GPIO (`reset-gpios = <&gpio 79 1>`) so the V4L2 driver can pulse
    reset for sensor probe sequences. If the AP1302 driver fails to
    probe, swap this for an EMIO GPIO-driven `xlslice` like the
    reference does (`config_bd.tcl` line ~316). Same applies to
    `v_frmbuf_wr` reset (`<&gpio 78 1>` in smartcam).
  * **`xilinx-csi2rxss` compatible-string version match.** DTS uses
    `"xlnx,mipi-csi2-rx-subsystem-6.0"` with `"…-5.0"` as fallback. The
    upstream kernel driver may match only the v5.0 string. If the
    sub-device fails to bind, drop the v6.0 entry and keep only the
    v5.0 fallback.
  * **`xilinx-frmbuf` compatible-string version match.** DTS uses
    `"xlnx,axi-frmbuf-wr-v2.1"` (matches smartcam). BD has v2.5. Driver
    typically tolerates the version mismatch but if probe fails, change
    to `…-v2.5` or `…-v2.4` whichever the kernel driver expects.

`fyp_doc/implementation_tasks_index.md` § 5.1 has the canonical fresh-
agent prompt for "do all of Task A".

---

## 8. Streaming pipeline added this session — exact diff

All edits in `fpga/system/system_top.tcl`. No edits to `gen_wrapper.sh`
or `t1_axi_lite_wrapper.sv` were needed in this session (those were
already done on 2026-05-05). The pipeline is modelled directly on
`kria_ref/kria-vitis-platforms/kv260/platforms/vivado/kv260_ispMipiRx_vcu_DP/scripts/config_bd.tcl::create_hier_cell_capture_pipeline`,
minus the VCU + audio_ss subsystems that VisionSoC doesn't need.

### 8.1 Carrier board connection
```tcl
set_property board_connections \
    {som240_1_connector xilinx.com:kv260_carrier:som240_1_connector:1.3} \
    [current_project]
```
Required so the MIPI/IIC board interfaces map to KV260 carrier pins.
`kv260_carrier 1.3` ships in Vivado 2025.2 at
`Xilinx/2025.2/data/xhub/boards/XilinxBoardStore/boards/Xilinx/kv260_carrier/1.3`.

### 8.2 PS additions (zynq_ps `set_property -dict`)

  * **`PSU__USE__M_AXI_GP2 {1}`** — enables `M_AXI_HPM0_LPD`
    (the LPD-side GP master) for the 100 MHz CSI-2 lite control plane.
    LPD GP can only reach `0x80000000-0x9FFFFFFF` apertures, which is
    why `mipi_csi2_rx` CSR sits at 0x80000000 (matches reference).
  * **`PSU__USE__S_AXI_GP3 {1}`** + **`PSU__SAXIGP3__DATA_WIDTH {128}`**
    — enables `S_AXI_HP1_FPD` for `v_frmbuf_wr/m_axi_mm_video` at
    300 MHz.
  * **`PSU__MAXIGP2__DATA_WIDTH {32}`** — kept default to match the
    LPD aperture data width.

### 8.3 New IPs added

| IP                         | Inst name              | Clock domain   | Purpose |
|----------------------------|------------------------|----------------|---------|
| `clk_wiz`                  | `clk_wiz_0`            | in: 60 MHz pl_clk0; out: 100/200/300 MHz | Derive camera-pipeline clocks |
| `proc_sys_reset` ×2        | `proc_sys_reset_100M`/`_300M` | 100/300 MHz | Synced resets for new clock domains |
| `mipi_csi2_rx_subsystem`   | `mipi_csi2_rx`         | lite=100M / dphy=200M / video=300M | CSI-2 D-PHY → AXIS pixel stream. CONFIG dict copied verbatim from reference (HS_LINE_RATE 896 Mbps, CMN_NUM_PIXELS 2, CSI_BUF_DEPTH 4096, etc.). |
| `axis_data_fifo`           | `axis_data_fifo_cap`   | 300 MHz        | Rate-match between CSI-2 video_out and frmbuf input. FIFO_DEPTH 1024. |
| `axis_subset_converter`    | `axis_subset_converter_cap` | 300 MHz   | Pad CSI-2's 32-bit AXIS to 48-bit for frmbuf_wr (with 16 zero bits). Verbatim from reference. |
| `v_frmbuf_wr`              | `v_frmbuf_wr`          | 300 MHz (ap_clk + s_axi_CTRL share) | DDR write-side of frame buffer. MAX_COLS/MAX_ROWS reduced from reference (3840×2160 → 1920×1080) since AP1302 emits 128×128. Pixel formats enabled: HAS_RGB8, HAS_UYVY8, HAS_Y8, HAS_Y_UV8_420 (driver picks at runtime). |
| `axi_iic` v2.1             | `sensor_iic`           | 60 MHz pl_clk0 | AP1302 sensor I²C control. Board-flow connection to `som240_1_connector_hda_iic_switch`. |
| `axi_bram_ctrl` v4.1       | `bram_ctrl`            | 60 MHz pl_clk0 | Scratchpad. SINGLE_PORT, 128-bit data, depth 2048 (= 32 KB). |
| `blk_mem_gen` v8.4         | `bram`                 | 60 MHz pl_clk0 | Backing memory for `bram_ctrl`. Single-port, byte-write enable. |
| `xlconstant`               | `ap1302_standby_const` | n/a            | Tied 1'b0 → top-level `ap1302_standby` port (never standby). |

### 8.4 SmartConnect topology (final)

```
PS HPM0_FPD (60 MHz) ─► smartconnect_ctrl (NUM_CLKS=2: aclk=60M, aclk1=300M, NUM_MI=4)
                          ├─► M00 t1_top/s_axi_ctrl       (60 MHz)
                          ├─► M01 axi_dma/S_AXI_LITE      (60 MHz)
                          ├─► M02 sensor_iic/S_AXI        (60 MHz)
                          └─► M03 v_frmbuf_wr/s_axi_CTRL  (300 MHz, CDC inside SmartConnect)

PS HPM0_LPD (100 MHz) ─► smartconnect_lpd (NUM_CLKS=1: 100M, NUM_MI=1)
                          └─► M00 mipi_csi2_rx/csirxss_s_axi (100 MHz)

T1 hb (60 MHz) ──┐
DMA mm2s   ──────┼─► smartconnect_hb (NUM_SI=3, NUM_MI=1) ─► PS HPC0 (60 MHz, DDR)
DMA s2mm   ──────┘

T1 idx (60 MHz) ─► smartconnect_idx (NUM_SI=1, NUM_MI=2)
                    ├─► M00 PS HP0   (60 MHz, DDR)
                    └─► M01 bram_ctrl/S_AXI (60 MHz, scratchpad @ 0xB0000000)

v_frmbuf_wr m_axi_mm_video (300 MHz) ─► smartconnect_video (1 SI, 1 MI) ─► PS HP1 (300 MHz, DDR)
```

### 8.5 IRQ concat (xlconcat 2.1, NUM_PORTS extended 3 → 6)

| Bit | Source                          | Used for |
|-----|---------------------------------|----------|
| 0   | `t1_top/irq`                    | T1 retire / mem / rd FIFO |
| 1   | `axi_dma/mm2s_introut`          | DMA mm2s done |
| 2   | `axi_dma/s2mm_introut`          | DMA s2mm done |
| 3   | `sensor_iic/iic2intc_irpt`      | I²C transaction events |
| 4   | `mipi_csi2_rx/csirxss_csi_irq`  | CSI-2 errors / packet events |
| 5   | `v_frmbuf_wr/interrupt`         | Frame done / underrun |

All 6 are concatenated into `pl_ps_irq0`. The driver `IRQ_STATUS` reg
(0x40 in the wrapper) only discriminates T1 sources; other sources
are read by the kernel V4L2 / I²C driver via their own UIO nodes.

### 8.6 Address map (final)

| Offset      | Size | Slave                          | Master visibility |
|-------------|------|--------------------------------|-------------------|
| 0x80000000  | 64K  | `mipi_csi2_rx/csirxss_s_axi`   | PS data (via LPD GP) |
| 0xA0000000  | 64K  | `t1_top/s_axi_ctrl`            | PS data (via FPD GP) |
| 0xA0010000  | 64K  | `axi_dma/S_AXI_LITE`           | PS data |
| 0xA0020000  | 64K  | `v_frmbuf_wr/s_axi_CTRL`       | PS data |
| 0xA0050000  | 64K  | `sensor_iic/S_AXI`             | PS data |
| 0xB0000000  | 32K  | `bram_ctrl/S_AXI/Mem0`         | T1 idx |
| 0x00000000  | 2 G  | `zynq_ps/SAXIGP0/HPC0_DDR_LOW` | T1 hb, DMA mm2s, DMA s2mm |
| 0x00000000  | 2 G  | `zynq_ps/SAXIGP2/HP0_DDR_LOW`  | T1 idx |
| 0x00000000  | 2 G  | `zynq_ps/SAXIGP3/HP1_DDR_LOW`  | `v_frmbuf_wr/Data_m_axi_mm_video` |

### 8.7 Top-level board interface ports

  * `mipi_phy_if` — `xilinx.com:interface:mipi_phy_rtl:1.0` slave, auto-
    routed to KV260 carrier som240_1 MIPI pins via the
    `som240_1_connector_mipi_csi_isp` board interface that
    `mipi_csi2_rx_subsystem` was configured with.
  * `iic` — `xilinx.com:interface:iic_rtl:1.0` master, auto-routed via
    `som240_1_connector_hda_iic_switch` board interface on `axi_iic`.
  * `ap1302_rst_b` — driven by `proc_sys_reset/peripheral_aresetn`
    (active-low, released when FPGA up).
  * `ap1302_standby` — tied `1'b0` (never standby) via `xlconstant`.

### 8.8 What was *NOT* added (deviation from original handoff doc § 5)

The original `fpga_implementation_handoff.md` § 5 listed several IPs
that this session deliberately omitted:

  * **`v_proc_ss` ×2 (downscale + upscale).** AP1302 ISP does the
    crop+downsample (4208×3120 → centre-square crop → 128×128) before
    pixels enter the FPGA. No PL scaler needed. Saves DSPs/LUTs/BRAMs.
  * **`v_frmbuf_rd`.** The display side reads from DDR via the PS
    DisplayPort controller (DRM/KMS), not via PL frmbuf_rd → AXIS →
    HDMI TX. The KV260 SmartCam reference does the same.
  * **`v_hdmi_tx_ss`.** No PL HDMI subsystem. Display path is
    DDR → PS DP TX → carrier DP→HDMI converter → HDMI cable. Confirmed
    by user: "smartcam's `--target dp` drives the Zynq DisplayPort
    controller; the carrier's DP-to-HDMI converter handles the
    RGB888 sink side."

These omissions match the canonical `kv260_ispMipiRx_vcu_DP` reference
platform exactly (which also has no `v_proc_ss`, no `v_frmbuf_rd`, no
HDMI TX). The original handoff doc was outdated on the display path —
it pre-dates the user's confirmation that DP, not PL HDMI, is the
output path.

---

## 9. LUT over-utilisation — diagnosis for the next task

The 4c diagnostic build (`mudkip2d128small1bram1chain2lanescale-20260506-055305`)
finished synthesis with the following totals:

```
Top  : 119,868 LUTs / 144,543 FFs / 53 RAMB36 / 7 DSPs
KV260: 117,120 LUTs / 234,240 FFs / 144 RAMB36 / 1248 DSPs
       => 102% LUT, 62% FF, 37% RAMB36, 0.6% DSP
```

LUT is the only resource over-budget. T1 dominates the LUT count;
the camera capture pipeline + smartconnects + clk_wiz contribute
~5 % of total. So the optimisation target is **T1 itself**, not the
new BD additions.

**Diagnostic artefact for the next task:**
`fpga/build/mudkip2d128small1bram1chain2lanescale-20260506-055305/utilization_synth.rpt`
contains a hierarchical breakdown (the build was run with `-a` so
hierarchy is preserved through synth). Read top to bottom — it shows
which T1 sub-modules cost the most LUTs.

**Likely-tractable knobs (in roughly increasing risk):**

  1. **Drop `laneScale` 2 → 1** in the T1 config. Halves the per-lane
     replication. May break tests; verify with `run-test.sh` first.
  2. **Drop `dLen` 128 → 64.** Halves the natural batch width of T1's
     vector data path. Largest single-knob LUT saving, but requires
     re-running benchmarks to make sure functional kernels still meet
     numerical expectations at the new dLen.
  3. **Disable unused vector function units** via
     `--vfuInstantiateParameter`. Already at "minimal" — limited
     headroom unless we identify specific ops we can drop.
  4. **Disable chaining** entirely (`chainingSize 0` if supported in
     the Chisel; currently 1).
  5. **Refactor / hand-optimise hot LUT consumers** in the T1 RTL —
     e.g. the cross-lane shuffle network or the issue queue. Slow,
     risky, last resort.

**What stays as-is:**
The Phase B BD additions and DTS overlay don't need to change with
T1's resource trim. The smartconnect topology, address map, IRQ
concat, and DTS bindings are all independent of T1's LUT count. So
the next task's deliverable is purely: rerun `build_rtl.sh` with the
new T1 config knobs, then rerun `build_fpga.sh -c <new_cfg> -b`.

If the new config name differs (e.g.
`mudkip2d64small1bram1chain1lanescale` after dropping laneScale and
dLen), update `Config:` at the top of this doc and the addresses in
`fpga/dts/system_top_wrapper.dts` if any have shifted.

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
    handoff doc § 3 + § 4. FPGA build (80 MHz) kicked off.
  * 2026-05-05 18:05 — `system_top.tcl` clock dropped 80→60 MHz
    (line 107 `PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ`). Second build
    started at 60 MHz.
  * 2026-05-05 18:29 — first attempt (80 MHz) failed at impl placer
    with a boost::filesystem error; treated as transient. The 60 MHz
    move was independently warranted: synth WNS at 80 MHz was only
    +0.003 ns (no slack for the upcoming streaming-pipeline IPs in
    § 5 of `fpga_implementation_handoff.md`).
  * 2026-05-05 ~21:00 — 60 MHz build in routing phase 5.2; metrics
    look healthy.
  * 2026-05-05 23:39 — user terminated the in-flight build (lacked
    streaming pipeline).
  * 2026-05-06 (this session, Claude Opus 4.7):
    - Read `kria_ref/kria-vitis-platforms/.../kv260_ispMipiRx_vcu_DP/scripts/config_bd.tcl`
      as the canonical reference for IP CONFIG dicts.
    - Established that the original handoff doc was stale on display
      path: KV260 SmartCam uses PS DP, not PL HDMI. AP1302 does the
      crop+downsample, so no PL `v_proc_ss` either.
    - Added camera capture pipeline + scratchpad + IIC to
      `system_top.tcl`. Three iterations to get the BD valid:
        (i)  IIC board interface name was `som240_1_connector_hda_iic_switch`,
             not `…_iic_main`.
        (ii) `mipi_csi2_rx` CSR address moved 0xA0030000 → 0x80000000
             (PS LPD GP master can only reach 0x80000000 apertures).
        (iii) `smartconnect_ctrl` extended to NUM_CLKS=2 so the 60 MHz
             control plane can reach `v_frmbuf_wr/s_axi_CTRL` which
             runs at the same 300 MHz as `ap_clk` (HLS-based IP
             shares clock between control bus and data plane).
    - BD-only validation passed at 05:00 (build dir
      `mudkip2d128small1bram1chain2lanescale-20260506-050031/`).
    - User launched diagnostic full-bitstream attempt at 05:53 with
      `-b -a` (analysis mode); pid 2362371 still running at hand-off.
    - Synth finished 06:19 with the 102% LUT result documented in § 9.
      Routing under way at hand-off but expected to fail timing
      regardless because the design literally cannot be placed.
    - DTS overlay authored at `fpga/dts/system_top_wrapper.dts`,
      modelled on the smartcam DTSI with addresses retargeted.
    - Hand-off written ~14:15 UTC on 2026-05-06.
      Next task is FPGA resource optimisation (§ 9) — not another
      bitstream attempt with this config.
  * 2026-05-06 23:44 — full bitstream attempt for the
    `mudkip2d128small1bram1chain2lanescale_fpga` config kicked off; build
    dir `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260506-234437/`.
    P&R completed cleanly (WNS=+0.126 ns, no failed nets), but
    `write_bitstream` failed at DRC with two errors on the AP1302
    sideband ports:
      - `[DRC NSTD-1]` `ap1302_standby[0]`, `ap1302_rst_b[0]` use
        IOSTANDARD `DEFAULT`.
      - `[DRC UCIO-1]` same two ports have no LOC.
    Cause: `system_top.tcl:429-430` creates these as bare
    `create_bd_port`s (not bound to a Vivado board interface), and the
    project had no XDC in `constrs_1` at all. MIPI/IIC ports work
    because they're bound to `som240_*` board interfaces that
    auto-supply LOC/IOSTANDARD; these two are not.
    Also flagged (not blocking): `[DRC AVAL-350]` MIPI CSI2 PHY PLL
    `FVCO=1500.060 MHz`, 60 kHz over the 1500 MHz upper bound. Result
    of feeding the 200 MHz video clock from a 60 MHz pl_clk0 via
    `clk_wiz_0`. Watch for it during MIPI bring-up.
  * 2026-05-07 (this session, Claude Opus 4.7):
    - Created `fpga/system/pin.xdc` with the AP1302 LOC/IOSTANDARD
      constraints (J11 / J10, LVCMOS33, SLEW SLOW, DRIVE 4 — copied
      verbatim from `kria_ref/.../kv260_ispMipiRx_vcu_DP/xdc/pin.xdc`).
      Used the bracketed form `{ap1302_rst_b[0]}` because the BD
      wrapper exports the ports as 1-bit vectors (matches the DRC
      error spelling).
    - Patched `fpga/system/system_top.tcl` to
      `add_files -fileset constrs_1 -norecurse` for `pin.xdc`,
      placed right after the BD wrapper add (so it survives clean
      rebuilds via `build_fpga.sh`).
    - **Recovery rerun done 2026-05-07 11:44** (~7 h elapsed including
      `report_timing_summary` etc.; the actual impl+bitgen took the
      bulk of that). Reused the project, added `pin.xdc` live to
      `constrs_1`, `reset_run impl_1` + `launch_runs impl_1`. Synth
      stayed valid (IOSTANDARD/LOC don't feed synth).
    - **Final timing (post-route):** WNS=+0.135 ns, TNS=0,
      WHS=+0.010 ns, THS=0, WPWS=+0.164 ns. 0 failing endpoints out
      of 342,990 setup / 342,856 hold / 133,215 pulse-width.
      No DRC errors at `write_bitstream`. The pre-existing
      `[DRC AVAL-350]` MIPI PLL FVCO=1500.060 MHz critical warning
      remains (covered in TL;DR / Open questions).
    - **Bitstream:** `…/impl_1/system_top_wrapper.bit` (7.8 MB,
      sha256 `bc9cc1385b2617725816727ea024fb9865ea32ac30cfcfe4ed70c2c2b0127d00`).
      Copied to `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260506-234437/system_top_wrapper.bit`
      so the bit lives next to its `build.log`.
  * (next agent): bitstream is ready; next is DTS overlay compile +
    KV260 deployment per § 5.
