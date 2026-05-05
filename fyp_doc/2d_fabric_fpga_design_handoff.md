# 2D-Fabric FPGA Deployment Handoff

**Audience:** future programmer or AI agent (Codex, Claude, etc.) who is
about to write the ARM-side driver and image-processing program for the
T1 vector unit deployed on AMD Kria KV260. Read this *before* writing
the first line of driver code. It is a sibling to
`fyp_doc/2d_fabric_handoff.md` (which is the programming model handoff
and stays the canonical reference for "how T1 behaves") — this document
covers the **PS↔PL software contract** specific to KV260.

This is not a stable spec. Several decisions below are concrete
recommendations the team has agreed on; others are flagged as "future
programmer to confirm." Treat the concrete ones as the contract that
makes the doc internally consistent, and lift them only with a good
reason.

---

## 0. Goals, audience, what to read first

### 0.1 Goals
This doc answers four engineering questions about putting T1 on KV260:

  1. How is the ARM-side driver structured and how does the user's
     image-processing program use it? (§ 1)
  2. How does the camera→T1→HDMI streaming pipeline come together for
     real-time display? (§ 2)
  3. How is DDR address space managed, and how do trained weights get
     loaded? (§ 3)
  4. Do we need a scratchpad / DMA prefetch path between DDR and T1,
     and how does it work? (§ 4)

§ 5 ports the simulator-side RVV programmer rules into the ARM-driver
context. § 6 lists open questions the future programmer must close out.

### 0.2 Locked-in environment

  * **Board**: AMD Kria KV260 vision starter kit + IAS camera daughter
    card.
  * **OS**: Ubuntu Server 24.04 LTS, already flashed on the SD card.
  * **Camera**: AR1335 13MP CMOS sensor + on-board AP1302 ISP, on the
    IAS connector. The AP1302 does demosaic / WB / exposure off-chip
    and outputs YUV422 (or RGB888) over MIPI CSI-2 — so the PL side
    does **not** need `v_demosaic` / `v_gamma_lut` / `v_proc_ss(CSC)`.
    This is a different pipeline than IMX219/RPi-cam and should not be
    confused with one.
  * **HDMI** output via the KV260 carrier card.
  * **T1 config**: `mudkip2d128small1bram1chain2lanescale`
    (`--dLen 128 --extensions zvl256b --laneScale 2 --rowNumber 1`).
  * **Per-frame compute footprint**: T1 processes **exactly one
    128×128 image per kernel pass**. Full-resolution camera frames
    are downscaled to 128×128 before T1 sees them, and the 128×128
    output is upscaled before HDMI display. **There is no tile loop.**

### 0.3 Read-first list

  1. `fyp_doc/2d_fabric_handoff.md` — T1 programming model. Required
     reading; the "stock RVV intuitions break here" rules apply on KV260
     too.
  2. `fyp_doc/system_explain.md` — block diagram of the T1 system.
  3. `fyp_doc/LSU_vertical_mode_handoff.md` — vertical-mode LSU
     resolution. Read before debugging any LSU-side issue.
  4. `tests/vision_task/benchmark_vadd.c` lines 5–305 and
     `tests/vision_task/benchmark_instructions.c` lines 5–90 — the
     RVV programmer rules R1–R8. § 5 of this doc tells you which still
     apply on KV260; the rules themselves live in those source files.

### 0.4 What this doc is NOT

This doc does not document T1's internal microarchitecture, the LSU's
chaining FSM, the SharedVRF banking, or anything about
`tests/vision_task/` simulator infrastructure. Those are upstream of
the PS↔PL boundary and live in their own docs.

---

## 1. Driver and main-program structure (Q1)

### 1.1 Mental model

The ARM core is the **scalar core**. T1 is a **coprocessor with no
autonomous control flow**: it does not fetch instructions, it does not
have a PC, it has no branch unit. The ARM emits one RVV instruction at
a time over AXI4-Lite registers, T1 executes it, the ARM blocks on IRQ
until completion drains, repeat.

```
┌──────────────────────┐                ┌────────────────────────────┐
│  ARM A53  (PS)       │  AXI4-Lite     │  AXI Lite Wrapper          │
│  Ubuntu 24.04 LTS    │  ─────────────►│  @ 0xA0000000 (64 KB BAR)  │
│  user-space program  │                │                            │
│  ┌────────────────┐  │  GP0 / HPM0    │  Issue regs / FIFOs / IRQ  │
│  │  main.c        │  │  (32-bit)      │                            │
│  │  + libt1       │  │                │           ▼                │
│  └────────────────┘  │                │       T1 core              │
│                      │                │  (HBM master → HPC0)       │
│  IRQ ◄───── pl_ps_irq0 ─────────────  │  (Idx master → HP0)        │
└──────────────────────┘                └────────────────────────────┘
                                                      │
                                                      ▼
                                                DDR (PS DDR4)
```

The ARM side is the loop body; T1 is a fancy ALU.

### 1.2 PREREQUISITE — wrapper extensions (BLOCKER)

The current `fpga/wrapper/t1_axi_lite_wrapper.sv` register map ends at
`0x40` (IRQ_STATUS). It is missing the registers needed for KV260
deployment. **Do not skip this step.** Before any vertical-mode kernel
will run on KV260 the wrapper must be extended and the bitstream
rebuilt.

> Note: `0x1C VCSR` already exists, but it is the *standard* RVV CSR
> (`vxrm[2:1]`, `vxsat[0]`). It is **not** the verticalMode bit.
> VerticalMode is the project's custom CSR `0x7c0` and needs its own
> register.

| Offset | Name             | R/W | Description |
|--------|------------------|-----|-------------|
| 0x44   | VERTICAL_MODE    | RW  | bit[0] = `issue_bits_verticalMode`, latched on issue handshake |
| 0x48   | PERF_TAG         | W   | write tag (1..255) = START a named span; write 0 = STOP |
| 0x4C   | PERF_DELTA       | RO  | cycles between most recent START/STOP pair |
| 0x50   | PERF_CYCLES_LO   | RO  | low 32 bits of free-running pl_clk cycle counter |
| 0x54   | PERF_CYCLES_HI   | RO  | high 32 bits of free-running pl_clk cycle counter |

Wrapper changes:

  * In `fpga/wrapper/t1_axi_lite_wrapper.sv`: bump `ADDR_WIDTH` from 7
    to 8, add a `reg_vertical_mode` register and an
    `issue_bits_verticalMode` output, and instantiate the perf
    counters. `PERF_TAG` triggers the 0→nonzero edge to latch
    `start_cycle = pl_cycles`, the nonzero→0 edge to compute and
    latch `delta = pl_cycles - start_cycle`. The `PERF_CYCLES_LO/HI`
    pair is just two reads off the same internal 64-bit counter.
  * In `fpga/system/gen_wrapper.sh`: add the new
    `issue_bits_verticalMode` output to the `t1_fpga_top.v` template
    and connect it through to T1's existing `verticalMode` issue field.
  * Rebuild bitstream. The 20260424 snapshot at
    `fpga/build/mudkip2d128small1bram1chain2lanescale-20260424-185300/`
    was produced before `T1Issue.verticalMode` existed and is
    functionally stale.

After the rebuild, all kernels that use vertical mode (anything that
issues `csrw 0x7c0` in the simulator) will work on KV260 by writing
`VERTICAL_MODE` instead of (or in addition to) the `csrw`. See § 5 for
how this maps onto the existing R1–R8 programmer rules.

### 1.3 Driver shape — `libt1.h` / `libt1.c`

Distribute as a small userspace library the user `#include`s into their
main program. **Not** a kernel module — kernel-mode is unnecessary
because UIO already gives userspace `mmap`+IRQ access.

```c
#include <stdint.h>

struct t1_op {
    uint32_t instruction;   // 32-bit RVV encoding
    uint32_t rs1, rs2;      // scalar operands (PA for memory ops)
    uint32_t vtype, vl, vstart, vcsr;
    uint8_t  vertical_mode; // 0 = horizontal, 1 = vertical
};

int      t1_init(void);
void     t1_close(void);

void     t1_issue(const struct t1_op *op);            // blocks via IRQ
int      t1_drain_rd(uint32_t *data, uint8_t *rd);    // pop rd FIFO
int      t1_wait_mem(unsigned n_events);              // wait for n vse drains

uint32_t t1_perf_start(uint8_t tag);  // writes PERF_TAG; returns cycles
uint32_t t1_perf_stop(void);          // writes 0 to PERF_TAG; returns delta
uint64_t t1_cycles(void);             // reads PERF_CYCLES_HI:LO
```

Implementation notes:

  * `t1_init` opens `/dev/uio0` (T1 wrapper) and `/dev/uio1` (AXI DMA),
    mmaps the BARs, enables IRQ_EN[2] (mem pending) by default. It
    returns -1 on failure with errno set; the caller fails-fast.
  * `t1_issue` writes registers `0x04..0x44` in order, then writes
    `CTRL = 1` (issue_start, W1S). Then `read(uio_fd, &irq_count, 4)`
    blocks until the wrapper raises IRQ. IRQ source is filtered via
    `IRQ_STATUS` to distinguish rd/csr/mem completions.
  * `t1_drain_rd` and `t1_wait_mem` are convenience wrappers over the
    rd FIFO and `MEM_COUNT` register; the user typically only needs
    the latter for `vse` retire counting.

### 1.4 Three programming styles for the kernel side

You will need to express RVV instruction sequences. There are three
ways:

| Style | What it is | Cost | When to use |
|-------|------------|------|-------------|
| A — Issue-per-call | Each RVV op is one `t1_issue()` call | ~10 AXI-Lite txns ≈ 1 µs/op @ 80 MHz | Bringup, debugging, short kernels |
| B — Pre-assembled microprogram | Kernel is a `uint32_t kernel[]`, driver loops and issues each | Same per-op cost as A but cleaner authoring | Production kernels, ports of `tests/vision_task/` asm |
| C — Hardware command queue | Wrapper FIFO of issue tuples; HW pulls and issues | Removes per-op AXI-Lite cost | Future work, requires wrapper extension |

**Recommendation: start with A, evolve to B** once kernels stabilise.

#### Style B toolchain (committed)

We commit to **`riscv64-linux-gnu-binutils`** — the standard GNU
triplet. It is `apt`-installable on Ubuntu Server 24.04 LTS, on any
Debian/Ubuntu dev host, and on most CI runners. Anyone with a Linux
workstation can produce identical kernel bytes; this is the portable
choice.

```sh
sudo apt install binutils-riscv64-linux-gnu
```

Build flow (ship as `vision_software/libt1/build_kernel.sh`):

```sh
#!/usr/bin/env bash
# build_kernel.sh kernel.S kernel.h
SRC=$1
DST=$2
riscv64-linux-gnu-as -march=rv32imafc_zvl256b "$SRC" -o /tmp/kernel.o
riscv64-linux-gnu-objcopy -O binary -j .text /tmp/kernel.o /tmp/kernel.bin
{
    echo "#pragma once"
    echo "#include <stdint.h>"
    printf "static const uint32_t kernel[] = {\n    "
    od -An -tx4 -w16 /tmp/kernel.bin | \
        awk '{ for (i=1;i<=NF;i++) printf "0x%s, ", $i; print "" }'
    echo "};"
} > "$DST"
```

The `.S` source is the only artefact users edit; `kernel.h` is
regenerated. The script can run on the Kria itself (single-step
workflow) or on a dev host — same bytes either way.

### 1.5 MMIO transport — UIO + DT overlay

We commit to **UIO via a device-tree overlay**. No `/dev/mem` bringup
phase; that step adds a transport-layer rewrite later for no real
gain.

  * Device-tree overlay declares `compatible = "generic-uio"` for both
    the AXI Lite wrapper at `0xA0000000` and the AXI DMA at
    `0xA0010000`, and includes the IRQ line.
  * Load the overlay with
    `sudo fpgautil -b system_top_wrapper.bit -o system_top_wrapper.dtbo`,
    or as a Kria app via `xmutil loadapp visionsoc`.
  * Userspace opens `/dev/uio0` (wrapper) and `/dev/uio1` (DMA),
    `mmap`s their reg windows, and `read(uio_fd)`s for IRQ.

This gives you interrupt-driven `t1_issue()` for free, and the same
mechanism extends to camera and HDMI when those come on line via
V4L2/DRM (which already use UIO-style fd interfaces).

### 1.6 Bitstream + DT loading

Day-to-day:
```sh
sudo fpgautil -b system_top_wrapper.bit -o system_top_wrapper.dtbo
```

For permanent install (survives reboots), use the Kria app pattern:

  1. Place `system_top_wrapper.bit` and overlay under
     `/lib/firmware/xilinx/visionsoc/`.
  2. Add the manifest expected by `xmutil`.
  3. `xmutil loadapp visionsoc` to load on demand,
     `xmutil unloadapp visionsoc` to unload.

This is the same convention as `xlnx-firmware-kv260-smartcam`; copy
its layout.

### 1.7 Cycle counting and IRQ-based wait

Three counters cover everything:

  * **Per-section / per-instruction cycles** — write the wrapper's
    `PERF_TAG` (0x48) with a non-zero tag at the start of the section,
    `0` at the end, then read `PERF_DELTA` (0x4C) for the cycle count.
    This is the same shape as the simulator's
    `place_counter(tag)` MMIO at `0x10000014`, so kernels that already
    instrument with `sw t0, 0(perf_reg)` port over by retargeting the
    address.
  * **End-to-end program cycles** — read `PERF_CYCLES_LO/HI` (0x50/0x54)
    at start and end of `main()` and subtract.
  * **Wallclock / FPS** — `clock_gettime(CLOCK_MONOTONIC, ...)` on the
    ARM side, separately. Useful for end-to-end FPS measurement which
    spans more than just T1 cycles.

IRQ-based wait inside `t1_issue`:

  * Enable `IRQ_EN[2]` (mem pending) for any kernel that ends with a
    `vse`; enable `IRQ_EN[0]` (rd pending) when consuming `vmv.x.s`
    results.
  * After kicking `CTRL.issue_start`, the driver does `read(uio_fd)`
    which blocks until the kernel module re-enables the IRQ on the
    next edge.
  * Filter the wakeup by reading `IRQ_STATUS` (`0x40`) so the driver
    doesn't get confused by spurious or unrelated IRQs.
  * **No busy-poll** in the driver. The wrapper's FIFOs have enough
    depth (`RD_FIFO_DEPTH=4`, `CSR_FIFO_DEPTH=4`) to absorb a few
    missed edges if the driver is briefly preempted.

---

## 2. Camera → downscale → T1 → upscale → HDMI (Q2)

### 2.1 Pipeline diagram

```
AR1335 + AP1302 (IAS) ─MIPI CSI-2 (YUV422)─► csi2_rx_ss ─AXIS─►
  v_proc_ss #1  (downscale 1080p ➔ 128×128, area resample)
                ▼
  v_frmbuf_wr ─► DDR  (capture_buf, 128×128, triple-buffered, on HP1)
                            ▲ ▼  HPC0  (T1 read+write)
                          ┌────────────────────┐
                          │  T1 vector unit    │
                          │  one kernel /frame │
                          └────────────────────┘
                            ▲ ▼  HPC0
  DDR  (process_buf, 128×128) ─►
  v_frmbuf_rd ─► v_proc_ss #2  (upscale 128×128 ➔ display res)
                ▼
  v_hdmi_tx_ss ─► HDMI connector (KV260 carrier)
```

The AP1302 is an on-board ISP — demosaic, white balance, exposure all
happen *before* the data hits PL. Therefore the PL pipeline is
intentionally short:

  * No `v_demosaic` (AP1302 does it).
  * No `v_gamma_lut` (AP1302 does it).
  * No CSC `v_proc_ss` (AP1302 outputs YUV422 directly; if you want
    RGB the AP1302 can be configured to emit RGB888 instead).

For T1's i8 SEW you typically take **only the Y (luma) channel through
T1** in v1 — the chroma can pass through unmodified, or be handled by
a parallel pass-through buffer.

#### Decision: downscale, not crop

Use `v_proc_ss` configured as a downscaler (Lanczos/area-resampling)
so the full sensor field-of-view shrinks proportionally to 128×128 —
the user keeps a "normal-looking" image. ROI cropping is rejected
because it discards most of the field-of-view. The ~15× downscale is
aggressive but `v_proc_ss` handles it (it is the same IP the
`kv260-smartcam` reference design uses for its scaling stages).

### 2.2 IP blocks to add to `fpga/system/system_top.tcl`

  * `mipi_csi2_rx_subsystem` — 4 lanes, YUV422 8-bit per channel.
  * `v_proc_ss` ×2 — one as downscaler (1080p→128×128), one as
    upscaler (128×128→display).
  * `v_frmbuf_wr` — downscaled stream → DDR via `S_AXI_HP1_FPD`.
  * `v_frmbuf_rd` — DDR → upscaler via `S_AXI_HP2_FPD`.
  * `v_hdmi_tx_subsystem` — drives the HDMI connector on the carrier.
  * `axi_iic` — for AP1302 sensor I²C config. Ubuntu has `i2c-tools`
    and `media-ctl` to talk to it once the V4L2 subdev driver is loaded.

Routing rule: shared SmartConnect to PS HP1/HP2 for camera/HDMI; **keep
T1's HPC0/HP0 unshared** so vector traffic doesn't contend with
camera/HDMI bandwidth. Today's `system_top.tcl` already keeps
`smartconnect_hb` and `smartconnect_idx` separate from the control plane
— extend that pattern, don't merge it.

### 2.3 Reference design — fork `kv260-smartcam`

AMD ships a reference design with the exact AR1335+AP1302+IAS hardware:

```sh
sudo apt install xlnx-firmware-kv260-smartcam
xmutil listapps   # confirms it's installed
```

Fork its DT overlay structure, AP1302 driver chain (I²C + V4L2 subdev),
and CSI-2 wiring. Drop in the downscaler + the T1 wrapper + the
upscaler. Do not roll the camera stack from scratch — the AP1302 has
non-trivial firmware initialisation that the smart-camera DT already
handles.

### 2.4 Frame-buffer strategy — one process per frame

Triple-buffer in DDR:

  * `capture_buf[3]` — frmbuf_wr writes the current downscaled frame.
  * `process_buf[3]` — T1 reads/writes here. After the kernel,
    `process_buf[i]` is what the upscaler reads.
  * Pointers rotate by index per V_SYNC.

Each buffer is 16 KB (128×128 i8) — tiny. Allocate via
physically-contiguous CMA: `reserved-memory` DT node + `udmabuf`,
or V4L2 `dma-buf` (which the smart-camera reference already uses).

### 2.5 Per-frame ARM control flow

```c
while (1) {
    int idx = wait_capture_done();           // V4L2 DQBUF, IRQ-driven
    uint32_t in_pa  = capture_buf_pa[idx];
    uint32_t out_pa = process_buf_pa[idx];
    run_t1_kernel(in_pa, weights_pa, out_pa); // emits N t1_issue() calls
    submit_for_display(idx);                  // DRM page-flip / V4L2 QBUF
}
```

**No tile loop.** One kernel pass = one frame. Edge handling for non-
multiple-of-128 source frames is the downscaler's job, not T1's.

### 2.6 Timing budget — comfortable

  * pl_clk0 = 80 MHz today (slated to drop to ≥50 MHz on re-synth for
    timing margin; numbers below cover both).
  * Per-frame budget at 30 fps: 33.3 ms = 2.66 M cycles @ 80 MHz, or
    1.66 M cycles @ 50 MHz.
  * One 128×128 kernel of `vle + vse + vadd` plus a few mode flips is
    a few hundred to a few thousand pl_clk cycles. **Even a deep
    multi-pass CV kernel fits inside one frame at 30 fps.**
  * AXI-Lite issue latency in Style A (~1 µs per instruction) for a
    50-instruction kernel is 50 µs ≪ 33 ms. Not a constraint.
  * 60 fps doubles the cycle pressure to ≈800 K–1.3 M cycles per
    frame; still comfortable for the kernel sizes we expect.

### 2.7 Cache coherence

  * **HPC0** (T1 HBM master) — I/O-coherent via ACE-Lite. Configure
    `ARCACHE = b1111` (write-back read+write allocate) and `AxDOMAIN
    = INNER_SHAREABLE` so the SCU snoops ARM caches. ARM-`kmalloc()`'d
    or `dma_alloc_coherent`-style buffers are visible to T1 without
    explicit flush.
  * **HP0** (T1 indexed master) — non-coherent. `ARCACHE = b0011`
    (non-cacheable). ARM must `__clean_dcache_area_poc()` before T1
    reads.
  * Recommendation: keep image data on HBM/HPC0; confine the indexed
    master to address-irregular ops (e.g. scratchpad, weights).
  * Camera/HDMI on HP1/HP2 — non-coherent. ARM does **not** read the
    capture buffer directly (sensor → DDR → T1 is a one-way pipe), so
    in practice this is a non-issue.

---

## 3. Address space and weight storage (Q3)

### 3.1 Two views of DDR

ARM sees virtual addresses (post-MMU). T1 sees **physical** addresses
on its AXI master (HPC0/HP0). Driver responsibility: every `rs1` /
`rs2` passed to `t1_issue()` is a PA, not a VA. `libt1` provides
either a `t1_va_to_pa()` helper or refuses VAs and forces the user
to allocate via `udmabuf`/`reserved-memory` — the latter is the
recommended discipline.

### 3.2 Allocating contiguous DDR

Three ways, in order of preference:

  1. **`u-dma-buf` kernel module** (recommended on Ubuntu Kria). Mount,
     set size, `mmap /dev/udmabuf0`, read PA from
     `/sys/class/u-dma-buf/udmabuf0/phys_addr`. The sysfs PA is what
     goes into `rs1`.
  2. `reserved-memory` DT node + `mmap /dev/mem` against the reserved
     region.
  3. dma-buf via libdrm dumb buffers — needed for V4L2/DRM
     integration. The smart-camera reference design uses this for
     camera frames; reuse the same mechanism rather than reinventing.

### 3.3 Suggested DDR memory map

Illustrative — the **future programmer locks the exact numbers in the
DT**. The point is that everything is contiguous, modest in size, and
PA-addressable from both ARM and T1.

```
0x00000000 – 0x6FFFFFFF   Linux kernel + rootfs cache (managed by Linux)
0x70000000 – 0x7000FFFF   Camera frame buffers (3 × 128×128 = 48 KB,
                           rounded up to 64 KB)
0x70010000 – 0x7001FFFF   T1 process / scratch tile (128×128)
0x70020000 – 0x77FFFFFF   Weights (read-only after boot, ≤128 MB)
```

Total reservation ≪128 MB. Plenty of room in the 2 GB DDR.

### 3.4 Weights — quantisation, layout, boot flow

  * **Quantisation.** SEW = 8, so all weights must be int8. Pretrained
    networks need a post-training-quantisation step (TFLite int8, ONNX
    QDQ, or NVIDIA QAT export). Document the per-layer scale/zero-point
    in a sidecar JSON if you need to dequantise on the ARM side.
  * **Layout.** Row-major, contiguous, **128-byte aligned** so one T1
    row = 128 elements at SEW=8 lines up exactly. Per-layer file
    `weights_layer_N.bin` of size `out_ch × in_ch × kH × kW` bytes.
  * **Boot flow.** A systemd unit on first boot reads `*.bin` from
    `/lib/firmware/visionsoc/` and copies them into the reserved DDR
    region via `mmap /dev/mem`. ARM and T1 see the same PA region;
    ARM uses it for any scalar logic that consumes weights, T1 reads
    it via `vle8.v rs1=weight_layer_pa`.
  * **Format header.** Document the layout in a small
    `vision_software/libt1/weights_format.h` so future programmers
    know what to write.

### 3.5 ARCACHE / cacheability on the AXI masters

  * HPC0: `ARCACHE=b1111`, `AxDOMAIN=INNER_SHAREABLE`.
  * HP0: `ARCACHE=b0011` (non-cacheable), manual flush required.
  * Reference: Zynq UltraScale+ TRM Ch. 14 (Cache Coherency) and Ch. 16
    (HPC vs HP slave ports).

---

## 4. Scratchpad and DMA prefetch (Q4)

### 4.1 Decision: scratchpad in v1

We add the software-managed scratchpad path now and measure, rather
than defer it. Rationale:

  * T1 processes only 128×128 = 16 KB per frame, so a 32 KB BRAM
    scratchpad holds **a full double-buffered frame on-chip**.
  * BRAM cost is small (~8 BRAM36 tiles, < 6 % of the KV260 budget).
  * AXI DMA can prefetch frame N+1 from DDR while T1 chews on frame N
    — overlaps T1 compute with DDR latency.
  * If profiling later shows it isn't pulling its weight, disabling it
    is a one-block tweak in `system_top.tcl` (§ 4.7). The reverse
    (adding the scratchpad after the rest of the design has settled)
    is more disruptive.

### 4.2 Block diagram

```
DDR (capture_buf, frame N+1)
        │
        ▼
   AXI DMA mm2s ─AXI4─► axi_bram_ctrl ─AXI4─► blk_mem_gen (32 KB, dual-port)
                                                       │ port B
                                                       ▼
                                                T1 indexed master
                                                 (vle8.v rs1 = 0xB0000000 + offset)
```

### 4.3 IP blocks to add

  * `axi_bram_ctrl` — 32 KB, AXI4 slave on port A, BRAM IF on port B.
  * `blk_mem_gen` — 32 KB true dual-port, byte-write enables, paired
    with the `axi_bram_ctrl`.
  * Extend `smartconnect_idx` to two slaves (T1 indexed master can
    reach both PS HP0 and the BRAM controller). Two masters on the
    SmartConnect: T1 indexed and AXI DMA mm2s/s2mm.
  * Address map: assign the BRAM controller at `0xB0000000` size 32 KB
    in the T1 indexed-master's view.

### 4.4 Double-buffered prefetch protocol

Two scratchpad halves, 16 KB each = one 128×128 i8 frame per half.

```c
for (int n = 0; ; n++) {
    int half_A = n & 1;          // T1 reads here
    int half_B = (n+1) & 1;      // DMA fills here

    // 1. Kick DMA mm2s for frame N+1 into half B
    dma_kick_mm2s(capture_buf_pa[(idx+1) % 3],
                  0xB0000000 + half_B * 0x4000,
                  16 * 1024);

    // 2. Run kernel on half A
    run_t1_kernel(0xB0000000 + half_A * 0x4000,
                  weights_pa,
                  0xB0000000 + half_A * 0x4000);   // in-place

    // 3. Wait DMA done IRQ on /dev/uio1
    dma_wait_done();

    // 4. Copy result of half A out to process_buf in DDR
    dma_kick_s2mm(0xB0000000 + half_A * 0x4000,
                  process_buf_pa[idx], 16 * 1024);
}
```

### 4.5 DMA descriptor lifecycle

  * ARM writes DMA control regs at `0xA0010000`: source PA, dest PA,
    length.
  * Wait IRQ via `pl_ps_irq0` on completion (UIO `/dev/uio1`).
  * **DMA knows what to fetch because ARM told it.** This is a
    software-managed scratchpad, not a cache. The user's main loop
    decides what to prefetch based on its per-frame structure.

### 4.6 Output buffer — keep it in BRAM until done

The kernel's output (T1 writes via `vse8.v`) targets the **same**
scratchpad half it just read — saves a DDR round-trip per pixel. Once
the kernel finishes, ARM kicks DMA s2mm to copy that half out to
`process_buf[idx]` in DDR for HDMI to read. Triple-buffer pointers in
DDR rotate as before.

### 4.7 Fallback — DDR-only

If the scratchpad path proves more trouble than it saves (e.g. DMA
descriptor management dominates ARM CPU time, or the BRAM port-B path
introduces a routing nightmare):

  * Skip the `axi_bram_ctrl`/`blk_mem_gen` blocks in `system_top.tcl`.
  * T1 reads/writes DDR via HPC0 directly. Buffers stay where they
    are; only the kernel's `rs1`/`rs2` change from `0xB000xxxx` PA to
    the DDR PA.
  * Profile via `PERF_DELTA` (0x4C) before deciding.

---

## 5. RVV kernel constraints inherited from the simulator-side rules

`tests/vision_task/benchmark_vadd.c` lines 36–184 documents R1–R8 — the
programmer rules that keep simulator-side kernels correct. On KV260,
some of those rules go away (Style B sidesteps the C compiler entirely)
but the hardware constraints stay.

Key insight: **Style B (pre-assembled microprogram) sidesteps R1, R2,
R3** because the ARM-side C compiler never sees vector instructions —
the kernel is a flat `uint32_t[]`. So:

| Rule | KV260 status |
|------|-------------|
| **R1** No auto-vec of non-kernel code | N/A under Style B; only matters if you invoke `riscv64-linux-gnu-gcc` on RVV intrinsics. Don't do that. Stay in asm. |
| **R2** No vector spills inside kernels | N/A under Style B. The kernel is a sequence of bytes; there are no function-call boundaries. |
| **R3** Don't enlarge the stack as a "fix" | N/A under Style B for the same reason. |
| **R4** LMUL=4, SEW=8, vl=128 | **Still a hardware constraint.** Encode `vsetvli zero, 128, e8, m4, ta, ma` at the top of every kernel. The 2D-fabric VRF banking assumes LMUL ≤ 4. |
| **R5** Disjoint LMUL=4 register groups | **Still a hardware constraint.** Use `v8 / v12 / v16 / v20 / v24 / v28` as group bases for `vrgather` and friends. |
| **R6** LSU was horizontal-only | RESOLVED 2026-05-04 per `LSU_vertical_mode_handoff.md`. After the wrapper VERTICAL_MODE extension and the bitstream rebuild, both H and V LSU work. |
| **R7** v0 mask is mode-dependent | **Still applies.** Build the mask under the same mode you intend to consume it. Don't share a mask across modes. |
| **R8** Intra-instruction consistency | **Still applies.** Memory layout reflects the active mode's permutation. `simple_instruction_vert_hori.c` and `simple_instruction_vert_lsu.c` are the canonical references. |

The takeaway: **Style B on KV260 is actually safer than Style A on the
simulator**, because the compiler-related failure modes (R1–R3) don't
exist. The remaining rules (R4, R5, R7, R8) are hardware contracts and
apply unchanged.

---

## 6. Open questions / future programmer to resolve

The decisions in this document are concrete enough to start coding, but
the following items still need a person to make a call:

  * **HPC0 coherent semantics.** Verify that ARM-write/T1-read works
    without manual flush under the current SmartConnect topology.
    Smoke test: ARM writes a known pattern via `kmalloc`, T1 issues
    `vle8.v` from that PA, ARM reads back what T1 stored. If the bytes
    differ, the SCU snoop wiring is broken.
  * **DT overlay path.** Roll your own DT overlay for the wrapper +
    DMA, or adapt the `kv260-smartcam` overlay. The smartcam overlay
    is the safer starting point.
  * **HDMI output resolution.** 1080p60 vs 720p60 vs 480p60 — locks
    the upscale ratio in `v_proc_ss #2`. Doesn't really change the
    per-frame timing budget given § 2.6.
  * **Quantisation tooling and model zoo.** Pick TFLite, ONNX QDQ, or
    a custom export — and document which.
  * **Bitstream rebuild cadence and owner.** Who owns the
    `fpga/system/build_fpga.sh` invocation and the timing closure
    workflow.

(Other ambiguities have been resolved in this doc:
register offsets locked at 0x44/0x48/0x4C/0x50/0x54;
downscale not crop;
GNU toolchain `riscv64-linux-gnu-as`;
scratchpad in v1 with a documented DDR-only fallback in § 4.7;
UIO transport;
AR1335+AP1302 pipeline.)

---

## Appendix A — Critical files

  * Modify: `fpga/wrapper/t1_axi_lite_wrapper.sv` — add VERTICAL_MODE +
    PERF_TAG/PERF_DELTA/PERF_CYCLES regs, bump ADDR_WIDTH.
  * Modify: `fpga/system/gen_wrapper.sh` — add
    `issue_bits_verticalMode` to the `t1_fpga_top.v` template.
  * Modify: `fpga/system/system_top.tcl` — add MIPI CSI-2 RX, two
    `v_proc_ss`, frmbuf_wr/rd, HDMI TX, AXI IIC, plus `axi_bram_ctrl` +
    `blk_mem_gen` for the scratchpad.
  * Reference: `fyp_doc/2d_fabric_handoff.md` (programming model),
    `tests/vision_task/benchmark_vadd.c` lines 5–305 and
    `benchmark_instructions.c` lines 5–90 (RVV programmer rules).
  * New (future programmer creates):
    - `vision_software/visionsoc_main/main.c` — image-processing main
      program.
    - `vision_software/libt1/libt1.c`, `libt1.h` — userspace driver.
    - `vision_software/libt1/build_kernel.sh` — `.S` → `kernel.h`
      helper.
    - `vision_software/libt1/weights_format.h` — weight layout
      contract.
    - DT overlay (`.dts` → `.dtbo`).

## Appendix B — Verification

A reasonable bringup sequence:

  1. **Smoke test.** `t1_init()` then `t1_issue()` of `vsetvli zero,
     128, e8, m4, ta, ma`. Read CTRL[1] `issue_ready`; expect 1. No
     DDR or DMA touched.
  2. **Single-instruction DDR roundtrip.** Pre-load a known pattern
     into a `udmabuf` PA; issue `vle8.v` from that PA into v8;
     `vse8.v` from v8 into a second `udmabuf` PA; ARM bytewise-
     compares. Confirms HPC0 path and basic LSU.
  3. **Port a simulator kernel.** Take
     `tests/vision_task/simple_instruction_asm.c`'s `grid_vadd`,
     assemble offline with `riscv64-linux-gnu-as`, embed as
     `uint32_t kernel[]`, run via `libt1` Style B, compare against
     the simulator's expected output.
  4. **Vertical mode regression.** With the wrapper extension + new
     bitstream in place, port a kernel from
     `tests/vision_task/simple_instruction_vert_lsu.c` and confirm
     transpose behaviour on real hardware.
  5. **Camera bringup separately.** V4L2 capture (gstreamer
     `xlnxvideosrc → kmssink`) → DDR → DRM HDMI passthrough at
     128×128. Verify video on the monitor before plumbing T1 in.
  6. **Full pipeline.** AR1335 → 128×128 capture buf → T1 kernel →
     128×128 process buf → upscaler → HDMI at 30 fps. Use
     `PERF_CYCLES` to measure end-to-end cycles per frame; check
     against the § 2.6 budget.
  7. **Scratchpad on/off comparison.** Run the same kernel with the
     BRAM scratchpad enabled and disabled; compare `PERF_DELTA`
     numbers to decide whether § 4.7's fallback is the better choice.

---

That's the contract. Welcome.
