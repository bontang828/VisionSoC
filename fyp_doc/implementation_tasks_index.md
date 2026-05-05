# Implementation Tasks Index — VisionSoC on Kria KV260

This is the **front door** for anyone (human, Codex, Claude, etc.) about
to implement part of the KV260 deployment. It does not contain the
implementation steps themselves — those live in the prescriptive
handoff docs. It tells you **which doc is the primary instruction set
for which task**, and **which other docs you must read first as
background.**

If you read only one section of this index, read § 5: it has copy-
pasteable prompts for spawning a fresh agent on each task.

---

## 1. How to use this index

There are two distinct, sequential tasks to complete the KV260
deployment:

  * **Task A — FPGA implementation.** Extend the wrapper, regenerate
    RTL, add IP blocks, build the bitstream, write the device-tree
    overlay.
  * **Task B — Driver implementation.** Build `libt1`, the
    `build_kernel.sh` helper, the `visionsoc_main` example program,
    and the boot-time setup.

**Task B depends on Task A.** Do not start B until A is complete and
the bitstream loads on the Kria with `/dev/uio0` and `/dev/uio1`
visible.

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

## 4. Cross-task contracts

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

## 5. One-shot prompts for a fresh agent session

Copy-paste either of these into a new Codex/Claude session to spawn
an implementation agent on the right task with the right context.

### 5.1 FPGA implementation prompt

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

### 5.2 Driver implementation prompt

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

---

## 6. Doc map

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
