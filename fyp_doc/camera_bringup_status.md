# KV260 Bringup Status — pickup doc for next agent

**Owner of last touch:** Claude (Opus 4.7), 2026-05-07 ~15:35 UTC
**Branch:** `fpga_driver`
**Target:** AMD Kria KV260, Ubuntu 22.04 LTS (jammy), reachable via
`ssh kv260` from this dev host (`cloud-vm-44-108`).

This doc tracks the in-flight Task C deployment work — putting the
VisionSoC bitstream + driver onto the Kria and bringing the
camera→T1→HDMI pipeline up. The plan itself lives in
`fyp_doc/implementation_tasks_index.md` § 4 (Task C); this file is
the live state — keep "Current state" at the top truthful so a
fresh agent can resume without rereading the whole conversation.

---

## 0. TL;DR for the next agent

  * **5K BITSTREAM IS THE CURRENT FLASHED ARTEFACT (2026-05-10).**
    Built from `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260510-025732/`,
    deployed to Kria via `bit2bin.py` + `scp` + `sudo install` +
    `fpgautil -b … -o …` (391 ms load). Camera capture pipeline
    is in the BD this time (mipi_csi2_rx + axis_data_fifo +
    axis_subset_converter + v_frmbuf_wr → HP1 DDR), but the dts
    nodes for it are still `status="disabled"` because § 6.3
    (axi_iic kernel panic on first probe) is unresolved. Same
    5/6 libt1 tests pass as on 5h. The 5h `.bit.bin` is preserved
    on the Kria at `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5h-backup`.
    Build details + the slim recipe that closed timing live in
    `fyp_doc/fpga_build_status.md` § 0 + the 5k row of the table.

  * **Bitstream LOADS cleanly on hardware.** `fpgautil -b
    system_top_wrapper.bit.bin -o system_top_wrapper.dtbo` succeeds
    in ~391 ms (5k) / ~414 ms (5h), the device-tree overlay applies
    cleanly. `/dev/uio*` enumerate as `uio0..3=axi-pmon` (PS-base
    AXI Performance Monitors, always present), `uio4=t1`,
    `uio5=dma`, `uio6=bram`. After a reboot the active overlay is
    `k26-starter-kits_image_1`; remove it first
    (`sudo rmdir /sys/kernel/config/device-tree/overlays/k26-starter-kits_image_1`)
    before `fpgautil`-loading visionsoc, otherwise both overlays
    coexist with the k26 PL still trying to drive the bitstream
    that 5k just replaced.

  * **AXI4-Lite read-lane bug FIXED 2026-05-09** in build 5h
    (`…fpga-20260509-121320/`). Root cause: PS-internal NIC-400 is
    natively 128-bit and, when `PSU__MAXIGP0__DATA_WIDTH=32`, emits
    multi-beat AXI4 bursts at HPM0_FPD to span each 16-byte slot;
    SmartConnect (in Low-Area Mode for the all-AXI4-Lite-32 fanout)
    correctly splits each burst into single-beat AXI4-Lite reads, the
    wrapper responds correctly to each, but the PS-side response
    repacking only restored lane 0, zero-filling the rest. The fix is
    `PSU__MAXIGP0__DATA_WIDTH=128` — the PS skips its internal
    downsizer entirely, emits a single `arsize=2,arlen=0` 32-bit
    transaction directly, no lane mapping required, the SmartConnect
    + AXI4-Lite slave handle it transparently.
    `triage_t1` now passes every offset on hardware (T1 wrapper +
    DMA control). **Full diagnosis chain in § 6.1.4; the resolution
    is captured in § 6.1.5.** Driver-side issues (§ 6.2 UIO ordering
    + two latent bugs surfaced by review) were fixed in `libt1.c` /
    `camera.c` / `display.c` on 2026-05-07 — see
    `fyp_doc/driver_implementation_status.md` § "Bringup-driven
    driver fixes" for the diff and consequences.

  * **Hardware test suite passing 2026-05-09 (5h) and re-passing
    2026-05-10 on 5k** (5/6 libt1 hw tests, identical results
    across both bitstreams). After Fix 2 unblocked the control
    plane, an apparent "LSU retire never fires" bug turned out
    to be a CPU-cache coherency issue on the udmabuf-backed
    `t1_buf`: udmabuf mmap is cached by default and Linux's
    `msync()` doesn't reliably manage cache for it. The proper
    interface is the udmabuf sysfs sync attributes. libt1 now
    has `t1_buf_sync_for_device(&buf)` (call before T1 reads)
    and `t1_buf_sync_for_cpu(&buf)` (call after T1 writes); they
    write to `/sys/class/u-dma-buf/udmabufN/sync_for_{cpu,device}`.
    The hw LSU itself (T1 m_axi_hb load AND store) is fully
    functional — proven via raw-mmio `lsu_load_probe` /
    `lsu_store_probe`, then the libt1 tests with the cache fix.
    § 6.4 has the diagnosis chain. **Result on hw (both 5h and
    5k):** `triage_t1`, `smoke`, `ddr_roundtrip`, `port_grid_vadd`,
    `vert_lsu` all PASS; `dma_loopback` fails because
    `axi_dma/S_AXIS_S2MM` is unconnected in the BD (longstanding
    critical warning, not LSU; the same warning persists in 5k).

  * **Camera path is currently disabled in the dts** (§ 6.3). The
    `axi_iic` block panicked the kernel on first probe with an
    Asynchronous SError Interrupt during `xiic_reinit`. Likely BD-
    level (clock / aresetn / aperture) — not source-side. The dts
    nodes for `axi_iic_sensor`, `isp_csiss`, `isp_fb_wr_csi`, and
    `isp_vcap_csi` now have `status = "disabled";` so the kernel
    skips them. As of 5k (2026-05-10) the bitstream actually has
    the camera IPs in the BD again (mipi_csi2_rx + v_frmbuf_wr +
    sensor_iic + clk_wiz + axis chain) — but they're dormant
    because the dts has them disabled. Two open follow-ups before
    camera streaming actually works:
      (a) resolve § 6.3 (axi_iic kernel panic) and re-enable the
          dts nodes one at a time;
      (b) fix the BD 41-237 `s_axis_video(3)` vs
          `axis_subset_converter/M_AXIS(2)` width mismatch — at
          5k's `SAMPLES_PER_CLOCK=1, HAS_UYVY8=1` config,
          `v_frmbuf_wr/s_axis_video` is 3 bytes wide (Vivado pads
          internally for format alignment), but the converter
          emits 2 bytes. Either widen the converter and zero-pad
          (`tdata={8'h00, tdata[15:0]}`) or accept that the upper
          byte feeding frmbuf is undefined and frames will be
          chroma-corrupted.

  * **Sudo on the Kria is passwordless for a scoped allowlist** —
    file `/etc/sudoers.d/visionsoc-nopasswd`, owner `root:root`,
    mode 440. The list covers `apt, apt-get, make, install, mkdir,
    rmdir, cp, rm, dmesg, modprobe, insmod, rmmod, depmod,
    fpgautil, xmutil, devmem2, systemctl, tee`, plus the libt1
    test binaries and `visionsoc_main` (full text in § 4). The
    file has been seen **silently truncated to 0 bytes after a
    Kria reboot** — root cause unknown; suspected interaction with
    cloud-init / unattended-upgrades. **Recovery is now
    one-shot:** a verified canonical copy lives at
    `~/visionsoc-nopasswd.template` on the Kria (persistent,
    matches `/etc` byte-for-byte as of 2026-05-07 16:06). To
    re-install:
    `ssh kv260 'sudo install -m 440 -o root -g root
    ~/visionsoc-nopasswd.template
    /etc/sudoers.d/visionsoc-nopasswd && sudo visudo -c'` —
    `install` is on the allowlist so this stays passwordless. If
    the allowlist itself was wiped, this hop will need an
    interactive `sudo` password, but the template content is still
    durable.

  * **`udmabuf` is now reboot-persistent (2026-05-07).** The ikwzm
    `u-dma-buf` kernel module is installed at
    `/lib/modules/$(uname -r)/extra/u-dma-buf.ko`, and the
    `/etc/modules-load.d/u-dma-buf.conf` +
    `/etc/modprobe.d/u-dma-buf.conf` files are now in place
    (auto-load on boot with `udmabuf0/1/2=4194304`). No manual
    `modprobe` step needed after a reboot. Verify with
    `lsmod | grep u_dma_buf` and `ls /dev/udmabuf*`.

  * **Iteration model.** Build everything natively on the Kria
    over ssh from this dev host. The dev host **does not have
    rsync**; use `scp -r` for source pushes. The Kria's HTTPS git
    access to GitHub also misbehaves ("could not read Username");
    clone any needed repos on the dev host and `scp -r` to the
    Kria.

---

## 1. Done so far

| Step | What | When | Notes |
|------|------|------|-------|
| 1.1  | Verified `ssh kv260` reachable; identified Ubuntu 22.04 jammy (not 24.04) | 2026-05-07 | Updated `MEMORY.md` index entry. |
| 1.2  | Installed scoped passwordless sudoers at `/etc/sudoers.d/visionsoc-nopasswd` | 2026-05-07 | User ran the bootstrap one-liner once interactively; everything else passwordless from there. **Re-bootstrap may be needed if sudoers gets truncated** — see § 0 TL;DR. |
| 1.3  | `apt-get install` of devmem2, binutils-riscv64-linux-gnu, libdrm-dev, linux-headers-$(uname -r), dkms, build-essential | 2026-05-07 | rc 0. |
| 1.4  | Cloned `github.com/ikwzm/udmabuf` on dev host (`/tmp/udmabuf`), `scp -r`'d to `kv260:~/u-dma-buf/` | 2026-05-07 | HTTPS clone *from* the Kria failed; dev-host clone worked. |
| 1.5  | Built `u-dma-buf.ko` on the Kria (`make`) | 2026-05-07 | One harmless gcc-version warning. |
| 1.6  | Installed `.ko` to `/lib/modules/$(uname -r)/extra/`, ran `depmod -a` | 2026-05-07 | Manual cp + depmod — Makefile has no `install` target. |
| 1.7  | `modprobe u-dma-buf udmabuf0=4M udmabuf1=4M udmabuf2=4M` | 2026-05-07 | `lsmod` shows `u_dma_buf`; `/dev/udmabuf{0,1,2}` chr nodes (major 507) created. **Lost on every reboot** until § 1 persistence files written. |
| 1.8  | Verified `/sys/class/u-dma-buf/udmabuf*/phys_addr` populated | 2026-05-07 | udmabuf0=0x38300000, udmabuf1=0x38700000, udmabuf2=0x38b00000, each 4 MB. |
| 1.9  | Bitstream + dtbo staged into `/lib/firmware/xilinx/visionsoc/` | 2026-05-07 | scp from dev host `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260506-234437/system_top_wrapper.bit`. |
| 1.10 | Converted `.bit` → `.bit.bin` (raw bitstream, no Vivado header) | 2026-05-07 | `fpgautil` requires the `.bin` form; the kernel's FPGA manager driver doesn't parse Vivado's ASCII header. Conversion via `/tmp/bit2bin.py` on the dev host (script archived under `/tmp`; consider promoting into `fpga/system/` for reuse). |
| 1.11 | Fixed dtbo `firmware-name` to point at `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin` | 2026-05-07 | First load attempted at `/lib/firmware/system_top_wrapper.bit.bin` and got `Direct firmware load … failed with error -2`. dts now uses the namespaced path. |
| 1.12 | Disabled the camera capture chain in dts (`axi_iic_sensor`, `isp_csiss`, `isp_fb_wr_csi`, `isp_vcap_csi` all `status = "disabled"`) | 2026-05-07 | Probing the IIC panicked the kernel; see § 6.3. With these disabled the overlay applies cleanly. |
| 1.13 | `fpgautil` load succeeded; overlay applied; `/dev/uio4=t1, /dev/uio5=dma, /dev/uio6=bram` enumerated | 2026-05-07 ~15:31 | `Time taken to load BIN is 414.000000 Milli Seconds`. |
| 1.14 | devmem2 smoke probe: PERF_CYCLES_LO advances; CTRL R/W; subset of new wrapper regs throw bus errors | 2026-05-07 ~15:34 | See § 6.1 — partial blocker for libt1 tests. |
| 1.15 | udmabuf modprobe persistence files written | 2026-05-07 16:04 | `/etc/modules-load.d/u-dma-buf.conf` (10 B) + `/etc/modprobe.d/u-dma-buf.conf` (69 B) on Kria. Auto-loads u-dma-buf with `udmabuf0/1/2=4194304` on every boot. Verified with `lsmod \| grep u_dma_buf` and `ls /dev/udmabuf*`. |
| 1.16 | Sudoers canonical template staged at `~ubuntu/visionsoc-nopasswd.template` | 2026-05-07 16:06 | 430 B, byte-identical to live `/etc/sudoers.d/visionsoc-nopasswd`. Recovery is now `sudo install -m 440 -o root -g root ~/visionsoc-nopasswd.template /etc/sudoers.d/visionsoc-nopasswd && sudo visudo -c` from the home dir, no /tmp dependency. |

(Persistence files for u-dma-buf and the sudoers template were
both staged 2026-05-07; rows above are kept truthful so a fresh
agent can verify state with `ls -l` rather than re-execute.)

---

## 2. Pending (in order)

| # | Step | Blocker / status |
|---|------|-------------------|
| 2.1 | **Diagnose AXI4-Lite-only-aligned-reads bug** (§ 6.1.4). The smartconnect-rebuild path was tried (build 5e, `…-20260508-131546/`); bug persists. Bug also reproduces on HPM0_LPD path (mipi_csi2_rx) so it's not smartconnect_ctrl-specific. § 6.1.4 lists ruled-out hypotheses, completed cheap probes, and proposed next steps. | Open — Linux UIO/`/dev/mem` pgprot/userspace-load hypotheses are now mostly ruled out. Current rebuild candidate keeps the main change only: `MAXIGP0=128`, `MAXIGP2=128`, with SmartConnects in `LOW_AREA` to reduce routing pressure. BD-only validation for the earlier `PERFORMANCE` variant succeeded in `…-20260508-194241/`, but full build `…-20260508-200459/` was stopped on 2026-05-09 after severe route congestion. A LOW_AREA `-b -a` run `…-20260509-001228/` was also stopped during synth so the active run could restart without analysis flattening; current active run is `…-20260509-002137/` (`-b`, no `-a`). No new bitstream has been tested yet. |
| 2.2 | **Fix libt1 UIO lookup** to find T1/DMA by `/sys/class/uio/uio*/name` instead of hard-coded `/dev/uio0` / `/dev/uio1` (§ 6.2). | DONE 2026-05-07. Plus latent fixes in `t1_dma_wait` (poll SR.IDLE — s2mm IRQ doesn't reach UIO via generic-uio) and a new `t1_va_to_pa_range` for V4L2/DRM contiguity. See `driver_implementation_status.md`. |
| 2.3 | `scp -r vision_software/ kv260:~/`, `make` libt1 + visionsoc_main + kernels on the Kria | Pending hardware availability. Builds clean on dev host with the 2.2 fixes applied. |
| 2.4 | Run libt1 hw tests in order: `smoke → ddr_roundtrip → dma_loopback → port_grid_vadd → vert_lsu` | Pending 2.1 + 2.3. |
| 2.5 | **Resolve `axi_iic` panic** (§ 6.3) — re-enable the camera path nodes in dts (drop `status="disabled"`), re-load. | BD-level investigation. Independent of 2.1–2.4. |
| 2.6 | `media-ctl` + `v4l2-ctl` → AP1302 to UYVY 128×128 | Pending 2.5. |
| 2.7 | Run `visionsoc_main`, observe live HDMI output, confirm ≥30 fps | Pending 2.4, 2.5, 2.6. |

---

## 6. Open hardware blockers (in priority order)

### 6.1 Most T1 wrapper registers don't actually work

> **2026-05-07 update — see § 6.1.3 for triage results.** The actual
> picture is sharper than what's documented in this section's
> original tables, and the root cause has shifted twice as more data
> came in:
>
>   1. There is **no SIGBUS** under UIO mmap. The original "bus error"
>      claim came from `devmem2` (which doesn't set the same MMU
>      attributes) and is misleading.
>   2. Writes to the upper-half registers **do stick** (proven via
>      MEM_COUNT decrement observed through IRQ_STATUS).
>   3. The actual symptom is a **read-data lane bug**: reads at
>      addresses where `araddr[3:2] == 00` (i.e. byte-offsets 0x00,
>      0x10, 0x20, 0x30, 0x40, 0x50) return correct data; reads at all
>      other addresses return constant 0.
>   4. The same pattern shows up on the AXI DMA (offset 0x00 reads
>      `0x00010002`, offset 0x04 reads `0`). The DMA is a stock Xilinx
>      IP — it is not the bug. Therefore the bug lives in
>      `smartconnect_ctrl`, not in the T1 wrapper RTL.
>
> § 6.1.2's BD edit (NUM_CLKS=1 + STRATEGY=AUTOMATIC) is in fact the
> right rebuild path — just for a different reason than the previous
> session hypothesized. The previous-session diagnosis of "writes
> dropped via smartconnect CDC" is wrong; the actual mechanism is
> read-response lane mishandling, almost certainly caused by
> NUM_CLKS=2 + STRATEGY=LOW_AREA on a 32-bit-on-both-sides crossing.
>
> The rest of this section's diagnosis preceded the triage and is
> kept verbatim for context.

After running `/tmp/probe_t1.c` (a UIO-mmap walk of every word offset
and a write-readback test on a few key registers), the picture is:

| Offset | Reg               | Read via UIO | Write sticks? |
|--------|-------------------|--------------|----------------|
| 0x00   | CTRL              | `0x2` ✓ correct | n/a (RO bits) |
| 0x04   | INSTRUCTION       | `0x0` (probably never written) | not tested |
| 0x08–0x1C | rs1/rs2/vtype/vl/vstart/vcsr | all `0x0` | not tested |
| 0x3C   | IRQ_EN            | `0x0` after writing `0x7` | **BROKEN — write does not stick** |
| 0x40   | IRQ_STATUS        | `0x0` ✓ (no IRQs pending) | n/a |
| 0x44   | VERTICAL_MODE     | `0x0` after writing `0x1`/`0xff..ff`/`0xdeadbeef` | **BROKEN — write does not stick** |
| 0x48   | PERF_TAG (W only) | n/a | unverified |
| 0x4C   | PERF_DELTA        | `0x0` | n/a (RO) |
| 0x50   | PERF_CYCLES_LO    | `0xa756b202` ✓ ticks | n/a (RO) |
| 0x54   | PERF_CYCLES_HI    | `0x0` ✓ correct (counter < 2^32 yet) | n/a (RO) |

So **only CTRL.read and PERF_CYCLES_LO.read actually work**. Every
other readable register returns `0`. Every writable register tested
silently drops the write.

Two further oddities:

  * **Behaviour differs by mapping:** access via `/dev/mem`
    (`devmem2`) gives `Bus error (SIGBUS)` for the same offsets
    where access via UIO mmap silently returns `0`. That's
    consistent with a kernel-level difference — UIO maps the
    region with attributes that absorb a SLVERR, `/dev/mem` does
    not.
  * **No clean low-bit pattern.** I initially suspected "all odd
    rd_addr fail" because 0x3C / 0x44 / 0x4C / 0x54 are all odd
    word indices. But under the UIO probe, even-indexed reads
    also return `0` (e.g. 0x10 reg_vtype, 0x18 reg_vstart). So
    the failure is more general than odd-index.

**Implication for libt1:** the `smoke` test reports
`cycles advanced by 1007275 in 10 ms` (PERF path works) and then
`FAIL: VERTICAL_MODE did not round-trip as 1` (write doesn't
stick → readback 0). Any test that issues an instruction —
i.e. anything beyond `smoke`'s perf check — needs to write
`reg_instruction` at 0x04, and our test shows writes to
`reg_irq_en` (0x3C) silently fail, so writes to `reg_instruction`
will too. **All `t1_issue`-based tests are blocked until this
is fixed on the FPGA side.**

**Diagnosis hypotheses (Vivado-side investigation needed):**

  * **Synthesis pruned the `reg_*` array.** **RULED OUT
    2026-05-07.** Walked the post-synth netlist
    (`fpga/build/t1_mudkip2d128small1bram1chain2lanescale_fpga_system/.../system_top_t1_top_0_sim_netlist.v`):
    - `reg_irq_en[2:0]` exists as three FDCEs gated by a LUT6
      with `INIT=64'h0000000008000000` whose inputs decode
      `wr_addr=6'h0F` (= 0x3C) ✓.
    - `reg_vertical_mode_reg` exists as a single FDCE; its
      next-state LUT (`reg_vertical_mode_i_1` INIT
      `64'hFFBFFFFF00800000`) decodes "set when wr_addr=0x11
      and w_data_reg[0]=1" and "clear when wr_addr=0x11 and
      w_data_reg[0]=0" — both consistent with the source.
    - `s_axi_rdata_reg[31:0]` exists as 32 FDCEs with
      `CE=p_19_in` (the arvalid&arready handshake) and
      `CLR=t1_reset` — same flop type and reset signal as
      `perf_cycles_reg[*]`, which works.
    - No `removed`, `pruned`, or `tied to constant` strings
      hit in `vivado_synth.log` for any wrapper signal.
    Conclusion: the wrapper IP is correctly synthesised. Bug
    is upstream of `t1_top/s_axi_ctrl`.
  * **Smartconnect_ctrl CDC bug — *primary hypothesis*.** This
    SmartConnect was upgraded to `NUM_CLKS=2` (60 MHz aclk +
    300 MHz aclk1) so it could reach `v_frmbuf_wr/s_axi_CTRL`
    at 300 MHz, and additionally has `STRATEGY=LOW_AREA`
    (`system_top.tcl:470-472`) which strips redundant
    pipelining/ID-renaming. With the 60↔300 MHz CDC and the
    area-optimised arbiter combined, M00→T1 transactions on
    the 60 MHz side may be partially corrupted at certain
    addresses. The same hypothesis explains the
    `axi_iic` panic in § 6.3 — both paths arrive via
    `smartconnect_ctrl`.
  * **Wrapper FSM aw_addr_reg width.** **RULED OUT.**
    `t1_fpga_top.v` (build artefact) declares
    `s_axi_ctrl_awaddr [7:0]` and the wrapper's
    `aw_addr_reg` is `[ADDR_WIDTH-1:0] = [7:0]` in the netlist.
    Address decoding is bit-perfect.
  * **Probe mmap reading wrong page.** Ruled out — the dts
    `reg = <0x0 0xa0000000 0x0 0x10000>` is correct, smoke
    test reads 0x50 successfully through the same mapping.

The next session should test the smartconnect hypothesis by
either (a) attaching a Vivado ILA to `smartconnect_ctrl/M00_AXI`
to see what AW/W/AR transactions actually reach the wrapper, or
(b) rebuilding with `smartconnect_ctrl/CONFIG.NUM_CLKS=1` and
`STRATEGY=AUTOMATIC`. To get NUM_CLKS=1 we also need to either
disable v_frmbuf_wr/CTRL access or split it onto its own
smartconnect — see § 6.4 below for the proposed BD edit.

The wrapper source (`fpga/wrapper/t1_axi_lite_wrapper.sv`) handles
all six in the `case (rd_addr)` / `case (wr_addr)` blocks with
correct constants (`6'h11` for 0x44, `6'h13` for 0x4C, `6'h14` for
0x50, `6'h15` for 0x54). The read FSM has `default: s_axi_rdata <= '0;`
and `s_axi_rresp = 2'b00; // OKAY`, so an unmatched address should
return zero with OKAY — there is **no SLVERR path in the wrapper
source**. Yet hardware shows SIGBUS for some addresses and not
others.

The `Bus error` from devmem2 is a SIGBUS, which on Linux ARM64
maps to either `SIGBUS_ADRERR` (no slave at address — DECERR) or
`SIGBUS_OBJERR` (slave returned SLVERR, or transaction timed out).
Since reads at 0x50 work, the wrapper *does* have an aperture
covering this range; the question is why specific offsets within
that aperture misbehave.

**Hypotheses to investigate next session:**

  * **Synthesis / place-and-route corruption.** The pattern (0x44
    fails, 0x48 OK, 0x4C fails, 0x50 OK) is suspicious — it could
    be a pruned signal in the case statement after some
    optimisation pass. Check `fpga/build/.../runs/synth_1/` opt
    summary for any "removed" or "tied to constant" warnings on
    `s_axi_rdata` / `reg_vertical_mode` / `perf_delta`. Also pull
    `vivado_synth.log` and search for these signal names.
  * **Clock domain crossing on smartconnect_ctrl.** `smartconnect_ctrl`
    was reconfigured to `NUM_CLKS=2` (60 MHz aclk + 300 MHz aclk1)
    so it could reach `v_frmbuf_wr/s_axi_CTRL` at 300 MHz. The T1
    wrapper's slave is on the 60 MHz clk. If the smartconnect's
    internal CDC FIFOs misroute or stall on certain low-byte
    address patterns, partial corruption could result. Try a
    minimal BD where `smartconnect_ctrl` is `NUM_CLKS=1` (camera
    path is disabled anyway in current dtbo).
  * **Wrapper aw_addr_reg width.** `ADDR_WIDTH=8` is set, port is
    8-bit, `aw_addr_reg` is `[ADDR_WIDTH-1:0]`. `wr_addr =
    aw_addr_reg[7:2]`. All correct in source. Worth confirming the
    elaborated netlist (`vivado -mode batch -source ...`) actually
    declares an 8-bit port and not a stale 7-bit one from a cached
    elaboration.
  * **Reset.** `reg_vertical_mode` is reset to 0 in the always_ff
    block. Could the reset be held active on this register? If
    `aresetn` is held low for some sub-domain, writes are
    swallowed but reads at 0x50 succeed because perf_cycles is in
    a different block. (Only one `always_ff` cluster handles all
    six though, so this seems unlikely.)

**Quick triage idea:** read 0x40 (IRQ_STATUS, 6'h10 — pre-existed
before the wrapper extension). If 0x40 reads OK but 0x44 fails,
the issue is *specifically* in the patch's added cases. If 0x40
also fails, the issue is broader (every odd-byte high address?).

### 6.1.1 Cheap on-Kria triage to run BEFORE rebuilding

Before launching a multi-hour Vivado rebuild, run these three
tests on the Kria with the existing bitstream loaded. They
distinguish "writes dropped" from "reads broken" without
hardware debug:

  1. **CTRL.W1S smoke (cheapest).** Write `0x1` to `0xa0000000`
     (CTRL bit 0 = issue_start, W1S). Read back `0xa0000000`.
     Expected if writes work: `bit[2]=1` (issue_pending). If
     read still returns `0x2`, writes to *low* offsets are
     also dropped — extends the failure beyond the
     "added in this patch" set.
  2. **PERF_TAG round-trip via PERF_DELTA.** Write `0x42` to
     `0xa0000048` (PERF_TAG W only — nonzero=START), busy-wait
     ~1k cycles, write `0x00` to `0xa0000048` (=STOP), read
     `0xa000004C` (PERF_DELTA). If non-zero, writes to 0x48
     work AND reads of 0x4C work — narrows the bug to a
     specific subset rather than the whole upper half.
  3. **IRQ_EN write side-effect.** Write `0x4` (T1_IRQ_MEM)
     to `0xa000003C`. Issue any LSU instruction, observe
     whether `read(uio_t1_fd)` returns within timeout. If it
     does, IRQ_EN write *did* stick even though readback shows
     0 — the bug is then ONLY in the read mux for those
     specific addresses.

These three tests are now bundled in
`vision_software/libt1/test/triage_t1.c` (built by the libt1
Makefile alongside the other tests). On the Kria, run:

```bash
cd ~/visionsoc/vision_software/libt1 && make test/triage_t1
sudo ./test/triage_t1            # default UIO path /dev/uio4
# sudo ./test/triage_t1 /dev/uio4  # or override
```

The probe is self-contained (no libt1 dependency, just mmap on the
T1 UIO), so a libt1 regression cannot mask a bus-fabric symptom.
Tests 1 and 2 are pure register pokes; test 3 writes IRQ_EN and
reads it back. To complete the *write side-effect* half of test 3,
run `test/vert_lsu` after `test/triage_t1` — if the LSU IRQ fires
even though IRQ_EN readback is 0, the write stuck and the bug is
purely in the read mux for those addresses.

Result feeds directly into the "smartconnect vs wrapper" split:
any write side-effect appearing on hardware contradicts the
"writes dropped" interpretation and points specifically at the
read path (probably the s_axi_rdata mux LUT chain in the
smartconnect's M00 read response routing).

### 6.1.2 Proposed BD edit if rebuild becomes the path

If § 6.1.1 confirms the smartconnect hypothesis, the minimal
BD change to validate is:

  * Set `smartconnect_ctrl` back to `NUM_CLKS=1`,
    `STRATEGY=AUTOMATIC`.
  * Drop `M03_AXI` (v_frmbuf_wr/s_axi_CTRL) from
    `smartconnect_ctrl` and either:
      - move v_frmbuf_wr/s_axi_CTRL onto its own dedicated
        smartconnect at 60 MHz (run frmbuf control at 60 MHz —
        v_frmbuf_wr's HLS expects a single clock for control
        + data, so this means downclocking the data path too
        unless we add a separate ap_clk → s_axi_CTRL CDC), OR
      - leave `M03_AXI` disconnected for now (camera path is
        already disabled in dts § 6.3).
  * Do NOT touch the rest of the BD — DMA, BRAM, T1's
    issue/retire path are independent of this hypothesis.

The existing build dir
`mudkip2d128small1bram1chain2lanescale_fpga-20260506-234437/`
must be preserved (memory rule: never delete past FPGA
artefacts) — copy `system_top.tcl` to a new build dir before
editing.

> **Superseded 2026-05-07.** § 6.1.3 ran the triage and disproved
> the smartconnect hypothesis. Do not rebuild the BD. The fix
> belongs in the wrapper RTL, not the BD.

### 6.1.3 Triage results (2026-05-07) — fix is in the wrapper RTL

`test/triage_t1` was run on the Kria against the existing
bitstream. Five sub-tests in one binary; relevant outputs:

```
[0] PERF_CYCLES advance check         PASS (wrapper is clocked)
[1] CTRL.W1S smoke                    PARTIAL (write reached FSM —
                                      bit[1] issue_ready cleared
                                      after W1S)
[2] PERF_TAG round-trip via PERF_DELTA FAIL (PERF_DELTA reads 0)
[2b] VERTICAL_MODE round-trip         FAIL (readback always 0)
[4] MEM_COUNT decrement via IRQ_STATUS PASS — and this is the
                                       smoking gun
[3] IRQ_EN write/readback             readback 0 (matches the
                                      stuck-readback pattern)
```

Initial register snapshot showed `IRQ_STATUS = 0x4` while
`MEM_COUNT = 0`. That is **internally inconsistent**: the wrapper
builds `irq_pending = { mem_count != 0, !csr_fifo_empty,
!rd_fifo_empty }`, so bit[2]=1 in IRQ_STATUS means `mem_count` is
genuinely non-zero in hardware — but the readback at offset 0x38
reports 0. So the read at 0x38 is wrong.

Test [4] then wrote `1` to MEM_COUNT (0x38) four times. Each write
to that offset decrements `mem_count`. After four writes,
IRQ_STATUS bit[2] cleared (read back as `0x0`). Conclusion:

  * **Writes to upper-half offsets DO stick** (mem_count went from
    >=4 down to 0 via four W1 decrements at 0x38).
  * **Reads at 0x38, 0x3C, 0x44, 0x4C are stuck at constant 0**,
    even when the underlying register holds a non-zero value
    (proven by IRQ_STATUS at 0x40, which reads correctly, observing
    the change driven by writes to 0x38).
  * **Reads at 0x00 (CTRL), 0x40 (IRQ_STATUS), 0x50/0x54
    (PERF_CYCLES_LO/HI) work correctly.**
  * **No SIGBUS** anywhere under UIO mmap. The earlier "Bus error"
    observation was via `devmem2` against `/dev/mem`, which doesn't
    set up the same MMU attributes; read it as a `devmem2` artefact,
    not a fabric DECERR/SLVERR.

What this means for the rebuild plan:

  * § 6.1.2's smartconnect hypothesis is falsified. The
    smartconnect is correctly delivering writes to the wrapper —
    we just observed a write side-effect propagate through the
    wrapper's internal state (mem_count → irq_pending → IRQ_STATUS
    readback). If the smartconnect were dropping AW/W on those
    addresses, the decrement could not have happened.
  * The bug is in `fpga/wrapper/t1_axi_lite_wrapper.sv`'s read
    multiplexer. The source for it (lines 404-427) has explicit
    cases for `6'h0E` (0x38), `6'h0F` (0x3C), `6'h11` (0x44),
    `6'h13` (0x4C) — yet hardware reports them all stuck at the
    `default` value 0. Working cases all happen to be either
    combinational (CTRL/issue_pending+ready, IRQ_STATUS/irq_pending)
    or unconditionally-updated registers (perf_cycles always
    increments). Broken cases are all conditionally-updated
    registers (mem_count, reg_irq_en, reg_vertical_mode,
    perf_delta).
  * Likely root cause class: synthesis is generating an incorrect
    LUT cone for those rdata mux paths, or there is a placement /
    timing issue specific to those bits' fanout. The
    previous-session netlist walk confirmed the *flops* exist with
    correct CE/CLR — but did not enumerate which read-decode mux
    paths actually feed `s_axi_rdata_reg`. That is the next
    investigation step.

Continued probing then localized the bug further:

  * **Lower-half round-trip test (0x04..0x1C):** wrote distinct
    patterns to INSTRUCTION (0x04), RS1_DATA (0x08), RS2_DATA (0x0C),
    VTYPE (0x10), VL (0x14), VSTART (0x18), VCSR (0x1C). Only
    **VTYPE (0x10) round-tripped**; the other six all read back 0.
  * **DMA cross-check:** read DMA MM2S_CR (0x00) and MM2S_SR (0x04)
    via uio5. After a soft reset, MM2S_CR reads 0x00010002 ✓ but
    MM2S_SR reads 0. Same for S2MM_CR/S2MM_SR at 0x30/0x34. The DMA
    is a stock Xilinx IP — the address pattern matches the T1
    wrapper exactly.

The fully-reduced pattern: **reads at addresses where
`araddr[3:2] == 00` (byte-offsets 0x00, 0x10, 0x20, 0x30, 0x40,
0x50) work; all other reads return 0**. This holds across both T1
wrapper (M00) and DMA (M01) of `smartconnect_ctrl`.

That's the fingerprint of read-data lane corruption — the kind of
thing that happens when the smartconnect's internal datapath is
wider than 32-bit and the M-side narrowing demuxer mishandles
`araddr[3:2]`. The shared element across all observed cases is
`smartconnect_ctrl` with `NUM_CLKS=2` + `STRATEGY=LOW_AREA`.

**Rebuild plan staged 2026-05-07** in `fpga/system/system_top.tcl`:

  * `smartconnect_ctrl`: `NUM_MI 4 → 3`, `NUM_CLKS 2 → 1`, dropped
    M03 (v_frmbuf_wr/s_axi_CTRL was the only 300 MHz client and the
    reason NUM_CLKS=2 existed at all).
  * Removed `smartconnect_ctrl` from the `STRATEGY=LOW_AREA` foreach
    loop — it now uses default (AUTOMATIC).
  * Disconnected `v_frmbuf_wr/s_axi_CTRL` (was M03). Camera path is
    dts-disabled in this bringup anyway (§ 6.3); when it's re-enabled,
    either run frmbuf at 60 MHz or give it a dedicated smartconnect.
  * Dropped frmbuf_wr IRQ from `irq_concat` (was In5; NUM_PORTS now 5).
  * Removed the `v_frmbuf_wr/s_axi_CTRL` address-map assignment
    (was 0xA0020000).

Both potential failure modes (NUM_CLKS=2 CDC quirk, LOW_AREA arbiter
stripping) are addressed in this single rebuild. If `v_frmbuf_wr`
unconnected slave-AXI port causes a `validate_bd_design` DRC error,
the fallback is to either (a) remove `v_frmbuf_wr` + `smartconnect_video`
entirely from the BD, or (b) add a 1-master-1-slave dedicated CDC
smartconnect for frmbuf_CTRL.

Build command (from repo root, see `fyp_doc/fpga_build_status.md`
§ 3 for full sequence):

```
bash fpga/system/build_fpga.sh -c mudkip2d128small1bram1chain2lanescale_fpga -b
```

This runs BD generation + synth + impl + bitstream in one go
(~1–2 hours wall time). Output lands in
`fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-<timestamp>/`.
Verify pre-launch:

```
diff <(grep -A2 'NUM_CLKS' fpga/system/system_top.tcl) /dev/null
# Should show: smartconnect_ctrl … NUM_CLKS {1}
# Should NOT show: NUM_CLKS {2}, aclk1, M03_AXI v_frmbuf_wr
```

Existing build dir
`mudkip2d128small1bram1chain2lanescale_fpga-20260506-234437/`
remains the reference bitstream until this rebuild lands.

### 6.1.4 Bug NOT in smartconnect_ctrl — handoff for next agent (2026-05-08)

**Status: still unresolved; previous diagnosis chain (§§ 6.1.1–6.1.3) is
PARTIALLY WRONG; do not act on it without re-validating.**

#### What the bug actually is (definitively confirmed)

Reads of any AXI4-Lite peripheral on this Zynq US+ design fail when
`araddr[3:0] != 0b0000` — i.e. only 16-byte-aligned reads succeed.
Writes work at all offsets. Confirmed on:

  * T1 wrapper (`0xA0000000`) — via `/dev/mem` (devmem2): reads at
    `0xA0000044`, `0xA000004C` etc. → `Bus error`; UIO and the
    later signal-guarded `/dev/mem` probe return 0 at the same
    offsets.
  * AXI DMA control (`0xA0010000`) — via devmem2: reads at
    `0xA0010004`, `0xA0010034` → SIGBUS.
  * mipi_csi2_rx CSR (`0x80000000`, on the **LPD** master not FPD) —
    via devmem2: reads at `0x80000004`, `0x80000014`, `0x80000044`
    → SIGBUS.

So: **the bug affects every AXI4-Lite slave in the design, on BOTH
HPM0_FPD and HPM0_LPD master ports.** It is not specific to
`smartconnect_ctrl`. Any explanation that points only at
`smartconnect_ctrl` (NUM_CLKS, STRATEGY, NUM_MI, M03 wiring) is
falsified by the LPD test result.

**2026-05-08 follow-up correction:** `devmem2` SIGBUS is not a reliable
indicator of an AXI SLVERR here. On this aarch64 userspace, `devmem2 ...
w` uses an `unsigned long` access, i.e. an 8-byte load; addresses that
are 4 mod 8 can SIGBUS from the unaligned 64-bit MMIO load. A new
signal-guarded userspace probe using explicit byte/halfword/word loads
and barriers reads the same non-16-byte offsets without a signal and
gets zero, matching UIO. The underlying hardware symptom is therefore
best described as **read-data lane loss/zeroing for lanes 1-3 of each
16-byte group**, not proven slave-error response.

The Linux mapping-attribute hypothesis has now been tested more strongly:

  * UIO and `/dev/mem` mappings show the same VMA flags
    (`pf io de dd`) and explicit aarch64 `ldr` + `dsb/isb` still
    returns zero at non-16-byte-aligned word offsets.
  * A kernel module using `ioremap_np()`/`readl()` sees the same pattern:
    DMA `0xA0010000/0x30` read `0x00010002`, while DMA `0x04/0x34` read
    `0`; T1 `0x00/0x10/0x50` read valid values while neighboring words
    read `0`; CSI-2 `0x80000000` reads `1` while `0x04+` read `0`.

That rules out UIO-specific pgprot and userspace compiler/load-shape
effects as the primary cause.

#### What was tested

| Test | Build / config | Result |
|---|---|---|
| Baseline bitstream `5a` (NUM_CLKS=2, LOW_AREA, M03 connected) | `…fpga-20260506-234437/` | bug present |
| Bitstream `5d` (NUM_CLKS=1, AUTOMATIC, M03 detached) | `…fpga-20260507-215432/` (failed routing 4243 overlaps) | n/a — never tested on hw |
| Bitstream `5e` (slimmed camera + same `smartconnect_ctrl` revert as 5d) | `…fpga-20260508-131546/` | bug present (identical address pattern) |
| `MEM_COUNT` decrement via IRQ_STATUS observation | 5e on hw | confirms WRITES work everywhere |
| Read T1 / DMA via devmem2 | 5e on hw | SIGBUS at non-aligned |
| Read mipi_csi2_rx via devmem2 (HPM0_LPD path) | 5e on hw | SIGBUS at non-aligned |
| Smartconnect_ctrl XCI inspection | 5d build artefacts | confirms internal switch is 32-bit (`MAX_PAYLD_BYTES=4`) |
| PS GP0 master config | 5d build artefacts | confirms `PSU__MAXIGP0__DATA_WIDTH=32` (and same for `MAXIGP2`) |
| Wrapper RTL `s_axi_rresp` | source | hardcoded to `2'b00` (OKAY); SLVERR is injected externally |

#### What was NOT tested (open questions)

  * Whether the same bug appears on a **non-AXI4-Lite slave** reachable
    through smartconnect — e.g. an AXI4-full register peripheral. This
    would distinguish "all AXI4-Lite slaves are broken" from "the
    smartconnect→Lite path is broken in this design specifically".
  * Whether the bug appears at the **smartconnect M-side** (i.e. on the
    cable feeding the wrapper) or only at the **PS-master S-side**.
    Would need a Vivado ILA on `smartconnect_ctrl/M00_AXI` to see what
    `arvalid` / `araddr` / `arlen` / `arsize` / `arburst` actually look
    like during a faulting transaction. **This is the single most
    valuable test that hasn't been run.**
  * Whether the kernel's UIO / `/dev/mem` mmap on this Ubuntu
    22.04 / 5.15 kernel uses **Device-nGnRnE** (strict, single-shot
    transactions per CPU access) or **Normal Non-Cacheable** (allows
    speculative / wider transactions). **Mostly tested 2026-05-08:**
    userspace barriers/access-width changes do not alter the pattern,
    and kernel-space `ioremap_np()`/`readl()` sees the same zeroed lanes.
  * Whether bitstream `5e`'s reduced-AXIMM-width changes (v_frmbuf_wr
    `AXIMM_DATA_WIDTH 128→64`) caused a regression elsewhere — i.e.
    whether `5e` made the bug worse compared to `5a`. Unlikely (they
    affect HP1 path, not control plane), but the comparison was never
    made directly.

#### Hypothesis (Gemini-suggested, partly disproven)

Gemini proposed two fixes:

1. **Fix 1: Move all AXI4-Lite slaves to HPM0_LPD** — disproven by the
   `0x80000004` SIGBUS observation. mipi_csi2_rx CSR is *already* on
   HPM0_LPD and exhibits the bug.

2. **Fix 2: Set `PSU__MAXIGP0__DATA_WIDTH=128`** and let the smartconnect
   handle 128→32 downsizing. Premise: "Vivado's SmartConnect IP… is
   significantly more robust at handling unaligned 128-bit to 32-bit
   AXI4-Lite downsizes than the internal PS downsizer." Plausible but
   unverified. **Not yet attempted.** Cheap to attempt — single-line BD
   change.

The shared premise was "the PS internal 32-bit downsizer is buggy and
issues 4-beat unaligned bursts to AXI4-Lite slaves, which the
protocol converter then rejects." This explains the data symmetry
(writes pass via WSTRB, reads can't) and the alignment pattern
(only 16-byte-aligned bursts are legal). It does NOT yet explain
why the same symptom appears on LPD where the master is configured
exactly the same way (`MAXIGP2_DATA_WIDTH=32`).

If the PS downsizer is the culprit on both FPD and LPD, then **Fix 2
applied to both ports** (set both `MAXIGP0` and `MAXIGP2` to 128-bit)
might be the actual fix. Untried.

Alternative hypotheses worth investigating:

  * **CPU-side cache/interconnect issue.** ARM Cortex-A53 with
    Device-nGnRnE memory should issue single-beat transactions; with
    Normal-NC (or wrong UIO driver flags) it can speculate or burst.
    A 16-byte cache-line-fetch behavior would match the symptom
    exactly. The bug then is in either the kernel mmap attributes or
    the CCI's translation. Test: write a small probe that uses
    explicit `dmb` / `dsb` barriers and inline-asm `ldr` to force
    strict device semantics, see if the bug persists.
  * **Smartconnect IP bug.** Vivado 2025.2 smartconnect MIGHT have a
    regression for AXI4 (full) → AXI4-Lite conversion specifically
    when the master issues partial bursts. Check Xilinx AR (Answer
    Records) for AR# entries about smartconnect 2025.2 + AXI4-Lite.
  * **Per-IP `arready` quirk.** Less likely since both T1 wrapper
    and stock Xilinx DMA exhibit the bug.

#### What I'd do next

In order of cost:

1. **Verify Linux memory-mapping attributes** (zero-cost, no rebuild).
   **DONE 2026-05-08.** `axi_lite_read_probe` and
   `axi_lite_kernel_probe` both reproduce the same pattern, so this is
   not a UIO/userspace pgprot bug.
2. **Try Fix 2** (single-line BD change: `PSU__MAXIGP0__DATA_WIDTH 32→128`,
   and possibly `PSU__MAXIGP2__DATA_WIDTH 32→128`). Rebuild, retest.
   ~3 h cost. Falsifies cleanly if it doesn't fix the bug.
   **Validated before rebuild 2026-05-08:** Vivado's generated PS IP XML
   uses the same choice list for `MAXIGP0` and `MAXIGP2`, and legal
   values are `{128, 64, 32}`. Applying the change to **both** ports is
   valid and matches the observation that both FPD and LPD control paths
   fail.
   **2026-05-09 build-strategy update:** the first full build attempt
   (`…-20260508-200459/`) also forced PS-to-AXI-Lite SmartConnects to
   `PERFORMANCE`; synthesis and placement completed, but route reported
   level-5 short congestion and "congestion is preventing the router from
   routing all nets", so the run was terminated before bitstream. Retry
   with `MAXIGP0/MAXIGP2=128` while keeping SmartConnects in `LOW_AREA`.
   A first LOW_AREA retry with `-a` (`…-20260509-001228/`) was stopped
   during synthesis; current retry is `…-20260509-002137/` with `-b`
   only, to let Vivado use its default synthesis hierarchy/flattening.
3. **Vivado ILA on `smartconnect_ctrl/M00_AXI`.** Captures the actual
   AR transaction shape (arlen, arburst, arsize) at the wrapper's
   doorstep during a faulting read. Definitively identifies whether
   the converter or the wrapper is at fault. ~1 day to wire and run
   (BD edit + new bitstream + scope-driver setup).
4. **Bypass the smartconnect entirely.** Connect PS GP0 directly to a
   single AXI4-Lite slave via an `axi_protocol_converter` IP (no
   smartconnect). If the bug disappears, smartconnect is at fault. If
   it persists, the issue is at the PS master.

#### Key files / artefacts

  * Triage probe: `vision_software/libt1/test/triage_t1.c` — ready to
    run on the Kria, exhibits the bug deterministically.
  * Userspace mapping/access probe:
    `vision_software/libt1/test/axi_lite_read_probe.c`.
  * Kernel-side mapping/access probe:
    `vision_software/libt1/test/axi_lite_kernel_probe/`.
  * Wrapper RTL: `fpga/wrapper/t1_axi_lite_wrapper.sv` — confirmed
    correct against post-synth netlist (§ 6.1, "RULED OUT" entries).
  * BD source: `fpga/system/system_top.tcl` — currently has the 5e
    edits applied (NUM_CLKS=1, AUTOMATIC, slimmed camera).
  * Failed routing build (5d): `fpga/build/…fpga-20260507-215432/`.
  * Successful 5e bitstream loaded on Kria: `…fpga-20260508-131546/`.
  * dmesg output of SIGBUS: see § 6.1.3 for capture commands.

### 6.1.5 Resolution (2026-05-09) — Fix 2 confirmed; what to know to avoid recurrence

**Root cause (definitive):** the PS-internal full-power-domain (FPD)
NIC-400 interconnect is 128-bit-wide natively. When the PS-PL master
port `M_AXI_HPM0_FPD` is configured at a *narrower* width
(`PSU__MAXIGP0__DATA_WIDTH < 128`), the PS inserts an internal
downsize bridge between the FPD switch and the master output. For
each CPU MMIO transaction reaching that bridge:

  * The CPU's 32-bit `ldr` / `str` enters the FPD interconnect as a
    128-bit transaction (the FPD switch's native width).
  * The bridge emits a multi-beat AXI4 burst at the master port —
    typically `arsize=2 (4 bytes/beat), arlen=3 (4 beats)` covering
    a full 16-byte aligned slot — to deliver the same data on the
    narrower bus.
  * For **writes**, this is symptom-free because AXI WSTRB lets the
    bridge drive `wstrb=0` on the beats whose lanes weren't
    requested by the CPU; SmartConnect's Low-Area Mode AXI4 →
    AXI4-Lite converter honors WSTRB by suppressing zero-strobe
    beats. So writes always landed correctly at every byte offset.
  * For **reads**, AXI has no `rstrb` equivalent: every beat must
    return data. SmartConnect (per PG247 §"Conversion to
    AXI4-Lite") splits the 4-beat burst into 4 single-beat
    AXI4-Lite reads at successive 4-byte offsets within the
    16-byte slot, the slave responds correctly to all 4, and
    SmartConnect aggregates the 4×32-bit responses back into the
    original 4-beat burst response on the master side. Then the
    PS-side downsize bridge needs to **repack** that 4-beat
    response back to 128 bits and route the requested 32-bit
    sub-word to the originating CPU ldr.
    **It is at this PS-side repacking stage that the bug lives** —
    only the lane corresponding to address `[3:0] == 0000` is
    placed correctly; lanes 1-3 of the 128-bit reassembly are
    zero-filled. So the CPU sees correct data only when its
    original byte offset was 16-byte-aligned, and zero otherwise.
    This is consistent with the symptom, identical on both
    HPM0_FPD and HPM0_LPD paths, and identical across SmartConnect
    NUM_CLKS / STRATEGY variants — because none of those affect
    the PS-side bridge.

The downsize bridge is NOT used when the master port is configured
at the FPD's native 128-bit width — the FPD's 128-bit transaction
flows directly to the master output, and the CPU's 32-bit `ldr`
emerges as a single `arsize=2, arlen=0` 32-bit AXI4 transaction
(per UG1085 §"NIC-400 Master Behavior"; see also PG247 §3.4
"Conversion to AXI4-Lite"). SmartConnect passes it through to the
AXI4-Lite slave, which responds with one `rdata` beat. The PS sees
that one beat at the master port, no repacking needed, no lane bug.

**The fix:** in `fpga/system/system_top.tcl`, the PS configuration
must set:

```tcl
CONFIG.PSU__MAXIGP0__DATA_WIDTH     {128} \
```

(and `PSU__MAXIGP2__DATA_WIDTH={128}` if `M_AXI_HPM0_LPD` is also
in use for AXI4-Lite slaves — the LPD has the same internal
architecture and the same downsize bug.)

The cost is purely in PL-side LUTs: SmartConnect now needs an
internal 128→32 size-converter at the S00 ingress instead of a
32-bit-throughout fabric path. In Low-Area Mode this is
implemented compactly via `sc_si_converter` and adds a few
hundred LUTs at most.

**Critical pitfall to avoid in future BD edits:** *don't*
"optimize" `PSU__MAXIGP0__DATA_WIDTH` back to 32 to save PL LUTs.
The XCI / Tcl looks like a free win — narrower master, smaller
SmartConnect — but the PS-side bridge bug means **any AXI4-Lite
slave on that master will have its non-aligned reads silently
zeroed.** Writes will continue to work (WSTRB-protected), so
unit tests that only exercise the write path may pass while
control-plane reads silently corrupt. Both `t1_axi_lite_wrapper`
*and* the stock Xilinx `axi_dma` control + Xilinx `mipi_csi2_rx`
CSR exhibited the bug — it is *not* a wrapper-RTL issue,
not a SmartConnect config issue, and not in any of the AXI4-Lite
slaves' RTL.

**Diagnostic chain (for posterity):** the symptom looked like a
wrapper read-mux bug (§ 6.1.3) and then like a SmartConnect_ctrl
bug (§ 6.1.4 first hypothesis). It was neither. The decisive
data was that the same `araddr[3:0] != 0` symptom reproduces on
the LPD path (which uses an entirely separate SmartConnect
instance), and that the SmartConnect XCI files showed
`MAX_PAYLD_BYTES=4` end-to-end (32-bit fabric in LAM, no
internal width conversion that could be mis-mapping lanes). That
left the PS-side downsize bridge as the only shared upstream
component — and Gemini's "Fix 2: set MAXIGP0_DATA_WIDTH=128"
suggestion landed exactly on the right knob, even though its
*explanation* of why ("SmartConnect's downsizer is more
robust") was off-target.

**How to validate the fix on a future build:** flash the
bitstream, run `vision_software/libt1/test/triage_t1`. With Fix 2
applied, all eight tests pass (initial register snapshot shows
non-zero values at non-aligned offsets; CTRL.W1S, MEM_COUNT
decrement via IRQ_STATUS, lower-half register round-trip all
PASS). The triage probe is bit-for-bit reproducible on hardware;
if any read at offset `0x04`/`0x08`/`0x0C`/`0x14`/etc returns
`0` after a write of a non-zero value, **the PS DATA_WIDTH was
reverted to 32 somewhere** — check `system_top.tcl` first, then
the .xci files in the synthesis project.

### 6.2 libt1 UIO node ordering mismatch — RESOLVED 2026-05-07

`vision_software/libt1/libt1.c` previously hard-coded:

```c
#define T1_UIO_PATH  "/dev/uio0"
#define DMA_UIO_PATH "/dev/uio1"
```

After our overlay loads, the actual mapping is `uio4=t1`,
`uio5=dma`, `uio6=bram` (because `/dev/uio0..3` are PS-base
`axi-pmon` nodes that always exist).

**Fix:** added `find_uio_by_name()` and `resolve_uio_path()` to
`libt1.c`. `t1_init` now resolves T1 / DMA paths in this order:
1. env-var override (`T1_UIO_PATH` / `DMA_UIO_PATH`),
2. binding-name lookup (`"t1"` / `"dma"` against
   `/sys/class/uio/uio*/name`),
3. fallback to `/dev/uio0` / `/dev/uio1` (legacy hosts).

Two related driver-side issues were also fixed in the same pass —
both surfaced when reading the BD wiring more carefully:

* **`t1_dma_wait` polls SR.IDLE.** `irq_concat` in
  `system_top.tcl:264-267` puts MM2S and S2MM IRQs on separate
  GIC SPIs (89/90/91); `generic-uio` only registers IRQ index 0
  (mm2s), so s2mm completion never reaches `/dev/uio_dma`.
  `t1_dma_wait` would hang the full timeout for an s2mm-only
  sync, and would race in `dma_loopback.c` (return on the first
  mm2s IRQ even if s2mm hadn't finished). It now polls
  `DMA_REG_MM2S_SR` and `DMA_REG_S2MM_SR` until both have
  `DMA_SR_IDLE` set, which is reliable for both channels.
* **`t1_va_to_pa_range(va, size)`** added; `camera.c` and
  `display.c` updated to use it. The previous `t1_va_to_pa(va)`
  returned only the first page's PA, which silently corrupted
  pages 2..N for multi-page V4L2 / DRM mappings. The range variant
  walks `/proc/self/pagemap` and fails with `errno = EXDEV` if the
  buffer isn't physically contiguous.

Full diff and consequences in
`fyp_doc/driver_implementation_status.md` § "Bringup-driven driver
fixes". Builds clean under `-Werror` for both `libt1` and
`visionsoc_main`. Awaiting hardware run once § 6.1 is unblocked.

### 6.3 `axi_iic` kernel panic on probe — RESOLVED ON 5m (2026-05-11)

**Status (2026-05-11 late):** Re-tested on the 5m bitstream. The
`xiic_reinit` panic documented below DOES NOT REPRODUCE — either F4
(BRAM rewire to smartconnect_hb) or F7 (axi_register_slice on T1 hb)
incidentally rebalanced the smartconnect_ctrl-side timing/reset such
that `xiic` initialises cleanly. Specifically, on 5m with the
`axi_iic_sensor` dts node re-enabled:

  * `xiic_i2c_probe → xiic_reinit` completes (no SError)
  * `pca954x 3-0074` registers all 4 muxed IIC buses (which requires
    real IIC ACK exchanges with the hardware — so the bus is
    electrically alive)
  * `ap1302 4-003c` driver attempts probe, fails at
    `Can't get reset GPIO: -2` — that is a software/dts issue
    (missing `reset-gpios` property), NOT an IIC fabric issue.
  * `sp_4issue_with_verify_probe` continues to pass post-IIC enable,
    confirming no T1/DMA regression.

**Action items now blocking camera bringup (no FPGA work needed):**

  1. ~~AP1302 reset-gpios~~ — DONE 2026-05-11 late. Added
     `reset-gpios = <&gpio 79 1>;` to the `ap1302` dts node
     referencing an unrouted EMIO bit. The driver requires the
     property to probe; our BD wires `ap1302_rst_b` from
     `peripheral_aresetn` (chip already de-reset at kernel start),
     so the dead-pin toggle is harmless. dmesg confirms:
     `ap1302 4-003c: AP1302 revision 0.2.6 detected`.
  2. Re-enable `isp_csiss`, `isp_fb_wr_csi`, `isp_vcap_csi` in the
     dts (drop their `status = "disabled"` lines) one at a time,
     reload, dmesg-check after each.
  3. `media-ctl --print-topology` + `v4l2-ctl --device=/dev/videoN
     --set-fmt-video=width=128,height=128,pixelformat=UYVY` to
     bring up the AP1302 → frmbuf chain.

The original 5h/5k/5l-era panic chain is preserved below for
posterity, but is no longer load-bearing for current work.

---

#### Original panic (5h/5k/5l, fixed by 5m's BD topology)

Loading the overlay with `axi_iic_sensor` enabled used to cause an
**Asynchronous SError Interrupt → kernel panic** during
`xiic_i2c_probe → xiic_reinit`. Stack trace: `xiic_reinit+0x188/0x260`.
SError code `0xbf000002`. The fault was the first MMIO write the
driver issued to the IIC controller — the write hit the AXI fabric
with SLVERR or DECERR, propagated as SError, and panicked the CPU.

The original hypothesis (clock/aresetn not driven) was open. We
never positively identified which 5m change fixed it; the leading
guess is F4's smartconnect_hb topology change rebalanced fabric
routing such that smartconnect_ctrl's reset deassertion now lines
up correctly with sensor_iic, but this wasn't ILA-verified. The
practical effect is what matters: on 5m, `xiic` runs.

---

## 3. Known gotchas (carried over from this session)

  1. **Repo name vs module name.** GitHub repo is
     `github.com/ikwzm/udmabuf`; kernel module compiled from it is
     `u-dma-buf` (with dashes); `/dev` nodes are `udmabuf0..N`
     (no dashes). Do not write the repo URL with dashes — it
     404s.
  2. **GitHub clones from the Kria** fail with "could not read
     Username" even for public repos. Do clones on the dev host
     and `scp -r` over.
  3. **Dev host has no `rsync`**, only `scp`. The Task C plan's
     `rsync` lines are aspirational — substitute `scp -r` until
     `apt install rsync` is done locally (out of scope for now).
  4. **ikwzm Makefile has no `install` target.** Manual
     `mkdir /lib/modules/$(uname -r)/extra` + `install` + `depmod
     -a` is required.
  5. **`sudo -n true` is a misleading sanity check** for our
     scoped sudoers — `true` isn't on the allowlist, so the
     check fails even when the listed commands all run
     passwordlessly. Use a listed command (e.g. `sudo -n
     install --version`) if you actually want to test.
  6. **udmabuf modprobe is not persistent** until the
     `/etc/modules-load.d` + `/etc/modprobe.d` files in § 1 are
     in place. After a Kria reboot, re-modprobe or set those up.
  7. **k26-starter-kits is the active overlay** at boot and
     publishes four `axi-pmon` `/dev/uioN` nodes. These will
     collide / mislead UIO-by-index lookups in libt1. `xmutil
     unloadapp` (or rmdir the overlay configfs entry directly)
     before loading visionsoc.
  8. **libt1.c hard-codes `T1_UIO_PATH=/dev/uio0`,
     `DMA_UIO_PATH=/dev/uio1`.** UIO enumeration order after
     `fpgautil` load is not guaranteed to match. If § 2.6 / § 2.8
     show a mismatch, replace with a name-based lookup against
     `/sys/class/uio/uio*/name`. Driver-side change in
     `vision_software/libt1/libt1.c`; a Task B follow-up.

---

## 4. Sudoers — full current contents

Live `/etc/sudoers.d/visionsoc-nopasswd` (mode 440, root:root,
verified 2026-05-07 16:06):

```
ubuntu ALL=(ALL) NOPASSWD: /usr/bin/apt, /usr/bin/apt-get, /usr/bin/make, /usr/bin/install, /usr/bin/mkdir, /usr/bin/rmdir, /usr/bin/cp, /usr/bin/rm, /usr/bin/dmesg, /usr/sbin/modprobe, /usr/sbin/insmod, /usr/sbin/rmmod, /usr/sbin/depmod, /usr/sbin/fpgautil, /usr/bin/xmutil, /usr/bin/devmem2, /usr/bin/systemctl, /usr/bin/tee, /home/ubuntu/vision_software/libt1/test/*, /home/ubuntu/vision_software/visionsoc_main/visionsoc_main
```

A canonical copy is staged at `~ubuntu/visionsoc-nopasswd.template`
on the Kria. `diff` against `/etc` returns clean. To recover after
truncation:

```sh
ssh kv260 'sudo install -m 440 -o root -g root \
  ~/visionsoc-nopasswd.template \
  /etc/sudoers.d/visionsoc-nopasswd && \
  sudo visudo -c'
```

`install` is on the allowlist, so this stays passwordless **as
long as the existing allowlist still covers it**. If the allowlist
itself has been wiped, that hop needs an interactive password, but
the template content is durable in $HOME.

If a future step needs additional binaries (likely candidates:
`/usr/bin/dpkg`, `/usr/bin/dtc`, `/usr/sbin/chattr`), extend the
list in *both* the live `/etc` file *and* the staged template, run
`sudo visudo -c`, and update the listing above so the two stay in
sync.

---

## 5. Verification snippet — one-shot health check

Run this any time to confirm the bringup-pre-bitstream state is
intact:

```sh
ssh kv260 '
  echo "=== sudoers ==="
  ls -l /etc/sudoers.d/visionsoc-nopasswd
  echo "=== udmabuf devs ==="
  ls /dev/udmabuf*
  for i in 0 1 2; do
    echo "udmabuf$i: phys_addr=$(cat /sys/class/u-dma-buf/udmabuf$i/phys_addr 2>/dev/null) size=$(cat /sys/class/u-dma-buf/udmabuf$i/size 2>/dev/null)"
  done
  echo "=== module ==="
  lsmod | grep u_dma_buf || echo "MODULE NOT LOADED"
  echo "=== prereqs ==="
  for p in devmem2 riscv64-linux-gnu-as dtc make gcc; do
    command -v $p >/dev/null && echo "$p: $(command -v $p)" || echo "$p: MISSING"
  done
'
```

### 6.4 "LSU retire never asserts" on hardware — RESOLVED 2026-05-09 (it was CPU cache, not LSU)

**Resolution (added 2026-05-09 evening):** the original symptom —
`ddr_roundtrip` showing the destination udmabuf as all zeros even
after `t1_issue(vse8)` returned success — was **not** a hardware
LSU bug. T1's `m_axi_hb` load and store paths both work
correctly: a raw-mmio probe (`vision_software/libt1/test/lsu_store_probe.c`)
issues `vle8 + vse8` in the same physical layout as
`ddr_roundtrip` and produces correct DRAM contents. The libt1
test failed because **udmabuf-backed `t1_buf` regions are
mmap'd cached** (libt1 opens `/dev/udmabufN` with
`O_RDWR | O_CLOEXEC`, no `O_SYNC`) and Linux's `msync()`
doesn't reliably perform the cache flush/invalidate operations
on udmabuf. CPU writes to `in.va` stayed in cache (T1 read
zeros from DRAM), and CPU reads from `out.va` returned cached
memset-zeros (T1 had written fresh data to DRAM but the cache
was stale).

**The fix is in libt1, not in the BD or RTL:**

  * Two new helpers, `t1_buf_sync_for_device(&buf)` and
    `t1_buf_sync_for_cpu(&buf)`, write to udmabuf's
    `/sys/class/u-dma-buf/udmabufN/sync_for_device` /
    `/sync_for_cpu` sysfs attributes. The udmabuf module performs
    the right L1/L2 cache op when those are written to.
  * The `t1_buf` struct grows a private `_udmabuf_idx` field so
    the helpers can reach the matching sysfs path.
  * Tests (`ddr_roundtrip`, `port_grid_vadd`, `vert_lsu`,
    `dma_loopback`) replace each `msync(...MS_SYNC)` before T1
    reads with `t1_buf_sync_for_device(&buf)`, and each
    `msync(...MS_INVALIDATE)` after T1 writes with
    `t1_buf_sync_for_cpu(&buf)`. **`O_SYNC` on the udmabuf open
    was tried and rejected** — it makes the mmap uncached, which
    breaks SIMD-optimised libc routines (`memset`/`memcmp` SIGBUS
    on uncached aliases of cacheable DRAM in this kernel).

Required reading for any code that allocates a `t1_buf` and
shares it with T1: bracket the LSU sequence with sync calls.
The ordering is:

```c
init_input(&in.va);                        // CPU writes
t1_buf_sync_for_device(&in);               // flush to DRAM
t1_buf_sync_for_device(&out);              // (also for the dest)
t1_issue({ vle8, in.pa, ... });
t1_issue({ vse8, out.pa, ... });
t1_buf_sync_for_cpu(&out);                 // invalidate before CPU reads
memcmp(in.va, out.va, ...);                // now sees actual DRAM
```

Future agents extending libt1: don't be tempted to drop the sync
calls "because msync should work". msync on udmabuf does not
reliably perform the right cache op.

#### Final test results (5h bitstream + libt1 cache fix)

| Test | Result | Notes |
|---|---|---|
| `smoke` | PASS | PERF_CYCLES + VERTICAL_MODE round-trip |
| `triage_t1` | ALL PASS | control plane; Fix 2 verified |
| `ddr_roundtrip` | PASS | LSU load + store (128-byte vlmax slice) |
| `port_grid_vadd` | PASS | full kernel: vle/vle/vsub/vse |
| `vert_lsu` | PASS | vertical-mode load + horizontal-mode store |
| `dma_loopback` | FAIL | `t1_dma_wait: Connection timed out` — pre-existing BD issue, `axi_dma/S_AXIS_S2MM` was never wired (visible as a critical warning in build.log since 5e). Not LSU-related. |

#### Original symptom (kept below for posterity)

**Status: open.** Discovered while running `ddr_roundtrip` /
`port_grid_vadd` / `vert_lsu` against the 5h bitstream
(`…fpga-20260509-121320/`) immediately after § 6.1.5's fix landed.

#### Symptom

`t1_init` succeeds, `triage_t1` passes (control plane fully working),
but every test that issues `vle8.v` or `vse8.v` fails with the output
udmabuf still at memset-zero. Direct register polling via the wrapper
BAR shows:

```
initial:  CTRL=0x2 INSTR=0x2050427 RS1=0x37d00000 VTYPE=0xc2 VL=0x80
poll[0..199]: CTRL=0x2  MEM_COUNT=0x0  IRQ_STATUS=0x0
```

200 ms after issuing both `vle8` (load) and `vse8` (store):

  * `CTRL == 0x2` (`issue_ready=1, issue_pending=0`) — wrapper
    has handed both issues off to T1 and is idle.
  * `MEM_COUNT == 0` — the wrapper's saturating retire counter
    never incremented.
  * `IRQ_STATUS == 0` — none of `mem_count != 0`, `csr_fifo_nonempty`,
    `rd_fifo_nonempty` ever asserted.
  * Output buffer at the udmabuf phys address (`0x37d00000` etc.) is
    still all zeros.

The wrapper increments `mem_count` on `retire_mem_valid` from T1
(`t1_axi_lite_wrapper.sv:227-238`). T1's `retire_mem_valid` is driven
from `slots_0_record_isLoadStore & slotCommit_0` (T1.sv line ~4594).
So `slotCommit_0` is never asserting for LSU instructions — *or* it
is asserting but the wire isn't reaching the wrapper.

#### What's been confirmed

  * Encoding is correct: `vle8.v v8, (a0)` = `0x02050407`,
    `vse8.v v8, (a0)` = `0x02050427`. Verified bit-by-bit against
    the RVV spec. Initial register snapshot shows the wrapper has
    these latched at 0x04 (INSTRUCTION).
  * `RS1` holds the right physical address (matches udmabuf phys
    addr from `/sys/class/u-dma-buf/udmabufN/phys_addr`).
  * `VTYPE = 0xC2` (e8/m4/ta/ma) and `VL = 0x80` (=128) are correct
    for this T1 config (`zvl256b` → VLEN=256, lmul=4 sew=8 →
    vlmax=128).
  * T1 accepts the issue (CTRL goes back to `issue_ready=1` after
    each `t1_issue`).
  * udmabuf phys address is in HPC0_FPD's reachable range
    (assigned 2GB at offset 0 in BD address map for
    `t1_top/m_axi_hb`).
  * Wrapper s_axi_ctrl path is fully working (control-plane reads
    + writes pass `triage_t1`).
  * `axi_dma` is unaffected — the LSU bug is on T1's `m_axi_hb`
    path, not on the DMA path.

#### What hasn't been tested yet

  * **A non-LSU instruction that retires to `rd_fifo`** (e.g.
    `vmv.x.s t0, v8`). Would tell us if T1's retire path works at
    all, and isolate the problem to the LSU specifically.
  * **An ILA on `t1_top/m_axi_hb`** (currently the ILA in the BD
    is on `smartconnect_ctrl/M00_AXI` which monitors the control
    plane, not the LSU's master-out path). To diagnose this issue,
    the ILA would need to move to `smartconnect_hb/S00_AXI` (the
    cable from T1 hb master into smartconnect_hb).
  * Whether T1's *internal* slot is genuinely deadlocked vs
    completing without firing `retire_mem_valid` to the wrapper.
    Would need T1-source-level instrumentation or simulation reproduction.
  * Whether the same RTL works on the t1emu simulator with the
    same instruction sequence — if yes, the bug is hardware-only,
    most likely in the AXI master path.

#### Proposed next steps

1. **Run a non-LSU smoke test** — e.g. issue `vmv.x.s t0, v8` (or
    similar scalar-destination op) and check `RD_FIFO_STS` / read
    `RD_POP_DATA`. If T1 retires non-memory ops correctly, the bug
    is LSU-specific. If T1 doesn't retire at all, the bug is in the
    issue/retire control path (wider scope).
2. **Move the ILA** to `smartconnect_hb/S00_AXI` for next rebuild;
    capture the `awvalid/wvalid/bvalid` cycle on a vse and see
    whether T1's `m_axi_hb` master is actually emitting transactions.
3. **Cross-check at simulation:** run the same instruction sequence
    in t1emu against this configuration and confirm
    `retire_mem_valid` fires. If sim works, hw doesn't, the bug is
    in the AXI master path or its synthesis.
4. **Check VRF / mask state:** `vstart=0, vmask=0` (default) — any
    chance a stale CSR from a previous test left T1 in an unusual
    state? Try a hard reset between tests (re-load bitstream) and
    repeat.

#### Driver-side workarounds (none ship-able)

`libt1`'s `t1_wait_mem` polls UIO IRQ then checks `IRQ_STATUS`;
neither fires here. `t1_dma_wait` polls `DMA_SR.IDLE` directly,
which works for DMA but isn't the right channel for T1 LSU
completion. There's no current "poll wrapper directly until
mem_count > 0" path in libt1; the test/diagnostic in
`ddr_roundtrip.c:60-86` is the live experiment doing this.

