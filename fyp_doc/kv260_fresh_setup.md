# KV260 Fresh-Board Setup Guide

End-to-end recipe for taking a sealed AMD Kria KV260 + AR1335/AP1302
IAS camera and bringing it to the point where `visionsoc_main` runs
the camera → DDR → URAM → T1 → URAM → DDR → HDMI pipeline at ~20 fps.

Each section is independent — if your board already has some steps
done, skip to the first one that's missing and use the verification
snippets at the end (§ 9) to confirm state.

This doc consolidates the operational steps from
`camera_bringup_status.md`, `camera_handoff_2026-05-13.md`,
`fpga_build_status.md`, and the run/sync scripts under
`vision_software/visionsoc_main/`. Where those docs disagree with
each other (older notes vs. resolved findings) this guide reflects
the resolved/current state only.

---

## Developer paths — which one are you? (READ FIRST)

**Decide this before installing anything on the dev host.** It
determines whether you need Vivado 2025.2 (≥ 80 GB install, ~1-2 h
download, ~30 min install).

### Path A — Kernel-only developer (no RTL changes)

You are writing/iterating C and RISC-V vector assembly in
`vision_software/` against an existing T1 bitstream. You will deploy
one of the two pre-built `.bit.bin` files (5r or 5t-maskopt — see
§ 0) and never rebuild the FPGA.

**Vivado is NOT required.** You can finish the entire fresh-board
setup, deploy the pre-built bitstream, and iterate kernels without
ever installing Vivado.

**All of the day-to-day developer scripts are pure Path A** (none
shell out to `vivado` or `bootgen`, none require Nix):

| Script | Purpose | Tools it actually uses |
|---|---|---|
| `vision_software/visionsoc_main/sync_kernel.sh` | swap the active T1 vector kernel, native-build on the board (§ 6) | `scp`, ssh, `make`, `riscv64-linux-gnu-as` (board side) |
| `vision_software/visionsoc_main/sync_perf.sh` | scp + native-build the perf binaries on the board (§ 8.1) | `scp`, ssh, `make` (board side) |
| `vision_software/visionsoc_main/gather_perf.sh` | run perf measurement, pull CSV, render plot (§ 8.1) | ssh, `scp`, `python3` + `matplotlib` (the only places it touches the deployed `.bit.bin` are an `sha256sum` for logging and a path string for `fpgautil` reload — never converts or builds) |
| `vision_software/visionsoc_main/perf/plot_perf.py` | render the perf CSV → stacked-bar PNG (§ 8.1) | `python3`, `matplotlib`, `numpy` |
| `vision_software/visionsoc_main/run_after_power_cycle.sh` | reload overlay + start `visionsoc_main` on the board (§ 8) | runs on the **board**, uses `fpgautil` (pre-installed on Kria-Ubuntu) — dev host doesn't need anything for this |

If you want to **simulate a kernel under Verilator before deploying
to hardware** (`./run-test.sh ...`), see Path C below — it's a
separate Nix-based toolchain orthogonal to Path A.

What you skip:
  * § 2.1's `bootgen` row (Vivado-bundled tool) — pre-built
    `.bit.bin` files are already in the repo build dirs.
  * § 7.1's "freshly built bitstream" branch — the pre-built path
    in § 7.1 is one `ls` to sanity-check the file exists.
  * The whole bitstream-rebuild iteration loop (not documented
    here; see `fpga_build_status.md`).

What you still need (full list in § 2.1): `openssh-client`, `git`,
`device-tree-compiler`, `python3` + `matplotlib` + `numpy` (only if
running the perf harness in § 8.1), and `screen`/`picocom` for
first-boot UART (§ 1.2). About **50-100 MB total on dev host**.

This is the recommended path if you are an FYP / vision software
developer extending kernels, tuning the camera→T1→display pipeline,
or measuring performance.

### Path B — RTL / bitstream developer

You are modifying the T1 vector core (Chisel/Scala under `t1/`),
the BD (`fpga/system/system_top.tcl`), the wrapper Verilog, or any
of the IP configs that change the synthesised hardware. You will
run Vivado to elaborate + synth + impl + write_bitstream, then
deploy your new `.bit.bin`.

**Vivado 2025.2 IS required.** Bitstream rebuild typically takes
5-9 hours of Vivado runtime per attempt (see
`fpga_build_status.md` §§ 0.7-0.13 for the project's iteration
trail and routing-cliff warnings at >85% LUT util).

Everything Path A installs, **plus**:
  * Vivado 2025.2 from AMD's installer (the project's reference
    install root is `~/Xilinx/2025.2/`). The `bootgen` binary in
    `~/Xilinx/2025.2/Vivado/bin/bootgen` is the only Vivado-bundled
    tool this setup doc directly invokes (§ 7.1).
  * Optionally: `nproc`, `time`, monitoring tools — Vivado is
    happy with whatever a stock Ubuntu dev host already has.

For the actual rebuild flow (out of scope of this fresh-setup
doc), see `fpga/system/build_fpga.sh` and `fpga_build_status.md` §§
1, 2, and the `0.x` "build in flight" sections.

### Path C — Kernel simulation via Verilator (`run-test.sh`)

You want to validate a T1 vector kernel *before* deploying it to
real hardware — the canonical pre-hardware test flow uses
`run-test.sh` at the repo root, which drives the Nix-built
Verilator emulator (`t1emu` or `t1rocketemu`) and, optionally,
diff-tests against Spike. Examples:

```sh
# Run a vector test under the default Verilator emu (t1emu):
./run-test.sh intrinsic.linear_normalization

# Add waveform tracing:
./run-test.sh intrinsic.linear_normalization -e verilator-emu-trace

# Diff-test against Spike (the --check flag):
./run-test.sh intrinsic.linear_normalization -e verilator-emu-trace --check
```

This is **strongly recommended for any new T1 kernel work** — it
catches register-spill bugs (§ `2d_fabric_handoff.md` § 3.2), CSR
0x7c0 mode mistakes, vredsum gotchas (§ 4.1), and other 2D-fabric
specific footguns before you spend 30+ s on each hardware deploy
iteration. Many of the project's memory entries (`run-test.sh
max-cycles default`, `T1_MIRROR_RTL_WRITES=1 for vert-mode`, etc.)
come from this workflow.

**Nix IS required for Path C** (everything `run-test.sh` builds is
expressed as Nix derivations in the project's `flake.nix`). You do
**not** need Vivado.

What Path C adds on top of Path A:
  * **Nix** with flakes enabled. Install per the project's
    `README.md` § "Nix setup" — either the official binary
    installer (https://nixos.org/manual/nix/stable/installation/installing-binary.html)
    + flake enable, or the Determinate Systems installer
    (https://github.com/DeterminateSystems/nix-installer) which
    enables flakes by default.
  * **First-build cost:** the initial `nix build` of the T1
    emulator pulls + compiles LLVM/Verilator/the RISC-V toolchain
    /the Chisel elaborator. Expect **multiple hours and 10-50 GB
    of disk** the first time; subsequent invocations hit the cache
    and are seconds-to-minutes.

What Path C does *not* need:
  * Vivado (Path B only)
  * The board / SSH / `vision_software/` (Path C runs entirely on
    the dev host — useful for kernel work even without a Kria on
    the bench)

For the kernel-programming contract that Path C tests will
target, read `fyp_doc/2d_fabric_handoff.md` (the
non-negotiable programmer reference for the 2D fabric).

### Paths are additive

A typical FYP developer ends up with **Path A + Path C** (deploy +
simulate), and only adds **Path B** if they need to change the
hardware. The three paths share most of the dev-host package list
in § 2.1 — only Vivado (Path B) and Nix (Path C) are unique to
their respective paths.

---

## 0. What you need on the bench

  * KV260 Vision AI Starter Kit (board + power brick + USB-UART
    micro-USB cable).
  * AR1335 + AP1302 IAS camera module + the 15-pin FFC ribbon (ships
    with the KV260 kit). Plug into the **CAM** connector on the
    KV260 carrier card.
  * Micro-SD card, ≥16 GB, Class 10 or better.
  * HDMI cable + a monitor that takes 1080p (HDMI out is on the
    carrier card).
  * Ethernet cable on the same subnet as your dev host.
  * Dev host with internet access. Dev-host package list is in
    § 2.1 — only ~50-100 MB for kernel-only work (Path A); Vivado
    is only needed for Path B (see the "Developer paths" section
    above).

**Two stable bitstreams are available** as of 2026-05-18. Both share
the same camera+display+URAM/DMA pipeline; pick by T1 vector core
size and area trade-offs.

### Option A — 5r (vLen=256, URAM DMA, original MaskUnit) — DEFAULT

`mudkip2d128small1bram1chain2lanescale_fpga` (LMUL=4 default,
vLen=256). URAM-backed 512 KB scratchpad. Compact, low-LUT design.
Kernels written for vLen=256 hardware (most existing demos).

Repo-relative path:

```
fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260516-010205/
    system_top_wrapper.bit.bin      ← use this with fpgautil
    system_top_wrapper.bit          ← Vivado-format, convert with bit2bin (§ 7.1)
```

Absolute path on this dev host:

```
/home/cbt22/code/code_fyp/VisionSoC/fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260516-010205/system_top_wrapper.bit.bin
```

The matching device-tree overlay is generated from
`fpga/dts/system_top_wrapper.dts`. Backup / revert target is **5q-r3**
at `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260515-024828/`
(see § 11 for the revert procedure).

### Option B — 5t-maskopt (vLen=1024, URAM DMA, MaskUnitFpga area-reduction)

`mudkip2d128big1bram1chain2lanescale_fpga_maskopt` (LMUL=1
architectural baseLMUL, vLen=1024). Same URAM-backed 512 KB
scratchpad, same camera pipeline. **4× larger vector register file**
than 5r — useful for kernels that benefit from longer vectors or
need more architectural registers. Built on top of all MaskUnit
area-reduction optimisations (catalog in
`fyp_doc/maskunit_fpga_optimisations.md`) so the design still fits
the KV260 LUT budget despite the 4× vector storage.

Repo-relative path:

```
fpga/build/mudkip2d128big1bram1chain2lanescale_fpga_maskopt-20260518-055430/
    system_top_wrapper.bit          ← Vivado-format, convert with bit2bin (§ 7.1)
    system_top_wrapper.bit.bin      ← (regenerate with bootgen — see § 7.1)
```

Absolute path on this dev host (Vivado .bit format):

```
/home/cbt22/code/code_fyp/VisionSoC/fpga/build/mudkip2d128big1bram1chain2lanescale_fpga_maskopt-20260518-055430/system_top_wrapper.bit
```

Resource summary (post-impl): 104,717 LUT / 143 BRAM36 + 2 BRAM18 (=
**144 BRAM tiles exactly = 100% of cap**) / 16 URAM. Timing: WNS
+0.239 ns, WHS +0.010 ns (setup + hold both clean). Verified
end-to-end with the Sobel kernel running at ~19 fps (steady-state)
on real hardware.

The matching device-tree overlay uses the same
`fpga/dts/system_top_wrapper.dts` source as 5r — the device-tree node
structure is shared because the camera/display/URAM/DMA peripherals
are identical; only the T1 internals differ, and T1 internals don't
appear in the device tree.

### Which to deploy?

* Use **5r** if you're running existing demos / kernels written for
  vLen=256.
* Use **5t-maskopt** if you have a kernel that benefits from the
  larger vector register file (e.g. ML inference with bigger
  weights, or any kernel re-tuned for vLen=1024 LMUL=1).
* They swap by replacing
  `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin` +
  `.dtbo` and reloading the overlay. See § 11 for the swap
  procedure.

---

## 1. Flash the SD card with Ubuntu 22.04 (jammy)

The KV260 ships supporting AMD's pre-built Ubuntu Server 22.04 LTS
image. **Do not use Ubuntu 24.04** — the Xilinx PPAs we need in § 5
ship `jammy/arm64` packages only.

  1. Download the official Ubuntu Server 22.04 image for KV260
     (`iot-kria-classic-desktop-2204-x06-…` or whichever the current
     Canonical KV260 release is on
     https://ubuntu.com/download/amd ).
  2. Flash to the SD with `dd`, BalenaEtcher, or `rpi-imager`:
     ```sh
     xzcat ubuntu-…-kria-arm64.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
     ```
     (Replace `/dev/sdX` with the real SD device — `lsblk` to
     confirm before running.)
  3. Eject the SD and insert into the KV260 SD slot.

### 1.1 Boot firmware (one-time, only on a never-used board)

The KV260's QSPI boot firmware needs to be the K26 SOM "Starter Kit"
firmware (≥ 2022.1) for Ubuntu to boot. Brand-new boards from AMD
already ship with the right firmware. If your board was previously
running PetaLinux or you have boot failures, follow Canonical's
KV260 boot-firmware-update procedure (USB-UART console + `xmutil
bootfw_update`) — out of scope for this doc.

### 1.2 First boot

  * Connect HDMI, Ethernet, micro-USB UART (to your dev host), and
    power.
  * On the dev host: `screen /dev/ttyUSB1 115200` (or `picocom`)
    to see the U-Boot + kernel console.
  * Default Ubuntu login is `ubuntu / ubuntu`; you'll be prompted
    to change the password on first SSH/console login.
  * Note the board's IP (Ethernet should DHCP); add it to
    `~/.ssh/config` on the dev host as `Host kv260`. Everything in
    this guide assumes `ssh kv260` is the canonical hop.

---

## 2. Bring the dev host into sync with the board

This is the workflow the project uses throughout — all source builds
happen **natively on the Kria** over SSH, not cross-compiled. The dev
host pushes sources via `scp -r` and the Kria runs `make`.

### 2.1 Dev-host packages

The dev host needs the following tools to drive everything in §§ 4-9.
The **Path** column refers to the developer-path decision at the top
of this doc — Path A rows are required for everyone; Path B rows
are only needed if you are rebuilding bitstreams.

| Tool | Path | Used by | Ubuntu/Debian package |
|---|:-:|---|---|
| `ssh`, `scp`, `ssh-copy-id` | A | every dev → board hop | `openssh-client` |
| `git` | A | clone `udmabuf` (§ 4) + `ap1302-firmware` (§ 5.1) | `git` |
| `dtc` (device-tree-compiler) | A | compile `system_top_wrapper.dts` → `.dtbo` (§ 7.2) | `device-tree-compiler` |
| `python3` + `matplotlib` + `numpy` | A | perf plotter `plot_perf.py` (§ 8.1, optional) | `python3 python3-matplotlib python3-numpy` |
| `screen` or `picocom` | A | USB-UART console during first boot (§ 1.2) | `screen` (or `picocom`) |
| **`bootgen`** | **B** | convert Vivado `.bit` → `.bit.bin` for `fpgautil` (§ 7.1, only needed for freshly-built bitstreams; the pre-built bitstreams in the repo already ship the `.bit.bin`) | from Vivado/Vitis 2025.2 install (`$XILINX/Vivado/bin/bootgen`) |
| **Vivado 2025.2** | **B** | rebuild bitstream from source | from AMD installer (~80 GB install, ~1-2 h download) |
| **`nix`** (flakes enabled) | **C** | drive `./run-test.sh` (Verilator simulation of T1 kernels) — all `nix build .#t1.<config>.<top>.verilator-emu` etc. invocations | upstream Nix binary installer or Determinate Systems installer (see project `README.md` § "Nix setup") |

One-shot install of the Path-A tools on a Debian/Ubuntu dev host
(this is all you need for kernel-only work that only iterates on
hardware):

```sh
sudo apt install -y openssh-client git device-tree-compiler \
                    python3 python3-matplotlib python3-numpy screen
```

**Path B addendum — only if you are also rebuilding bitstreams.**
Install Vivado/Vitis 2025.2 from AMD's installer
(https://www.xilinx.com/support/download.html). The project's
reference install root is `~/Xilinx/2025.2/`. Then add `bootgen` to
PATH so § 7.1 finds it:

```sh
export PATH="$HOME/Xilinx/2025.2/Vivado/bin:$PATH"
which bootgen     # should return $HOME/Xilinx/2025.2/Vivado/bin/bootgen
```

(You can put the `export` line in `~/.bashrc` to make it permanent.)

**Path C addendum — only if you want pre-hardware kernel simulation
via `./run-test.sh`.** Install Nix with flakes enabled. The
project's `README.md` § "Nix setup" is the canonical reference;
the short form (Determinate Systems installer, flakes on by
default):

```sh
curl --proto '=https' --tlsv1.2 -sSf -L \
    https://install.determinate.systems/nix | sh -s -- install
# Re-open your shell, then verify:
nix --version
nix flake show /home/cbt22/code/code_fyp/VisionSoC 2>&1 | head -5
```

Or the upstream installer + manual flake enable: see
https://nixos.org/manual/nix/stable/installation/installing-binary.html
then https://nixos.wiki/wiki/Flakes#Enable_flakes.

First `./run-test.sh <case>` invocation will pull + compile LLVM /
Verilator / RISC-V toolchain / the Chisel elaborator — expect
**multiple hours and 10-50 GB** on first run, seconds-to-minutes
after that (cached). Cycle-counts cap defaults to 10M; full-grid
kernels need `--max-cycles 50000000` per
`feedback_max_cycles_default.md`.

### 2.2 SSH from dev host to board

```sh
# On the dev host, ~/.ssh/config:
Host kv260
    HostName <board-ip>
    User ubuntu
    IdentityFile ~/.ssh/id_ed25519  # or whichever key you push to the board
    StrictHostKeyChecking accept-new

# First-time: copy your pubkey so passwordless ssh works
ssh-copy-id kv260
```

Smoke-test the link:
```sh
ssh kv260 'uname -a; cat /etc/os-release | head -2'
# Expect: Linux …-xilinx-zynqmp aarch64 / Ubuntu 22.04
```

Known dev-host quirks (carried from `camera_bringup_status.md` § 3):

  * **No `rsync` on the dev host (this project's reference dev
    host)** — every script uses `scp -r` instead. If your dev host
    has `rsync`, the project scripts still work as written.
  * **`git clone` from the Kria fails for many HTTPS repos**
    ("could not read Username"). When you need a third-party repo
    on the board, clone on the dev host and `scp -r` it over. This
    is why §§ 4 and 5.1 below do the clone on the dev host first.

---

## 3. Passwordless sudo + base apt packages on the board

### 3.1 Scoped passwordless sudoers

The build, deploy, and run scripts call `sudo` for `apt`, `make
install`, `fpgautil`, `xmutil`, `systemctl`, `devmem2`, etc.
Without a sudoers allowlist every step prompts for a password. The
project ships a 440 root:root sudoers fragment at
`/etc/sudoers.d/visionsoc-nopasswd`:

```
ubuntu ALL=(ALL) NOPASSWD: /usr/bin/apt, /usr/bin/apt-get, /usr/bin/make, \
/usr/bin/install, /usr/bin/mkdir, /usr/bin/rmdir, /usr/bin/cp, /usr/bin/rm, \
/usr/bin/dmesg, /usr/sbin/modprobe, /usr/sbin/insmod, /usr/sbin/rmmod, \
/usr/sbin/depmod, /usr/sbin/fpgautil, /usr/bin/xmutil, /usr/bin/devmem2, \
/usr/bin/systemctl, /usr/bin/tee, \
/home/ubuntu/vision_software/libt1/test/*, \
/home/ubuntu/vision_software/visionsoc_main/visionsoc_main
```

Install it once (this is the only interactive-password step in the
whole flow):

```sh
ssh kv260
# in the board shell:
sudo tee /etc/sudoers.d/visionsoc-nopasswd > /dev/null <<'EOF'
ubuntu ALL=(ALL) NOPASSWD: /usr/bin/apt, /usr/bin/apt-get, /usr/bin/make, /usr/bin/install, /usr/bin/mkdir, /usr/bin/rmdir, /usr/bin/cp, /usr/bin/rm, /usr/bin/dmesg, /usr/sbin/modprobe, /usr/sbin/insmod, /usr/sbin/rmmod, /usr/sbin/depmod, /usr/sbin/fpgautil, /usr/bin/xmutil, /usr/bin/devmem2, /usr/bin/systemctl, /usr/bin/tee, /home/ubuntu/vision_software/libt1/test/*, /home/ubuntu/vision_software/visionsoc_main/visionsoc_main
EOF
sudo chmod 440 /etc/sudoers.d/visionsoc-nopasswd
sudo chown root:root /etc/sudoers.d/visionsoc-nopasswd
sudo visudo -c                              # syntax check
cp /etc/sudoers.d/visionsoc-nopasswd ~/visionsoc-nopasswd.template   # recovery copy
```

If the file gets truncated (it has been observed wiped after some
reboots — suspected cloud-init interaction):
```sh
ssh kv260 'sudo install -m 440 -o root -g root \
  ~/visionsoc-nopasswd.template \
  /etc/sudoers.d/visionsoc-nopasswd && sudo visudo -c'
```
`install` is on the allowlist so the recovery itself stays
passwordless once the template is in `$HOME`.

### 3.2 Base packages

These are needed for everything that follows (RISC-V assembler for
T1 kernels, libdrm for HDMI, kernel headers for u-dma-buf, dtc for
ad-hoc dtbo re-compile on the board, `gst-inspect-1.0` for the § 9.1
pre-flight check, etc.):

```sh
ssh kv260 '
  sudo apt update
  sudo apt install -y \
    devmem2 \
    binutils-riscv64-linux-gnu \
    libdrm-dev \
    linux-headers-$(uname -r) \
    build-essential \
    pkg-config \
    v4l-utils \
    media-ctl \
    i2c-tools \
    device-tree-compiler \
    gstreamer1.0-tools
'
```

Notes:
  * `media-ctl` is part of `v4l-utils` on Ubuntu — both lines kept
    for clarity.
  * `device-tree-compiler` provides `dtc`. The fresh-setup flow only
    needs `dtc` on the dev host (§ 7.2 compiles the dtbo there) but
    keeping it on the board too is useful for in-place edits during
    debug and is what the § 9.1 pre-flight check expects.
  * `gstreamer1.0-tools` provides `gst-inspect-1.0`, used to verify
    `mediasrcbin` installed correctly in § 5.2. It is also pulled in
    transitively by `vvas-essentials` in § 5.2; the explicit install
    here just guarantees ordering.
  * `pkg-config` is used by the `visionsoc_main` Makefile to find
    `libdrm` via `pkg-config --cflags/--libs libdrm`. It is usually
    pre-installed (it's a dep of `build-essential` on Ubuntu) but
    listed here to be explicit.

---

## 4. u-dma-buf kernel module (contiguous DMA buffers)

T1's DMA engine needs physically contiguous, cache-coherent DDR
buffers. The Linux kernel doesn't expose CMA to userspace directly,
so we use Ichiro Kawazome's `u-dma-buf` module which carves out
named contiguous buffers and exposes them as `/dev/udmabufN`.

The module is **not** in apt — build from source:

```sh
# On the dev host:
git clone https://github.com/ikwzm/udmabuf.git /tmp/udmabuf
scp -r /tmp/udmabuf kv260:~/u-dma-buf

# On the board:
ssh kv260 '
  cd ~/u-dma-buf && make
  sudo mkdir -p /lib/modules/$(uname -r)/extra
  sudo install -m 644 u-dma-buf.ko /lib/modules/$(uname -r)/extra/
  sudo depmod -a
'
```

Make it auto-load on every boot with three pre-allocated 4 MB
buffers (this is what libt1 expects):

```sh
ssh kv260 '
  echo "u-dma-buf" | sudo tee /etc/modules-load.d/u-dma-buf.conf
  echo "options u-dma-buf udmabuf0=4194304 udmabuf1=4194304 udmabuf2=4194304" \
    | sudo tee /etc/modprobe.d/u-dma-buf.conf
  sudo modprobe u-dma-buf udmabuf0=4194304 udmabuf1=4194304 udmabuf2=4194304
'
```

Verify:
```sh
ssh kv260 '
  lsmod | grep u_dma_buf
  ls /dev/udmabuf*
  for i in 0 1 2; do
    echo "udmabuf$i: phys=$(cat /sys/class/u-dma-buf/udmabuf$i/phys_addr) \
size=$(cat /sys/class/u-dma-buf/udmabuf$i/size)"
  done
'
# Expect three 0x400000-byte buffers at distinct phys addrs.
```

Gotchas (carried over):
  * Repo is `ikwzm/udmabuf` (no dash). Module is `u-dma-buf` (with
    dashes). Device nodes are `udmabufN` (no dashes). Don't write
    the repo URL with dashes — it 404s.
  * The Makefile has no `install` target — the manual `install` +
    `depmod` above is required.

---

## 5. Camera enablement — AP1302 firmware + mediasrcbin

This is the part the user remembered as "the camera app that needed
docker". **It does not actually need docker** — the relevant
gstreamer plugin (`mediasrcbin`) is missing from stock Ubuntu but
**is shipped in the Xilinx-Ubuntu PPAs**, which `Xilinx/kria-docker`
just happened to document the apt-source URLs for. We install
straight to the host Ubuntu, no container.

Background on why this is necessary (see
`camera_handoff_2026-05-13.md` § 4.18-4.22 for the full trail):
plain `v4l2src` / `v4l2-ctl --stream-mmap` can't negotiate the
format chain against the AP1302 — they leave the AP1302 HINF
counter throttled to ~1/100 of the sensor frame rate. Xilinx's
internal `mediasrcbin` gst element drives the V4L2 negotiation
correctly and unblocks the camera. We tried building VVAS,
loading the smartcam app, reverse-engineering ioctls — none of
those work on stock Ubuntu. The PPA install is the only path that
shipped end-to-end on Ubuntu 22.04 jammy/arm64.

### 5.1 AP1302 sensor firmware

Stock KV260 Ubuntu ships a Nov 2021 vintage AP1302 firmware blob
that panics with a CRC mismatch on modern AP1302 silicon. Replace
it with the current `Xilinx/ap1302-firmware` HEAD:

```sh
# On the dev host:
git clone https://github.com/Xilinx/ap1302-firmware.git /tmp/ap1302-fw
scp /tmp/ap1302-fw/ap1302_ar1335_single_fw.bin kv260:/tmp/

# On the board:
ssh kv260 '
  # Back up the stock blob first (the older one is preserved repo-side too):
  sudo install -m 644 /lib/firmware/ap1302_ar1335_single_fw.bin \
    /lib/firmware/ap1302_ar1335_single_fw.bin.old
  sudo install -m 644 /tmp/ap1302_ar1335_single_fw.bin \
    /lib/firmware/ap1302_ar1335_single_fw.bin
'
```

This file persists on rootfs and survives reboots / overlay loads.
**Do not** unbind/rebind the ap1302 driver after this — it triggers
a kernel oops; just reload the FPGA overlay (§ 7).

### 5.2 Xilinx PPAs + mediasrcbin install

`mediasrcbin` lives in the Xilinx-patched `gstreamer1.0-plugins-bad`
package available from two Launchpad PPAs:

  * `ppa:ubuntu-xilinx/sdk` — provides `vvas-essentials`
  * `ppa:ubuntu-xilinx/gstreamer` — provides the patched
    `gstreamer1.0-plugins-bad`

It is **not** in the stock Ubuntu archive and **not** in any
`Xilinx/vvas` GitHub release — see `camera_handoff_2026-05-13.md`
§§ 4.18-4.24 for the trail (`vvas-gst-plugins` was tried from
source, `xlnx-app-kv260-smartcam` was tried via `xmutil loadapp`,
neither shipped `mediasrcbin` standalone). The PPA path is the
only one that worked end-to-end on Ubuntu 22.04 jammy/arm64.

```sh
ssh kv260 '
  # Add the two ubuntu-xilinx PPAs (gstreamer + sdk):
  echo "deb https://ppa.launchpadcontent.net/ubuntu-xilinx/sdk/ubuntu jammy main" \
    | sudo tee /etc/apt/sources.list.d/ubuntu-xilinx-sdk.list
  echo "deb https://ppa.launchpadcontent.net/ubuntu-xilinx/gstreamer/ubuntu jammy main" \
    | sudo tee /etc/apt/sources.list.d/ubuntu-xilinx-gstreamer.list

  # PPA signing key (52150A179A9E84C9 = Launchpad PPA for Ubuntu Xilinx):
  gpg --keyserver keyserver.ubuntu.com --recv-keys 52150A179A9E84C9
  gpg --export 52150A179A9E84C9 | sudo tee /etc/apt/trusted.gpg.d/ubuntu-xilinx.gpg

  sudo apt update
  sudo apt install -y vvas-essentials gstreamer1.0-plugins-bad
'
```

Verify `mediasrcbin` is now visible to GStreamer:
```sh
ssh kv260 'gst-inspect-1.0 mediasrcbin | head -8'
# Expect:
#   Name      mediasrcbin
#   Filename  /usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstmediasrcbin.so
```

The pure-Ubuntu `gstreamer1.0-plugins-bad` (no `vvas-essentials`)
does **not** include mediasrcbin — the Xilinx-patched build does.
That's the whole reason for the PPA hop.

### 5.3 (Optional) Docker reference

For completeness: the apt-source URLs above were originally
discovered from `Xilinx/kria-docker`'s `kria-runtime` Dockerfile
(`dockerfiles/kria-runtime`). If you ever need a containerised
camera stack, that image is the canonical reference. Nothing in
this project actually uses docker — everything runs on the host
Ubuntu — but the Dockerfile is a good cross-check if a future PPA
change breaks the plain-apt path documented in § 5.2.

---

## 6. Sync the project sources to the board

The repo lives on the dev host. The board needs a working copy of
`vision_software/` (libt1 + visionsoc_main + kernels). One-time:

```sh
# On the dev host, in the VisionSoC checkout root:
scp -r vision_software kv260:~/

ssh kv260 '
  cd ~/vision_software/libt1 && make
  cd ~/vision_software/visionsoc_main && make
'
```

For ongoing iteration — when you change a T1 kernel and want to
swap it in:

```sh
# On the dev host:
cd vision_software/visionsoc_main
./sync_kernel.sh frame_passthrough   # or sobel, or your own
```

`sync_kernel.sh` does:
  1. Rewrites `kernels/active_kernel.h` to `#include
     "<name>_select.h"`.
  2. `scp`s sources + all `kernels/*.S` + `kernels/*.h` to the
     board.
  3. Runs `build_kernel.sh` + `make` on the board (native aarch64
     — the dev host's x86-64 gcc cannot produce an aarch64
     binary).
  4. `scp`s the assembler-verified `kernels/<name>.h` back so the
     local repo stays in sync.

Make sure `visionsoc_main` is **not** running on the board when
you re-sync; step 3 overwrites the binary and will fail with
"Text file busy" otherwise.

---

## 7. Stage the bitstream + DTBO; load them with fpgautil

> **Stable bitstream source-of-truth (5r):**
> `fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260516-010205/system_top_wrapper.bit.bin`
> — every command in § 7 deploys this file unless you change `$BUILD`.

### 7.1 Convert the .bit to .bit.bin (one-time per build)

`fpgautil` requires the raw `.bit.bin` format (no Vivado ASCII
header). **Both pre-built bitstream dirs (5r and 5t-maskopt) already
contain the converted `.bit.bin`**, so for the deployed bitstreams
you can skip the conversion entirely. Just pick one and set `$BUILD`
for use in § 7.3:

```sh
# Run from the VisionSoC repo root on the dev host.
# Pick ONE of the two stable bitstreams (see § 0):
BUILD=fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260516-010205          # 5r (vLen=256)
# or:
# BUILD=fpga/build/mudkip2d128big1bram1chain2lanescale_fpga_maskopt-20260518-055430  # 5t-maskopt (vLen=1024)

ls $BUILD/system_top_wrapper.bit.bin    # sanity-check it exists
```

If you are deploying a *freshly built* bitstream that has only the
Vivado `.bit` and no `.bit.bin`, run `bootgen` against the build
dir's `.bif` file (every build dir ships a 4-line bif like
`5r.bif` / `deploy.bif`). The `bootgen` binary is part of the
Vivado/Vitis 2025.2 install (see § 2.1):

```sh
# Run inside the build dir on the dev host (path-relative refs).
cd $BUILD
ls *.bif                                        # e.g. 5r.bif or deploy.bif
bootgen -arch zynqmp -image 5r.bif -w -o system_top_wrapper.bit.bin
```

A typical `.bif` is just:

```
all:
{
    [destination_device = pl] system_top_wrapper.bit
}
```

If none exists, create one with the snippet above. `fpga/system/deploy_camtest3.sh`
has the canonical bootgen invocation as a working reference.

### 7.2 Compile the device-tree overlay

The DTS lives at `fpga/dts/system_top_wrapper.dts`. The `firmware-name`
property inside must point at the same path you scp the `.bit.bin`
to (i.e. `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin`).
This is already set correctly in the repo.

```sh
# On the dev host:
dtc -@ -I dts -O dtb -o /tmp/system_top_wrapper.dtbo \
    fpga/dts/system_top_wrapper.dts
```

### 7.3 Install to /lib/firmware/xilinx/visionsoc/ on the board

```sh
# Dev host → board:
scp $BUILD/system_top_wrapper.bit.bin kv260:/tmp/
scp /tmp/system_top_wrapper.dtbo      kv260:/tmp/

# On the board (these paths are what the dtbo's firmware-name expects):
ssh kv260 '
  sudo mkdir -p /lib/firmware/xilinx/visionsoc
  sudo install -m 644 /tmp/system_top_wrapper.bit.bin \
    /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin
  sudo install -m 644 /tmp/system_top_wrapper.dtbo \
    /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo
'
```

### 7.4 Load via fpgautil

You can do this manually for the first sanity check:
```sh
ssh kv260 '
  # Drop the boot-default starter-kit overlay first (it occupies the PL
  # slot with axi-pmon UIO nodes that collide with our enumeration):
  for ov in full k26-starter-kits_image_1; do
    [ -d /sys/kernel/config/device-tree/overlays/$ov ] && \
      sudo rmdir /sys/kernel/config/device-tree/overlays/$ov
  done

  sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin \
                -o /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo
'
# Expect: "Time taken to load BIN is ~400 Milli Seconds" + new /dev/uioN nodes.
```

Verify the UIO enumeration:
```sh
ssh kv260 '
  for u in /sys/class/uio/uio*; do
    echo "$u -> $(cat $u/name)"
  done
'
# Expect (5r): uio4=t1, uio5=dma, uio6=bram (size 0x80000 = 512 KB).
```

In normal operation you never run § 7.4 by hand — the
`run_after_power_cycle.sh` wrapper in § 8 does it for you.

---

## 8. Kick the pipeline off — run_after_power_cycle.sh

Once everything in §§ 1-7 is staged, the operational loop after
every power cycle / reboot is one command from the board shell:

```sh
ssh kv260
cd ~/vision_software/visionsoc_main
./run_after_power_cycle.sh
```

What this script does (see
`vision_software/visionsoc_main/run_after_power_cycle.sh`):

  1. Re-exec itself under `sudo` if not already root.
  2. Kill any stale `visionsoc_main` process.
  3. Drop the `full` / `k26-starter-kits_image_1` overlays so the
     PL slot is free.
  4. `fpgautil -b … -o …` to load the visionsoc bitstream + dtbo.
  5. Wait up to 20 s for `/dev/media0`, `/dev/video0`,
     `/dev/v4l-subdev1`, `/dev/v4l-subdev2` to enumerate.
  6. Set the AP1302, csiss source-pad, and csiss sink-pad formats
     to `VYYUYY8_1X24/128x128` (matches what the 5r BD expects;
     field=none is required, see the `project_v4l2_negotiation_field_none`
     memory).
  7. Print the media graph for visual sanity.
  8. `systemctl stop gdm` (the Ubuntu desktop holds the DRM device
     — we need exclusive access for HDMI output).
  9. `./visionsoc_main` — kicks the camera→DDR→URAM→T1→URAM→DDR
     →HDMI loop.
  10. On Ctrl-C / SIGTERM, trap-restores GDM.

If step 4 or 5 fails the script dumps the active overlays and the
last 80 lines of dmesg filtered for fpga/firmware/overlay/camera
keywords, then exits non-zero.

### 8.1 Performance measurement workflow (optional)

For cycle/timing breakdown of the kernels — `sobel`, `optical_flow`,
`matmul_8bitraw_short`, and the full `pipeline` (frame end-to-end) —
the repo ships a four-piece harness under `vision_software/visionsoc_main/`:

| Script / file | Where it runs | Purpose |
|---|---|---|
| `sync_perf.sh` | dev host | scp's the perf C sources + `run_perf.sh` to the board and native-builds `{visionsoc_main,sobel,optical_flow,matmul_8bitraw_short}_perf` on aarch64 |
| `run_perf.sh` | board | invoked over ssh by `gather_perf.sh`; runs the perf binary, writes CSV to `/tmp/<mode>_perf.csv` |
| `gather_perf.sh` | dev host | orchestrator: ssh→`run_perf.sh`, scp CSV back, run `plot_perf.py`, drop everything in `perf/<UTC-timestamp>-<mode>/` |
| `perf/plot_perf.py` | dev host | reads the CSV and renders a stacked-bar PNG (matplotlib backend `Agg`, no display required) |

All four scripts arrive on the board for free as part of § 6's
`scp -r vision_software kv260:~/` (sync_perf.sh and gather_perf.sh
sit in the same directory). No new sudoers entries needed — the perf
binaries get installed into `~/vision_software/libt1/test/` on the
board, which is already on the NOPASSWD allowlist from § 3.1.

**Pre-reqs:** § 2.1's dev-host install must include
`python3-matplotlib` and `python3-numpy` (the plotter's imports).

**Canonical invocation:**

```sh
# Dev host, in vision_software/visionsoc_main/:
./sync_perf.sh                        # one-time after editing any *_perf.c
./gather_perf.sh sobel    100         # 100 sobel iterations
./gather_perf.sh pipeline 30          # 30 frames through full pipeline
./gather_perf.sh both     100         # both modes back-to-back
```

Outputs land in `vision_software/visionsoc_main/perf/<UTC>-<mode>/`:

```
perf/2026-05-22T10-15-32Z-sobel/
  sobel_perf.csv
  sobel_breakdown.per_instr.png
  sobel_breakdown.grouped.png
  run.log
  meta.txt        # mode, N, t1-hz, board hostname, bitstream sha256
```

Full reference (csv schema, what each binary measures, how the
T1-cycles vs wall-µs decomposition is computed): see
`vision_software/visionsoc_main/perf/README.md`.

---

## 9. End-to-end smoke test

Do this on a fresh board after § 8 has completed successfully. The
target is the `frame_passthrough` kernel because it makes data
integrity directly observable: the script prints `camY` and `outY`
brightness statistics, and they must match byte-identically every
frame.

### 9.1 Pre-flight state check (no run)

```sh
ssh kv260 '
  echo "=== sudoers ==="
  ls -l /etc/sudoers.d/visionsoc-nopasswd
  echo "=== udmabuf devs ==="
  ls /dev/udmabuf*
  for i in 0 1 2; do
    echo "udmabuf$i: phys=$(cat /sys/class/u-dma-buf/udmabuf$i/phys_addr 2>/dev/null) size=$(cat /sys/class/u-dma-buf/udmabuf$i/size 2>/dev/null)"
  done
  echo "=== u_dma_buf module ==="
  lsmod | grep u_dma_buf || echo "MODULE NOT LOADED"
  echo "=== mediasrcbin ==="
  gst-inspect-1.0 mediasrcbin >/dev/null 2>&1 && echo "OK" || echo "MISSING"
  echo "=== tools ==="
  for p in devmem2 riscv64-linux-gnu-as dtc make gcc media-ctl v4l2-ctl; do
    command -v $p >/dev/null && echo "$p: $(command -v $p)" || echo "$p: MISSING"
  done
  echo "=== bitstream staged ==="
  ls -l /lib/firmware/xilinx/visionsoc/ 2>/dev/null || echo "NOT STAGED"
  echo "=== AP1302 firmware ==="
  sha256sum /lib/firmware/ap1302_ar1335_single_fw.bin
'
```

A green run shows: sudoers 440 root:root, three udmabuf nodes at
0x4MB each, `u_dma_buf` loaded, mediasrcbin OK, all six tools
present, both `.bit.bin` + `.dtbo` staged, AP1302 firmware sha
starts `2dd09e34…`.

### 9.2 Set the active kernel + sync

```sh
# Dev host:
cd vision_software/visionsoc_main
./sync_kernel.sh frame_passthrough
```

### 9.3 Load + run

```sh
ssh kv260 'cd ~/vision_software/visionsoc_main && ./run_after_power_cycle.sh'
```

Expected stdout (after a few seconds of overlay load + media-ctl
setup):
```
camera: mplane query nplanes=1 p0.len=24576 p0.off=0 p1.len=0 p1.off=0
frame 32,  last kernel 22302 cycles, 17.7 fps, camY 17/254/79.1, outY 17/254/79.1
frame 64,  last kernel 22303 cycles, 18.1 fps, camY 17/254/81.0, outY 17/254/81.0
frame 96,  last kernel 22230 cycles, 20.0 fps, camY 17/254/80.4, outY 17/254/80.4
…
```

Pass criteria:
  * `camY` and `outY` triplets are byte-identical every frame
    (proves the camera → DDR → URAM mm2s → T1 → URAM → DDR s2mm
    → display round-trip is data-faithful).
  * Frame rate climbs to ~20 fps and holds steady.
  * HDMI output shows the live camera feed in greyscale (Y plane
    only — chroma is PS-filled neutral by default).

Ctrl-C exits cleanly and the SIGTERM trap restarts GDM.

---

## 10. Subsequent boots (TL;DR)

Once §§ 1-7 are done once, the per-boot procedure collapses to:

```sh
ssh kv260
cd ~/vision_software/visionsoc_main
./run_after_power_cycle.sh
```

Everything from § 5.1 (AP1302 firmware), § 5.2 (mediasrcbin), § 4
(u-dma-buf auto-load), and § 7.3 (staged bitstream/dtbo) persists
across reboots. The only thing that needs re-doing on every power
cycle is dropping the boot-default starter-kit overlay and loading
visionsoc — which is exactly what the script does.

If you only changed a kernel:
```sh
# dev host:
./sync_kernel.sh <kernel_name>
# board (in a separate shell):
./run_after_power_cycle.sh    # only if not already running
```

If you only changed visionsoc_main C code (no overlay change
needed), kill the current run with Ctrl-C, re-sync via
`sync_kernel.sh` (which scp's main.c too), and just re-run
`./visionsoc_main` — no overlay reload required.

---

## 11. Recovery / revert

  * **Sudoers wiped** → re-install from the template
    (`~/visionsoc-nopasswd.template`); see § 3.1.
  * **u-dma-buf missing after reboot** → check
    `/etc/modules-load.d/u-dma-buf.conf` and
    `/etc/modprobe.d/u-dma-buf.conf` exist (§ 4).
  * **AP1302 CRC mismatch panic in dmesg** → re-install firmware
    from `Xilinx/ap1302-firmware` HEAD (§ 5.1). Do not unbind/rebind
    the ap1302 driver — that triggers a kernel oops; reload the
    FPGA overlay instead.
  * **mediasrcbin missing after `apt upgrade`** → re-add the
    two PPAs (§ 5.2). Stock `gstreamer1.0-plugins-bad` from the
    Ubuntu archive does not contain mediasrcbin and will mask the
    Xilinx-patched version if a later apt run pulls it in.
  * **Need to revert from 5r to 5q-r3, or swap between 5r ↔
    5t-maskopt** → if the destination bitstream is staged at
    `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.<tag>-backup`,
    swap it in and reload:
    ```sh
    sudo cp /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.<tag>-backup \
            /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin
    sudo cp /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo.<tag>-backup \
            /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo
    sudo rmdir /sys/kernel/config/device-tree/overlays/full && sleep 1
    sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin \
                  -o /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo
    ```
    **For a freshly set-up KV260 those `.<tag>-backup` files won't
    exist** — they're a convention that grew on the project's own
    deployed board (5q-r3 backup is preserved per
    `fpga_build_status.md` § 0.11; 5q-final earlier). To create
    them on a fresh board, before swapping, snapshot the currently
    active files:
    ```sh
    sudo cp /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin \
            /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.<current-tag>-backup
    sudo cp /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo \
            /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo.<current-tag>-backup
    ```
    Then repeat § 7.3 with the new build dir's `$BUILD` to install
    + reload the new bitstream.

---

## 12. Cross-references

  * `fyp_doc/fpga_build_status.md` — bitstream history, 5r/5q-r3
    deltas, BD/dts changes.
  * `fyp_doc/camera_bringup_status.md` — full bringup trail,
    sudoers contents, udmabuf module gotchas, deferred-tools
    table.
  * `fyp_doc/camera_handoff_2026-05-13.md` § 4.22-4.24 — PPA +
    mediasrcbin discovery trail (in case the PPA URL changes).
  * `fyp_doc/2d_fabric_handoff.md` — once the pipeline is live,
    read this before writing or debugging any T1 vector kernel.
  * `fyp_doc/driver_function_spec.md` + `vision_software/libt1/` —
    libt1 API surface.
  * `vision_software/visionsoc_main/perf/README.md` — perf-harness
    binary catalogue, CSV schema, T1-cycles/wall-µs decomposition
    reference. Read before invoking § 8.1.
