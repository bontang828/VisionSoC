# Camera bringup status — handoff for next agent (2026-05-13 late)

**Branch:** `fpga_driver`
**Last touch:** Claude (Opus 4.7), 2026-05-13 ~17:00 BST

## On format choice — yes the doc recommends NV12

The handoff recommends **NV12** for both DDR efficiency (24 KB vs
32 KB for UYVY at 128×128) and T1 ergonomics (Y plane contiguous
at buffer offset 0 → single `vle8.v` reads greyscale, no
Y-extract pre-pass via stride-2 LSU or vrgather). Smartcam uses
NV12 as its canonical format on this exact hardware, so it is the
known-good config. **However**, if § 7.5 untried hypotheses get
UYVY working faster, that's also fine — the T1 kernels just need
~5-10% more code for the Y-extract; not a blocker. The 5q recipe
can ship either way.

## 0. TL;DR

The **camera DATA path works** at the receiver level on the camtest3 v8
bitstream — CSI2RX decodes 64,800+ packets/streamattempt, AP1302 emits
30+ fps. The remaining wall is the **V4L2 framework format-negotiation
chain** between AP1302 driver, csiss driver, and frmbuf driver — they
can't agree on a common mbus/v4l2 format chain for NV12 capture
through `gst-launch v4l2src`. Smartcam uses Xilinx-internal
`mediasrcbin` plugin to handle this; it's not in Ubuntu repos.

**Two decision-options for the user, both documented below:**
  * **A** — defer V4L2 capture, use synthetic preloaded frames for
    vision_program (T1-side already works); merge v8 recipe into the
    full BD as 5q with T1.
  * **B** — bypass V4L2: small C program that mmaps `/dev/mem` and
    drives `v_frmbuf_wr` directly. ~100 LoC, no rebuild.

## 1. The verified-working recipe (camtest3 v8)

Files in repo (already committed locally, not pushed):
  * `fpga/system/system_top_camtest3.tcl` — BD definition (NO T1; camera-only)
  * `fpga/system/build_camtest3.sh` — wrapper for parallel build
  * `fpga/system/test_camtest3_v2.sh` — deploy + smoke-test helper
  * `fpga/dts/system_top_camtest3.dts` — companion device-tree overlay

Built bitstream lives at: `fpga/build/camtest3-20260513-145152/system_top_camtest3_wrapper.bit`

Key settings vs 5o baseline:

| Setting | 5o (broken camera) | camtest3 v8 (works at receiver) |
|---|---|---|
| `PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ` | 60 | **100** (FVCO 1500.06→1500.00 MHz) |
| `mipi_csi2_rx CMN_NUM_PIXELS` | 1 | **2** (ppc=1 silently broke decode) |
| `mipi_csi2_rx CSI_BUF_DEPTH` | 256 | **4096** |
| `v_frmbuf_wr HAS_UYVY8` | 1 | **0** (drop UYVY) |
| `v_frmbuf_wr HAS_Y_UV8_420` | 0 | **1** (enable NV12 demux) |
| `v_frmbuf_wr MAX_NR_PLANES` | 1 | **2** |
| `v_frmbuf_wr SAMPLES_PER_CLOCK` | 1 | **2** |
| `v_frmbuf_wr AXIMM_DATA_WIDTH` | 64 | **128** |
| `axis_subset_converter TDATA_REMAP` | `{8'b0,tdata[15:0]}` | **`{16'b0000000000000000,tdata[31:0]}`** (smartcam-exact) |
| `axis_subset_converter S_TDATA_NUM_BYTES` | 2 | **4** |
| `axis_subset_converter M_TDATA_NUM_BYTES` | 3 | **6** |
| `axis_data_fifo FIFO_DEPTH` | 256 | **1024** |
| `ap1302_rst_b` wiring | `peripheral_aresetn` | **`emio_gpio_o[1]` via xlslice** (gpio 79) |
| `system_ila + debug_bridge` | none | none (diagnostic only — removed in v6+) |

Built util: **8,053 LUTs (6.88%)**, 0 critical warnings, 25m wall.

## 2. State on Kria right now

The board has been used by another agent for testing. To return to
the camera-test state, follow §3 reload steps.

Files in place:
  * `/lib/firmware/xilinx/visionsoc/system_top_camtest3.bit.bin` — v8 bitstream
  * `/lib/firmware/xilinx/camtest3/system_top_camtest3.dtbo` — v8 dtbo (NV12 chain)
  * `/home/ubuntu/vision_software/libt1/test/csi_dump` — register-dump tool (build from /tmp/csi_dump.c on Kria)
  * `/tmp/xvcserver-src/xvcServer_mmap` — XVC daemon binary (legacy from earlier ILA attempt; not used in v8)

Sudoers expansions added (durable in `~ubuntu/visionsoc-nopasswd.template`
and `/etc/sudoers.d/visionsoc-nopasswd`):
`v4l2-dbg, i2c{get,set,transfer,detect}, media-ctl, v4l2-ctl, timeout,
kill, pkill, fuser, lsof, cat, ls, sha256sum, /tmp/xvcserver-src/xvcServer_mmap`.

## 3. Reload sequence

```sh
# 1) Re-flash + dtbo
ssh kv260 '
  sudo rmdir /sys/kernel/config/device-tree/overlays/full 2>/dev/null || true
  sudo rmdir /sys/kernel/config/device-tree/overlays/k26-starter-kits_image_1 2>/dev/null || true
  sleep 1
  sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_camtest3.bit.bin \
                -o /lib/firmware/xilinx/camtest3/system_top_camtest3.dtbo
'

# 2) Sanity — should see AP1302 detected + /dev/video0
ssh kv260 'sudo dmesg | grep -i "ap1302\|csiss\|frmbuf" | tail -10'
ssh kv260 'ls /dev/video* /dev/media* /dev/v4l-subdev*'

# 3) Wake AP1302 (un-stall sequence; chip toggles via two-write SYS_START)
ssh kv260 '
  sudo i2ctransfer -f -y 4 w4@0x3c 0x60 0x1a 0x83 0x40
  sleep 0.5
  echo "SYS_START: $(sudo i2ctransfer -f -y 4 w2@0x3c 0x60 0x1a r2)"
  # Should read 0x80 0x40 = PLL_LOCK | STALL_MODE_DISABLED (un-stalled)
  sudo devmem2 0x80000000 w 0x1   # force csiss CCR.ENABLE
'

# 4) Verify receiver is decoding packets — should show CSR=non-zero in [31:16]
ssh kv260 'sleep 2 && sudo /home/ubuntu/vision_software/libt1/test/csi_dump | head -10'
# Expected: 0x0010 = 0xXXXX0000 with XXXX > 0 (PKTCNT increases ~30/sec)
# 0x0024 = 0x80020000 → bit 31 FR (Frame Received) — receiver sees frames
```

If step 4 shows CSR > 0, **camera data path is alive**. Frame
delivery to memory is the remaining piece.

## 4. The V4L2 wall — what specifically is broken

### 4.1 Negotiation gap (SOLVED 2026-05-13 evening — see § 4.2 for new wall)

After many iterations, the *negotiation* gap is **mbus-code mismatch +
field-spec mismatch**:

  * AP1302 source pad outputs `UYYVYY8_0_5X24` (0x2026) by default after
    `media-ctl` set (chip emits NV12 / YUV420 via PREVIEW_OUT_FMT =
    FT_YUV_JFIF | FST_YUV_420).
  * csiss with `xlnx,csi-pxl-format = <0x18>` (YUV420_8B) maps to
    `MEDIA_BUS_FMT_VYYUYY8_1X24` (0x2100) ONLY — see
    `xilinx-csi2rxss.c:172` `xcsi2dt_mbus_lut[]`. It does NOT recognize
    `UYYVYY8_0_5X24`.
  * AP1302 supports **both** codes (`UYYVYY8_0_5X24` AND `VYYUYY8_1X24`
    in `supported_video_formats[]` at `ap1302.c:464`); both write the
    SAME `PREVIEW_OUT_FMT` value (`FT_YUV_JFIF | FST_YUV_420`). Only
    the V4L2 graph code differs.
  * AP1302 sets field = `V4L2_FIELD_NONE`; csiss inits to
    `V4L2_FIELD_ANY`. Default `v4l2_subdev_link_validate_default`
    rejects the link with `-EPIPE` because field doesn't match.

**Fix (no rebuild required):**
```sh
sudo v4l2-ctl -d /dev/v4l-subdev1 --set-subdev-fmt \
              pad=2,width=128,height=128,code=0x2100,field=none
sudo v4l2-ctl -d /dev/v4l-subdev2 --set-subdev-fmt \
              pad=0,width=128,height=128,code=0x2100,field=none
```

After this, STREAMON succeeds, csiss receives frames, debug log shows
`Frame Received: 1, 2, 3, 4`.

### 4.2 The actual frmbuf wall — `ap_done` never asserts

Even with negotiation fixed, `yavta -B capture-mplane -c1 -n4 -s 128x128
-f NV12M ...` captures **zero bytes**. DQBUF blocks for the full
timeout. State during capture:

  * AP1302 SYS_START = `0x8040` (PLL_LOCK | STALL_MODE_DISABLED — chip
    emitting frames per docs)
  * csiss CCR=1, CSR.PKTCNT increases — frames decoded at native ~30fps
    rate (visible in dmesg "Frame Received" log timestamps)
  * frmbuf: ap_start=1 (transitions 0x81 → 0x61 = AP_START | FLUSH |
    FLUSH_DONE later), **stride=0x80 = 128 (correct)**, width=0x80=128,
    height=0x80=128, format=0x13=NV12, PA_PLANE0 set
  * Plane-0 PA in CTRL register **DOES advance** between samples
    (e.g. 0x375B0000 → 0x375C4000 between t=2 and t=3), proving the
    IP successfully completes some descriptors. But yavta DQBUF never
    returns.
  * frmbuf `+0x04` (GIE), `+0x0C` (IPISR), `+0x3C` (PA_PLANE1), `+0x54`
    (ADDR3) reads return empty via devmem2 — this is the known
    devmem2-on-AXI4Lite artefact (project_devmem2_axi4lite_artefact.md),
    NOT a fabric break.

Verified from `drivers/dma/xilinx/xilinx_frmbuf.c` (linux-xlnx
xlnx_rebase_v5.15_LTS):
  * `XILINX_FRMBUF_ADDR2_OFFSET=0x3c` is the chroma-plane PA register.
  * `xilinx_frmbuf_start_transfer()` writes both luma + chroma plane
    addresses, then WIDTH/STRIDE/HEIGHT/FMT, then `xilinx_frmbuf_start`.
  * For V4L2 capture frame_size=2 (NV12), `hw->chroma_plane_addr[0] =
    dst_start + numf*stride + dst_icg`. The driver IS programming
    PA_PLANE1.

**Found a separate bug — frmbuf reset GPIO is unrouted:**
  * `xilinx_frmbuf_reset()` calls `gpiod_set_value(rst_gpio, 1)` then
    `gpiod_set_value(rst_gpio, 0)` to hard-reset the IP.
  * dts declares `reset-gpios = <&gpio 78 1>` (EMIO bit 0).
  * The camtest3 BD's `ap1302_rst_slice` only takes EMIO bit 1; bit 0
    is **dangling — not connected to anything in the BD**.
  * So driver-side resets are silent no-ops. IP state persists across
    STREAMON/STREAMOFF cycles. (Power-on reset still works via
    `proc_sys_reset_300M/peripheral_aresetn` wiring to ap_rst_n —
    that's why `frmbuf+0x00 = 0x4` after a fresh dtbo reload.)
  * **Fix for next BD rebuild**: route `emio_gpio_o[0]` through an
    inverter (or via xlconcat keeping all bits intact) to v_frmbuf_wr's
    `ap_rst_n` input. Or change dts to remove `reset-gpios` entirely
    and rely on power-on reset only.

Reasons §4.2 wall persists:
  * Hypothesis A: TUSER/TLAST may not be propagating through the
    `axis_subset_converter_cap` properly. The BD comment says "let
    Vivado auto-propagate", but the SOF/EOL signaling on v_frmbuf_wr's
    AXIS input might be broken — frmbuf accepts data but never sees
    frame-end → never asserts ap_done.
  * Hypothesis B: V4L2 multi-plane completion path. Even though the
    PA_PLANE0 register advances, the V4L2 framework may not be
    surfacing the buffer to DQBUF for unknown reasons (yavta DQBUF
    blocked for 20s; with longer wait still no frame).
  * Hypothesis C: The first frame after un-stall is corrupt (CSI
    Virtual Channel 0 Frame Sync Error logged on first run), and
    yavta only requests 1 frame; later frames may work but never
    requested.

Concrete next steps:
  * Read raw memory at the advancing PA via `devmem2 0x375Bxxxx w` for
    several offsets in the 24KB region — if Y plane has real bytes,
    frmbuf IS writing → bug is in V4L2 buffer-completion path. If
    bytes are zero, frmbuf is silent → bug is upstream (TUSER/TLAST).
  * Test with `yavta -c10` (many frames) — first might be lost to
    SOF sync, later might come through.

### 4.9 ROOT CAUSE FOUND — AP1302 HINF (MIPI output) is throttled

Per ChatGPT decision tree, ran focused diagnostic: while yavta is in
STREAMON state, sample AP1302 status registers + CSI PKTCNT every 1s
for 10s.

**The smoking gun came from `v4l2-dbg --log-status -d /dev/v4l-subdev2`
(AP1302 subdev) during yavta:**

```
Frame counters: ICP 13515, HINF 11, BRAC 1
SINF0 L0 state: LP00   stop_s:296 hs_req_s:7 hs_s:697
SINF0 L1 state: LP00   stop_s:271 hs_req_s:4 hs_s:725
SINF0 L2 state: LP11   stop_s:281 hs_req_s:8 hs_s:711
SINF0 L3 state: LP11   stop_s:296 hs_req_s:4 hs_s:700
```

Interpretation:
  * **ICP=13515 frames** — image capture pipeline, the AR1335 sensor
    side. Sensor is producing frames at the expected rate.
  * **HINF=11 frames** — Host Interface (the MIPI OUTPUT to KV260).
    Only 11 frames have been emitted over the MIPI link in 10+s.
    Sensor produces >1000× as many frames as the chip emits.
  * BRAC=1 — irrelevant; probably "bracket capture" or similar
    one-shot counter.
  * MIPI lane states are mostly LP idle, with sparse HS transitions.

So AP1302's ISP is processing sensor frames fine, but the **HINF
(MIPI TX) stage is throttled**. This matches ChatGPT's "Case 2":
"AP1302 ISP is alive internally but MIPI output is
disabled/misconfigured."

### 4.10 Bug suspect in kernel ap1302 driver — HINF_CTRL bits

Inspecting `drivers/media/i2c/ap1302.c` `ap1302_configure()`:

```c
ap1302_write(ap1302, AP1302_PREVIEW_HINF_CTRL,
    AP1302_PREVIEW_HINF_CTRL_SPOOF |
    AP1302_PREVIEW_HINF_CTRL_MIPI_LANES(data_lanes), &ret);
// = SPOOF(bit 4) | MIPI_LANES(4) = 0x14
```

The bit definitions in the same file:
```
BIT(5)  AP1302_PREVIEW_HINF_CTRL_MIPI_CONT_CLK  -- continuous clock
BIT(4)  AP1302_PREVIEW_HINF_CTRL_SPOOF
BIT(3)  AP1302_PREVIEW_HINF_CTRL_MIPI_MODE      -- MIPI vs parallel
BIT(2-0) AP1302_PREVIEW_HINF_CTRL_MIPI_LANES(n)
```

The driver **does NOT set `MIPI_MODE` (bit 3) or `MIPI_CONT_CLK`
(bit 5)**. If the chip default puts it in BT.656 parallel-out mode
on every config-rewrite, only ~11 frames per ~13k sensor frames
would make it out the MIPI bus — exactly what we observe.

Cross-check candidates:
  * AP1302 datasheet `ap1302_preview_hinf_ctrl` register definition
    (ONSemi). The driver's bit naming is correct per source, but
    the polarity / default may not be what the comment suggests.
  * Smartcam reference: run `v4l2-dbg --log-status` on a working
    smartcam build's `/dev/v4l-subdev*` for AP1302 and compare HINF
    counter incrementing rate.
  * Try a `__patched` driver build that sets MIPI_MODE + CONT_CLK
    (0x14 → 0x3C). If HINF starts incrementing → driver bug
    confirmed and fixable.

Alternative test (no rebuild): write `HINF_CTRL = 0x3C` directly
via i2c while yavta is in STREAMON wait. Then check if HINF
counter (via `v4l2-dbg --log-status`) starts incrementing.

```sh
# AP1302 reg 0x2030, write 16-bit value 0x003C:
sudo i2ctransfer -f -y 4 w4@0x3c 0x20 0x30 0x00 0x3C
```

### 4.11 Live HINF_CTRL override test — INCONCLUSIVE (chip stalled during test)

Ran the test above. Result:

```
Before HINF override:
  HINF_CTRL=0x0014 SYS_START=0x8240 (STALLED — see correction below)
  Frame counters: ICP 16509, HINF 91, BRAC 1

After writing 0x3C (MIPI_MODE+SPOOF+CONT_CLK+lanes=4):
  HINF_CTRL=0x003C (write took)
  Frame counters: ICP 16669 (+160 in 3s ≈ 53fps), HINF 91 (UNCHANGED)
```

**Correction (2026-05-13 late, post-ChatGPT review):** This test
is **not a valid disproof** of the HINF_CTRL hypothesis. SYS_START
readback = `0x8240` (STALL_STATUS=1) means the chip was stalled
during the entire override test. yavta **failed STREAMON with
EPIPE** in this attempt — the kernel never called
`ap1302_stall(false)`. With the chip stalled, HINF cannot advance
regardless of HINF_CTRL value.

More precise result: *writing HINF_CTRL=0x3C while AP1302 was
stalled did not itself unstall the chip or start HINF emission.*
This is unsurprising — HINF_CTRL is not a stall-control register.

Re-test sequence required to actually probe the HINF_CTRL
hypothesis (next session):
  1. Force-stall AP1302 (§4.3) **before** yavta to ensure csiss
     soft reset can complete.
  2. Apply subdev fmt fix (§4.1).
  3. Start yavta `-B capture-mplane`.
  4. Confirm STREAMON succeeded (no EPIPE in yavta log).
  5. Confirm SYS_START reads `0x8040` (running, per docs).
  6. Confirm ICP and HINF are both advancing (HINF may be slow).
  7. Sample for 3s — record HINF/s rate.
  8. Then override HINF_CTRL=0x3C via i2c.
  9. Sample again for 3s — compare HINF/s rate.

If HINF/s increases after override, kernel driver bug confirmed.
If HINF/s unchanged, the bug is elsewhere (firmware/driver/sensor
init sequence).

### 4.13 Corrected HINF_CTRL re-test result — TRULY DISPROVED

Ran the corrected procedure (force-stall + fmt-fix + yavta + verify
STREAMON success + observe ICP/HINF deltas).

Pre-test state:
  * yavta STREAMON: ok (no Broken pipe)
  * SYS_START readback after STREAMON: `0x8240` — chip reports
    STALL_STATUS=1 (halted) but STALL_EN=0 + STALL_MODE_DISABLED.
    Effectively still halted.

**Baseline (HINF_CTRL=0x14, kernel-driver-set):**
```
ICP 892 → 961 → 1029   (+137 in ~6s, ~22fps)
HINF  35 →  35 →   35   (FROZEN)
```

**After HINF_CTRL=0x3C override (MIPI_MODE+CONT_CLK added):**
```
ICP 1100 → 1168 → 1237 (+137 in ~5s, ~27fps)
HINF   35 →   35 →   35 (STILL FROZEN — no change from override)
```

Conclusion: **HINF_CTRL bits are NOT the bug.** Even in a known
streaming state with successful STREAMON, adding MIPI_MODE and
CONT_CLK to HINF_CTRL had zero effect on HINF emission rate.

### 4.14 SYS_START GO bit test — also no effect

Tried writing `SYS_START = 0x8350` (added GO bit BIT(4) to the
running state). Driver source defines `AP1302_SYS_START_GO` =
BIT(4) but never writes it.

Pre-write state:
  * yavta STREAMON: ok
  * SYS_START: `0x8340` (this run reports running state, unlike § 4.13
    which had `0x8240`. Chip state varies between runs.)

**Baseline (no GO bit):**
```
ICP 5647   HINF 40
```

**After writing SYS_START = 0x8350 (GO bit set):**
```
SYS_START readback: 0x8350 ✓ (write took)
ICP 5707 → 5775 → 5844 (+197 in ~6s, ~33fps)
HINF   40 →   40 →   40 (FROZEN)
```

Conclusion: **GO bit also doesn't kick HINF emission.** Sensor side
keeps producing frames; MIPI TX stage stays gated.

### 4.15 Summary: the actual unknown

Through §4.13 and §4.14 we have:
  * Falsified: missing HINF_CTRL bits (MIPI_MODE / CONT_CLK)
  * Falsified: missing SYS_START GO bit
  * Confirmed: AP1302 ISP is running (ICP +20-33fps)
  * Confirmed: AP1302 MIPI TX is silent (HINF frozen at ~40)
  * Observation: chip emits a small burst of HINF frames at
    STREAMON / state transitions (HINF jumps from 0 → ~35-91 then
    freezes), then nothing.

The gate on AP1302 MIPI emission is somewhere we haven't probed.
Candidates ChatGPT identified:
  * AP1302 firmware version mismatch vs what driver expects
  * Missing AR1335 sensor stream-enable command (chip-private i2c
    from ap1302 firmware)
  * Other AE/AWB/exposure controls written by
    `__v4l2_ctrl_handler_setup` that we haven't audited
  * `AP1302_ADV_HINF_MIPI_*` advanced registers in the 0x84xxxx
    range — driver only reads `AP1302_ADV_HINF_MIPI_T3` for clock
    timing tuning; maybe other registers in this range need
    explicit writes
  * Bootdata-stage configuration that gets undone by reconfig
    on every `s_stream(1)` call

The definitive way forward is to compare with a working SmartCam
reference: load `kv260-smartcam` app, run identical
`v4l2-dbg --log-status` cadence, and diff:
  * HINF rate
  * SYS_START readback during stream
  * AP1302 control register state
  * AP1302 firmware path/hash

If SmartCam's HINF advances at ICP rate, then we have a clean
"good vs broken" pair to bisect the difference.

### 4.16 SmartCam baseline with yavta — same throttle, runtime is the bug

Loaded smartcam via `sudo xmutil loadapp kv260-smartcam`. This
replaces bitstream + dtbo with `kv260-smartcam.bin` +
`kv260-smartcam.dtbo`. Then ran identical V4L2 setup + yavta:

```
csiss enum_mbus_code pad=0: 0x2100 (VYYUYY8_1X24)  ← matches our v8
set ap1302 pad 2: 0x2100/128x128/field=none  ✓
set csiss pad 0: 0x2100/128x128/field=none  ✓
yavta -B capture-mplane -c10 -n4 -s 128x128 -f NV12M /dev/video1
STREAMON: ok
SYS_START: 0x82 0x40  (same as our v8 — STALL_STATUS=1)
```

**SmartCam HINF rate during yavta:**
```
ICP 583 → 668 → 753  (+170 in 6s ≈ 28fps — sensor still fine)
HINF 28 → 28 → 28    (FROZEN — same throttle as our v8)
```

**This is the definitive finding.** With smartcam's bitstream and
smartcam's dtbo, yavta still triggers the HINF throttle. So:

  * Our v8 bitstream is NOT the bug.
  * Our camtest3 dtbo (csi-pxl-format=0x18, etc.) is NOT the bug.
  * The bug is in the **runtime V4L2 control sequence** — yavta
    sets the format and calls STREAMON, but doesn't issue the
    additional sensor/AP1302 control writes that smartcam's
    `mediasrcbin` gst-element does internally.

Smartcam doesn't bundle `mediasrcbin` as a standalone Ubuntu
package — it's part of `xlnx-app-kv260-smartcam` (not installed
on this Ubuntu Server image; needs `vvas-gst-plugins` build from
`github.com/Xilinx/vvas` or downloading the Xilinx archive deb).

### 4.17 What likely needs to happen

The AP1302 HINF needs additional control writes beyond what the
default `__v4l2_ctrl_handler_setup` does. Candidates:
  * AR1335 sensor mode / streaming-enable command (chip-private
    i2c sequence that mediasrcbin triggers via the v4l2-control
    `scene_mode` or `power_line_frequency` or a custom control)
  * Pre-streaming bootdata reload / re-init from firmware
  * A specific frame-rate setting via `s_frame_interval` subdev
    op (yavta doesn't set frame rate)
  * Sensor-side stream-on via the AP1302 sensor subdev
    (entity `ar1335 0`, /dev/v4l-subdev0) — yavta only streams
    on /dev/video, doesn't touch sensor subdev directly

To replicate mediasrcbin's behavior without building it:
  * Run yavta + simultaneously stream on the ar1335 sensor subdev
    via `v4l2-ctl -d /dev/v4l-subdev0 --stream-mmap=...` (if
    supported)
  * Try setting all v4l2 controls to non-default values to force
    chip writes: `v4l2-ctl -c brightness=300,contrast=300,...`
  * Issue `VIDIOC_S_FRAME_INTERVAL` to set fps explicitly
  * Read `xlnx-app-kv260-smartcam` deb (from
    `archive.ubuntu.com/ubuntu/pool/multiverse/x/xlnx-app-kv260-smartcam/`
    or similar) and extract the binary to look at i2c traffic via
    strace

### 4.18 mediasrcbin / vvas-gst-plugins not available in xilinx-apps PPA

With Kria internet working (2026-05-13 ~21:40 BST), confirmed via
direct PPA query:

```
https://ppa.launchpadcontent.net/xilinx-apps/ppa/ubuntu/pool/main/x/
```

Available packages:
  * `xlnx-firmware-kv260-smartcam` — just bit + dtbo + xclbin
    (already installed)
  * `xlnx-app-kr260-mv-defect-detect`, `xlnx-app-kd240-*` — other
    Kria boards' apps, not kv260
  * `xlnx-tsn-utils`, etc.

**No** `xlnx-app-kv260-smartcam`, **no** `vvas-gst-plugins`, **no**
`mediasrcbin` packages exist in the xilinx-apps PPA jammy/arm64
distribution. The actual smartcam app appears to be distributed
only via:
  * Xilinx official KV260 release tarballs (not in standard PPAs)
  * Vitis-AI runtime SDK
  * Manual build from `github.com/Xilinx/vvas` source
    (`VVAS_REL_v3.0` is current latest release on github; releases
    are source-only, no prebuilt arm64 deb)

Building vvas-gst-plugins for arm64 requires the Xilinx kernel
headers + gstreamer 1.0 dev + vitis-ai dependencies. Estimated
2-4 hours of cross-compile setup.

### 4.19 Kernel panic on rapid overlay swap

Observed twice during this session: `fpgautil unload`/`xmutil
unloadapp` followed by `fpgautil -b ... -o ...` for the new
bitstream-dtbo pair triggers a kernel panic ~30-50% of the time.
Power-cycle is the only recovery.

Workaround:
  * After unloading an overlay, wait ≥5 seconds before loading
    the next one
  * Don't swap overlays unnecessarily — pick one and stay with it
    for the test
  * Prefer `sudo rmdir
    /sys/kernel/config/device-tree/overlays/*/` first (per-overlay
    cleanup), then `sleep 2`, then `fpgautil -b ... -o ...`

This is unrelated to our camera bringup but worth knowing.

### 4.20 Final session findings & recommendations

**Definitively established:**
  1. V4L2 negotiation needs `field=none` + mbus `0x2100` (§4.1).
     This is the *necessary* setup for STREAMON to succeed but
     not sufficient for actual frame capture.
  2. AP1302 ISP works fine (ICP advances at ~30fps per
     `v4l2-dbg --log-status`).
  3. AP1302 MIPI TX (HINF) is throttled. Only ~10-90 frames are
     emitted on the MIPI link per several thousand sensor frames.
  4. The HINF throttle is **NOT caused by**: our bitstream
     (smartcam bitstream shows same), our dtbo (smartcam dtbo
     shows same), `HINF_CTRL` bits, `SYS_START GO` bit, or any
     register we tested via direct i2c writes.
  5. The bug is in the **runtime V4L2 control sequence** —
     specifically, the additional sensor/AP1302 configuration that
     `mediasrcbin` (smartcam's gstreamer plugin) does at stream-on
     but yavta/v4l2-ctl don't.

**Recommended next steps if camera bringup is needed:**
  1. **Build vvas-gst-plugins** to get `mediasrcbin` (~2-4 hour
     effort). After that, smartcam-style capture should work.
  2. **Reverse-engineer mediasrcbin** by strace'ing a working
     smartcam app on a Xilinx PetaLinux image, then replicate the
     specific i2c/v4l2 control writes that we miss. This is more
     work but cheaper than the full vvas build.
  3. **Defer camera, use preloaded frames (Option A in § 5)** for
     the FYP demo — the vision_program pipeline works with disk-
     loaded frames via existing libt1 patterns.

**For this FYP cycle**, Option 3 is the pragmatic choice. The
camera path has been narrowed to a known type of problem (missing
runtime control sequence) and documented thoroughly enough that a
later effort can pick it up.

### 4.21 VVAS source check + V4L2 control-write attempts (2026-05-13 22:00 BST)

Cloned `github.com/Xilinx/VVAS` (tags v1.0, v1.1, v2.0, v3.0).
**None of the VVAS releases contain `mediasrcbin`** — the gst/
folder has tracker, defunnel, funnel, metaaffixer, metaconvert,
reorderframe, roigen, skipframe; sys/ has abrscaler, compositor,
filter, infer, multicrop, multisrc, optflow, overlay, videodec.
No mediasrcbin in any release.

The actual `Xilinx/smartcam` repo (branches: 2021.1, 2022.1) has
the smartcam C++ app that CONSUMES mediasrcbin via gstreamer
pipeline string:
```
mediasrcbin name=videosrc media-device=/dev/mediaX !
  video/x-raw, width=W, height=H, format=NV12, framerate=N/1
```

**mediasrcbin only ships in PetaLinux BSP**, not in any open-source
Xilinx repo. To get it: build PetaLinux for KV260 with smartcam
package group, or extract from Xilinx's KV260 release tarball.

Attempted V4L2-level workarounds (all failed to kick HINF):
  * Write multiple v4l2 controls during streaming
    (`brightness=500,contrast=500,saturation=4500,gain=1024,
    exposure=10`) — chip writes succeeded per i2c, HINF still
    frozen
  * `low_latency_controls = 2` on ar1335 subdev — needs BEFORE
    STREAMON to set (EBUSY during stream); even with correct
    syntax pre-STREAMON, HINF stays frozen
  * `VIDIOC_S_PARM` on /dev/video0 — driver returns
    `Inappropriate ioctl for device` (xilinx-vipp doesn't
    implement set-parm/framerate)
  * `VIDIOC_SUBDEV_S_FRAME_INTERVAL` on any subdev pad — all
    return `Inappropriate ioctl for device` (no driver implements
    frame-interval ops)
  * Write `HINF_CTRL = 0x3C` via i2c (MIPI_MODE + CONT_CLK) —
    no effect on HINF
  * Write `SYS_START = 0x8350` (GO bit set) via i2c — no effect

Whatever mediasrcbin does is not reachable via the public V4L2 +
i2c surface. Most plausible candidates remaining:
  1. mediasrcbin uses an XRT/Vitis-specific ioctl through the
     xclbin that we don't have
  2. mediasrcbin writes AP1302-specific registers via a custom
     v4l2-priv-ioctl that the standard ap1302.c driver exposes
     but tools like v4l2-ctl don't surface
  3. mediasrcbin requires the sensor stream-on path through
     subdev1 (ar1335) but yavta only streams /dev/video0

**Bottom-line recommendation:** ship the FYP demo via preloaded
frames (Option A § 5). Camera live-capture needs either:
  - PetaLinux build with smartcam packagegroup (~3-4hr setup)
  - Reverse-engineering mediasrcbin via strace on a PetaLinux
    runtime (need access to such a system)
  - Patching the ap1302.c kernel driver to bypass mediasrcbin
    entirely (would need to understand what subdev-priv ioctls
    are being made)

None of these fit within FYP demo time budget. Preloaded frames
is the right call.

### 4.22 BREAKTHROUGH: mediasrcbin found on Ubuntu via Xilinx PPA — CAMERA WORKS

**2026-05-14 01:55 BST — camera live capture is working.**

Following ChatGPT's pointer (AMD PetaLinux porting guide names
`gstreamer1.0-plugins-bad-mediasrcbin`) and AMD forum reply
(https://github.com/Xilinx/kria-docker/blob/main/dockerfiles/kria-runtime
shows the exact PPA setup), the mediasrcbin GStreamer plugin is
available on Ubuntu Server 22.04 via two PPAs:

  * `ppa:ubuntu-xilinx/sdk`
  * `ppa:ubuntu-xilinx/gstreamer`

These contain the Xilinx-patched `gstreamer1.0-plugins-bad` which
includes `mediasrcbin`. To install (Ubuntu jammy/arm64):

```sh
# Add PPAs (sudo via tee since add-apt-repository needs interactive sudo)
echo "deb https://ppa.launchpadcontent.net/ubuntu-xilinx/sdk/ubuntu jammy main" | \
    sudo tee /etc/apt/sources.list.d/ubuntu-xilinx-sdk.list
echo "deb https://ppa.launchpadcontent.net/ubuntu-xilinx/gstreamer/ubuntu jammy main" | \
    sudo tee /etc/apt/sources.list.d/ubuntu-xilinx-gstreamer.list

# Add GPG key (id 52150A179A9E84C9 = Launchpad PPA for Ubuntu Xilinx)
gpg --keyserver keyserver.ubuntu.com --recv-keys 52150A179A9E84C9
gpg --export 52150A179A9E84C9 | sudo tee /etc/apt/trusted.gpg.d/ubuntu-xilinx.gpg

sudo apt update
sudo apt install -y vvas-essentials gstreamer1.0-plugins-bad
```

Verify:
```
$ gst-inspect-1.0 mediasrcbin
Plugin Details:
  Name                     mediasrcbin
  Description              Media Source Bin
  Filename                 /usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstmediasrcbin.so
  Version                  0.1
```

### 4.23 Working camera pipeline

After fresh `fpgautil` reload of camtest3 v8 bitstream + dtbo (no
need for manual AP1302 stall or subdev-fmt setup beforehand —
mediasrcbin does all of that internally), this pipeline captures
real camera data:

```sh
gst-launch-1.0 -e mediasrcbin name=videosrc media-device=/dev/media0 ! \
    video/x-raw,width=128,height=128,format=NV12,framerate=30/1 ! \
    filesink location=/tmp/frame.nv12
```

Result: 5 MB file in 8s = 207 frames × 24576 bytes (NV12 128x128) =
~26fps capture. First 64 bytes show real luminance data (smooth
0x3c-0xc9 gradients).

HINF counter advances at ~50fps during streaming (confirmed via
`v4l2-dbg --log-status -d /dev/v4l-subdev1`) — vs yavta which left
HINF frozen at 24-28.

`(gst_structure_fixate_field_nearest_int: assertion 'IS_MUTABLE
(structure)' failed)` warnings during pipeline construction are
harmless — the pipeline still negotiates and streams correctly.

### 4.24 Integration path for vision_program demo

The mediasrcbin output is raw NV12 frames. For the T1 vision pipeline,
options:

  1. **filesink → T1 reads file (per-frame):**
     `gst-launch-1.0 ... ! multifilesink location=/dev/shm/frame-%05d.nv12`
     T1 driver polls /dev/shm/ for next frame, runs vision kernel,
     outputs back to disk or HDMI.

  2. **appsink → C app feeds T1 directly:**
     Use GstAppSink in a C/C++ wrapper that pulls each buffer and
     calls libt1 transfer-to-T1 functions. Lower latency but more code.

  3. **fdsink → named pipe → existing vision_program:**
     mediasrcbin writes to a fifo; existing libt1/vision_program reads
     from fifo with minimal code change (replace its file-load path with
     fifo-read path).

Option 3 is the lightest integration if vision_program already has
file-load capability.

### 4.25 In-flight: camtest4 LUT-trim build (2026-05-14 ~03:00 BST)

Now that mediasrcbin works (§ 4.22-4.23) and the camera pipeline is
proven end-to-end, testing if the 1/256/1 LUT trim works at
pl_clk0=100 (it was thought-broken in the 5o era but that was
SUPERSEDED by the FVCO finding — pl_clk0=60 was the real cause).

Build files:
  * `fpga/system/system_top_camtest4.tcl` (camera-only, 1/256/1
    trim, pl_clk0=100)
  * `fpga/system/build_camtest4.sh`
  * `fpga/dts/system_top_camtest4.dts` (matching dt: `xlnx,ppc=1`,
    `xlnx,pixels-per-clock=1`, `xlnx,axis-tdata-width=16`)

Diffs from camtest3 v8:

| Setting | v8 (smartcam-canonical) | v8-trim (1/256/1) |
|---|---|---|
| `mipi_csi2_rx CMN_NUM_PIXELS` | 2 | **1** |
| `mipi_csi2_rx CSI_BUF_DEPTH` | 4096 | **256** |
| `axis_data_fifo_cap FIFO_DEPTH` | 1024 | **256** |
| `axis_subset_converter S_TDATA_NUM_BYTES` | 4 | **2** |
| `axis_subset_converter M_TDATA_NUM_BYTES` | 6 | **3** |
| `axis_subset_converter TDATA_REMAP` | `{16'b0,tdata[31:0]}` | **`{8'b0,tdata[15:0]}`** |
| `v_frmbuf_wr SAMPLES_PER_CLOCK` | 2 | **1** |
| `v_frmbuf_wr AXIMM_DATA_WIDTH` | 128 | **64** |
| `v_frmbuf_wr C_M_AXI_MM_VIDEO_DATA_WIDTH` | 128 | **64** |
| dt `xlnx,ppc` | 2 | **1** |
| dt `xlnx,axis-tdata-width` | 32 | **16** |
| dt `xlnx,pixels-per-clock` | 2 | **1** |

Expected savings vs v8: ~2.5-3.5k LUTs (out of v8's 8053) and
~2-3 BRAMs (out of 9.5) per the comments in `system_top.tcl` for the
historical 5q-trim attempt.

Pass criterion: after mediasrcbin captures a frame with the new
bitstream, the file is non-zero and contains valid NV12 image data.

### 4.26 camtest4 1/256/1 build RESULT — works, but smaller LUT savings than estimated

Build finished 2026-05-14 03:48 BST. 25m wall, 0 critical warnings.

| Metric | camtest3 v8 (2/4096/2) | camtest4 (1/256/1) | Δ |
|---|---|---|---|
| CLB LUTs | 8053 (6.88%) | **7805 (6.66%)** | **−248 (−3.1%)** |
| CLB Registers | 11198 (4.78%) | 10680 (4.56%) | −518 (−4.6%) |
| Block RAM Tile | 9.5 (6.60%) | **7.5 (5.21%)** | **−2 (−21%)** |

Estimated savings were 2.5-3.5k LUTs per the 5q-era comments in
`system_top.tcl`. Actual: only 248 LUTs. The historical estimate
was inaccurate — most of v8's LUT cost is structural (D-PHY
receiver, CSI-2 protocol decoder, AXI smartconnects, AP1302 I2C
controller) and doesn't shrink with pipeline-width changes. The
SAMPLES_PER_CLOCK and ppc settings only affect a few hundred LUTs.

**BRAM savings are real (2 fewer tiles, 21% drop)** — the
CSI_BUF_DEPTH 4096→256 change removes deep line-buffer BRAMs.

Deploy + test (2026-05-14 ~03:55 BST):
  * Files placed: `/lib/firmware/xilinx/visionsoc/system_top_camtest4.bit.bin`
    + `/lib/firmware/xilinx/camtest4/system_top_camtest4.dtbo`
  * `fpgautil` reload + sleep 5s → overlay loaded cleanly
  * mediasrcbin pipeline (identical to v8 test) captured:
    `5013504 bytes = 204 NV12 frames in 8s ≈ 25.5 fps`
  * HINF counter advanced to 225 — normal MIPI emission rate
  * Frame data verified valid (sample saved to
    `captured_img/camera_frame_128x128_camtest4_rgb.png`).

**Verdict: 1/256/1 trim works at pl_clk0=100.** The 5o-era
hypothesis that the trim broke the camera was indeed superseded by
the FVCO finding (pl_clk0=60 was the real cause).

For 5q merge:
  * Use camtest4's 1/256/1 trim config for the camera section
  * Save 2 BRAMs and ~250 LUTs vs v8
  * Tiny win on LUT-side; meaningful win on BRAM if T1 fabric is
    BRAM-tight

Why did yavta EPIPE this time after working before? csiss soft
reset timed out (`xcsi2rxss_soft_reset` reports `-ETIME = -62`).
This happens because csiss's CSR.RIPCD bit stays asserted —
which happens when AP1302 had emitted packets recently and the
csiss is mid-reception. § 4.3 fix is to force-stall AP1302 via
i2c BEFORE running yavta:

```sh
sudo i2ctransfer -f -y 4 w4@0x3c 0x60 0x1a 0x80 0x40
sudo i2ctransfer -f -y 4 w4@0x3c 0x60 0x1a 0x81 0x40
```

### 4.12 Final state — what we know, what we don't

**Established (this session, 2026-05-13 evening):**

  * V4L2 negotiation completely solved (§4.1) — set-subdev-fmt
    with code=0x2100 + field=none on both AP1302 pad 2 and
    csiss pad 0.
  * V4L2 STREAMON path works when csiss isn't wedged AND
    format chain matches.
  * BD AXIS chain is functionally identical to smartcam
    reference (§4.6) — no observable diff in IP config or
    wiring at the AXIS pipeline level.
  * AP1302 ISP/sensor side works fine (§4.9) — ICP frame
    counter advances at ~50fps, AR1335 producing valid frames.
  * **AP1302 HINF (MIPI TX) is the actual blocker (§4.9)** —
    HINF counter advances ~11-91 frames per several thousand
    ICP frames. The chip throttles MIPI emission to a fraction
    of input rate.
  * The kernel driver `ap1302_configure()` HINF_CTRL bit
    setting (=0x14) is NOT the bug. Setting bits 3, 5 via
    direct i2c write didn't help (§4.11).

**Unknown (next debug needs):**

  * What register/control gates HINF MIPI emission inside
    AP1302? The chip might need:
    - An explicit "start streaming" / "go" command (e.g.
      SYS_START GO bit, or a different streaming enable).
    - The AR1335 sensor stream-on command (which goes through
      ap1302 firmware, not directly via i2c). Smartcam may
      enable AR1335 via specific control writes.
    - A specific framerate / clocking setting that we lack.
  * Why does HINF=91 (not zero)? The chip emits SOMETHING but
    at a tiny fraction. Maybe sporadic frames slip through
    when other conditions briefly align.
  * The "S(13xxx)" console hex output in dmesg — possibly
    serial-port noise from somewhere — looks like AP1302
    firmware debug output (stream-frame counters in
    chip-private format) bleeding through.

**Concrete next-session actions:**

  1. Look at AP1302 datasheet for HINF streaming enable. Key
     registers to investigate: SYS_START GO bit (BIT 4), CTRL
     register, advanced HINF MIPI control regs (0x84xxxx
     range — `AP1302_ADV_HINF_MIPI_T3` etc.).
  2. Compare with smartcam working state — run identical
     `v4l2-dbg --log-status` on a smartcam-loaded board and
     diff HINF counter rate. Smartcam should show HINF ≈ ICP.
  3. Try a `__v4l2_ctrl_handler_setup` equivalent via raw i2c
     — set AE/AWB/exposure controls to typical defaults.
     The kernel does this during s_stream(1) but our test
     might run before/in between.
  4. Look at AP1302 driver patches in smartcam build — maybe
     there's a build-specific stream-start sequence missing
     in our generic ap1302.c.

### 4.8 Option B hybrid — surprising new wall: csi PKTCNT=0 mid-stream

`/tmp/optb2.c` (built and committed; sudoers entry added): same as
optb but **doesn't touch AP1302**. Instead the workflow is:
  1. yavta runs in background to drive AP1302 + STREAMON via kernel.
  2. After yavta is in DQBUF wait, optb2 halts frmbuf (CTRL=0),
     re-points ADDR/ADDR2 to udmabuf2 (PA `0x38100000`), kicks
     CTRL=AP_START.
  3. Polls for AP_DONE while reading csi CSR.

Test results:
  * AP1302 SYS_START readback while yavta running = `0x8340` (the
    documented streaming state) ✓ — kernel driver properly un-stalls.
  * csi CSR = 0 (PKTCNT=0) at t=3s into yavta capture ✗ — **packets
    are not arriving at csiss** even though AP1302 says it's
    streaming.
  * After optb2 kicks frmbuf, ISR.ap_done_irq fires after ~9.7s,
    but: CTRL=`0x61` (FLUSH bit was sticky from prior STREAMOFF in
    earlier yavta cycle), buffer is entirely sentinel `0xAB` — no
    bytes written to udmabuf.
  * Final state CTRL=`0x61`, ISR=`0x3` (AP_DONE + AP_READY pending).

Interpretation: the AP_DONE we got was almost certainly **spurious
flush completion**, not a real frame. The smoking gun: csi.PKTCNT=0
means no real MIPI data was decoded, so frmbuf had nothing valid to
write.

**Revised root-cause hypothesis:** Even when AP1302 SYS_START reads
back as un-stalled, **the chip isn't actually emitting MIPI packets**
during steady-state capture. The 7-second gaps in "Frame Received"
log timestamps + clustered bursts at STREAMOFF (§ 4.2) are consistent
with the chip being mostly idle and only occasionally emitting (or
emitting bursts when state transitions).

This is upstream of frmbuf and unrelated to BD AXIS wiring. **It's an
AP1302 firmware/control issue.** Possible causes:
  * Chip is in a quiescent mode that needs additional un-stall via
    GO bit, SW_RESET, or sensor restart command.
  * Sensor (AR1335) isn't running properly — its clock or power might
    have an issue post-fmt-changes.
  * `__v4l2_ctrl_handler_setup` writes (which the kernel driver
    triggers as part of s_stream(1)) leave the chip in a stalled
    state for some reason — maybe a control setting isn't valid for
    the format.

Concrete next steps (if continuing camera bringup later):
  1. After yavta STREAMON, dump all AP1302 status registers via i2c:
     R0x6004 (warnings), R0x6018 (frame count), R0x6024 (mipi state),
     R0x5100 (AWB status), R0xe014 (general status). Compare to
     smartcam's reported running values from `v4l2-dbg --log-status`.
  2. Run `v4l2-dbg --log-status -d /dev/v4l-subdev1` while yavta is
     in DQBUF wait — the AP1302 driver implements `log_status` and
     will dump the chip's internal state.
  3. Look up AP1302 datasheet for the proper "begin streaming"
     sequence — maybe writing GO bit (SYS_START BIT(4)) is what's
     missing.

### 4.7 Option B direct frmbuf test — AP1302 won't emit without full driver init

Built `/tmp/optb.c` (compiled as `/tmp/optb` on Kria) — 100-LoC program
that mmaps frmbuf CSR + udmabuf2 (4MB @ PA `0x38100000`), drives AP1302
via raw `/dev/i2c-4`, manually configures and kicks frmbuf. Test
sequence:

  1. Stall AP1302 (write `0x8040` then `0x8140` to `SYS_START`).
  2. Configure: `PREVIEW_HINF_CTRL=0x14` (SPOOF | MIPI_LANES(4)),
     `PREVIEW_WIDTH=128`, `PREVIEW_HEIGHT=128`, `PREVIEW_OUT_FMT=0x31`
     (FT_YUV_JFIF | FST_YUV_420 = NV12).
  3. Reset csiss + enable CCR. Verify `CCR=1, PCR=0x1B`.
  4. Configure frmbuf: WIDTH=128, HEIGHT=128, STRIDE=128, FMT=19
     (Y_UV8_420), ADDR=PA, ADDR2=PA+16384. Verify registers.
  5. Set AP_START on frmbuf (CTRL=0x1).
  6. Un-stall AP1302 (write `0x8340` to `SYS_START`).
  7. Poll frmbuf CTRL.AP_DONE for 10s.

Result: **AP1302 didn't emit** — SYS_START readback after unstall write
is `0x8140` (STALL_EN=1, STALL_STATUS=0), not `0x8340` (the expected
streaming state). csi PKTCNT stayed 0. frmbuf CTRL stuck at `0x61`
(AP_START | FLUSH | FLUSH_DONE). Buffer untouched, all sentinel bytes.

The kernel `ap1302_s_stream(1)` path does more than just write the 4
PREVIEW_* registers — it also calls `__v4l2_ctrl_handler_setup` which
walks the entire v4l2_ctrl_handler list (autoexposure, AWB, gain,
brightness, contrast, etc.) and writes each one's chip register.
Plus sensor management (AR1335 binding) which runs on probe but may
also need explicit per-stream coaxing.

**The hybrid path that would work:**
  1. Run yavta to STREAMON state (lets kernel driver fully initialize
     AP1302 + un-stall + start sensor flow). yavta will block on
     DQBUF.
  2. While yavta is blocked, take over frmbuf CSR via `/dev/mem`:
     halt with CTRL=0, override ADDR/ADDR2 to point at udmabuf,
     re-arm CTRL=0x1.
  3. Poll for ap_done, read udmabuf.

This is the appropriate Option B variant given the AP1302 driver
complexity. ~30 more min to add to `/tmp/optb.c`.

### 4.6 BD AXIS chain compared to smartcam reference — IDENTICAL

Cloned smartcam BD `kv260_ispMipiRx_vcu_DP/scripts/config_bd.tcl`
(via sparse-checkout `git@github.com:Xilinx/kria-vitis-platforms.git`)
and compared line-by-line with our `system_top_camtest3.tcl`.

Configs that match:
  * `axis_data_fifo_cap`: FIFO_DEPTH=1024 — same
  * `axis_subset_converter`: M_TDATA_NUM_BYTES=6, M_TDEST_WIDTH=1,
    S_TDATA_NUM_BYTES=4, S_TDEST_WIDTH=10, TDATA_REMAP=`{...,tdata[31:0]}`
    (16 leading zeros, semantically equivalent literal forms),
    TDEST_REMAP=`{tdest[0:0]}` — same
  * `v_frmbuf_wr`: AXIMM_DATA_WIDTH=128, HAS_Y_UV8_420=1, MAX_NR_PLANES=2,
    SAMPLES_PER_CLOCK=2 — same (only MAX_COLS/ROWS differ: ours 256
    vs smartcam 3840/2160, but we're at 128x128 so this is fine)
  * `mipi_csi2_rx_subsystem`: CMN_NUM_PIXELS=2, CSI_BUF_DEPTH=4096,
    C_CSI_FILTER_USERDATATYPE=true, C_HS_LINE_RATE=896, C_HS_SETTLE_NS=146
    — same
  * AXIS chain wiring: csiss/video_out → fifo/S_AXIS, fifo/M_AXIS →
    subset/S_AXIS, subset/M_AXIS → frmbuf/s_axis_video — same

**Configs that differ (one significant):**
  * `v_frmbuf_wr/ap_rst_n` source:
    - smartcam: `xlslice_0/Dout` where xlslice extracts EMIO bit 0
      from `zynq_ps/emio_gpio_o` (92-bit, GPIO_EMIO width 92). So the
      Linux frmbuf driver's `gpiod_set_value(rst_gpio, 1/0)` actually
      drives a real reset wire.
    - ours: `proc_sys_reset_300M/peripheral_aresetn`. Always-high
      during normal operation; only toggles on PS reset / fpgautil
      reload. Driver-side reset is a silent no-op (EMIO bit 0
      dangling).
  * GPIO_EMIO width: smartcam 92 (lots of resets routed), ours 2.

Practical effect: at fresh dtbo-reload after fpgautil, the frmbuf IP
is in clean state in both BDs (peripheral_aresetn pulse on bitstream
load). For the FIRST STREAMON attempt, both should behave the same.
For subsequent STREAMON cycles, smartcam can re-reset, we can't —
but that doesn't explain first-attempt failure.

**So: the AXIS chain is functionally identical and the first-attempt
behavior diff isn't explained by the reset wiring.** The remaining
hypothesis is in the V4L2 framework / bridge driver logic, OR a
subtle difference in IP synthesis (e.g. Vivado IP version, board
files, or LUT-trimming during impl).

### 4.5 Direct DDR peek attempted, result: AXIS framing is the culprit

Built `/tmp/ddr_peek.c` (compiled as `/tmp/ddr_peek` on Kria) that
mmaps an arbitrary PA via `/dev/mem` and dumps bytes. Tried to peek
the V4L2 buffer at `frmbuf+0x30` = `0x375B0000` (CMA-allocated by
videobuf2-dma-contig). **mmap returns `Operation not permitted`** —
CMA pages are not user-mappable through `/dev/mem` by default
(`CONFIG_STRICT_DEVMEM=y` in this kernel build).

Side-effect of trying: in a second clean run, frmbuf's PA register
**stayed at `0x375B0000` throughout 2-8 seconds** of capture, did not
advance. Earlier (different run after fmt fixes), it had advanced
0x375B0000 → 0x375C4000. The non-advancement here, combined with
csiss "Frame Received" log eventually firing at ~30fps timestamps,
points to:

**Most likely root cause of § 4.2 wall: AXIS frame framing
(TUSER for SOF, TLAST for EOL) is NOT propagating from
`mipi_csi2_rx/video_out` through `axis_data_fifo_cap` and
`axis_subset_converter_cap` to `v_frmbuf_wr/s_axis_video`.**

The BD comment in `fpga/system/system_top_camtest3.tcl` claims
"Vivado auto-propagates TUSER (SOF) and TLAST from csiss video_out"
but the symptoms don't bear this out. v_frmbuf_wr writes data
continuously (it's getting AXIS beats) but never detects a frame
boundary → never asserts ap_done → V4L2 DQBUF blocks forever.

Implications:
  * **Option B (direct /dev/mem frmbuf) won't help** — the BD wiring
    bug affects the hardware path, not the kernel driver. Configuring
    frmbuf manually via mmap'd CSR would still see the same broken
    AXIS framing → same wall.
  * The fix must be at BD-level. Add explicit TUSER/TLAST propagation
    config to `axis_subset_converter_cap` (it has CONFIG.TUSER_WIDTH
    and similar parameters) OR add a `axis_tuser_remap` or similar
    intermediate IP. ~25-min rebuild.
  * Alternative: read the Vivado-generated `system_top_wrapper.v`
    after a build and check what's actually connected on the AXIS
    busses between csiss and frmbuf. If TUSER/TLAST signals are
    visibly zeroed-out, fix the BD and rebuild.

### 4.3 Other diagnostics gathered

  * csiss soft reset (`xcsi2rxss_soft_reset` at `xilinx-csi2rxss.c:323`)
    polls `CSR & RIPCD` to clear within 1000μs. If AP1302 is already
    un-stalled and emitting data when STREAMON happens, RIPCD stays
    asserted (active reception interferes with reset) and reset times
    out with `-ETIME` (62). **Workaround:** force-stall AP1302
    before STREAMON:
    ```sh
    sudo i2ctransfer -f -y 4 w4@0x3c 0x60 0x1a 0x80 0x40  # write 0x8040 = un-stall toggle
    sudo i2ctransfer -f -y 4 w4@0x3c 0x60 0x1a 0x81 0x40  # write 0x8140 = STALL_EN, stall mode disabled
    ```
    After this, SYS_START reads `0x8040` (stalled — chip won't emit
    until driver's s_stream(1) unstalls). STREAMON then succeeds.
  * v4l2-ctl on this Kria (`v4l2-utils 5.15`) doesn't accept
    `--get-fmt-video-mplane` / `--set-fmt-video-mplane`. Use `yavta -B
    capture-mplane` instead.
  * mediasrcbin **is not installable from apt** on Ubuntu Server 22.04.
    `kv260-smartcam` firmware (`/usr/lib/firmware/xilinx/kv260-smartcam/`)
    is just bit + dtbo + xclbin — no gstreamer plugin in either the
    firmware package or `/opt/xilinx/*`. Would need to build
    `vvas-gst-plugins` from `github.com/Xilinx/vvas` (cross-compile to
    arm64, package, ship).

### 4.4 State just before this handoff write

The receiver-level state during failed STREAMON:
  * CCR=1, PCR=0x1B (4 lanes), ISR=0x00020000 (STOP — no FR this time
    because the soft reset prevented the chip's flow from getting
    fully established)
  * CSR.PKTCNT increases (we observed 0x05ed = 1517 packets / capture
    window) → packets ARE being decoded
  * frmbuf ctrl=0xE1, stride=0x80 (correct now), width/height/format
    correct
  * frmbuf never asserts ap_done; v4l2 DQBUF blocks forever

## 5. Option A: defer V4L2, ship 5q with T1 + synthetic frames

Steps:
1. Take camtest3 v8 BD diffs (§1 table) and apply to
   `fpga/system/system_top.tcl` (the production BD with T1):
   - PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ 60 → 100
   - Bring TUSER/TDATA/buf_depth/ppc/samples/AXIMM changes from
     `system_top_camtest3.tcl` to system_top.tcl camera section.
2. T1 currently designed at 60 MHz — at 100 MHz timing closure must
   be re-verified. If T1 fails timing in synth/impl:
   - Option (a): add `axi_clock_converter` between T1 and rest,
     give T1 its own 60 MHz clock from a new `clk_wiz_0/clk_60M` output
   - Option (b): re-pipeline T1 RTL for 100 MHz (much more work)
3. Build 5q (~4-5h wall) and deploy.
4. Vision program demos use pre-loaded frames from disk via libt1
   (existing pattern at `tests/vision_program/visualize/render_inputs.py`).
5. Document live camera capture as future work pending mediasrcbin
   or custom V4L2 client.

LUT budget for 5q: 5o was 87.96% synth. v8 BD camera-section is +200-1000
LUTs over 5o (ppc=2 + NV12 frmbuf cost ~+1k, but AXIMM 128 + smaller
axis widths offset some). Should land ~88.5-89% synth. Routability
within margin; **likely needs LUT trims elsewhere** (axi_dma broken-prop
loopback or similar) if it doesn't route.

## 6. Option B: direct frmbuf via /dev/mem (~100 LoC)

No bitstream rebuild needed. Skip v4l2 entirely. The kernel modules
xilinx-csi2rxss and xilinx-frmbuf still claim their hardware, but we
mmap their registers from userspace through `/dev/mem`.

Sketch of the program:

```c
// Sketch — pseudocode, ~100 lines real
int main(void) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);

    // Map frmbuf CSR @ 0x80010000
    volatile u32 *frmbuf = mmap(NULL, 0x10000, PROT_RW, MAP_SHARED, fd, 0x80010000);

    // Allocate a udmabuf or similar contiguous DDR buffer (32 KB for UYVY 128x128
    // or 24 KB for NV12). Get its physical address.
    int udma_fd = open("/dev/udmabuf0", O_RDWR);
    u32 buf_pa = read_phys_addr("/sys/class/u-dma-buf/udmabuf0/phys_addr");
    volatile u8 *buf_va = mmap(NULL, 32768, PROT_RW, MAP_SHARED, udma_fd, 0);

    // Configure frmbuf
    frmbuf[0x10/4] = 128;      // Width
    frmbuf[0x18/4] = 128;      // Height
    frmbuf[0x20/4] = 256;      // Stride (UYVY) or 128 (NV12 Y plane)
    frmbuf[0x28/4] = 0x1C;     // Video format = UYVY (v8 is NV12=0x1B; pick matching)
    frmbuf[0x30/4] = buf_pa;   // Plane 0 dest addr
    frmbuf[0x3C/4] = buf_pa + 16384;  // Plane 1 (UV) for NV12

    // Enable csiss
    volatile u32 *csiss = mmap(NULL, 0x10000, PROT_RW, MAP_SHARED, fd, 0x80000000);
    csiss[0x00/4] = 1;  // CCR.ENABLE

    // Un-stall AP1302 via i2c (use existing i2cset utility or open /dev/i2c-4)
    system("sudo i2ctransfer -f -y 4 w4@0x3c 0x60 0x1a 0x83 0x40");

    // Kick frmbuf
    frmbuf[0x00/4] = 0x81;  // ap_start + auto_restart

    // Wait for ap_done
    while ((frmbuf[0x00/4] & 0x2) == 0) usleep(1000);

    // Read 32 KB out of buf_va, write to file
    FILE *f = fopen("/tmp/frame.bin", "wb");
    fwrite((void*)buf_va, 1, 32768, f);
    fclose(f);
}
```

Considerations:
  * Use the existing `udmabuf0` reservation (4 MB at known PA). The
    libt1 already mmaps this for T1 input/output buffers — same pattern.
  * For NV12 frmbuf (v8 config), Y plane at `pa+0`, UV plane at `pa+16384`,
    total 24 KB; T1 reads `buf_va[0..16383]` as greyscale.
  * For UYVY frmbuf (would need rebuild back to v7 — UYVY frmbuf), use 32 KB
    with stride=256.
  * The Linux V4L2 drivers will still be present but we don't call them.
    They might try to claim frmbuf via probe — that's OK, the kernel
    driver doesn't touch the register space while our userspace
    mmap is active (assuming we don't STREAMON it via v4l2).

Risk: the kernel csiss/frmbuf drivers may auto-start at probe time
and write their own register values, conflicting with ours. Mitigate
by reading registers AFTER our writes to confirm they stuck.

Estimated time: 1-2h to write + test.

## 7. History of what didn't work (so the next agent doesn't repeat)

| Build | Changes | CSR PKTCNT | frame size | Notes |
|---|---|---|---|---|
| 5o, 5p | baseline | 0 | 0 | FVCO bug |
| camtest1 | + reset GPIO via EMIO bit 1 | 0 | 0 | Reset wiring alone not the fix |
| camtest2 | + ppc=2/buf=4096/samples=2 (smartcam-match) | 65k | 0 | First time CSR > 0; chip emits via pl_clk0=100; frmbuf doesn't capture |
| camtest3 v3 slim | + pl_clk0=50 + ppc=1 + TUSER_WIDTH=1 | 0 | 0 | pl_clk0=50 mysteriously breaks DPHY; TUSER forced widths break propagation |
| camtest3 v5 | pl_clk0=50 + ppc=2 + TUSER | 0 | 0 | pl_clk0=50 still broken at ppc=2 |
| camtest3 v6 | pl_clk0=100 + ppc=2 + TUSER | 65k | 0 | Camera path works; frmbuf still no capture |
| camtest3 v7 | + smartcam-exact TDATA_REMAP (UYVY frmbuf) | 65k | 0 | TDATA_REMAP fixed; frmbuf still no capture (was UYVY mode, smartcam is NV12) |
| **camtest3 v8** | + frmbuf NV12 mode (full smartcam match) | 65k | 0 | v4l2-ctl/gst-launch v4l2src can't negotiate format chain |

Wrong turns logged for memory:
  * `xlnx,csi-pxl-format = 0x18` makes csiss reject `UYVY8_1X16` mbus
    even though AP1302 outputs UYVY. Keep csi-pxl-format=0x1E unless
    chip is reconfigured to NV12 emission via `UYYVYY8_0_5X24`.
  * Adding `system_ila` + `debug_bridge` produces CRITICAL WARNING
    [Chipscope 16-336] (no dbg_hub auto-insert with explicit bridge).
    Tries to use XVC over SSH stalled at this hub-wiring problem.
    Removed in v6+.
  * Trying to use `v4l2-ctl --set-fmt-video` (single-planar) on a
    multiplanar driver gives stride=3840 default and 0-byte capture.

## 7.5 V4L2 hypotheses (2026-05-13 late results)

Status as of this handoff:
  * 7.5.1 (yavta) — **partial success**: installed, recognizes
    `capture-mplane`, correctly sets `/dev/video0` to `NV12M
    128x128 stride=128`. STREAMON succeeds (with § 4.1 fix applied).
    Still hits § 4.2 wall (DQBUF blocks).
  * 7.5.4 + 7.5.10 — **partial success**: the field/mbus fix (§ 4.1)
    found via this path. Single-plane v4l2-ctl --set-fmt-video failed
    on mplane device with default stride 3840.
  * 7.5.3 (IRQ arm) — IPIER=0x1 is set by the driver, so manual arming
    not needed. § 4.2 wall persists.
  * 7.5.7 (loadapp smartcam) — **not viable**: no mediasrcbin in
    `kv260-smartcam` firmware (just bit+dtbo+xclbin) and no
    `/opt/xilinx` install. Would need source build per 7.5.2.

Remaining untried (ordered by likelihood-of-success / cost):

1. **`yavta`** — a small, focused V4L2 test tool that has solid
   multiplanar API support (used heavily for V4L2 driver dev). Maybe
   succeeds where v4l2-ctl and `gst-launch v4l2src` failed.
   ```sh
   sudo apt install yavta   # or build from upstream git
   yavta -c1 -f NV12 -s 128x128 --capture=1 --file=/tmp/frame.nv12 /dev/video0
   ```

2. **Build `mediasrcbin` from source** — Xilinx publishes it as part
   of `vvas-gst-plugins` (now AMD Vitis AI). Repo:
   `github.com/Xilinx/vvas`. Build the `mediasrcbin` plugin only,
   `LD_LIBRARY_PATH` it into gstreamer, then the smartcam
   one-liner works.

3. **Frmbuf interrupt enable bits** — the kernel xilinx-frmbuf
   driver may require explicit interrupt arming before frame-complete
   fires. Read driver source: `IPIER (0x08) = 0x1` and `GIE (0x04) = 0x1`
   should be set. Check if the bridge driver actually programs these
   when starting capture. If not, manually write them.

4. **Set frmbuf sink pad format via media-ctl** — we never explicitly
   set `isp_fb_wr_csi`'s sink pad format. Try:
   ```
   media-ctl -V "axi:isp_vcap_csi:0 [fmt:UYVY8_1X16/128x128]"
   ```

5. **dts `xlnx,vid-formats` with multiple options** — currently `"nv12"`.
   Try `"nv12,uyvy"` (comma-separated) to give the bridge negotiation
   flexibility. Same dtbo recompile, no Vivado rebuild.

6. **Probe `MEDIA_BUS_FMT_VYYUYY8_1X24` by raw hex** — v4l2-utils on
   Kria didn't accept the string `VYYUYY8_1X24` (Ubuntu 22.04 version).
   Try by integer: `media-ctl -V "...:0 [fmt:0x202c/128x128]"` (or
   whatever the actual code is — look up in `videodev2.h` headers).

7. **`xmutil loadapp kv260-smartcam` then `apt search smartcam`** —
   loading the smartcam app may install `mediasrcbin` as a side
   effect (it lives in `/opt/xilinx/kv260-smartcam/lib/gstreamer-1.0`
   or similar). After loadapp:
   `export GST_PLUGIN_PATH=/opt/xilinx/kv260-smartcam/lib/gstreamer-1.0`.

8. **Check `xilinx-vipp` driver source** for what mbus→v4l2 format
   mappings it has. The smartcam working chain may need a specific
   mbus code we haven't tried. Read
   `drivers/media/platform/xilinx/xilinx-vipp.c` + `xilinx-dma.c` for
   the conversion table.

9. **Disable `C_CSI_FILTER_USERDATATYPE`** (requires rebuild) — set
   to `false` in the BD. Then csiss accepts ALL incoming MIPI data
   types, removing the dts-vs-mbus negotiation choke point. ~25 min
   rebuild.

10. **`v4l2-ctl --stream-mmap=4 --stream-poll`** — explicit buffer
    count + poll mode. Different buffer-management path that may
    bypass the failing negotiation. No rebuild.

## 8. Key references

  * `fyp_doc/fpga_build_status.md` — full build trail, this work
    is the latest entry (§ 0.2 camera status updated)
  * Memory:
    - `project_dphy_fvco_pl_clk0.md` — why pl_clk0=100 not 60 (FVCO violation)
    - `project_camtest3_v8_recipe.md` — the recipe (this doc is a
      detailed companion)
    - `project_ap1302_diag_toolkit.md` — register addresses for AP1302
      and CSI2RX from linux-xlnx kernel source, locally fetched at
      `/tmp/ap1302-src/`
    - `project_visionsoc_camera_break.md` — early investigation
      (superseded by FVCO finding)
  * Smartcam reference: cloned at `/tmp/kvplat/kv260/platforms/kv260_ispMipiRx_vcu_DP/scripts/config_bd.tcl`
  * Xilinx kernel source: `/tmp/ap1302-src/drivers/media/{i2c/ap1302.c,platform/xilinx/xilinx-csi2rxss.c}`
