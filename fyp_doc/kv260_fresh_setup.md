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
  * Dev host with internet access, capable of running Vivado 2025.2
    if you ever need to re-build a bitstream (not required to just
    re-deploy the existing `.bit.bin`).

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

  * **No `rsync` on the dev host** — use `scp -r` for all source
    syncs. The scripts in `vision_software/` already do this.
  * **`git clone` from the Kria fails for many HTTPS repos**
    ("could not read Username"). When you need a third-party repo
    on the board, clone on the dev host and `scp -r` it over.

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
T1 kernels, libdrm for HDMI, kernel headers for u-dma-buf, etc.):

```sh
ssh kv260 '
  sudo apt update
  sudo apt install -y \
    devmem2 \
    binutils-riscv64-linux-gnu \
    libdrm-dev \
    linux-headers-$(uname -r) \
    dkms \
    build-essential \
    v4l-utils \
    media-ctl \
    i2c-tools
'
```

(`media-ctl` is part of `v4l-utils` on Ubuntu — both lines kept for
clarity.)

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
header). The 5r build dir already contains the converted file, so
**for the deployed 5r bitstream you can skip this step entirely** —
just set `$BUILD` for use in § 7.3 and move on:

```sh
# Run from the VisionSoC repo root on the dev host:
BUILD=fpga/build/mudkip2d128small1bram1chain2lanescale_fpga-20260516-010205
ls $BUILD/system_top_wrapper.bit.bin    # sanity-check it exists
```

Only do the conversion below for a freshly built bitstream that
doesn't have a `.bit.bin` yet:

```sh
# bit2bin.py (small Python helper; if not in the repo, drop it in /tmp):
python3 /tmp/bit2bin.py \
    $BUILD/system_top_wrapper.bit \
    $BUILD/system_top_wrapper.bit.bin
```

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
  * **Need to revert from 5r to 5q-r3** → on the Kria:
    ```sh
    sudo cp /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5q-r3-backup \
            /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin
    sudo cp /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo.5q-r3-backup \
            /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo
    sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin \
                  -o /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo
    ```
    (5q-r3 backup is preserved on the deployed board per
    `fpga_build_status.md` § 0.11.)

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
