# HDMI / Display Bringup Notes

**Owner of last touch:** Codex, 2026-05-14

This note records the display/runtime changes made while debugging why the
KV260 monitor showed the Ubuntu login screen, fallback DRM modes, or "no
signal" instead of the VisionSoC camera feed.

## Summary

The active display path is **not** a PL HDMI transmitter. It is:

```
PS DDR framebuffer
  -> ZynqMP PS DisplayPort controller (`zynqmp-display` / DRM/KMS)
  -> KV260 carrier DP-to-HDMI bridge
  -> HDMI monitor
```

There is no active `v_hdmi_tx_ss` IP in this path. HDMI clock-domain notes for
`v_hdmi_tx_ss` do not apply to the current bitstream.

The live board had GDM/Xorg running and showing the Ubuntu login screen. That
means Xorg owns DRM master on `/dev/dri/card0`; raw KMS programs such as
`visionsoc_main` and `display_smoke` cannot take over the monitor while GDM is
active. Stop GDM before running raw DRM/KMS display tests:

```sh
sudo systemctl stop gdm
cd ~/vision_software/visionsoc_main
./display_smoke
# or: sudo ./visionsoc_main
sudo systemctl start gdm
```

## Live Findings

### EDID / modes

The first probe saw empty EDID and only generic fallback modes:

```
/sys/class/drm/card0-DP-1/status  = connected
/sys/class/drm/card0-DP-1/enabled = enabled
edid bytes = 0
modes = 1024x768, 800x600, 848x480, 640x480
```

After later GDM/display cycling, EDID became readable (`256` bytes) and DRM
advertised real monitor modes including `1920x1080`. The EDID issue therefore
appears to be a live link/DDC state problem, not a code assumption.

### Ubuntu login screen

Seeing the Ubuntu login screen is expected while GDM is active. It proves the
PS DisplayPort -> carrier DP-to-HDMI -> monitor path works, but it also means
`visionsoc_main` cannot drive the same DRM device directly until GDM releases
DRM master.

`fuser -v /dev/dri/card0` showed Xorg with DRM master access (`F...m`).

### Display-only smoke test

Added `vision_software/visionsoc_main/display_smoke.c`. It opens the same
`display.c` KMS path and generates a synthetic NV12 test pattern, avoiding
camera and T1. With GDM stopped, this test successfully drove `1920x1080` and
returned `RC=0`.

This isolates HDMI/DP scanout as working. Later full-pipeline testing also
confirmed that the camera/T1/display loop runs when GDM is stopped and the
camera media graph is configured.

### Full pipeline test

After switching camera dequeue to blocking `VIDIOC_DQBUF`, the full app ran on
the board with GDM stopped:

```
camera: mplane query nplanes=1 p0.len=24576 p0.off=0 p1.len=0 p1.off=0
frame 32, last kernel 24129 cycles, 27.2 fps
frame 64, last kernel 24111 cycles, 18.8 fps
...
frame 1504, last kernel 24218 cycles, 30.0 fps
frame 1536, last kernel 24411 cycles, 30.0 fps
frame 1600, last kernel 24484 cycles, 30.0 fps
```

The run was stopped externally after the proof test. This means the current
software path can capture camera frames, move the Y plane through T1, assemble
NV12, scale it in PS software to the DRM mode, and scan it out through
PS DisplayPort.

## Code Changes

### `display.c`

1. Final display fix: use direct RGB565 CRTC scanout.

   An intermediate version used a black RGB565 primary plane plus a mode-sized
   NV12 overlay plane. The app ran at 30 fps and camera/T1 output stats were
   non-black, but the monitor stayed black. This is consistent with the
   full-screen primary plane covering the overlay, or otherwise ambiguous
   zynqmp overlay ordering.

   The current code avoids that path entirely: `display.c` allocates two
   mode-sized RGB565 dumb buffers, `main.c` converts the 128x128 Y plane to
   grayscale RGB565 while scaling to the active mode, and `display_qbuf()`
   sets the CRTC framebuffer directly.

   The scaler preserves the 128x128 aspect ratio: it scales into the largest
   centered square that fits the active mode and fills the remaining area with
   black bars. On a 1920x1080 monitor, the camera appears as a centered
   1080x1080 square with black bars on the left and right.

2. Changed the primary framebuffer format from `DRM_FORMAT_XRGB8888` to
   `DRM_FORMAT_RGB565`.

   Reason: live DRM debugfs showed Xorg using `RG16`/RGB565 on the active
   primary plane. `drmModeSetCrtc()` rejected the XRGB8888 primary framebuffer
   with `EINVAL`:

   ```
   display: SetCrtc crtc=41 conn=43 fb=47 mode=1920x1080 failed: Invalid argument
   ```

   RGB565 mode-setting succeeds.

3. Stopped relying on hardware plane scaling or NV12 overlay composition.

   Reason: the ZynqMP display driver logged:

   ```
   zynqmp-display ... Layer width:height must be 1920:1080
   ```

   So the display framebuffer must be mode-sized. The software now fills a
   mode-sized RGB565 dumb buffer.

4. Added targeted diagnostic prints around failing DRM calls:
   - dumb-buffer creation
   - `drmModeAddFB2`
   - dumb-buffer mmap
   - `drmModeSetCrtc`
   - `drmModeSetPlane`

### `main.c`

1. Replaced the BRAM/DMA path with DDR-to-DDR T1 passthrough:
   - camera Y plane is staged in a `t1_buf` input udmabuf
   - T1 reads from that udmabuf and writes Y into an output udmabuf
   - PS copies the UV plane so the output is complete NV12

2. Added `copy_nv12_to_display()`.

   This scales the 128x128 NV12 frame into the current DRM mode in software
   before scanout. Current observed mode is `1920x1080`, but the function uses
   `db.width` / `db.height` from DRM and is not hardcoded to 1080p.

3. Removed the hard assumption that display pitch is exactly `128`.

   DRM dumb buffers can have aligned pitch. The display copy is now pitch-aware.

4. Avoided passing V4L2 MMAP physical addresses directly to T1.

   Reason: V4L2 MMAP buffers do not provide a reliable userspace physical
   address on this setup. The code now copies the camera Y plane into a known
   udmabuf (`t1_buf_alloc`) and passes that udmabuf PA to T1.

### `camera.c` / `camera.h`

1. Switched camera negotiation to NV12.

2. Added support for `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`.

   Live `/dev/video0` reports:

   ```
   Driver name: xilinx-vipp
   Capabilities: Video Capture Multiplanar
   Formats: NM12, NV12
   ```

   The previous single-planar `V4L2_BUF_TYPE_VIDEO_CAPTURE` API failed with
   `EINVAL`.

3. Added support for one-plane or two-plane NV12 mmap buffers:
   - `camera_buf.va` / `length` for Y or contiguous NV12
   - `camera_buf.uv_va` / `uv_length` when UV is exposed as a second plane

4. Removed physical-address translation for V4L2 MMAP buffers.

   The code keeps `cb.pa = 0` for V4L2 buffers. T1 input now comes from the
   staging udmabuf in `main.c`.

5. Added V4L2 error diagnostics around:
   - `VIDIOC_QUERYCAP`
   - `VIDIOC_S_FMT`
   - `VIDIOC_REQBUFS`
   - `VIDIOC_QUERYBUF`
   - `VIDIOC_QBUF`
   - `VIDIOC_STREAMON`

6. Changed `camera_dqbuf()` from nonblocking `poll()` plus `VIDIOC_DQBUF` to a
   blocking `VIDIOC_DQBUF` loop.

   Reason: `v4l2-ctl` could capture frames from the same device after media
   setup, but the app's nonblocking/poll path timed out while the Xilinx CSI
   receiver reported line-buffer problems. Blocking dequeue matches the
   working `v4l2-ctl` behavior more closely and fixed the full app run.

7. Made camera dequeue signal-aware with `camera_set_cancel_flag()`.

   Reason: if `VIDIOC_DQBUF` blocks while CSI2RX is wedged, Ctrl-C/TERM must
   break out and let `camera_close()` issue `VIDIOC_STREAMOFF`. The previous
   loop could retry an interrupted dequeue forever, forcing SIGKILL and making
   it easier to leave the CSI receiver in a bad state.

## Current Status

The HDMI/DP display path is working. The camera feed is not visible while the
Ubuntu login screen is visible because GDM/Xorg owns DRM master. For raw KMS
testing, stop GDM before launching the app:

```sh
sudo systemctl stop gdm
cd ~/vision_software/visionsoc_main
sudo ./visionsoc_main
sudo systemctl start gdm
```

`visionsoc_main` currently needs root privileges. Running it as the normal
`kv260` user failed at T1 setup:

```
t1_init: Permission denied
```

So the app must be launched with `sudo`, or the T1/display/video device
permissions need to be relaxed deliberately.

To leave the camera feed running after SSH disconnects:

```sh
sudo systemctl stop gdm
cd ~/vision_software/visionsoc_main
sudo sh -c 'nohup ./visionsoc_main > /tmp/visionsoc_main.log 2>&1 & echo $! > /tmp/visionsoc_main.pid'
tail -f /tmp/visionsoc_main.log
```

To restore the Ubuntu login screen:

```sh
sudo pkill visionsoc_main
sudo systemctl start gdm
```

If the CSI receiver has been left in a bad state, the app can fail at stream
start:

```
camera: VIDIOC_STREAMON failed: Broken pipe
camera_open: Broken pipe
```

It can also reach `STREAMON` and then hang before the first frame while the
kernel reports:

```
xilinx-csi2rxss 80000000.csiss: Stream Line Buffer Full!
```

Kernel log shows the underlying failure:

```
xilinx-csi2rxss 80000000.csiss: soft reset timed out!
xilinx-video axi:isp_vcap_csi: s_stream on failed on subdev
xilinx-video axi:isp_vcap_csi: ret = -62 for entity 80000000.csiss
```

That is a CSI/V4L2 receiver state problem, not an HDMI/DRM problem. Reloading
the overlay cleared this condition during debug:

```sh
sudo rmdir /sys/kernel/config/device-tree/overlays/full 2>/dev/null || true
sudo rmdir /sys/kernel/config/device-tree/overlays/k26-starter-kits_image_1 2>/dev/null || true
sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin \
  -o /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo
```

After a power cycle the active overlay can be the default
`k26-starter-kits_image_1`. Remove it before loading VisionSoC. If it is left
active, `fpgautil` may load the BIN but fail to apply the VisionSoC DT overlay,
leaving no `/dev/media0`, `/dev/video0`, or `/dev/v4l-subdev*` nodes.

The local helper script `vision_software/visionsoc_main/run_after_power_cycle.sh`
performs this sequence and prints recent overlay/camera `dmesg` lines if DTBO
application fails.

Older command kept here for reference, but it is insufficient after a clean
boot if the starter-kit overlay is active:

```sh
sudo rmdir /sys/kernel/config/device-tree/overlays/full 2>/dev/null || true
sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin \
  -o /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo
```

After the overlay reload, wait for `/dev/v4l-subdev1` and `/dev/v4l-subdev2`,
then restore the 128x128 media-pad formats:

```sh
sudo v4l2-ctl -d /dev/v4l-subdev1 \
  --set-subdev-fmt pad=2,width=128,height=128,code=0x2100,field=none
sudo v4l2-ctl -d /dev/v4l-subdev2 \
  --set-subdev-fmt pad=0,width=128,height=128,code=0x2100,field=none
```

Subdevice numbers are not stable after every overlay load. On one power-cycle
run, the graph came back as:

```
/dev/v4l-subdev0 = 80000000.csiss
/dev/v4l-subdev1 = ar1335 0
/dev/v4l-subdev2 = ap1302.4-003c
```

The fixed helper script therefore uses media entity names instead:

```sh
media-ctl -d /dev/media0 \
  -V '"ap1302.4-003c":2 [fmt:VYYUYY8_1X24/128x128 field:none colorspace:srgb]'
media-ctl -d /dev/media0 \
  -V '"80000000.csiss":0 [fmt:VYYUYY8_1X24/128x128 field:none colorspace:srgb]'
media-ctl -d /dev/media0 \
  -V '"80000000.csiss":1 [fmt:VYYUYY8_1X24/128x128 field:none colorspace:srgb]'
```

After that fix, a post-power-cycle V4L2 capture succeeded:

```
VIDIOC_STREAMON returned 0
cap dqbuf: 0 ... bytesused: 737280
/tmp/powercycle_check.nv12 = 24576 bytes
```

That captured camera frame was not black:

```
Y min/max/avg  = 24 / 254 / 104.6
UV min/max/avg = 76 / 188 / 132.7
```

So if the monitor is black while `visionsoc_main` prints frame logs, the
camera is not the source of the black frame. The next debug build adds:

```sh
VISIONSOC_MARKER=1     # draw a white border/cross after NV12 scaling
VISIONSOC_BYPASS_T1=1  # copy camera Y on PS instead of through T1
```

Frame logs also print camera-Y and output-Y min/max/average. Use these to
separate display-plane composition from T1/output-buffer content.

## Assumptions

1. The active bitstream uses PS DisplayPort output, not PL HDMI TX.
2. The monitor should be driven through `/dev/dri/card0` / `card0-DP-1`.
3. Raw KMS programs must run without GDM/Xorg owning DRM master.
4. The ZynqMP display layer requires mode-sized layers; do software scaling
   from 128x128 to the active mode.
5. V4L2 MMAP camera buffers should not be used as direct T1 physical-address
   inputs; stage into udmabuf first.
6. The camera media graph is configured externally before running the app:
   AP1302 pad 2 and CSI2RX pad 0 are both set to 128x128 `VYYUYY8_1X24`
   (`code=0x2100`).
7. If GDM is active, the expected visible output is the Ubuntu login screen,
   not the raw KMS camera feed.
