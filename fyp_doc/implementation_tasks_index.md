# Implementation Tasks Index — VisionSoC on Kria KV260

This is the **front door** for anyone (human, Codex, Claude, etc.) about
to implement part of the KV260 deployment. It does not contain the
implementation steps themselves — those live in the prescriptive
handoff docs. It tells you **which doc is the primary instruction set
for which task**, and **which other docs you must read first as
background.**

If you read only one section of this index, read § 6: it has copy-
pasteable prompts for spawning a fresh agent on each task.

---

## 1. How to use this index

There are three distinct, sequential tasks to complete the KV260
deployment:

  * **Task A — FPGA implementation.** Extend the wrapper, regenerate
    RTL, add IP blocks, build the bitstream, write the device-tree
    overlay.
  * **Task B — Driver implementation.** Build `libt1`, the
    `build_kernel.sh` helper, the `visionsoc_main` example program,
    and the boot-time setup.
  * **Task C — On-Kria deployment and end-to-end run.** scp the
    bitstream + dtbo, install Kria-side prerequisites, load the
    overlay, run the libt1 hardware tests in sequence, and bring up
    the camera→T1→HDMI pipeline.

**Tasks form a strict chain: A → B → C.** B's hardware verification
is folded into C; do not start C until A has produced a fitting
bitstream and B compiles cleanly on the dev host.

For each task, this index lists:

  * The **primary instruction set** doc — read top-to-bottom and
    follow it.
  * The **required background** docs — read first to understand *why*
    each step exists.
  * The **source files** to read for grounding.
  * The **definition of done** (verification checkpoint).

---

## 2. Task A — FPGA implementation

### 2.1 Primary instruction set
**`fyp_doc/fpga_implementation_handoff.md`** (~714 lines)

A step-by-step prescriptive guide. Sections:

  1. § 0 — scope, tools, prerequisites.
  2. § 1 — target wrapper register map (source of truth).
  3. § 2 — Step 1: regenerate T1 RTL.
  4. § 3 — Step 2: extend the AXI Lite wrapper SV.
  5. § 4 — Step 3: update `gen_wrapper.sh` template.
  6. § 5 — Step 4: extend the Vivado block design TCL.
  7. § 6 — Step 5: build the bitstream.
  8. § 7 — Step 6: device-tree overlay.
  9. § 8 — Step 7: hardware verification (smoke + camera).
  10. § 8.4 — debugging / common failures.

### 2.2 Required background — read before starting

  * **`fyp_doc/2d_fabric_fpga_design_handoff.md`** (~727 lines) —
    design-rationale doc. Explains *why* the wrapper extensions and
    IP-block additions are what they are. Read at minimum:
    - § 0 (locked-in environment),
    - § 1.2 (verticalMode wrapper-extension blocker),
    - § 2 (camera pipeline rationale),
    - § 4 (scratchpad rationale).
  * **`fyp_doc/2d_fabric_handoff.md`** — T1 programming model. Skim §
    2 (the H/V mode switch) so the importance of `verticalMode`
    plumbing is clear.
  * **`fyp_doc/LSU_vertical_mode_handoff.md`** — explains why the
    LSU now honours the verticalMode snapshot. Useful background for
    the wrapper-design section.

### 2.3 Source files to read for grounding

  * `fpga/wrapper/t1_axi_lite_wrapper.sv` — current wrapper. You will
    edit this file.
  * `fpga/system/system_top.tcl` — current Vivado BD script. You will
    edit this.
  * `fpga/system/gen_wrapper.sh` — current Verilog wrapper template.
    You will edit this.
  * `fpga/system/build_fpga.sh` — current build orchestrator. You
    will not edit this; you will invoke it.
  * `t1/src/Bundles.scala:659` — confirms `T1Issue.verticalMode` is
    already in Scala (no Scala changes needed).
  * `t1/src/T1.scala:566` — confirms it is wired through.
  * `fpga/build/mudkip2d128small1bram1chain2lanescale-20260424-185300/`
    — current (stale) bitstream artefacts. Inspect
    `timing_impl.rpt` and `utilization_impl.rpt` for a baseline.

### 2.4 Definition of done

`fpga_implementation_handoff.md` § 8 verifies on hardware:

  * `fpgautil -b ... -o ...` loads cleanly.
  * `/dev/uio0` and `/dev/uio1` exist.
  * `devmem2 0xa0000050 w` returns advancing values (free-running
    cycle counter ticking).
  * `devmem2 0xa0000044 w 0x1` followed by a read returns `0x1`
    (VERTICAL_MODE round-trip).
  * Camera independent path (`gst-launch-1.0 v4l2src ! kmssink`)
    shows live video on HDMI.

When all five pass, Task A is done.

---

## 3. Task B — Driver implementation

### 3.1 Primary instruction set
**`fyp_doc/driver_implementation_handoff.md`** (~926 lines)

A step-by-step prescriptive guide. Sections:

> **Companion doc:** `fyp_doc/driver_function_spec.md` is a per-file,
> per-function checklist with implementation order, signatures, and
> what each function does. Use it as the to-do list while
> implementing; refer back to the handoff doc above for the full
> code sketches. The two are kept in sync — handoff = prose +
> example code, function_spec = scoped checklist.

  1. § 0 — scope, prereqs (note the `apt install` list).
  2. § 1 — file layout under `vision_software/`.
  3. § 2 — `libt1_regs.h` (mirror of FPGA register map).
  4. § 3 — public API (`libt1.h`).
  5. § 4 — implementation (`libt1.c`) — `t1_init`, `t1_issue`, perf
     helpers, udmabuf allocator, DMA helpers.
  6. § 5 — weight format header.
  7. § 6 — `build_kernel.sh` (.S → uint32_t[]).
  8. § 7 — three test programs: smoke, ddr_roundtrip, port_grid_vadd.
  9. § 8 — Makefile.
  10. § 9 — `visionsoc_main/main.c` example.
  11. § 10 — ordered bringup sequence (8 steps).
  12. § 10.1 — debugging / common failures.

### 3.2 Required background — read before starting

  * **`fyp_doc/2d_fabric_handoff.md`** — required reading. Explains
    the 2D fabric, R1–R8 programmer rules. Without this, kernels you
    write will produce wrong results that look right.
  * **`fyp_doc/2d_fabric_fpga_design_handoff.md`** § 1 (driver
    structure rationale) and § 5 (which simulator-side rules R1–R8
    still apply on KV260).
  * **`fyp_doc/fpga_implementation_handoff.md`** § 1 — the register
    map. `libt1_regs.h` must mirror this exactly.
  * **`tests/vision_task/benchmark_vadd.c`** lines 5–305 — the
    canonical RVV programmer rules (R1–R8) with concrete examples.
    Read in source.
  * **`tests/vision_task/benchmark_instructions.c`** lines 5–90 —
    counterpart for instruction-level benchmarking patterns.

### 3.3 Source files to read for grounding

  * `tests/vision_task/simple_instruction_asm.c` — canonical naked-asm
    kernel pattern. Port this directly to `kernels/grid_vadd.S`.
  * `tests/vision_task/simple_instruction_vert_lsu.c` — vertical-mode
    LSU kernel. Use this for the vertical regression in § 10 step 5
    of the driver impl doc.
  * `tests/vision_task/simple_instruction_gather.c`,
    `simple_instruction_gather_scalar.c`,
    `simple_instruction_vert_hori.c` — additional kernel-pattern
    references.
  * `tests/emurt/emurt.c` and `tests/t1_main.S` — simulator runtime.
    Useful only as a contrast: this is the runtime the kernels were
    written for, and the driver doc explains what changes when the
    runtime is Linux/Ubuntu instead.

### 3.4 Prerequisite

**Task A must be complete.** Before you start Task B, verify on the
Kria:

```sh
ls /dev/uio*                          # /dev/uio0 and /dev/uio1
sudo devmem2 0xa0000050 w             # value should change between calls
sudo devmem2 0xa0000044 w 0x1
sudo devmem2 0xa0000044 w             # should read 0x1
```

If any of these fail, go back to Task A.

### 3.5 Definition of done

`driver_implementation_handoff.md` § 10 walks through eight ordered
steps:

  1. FPGA side complete (Task A's verification).
  2. `smoke.c` passes.
  3. `ddr_roundtrip.c` passes (vle + vse round-trip via udmabuf).
  4. `port_grid_vadd.c` passes (Style B, simulator-kernel port).
  5. Vertical-mode regression passes (`simple_instruction_vert_lsu`
     ported).
  6. Camera bringup standalone (gstreamer to HDMI, no T1).
  7. DMA path standalone (16 KB udmabuf-to-udmabuf copy via mm2s).
  8. Full pipeline at ≥30 fps.

When step 8 passes, Task B is done.

---

## 4. Task C — Deploy on Kria KV260 and run end-to-end

### 4.1 Primary instruction set

This section, plus `fyp_doc/camera_bringup_status.md` for the
in-flight execution state (what's been done, what's pending,
known gotchas, sudoers contents, exact shell snippets). The plan
below is the *what*; the status doc is the *where-we-are* and
contains the *how* for any step whose recipe has been pinned down.

The Kria is reachable from this dev host via `ssh kv260` (verified
2026-05-07) and has a scoped passwordless sudoers entry for the
`ubuntu` user (see status doc § 4 for the full allowlist).

#### Plan steps

  1. **Kria-side prerequisites (one-time).** apt-install
     `devmem2`, `binutils-riscv64-linux-gnu`, `libdrm-dev`,
     `linux-headers-$(uname -r)`, `dkms`, `build-essential`. Build
     and load the **ikwzm `u-dma-buf` kernel module** (the
     mainline `/dev/udmabuf` is a different driver — libt1 needs
     ikwzm's `/dev/udmabufN` + `phys_addr` sysfs). Allocate three
     4 MB buffers via modprobe params. *State: done 2026-05-07
     (status doc § 1). Persistence files not yet written.*
  2. **Free the FPGA from the default app.** `sudo xmutil
     unloadapp` to drop `k26-starter-kits` so its `/dev/uio*`
     nodes don't collide with the visionsoc overlay. Restart
     `dfx-mgrd` or `rmdir` the configfs entry as fallback.
  3. **Stage + load the bitstream.** `scp` `system_top_wrapper.bit`
     and `system_top_wrapper.dts` to the Kria, compile the dtbo
     there with `dtc -@ -I dts -O dtb`, install both into
     `/lib/firmware/xilinx/visionsoc/`, then `sudo fpgautil -b … -o …`.
  4. **Smoke-check the FPGA** (Task A's DoD on real silicon).
     Verify `/sys/class/uio/uio*/name` reports `t1_top` /
     `axi_dma` etc. (not `axi-pmon`); `devmem2 0xa0000050 w`
     advances run-to-run; `0xa0000044` round-trips `0x1`. If the
     UIO index ordering differs from libt1's hard-coded
     `/dev/uio0=T1`, `/dev/uio1=DMA`, fix `libt1.c` to look up
     by binding name (Task B follow-up).
  5. **Build the driver natively on the Kria.** `scp -r
     vision_software/ kv260:~/`, then `make` libt1 + `make
     kernels` + `make` for `visionsoc_main`. (Dev host has no
     `rsync`; use `scp -r`.) Native build avoids cross-toolchain
     pain and lets `libdrm` pkg-config find the right paths.
  6. **Run libt1 hardware tests in order.** `smoke →
     ddr_roundtrip → dma_loopback → port_grid_vadd → vert_lsu`.
     Mirrors `driver_implementation_handoff.md` § 10 steps 2–5.
  7. **Configure the camera path.** Use `media-ctl` + `v4l2-ctl`
     to set the AP1302 sub-device to UYVY8_1X16 / 128×128. The
     exact entity name and pad numbers depend on what Vivado's
     v_frmbuf binding enumerates after the overlay loads — see
     status doc when the recipe is pinned.
  8. **Run end-to-end.** `sudo ./visionsoc_main`. Expects
     `/dev/dri/card0` free (no X/gdm holding it). Live
     128×128 camera → DMA → T1 (`grid_vadd` placeholder) → DMA →
     DP-TX → HDMI at ≥30 fps.
  9. **Iteration loop after first success.** `scp -r
     vision_software/ kv260:~/`, ssh `make`, ssh-run the test.
     Bitstream reload only on RTL/BD changes; suggested helper
     `scripts/deploy_kv260.sh --reload-fpga`.

For the actual shell snippets, current state, and known
gotchas, see `fyp_doc/camera_bringup_status.md`.

### 4.2 Required background — read before starting

  * **`fyp_doc/camera_bringup_status.md`** — live state of Task C.
    Read this first; it tells you which steps above are done and
    which gotchas have been hit.
  * **`fyp_doc/fpga_build_status.md`** — confirm a fitting
    bitstream exists. As of 2026-05-06 the design overflows LUTs at
    102 % on the standard preset; `_fpga` preset (DSP-mapped
    multiplier + stub divider) was added to fix it. See
    `lut_optimisation_div_dsp.md` for the optimisation work and
    the sim-parity matrix that gates Task A's bitstream rebuild.
  * **`fyp_doc/driver_implementation_status.md`** — confirms libt1
    + visionsoc_main compile cleanly on the dev host. § "Important
    caveats" lists runtime gaps that may bite during steps 4 / 6 /
    8 (UIO ordering, DRM seat ownership, DMA ↔ scratchpad
    plumbing, AP1302 V4L2 config).
  * **`fyp_doc/fpga_implementation_handoff.md`** § 8 — the original
    on-board verification recipe that steps 4 / 7 / 8 expand on.
  * **`fyp_doc/driver_implementation_handoff.md`** § 10 — the
    eight-step bringup sequence step 6 mirrors.

### 4.3 Source files / artefacts to have ready

  * `fpga/build/<config>-<TS>/system_top_wrapper.bit` — produced by
    Task A after the resource-fitting build closes timing.
  * `fpga/dts/system_top_wrapper.dts` — already committed; compile
    to `.dtbo` on the Kria in step 3.
  * `vision_software/libt1/{libt1.{a,so},test/*}` — built by Task B.
  * `vision_software/visionsoc_main/visionsoc_main` — built by
    Task B.

No source-file edits are expected during C. If step 4 reveals a
hard-coded UIO index mismatch, fix it in
`vision_software/libt1/libt1.c` and re-run step 5 onwards.

### 4.4 Prerequisites

Tasks A and B must both be *built* (their on-hardware verification
is what Task C performs). Specifically:

  * `system_top_wrapper.bit` exists; `system_top_wrapper.dts`
    addresses match the wrapper register map in § 5 below.
  * `libt1.{a,so}` and the five test binaries link cleanly under
    `vision_software/libt1/`.
  * `visionsoc_main` links against `libt1.a` and `libdrm`.
  * `ssh kv260` reachable; passwordless sudo configured (status
    doc § 4).

### 4.5 Definition of done

  1. Step 4 — UIO nodes show T1 / DMA names (not `axi-pmon`),
     `devmem2 0xa0000050 w` advances between calls,
     `0xa0000044` round-trips `0x1`.
  2. Step 6 — all five libt1 hardware tests pass.
  3. Step 7 — `v4l2-ctl --stream-mmap` captures a non-empty
     `/tmp/cap.uyvy` from `/dev/video0`.
  4. Step 8 — `visionsoc_main` shows live 128×128 camera frames on
     the HDMI monitor through the DP-TX path.
  5. Throughput: ≥30 fps end-to-end on the visible pipeline.

When all five pass, Task C — and the project — is done.

---

## 5. Cross-task contracts

The two tasks meet at three interfaces. Both sides must agree:

| Contract | FPGA side (Task A) | Driver side (Task B) |
|----------|--------------------|----------------------|
| **Register map** | `fpga/wrapper/t1_axi_lite_wrapper.sv` defines offsets 0x00–0x54 | `vision_software/libt1/libt1_regs.h` mirrors them. **Drift = silent corruption.** |
| **Device-tree overlay** | FPGA side authors `dts/system_top_wrapper.dts` and produces `.dtbo` | Driver side reads the resulting `/dev/uioN` paths (uio0=T1, uio1=DMA) and `/dev/video0`, `/dev/dri/card0` |
| **Artefact location** | Bitstream + dtbo end up at `/lib/firmware/visionsoc/system_top_wrapper.{bit,dtbo}` | systemd unit `visionsoc.service` loads them via `fpgautil -b ... -o ...` |
| **Memory map** | DDR reservation + scratchpad `0xB0000000–0xB0007FFF` declared in DTS | Driver allocates buffers via `udmabuf` from the reserved region; passes PAs to `rs1`/`rs2` |
| **Interrupts** | `pl_ps_irq0` carries T1 wrapper IRQ + DMA mm2s/s2mm IRQ + frmbuf/HDMI/IIC IRQs | UIO `read(fd)` blocks on the right line; `IRQ_STATUS` (0x40) discriminates source |

If the FPGA side changes a register offset, the driver's
`libt1_regs.h` must change in the same commit. If the driver expects
a new IRQ source, the FPGA side must wire it through `irq_concat`.

---

## 6. One-shot prompts for a fresh agent session

Copy-paste any of these into a new Codex/Claude session to spawn
an implementation agent on the right task with the right context.

### 6.1 FPGA implementation prompt

```
You are implementing the FPGA-side changes for VisionSoC on AMD Kria
KV260. Your primary instruction set is
fyp_doc/fpga_implementation_handoff.md — follow it section by section.

Required background reading before you write any code:
  1. fyp_doc/2d_fabric_fpga_design_handoff.md (design rationale —
     read § 0, § 1.2, § 2, § 4 at minimum)
  2. fyp_doc/2d_fabric_handoff.md (T1 programming model — skim § 2)
  3. fyp_doc/LSU_vertical_mode_handoff.md

Source files to read for grounding (do not edit Scala):
  - fpga/wrapper/t1_axi_lite_wrapper.sv
  - fpga/system/system_top.tcl
  - fpga/system/gen_wrapper.sh
  - t1/src/Bundles.scala (verify line 659 has T1Issue.verticalMode)
  - t1/src/T1.scala (verify line 566 wires it through)

The task is complete when § 8 of fpga_implementation_handoff.md
verifies cleanly on hardware: fpgautil loads, /dev/uio0 + /dev/uio1
exist, devmem2 confirms PERF_CYCLES advances and VERTICAL_MODE round-
trips, and gstreamer can stream the camera to HDMI.

If any step fails, consult § 8.4 (debugging) before re-trying.
```

### 6.2 Driver implementation prompt

```
You are implementing the ARM-side driver and main program for VisionSoC
on AMD Kria KV260 running Ubuntu Server 24.04 LTS. Your primary
instruction set is fyp_doc/driver_implementation_handoff.md — follow it
section by section.

PREREQUISITE: the FPGA side (Task A) must be complete first. Verify by
running:
    ls /dev/uio*
    sudo devmem2 0xa0000050 w   (value should change between calls)
    sudo devmem2 0xa0000044 w 0x1
    sudo devmem2 0xa0000044 w   (should read 0x1)
If any of these fail, stop and complete Task A using
fyp_doc/fpga_implementation_handoff.md before continuing.

Required background reading:
  1. fyp_doc/2d_fabric_handoff.md (T1 programming model — REQUIRED;
     ignore this and your kernels will be silently wrong)
  2. fyp_doc/2d_fabric_fpga_design_handoff.md § 1 and § 5
  3. fyp_doc/fpga_implementation_handoff.md § 1 (register map — your
     libt1_regs.h must mirror this exactly)
  4. tests/vision_task/benchmark_vadd.c lines 5–305 (RVV programmer
     rules R1–R8)
  5. tests/vision_task/benchmark_instructions.c lines 5–90

Source files to read for grounding (do not edit):
  - tests/vision_task/simple_instruction_asm.c (canonical kernel)
  - tests/vision_task/simple_instruction_vert_lsu.c (vertical kernel
    for the regression in § 10 step 5)

The task is complete when § 10 of driver_implementation_handoff.md
walks through all 8 bringup steps successfully, ending with a 30+ fps
camera→T1→HDMI pipeline.

If any step fails, consult § 10.1 (debugging) before re-trying.
```

### 6.3 KV260 deployment prompt

```
You are deploying VisionSoC onto an AMD Kria KV260 running Ubuntu
22.04 (jammy) and bringing the camera→T1→HDMI pipeline up live. Your
primary instruction set is fyp_doc/implementation_tasks_index.md § 4
(Task C) — follow § 4.1.1 through § 4.1.9 in order.

PREREQUISITES: Tasks A and B must both be *built* — i.e. a fitting
bitstream exists at fpga/build/<config>-<TS>/system_top_wrapper.bit
and `make` runs clean in vision_software/libt1 + visionsoc_main on
the dev host. Do NOT start Task C until both hold.

The Kria is reachable via `ssh kv260` (verified 2026-05-07). Sudo
on the Kria needs a password — use `ssh -t kv260 sudo …` to forward
the prompt. Your iteration loop is:
  1. edit on the dev host,
  2. `rsync vision_software/ kv260:~/vision_software/`,
  3. `ssh kv260 'cd ~/vision_software/{libt1,visionsoc_main} && make'`,
  4. ssh and run the relevant test under sudo.
A bitstream reload is only needed for RTL/BD changes — see § 4.1.9
for the helper script template.

Required background reading:
  1. fyp_doc/fpga_build_status.md — confirm a fitting bitstream
     exists (LUT 102 % over-utilisation must be resolved first via
     the `_fpga` preset; see fyp_doc/lut_optimisation_div_dsp.md).
  2. fyp_doc/driver_implementation_status.md — known caveats that
     bite during § 4.1.4 / § 4.1.6 / § 4.1.8.
  3. fyp_doc/fpga_implementation_handoff.md § 8 — the original
     hardware-verification recipe.
  4. fyp_doc/driver_implementation_handoff.md § 10 — the bringup
     sequence § 4.1.6 mirrors.

Known gaps that may require Task B follow-ups:
  * `/dev/udmabuf` on the Kria is the mainline kernel udmabuf, NOT
    ikwzm u-dma-buf. § 4.1.1 walks through building + loading the
    ikwzm module.
  * UIO node ordering after our overlay loads may not match the
    libt1 hard-coded `/dev/uio0=T1` / `/dev/uio1=DMA`. If § 4.1.4
    shows different mapping, fix libt1.c to look up by
    `/sys/class/uio/uio*/name`.

The task is complete when all five DoD checks in § 4.5 pass — UIO
sanity, all five libt1 hw tests, V4L2 capture, visionsoc_main on
HDMI, and ≥30 fps end-to-end.
```

---

## 7. Doc map

```
fyp_doc/
├── implementation_tasks_index.md         ← you are here
├── 2d_fabric_handoff.md                  ← T1 programming model (req'd reading for both tasks)
├── 2d_fabric_fpga_design_handoff.md      ← KV260 design rationale (req'd reading for both tasks)
├── LSU_vertical_mode_handoff.md          ← LSU axis behaviour (background)
├── fpga_implementation_handoff.md        ← Task A primary instruction set
├── fpga_build_status.md                  ← in-flight Task A build status (live state)
├── driver_implementation_handoff.md      ← Task B primary instruction set
├── driver_function_spec.md               ← Task B per-function checklist (companion to handoff)
├── system_explain.md                     ← block-diagram overview
├── vrgather_vx_debug_handoff.md          ← topic-specific debug log
└── ...
```
